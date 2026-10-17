#include "mjbase/reference/reflection_interpreter.hpp"

#include <string>
#include <vector>

#include "jbc/analysis/frame.hpp"

#include "mjbase/analysis/alias_analysis.hpp"
#include "mjbase/analysis/cp_refs.hpp"
#include "mjbase/asm/analyzer/frame_query.hpp"

namespace mjbase::reference {

using jbc::core::Op;
using jbc::core::Shape;
using jbc::analysis::VKind;
using mjbase::analysis::AliasAnalysis;
using mjbase::analysis::AliasValue;
using mjbase::analysis::MemberRefSem;
using mjbase::analysis::aliasInterned;
using mjbase::analysis::mergeAliasValues;
using mjbase::analysis::resolveClassName;
using mjbase::analysis::resolveMemberRef;

namespace {

bool isInvokeKind(Op op)
{
    return op == Op::INVOKEVIRTUAL || op == Op::INVOKESPECIAL || op == Op::INVOKESTATIC ||
           op == Op::INVOKEINTERFACE;
}

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

bool isPrimitiveClassKeyword(std::string_view name)
{
    return name == "int" || name == "long" || name == "double" || name == "float" ||
           name == "boolean" || name == "char" || name == "byte" || name == "short" ||
           name == "void";
}

/// CP Class 名 → 类常量追踪名（口径见头注 classLiteralTrackingName）；不成事实返回空。
std::string_view classLiteralName(std::string_view cpClassName)
{
    if (cpClassName.empty()) return {};
    if (cpClassName.front() == '[') return cpClassName;   // 数组：描述符原样
    if (cpClassName.size() < 2 || isPrimitiveClassKeyword(cpClassName)) return {};
    return normalizeClassName(cpClassName);
}

/// LDC class 常量 / GETSTATIC TYPE → 参数描述符形态（"Lx;" / "[..." / "I"）。
std::string readReflectionClassArgumentDescriptor(const jbc::ir::Insn&            insn,
                                                  const jbc::core::ConstantPool& cp)
{
    if (insn.op == Op::LDC || insn.op == Op::LDC_W) {
        const std::string_view name = classLiteralName(resolveClassName(cp, insn.cp));
        if (name.empty()) return {};
        return classLiteralDescriptor(name);
    }
    MemberRefSem ref;
    if (insn.op == Op::GETSTATIC && resolveMemberRef(cp, insn.cp, ref)) {
        const std::string_view primitive =
            primitiveDescriptorFromTypeField(ref.owner, ref.name, ref.desc);
        if (!primitive.empty()) return std::string(primitive);
    }
    return {};
}

/// "(Lx;[J)" 形态拼装：长度非负、下标全覆盖且不重复（旧栈 buildReflectionArgumentDescriptor）。
std::string buildReflectionArgumentDescriptor(
    ca::i32 arraySize, std::vector<std::pair<ca::i32, std::string>> entries)
{
    if (arraySize < 0) return {};

    std::vector<std::string> args(static_cast<ca::usize>(arraySize));
    std::vector<bool>        seen(static_cast<ca::usize>(arraySize), false);
    for (auto& [index, desc] : entries) {
        if (index < 0 || index >= arraySize || seen[static_cast<ca::usize>(index)]) return {};
        args[static_cast<ca::usize>(index)] = std::move(desc);
        seen[static_cast<ca::usize>(index)] = true;
    }
    for (bool itemSeen : seen) {
        if (!itemSeen) return {};
    }

    std::string descriptor = "(";
    for (const auto& arg : args) descriptor += arg;
    descriptor += ")";
    return descriptor;
}

/// 方法内全部跳转/switch 的目标标签（Branch 的 target + switch 的 targets）。
template <typename Visit>
void forEachBranchTarget(const jbc::ir::CodeUnit& code, Visit&& visit)
{
    for (const jbc::ir::InsnId id : code.dom.order()) {
        const jbc::ir::Insn* in = code.dom.peek(id);
        if (in == nullptr) continue;
        const Shape sh = shapeOf(in->op);
        if (sh == Shape::Branch2 || sh == Shape::Branch4) {
            visit(in->target);
        }
        else if (sh == Shape::TableSwitch || sh == Shape::LookupSwitch) {
            for (const jbc::ir::LabelId label : in->targets) visit(label);
        }
    }
}

}   // namespace

bool regionHasIncomingBranches(const jbc::ir::CodeUnit& code, ca::usize firstPos, ca::usize lastPos)
{
    std::unordered_map<ca::u32, ca::usize> posOf;
    const auto&                            order = code.dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) posOf.emplace(order[i].slot, i);

    bool hit = false;
    forEachBranchTarget(code, [&](jbc::ir::LabelId label) {
        if (hit || !code.dom.validLabel(label)) return;
        const auto target = code.dom.resolveLabel(label);
        if (target.is_err()) return;
        const jbc::ir::LabelTarget t = target.unwrap();
        if (t.isEnd()) return;
        const auto it = posOf.find(t.insn.slot);
        if (it == posOf.end()) return;
        if (it->second > firstPos && it->second <= lastPos) hit = true;
    });
    return hit;
}

namespace {

/// ALOAD 中转的局部 Class[] 形态解析（旧栈 localReflectionArgumentDescriptor）。
std::string localReflectionArgumentDescriptor(const jbc::ir::CodeUnit&       code,
                                              const jbc::core::ConstantPool& cp,
                                              ca::usize                      aloadPos)
{
    const auto&          order = code.dom.order();
    const jbc::ir::Insn* aload = code.dom.peek(order[aloadPos]);
    if (aload == nullptr || aload->op != Op::ALOAD) return {};
    const ca::i32 arrayLocal = aload->a;

    // 反向找同变量的 ASTORE（同变量 ALOAD 跳过；其他同变量 load/store 拒绝）。
    ca::usize arrayStorePos = 0;
    bool      found          = false;
    for (ca::usize cursor = aloadPos; cursor-- > 0;) {
        const jbc::ir::Insn* in = code.dom.peek(order[cursor]);
        if (in == nullptr) continue;
        const bool loadFamily = (in->op >= Op::ILOAD_0 && in->op <= Op::ALOAD_3) ||
                                in->op == Op::ILOAD || in->op == Op::LLOAD ||
                                in->op == Op::FLOAD || in->op == Op::DLOAD ||
                                in->op == Op::ALOAD;
        const bool storeFamily = (in->op >= Op::ISTORE_0 && in->op <= Op::ASTORE_3) ||
                                 in->op == Op::ISTORE || in->op == Op::LSTORE ||
                                 in->op == Op::FSTORE || in->op == Op::DSTORE ||
                                 in->op == Op::ASTORE;
        if (!loadFamily && !storeFamily) continue;   // 非 var 指令跳过（旧栈同口径）
        if (in->a != arrayLocal) continue;
        if (in->op == Op::ASTORE) {
            arrayStorePos = cursor;
            found         = true;
            break;
        }
        if (in->op != Op::ALOAD) return {};
    }
    if (!found) return {};

    if (arrayStorePos == 0) return {};
    const jbc::ir::Insn* arrayInsn = code.dom.peek(order[arrayStorePos - 1]);
    if (arrayInsn == nullptr || arrayInsn->op != Op::ANEWARRAY) return {};
    if (resolveClassName(cp, arrayInsn->cp) != "java/lang/Class") return {};

    const ca::usize arrayInsnPos = arrayStorePos - 1;
    if (arrayInsnPos == 0) return {};
    const jbc::ir::Insn* sizeInsn = code.dom.peek(order[arrayInsnPos - 1]);
    ca::i32              arraySize = -1;
    if (sizeInsn == nullptr || !intConstantOf(*sizeInsn, arraySize, false) || arraySize < 0 ||
        regionHasIncomingBranches(code, arrayInsnPos - 1, aloadPos)) {
        return {};
    }

    // 直线填充序列：(ALOAD; 常量下标; 类常量/TYPE; AASTORE)* 直到 arrayLoad。
    std::vector<std::pair<ca::i32, std::string>> entries;
    ca::usize                                    cursor = arrayStorePos + 1;
    while (cursor < aloadPos) {
        const jbc::ir::Insn* loadInsn = code.dom.peek(order[cursor]);
        if (loadInsn == nullptr || loadInsn->op != Op::ALOAD || loadInsn->a != arrayLocal) {
            return {};
        }
        ++cursor;
        if (cursor >= aloadPos) return {};
        const jbc::ir::Insn* indexInsn = code.dom.peek(order[cursor]);
        ca::i32              index     = -1;
        if (indexInsn == nullptr || !intConstantOf(*indexInsn, index, false)) return {};

        ++cursor;
        if (cursor >= aloadPos) return {};
        const jbc::ir::Insn* valueInsn = code.dom.peek(order[cursor]);
        if (valueInsn == nullptr) return {};
        std::string argDesc = readReflectionClassArgumentDescriptor(*valueInsn, cp);
        if (argDesc.empty()) return {};

        ++cursor;
        if (cursor >= aloadPos) return {};
        const jbc::ir::Insn* storeInsn = code.dom.peek(order[cursor]);
        if (storeInsn == nullptr || storeInsn->op != Op::AASTORE) return {};
        entries.push_back({index, std::move(argDesc)});
        ++cursor;
    }
    if (cursor != aloadPos) return {};
    return buildReflectionArgumentDescriptor(arraySize, std::move(entries));
}

// ── 别名格互转（旧栈 reflectionAlias / reflectionValueFromAlias 照迁）──

AliasValue reflectionAlias(const ReflectionValue& value)
{
    switch (value.kind) {
    case ReflectionValue::Kind::String:
        return AliasValue::primitive("string:" + std::string(value.name));
    case ReflectionValue::Kind::Class:
        return AliasValue::object("class:" + std::string(value.owner));
    case ReflectionValue::Kind::Method:
        return AliasValue::object("method:" + std::string(value.owner) + "." +
                                  std::string(value.name) + std::string(value.desc) + ":" +
                                  std::string(value.reflectionArgDesc));
    case ReflectionValue::Kind::Constructor:
        return AliasValue::object("constructor:" + std::string(value.owner) +
                                  std::string(value.desc) + ":" +
                                  std::string(value.reflectionArgDesc));
    case ReflectionValue::Kind::Field:
        return AliasValue::object("field:" + std::string(value.owner) + "." +
                                  std::string(value.name) + ":" + std::string(value.desc));
    case ReflectionValue::Kind::Unknown: break;
    }
    return AliasValue::unknown();
}

ReflectionValue reflectionValueFromAlias(const AliasValue& alias)
{
    ReflectionValue value;
    if (!alias.isKnown()) return value;
    if (alias.identity.substr(0, 7) == "string:") {
        value.kind = ReflectionValue::Kind::String;
        value.name = aliasInterned(alias.identity.substr(7));
    }
    else if (alias.identity.substr(0, 6) == "class:") {
        value.kind  = ReflectionValue::Kind::Class;
        value.owner = normalizeClassName(alias.identity.substr(6));
    }
    return value;
}

/// "field:owner.name:desc" 身份 → 字段槽查询（旧栈 resolveFieldAlias 照迁）。
const AliasValue* resolveFieldAlias(const AliasAnalysis& aliases, const AliasValue& value)
{
    constexpr std::string_view prefix = "field:";
    if (value.identity.substr(0, prefix.size()) != prefix) return nullptr;
    const std::string_view body  = value.identity.substr(prefix.size());
    const auto             split = body.find('.');
    const auto             desc  = body.rfind(':');
    if (split == std::string_view::npos || desc == std::string_view::npos || desc <= split + 1) {
        return nullptr;
    }
    return aliases.fieldValueOf(body.substr(0, split),
                                body.substr(split + 1, desc - split - 1), body.substr(desc + 1));
}

bool isMethodCall(std::string_view owner, std::string_view name, std::string_view desc,
                  const char* expectOwner, const char* expectName, const char* expectDesc)
{
    return owner == expectOwner && name == expectName && desc == expectDesc;
}

bool isReflectionFieldExecution(std::string_view name, std::string_view desc)
{
    if (name == "get") return desc == "(Ljava/lang/Object;)Ljava/lang/Object;";
    if (name == "set") return desc == "(Ljava/lang/Object;Ljava/lang/Object;)V";

    struct PrimitiveAccessor
    {
        const char* suffix;
        const char* desc;
    };
    static const PrimitiveAccessor kAccessors[] = {
        {"Boolean", "Z"}, {"Byte", "B"},  {"Char", "C"},  {"Short", "S"},
        {"Int", "I"},     {"Long", "J"},  {"Float", "F"}, {"Double", "D"},
    };
    for (const auto& accessor : kAccessors) {
        if (name == std::string("get") + accessor.suffix) {
            return desc == std::string("(Ljava/lang/Object;)") + accessor.desc;
        }
        if (name == std::string("set") + accessor.suffix) {
            return desc == std::string("(Ljava/lang/Object;") + accessor.desc + ")V";
        }
    }
    return false;
}

// ── 值域（IValueDomain<ReflectionValue>）：常量闭包 + CFG join 合并 ──

/// 域环境：池/前轮摘要/方法身份/注入表（invoke 产点，迭代探测-注入）。
struct ReflectionDomainEnv
{
    const jbc::core::ConstantPool*                      cp       = nullptr;
    const jbc::ir::CodeUnit*                            code     = nullptr;
    const AliasAnalysis*                                aliases  = nullptr;
    std::string_view                                    owner;
    std::string_view                                    name;
    std::string_view                                    desc;
    bool                                                isStatic = false;
    const std::unordered_map<ca::u32, ReflectionValue>* injected = nullptr;
};

class ReflectionDomain final : public jbc::analysis::IValueDomain<ReflectionValue>
{
public:
    explicit ReflectionDomain(const ReflectionDomainEnv& env) noexcept
        : env_(env)
    {}

    ReflectionValue top() override { return ReflectionValue{}; }

    /// join：相等反射值存活；分歧（含 Unknown）→ Unknown（fail-closed）。
    ReflectionValue merge(const ReflectionValue& a, const ReflectionValue& b) override
    {
        const AliasValue merged = mergeAliasValues(reflectionAlias(a), reflectionAlias(b));
        if (merged.isKnown()) return a;
        return ReflectionValue{};
    }

    /// 一切产值点：常量/ldc/GETSTATIC 别名回填；invoke 结果查注入表。
    ReflectionValue newValue(const jbc::ir::Insn& in, jbc::ir::InsnId at, VKind shape) override
    {
        (void)shape;
        if (env_.injected != nullptr) {
            const auto injected = env_.injected->find(at.slot);
            if (injected != env_.injected->end()) return injected->second;
        }
        if (in.op == Op::LDC || in.op == Op::LDC_W) {
            ReflectionValue value;
            if (env_.cp->valid(in.cp)) {
                const auto& e = env_.cp->at(in.cp);
                if (e.tag == jbc::core::CpTag::String && env_.cp->isUtf8(e.a)) {
                    value.kind = ReflectionValue::Kind::String;
                    value.name = aliasInterned(env_.cp->utf8Of(e.a));
                    return value;
                }
                if (e.tag == jbc::core::CpTag::Class && env_.cp->isUtf8(e.a)) {
                    const std::string_view name = classLiteralName(env_.cp->utf8Of(e.a));
                    if (!name.empty()) {
                        value.kind  = ReflectionValue::Kind::Class;
                        value.owner = name;
                        return value;
                    }
                }
            }
            return value;
        }
        if (in.op == Op::GETSTATIC) {
            MemberRefSem ref;
            if (!resolveMemberRef(*env_.cp, in.cp, ref)) return ReflectionValue{};
            if (env_.aliases != nullptr) {
                if (const auto* alias = env_.aliases->fieldValueOf(ref.owner, ref.name, ref.desc)) {
                    return reflectionValueFromAlias(*alias);
                }
            }
            return ReflectionValue{};
        }
        // CHECKCAST/ANEWARRAY/NEW/load 族 fallback：旧解释器一元/二元路径产
        // unknown（反射值被截断）；引擎把它们统一路由到 newValue，语义一致。
        return ReflectionValue{};
    }

    ReflectionValue newParameter(ca::u32 localIdx, VKind shape) override
    {
        (void)localIdx;
        (void)shape;
        return ReflectionValue{};   // 旧栈 newValue(type) → unknown
    }

    ReflectionValue newException(const jbc::ir::CodeUnit::Exc&) override
    {
        return ReflectionValue{};
    }

    ca::u8 width(const ReflectionValue&) override { return 1; }

    bool equal(const ReflectionValue& a, const ReflectionValue& b) override { return a == b; }

private:
    ReflectionDomainEnv env_;
};

/// invoke 产点迁移函数（旧栈 naryOperation 语义照迁；别名传播 + 查找族产点）。
ReflectionValue transferInvoke(const ReflectionDomainEnv&                               env,
                               const mjbase::asm_analyzer::FrameQuery<ReflectionValue>& frames,
                               const jbc::ir::Insn& in, jbc::ir::InsnId id, ca::usize pos,
                               const MemberRefSem& ref)
{
    ReflectionValue result;
    const auto      argTypes = paramTypes(ref.desc);
    const bool      isStatic = in.op == Op::INVOKESTATIC;
    const ca::u32   required = static_cast<ca::u32>(argTypes.size()) + (isStatic ? 0u : 1u);
    if (required > 0 && frames.stackFromTop(id, required - 1) == nullptr) {
        return result;   // 栈深不足（可达帧由引擎保证栈形，此为防御性短路）
    }

    // values 布局：[receiver?] + 显式实参（旧栈一致）。
    std::vector<ReflectionValue> values;
    values.reserve(required);
    if (!isStatic) {
        const auto* receiver = frames.stackFromTop(id, static_cast<ca::u32>(argTypes.size()));
        values.push_back(receiver != nullptr ? *receiver : ReflectionValue{});
    }
    for (ca::usize i = 0; i < argTypes.size(); ++i) {
        const auto* arg = frames.stackFromTop(id, static_cast<ca::u32>(argTypes.size() - 1 - i));
        values.push_back(arg != nullptr ? *arg : ReflectionValue{});
    }

    // alias 返回传播：param:/this:/field:/class:/string: 身份（旧栈同序）。
    if (env.aliases != nullptr) {
        const auto* summary = env.aliases->summaryOf(ref.owner, ref.name, ref.desc);
        if (summary != nullptr) {
            const std::string_view identity = summary->returnValue.identity;
            const bool             known    = summary->returnValue.isKnown();
            if (known && identity.substr(0, 6) == "param:") {
                ca::i32 parameter = -1;
                if (!identity.substr(6).empty()) {
                    parameter = 0;
                    for (const char c : identity.substr(6)) {
                        if (c < '0' || c > '9') {
                            parameter = -1;
                            break;
                        }
                        parameter = parameter * 10 + (c - '0');
                    }
                }
                if (parameter >= 0) {
                    const auto index = static_cast<ca::usize>(parameter) + (isStatic ? 0U : 1U);
                    if (index < values.size() &&
                        values[index].kind != ReflectionValue::Kind::Unknown) {
                        return values[index];
                    }
                }
            }
            if (known && identity.substr(0, 5) == "this:" && !values.empty() &&
                values[0].kind != ReflectionValue::Kind::Unknown) {
                return values[0];
            }
            AliasValue alias = summary->returnValue;
            if (const auto* field = resolveFieldAlias(*env.aliases, alias)) alias = *field;
            const ReflectionValue reflected = reflectionValueFromAlias(alias);
            if (reflected.kind != ReflectionValue::Kind::Unknown) return reflected;
        }
    }

    // Class.forName(String) → Class。
    if (isMethodCall(ref.owner, ref.name, ref.desc, "java/lang/Class", "forName",
                     "(Ljava/lang/String;)Ljava/lang/Class;") &&
        !values.empty() && values[0].kind == ReflectionValue::Kind::String) {
        result.kind  = ReflectionValue::Kind::Class;
        result.owner = normalizeClassName(values[0].name);
        return result;
    }

    if (values.empty()) return result;
    const ReflectionValue& receiver = values[0];

    const bool methodLookup =
        isMethodCall(ref.owner, ref.name, ref.desc, "java/lang/Class", "getMethod",
                     "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;") ||
        isMethodCall(ref.owner, ref.name, ref.desc, "java/lang/Class", "getDeclaredMethod",
                     "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
    if (receiver.kind == ReflectionValue::Kind::Class && values.size() >= 2 &&
        values[1].kind == ReflectionValue::Kind::String && methodLookup) {
        result.kind              = ReflectionValue::Kind::Method;
        result.owner             = receiver.owner;
        result.name              = values[1].name;
        result.reflectionArgDesc = aliasInterned(reflectionArgumentDescriptor(
            *env.code, *env.cp, pos));
        return result;
    }

    const bool constructorLookup =
        isMethodCall(ref.owner, ref.name, ref.desc, "java/lang/Class", "getConstructor",
                     "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;") ||
        isMethodCall(ref.owner, ref.name, ref.desc, "java/lang/Class", "getDeclaredConstructor",
                     "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;");
    if (receiver.kind == ReflectionValue::Kind::Class && constructorLookup) {
        result.kind              = ReflectionValue::Kind::Constructor;
        result.owner             = receiver.owner;
        result.name              = aliasInterned("<init>");
        result.reflectionArgDesc = aliasInterned(reflectionArgumentDescriptor(
            *env.code, *env.cp, pos));
        return result;
    }

    const bool fieldLookup =
        isMethodCall(ref.owner, ref.name, ref.desc, "java/lang/Class", "getField",
                     "(Ljava/lang/String;)Ljava/lang/reflect/Field;") ||
        isMethodCall(ref.owner, ref.name, ref.desc, "java/lang/Class", "getDeclaredField",
                     "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
    if (receiver.kind == ReflectionValue::Kind::Class && values.size() >= 2 &&
        values[1].kind == ReflectionValue::Kind::String && fieldLookup) {
        result.kind  = ReflectionValue::Kind::Field;
        result.owner = receiver.owner;
        result.name  = values[1].name;
        return result;
    }

    return result;
}

}   // namespace

// ── 导出纯函数 ──────────────────────────────────────────────

std::string_view normalizeClassName(std::string_view value)
{
    std::string out;
    if (value.size() >= 2 && value.front() == 'L' && value.back() == ';') {
        out.assign(value.substr(1, value.size() - 2));
    }
    else {
        out.assign(value);
    }
    bool changed = out != value;
    for (auto& c : out) {
        if (c == '.') {
            c       = '/';
            changed = true;
        }
    }
    return changed ? aliasInterned(out) : value;
}

std::string_view classLiteralTrackingName(std::string_view cpClassName)
{
    return classLiteralName(cpClassName);
}

std::string classLiteralDescriptor(std::string_view trackingName)
{
    if (!trackingName.empty() && trackingName.front() == '[') return std::string(trackingName);
    if (trackingName.empty()) return {};
    return "L" + std::string(trackingName) + ";";
}

bool intConstantOf(const jbc::ir::Insn& insn, ca::i32& out, bool allowNegative)
{
    const Op op = insn.op;
    if (op >= Op::ICONST_0 && op <= Op::ICONST_5) {
        out = static_cast<ca::i32>(op) - static_cast<ca::i32>(Op::ICONST_0);
        return true;
    }
    if (op == Op::ICONST_M1) {
        if (!allowNegative) return false;
        out = -1;
        return true;
    }
    if (op == Op::BIPUSH || op == Op::SIPUSH) {
        out = insn.a;
        return allowNegative || out >= 0;
    }
    return false;
}

std::string_view primitiveDescriptorFromTypeField(std::string_view owner, std::string_view name,
                                                  std::string_view desc)
{
    if (name != "TYPE" || desc != "Ljava/lang/Class;") return {};
    if (owner == "java/lang/Boolean") return "Z";
    if (owner == "java/lang/Byte") return "B";
    if (owner == "java/lang/Character") return "C";
    if (owner == "java/lang/Short") return "S";
    if (owner == "java/lang/Integer") return "I";
    if (owner == "java/lang/Long") return "J";
    if (owner == "java/lang/Float") return "F";
    if (owner == "java/lang/Double") return "D";
    return {};
}

std::string_view reflectionArgumentDescriptor(const jbc::ir::CodeUnit&       code,
                                              const jbc::core::ConstantPool& cp,
                                              ca::usize                      lookupPos)
{
    const auto& order = code.dom.order();
    if (lookupPos == 0 || lookupPos >= order.size()) return {};
    const jbc::ir::Insn* first = code.dom.peek(order[lookupPos - 1]);
    if (first == nullptr) return {};
    if (first->op == Op::ALOAD) {
        return aliasInterned(localReflectionArgumentDescriptor(code, cp, lookupPos - 1));
    }

    // 内联 DUP 形态：(ANEWARRAY Class; 常量长度; (DUP; 常量下标; 类常量/TYPE;
    // AASTORE)*) 紧邻 lookup。
    std::vector<std::pair<ca::i32, std::string>> entries;
    ca::usize                                    cursor = lookupPos - 1;
    while (cursor < order.size()) {
        const jbc::ir::Insn* in = code.dom.peek(order[cursor]);
        if (in == nullptr || in->op != Op::AASTORE) break;
        if (cursor < 2) return {};
        const jbc::ir::Insn* valueInsn = code.dom.peek(order[cursor - 1]);
        if (valueInsn == nullptr) return {};
        std::string argDesc = readReflectionClassArgumentDescriptor(*valueInsn, cp);
        if (argDesc.empty()) return {};

        const jbc::ir::Insn* indexInsn = code.dom.peek(order[cursor - 2]);
        ca::i32              index     = -1;
        if (indexInsn == nullptr || !intConstantOf(*indexInsn, index, false)) return {};

        if (cursor < 3) return {};
        const jbc::ir::Insn* dupInsn = code.dom.peek(order[cursor - 3]);
        if (dupInsn == nullptr || dupInsn->op != Op::DUP) return {};

        entries.push_back({index, std::move(argDesc)});
        if (cursor < 4) return {};
        cursor -= 4;
    }
    if (cursor >= order.size()) return {};
    const jbc::ir::Insn* arrayInsn = code.dom.peek(order[cursor]);
    if (arrayInsn == nullptr || arrayInsn->op != Op::ANEWARRAY) return {};
    if (resolveClassName(cp, arrayInsn->cp) != "java/lang/Class") return {};
    if (cursor == 0) return {};
    const jbc::ir::Insn* sizeInsn = code.dom.peek(order[cursor - 1]);
    ca::i32              arraySize = -1;
    if (sizeInsn == nullptr || !intConstantOf(*sizeInsn, arraySize, false) || arraySize < 0) {
        return {};
    }
    return aliasInterned(buildReflectionArgumentDescriptor(arraySize, std::move(entries)));
}

bool resolveReflectionExecution(jbc::core::Op op, std::string_view owner, std::string_view name,
                                std::string_view desc, const ReflectionValue& receiver,
                                ReflectionExecutionTarget& target)
{
    (void)op;
    if (isMethodCall(owner, name, desc, "java/lang/reflect/Method", "invoke",
                     "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;") &&
        receiver.kind == ReflectionValue::Kind::Method) {
        target.kind              = ReflectionExecutionTarget::Kind::Method;
        target.owner             = receiver.owner;
        target.name              = receiver.name;
        target.desc              = receiver.desc;
        target.reflectionArgDesc = receiver.reflectionArgDesc;
        return true;
    }
    if (isMethodCall(owner, name, desc, "java/lang/Class", "newInstance",
                     "()Ljava/lang/Object;") &&
        receiver.kind == ReflectionValue::Kind::Class) {
        target.kind              = ReflectionExecutionTarget::Kind::Constructor;
        target.owner             = receiver.owner;
        target.name              = aliasInterned("<init>");
        target.desc              = aliasInterned("()V");
        target.reflectionArgDesc = aliasInterned("()");
        return true;
    }
    if (isMethodCall(owner, name, desc, "java/lang/reflect/Constructor", "newInstance",
                     "([Ljava/lang/Object;)Ljava/lang/Object;") &&
        receiver.kind == ReflectionValue::Kind::Constructor) {
        target.kind              = ReflectionExecutionTarget::Kind::Constructor;
        target.owner             = receiver.owner;
        target.name              = aliasInterned("<init>");
        target.desc              = receiver.desc;
        target.reflectionArgDesc = receiver.reflectionArgDesc;
        return true;
    }
    if (owner == "java/lang/reflect/Field" && isReflectionFieldExecution(name, desc) &&
        receiver.kind == ReflectionValue::Kind::Field) {
        target.kind  = ReflectionExecutionTarget::Kind::Field;
        target.owner = receiver.owner;
        target.name  = receiver.name;
        target.desc  = receiver.desc;
        return true;
    }
    return false;
}

ReflectionSnapshot snapshotReflectionFacts(const ReflectionQueryContext& ctx)
{
    ReflectionSnapshot snap;
    if (ctx.code == nullptr || ctx.cp == nullptr) return snap;
    const jbc::ir::CodeUnit& code  = *ctx.code;
    const auto&              order = code.dom.order();
    if (order.empty()) {
        snap.ok = true;
        return snap;
    }

    ReflectionDomainEnv env;
    env.cp       = ctx.cp;
    env.code     = ctx.code;
    env.aliases  = ctx.aliases;
    env.owner    = ctx.owner;
    env.name     = ctx.name;
    env.desc     = ctx.desc;
    env.isStatic = ctx.isStatic;

    std::unordered_map<ca::u32, ReflectionValue> injected;
    const jbc::analysis::MethodSig               sig{ctx.isStatic, ctx.desc};

    const ca::usize roundBound = order.size() + 1;
    for (ca::usize round = 0; round < roundBound; ++round) {
        env.injected = &injected;
        // 域构造时按值拷贝环境：每轮以挂上注入表的环境重建域（BindingDomain
        // 的 runEngine 同款纪律），否则注入表永远到不了域。
        ReflectionDomain                             dom(env);
        jbc::analysis::DomainEngine<ReflectionValue> engine(*ctx.cp, dom);
        jbc::analysis::DomainResult<ReflectionValue> result = engine.analyze(code, sig);
        if (!result.summary.ok || result.summary.hasSubroutine) {
            return ReflectionSnapshot{};   // fail-open：引擎拒斥（旧栈空快照同口径）
        }
        mjbase::asm_analyzer::FrameQuery<ReflectionValue> frames(code, std::move(result));

        // 本轮 invoke 产点重算（注入表探测-注入）。
        bool changed = false;
        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const jbc::ir::InsnId id = order[pos];
            const jbc::ir::Insn*  in = code.dom.peek(id);
            if (in == nullptr || !frames.reachable(id) || !isInvokeKind(in->op)) continue;
            MemberRefSem ref;
            if (!resolveMemberRef(*ctx.cp, in->cp, ref)) continue;
            const ReflectionValue value = transferInvoke(env, frames, *in, id, pos, ref);
            const auto            it    = injected.find(id.slot);
            if (it == injected.end()) {
                injected.emplace(id.slot, value);
                changed = true;
            }
            else if (!(it->second == value)) {
                it->second = value;
                changed    = true;
            }
        }
        if (!changed) {
            // 收口：逐调用点接收者快照（含 alias "class:X" 接收者覆盖）。
            for (ca::usize pos = 0; pos < order.size(); ++pos) {
                const jbc::ir::InsnId id = order[pos];
                const jbc::ir::Insn*  in = code.dom.peek(id);
                if (in == nullptr || !frames.reachable(id) || !isInvokeKind(in->op)) continue;
                MemberRefSem ref;
                if (!resolveMemberRef(*ctx.cp, in->cp, ref)) continue;
                const auto argTypes  = paramTypes(ref.desc);
                const auto receiver = frames.stackFromTop(id, static_cast<ca::u32>(argTypes.size()));
                if (receiver == nullptr) continue;   // 栈深不足：旧栈 receiverIndex<0 同口径
                ReflectionValue value = *receiver;
                if (ctx.aliases != nullptr) {
                    const auto* alias = ctx.aliases->reflectionReceiverOf(
                        ctx.owner, ctx.name, ctx.desc, static_cast<ca::i32>(pos));
                    if (alias != nullptr && alias->isKnown() &&
                        alias->identity.substr(0, 6) == "class:") {
                        value.kind  = ReflectionValue::Kind::Class;
                        value.owner = aliasInterned(alias->identity.substr(6));
                    }
                }
                snap.receivers.emplace(static_cast<ca::u32>(pos), value);
            }
            snap.ok = true;
            return snap;
        }
    }
    return ReflectionSnapshot{};   // 注入不收敛：保守 fail-open
}

}   // namespace mjbase::reference
