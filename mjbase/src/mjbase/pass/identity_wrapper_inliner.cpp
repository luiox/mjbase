#include "mjbase/pass/identity_wrapper_inliner.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using MemberKind = jbc::ir::ClassSession::MemberSem::Kind;

constexpr ca::u16 kAccPublic       = 0x0001;
constexpr ca::u16 kAccPrivate      = 0x0002;
constexpr ca::u16 kAccStatic       = 0x0008;
constexpr ca::u16 kAccSynchronized = 0x0020;

/// JDK 装拆箱白名单：八个 box 类（public final，方法恒 public）。
struct BoxType
{
    std::string_view owner;
    char             prim;
    std::string_view valueName;
};

constexpr BoxType kBoxTypes[] = {
    {"java/lang/Integer",   'I', "intValue"},
    {"java/lang/Long",      'J', "longValue"},
    {"java/lang/Float",     'F', "floatValue"},
    {"java/lang/Double",    'D', "doubleValue"},
    {"java/lang/Short",     'S', "shortValue"},
    {"java/lang/Byte",      'B', "byteValue"},
    {"java/lang/Character", 'C', "charValue"},
    {"java/lang/Boolean",   'Z', "booleanValue"},
};

/// 壳表/声明表键：owner + '\n' + name + '\n' + desc。
std::string refKey(std::string_view owner, std::string_view name, std::string_view desc)
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

/// 改写后的调用点目标（壳表值；callSiteOp 是调用点应呈现的 invoke op）。
struct TargetRef
{
    ca::i32     callSiteOp = 0;
    std::string owner;
    std::string name;
    std::string desc;
};

/// 壳表 + 容器内方法声明访问位（访问门查询面）。
struct WrapperIndex
{
    std::unordered_map<std::string, TargetRef> wrappers;
    std::unordered_map<std::string, ca::u32>   declared;
};

/// 描述符参数条目：类别字符（L/[ 记引用）+ 槽宽（J/D 双槽）。
struct ParamType
{
    char    typeChar = 'V';
    ca::u8  slots    = 1;
};

/// 跳过一个描述符类型（自 i 起，'(' 内或返回位皆适用）；畸形返回 npos。
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

/// 解析方法描述符：参数类别/槽宽 + 返回类别字符；畸形返回 false。
bool parseDesc(std::string_view desc, std::vector<ParamType>& params, char& retChar)
{
    params.clear();
    ca::usize i = 0;
    if (i >= desc.size() || desc[i] != '(') return false;
    ++i;
    while (i < desc.size() && desc[i] != ')') {
        const char t   = desc[i];
        const ca::usize next = skipType(desc, i);
        if (next == std::string_view::npos) return false;
        if (t == 'J' || t == 'D') params.push_back({t, 2});
        else params.push_back({t, 1});
        i = next;
    }
    if (i >= desc.size() || desc[i] != ')') return false;
    ++i;
    const ca::usize next = skipType(desc, i);
    if (next == std::string_view::npos || next != desc.size()) return false;
    retChar = desc[i];
    return true;
}

/// 去掉首参后的描述符（形态 B 匹配面）；无参或畸形返回 nullopt。
std::optional<std::string> stripFirstParam(std::string_view desc)
{
    if (desc.empty() || desc[0] != '(' || desc.size() < 2 || desc[1] == ')') {
        return std::nullopt;
    }
    const ca::usize end = skipType(desc, 1);
    if (end == std::string_view::npos || end > desc.size()) return std::nullopt;
    std::string out;
    out.reserve(desc.size());
    out.push_back('(');
    out.append(desc.substr(end));
    return out;
}

/// 装载指令与描述符参数位是否匹配（_n 形态槽位由操作码携带，泛型形态看 a）。
bool loadMatches(char typeChar, const jbc::ir::Insn& in, ca::i32 expectSlot)
{
    ca::i32 generic = -1;
    ca::i32 base    = -1;
    switch (typeChar) {
    case 'I': case 'S': case 'B': case 'C': case 'Z':
        generic = 0x15; base = static_cast<ca::i32>(Op::ILOAD_0); break;
    case 'J': generic = 0x16; base = static_cast<ca::i32>(Op::LLOAD_0); break;
    case 'F': generic = 0x17; base = static_cast<ca::i32>(Op::FLOAD_0); break;
    case 'D': generic = 0x18; base = static_cast<ca::i32>(Op::DLOAD_0); break;
    case 'L': case '[':
        generic = 0x19; base = static_cast<ca::i32>(Op::ALOAD_0); break;
    default: return false;
    }
    const ca::i32 v = static_cast<ca::i32>(in.op);
    if (v >= base && v <= base + 3) return (v - base) == expectSlot;
    if (v == generic) return in.a == expectSlot;
    return false;
}

/// 返回指令与返回类别是否匹配。
bool returnMatches(char retChar, const jbc::ir::Insn& in)
{
    switch (retChar) {
    case 'V': return in.op == Op::RETURN;
    case 'I': case 'S': case 'B': case 'C': case 'Z': return in.op == Op::IRETURN;
    case 'J': return in.op == Op::LRETURN;
    case 'F': return in.op == Op::FRETURN;
    case 'D': return in.op == Op::DRETURN;
    case 'L': case '[': return in.op == Op::ARETURN;
    default: return false;
    }
}

/// invokeinterface count：接收者 + 参数槽位总数（J/D 双槽）。
ca::i32 interfaceCountOf(std::string_view targetDesc)
{
    std::vector<ParamType> params;
    char                   ret = 'V';
    if (!parseDesc(targetDesc, params, ret)) return 0;
    ca::i32 count = 1;
    for (const ParamType& p : params) count += p.slots;
    return count;
}

/// 壳体形态判定：通过返回调用点改写目标，否则 nullopt。
/// 前置（调用方保证）：static、非 synchronized、名非 '<'、hasCode。
std::optional<TargetRef> wrapperShapeOf(jbc::ir::ClassSession& session, ca::usize methodIdx)
{
    const jbc::ir::ClassUnit&  unit = session.unit();
    const jbc::ir::MethodUnit& mu   = unit.methods[methodIdx];
    const std::string_view desc    = unit.symbols().str(mu.desc);

    std::vector<ParamType> params;
    char                   ret = 'V';
    if (!parseDesc(desc, params, ret)) return std::nullopt;

    auto promoted = jbc::ir::promoteMethod(session, methodIdx);
    if (promoted.is_err()) return std::nullopt;
    jbc::ir::CodeUnit* cu = promoted.unwrap().get();
    if (cu == nullptr || !cu->exc.empty()) return std::nullopt;
    const std::vector<jbc::ir::InsnId>& order = cu->dom.order();
    if (order.size() != params.size() + 2) return std::nullopt;

    ca::i32 slot = 0;
    for (ca::usize i = 0; i < params.size(); ++i) {
        const jbc::ir::Insn* in = cu->dom.peek(order[i]);
        if (in == nullptr || !loadMatches(params[i].typeChar, *in, slot)) return std::nullopt;
        slot += params[i].slots;
    }
    const jbc::ir::Insn* call = cu->dom.peek(order[params.size()]);
    const jbc::ir::Insn* back = cu->dom.peek(order[params.size() + 1]);
    if (call == nullptr || back == nullptr || !returnMatches(ret, *back)) return std::nullopt;

    jbc::ir::ClassSession::MemberSem target;
    if (!session.memberOf(*call, target)) return std::nullopt;

    TargetRef out;
    out.callSiteOp = static_cast<ca::i32>(call->op);
    const auto op  = static_cast<Op>(call->op);
    if (op == Op::INVOKESTATIC) {
        if (target.kind != MemberKind::Method) return std::nullopt;
        if (target.desc != desc) return std::nullopt;                 // 形态 A：全同
    } else if (op == Op::INVOKEVIRTUAL || op == Op::INVOKEINTERFACE) {
        const auto expectKind = op == Op::INVOKEINTERFACE ? MemberKind::InterfaceMethod
                                                          : MemberKind::Method;
        if (target.kind != expectKind) return std::nullopt;
        const auto stripped = stripFirstParam(desc);                  // 形态 B：去首参
        if (!stripped.has_value() || *stripped != target.desc) return std::nullopt;
    } else {
        return std::nullopt;   // invokespecial / invokedynamic 不进壳表
    }
    out.owner.assign(target.owner);
    out.name.assign(target.name);
    out.desc.assign(target.desc);
    return out;
}

/// 路径压缩 + 环拒收：raw（壳 → 自身体目标）解析为「壳 → 最终目标」；
/// 环上（含自递归）与指向死链的成员剔除。
void compressWrappers(std::unordered_map<std::string, TargetRef>& raw)
{
    enum class Mark : ca::u8 { Dead, Alive };
    std::unordered_map<std::string, Mark>          marks;
    std::unordered_map<std::string, TargetRef>     out;

    for (auto& [key, entry] : raw) {
        (void)entry;
        if (marks.count(key) != 0) continue;

        std::vector<std::string>       path{key};
        std::unordered_set<std::string> onPath{key};
        std::string cur = key;
        TargetRef   finalRef{};
        bool        alive = false;
        while (true) {
            const TargetRef& t       = raw[cur];
            const std::string nextKey = refKey(t.owner, t.name, t.desc);
            if (const auto m = marks.find(nextKey); m != marks.end()) {
                if (m->second == Mark::Alive) {
                    finalRef = out[nextKey];
                    alive    = true;
                }
                break;   // Dead：随链死
            }
            if (onPath.count(nextKey) != 0) break;   // 环：整 path 死
            const auto nextIt = raw.find(nextKey);
            if (nextIt == raw.end()) {
                finalRef = raw[cur];                 // 终点目标即最终目标
                alive    = true;
                break;
            }
            path.push_back(nextKey);
            onPath.insert(nextKey);
            cur = nextKey;
        }
        for (const std::string& k : path) {
            marks[k] = alive ? Mark::Alive : Mark::Dead;
            if (alive) out[k] = finalRef;
        }
    }
    raw = std::move(out);
}

/// 同包判定（'/' 前缀段相等；默认包与命名包不同包）。
bool samePackage(std::string_view a, std::string_view b)
{
    const auto slashOf = [](std::string_view n) { return n.rfind('/'); };
    const auto sa = slashOf(a);
    const auto sb = slashOf(b);
    if (sa == std::string_view::npos || sb == std::string_view::npos) {
        return sa == sb;
    }
    return a.substr(0, sa) == b.substr(0, sb);
}

/// 容器外目标白名单：JDK 八 box 类的 valueOf（形态 A）与 <x>Value（形态 B）。
bool isPublicBoxTarget(const TargetRef& t)
{
    const bool staticForm = t.callSiteOp == static_cast<ca::i32>(Op::INVOKESTATIC);
    for (const BoxType& box : kBoxTypes) {
        if (t.owner != box.owner) continue;
        if (staticForm && t.name == "valueOf") {
            std::string expect;
            expect.push_back('(');
            expect.push_back(box.prim);
            expect.append(")L");
            expect.append(box.owner);
            expect.push_back(';');
            return t.desc == expect;
        }
        if (!staticForm && t.name == box.valueName) {
            std::string expect;
            expect.append("()");
            expect.push_back(box.prim);
            return t.desc == expect;
        }
    }
    return false;
}

/// 最终目标从调用者类可达（§5.4.4 访问控制保守面；protected 跨包子类不认）。
bool targetAccessibleFrom(const TargetRef&                                 t,
                          const std::unordered_map<std::string, ca::u32>& declared,
                          std::string_view                                  callerName)
{
    const auto it = declared.find(refKey(t.owner, t.name, t.desc));
    if (it == declared.end()) return isPublicBoxTarget(t);
    const ca::u32 access = it->second;
    if ((access & kAccPublic) != 0) return true;
    if (t.owner == callerName) return true;
    if ((access & kAccPrivate) != 0) return false;
    return samePackage(t.owner, callerName);
}

/// 阶段一：扫描容器构建壳表（已压缩）与声明访问位表。
WrapperIndex buildWrapperIndex(mjbase::model::ResourceContainer& container)
{
    WrapperIndex                       idx;
    std::unordered_map<std::string, TargetRef> raw;

    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource.get());
        if (cls == nullptr || cls->get().empty()) continue;
        auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
        if (loaded.is_err()) continue;
        jbc::ir::ClassSession& session = *loaded.unwrap();
        const jbc::ir::ClassUnit& unit = session.unit();
        const std::string_view  self  = session.backing().thisName();

        for (ca::usize i = 0; i < unit.methods.size(); ++i) {
            const jbc::ir::MethodUnit& mu   = unit.methods[i];
            const std::string_view     name = unit.symbols().str(mu.name);
            const std::string_view     desc = unit.symbols().str(mu.desc);
            idx.declared.emplace(refKey(self, name, desc), mu.access);

            if ((mu.access & kAccStatic) == 0) continue;
            if ((mu.access & kAccSynchronized) != 0) continue;
            if (!name.empty() && name.front() == '<') continue;
            if (!jbc::ir::hasCode(session, i)) continue;

            auto shape = wrapperShapeOf(session, i);
            if (!shape.has_value()) continue;
            raw.emplace(refKey(self, name, desc), std::move(*shape));
        }
    }
    compressWrappers(raw);
    idx.wrappers = std::move(raw);
    return idx;
}

/// 阶段二（单类）：装载 + 逐站点改写 + 逐方法 lower。返回改写后的类字节；
/// 无命中或任一物化失败返回 nullopt（整类放弃，容器不动）。
std::optional<std::vector<ca::u8>> rewriteCaller(const std::vector<ca::u8>& corpus,
                                                 const WrapperIndex&        idx)
{
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
    if (loaded.is_err()) return std::nullopt;
    jbc::ir::ClassSession& session = *loaded.unwrap();
    const std::string     callerName(session.backing().thisName());

    bool rewrittenAny = false;
    for (ca::usize mi = 0; mi < session.unit().methods.size(); ++mi) {
        if (!jbc::ir::hasCode(session, mi)) continue;
        auto promoted = jbc::ir::promoteMethod(session, mi);
        if (promoted.is_err()) return std::nullopt;
        jbc::ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) continue;

        jbc::ir::CodeDom& dom = cu->dom;
        bool methodChanged    = false;
        for (const jbc::ir::InsnId id : dom.order()) {
            const jbc::ir::Insn* in = dom.peek(id);
            if (in == nullptr || in->op != Op::INVOKESTATIC) continue;
            jbc::ir::ClassSession::MemberSem enc;
            if (!session.memberOf(*in, enc) || enc.kind != MemberKind::Method) continue;
            const auto wit = idx.wrappers.find(refKey(enc.owner, enc.name, enc.desc));
            if (wit == idx.wrappers.end()) continue;
            const TargetRef& t = wit->second;
            if (!targetAccessibleFrom(t, idx.declared, callerName)) continue;

            auto mut = dom.get(id);
            if (mut.is_err()) continue;
            jbc::ir::Insn* insn = mut.unwrap();
            const Op      savedOp = insn->op;
            const ca::i32 savedA  = insn->a;
            const ca::i32 savedB  = insn->b;
            insn->op = static_cast<Op>(t.callSiteOp);
            if (t.callSiteOp == static_cast<ca::i32>(Op::INVOKEINTERFACE)) {
                insn->a = interfaceCountOf(t.desc);
                insn->b = 0;
            }
            const auto kind = t.callSiteOp == static_cast<ca::i32>(Op::INVOKEINTERFACE)
                                  ? MemberKind::InterfaceMethod
                                  : MemberKind::Method;
            if (!session.setMember(*insn, kind, t.owner, t.name, t.desc)) {
                insn->op = savedOp;   // 改写拒绝：恢复原形，放弃该站点
                insn->a  = savedA;
                insn->b  = savedB;
                continue;
            }
            methodChanged = true;
        }
        if (!methodChanged) continue;
        if (jbc::ir::lowerMethod(session, mi).is_err()) return std::nullopt;
        rewrittenAny = true;
    }

    if (!rewrittenAny) return std::nullopt;
    auto encoded = jbc::core::writeClass(session.backing());
    if (encoded.is_err()) return std::nullopt;
    return std::move(encoded).unwrap();
}

}   // namespace

mjbase::spi::PreservedAnalyses IdentityWrapperInliner::run(
    mjbase::model::ResourceContainer& container, mjbase::spi::GlobalAnalysisManager& am)
{
    (void)am;   // 自包含两阶段，不消费注册分析
    WrapperIndex idx = buildWrapperIndex(container);
    if (idx.wrappers.empty()) return mjbase::spi::PreservedAnalyses::all();

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
        auto rewritten = rewriteCaller(cls->get(), idx);
        if (!rewritten.has_value()) continue;
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
