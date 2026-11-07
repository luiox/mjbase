#include "mjbase/pass/indy_site_inliner.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "jbc/type/descriptor.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/asm/classpath/indy.hpp"
#include "mjbase/asm/sem/insert_prim.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

namespace asm_classpath = mjbase::asm_classpath;
using jbc::core::ConstantPool;
using jbc::core::CpTag;
using jbc::core::Op;
using jbc::type::MethodDesc;
using jbc::type::TypeDesc;

/// 显式局部槽号编码上界：IR 不产 WIDE 前缀（Insn.a 按 u8 编码），fresh 槽
/// 越界即拒（fail-closed，见 pass 头注「改写落笔」）。
constexpr ca::u32 kMaxEncodedSlot = 255;

/// TypeDesc → 描述符首字符（槽位宽/装载族选型轴；数组前缀恒 '['）。
char descHeadOf(const TypeDesc& t) noexcept
{
    if (t.array_dims != 0) return '[';
    if (t.kind == TypeDesc::Kind::Class) return 'L';
    switch (t.primitive) {
    case jbc::type::BaseType::Byte:    return 'B';
    case jbc::type::BaseType::Char:    return 'C';
    case jbc::type::BaseType::Double:  return 'D';
    case jbc::type::BaseType::Float:   return 'F';
    case jbc::type::BaseType::Int:     return 'I';
    case jbc::type::BaseType::Long:    return 'J';
    case jbc::type::BaseType::Short:   return 'S';
    case jbc::type::BaseType::Boolean: return 'Z';
    }
    return 'I';
}

/// TypeDesc 结构等形：kind/primitive/类内部名/数组维数逐位同（跨输入串的
/// 描述符等形判据，不做任何适配——见 pass 头注「改写判据矩阵」）。
bool sameType(const TypeDesc& a, const TypeDesc& b) noexcept
{
    if (a.kind != b.kind || a.array_dims != b.array_dims) return false;
    if (a.kind == TypeDesc::Kind::Primitive) return a.primitive == b.primitive;
    return a.class_name == b.class_name;
}

/// 描述符解析（空参/坏形 Err，不抛）。params/ret 视图借用输入串。
bool parseMethod(std::string_view desc, MethodDesc& out) noexcept
{
    auto r = jbc::type::parseMethodDescriptor(desc);
    if (r.is_err()) return false;
    out = std::move(r).unwrap();
    return true;
}

/// CP Utf8 条目内容视图（tag/越界不符返回空视图）。
std::string_view utf8Of(const ConstantPool& cp, ca::u32 index) noexcept
{
    if (!cp.valid(index)) return {};
    const auto& e = cp.at(index);
    if (e.tag != CpTag::Utf8) return {};
    return std::string_view(reinterpret_cast<const char*>(e.utf8), e.utf8Len);
}

/// CP 成员引用三段视图（Fieldref/Methodref/InterfaceMethodref；结构不符返回
/// false）。isInterface 按引用条目 tag 判。
bool memberRefOf(const ConstantPool& cp, ca::u32 index, spi::IndyHandleView& out) noexcept
{
    if (!cp.valid(index)) return false;
    const auto& ref = cp.at(index);
    if (ref.tag != CpTag::Methodref && ref.tag != CpTag::InterfaceMethodref &&
        ref.tag != CpTag::Fieldref) {
        return false;
    }
    if (!cp.valid(ref.a) || !cp.valid(ref.b)) return false;
    const auto& owner = cp.at(ref.a);
    const auto& nat   = cp.at(ref.b);
    if (owner.tag != CpTag::Class || nat.tag != CpTag::NameAndType) return false;
    out.kind        = 0;   // 调用方按需覆写（MethodHandle 条目才有 reference_kind）
    out.owner       = utf8Of(cp, owner.a);
    out.name        = utf8Of(cp, nat.a);
    out.desc        = utf8Of(cp, nat.b);
    out.isInterface = ref.tag == CpTag::InterfaceMethodref;
    return !out.owner.empty();
}

/// CP 条目 → BSM 静态参数值形态（九族 + Unreadable 占位，见 SPI 头注）。
spi::IndyBsmArg bsmArgOf(const ConstantPool& cp, ca::u32 index) noexcept
{
    spi::IndyBsmArg arg;
    arg.kind = spi::IndyBsmArg::Kind::Unreadable;
    if (!cp.valid(index)) return arg;
    const auto& e = cp.at(index);
    switch (e.tag) {
    case CpTag::Integer:
        arg.kind = spi::IndyBsmArg::Kind::Int;
        arg.num  = e.ival;
        return arg;
    case CpTag::Long:
        arg.kind = spi::IndyBsmArg::Kind::Long;
        arg.num  = e.lval;
        return arg;
    case CpTag::Float:
        arg.kind = spi::IndyBsmArg::Kind::Float;
        arg.num  = e.ival;   // 原始位型（i32 解释）
        return arg;
    case CpTag::Double:
        arg.kind = spi::IndyBsmArg::Kind::Double;
        arg.num  = e.lval;   // 原始位型（i64 解释）
        return arg;
    case CpTag::String:
        arg.kind = spi::IndyBsmArg::Kind::String;
        arg.str  = utf8Of(cp, e.a);
        return arg;
    case CpTag::Class:
        arg.kind = spi::IndyBsmArg::Kind::Class;
        arg.str  = utf8Of(cp, e.a);
        return arg;
    case CpTag::MethodType:
        arg.kind = spi::IndyBsmArg::Kind::MethodType;
        arg.str  = utf8Of(cp, e.a);
        return arg;
    case CpTag::MethodHandle: {
        spi::IndyHandleView ref;
        if (!cp.valid(e.b) || !memberRefOf(cp, e.b, ref)) return arg;
        arg.kind   = spi::IndyBsmArg::Kind::MethodHandle;
        arg.handle = ref;
        arg.handle.kind = static_cast<ca::u16>(e.a);   // reference_kind
        return arg;
    }
    case CpTag::Dynamic:
        if (!cp.valid(e.b)) return arg;
        {
            const auto& nat = cp.at(e.b);
            if (nat.tag != CpTag::NameAndType || !cp.valid(nat.a) || !cp.valid(nat.b)) {
                return arg;
            }
            arg.kind = spi::IndyBsmArg::Kind::Dynamic;
            arg.str  = utf8Of(cp, nat.a);
            arg.desc = utf8Of(cp, nat.b);
            return arg;
        }
    default:
        return arg;   // 引用/NameAndType/Utf8 等非 bootstrap_arguments 形态
    }
}

/// 站点 CP 条目（CONSTANT_InvokeDynamic）→ BSM 表条目；结构不符返回 nullptr。
const jbc::core::BootstrapMethodsAttr::Entry* bsmEntryOf(
    jbc::ir::ClassSession& session, const jbc::ir::Insn& insn,
    const jbc::core::BootstrapMethodsAttr& table)
{
    const ConstantPool& cp = session.backing().cp;
    if (!cp.valid(insn.cp)) return nullptr;
    const auto& indy = cp.at(insn.cp);
    if (indy.tag != CpTag::InvokeDynamic) return nullptr;
    if (indy.a >= table.entries.size()) return nullptr;
    return &table.entries[indy.a];
}

/// 类级 BootstrapMethods 属性解码视图（属性缺失/解码失败返回 nullptr；
/// 解码产物驻留 backing，指针存活期 = 会话）。
const jbc::core::BootstrapMethodsAttr* bootstrapMethodsOf(jbc::ir::ClassSession& session)
{
    jbc::core::ClassFile& cf = session.backing();
    jbc::core::AttrSlot*  slot = cf.findClassAttr("BootstrapMethods");
    if (slot == nullptr) return nullptr;
    auto decoded = cf.decodeAttr(*slot);
    if (decoded.is_err()) return nullptr;
    return ca::core::tag_cast::dyn_cast<jbc::core::BootstrapMethodsAttr>(decoded.unwrap());
}

/// 站点事实包采集：家族 + BSM 句柄 + 静态参数按序（视图借用会话）。
/// 返回 false = 站点事实不可读（保守保留，不进 resolver 面）。
bool collectSiteFacts(jbc::ir::ClassSession& session, const jbc::ir::Insn& insn,
                      std::string_view ownerClass, std::string_view methodName,
                      std::string_view methodDesc, asm_classpath::BootstrapFamily& family,
                      spi::IndySiteFacts& facts)
{
    family = asm_classpath::BootstrapFamily::Unknown;
    facts  = spi::IndySiteFacts{};
    asm_classpath::IndyResolution resolution;
    if (!asm_classpath::resolveIndyInsn(session, insn, resolution)) return false;
    family = resolution.family;

    facts.ownerClass = ownerClass;
    facts.methodName = methodName;
    facts.methodDesc = methodDesc;
    facts.siteName   = resolution.name;
    facts.siteDesc   = resolution.desc;
    facts.bootstrap.kind        = resolution.bootstrap.kind;
    facts.bootstrap.owner       = resolution.bootstrap.owner;
    facts.bootstrap.name        = resolution.bootstrap.name;
    facts.bootstrap.desc        = resolution.bootstrap.desc;
    facts.bootstrap.isInterface = resolution.bootstrap.isInterface;

    const jbc::core::BootstrapMethodsAttr* table = bootstrapMethodsOf(session);
    if (table == nullptr) return true;   // 表缺失：bsmArgs 恒空，家族事实仍交付
    const jbc::core::BootstrapMethodsAttr::Entry* entry = bsmEntryOf(session, insn, *table);
    if (entry == nullptr) return true;
    const ConstantPool& cp = session.backing().cp;
    for (const ca::u32 arg : entry->args) {
        facts.bsmArgs.push_back(bsmArgOf(cp, arg));
    }
    return true;
}

/// 改写判据矩阵（pass 头注「改写判据矩阵」的代码形态）：门全过返回 true。
/// Ps/Rs 借站点描述符，Pt/Rt 借目标描述符（跨输入串的结构等形比较）。
bool rewriteGate(spi::ResolvedIndyTarget::Form form, std::string_view owner,
                 ca::u32 trailingMetadataArgs, const MethodDesc& site, const MethodDesc& target)
{
    const ca::usize psCount = site.params.size();
    if (trailingMetadataArgs > psCount) return false;
    const ca::usize represented = psCount - trailingMetadataArgs;

    if (form == spi::ResolvedIndyTarget::Form::Static) {
        if (represented != target.params.size()) return false;
        for (ca::usize i = 0; i < represented; ++i) {
            if (!sameType(site.params[i], target.params[i])) return false;
        }
    }
    else {
        // Virtual/Interface：首参数 = 接收者（非数组类形且内部名 == 目标 owner）。
        if (represented != target.params.size() + 1) return false;
        const TypeDesc& receiver = site.params[0];
        if (receiver.kind != TypeDesc::Kind::Class || receiver.array_dims != 0) return false;
        if (receiver.class_name != owner) return false;
        for (ca::usize i = 0; i < target.params.size(); ++i) {
            if (!sameType(site.params[i + 1], target.params[i])) return false;
        }
    }
    // 返回段恒等形。
    if (site.ret_void != target.ret_void) return false;
    if (!site.ret_void && !sameType(site.ret, target.ret)) return false;
    return true;
}

/// 实参暂存洗牌指令序列：站点实参逆参序存 fresh 局部 + represented 正参序
/// 装载（栈上恢复目标实参形）。槽位分配失败（越 255 上界）返回空。
std::vector<jbc::ir::Insn> shuffleInsns(const MethodDesc& siteDesc, ca::u32 trailing,
                                        ca::u16 maxLocals, ca::u32& nextSlot)
{
    std::vector<char> heads;
    heads.reserve(siteDesc.params.size());
    for (const TypeDesc& t : siteDesc.params) heads.push_back(descHeadOf(t));

    std::vector<ca::u32> slots(heads.size(), 0);
    ca::u32              cursor = maxLocals;
    for (ca::usize i = 0; i < heads.size(); ++i) {
        slots[i] = cursor;
        cursor += asm_sem::descSlots(heads[i]);
        if (cursor > kMaxEncodedSlot + 1) return {};   // 槽号越界：拒（IR 不产 WIDE）
    }
    nextSlot = cursor;

    std::vector<jbc::ir::Insn> out;
    out.reserve(heads.size() * 2);
    const auto familyOp = [](asm_sem::ValueFamily f, bool store) {
        using asm_sem::ValueFamily;
        switch (f) {
        case ValueFamily::Long:   return store ? Op::LSTORE : Op::LLOAD;
        case ValueFamily::Float:  return store ? Op::FSTORE : Op::FLOAD;
        case ValueFamily::Double: return store ? Op::DSTORE : Op::DLOAD;
        case ValueFamily::Ref:    return store ? Op::ASTORE : Op::ALOAD;
        default:                  return store ? Op::ISTORE : Op::ILOAD;
        }
    };
    // 逆参序存储（栈顶 = 末参）。
    for (ca::usize i = heads.size(); i > 0; --i) {
        jbc::ir::Insn store;
        store.op = familyOp(asm_sem::familyOfDesc(heads[i - 1]), true);
        store.a  = static_cast<ca::i32>(slots[i - 1]);
        out.push_back(store);
    }
    // 正参序装载（去尾元数据段）。
    for (ca::usize i = 0; i < heads.size() - trailing; ++i) {
        jbc::ir::Insn load;
        load.op = familyOp(asm_sem::familyOfDesc(heads[i]), false);
        load.a  = static_cast<ca::i32>(slots[i]);
        out.push_back(load);
    }
    return out;
}

/// 单方法处理：逐站点采集 → resolver → 判据 → 改写。返回是否有改写；
/// 整类放弃（装载/插入/lower 失败的不可回退面）返回 nullopt。
std::optional<bool> rewriteMethod(jbc::ir::ClassSession& session, jbc::ir::CodeUnit& cu,
                                  std::string_view methodName, std::string_view methodDesc,
                                  std::string_view ownerClass,
                                  const mjbase::spi::IndyResolverManager& manager)
{
    jbc::ir::CodeDom&                   dom   = cu.dom;
    const std::vector<jbc::ir::InsnId>& order = dom.order();

    // 先收集（程序序），后逆序处理：插入不扰动未处理站点的坐标。
    std::vector<jbc::ir::InsnId> sites;
    for (const jbc::ir::InsnId id : order) {
        const jbc::ir::Insn* in = dom.peek(id);
        if (in != nullptr && in->op == Op::INVOKEDYNAMIC) sites.push_back(id);
    }

    bool rewrittenAny = false;
    for (ca::usize r = sites.size(); r > 0; --r) {
        const jbc::ir::InsnId siteId = sites[r - 1];
        jbc::ir::Insn*        insn   = nullptr;
        {
            auto mutableInsn = dom.get(siteId);
            if (mutableInsn.is_err()) continue;
            insn = mutableInsn.unwrap();
        }
        const jbc::ir::Insn* in = dom.peek(siteId);
        if (in == nullptr) continue;

        asm_classpath::BootstrapFamily family = asm_classpath::BootstrapFamily::Unknown;
        spi::IndySiteFacts             facts;
        if (!collectSiteFacts(session, *in, ownerClass, methodName, methodDesc, family, facts)) {
            continue;
        }
        // 生成面语义：lambda 与字符串拼接站点不进 resolver 面（字节不动）。
        if (family == asm_classpath::BootstrapFamily::LambdaMetafactory ||
            family == asm_classpath::BootstrapFamily::StringConcatFactory) {
            continue;
        }

        auto resolved = manager.resolveFirst(facts);
        if (resolved.is_err()) continue;   // 无目标/失败：站点原样保留
        const spi::ResolvedIndyTarget& target = resolved.unwrap();
        if (target.name.empty() || target.name.front() == '<') continue;   // <init>/<clinit> 防御位

        MethodDesc siteDesc;
        MethodDesc targetDesc;
        if (!parseMethod(facts.siteDesc, siteDesc)) continue;
        if (!parseMethod(target.desc, targetDesc)) continue;
        if (!rewriteGate(target.form, target.owner, target.trailingMetadataArgs, siteDesc,
                         targetDesc)) {
            continue;
        }

        const bool shuffle = target.trailingMetadataArgs > 0;
        // 洗牌改变站点前置边界形态：锚定标签（分支/异常/调试边界）在站点上时拒。
        if (shuffle && method_rewrite_util::labelsAnchorAt(dom, siteId)) continue;

        if (shuffle) {
            ca::u32                   nextSlot = 0;
            const std::vector<jbc::ir::Insn> insns =
                shuffleInsns(siteDesc, target.trailingMetadataArgs, cu.maxLocals, nextSlot);
            if (insns.empty()) continue;   // 槽号越界：拒
            bool inserted = true;
            for (const jbc::ir::Insn& step : insns) {
                if (dom.insertBefore(siteId, step).is_err()) {
                    inserted = false;
                    break;
                }
            }
            if (!inserted) return std::nullopt;   // 插入面失败不可回退：整类放弃
            cu.maxLocals = static_cast<ca::u16>(
                std::max<ca::u32>(cu.maxLocals, nextSlot));
        }

        // 原位改写：INVOKESTATIC/INVOKEVIRTUAL + Methodref、INVOKEINTERFACE +
        // InterfaceMethodref（count 随目标描述符重算，intern 面 libjbc 自理）。
        const Op          savedOp = insn->op;
        const ca::i32     savedA  = insn->a;
        const ca::u32     savedCp = insn->cp;
        insn->op = target.form == spi::ResolvedIndyTarget::Form::Interface
                       ? Op::INVOKEINTERFACE
                       : (target.form == spi::ResolvedIndyTarget::Form::Virtual ? Op::INVOKEVIRTUAL
                                                                                : Op::INVOKESTATIC);
        const bool interned = session.setMember(
            *insn,
            target.form == spi::ResolvedIndyTarget::Form::Interface
                ? jbc::ir::ClassSession::MemberSem::Kind::InterfaceMethod
                : jbc::ir::ClassSession::MemberSem::Kind::Method,
            target.owner, target.name, target.desc);
        if (!interned) {
            insn->op = savedOp;   // 改写拒绝：恢复原形，站点保守保留
            insn->a  = savedA;
            insn->cp = savedCp;
            continue;
        }
        rewrittenAny = true;
    }
    return rewrittenAny;
}

/// 单类处理：装载 → 逐方法 promote/改写/lower → 重编码。无改写返回
/// nullopt（调用方保持容器原字节）；任一不可回退面失败亦 nullopt。
std::optional<std::vector<ca::u8>> rewriteOneClass(
    const std::vector<ca::u8>& corpus, const mjbase::spi::IndyResolverManager& manager)
{
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
    if (loaded.is_err()) return std::nullopt;   // 装载失败：整类放弃
    auto&               session = *loaded.unwrap();
    jbc::ir::ClassUnit& unit    = session.unit();
    const std::string   thisName(session.backing().thisName());

    bool rewrittenAny = false;
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        if (!jbc::ir::hasCode(session, i)) continue;
        auto promoted = jbc::ir::promoteMethod(session, i);
        if (promoted.is_err()) return std::nullopt;   // 升格失败：整类放弃
        jbc::ir::CodeUnit& cu = *promoted.unwrap().get();

        const std::string_view name = unit.symbols().str(unit.methods[i].name);
        const std::string_view desc = unit.symbols().str(unit.methods[i].desc);
        auto outcome = rewriteMethod(session, cu, name, desc, thisName, manager);
        if (!outcome.has_value()) return std::nullopt;
        if (!*outcome) continue;
        if (jbc::ir::lowerMethod(session, i).is_err()) {
            return std::nullopt;   // 重定位失败：整类放弃（会话弃置，容器不动）
        }
        rewrittenAny = true;
    }

    if (!rewrittenAny) return std::nullopt;
    auto encoded = jbc::core::writeClass(session.backing());
    if (encoded.is_err()) return std::nullopt;   // 序列化失败：整类放弃
    return std::move(encoded).unwrap();
}

}   // namespace

mjbase::spi::PreservedAnalyses IndySiteInliner::run(mjbase::model::ResourceContainer&  container,
                                                    mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;   // 本 pass 不消费分析面；签名随 GlobalPass 契约。
    // 容器级 setup（SPI 契约：一次运行调用一次；失败者本轮整体排除）。
    (void)manager_.setupAll(container);

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
    };
    std::vector<PendingWrite> pending;

    for (auto& [uri, resource] : container.getResources()) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr || cls->get().empty()) continue;
        auto rewritten = rewriteOneClass(cls->get(), manager_);
        if (!rewritten.has_value()) continue;   // 无改写/放弃：原资源保持原样
        PendingWrite item;
        item.uri.assign(uri);
        item.bytes = std::move(*rewritten);
        pending.push_back(std::move(item));
    }

    if (pending.empty()) return mjbase::spi::PreservedAnalyses::all();
    for (PendingWrite& item : pending) {
        container.replaceResource(item.uri,
                                  std::make_unique<mjbase::model::ClassResource>(
                                      item.uri, std::move(item.bytes)));
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
