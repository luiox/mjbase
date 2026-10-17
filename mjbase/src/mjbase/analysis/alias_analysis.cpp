#include "mjbase/analysis/alias_analysis.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "jbc/core/opcode.hpp"

#include "mjbase/asm/analyzer/frame_query.hpp"

#include "class_scan.hpp"
#include "cp_refs.hpp"
#include "mjbase/hierarchy/summary.hpp"

namespace mjbase::analysis {

using mjbase::hierarchy::kAccStatic;

namespace {

using jbc::core::Op;
using jbc::analysis::VKind;

// ---------------------------------------------------------------------------
// 驻留池与哈希
// ---------------------------------------------------------------------------

/// 别名身份/字段键共用的进程级只增驻留池（旧栈 aliasInterned 的池化形态）。
KeyPool& identityPool()
{
    static KeyPool pool;
    return pool;
}


}   // namespace

std::size_t SlotKeyHash::operator()(const SlotKey& k) const noexcept
{
    const std::hash<std::string_view> h;
    const std::size_t a = h(k.identity);
    const std::size_t b = (static_cast<std::size_t>(k.field.cls) << 40) ^
                          (static_cast<std::size_t>(k.field.name) << 20) ^ k.field.desc;
    return a * 0x9E3779B97F4A7C15ull ^ b;
}

std::size_t PairViewHash::operator()(
    const std::pair<std::string_view, std::string_view>& k) const noexcept
{
    const std::hash<std::string_view> h;
    return h(k.first) * 0x9E3779B97F4A7C15ull ^ (h(k.second) + 0x9E3779B97F4A7C15ull);
}

// ---------------------------------------------------------------------------
// mjbase::analysis 作用域：hpp 私有 buildFixedPoint 签名按前向声明引用本类型，
// 必须与实现同型（不放匿名命名空间）。
// 方法上下文：一个被分析方法的静态视图（会话 + 代码 + 身份）
// ---------------------------------------------------------------------------

struct AliasMethodContext
{
    const jbc::ir::ClassSession* session = nullptr;
    const jbc::ir::CodeUnit*     code    = nullptr;   ///< null = 无代码/升格失败
    MemberKey                    method{};
    std::string_view             owner;
    std::string_view             name;
    std::string_view             desc;
    bool                         isStatic       = false;
    bool                         malformed      = false;   ///< 描述符不可解析（不产出摘要）
    bool                         hasControlFlow = false;
    std::vector<std::string_view> paramDescs;
    std::unordered_map<ca::u32, ca::usize> paramSlots;   ///< local 槽 → 形参序号
    std::unordered_map<ca::u32, ca::usize> posOf;        ///< InsnId.slot → order 下标
    jbc::analysis::MethodSig     sig{};
    std::string                  methodPrefix;   ///< "owner.name+desc"（new:/array: 身份前缀）
    std::string                  methodKey;      ///< "owner\nname\ndesc"（andersen 节点前缀）

    const jbc::core::ConstantPool& cp() const { return session->backing().cp; }
};

/// load 族指令的 local 槽位（含 ALOAD_0..3/ILOAD_0..3 紧凑形）；非 load 族 false。
inline bool loadLocalIndexOf(const jbc::ir::Insn& in, ca::u32& out)
{
    using jbc::core::Op;
    switch (in.op) {
    case Op::ILOAD: case Op::LLOAD: case Op::FLOAD: case Op::DLOAD: case Op::ALOAD:
        out = static_cast<ca::u32>(in.a);
        return true;
    default:
        if (in.op >= Op::ILOAD_0 && in.op <= Op::ALOAD_3) {
            out = static_cast<ca::u32>(static_cast<ca::u8>(in.op) -
                                       static_cast<ca::u8>(Op::ILOAD_0));
            return true;
        }
        return false;
    }
}

std::string_view aliasInterned(std::string_view text)
{
    return identityPool().str(identityPool().intern(text));
}

MemberKey aliasMemberKey(std::string_view owner, std::string_view name, std::string_view desc)
{
    return memberKeyOf(identityPool(), classKeyOf(identityPool(), owner), name, desc);
}

AliasValue mergeAliasValues(const AliasValue& lhs, const AliasValue& rhs)
{
    if (lhs.isKnown() && rhs.isKnown() && lhs == rhs) return lhs;
    AliasValue result;
    result.slots =
        lhs.slots == rhs.slots ? lhs.slots : (lhs.slots < rhs.slots ? lhs.slots : rhs.slots);
    result.pointsTo = lhs.pointsTo;
    result.pointsTo.insert(result.pointsTo.end(), rhs.pointsTo.begin(), rhs.pointsTo.end());
    std::sort(result.pointsTo.begin(), result.pointsTo.end());
    result.pointsTo.erase(std::unique(result.pointsTo.begin(), result.pointsTo.end()),
                          result.pointsTo.end());
    return result;
}

AliasFrame mergeAliasFrames(const AliasFrame& lhs, const AliasFrame& rhs)
{
    AliasFrame result;
    result.hasExceptionalPredecessor =
        lhs.hasExceptionalPredecessor || rhs.hasExceptionalPredecessor;
    result.locals = lhs.locals;
    for (const auto& [slot, value] : rhs.locals) {
        auto it = result.locals.find(slot);
        if (it == result.locals.end()) {
            result.locals.emplace(slot, AliasValue::unknown());
        }
        else {
            it->second = mergeAliasValues(it->second, value);
        }
    }
    // 只在左侧存在的槽位对右侧前驱同样是未知入值；保持交换与失败闭合。
    for (const auto& [slot, value] : lhs.locals) {
        if (rhs.locals.find(slot) == rhs.locals.end()) {
            result.locals[slot] = AliasValue::unknown();
        }
    }
    return result;
}

namespace {

// ---------------------------------------------------------------------------
// 描述符与容器集合（旧栈 Type::getSort / getInternalName 判定的描述符直读版）
// ---------------------------------------------------------------------------

/// 方法描述符的参数类型序列（"(" 后逐类型；复用 jbc 引擎的 oneType 扫描）。
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

/// 方法描述符的返回类型；畸形返回空。
std::string_view returnTypeOf(std::string_view desc)
{
    if (desc.empty() || desc.front() != '(') return {};
    const auto close = desc.find(')');
    if (close == std::string_view::npos) return {};
    return desc.substr(close + 1);
}

/// 类型描述符 → 类别内部名（"Ljava/util/List;" → "java/util/List"；数组原样）。
std::string_view internalNameOf(std::string_view desc)
{
    if (desc.size() >= 2 && desc.front() == 'L' && desc.back() == ';') {
        return desc.substr(1, desc.size() - 2);
    }
    return desc;
}

bool isContainerDesc(std::string_view desc)
{
    if (desc.empty() || desc.front() != 'L') return false;
    const std::string_view name = internalNameOf(desc);
    return name == "java/util/Collection" || name == "java/util/List" || name == "java/util/Set" ||
           name == "java/util/Deque" || name == "java/util/Map" || name == "java/util/Queue" ||
           name == "java/util/ArrayList" || name == "java/util/LinkedList" ||
           name == "java/util/Vector" || name == "java/util/Stack" ||
           name == "java/util/ArrayDeque" || name == "java/util/PriorityQueue" ||
           name == "java/util/HashMap" || name == "java/util/LinkedHashMap" ||
           name == "java/util/TreeMap" || name == "java/util/WeakHashMap" ||
           name == "java/util/IdentityHashMap" ||
           name == "java/util/concurrent/ConcurrentHashMap" ||
           name == "java/util/concurrent/CopyOnWriteArrayList" ||
           name == "java/util/concurrent/LinkedBlockingQueue";
}

bool isMapOwner(std::string_view owner)
{
    return owner == "java/util/Map" || owner == "java/util/HashMap" ||
           owner == "java/util/LinkedHashMap" || owner == "java/util/TreeMap" ||
           owner == "java/util/WeakHashMap" || owner == "java/util/IdentityHashMap" ||
           owner == "java/util/concurrent/ConcurrentHashMap";
}

bool isListOwner(std::string_view owner)
{
    return owner == "java/util/List" || owner == "java/util/Collection" ||
           owner == "java/util/ArrayList" || owner == "java/util/LinkedList" ||
           owner == "java/util/Vector" || owner == "java/util/Stack" ||
           owner == "java/util/concurrent/CopyOnWriteArrayList";
}

bool isQueueOwner(std::string_view owner)
{
    return owner == "java/util/Queue" || owner == "java/util/Deque" ||
           owner == "java/util/ArrayDeque" || owner == "java/util/PriorityQueue" ||
           owner == "java/util/concurrent/LinkedBlockingQueue";
}

bool isReflectionLookup(std::string_view owner, std::string_view name)
{
    if (owner != "java/lang/Class") return false;
    return name == "getMethod" || name == "getDeclaredMethod" || name == "getConstructor" ||
           name == "getDeclaredConstructor" || name == "getField" || name == "getDeclaredField";
}

/// 类型描述符 → 别名值（旧栈 valueForType 的排序判定：数组→容器→对象→原始）。
AliasValue valueForType(std::string_view desc, std::string_view identity, ca::u8 width = 1)
{
    if (!desc.empty() && desc.front() == '[') return AliasValue::array(identity);
    if (isContainerDesc(desc)) return AliasValue::container(identity);
    if (!desc.empty() && desc.front() == 'L') return AliasValue::object(identity);
    AliasValue value = AliasValue::primitive(identity);
    value.slots      = width;
    return value;
}

/// 形参描述符的槽宽（J/D 双槽）。
ca::u8 descWidth(std::string_view desc)
{
    return desc == "J" || desc == "D" ? 2 : 1;
}

/// "param:N" 身份的 N；非形参身份返回 -1。
ca::i32 parameterIdentityIndex(const AliasValue& value)
{
    constexpr std::string_view prefix = "param:";
    if (value.identity.substr(0, prefix.size()) != prefix) return -1;
    const std::string_view rest = value.identity.substr(prefix.size());
    if (rest.empty()) return -1;
    ca::i32 out = 0;
    for (const char c : rest) {
        if (c < '0' || c > '9') return -1;
        out = out * 10 + (c - '0');
    }
    return out;
}

/// "this:" 前缀身份（接收者形参）。
bool isThisIdentity(const AliasValue& value)
{
    return value.identity.substr(0, 5) == "this:";
}

bool isFormalIdentity(const AliasValue& value)
{
    return value.isKnown() && (value.identity.substr(0, 6) == "param:" || isThisIdentity(value));
}

/// 旧栈 instantiateReturn：callee 返回值按调用点 σ 实例化。
AliasValue instantiateReturn(const AliasMethodSummary*      summary,
                             const std::vector<AliasValue>& arguments, const AliasValue& receiver,
                             std::string_view retDesc, ca::u8 width)
{
    if (summary == nullptr) return AliasValue::unknown(width);
    if (!summary->returnValue.isKnown()) return summary->returnValue;
    const auto parameterIndex = parameterIdentityIndex(summary->returnValue);
    if (parameterIndex >= 0 && static_cast<ca::usize>(parameterIndex) < arguments.size()) {
        AliasValue out = arguments[static_cast<ca::usize>(parameterIndex)];
        out.slots      = width;
        return out;
    }
    if (isThisIdentity(summary->returnValue)) {
        AliasValue out = receiver;
        out.slots      = width;
        return out;
    }
    return valueForType(retDesc, summary->returnValue.identity, width);
}

/// 旧栈 instantiateFact：事实身份按调用点 σ 重写。
AliasValue instantiateFact(const AliasValue& value, const std::vector<AliasValue>& arguments,
                           const AliasValue& receiver)
{
    if (!value.isKnown()) return value;
    const auto parameterIndex = parameterIdentityIndex(value);
    if (parameterIndex >= 0 && static_cast<ca::usize>(parameterIndex) < arguments.size()) {
        return arguments[static_cast<ca::usize>(parameterIndex)];
    }
    if (isThisIdentity(value)) return receiver;
    return value;
}

bool isInvokeKind(Op op)
{
    return op == Op::INVOKEVIRTUAL || op == Op::INVOKESPECIAL || op == Op::INVOKESTATIC ||
           op == Op::INVOKEINTERFACE;
}

/// 逐方法构建上下文（collectCodeUnits 就地升格；无代码/升格失败 = code 为空）。
void buildMethodContexts(const std::vector<ScannedClass>&                  classes,
                         std::vector<std::unique_ptr<AliasMethodContext>>& out)
{
    for (const auto& clazz : classes) {
        jbc::ir::ClassSession& session = *clazz.session;
        jbc::ir::ClassUnit&    unit    = session.unit();
        (void)jbc::ir::collectCodeUnits(session);   // 无代码与真失败分流后统一借 m.code
        for (const auto& m : unit.methods) {
            auto ctx             = std::make_unique<AliasMethodContext>();
            ctx->session         = &session;
            ctx->owner           = unit.symbols().str(unit.thisClass);
            ctx->name            = unit.symbols().str(m.name);
            ctx->desc            = unit.symbols().str(m.desc);
            ctx->method          = aliasMemberKey(ctx->owner, ctx->name, ctx->desc);
            ctx->isStatic        = (m.access & kAccStatic) != 0;
            ctx->methodPrefix    = std::string(ctx->owner) + "." + std::string(ctx->name) +
                                std::string(ctx->desc);
            ctx->methodKey       = std::string(ctx->owner) + "\n" + std::string(ctx->name) + "\n" +
                              std::string(ctx->desc);
            ctx->sig.isStatic    = ctx->isStatic;
            ctx->sig.desc        = ctx->desc;
            ctx->code            = m.code.get();

            ctx->paramDescs = paramTypes(ctx->desc);
            if (!ctx->desc.empty() && ctx->desc.front() != '(') {
                ctx->malformed = true;   // 旧栈 Type::getArgumentTypes 抛出同口径
            }
            else {
                ca::u32 slot = ctx->isStatic ? 0 : 1;
                for (ca::usize i = 0; i < ctx->paramDescs.size(); ++i) {
                    ctx->paramSlots[slot] = i;
                    slot += static_cast<ca::u32>(
                        jbc::analysis::detail::typeSlots(ctx->paramDescs[i]));
                }
            }

            if (ctx->code != nullptr) {
                const auto& order = ctx->code->dom.order();
                // 异常表同样要求 CFG 精化（处理器入口帧只有引擎能合并；
                // 线性走查会把处理器路径的 ARETURN 误并入正常返回）。
                if (!ctx->code->exc.empty()) ctx->hasControlFlow = true;
                for (ca::usize i = 0; i < order.size(); ++i) {
                    ctx->posOf.emplace(order[i].slot, i);
                    const jbc::ir::Insn* in = ctx->code->dom.peek(order[i]);
                    if (in == nullptr) continue;
                    const jbc::core::Shape sh = jbc::core::shapeOf(in->op);
                    if (sh == jbc::core::Shape::Branch2 || sh == jbc::core::Shape::Branch4 ||
                        sh == jbc::core::Shape::TableSwitch ||
                        sh == jbc::core::Shape::LookupSwitch) {
                        ctx->hasControlFlow = true;
                    }
                }
            }
            out.push_back(std::move(ctx));
        }
    }
}

// ---------------------------------------------------------------------------
// 线性走查（旧栈 summarize 的主走查：自带栈/局部状态，操作数全程可见）
// ---------------------------------------------------------------------------

using FieldSlotKey   = SlotKey;
using ElementSlotKey = std::pair<std::string_view, std::string_view>;

struct LinearWalk
{
    AliasMethodSummary                                           summary;
    std::vector<AliasValue>                                      stack;
    std::unordered_map<ca::i32, AliasValue>                      locals;
    std::unordered_map<ElementSlotKey, AliasValue, PairViewHash> arrayElements;
    std::unordered_map<ElementSlotKey, AliasValue, PairViewHash> containerElements;
    std::unordered_map<FieldSlotKey, AliasValue, SlotKeyHash>    instanceFields;
    bool                                                         sawReturn = false;
    bool                                                         malformedCall = false;

    AliasValue popAlias()
    {
        if (stack.empty()) return AliasValue::unknown();
        AliasValue value = stack.back();
        stack.pop_back();
        return value;
    }

    static FieldSlotKey fieldKey(const AliasValue& receiver, const MemberKey& field)
    {
        return FieldSlotKey{receiver.identity, field};
    }
    static ElementSlotKey elementKey(const AliasValue& holder, const AliasValue& key)
    {
        return ElementSlotKey{holder.identity,
                              key.isKnown() ? key.identity : std::string_view{}};
    }

    void invalidateContainer(const AliasValue& target)
    {
        if (!target.isKnown()) return;
        for (auto it = containerElements.begin(); it != containerElements.end();) {
            if (it->first.first == target.identity) {
                it = containerElements.erase(it);
            }
            else {
                ++it;
            }
        }
        containerElements[elementKey(target, AliasValue::unknown())] = AliasValue::unknown();
        summary.containerWrites.push_back({target, AliasValue::unknown(), AliasValue::unknown()});
    }
};

std::optional<AliasMethodSummary> summarizeLinear(const AliasMethodContext& ctx,
                                                  const AliasAnalysis*      previous)
{
    LinearWalk walk;
    walk.summary.method = ctx.method;

    ca::i32 slot = 0;
    if (!ctx.isStatic) {
        walk.locals[slot++] = AliasValue::object("this:" + std::string(ctx.owner));
    }
    for (ca::usize i = 0; i < ctx.paramDescs.size(); ++i) {
        const AliasValue value =
            valueForType(ctx.paramDescs[i], "param:" + std::to_string(i),
                         descWidth(ctx.paramDescs[i]));
        walk.summary.parameters.push_back(value);
        walk.locals[slot] = value;
        slot += static_cast<ca::i32>(jbc::analysis::detail::typeSlots(ctx.paramDescs[i]));
    }

    if (ctx.code == nullptr) {
        return walk.summary;   // 无代码方法：参数摘要 + 未知返回（旧栈空指令表同形）
    }

    const jbc::ir::CodeUnit&       u     = *ctx.code;
    const jbc::core::ConstantPool& cp    = ctx.cp();
    const auto&                    order = u.dom.order();

    for (ca::usize insnIndex = 0; insnIndex < order.size(); ++insnIndex) {
        const jbc::ir::Insn* in = u.dom.peek(order[insnIndex]);
        if (in == nullptr) continue;
        const Op op = in->op;

        switch (op) {
        case Op::LDC:
        case Op::LDC_W:
        case Op::LDC2_W:
        {
            if (cp.valid(in->cp)) {
                const auto& e = cp.at(in->cp);
                if (e.tag == jbc::core::CpTag::String && cp.isUtf8(e.a)) {
                    walk.stack.push_back(
                        AliasValue::primitive("string:" + std::string(cp.utf8Of(e.a))));
                    break;
                }
                if (e.tag == jbc::core::CpTag::Class && cp.isUtf8(e.a)) {
                    const std::string_view n = cp.utf8Of(e.a);
                    // 旧栈 Type::getObjectType(name).getSort()==OBJECT|ARRAY：内部名
                    // （>1 字符）或数组描述符；单字符为原始/void 排序，不成事实。
                    if (!n.empty() && (n.front() == '[' || n.size() > 1)) {
                        walk.stack.push_back(AliasValue::object("class:" + std::string(n)));
                        break;
                    }
                }
            }
            walk.stack.push_back(AliasValue::unknown());
            break;
        }
        case Op::ACONST_NULL: walk.stack.push_back(AliasValue::nullValue()); break;
        case Op::ICONST_M1:
        case Op::ICONST_0:
        case Op::ICONST_1:
        case Op::ICONST_2:
        case Op::ICONST_3:
        case Op::ICONST_4:
        case Op::ICONST_5:
        {
            const ca::i32 v = op == Op::ICONST_M1
                                  ? -1
                                  : static_cast<ca::i32>(op) - static_cast<ca::i32>(Op::ICONST_0);
            walk.stack.push_back(AliasValue::primitive("int:" + std::to_string(v)));
            break;
        }
        case Op::ALOAD:
        case Op::ILOAD:
        {
            auto it = walk.locals.find(in->a);
            walk.stack.push_back(it != walk.locals.end() ? it->second : AliasValue::unknown());
            break;
        }
        case Op::ASTORE: walk.locals[in->a] = walk.popAlias(); break;
        case Op::DUP:
            walk.stack.push_back(walk.stack.empty() ? AliasValue::unknown() : walk.stack.back());
            break;
        case Op::NEW:
        {
            const std::string_view cls = resolveClassName(cp, in->cp);
            walk.stack.push_back(AliasValue::object("new:" + ctx.methodPrefix + ":" +
                                                    std::to_string(insnIndex) + ":" +
                                                    std::string(cls)));
            break;
        }
        case Op::NEWARRAY:
        case Op::ANEWARRAY:
            walk.popAlias();
            walk.stack.push_back(
                AliasValue::array("array:" + ctx.methodPrefix + ":" + std::to_string(insnIndex)));
            break;
        case Op::MULTIANEWARRAY:
        {
            const auto dims = static_cast<ca::u32>(in->a);
            for (ca::u32 d = 0; d < dims; ++d) walk.popAlias();
            walk.stack.push_back(
                AliasValue::array("array:" + ctx.methodPrefix + ":" + std::to_string(insnIndex)));
            break;
        }
        case Op::GETSTATIC:
        {
            MemberRefSem ref;
            if (!resolveMemberRef(cp, in->cp, ref)) break;
            const AliasValue* prior = previous != nullptr
                                          ? previous->fieldValueOf(ref.owner, ref.name, ref.desc)
                                          : nullptr;
            AliasValue value =
                prior != nullptr
                    ? *prior
                    : valueForType(ref.desc,
                                   "field:" + std::string(ref.owner) + "." + std::string(ref.name) +
                                       ":" + std::string(ref.desc),
                                   descWidth(ref.desc));
            walk.stack.push_back(value);
            break;
        }
        case Op::GETFIELD:
        {
            const AliasValue receiver = walk.popAlias();
            MemberRefSem ref;
            if (!resolveMemberRef(cp, in->cp, ref)) break;
            const MemberKey field = aliasMemberKey(ref.owner, ref.name, ref.desc);
            const auto      local = walk.instanceFields.find(LinearWalk::fieldKey(receiver, field));
            const auto      prior =
                previous != nullptr
                    ? previous->resolvedFieldValueOf(receiver, ref.owner, ref.name, ref.desc)
                    : std::nullopt;
            AliasValue value =
                local != walk.instanceFields.end()
                    ? local->second
                    : (prior.has_value()
                           ? *prior
                           : valueForType(ref.desc,
                                          "field:" + std::string(ref.owner) + "." +
                                              std::string(ref.name) + ":" + std::string(ref.desc),
                                          descWidth(ref.desc)));
            walk.stack.push_back(value);
            break;
        }
        case Op::PUTFIELD:
        {
            const AliasValue value    = walk.popAlias();
            const AliasValue receiver = walk.popAlias();
            MemberRefSem ref;
            if (!resolveMemberRef(cp, in->cp, ref)) break;
            const MemberKey field = aliasMemberKey(ref.owner, ref.name, ref.desc);
            if (receiver.isKnown()) {
                walk.instanceFields[LinearWalk::fieldKey(receiver, field)] = value;
            }
            walk.summary.fieldWrites.push_back({field, receiver, value, false});
            break;
        }
        case Op::PUTSTATIC:
        {
            const AliasValue value = walk.popAlias();
            MemberRefSem ref;
            if (!resolveMemberRef(cp, in->cp, ref)) break;
            walk.summary.fieldWrites.push_back(
                {aliasMemberKey(ref.owner, ref.name, ref.desc), AliasValue::unknown(), value, true});
            break;
        }
        case Op::IASTORE:
        case Op::LASTORE:
        case Op::FASTORE:
        case Op::DASTORE:
        case Op::AASTORE:
        case Op::BASTORE:
        case Op::CASTORE:
        case Op::SASTORE:
        {
            const AliasValue value = walk.popAlias();
            const AliasValue index = walk.popAlias();
            const AliasValue array = walk.popAlias();
            if (array.isKnown()) {
                const auto key = LinearWalk::elementKey(array, index);
                auto       it  = walk.arrayElements.find(key);
                walk.arrayElements[key] =
                    it == walk.arrayElements.end() ? value : mergeAliasValues(it->second, value);
            }
            walk.summary.arrayWrites.push_back({array, index, value});
            break;
        }
        case Op::IALOAD:
        case Op::LALOAD:
        case Op::FALOAD:
        case Op::DALOAD:
        case Op::AALOAD:
        case Op::BALOAD:
        case Op::CALOAD:
        case Op::SALOAD:
        {
            const AliasValue index = walk.popAlias();
            const AliasValue array = walk.popAlias();
            AliasValue value      = AliasValue::unknown();
            bool       foundLocal = false;
            if (array.isKnown()) {
                auto it = walk.arrayElements.find(LinearWalk::elementKey(array, index));
                if (it == walk.arrayElements.end() && index.isKnown()) {
                    it = walk.arrayElements.find(ElementSlotKey{array.identity, std::string_view{}});
                }
                if (it != walk.arrayElements.end()) {
                    value      = it->second;
                    foundLocal = true;
                }
            }
            if (!foundLocal && previous != nullptr) {
                const auto prior = previous->resolvedArrayValueOf(array, index);
                if (prior.has_value()) value = *prior;
            }
            walk.stack.push_back(value);
            break;
        }
        case Op::INVOKEVIRTUAL:
        case Op::INVOKESPECIAL:
        case Op::INVOKESTATIC:
        case Op::INVOKEINTERFACE:
        {
            MemberRefSem ref;
            if (!resolveMemberRef(cp, in->cp, ref)) break;
            if (ref.desc.empty() || ref.desc.front() != '(') {
                // 旧栈 Type::getArgumentTypes 在调用点抛出：整个方法无摘要。
                walk.malformedCall = true;
                break;
            }
            const bool             callStatic = op == Op::INVOKESTATIC;
            const auto             argTypes   = paramTypes(ref.desc);
            const std::string_view callOwner  = ref.owner;
            const std::string_view callName   = ref.name;

            if (isReflectionLookup(callOwner, callName) && walk.stack.size() > argTypes.size()) {
                const auto receiverIndex = walk.stack.size() - argTypes.size() - 1;
                walk.summary.reflectionReceivers.push_back(
                    {static_cast<ca::i32>(insnIndex), walk.stack[receiverIndex]});
            }
            std::vector<AliasValue> arguments(argTypes.size());
            for (ca::usize i = argTypes.size(); i > 0; --i) {
                arguments[i - 1] = walk.popAlias();
            }
            AliasValue receiver = callStatic ? AliasValue::unknown() : walk.popAlias();
            const std::string_view retDesc = returnTypeOf(ref.desc);
            const bool containerWrite =
                (isMapOwner(callOwner) || isListOwner(callOwner) || isQueueOwner(callOwner)) &&
                (callName == "add" || callName == "offer" || callName == "put" ||
                 callName == "set" || callName == "push");
            const bool containerDestructiveRead =
                isQueueOwner(callOwner) && arguments.empty() &&
                (callName == "poll" || callName == "pop" || callName == "remove");
            const bool containerRead =
                (isMapOwner(callOwner) || isListOwner(callOwner) || isQueueOwner(callOwner)) &&
                (callName == "get" || callName == "peek" || callName == "element" ||
                 callName == "getOrDefault" || containerDestructiveRead);
            const bool containerInvalidate =
                (isMapOwner(callOwner) || isListOwner(callOwner) || isQueueOwner(callOwner)) &&
                (callName == "clear" || callName == "remove" || callName == "removeAll" ||
                 callName == "retainAll" || callName == "putAll" || callName == "addAll" ||
                 callName == "replaceAll" || callName == "compute" ||
                 callName == "computeIfAbsent" || callName == "computeIfPresent" ||
                 callName == "merge") &&
                !containerDestructiveRead;
            AliasValue containerKeyValue = AliasValue::unknown();
            if (isMapOwner(callOwner)) {
                if (!arguments.empty()) containerKeyValue = arguments.front();
            }
            else if (callName == "set" && !arguments.empty()) {
                containerKeyValue = arguments.front();
            }
            const AliasMethodSummary* callee =
                previous != nullptr ? previous->summaryOf(callOwner, callName, ref.desc) : nullptr;
            if (callee != nullptr) {
                AliasMethodSummary::CallBinding binding;
                binding.callee           = aliasMemberKey(callOwner, callName, ref.desc);
                binding.instructionIndex = static_cast<ca::i32>(insnIndex);
                binding.receiver         = receiver;
                binding.arguments        = arguments;
                binding.fwReceiver       = receiver;
                binding.fwArguments      = arguments;
                walk.summary.callBindings.push_back(std::move(binding));

                // 展开流以 callee 槽位播种（环上路径每方法至多一次）。
                std::vector<ca::usize> visiting;
                ca::usize              calleeSlot = 0;
                if (previous->resolveBinding(binding.callee, &calleeSlot) != nullptr) {
                    visiting.push_back(calleeSlot);
                }
                std::vector<AliasMethodSummary::FieldWrite> inlineFieldWrites;
                previous->expandFieldWrites(*callee, visiting, inlineFieldWrites);
                for (const auto& write : inlineFieldWrites) {
                    const auto actualReceiver = instantiateFact(write.receiver, arguments, receiver);
                    const auto actualValue    = instantiateFact(write.value, arguments, receiver);
                    if (!write.isStatic) {
                        std::vector<std::string_view> receivers = actualReceiver.pointsTo;
                        if (receivers.empty() && actualReceiver.isKnown()) {
                            receivers.push_back(actualReceiver.identity);
                        }
                        for (const auto identity : receivers) {
                            const AliasValue target = AliasValue::object(identity);
                            const auto       key    = LinearWalk::fieldKey(target, write.field);
                            auto             it     = walk.instanceFields.find(key);
                            walk.instanceFields[key] =
                                it == walk.instanceFields.end()
                                    ? actualValue
                                    : mergeAliasValues(it->second, actualValue);
                        }
                    }
                }
                std::vector<AliasMethodSummary::ArrayWrite> inlineArrayWrites;
                previous->expandArrayWrites(*callee, visiting, inlineArrayWrites);
                for (const auto& write : inlineArrayWrites) {
                    const auto actualArray = instantiateFact(write.array, arguments, receiver);
                    const auto actualIndex = instantiateFact(write.index, arguments, receiver);
                    const auto actualValue = instantiateFact(write.value, arguments, receiver);
                    if (actualArray.isKnown()) {
                        const auto key = LinearWalk::elementKey(actualArray, actualIndex);
                        auto       it  = walk.arrayElements.find(key);
                        walk.arrayElements[key] =
                            it == walk.arrayElements.end() ? actualValue
                                                           : mergeAliasValues(it->second, actualValue);
                    }
                }
                std::vector<AliasMethodSummary::ContainerWrite> inlineContainerWrites;
                previous->expandContainerWrites(*callee, visiting, inlineContainerWrites);
                for (const auto& write : inlineContainerWrites) {
                    const auto actualContainer =
                        instantiateFact(write.container, arguments, receiver);
                    const auto actualKey   = instantiateFact(write.key, arguments, receiver);
                    const auto actualValue = instantiateFact(write.value, arguments, receiver);
                    if (actualContainer.isKnown()) {
                        if (!actualValue.isKnown()) {
                            for (auto it = walk.containerElements.begin();
                                 it != walk.containerElements.end();) {
                                if (it->first.first == actualContainer.identity) {
                                    it = walk.containerElements.erase(it);
                                }
                                else {
                                    ++it;
                                }
                            }
                        }
                        walk.containerElements[LinearWalk::elementKey(actualContainer, actualKey)] =
                            actualValue;
                    }
                }
            }
            if (containerWrite && !arguments.empty() && receiver.isKnown()) {
                const auto key   = LinearWalk::elementKey(receiver, containerKeyValue);
                auto       it    = walk.containerElements.find(key);
                const auto value = arguments.back();
                walk.containerElements[key] =
                    it == walk.containerElements.end() ? value
                                                       : mergeAliasValues(it->second, value);
                walk.summary.containerWrites.push_back({receiver, containerKeyValue, value});
            }
            const bool unknownContainerCall =
                callee == nullptr && !containerWrite && !containerRead && !containerInvalidate;
            if (unknownContainerCall) {
                walk.invalidateContainer(receiver);
                for (const auto& argument : arguments) {
                    if (argument.kind == AliasValueKind::Container) {
                        walk.invalidateContainer(argument);
                    }
                }
            }
            if (containerInvalidate && receiver.isKnown()) {
                walk.invalidateContainer(receiver);
            }
            if (!retDesc.empty() && retDesc.front() != 'V') {
                bool pushed = false;
                if (containerRead && receiver.isKnown()) {
                    auto local = walk.containerElements.find(
                        LinearWalk::elementKey(receiver, containerKeyValue));
                    if (local == walk.containerElements.end() && containerKeyValue.isKnown()) {
                        local = walk.containerElements.find(
                            ElementSlotKey{receiver.identity, std::string_view{}});
                    }
                    if (local != walk.containerElements.end()) {
                        const auto value = local->second;
                        if (containerDestructiveRead) walk.invalidateContainer(receiver);
                        walk.stack.push_back(value);
                        pushed = true;
                    }
                    else if (previous != nullptr) {
                        const auto* prior = previous->containerValueOf(receiver, containerKeyValue);
                        if (prior != nullptr) {
                            const auto value = *prior;
                            if (containerDestructiveRead) walk.invalidateContainer(receiver);
                            walk.stack.push_back(value);
                            pushed = true;
                        }
                    }
                }
                if (!pushed) {
                    if (containerDestructiveRead && receiver.isKnown()) {
                        walk.invalidateContainer(receiver);
                    }
                    walk.stack.push_back(
                        instantiateReturn(callee, arguments, receiver, retDesc, descWidth(retDesc)));
                }
            }
            break;
        }
        case Op::ARETURN:
        {
            const AliasValue value = walk.popAlias();
            walk.summary.returnValue =
                walk.sawReturn ? mergeAliasValues(walk.summary.returnValue, value) : value;
            walk.sawReturn = true;
            break;
        }
        case Op::ATHROW:
            walk.summary.hasExceptionalExit = true;
            walk.popAlias();
            break;
        default: break;
        }
        if (walk.malformedCall) break;
    }
    if (walk.malformedCall) return std::nullopt;   // 畸形调用点：旧栈同口径，无摘要
    return walk.summary;
}

// ---------------------------------------------------------------------------
// 别名事实域（IValueDomain<AliasValue>）：产点事实 + CFG join 合并
// ---------------------------------------------------------------------------

/// @brief 域环境：池/前轮摘要/方法身份 + 操作数依赖事实注入表。
struct AliasDomainEnv
{
    const jbc::core::ConstantPool*                    cp        = nullptr;
    const AliasAnalysis*                              previous  = nullptr;
    const AliasMethodContext*                         ctx       = nullptr;
    /// 主跑注入表：InsnId.slot → 操作数依赖事实（GETFIELD/CHECKCAST/invoke 结果）。
    const std::unordered_map<ca::u32, AliasValue>*    injected  = nullptr;
};

class AliasDomain final : public jbc::analysis::IValueDomain<AliasValue>
{
public:
    explicit AliasDomain(const AliasDomainEnv& env) noexcept
        : env_(env)
    {}

    AliasValue top() override { return AliasValue::unknown(); }

    /// join：同事实保持；分歧（含 Unknown）→ Unknown，保留诊断 points-to 并集。
    AliasValue merge(const AliasValue& a, const AliasValue& b) override
    {
        return mergeAliasValues(a, b);
    }

    /// 一切产值点：操作数无关事实就地成值；操作数依赖产点查注入表。
    AliasValue newValue(const jbc::ir::Insn& in, jbc::ir::InsnId at, VKind shape) override
    {
        const ca::u8 width =
            shape == VKind::Long || shape == VKind::Double ? 2 : 1;
        if (env_.injected != nullptr) {
            const auto injected = env_.injected->find(at.slot);
            if (injected != env_.injected->end()) {
                AliasValue v = injected->second;
                v.slots      = width;
                return v;
            }
        }

        const jbc::core::ConstantPool& cp = *env_.cp;
        // 引擎空槽 fallback（处理器入口帧不带局部）：形参槽回退形参身份——
        // 旧栈流程分析在异常前驱合并局部，形参在处理器内可见（语义对齐）。
        ca::u32 loadSlot = 0;
        if (loadLocalIndexOf(in, loadSlot)) {
            if (!env_.ctx->isStatic && loadSlot == 0) {
                return AliasValue::object("this:" + std::string(env_.ctx->owner));
            }
            const auto pit = env_.ctx->paramSlots.find(loadSlot);
            if (pit != env_.ctx->paramSlots.end()) {
                const auto i = pit->second;
                return valueForType(env_.ctx->paramDescs[i],
                                    "param:" + std::to_string(i),
                                    descWidth(env_.ctx->paramDescs[i]));
            }
            return AliasValue::unknown(width);
        }
        switch (in.op) {
        case Op::ACONST_NULL: return AliasValue::nullValue();
        case Op::NEW:
        {
            const std::string_view cls = resolveClassName(cp, in.cp);
            return AliasValue::object("new:" + env_.ctx->methodPrefix + ":" +
                                      std::to_string(orderPos(at)) + ":" + std::string(cls));
        }
        case Op::NEWARRAY:
        case Op::ANEWARRAY:
        case Op::MULTIANEWARRAY:
            return AliasValue::array("array:" + env_.ctx->methodPrefix + ":" +
                                     std::to_string(orderPos(at)));
        case Op::LDC:
        case Op::LDC_W:
        case Op::LDC2_W:
        {
            if (cp.valid(in.cp)) {
                const auto& e = cp.at(in.cp);
                if (e.tag == jbc::core::CpTag::String && cp.isUtf8(e.a)) {
                    return AliasValue::primitive("string:" + std::string(cp.utf8Of(e.a)));
                }
                if (e.tag == jbc::core::CpTag::Class && cp.isUtf8(e.a)) {
                    const std::string_view n = cp.utf8Of(e.a);
                    if (!n.empty() && (n.front() == '[' || n.size() > 1)) {
                        return AliasValue::object("class:" + std::string(n));
                    }
                }
            }
            return AliasValue::unknown(width);
        }
        case Op::GETSTATIC:
        {
            MemberRefSem ref;
            if (!resolveMemberRef(cp, in.cp, ref)) return AliasValue::unknown(width);
            const AliasValue* prior = env_.previous != nullptr
                                          ? env_.previous->fieldValueOf(ref.owner, ref.name, ref.desc)
                                          : nullptr;
            if (prior != nullptr) {
                AliasValue v = *prior;
                v.slots      = width;
                return v;
            }
            return valueForType(ref.desc,
                                "field:" + std::string(ref.owner) + "." + std::string(ref.name) +
                                    ":" + std::string(ref.desc),
                                width);
        }
        case Op::GETFIELD:
        {
            MemberRefSem ref;
            if (!resolveMemberRef(cp, in.cp, ref)) return AliasValue::unknown(width);
            return valueForType(ref.desc,
                                "field:" + std::string(ref.owner) + "." + std::string(ref.name) +
                                    ":" + std::string(ref.desc),
                                width);
        }
        case Op::INVOKEVIRTUAL:
        case Op::INVOKESPECIAL:
        case Op::INVOKESTATIC:
        case Op::INVOKEINTERFACE:
        {
            // 操作数无关部分：callee 返回值为具体身份时直接按返回类型成值；
            // param:/this: 替换与未知返回的 points-to 保持依赖注入表。
            MemberRefSem ref;
            if (!resolveMemberRef(cp, in.cp, ref)) return AliasValue::unknown(width);
            const AliasMethodSummary* callee =
                env_.previous != nullptr
                    ? env_.previous->summaryOf(ref.owner, ref.name, ref.desc)
                    : nullptr;
            if (callee == nullptr) return AliasValue::unknown(width);
            const std::string_view retDesc = returnTypeOf(ref.desc);
            if (callee->returnValue.isKnown() && parameterIdentityIndex(callee->returnValue) < 0 &&
                !isThisIdentity(callee->returnValue)) {
                return valueForType(retDesc, callee->returnValue.identity, width);
            }
            return AliasValue::unknown(width);
        }
        default: return AliasValue::unknown(width);
        }
    }

    /// 方法入参：this / param:N 身份（按描述符分类，cat2 双槽）。
    AliasValue newParameter(ca::u32 localIdx, VKind shape) override
    {
        const ca::u8 width = shape == VKind::Long || shape == VKind::Double ? 2 : 1;
        if (!env_.ctx->isStatic && localIdx == 0) {
            return AliasValue::object("this:" + std::string(env_.ctx->owner));
        }
        const auto it = env_.ctx->paramSlots.find(localIdx);
        if (it != env_.ctx->paramSlots.end()) {
            const auto i = it->second;
            return valueForType(env_.ctx->paramDescs[i], "param:" + std::to_string(i),
                                descWidth(env_.ctx->paramDescs[i]));
        }
        return AliasValue::unknown(width);
    }

    /// 异常处理器入口 throwable：未知来源。
    AliasValue newException(const jbc::ir::CodeUnit::Exc&) override
    {
        return AliasValue::unknown();
    }

    ca::u8 width(const AliasValue& v) override { return v.slots; }

    /// fixpoint 相等：类别/身份/points-to/槽宽逐项。
    bool equal(const AliasValue& a, const AliasValue& b) override
    {
        return a.kind == b.kind && a.identity == b.identity && a.slots == b.slots &&
               a.pointsTo == b.pointsTo;
    }

private:
    ca::usize orderPos(jbc::ir::InsnId at) const
    {
        const auto it = env_.ctx->posOf.find(at.slot);
        return it != env_.ctx->posOf.end() ? it->second : 0;
    }

    AliasDomainEnv env_;
};

// ---------------------------------------------------------------------------
// CFG 精化（旧栈 analyzeControlFlowFacts 的域引擎形态）
// ---------------------------------------------------------------------------

struct ControlFlowAliasFacts
{
    std::optional<AliasValue>                   returnValue;
    std::vector<AliasMethodSummary::FieldWrite> fieldWrites;

    /// 调用点 fw σ（CFG 帧值），由 summarize 重写对应绑定的 FW 组。
    struct CallFlowSigma
    {
        ca::i32                 instructionIndex = -1;
        AliasValue              receiver;
        std::vector<AliasValue> arguments;
        bool                    eligible = false;
    };
    std::vector<CallFlowSigma> flowSigmas;
    bool                       snapshotFailed = false;
};

/// 探测跑（注入关闭）→ 从探测帧读操作数依赖事实 → 注入主跑。
ControlFlowAliasFacts analyzeControlFlowFacts(const AliasMethodContext& ctx,
                                              const AliasAnalysis*      previous)
{
    ControlFlowAliasFacts result;
    if (ctx.code == nullptr) {
        result.snapshotFailed = true;
        return result;
    }
    const jbc::ir::CodeUnit& u = *ctx.code;

    AliasDomainEnv env;
    env.cp       = &ctx.cp();
    env.previous = previous;
    env.ctx      = &ctx;

    // 探测跑：操作数依赖产点退回保守缺省（GETFIELD → field: 缺省，invoke →
    // 具体身份/未知）。产点事实中操作数无关部分（this/param/new/class/
    // string/array/getstatic）与旧解释器逐点一致。
    AliasDomain probeDom(env);
    jbc::analysis::DomainEngine<AliasValue> probeEngine(*env.cp, probeDom);
    jbc::analysis::DomainResult<AliasValue> probe = probeEngine.analyze(u, ctx.sig);
    if (!probe.summary.ok || probe.summary.hasSubroutine) {
        // fail-open：引擎拒斥（JSR/RET、预算超限）与旧栈空快照同口径。
        result.snapshotFailed = true;
        return result;
    }

    // 注入表：探测帧读操作数 → 前轮摘要实例化（旧解释器的 nary/unary 语义）。
    std::unordered_map<ca::u32, AliasValue> injected;
    {
        asm_analyzer::FrameQuery<AliasValue> probeQuery(u, std::move(probe));
        const auto&                          order = u.dom.order();
        for (ca::usize pos = 0; pos < order.size(); ++pos) {
            const jbc::ir::InsnId id = order[pos];
            const jbc::ir::Insn*  in = u.dom.peek(id);
            if (in == nullptr || !probeQuery.reachable(id)) continue;
            if (in->op == Op::GETFIELD) {
                const AliasValue* receiver = probeQuery.stackFromTop(id, 0);
                if (receiver == nullptr || previous == nullptr) continue;
                MemberRefSem ref;
                if (!resolveMemberRef(ctx.cp(), in->cp, ref)) continue;
                const auto prior =
                    previous->resolvedFieldValueOf(*receiver, ref.owner, ref.name, ref.desc);
                if (prior.has_value()) injected.emplace(id.slot, *prior);
            }
            else if (in->op == Op::CHECKCAST) {
                const AliasValue* operand = probeQuery.stackFromTop(id, 0);
                if (operand != nullptr) injected.emplace(id.slot, *operand);
            }
            else if (isInvokeKind(in->op)) {
                MemberRefSem ref;
                if (!resolveMemberRef(ctx.cp(), in->cp, ref)) continue;
                const AliasMethodSummary* callee =
                    previous != nullptr ? previous->summaryOf(ref.owner, ref.name, ref.desc)
                                        : nullptr;
                if (callee == nullptr) continue;
                const auto  argTypes  = paramTypes(ref.desc);
                const bool  isStatic  = in->op == Op::INVOKESTATIC;
                const auto  required  = argTypes.size() + (isStatic ? 0u : 1u);
                const auto* deepest   = probeQuery.stackFromTop(id, static_cast<ca::u32>(required - 1));
                if (deepest == nullptr) continue;   // 栈深不足：旧栈该调用点跳过
                const std::string_view retDesc = returnTypeOf(ref.desc);
                const auto     offset  = isStatic ? 0u : 1u;
                const ca::u32  base    = static_cast<ca::u32>(required - 1 - offset);
                AliasValue              receiver = AliasValue::unknown();
                std::vector<AliasValue> arguments(argTypes.size());
                if (!isStatic) receiver = *deepest;
                for (ca::usize i = 0; i < argTypes.size(); ++i) {
                    // 栈顶向内第 base-i 个即第 i 个形参（越界 → 未知）。
                    const auto* arg = base >= static_cast<ca::u32>(i)
                                          ? probeQuery.stackFromTop(id, base - static_cast<ca::u32>(i))
                                          : nullptr;
                    arguments[i] = arg != nullptr ? *arg : AliasValue::unknown();
                }
                const AliasValue value =
                    instantiateReturn(callee, arguments, receiver, retDesc, descWidth(retDesc));
                injected.emplace(id.slot, value);
            }
        }
    }

    // 主跑：注入后帧值与旧解释器在操作数无关链上逐点一致。
    env.injected = &injected;
    AliasDomain mainDom(env);
    jbc::analysis::DomainEngine<AliasValue> mainEngine(*env.cp, mainDom);
    jbc::analysis::DomainResult<AliasValue> main = mainEngine.analyze(u, ctx.sig);
    if (!main.summary.ok || main.summary.hasSubroutine) {
        result.snapshotFailed = true;
        return result;
    }

    asm_analyzer::FrameQuery<AliasValue> frames(u, std::move(main));
    bool sawReturn = false;
    for (ca::usize pos = 0; pos < u.dom.order().size(); ++pos) {
        const jbc::ir::InsnId id  = u.dom.order()[pos];
        const jbc::ir::Insn*  in  = u.dom.peek(id);
        if (in == nullptr || !frames.reachable(id)) continue;
        if (in->op == Op::PUTFIELD) {
            const AliasValue* value    = frames.stackFromTop(id, 0);
            const AliasValue* receiver = frames.stackFromTop(id, 1);
            if (value == nullptr || receiver == nullptr) continue;
            MemberRefSem ref;
            if (!resolveMemberRef(ctx.cp(), in->cp, ref)) continue;
            result.fieldWrites.push_back({aliasMemberKey(ref.owner, ref.name, ref.desc), *receiver,
                                          *value, false});
        }
        else if (in->op == Op::PUTSTATIC) {
            const AliasValue* value = frames.stackFromTop(id, 0);
            if (value == nullptr) continue;
            MemberRefSem ref;
            if (!resolveMemberRef(ctx.cp(), in->cp, ref)) continue;
            result.fieldWrites.push_back(
                {aliasMemberKey(ref.owner, ref.name, ref.desc), AliasValue::unknown(), *value, true});
        }
        else if (in->op == Op::ARETURN) {
            const AliasValue* value = frames.stackFromTop(id, 0);
            if (value == nullptr) continue;
            result.returnValue =
                sawReturn ? mergeAliasValues(*result.returnValue, *value) : *value;
            sawReturn = true;
        }
        else if (isInvokeKind(in->op) && previous != nullptr) {
            MemberRefSem ref;
            if (!resolveMemberRef(ctx.cp(), in->cp, ref)) continue;
            const auto* callee = previous->summaryOf(ref.owner, ref.name, ref.desc);
            if (callee == nullptr) continue;
            ControlFlowAliasFacts::CallFlowSigma sigma;
            sigma.instructionIndex = static_cast<ca::i32>(pos);
            const auto argTypes    = paramTypes(ref.desc);
            const bool isStatic    = in->op == Op::INVOKESTATIC;
            const auto required    = argTypes.size() + (isStatic ? 0u : 1u);
            const auto offset      = isStatic ? 0u : 1u;
            if (frames.stackFromTop(id, static_cast<ca::u32>(required - 1)) != nullptr) {
                sigma.receiver = isStatic ? AliasValue::unknown()
                                          : *frames.stackFromTop(id, static_cast<ca::u32>(required - 1));
                sigma.arguments.reserve(argTypes.size());
                for (ca::usize argument = 0; argument < argTypes.size(); ++argument) {
                    const auto* arg = frames.stackFromTop(
                        id, static_cast<ca::u32>(required - 1 - offset - argument));
                    sigma.arguments.push_back(arg != nullptr ? *arg : AliasValue::unknown());
                }
                sigma.eligible = true;
            }
            result.flowSigmas.push_back(std::move(sigma));
        }
    }
    return result;
}

/// 摘要构建：线性走查 + 分支方法的 CFG 精化（ returnValue/fieldWrites 重写）。
std::optional<AliasMethodSummary> summarizeMethod(const AliasMethodContext& ctx,
                                                  const AliasAnalysis*      previous)
{
    if (ctx.malformed) return std::nullopt;
    auto linear = summarizeLinear(ctx, previous);
    if (!linear.has_value()) return std::nullopt;   // 畸形调用点：无摘要（旧栈同口径）
    AliasMethodSummary summary = std::move(*linear);

    if (ctx.hasControlFlow) {
        const ControlFlowAliasFacts flowFacts = analyzeControlFlowFacts(ctx, previous);
        if (flowFacts.snapshotFailed) {
            summary.returnValue = AliasValue::unknown();
            summary.fieldWrites.clear();
            for (auto& binding : summary.callBindings) binding.fieldWritesActive = false;
        }
        else {
            if (flowFacts.returnValue.has_value()) {
                summary.returnValue = *flowFacts.returnValue;
            }
            summary.fieldWrites = flowFacts.fieldWrites;
            // 绑定 FW 组按 flow 语义重写：调用点按 CFG 帧值重写 fw σ，栈深不足
            // 的调用点 FW 组失效（与旧栈逐调用点 continue 语义一致）。
            for (const auto& sigma : flowFacts.flowSigmas) {
                for (auto& binding : summary.callBindings) {
                    if (binding.instructionIndex != sigma.instructionIndex) continue;
                    if (sigma.eligible) {
                        binding.fwReceiver  = sigma.receiver;
                        binding.fwArguments = sigma.arguments;
                    }
                    else {
                        binding.fieldWritesActive = false;
                    }
                    break;
                }
            }
        }
    }
    if (ctx.code != nullptr && !ctx.code->exc.empty()) {
        // 异常处理器入口已在引擎侧合并真实保护区前驱；保留异常出口位供消费方。
        summary.hasExceptionalExit = true;
    }
    return summary;
}

// ---------------------------------------------------------------------------
// Andersen 字节码约束（旧栈 AndersenConstraintInterpreter 的域引擎形态）
// ---------------------------------------------------------------------------

/// @brief 约束值：节点句柄 + 槽宽（cat2 布局需要）。
struct ConstraintNode
{
    AndersenPointsToSolver::NameId node  = 0;
    ca::u8                         slots = 1;
};

class AndersenConstraintDomain final : public jbc::analysis::IValueDomain<ConstraintNode>
{
public:
    AndersenConstraintDomain(AndersenPointsToSolver& solver, const AliasMethodContext& ctx)
        : solver_(solver)
        , ctx_(ctx)
        , method_(ctx.methodKey)
    {}

    ConstraintNode top() override { return ConstraintNode{}; }

    ConstraintNode merge(const ConstraintNode& lhs, const ConstraintNode& rhs) override
    {
        if (lhs.node == 0) return rhs;
        if (rhs.node == 0) return lhs;
        if (lhs.node == rhs.node) return lhs;
        std::vector<AndersenPointsToSolver::NameId> sources;
        const auto appendSources = [&](AndersenPointsToSolver::NameId node) {
            const auto found = mergeSources_.find(node);
            if (found == mergeSources_.end()) {
                sources.push_back(node);
            }
            else {
                sources.insert(sources.end(), found->second.begin(), found->second.end());
            }
        };
        appendSources(lhs.node);
        appendSources(rhs.node);
        std::sort(sources.begin(), sources.end());
        sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
        std::string nodeText = method_ + "\nmerge";
        for (const auto source : sources) {
            nodeText += "\n";
            nodeText += std::string(solver_.nameOf(source));
        }
        const auto node = solver_.intern(nodeText);
        mergeSources_[node] = sources;
        for (const auto source : sources) solver_.addCopy(node, source);
        return ConstraintNode{node, lhs.slots == rhs.slots ? lhs.slots
                                                           : (lhs.slots < rhs.slots ? lhs.slots
                                                                                    : rhs.slots)};
    }

    ConstraintNode newValue(const jbc::ir::Insn& in, jbc::ir::InsnId at, VKind shape) override
    {
        const ca::u8 width = shape == VKind::Long || shape == VKind::Double ? 2 : 1;
        const jbc::core::ConstantPool& cp = ctx_.cp();
        // 处理器入口帧空槽回退：load 族落到形参节点（与别名域同口径）。
        ca::u32 loadSlot = 0;
        if (loadLocalIndexOf(in, loadSlot)) {
            if (!ctx_.isStatic && loadSlot == 0) {
                return ConstraintNode{thisNodeId(), width};
            }
            const auto pit = ctx_.paramSlots.find(loadSlot);
            if (pit != ctx_.paramSlots.end()) {
                return ConstraintNode{solver_.internIndexed(
                                          method_ + "\nparam:", static_cast<ca::i64>(pit->second)),
                                      width};
            }
            return ConstraintNode{0, width};
        }
        if (in.op == Op::NEW) {
            const auto       node = temporary(at);
            const std::string_view cls = resolveClassName(cp, in.cp);
            // 分配点名物化一次并内码（旧栈同串形："new:method\nkey:idx:type"）。
            std::string alloc;
            alloc.assign("new:").append(method_).append(1, ':').append(
                std::to_string(orderPos(at))).append(1, ':').append(cls);
            solver_.addAddressOf(node, solver_.intern(alloc));
            return ConstraintNode{node, width};
        }
        if (in.op == Op::GETSTATIC) {
            MemberRefSem ref;
            if (resolveMemberRef(cp, in.cp, ref)) {
                return ConstraintNode{staticField(ref), width};
            }
            return ConstraintNode{0, width};
        }
        if (in.op == Op::ANEWARRAY || in.op == Op::NEWARRAY || in.op == Op::MULTIANEWARRAY) {
            const auto node = temporary(at);
            solver_.addAddressOf(node, solver_.internIndexed("array:" + method_ + ":",
                                                             static_cast<ca::i64>(orderPos(at))));
            return ConstraintNode{node, width};
        }
        if (in.op == Op::GETFIELD || isInvokeKind(in.op)) {
            // 结果节点 = 指令临时（与旧栈同身份）；操作数依赖边在帧后处理补。
            return ConstraintNode{temporary(at), width};
        }
        return ConstraintNode{0, width};
    }

    ConstraintNode newParameter(ca::u32 localIdx, VKind shape) override
    {
        const ca::u8 width = shape == VKind::Long || shape == VKind::Double ? 2 : 1;
        if (!ctx_.isStatic && localIdx == 0) return ConstraintNode{thisNodeId(), width};
        const auto it = ctx_.paramSlots.find(localIdx);
        if (it != ctx_.paramSlots.end()) {
            return ConstraintNode{solver_.internIndexed(method_ + "\nparam:",
                                                        static_cast<ca::i64>(it->second)),
                                  width};
        }
        return ConstraintNode{0, width};
    }

    ConstraintNode newException(const jbc::ir::CodeUnit::Exc&) override { return ConstraintNode{}; }

    ConstraintNode binary(Op op, const ConstraintNode& a, const ConstraintNode& b, VKind shape,
                          const jbc::ir::Insn& at, jbc::ir::InsnId id) override
    {
        if (op == Op::AALOAD && a.node != 0) {
            const auto node = temporary(id);
            solver_.addLoad(node, a.node, arrayElementFieldId());
            return ConstraintNode{node, 1};
        }
        (void)b;
        (void)at;
        return ConstraintNode{0, static_cast<ca::u8>(shape == VKind::Long || shape == VKind::Double ? 2 : 1)};
    }

    ca::u8 width(const ConstraintNode& v) override { return v.slots; }

    bool equal(const ConstraintNode& a, const ConstraintNode& b) override
    {
        return a.node == b.node && a.slots == b.slots;
    }

    // ── 帧后处理（操作数依赖边；引擎产点钩子看不到操作数栈）──

    /// GETFIELD：addLoad(temp, receiverNode, fieldName)。
    void addGetFieldEdge(const jbc::ir::Insn& in, jbc::ir::InsnId at,
                         AndersenPointsToSolver::NameId receiverNode)
    {
        if (receiverNode == 0) return;
        MemberRefSem ref;
        if (!resolveMemberRef(ctx_.cp(), in.cp, ref)) return;
        solver_.addLoad(temporary(at), receiverNode, fieldName(ref));
    }

    /// CHECKCAST：透传 = 结果临时节点 ← 操作数节点的 copy 边（约束语义等价）。
    void addCheckCastEdge(jbc::ir::InsnId at, AndersenPointsToSolver::NameId operandNode)
    {
        if (operandNode == 0) return;
        solver_.addCopy(temporary(at), operandNode);
    }

    /// PUTFIELD：addStore(receiverNode, fieldName, valueNode)。
    void addPutFieldEdge(const jbc::ir::Insn& in, AndersenPointsToSolver::NameId receiverNode,
                         AndersenPointsToSolver::NameId valueNode)
    {
        if (receiverNode == 0 || valueNode == 0) return;
        MemberRefSem ref;
        if (!resolveMemberRef(ctx_.cp(), in.cp, ref)) return;
        solver_.addStore(receiverNode, fieldName(ref), valueNode);
    }

    /// PUTSTATIC：addCopy(staticField, valueNode)。
    void addPutStaticEdge(const jbc::ir::Insn& in, AndersenPointsToSolver::NameId valueNode)
    {
        if (valueNode == 0) return;
        MemberRefSem ref;
        if (!resolveMemberRef(ctx_.cp(), in.cp, ref)) return;
        solver_.addCopy(staticField(ref), valueNode);
    }

    /// AASTORE：addStore(arrayNode, "[]", valueNode)。
    void addArrayStoreEdge(AndersenPointsToSolver::NameId arrayNode,
                           AndersenPointsToSolver::NameId valueNode)
    {
        if (arrayNode == 0 || valueNode == 0) return;
        solver_.addStore(arrayNode, arrayElementFieldId(), valueNode);
    }

    /// ARETURN：addCopy(returnNode, valueNode)。
    void addReturnEdge(AndersenPointsToSolver::NameId valueNode)
    {
        if (valueNode == 0) return;
        solver_.addCopy(returnNodeId(), valueNode);
    }

    /// invoke：receiver/args → callee 形参节点 copy 边（返回边已在 newValue 建）。
    void addInvokeEdges(const jbc::ir::Insn& in, AndersenPointsToSolver::NameId receiverNode,
                        const std::vector<AndersenPointsToSolver::NameId>& argumentNodes)
    {
        MemberRefSem ref;
        if (!resolveMemberRef(ctx_.cp(), in.cp, ref)) return;
        const std::string calleeKey =
            std::string(ref.owner) + "\n" + std::string(ref.name) + "\n" + std::string(ref.desc);
        const bool isStatic = in.op == Op::INVOKESTATIC;
        if (!isStatic && receiverNode != 0) {
            solver_.addCopy(calleeThisId(calleeKey), receiverNode);
        }
        for (ca::usize index = 0; index < argumentNodes.size(); ++index) {
            if (argumentNodes[index] == 0) continue;
            solver_.addCopy(calleeParamId(calleeKey, index), argumentNodes[index]);
        }
    }

private:
    AndersenPointsToSolver::NameId temporary(jbc::ir::InsnId at)
    {
        return solver_.internIndexed(method_ + "\ninsn:", static_cast<ca::i64>(orderPos(at)));
    }

    AndersenPointsToSolver::NameId thisNodeId()
    {
        if (thisNodeId_ == 0) thisNodeId_ = solver_.intern(method_ + "\nthis") + 1;
        return thisNodeId_ - 1;
    }

    AndersenPointsToSolver::NameId returnNodeId()
    {
        if (returnNodeId_ == 0) returnNodeId_ = solver_.intern(method_ + "\nreturn") + 1;
        return returnNodeId_ - 1;
    }

    AndersenPointsToSolver::NameId calleeThisId(const std::string& calleeKey)
    {
        return solver_.intern(calleeKey + "\nthis");
    }

    AndersenPointsToSolver::NameId calleeParamId(const std::string& calleeKey, ca::usize index)
    {
        return solver_.internIndexed(calleeKey + "\nparam:", static_cast<ca::i64>(index));
    }

    AndersenPointsToSolver::NameId fieldName(const MemberRefSem& ref)
    {
        std::string scratch;
        scratch.assign(ref.owner).append(1, '.').append(ref.name).append(1, ':').append(ref.desc);
        return solver_.intern(scratch);
    }

    AndersenPointsToSolver::NameId staticField(const MemberRefSem& ref)
    {
        std::string scratch;
        scratch.assign("static-field\n").append(ref.owner).append(1, '.').append(ref.name).append(1, ':').append(ref.desc);
        return solver_.intern(scratch);
    }

    AndersenPointsToSolver::NameId arrayElementFieldId()
    {
        if (arrayElementFieldId_ == 0) arrayElementFieldId_ = solver_.intern("[]") + 1;
        return arrayElementFieldId_ - 1;
    }

    ca::usize orderPos(jbc::ir::InsnId at) const
    {
        const auto it = ctx_.posOf.find(at.slot);
        return it != ctx_.posOf.end() ? it->second : 0;
    }

    AndersenPointsToSolver&       solver_;
    const AliasMethodContext&     ctx_;
    std::string                   method_;
    AndersenPointsToSolver::NameId thisNodeId_          = 0;
    AndersenPointsToSolver::NameId returnNodeId_        = 0;
    AndersenPointsToSolver::NameId arrayElementFieldId_ = 0;
    std::unordered_map<AndersenPointsToSolver::NameId, std::vector<AndersenPointsToSolver::NameId>>
        mergeSources_;
};

/// 逐方法抽约束 + 一次 solve（旧栈 indexAndersenBytecode 的域引擎形态）。
void indexAndersenBytecode(const std::vector<std::unique_ptr<AliasMethodContext>>& ctxs,
                           AndersenPointsToSolver&                                 solver)
{
    for (const auto& ctx : ctxs) {
        if (ctx->code == nullptr || ctx->malformed) continue;
        const jbc::ir::CodeUnit& u = *ctx->code;

        AndersenConstraintDomain dom(solver, *ctx);
        jbc::analysis::DomainEngine<ConstraintNode> engine(ctx->cp(), dom);
        jbc::analysis::DomainResult<ConstraintNode> result = engine.analyze(u, ctx->sig);
        // fail-open：引擎拒斥或子程序残留不产出约束（旧栈空快照同口径）。
        if (!result.summary.ok || result.summary.hasSubroutine) continue;

        asm_analyzer::FrameQuery<ConstraintNode> frames(u, std::move(result));
        for (const auto& id : u.dom.order()) {
            const jbc::ir::Insn* in = u.dom.peek(id);
            if (in == nullptr || !frames.reachable(id)) continue;
            if (in->op == Op::GETFIELD) {
                const ConstraintNode* receiver = frames.stackFromTop(id, 0);
                if (receiver != nullptr) dom.addGetFieldEdge(*in, id, receiver->node);
            }
            else if (in->op == Op::CHECKCAST) {
                const ConstraintNode* operand = frames.stackFromTop(id, 0);
                if (operand != nullptr) dom.addCheckCastEdge(id, operand->node);
            }
            else if (in->op == Op::PUTFIELD) {
                const ConstraintNode* value    = frames.stackFromTop(id, 0);
                const ConstraintNode* receiver = frames.stackFromTop(id, 1);
                if (value != nullptr && receiver != nullptr) {
                    dom.addPutFieldEdge(*in, receiver->node, value->node);
                }
            }
            else if (in->op == Op::PUTSTATIC) {
                const ConstraintNode* value = frames.stackFromTop(id, 0);
                if (value != nullptr) dom.addPutStaticEdge(*in, value->node);
            }
            else if (in->op == Op::AASTORE) {
                const ConstraintNode* value = frames.stackFromTop(id, 0);
                const ConstraintNode* array = frames.stackFromTop(id, 2);
                if (value != nullptr && array != nullptr) {
                    dom.addArrayStoreEdge(array->node, value->node);
                }
            }
            else if (in->op == Op::ARETURN) {
                const ConstraintNode* value = frames.stackFromTop(id, 0);
                if (value != nullptr) dom.addReturnEdge(value->node);
            }
            else if (isInvokeKind(in->op)) {
                MemberRefSem ref;
                if (!resolveMemberRef(ctx->cp(), in->cp, ref)) continue;
                const auto     argTypes = paramTypes(ref.desc);
                const bool     isStatic = in->op == Op::INVOKESTATIC;
                const auto     required = argTypes.size() + (isStatic ? 0u : 1u);
                const auto     offset   = isStatic ? 0u : 1u;
                if (frames.stackFromTop(id, static_cast<ca::u32>(required - 1)) == nullptr) {
                    continue;   // 栈深不足：旧栈读帧缺省跳过
                }
                const ConstraintNode* receiver =
                    isStatic ? nullptr : frames.stackFromTop(id, static_cast<ca::u32>(required - 1));
                std::vector<AndersenPointsToSolver::NameId> argumentNodes(argTypes.size(), 0);
                for (ca::usize i = 0; i < argTypes.size(); ++i) {
                    const ConstraintNode* arg =
                        frames.stackFromTop(id, static_cast<ca::u32>(required - 1 - offset - i));
                    if (arg != nullptr) argumentNodes[i] = arg->node;
                }
                dom.addInvokeEdges(*in, receiver != nullptr ? receiver->node : 0, argumentNodes);
            }
        }
    }
    solver.solve();
}

// ---------------------------------------------------------------------------
// 摘要比较（旧栈 sameSummary 家族）
// ---------------------------------------------------------------------------

bool sameAliasFact(const AliasValue& lhs, const AliasValue& rhs)
{
    return lhs.kind == rhs.kind && lhs.identity == rhs.identity && lhs.pointsTo == rhs.pointsTo;
}

bool sameFieldWrite(const AliasMethodSummary::FieldWrite& lhs,
                    const AliasMethodSummary::FieldWrite& rhs)
{
    return lhs.field == rhs.field && sameAliasFact(lhs.receiver, rhs.receiver) &&
           sameAliasFact(lhs.value, rhs.value) && lhs.isStatic == rhs.isStatic;
}

bool sameContainerWrite(const AliasMethodSummary::ContainerWrite& lhs,
                        const AliasMethodSummary::ContainerWrite& rhs)
{
    return sameAliasFact(lhs.container, rhs.container) && sameAliasFact(lhs.key, rhs.key) &&
           sameAliasFact(lhs.value, rhs.value);
}

bool sameArrayWrite(const AliasMethodSummary::ArrayWrite& lhs,
                    const AliasMethodSummary::ArrayWrite& rhs)
{
    return sameAliasFact(lhs.array, rhs.array) && sameAliasFact(lhs.index, rhs.index) &&
           sameAliasFact(lhs.value, rhs.value);
}

bool sameCallBinding(const AliasMethodSummary::CallBinding& lhs,
                     const AliasMethodSummary::CallBinding& rhs)
{
    if (lhs.callee != rhs.callee || lhs.instructionIndex != rhs.instructionIndex ||
        !sameAliasFact(lhs.receiver, rhs.receiver) ||
        !sameAliasFact(lhs.fwReceiver, rhs.fwReceiver) ||
        lhs.arguments.size() != rhs.arguments.size() ||
        lhs.fwArguments.size() != rhs.fwArguments.size() ||
        lhs.fieldWritesActive != rhs.fieldWritesActive ||
        lhs.arrayWritesActive != rhs.arrayWritesActive ||
        lhs.containerWritesActive != rhs.containerWritesActive) {
        return false;
    }
    for (ca::usize index = 0; index < lhs.arguments.size(); ++index) {
        if (!sameAliasFact(lhs.arguments[index], rhs.arguments[index])) return false;
    }
    for (ca::usize index = 0; index < lhs.fwArguments.size(); ++index) {
        if (!sameAliasFact(lhs.fwArguments[index], rhs.fwArguments[index])) return false;
    }
    return true;
}

bool sameSummary(const AliasMethodSummary& lhs, const AliasMethodSummary& rhs)
{
    if (lhs.method != rhs.method || lhs.hasExceptionalExit != rhs.hasExceptionalExit ||
        lhs.parameters.size() != rhs.parameters.size() ||
        lhs.fieldWrites.size() != rhs.fieldWrites.size() ||
        lhs.arrayWrites.size() != rhs.arrayWrites.size() ||
        lhs.containerWrites.size() != rhs.containerWrites.size() ||
        lhs.reflectionReceivers.size() != rhs.reflectionReceivers.size() ||
        lhs.callBindings.size() != rhs.callBindings.size() ||
        !sameAliasFact(lhs.returnValue, rhs.returnValue)) {
        return false;
    }
    for (ca::usize index = 0; index < lhs.parameters.size(); ++index) {
        if (!sameAliasFact(lhs.parameters[index], rhs.parameters[index])) return false;
    }
    for (ca::usize index = 0; index < lhs.fieldWrites.size(); ++index) {
        if (!sameFieldWrite(lhs.fieldWrites[index], rhs.fieldWrites[index])) return false;
    }
    for (ca::usize index = 0; index < lhs.arrayWrites.size(); ++index) {
        if (!sameArrayWrite(lhs.arrayWrites[index], rhs.arrayWrites[index])) return false;
    }
    for (ca::usize index = 0; index < lhs.containerWrites.size(); ++index) {
        if (!sameContainerWrite(lhs.containerWrites[index], rhs.containerWrites[index])) {
            return false;
        }
    }
    for (ca::usize index = 0; index < lhs.reflectionReceivers.size(); ++index) {
        if (lhs.reflectionReceivers[index].instructionIndex !=
                rhs.reflectionReceivers[index].instructionIndex ||
            !sameAliasFact(lhs.reflectionReceivers[index].receiver,
                           rhs.reflectionReceivers[index].receiver)) {
            return false;
        }
    }
    for (ca::usize index = 0; index < lhs.callBindings.size(); ++index) {
        if (!sameCallBinding(lhs.callBindings[index], rhs.callBindings[index])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 反射闭包作用域选择（旧栈 selectReflectionClosure 的会话形态）
// ---------------------------------------------------------------------------

std::vector<const AliasMethodContext*> selectReflectionClosure(
    const std::vector<std::unique_ptr<AliasMethodContext>>& ctxs)
{
    // 类名 → 成员上下文集合（扫描以类为粒度）。
    std::unordered_map<std::string_view, std::vector<const AliasMethodContext*>> byClass;
    for (const auto& ctx : ctxs) byClass[ctx->owner].push_back(ctx.get());

    std::unordered_set<std::string_view> inScope;
    std::vector<std::string_view>        queue;
    std::unordered_map<std::string_view, std::vector<std::string_view>> callTargets;

    for (const auto& [owner, list] : byClass) {
        bool seed = false;
        std::vector<std::string_view>& targets = callTargets[owner];
        for (const AliasMethodContext* ctx : list) {
            if (ctx->session == nullptr) continue;
            const jbc::ir::ClassUnit& unit = ctx->session->unit();
            // R4：声明 Class 形静态字段（静态类常量缓存的典型声明点）。
            for (const auto& field : unit.fields) {
                if ((field.access & kAccStatic) != 0 &&
                    unit.symbols().str(field.desc) == "Ljava/lang/Class;") {
                    seed = true;
                    break;
                }
            }
            if (ctx->code == nullptr) continue;
            const jbc::ir::CodeUnit& u = *ctx->code;
            for (const auto& id : u.dom.order()) {
                const jbc::ir::Insn* in = u.dom.peek(id);
                if (in == nullptr) continue;
                MemberRefSem ref;
                if (isInvokeKind(in->op) && resolveMemberRef(ctx->cp(), in->cp, ref)) {
                    if (isReflectionLookup(ref.owner, ref.name)) seed = true;   // R1
                    targets.push_back(ref.owner);
                    continue;
                }
                if ((in->op == Op::GETSTATIC || in->op == Op::PUTSTATIC) &&
                    resolveMemberRef(ctx->cp(), in->cp, ref)) {
                    // R3：Class 形静态槽读写（槽按 field 全局聚合，跨类介导）。
                    if (ref.desc == "Ljava/lang/Class;") seed = true;
                }
            }
        }
        if (seed && inScope.insert(owner).second) queue.push_back(owner);
    }

    // R2：容器内传递 callee 闭包（JDK 目标不在容器内，自然截断）。
    for (ca::usize cursor = 0; cursor < queue.size(); ++cursor) {
        auto found = callTargets.find(queue[cursor]);
        if (found == callTargets.end()) continue;
        for (const auto target : found->second) {
            if (byClass.count(target) != 0 && inScope.insert(target).second) {
                queue.push_back(target);
            }
        }
    }

    std::vector<const AliasMethodContext*> scope;
    for (const auto& ctx : ctxs) {
        if (inScope.count(ctx->owner) != 0) scope.push_back(ctx.get());
    }
    return scope;
}

}   // namespace

// ---------------------------------------------------------------------------
// AliasAnalysis：索引 / 沉积 / 固定点 / 查询
// ---------------------------------------------------------------------------

void AliasAnalysis::clear()
{
    byKey_.clear();
    summaries_.clear();
    fields_.clear();
    fieldsByKey_.clear();
    arrayValues_.clear();
    containerValues_.clear();
    andersen_.clear();
    convergenceRounds_ = 0;
    expansionSource_   = nullptr;
    suspendAndersenIndexing_ = false;
}

void AliasAnalysis::build(const mjbase::model::ResourceContainer& container)
{
    clear();
    auto classes = scanClassResources(container);
    std::vector<std::unique_ptr<AliasMethodContext>> ctxs;
    buildMethodContexts(classes, ctxs);

    std::vector<const AliasMethodContext*> scope;
    scope.reserve(ctxs.size());
    for (const auto& ctx : ctxs) scope.push_back(ctx.get());
    buildFixedPoint(scope, scope.size());
    // 收敛后一次性做约束图字节码索引（旧栈固定点后调 indexAndersenBytecode）；
    // ctxs/会话随本函数退出消亡，摘要/槽值均为自持值（身份走进程级驻留池）。
    indexAndersenBytecode(ctxs, andersen_);
}

void AliasAnalysis::buildReflectionClosure(const mjbase::model::ResourceContainer& container)
{
    clear();
    auto classes = scanClassResources(container);
    std::vector<std::unique_ptr<AliasMethodContext>> ctxs;
    buildMethodContexts(classes, ctxs);

    const std::vector<const AliasMethodContext*> scope = selectReflectionClosure(ctxs);
    ca::usize scopedMethodCount = 0;
    for (const auto* ctx : scope) ++scopedMethodCount;
    buildFixedPoint(scope, scopedMethodCount);
    indexAndersenBytecode(ctxs, andersen_);
}

const AliasMethodSummary* AliasAnalysis::resolveBinding(const MemberKey& calleeKey,
                                                        ca::usize*       slotIndex) const
{
    const AliasAnalysis* domain = expansionSource_ != nullptr ? expansionSource_ : this;
    const auto           it     = domain->byKey_.find(calleeKey);
    if (it == domain->byKey_.end()) return nullptr;
    if (slotIndex != nullptr) *slotIndex = it->second;
    return &domain->summaries_[it->second];
}

void AliasAnalysis::expandFieldWrites(const AliasMethodSummary&                    summary,
                                      std::vector<ca::usize>&                      visiting,
                                      std::vector<AliasMethodSummary::FieldWrite>& out) const
{
    const AliasAnalysis* source = expansionSource_ != nullptr ? expansionSource_ : this;
    out.insert(out.end(), summary.fieldWrites.begin(), summary.fieldWrites.end());
    for (const auto& binding : summary.callBindings) {
        if (!binding.fieldWritesActive) continue;
        ca::usize                 slot   = 0;
        const AliasMethodSummary* callee = resolveBinding(binding.callee, &slot);
        if (callee == nullptr) continue;
        std::vector<AliasMethodSummary::FieldWrite> inner;
        if (std::find(visiting.begin(), visiting.end(), slot) != visiting.end()) {
            // 环重入：只贡献 callee 自身事实（按本绑定实例化）。
            inner.assign(callee->fieldWrites.begin(), callee->fieldWrites.end());
        }
        else {
            visiting.push_back(slot);
            source->expandFieldWrites(*callee, visiting, inner);
            visiting.pop_back();
        }
        for (const auto& write : inner) {
            out.push_back({write.field,
                           instantiateFact(write.receiver, binding.fwArguments, binding.fwReceiver),
                           instantiateFact(write.value, binding.fwArguments, binding.fwReceiver),
                           write.isStatic});
        }
    }
}

void AliasAnalysis::expandArrayWrites(const AliasMethodSummary&                    summary,
                                      std::vector<ca::usize>&                      visiting,
                                      std::vector<AliasMethodSummary::ArrayWrite>& out) const
{
    const AliasAnalysis* source = expansionSource_ != nullptr ? expansionSource_ : this;
    out.insert(out.end(), summary.arrayWrites.begin(), summary.arrayWrites.end());
    for (const auto& binding : summary.callBindings) {
        if (!binding.arrayWritesActive) continue;
        ca::usize                 slot   = 0;
        const AliasMethodSummary* callee = resolveBinding(binding.callee, &slot);
        if (callee == nullptr) continue;
        std::vector<AliasMethodSummary::ArrayWrite> inner;
        if (std::find(visiting.begin(), visiting.end(), slot) != visiting.end()) {
            inner.assign(callee->arrayWrites.begin(), callee->arrayWrites.end());
        }
        else {
            visiting.push_back(slot);
            source->expandArrayWrites(*callee, visiting, inner);
            visiting.pop_back();
        }
        for (const auto& write : inner) {
            out.push_back({instantiateFact(write.array, binding.arguments, binding.receiver),
                           instantiateFact(write.index, binding.arguments, binding.receiver),
                           instantiateFact(write.value, binding.arguments, binding.receiver)});
        }
    }
}

void AliasAnalysis::expandContainerWrites(
    const AliasMethodSummary&                                    summary,
    std::vector<ca::usize>&                                      visiting,
    std::vector<AliasMethodSummary::ContainerWrite>&             out) const
{
    const AliasAnalysis* source = expansionSource_ != nullptr ? expansionSource_ : this;
    out.insert(out.end(), summary.containerWrites.begin(), summary.containerWrites.end());
    for (const auto& binding : summary.callBindings) {
        if (!binding.containerWritesActive) continue;
        ca::usize                 slot   = 0;
        const AliasMethodSummary* callee = resolveBinding(binding.callee, &slot);
        if (callee == nullptr) continue;
        std::vector<AliasMethodSummary::ContainerWrite> inner;
        if (std::find(visiting.begin(), visiting.end(), slot) != visiting.end()) {
            inner.assign(callee->containerWrites.begin(), callee->containerWrites.end());
        }
        else {
            visiting.push_back(slot);
            source->expandContainerWrites(*callee, visiting, inner);
            visiting.pop_back();
        }
        for (const auto& write : inner) {
            out.push_back({instantiateFact(write.container, binding.arguments, binding.receiver),
                           instantiateFact(write.key, binding.arguments, binding.receiver),
                           instantiateFact(write.value, binding.arguments, binding.receiver)});
        }
    }
}

AliasExpandedFacts AliasAnalysis::expandedFactsOf(std::string_view owner, std::string_view name,
                                                  std::string_view desc) const
{
    AliasExpandedFacts     facts;
    const AliasAnalysis*   domain  = expansionSource_ != nullptr ? expansionSource_ : this;
    const auto*            summary = domain->summaryOf(owner, name, desc);
    if (summary == nullptr) return facts;
    const auto startIt = domain->byKey_.find(aliasMemberKey(owner, name, desc));
    std::vector<ca::usize> visiting;
    if (startIt != domain->byKey_.end()) visiting.push_back(startIt->second);
    domain->expandFieldWrites(*summary, visiting, facts.fieldWrites);
    visiting.assign(visiting.begin(), visiting.begin() + 1);
    domain->expandArrayWrites(*summary, visiting, facts.arrayWrites);
    visiting.assign(visiting.begin(), visiting.begin() + 1);
    domain->expandContainerWrites(*summary, visiting, facts.containerWrites);
    return facts;
}

bool AliasAnalysis::slotStatesDiffer(const AliasAnalysis& other) const
{
    if (fields_.size() != other.fields_.size() ||
        arrayValues_.size() != other.arrayValues_.size() ||
        containerValues_.size() != other.containerValues_.size()) {
        return true;
    }
    for (const auto& [key, index] : fieldsByKey_) {
        const auto it = other.fieldsByKey_.find(key);
        if (it == other.fieldsByKey_.end()) return true;
        if (!sameAliasFact(fields_[index].value, other.fields_[it->second].value)) return true;
    }
    for (const auto& [key, value] : arrayValues_) {
        const auto it = other.arrayValues_.find(key);
        if (it == other.arrayValues_.end() || !sameAliasFact(value, it->second)) return true;
    }
    for (const auto& [key, value] : containerValues_) {
        const auto it = other.containerValues_.find(key);
        if (it == other.containerValues_.end() || !sameAliasFact(value, it->second)) return true;
    }
    return false;
}

void AliasAnalysis::buildFixedPoint(const std::vector<const AliasMethodContext*>& scope,
                                    ca::usize                                     roundBound)
{
    // Jacobi 全量重算：每轮所有方法重算（读上一轮快照），逐方法 sameSummary
    // + 轮末槽状态实比对账；轮数与旧栈 memoized 版恒等（旧 P1 文档不变量）。
    AliasAnalysis current;
    ca::usize     completedRounds = 0;
    for (ca::usize round = 0; round < roundBound + 1; ++round) {
        AliasAnalysis next;
        next.expansionSource_         = &current;
        next.suspendAndersenIndexing_ = true;
        bool anyChange = false;

        for (const AliasMethodContext* ctx : scope) {
            const auto summary = summarizeMethod(*ctx, &current);
            const auto* oldSummary =
                current.summaryOf(ctx->owner, ctx->name, ctx->desc);
            if (!summary.has_value()) {
                // 畸形方法本轮不产出事实；摘要从有到无按内容变更记账。
                if (oldSummary != nullptr) anyChange = true;
                continue;
            }
            if (oldSummary == nullptr || !sameSummary(*oldSummary, *summary)) {
                anyChange = true;
            }
            next.addSummary(std::move(*summary));
        }

        // 轮末槽对账：摘要未变但槽值漂移（callee 沉积变化）也计变更。
        if (next.slotStatesDiffer(current)) anyChange = true;

        completedRounds = round + 1;
        current         = std::move(next);
        current.expansionSource_         = nullptr;
        current.suspendAndersenIndexing_ = false;
        if (!anyChange) break;
    }
    *this                            = std::move(current);
    expansionSource_         = nullptr;
    suspendAndersenIndexing_ = false;
    convergenceRounds_       = completedRounds;
}

void AliasAnalysis::addSummary(AliasMethodSummary summary)
{
    const auto k    = summary.method;
    auto       it   = byKey_.find(k);
    ca::usize  index;
    if (it == byKey_.end()) {
        index      = summaries_.size();
        byKey_[k]  = index;
        summaries_.push_back(std::move(summary));
    }
    else {
        index             = it->second;
        summaries_[index] = std::move(summary);
    }

    // 索引与槽记账消费「展开后」的等价事实流（自身事实 + 逐绑定实例化的
    // callee 展开流），展开域 = expansionSource_（固定点期间=前一轮快照）。
    const AliasAnalysis* source = expansionSource_ != nullptr ? expansionSource_ : this;
    std::vector<ca::usize>                      visiting;
    const auto                                  selfSlotIt = source->byKey_.find(k);
    if (selfSlotIt != source->byKey_.end()) visiting.push_back(selfSlotIt->second);
    std::vector<AliasMethodSummary::FieldWrite> expandedFieldWrites;
    source->expandFieldWrites(summaries_[index], visiting, expandedFieldWrites);
    for (const auto& write : expandedFieldWrites) {
        std::vector<AliasValue> receivers;
        if (write.isStatic) {
            receivers.push_back(AliasValue::unknown());
        }
        else if (write.receiver.isKnown()) {
            receivers.push_back(write.receiver);
        }
        else {
            for (const auto identity : write.receiver.pointsTo) {
                receivers.push_back(AliasValue::object(identity));
            }
        }
        for (const auto& receiver : receivers) {
            if (!write.isStatic && isFormalIdentity(receiver)) continue;
            const SlotKey fieldName{receiver.identity, write.field};
            auto          fieldIt = fieldsByKey_.find(fieldName);
            if (fieldIt == fieldsByKey_.end()) {
                fieldsByKey_[fieldName] = fields_.size();
                fields_.push_back({write.field, receiver, write.value});
            }
            else {
                fields_[fieldIt->second].value =
                    mergeAliasValues(fields_[fieldIt->second].value, write.value);
            }
        }
    }
    std::vector<AliasMethodSummary::ArrayWrite> expandedArrayWrites;
    visiting.clear();
    if (selfSlotIt != source->byKey_.end()) visiting.push_back(selfSlotIt->second);
    source->expandArrayWrites(summaries_[index], visiting, expandedArrayWrites);
    for (const auto& write : expandedArrayWrites) {
        std::vector<AliasValue> arrays;
        if (write.array.isKnown()) {
            arrays.push_back(write.array);
        }
        else {
            for (const auto identity : write.array.pointsTo) {
                arrays.push_back(AliasValue::array(identity));
            }
        }
        for (const auto& array : arrays) {
            if (isFormalIdentity(array)) continue;
            const ElementSlotKey key{
                array.identity, write.index.isKnown() ? write.index.identity : std::string_view{}};
            auto it = arrayValues_.find(key);
            arrayValues_[key] =
                it == arrayValues_.end() ? write.value : mergeAliasValues(it->second, write.value);
        }
    }
    std::vector<AliasMethodSummary::ContainerWrite> expandedContainerWrites;
    visiting.clear();
    if (selfSlotIt != source->byKey_.end()) visiting.push_back(selfSlotIt->second);
    source->expandContainerWrites(summaries_[index], visiting, expandedContainerWrites);
    for (const auto& write : expandedContainerWrites) {
        if (!write.container.isKnown() || isFormalIdentity(write.container)) continue;
        const ElementSlotKey ckey{
            write.container.identity,
            write.key.isKnown() ? write.key.identity : std::string_view{}};
        auto it = containerValues_.find(ckey);
        if (it == containerValues_.end()) {
            containerValues_.emplace(ckey, write.value);
        }
        else {
            it->second = mergeAliasValues(it->second, write.value);
        }
    }
    if (!suspendAndersenIndexing_) indexAndersenSummary(summaries_[index]);
}

void AliasAnalysis::indexAndersenSummary(const AliasMethodSummary& summary)
{
    const std::string method = [&] {
        const KeyPool& pool = identityPool();
        return std::string(pool.str(summary.method.cls)) + "\n" +
               std::string(pool.str(summary.method.name)) + "\n" +
               std::string(pool.str(summary.method.desc));
    }();
    const auto valueNode = [&](const AliasValue& value) {
        const auto& identity = value.identity;
        if (identity.substr(0, 6) == "param:" || identity.substr(0, 5) == "this:") {
            return andersen_.intern(method + "\n" + std::string(identity));
        }
        return andersen_.intern(method + "\nvalue\n" + std::string(identity));
    };
    const auto constrainValue = [&](AndersenPointsToSolver::NameId target,
                                    const AliasValue&             value) {
        if (!value.isKnown() && value.pointsTo.empty()) return;
        const auto& identity = value.identity;
        if (identity.substr(0, 6) == "param:" || identity.substr(0, 5) == "this:") {
            andersen_.addCopy(target, andersen_.intern(method + "\n" + std::string(identity)));
        }
        if (value.isKnown() && identity.substr(0, 6) != "param:" &&
            identity.substr(0, 5) != "this:") {
            andersen_.addAddressOf(target, andersen_.intern(identity));
        }
        for (const auto point : value.pointsTo) {
            if (point.substr(0, 6) == "param:" || point.substr(0, 5) == "this:") continue;
            andersen_.addAddressOf(target, andersen_.intern(point));
        }
    };

    for (ca::usize index = 0; index < summary.parameters.size(); ++index) {
        constrainValue(
            andersen_.internIndexed(method + "\nparam-slot\n", static_cast<ca::i64>(index)),
            summary.parameters[index]);
    }
    const auto returnNode = andersen_.intern(method + "\nreturn");
    constrainValue(returnNode, summary.returnValue);
    for (const auto& write : summary.fieldWrites) {
        if (write.isStatic) continue;
        const auto receiver = valueNode(write.receiver);
        const auto value    = valueNode(write.value);
        constrainValue(receiver, write.receiver);
        constrainValue(value, write.value);
        const KeyPool& pool = identityPool();
        std::string fieldName;
        fieldName.assign(pool.str(write.field.cls)).append(1, '.')
            .append(pool.str(write.field.name)).append(1, ':')
            .append(pool.str(write.field.desc));
        andersen_.addStore(receiver, andersen_.intern(fieldName), value);
    }
    andersen_.solve();
}

const AliasValue* AliasAnalysis::fieldValueOf(std::string_view owner, std::string_view name,
                                              std::string_view desc) const
{
    // 静态槽 = unknown 接收者（身份空视图）。
    const auto it = fieldsByKey_.find(SlotKey{std::string_view{}, aliasMemberKey(owner, name, desc)});
    return it == fieldsByKey_.end() ? nullptr : &fields_[it->second].value;
}

const AliasValue* AliasAnalysis::fieldValueOf(const AliasValue& receiver, std::string_view owner,
                                              std::string_view name, std::string_view desc) const
{
    if (!receiver.isKnown()) return nullptr;
    const auto it = fieldsByKey_.find(SlotKey{receiver.identity, aliasMemberKey(owner, name, desc)});
    return it == fieldsByKey_.end() ? nullptr : &fields_[it->second].value;
}

std::optional<AliasValue> AliasAnalysis::resolvedFieldValueOf(const AliasValue& receiver,
                                                              std::string_view  owner,
                                                              std::string_view  name,
                                                              std::string_view  desc) const
{
    if (receiver.isKnown()) {
        const AliasValue* value = fieldValueOf(receiver, owner, name, desc);
        if (value != nullptr) return *value;
        return std::nullopt;
    }
    if (receiver.pointsTo.empty()) return std::nullopt;

    std::optional<AliasValue> result;
    bool                      missingTarget = false;
    for (const auto identity : receiver.pointsTo) {
        const AliasValue   target = AliasValue::object(identity);
        const AliasValue*  value  = fieldValueOf(target, owner, name, desc);
        if (value == nullptr) {
            missingTarget = true;
            continue;
        }
        result = result.has_value() ? mergeAliasValues(*result, *value)
                                    : std::optional<AliasValue>(*value);
    }
    if (!result.has_value()) return std::nullopt;
    if (missingTarget) *result = mergeAliasValues(*result, AliasValue::unknown());
    return result;
}

const AliasValue* AliasAnalysis::containerValueOf(std::string_view identity) const
{
    auto it = containerValues_.find({identity, std::string_view{}});
    return it == containerValues_.end() ? nullptr : &it->second;
}

const AliasValue* AliasAnalysis::containerValueOf(const AliasValue& container,
                                                  const AliasValue& key) const
{
    if (!container.isKnown()) return nullptr;
    auto it = containerValues_.find(
        {container.identity, key.isKnown() ? key.identity : std::string_view{}});
    if (it == containerValues_.end() && key.isKnown()) {
        it = containerValues_.find({container.identity, std::string_view{}});
    }
    return it == containerValues_.end() ? nullptr : &it->second;
}

const AliasValue* AliasAnalysis::arrayValueOf(const AliasValue& array, const AliasValue& index) const
{
    if (!array.isKnown()) return nullptr;
    auto it = arrayValues_.find(
        {array.identity, index.isKnown() ? index.identity : std::string_view{}});
    if (it == arrayValues_.end() && index.isKnown()) {
        it = arrayValues_.find({array.identity, std::string_view{}});
    }
    return it == arrayValues_.end() ? nullptr : &it->second;
}

std::optional<AliasValue> AliasAnalysis::resolvedArrayValueOf(const AliasValue& array,
                                                              const AliasValue& index) const
{
    if (array.isKnown()) {
        const AliasValue* value = arrayValueOf(array, index);
        if (value != nullptr) return *value;
        return std::nullopt;
    }
    if (array.pointsTo.empty()) return std::nullopt;

    std::optional<AliasValue> result;
    bool                      missingTarget = false;
    for (const auto identity : array.pointsTo) {
        const AliasValue*  value = arrayValueOf(AliasValue::array(identity), index);
        if (value == nullptr) {
            missingTarget = true;
            continue;
        }
        result = result.has_value() ? mergeAliasValues(*result, *value)
                                    : std::optional<AliasValue>(*value);
    }
    if (!result.has_value()) return std::nullopt;
    if (missingTarget) *result = mergeAliasValues(*result, AliasValue::unknown());
    return result;
}

std::vector<std::string> AliasAnalysis::possibleTypesOf(const AliasValue& value) const
{
    std::vector<std::string_view> identities = value.pointsTo;
    if (identities.empty() && value.isKnown()) identities.push_back(value.identity);
    std::vector<std::string> types;
    for (const auto identity : identities) {
        if (identity.substr(0, 4) != "new:") continue;
        const auto separator = identity.rfind(':');
        if (separator == std::string_view::npos || separator + 1 >= identity.size()) continue;
        types.emplace_back(identity.substr(separator + 1));
    }
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    return types;
}

std::vector<std::string> AliasAnalysis::possibleReturnTypesOf(std::string_view owner,
                                                              std::string_view name,
                                                              std::string_view desc) const
{
    const AliasMethodSummary* summary = summaryOf(owner, name, desc);
    return summary != nullptr ? possibleTypesOf(summary->returnValue) : std::vector<std::string>{};
}

std::vector<std::string> AliasAnalysis::andersenReturnPointsToOf(std::string_view owner,
                                                                 std::string_view name,
                                                                 std::string_view desc) const
{
    return andersen_.pointsTo(std::string(owner) + "\n" + std::string(name) + "\n" +
                              std::string(desc) + "\nreturn");
}

std::vector<std::string> AliasAnalysis::andersenParameterPointsToOf(std::string_view owner,
                                                                    std::string_view name,
                                                                    std::string_view desc,
                                                                    ca::usize parameter) const
{
    return andersen_.pointsTo(std::string(owner) + "\n" + std::string(name) + "\n" +
                              std::string(desc) + "\nparam:" + std::to_string(parameter));
}

const AliasMethodSummary* AliasAnalysis::summaryOf(std::string_view owner, std::string_view name,
                                                   std::string_view desc) const
{
    const auto it = byKey_.find(aliasMemberKey(owner, name, desc));
    return it == byKey_.end() ? nullptr : &summaries_[it->second];
}

const AliasValue* AliasAnalysis::reflectionReceiverOf(std::string_view owner,
                                                      std::string_view name,
                                                      std::string_view desc,
                                                      ca::i32          instructionIndex) const
{
    const AliasMethodSummary* summary = summaryOf(owner, name, desc);
    if (summary == nullptr) return nullptr;
    for (const auto& fact : summary->reflectionReceivers) {
        if (fact.instructionIndex == instructionIndex) return &fact.receiver;
    }
    return nullptr;
}

}   // namespace mjbase::analysis
