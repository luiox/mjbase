#include "mjbase/workspace/class_mutation_service.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "jbc/analysis/frame.hpp"
#include "jbc/core/insn_codec.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/error.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/jasm/resource_jasm_service.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/class_resource_pending_commit.hpp"
#include "mjbase/workspace/classfile_support.hpp"

namespace mjbase::workspace {
namespace {

using jbc::core::CodeAttr;
using jbc::core::Op;
using jbc::core::RawInsn;
using jbc::core::RawAttr;
using jbc::ir::ClassSession;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using jbc::ir::kNoInsn;
using support::utf8Text;

/// @brief NEWARRAY 元素类型码（JVMS 6.5 atable；jbc 未提供具名常量）。
constexpr ca::i32 kAtypeBoolean = 4;
constexpr ca::i32 kAtypeLong    = 11;

WorkspaceErrorInfo error(WorkspaceError code, const std::string& operation,
                         const std::string& detail)
{
    return WorkspaceErrorInfo{code, operation + ": " + detail};
}

jbc::core::MemberInfo* findMethod(jbc::core::ClassFile& cf, const MemberSelector& selector)
{
    for (auto& method : cf.methods) {
        if (utf8Text(cf, method.nameCp) == selector.name &&
            utf8Text(cf, method.descCp) == selector.descriptor)
            return &method;
    }
    return nullptr;
}

jbc::core::MemberInfo* findField(jbc::core::ClassFile& cf, const MemberSelector& selector)
{
    for (auto& field : cf.fields) {
        if (utf8Text(cf, field.nameCp) == selector.name &&
            utf8Text(cf, field.descCp) == selector.descriptor)
            return &field;
    }
    return nullptr;
}

// ── 访问位校验（照旧直迁 validateVisibility/validateAccess）──────────────

WorkspaceErrorInfo validateVisibility(ca::i32 access, const std::string& operation)
{
    const ca::i32 visibility = access & (kAccPublic | kAccPrivate | kAccProtected);
    if (visibility != 0 && visibility != kAccPublic && visibility != kAccPrivate &&
        visibility != kAccProtected)
        return error(WorkspaceError::InvalidArgument, operation,
                     "public, private and protected are mutually exclusive");
    return WorkspaceErrorInfo{};
}

WorkspaceErrorInfo validateAccess(ca::i32 access, AnnotationTarget target,
                                  const std::string& operation)
{
    WorkspaceErrorInfo status = validateVisibility(access, operation);
    if (status.is_err()) return status;
    ca::i32 allowed = 0;
    switch (target) {
    case AnnotationTarget::Class:
        allowed = kAccPublic | kAccFinal | kAccSuper | kAccInterface | kAccAbstract |
                  kAccSynthetic | kAccAnnotation | kAccEnum | kAccModule | kAccRecord |
                  kAccDeprecated | kAccSealed;
        if ((access & kAccFinal) && (access & kAccAbstract))
            return error(WorkspaceError::InvalidArgument, operation,
                         "class cannot be final and abstract");
        if ((access & kAccAnnotation) && !(access & kAccInterface))
            return error(WorkspaceError::InvalidArgument, operation,
                         "annotation class must be an interface");
        if ((access & kAccInterface) && !(access & kAccAbstract))
            return error(WorkspaceError::InvalidArgument, operation,
                         "interface must be abstract");
        break;
    case AnnotationTarget::Field:
        allowed = kAccPublic | kAccPrivate | kAccProtected | kAccStatic | kAccFinal |
                  kAccVolatile | kAccTransient | kAccSynthetic | kAccEnum | kAccDeprecated;
        if ((access & kAccFinal) && (access & kAccVolatile))
            return error(WorkspaceError::InvalidArgument, operation,
                         "field cannot be final and volatile");
        break;
    case AnnotationTarget::Method:
        allowed = kAccPublic | kAccPrivate | kAccProtected | kAccStatic | kAccFinal |
                  kAccSynchronized | kAccBridge | kAccVarargs | kAccNative | kAccAbstract |
                  kAccStrict | kAccSynthetic | kAccDeprecated;
        if ((access & kAccAbstract) &&
            (access & (kAccPrivate | kAccStatic | kAccFinal | kAccSynchronized | kAccNative |
                       kAccStrict)))
            return error(WorkspaceError::InvalidArgument, operation,
                         "abstract method has an incompatible access flag");
        break;
    }
    if ((access & ~allowed) != 0)
        return error(WorkspaceError::InvalidArgument, operation,
                     "access contains invalid flag bits");
    return WorkspaceErrorInfo{};
}

// ── 访问位增量编辑（change_access）─────────────────────────────────

/// @brief "deprecated" 名单项的归属：Deprecated 属性（非 access flag）。
constexpr const char* kDeprecatedFlagName = "deprecated";

/// @brief Deprecated 属性增删（0x20000 是类属性不是 access flag；映射到
///        属性编辑，禁止静默丢弃）。零长度属性即 JVMS 的 Deprecated 形态。
void setDeprecatedAttribute(jbc::core::ClassFile& cf, bool present)
{
    jbc::core::AttrSlot* existing = support::findAttr(cf, cf.attrs, "Deprecated");
    if (present && existing == nullptr) {
        cf.attrs.push_back(jbc::core::AttrSlot::makeNew(
            support::internUtf8(cf, "Deprecated"),
            std::make_unique<RawAttr>(std::vector<ca::u8>{})));
    }
    else if (!present && existing != nullptr) {
        for (auto it = cf.attrs.begin(); it != cf.attrs.end(); ++it) {
            if (cf.cp.utf8Equals(it->nameCp, "Deprecated")) {
                cf.attrs.erase(it);
                return;
            }
        }
    }
}

/// @brief flag 名单 → 位掩码（change_access 增量语义的解析面）。
///
/// 未知名报错并列出 class 目标合法 flag 全集；"deprecated" 单列（Deprecated
/// 属性，非 access flag——错误提示一并注明）。
WorkspaceErrorInfo parseClassAccessFlagList(const std::vector<std::string>& names,
                                            const std::string&              group,
                                            ca::u32&                        mask,
                                            bool&                           deprecated)
{
    static const std::pair<const char*, ca::u32> kClassFlags[] = {
        {"public", kAccPublic},     {"final", kAccFinal},
        {"super", kAccSuper},       {"interface", kAccInterface},
        {"abstract", kAccAbstract}, {"synthetic", kAccSynthetic},
        {"annotation", kAccAnnotation}, {"enum", kAccEnum},
        {"module", kAccModule},     {"record", kAccRecord},
        {"sealed", kAccSealed},
    };
    mask       = 0;
    deprecated = false;
    for (const std::string& name : names) {
        if (name == kDeprecatedFlagName) {
            deprecated = true;
            continue;
        }
        bool known = false;
        for (const auto& entry : kClassFlags) {
            if (name == entry.first) {
                mask |= entry.second;
                known = true;
                break;
            }
        }
        if (known) continue;
        std::string legal;
        for (const auto& entry : kClassFlags) {
            if (!legal.empty()) legal += ", ";
            legal += entry.first;
        }
        return error(WorkspaceError::InvalidArgument, "change class access",
                     "unknown access flag '" + group + ": " + name +
                         "' (legal flags: " + legal + "; '" + kDeprecatedFlagName +
                         "' is a class attribute, not an access flag)");
    }
    return WorkspaceErrorInfo{};
}

// ── 常量物化 ─────────────────────────────────────────────────────────────
//
// libjbc 常量池只追加面覆盖 Utf8/String/Class/Integer/Long/Float/Double 与
// 成员引用（经会话 intern 去重）；MethodHandle/MethodType/Dynamic/
// InvokeDynamic 无追加 API（主线缺口，迁移报告项）——对应请求在入口拒绝。

WorkspaceErrorInfo validateHandle(const HandleSpec& spec)
{
    if (spec.tag < 1 || spec.tag > 9 || spec.owner.empty() || spec.name.empty() ||
        spec.descriptor.empty()) {
        return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                  "handle requires tag 1..9, owner, name and descriptor"};
    }
    return WorkspaceErrorInfo{};
}

/// @brief 校验常量规格可物化（Handle/Dynamic 因 libjbc CP 追加缺口被拒）。
WorkspaceErrorInfo validateConstant(const ConstantSpec& spec, ca::usize depth = 0)
{
    if (depth > 8)
        return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                  "constant dynamic nesting exceeds 8"};
    switch (spec.kind) {
    case ConstantKind::String:
    case ConstantKind::Integer:
    case ConstantKind::Long:
    case ConstantKind::Float:
    case ConstantKind::Double:
        return WorkspaceErrorInfo{};
    case ConstantKind::Type:
        if (spec.descriptor.empty())
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "type constant descriptor is required"};
        return WorkspaceErrorInfo{};
    case ConstantKind::Handle:
        return validateHandle(spec.handle);
    case ConstantKind::Dynamic:
    {
        if (spec.name.empty() || spec.descriptor.empty())
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "constant dynamic name and descriptor are required"};
        if (spec.bootstrapArguments.size() > 128)
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "bootstrap argument count exceeds 128"};
        WorkspaceErrorInfo status = validateHandle(spec.bootstrapMethod);
        if (status.is_err()) return status;
        for (const ConstantSpec& argument : spec.bootstrapArguments) {
            status = validateConstant(argument, depth + 1);
            if (status.is_err()) return status;
        }
        return WorkspaceErrorInfo{};
    }
    }
    return WorkspaceErrorInfo{WorkspaceError::InvalidArgument, "unsupported constant kind"};
}

/// @brief 物化常量为 CP 索引；前置条件 validateConstant 已通过。
ca::u32 makeConstantCp(ClassSession& session, const ConstantSpec& spec)
{
    jbc::core::ClassFile& cf = session.backing();
    switch (spec.kind) {
    case ConstantKind::String: return session.internString(spec.stringValue);
    case ConstantKind::Integer: return session.internInteger(spec.intValue);
    case ConstantKind::Long: return cf.cp.addLong(spec.longValue);
    case ConstantKind::Float:
    {
        ca::u32 bits = 0;
        std::memcpy(&bits, &spec.floatValue, sizeof(bits));
        return cf.cp.addFloatBits(bits);
    }
    case ConstantKind::Double:
    {
        ca::u64 bits = 0;
        std::memcpy(&bits, &spec.doubleValue, sizeof(bits));
        return cf.cp.addDoubleBits(bits);
    }
    case ConstantKind::Type: return session.internClass(spec.descriptor);
    default: return 0;   // Handle/Dynamic 在 validateConstant 已拒绝
    }
}

/// @brief 字段常量：种类须匹配描述符（照旧 fieldConstant 校验）。
WorkspaceErrorInfo fieldConstant(const ClassMutation& mutation)
{
    if (!mutation.hasFieldValue) return WorkspaceErrorInfo{};
    const std::string& descriptor = mutation.member.descriptor;
    const ConstantKind kind       = mutation.fieldValue.kind;
    const bool         valid =
        ((descriptor == "Z" || descriptor == "B" || descriptor == "C" || descriptor == "S" ||
          descriptor == "I") &&
         kind == ConstantKind::Integer) ||
        (descriptor == "J" && kind == ConstantKind::Long) ||
        (descriptor == "F" && kind == ConstantKind::Float) ||
        (descriptor == "D" && kind == ConstantKind::Double) ||
        (descriptor == "Ljava/lang/String;" && kind == ConstantKind::String);
    if (!valid)
        return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                  "field constant kind does not match its descriptor"};
    return validateConstant(mutation.fieldValue);
}

// ── 指令形状校验与物化（照旧 validInstructionShape/makeInstruction）──────

bool isSimpleOpcode(ca::i32 opcode)
{
    using O = Op;
    const auto at = [](O op) { return static_cast<ca::i32>(op); };
    return (opcode >= at(O::NOP) && opcode <= at(O::DCONST_1)) ||
           (opcode >= at(O::IALOAD) && opcode <= at(O::SALOAD)) ||
           (opcode >= at(O::IASTORE) && opcode <= at(O::SASTORE)) ||
           (opcode >= at(O::POP) && opcode <= at(O::SWAP)) ||
           (opcode >= at(O::IADD) && opcode <= at(O::LXOR)) ||
           (opcode >= at(O::I2L) && opcode <= at(O::I2S)) ||
           (opcode >= at(O::LCMP) && opcode <= at(O::DCMPG)) ||
           (opcode >= at(O::IRETURN) && opcode <= at(O::RETURN)) ||
           opcode == at(O::ARRAYLENGTH) || opcode == at(O::ATHROW) ||
           opcode == at(O::MONITORENTER) || opcode == at(O::MONITOREXIT);
}

/// @brief 指令形状校验；detail 输出旧栈同文本。
bool validInstructionShape(const InstructionSpec& spec, std::string& detail)
{
    using O = Op;
    const auto at = [](O op) { return static_cast<ca::i32>(op); };
    switch (spec.kind) {
    case InstructionKind::Simple:
        if (!isSimpleOpcode(spec.opcode)) detail = "opcode is not a no-operand JVM instruction";
        break;
    case InstructionKind::Int:
        if (spec.opcode != at(O::BIPUSH) && spec.opcode != at(O::SIPUSH) &&
            spec.opcode != at(O::NEWARRAY))
            detail = "opcode is not an int-operand JVM instruction";
        else if (spec.opcode == at(O::BIPUSH) && (spec.operand < -128 || spec.operand > 127))
            detail = "bipush operand is outside -128..127";
        else if (spec.opcode == at(O::SIPUSH) &&
                 (spec.operand < -32768 || spec.operand > 32767))
            detail = "sipush operand is outside -32768..32767";
        else if (spec.opcode == at(O::NEWARRAY) &&
                 (spec.operand < kAtypeBoolean || spec.operand > kAtypeLong))
            detail = "newarray operand is outside the JVM primitive type range";
        break;
    case InstructionKind::Var:
        if (!((spec.opcode >= at(O::ILOAD) && spec.opcode <= at(O::ALOAD)) ||
              (spec.opcode >= at(O::ISTORE) && spec.opcode <= at(O::ASTORE)) ||
              spec.opcode == at(O::RET)))
            detail = "opcode is not a local-variable JVM instruction";
        else if (spec.operand < 0 || spec.operand > 65535)
            detail = "local variable slot is outside 0..65535";
        break;
    case InstructionKind::Type:
        if (spec.opcode != at(O::NEW) && spec.opcode != at(O::ANEWARRAY) &&
            spec.opcode != at(O::CHECKCAST) && spec.opcode != at(O::INSTANCEOF))
            detail = "opcode is not a type JVM instruction";
        else if (spec.descriptor.empty())
            detail = "type instruction descriptor is required";
        break;
    case InstructionKind::Field:
        if (spec.opcode < at(O::GETSTATIC) || spec.opcode > at(O::PUTFIELD))
            detail = "opcode is not a field JVM instruction";
        else if (spec.owner.empty() || spec.name.empty() || spec.descriptor.empty())
            detail = "field instruction requires owner, name and descriptor";
        break;
    case InstructionKind::Method:
        if (spec.opcode < at(O::INVOKEVIRTUAL) || spec.opcode > at(O::INVOKEINTERFACE))
            detail = "opcode is not a method JVM instruction";
        else if (spec.owner.empty() || spec.name.empty() || spec.descriptor.empty())
            detail = "method instruction requires owner, name and descriptor";
        break;
    case InstructionKind::Jump:
        if (!((spec.opcode >= at(O::IFEQ) && spec.opcode <= at(O::JSR)) ||
              spec.opcode == at(O::IFNULL) || spec.opcode == at(O::IFNONNULL)))
            detail = "opcode is not a jump JVM instruction";
        break;
    case InstructionKind::Iinc:
        if (spec.operand < 0 || spec.operand > 65535 || spec.increment < -32768 ||
            spec.increment > 32767)
            detail = "iinc slot or increment is outside the JVM wide range";
        break;
    default: break;
    }
    return detail.empty();
}

/// @brief 物化一条指令为 jbc Insn（CP 操作数经会话 intern；跳转/switch 的目标
///        句柄经 @p outTarget 交付——此时 Insn 尚未进入 DOM）。
/// @param labels 符号标签 → DOM 句柄；Jump/switch 形状必查。
WorkspaceErrorInfo makeInstruction(ClassSession& session, const InstructionSpec& spec,
                                   const std::unordered_map<std::string, LabelId>& labels,
                                   Insn& out, LabelId& outTarget)
{
    std::string detail;
    if (!validInstructionShape(spec, detail))
        return WorkspaceErrorInfo{WorkspaceError::InvalidArgument, detail};
    jbc::core::ClassFile& cf = session.backing();
    out                      = Insn{};
    out.op                   = static_cast<Op>(spec.opcode);
    switch (spec.kind) {
    case InstructionKind::Simple: return WorkspaceErrorInfo{};
    case InstructionKind::Int: out.a = spec.operand; return WorkspaceErrorInfo{};
    case InstructionKind::Var:
        out.a    = spec.operand;
        out.wide = spec.operand > 255;
        return WorkspaceErrorInfo{};
    case InstructionKind::Type:
        out.cp = session.internClass(spec.descriptor);
        return WorkspaceErrorInfo{};
    case InstructionKind::Field:
        out.cp = session.internFieldRef(spec.owner, spec.name, spec.descriptor);
        return WorkspaceErrorInfo{};
    case InstructionKind::Method:
        out.cp = spec.opcode == static_cast<ca::i32>(Op::INVOKEINTERFACE)
                     ? session.internInterfaceMethodRef(spec.owner, spec.name, spec.descriptor)
                     : session.internMethodRef(spec.owner, spec.name, spec.descriptor);
        return WorkspaceErrorInfo{};
    case InstructionKind::Jump:
    {
        const auto found = labels.find(spec.label);
        if (spec.label.empty() || found == labels.end())
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "unknown label: " + spec.label};
        outTarget = found->second;
        return WorkspaceErrorInfo{};
    }
    case InstructionKind::Iinc:
        out.a    = spec.operand;
        out.b    = spec.increment;
        out.wide = spec.operand > 255 || spec.increment < -128 || spec.increment > 127;
        return WorkspaceErrorInfo{};
    case InstructionKind::TableSwitch:
    {
        if (spec.maximum < spec.minimum ||
            static_cast<ca::usize>(spec.maximum - spec.minimum + 1) != spec.labels.size()) {
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "tableswitch labels must cover every key from minimum "
                                      "through maximum"};
        }
        out.op = Op::TABLESWITCH;   // kind 即 opcode（spec.opcode 无需另给）
        const auto defaultFound = labels.find(spec.defaultLabel);
        if (spec.defaultLabel.empty() || defaultFound == labels.end())
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "unknown label: " + spec.defaultLabel};
        outTarget = defaultFound->second;
        for (const std::string& id : spec.labels) {
            const auto found = labels.find(id);
            if (id.empty() || found == labels.end())
                return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                          "unknown label: " + id};
            out.targets.push_back(found->second);
        }
        out.a = spec.minimum;
        out.b = spec.maximum;   // low..high 与 targets 数量一致（importFrom 复核口径）
        return WorkspaceErrorInfo{};
    }
    case InstructionKind::LookupSwitch:
    {
        if (spec.keys.size() != spec.labels.size() ||
            !std::is_sorted(spec.keys.begin(), spec.keys.end()) ||
            std::adjacent_find(spec.keys.begin(), spec.keys.end()) != spec.keys.end()) {
            return WorkspaceErrorInfo{
                WorkspaceError::InvalidArgument,
                "lookupswitch keys and labels must have equal size with unique ascending keys"};
        }
        out.op = Op::LOOKUPSWITCH;   // kind 即 opcode（spec.opcode 无需另给）
        const auto defaultFound = labels.find(spec.defaultLabel);
        if (spec.defaultLabel.empty() || defaultFound == labels.end())
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "unknown label: " + spec.defaultLabel};
        outTarget = defaultFound->second;
        for (const std::string& id : spec.labels) {
            const auto found = labels.find(id);
            if (id.empty() || found == labels.end())
                return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                          "unknown label: " + id};
            out.targets.push_back(found->second);
        }
        out.keys = spec.keys;
        return WorkspaceErrorInfo{};
    }
    case InstructionKind::MultiANewArray:
        if (spec.descriptor.empty() || spec.dimensions < 1 || spec.dimensions > 255) {
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "multianewarray requires an array descriptor and "
                                      "dimensions in 1..255"};
        }
        out.op = Op::MULTIANEWARRAY;   // kind 即 opcode（spec.opcode 无需另给）
        out.cp = session.internClass(spec.descriptor);
        out.a  = spec.dimensions;
        return WorkspaceErrorInfo{};
    case InstructionKind::LdcString:
        out.op = Op::LDC;   // 索引超 u8 时经 widenLdcIfNeeded 拓宽为 LDC_W
        out.cp = session.internString(spec.stringValue);
        return WorkspaceErrorInfo{};
    case InstructionKind::LdcInt:
        out.op = Op::LDC;
        out.cp = session.internInteger(spec.operand);
        return WorkspaceErrorInfo{};
    case InstructionKind::LdcLong:
        out.op = Op::LDC2_W;
        out.cp = cf.cp.addLong(spec.longValue);
        return WorkspaceErrorInfo{};
    case InstructionKind::LdcFloat:
    {
        ca::u32 bits = 0;
        std::memcpy(&bits, &spec.floatValue, sizeof(bits));
        out.op = Op::LDC;
        out.cp = cf.cp.addFloatBits(bits);
        return WorkspaceErrorInfo{};
    }
    case InstructionKind::LdcDouble:
    {
        ca::u64 bits = 0;
        std::memcpy(&bits, &spec.doubleValue, sizeof(bits));
        out.op = Op::LDC2_W;
        out.cp = cf.cp.addDoubleBits(bits);
        return WorkspaceErrorInfo{};
    }
    case InstructionKind::LdcType:
        if (spec.descriptor.empty())
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "LDC type descriptor is required"};
        out.op = Op::LDC;
        out.cp = session.internClass(spec.descriptor);
        return WorkspaceErrorInfo{};
    case InstructionKind::LdcHandle:
    case InstructionKind::LdcDynamic:
    case InstructionKind::InvokeDynamic:
        // libjbc 常量池无 MethodHandle/MethodType/Dynamic/InvokeDynamic 追加
        // API（主线缺口）——入口即拒，产物不出半成品。
        return WorkspaceErrorInfo{
            WorkspaceError::InvalidArgument,
            "method handle and dynamic constants are not supported by the libjbc constant "
            "pool append surface"};
    default:
        return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                  "unsupported instruction specification"};
    }
}

/// @brief LDC 的 u8 CP 索引拓宽（旧栈 ASM ClassWriter 自动拓宽的等价物）。
void widenLdcIfNeeded(Insn& insn)
{
    if ((insn.op == Op::LDC || insn.op == Op::LDC_W) && insn.cp > 255) insn.op = Op::LDC_W;
}

/// @brief 在 labels 里找符号标签；空名/未知均按旧栈 "unknown label: <id>" 报错。
WorkspaceErrorInfo findSymbolicLabel(const std::unordered_map<std::string, LabelId>& labels,
                                     const std::string& id, LabelId& out)
{
    const auto found = labels.find(id);
    if (id.empty() || found == labels.end())
        return WorkspaceErrorInfo{WorkspaceError::InvalidArgument, "unknown label: " + id};
    out = found->second;
    return WorkspaceErrorInfo{};
}

/// @brief 标签是否已锚定到具体指令（悬空/锚方法末都算未锚定）。
bool labelAnchored(const jbc::ir::CodeDom& dom, LabelId label)
{
    auto resolved = dom.resolveLabel(label);
    if (resolved.is_err()) return false;
    return !std::move(resolved).unwrap().isEnd();
}

/// @brief COMPUTE_MAXS 等价物：帧引擎重算 maxStack/maxLocals 并叠加旧栈下限
///        （异常处理帧 ≥1 栈槽；maxLocals ≥ 参数槽位 + 非 static 的 this 槽）。
WorkspaceErrorInfo recomputeFrameMaxes(ClassSession& session, jbc::ir::CodeUnit& unit,
                                       const jbc::core::MemberInfo& method)
{
    jbc::analysis::FrameEngine               engine(session.backing().cp);
    const jbc::analysis::FrameSummary summary = engine.compute(unit);
    const std::string                 descriptor = utf8Text(session.backing(), method.descCp);
    const ca::i32 parsed = jbc::analysis::methodArgSlots(descriptor);
    const ca::u32 argSlots = static_cast<ca::u32>(parsed < 0 ? 0 : parsed);
    const ca::u32 localsFloor = argSlots + ((method.access & kAccStatic) != 0 ? 0U : 1U);
    ca::u32 maxStack = summary.maxStack;
    if (!unit.exc.empty()) maxStack = std::max(maxStack, 1U);
    unit.maxStack  = static_cast<ca::u16>(std::max(maxStack, 0U));
    unit.maxLocals = static_cast<ca::u16>(std::max(summary.maxLocals, localsFloor));
    return WorkspaceErrorInfo{};
}

// ── 方法体构建（libjbc CodeUnit → PC 形态 BuiltBody）────────────────────

struct BuiltBody
{
    std::vector<ca::u8>                                 code;
    std::vector<CodeAttr::ExcEntry>                     excTable;
    ca::u16                                             maxStack  = 0;
    ca::u16                                             maxLocals = 0;
    std::vector<jbc::core::LineNumberTableAttr::Row>    lines;
    std::vector<jbc::core::LocalVariableTableAttr::Row> lvt;
    std::vector<jbc::core::LocalVariableTableAttr::Row> lvtt;
};

/// @brief 构建整方法体：指令序列 + 异常表 + 行号/局部变量调试表 + COMPUTE_MAXS。
///
/// 标签语义按 libjbc 重定型：Label 规格是「下一条指令的前置边界」（方法末尾
/// 悬空时锚到方法末），Line 规格绑定到下一条指令的导出 PC。try/catch 边界
/// 先符号标签、后代码指令序号（序号边界是 libjbc 面「标签非指令」的等价物，
/// 旧栈要求该序号恰是 LabelNode）。局部变量/行号行由导出 PC 落表。
WorkspaceErrorInfo buildMethodBody(ClassSession&                         session,
                                   jbc::core::MemberInfo&                method,
                                   const std::vector<InstructionSpec>&   specs,
                                   const std::vector<TryCatchSpec>&      blocks,
                                   const std::vector<LocalVariableSpec>& locals,
                                   BuiltBody&                            out)
{
    // 标签 ID 唯一且非空（旧栈 replaceMethodBody 的前置校验）。
    std::unordered_set<std::string> labelIds;
    for (const InstructionSpec& spec : specs) {
        if (spec.kind != InstructionKind::Label) continue;
        if (spec.label.empty() || !labelIds.insert(spec.label).second)
            return error(WorkspaceError::InvalidArgument, "replace method body",
                         "label IDs must be non-empty and unique");
    }

    jbc::ir::CodeUnit                        unit;
    std::unordered_map<std::string, LabelId> labels;
    for (const InstructionSpec& spec : specs) {
        if (spec.kind != InstructionKind::Label) continue;
        auto created = unit.dom.createLabel();
        if (created.is_err())   // 理论不可达：新建 DOM 上 createLabel 恒成功
            return error(WorkspaceError::Internal, "replace method body",
                         "label allocation failed");
        labels.emplace(spec.label, std::move(created).unwrap());
    }

    // pendingAnchors：待绑定到下一条指令的标签（Label 规格落位于自身位置）；
    // pendingLines：待定行号。
    std::vector<LabelId> pendingAnchors;
    pendingAnchors.reserve(labels.size());
    std::vector<std::pair<ca::i32, InsnId>> pendingLines;   // {line, anchor}

    for (const InstructionSpec& spec : specs) {
        if (spec.kind == InstructionKind::Label) {
            // 标签 = 下一条指令的前置边界；入队等待下一条真实指令锚定。
            pendingAnchors.push_back(labels.find(spec.label)->second);
            continue;
        }
        if (spec.kind == InstructionKind::Line) {
            if (spec.line < 1)
                return error(WorkspaceError::InvalidArgument, "replace method body",
                             "line number must be positive");
            pendingLines.emplace_back(spec.line, kNoInsn);
            continue;
        }

        Insn    insn;
        LabelId target;
        WorkspaceErrorInfo status = makeInstruction(session, spec, labels, insn, target);
        if (status.is_err())
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "replace method body: " + status.message};
        widenLdcIfNeeded(insn);

        // 当前 libjbc append 校验要求分支目标已锚定；前向引用（标签尚未锚定）
        // 先以无操作数形态落位，append 后经 get() 原位换回真实 op 并写 target。
        const bool deferredBranch = target != LabelId{} && !labelAnchored(unit.dom, target);
        const Op   realOp         = insn.op;
        if (deferredBranch) insn.op = Op::NOP;

        auto appended = unit.dom.append(std::move(insn));
        if (appended.is_err())
            return error(WorkspaceError::InvalidArgument, "replace method body",
                         "instruction append failed (" +
                             std::to_string(
                                 static_cast<int>(std::move(appended).unwrap_err())) +
                             ") at spec kind=" + std::to_string(static_cast<int>(spec.kind)) +
                             " op=" + std::to_string(static_cast<int>(realOp)));
        const InsnId id = std::move(appended).unwrap();

        if (target != LabelId{}) {
            auto* mutableInsn = std::move(unit.dom.get(id)).unwrap();
            if (deferredBranch) mutableInsn->op = realOp;
            mutableInsn->target = target;
        }
        for (LabelId anchor : pendingAnchors) (void)unit.dom.bindLabel(anchor, id);
        pendingAnchors.clear();
        for (auto& [line, anchor] : pendingLines)
            if (anchor == kNoInsn) anchor = id;
    }
    // 悬空标签锚到方法末；悬空行号同。
    for (LabelId anchor : pendingAnchors) (void)unit.dom.bindLabel(anchor);
    pendingAnchors.clear();

    // 异常表：符号标签优先，否则代码指令序号（锚到该指令前置边界）。
    const auto resolveBoundary = [&](const std::string& id, ca::i32 index,
                                     LabelId& outLabel) -> WorkspaceErrorInfo {
        if (!id.empty()) return findSymbolicLabel(labels, id, outLabel);
        const auto& order = unit.dom.order();
        if (index < 0 || static_cast<ca::usize>(index) >= order.size())
            return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                      "all boundaries must identify bound labels"};
        auto created = unit.dom.createLabel(order[static_cast<ca::usize>(index)]);
        if (created.is_err())
            return error(WorkspaceError::InvalidArgument, "replace method body",
                         "boundary label allocation failed");
        outLabel = std::move(created).unwrap();
        return WorkspaceErrorInfo{};
    };
    for (const TryCatchSpec& block : blocks) {
        LabelId start;
        LabelId end;
        LabelId handler;
        WorkspaceErrorInfo status =
            resolveBoundary(block.startLabel, block.startInstruction, start);
        if (status.is_err()) return status;
        status = resolveBoundary(block.endLabel, block.endInstruction, end);
        if (status.is_err()) return status;
        status = resolveBoundary(block.handlerLabel, block.handlerInstruction, handler);
        if (status.is_err()) return status;
        unit.exc.push_back(
            {start, end, handler, block.type.empty() ? 0U : session.internClass(block.type)});
    }

    // 导出字节并解析标签 PC。
    auto raw = unit.dom.exportRaw();
    if (raw.is_err())
        return error(WorkspaceError::InvalidArgument, "replace method body",
                     "method body export failed (" +
                         std::to_string(static_cast<int>(std::move(raw).unwrap_err())) + ")");
    std::vector<RawInsn> insns = std::move(raw).unwrap();
    auto                 encoded = jbc::core::encodeInsns(insns);
    if (encoded.is_err())
        return error(WorkspaceError::InvalidArgument, "replace method body",
                     "method body encode failed (" +
                         std::to_string(static_cast<int>(std::move(encoded).unwrap_err())) +
                         ")");
    out.code = std::move(encoded).unwrap();

    const auto& order = unit.dom.order();
    // InsnId 无 std::hash 特化；order 规模即方法指令数，线性查找足够。
    const auto indexOf = [&order](InsnId id) -> std::optional<ca::usize> {
        for (ca::usize i = 0; i < order.size(); ++i) {
            if (order[i] == id) return i;
        }
        return std::nullopt;
    };
    const ca::u32 codeLength = static_cast<ca::u32>(out.code.size());
    const auto    pcOf       = [&](LabelId label) -> ca::u32 {
        auto resolved = unit.dom.resolveLabel(label);
        if (resolved.is_err()) return codeLength;
        const auto target = std::move(resolved).unwrap();
        if (target.isEnd()) return codeLength;
        const auto found = indexOf(target.insn);
        if (!found.has_value()) return codeLength;
        return insns[*found].pc;
    };

    for (const jbc::ir::CodeUnit::Exc& entry : unit.exc) {
        CodeAttr::ExcEntry row;
        row.startPc   = pcOf(entry.start);
        row.endPc     = pcOf(entry.end);
        row.handlerPc = pcOf(entry.handler);
        row.catchType = entry.catchCp;
        out.excTable.push_back(row);
    }

    for (const auto& [line, anchor] : pendingLines) {
        ca::u32 pc = codeLength;
        if (anchor != kNoInsn) {
            const auto found = indexOf(anchor);
            if (found.has_value()) pc = insns[*found].pc;
        }
        out.lines.push_back({pc, static_cast<ca::u32>(line)});
    }

    for (const LocalVariableSpec& local : locals) {
        LabelId start;
        LabelId end;
        WorkspaceErrorInfo status = findSymbolicLabel(labels, local.startLabel, start);
        if (status.is_err() || local.name.empty() || local.descriptor.empty() ||
            local.index < 0)
            return error(WorkspaceError::InvalidArgument, "replace method body",
                         status.message.empty()
                             ? std::string("invalid local variable definition")
                             : status.message);
        status = findSymbolicLabel(labels, local.endLabel, end);
        if (status.is_err())
            return error(WorkspaceError::InvalidArgument, "replace method body",
                         status.message.empty()
                             ? std::string("invalid local variable definition")
                             : status.message);
        const ca::u32 startPc = pcOf(start);
        const ca::u32 endPc   = pcOf(end);
        if (endPc < startPc)
            return error(WorkspaceError::InvalidArgument, "replace method body",
                         "invalid local variable definition");
        jbc::core::LocalVariableTableAttr::Row row;
        row.startPc = startPc;
        row.length  = endPc - startPc;
        row.nameCp  = support::internUtf8(session.backing(), local.name);
        row.descCp  = support::internUtf8(
            session.backing(), local.signature.empty() ? local.descriptor : local.signature);
        row.slot = static_cast<ca::u32>(local.index);
        if (local.signature.empty())
            out.lvt.push_back(row);
        else
            out.lvtt.push_back(row);
    }

    WorkspaceErrorInfo maxes = recomputeFrameMaxes(session, unit, method);
    if (maxes.is_err()) return maxes;
    out.maxStack  = unit.maxStack;
    out.maxLocals = unit.maxLocals;
    return WorkspaceErrorInfo{};
}

/// @brief 把 BuiltBody 写进 Code 属性体（新建 CodeAttr + 调试表嵌套槽）。
///        旧栈 COMPUTE_FRAMES 整体重建不带 StackMapTable（pass-contract §9）。
void applyBuiltBody(ClassSession& session, jbc::core::AttrSlot& slot, const BuiltBody& body)
{
    auto code      = std::make_unique<CodeAttr>();
    code->maxStack = body.maxStack;
    code->maxLocals = body.maxLocals;
    code->code      = body.code;
    code->excTable  = body.excTable;
    const auto lineName = support::internUtf8(session.backing(), "LineNumberTable");
    const auto lvtName  = support::internUtf8(session.backing(), "LocalVariableTable");
    const auto lvttName = support::internUtf8(session.backing(), "LocalVariableTypeTable");
    if (!body.lines.empty()) {
        auto lines  = std::make_unique<jbc::core::LineNumberTableAttr>();
        lines->rows = body.lines;
        code->attrs.push_back(jbc::core::AttrSlot::makeNew(lineName, std::move(lines)));
    }
    if (!body.lvt.empty()) {
        auto lvt  = std::make_unique<jbc::core::LocalVariableTableAttr>();
        lvt->rows = body.lvt;
        code->attrs.push_back(jbc::core::AttrSlot::makeNew(lvtName, std::move(lvt)));
    }
    if (!body.lvtt.empty()) {
        auto lvtt  = std::make_unique<jbc::core::LocalVariableTableAttr>();
        lvtt->rows = body.lvtt;
        code->attrs.push_back(jbc::core::AttrSlot::makeNew(lvttName, std::move(lvtt)));
    }
    slot.body  = std::move(code);
    slot.state = jbc::core::AttrSlot::State::Modified;
}

/// @brief 把注解附着位置的属性槽补齐为单注解形态（槽不存在时新建）。
jbc::core::AttrSlot* ensureAnnotationSlot(jbc::core::ClassFile&       cf,
                                          std::vector<jbc::core::AttrSlot>& attrs,
                                          bool                        visible)
{
    const char* name = visible ? "RuntimeVisibleAnnotations" : "RuntimeInvisibleAnnotations";
    if (auto* slot = support::findAttr(cf, attrs, name)) return slot;
    attrs.push_back(jbc::core::AttrSlot::makeNew(
        support::internUtf8(cf, name), std::make_unique<jbc::core::AnnotationsAttr>()));
    return &attrs.back();
}

}   // namespace

jasm::Result<jasm::JvmJasmClass> ClassMutationService::applyJasm(
    const std::string& uri, ca::str::Utf16StringRef sourceName,
    ca::str::Utf16StringRef source, jasm::ValidationMode validationMode)
{
    // 重建管线的校验门跟随请求档位：off 时 assemble 后跳过 validate。
    jasm::JvmJasmContext context;
    context.validationMode = validationMode;
    jasm::ResourceJasmService service(*container_);
    return service.assemble(uri, sourceName, source, context);
}

ClassMutationOutcome ClassMutationService::apply(const ClassMutation& mutation,
                                                 jasm::ValidationMode validationMode)
{
    const auto failure = [](WorkspaceErrorInfo status) {
        ClassMutationOutcome result;
        result.status = std::move(status);
        return result;
    };

    if (mutation.classUri.empty())
        return failure(
            error(WorkspaceError::InvalidArgument, "mutate class", "class URI is empty"));
    model::IResource* rawResource = container_->getResource(mutation.classUri);
    if (!ca::core::tag_cast::isa<model::ClassResource>(rawResource))
        return failure(error(WorkspaceError::NotFound, "mutate class", mutation.classUri));
    auto* resource = ca::core::tag_cast::cast<model::ClassResource>(rawResource);

    try {
        auto parsed = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(resource->get()));
        if (parsed.is_err())
            return failure(
                error(WorkspaceError::DataLoss, "mutate class", "class bytes cannot be parsed"));
        std::shared_ptr<ClassSession> session = std::move(parsed).unwrap();
        jbc::core::ClassFile&         cf      = session->backing();

        WorkspaceErrorInfo status;
        switch (mutation.kind) {
        case ClassMutationKind::SetClassAccess:
        {
            status = validateAccess(mutation.access, AnnotationTarget::Class, "set class access");
            if (status.is_ok()) {
                // 旧字段旧语义（版本兼容契约：旧工程重放按整体替换解释）；
                // 0x20000 的 deprecated 位不再静默丢弃——映射为 Deprecated 属性。
                cf.access = static_cast<ca::u16>(mutation.access & 0xFFFF);
                if ((mutation.access & kAccDeprecated) != 0) setDeprecatedAttribute(cf, true);
            }
            break;
        }
        case ClassMutationKind::ChangeClassAccess:
        {
            // 增量语义：set 置位、clear 清位、未提及位一律保留。
            ca::u32 setMask          = 0;
            ca::u32 clearMask        = 0;
            bool    setDeprecated    = false;
            bool    clearDeprecated  = false;
            status =
                parseClassAccessFlagList(mutation.setFlags, "set", setMask, setDeprecated);
            if (status.is_err()) break;
            status =
                parseClassAccessFlagList(mutation.clearFlags, "clear", clearMask, clearDeprecated);
            if (status.is_err()) break;
            ca::u32 conflict = setMask & clearMask;
            if (conflict != 0) {
                std::string names;
                for (const auto& entry : {std::pair<const char*, ca::u32>{"public", kAccPublic},
                                          {"final", kAccFinal},
                                          {"super", kAccSuper},
                                          {"interface", kAccInterface},
                                          {"abstract", kAccAbstract},
                                          {"synthetic", kAccSynthetic},
                                          {"annotation", kAccAnnotation},
                                          {"enum", kAccEnum},
                                          {"module", kAccModule},
                                          {"record", kAccRecord},
                                          {"sealed", kAccSealed}}) {
                    if ((conflict & entry.second) != 0) {
                        if (!names.empty()) names += ", ";
                        names += entry.first;
                    }
                }
                status = error(WorkspaceError::InvalidArgument, "change class access",
                               "flag cannot be both set and cleared: " + names);
                break;
            }
            const ca::u32 merged =
                (static_cast<ca::u32>(cf.access) & ~clearMask) | setMask;
            // 结果位校验与整体替换同规则（可见性互斥 / final+abstract 等）。
            status = validateAccess(static_cast<ca::i32>(merged), AnnotationTarget::Class,
                                    "change class access");
            if (status.is_err()) break;
            cf.access = static_cast<ca::u16>(merged);
            if (setDeprecated) setDeprecatedAttribute(cf, true);
            if (clearDeprecated) setDeprecatedAttribute(cf, false);
            break;
        }
        case ClassMutationKind::SetFieldAccess:
        {
            jbc::core::MemberInfo* field = findField(cf, mutation.member);
            if (field == nullptr)
                status = error(WorkspaceError::NotFound, "set field access", mutation.member.name);
            else {
                status =
                    validateAccess(mutation.access, AnnotationTarget::Field, "set field access");
                if (status.is_ok()) field->access = static_cast<ca::u16>(mutation.access);
            }
            break;
        }
        case ClassMutationKind::SetMethodAccess:
        {
            jbc::core::MemberInfo* method = findMethod(cf, mutation.member);
            if (method == nullptr)
                status = error(WorkspaceError::NotFound, "set method access", mutation.member.name);
            else {
                status = validateAccess(mutation.access, AnnotationTarget::Method,
                                        "set method access");
                if (status.is_ok()) {
                    const bool dropsBody = (mutation.access & (kAccAbstract | kAccNative)) != 0;
                    if (dropsBody && support::ensureCode(cf, *method) != nullptr)
                        status = error(WorkspaceError::InvalidArgument, "set method access",
                                       "cannot mark a method with an existing body abstract/"
                                       "native");
                    else
                        method->access = static_cast<ca::u16>(mutation.access);
                }
            }
            break;
        }
        case ClassMutationKind::AddField:
        {
            if (findField(cf, mutation.member) != nullptr) {
                status = error(WorkspaceError::AlreadyExists, "add field", mutation.member.name);
                break;
            }
            if (mutation.member.name.empty() || mutation.member.descriptor.empty()) {
                status = error(WorkspaceError::InvalidArgument, "add field",
                               "name and descriptor are required");
                break;
            }
            status = validateAccess(mutation.access, AnnotationTarget::Field, "add field");
            if (status.is_err()) break;
            status = fieldConstant(mutation);
            if (status.is_err()) break;

            jbc::core::MemberInfo field;
            field.access = static_cast<ca::u16>(mutation.access);
            field.nameCp = support::internUtf8(cf, mutation.member.name);
            field.descCp = support::internUtf8(cf, mutation.member.descriptor);
            if (!mutation.signature.empty()) {
                auto sig = std::make_unique<jbc::core::SignatureAttr>();
                sig->signatureCp = support::internUtf8(cf, mutation.signature);
                field.attrs.push_back(jbc::core::AttrSlot::makeNew(
                    support::internUtf8(cf, "Signature"), std::move(sig)));
            }
            if (mutation.hasFieldValue) {
                const ca::u32 valueIndex = makeConstantCp(*session, mutation.fieldValue);
                const std::vector<ca::u8> payload{static_cast<ca::u8>(valueIndex >> 8),
                                                  static_cast<ca::u8>(valueIndex)};
                field.attrs.push_back(jbc::core::AttrSlot::makeNew(
                    support::internUtf8(cf, "ConstantValue"),
                    std::make_unique<RawAttr>(std::move(payload))));
            }
            cf.fields.push_back(std::move(field));
            break;
        }
        case ClassMutationKind::RemoveField:
        {
            bool removed = false;
            for (auto it = cf.fields.begin(); it != cf.fields.end(); ++it) {
                if (utf8Text(cf, it->nameCp) == mutation.member.name &&
                    utf8Text(cf, it->descCp) == mutation.member.descriptor) {
                    cf.fields.erase(it);
                    removed = true;
                    break;
                }
            }
            if (!removed)
                status = error(WorkspaceError::NotFound, "remove field", mutation.member.name);
            break;
        }
        case ClassMutationKind::AddMethod:
        {
            if (findMethod(cf, mutation.member) != nullptr) {
                status = error(WorkspaceError::AlreadyExists, "add method", mutation.member.name);
                break;
            }
            if (mutation.member.name.empty() || mutation.member.descriptor.empty()) {
                status = error(WorkspaceError::InvalidArgument, "add method",
                               "name and descriptor are required");
                break;
            }
            status = validateAccess(mutation.access, AnnotationTarget::Method, "add method");
            if (status.is_err()) break;
            const bool bodyForbidden = (mutation.access & (kAccAbstract | kAccNative)) != 0;
            if (mutation.initialInstructions.empty() && !bodyForbidden) {
                status = error(WorkspaceError::InvalidArgument, "add method",
                               "non abstract/native methods require initial instructions");
                break;
            }
            if (!mutation.initialInstructions.empty() && bodyForbidden) {
                status = error(WorkspaceError::InvalidArgument, "add method",
                               "abstract/native methods cannot define initial instructions");
                break;
            }
            bool exceptionInvalid = false;
            for (const std::string& exception : mutation.exceptions) {
                if (exception.empty()) {
                    status = error(WorkspaceError::InvalidArgument, "add method",
                                   "exception internal names must be non-empty");
                    exceptionInvalid = true;
                    break;
                }
            }
            if (exceptionInvalid) break;

            jbc::core::MemberInfo method;
            method.access = static_cast<ca::u16>(mutation.access);
            method.nameCp = support::internUtf8(cf, mutation.member.name);
            method.descCp = support::internUtf8(cf, mutation.member.descriptor);
            if (!mutation.signature.empty()) {
                auto sig = std::make_unique<jbc::core::SignatureAttr>();
                sig->signatureCp = support::internUtf8(cf, mutation.signature);
                method.attrs.push_back(jbc::core::AttrSlot::makeNew(
                    support::internUtf8(cf, "Signature"), std::move(sig)));
            }
            if (!mutation.exceptions.empty()) {
                // typed ExceptionsAttr（B3 分派接线后 Raw 形不再是该属性的
                // 会话内合法形态；字节输出与旧 Raw 手拼逐字节一致）。
                auto exc = std::make_unique<jbc::core::ExceptionsAttr>();
                exc->exceptionCps.reserve(mutation.exceptions.size());
                for (const std::string& exception : mutation.exceptions) {
                    exc->exceptionCps.push_back(session->internClass(exception));
                }
                method.attrs.push_back(jbc::core::AttrSlot::makeNew(
                    support::internUtf8(cf, "Exceptions"), std::move(exc)));
            }
            if (!bodyForbidden) {
                BuiltBody body;
                status = buildMethodBody(*session, method, mutation.initialInstructions,
                                         mutation.tryCatchBlocks, mutation.localVariables, body);
                if (status.is_err()) break;
                jbc::core::AttrSlot slot = jbc::core::AttrSlot::makeNew(
                    support::internUtf8(cf, "Code"), std::make_unique<CodeAttr>());
                applyBuiltBody(*session, slot, body);
                method.attrs.push_back(std::move(slot));
            }
            cf.methods.push_back(std::move(method));
            break;
        }
        case ClassMutationKind::RemoveMethod:
        {
            bool removed = false;
            for (auto it = cf.methods.begin(); it != cf.methods.end(); ++it) {
                if (utf8Text(cf, it->nameCp) == mutation.member.name &&
                    utf8Text(cf, it->descCp) == mutation.member.descriptor) {
                    cf.methods.erase(it);
                    removed = true;
                    break;
                }
            }
            if (!removed)
                status = error(WorkspaceError::NotFound, "remove method", mutation.member.name);
            break;
        }
        case ClassMutationKind::AddMarkerAnnotation:
        case ClassMutationKind::RemoveAnnotation:
        {
            if (mutation.annotationDescriptor.empty()) {
                status = error(WorkspaceError::InvalidArgument, "edit annotation",
                               "annotation descriptor is required");
                break;
            }
            bool visible = true;
            jbc::core::MemberInfo* member = nullptr;
            switch (mutation.annotationTarget) {
            case AnnotationTarget::Class: break;
            case AnnotationTarget::Field: member = findField(cf, mutation.member); break;
            case AnnotationTarget::Method: member = findMethod(cf, mutation.member); break;
            }
            if (mutation.annotationTarget != AnnotationTarget::Class && member == nullptr) {
                status = error(WorkspaceError::NotFound, "edit annotation",
                               mutation.member.name.empty() ? mutation.classUri
                                                            : mutation.member.name);
                break;
            }
            jbc::core::AttrSlot* slot = ensureAnnotationSlot(
                cf, member != nullptr ? member->attrs : cf.attrs, visible);
            if (!slot->decoded()) {
                if (cf.decodeAttr(*slot).is_err()) {
                    status = error(WorkspaceError::DataLoss, "edit annotation",
                                   "annotation attribute cannot be decoded");
                    break;
                }
            }
            // 槽本体非 const、由候选容器持有；peek() 的 const 面在此收窄回写路径。
            auto* annotations = const_cast<jbc::core::AnnotationsAttr*>(
                ca::core::tag_cast::cast<const jbc::core::AnnotationsAttr>(slot->peek()));
            if (annotations == nullptr) {
                status = error(WorkspaceError::DataLoss, "edit annotation",
                               "annotation attribute has an unexpected shape");
                break;
            }
            if (mutation.kind == ClassMutationKind::AddMarkerAnnotation) {
                const bool duplicate = std::any_of(
                    annotations->annos.begin(), annotations->annos.end(),
                    [&](const jbc::core::AnnotationsAttr::Annotation& item) {
                        return utf8Text(cf, item.typeCp) == mutation.annotationDescriptor;
                    });
                if (duplicate) {
                    status = error(WorkspaceError::AlreadyExists, "add annotation",
                                   mutation.annotationDescriptor);
                    break;
                }
                jbc::core::AnnotationsAttr::Annotation anno;
                anno.typeCp = support::internUtf8(cf, mutation.annotationDescriptor);
                (void)slot->mutate();
                annotations->annos.push_back(std::move(anno));
                break;
            }
            bool removed = false;
            for (auto it = annotations->annos.begin(); it != annotations->annos.end(); ++it) {
                if (utf8Text(cf, it->typeCp) == mutation.annotationDescriptor) {
                    (void)slot->mutate();
                    annotations->annos.erase(it);
                    removed = true;
                    break;
                }
            }
            if (!removed)
                status = error(WorkspaceError::NotFound, "remove annotation",
                               mutation.annotationDescriptor);
            break;
        }
        case ClassMutationKind::RenameLocal:
        {
            jbc::core::MemberInfo* method = findMethod(cf, mutation.member);
            if (method == nullptr) {
                status = error(WorkspaceError::NotFound, "rename local", mutation.member.name);
                break;
            }
            bool changed = false;
            if (jbc::core::AttrSlot* codeSlot = support::findAttr(cf, method->attrs, "Code")) {
                if (!codeSlot->decoded()) (void)cf.decodeAttr(*codeSlot);
                // 槽本体非 const、由候选容器持有；peek() 的 const 面在此收窄回写路径。
                auto* code = const_cast<CodeAttr*>(
                    ca::core::tag_cast::cast<const CodeAttr>(codeSlot->peek()));
                if (code != nullptr) {
                    for (jbc::core::AttrSlot& nested : code->attrs) {
                        if (!cf.cp.utf8Equals(nested.nameCp, "LocalVariableTable") &&
                            !cf.cp.utf8Equals(nested.nameCp, "LocalVariableTypeTable"))
                            continue;
                        if (!nested.decoded()) {
                            if (cf.decodeAttr(nested).is_err()) continue;
                        }
                        auto* table = const_cast<jbc::core::LocalVariableTableAttr*>(
                            ca::core::tag_cast::cast<const jbc::core::LocalVariableTableAttr>(
                                nested.peek()));
                        if (table == nullptr) continue;
                        bool tableChanged = false;
                        for (jbc::core::LocalVariableTableAttr::Row& row : table->rows) {
                            if (utf8Text(cf, row.nameCp) != mutation.localName) continue;
                            if (mutation.localIndex >= 0 &&
                                static_cast<ca::i32>(row.slot) != mutation.localIndex)
                                continue;
                            row.nameCp = support::internUtf8(cf, mutation.replacementLocalName);
                            tableChanged = true;
                        }
                        if (tableChanged) {
                            (void)nested.mutate();
                            changed = true;
                        }
                    }
                }
            }
            if (!changed)
                status = error(WorkspaceError::NotFound, "rename local", mutation.localName);
            break;
        }
        case ClassMutationKind::ReplaceTryCatchBlocks:
        {
            jbc::core::MemberInfo* method = findMethod(cf, mutation.member);
            if (method == nullptr) {
                status = error(WorkspaceError::NotFound, "replace try catch blocks",
                               mutation.member.name);
                break;
            }
            // 空块 + 无绑定：照旧语义为成功无操作（不动方法体）。
            if (mutation.tryCatchBlocks.empty() && mutation.existingLabels.empty()) break;
            const ca::usize methodIndex =
                static_cast<ca::usize>(method - cf.methods.data());
            auto promoted = jbc::ir::promoteMethod(*session, methodIndex);
            if (promoted.is_err()) {
                status = error(WorkspaceError::InvalidArgument, "replace try catch blocks",
                               "all boundaries must identify bound labels");
                break;
            }
            jbc::ir::CodeUnitRef unit = std::move(promoted).unwrap();

            std::unordered_map<std::string, LabelId> labels;
            bool bindingInvalid = false;
            for (const LabelBindingSpec& binding : mutation.existingLabels) {
                if (binding.id.empty() || labels.find(binding.id) != labels.end()) {
                    status = error(WorkspaceError::InvalidArgument, "bind labels",
                                   "label IDs must be non-empty and unique");
                    bindingInvalid = true;
                    break;
                }
                const auto& order = unit->dom.order();
                if (binding.instructionIndex < 0 ||
                    static_cast<ca::usize>(binding.instructionIndex) >= order.size()) {
                    status = error(WorkspaceError::InvalidArgument, "bind labels",
                                   "label instruction index is outside the method");
                    bindingInvalid = true;
                    break;
                }
                auto created = unit->dom.createLabel(
                    order[static_cast<ca::usize>(binding.instructionIndex)]);
                if (created.is_err()) {
                    status = error(WorkspaceError::InvalidArgument, "bind labels",
                                   "label allocation failed");
                    bindingInvalid = true;
                    break;
                }
                labels.emplace(binding.id, std::move(created).unwrap());
            }
            if (bindingInvalid) break;

            const auto resolve = [&](const std::string& id, ca::i32 index,
                                     LabelId& outLabel) -> WorkspaceErrorInfo {
                if (!id.empty()) return findSymbolicLabel(labels, id, outLabel);
                const auto& order = unit->dom.order();
                if (index < 0 || static_cast<ca::usize>(index) >= order.size())
                    return WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                              "all boundaries must identify bound labels"};
                auto created = unit->dom.createLabel(order[static_cast<ca::usize>(index)]);
                if (created.is_err())
                    return error(WorkspaceError::InvalidArgument, "replace try catch blocks",
                                 "boundary label allocation failed");
                outLabel = std::move(created).unwrap();
                return WorkspaceErrorInfo{};
            };
            unit->exc.clear();
            bool boundaryInvalid = false;
            for (const TryCatchSpec& block : mutation.tryCatchBlocks) {
                LabelId start;
                LabelId end;
                LabelId handler;
                status = resolve(block.startLabel, block.startInstruction, start);
                if (status.is_err()) {
                    boundaryInvalid = true;
                    break;
                }
                status = resolve(block.endLabel, block.endInstruction, end);
                if (status.is_err()) {
                    boundaryInvalid = true;
                    break;
                }
                status = resolve(block.handlerLabel, block.handlerInstruction, handler);
                if (status.is_err()) {
                    boundaryInvalid = true;
                    break;
                }
                unit->exc.push_back({start, end, handler,
                                     block.type.empty() ? 0U : session->internClass(block.type)});
            }
            if (boundaryInvalid) break;
            status = recomputeFrameMaxes(*session, *unit, *method);
            if (status.is_err()) break;
            auto lowered = jbc::ir::lowerMethod(*session, methodIndex);
            if (lowered.is_err()) {
                status = error(WorkspaceError::InvalidArgument, "replace try catch blocks",
                               "method body cannot be written back");
                break;
            }
            break;
        }
        case ClassMutationKind::ReplaceMethodBody:
        {
            jbc::core::MemberInfo* method = findMethod(cf, mutation.member);
            if (method == nullptr) {
                status = error(WorkspaceError::NotFound, "replace method body",
                               mutation.member.name);
                break;
            }
            if ((method->access & (kAccAbstract | kAccNative)) != 0) {
                status = error(WorkspaceError::InvalidArgument, "replace method body",
                               "abstract/native methods cannot have a body");
                break;
            }
            if (mutation.initialInstructions.empty()) {
                status = error(WorkspaceError::InvalidArgument, "replace method body",
                               "instructions are required");
                break;
            }
            BuiltBody body;
            status = buildMethodBody(*session, *method, mutation.initialInstructions,
                                     mutation.tryCatchBlocks, mutation.localVariables, body);
            if (status.is_err()) break;
            // 整体重建：旧 Code 槽原位换体（含丢弃全部旧嵌套属性——旧栈
            // COMPUTE_FRAMES + 空标签集合同样丢弃旧 LNT/LVT/SMT）。
            jbc::core::AttrSlot* codeSlot = support::findAttr(cf, method->attrs, "Code");
            if (codeSlot == nullptr) {
                method->attrs.push_back(jbc::core::AttrSlot::makeNew(
                    support::internUtf8(cf, "Code"), std::make_unique<CodeAttr>()));
                codeSlot = &method->attrs.back();
            }
            else if (!codeSlot->decoded()) {
                (void)cf.decodeAttr(*codeSlot);
            }
            applyBuiltBody(*session, *codeSlot, body);
            break;
        }
        case ClassMutationKind::InsertInstruction:
        case ClassMutationKind::ReplaceInstruction:
        case ClassMutationKind::RemoveInstruction:
        {
            jbc::core::MemberInfo* method = findMethod(cf, mutation.member);
            if (method == nullptr) {
                status = error(WorkspaceError::NotFound, "edit instruction", mutation.member.name);
                break;
            }
            const ca::usize methodIndex = static_cast<ca::usize>(method - cf.methods.data());
            if (!jbc::ir::hasCode(*session, methodIndex)) {
                status = error(WorkspaceError::InvalidArgument, "edit instruction",
                               "instruction index is outside the method");
                break;
            }
            auto promoted = jbc::ir::promoteMethod(*session, methodIndex);
            if (promoted.is_err()) {
                status = error(WorkspaceError::DataLoss, "edit instruction",
                               "method body cannot be promoted");
                break;
            }
            jbc::ir::CodeUnitRef unit    = std::move(promoted).unwrap();
            const auto&          order   = unit->dom.order();
            const ca::i32        index   = mutation.instructionIndex;
            if (index < 0 || static_cast<ca::usize>(index) >= order.size()) {
                status = error(WorkspaceError::InvalidArgument, "edit instruction",
                               "instruction index is outside the method");
                break;
            }
            const InsnId targetId = order[static_cast<ca::usize>(index)];

            // 符号标签绑定（旧栈 existingLabels：绑定 ID → 指令序号）。
            std::unordered_map<std::string, LabelId> labels;
            bool bindingInvalid = false;
            for (const LabelBindingSpec& binding : mutation.existingLabels) {
                if (binding.id.empty() || labels.find(binding.id) != labels.end()) {
                    status = error(WorkspaceError::InvalidArgument, "bind labels",
                                   "label IDs must be non-empty and unique");
                    bindingInvalid = true;
                    break;
                }
                if (binding.instructionIndex < 0 ||
                    static_cast<ca::usize>(binding.instructionIndex) >= order.size()) {
                    status = error(WorkspaceError::InvalidArgument, "bind labels",
                                   "label instruction index is outside the method");
                    bindingInvalid = true;
                    break;
                }
                auto created = unit->dom.createLabel(
                    order[static_cast<ca::usize>(binding.instructionIndex)]);
                if (created.is_err()) {
                    status = error(WorkspaceError::InvalidArgument, "bind labels",
                                   "label allocation failed");
                    bindingInvalid = true;
                    break;
                }
                labels.emplace(binding.id, std::move(created).unwrap());
            }
            if (bindingInvalid) break;

            if (mutation.kind == ClassMutationKind::RemoveInstruction) {
                auto erased = unit->dom.erase(targetId);
                if (erased.is_err()) {
                    status = error(
                        WorkspaceError::InvalidArgument, "edit instruction",
                        "instruction is still referenced by branches or exception boundaries");
                    break;
                }
            }
            else if (mutation.kind == ClassMutationKind::ReplaceInstruction) {
                if (mutation.instruction.kind == InstructionKind::Label) {
                    // libjbc 面标签是边界不是指令：以标签替指令不可表达。
                    status = error(WorkspaceError::InvalidArgument, "edit instruction",
                                   "replacing an instruction with a label is not supported");
                    break;
                }
                Insn    insn;
                LabelId target;
                status = makeInstruction(*session, mutation.instruction, labels, insn, target);
                if (status.is_err()) {
                    status = WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                                "edit instruction: " + status.message};
                    break;
                }
                widenLdcIfNeeded(insn);
                auto* slot = std::move(unit->dom.get(targetId)).unwrap();
                // 保留旧 srcPc：pcmap 以旧 PC 重定向 LNT（relocation contract）。
                const ca::u32 oldPc = slot->srcPc;
                insn.srcPc          = oldPc;
                if (target != LabelId{}) insn.target = target;
                *slot = std::move(insn);
            }
            else   // InsertInstruction
            {
                if (mutation.instruction.kind == InstructionKind::Label) {
                    if (mutation.instruction.label.empty() ||
                        labels.find(mutation.instruction.label) != labels.end()) {
                        status = error(WorkspaceError::InvalidArgument, "edit instruction",
                                       "new label ID must be non-empty and not already bound");
                        break;
                    }
                    // 插入标签 = 在插入点创建前置边界：insertBefore 锚到目标
                    // 指令；insertAfter 锚到后继（末尾则锚方法末）。
                    InsnId anchor = kNoInsn;
                    if (mutation.insertAfter) {
                        const bool atEnd = static_cast<ca::usize>(index) + 1 >= order.size();
                        anchor = atEnd ? kNoInsn : order[static_cast<ca::usize>(index) + 1];
                    }
                    else {
                        anchor = targetId;
                    }
                    auto created = unit->dom.createLabel(anchor);
                    if (created.is_err()) {
                        status = error(WorkspaceError::InvalidArgument, "edit instruction",
                                       "label allocation failed");
                        break;
                    }
                    break;
                }
                Insn    insn;
                LabelId target;
                status = makeInstruction(*session, mutation.instruction, labels, insn, target);
                if (status.is_err()) {
                    status = WorkspaceErrorInfo{WorkspaceError::InvalidArgument,
                                                "edit instruction: " + status.message};
                    break;
                }
                widenLdcIfNeeded(insn);
                auto inserted = mutation.insertAfter
                                    ? unit->dom.insertAfter(targetId, std::move(insn))
                                    : unit->dom.insertBefore(targetId, std::move(insn));
                if (inserted.is_err()) {
                    status = error(WorkspaceError::InvalidArgument, "edit instruction",
                                   "instruction insert failed");
                    break;
                }
                const InsnId newId = std::move(inserted).unwrap();
                if (target != LabelId{}) {
                    auto* slot  = std::move(unit->dom.get(newId)).unwrap();
                    slot->target = target;
                }
            }
            if (status.is_err()) break;

            status = recomputeFrameMaxes(*session, *unit, *method);
            if (status.is_err()) break;
            auto lowered = jbc::ir::lowerMethod(*session, methodIndex);
            if (lowered.is_err()) {
                status = error(WorkspaceError::InvalidArgument, "edit instruction",
                               "method body cannot be written back");
                break;
            }
            break;
        }
        }
        if (status.is_err()) return failure(std::move(status));

        // 写回：jbc 序列化 → Jasm 验证（写回后整类门，档位随请求：off
        // 直通，与 jasm 重建路径同语义）→ spi 两阶段提交。
        auto written = jbc::core::writeClass(cf);
        if (written.is_err())
            return failure(error(WorkspaceError::Internal, "mutate class",
                                 "class serialization failed"));
        std::vector<ca::u8> bytes = std::move(written).unwrap();

        jasm::JvmJasmContext gateContext;
        gateContext.validationMode = validationMode;
        jasm::JvmJasmService validator;
        auto                 validated = validator.validate(bytes, gateContext);
        if (!validated.isOk()) {
            ClassMutationOutcome result;
            result.status = error(WorkspaceError::InvalidArgument, "mutate class",
                                  "candidate class did not pass validation");
            result.diagnostics = std::move(validated.diagnostics);
            return result;
        }
        spi::ClassResourcePendingCommit pending;
        pending.stage(resource, std::move(bytes));
        pending.commit();
        ClassMutationOutcome result;
        result.diagnostics = std::move(validated.diagnostics);
        return result;
    }
    catch (const std::exception& exception) {
        return failure(error(WorkspaceError::Internal, "mutate class", exception.what()));
    }
}

}   // namespace mjbase::workspace
