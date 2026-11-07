#include "mjbase/pass/method_inliner.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"

#include "mjbase/analysis/method_effect_analysis.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/frame_rewire.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using MemberKind = jbc::ir::ClassSession::MemberSem::Kind;
using ConstKind  = jbc::ir::ClassSession::ConstSem::Kind;

constexpr ca::u16 kAccPublic       = 0x0001;
constexpr ca::u16 kAccPrivate      = 0x0002;
constexpr ca::u16 kAccProtected    = 0x0004;
constexpr ca::u16 kAccStatic       = 0x0008;
constexpr ca::u16 kAccFinal        = 0x0010;
constexpr ca::u16 kAccNative       = 0x0100;
constexpr ca::u16 kAccAbstract     = 0x0400;
constexpr ca::u16 kAccInterface    = 0x0200;
constexpr ca::u16 kAccSynchronized = 0x0020;

constexpr ca::u32 kMaxLocalsLimit = 0xFFFFu;

// ── 描述符解析 ──

struct ParamType
{
    char   typeChar = 'V';
    ca::u8 slots    = 1;
};

/// 跳过一个描述符类型；畸形返回 npos（identity_wrapper_inliner 同款）。
ca::usize skipType(std::string_view desc, ca::usize i)
{
    if (i >= desc.size()) return std::string_view::npos;
    const char t = desc[i];
    if (t == 'L') {
        const ca::usize semi = desc.find(';', i);
        return semi == std::string_view::npos ? std::string_view::npos : semi + 1;
    }
    if (t == '[') {
        ca::usize j = i;
        while (j < desc.size() && desc[j] == '[') ++j;
        if (j >= desc.size()) return std::string_view::npos;
        if (desc[j] == 'L') {
            const ca::usize semi = desc.find(';', j);
            return semi == std::string_view::npos ? std::string_view::npos : semi + 1;
        }
        if (desc[j]=='I'||desc[j]=='S'||desc[j]=='B'||desc[j]=='C'||desc[j]=='Z'||
            desc[j]=='J'||desc[j]=='F'||desc[j]=='D') return j + 1;
        return std::string_view::npos;
    }
    if (t=='I'||t=='S'||t=='B'||t=='C'||t=='Z'||t=='J'||t=='F'||t=='D') return i + 1;
    return std::string_view::npos;
}

/// 解析方法描述符参数表（含类别字符与槽宽）；畸形返回 false。
bool parseParams(std::string_view desc, std::vector<ParamType>& params)
{
    params.clear();
    ca::usize i = 0;
    if (i >= desc.size() || desc[i] != '(') return false;
    ++i;
    while (i < desc.size() && desc[i] != ')') {
        const char t = desc[i];
        const ca::usize next = skipType(desc, i);
        if (next == std::string_view::npos) return false;
        params.push_back({t, static_cast<ca::u8>(t == 'J' || t == 'D' ? 2 : 1)});
        i = next;
    }
    if (i >= desc.size() || desc[i] != ')') return false;
    ++i;
    return skipType(desc, i) == desc.size();
}

/// 参数类别字符对应的 store 操作码。
Op storeOpOf(char typeChar)
{
    switch (typeChar) {
    case 'I': case 'S': case 'B': case 'C': case 'Z': return Op::ISTORE;
    case 'J': return Op::LSTORE;
    case 'F': return Op::FSTORE;
    case 'D': return Op::DSTORE;
    case 'L': case '[': return Op::ASTORE;
    default: return Op::NOP;
    }
}

// ── 局部变量操作码判定（quick 形态与泛形；wide 前缀已归一为 wide 标志）──

bool isGenericLoad(Op op)
{
    return op >= Op::ILOAD && op <= Op::ALOAD;
}
bool isQuickLoad(Op op)
{
    return op >= Op::ILOAD_0 && op <= Op::ALOAD_3;
}
bool isGenericStore(Op op)
{
    return op >= Op::ISTORE && op <= Op::ASTORE;
}
bool isQuickStore(Op op)
{
    return op >= Op::ISTORE_0 && op <= Op::ASTORE_3;
}
bool isLocalLoad(Op op)
{
    return isGenericLoad(op) || isQuickLoad(op);
}
bool isLocalStore(Op op)
{
    return isGenericStore(op) || isQuickStore(op);
}

/// 装载/存储操作码的槽位（quick 形态由操作码携带）。
ca::u32 localLoadSlotOf(Op op, ca::i32 a)
{
    if (isQuickLoad(op)) {
        return static_cast<ca::u32>(static_cast<ca::i32>(op) - static_cast<ca::i32>(Op::ILOAD_0));
    }
    return static_cast<ca::u32>(a);
}
ca::u32 localStoreSlotOf(Op op, ca::i32 a)
{
    if (isQuickStore(op)) {
        return static_cast<ca::u32>(static_cast<ca::i32>(op) - static_cast<ca::i32>(Op::ISTORE_0));
    }
    return static_cast<ca::u32>(a);
}

/// quick 形态映射回泛形操作码（槽位进 a；源编码宽度语义见 wide 保留规则）。
Op genericLoadOf(Op quick)
{
    if (quick <= Op::ILOAD_3) return Op::ILOAD;
    if (quick <= Op::LLOAD_3) return Op::LLOAD;
    if (quick <= Op::FLOAD_3) return Op::FLOAD;
    if (quick <= Op::DLOAD_3) return Op::DLOAD;
    return Op::ALOAD;
}
Op genericStoreOf(Op quick)
{
    if (quick <= Op::ISTORE_3) return Op::ISTORE;
    if (quick <= Op::LSTORE_3) return Op::LSTORE;
    if (quick <= Op::FSTORE_3) return Op::FSTORE;
    if (quick <= Op::DSTORE_3) return Op::DSTORE;
    return Op::ASTORE;
}

/// cat2 局部操作（L/D 族装载与存储，占双槽）。
bool isCat2LocalOp(Op op)
{
    switch (op) {
    case Op::LLOAD: case Op::DLOAD: case Op::LLOAD_0: case Op::LLOAD_1:
    case Op::LLOAD_2: case Op::LLOAD_3: case Op::DLOAD_0: case Op::DLOAD_1:
    case Op::DLOAD_2: case Op::DLOAD_3: case Op::LSTORE: case Op::DSTORE:
    case Op::LSTORE_0: case Op::LSTORE_1: case Op::LSTORE_2: case Op::LSTORE_3:
    case Op::DSTORE_0: case Op::DSTORE_1: case Op::DSTORE_2: case Op::DSTORE_3:
        return true;
    default: return false;
    }
}

bool isSwitchOp(Op op)
{
    return op == Op::TABLESWITCH || op == Op::LOOKUPSWITCH;
}

bool isBranchOp(Op op)
{
    switch (op) {
    case Op::GOTO: case Op::GOTO_W: case Op::IFEQ: case Op::IFNE: case Op::IFLT:
    case Op::IFGE: case Op::IFGT: case Op::IFLE: case Op::IF_ICMPEQ:
    case Op::IF_ICMPNE: case Op::IF_ICMPLT: case Op::IF_ICMPGE: case Op::IF_ICMPGT:
    case Op::IF_ICMPLE: case Op::IF_ACMPEQ: case Op::IF_ACMPNE: case Op::IFNULL:
    case Op::IFNONNULL:
        return true;
    default: return false;
    }
}

bool isReturnOp(Op op)
{
    return op >= Op::IRETURN && op <= Op::RETURN;
}

bool isInvokeOp(Op op)
{
    return op == Op::INVOKEVIRTUAL || op == Op::INVOKESPECIAL ||
           op == Op::INVOKESTATIC || op == Op::INVOKEINTERFACE ||
           op == Op::INVOKEDYNAMIC;
}

bool isFieldOp(Op op)
{
    return op == Op::GETSTATIC || op == Op::PUTSTATIC || op == Op::GETFIELD ||
           op == Op::PUTFIELD;
}

bool isClassOp(Op op)
{
    return op == Op::NEW || op == Op::ANEWARRAY || op == Op::CHECKCAST ||
           op == Op::INSTANCEOF || op == Op::MULTIANEWARRAY;
}

// ── 快照结构（被调体冻结副本；跨会话物化按语义 intern，不携带源池索引）──

struct CalleeInsn
{
    Op   op   = Op::NOP;
    bool wide = false;
    ca::i32 a = 0;
    ca::i32 b = 0;

    enum class Payload : ca::u8 {
        None, Member, Cls, ConstInt, ConstLong, ConstFloat, ConstDouble,
        ConstString, ConstClass,
    };
    Payload    payload    = Payload::None;
    MemberKind memberKind = MemberKind::Method;
    std::string memberOwner;
    std::string memberName;
    std::string memberDesc;
    std::string className;
    ca::i64     num  = 0;
    ca::u64     bits = 0;
    std::string str;

    ca::u32 targetPos = 0;   ///< 分支目标（switch = default 目标）在快照体中的序位
    /// switch 逐 case 目标序位（与 dom targets 下标对齐；tableswitch 键由
    /// low+下标派生，lookupswitch 键原样随迁）。
    std::vector<ca::u32> switchTargetPos;
    std::vector<ca::i32> switchKeys;   ///< 仅 lookupswitch：键表原样随迁
    ca::u32 localSlot = 0;   ///< var/iinc 原槽位
    bool    cat2      = false;
};

struct CalleeBody
{
    std::vector<CalleeInsn>  insns;
    /// 参数/接收者落槽布局：(类别字符, 被调槽位)；非 static 首项 = receiver。
    std::vector<std::pair<char, ca::u32>> paramLayout;
    std::vector<std::string> bodyCalls;   ///< 体直调目标 key（递归判定消费）
    bool    isStatic       = false;
    bool    isPrivate      = false;
    bool    protectedMethod = false;
    bool    publicOwner    = false;
    bool    publicMethod   = false;
    bool    methodFinal    = false;   ///< 声明方法 ACC_FINAL（去虚化证据位）
    bool    ownerFinal     = false;   ///< 声明类 ACC_FINAL（去虚化证据位）
    bool    ownerHasClinit = false;
    ca::u32 paramSlots   = 0;   ///< 含 receiver（非 static 槽 0 = this）
    ca::u32 localsNeeded = 0;   ///< max(paramSlots, 体用槽上界)
};

std::string methodKey(std::string_view owner, std::string_view name, std::string_view desc)
{
    std::string k;
    k.reserve(owner.size() + name.size() + desc.size() + 2);
    k.append(owner);
    k.push_back('\n');
    k.append(name);
    k.push_back('\n');
    k.append(desc);
    return k;
}

/// 被调体快照捕获：结构门（异常表/jsr/虚调用/indy/末边界/预算/回边帽）
/// 任一不过返回 nullopt；成员/类/常量载荷按语义视图读取。
std::optional<CalleeBody> captureCalleeBody(jbc::ir::ClassSession& session, ca::usize methodIdx)
{
    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) return std::nullopt;
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr || !cu->exc.empty()) return std::nullopt;

    const jbc::ir::ClassUnit&  unit = session.unit();
    const jbc::ir::MethodUnit& mu   = unit.methods[methodIdx];
    const std::string_view     desc = unit.symbols().str(mu.desc);

    std::vector<ParamType> params;
    if (!parseParams(desc, params)) return std::nullopt;

    const std::vector<jbc::ir::InsnId>& order = cu->dom.order();
    if (order.empty() || order.size() > kMaxInlineCalleeInsns) return std::nullopt;

    std::unordered_map<ca::u32, ca::u32> posOf;
    posOf.reserve(order.size());
    for (ca::usize i = 0; i < order.size(); ++i) {
        posOf.emplace(order[i].slot, static_cast<ca::u32>(i));
    }

    CalleeBody body;
    body.isStatic        = (mu.access & kAccStatic) != 0;
    body.isPrivate       = (mu.access & kAccPrivate) != 0;
    body.protectedMethod = (mu.access & kAccProtected) != 0;
    body.publicMethod    = (mu.access & kAccPublic) != 0;
    body.methodFinal     = (mu.access & kAccFinal) != 0;

    ca::u32 slotCursor = body.isStatic ? 0u : 1u;
    if (!body.isStatic) body.paramLayout.emplace_back('L', 0u);   // receiver
    for (const ParamType& p : params) {
        body.paramLayout.emplace_back(p.typeChar, slotCursor);
        slotCursor += p.slots;
    }
    body.paramSlots = slotCursor;

    ca::u32 maxSlotEnd  = 0;
    bool    hasLocalUse = false;
    bool    hasReturn   = false;
    ca::u32 backEdges   = 0;   // v2：回边帽（分支与 switch 目标同帽计入）
    for (ca::usize i = 0; i < order.size(); ++i) {
        const jbc::ir::Insn* in = cu->dom.peek(order[i]);
        if (in == nullptr) return std::nullopt;

        CalleeInsn snap;
        snap.op   = in->op;
        snap.wide = in->wide;
        snap.a    = in->a;
        snap.b    = in->b;
        if (isReturnOp(in->op)) hasReturn = true;

        if (in->op == Op::JSR || in->op == Op::JSR_W || in->op == Op::RET) {
            return std::nullopt;
        }
        if (in->op == Op::INVOKEDYNAMIC) return std::nullopt;      // NotModeled 口径
        if (in->op == Op::INVOKEVIRTUAL || in->op == Op::INVOKEINTERFACE) {
            return std::nullopt;                                    // 体含虚调用：纯度不可证
        }

        if (isSwitchOp(in->op)) {
            // v2：switch 随体——default 与逐 case 目标解析（末边界拒收、
            // 回边计帽）；键表 lookupswitch 原样随迁。
            const auto resolvedDefault = cu->dom.resolveLabel(in->target);
            if (resolvedDefault.is_err()) return std::nullopt;
            const jbc::ir::LabelTarget defaultTarget = std::move(resolvedDefault).unwrap();
            if (defaultTarget.isEnd()) return std::nullopt;         // 末边界目标拒收
            const auto defIt = posOf.find(defaultTarget.insn.slot);
            if (defIt == posOf.end()) return std::nullopt;
            if (defIt->second <= i && ++backEdges > kMaxInlineCalleeBackEdges) {
                return std::nullopt;
            }
            snap.targetPos = defIt->second;
            for (const jbc::ir::LabelId& caseLabel : in->targets) {
                const auto resolvedCase = cu->dom.resolveLabel(caseLabel);
                if (resolvedCase.is_err()) return std::nullopt;
                const jbc::ir::LabelTarget caseTarget = std::move(resolvedCase).unwrap();
                if (caseTarget.isEnd()) return std::nullopt;
                const auto caseIt = posOf.find(caseTarget.insn.slot);
                if (caseIt == posOf.end()) return std::nullopt;
                if (caseIt->second <= i && ++backEdges > kMaxInlineCalleeBackEdges) {
                    return std::nullopt;
                }
                snap.switchTargetPos.push_back(caseIt->second);
            }
            if (in->op == Op::LOOKUPSWITCH) snap.switchKeys = in->keys;
        } else if (isInvokeOp(in->op) || isFieldOp(in->op)) {
            jbc::ir::ClassSession::MemberSem sem;
            if (!session.memberOf(*in, sem)) return std::nullopt;
            snap.payload    = CalleeInsn::Payload::Member;
            snap.memberKind = sem.kind;
            snap.memberOwner.assign(sem.owner);
            snap.memberName.assign(sem.name);
            snap.memberDesc.assign(sem.desc);
            if (isInvokeOp(in->op)) {
                body.bodyCalls.push_back(methodKey(sem.owner, sem.name, sem.desc));
            }
        } else if (isClassOp(in->op)) {
            const std::string_view cls = session.classOf(*in);
            if (cls.empty()) return std::nullopt;
            snap.payload  = CalleeInsn::Payload::Cls;
            snap.className.assign(cls);
        } else if (in->op == Op::LDC || in->op == Op::LDC_W || in->op == Op::LDC2_W) {
            jbc::ir::ClassSession::ConstSem sem;
            if (!session.constOf(*in, sem)) return std::nullopt;
            switch (sem.kind) {
            case ConstKind::Int:    snap.payload = CalleeInsn::Payload::ConstInt; break;
            case ConstKind::Long:   snap.payload = CalleeInsn::Payload::ConstLong; break;
            case ConstKind::Float:  snap.payload = CalleeInsn::Payload::ConstFloat; break;
            case ConstKind::Double: snap.payload = CalleeInsn::Payload::ConstDouble; break;
            case ConstKind::String: snap.payload = CalleeInsn::Payload::ConstString; break;
            case ConstKind::Class:  snap.payload = CalleeInsn::Payload::ConstClass; break;
            case ConstKind::MethodType:
            case ConstKind::MethodHandle:
            case ConstKind::Dynamic: return std::nullopt;          // NotModeled 口径
            }
            snap.num  = sem.num;
            snap.bits = sem.bits;
            snap.str.assign(sem.str);
        } else if (isLocalLoad(in->op) || isLocalStore(in->op)) {
            const ca::u32 slot = isLocalLoad(in->op) ? localLoadSlotOf(in->op, in->a)
                                                     : localStoreSlotOf(in->op, in->a);
            snap.localSlot = slot;
            snap.cat2      = isCat2LocalOp(in->op);
            maxSlotEnd     = std::max(maxSlotEnd, slot + (snap.cat2 ? 1u : 0u));
            hasLocalUse    = true;
        } else if (in->op == Op::IINC) {
            snap.localSlot = static_cast<ca::u32>(in->a);
            maxSlotEnd     = std::max(maxSlotEnd, snap.localSlot);
            hasLocalUse    = true;
        } else if (isBranchOp(in->op)) {
            const auto resolved = cu->dom.resolveLabel(in->target);
            if (resolved.is_err()) return std::nullopt;
            const jbc::ir::LabelTarget target = std::move(resolved).unwrap();
            if (target.isEnd()) return std::nullopt;               // 末边界目标拒收
            const auto it = posOf.find(target.insn.slot);
            if (it == posOf.end()) return std::nullopt;
            if (it->second <= i && ++backEdges > kMaxInlineCalleeBackEdges) {
                return std::nullopt;                               // 回边超帽拒收（v2 浅循环帽）
            }
            snap.targetPos = it->second;
        }
        // 其余 op（常量/算术/栈操作等）载荷 None，op/wide/a/b 原样随迁。
        body.insns.push_back(std::move(snap));
    }

    if (!hasReturn) return std::nullopt;   // 无返回指令的体不产值，拼接必栈形失衡
    body.localsNeeded = hasLocalUse ? std::max(body.paramSlots, maxSlotEnd + 1)
                                    : body.paramSlots;
    return body;
}

/// 跨类体安全面：被搬到 caller 后成员访问检查改用 caller 上下文，只放行
/// 不依赖被调类访问上下文的指令（旧栈 isCrossClassBodySafe 同口径）。
bool bodyCrossClassSafe(const CalleeBody& body)
{
    for (const CalleeInsn& snap : body.insns) {
        if (snap.payload == CalleeInsn::Payload::Member ||
            snap.payload == CalleeInsn::Payload::Cls ||
            snap.payload == CalleeInsn::Payload::ConstClass) {
            return false;
        }
    }
    return true;
}

// ── 已装载类与候选表 ──

struct LoadedClass
{
    std::string                            name;
    std::string                            uri;
    std::shared_ptr<jbc::ir::ClassSession> session;
};

/// 阶段一：装载 + 事实面（候选快照 / 直调边 / 递归拒绝 / 纯度）。
std::vector<LoadedClass> buildCandidates(
    mjbase::model::ResourceContainer& container,
    std::unordered_map<std::string, CalleeBody>& candidates)
{
    std::vector<LoadedClass> loaded;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr || cls->get().empty()) continue;
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (sessionResult.is_err()) continue;   // 不可解析类跳过（保守不写回）
        LoadedClass entry;
        entry.session = std::move(sessionResult).unwrap();
        entry.name.assign(entry.session->backing().thisName());
        entry.uri.assign(cls->getLocation().to_std_string());
        if (entry.name.empty()) continue;
        loaded.push_back(std::move(entry));
    }
    // 容器迭代序跨平台漂移：统一按内部名排序固定处理序（输出逐位稳定）。
    std::sort(loaded.begin(), loaded.end(),
              [](const LoadedClass& a, const LoadedClass& b) { return a.name < b.name; });

    // 纯度模型：MethodEffectModel 全容器重建（局部分类 + 调用边不动点）。
    mjbase::analysis::MethodEffectModel effects;
    effects.build(container);

    // 全量直调边（递归可达性判定消费）。
    std::unordered_map<std::string, std::vector<std::string>> callEdges;
    for (const LoadedClass& entry : loaded) {
        jbc::ir::ClassSession&    session = *entry.session;
        const jbc::ir::ClassUnit& unit    = session.unit();
        const auto visits = jbc::ir::collectCodeUnits(session);
        for (const auto& visit : visits) {
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok || visit.unit.get() == nullptr) {
                continue;
            }
            const jbc::ir::CodeUnit*   cu  = visit.unit.get();
            const jbc::ir::MethodUnit& mu  = unit.methods[visit.methodIdx];
            const std::string          key = methodKey(entry.name,
                                                          unit.symbols().str(mu.name),
                                                          unit.symbols().str(mu.desc));
            std::vector<std::string>&  edges = callEdges[key];
            for (const jbc::ir::InsnId id : cu->dom.order()) {
                const jbc::ir::Insn* in = cu->dom.peek(id);
                if (in == nullptr || !isInvokeOp(in->op) || in->op == Op::INVOKEDYNAMIC) continue;
                jbc::ir::ClassSession::MemberSem sem;
                if (!session.memberOf(*in, sem)) continue;
                edges.push_back(methodKey(sem.owner, sem.name, sem.desc));
            }
        }
    }

    // 递归可达性：从候选体直调目标沿直调边 DFS，可达回候选即拒收
    // （直接自调是自环特例）。拼接只删调用/复制既有调用，不改边闭包，
    // 阶段一判定在整轮内充分。
    const auto reachesSelf = [&callEdges](const std::string&              self,
                                          const std::vector<std::string>& directCalls) {
        std::unordered_set<std::string> visited;
        std::vector<std::string>        work(directCalls);
        while (!work.empty()) {
            const std::string cur = work.back();
            work.pop_back();
            if (cur == self) return true;
            if (!visited.insert(cur).second) continue;
            const auto it = callEdges.find(cur);
            if (it == callEdges.end()) continue;
            for (const std::string& next : it->second) work.push_back(next);
        }
        return false;
    };

    for (const LoadedClass& entry : loaded) {
        jbc::ir::ClassSession&    session = *entry.session;
        const jbc::ir::ClassUnit& unit    = session.unit();
        const std::string_view    self    = session.backing().thisName();
        const bool ownerPublic    = (unit.access & kAccPublic) != 0;
        const bool ownerInterface = (unit.access & kAccInterface) != 0;
        const bool ownerFinal     = (unit.access & kAccFinal) != 0;
        bool       hasClinit      = false;
        for (const jbc::ir::MethodUnit& mu : unit.methods) {
            if (unit.symbols().str(mu.name) == "<clinit>") hasClinit = true;
        }

        for (ca::usize i = 0; i < unit.methods.size(); ++i) {
            const jbc::ir::MethodUnit& mu   = unit.methods[i];
            const std::string_view     name = unit.symbols().str(mu.name);
            const std::string_view     desc = unit.symbols().str(mu.desc);

            if (!name.empty() && name.front() == '<') continue;
            if ((mu.access & (kAccAbstract | kAccNative | kAccSynchronized)) != 0) continue;
            const bool isStatic  = (mu.access & kAccStatic) != 0;
            const bool isPrivate = (mu.access & kAccPrivate) != 0;
            // v2 候选扩面：virtual 面 = final 方法 / final 类的虚方法（唯一
            // 实现可证）；接口面只收 private（javac 对 private 接口方法发
            // invokeinterface/invokestatic 直调，分派不参与）。
            const bool virtualDevirt =
                !isStatic && !isPrivate &&
                ((mu.access & kAccFinal) != 0 || ownerFinal);
            if (!(isStatic || isPrivate || virtualDevirt)) continue;
            if (ownerInterface && !isPrivate) continue;
            if (!jbc::ir::hasCode(session, i)) continue;

            const std::string key = methodKey(self, name, desc);
            // 纯度门：Pure 才内联（ReadOnly/Unknown/Impure 一律拒收）。
            if (!effects.isPure(self, name, desc)) continue;

            auto body = captureCalleeBody(session, i);
            if (!body.has_value()) continue;
            // 递归门：直接自调与间接可达回自身。
            if (reachesSelf(key, body->bodyCalls)) continue;
            // 体自身帧可算门：栈形失衡/局部越界的畸形体在候选层拒收（调用点
            // 原样保留），不升级为 caller 整类放弃。BestEffort 形状域无需层次
            // 解析器（克隆体白名单 op 的形状检查即充分；带环/switch 形态由
            // 域引擎 worklist 数据流覆盖）。
            {
                auto promotedChk = jbc::ir::promoteMethod(session, i);
                if (promotedChk.is_err()) continue;
                jbc::ir::CodeUnit* chk = promotedChk.unwrap().get();
                if (chk == nullptr) continue;
                jbc::analysis::MethodSig sig;
                sig.isStatic   = (mu.access & kAccStatic) != 0;
                sig.name       = name;
                sig.desc       = desc;
                sig.thisClass  = self;
                jbc::analysis::FrameEngine shapeEngine(session.backing().cp);
                std::vector<jbc::analysis::FrameAt> chkFrames;
                if (!shapeEngine.compute(*chk, sig, chkFrames).ok) continue;
            }

            body->publicOwner    = ownerPublic;
            body->ownerFinal     = ownerFinal;
            body->ownerHasClinit = hasClinit;
            candidates.emplace(key, std::move(*body));
        }
    }
    return loaded;
}

// ── v2 判定事实：去虚化层次图 + nest 属性读取 ──

///  Rewrite 循环共享的判定事实（run 层构建，非持有引用）。
struct InlinerFacts
{
    const mjbase::hierarchy::ClassHierarchy* hierarchy = nullptr;  ///< 容器全量登记图
    std::unordered_map<std::string, jbc::ir::ClassSession*>* sessions = nullptr;
    const mjbase::KeyPool* pool = nullptr;   ///< 层次图绑定池（摘要成员名/描述符解析）
};

/// CP Class 条目 → 内部名（原始 CP 视图；与 thisName 同表征，判定层逐字节
/// 比较自洽）；非 Class/越界返回空。
std::string classInternalNameOf(const jbc::core::ClassFile& cf, ca::u32 classCp)
{
    if (!cf.cp.valid(classCp) || cf.cp.tagOf(classCp) != jbc::core::CpTag::Class) return {};
    return std::string(cf.cp.utf8Of(cf.cp.at(classCp).a));
}

/// NestHost 属性目标（无属性/畸形返回 nullopt——调用方按「宿主 = 自身」
/// 或 fail-closed 收口，见 sameNestProof）。
std::optional<std::string> nestHostOf(jbc::ir::ClassSession& session)
{
    jbc::core::ClassFile& cf = session.backing();
    for (const jbc::core::AttrSlot& slot : cf.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "NestHost")) continue;
        auto d = jbc::core::decodeNestHostBody(cf, slot);
        if (d.is_err()) return std::nullopt;
        // 所有权先落 local 再 cast：链式 *std::move(d).unwrap() 的临时
        // unique_ptr 在声明语句末析构，引用悬垂（UB 读 freed 内存）。
        auto body = std::move(d).unwrap();
        auto& nh  = static_cast<jbc::core::NestHostAttr&>(*body);
        return classInternalNameOf(cf, nh.hostClassCp);
    }
    return std::nullopt;
}

/// NestMembers 属性成员名表（无属性返回 nullopt；畸形条目跳过——列表语义
/// 只做「包含 claimant」判定，缺名即 fail-closed）。
std::optional<std::vector<std::string>> nestMembersOf(jbc::ir::ClassSession& session)
{
    jbc::core::ClassFile& cf = session.backing();
    for (const jbc::core::AttrSlot& slot : cf.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "NestMembers")) continue;
        auto d = jbc::core::decodeNestMembersBody(cf, slot);
        if (d.is_err()) return std::nullopt;
        auto body = std::move(d).unwrap();
        auto& nm  = static_cast<jbc::core::NestMembersAttr&>(*body);
        std::vector<std::string> names;
        names.reserve(nm.memberCps.size());
        for (const ca::u32 classCp : nm.memberCps) {
            std::string name = classInternalNameOf(cf, classCp);
            if (!name.empty()) names.push_back(std::move(name));
        }
        return names;
    }
    return std::nullopt;
}

/// nestmate 判据（JVMS 4.7.29/5.4.4 口径）：双方 nest 宿主同名，且宿主在
/// 容器内、其 NestMembers 逐名列出双方（宿主自身豁免）。镜像 HotSpot 的
/// 懒验证——claimant 必须被宿主成员表收录，属性缺失/不列名一律拒绝。
bool sameNestProof(const InlinerFacts& facts, std::string_view caller,
                   std::string_view declaree)
{
    if (caller == declaree) return true;
    const auto itA = facts.sessions->find(std::string(caller));
    const auto itB = facts.sessions->find(std::string(declaree));
    if (itA == facts.sessions->end() || itB == facts.sessions->end()) return false;
    const auto hostA = nestHostOf(*itA->second);
    const auto hostB = nestHostOf(*itB->second);
    const std::string hostNameA = hostA.has_value() ? *hostA : std::string(caller);
    const std::string hostNameB = hostB.has_value() ? *hostB : std::string(declaree);
    if (hostNameA != hostNameB) return false;

    const auto hostIt = facts.sessions->find(hostNameA);
    if (hostIt == facts.sessions->end()) return false;   // 宿主不在容器：准入不可验证
    const auto members = nestMembersOf(*hostIt->second);
    if (!members.has_value()) return false;              // 无成员表：准入不可验证
    const auto lists = [&](std::string_view cls) {
        if (cls == hostNameA) return true;
        const std::string key(cls);
        return std::find(members->begin(), members->end(), key) != members->end();
    };
    return lists(caller) && lists(declaree);
}

/// protected 访问面：caller ⊑ declaree（容器内 super 链逐段可证；链中断/
/// 未登记即拒）。JVM protected 规则 = 同运行时包（字节不可证）或子类
/// 关系（字节可证）。
bool subclassProof(const InlinerFacts& facts, std::string_view caller,
                   std::string_view declaree)
{
    if (caller == declaree) return true;
    const auto sub = facts.hierarchy->findClass(caller);
    const auto sup = facts.hierarchy->findClass(declaree);
    if (!sub.valid() || !sup.valid()) return false;
    return facts.hierarchy->isSubclass(sub, sup);
}

/// 去虚化解析产物。
struct DevirtResolution
{
    enum class Kind : ca::u8
    {
        None,          ///< 唯一实现不可证（含解析中断/歧义/ICCE 形态）
        PrivateDecl,   ///< 声明 private：不参与虚分派，直调
        FinalMethod,   ///< 声明 ACC_FINAL：任何子类不可覆盖
        FinalClass,    ///< 声明类 ACC_FINAL：接收者运行时类型恰为该类
    };
    Kind        kind  = Kind::None;
    std::string owner;   ///< 唯一目标声明类（Kind != None 时有效）
};

/// invokevirtual / invokeinterface 去虚化解析：沿符号 owner 的容器内链
/// （类 = super 链最近优先；接口 = 自身 + 接口闭包）找 name+desc 声明者。
/// 链中断（悬挂 super）、类链无声明（default 面不裁决）、接口闭包多声明
/// （最大特定歧义）、static/abstract/`<init>` 命中一律 None。
DevirtResolution resolveDevirtualization(const InlinerFacts& facts,
                                         const jbc::ir::ClassSession::MemberSem& enc)
{
    DevirtResolution out;
    const mjbase::hierarchy::ClassHierarchy& h    = *facts.hierarchy;
    const mjbase::KeyPool&                   pool = *facts.pool;
    const auto startKey = h.findClass(enc.owner);
    if (!startKey.valid()) return out;   // 符号 owner 未登记：解析不可证
    const auto* startSummary = h.summary(startKey);
    if (startSummary == nullptr) return out;

    std::vector<mjbase::ClassKey> order;
    const bool ownerIsInterface = (startSummary->access & kAccInterface) != 0;
    if (ownerIsInterface) {
        order.push_back(startKey);
        const auto closure = h.interfaceClosure(startKey);
        order.insert(order.end(), closure.begin(), closure.end());
    } else {
        order = h.ancestors(startKey);   // super 链全序，最近优先 = 解析序
    }

    // 首个声明者 + 全链命中计数。类链：JVM 方法解析沿 super 链自近及远首
    // 命中即胜（JVMS 5.4.5），悬挂键（容器外类）只是链的可视边界——截断
    // 而非拒收：命中已收集时近者胜不受链外影响，未命中时解析本不可证。
    // 接口闭包：最大特定歧义判定需要闭包完整（未登记分支可能藏着第二
    // 声明者），悬挂即 fail-closed。
    mjbase::ClassKey firstDeclarer{};
    ca::u32 matchCount = 0;
    for (const auto& key : order) {
        const auto* sum = h.summary(key);
        if (sum == nullptr) {
            if (ownerIsInterface) return out;   // 闭包不完整：歧义不可排除
            break;                              // 类链：悬挂截断，命中即近者胜
        }
        bool declared = false;
        for (const mjbase::hierarchy::MemberSummary& m : sum->methods) {
            if (pool.str(m.name) == enc.name && pool.str(m.desc) == enc.desc) {
                declared = true;
                break;
            }
        }
        if (declared) {
            if (matchCount == 0) firstDeclarer = key;
            ++matchCount;
        }
    }
    if (firstDeclarer.id == 0) return out;
    if (ownerIsInterface && matchCount != 1) return out;

    const auto* declarerSummary = h.summary(firstDeclarer);
    if (declarerSummary == nullptr) return out;
    const mjbase::hierarchy::MemberSummary* decl = nullptr;
    for (const mjbase::hierarchy::MemberSummary& m : declarerSummary->methods) {
        if (pool.str(m.name) == enc.name && pool.str(m.desc) == enc.desc) {
            decl = &m;
            break;
        }
    }
    if (decl == nullptr) return out;
    if (decl->isStatic) return out;                     // static 命中 = ICCE 形态
    if ((decl->access & kAccAbstract) != 0) return out; // abstract 无体可内联
    const std::string_view declName = pool.str(decl->name);
    if (!declName.empty() && declName.front() == '<') return out;

    out.owner.assign(h.nameOf(firstDeclarer));
    if ((decl->access & kAccPrivate) != 0) {
        out.kind = DevirtResolution::Kind::PrivateDecl;
    } else if ((decl->access & kAccFinal) != 0) {
        out.kind = DevirtResolution::Kind::FinalMethod;
    } else if ((declarerSummary->access & kAccFinal) != 0) {
        out.kind = DevirtResolution::Kind::FinalClass;
    }
    if (out.kind == DevirtResolution::Kind::None) out.owner.clear();
    return out;
}

// ── 拼接 ──

/// 单条快照指令物化进 caller dom（callId 之前）。返回新指令句柄；
/// 失败返回 kNoInsn（类级放弃）。分支/switch 目标由 spliceSite 统一延后补锚。
jbc::ir::InsnId materializeInsn(jbc::ir::CodeDom& dom, jbc::ir::ClassSession& session,
                                const CalleeInsn& snap, ca::u32 localBase,
                                jbc::ir::LabelId contLabel, jbc::ir::InsnId callId)
{
    jbc::ir::Insn out;
    out.op   = snap.op;
    out.wide = snap.wide;
    out.a    = snap.a;
    out.b    = snap.b;

    switch (snap.payload) {
    case CalleeInsn::Payload::None:
        break;
    case CalleeInsn::Payload::Member: {
        if (!session.setMember(out, snap.memberKind, snap.memberOwner, snap.memberName,
                               snap.memberDesc)) {
            return jbc::ir::kNoInsn;
        }
        break;
    }
    case CalleeInsn::Payload::Cls: {
        if (!session.setClass(out, snap.className)) return jbc::ir::kNoInsn;
        break;
    }
    case CalleeInsn::Payload::ConstInt: {
        out.cp = session.internInteger(static_cast<ca::i32>(snap.num));
        if (out.cp > 0xFF && out.op == Op::LDC) out.op = Op::LDC_W;
        break;
    }
    case CalleeInsn::Payload::ConstLong: {
        out.cp = session.internLong(snap.num);
        out.op = Op::LDC2_W;
        break;
    }
    case CalleeInsn::Payload::ConstFloat: {
        out.cp = session.internFloatBits(static_cast<ca::u32>(snap.bits));
        if (out.cp > 0xFF && out.op == Op::LDC) out.op = Op::LDC_W;
        break;
    }
    case CalleeInsn::Payload::ConstDouble: {
        out.cp = session.internDoubleBits(snap.bits);
        out.op = Op::LDC2_W;
        break;
    }
    case CalleeInsn::Payload::ConstString: {
        if (!session.setString(out, snap.str)) return jbc::ir::kNoInsn;
        break;
    }
    case CalleeInsn::Payload::ConstClass: {
        out.cp = session.internClass(snap.className);
        if (out.cp > 0xFF && out.op == Op::LDC) out.op = Op::LDC_W;
        break;
    }
    }

    if (isReturnOp(out.op)) {
        // 返回值已留调用点栈上：出口汇合到调用点后继（统一出口）。
        out.op     = Op::GOTO;
        out.wide   = false;
        out.target = contLabel;
    } else if (isSwitchOp(out.op)) {
        // switch 多目标延后补锚（default 与逐 case；键表随 lookupswitch 原样）。
        out.target  = jbc::ir::kNoLabel;
        out.keys    = snap.switchKeys;
        out.targets.assign(snap.switchTargetPos.size(), jbc::ir::kNoLabel);
    } else if (isBranchOp(out.op)) {
        out.target = jbc::ir::kNoLabel;   // 延后补锚（受控延后绑定态）
    } else if (isLocalLoad(out.op) || isLocalStore(out.op)) {
        if (isQuickLoad(out.op)) out.op = genericLoadOf(out.op);
        if (isQuickStore(out.op)) out.op = genericStoreOf(out.op);
        const ca::u32 slot = localBase + snap.localSlot;
        out.a    = static_cast<ca::i32>(slot);
        // 源 wide 编码保真（iinc 宽常量语义依赖 wide 位），槽位越 u1 域升 wide。
        out.wide = snap.wide || slot > 0xFF;
    } else if (out.op == Op::IINC) {
        const ca::u32 slot = localBase + snap.localSlot;
        out.a    = static_cast<ca::i32>(slot);
        out.wide = snap.wide || slot > 0xFF;
    }

    auto inserted = dom.insertBefore(callId, out);
    if (inserted.is_err()) return jbc::ir::kNoInsn;
    return inserted.unwrap();
}

/// 调用点拼接：参数/接收者落 fresh 槽 → 接收者 null 守卫（实例被调）→
/// 克隆体 → 分支/switch 补锚 → 调用点入边重锚 → 摘除调用。任一步失败
/// 返回 false（dom 留脏，类级放弃不写回）。
bool spliceSite(jbc::ir::CodeUnit& cu, jbc::ir::ClassSession& session,
                const jbc::ir::InsnId callId, const CalleeBody& callee, ca::u32 localBase)
{
    jbc::ir::CodeDom& dom = cu.dom;

    // 当前序里定位调用点与后继（前序拼接可能已移动位置）。
    const std::vector<jbc::ir::InsnId>& order = dom.order();
    ca::usize pos = order.size();
    for (ca::usize k = 0; k < order.size(); ++k) {
        if (order[k] == callId) {
            pos = k;
            break;
        }
    }
    if (pos + 1 >= order.size()) return false;   // 无顺序后继 → 保守拒收
    const jbc::ir::InsnId succId = order[pos + 1];

    // 统一出口标签：锚定调用点后继（GOTO 携返回值落此处，与原 invoke
    // 压值形态逐点相同）。
    auto contLabelR = dom.createLabel(succId);
    if (contLabelR.is_err()) return false;
    const jbc::ir::LabelId contLabel = contLabelR.unwrap();

    // 参数/接收者落槽：栈顶最后参数先弹 → 逆被调槽序 store。
    jbc::ir::InsnId firstSpliced = jbc::ir::kNoInsn;
    for (ca::usize r = callee.paramLayout.size(); r > 0; --r) {
        const auto& [typeChar, calleeSlot] = callee.paramLayout[r - 1];
        jbc::ir::Insn store;
        store.op        = storeOpOf(typeChar);
        const ca::u32 slot   = localBase + calleeSlot;
        store.a        = static_cast<ca::i32>(slot);
        store.wide     = slot > 0xFF;
        if (store.op == Op::NOP) return false;
        auto inserted = dom.insertBefore(callId, store);
        if (inserted.is_err()) return false;
        if (firstSpliced == jbc::ir::kNoInsn) firstSpliced = inserted.unwrap();
    }

    // 接收者 null 守卫（v2，实例被调）：invoke 家族在调用点做接收者 null
    // 检查，克隆体白名单指令永不解引用接收者——守卫补齐该检查。null 路径
    // ACONST_NULL + ATHROW 抛同型 NPE；异常表标签随调用点入边重锚使守卫的
    // 处理器覆盖域与原调用一致（起点锚重锚后守卫整体落入、终点锚重锚后
    // 整体落在域外，与原调用内外关系相同）。
    // 守卫的 IFNONNULL 以未绑目标形态插入，必须在 begin/endDeferredTargets
    // 受控延后区内进行（dom 拒绝区外携带空目标的分支指令）。
    jbc::ir::InsnId nullGuardBranch = jbc::ir::kNoInsn;
    dom.beginDeferredTargets();
    if (!callee.isStatic) {
        jbc::ir::Insn load;
        load.op = Op::ALOAD;
        load.a  = static_cast<ca::i32>(localBase + callee.paramLayout.front().second);
        auto loadId = dom.insertBefore(callId, load);
        if (loadId.is_err()) {
            dom.endDeferredTargets();
            return false;
        }

        jbc::ir::Insn guard;
        guard.op = Op::IFNONNULL;
        guard.target = jbc::ir::kNoLabel;   // 延后补锚到克隆体首指令
        auto guardId = dom.insertBefore(callId, guard);
        if (guardId.is_err()) {
            dom.endDeferredTargets();
            return false;
        }
        nullGuardBranch = guardId.unwrap();

        jbc::ir::Insn nullCst;
        nullCst.op = Op::ACONST_NULL;
        if (dom.insertBefore(callId, nullCst).is_err()) {
            dom.endDeferredTargets();
            return false;
        }

        jbc::ir::Insn rethrow;
        rethrow.op = Op::ATHROW;
        if (dom.insertBefore(callId, rethrow).is_err()) {
            dom.endDeferredTargets();
            return false;
        }
    }

    // 克隆体（受控延后绑定态：前向分支目标先占位后补锚）。
    std::vector<jbc::ir::InsnId>                     cloneIds;
    std::vector<std::pair<ca::u32, jbc::ir::InsnId>> branches;   ///< 单目标分支
    std::vector<std::pair<ca::u32, jbc::ir::InsnId>> switches;   ///< 多目标 switch
    cloneIds.reserve(callee.insns.size());
    bool cloneFailed = false;
    for (ca::usize i = 0; i < callee.insns.size(); ++i) {
        const jbc::ir::InsnId id = materializeInsn(dom, session, callee.insns[i], localBase,
                                                   contLabel, callId);
        if (id == jbc::ir::kNoInsn) {
            cloneFailed = true;
            break;
        }
        if (isBranchOp(callee.insns[i].op)) branches.emplace_back(i, id);
        if (isSwitchOp(callee.insns[i].op)) switches.emplace_back(i, id);
        cloneIds.push_back(id);
    }
    if (!cloneFailed) {
        for (const auto& [snapPos, insnId] : branches) {
            const ca::u32 targetPos = callee.insns[snapPos].targetPos;
            if (targetPos >= cloneIds.size()) {
                cloneFailed = true;
                break;
            }
            auto labelR = dom.createLabel(cloneIds[targetPos]);
            if (labelR.is_err()) {
                cloneFailed = true;
                break;
            }
            auto mut = dom.get(insnId);
            if (mut.is_err()) {
                cloneFailed = true;
                break;
            }
            mut.unwrap()->target = labelR.unwrap();
        }
    }
    if (!cloneFailed) {
        for (const auto& [snapPos, insnId] : switches) {
            const CalleeInsn& snap = callee.insns[snapPos];
            auto mut = dom.get(insnId);
            if (mut.is_err()) {
                cloneFailed = true;
                break;
            }
            jbc::ir::Insn* out = mut.unwrap();
            auto defaultLabel = dom.createLabel(cloneIds[snap.targetPos]);
            if (defaultLabel.is_err()) {
                cloneFailed = true;
                break;
            }
            out->target = defaultLabel.unwrap();
            if (out->targets.size() != snap.switchTargetPos.size()) {
                cloneFailed = true;
                break;
            }
            bool caseFailed = false;
            for (ca::usize j = 0; j < snap.switchTargetPos.size(); ++j) {
                const ca::u32 casePos = snap.switchTargetPos[j];
                if (casePos >= cloneIds.size()) {
                    caseFailed = true;
                    break;
                }
                auto caseLabel = dom.createLabel(cloneIds[casePos]);
                if (caseLabel.is_err()) {
                    caseFailed = true;
                    break;
                }
                out->targets[j] = caseLabel.unwrap();
            }
            if (caseFailed) {
                cloneFailed = true;
                break;
            }
        }
    }
    if (!cloneFailed && nullGuardBranch != jbc::ir::kNoInsn) {
        if (cloneIds.empty()) {
            cloneFailed = true;
        } else {
            auto guardLabel = dom.createLabel(cloneIds.front());
            if (guardLabel.is_err()) {
                cloneFailed = true;
            } else {
                auto mut = dom.get(nullGuardBranch);
                if (mut.is_err()) {
                    cloneFailed = true;
                } else {
                    mut.unwrap()->target = guardLabel.unwrap();
                }
            }
        }
    }
    if (!cloneFailed && dom.endDeferredTargets().is_err()) cloneFailed = true;
    if (cloneFailed) {
        dom.endDeferredTargets();
        return false;
    }
    if (firstSpliced == jbc::ir::kNoInsn) {
        if (cloneIds.empty()) return false;   // 空体不会发生（capture 要求含 return）
        firstSpliced = cloneIds.front();      // 零参被调：首条克隆体指令即首条插入
    }

    // 调用点入边重锚到首条插入指令（入边路径必须先执行参数落槽；
    // 异常表/调试范围边界标签随迁已被论证中性——克隆体白名单内 op
    // 不抛异常、store 族无栈效应，null 守卫的异常覆盖域与原调用一致）。
    // 随后摘除调用。
    for (const jbc::ir::LabelId label : dom.labelsAt(callId)) {
        if (dom.bindLabel(label, firstSpliced).is_err()) return false;
    }
    return dom.erase(callId).is_ok();
}

// ── 单类改写 ──

enum class RewriteOutcome
{
    Unchanged,   ///< 无站点命中（或版本门/空类跳过）
    Changed,     ///< 有改写，可序列化写回
    Abandoned,   ///< 改写中途失败：整类放弃，容器原字节保留
};

RewriteOutcome rewriteClass(LoadedClass&                                 entry,
                            const std::unordered_map<std::string, CalleeBody>& candidates,
                            mjbase::KeyPool& pool,
                            jbc::analysis::ITypeResolver& resolver,
                            const InlinerFacts& facts)
{
    jbc::ir::ClassSession& session = *entry.session;
    // v1 类版本门：SMT 语义（≥50）才收，帧链全再生兜底。
    if (session.backing().major < 50) return RewriteOutcome::Unchanged;

    jbc::ir::ClassUnit& unit    = session.unit();
    const std::string   selfName(session.backing().thisName());
    bool                changedAny = false;

    for (ca::usize mi = 0; mi < unit.methods.size(); ++mi) {
        const jbc::ir::MethodUnit& mu = unit.methods[mi];
        if (!jbc::ir::hasCode(session, mi)) continue;
        auto promoted = jbc::ir::promoteMethod(session, mi);
        if (promoted.is_err()) continue;
        jbc::ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) continue;

        bool methodChanged = false;
        // 站点迭代走序快照：句柄跨拼接稳定（槽位永不回收），位置现查。
        const std::vector<jbc::ir::InsnId> orderSnapshot = cu->dom.order();
        for (const jbc::ir::InsnId id : orderSnapshot) {
            const jbc::ir::Insn* in = cu->dom.peek(id);
            if (in == nullptr) continue;
            if (in->op != Op::INVOKESTATIC && in->op != Op::INVOKESPECIAL &&
                in->op != Op::INVOKEVIRTUAL && in->op != Op::INVOKEINTERFACE) {
                continue;
            }
            jbc::ir::ClassSession::MemberSem enc;
            if (!session.memberOf(*in, enc)) continue;
            // invokeinterface 的 CP 语义族 = InterfaceMethod（memberOf 按 op 定
            // kind）；方法解析两 tag 皆收（JVMS 5.4.3.3），站点门两族同放。
            if (enc.kind != MemberKind::Method && enc.kind != MemberKind::InterfaceMethod) {
                continue;
            }

            // ── 去虚化/解析门（v2）：virtual/interface 站点先解析唯一目标
            // 声明者，候选按声明者键查；static/special 站点符号 owner 即
            // 目标（v1 口径）。
            std::string targetOwner(enc.owner);
            if (in->op == Op::INVOKEVIRTUAL || in->op == Op::INVOKEINTERFACE) {
                const DevirtResolution res = resolveDevirtualization(facts, enc);
                if (res.kind == DevirtResolution::Kind::None) continue;
                targetOwner = res.owner;
            }
            const auto candIt = candidates.find(methodKey(targetOwner, enc.name, enc.desc));
            if (candIt == candidates.end()) continue;
            const CalleeBody& callee = candIt->second;

            // ── 分派形门：static↔static；special↔private 非 static
            //（super 调用/<init> 面不在挂账内）；virtual/interface↔实例
            //（interface 只认 private——解析层已拒，此处双保险）。
            if (in->op == Op::INVOKESTATIC && !callee.isStatic) continue;
            if (in->op == Op::INVOKESPECIAL && (callee.isStatic || !callee.isPrivate)) {
                continue;
            }
            if (in->op == Op::INVOKEINTERFACE && callee.isStatic) continue;

            const bool sameTargetClass = targetOwner == selfName;

            // ── 访问面门（跨声明类）：v1 双 public 面原样；v2 private 走
            // nestmate 属性链、protected 走子类链、包私有不可证拒收。
            if (!sameTargetClass) {
                if (callee.publicMethod && callee.publicOwner) {
                    // v1 双 public 面
                } else if (callee.isPrivate) {
                    if (!sameNestProof(facts, selfName, targetOwner)) continue;
                } else if (callee.protectedMethod) {
                    if (!subclassProof(facts, selfName, targetOwner)) continue;
                } else {
                    continue;   // 包私有跨类：运行时包等价字节不可证（停报面）
                }
                // 初始化触发门（仅 static：invokestatic 触发声明类/接口
                // 初始化；实例调用不触发——JVMS 5.5 无此 bullet，接收者
                // 存在性已背书声明类已初始化、null 路径 NPE 先于初始化）。
                if (callee.isStatic && callee.ownerHasClinit) continue;
                if (!bodyCrossClassSafe(callee)) continue;
            }

            const ca::u32 localBase = cu->maxLocals;
            if (static_cast<ca::u32>(localBase) + callee.localsNeeded > kMaxLocalsLimit) continue;

            if (!spliceSite(*cu, session, id, callee, localBase)) {
                return RewriteOutcome::Abandoned;   // 克隆中途失败，整类不写回
            }
            cu->maxLocals = static_cast<ca::u16>(localBase + callee.localsNeeded);
            methodChanged = true;
        }
        if (!methodChanged) continue;
        changedAny = true;

        // 写回链：嵌套属性适配 → lower → Strict 帧重算 + maxes 采用 +
        // SMT 再生（异常向上传播，由 run 层按类放弃收敛）。
        mjbase::spi::frame_rewire::prepareNestedAttrsForLayout(session, *cu);
        if (jbc::ir::lowerMethod(session, mi).is_err()) return RewriteOutcome::Abandoned;
        const mjbase::MemberKey key =
            mjbase::memberKeyOf(pool, mjbase::classKeyOf(pool, selfName),
                                unit.symbols().str(mu.name), unit.symbols().str(mu.desc));
        // 堆分配 scope：unique_ptr 局部持有、地址互异且贯穿本次调用
        //（pass-contract §1 自造 scope 的地址唯一自证）。
        const auto scope = std::make_unique<mjbase::spi::MethodScope>(mjbase::spi::MethodScope{
            mjbase::spi::MethodTarget{key, mu.id, unit.symbols().str(mu.name),
                                      unit.symbols().str(mu.desc), mu.access, session},
            *cu});
        mjbase::spi::frame_rewire::rewireFramesAfterLower(session, *scope, resolver);
    }
    return changedAny ? RewriteOutcome::Changed : RewriteOutcome::Unchanged;
}

}   // namespace

mjbase::spi::PreservedAnalyses MethodInliner::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;   // 自包含两阶段，不消费注册分析

    std::unordered_map<std::string, CalleeBody> candidates;
    std::vector<LoadedClass> loaded = buildCandidates(container, candidates);
    if (candidates.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 帧再生面：CommonSuperResolver 不可判定回退 java/lang/Object
    //（tool_class_combiner 同口径——克隆体纯计算面的引用合并不依赖真实层次；
    // 该图保持空载，v2 判定事实走独立 devirtHierarchy，v1 帧行为零漂移）。
    mjbase::KeyPool                        pool;
    mjbase::hierarchy::ClassHierarchy      hierarchy{pool};
    mjbase::spi::CommonSuperClassResolver  superResolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{superResolver};

    // v2 判定事实：容器全量层次图（去虚化/protected 面判定）+ 会话索引
    //（nest 属性读取）。环/重复登记被拒的类按未登记处理（判定层 fail-closed）。
    mjbase::hierarchy::ClassHierarchy devirtHierarchy{pool};
    for (const LoadedClass& cls : loaded) {
        (void)devirtHierarchy.addClass(mjbase::hierarchy::fillSummary(*cls.session, pool));
    }
    std::unordered_map<std::string, jbc::ir::ClassSession*> sessionsByName;
    sessionsByName.reserve(loaded.size());
    for (const LoadedClass& cls : loaded) {
        sessionsByName.emplace(cls.name, cls.session.get());
    }
    InlinerFacts facts;
    facts.hierarchy = &devirtHierarchy;
    facts.sessions  = &sessionsByName;
    facts.pool      = &pool;

    struct PendingWrite
    {
        std::string         uri;
        std::vector<ca::u8> bytes;
    };
    std::vector<PendingWrite> pending;

    for (LoadedClass& entry : loaded) {
        RewriteOutcome outcome = RewriteOutcome::Unchanged;
        try {
            outcome = rewriteClass(entry, candidates, pool, typeResolver, facts);
        } catch (const std::exception&) {
            continue;   // 类级放弃（帧重算 Strict 不可算等）：容器原字节保留
        }
        if (outcome != RewriteOutcome::Changed) continue;
        auto encoded = jbc::core::writeClass(entry.session->backing());
        if (encoded.is_err()) continue;   // 序列化失败：整类放弃
        PendingWrite item;
        item.uri   = entry.uri;
        item.bytes = std::move(encoded).unwrap();
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
