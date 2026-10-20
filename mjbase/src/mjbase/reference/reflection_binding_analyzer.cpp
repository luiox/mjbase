#include "mjbase/reference/reflection_binding_analyzer.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

#include "jbc/analysis/domain.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/analysis/cp_refs.hpp"
#include "mjbase/asm/analyzer/frame_query.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/reference/reflection_interpreter.hpp"

namespace mjbase::reference {

using jbc::core::AttrSlot;
using jbc::core::CpTag;
using jbc::core::Op;
using jbc::analysis::VKind;
using jbc::ir::SymbolId;
using ConstSem     = jbc::ir::ClassSession::ConstSem;
using MemberRefSem = mjbase::analysis::MemberRefSem;
using mjbase::analysis::resolveClassName;
using mjbase::analysis::resolveMemberRef;

namespace {

// ── 基础助手 ─────────────────────────────────────────────────

/// 方法描述符的参数类型序列（"(" 后逐类型；与 alias_analysis 同源扫描口径）。
std::vector<std::string_view> paramTypes(std::string_view desc)
{
    std::vector<std::string_view> types;
    if (desc.empty() || desc.front() != '(') return types;
    ca::usize pos = 1;
    while (pos < desc.size() && desc[pos] != ')') {
        const std::string_view t = jbc::analysis::detail::oneType(desc, pos);
        if (t.empty()) break;
        types.push_back(t);
        pos += t.size();
    }
    return types;
}

/// 追踪名（internal name / 数组描述符 / 单字符原始类型）转参数描述符形态。
std::string toParameterDescriptor(std::string_view name)
{
    if (name.size() == 1) return std::string(name);
    if (!name.empty() && name.front() == '[') return std::string(name);
    return "L" + std::string(name) + ";";
}

bool isInvokeFamily(Op op)
{
    return op == Op::INVOKEVIRTUAL || op == Op::INVOKESPECIAL || op == Op::INVOKESTATIC ||
           op == Op::INVOKEINTERFACE;
}

bool isStringBuilderOwner(std::string_view owner)
{
    return owner == "java/lang/StringBuilder" || owner == "java/lang/StringBuffer";
}

bool isStringBuilderType(std::string_view internalName)
{
    return internalName == "java/lang/StringBuilder" || internalName == "java/lang/StringBuffer";
}

/// 原始类型 TYPE 常量（Integer.TYPE 等）→ 单字符描述符；非 TYPE 返回空。
std::string_view primitiveTypeFieldName(std::string_view owner, std::string_view name)
{
    if (name != "TYPE") return {};
    if (owner == "java/lang/Boolean") return "Z";
    if (owner == "java/lang/Byte") return "B";
    if (owner == "java/lang/Character") return "C";
    if (owner == "java/lang/Short") return "S";
    if (owner == "java/lang/Integer") return "I";
    if (owner == "java/lang/Long") return "J";
    if (owner == "java/lang/Float") return "F";
    if (owner == "java/lang/Double") return "D";
    if (owner == "java/lang/Void") return "V";
    return {};
}

// ── sink 目录（数据驱动，旧栈逐项照迁）────────────────────────

std::vector<ReflectionSinkEntry> buildSinkCatalog()
{
    std::vector<ReflectionSinkEntry> catalog;
    auto add = [&catalog](const char* owner, const char* name, const char* desc,
                          ReflectionApiKind kind, int classNameArg, int memberNameArg,
                          int paramTypesArg, bool requiresClassReceiver, bool producesMemberObject,
                          bool catalogOnly, bool memberEnumeration) {
        catalog.push_back({owner,
                           name,
                           desc,
                           kind,
                           classNameArg,
                           memberNameArg,
                           paramTypesArg,
                           requiresClassReceiver,
                           producesMemberObject,
                           catalogOnly,
                           memberEnumeration});
    };

    // 类加载（类名实参槽 0）
    add("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;",
        ReflectionApiKind::ClassForName, 0, -1, -1, false, false, false, false);
    add("java/lang/Class", "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",
        ReflectionApiKind::ClassForName, 0, -1, -1, false, false, false, false);
    add("java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;",
        ReflectionApiKind::ClassLoaderLoadClass, 0, -1, -1, false, false, false, false);

    // 成员查找（单数）：接收者必须是已知 Class 对象
    add("java/lang/Class", "getMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",
        ReflectionApiKind::MemberLookup, -1, 0, 1, true, true, false, false);
    add("java/lang/Class", "getDeclaredMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",
        ReflectionApiKind::MemberLookup, -1, 0, 1, true, true, false, false);
    add("java/lang/Class", "getConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",
        ReflectionApiKind::MemberLookup, -1, -1, 0, true, true, false, false);
    add("java/lang/Class", "getDeclaredConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",
        ReflectionApiKind::MemberLookup, -1, -1, 0, true, true, false, false);
    add("java/lang/Class", "getField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;",
        ReflectionApiKind::MemberLookup, -1, 0, -1, true, true, false, false);
    add("java/lang/Class", "getDeclaredField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;",
        ReflectionApiKind::MemberLookup, -1, 0, -1, true, true, false, false);

    // 成员查找（复数枚举）：无单一目标，恒 Pending(member_enumeration)
    add("java/lang/Class", "getMethods", "()[Ljava/lang/reflect/Method;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getDeclaredMethods", "()[Ljava/lang/reflect/Method;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getConstructors", "()[Ljava/lang/reflect/Constructor;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getDeclaredConstructors", "()[Ljava/lang/reflect/Constructor;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getFields", "()[Ljava/lang/reflect/Field;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getDeclaredFields", "()[Ljava/lang/reflect/Field;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);

    // 反射执行 / 字段访问
    add("java/lang/reflect/Method", "invoke",
        "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",
        ReflectionApiKind::MethodInvoke, -1, -1, -1, false, false, false, false);
    add("java/lang/reflect/Constructor", "newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;",
        ReflectionApiKind::ConstructorNewInstance, -1, -1, -1, false, false, false, false);
    add("java/lang/Class", "newInstance", "()Ljava/lang/Object;",
        ReflectionApiKind::ClassNewInstance, -1, -1, -1, false, false, false, false);
    add("java/lang/reflect/Field", "get", "(Ljava/lang/Object;)Ljava/lang/Object;",
        ReflectionApiKind::FieldAccess, -1, -1, -1, false, false, false, false);
    add("java/lang/reflect/Field", "set", "(Ljava/lang/Object;Ljava/lang/Object;)V",
        ReflectionApiKind::FieldAccess, -1, -1, -1, false, false, false, false);

    // MethodHandles.Lookup：catalog-only（首版不解析）
    add("java/lang/invoke/MethodHandles$Lookup", "findVirtual",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findStatic",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findConstructor",
        "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findSpecial",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findGetter",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findSetter",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)V",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findStaticGetter",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findStaticSetter",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)V",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflect",
        "(Ljava/lang/reflect/Method;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectConstructor",
        "(Ljava/lang/reflect/Constructor;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectGetter",
        "(Ljava/lang/reflect/Field;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectSetter",
        "(Ljava/lang/reflect/Field;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectSpecial",
        "(Ljava/lang/reflect/Method;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectField",
        "(Ljava/lang/reflect/Field;Z)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);

    return catalog;
}

const ReflectionSinkEntry* matchSink(std::string_view owner, std::string_view name,
                                     std::string_view desc)
{
    static const std::vector<ReflectionSinkEntry> catalog = buildSinkCatalog();
    for (const auto& entry : catalog) {
        if (owner == entry.owner && name == entry.name && desc == entry.desc) {
            return &entry;
        }
    }
    return nullptr;
}

// ── 抽象值域载荷 ─────────────────────────────────────────────
//
// 覆盖反射 sink 实参追踪所需的最小闭包：字符串内容、类名、SB 链、Class[]/
// String[] 名表、成员对象、int（下标/拼接）。其余全部 Unknown（fail-closed）。
// 可变载荷（SB/数组）经 shared_ptr 共享（引用语义，变异 pass 依赖）；
// 纯值路径的分支克隆不污染兄弟路径——共享变异的冲突折损由 merge 兜底。

struct StringBuilderState
{
    std::string text;   ///< 已知内容（UTF-8 字节，classfile 解码后口径）
    bool known     = true;
    /// @brief 内容含歧义段（AmbiguousString 实参）：文本仍是已知前缀，但已不完整。
    bool ambiguous = false;
};

struct ArrayState
{
    bool isClassArray = false;
    /// @brief 元素个数未知、下标非常量或合并冲突时置 true：aaload 结果降级。
    bool    ambiguous = false;
    bool    sizeKnown = false;
    ca::i32 size      = 0;
    /// @brief 元素值：ClassArray 存类名（追踪名形态），NameTable 存字符串。
    std::vector<std::optional<std::string>> elements;

    bool operator==(const ArrayState& other) const
    {
        return isClassArray == other.isClassArray && ambiguous == other.ambiguous &&
               sizeKnown == other.sizeKnown && size == other.size && elements == other.elements;
    }
};

struct MemberState
{
    bool        isField = false;
    std::string owner;   ///< 目标类 internal name
    std::string name;    ///< 成员名（构造器为 "<init>"；未知为空）
    std::string desc;    ///< 参数类型全量可判定时的方法描述符；否则为空（fail-closed）

    bool operator==(const MemberState& other) const
    {
        return isField == other.isField && owner == other.owner && name == other.name &&
               desc == other.desc;
    }
};

struct BindingValue
{
    enum class Kind
    {
        Unknown,
        String,
        /// @brief 已知是字符串但内容受非常量下标影响（名表 aaload 非常量下标产物）。
        AmbiguousString,
        StringBuilder,
        ClassObject,
        ClassArray,
        NameTable,
        MemberObject,
        Int,
    };

    Kind                                kind     = Kind::Unknown;
    std::string                         text;    ///< String/ClassObject 载荷
    int                                 intValue = 0;
    std::shared_ptr<StringBuilderState> builder;
    std::shared_ptr<ArrayState>         array;
    std::shared_ptr<MemberState>        member;
    ca::u8                              slots = 1;   ///< 槽位（cat2=2，随 LDC2_W 形态）

    static BindingValue unknown(ca::u8 width = 1)
    {
        BindingValue value;
        value.slots = width;
        return value;
    }

    bool operator==(const BindingValue& other) const
    {
        if (kind != other.kind) return false;
        switch (kind) {
        case Kind::Unknown: return true;
        case Kind::String:
        case Kind::AmbiguousString:
        case Kind::ClassObject: return text == other.text;
        case Kind::Int: return intValue == other.intValue;
        case Kind::StringBuilder:
            if (!builder || !other.builder) return builder == other.builder;
            return builder->known == other.builder->known &&
                   builder->ambiguous == other.builder->ambiguous &&
                   (!builder->known || builder->text == other.builder->text);
        case Kind::ClassArray:
        case Kind::NameTable:
            if (!array || !other.array) return array == other.array;
            return *array == *other.array;
        case Kind::MemberObject:
            if (!member || !other.member) return member == other.member;
            return *member == *other.member;
        }
        return true;
    }
    bool operator!=(const BindingValue& other) const { return !(*this == other); }
};

/// @brief SB/concat 实参渲染结果：text 为可拼接文本段，ambiguous 标记该段来自
/// 歧义产物（AmbiguousString，内容受非常量下标影响、文本只是已知前缀）。
struct RenderedConcatPart
{
    std::string text;
    bool        ambiguous = false;
};

/// @brief SB/concat 实参渲染：String 原样、Int 十进制、AmbiguousString 产出歧义
/// 哨兵段（区分"无法渲染"与"确认含歧义"，保 reason 归因）；其余 fail-closed。
std::optional<RenderedConcatPart> renderConcatArgument(const BindingValue& value)
{
    if (value.kind == BindingValue::Kind::String)
        return RenderedConcatPart{value.text, false};
    if (value.kind == BindingValue::Kind::Int)
        return RenderedConcatPart{std::to_string(value.intValue), false};
    if (value.kind == BindingValue::Kind::AmbiguousString)
        return RenderedConcatPart{std::string(), true};
    return std::nullopt;
}

}   // namespace

// ── 域插件（迭代探测-注入 + 延迟变异 pass 的纯值面）────────────

namespace {

/// 域环境：池/分析器常量表/注入表。
struct BindingDomainEnv
{
    const jbc::core::ConstantPool*                             cp       = nullptr;
    const std::unordered_map<std::string_view, std::string_view>* stringConstants = nullptr;
    const std::unordered_map<std::string_view, std::vector<std::string>>* nameTables = nullptr;
    const std::unordered_map<ca::u32, BindingValue>*            injected = nullptr;
};

class BindingDomain final : public jbc::analysis::IValueDomain<BindingValue>
{
public:
    explicit BindingDomain(const BindingDomainEnv& env) noexcept
        : env_(env)
    {}

    BindingValue top() override { return BindingValue::unknown(); }

    /// join：内容一致保留；SB 冲突保形态降内容；数组冲突逐位折损 + ambiguous；
    /// 其余 Unknown（fail-closed，旧栈 merge 照迁）。
    BindingValue merge(const BindingValue& value1, const BindingValue& value2) override
    {
        if (value1 == value2) {
            BindingValue result = value1;
            result.slots        = std::max(value1.slots, value2.slots);
            return result;
        }
        // StringBuilder 合并：内容一致保留，冲突降 unknown 内容（保形态）。
        if (value1.kind == BindingValue::Kind::StringBuilder &&
            value2.kind == BindingValue::Kind::StringBuilder && value1.builder && value2.builder) {
            auto         state = std::make_shared<StringBuilderState>();
            state->known       = false;
            BindingValue result = BindingValue::unknown();
            result.kind         = BindingValue::Kind::StringBuilder;
            result.builder      = std::move(state);
            return result;
        }
        // ClassArray/NameTable 合并：逐位一致保留，冲突置 ambiguous（保数组形态）。
        if (((value1.kind == BindingValue::Kind::ClassArray &&
              value2.kind == BindingValue::Kind::ClassArray) ||
             (value1.kind == BindingValue::Kind::NameTable &&
              value2.kind == BindingValue::Kind::NameTable)) &&
            value1.array && value2.array &&
            value1.array->isClassArray == value2.array->isClassArray &&
            value1.array->sizeKnown == value2.array->sizeKnown &&
            value1.array->size == value2.array->size &&
            value1.array->elements.size() == value2.array->elements.size()) {
            auto state = std::make_shared<ArrayState>(*value1.array);
            for (std::size_t i = 0; i < state->elements.size(); ++i) {
                const auto& a = value1.array->elements[i];
                const auto& b = value2.array->elements[i];
                if (a.has_value() != b.has_value() || (a.has_value() && *a != *b)) {
                    state->elements[i].reset();
                    state->ambiguous = true;
                }
            }
            if (value1.array->ambiguous || value2.array->ambiguous) state->ambiguous = true;
            BindingValue result = BindingValue::unknown();
            result.kind         = value1.kind;
            result.array        = std::move(state);
            return result;
        }
        return BindingValue::unknown(static_cast<ca::u8>(std::max(value1.slots, value2.slots)));
    }

    /// 纯产值点：int 常量、ldc、GETSTATIC 常量/名表回填、NEW SB。操作数依赖
    /// 产点（ANEWARRAY/AALOAD 之外还有 invoke/indy/toString）查注入表。
    BindingValue newValue(const jbc::ir::Insn& in, jbc::ir::InsnId at, VKind shape) override
    {
        const ca::u8 width = shape == VKind::Long || shape == VKind::Double ? 2 : 1;
        if (env_.injected != nullptr) {
            const auto injected = env_.injected->find(at.slot);
            if (injected != env_.injected->end()) {
                BindingValue v = injected->second;
                v.slots        = width;
                return v;
            }
        }

        // int 常量：名表下标与拼接需要（binding 口径接受负常量）。
        ca::i32 constant = 0;
        if (intConstantOf(in, constant, true)) {
            BindingValue value = BindingValue::unknown(width);
            value.kind         = BindingValue::Kind::Int;
            value.intValue     = constant;
            return value;
        }

        if (in.op == Op::LDC || in.op == Op::LDC_W || in.op == Op::LDC2_W) {
            BindingValue value = BindingValue::unknown(width);
            if (!env_.cp->valid(in.cp)) return value;
            const auto& e = env_.cp->at(in.cp);
            if (e.tag == CpTag::String && env_.cp->isUtf8(e.a)) {
                value.kind = BindingValue::Kind::String;
                value.text = std::string(env_.cp->utf8Of(e.a));
                return value;
            }
            if (e.tag == CpTag::Integer) {
                value.kind     = BindingValue::Kind::Int;
                value.intValue = e.ival;
                return value;
            }
            if (e.tag == CpTag::Class && env_.cp->isUtf8(e.a)) {
                // 类常量转追踪名：数组取描述符原样，其余取内部名（旧栈
                // classLiteralName 口径；CP Class 条目恒为 OBJECT/ARRAY 形态）。
                const std::string_view n = env_.cp->utf8Of(e.a);
                if (!n.empty()) {
                    value.kind = BindingValue::Kind::ClassObject;
                    value.text = std::string(n);
                }
            }
            return value;
        }

        if (in.op == Op::GETSTATIC) {
            BindingValue value = BindingValue::unknown(width);
            MemberRefSem ref;
            if (!resolveMemberRef(*env_.cp, in.cp, ref)) return value;

            if (ref.desc == "Ljava/lang/String;") {
                // 表键 "owner.name" 为驻留视图；查询键瞬时拼接（不入存储）。
                const std::string constantKey = std::string(ref.owner) + "." + std::string(ref.name);
                const auto      found = env_.stringConstants->find(constantKey);
                if (found != env_.stringConstants->end()) {
                    value.kind = BindingValue::Kind::String;
                    value.text = std::string(found->second);
                }
                return value;
            }
            if (ref.desc == "[Ljava/lang/String;") {
                const std::string tableKey = std::string(ref.owner) + "." + std::string(ref.name);
                const auto      found = env_.nameTables->find(tableKey);
                if (found != env_.nameTables->end()) {
                    auto state          = std::make_shared<ArrayState>();
                    state->isClassArray = false;
                    state->sizeKnown    = true;
                    state->size         = static_cast<ca::i32>(found->second.size());
                    state->elements.reserve(found->second.size());
                    for (const auto& element : found->second) {
                        state->elements.emplace_back(element);
                    }
                    value.kind  = BindingValue::Kind::NameTable;
                    value.array = std::move(state);
                }
                return value;
            }
            if (ref.desc == "Ljava/lang/Class;") {
                const std::string_view primitive = primitiveTypeFieldName(ref.owner, ref.name);
                if (!primitive.empty()) {
                    value.kind = BindingValue::Kind::ClassObject;
                    value.text = std::string(primitive);
                }
            }
            return value;
        }

        if (in.op == Op::NEW) {
            const std::string_view cls = resolveClassName(*env_.cp, in.cp);
            if (isStringBuilderType(cls)) {
                BindingValue value = BindingValue::unknown(width);
                auto         state = std::make_shared<StringBuilderState>();
                state->known        = true;
                value.kind          = BindingValue::Kind::StringBuilder;
                value.builder       = std::move(state);
                return value;
            }
        }

        return BindingValue::unknown(width);
    }

    /// 一元：CHECKCAST 类型断言透传；ANEWARRAY 操作数依赖（注入表补齐）。
    BindingValue unary(const jbc::core::Op op, const BindingValue& a, VKind shape,
                       const jbc::ir::Insn& at, jbc::ir::InsnId id) override
    {
        (void)at;
        (void)id;
        if (op == Op::CHECKCAST) return a;
        return BindingValue::unknown(shape == VKind::Long || shape == VKind::Double ? 2 : 1);
    }

    /// 二元：AALOAD（Class[]/名表下标取值）与 IADD/ISUB（Int 算术）。
    BindingValue binary(const jbc::core::Op op, const BindingValue& value1,
                        const BindingValue& value2, VKind shape, const jbc::ir::Insn& at,
                        jbc::ir::InsnId id) override
    {
        (void)at;
        (void)id;
        const ca::u8     width    = shape == VKind::Long || shape == VKind::Double ? 2 : 1;
        const BindingValue fallback = BindingValue::unknown(width);

        if (op == Op::AALOAD) {
            const BindingValue& array = value1;
            const BindingValue& index = value2;
            if (array.kind == BindingValue::Kind::ClassArray) {
                // Class[] 元素：常量下标精确取类名；其余 fail-closed Unknown。
                if (array.array && !array.array->ambiguous && array.array->sizeKnown &&
                    index.kind == BindingValue::Kind::Int && index.intValue >= 0 &&
                    static_cast<std::size_t>(index.intValue) < array.array->elements.size()) {
                    const auto& element =
                        array.array->elements[static_cast<std::size_t>(index.intValue)];
                    if (element) {
                        BindingValue result = fallback;
                        result.kind         = BindingValue::Kind::ClassObject;
                        result.text         = *element;
                        return result;
                    }
                }
                return fallback;
            }
            if (array.kind == BindingValue::Kind::NameTable) {
                // 名表：常量下标精确；非常量下标 → AmbiguousString（该 sink 整体
                // 降 Pending(ambiguous_array)，issue #1026 口径）。
                BindingValue result = fallback;
                if (!array.array || array.array->ambiguous ||
                    index.kind != BindingValue::Kind::Int) {
                    result.kind = BindingValue::Kind::AmbiguousString;
                    return result;
                }
                if (index.intValue >= 0 &&
                    static_cast<std::size_t>(index.intValue) < array.array->elements.size()) {
                    const auto& element =
                        array.array->elements[static_cast<std::size_t>(index.intValue)];
                    if (element) {
                        result.kind = BindingValue::Kind::String;
                        result.text = *element;
                    }
                }
                return result;
            }
            return fallback;
        }

        if ((op == Op::IADD || op == Op::ISUB) && value1.kind == BindingValue::Kind::Int &&
            value2.kind == BindingValue::Kind::Int) {
            BindingValue result = fallback;
            result.kind         = BindingValue::Kind::Int;
            result.intValue     = op == Op::IADD ? value1.intValue + value2.intValue
                                                 : value1.intValue - value2.intValue;
            return result;
        }
        return fallback;
    }

    BindingValue newParameter(ca::u32 localIdx, VKind shape) override
    {
        (void)localIdx;
        return BindingValue::unknown(shape == VKind::Long || shape == VKind::Double ? 2 : 1);
    }

    BindingValue newException(const jbc::ir::CodeUnit::Exc&) override
    {
        return BindingValue::unknown();
    }

    ca::u8 width(const BindingValue& v) override { return v.slots; }

    bool equal(const BindingValue& a, const BindingValue& b) override { return a == b; }

private:
    BindingDomainEnv env_;
};

}   // namespace

// ── 标签函数 ─────────────────────────────────────────────────

const char* reflectionApiKindLabel(ReflectionApiKind kind)
{
    switch (kind) {
    case ReflectionApiKind::ClassForName: return "Class.forName";
    case ReflectionApiKind::ClassLoaderLoadClass: return "ClassLoader.loadClass";
    case ReflectionApiKind::MemberLookup: return "Class.member-lookup";
    case ReflectionApiKind::MethodInvoke: return "Method.invoke";
    case ReflectionApiKind::ConstructorNewInstance: return "Constructor.newInstance";
    case ReflectionApiKind::ClassNewInstance: return "Class.newInstance";
    case ReflectionApiKind::FieldAccess: return "Field.access";
    case ReflectionApiKind::LookupMethodHandle: return "MethodHandles.Lookup";
    }
    return "unknown";
}

const char* reflectionPendingReasonLabel(ReflectionPendingReason reason)
{
    switch (reason) {
    case ReflectionPendingReason::None: return "";
    case ReflectionPendingReason::UnknownArgs: return "unknown_args";
    case ReflectionPendingReason::AmbiguousArray: return "ambiguous_array";
    case ReflectionPendingReason::UnknownReceiver: return "unknown_receiver";
    case ReflectionPendingReason::NoSuchClass: return "no_such_class";
    case ReflectionPendingReason::NoSuchMember: return "no_such_member";
    case ReflectionPendingReason::MemberEnumeration: return "member_enumeration";
    }
    return "unknown";
}

const char* reflectionBindingStateLabel(ReflectionBindingState state)
{
    switch (state) {
    case ReflectionBindingState::Resolved: return "resolved";
    case ReflectionBindingState::Pending: return "pending";
    case ReflectionBindingState::Unsupported: return "unsupported";
    }
    return "unknown";
}

const std::vector<ReflectionSinkEntry>& reflectionSinkCatalog()
{
    static const std::vector<ReflectionSinkEntry> catalog = buildSinkCatalog();
    return catalog;
}

// ── 方法体管线（探测-注入 + 延迟变异 pass 的编排）────────────

namespace {

using FrameQueryT    = mjbase::asm_analyzer::FrameQuery<BindingValue>;
using InjectionTable = std::unordered_map<ca::u32, BindingValue>;

/// 注入表 upsert：值等价（BindingValue::operator==）时不计变化——共享对象每轮
/// 重建的收敛判定靠它（内容等价 ⇒ 注入稳定）。
bool upsertInjection(InjectionTable& table, ca::u32 slot, const BindingValue& value)
{
    const auto it = table.find(slot);
    if (it == table.end()) {
        table.emplace(slot, value);
        return true;
    }
    if (it->second == value) return false;
    it->second = value;
    return true;
}

/// 一个方法体分析会话：域环境/注入表/帧查询的共用载体。
struct MethodPipeline
{
    const jbc::ir::CodeUnit*       code = nullptr;
    const jbc::core::ConstantPool* cp   = nullptr;
    BindingDomainEnv               env;
    InjectionTable                 injected;
    std::unique_ptr<FrameQueryT>   frames;

    /// 引擎一轮（注入表挂进环境）；JSR/RET 或畸形控制流 → false（fail-open）。
    bool runEngine(bool isStatic, std::string_view desc)
    {
        env.injected                              = &injected;
        BindingDomain                             dom(env);
        const jbc::analysis::MethodSig            sig{isStatic, desc};
        jbc::analysis::DomainEngine<BindingValue> engine(*cp, dom);
        jbc::analysis::DomainResult<BindingValue> result = engine.analyze(*code, sig);
        if (!result.summary.ok || result.summary.hasSubroutine) {
            frames.reset();
            return false;
        }
        frames = std::make_unique<FrameQueryT>(*code, std::move(result));
        return true;
    }
};

/// @brief StringConcatFactory indy 折叠（旧栈 foldConcat 照迁）。
///
/// makeConcatWithConstants：recipe（\u0001 占位按序消费动态实参；\u0002 静态
/// 常量占位 fail-closed）；makeConcat：全参顺序拼接。歧义实参经哨兵渲染不阻断
/// 折叠；任一实参完全不可渲染 → Unknown。
BindingValue foldConcat(bool withConstants, std::string_view recipe,
                        const std::vector<BindingValue>& values)
{
    bool        ambiguous = false;
    std::string out;
    auto appendPart = [&](const BindingValue& value) -> bool {
        auto part = renderConcatArgument(value);
        if (!part) return false;
        if (part->ambiguous) {
            ambiguous = true;
        }
        else {
            out += part->text;
        }
        return true;
    };

    if (withConstants) {
        ca::usize argIdx = 0;
        for (const char c : recipe) {
            if (c == '\x01') {
                if (argIdx >= values.size()) return BindingValue::unknown();
                if (!appendPart(values[argIdx])) return BindingValue::unknown();
                ++argIdx;
            }
            else if (c == '\x02') {
                return BindingValue::unknown();   // 静态常量占位（bsmArgs[1..]）：罕见，fail-closed
            }
            else {
                out += c;
            }
        }
    }
    else {
        for (const auto& value : values) {
            if (!appendPart(value)) return BindingValue::unknown();
        }
    }
    BindingValue result = BindingValue::unknown();
    if (ambiguous) {
        result.kind = BindingValue::Kind::AmbiguousString;
        return result;
    }
    result.kind = BindingValue::Kind::String;
    result.text = std::move(out);
    return result;
}

/// @brief indy 拼接产点（StringConcatFactory.makeConcat*）；非拼接 indy 返回
/// unknown（与旧栈 basic fallback 一致，仍走 upsert 保持无方向性陈旧）。
BindingValue concatProduction(MethodPipeline& p, const jbc::ir::Insn& in, jbc::ir::InsnId id,
                              jbc::ir::ClassSession& session)
{
    // indy 的 bsm 不在指令上：CP InvokeDynamic.a → BootstrapMethods 表项。
    // decodeAttr 是非 const 突变缓存接口：backing 取非 const 引用。
    jbc::core::ClassFile&     cf = session.backing();
    const auto*                 body =
        [&]() -> const jbc::core::AttrBody* {
            for (const AttrSlot& slot : cf.attrs) {
                if (cf.utf8(slot.nameCp) != "BootstrapMethods") continue;
                auto decoded = cf.decodeAttr(const_cast<AttrSlot&>(slot));
                if (decoded.is_err()) return nullptr;
                return decoded.unwrap();
            }
            return nullptr;
        }();
    const auto* modeled =
        body != nullptr
            ? ca::core::tag_cast::dyn_cast<jbc::core::BootstrapMethodsAttr>(body)
            : nullptr;
    if (modeled == nullptr || !p.cp->valid(in.cp)) return BindingValue::unknown();
    const auto& indy = p.cp->at(in.cp);
    if (indy.tag != CpTag::InvokeDynamic || indy.a >= modeled->entries.size()) {
        return BindingValue::unknown();
    }
    const auto& entry = modeled->entries[indy.a];
    if (entry.args.empty()) return BindingValue::unknown();

    // bsm 方法句柄：owner 必须 StringConcatFactory，name 决定折叠口径。
    std::string_view bsmOwner;
    std::string_view bsmName;
    {
        if (!p.cp->valid(entry.methodRef)) return BindingValue::unknown();
        const auto& mh = p.cp->at(entry.methodRef);
        if (mh.tag != CpTag::MethodHandle || !p.cp->valid(mh.b)) return BindingValue::unknown();
        const auto& ref = p.cp->at(mh.b);
        if (ref.tag != CpTag::Methodref && ref.tag != CpTag::InterfaceMethodref) {
            return BindingValue::unknown();
        }
        mjbase::analysis::MemberRefSem member;
        if (!resolveMemberRef(*p.cp, mh.b, member)) return BindingValue::unknown();
        bsmOwner = member.owner;
        bsmName  = member.name;
    }
    if (bsmOwner != "java/lang/invoke/StringConcatFactory") return BindingValue::unknown();
    const bool withConstants = bsmName == "makeConcatWithConstants";
    if (!withConstants && bsmName != "makeConcat") return BindingValue::unknown();

    std::string_view recipe;
    if (withConstants) {
        const ca::u32 recipeCp = entry.args[0];
        if (!p.cp->valid(recipeCp)) return BindingValue::unknown();
        const auto& e = p.cp->at(recipeCp);
        if (e.tag != CpTag::String || !p.cp->isUtf8(e.a)) return BindingValue::unknown();
        recipe = p.cp->utf8Of(e.a);
    }

    // 动态实参：调用点描述符的参数槽（indy 无接收者）。
    jbc::ir::ClassSession::MemberSem sem;
    if (!session.memberOf(in, sem)) return BindingValue::unknown();
    const auto                argTypes = paramTypes(sem.desc);
    std::vector<BindingValue> values;
    values.reserve(argTypes.size());
    for (ca::usize i = 0; i < argTypes.size(); ++i) {
        const auto* v = p.frames->stackFromTop(id, static_cast<ca::u32>(argTypes.size() - 1 - i));
        values.push_back(v != nullptr ? *v : BindingValue::unknown());
    }
    return foldConcat(withConstants, recipe, values);
}

/// @brief 纯产值点后处理（值轮）：sink 产点（forName/loadClass → ClassObject、
/// 成员查找 → MemberObject）、SB toString、indy 拼接。全部无条件 upsert
/// （产不出 = unknown 覆盖，无方向性陈旧）。
bool recomputePureProductions(MethodPipeline& p, jbc::ir::ClassSession& session)
{
    bool        changed = false;
    const auto& order   = p.code->dom.order();
    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const jbc::ir::InsnId id = order[pos];
        const jbc::ir::Insn*  in = p.code->dom.peek(id);
        if (in == nullptr || !p.frames->reachable(id)) continue;
        const Op op = in->op;

        if (op == Op::INVOKEDYNAMIC) {
            upsertInjection(p.injected, id.slot, concatProduction(p, *in, id, session));
            changed = true;
            continue;
        }
        if (!isInvokeFamily(op)) continue;

        jbc::ir::ClassSession::MemberSem sem;
        if (!session.memberOf(*in, sem)) continue;
        const auto    argTypes = paramTypes(sem.desc);
        const ca::u32 argCount = static_cast<ca::u32>(argTypes.size());
        const bool    isStatic = op == Op::INVOKESTATIC;

        // 显式实参槽（0 = 第一个实参，不含接收者）。栈形 [recv?, a0..aN-1]：
        // a_i 的 fromTop = argCount-1-i（与 isStatic 无关——静态栈只是少了
        // 接收者一格，实参的相对序不变）。
        auto valueAt = [&](int slot) -> const BindingValue* {
            if (slot < 0) return nullptr;
            const ca::u32 fromTop = static_cast<ca::u32>(argCount - 1 - slot);
            return p.frames->stackFromTop(id, fromTop);
        };

        // SB toString：读共享 builder 当前内容（变异 pass 之后的值轮拿到终值）。
        if (isStringBuilderOwner(sem.owner) && sem.name == "toString" && op == Op::INVOKEVIRTUAL) {
            BindingValue result       = BindingValue::unknown();
            const auto*  receiver     = p.frames->stackFromTop(id, argCount);
            if (receiver != nullptr && receiver->kind == BindingValue::Kind::StringBuilder &&
                receiver->builder && receiver->builder->known) {
                if (receiver->builder->ambiguous) {
                    result.kind = BindingValue::Kind::AmbiguousString;
                }
                else {
                    result.kind = BindingValue::Kind::String;
                    result.text = receiver->builder->text;
                }
            }
            changed |= upsertInjection(p.injected, id.slot, result);
            continue;
        }

        // SB 变异族（append/insert/<init>）归变异 pass，纯轮跳过。
        if (isStringBuilderOwner(sem.owner)) continue;

        const ReflectionSinkEntry* sink = matchSink(sem.owner, sem.name, sem.desc);
        if (sink == nullptr) {
            changed |= upsertInjection(p.injected, id.slot, BindingValue::unknown());
            continue;
        }

        BindingValue result = BindingValue::unknown();

        // 类加载 sink：类名实参可判定时产出 ClassObject（供后续成员查找接收）。
        if (sink->classNameArg >= 0) {
            const BindingValue* argument = valueAt(sink->classNameArg);
            if (argument != nullptr && argument->kind == BindingValue::Kind::String) {
                result.kind = BindingValue::Kind::ClassObject;
                result.text = std::string(normalizeClassName(argument->text));
            }
            changed |= upsertInjection(p.injected, id.slot, result);
            continue;
        }

        // 成员查找：产出成员对象（供 Method.invoke/Field.get/set/newInstance 追踪）。
        if (sink->producesMemberObject) {
            auto state     = std::make_shared<MemberState>();
            state->isField = std::strstr(sink->name, "Field") != nullptr;
            const bool isConstructor = std::strstr(sink->name, "Constructor") != nullptr;
            state->name     = isConstructor ? "<init>" : std::string();

            const BindingValue* receiver =
                isStatic ? nullptr : p.frames->stackFromTop(id, argCount);
            if (receiver != nullptr && receiver->kind == BindingValue::Kind::ClassObject) {
                state->owner = std::string(normalizeClassName(receiver->text));
            }
            if (sink->memberNameArg >= 0) {
                const BindingValue* name = valueAt(sink->memberNameArg);
                if (name != nullptr && name->kind == BindingValue::Kind::String) {
                    state->name = name->text;
                }
            }
            if (sink->paramTypesArg >= 0) {
                const BindingValue* types = valueAt(sink->paramTypesArg);
                if (types != nullptr && types->kind == BindingValue::Kind::ClassArray &&
                    types->array != nullptr && types->array->sizeKnown &&
                    !types->array->ambiguous) {
                    bool        allKnown = true;
                    std::string desc     = "(";
                    for (const auto& element : types->array->elements) {
                        if (!element) {
                            allKnown = false;
                            break;
                        }
                        desc += toParameterDescriptor(*element);
                    }
                    if (allKnown) state->desc = desc + ")";
                }
            }
            result.kind   = BindingValue::Kind::MemberObject;
            result.member = std::move(state);
            changed |= upsertInjection(p.injected, id.slot, result);
            continue;
        }

        changed |= upsertInjection(p.injected, id.slot, result);
    }
    return changed;
}

/// @brief 变异 pass（值轮收敛后按程序序一次性施加；旧栈共享变异引用语义）。
///
/// 覆盖：NEW StringBuilder/StringBuffer（新对象登记）、SB append/insert/<init>
/// （就地改内容 + 产出同一对象）、ANEWARRAY（新数组登记）、AASTORE（就地写元素）。
/// 内容不变（含写入等值）不计变化——外层收敛判定。
bool applyMutations(MethodPipeline& p, jbc::ir::ClassSession& session)
{
    bool        changed = false;
    const auto& order   = p.code->dom.order();
    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const jbc::ir::InsnId id = order[pos];
        const jbc::ir::Insn*  in = p.code->dom.peek(id);
        if (in == nullptr || !p.frames->reachable(id)) continue;
        const Op op = in->op;

        if (op == Op::NEW) {
            const std::string_view cls = resolveClassName(*p.cp, in->cp);
            if (!isStringBuilderType(cls)) continue;
            // 共享身份重放：沿用上一轮注入的同一 builder——值轮经
            // NEW→ASTORE→ALOAD 传给 append/toString 接收者的正是它，引用语义
            // 链不能断（新建对象会让值轮永远看到空串，外层不收敛）；内容重置
            // 为新建空串，本 pass 程序序内的 append/<init> 随后重放写入。重置
            // 本身不计入变化：外层收敛只判重放写入是否等价。
            const auto prev                        = p.injected.find(id.slot);
            std::shared_ptr<StringBuilderState> state =
                (prev != p.injected.end() && prev->second.builder != nullptr)
                    ? prev->second.builder
                    : std::make_shared<StringBuilderState>();
            *state           = StringBuilderState{};
            BindingValue value = BindingValue::unknown();
            value.kind         = BindingValue::Kind::StringBuilder;
            value.builder      = state;
            upsertInjection(p.injected, id.slot, value);
            continue;
        }

        if (op == Op::ANEWARRAY) {
            const std::string_view elem = resolveClassName(*p.cp, in->cp);
            auto                   state = std::make_shared<ArrayState>();
            state->isClassArray         = elem == "java/lang/Class";
            const BindingValue* sizeVal = p.frames->stackFromTop(id, 0);
            if (sizeVal != nullptr && sizeVal->kind == BindingValue::Kind::Int &&
                sizeVal->intValue >= 0) {
                state->sizeKnown = true;
                state->size      = sizeVal->intValue;
                state->elements.assign(static_cast<std::size_t>(sizeVal->intValue),
                                       std::nullopt);
            }
            else {
                state->ambiguous = true;
            }
            BindingValue value = BindingValue::unknown();
            value.kind         = BindingValue::Kind::ClassArray;
            value.array        = std::move(state);
            changed |= upsertInjection(p.injected, id.slot, value);
            continue;
        }

        if (op == Op::AASTORE) {
            const BindingValue* array  = p.frames->stackFromTop(id, 2);
            const BindingValue* index  = p.frames->stackFromTop(id, 1);
            const BindingValue* writer = p.frames->stackFromTop(id, 0);
            if (array == nullptr || array->array == nullptr) continue;
            auto&        state = *array->array;
            BindingValue w     = writer != nullptr ? *writer : BindingValue::unknown();
            if (w.kind == BindingValue::Kind::ClassObject) {
                w.text = std::string(normalizeClassName(w.text));
            }
            const bool valueOk = state.isClassArray
                                     ? w.kind == BindingValue::Kind::ClassObject
                                     : w.kind == BindingValue::Kind::String;
            if (state.ambiguous || index == nullptr || index->kind != BindingValue::Kind::Int ||
                !valueOk) {
                if (!state.ambiguous) {
                    state.ambiguous = true;
                    changed         = true;
                }
            }
            else if (index->intValue < 0 ||
                     static_cast<std::size_t>(index->intValue) >= state.elements.size()) {
                if (!state.ambiguous) {
                    state.ambiguous = true;
                    changed         = true;
                }
            }
            else {
                auto& slotElem = state.elements[static_cast<std::size_t>(index->intValue)];
                if (!slotElem.has_value() || *slotElem != w.text) {
                    slotElem = w.text;
                    changed  = true;
                }
            }
            continue;
        }

        if (!isInvokeFamily(op)) continue;
        jbc::ir::ClassSession::MemberSem sem;
        if (!session.memberOf(*in, sem)) continue;
        if (!isStringBuilderOwner(sem.owner)) continue;

        const auto    argTypes = paramTypes(sem.desc);
        const ca::u32 argCount = static_cast<ca::u32>(argTypes.size());
        const bool    init     = sem.name == "<init>" && in->op == Op::INVOKESPECIAL;
        if (!init && sem.name != "append" && sem.name != "insert") continue;
        if (!init && in->op != Op::INVOKEVIRTUAL) continue;

        const BindingValue* receiver = p.frames->stackFromTop(id, argCount);
        if (receiver == nullptr || receiver->kind != BindingValue::Kind::StringBuilder ||
            receiver->builder == nullptr) {
            continue;
        }

        // 实参：append 取末实参；insert(index, x) 取第 2 实参（index 槽 0，
        // 缺省回退槽 0）；<init>(String) 取槽 0。<init>()V 不携带内容实参：
        // 新建 SB 本就是已知空串（旧栈 init 段 values.size()>=2 同口径），
        // 走 appended==nullptr 的 fail-closed 会误杀折叠链。
        if (init && argCount == 0) continue;
        const BindingValue* appended = nullptr;
        auto valueAt = [&](int slot) -> const BindingValue* {
            if (slot < 0) return nullptr;
            const ca::u32 fromTop = static_cast<ca::u32>(argCount - 1 - slot);
            return p.frames->stackFromTop(id, fromTop);
        };
        if (sem.name == "append") {
            appended = valueAt(0);
        }
        else if (sem.name == "insert") {
            appended = valueAt(1);
            if (appended == nullptr) appended = valueAt(0);
        }
        else if (argCount >= 1) {
            appended = valueAt(0);
        }

        StringBuilderState& state            = *receiver->builder;
        const std::string   textBefore       = state.text;
        const bool          knownBefore      = state.known;
        const bool          ambiguousBefore  = state.ambiguous;
        if (appended == nullptr) {
            state.known = false;
        }
        else if (state.known) {
            if (auto part = renderConcatArgument(*appended)) {
                if (part->ambiguous) {
                    state.ambiguous = true;
                }
                else {
                    state.text += part->text;
                }
            }
            else {
                state.known = false;
            }
        }
        if (state.text != textBefore || state.known != knownBefore ||
            state.ambiguous != ambiguousBefore) {
            changed = true;
        }

        // 产出：append/insert 返回 SB 本身（链式调用的接收者）；<init> 同对象。
        BindingValue result  = BindingValue::unknown();
        result.kind          = BindingValue::Kind::StringBuilder;
        result.builder       = receiver->builder;
        changed |= upsertInjection(p.injected, id.slot, result);
    }
    return changed;
}

}   // namespace

void ReflectionBindingAnalyzer::scanStaticNameTables(jbc::ir::ClassSession& session,
                                                     std::string_view       className)
{
    jbc::core::ClassFile&     cf   = session.backing();
    jbc::core::ConstantPool&  cp   = cf.cp;
    const jbc::ir::ClassUnit& unit = session.unit();

    const std::vector<jbc::ir::CodeVisit> visits = jbc::ir::collectCodeUnits(session);
    for (ca::usize mi = 0; mi < unit.methods.size() && mi < visits.size(); ++mi) {
        if (unit.symbols().str(unit.methods[mi].name) != "<clinit>") continue;
        if (visits[mi].kind != jbc::ir::CodeVisit::Kind::Ok) continue;
        const jbc::ir::CodeUnit& code  = *visits[mi].unit;
        const auto&              order = code.dom.order();

        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const jbc::ir::Insn* in = code.dom.peek(order[pos]);
            if (in == nullptr || in->op != Op::ANEWARRAY) continue;
            // ANEWARRAY 的 desc 是元素类型：String[] 建数组为 ANEWARRAY java/lang/String
            if (resolveClassName(cp, in->cp) != "java/lang/String") continue;
            if (pos == 0) continue;
            const jbc::ir::Insn* sizeInsn = code.dom.peek(order[pos - 1]);
            ca::i32              size     = -1;
            if (sizeInsn == nullptr || !intConstantOf(*sizeInsn, size, true) || size < 0) {
                continue;
            }

            // 直线填充序列：(DUP; <const>; LDC string; AASTORE)* 直到 PUTSTATIC
            std::vector<std::optional<std::string>> elements(static_cast<ca::usize>(size));
            bool                                    broken = false;
            ca::usize                               putPos = 0;
            bool                                    hasPut = false;
            ca::usize                               cursor = pos + 1;
            while (cursor < order.size()) {
                const jbc::ir::Insn* step = code.dom.peek(order[cursor]);
                if (step == nullptr) {
                    broken = true;
                    break;
                }
                if (step->op == Op::PUTSTATIC) {
                    putPos = cursor;
                    hasPut = true;
                    break;
                }
                if (step->op != Op::DUP || cursor + 3 >= order.size()) {
                    broken = true;
                    break;
                }
                const jbc::ir::Insn* indexInsn = code.dom.peek(order[cursor + 1]);
                ca::i32              index     = -1;
                if (indexInsn == nullptr || !intConstantOf(*indexInsn, index, true) || index < 0 ||
                    static_cast<ca::usize>(index) >= elements.size() ||
                    elements[static_cast<ca::usize>(index)].has_value()) {
                    broken = true;
                    break;
                }
                const jbc::ir::Insn*       valueInsn = code.dom.peek(order[cursor + 2]);
                std::optional<std::string> value;
                if (valueInsn != nullptr &&
                    (valueInsn->op == Op::LDC || valueInsn->op == Op::LDC_W)) {
                    ConstSem csem;
                    if (session.constOf(*valueInsn, csem) &&
                        csem.kind == ConstSem::Kind::String) {
                        value = std::string(csem.str);
                    }
                }
                if (!value) {
                    broken = true;
                    break;
                }
                const jbc::ir::Insn* storeInsn = code.dom.peek(order[cursor + 3]);
                if (storeInsn == nullptr || storeInsn->op != Op::AASTORE) {
                    broken = true;
                    break;
                }
                elements[static_cast<ca::usize>(index)] = std::move(*value);
                cursor += 4;
            }

            if (broken || !hasPut) continue;
            const jbc::ir::Insn* put = code.dom.peek(order[putPos]);
            jbc::ir::ClassSession::MemberSem sem;
            if (put == nullptr || !session.memberOf(*put, sem) || sem.owner != className ||
                sem.desc != "[Ljava/lang/String;") {
                continue;
            }

            bool full = true;
            for (const auto& element : elements) {
                if (!element) {
                    full = false;
                    break;
                }
            }
            if (!full) continue;
            if (regionHasIncomingBranches(code, pos - 1, putPos)) continue;

            std::vector<std::string> table;
            table.reserve(elements.size());
            for (const auto& element : elements) table.push_back(*element);
            const std::string key = std::string(className) + "." + std::string(sem.name);
            nameTables_[pool_.str(pool_.intern(key))] = std::move(table);
        }
    }
}

void ReflectionBindingAnalyzer::analyzeSession(jbc::ir::ClassSession& session)
{
    jbc::core::ClassFile&      cf   = session.backing();
    const jbc::ir::ClassUnit&  unit = session.unit();
    const jbc::ir::SymbolPool& syms = unit.symbols();
    const std::string_view     className = syms.str(unit.thisClass);

    const std::vector<jbc::ir::CodeVisit> visits = jbc::ir::collectCodeUnits(session);
    for (ca::usize mi = 0; mi < unit.methods.size() && mi < visits.size(); ++mi) {
        const jbc::ir::MethodUnit& method = unit.methods[mi];
        if (visits[mi].kind != jbc::ir::CodeVisit::Kind::Ok) continue;   // 抽象/原生/畸形
        const jbc::ir::CodeUnit& code  = *visits[mi].unit;
        const auto&              order = code.dom.order();

        MethodPipeline p;
        p.code                = &code;
        p.cp                  = &cf.cp;
        p.env.cp              = &cf.cp;
        p.env.stringConstants = &stringConstants_;
        p.env.nameTables      = &nameTables_;

        const bool             isStatic   = (method.access & mjbase::hierarchy::kAccStatic) != 0;
        const std::string_view methodDesc = syms.str(method.desc);

        // 外层：值轮收敛 → 变异 pass → 再值轮……共享内容不再变化即收口
        // （变异每轮重建共享对象，内容等价由注入 upsert 的值等价判定收敛）。
        for (ca::usize outer = 0; outer < 4; ++outer) {
            bool stable = false;
            for (ca::usize round = 0; round <= order.size() + 1; ++round) {
                if (!p.runEngine(isStatic, methodDesc)) {
                    stable = false;
                    break;
                }
                if (!recomputePureProductions(p, session)) {
                    stable = true;
                    break;
                }
            }
            if (!stable) break;   // fail-open（JSR/RET、畸形控制流、注入不收敛）
            if (!applyMutations(p, session)) break;
        }
        if (!p.frames) continue;   // 该方法控制流不可分析：不产出绑定（fail-closed）

        // ── sink 落解（旧栈 analyzeClass sink 段照迁，帧读取换 FrameQuery）──
        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const jbc::ir::InsnId id = order[pos];
            const jbc::ir::Insn*  in = code.dom.peek(id);
            if (in == nullptr || !p.frames->reachable(id) || !isInvokeFamily(in->op)) continue;
            jbc::ir::ClassSession::MemberSem sem;
            if (!session.memberOf(*in, sem)) continue;
            const ReflectionSinkEntry* sink = matchSink(sem.owner, sem.name, sem.desc);
            if (sink == nullptr) continue;

            const auto    argumentCount = static_cast<ca::i32>(paramTypes(sem.desc).size());
            const bool    isStatic      = in->op == Op::INVOKESTATIC;

            ReflectionBinding binding;
            binding.site.sourceClass  = pool_.str(pool_.intern(className));
            binding.site.sourceMethod = pool_.str(pool_.intern(syms.str(method.name)));
            binding.site.sourceDesc   = pool_.str(pool_.intern(methodDesc));
            binding.site.opcode       = static_cast<ca::i32>(in->op);
            binding.site.insnIndex    = static_cast<ca::i32>(pos);
            binding.api               = sink->kind;

            // 槽位以显式实参为基准（不含接收者）；sink 描述符全为引用实参，
            // 槽位数即参数类型数。
            auto argumentAt = [&](int slot) -> BindingValue {
                if (slot < 0 || argumentCount <= 0) return BindingValue::unknown();
                const ca::u32 fromTop = static_cast<ca::u32>(argumentCount - 1 - slot);
                const auto*   v       = p.frames->stackFromTop(id, fromTop);
                return v != nullptr ? *v : BindingValue::unknown();
            };
            auto isv = [&](std::string text) { return pool_.str(pool_.intern(text)); };

            // ── catalog-only：Lookup 族 ──
            if (sink->catalogOnly) {
                binding.state = ReflectionBindingState::Unsupported;
                bindings_.push_back(std::move(binding));
                continue;
            }

            // ── 类名 sink（forName/loadClass）──
            if (sink->kind == ReflectionApiKind::ClassForName ||
                sink->kind == ReflectionApiKind::ClassLoaderLoadClass) {
                const BindingValue argument = argumentAt(sink->classNameArg);
                if (argument.kind == BindingValue::Kind::AmbiguousString) {
                    binding.state     = ReflectionBindingState::Pending;
                    binding.reason    = ReflectionPendingReason::AmbiguousArray;
                    binding.className = isv(std::string(normalizeClassName(argument.text)));
                }
                else if (argument.kind != BindingValue::Kind::String) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownArgs;
                }
                else {
                    binding.className = isv(std::string(normalizeClassName(argument.text)));
                    if (classes_.find(binding.className) == classes_.end()) {
                        binding.state  = ReflectionBindingState::Pending;
                        binding.reason = ReflectionPendingReason::NoSuchClass;
                    }
                    else {
                        binding.state        = ReflectionBindingState::Resolved;
                        binding.target.owner = binding.className;
                    }
                }
                bindings_.push_back(std::move(binding));
                continue;
            }

            // ── 成员查找族 ──
            if (sink->kind == ReflectionApiKind::MemberLookup) {
                const ca::u32 receiverFromTop =
                    isStatic ? static_cast<ca::u32>(argumentCount - 1)
                             : static_cast<ca::u32>(argumentCount);
                const BindingValue* receiverSlot = p.frames->stackFromTop(id, receiverFromTop);
                const BindingValue receiver =
                    receiverSlot != nullptr ? *receiverSlot : BindingValue::unknown();
                if (receiver.kind == BindingValue::Kind::ClassObject) {
                    binding.className = isv(std::string(normalizeClassName(receiver.text)));
                }

                if (sink->memberEnumeration) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = binding.className.empty()
                                         ? ReflectionPendingReason::UnknownReceiver
                                         : ReflectionPendingReason::MemberEnumeration;
                    bindings_.push_back(std::move(binding));
                    continue;
                }

                if (sink->memberNameArg >= 0) {
                    const BindingValue nameArgument = argumentAt(sink->memberNameArg);
                    if (nameArgument.kind == BindingValue::Kind::String) {
                        binding.memberName = isv(nameArgument.text);
                    }
                    else if (nameArgument.kind == BindingValue::Kind::AmbiguousString) {
                        binding.state  = ReflectionBindingState::Pending;
                        binding.reason = ReflectionPendingReason::AmbiguousArray;
                        bindings_.push_back(std::move(binding));
                        continue;
                    }
                }

                // 参数类型两级口径：Class[] 元素全知 → 构建完整参数描述符（精确
                // 匹配用）；元素不全/含 Unknown 元素 → 仅个数（回退宽松匹配）。
                bool        typesVerified = true;
                std::string wantParams;
                if (sink->paramTypesArg >= 0) {
                    const BindingValue types = argumentAt(sink->paramTypesArg);
                    if (types.kind == BindingValue::Kind::ClassArray && types.array &&
                        types.array->sizeKnown && !types.array->ambiguous) {
                        binding.paramTypeCount = types.array->size;
                        bool        allKnown   = true;
                        std::string desc       = "(";
                        for (const auto& element : types.array->elements) {
                            if (!element) {
                                allKnown = false;
                                break;
                            }
                            desc += toParameterDescriptor(*element);
                        }
                        if (allKnown) wantParams = desc + ")";
                        typesVerified = true;
                    }
                    else {
                        typesVerified = false;
                    }
                }
                // 构造器查找无名字实参槽：目标成员名固定为 <init>。
                const bool isConstructorSink = std::strstr(sink->name, "Constructor") != nullptr;
                if (isConstructorSink && binding.memberName.empty()) {
                    binding.memberName = pool_.str(pool_.intern("<init>"));
                }

                if (binding.className.empty()) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownReceiver;
                    bindings_.push_back(std::move(binding));
                    continue;
                }
                if (binding.memberName.empty() || !typesVerified) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownArgs;
                    bindings_.push_back(std::move(binding));
                    continue;
                }

                // 存在性验证（fail-closed 核心）：类在容器内 + 成员命中声明 +
                // 参数类型可验证。Class[] 元素全知时按参数列表精确匹配（与执行族
                // 同口径，消除同名同参数个数重载的误报面）；元素不全时放宽为个数
                // 匹配（命中第一个名字+个数相符声明并记录其真实描述符）。
                auto classIt = classes_.find(binding.className);
                if (classIt == classes_.end()) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::NoSuchClass;
                    bindings_.push_back(std::move(binding));
                    continue;
                }
                bool matched = false;
                if (std::strstr(sink->name, "Field") != nullptr) {
                    // 字段无重载：名字命中即唯一。
                    for (const auto& [fieldName, fieldDesc] : classIt->second.fields) {
                        if (fieldName == binding.memberName) {
                            binding.target.owner = binding.className;
                            binding.target.name  = fieldName;
                            binding.target.desc  = fieldDesc;
                            matched              = true;
                            break;
                        }
                    }
                }
                else {
                    const bool isConstructor = isConstructorSink;
                    if (isConstructor) binding.memberName = pool_.str(pool_.intern("<init>"));
                    auto paramListOf = [](std::string_view desc) {
                        const auto close = desc.find(')');
                        return close == std::string_view::npos ? desc : desc.substr(0, close + 1);
                    };
                    for (const auto& [methodName, methodDesc] : classIt->second.methods) {
                        if (methodName != binding.memberName) continue;
                        if (paramTypes(methodDesc).size() !=
                            static_cast<ca::usize>(binding.paramTypeCount)) {
                            continue;
                        }
                        if (!wantParams.empty() && paramListOf(methodDesc) != wantParams) {
                            continue;
                        }
                        binding.target.owner = binding.className;
                        binding.target.name  = methodName;
                        binding.target.desc  = methodDesc;
                        matched              = true;
                        break;
                    }
                }
                if (!matched) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::NoSuchMember;
                }
                else {
                    binding.state = ReflectionBindingState::Resolved;
                }
                bindings_.push_back(std::move(binding));
                continue;
            }

            // ── 反射执行 / 字段访问族 ──
            const ca::u32 receiverFromTop = isStatic ? static_cast<ca::u32>(argumentCount - 1)
                                                     : static_cast<ca::u32>(argumentCount);
            const BindingValue* receiverSlot = p.frames->stackFromTop(id, receiverFromTop);
            const BindingValue receiver =
                receiverSlot != nullptr ? *receiverSlot : BindingValue::unknown();

            if (sink->kind == ReflectionApiKind::ClassNewInstance) {
                if (receiver.kind != BindingValue::Kind::ClassObject) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownReceiver;
                    bindings_.push_back(std::move(binding));
                    continue;
                }
                binding.className = isv(std::string(normalizeClassName(receiver.text)));
                auto classIt      = classes_.find(binding.className);
                if (classIt == classes_.end()) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::NoSuchClass;
                }
                else {
                    binding.state          = ReflectionBindingState::Resolved;
                    binding.target.owner   = binding.className;
                    binding.target.name    = pool_.str(pool_.intern("<init>"));
                    binding.target.desc    = pool_.str(pool_.intern("()V"));
                    binding.paramTypeCount = 0;
                }
                bindings_.push_back(std::move(binding));
                continue;
            }

            if (receiver.kind != BindingValue::Kind::MemberObject || !receiver.member) {
                binding.state  = ReflectionBindingState::Pending;
                binding.reason = ReflectionPendingReason::UnknownReceiver;
                bindings_.push_back(std::move(binding));
                continue;
            }

            binding.className  = isv(std::string(normalizeClassName(receiver.member->owner)));
            binding.memberName = isv(receiver.member->name);
            auto classIt       = classes_.find(binding.className);
            if (binding.className.empty() || classIt == classes_.end()) {
                binding.state  = ReflectionBindingState::Pending;
                binding.reason = ReflectionPendingReason::NoSuchClass;
                bindings_.push_back(std::move(binding));
                continue;
            }
            if (binding.memberName.empty()) {
                binding.state  = ReflectionBindingState::Pending;
                binding.reason = ReflectionPendingReason::UnknownArgs;
                bindings_.push_back(std::move(binding));
                continue;
            }

            bool matched = false;
            if (receiver.member->isField) {
                // 字段无重载：名字命中即唯一（desc 由声明回填）。
                for (const auto& [fieldName, fieldDesc] : classIt->second.fields) {
                    if (fieldName == binding.memberName) {
                        binding.target.owner = binding.className;
                        binding.target.name  = fieldName;
                        binding.target.desc  = fieldDesc;
                        matched              = true;
                        break;
                    }
                }
            }
            else {
                if (receiver.member->desc.empty()) {
                    // 方法/构造器参数列表不可判定：无法验证 → fail-closed。
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownArgs;
                    bindings_.push_back(std::move(binding));
                    continue;
                }
                // MemberState.desc 是参数列表形态 "(...)"（无返回类型），与声明
                // 描述符的参数段精确比对。
                const auto        wantClose = receiver.member->desc.find(')');
                const std::string wantParams =
                    wantClose == std::string::npos ? receiver.member->desc
                                                   : receiver.member->desc.substr(0, wantClose + 1);
                auto paramListOf = [](std::string_view desc) {
                    const auto close = desc.find(')');
                    return close == std::string_view::npos ? desc : desc.substr(0, close + 1);
                };
                for (const auto& [methodName, methodDesc] : classIt->second.methods) {
                    if (methodName == binding.memberName && paramListOf(methodDesc) == wantParams) {
                        binding.target.owner = binding.className;
                        binding.target.name  = methodName;
                        binding.target.desc  = methodDesc;
                        matched              = true;
                        break;
                    }
                }
            }
            if (!matched) {
                binding.state  = ReflectionBindingState::Pending;
                binding.reason = ReflectionPendingReason::NoSuchMember;
            }
            else {
                binding.state = ReflectionBindingState::Resolved;
                if (!receiver.member->isField && !binding.target.desc.empty()) {
                    binding.paramTypeCount =
                        static_cast<ca::i32>(paramTypes(binding.target.desc).size());
                }
            }
            bindings_.push_back(std::move(binding));
        }
    }
}

// ── 类级编排（旧栈 clear/summarize/bindingsOfClass/build/indexDeclarations 照迁；
//    文本一律 pool_ 驻留视图——键寿命与驻留池同在，内容寻址等值）──────────

void ReflectionBindingAnalyzer::clear()
{
    classes_.clear();
    stringConstants_.clear();
    nameTables_.clear();
    bindings_.clear();
}

ReflectionBindingSummary ReflectionBindingAnalyzer::summarize() const
{
    ReflectionBindingSummary summary;
    summary.byKind.assign(static_cast<std::size_t>(ReflectionApiKind::LookupMethodHandle) + 1, 0);
    for (const ReflectionBinding& binding : bindings_) {
        ++summary.total;
        switch (binding.state) {
        case ReflectionBindingState::Resolved: ++summary.resolved; break;
        case ReflectionBindingState::Pending: ++summary.pending; break;
        case ReflectionBindingState::Unsupported: ++summary.unsupported; break;
        }
        const std::size_t kindIndex = static_cast<std::size_t>(binding.api);
        if (kindIndex < summary.byKind.size()) ++summary.byKind[kindIndex];
    }
    return summary;
}

std::vector<const ReflectionBinding*> ReflectionBindingAnalyzer::bindingsOfClass(
    std::string_view sourceClass) const
{
    std::vector<const ReflectionBinding*> result;
    for (const ReflectionBinding& binding : bindings_) {
        if (binding.site.sourceClass == sourceClass) result.push_back(&binding);
    }
    return result;
}

void ReflectionBindingAnalyzer::indexSession(jbc::ir::ClassSession& session)
{
    jbc::core::ClassFile&      cf   = session.backing();
    jbc::core::ConstantPool&   cp   = cf.cp;
    const jbc::ir::ClassUnit&  unit = session.unit();
    const jbc::ir::SymbolPool& syms = unit.symbols();

    const std::string_view className = pool_.str(pool_.intern(syms.str(unit.thisClass)));

    // 声明底表（first-seen wins；旧栈 try_emplace 同口径）。
    auto [it, inserted] = classes_.try_emplace(className);
    if (inserted) {
        ClassInfo& info = it->second;
        info.methods.reserve(unit.methods.size());
        for (const jbc::ir::MethodUnit& method : unit.methods) {
            info.methods.emplace_back(pool_.str(pool_.intern(syms.str(method.name))),
                                      pool_.str(pool_.intern(syms.str(method.desc))));
        }
        const ca::u32 kStaticFinal = mjbase::hierarchy::kAccStatic | mjbase::hierarchy::kAccFinal;
        for (ca::usize fi = 0; fi < unit.fields.size() && fi < cf.fields.size(); ++fi) {
            const jbc::ir::FieldUnit&    field   = unit.fields[fi];
            const jbc::core::MemberInfo& backing = cf.fields[fi];
            const std::string_view fieldName = pool_.str(pool_.intern(syms.str(field.name)));
            const std::string_view fieldDesc = pool_.str(pool_.intern(syms.str(field.desc)));
            info.fields.emplace_back(fieldName, fieldDesc);

            // static final String 常量：ConstantValue → String CP → utf8。
            if ((field.access & kStaticFinal) != kStaticFinal) continue;
            if (fieldDesc != "Ljava/lang/String;") continue;
            for (const jbc::core::AttrSlot& slot : backing.attrs) {
                if (cp.utf8Of(slot.nameCp) != "ConstantValue") continue;
                auto decoded = cf.decodeAttr(const_cast<jbc::core::AttrSlot&>(slot));
                if (decoded.is_err()) break;
                const auto* raw =
                    ca::core::tag_cast::dyn_cast<jbc::core::RawAttr>(decoded.unwrap());
                if (raw == nullptr || raw->bytes().size() < 2) break;
                const ca::u16 valueCp =
                    static_cast<ca::u16>((raw->bytes()[0] << 8) | raw->bytes()[1]);
                if (cp.valid(valueCp)) {
                    const auto& entry = cp.at(valueCp);
                    if (entry.tag == CpTag::String && cp.isUtf8(entry.a)) {
                        const std::string key =
                            std::string(className) + "." + std::string(fieldName);
                        stringConstants_[pool_.str(pool_.intern(key))] =
                            std::string(cp.utf8Of(entry.a));
                    }
                }
                break;   // ConstantValue 每字段至多一个
            }
        }
    }
    scanStaticNameTables(session, className);
}

void ReflectionBindingAnalyzer::build(const mjbase::model::ResourceContainer& container)
{
    clear();

    // 两趟扫描（旧栈 build 同构）：先全量声明底表/常量/名表，再逐方法值流
    // 落解。畸形类在 scan 阶段已 fail-open 跳过，不产出会话。
    const auto scanned = mjbase::analysis::scanClassResources(container);
    for (const auto& scannedClass : scanned) {
        indexSession(*scannedClass.session);
    }
    for (const auto& scannedClass : scanned) {
        analyzeSession(*scannedClass.session);
    }
}

}   // namespace mjbase::reference
