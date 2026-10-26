#include "mjbase/pass/argument_array_scalarizer.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/spi/class_resource_pending_commit.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::InsnId;

/// 装载条目：会话 + 写回目标。
struct LoadedClass
{
    std::string                            name;
    mjbase::model::ClassResource*          resource = nullptr;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

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
        entry.session  = std::move(made).unwrap();
        entry.resource = const_cast<mjbase::model::ClassResource*>(cls);
        entry.name.assign(entry.session->backing().thisName());
        if (entry.name.empty()) return false;
        out.push_back(std::move(entry));
    }
    return true;
}

/// 成员三元组（owner/name/desc 全比较）。
struct MemberTriple
{
    std::string owner;
    std::string name;
    std::string desc;

    bool operator==(const MemberTriple& o) const
    {
        return owner == o.owner && name == o.name && desc == o.desc;
    }
};

struct MemberTripleHash
{
    std::size_t operator()(const MemberTriple& k) const noexcept
    {
        return std::hash<std::string>()(k.owner) * 31 ^ std::hash<std::string>()(k.name)
               ^ std::hash<std::string>()(k.desc);
    }
};

bool resolveMemberTriple(const jbc::core::ConstantPool& cp, ca::u32 refIdx, MemberTriple& out)
{
    if (!cp.valid(refIdx)) return false;
    const auto& ref = cp.at(refIdx);
    if (ref.tag != jbc::core::CpTag::Methodref
        && ref.tag != jbc::core::CpTag::InterfaceMethodref) {
        return false;
    }
    if (!cp.valid(ref.a) || !cp.valid(ref.b)) return false;
    const auto& cls = cp.at(ref.a);
    if (cls.tag != jbc::core::CpTag::Class) return false;
    const auto& nt = cp.at(ref.b);
    if (nt.tag != jbc::core::CpTag::NameAndType) return false;
    out.owner.assign(cp.utf8Of(cls.a));
    out.name.assign(cp.utf8Of(nt.a));
    out.desc.assign(cp.utf8Of(nt.b));
    return true;
}

/// 全容器方法柄钉死面（MethodHandle 引用 5..9；ldc/indy bootstrap 终落于此）。
void collectHandlePins(const jbc::core::ClassFile&                          cf,
                       std::unordered_set<MemberTriple, MemberTripleHash>& pins)
{
    const jbc::core::ConstantPool& cp = cf.cp;
    for (ca::u32 idx = 1; idx < cp.size(); ++idx) {
        const auto& entry = cp.at(idx);
        if (entry.tag != jbc::core::CpTag::MethodHandle) continue;
        if (entry.a < 5 || entry.a > 9) continue;
        MemberTriple triple;
        if (resolveMemberTriple(cp, entry.b, triple)) pins.insert(std::move(triple));
    }
}

/// 局部访问分类（load/store/iinc/ret 全族；宽形记基槽）。
struct LocalAccess
{
    ca::u32 slot   = 0;
    Op      generic = Op::NOP;   ///< 泛型形操作码（紧凑形回写目标）
};

bool localAccessOf(const jbc::ir::Insn& in, LocalAccess& out)
{
    const auto inRange = [](Op v, Op f, Op l) { return v >= f && v <= l; };
    const auto compact = [](Op v, Op f) {
        return static_cast<ca::u32>(v) - static_cast<ca::u32>(f);
    };
    switch (in.op) {
    case Op::ILOAD: case Op::FLOAD: case Op::ALOAD:
    case Op::ISTORE: case Op::FSTORE: case Op::ASTORE:
    case Op::LLOAD: case Op::DLOAD:
    case Op::LSTORE: case Op::DSTORE:
    case Op::IINC: case Op::RET:
        out = {static_cast<ca::u32>(in.a), in.op};
        return true;
    default: break;
    }
    if (inRange(in.op, Op::ILOAD_0, Op::ILOAD_3)) { out = {compact(in.op, Op::ILOAD_0), Op::ILOAD}; return true; }
    if (inRange(in.op, Op::LLOAD_0, Op::LLOAD_3)) { out = {compact(in.op, Op::LLOAD_0), Op::LLOAD}; return true; }
    if (inRange(in.op, Op::FLOAD_0, Op::FLOAD_3)) { out = {compact(in.op, Op::FLOAD_0), Op::FLOAD}; return true; }
    if (inRange(in.op, Op::DLOAD_0, Op::DLOAD_3)) { out = {compact(in.op, Op::DLOAD_0), Op::DLOAD}; return true; }
    if (inRange(in.op, Op::ALOAD_0, Op::ALOAD_3)) { out = {compact(in.op, Op::ALOAD_0), Op::ALOAD}; return true; }
    if (inRange(in.op, Op::ISTORE_0, Op::ISTORE_3)) { out = {compact(in.op, Op::ISTORE_0), Op::ISTORE}; return true; }
    if (inRange(in.op, Op::LSTORE_0, Op::LSTORE_3)) { out = {compact(in.op, Op::LSTORE_0), Op::LSTORE}; return true; }
    if (inRange(in.op, Op::FSTORE_0, Op::FSTORE_3)) { out = {compact(in.op, Op::FSTORE_0), Op::FSTORE}; return true; }
    if (inRange(in.op, Op::DSTORE_0, Op::DSTORE_3)) { out = {compact(in.op, Op::DSTORE_0), Op::DSTORE}; return true; }
    if (inRange(in.op, Op::ASTORE_0, Op::ASTORE_3)) { out = {compact(in.op, Op::ASTORE_0), Op::ASTORE}; return true; }
    return false;
}

/// 内部名 → 描述符形（数组名原样）。
std::string typeDescOf(std::string_view internalName)
{
    if (!internalName.empty() && internalName[0] == '[') return std::string(internalName);
    std::string out("L");
    out.append(internalName).append(";");
    return out;
}

/// 被调方候选（会话存续期内有效）。
struct CalleePlan
{
    MemberTriple             key;
    ca::u32                  paramSlot = 0;   ///< static：0；实例：1（槽 0 = this）
    bool                     isStatic  = true;
    ca::u32                  n         = 0;
    std::vector<std::string> elemDescs;       ///< 按下标序的新参数描述符
    std::vector<std::string> elemNames;       ///< 按下标序的 checkcast 内部名
    LoadedClass*             owner     = nullptr;
    ca::usize                methodIdx = 0;
    jbc::ir::CodeUnitRef     cu;   ///< 分析期升格保活（promote 重建语义下
                                   ///< apply 期不得再 promote，句柄复用）
    /// 取参窗（head 保留改写；junk 三条删除）。重复取同一元素的窗共用
    /// 同一下标的新参数槽。
    struct Window
    {
        InsnId              head;
        ca::u32             index = 0;
        std::vector<InsnId> junk;
    };
    std::vector<Window> windows;
    bool                dead = false;   ///< 任一判定失败 → 弃
};

/// 被调方分析：取参窗收集 + 槽触碰/逃逸审计 + 下标完整性。
void analyzeCallee(CalleePlan& plan, jbc::ir::ClassSession& session, ca::usize methodIdx)
{
    if (!jbc::ir::hasCode(session, methodIdx)) {
        plan.dead = true;
        return;
    }
    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) {
        plan.dead = true;
        return;
    }
    plan.cu                   = std::move(promoted).unwrap();
    jbc::ir::CodeUnit&        cu    = *plan.cu.get();
    const std::vector<InsnId> order = cu.dom.order();
    const ca::u32             p     = plan.paramSlot;

    // 取参窗：[aload p; intConst i; AALOAD; CHECKCAST Ti]。
    std::map<ca::u32, std::string> elemByName;   // 下标 → Ti（重复取同元素须同型）
    std::set<ca::u32>              windowAaloads;   ///< InsnId::slot 键（CodeUnit 内唯一）
    std::set<ca::u32>              windowHeads;
    for (ca::usize i = 0; i + 3 < order.size(); ++i) {
        const jbc::ir::Insn* head = cu.dom.peek(order[i]);
        const jbc::ir::Insn* cIn  = cu.dom.peek(order[i + 1]);
        const jbc::ir::Insn* aal  = cu.dom.peek(order[i + 2]);
        const jbc::ir::Insn* cc   = cu.dom.peek(order[i + 3]);
        if (head == nullptr || cIn == nullptr || aal == nullptr || cc == nullptr) continue;
        LocalAccess la;
        if (!localAccessOf(*head, la) || la.generic != Op::ALOAD || la.slot != p) continue;
        ca::i32 idx = 0;
        if (!method_rewrite_util::intValueOf(session, *cIn, idx)) continue;
        if (idx < 0 || aal->op != Op::AALOAD || cc->op != Op::CHECKCAST) continue;
        const std::string_view ti = session.classOf(*cc);
        if (ti.empty()) continue;
        // 窗内删点不得锚定标签（head 改写保锚）。
        if (method_rewrite_util::labelsAnchorAt(cu.dom, order[i + 1])
            || method_rewrite_util::labelsAnchorAt(cu.dom, order[i + 2])
            || method_rewrite_util::labelsAnchorAt(cu.dom, order[i + 3])) {
            plan.dead = true;
            return;
        }
        const ca::u32 e = static_cast<ca::u32>(idx);
        const auto    hit = elemByName.find(e);
        if (hit != elemByName.end() && hit->second != ti) {
            plan.dead = true;   // 同元素异型取参：类型证据冲突
            return;
        }
        if (hit == elemByName.end()) elemByName.emplace(e, ti);
        CalleePlan::Window w;
        w.head  = order[i];
        w.index = e;
        w.junk  = {order[i + 1], order[i + 2], order[i + 3]};
        plan.windows.push_back(std::move(w));
        windowAaloads.insert(order[i + 2].slot);
        windowHeads.insert(order[i].slot);
    }
    if (elemByName.empty()) { plan.dead = true; return; }
    // 下标完整覆盖 0..n-1（无缺口）。
    plan.n = elemByName.rbegin()->first + 1;
    if (elemByName.size() != plan.n) { plan.dead = true; return; }
    for (ca::u32 e = 0; e < plan.n; ++e) {
        plan.elemNames.push_back(elemByName.at(e));
        plan.elemDescs.push_back(typeDescOf(elemByName.at(e)));
    }
    // 参数槽触碰审计：非窗头的 aload p / 任何 store/iinc/ret 触碰 → 弃。
    for (const InsnId id : order) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) continue;
        LocalAccess la;
        if (!localAccessOf(*in, la)) continue;
        if (la.slot != p) continue;
        if (la.generic == Op::ALOAD && windowHeads.count(id.slot) != 0) continue;
        plan.dead = true;
        return;
    }
    // 逃逸审计：入参定义的全部使用边都落在窗的 aaload 上（数组不逃逸）。
    const jbc::analysis::MethodSig sig{plan.isStatic, plan.key.desc};
    const mjbase::asm_analyzer::MethodDefUse du =
        mjbase::asm_analyzer::analyzeDefUse(session.backing().cp, cu, sig);
    if (!du.ok() || du.truncated()) {
        plan.dead = true;
        return;
    }
    for (const auto* e : du.usesOf(mjbase::asm_analyzer::DefId::parameter(p))) {
        // aload p 把参数定义原样压栈，aaload 处会再记一条栈使用边——两类
        // 位点（窗头局部读 + 窗内 aaload 栈用）都是窗内合法使用。
        if (windowHeads.count(e->use.slot) == 0 && windowAaloads.count(e->use.slot) == 0) {
            plan.dead = true;
            return;
        }
    }
}

/// 调用方方法描述符 → 引用型入参槽表（实例方法槽 0 = this，声明型为
/// 调用方类自身）；基本型只推槽号（J/D 宽 2），不产出类型证据。
void collectCallerRefParams(std::string_view desc, bool isStatic,
                            std::string_view thisClass,
                            std::map<ca::u32, std::string>& out)
{
    const ca::usize endP = desc.find(')');
    if (desc.empty() || desc[0] != '(' || endP == std::string_view::npos) return;
    ca::u32 slot = 0;
    if (!isStatic) {
        out.emplace(0, std::string(thisClass));
        slot = 1;
    }
    ca::usize k = 1;
    while (k < endP) {
        const ca::usize start = k;
        ca::u32         width = 1;
        const char      c     = desc[k];
        if (c == 'L') {
            while (k < endP && desc[k] != ';') ++k;
            if (k >= endP) return;
            ++k;
            // 对象型去 L/; 壳出内部名（与 checkcast 类名同形）。
            out.emplace(slot, std::string(desc.substr(start + 1, k - start - 2)));
        } else if (c == '[') {
            while (k < endP && desc[k] == '[') ++k;
            if (k < endP && desc[k] == 'L') {
                while (k < endP && desc[k] != ';') ++k;
                if (k >= endP) return;
            }
            ++k;
            out.emplace(slot, std::string(desc.substr(start, k - start)));
        } else {
            if (c == 'J' || c == 'D') width = 2;
            ++k;
        }
        slot += width;
    }
}

/// 调用方实参类型上下文（切片 2 对齐面）：入参声明槽型 + 惰性 def-use
/// （仅 aload 实参需要到达定值证据时才构建，单方法至多一次）。
struct CallerArgCtx
{
    bool                           isStatic = false;
    std::string                    thisClass;
    std::map<ca::u32, std::string> refTypeBySlot;   ///< 引用型入参槽 → 声明内部名
    jbc::ir::CodeUnit*             cu       = nullptr;
    std::string                    desc;
    std::optional<mjbase::asm_analyzer::MethodDefUse> du;
};

/// 装箱工厂 owner → valueOf 入参基本类型字母；非装箱族返回空。
std::string_view boxPrimOf(std::string_view owner)
{
    const std::string_view known[] = {"java/lang/Integer",   "java/lang/Long", "java/lang/Double",
                                      "java/lang/Float",     "java/lang/Short", "java/lang/Byte",
                                      "java/lang/Character", "java/lang/Boolean"};
    const std::string_view prims[] = {"I", "J", "D", "F", "S", "B", "C", "Z"};
    for (ca::usize i = 0; i < 8; ++i) {
        if (owner == known[i]) return prims[i];
    }
    return {};
}

/// 单值基本型生产者：op 族须与装箱工厂入参字母匹配（B/S/C/Z 走 I 面）。
bool primPushMatches(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                     std::string_view prim)
{
    using jbc::core::Op;
    const auto kLong = jbc::ir::ClassSession::ConstSem::Kind::Long;
    const auto kFlt  = jbc::ir::ClassSession::ConstSem::Kind::Float;
    const auto kDbl  = jbc::ir::ClassSession::ConstSem::Kind::Double;
    if (prim == "I" || prim == "B" || prim == "S" || prim == "C" || prim == "Z") {
        if ((in.op >= Op::ICONST_M1 && in.op <= Op::ICONST_5) || in.op == Op::BIPUSH
            || in.op == Op::SIPUSH || in.op == Op::ILOAD
            || (in.op >= Op::ILOAD_0 && in.op <= Op::ILOAD_3)) {
            return true;
        }
        ca::i32 iv = 0;
        return method_rewrite_util::intValueOf(session, in, iv);
    }
    jbc::ir::ClassSession::ConstSem c;
    if (prim == "J") {
        if (in.op == Op::LCONST_0 || in.op == Op::LCONST_1 || in.op == Op::LLOAD
            || (in.op >= Op::LLOAD_0 && in.op <= Op::LLOAD_3)) {
            return true;
        }
        return in.op == Op::LDC2_W && session.constOf(in, c) && c.kind == kLong;
    }
    if (prim == "F") {
        if ((in.op >= Op::FCONST_0 && in.op <= Op::FCONST_2) || in.op == Op::FLOAD
            || (in.op >= Op::FLOAD_0 && in.op <= Op::FLOAD_3)) {
            return true;
        }
        return (in.op == Op::LDC || in.op == Op::LDC_W) && session.constOf(in, c)
            && c.kind == kFlt;
    }
    if (prim == "D") {
        if ((in.op >= Op::DCONST_0 && in.op <= Op::DCONST_1) || in.op == Op::DLOAD
            || (in.op >= Op::DLOAD_0 && in.op <= Op::DLOAD_3)) {
            return true;
        }
        return in.op == Op::LDC2_W && session.constOf(in, c) && c.kind == kDbl;
    }
    return false;
}

/// 单值生产者（String.valueOf 前置面）：基本型/引用常量/aconst_null/aload。
bool isSingleValueProducer(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in)
{
    static constexpr std::string_view kAllPrims[] = {"I", "J", "F", "D"};
    for (const std::string_view pr : kAllPrims) {
        if (primPushMatches(session, in, pr)) return true;
    }
    if (in.op == Op::ACONST_NULL) return true;
    LocalAccess la;
    if (localAccessOf(in, la) && la.generic == Op::ALOAD) return true;
    jbc::ir::ClassSession::ConstSem c;
    if (session.constOf(in, c)) {
        return c.kind == jbc::ir::ClassSession::ConstSem::Kind::String
            || c.kind == jbc::ir::ClassSession::ConstSem::Kind::Class;
    }
    return false;
}

bool argMatchesTi(const jbc::ir::ClassSession& session, const jbc::ir::Insn& arg,
                  jbc::ir::InsnId argId, std::string_view tiName, CallerArgCtx& actx);

/// 实参组类型守卫（切片 3 面向）：组净效果须恰为压入 1 个引用。已判形：
/// 单指令（argMatchesTi 全面）；双指令装箱链 [单值生产者;
/// <wrapper>.valueOf(<prim>) / String.valueOf(...)]——产出型由调用决定，
/// 生产者只做宽度形态校验。其余形态 fail-closed 拒收。
bool argGroupMatchesTi(const jbc::ir::ClassSession& session, jbc::ir::CodeUnit& cu,
                       const std::vector<InsnId>& order, ca::usize from, ca::usize to,
                       std::string_view tiName, CallerArgCtx& actx)
{
    const ca::usize len = to - from;
    if (len == 1) {
        const jbc::ir::Insn* a = cu.dom.peek(order[from]);
        return a != nullptr && argMatchesTi(session, *a, order[from], tiName, actx);
    }
    if (len != 2) return false;
    const jbc::ir::Insn* a = cu.dom.peek(order[from]);
    const jbc::ir::Insn* b = cu.dom.peek(order[from + 1]);
    if (a == nullptr || b == nullptr || b->op != Op::INVOKESTATIC) return false;
    jbc::ir::ClassSession::MemberSem ms;
    if (!session.memberOf(*b, ms)
        || ms.kind != jbc::ir::ClassSession::MemberSem::Kind::Method) {
        return false;
    }
    if (ms.name == "valueOf") {
        const std::string_view prim = boxPrimOf(ms.owner);
        if (prim.empty()) return false;
        std::string want;
        want.reserve(ms.owner.size() + prim.size() + 4);
        want.append("(").append(prim).append(")L").append(ms.owner).append(";");
        if (ms.desc != want) return false;
        if (!primPushMatches(session, *a, prim)) return false;
        return tiName == ms.owner || tiName == "java/lang/Object";
    }
    if (ms.owner == "java/lang/String" && ms.name == "valueOf" && ms.desc.size() >= 19
        && ms.desc.substr(ms.desc.size() - 19) == ")Ljava/lang/String;") {
        if (!isSingleValueProducer(session, *a)) return false;
        return tiName == "java/lang/String" || tiName == "java/lang/Object";
    }
    return false;
}

constexpr ca::usize kMaxArgInsns = 8;   ///< 实参组长度上界（扫描有界）

/// 单指令实参类型守卫（切片 2 对齐面）：
///   - aconst_null → 任意引用 Ti；
///   - ldc String → Ti ∈ {String, Object}；ldc Class 常量 → Ti ∈ {常量类, Object}；
///   - aload s → 槽类型提升：读点到达定值恰为调用方同槽入参定值（单定义、
///     未截断——槽未被重存储/多前驱合流）时 Ti ∈ {声明型, Object}，否则
///     仅 Object。多指令实参形态（装箱工厂等）挂账切片 3，与窗形扩展一并。
bool argMatchesTi(const jbc::ir::ClassSession& session, const jbc::ir::Insn& arg,
                  jbc::ir::InsnId argId, std::string_view tiName, CallerArgCtx& actx)
{
    if (arg.op == Op::ACONST_NULL) return true;   // null 匹配任意引用 Ti
    jbc::ir::ClassSession::ConstSem cs;
    if (session.constOf(arg, cs)) {
        if (cs.kind == jbc::ir::ClassSession::ConstSem::Kind::String) {
            return tiName == "java/lang/String" || tiName == "java/lang/Object";
        }
        if (cs.kind == jbc::ir::ClassSession::ConstSem::Kind::Class) {
            return tiName == cs.str || tiName == "java/lang/Object";
        }
        return false;   // 数值 CP 常量入引用数组位未装箱，字节码不可验证
    }
    LocalAccess la;
    if (localAccessOf(arg, la) && la.generic == Op::ALOAD) {
        if (tiName == "java/lang/Object") return true;
        if (!actx.du.has_value()) {
            const jbc::analysis::MethodSig sig{actx.isStatic, actx.desc};
            actx.du.emplace(mjbase::asm_analyzer::analyzeDefUse(session.backing().cp, *actx.cu, sig));
        }
        if (!actx.du->ok() || actx.du->truncated()) return false;
        const mjbase::asm_analyzer::LocalAccess* read = nullptr;
        for (const mjbase::asm_analyzer::LocalAccess* ra : actx.du->readsOfLocal(la.slot)) {
            if (ra->insn.slot == argId.slot) {
                read = ra;
                break;
            }
        }
        if (read == nullptr || read->defs.size() != 1 || read->defs.truncated()) return false;
        const mjbase::asm_analyzer::DefId d = read->defs.at(0);
        if (d.kind != mjbase::asm_analyzer::DefId::Kind::Parameter || d.id != la.slot) return false;
        const auto hit = actx.refTypeBySlot.find(la.slot);
        if (hit == actx.refTypeBySlot.end()) return false;
        return tiName == hit->second;
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses ArgumentArrayScalarizer::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;
    std::vector<LoadedClass> loaded;
    if (!loadAllClasses(container, loaded)) return mjbase::spi::PreservedAnalyses::all();
    if (loaded.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 句柄钉死面。
    std::unordered_set<MemberTriple, MemberTripleHash> pins;
    for (const LoadedClass& lc : loaded) {
        collectHandlePins(lc.session->backing(), pins);
    }

    // 候选收集：static + ([Ljava/lang/Object;)R + 无钉 + 本类内同名同描述符唯一
    // （隐藏面由调用侧 owner 校验兜底：CP owner ≠ 声明类的位点整族拒收）。
    std::vector<CalleePlan> plans;
    for (LoadedClass& lc : loaded) {
        jbc::ir::ClassSession&  session = *lc.session;
        jbc::ir::ClassUnit&     u       = session.unit();
        const jbc::ir::SymbolPool& sym  = u.symbols();
        for (ca::usize idx = 0; idx < u.methods.size(); ++idx) {
            const jbc::ir::MethodUnit& m = u.methods[idx];
            const std::string name(std::string_view(sym.str(m.name)));
            const std::string desc(std::string_view(sym.str(m.desc)));
            if (desc.compare(0, 21, "([Ljava/lang/Object;)") != 0) continue;
            const MemberTriple key{lc.name, name, desc};
            if (pins.count(key) != 0) continue;
            ca::u32 sameCount = 0;
            for (const jbc::ir::MethodUnit& mm : u.methods) {
                if (std::string_view(sym.str(mm.name)) == name
                    && std::string_view(sym.str(mm.desc)) == desc) {
                    ++sameCount;
                }
            }
            if (sameCount != 1) continue;
            CalleePlan plan;
            plan.key       = key;
            plan.isStatic  = (m.access & mjbase::hierarchy::kAccStatic) != 0;
            plan.paramSlot = plan.isStatic ? 0 : 1;
            plan.owner     = &lc;
            plan.methodIdx = idx;
            analyzeCallee(plan, session, idx);
            plans.push_back(std::move(plan));
        }
    }

    // 调用侧扫描：全部 INVOKESTATIC 位点按 (name, desc) 匹配候选；任一位
    // 点非规范窗 / owner 非声明类 → 候选弃。编辑按（调用方法 × 候选）分组，
    // 候选死亡只丢自己的组。
    struct CallerEdit
    {
        LoadedClass*           owner     = nullptr;
        ca::usize              methodIdx = 0;
        jbc::ir::CodeUnitRef   cu;   ///< 分析期升格保活
        std::vector<InsnId>    eraseIds;
        std::vector<ca::usize> planIdx;   ///< 本编辑涉及的候选（存活才应用）
    };
    std::vector<CallerEdit> callerEdits;
    for (LoadedClass& lc : loaded) {
        jbc::ir::ClassSession& session = *lc.session;
        jbc::ir::ClassUnit&    unit    = session.unit();
        for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
            if (!jbc::ir::hasCode(session, idx)) continue;
            // 已升格（被调方分析期）直接复用——promote 幂等语义是重建，
            // 重建会使既有 CodeUnitRef/InsnId 全部失效。
            jbc::ir::CodeUnit* cuPtr = unit.methods[idx].code.get();
            if (cuPtr == nullptr) {
                auto promoted = jbc::ir::promoteMethod(session, idx);
                if (promoted.is_err()) continue;
                cuPtr = std::move(promoted).unwrap().get();
            }
            jbc::ir::CodeUnit&        cu = *cuPtr;
            const std::vector<InsnId> order = cu.dom.order();
            const ca::usize           n     = order.size();
            std::vector<InsnId>       methodErases;
            std::set<ca::usize>       methodPlans;
            // 切片 2：调用方实参类型上下文（入参声明槽型，惰性 def-use）。
            CallerArgCtx actx;
            actx.isStatic  = (unit.methods[idx].access & mjbase::hierarchy::kAccStatic) != 0;
            actx.thisClass = lc.name;
            actx.cu        = &cu;
            const jbc::ir::SymbolPool& symC = unit.symbols();
            actx.desc      = std::string(std::string_view(symC.str(unit.methods[idx].desc)));
            collectCallerRefParams(actx.desc, actx.isStatic, lc.name, actx.refTypeBySlot);
            for (ca::usize t = 0; t < n; ++t) {
                const jbc::ir::Insn* inv = cu.dom.peek(order[t]);
                if (inv == nullptr
                    || (inv->op != Op::INVOKESTATIC && inv->op != Op::INVOKEVIRTUAL
                        && inv->op != Op::INVOKESPECIAL)) {
                    continue;
                }
                jbc::ir::ClassSession::MemberSem sem;
                if (!session.memberOf(*inv, sem)) continue;
                ca::usize planIdx = plans.size();
                for (ca::usize ci = 0; ci < plans.size(); ++ci) {
                    if (plans[ci].key.name == sem.name && plans[ci].key.desc == sem.desc) {
                        planIdx = ci;
                        break;
                    }
                }
                if (planIdx == plans.size()) continue;
                CalleePlan& plan = plans[planIdx];
                if (plan.dead) continue;   // 已死候选的残余位点静默跳过
                if ((inv->op == Op::INVOKESTATIC) != plan.isStatic) continue;
                if (sem.owner != plan.key.owner) { plan.dead = true; continue; }
                // 规范装箱窗（切片 3 顺序扫描形）：[长度常量; anewarray
                // Object; (dup; 实参组; intConst e; aastore)×m; invoke]。
                // 实参组非空、至多 kMaxArgInsns 条（装箱工厂链）；窗头锚定
                // 回扫（组不等长），元素边界回溯容嵌套同形序列。
                const ca::usize m      = plan.n;
                const ca::usize maxWin = 2 + m * (3 + kMaxArgInsns);
                if (t < 4 * m + 2) { plan.dead = true; continue; }
                bool                ok     = false;
                std::vector<InsnId> windowErases;
                for (ca::usize back = 4 * m + 1; !ok && back <= maxWin && back <= t;
                     ++back) {
                    const ca::usize wsTry = t - back;
                    ca::i32         len   = 0;
                    const jbc::ir::Insn* lenIn  = cu.dom.peek(order[wsTry]);
                    const jbc::ir::Insn* newArr = cu.dom.peek(order[wsTry + 1]);
                    if (lenIn == nullptr || newArr == nullptr
                        || !method_rewrite_util::intValueOf(session, *lenIn, len)
                        || static_cast<ca::u32>(len) != m || newArr->op != Op::ANEWARRAY
                        || session.classOf(*newArr) != "java/lang/Object") {
                        continue;
                    }
                    std::vector<InsnId> erases{order[wsTry], order[wsTry + 1]};
                    ca::usize           c      = wsTry + 2;
                    bool                scanOk = true;
                    for (ca::usize e = 0; scanOk && e < m; ++e) {
                        if (c + 4 > t) {
                            scanOk = false;
                            break;
                        }
                        const jbc::ir::Insn* dup = cu.dom.peek(order[c]);
                        if (dup == nullptr || dup->op != Op::DUP) {
                            scanOk = false;
                            break;
                        }
                        bool matched = false;
                        for (ca::usize bound = c + 1;
                             !matched && bound + 1 < t && bound - c - 1 < kMaxArgInsns;
                             ++bound) {
                            const jbc::ir::Insn* idxIn = cu.dom.peek(order[bound]);
                            const jbc::ir::Insn* store = cu.dom.peek(order[bound + 1]);
                            ca::i32              ix    = 0;
                            if (idxIn == nullptr || store == nullptr) break;
                            if (!method_rewrite_util::intValueOf(session, *idxIn, ix)
                                || static_cast<ca::u32>(ix) != e
                                || store->op != Op::AASTORE) {
                                continue;
                            }
                            if (!argGroupMatchesTi(session, cu, order, c + 1, bound,
                                                   plan.elemNames[e], actx)) {
                                continue;   // 试下一候选边界（嵌套同形序列）
                            }
                            erases.push_back(order[c]);          // dup
                            erases.push_back(order[bound]);      // 下标
                            erases.push_back(order[bound + 1]);  // aastore
                            c       = bound + 2;
                            matched = true;
                        }
                        if (!matched) scanOk = false;
                    }
                    if (!scanOk || c != t) continue;
                    ok           = true;
                    windowErases = std::move(erases);
                }
                if (!ok) { plan.dead = true; continue; }
                for (const InsnId id : windowErases) {
                    if (method_rewrite_util::labelsAnchorAt(cu.dom, id)) {
                        plan.dead = true;
                        ok = false;
                        break;
                    }
                }
                if (!ok) continue;
                methodErases.insert(methodErases.end(), windowErases.begin(),
                                    windowErases.end());
                methodPlans.insert(planIdx);
            }
            if (!methodErases.empty()) {
                CallerEdit edit;
                edit.owner     = &lc;
                edit.methodIdx = idx;
                edit.cu        = jbc::ir::CodeUnitRef(lc.session, &cu);
                edit.eraseIds  = std::move(methodErases);
                edit.planIdx   = std::vector<ca::usize>(methodPlans.begin(),
                                                        methodPlans.end());
                callerEdits.push_back(std::move(edit));
            }
        }
    }

    // 存活候选须至少有一个调用窗（无调用点不改写）。
    for (ca::usize ci = 0; ci < plans.size(); ++ci) {
        if (plans[ci].dead) continue;
        bool called = false;
        for (const CallerEdit& edit : callerEdits) {
            if (std::find(edit.planIdx.begin(), edit.planIdx.end(), ci) != edit.planIdx.end()) {
                called = true;
                break;
            }
        }
        if (!called) plans[ci].dead = true;
    }

    // 应用阶段一：被调方改写 + 调用方删窗（同会话内），lower 后统一暂存
    // 字节（同类兼具两种角色时按最终态一次暂存）。
    mjbase::spi::ClassResourcePendingCommit pending;
    std::set<LoadedClass*>                  touched;
    bool                                    anyModified = false;

    for (CallerEdit& edit : callerEdits) {
        // 只应用存活候选的编辑：涉及任一死亡候选的整方法跳过（fail-closed，
        // 同方法内窗的 id 集按方法混装，无法按候选精确剥离）。
        bool allAlive = true;
        for (const ca::usize ci : edit.planIdx) {
            if (plans[ci].dead) allAlive = false;
        }
        if (!allAlive) continue;
        LoadedClass&           lc      = *edit.owner;
        jbc::ir::ClassSession& session = *lc.session;
        jbc::ir::CodeUnit&     cu      = *edit.cu.get();
        jbc::ir::CodeEditor    editor(cu);
        if (editor.eraseBatch(edit.eraseIds,
                              jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                .is_err()
            || jbc::ir::lowerMethod(session, edit.methodIdx).is_err()) {
            // 应用期失败（判定期已排除的防御路径）：联动弃目标——desc
            // 维持不变；同候选已删窗的其他调用方与旧签名失配属内部错误
            // 面（判定期形态守卫应保证应用期零失败），不静默吞。
            for (const ca::usize ci : edit.planIdx) plans[ci].dead = true;
            continue;
        }
        anyModified = true;
        touched.insert(&lc);
    }

    for (CalleePlan& plan : plans) {
        if (plan.dead) continue;
        LoadedClass&           lc      = *plan.owner;
        jbc::ir::ClassSession& session = *lc.session;
        jbc::ir::CodeUnit&     cu      = *plan.cu.get();
        const ca::u32      p     = plan.paramSlot;
        const ca::u32      shift = plan.n - 1;
        // 局部平移：slot > p 整体 +shift（恒写泛型形——对 ≤3 槽同样合法）。
        if (shift > 0) {
            for (const InsnId id : cu.dom.order()) {
                auto got = cu.dom.get(id);
                if (got.is_err()) break;
                jbc::ir::Insn* in = std::move(got).unwrap();
                LocalAccess    la;
                if (!localAccessOf(*in, la)) continue;
                if (la.slot <= p) continue;
                in->op = la.generic;
                in->a  = static_cast<ca::i32>(la.slot + shift);
            }
            cu.maxLocals = static_cast<ca::u16>(cu.maxLocals + shift);
        }
        // 取参窗：head 改 ALOAD p+i（泛型形），junk 三条删。
        jbc::ir::CodeEditor editor(cu);
        bool                applyOk = true;
        for (const CalleePlan::Window& w : plan.windows) {
            auto got = cu.dom.get(w.head);
            if (got.is_err()) {
                applyOk = false;
                break;
            }
            jbc::ir::Insn* head = std::move(got).unwrap();
            head->op   = Op::ALOAD;
            head->a    = static_cast<ca::i32>(p + w.index);
            head->wide = false;
            if (editor.eraseBatch(w.junk,
                                  jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                    .is_err()) {
                applyOk = false;
                break;
            }
        }
        if (!applyOk || jbc::ir::lowerMethod(session, plan.methodIdx).is_err()) {
            plan.dead = true;
            continue;
        }
        anyModified = true;
        touched.insert(&lc);
    }

    if (!anyModified) return mjbase::spi::PreservedAnalyses::all();
    for (LoadedClass* lc : touched) {
        auto written = jbc::core::writeClass(lc->session->backing());
        if (written.is_err()) return mjbase::spi::PreservedAnalyses::all();
        pending.stage(lc->resource, std::move(written).unwrap());
    }
    pending.commit();

    // 应用阶段二：描述符同步（声明面 + 全部引用面）。
    mjbase::asm_remapper::MemberRemapPlan remap;
    for (const CalleePlan& plan : plans) {
        if (plan.dead) continue;
        const std::string ret(plan.key.desc, plan.key.desc.find(')') + 1);
        std::string       newDesc = "(";
        for (const std::string& d : plan.elemDescs) newDesc += d;
        newDesc += ")" + ret;
        remap.mapMethod(plan.key.owner, plan.key.name, plan.key.desc, plan.key.owner,
                        plan.key.name, newDesc);
    }
    auto remapped = mjbase::asm_remapper::applyResourceRemapPlan(container, remap);
    if (remapped.is_err()) return mjbase::spi::PreservedAnalyses::all();
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
