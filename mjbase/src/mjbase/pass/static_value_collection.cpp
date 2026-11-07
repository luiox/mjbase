#include "mjbase/pass/static_value_collection.hpp"

#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/constant_folder.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {
namespace {

using jbc::core::Op;

constexpr ca::u16 kAccStatic   = 0x0008;
constexpr ca::u16 kAccFinal    = 0x0010;
constexpr ca::u16 kAccVolatile = 0x0040;

/// 字段身份键：owner + name + desc 拼接（普查与视图写入共用；std::string
/// 仅作键存储，接口面仍为 string_view）。
std::string keyOf(std::string_view owner, std::string_view name, std::string_view desc)
{
    std::string key;
    key.reserve(owner.size() + name.size() + desc.size());
    key.append(owner).append(name).append(desc);
    return key;
}

/// 装载完成的类：会话（backing + IR 同生共死）+ 字节回读源（scratch 重解析用）。
struct LoadedClass
{
    std::string                            name;
    const mjbase::model::ClassResource*    resource = nullptr;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 容器内全部 class 资源装载；任一空 bytes / 不可解析 / 无类名返回 false
/// （普查全量性前提：缺一类即写点普查/父链证据不完整，整件放弃）。
bool loadAllClasses(mjbase::model::ResourceContainer& container, std::vector<LoadedClass>& out)
{
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr) continue;
        if (cls->get().empty()) return false;
        auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{cls->get()});
        if (made.is_err()) return false;
        LoadedClass entry;
        entry.session = std::move(made).unwrap();
        entry.name.assign(entry.session->backing().thisName());
        if (entry.name.empty()) return false;
        entry.resource = cls;
        out.push_back(std::move(entry));
    }
    return true;
}

/// 常量装载判定 → StaticValue（int/long/float/double 走
/// method_rewrite_util 读取面，String 走 ldc 族 constOf）。不匹配返回 false。
bool staticValuePush(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                     StaticValue& out)
{
    out = StaticValue{};
    ca::i32 iv = 0;
    if (method_rewrite_util::intValueOf(session, in, iv)) {
        out.kind = StaticValue::Kind::Int;
        out.num  = iv;
        return true;
    }
    ca::i64 lv = 0;
    if (method_rewrite_util::longValueOf(session, in, lv)) {
        out.kind = StaticValue::Kind::Long;
        out.num  = lv;
        return true;
    }
    float fv = 0.0f;
    if (method_rewrite_util::floatValueOf(session, in, fv)) {
        ca::u32 bits = 0;
        static_assert(sizeof(fv) == sizeof(bits), "float 位图宽度");
        std::memcpy(&bits, &fv, sizeof(bits));
        out.kind = StaticValue::Kind::Float;
        out.bits = bits;
        return true;
    }
    double dv = 0.0;
    if (method_rewrite_util::doubleValueOf(session, in, dv)) {
        ca::u64 bits = 0;
        static_assert(sizeof(dv) == sizeof(bits), "double 位图宽度");
        std::memcpy(&bits, &dv, sizeof(bits));
        out.kind = StaticValue::Kind::Double;
        out.bits = bits;
        return true;
    }
    if (in.op == Op::LDC || in.op == Op::LDC_W) {
        jbc::ir::ClassSession::ConstSem c;
        if (session.constOf(in, c) &&
            c.kind == jbc::ir::ClassSession::ConstSem::Kind::String) {
            out.kind = StaticValue::Kind::Str;
            out.str.assign(c.str);
            return true;
        }
    }
    return false;
}

/// 纯算术二元 op（与 constant_folder 的 binOpOf 支持集逐条对齐——走廊
/// 准入集就是「ConstantFolder 可折叠形态」的超集口径，本件不重写算术）。
bool isCorridorArith(Op op) noexcept
{
    switch (op) {
    case Op::IADD:
    case Op::ISUB:
    case Op::IMUL:
    case Op::IDIV:
    case Op::IREM:
    case Op::IAND:
    case Op::IOR:
    case Op::IXOR:
    case Op::ISHL:
    case Op::ISHR:
    case Op::IUSHR:
    case Op::LADD:
    case Op::LSUB:
    case Op::LMUL:
    case Op::LDIV:
    case Op::LREM:
    case Op::LAND:
    case Op::LOR:
    case Op::LXOR:
    case Op::LSHL:
    case Op::LSHR:
    case Op::LUSHR:
    case Op::FADD:
    case Op::FSUB:
    case Op::FMUL:
    case Op::FDIV:
    case Op::DADD:
    case Op::DSUB:
    case Op::DMUL:
    case Op::DDIV:
        return true;
    default:
        return false;
    }
}

/// 走廊纯度门：clinit 指令集 ⊆ {常量装载，纯算术，NOP，PUTSTATIC(本类)，
/// RETURN} 且异常表为空且指令数 ≤ 预算。无 clinit 视为纯。任一越界即
/// 假——整类不可发布（见头注判据节）。
bool corridorPure(jbc::ir::ClassSession& session, std::string_view thisName)
{
    jbc::ir::ClassUnit& unit = session.unit();
    const ca::usize     idx  = unit.findMethod("<clinit>", "()V");
    if (idx == jbc::ir::kNoMethod) return true;
    if (!jbc::ir::hasCode(session, idx)) return true;
    auto promoted = jbc::ir::promoteMethod(session, idx);
    if (promoted.is_err()) return false;
    auto               cuRef = std::move(promoted).unwrap();
    jbc::ir::CodeUnit& cu    = *cuRef.get();

    if (!cu.exc.empty()) return false;
    const std::vector<jbc::ir::InsnId>& order = cu.dom.order();
    if (order.size() > StaticValueCollection::kMaxCorridorInsns) return false;

    for (const jbc::ir::InsnId id : order) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) return false;
        const Op op = in->op;
        if (op == Op::NOP || op == Op::RETURN) continue;
        if (op == Op::PUTSTATIC) {
            jbc::ir::ClassSession::MemberSem ref;
            if (!session.memberOf(*in, ref)) return false;
            if (ref.kind != jbc::ir::ClassSession::MemberSem::Kind::Field) return false;
            if (ref.owner != thisName) return false;   // foreign 静态写 = 可观察效应
            continue;
        }
        StaticValue ignored;
        if (staticValuePush(session, *in, ignored)) continue;
        if (isCorridorArith(op)) continue;
        return false;
    }
    return true;
}

/// 父链纯度：从 name 沿 super 逐级走廊判（own 判定经 memo 复用）；链上
/// 类不在容器且非 ``java/`` 前缀 → 拒（初始化效应不可证）；环 → 拒。
bool superChainPure(const std::string&                                   name,
                    const std::unordered_map<std::string, LoadedClass>&  byName,
                    std::unordered_map<std::string, bool>&               ownMemo)
{
    std::unordered_set<std::string> visited;
    std::string                     current = name;
    while (true) {
        if (!visited.insert(current).second) return false;   // 父链环，fail closed
        if (current.rfind("java/", 0) == 0) return true;     // JDK 基座截断（Recaf 同口径）
        const auto it = byName.find(current);
        if (it == byName.end()) return false;
        const auto memoIt = ownMemo.find(current);
        if (memoIt != ownMemo.end()) {
            if (!memoIt->second) return false;
        }
        else {
            const bool own = corridorPure(*it->second.session, current);
            ownMemo.emplace(current, own);
            if (!own) return false;
        }
        const std::string_view super = it->second.session->backing().superName();
        if (super.empty() || super == current) return true;
        current.assign(super);
    }
}

/// 字段是否带 ConstantValue 属性（Raw 槽名判断，与 StaticFieldDefaultInliner
/// 写入面同口径）。
bool hasConstantValueAttr(const jbc::core::ClassFile& cf, const jbc::core::MemberInfo& f)
{
    for (const jbc::core::AttrSlot& slot : f.attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, "ConstantValue")) return true;
    }
    return false;
}

/// ConstantValue 属性值解析（const 只读面，不触发 lazy 解码——decodeAttr 是
/// 可变入口）：优先读槽上已解码体（ConstantValueAttr / RawAttr 双形态），
/// 未解码则走源字节 span 直读（JVMS 4.7.2 体恰为 u2 CP 索引）。CP 值条目
/// 按 tag 分派，不支持形态返回 false。
bool constantValueOf(const jbc::core::ClassFile& cf, const jbc::core::MemberInfo& f,
                     StaticValue& out)
{
    for (const jbc::core::AttrSlot& slot : f.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "ConstantValue")) continue;
        ca::u32 idx = 0;
        if (const jbc::core::AttrBody* body = slot.peek(); body != nullptr) {
            if (body->kind() == jbc::core::AttrKind::ConstantValue) {
                idx = ca::core::tag_cast::cast<const jbc::core::ConstantValueAttr>(body)
                          ->constantValueCp;
            }
            else if (body->kind() == jbc::core::AttrKind::Raw) {
                const auto* raw = ca::core::tag_cast::cast<const jbc::core::RawAttr>(body);
                if (raw == nullptr || raw->bytes().size() != 2) return false;
                idx = (static_cast<ca::u32>(raw->bytes()[0]) << 8) |
                      static_cast<ca::u32>(raw->bytes()[1]);
            }
            else {
                return false;
            }
        }
        else if (cf.hasSource() && slot.data.length >= 2) {
            const jbc::core::ByteView src = cf.source();
            idx = (static_cast<ca::u32>(src[slot.data.offset]) << 8) |
                  static_cast<ca::u32>(src[slot.data.offset + 1]);
        }
        else {
            return false;   // 无解码体且无源字节：证据不可得，fail closed
        }
        if (!cf.cp.valid(idx)) return false;
        const jbc::core::ConstantPool::Entry& e = cf.cp.at(idx);
        out                                     = StaticValue{};
        switch (e.tag) {
        case jbc::core::CpTag::Integer:
            out.kind = StaticValue::Kind::Int;
            out.num  = e.ival;
            return true;
        case jbc::core::CpTag::Long:
            out.kind = StaticValue::Kind::Long;
            out.num  = e.lval;
            return true;
        case jbc::core::CpTag::Float:
            out.kind = StaticValue::Kind::Float;
            out.bits = static_cast<ca::u32>(e.ival);
            return true;
        case jbc::core::CpTag::Double:
            out.kind = StaticValue::Kind::Double;
            out.bits = static_cast<ca::u64>(e.lval);
            return true;
        case jbc::core::CpTag::String:
            if (!cf.cp.valid(e.a)) return false;
            out.kind = StaticValue::Kind::Str;
            out.str.assign(cf.utf8(e.a));
            return true;
        default:
            return false;
        }
    }
    return false;
}

/// 容器全域 PUTSTATIC 普查的值。
struct WriteCensus
{
    ca::usize total        = 0;   ///< 全容器写点数
    ca::usize inOwnerClinit = 0;  ///< 其中落在 owner 类 `<clinit>()V` 内的
};

/// 逐类走廊收集（scratch 会话：重解析 → 纯度门 → ConstantFolder 折叠 →
/// 「常量装载 + PUTSTATIC」对读取）。scratch 上的折叠与失败都不回传容器，
/// 任一环节失败按该类无贡献处理（fail closed）。
void collectClassFromClinit(const LoadedClass&                                   lc,
                            const std::unordered_map<std::string, WriteCensus>&  census,
                            ca::str::Utf8StringArena&                            arena,
                            StaticValueView&                                     out)
{
    auto made =
        jbc::ir::makeSessionFromBytes(jbc::core::ByteView{lc.resource->get()});
    if (made.is_err()) return;
    auto scratch = std::move(made).unwrap();

    jbc::ir::ClassUnit&  unit       = scratch->unit();
    const std::string_view thisName = unit.symbols().str(unit.thisClass);
    const ca::usize       clinitIdx = unit.findMethod("<clinit>", "()V");
    if (clinitIdx == jbc::ir::kNoMethod) return;
    if (!jbc::ir::hasCode(*scratch, clinitIdx)) return;
    if (!corridorPure(*scratch, thisName)) return;

    auto promoted = jbc::ir::promoteMethod(*scratch, clinitIdx);
    if (promoted.is_err()) return;
    auto cuRef = std::move(promoted).unwrap();

    // 复用既有常量折叠面：走廊内常量算术链在 scratch 上折叠为单条装载，
    // 折叠语义（除零 / MIN÷-1 红线、NaN 位图、移位掩宽）全部继承
    // ConstantFolder，本件不重写任何算术。
    {
        mjbase::KeyPool                       pool;
        const mjbase::ClassKey                ownerKey = mjbase::classKeyOf(pool, lc.name);
        const mjbase::MemberKey               mkey =
            mjbase::memberKeyOf(pool, ownerKey, "<clinit>", "()V");
        jbc::ir::MethodUnit&                  mu = scratch->unit().methods[clinitIdx];
        mjbase::spi::MethodScope              scope{
            mjbase::spi::MethodTarget{mkey, mu.id, unit.symbols().str(mu.name),
                                      unit.symbols().str(mu.desc), mu.access, *scratch},
            *cuRef.get()};
        mjbase::spi::MethodAnalysisManager mam(arena);
        ConstantFolder                     folder;
        (void)folder.run(scope, mam);
    }

    const jbc::ir::CodeUnit&            cu    = *cuRef.get();
    const std::vector<jbc::ir::InsnId>& order = cu.dom.order();
    for (ca::usize i = 1; i < order.size(); ++i) {
        const jbc::ir::Insn* put = cu.dom.peek(order[i]);
        if (put == nullptr || put->op != Op::PUTSTATIC) continue;
        jbc::ir::ClassSession::MemberSem ref;
        if (!scratch->memberOf(*put, ref) ||
            ref.kind != jbc::ir::ClassSession::MemberSem::Kind::Field ||
            ref.owner != thisName) {
            continue;
        }
        // 值面：顺序前驱恰为匹配族常量装载（折叠后形态）；否则该字段拒。
        const jbc::ir::Insn* load = cu.dom.peek(order[i - 1]);
        if (load == nullptr) continue;
        StaticValue value;
        if (!staticValuePush(*scratch, *load, value)) continue;
        if (!staticValueFamilyMatches(ref.desc, value.kind)) continue;
        // 字段面：字段必须存在、static 且非 volatile（volatile 桶）。
        const ca::usize fieldIdx = unit.findField(ref.name, ref.desc);
        if (fieldIdx == jbc::ir::kNoField) continue;
        const jbc::ir::FieldUnit& field = unit.fields[fieldIdx];
        if ((field.access & kAccStatic) == 0) continue;
        if ((field.access & kAccVolatile) != 0) continue;
        // 单写者：全域恰一写点且在 owner clinit（多次写冲突桶 + 非 clinit 写桶）。
        const auto cit = census.find(keyOf(ref.owner, ref.name, ref.desc));
        if (cit == census.end()) continue;
        if (cit->second.total != 1 || cit->second.inOwnerClinit != 1) continue;
        out.put(std::string(ref.owner), std::string(ref.name), std::string(ref.desc),
                std::move(value));
    }
}

/// 逐类 ConstantValue 面收编（链式来源：StaticFieldDefaultInliner 前道
/// 产物 + javac 常量字段原生形态）。
void collectClassFromAttributes(const LoadedClass&                                  lc,
                                const std::unordered_map<std::string, WriteCensus>& census,
                                StaticValueView&                                    out)
{
    jbc::ir::ClassSession& session = *lc.session;
    jbc::ir::ClassUnit&    unit    = session.unit();
    jbc::core::ClassFile&  cf      = session.backing();
    for (const jbc::ir::FieldUnit& f : unit.fields) {
        const std::string_view name = unit.symbols().str(f.name);
        const std::string_view desc = unit.symbols().str(f.desc);
        if ((f.access & kAccStatic) == 0) continue;
        if ((f.access & kAccFinal) == 0) continue;   // JVM 只对 static final 生效该属性
        if ((f.access & kAccVolatile) != 0) continue;
        StaticValue::Kind kind = StaticValue::Kind::Int;
        if (!staticValueFamilyKind(desc, kind)) continue;
        const auto cit = census.find(keyOf(lc.name, name, desc));
        if (cit != census.end() && cit->second.total != 0) continue;   // 有写点 → 走走廊面
        if (f.backingIndex >= cf.fields.size()) continue;
        StaticValue value;
        if (!constantValueOf(cf, cf.fields[f.backingIndex], value)) continue;
        if (!staticValueFamilyMatches(desc, value.kind)) continue;
        out.put(std::string(lc.name), std::string(name), std::string(desc), std::move(value));
    }
}

}   // namespace

void StaticValueView::put(std::string owner, std::string name, std::string desc,
                          StaticValue value)
{
    values_.insert_or_assign(Key{std::move(owner), std::move(name), std::move(desc)},
                             std::move(value));
}

const StaticValue* StaticValueView::find(std::string_view owner, std::string_view name,
                                         std::string_view desc) const
{
    const auto it = values_.find(Key{std::string(owner), std::string(name), std::string(desc)});
    return it == values_.end() ? nullptr : &it->second;
}

mjbase::spi::PreservedAnalyses StaticValueCollection::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    std::vector<LoadedClass> loaded;
    if (!loadAllClasses(container, loaded)) return mjbase::spi::PreservedAnalyses::all();
    if (loaded.empty()) {
        am.clearPinned<StaticValueViewAnalysis>();   // 空容器同「无内容」口径：撤钉
        return mjbase::spi::PreservedAnalyses::all();
    }

    // ── 容器全域 PUTSTATIC 普查（单写者判据的全域证据）──
    std::unordered_map<std::string, WriteCensus> census;
    std::unordered_set<std::string>              ownersWithClinitWrite;
    for (const LoadedClass& lc : loaded) {
        jbc::ir::ClassSession& session = *lc.session;
        jbc::ir::ClassUnit&    unit    = session.unit();
        for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
            if (!jbc::ir::hasCode(session, idx)) continue;
            auto promoted = jbc::ir::promoteMethod(session, idx);
            if (promoted.is_err()) continue;   // 升格失败的类：走廊/属性面判据自然拒绝
            auto               cuRef = std::move(promoted).unwrap();
            jbc::ir::CodeUnit& cu    = *cuRef.get();
            const std::string_view mname = unit.symbols().str(unit.methods[idx].name);
            const std::string_view mdesc = unit.symbols().str(unit.methods[idx].desc);
            const bool             methodIsOwnerClinit =
                mname == "<clinit>" && mdesc == "()V";
            for (const jbc::ir::InsnId id : cu.dom.order()) {
                const jbc::ir::Insn* in = cu.dom.peek(id);
                if (in == nullptr || in->op != Op::PUTSTATIC) continue;
                jbc::ir::ClassSession::MemberSem ref;
                if (!session.memberOf(*in, ref) ||
                    ref.kind != jbc::ir::ClassSession::MemberSem::Kind::Field) {
                    continue;
                }
                const std::string key = keyOf(ref.owner, ref.name, ref.desc);
                ++census[key].total;
                if (methodIsOwnerClinit && ref.owner == std::string_view(lc.name)) {
                    ++census[key].inOwnerClinit;
                    ownersWithClinitWrite.insert(std::string(ref.owner));
                }
            }
        }
    }

    // ── 逐类发布：走廊面（clinit 写）+ 属性面（ConstantValue）──
    std::unordered_map<std::string, LoadedClass> byName;
    for (LoadedClass& lc : loaded) {
        std::string name = lc.name;   // 先拷键再移动值（emplace 实参求值序无关化）
        byName.emplace(std::move(name), std::move(lc));
    }

    std::unordered_map<std::string, bool> corridorMemo;
    StaticValueView                       view;
    for (const auto& [name, lc] : byName) {
        const bool hasClinitWrites = ownersWithClinitWrite.count(name) != 0;
        bool       hasAttrCandidates = false;
        {
            jbc::ir::ClassUnit& unit = lc.session->unit();
            for (const jbc::ir::FieldUnit& f : unit.fields) {
                if ((f.access & kAccStatic) == 0 || (f.access & kAccFinal) == 0 ||
                    (f.access & kAccVolatile) != 0) {
                    continue;
                }
                StaticValue::Kind ignored = StaticValue::Kind::Int;
                if (!staticValueFamilyKind(unit.symbols().str(f.desc), ignored)) continue;
                const auto c = census.find(keyOf(name, unit.symbols().str(f.name),
                                                 unit.symbols().str(f.desc)));
                if (c != census.end() && c->second.total != 0) continue;
                if (hasConstantValueAttr(lc.session->backing(),
                                         lc.session->backing().fields[f.backingIndex])) {
                    hasAttrCandidates = true;
                    break;
                }
            }
        }
        if (!hasClinitWrites && !hasAttrCandidates) continue;
        // 发布门：本类与父链全走廊纯（门结果经 memo 跨类复用；门失败整类
        // 无贡献——物化件删 GETSTATIC 不丢父链初始化效应的安全性前提）。
        if (!superChainPure(name, byName, corridorMemo)) continue;
        try {
            if (hasClinitWrites) collectClassFromClinit(lc, census, am.arena(), view);
            collectClassFromAttributes(lc, census, view);
        }
        catch (const std::exception&) {
            // scratch 求值面容错：该类不发布，容器字节未触碰（fail closed）。
        }
    }

    if (view.empty()) {
        am.clearPinned<StaticValueViewAnalysis>();   // 无内容 → 物化件必须 no-op
        return mjbase::spi::PreservedAnalyses::all();
    }
    am.setPinned<StaticValueViewAnalysis>(std::move(view));
    return mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
