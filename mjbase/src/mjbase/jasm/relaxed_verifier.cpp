#include "relaxed_verifier.hpp"

#include <deque>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"
#include "jbc/type/descriptor.hpp"

namespace mjbase::jasm {
namespace {

namespace ja = jbc::analysis;
namespace jr = jbc::ir;
namespace jc = jbc::core;
namespace jt = jbc::type;

using ja::FrameAt;
using ja::FrameEngine;
using ja::FramePolicy;
using ja::MethodSig;
using ja::VKind;
using ja::VType;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// @brief 层次解析视图：容器内已登记类走层次图（含接口实现）；未登记（外部/
///        JDK）类型一律不可解析——赋值/合并判定按 fail-open 处置，层次不可
///        解析时不得判非法。
class HierarchyView final
{
public:
    explicit HierarchyView(const mjbase::hierarchy::ClassHierarchy* hierarchy)
        : hierarchy_(hierarchy)
    {}

    /// @brief 类型是否可解析（层次图已登记）；无图为恒 false。
    bool resolvable(const std::string& internalName) const
    {
        if (hierarchy_ == nullptr) return false;
        return hierarchy_->findClass(internalName).valid();
    }

    /// @brief 子类型判定（含接口实现）；两侧必须已登记（调用方先 resolvable）。
    bool isSubtype(const std::string& child, const std::string& parent) const
    {
        const mjbase::ClassKey childKey  = hierarchy_->findClass(child);
        const mjbase::ClassKey parentKey = hierarchy_->findClass(parent);
        if (!childKey.valid() || !parentKey.valid()) return false;
        return hierarchy_->isSubtype(childKey, parentKey);
    }

    /// @brief 公共超类（合并用）；任一侧不可解析回落 java/lang/Object。
    std::string commonSuperclass(const std::string& first, const std::string& second) const
    {
        if (first == second) return first;
        if (hierarchy_ != nullptr) {
            const mjbase::ClassKey firstKey  = hierarchy_->findClass(first);
            const mjbase::ClassKey secondKey = hierarchy_->findClass(second);
            if (firstKey.valid() && secondKey.valid()) {
                const mjbase::ClassKey commonKey = hierarchy_->commonAncestor(firstKey, secondKey);
                if (commonKey.valid()) return std::string{hierarchy_->nameOf(commonKey)};
            }
        }
        return "java/lang/Object";
    }

private:
    const mjbase::hierarchy::ClassHierarchy* hierarchy_;
};

/// @brief 数组形态：名字（内部名或 '[' 开头数组描述符）的维度与引用基座。
///
/// "[Ljava/lang/String;" → dim 1 / elem "java/lang/String"；"[[I" → dim 2 /
/// 引用基座缺席；标量 dim 0。
struct ArrayShape
{
    ca::u32     dim     = 0;
    std::string elem;
    bool        refElem = false;
};

ArrayShape arrayShape(std::string_view name)
{
    ArrayShape shape;
    ca::usize  index = 0;
    while (index < name.size() && name[index] == '[') {
        ++shape.dim;
        ++index;
    }
    if (shape.dim > 0 && index + 1 < name.size() && name[index] == 'L' && name.back() == ';') {
        shape.refElem = true;
        shape.elem    = std::string(name.substr(index + 1, name.size() - index - 2));
    }
    return shape;
}

/// @brief relaxed 合并策略：Strict 帧域引用合并的 JVM 赋值兼容语义。
///
/// 同名引用合并为自身；跨类引用经 HierarchyView 求公共父类，同维引用元素
/// 数组先归并基座再按全维度回包（String[] × CharSequence[] →
/// CharSequence[]，比旧栈整体降 Object 更精确）；不可解析一律回落
/// java/lang/Object——合并产物只放宽不收紧。合并产物的名字由本对象自持
///（帧域对 resolver 返回的 VType.name 只借用，寿命约定覆盖 compute 与
/// 扫描期）。
class RelaxedResolver final : public ja::ITypeResolver
{
public:
    explicit RelaxedResolver(const HierarchyView& hierarchy)
        : hierarchy_(hierarchy)
    {}

    ca::core::Result<VType, ja::TypeMergeError> merge(VType first, VType second) override
    {
        using ja::TypeMergeError;
        if (!first.name.empty() && first.name == second.name)
            return ca::core::Ok(VType::ref(first.name));
        if (first.name.empty() || second.name.empty())
            return ca::core::Err(TypeMergeError::Unresolvable);
        const ArrayShape shapeA = arrayShape(first.name);
        const ArrayShape shapeB = arrayShape(second.name);
        if (shapeA.dim > 0 && shapeA.dim == shapeB.dim && shapeA.refElem && shapeB.refElem) {
            const std::string common = hierarchy_.commonSuperclass(shapeA.elem, shapeB.elem);
            if (common != "java/lang/Object")
                return ca::core::Ok(
                    VType::ref(own(std::string(shapeA.dim - 1, '[') + "[L" + common + ";")));
        }
        return ca::core::Ok(VType::ref(own(hierarchy_.commonSuperclass(
            std::string(first.name), std::string(second.name)))));
    }

private:
    std::string_view own(const std::string& value)
    {
        owned_.push_back(value);
        return owned_.back();
    }

    const HierarchyView&    hierarchy_;
    std::deque<std::string> owned_;
};

/// @brief primitive 描述符基类型 → 帧域 VKind（B/C/S/Z 与 I 同格，JVMS 帧
///        语义不区分窄整型）。
VKind primKind(jt::BaseType base)
{
    switch (base) {
    case jt::BaseType::Long: return VKind::Long;
    case jt::BaseType::Double: return VKind::Double;
    case jt::BaseType::Float: return VKind::Float;
    default: return VKind::Int;
    }
}

/// @brief 引用族判定：Ref / Null / UninitThis / Uninitialized 都是引用形态。
bool refKind(VKind k)
{
    return k == VKind::Ref || k == VKind::Null || k == VKind::UninitThis ||
           k == VKind::Uninitialized;
}

/// @brief TypeDesc → 名字面（内部名 / '[' 数组描述符 / prim 字符）。
std::string nameOfTypeDesc(const jt::TypeDesc& type)
{
    static constexpr char kBaseChars[] = {'B', 'C', 'D', 'F', 'I', 'J', 'S', 'Z'};
    const std::string     dims(static_cast<ca::usize>(type.array_dims), '[');
    if (type.kind == jt::TypeDesc::Kind::Class) {
        if (type.array_dims == 0) return std::string(type.class_name);
        return dims + "L" + std::string(type.class_name) + ";";
    }
    return dims
           + std::string(1, kBaseChars[static_cast<ca::u8>(type.primitive)]);
}

/// @brief JVM 引用赋值兼容判定（标量名面）。
///
/// 同名恒真；任一侧 java/lang/Object 恒真；两侧都可在层次图解析 → 子类型
/// 判定（含接口实现）；任一侧不可解析（外部/JDK 类型无字节码可查）→ 不得
/// 判非法（fail-open）。
bool assignableNames(const HierarchyView& hierarchy, const std::string& expected,
                     const std::string& actual)
{
    if (expected == actual) return true;
    if (expected == "java/lang/Object") return true;
    if (!hierarchy.resolvable(expected) || !hierarchy.resolvable(actual)) return true;
    return hierarchy.isSubtype(actual, expected);
}

/// @brief prim 标量名（B/C/D/F/I/J/S/Z 单字符）→ 帧域 VKind。
VKind primKindOfName(std::string_view name)
{
    if (name == "J") return VKind::Long;
    if (name == "D") return VKind::Double;
    if (name == "F") return VKind::Float;
    return VKind::Int;
}

/// @brief 名字面是否 prim 标量（单字符 BCDFIJSZ；数组/内部名/空均非）。
bool isPrimScalarName(std::string_view name)
{
    return name.size() == 1
           && std::string_view("BCDFIJSZ").find(name.front()) != std::string_view::npos;
}

/// @brief JVM 赋值兼容判定（帧值 × 名字目标面，含数组协变）。
///
/// 未知槽放行；null 对任意引用目标合法；prim 族按格精确（Int/Float/Long/
/// Double）；引用面：深维数组赋浅维引用目标放行（String[] → Object）、
/// primitive 基座数组按标量 Object（int[] 是 Object）、同维引用基座递归、
/// 层次缺席 fail-open。名字面统一内部名口径（标量内部名 / '[' 数组描述符，
/// nameOfTypeDesc 产出）。
bool relaxedAssignable(const HierarchyView& hierarchy, const VType& value,
                       std::string_view targetName)
{
    if (value.k == VKind::Top || value.k == VKind::Bottom) return true;
    const ArrayShape shapeT    = arrayShape(targetName);
    const bool       targetRef = shapeT.dim > 0 || !isPrimScalarName(targetName);
    if (!refKind(value.k)) {
        if (targetRef) return false;   ///< prim 值 → 引用目标
        return value.k == primKindOfName(targetName);
    }
    if (value.k == VKind::Null) return targetRef;
    if (!targetRef) return false;   ///< 引用值 → prim 目标
    if (value.name == targetName) return true;   ///< 同名（含同形数组）恒真

    const ArrayShape shapeV = arrayShape(value.name);
    if (shapeV.dim < shapeT.dim) return false;
    if (shapeV.dim > shapeT.dim) return true;   ///< 深维提升 Object（String[] → Object）
    if (shapeV.dim == 0)
        return assignableNames(hierarchy, std::string(targetName), std::string(value.name));
    // 同维数组：primitive 基座 / 元素类别错配拒绝（int[] ↛ Object[]、
    // String[] ↛ int[]），引用基座递归。
    if (!shapeT.refElem || !shapeV.refElem) return false;
    return assignableNames(hierarchy, shapeT.elem, shapeV.elem);
}

/// @brief 帧栈自顶数第 fromTop 个已知值行（Top/Bottom 未知行跳过；不足为
///        nullptr）。
const VType* valueAt(const FrameAt& frame, ca::usize fromTop)
{
    ca::usize seen = 0;
    for (ca::usize i = frame.stack.size(); i-- > 0;) {
        if (frame.stack[i].k == VKind::Top || frame.stack[i].k == VKind::Bottom) continue;
        if (seen == fromTop) return &frame.stack[i];
        ++seen;
    }
    return nullptr;
}

/// @brief Memberref → 成员描述符（非成员引用失败）。
bool memberDescOf(const jc::ClassFile& cf, ca::u32 cpIndex, std::string_view& desc)
{
    if (!cf.cp.valid(cpIndex)) return false;
    const jc::ConstantPool::Entry& entry = cf.cp.at(cpIndex);
    if (entry.tag != jc::CpTag::Fieldref && entry.tag != jc::CpTag::Methodref &&
        entry.tag != jc::CpTag::InterfaceMethodref)
        return false;
    const jc::ConstantPool::Entry& nat = cf.cp.at(entry.b);
    if (nat.tag != jc::CpTag::NameAndType) return false;
    desc = cf.utf8(nat.b);
    return true;
}

/// @brief Memberref → owner 类内部名（非成员引用/类名缺失失败）。
bool memberOwnerOf(const jc::ClassFile& cf, ca::u32 cpIndex, std::string_view& owner)
{
    if (!cf.cp.valid(cpIndex)) return false;
    const jc::ConstantPool::Entry& entry = cf.cp.at(cpIndex);
    if (entry.tag != jc::CpTag::Fieldref && entry.tag != jc::CpTag::Methodref &&
        entry.tag != jc::CpTag::InterfaceMethodref)
        return false;
    owner = cf.className(entry.a);
    return !owner.empty();
}

/// @brief 单指令类别复核（JVMS 4.10.1 操作数类别面，relaxed 赋值口径）；
///        违规产出 Error（"Method name<desc>: …" 形态）。
void checkInsnKinds(const jc::ClassFile& cf, const MethodSig& sig, const jr::Insn& insn,
                    const FrameAt& frame, const HierarchyView& hierarchy,
                    std::vector<Diagnostic>& diagnostics)
{
    const auto fail = [&diagnostics, &sig](const std::string& detail) {
        diagnostics.push_back(
            {DiagnosticSeverity::Error, DiagnosticStage::Validate,
             utf16("Method " + std::string(sig.name) + std::string(sig.desc) + ": " + detail),
             std::nullopt});
    };
    const auto expectTop = [&frame, &fail](VKind want, const char* what) {
        const VType* got = valueAt(frame, 0);
        if (got != nullptr && got->k != want)
            fail(what + std::string(" with incompatible stack top"));
    };
    const auto expectRefTop = [&frame, &fail](const char* what) {
        const VType* got = valueAt(frame, 0);
        if (got != nullptr && !refKind(got->k))
            fail(what + std::string(" with non-reference stack top"));
    };
    // 引用赋值点：帧值 × 目标描述符（relaxedAssignable 承载协变/fail-open）。
    const auto expectAssignable = [&](const char* what, std::string_view targetName) {
        const VType* got = valueAt(frame, 0);
        if (got != nullptr && !relaxedAssignable(hierarchy, *got, targetName))
            fail(what + std::string(" value is not assignable to ")
                 + std::string(targetName));
    };
    // 成员描述符参数区解析（有成员引用时）。
    std::string_view memberDesc;
    const bool       hasMember = memberDescOf(cf, insn.cp, memberDesc);
    jt::MethodDesc   callee;
    bool             calleeOk = false;
    if (hasMember
        && (insn.op == jc::Op::INVOKEVIRTUAL || insn.op == jc::Op::INVOKESPECIAL
            || insn.op == jc::Op::INVOKESTATIC || insn.op == jc::Op::INVOKEINTERFACE)) {
        const auto parsed = jt::parseMethodDescriptor(memberDesc);
        if (parsed.is_ok()) {
            callee   = std::move(parsed).unwrap();
            calleeOk = true;
        }
    }
    // fromTop 变体的期望检查（valueAt 的非栈顶版）。
    const auto expectTopAt = [&frame, &fail](ca::usize fromTop, VKind want, const char* what) {
        const VType* got = valueAt(frame, fromTop);
        if (got != nullptr && got->k != want)
            fail(what + std::string(" with incompatible stack operand"));
    };
    const auto expectRefTopAt = [&frame, &fail](ca::usize fromTop, const char* what) {
        const VType* got = valueAt(frame, fromTop);
        if (got != nullptr && !refKind(got->k))
            fail(what + std::string(" with non-reference operand"));
    };
    // 实参/接收者检查：按描述符逐参自栈顶向栈底对位（cat2 双槽）。prim 参数
    // 核对精确格；引用参数走 relaxedAssignable（协变捷径/fail-open 放行）。
    const auto checkArgs = [&]() {
        if (!calleeOk) return;
        ca::usize fromTop = 0;
        const auto slotOf = [](const jt::TypeDesc& t) {
            return t.kind == jt::TypeDesc::Kind::Primitive
                       && (t.primitive == jt::BaseType::Long
                           || t.primitive == jt::BaseType::Double)
                     ? 2u
                     : 1u;
        };
        for (ca::usize index = callee.params.size(); index-- > 0;) {
            const jt::TypeDesc& param = callee.params[index];
            if (const VType* got = valueAt(frame, fromTop); got != nullptr) {
                if (param.kind == jt::TypeDesc::Kind::Primitive && param.array_dims == 0) {
                    expectTopAt(fromTop, primKind(param.primitive), "argument");
                }
                else if (!relaxedAssignable(hierarchy, *got, nameOfTypeDesc(param))) {
                    fail("argument is not assignable to " + nameOfTypeDesc(param));
                }
            }
            fromTop += slotOf(param);
        }
        if (insn.op != jc::Op::INVOKESTATIC) {
            const VType* receiver = valueAt(frame, fromTop);
            if (receiver != nullptr) expectRefTopAt(fromTop, "invoke receiver");
            // 接收者名字互赋（虚分发族）：receiver 须可赋给 CP owner 类。
            // INVOKESPECIAL 不查——<init> 的 uninit 槽与 super 调用的 this
            // 接收者相对 owner 是合法非同形态（精确比对会误报构造器链与
            // super.m() 这类 javac 常规形态）。
            if (receiver != nullptr && insn.op != jc::Op::INVOKESPECIAL) {
                std::string_view owner;
                if (memberOwnerOf(cf, insn.cp, owner) && !owner.empty()
                    && owner.front() != '[') {
                    if (!relaxedAssignable(hierarchy, *receiver, owner))
                        fail("invoke receiver is not assignable to " + std::string(owner));
                }
            }
        }
    };

    switch (insn.op) {
    case jc::Op::IRETURN: expectTop(VKind::Int, "ireturn"); break;
    case jc::Op::LRETURN: expectTop(VKind::Long, "lreturn"); break;
    case jc::Op::FRETURN: expectTop(VKind::Float, "freturn"); break;
    case jc::Op::DRETURN: expectTop(VKind::Double, "dreturn"); break;
    case jc::Op::ARETURN: {
        expectRefTop("areturn");
        const VType* got = valueAt(frame, 0);
        // 返回值互赋：栈顶须可赋给方法描述符的返回类型（引用返回才查；
        // prim 返回类别由 IRETURN 族承载）。relaxed 口径：数组协变放行
        // （String[] 作 Object[] 返回，strict 档按约拒绝）。
        const ca::usize close = std::string_view(sig.desc).find(')');
        if (got != nullptr && close != std::string_view::npos) {
            const std::string_view ret = std::string_view(sig.desc).substr(close + 1);
            const auto parsed             = jt::parseFieldType(ret);
            if (parsed.is_ok()) {
                const jt::TypeDesc& type = parsed.unwrap();
                if (type.kind == jt::TypeDesc::Kind::Class
                    || type.array_dims > 0) {
                    if (!relaxedAssignable(hierarchy, *got, nameOfTypeDesc(type)))
                        fail("areturn value is not assignable to " + std::string(ret));
                }
            }
        }
        break;
    }
    case jc::Op::RETURN:
        if (!sig.desc.empty()
            && std::string_view(sig.desc).find(")V") != std::string_view(sig.desc).size() - 2)
            fail("return in method with non-void return type");
        break;
    case jc::Op::ARRAYLENGTH: expectRefTop("arraylength"); break;
    case jc::Op::IALOAD: case jc::Op::LALOAD: case jc::Op::FALOAD: case jc::Op::DALOAD:
    case jc::Op::BALOAD: case jc::Op::CALOAD: case jc::Op::SALOAD:
    case jc::Op::AALOAD:
        expectTopAt(1, VKind::Int, "array load");   ///< index
        expectRefTopAt(0, "array load");            ///< arrayref
        break;
    case jc::Op::IASTORE: case jc::Op::LASTORE: case jc::Op::FASTORE: case jc::Op::DASTORE:
    case jc::Op::BASTORE: case jc::Op::CASTORE: case jc::Op::SASTORE:
        expectRefTopAt(0, "array store");    ///< value（prim 数组元素类别由帧域
                                             ///< 宽度面承载）
        expectTopAt(1, VKind::Int, "array store");   ///< index
        expectRefTopAt(2, "array store");            ///< arrayref
        break;
    case jc::Op::AASTORE:
        // aastore 值只核引用形态不核元素类型（relaxed 收窄，见头文件差异 5）。
        expectRefTopAt(0, "array store");
        expectTopAt(1, VKind::Int, "array store");
        expectRefTopAt(2, "array store");
        break;
    case jc::Op::ATHROW:
    case jc::Op::MONITORENTER:
    case jc::Op::MONITOREXIT: expectRefTop("instruction"); break;
    case jc::Op::GETFIELD: {
        // 到达帧栈顶就是 receiver（字段值尚未压栈）。
        if (hasMember) expectRefTop("field access");
        break;
    }
    case jc::Op::PUTFIELD: {
        if (hasMember) {
            const ca::usize slots = (memberDesc == "J" || memberDesc == "D") ? 2u : 1u;
            const auto      parsed = jt::parseFieldType(memberDesc);
            if (parsed.is_ok()) {
                const jt::TypeDesc& field = parsed.unwrap();
                if (const VType* got = valueAt(frame, 0); got != nullptr) {
                    if (field.kind == jt::TypeDesc::Kind::Primitive && field.array_dims == 0) {
                        expectTop(primKind(field.primitive), "field store");
                    }
                    else if (!relaxedAssignable(hierarchy, *got, nameOfTypeDesc(field))) {
                        fail("field store value is not assignable to "
                             + std::string(memberDesc));
                    }
                }
            }
            expectRefTopAt(slots, "field access");
        }
        break;
    }
    case jc::Op::PUTSTATIC: {
        if (hasMember) {
            const auto parsed = jt::parseFieldType(memberDesc);
            if (parsed.is_ok()) {
                const jt::TypeDesc& field = parsed.unwrap();
                if (const VType* got = valueAt(frame, 0); got != nullptr) {
                    if (field.kind == jt::TypeDesc::Kind::Primitive && field.array_dims == 0) {
                        expectTop(primKind(field.primitive), "field store");
                    }
                    else if (!relaxedAssignable(hierarchy, *got, nameOfTypeDesc(field))) {
                        fail("field store value is not assignable to "
                             + std::string(memberDesc));
                    }
                }
            }
        }
        break;
    }
    case jc::Op::INVOKEVIRTUAL:
    case jc::Op::INVOKESPECIAL:
    case jc::Op::INVOKESTATIC:
    case jc::Op::INVOKEINTERFACE: checkArgs(); break;
    case jc::Op::ISTORE: case jc::Op::ISTORE_0: case jc::Op::ISTORE_1:
    case jc::Op::ISTORE_2: case jc::Op::ISTORE_3:
        expectTop(VKind::Int, "istore");
        break;
    case jc::Op::LSTORE: case jc::Op::LSTORE_0: case jc::Op::LSTORE_1:
    case jc::Op::LSTORE_2: case jc::Op::LSTORE_3:
        expectTop(VKind::Long, "lstore");
        break;
    case jc::Op::FSTORE: case jc::Op::FSTORE_0: case jc::Op::FSTORE_1:
    case jc::Op::FSTORE_2: case jc::Op::FSTORE_3:
        expectTop(VKind::Float, "fstore");
        break;
    case jc::Op::DSTORE: case jc::Op::DSTORE_0: case jc::Op::DSTORE_1:
    case jc::Op::DSTORE_2: case jc::Op::DSTORE_3:
        expectTop(VKind::Double, "dstore");
        break;
    case jc::Op::ASTORE: case jc::Op::ASTORE_0: case jc::Op::ASTORE_1:
    case jc::Op::ASTORE_2: case jc::Op::ASTORE_3:
        expectRefTop("astore");
        break;
    case jc::Op::TABLESWITCH:
    case jc::Op::LOOKUPSWITCH: expectTop(VKind::Int, "switch"); break;
    case jc::Op::CHECKCAST:
    case jc::Op::INSTANCEOF:
    case jc::Op::IFNULL:
    case jc::Op::IFNONNULL: expectRefTop("instruction"); break;
    default: break;
    }
}

std::vector<Diagnostic> analyzeRelaxedClass(const std::vector<ca::u8>&               classBytes,
                                            const mjbase::hierarchy::ClassHierarchy* hierarchy)
{
    std::vector<Diagnostic> diagnostics;
    try {
        auto opened = jr::makeSessionFromBytes(jc::ByteView{classBytes.data(), classBytes.size()});
        if (opened.is_err()) {
            const auto& failure = opened.unwrap_err();
            const auto  message
                = failure.stage == jr::SessionError::Stage::Read
                      ? std::string("classfile read failed: ") + jc::to_string(failure.read)
                      : std::string("classfile session failed: ") + jr::to_string(failure.session);
            diagnostics.push_back(
                {DiagnosticSeverity::Error, DiagnosticStage::Validate, utf16(message),
                 std::nullopt});
            return diagnostics;
        }
        auto              owned   = std::move(opened).unwrap();   // shared_ptr：持活会话
        jr::ClassSession& session = *owned;
        jc::ClassFile&    cf      = session.backing();

        const HierarchyView view(hierarchy);
        RelaxedResolver     resolver(view);
        for (ca::usize index = 0; index < cf.methods.size(); ++index) {
            const jc::MemberInfo& m    = cf.methods[index];
            const std::string     name{cf.utf8(m.nameCp)};
            const std::string     desc{cf.utf8(m.descCp)};
            if (!jr::hasCode(session, index)) continue;
            auto promoted = jr::promoteMethod(session, index);
            if (promoted.is_err()) {
                diagnostics.push_back(
                    {DiagnosticSeverity::Error, DiagnosticStage::Validate,
                     utf16("Method " + name + desc + ": body promote failed: "
                           + jr::to_string(promoted.unwrap_err())),
                     std::nullopt});
                continue;
            }
            jr::CodeUnitRef unit = std::move(promoted).unwrap();

            MethodSig sig;
            sig.isStatic  = (m.access & 0x0008) != 0;
            sig.desc      = cf.utf8(m.descCp);
            sig.name      = cf.utf8(m.nameCp);
            sig.thisClass = cf.thisName();

            FrameEngine               engine(cf.cp, resolver, FramePolicy::Strict);
            std::vector<FrameAt>      frames;
            const ja::FrameSummary summary = engine.compute(*unit, sig, frames);
            if (!summary.ok) {
                diagnostics.push_back(
                    {DiagnosticSeverity::Error, DiagnosticStage::Validate,
                     utf16("Method " + name + desc + ": analysis failed: "
                           + ja::to_string(summary.fail)),
                     std::nullopt});
                continue;
            }
            ca::usize position = 0;
            for (const jr::InsnId id : unit->dom.order()) {
                if (position >= frames.size()) break;
                const jr::Insn* insn = unit->dom.peek(id);
                if (insn != nullptr && frames[position].reachable)
                    checkInsnKinds(cf, sig, *insn, frames[position], view, diagnostics);
                ++position;
            }
        }
    }
    catch (const std::exception& exception) {
        diagnostics.push_back({DiagnosticSeverity::Error, DiagnosticStage::Validate,
                               utf16(exception.what()), std::nullopt});
    }
    return diagnostics;
}

}   // namespace

std::vector<Diagnostic> analyzeRelaxed(const std::vector<ca::u8>&                    classBytes,
                                       const mjbase::hierarchy::ClassHierarchy* hierarchy)
{
    return analyzeRelaxedClass(classBytes, hierarchy);
}

}   // namespace mjbase::jasm
