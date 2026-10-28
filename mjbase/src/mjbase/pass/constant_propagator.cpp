#include "mjbase/pass/constant_propagator.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::DefUseEdge;
using mjbase::asm_analyzer::MethodDefUse;

/// 槽族（装载/追踪严格匹配的依据）。
enum class LocalKind : ca::u8
{
    Int,
    Long,
    Float,
    Double,
    Ref,
};

/// load 族 →（槽位，槽族）；非 load 族返回 false。
bool classifyLoad(const Insn& in, ca::u32& slot, LocalKind& kind)
{
    const ca::u32 op = static_cast<ca::u32>(in.op);
    switch (in.op) {
    case Op::ILOAD: slot = static_cast<ca::u32>(in.a); kind = LocalKind::Int; return true;
    case Op::LLOAD: slot = static_cast<ca::u32>(in.a); kind = LocalKind::Long; return true;
    case Op::FLOAD: slot = static_cast<ca::u32>(in.a); kind = LocalKind::Float; return true;
    case Op::DLOAD: slot = static_cast<ca::u32>(in.a); kind = LocalKind::Double; return true;
    case Op::ALOAD: slot = static_cast<ca::u32>(in.a); kind = LocalKind::Ref; return true;
    default: break;
    }
    constexpr ca::u32 kLo = static_cast<ca::u32>(Op::ILOAD_0);
    constexpr ca::u32 kHi = static_cast<ca::u32>(Op::ALOAD_3);
    if (op < kLo || op > kHi) return false;
    slot           = (op - kLo) % 4;
    const ca::u32 c = (op - kLo) / 4;
    kind           = static_cast<LocalKind>(c);   ///< Int/Long/Float/Double/Ref 序一致
    return true;
}

/// store 族 → 槽族（非 store 返回 false；槽位覆盖经 storeLocalSpan）。
bool storeKindOf(const Insn& in, LocalKind& kind)
{
    switch (in.op) {
    case Op::ISTORE:
    case Op::ISTORE_0:
    case Op::ISTORE_1:
    case Op::ISTORE_2:
    case Op::ISTORE_3: kind = LocalKind::Int; return true;
    case Op::LSTORE:
    case Op::LSTORE_0:
    case Op::LSTORE_1:
    case Op::LSTORE_2:
    case Op::LSTORE_3: kind = LocalKind::Long; return true;
    case Op::FSTORE:
    case Op::FSTORE_0:
    case Op::FSTORE_1:
    case Op::FSTORE_2:
    case Op::FSTORE_3: kind = LocalKind::Float; return true;
    case Op::DSTORE:
    case Op::DSTORE_0:
    case Op::DSTORE_1:
    case Op::DSTORE_2:
    case Op::DSTORE_3: kind = LocalKind::Double; return true;
    case Op::ASTORE:
    case Op::ASTORE_0:
    case Op::ASTORE_1:
    case Op::ASTORE_2:
    case Op::ASTORE_3: kind = LocalKind::Ref; return true;
    default: return false;
    }
}

/// 常量生产者按槽族取值（引用槽只认 ACONST_NULL）。
bool constValueOf(const jbc::ir::ClassSession& session, const Insn& producer, LocalKind kind,
                  ca::i32& iv, ca::i64& lv, float& fv, double& dv)
{
    switch (kind) {
    case LocalKind::Int: return method_rewrite_util::intValueOf(session, producer, iv);
    case LocalKind::Long: return method_rewrite_util::longValueOf(session, producer, lv);
    case LocalKind::Float: return method_rewrite_util::floatValueOf(session, producer, fv);
    case LocalKind::Double: return method_rewrite_util::doubleValueOf(session, producer, dv);
    case LocalKind::Ref: return producer.op == Op::ACONST_NULL;
    }
    return false;
}

/// 把装载指令原位改写为按槽族物化的常量生产者（srcPc 影线保持原位）。
void rewriteToConst(jbc::ir::ClassSession& session, Insn* in, LocalKind kind, ca::i32 iv,
                    ca::i64 lv, float fv, double dv)
{
    Insn next;
    switch (kind) {
    case LocalKind::Int: next = method_rewrite_util::materializeIntConst(session, iv); break;
    case LocalKind::Long: next = method_rewrite_util::materializeLongConst(session, lv); break;
    case LocalKind::Float:
        next = method_rewrite_util::materializeFloatConst(session, fv);
        break;
    case LocalKind::Double:
        next = method_rewrite_util::materializeDoubleConst(session, dv);
        break;
    case LocalKind::Ref: next = jbc::ir::CodeEditor::simple(Op::ACONST_NULL); break;
    }
    const ca::u32 srcPc = in->srcPc;
    *in                 = next;
    in->srcPc           = srcPc;
}

}   // namespace

mjbase::spi::PreservedAnalyses ConstantPropagator::run(mjbase::spi::MethodScope& scope,
                                                       mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeDom& dom = scope.code.dom;
    if (dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();

    // 签名播种与负向结论纪律同 def_use 契约：截断集合上的「唯一定义」
    // 不可靠，整件跳过。
    const jbc::analysis::MethodSig sig{
        (scope.access & mjbase::hierarchy::kAccStatic) != 0, scope.desc};
    const MethodDefUse du =
        mjbase::asm_analyzer::analyzeDefUse(scope.session.backing().cp, scope.code, sig);
    if (!du.ok() || du.truncated()) return mjbase::spi::PreservedAnalyses::all();

    // 常量生产者缓存：def 标号 →（已验证的物化形态）。同一 store 的多个
    // 读点共享一次验证。
    struct Prod
    {
        bool      valid = false;
        LocalKind kind  = LocalKind::Int;
        ca::i32   iv    = 0;
        ca::i64   lv    = 0;
        float     fv    = 0.0f;
        double    dv    = 0.0;
    };
    std::unordered_map<ca::u32, Prod> prodCache;   // key = store def 指令 slot

    bool modified = false;
    for (const mjbase::asm_analyzer::LocalAccess& access : du.localAccesses()) {
        if (access.kind != mjbase::asm_analyzer::LocalAccess::Kind::Read) continue;
        if (access.defs.truncated() || access.defs.size() != 1) continue;
        const DefId def = access.defs.at(0);
        if (def.kind != DefId::Kind::Instruction) continue;   // 入参定义非常量

        // 定义侧核验（缓存）：store 槽族 + 弹值定义唯一且为常量生产者。
        auto prodIt = prodCache.find(def.id);
        if (prodIt == prodCache.end()) {
            Prod p;
            const InsnId* defInsn = du.instructionOf(def);
            const Insn*   store   = defInsn != nullptr ? dom.peek(*defInsn) : nullptr;
            ca::u32       base    = 0;
            ca::u32       span    = 0;
            if (store != nullptr && method_rewrite_util::storeLocalSpan(*store, base, span)
                && base == access.local && storeKindOf(*store, p.kind)) {
                // 弹值（栈顶值）到达定义：恰一且为指令定义。
                DefId valueDef{};
                int   valueDefs = 0;
                for (const DefUseEdge* e : du.usesAt(*defInsn)) {
                    if (e->slot != mjbase::asm_analyzer::DefUseSlot::Stack
                        || e->index != 0 || e->use != *defInsn) {
                        continue;
                    }
                    if (valueDefs == 0) {
                        valueDef = e->def;
                    }
                    ++valueDefs;
                }
                const InsnId* valInsn = nullptr;
                const Insn*   valPeek = nullptr;
                if (valueDefs == 1 && valueDef.kind == DefId::Kind::Instruction
                    && (valInsn = du.instructionOf(valueDef)) != nullptr
                    && (valPeek = dom.peek(*valInsn)) != nullptr) {
                    p.valid = constValueOf(scope.session, *valPeek, p.kind, p.iv, p.lv, p.fv,
                                           p.dv);
                }
            }
            prodIt = prodCache.emplace(def.id, p).first;
        }
        if (!prodIt->second.valid) continue;

        // 装载侧：槽族严格匹配（load 的 base 槽即 access.local）。
        const Insn* load = dom.peek(access.insn);
        if (load == nullptr) continue;
        ca::u32   slot = 0;
        LocalKind kind = LocalKind::Int;
        if (!classifyLoad(*load, slot, kind) || kind != prodIt->second.kind) continue;

        auto got = dom.get(access.insn);
        if (got.is_err()) continue;
        rewriteToConst(scope.session, std::move(got).unwrap(), prodIt->second.kind,
                       prodIt->second.iv, prodIt->second.lv, prodIt->second.fv,
                       prodIt->second.dv);
        modified = true;
    }

    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
