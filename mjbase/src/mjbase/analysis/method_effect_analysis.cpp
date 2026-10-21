#include "mjbase/analysis/method_effect_analysis.hpp"

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "jbc/core/opcode.hpp"

#include "class_scan.hpp"
#include "cp_refs.hpp"
#include "mjbase/hierarchy/summary.hpp"

namespace mjbase::analysis {

namespace {

using jbc::core::Op;

/// JVM 方法访问位：native（hierarchy/summary.hpp 未收编，效应层本地补齐）。
inline constexpr ca::u32 kAccNative = 0x0100;

MemberKey methodKeyOf(KeyPool& pool, std::string_view owner, std::string_view name,
                      std::string_view desc)
{
    return memberKeyOf(pool, classKeyOf(pool, owner), name, desc);
}

bool isCtorOrClinitName(std::string_view name)
{
    return name == "<init>" || name == "<clinit>";
}

MethodEffectKind mergeEffect(MethodEffectKind lhs, MethodEffectKind rhs)
{
    if (lhs == MethodEffectKind::Impure || rhs == MethodEffectKind::Impure) {
        return MethodEffectKind::Impure;
    }
    if (lhs == MethodEffectKind::Unknown || rhs == MethodEffectKind::Unknown) {
        return MethodEffectKind::Unknown;
    }
    if (lhs == MethodEffectKind::ReadOnly || rhs == MethodEffectKind::ReadOnly) {
        return MethodEffectKind::ReadOnly;
    }
    return MethodEffectKind::Pure;
}

bool isArrayStore(Op op)
{
    return op >= Op::IASTORE && op <= Op::SASTORE;
}

bool isFieldRead(Op op)
{
    return op == Op::GETSTATIC || op == Op::GETFIELD;
}

bool isFieldWrite(Op op)
{
    return op == Op::PUTSTATIC || op == Op::PUTFIELD;
}

bool isAllocation(Op op)
{
    return op == Op::NEW || op == Op::NEWARRAY || op == Op::ANEWARRAY ||
           op == Op::MULTIANEWARRAY;
}

MethodEffectKind knownExternalEffect(const KeyPool& pool, MemberKey method)
{
    const auto owner = pool.str(method.cls);
    const auto keyed = std::string(pool.str(method.name)) + "\n" + std::string(pool.str(method.desc));
    if (owner == "java/lang/String") {
        static const std::unordered_set<std::string> kReadOnlyStringMethods{
            "length\n()I",
            "isEmpty\n()Z",
            "charAt\n(I)C",
            "codePointAt\n(I)I",
        };
        if (kReadOnlyStringMethods.count(keyed) != 0) return MethodEffectKind::ReadOnly;
    }
    if (owner == "java/lang/Math" || owner == "java/lang/StrictMath") {
        static const std::unordered_set<std::string> kPureMathMethods{
            "abs\n(I)I",  "abs\n(J)J",  "abs\n(F)F",  "abs\n(D)D",
            "min\n(II)I", "min\n(JJ)J", "min\n(FF)F", "min\n(DD)D",
            "max\n(II)I", "max\n(JJ)J", "max\n(FF)F", "max\n(DD)D",
        };
        if (kPureMathMethods.count(keyed) != 0) return MethodEffectKind::Pure;
    }
    return MethodEffectKind::Unknown;
}

/// 一个方法的局部扫描产物。
struct ScannedMethod
{
    MethodEffectSummary       summary;
    MethodEffectKind          localEffect = MethodEffectKind::Pure;
    std::vector<MemberKey>    calls;
    MemberKey                 constantCall{};   ///< 纯零参静态调用（常量返回包装候选）
    ConstantReturnValue       directConstant;
};

/// 常量返回形状：恰好两条可执行指令（producer + return），无异常表。
void scanConstantReturn(const jbc::ir::CodeUnit& u, const jbc::core::ConstantPool& cp,
                        KeyPool& pool, ScannedMethod& scanned)
{
    if (!scanned.summary.hasBody || !u.exc.empty()) return;
    const auto& order = u.dom.order();
    if (order.size() != 2) return;
    const jbc::ir::Insn* producer = u.dom.peek(order[0]);
    const jbc::ir::Insn* ret      = u.dom.peek(order[1]);
    if (producer == nullptr || ret == nullptr) return;

    ConstantReturnValue& out = scanned.directConstant;
    if (ret->op == Op::IRETURN &&
        (producer->op == Op::ICONST_M1 ||
         (producer->op >= Op::ICONST_0 && producer->op <= Op::ICONST_5))) {
        out.kind     = ConstantReturnKind::Int;
        out.intValue = producer->op == Op::ICONST_M1 ? -1
                                                     : static_cast<ca::i32>(producer->op) -
                                                           static_cast<ca::i32>(Op::ICONST_0);
        return;
    }
    if (ret->op == Op::LRETURN &&
        (producer->op == Op::LCONST_0 || producer->op == Op::LCONST_1)) {
        out.kind     = ConstantReturnKind::Long;
        out.intValue = static_cast<ca::i32>(producer->op) - static_cast<ca::i32>(Op::LCONST_0);
        return;
    }
    if (ret->op == Op::FRETURN && producer->op >= Op::FCONST_0 && producer->op <= Op::FCONST_2) {
        out.kind          = ConstantReturnKind::Float;
        out.floatingValue = static_cast<double>(static_cast<ca::i32>(producer->op) -
                                                static_cast<ca::i32>(Op::FCONST_0));
        return;
    }
    if (ret->op == Op::DRETURN &&
        (producer->op == Op::DCONST_0 || producer->op == Op::DCONST_1)) {
        out.kind = ConstantReturnKind::Double;
        out.floatingValue =
            static_cast<double>(static_cast<ca::i32>(producer->op) - static_cast<ca::i32>(Op::DCONST_0));
        return;
    }
    if (ret->op == Op::ARETURN && producer->op == Op::ACONST_NULL) {
        out.kind = ConstantReturnKind::Null;
        return;
    }
    if (producer->op == Op::LDC || producer->op == Op::LDC_W || producer->op == Op::LDC2_W) {
        // ldc 载荷按常量池直读（CodeUnit 不携带会话语义层）。
        if (cp.valid(producer->cp)) {
            const auto& e = cp.at(producer->cp);
            if (ret->op == Op::IRETURN && e.tag == jbc::core::CpTag::Integer) {
                out.kind     = ConstantReturnKind::Int;
                out.intValue = e.ival;
                return;
            }
            if (ret->op == Op::LRETURN && e.tag == jbc::core::CpTag::Long) {
                out.kind     = ConstantReturnKind::Long;
                out.intValue = e.lval;
                return;
            }
            if (ret->op == Op::FRETURN && e.tag == jbc::core::CpTag::Float) {
                out.kind = ConstantReturnKind::Float;
                const ca::u32 bits = static_cast<ca::u32>(e.ival);
                float         f    = 0.0f;
                static_assert(sizeof(float) == sizeof(ca::u32), "float must be 32-bit");
                std::memcpy(&f, &bits, sizeof(f));
                out.floatingValue = static_cast<double>(f);
                return;
            }
            if (ret->op == Op::DRETURN && e.tag == jbc::core::CpTag::Double) {
                out.kind = ConstantReturnKind::Double;
                const ca::u64 bits = static_cast<ca::u64>(e.lval);
                double        d    = 0.0;
                std::memcpy(&d, &bits, sizeof(d));
                out.floatingValue = d;
                return;
            }
            if (ret->op == Op::ARETURN && e.tag == jbc::core::CpTag::String && cp.isUtf8(e.a)) {
                out.kind        = ConstantReturnKind::String;
                out.stringValue = std::string(cp.utf8Of(e.a));
                return;
            }
        }
    }
    if (producer->op == Op::INVOKESTATIC) {
        MemberRefSem ref;
        if (resolveMemberRef(cp, producer->cp, ref)) {
            // 无参静态调用：描述符括号内无参数类型（')' 紧随 '('）。
            const std::string_view callDesc = ref.desc;
            if (!callDesc.empty() && callDesc.front() == '(') {
                const auto close = callDesc.find(')');
                if (close == 1) {
                    scanned.constantCall = methodKeyOf(pool, ref.owner, ref.name, ref.desc);
                }
            }
        }
    }
}

}   // namespace

const char* methodEffectKindName(MethodEffectKind kind)
{
    switch (kind) {
    case MethodEffectKind::Pure: return "Pure";
    case MethodEffectKind::ReadOnly: return "ReadOnly";
    case MethodEffectKind::Impure: return "Impure";
    case MethodEffectKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

void MethodEffectModel::clear()
{
    pool_       = KeyPool{};
    summaries_.clear();
    byKey_.clear();
}

void MethodEffectModel::build(const mjbase::model::ResourceContainer& container)
{
    clear();
    auto classes = scanClassResources(container);

    std::vector<ScannedMethod> methods;
    for (const auto& clazz : classes) {
        jbc::ir::ClassSession& session = *clazz.session;
        // 扫描前就地升格（scanClassResources 只建会话不升格）。
        (void)jbc::ir::collectCodeUnits(session);
        const jbc::ir::ClassUnit& unit = session.unit();
        for (const auto& m : unit.methods) {
            const std::string_view owner = unit.symbols().str(unit.thisClass);
            const std::string_view name  = unit.symbols().str(m.name);
            const std::string_view desc  = unit.symbols().str(m.desc);
            ScannedMethod scanned;
            scanned.summary.method  = methodKeyOf(pool_, owner, name, desc);
            scanned.summary.declared = true;
            const bool isStatic      = (m.access & mjbase::hierarchy::kAccStatic) != 0;
            const bool hasBody       = (m.access & (mjbase::hierarchy::kAccAbstract |
                                                    kAccNative)) == 0;
            scanned.summary.hasBody  = hasBody;

            if (!hasBody || isCtorOrClinitName(name)) {
                scanned.localEffect = MethodEffectKind::Unknown;
                methods.push_back(std::move(scanned));
                continue;
            }
            if (m.code == nullptr) {
                // 声明有体但无 Code（畸形）：降为 Unknown 单方法，
                // 效应层查询不因坏资源整体不可用。
                scanned.localEffect = MethodEffectKind::Unknown;
                methods.push_back(std::move(scanned));
                continue;
            }
            const jbc::ir::CodeUnit& u = *m.code;

            MethodEffectKind local = MethodEffectKind::Pure;
            for (const auto& id : u.dom.order()) {
                const jbc::ir::Insn* in = u.dom.peek(id);
                if (in == nullptr) continue;
                if (in->op == Op::INVOKEVIRTUAL || in->op == Op::INVOKESPECIAL ||
                    in->op == Op::INVOKESTATIC || in->op == Op::INVOKEINTERFACE) {
                    MemberRefSem ref;
                    if (resolveMemberRef(session.backing().cp, in->cp, ref)) {
                        scanned.calls.push_back(methodKeyOf(pool_, ref.owner, ref.name, ref.desc));
                    }
                    continue;
                }
                if (in->op == Op::INVOKEDYNAMIC) {
                    local = mergeEffect(local, MethodEffectKind::Impure);
                    continue;
                }
                if (isFieldWrite(in->op) || isArrayStore(in->op) || isAllocation(in->op) ||
                    in->op == Op::ATHROW || in->op == Op::MONITORENTER ||
                    in->op == Op::MONITOREXIT) {
                    local = mergeEffect(local, MethodEffectKind::Impure);
                }
                else if (isFieldRead(in->op) || in->op == Op::ARRAYLENGTH) {
                    local = mergeEffect(local, MethodEffectKind::ReadOnly);
                }
            }
            scanned.localEffect = local;
            scanConstantReturn(u, session.backing().cp, pool_, scanned);
            methods.push_back(std::move(scanned));
        }
    }

    // 局部事实单调；沿直接调用边迭代让 Pure/ReadOnly 被调改善调用方，
    // Unknown/外部边保持保守。
    std::unordered_map<MemberKey, MethodEffectKind, MemberKeyHash>    currentEffects;
    std::unordered_map<MemberKey, ConstantReturnValue, MemberKeyHash> currentConstants;
    for (const auto& method : methods) {
        currentEffects[method.summary.method]  = method.localEffect;
        currentConstants[method.summary.method] = method.directConstant;
    }
    bool changed = true;
    for (ca::usize round = 0; changed && round < methods.size() + 1; ++round) {
        changed = false;
        for (const auto& method : methods) {
            MethodEffectKind effect = method.localEffect;
            for (const auto call : method.calls) {
                auto               it     = currentEffects.find(call);
                const MethodEffectKind callee = it == currentEffects.end()
                                                    ? knownExternalEffect(pool_, call)
                                                    : it->second;
                effect = mergeEffect(effect, callee);
            }
            auto& slot = currentEffects[method.summary.method];
            if (slot == effect) continue;
            slot    = effect;
            changed = true;
        }
        for (const auto& method : methods) {
            ConstantReturnValue next = method.directConstant;
            if (!next.isKnown() && currentEffects[method.summary.method] == MethodEffectKind::Pure &&
                method.constantCall.name != 0) {
                auto it = currentConstants.find(method.constantCall);
                if (it != currentConstants.end()) next = it->second;
            }
            if (!(currentConstants[method.summary.method] == next)) {
                currentConstants[method.summary.method] = next;
                changed                                 = true;
            }
        }
    }

    for (auto& method : methods) {
        auto summary           = method.summary;
        summary.kind           = currentEffects[summary.method];
        summary.constantReturn = currentConstants[summary.method];
        const auto key         = summary.method;
        auto       it          = byKey_.find(key);
        if (it != byKey_.end()) {
            summaries_[it->second] = std::move(summary);
            continue;
        }
        byKey_[key] = summaries_.size();
        summaries_.push_back(std::move(summary));
    }
}

const MethodEffectSummary* MethodEffectModel::effectOf(std::string_view owner,
                                                       std::string_view name,
                                                       std::string_view desc) const
{
    const auto it = byKey_.find(methodKeyOf(pool_, owner, name, desc));
    return it == byKey_.end() ? nullptr : &summaries_[it->second];
}

MethodEffectKind MethodEffectModel::effectKindOf(std::string_view owner, std::string_view name,
                                                 std::string_view desc) const
{
    const auto* summary = effectOf(owner, name, desc);
    return summary != nullptr ? summary->kind : MethodEffectKind::Unknown;
}

bool MethodEffectModel::isPure(std::string_view owner, std::string_view name,
                               std::string_view desc) const
{
    return effectKindOf(owner, name, desc) == MethodEffectKind::Pure;
}

bool MethodEffectModel::isReadOnlyOrPure(std::string_view owner, std::string_view name,
                                         std::string_view desc) const
{
    const MethodEffectKind kind = effectKindOf(owner, name, desc);
    return kind == MethodEffectKind::Pure || kind == MethodEffectKind::ReadOnly;
}

ConstantReturnValue MethodEffectModel::constantReturnOf(std::string_view owner,
                                                        std::string_view name,
                                                        std::string_view desc) const
{
    const auto* summary = effectOf(owner, name, desc);
    return summary != nullptr ? summary->constantReturn : ConstantReturnValue{};
}

}   // namespace mjbase::analysis
