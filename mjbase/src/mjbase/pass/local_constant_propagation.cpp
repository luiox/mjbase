#include "mjbase/pass/local_constant_propagation.hpp"

#include <unordered_map>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::Insn;
using jbc::ir::InsnId;

/// 追踪槽的常量类别（槽族严格匹配的依据）。
enum class TrackKind : ca::u8
{
    Int,
    Long,
    Float,
    Double,
    Null,   ///< 仅 ACONST_NULL（引用槽唯一放行形态）
};

struct Track
{
    TrackKind kind = TrackKind::Int;
    ca::i32   i    = 0;
    ca::i64   l    = 0;
    float     f    = 0.0f;
    double    d    = 0.0;
};

/// load 族 →（槽位，类别）；非 load 族返回 false。快捷形 ILOAD_0..ALOAD_3
/// 槽位与类别由 opcode 数值隐含（每类 4 个连续编码）。
bool classifyLoad(const Insn& in, ca::u32& slot, TrackKind& kind)
{
    const ca::u32 op = static_cast<ca::u32>(in.op);
    switch (in.op) {
    case Op::ILOAD: slot = static_cast<ca::u32>(in.a); kind = TrackKind::Int; return true;
    case Op::LLOAD: slot = static_cast<ca::u32>(in.a); kind = TrackKind::Long; return true;
    case Op::FLOAD: slot = static_cast<ca::u32>(in.a); kind = TrackKind::Float; return true;
    case Op::DLOAD: slot = static_cast<ca::u32>(in.a); kind = TrackKind::Double; return true;
    case Op::ALOAD: slot = static_cast<ca::u32>(in.a); kind = TrackKind::Null; return true;
    default: break;
    }
    constexpr ca::u32 kShorthandLo = static_cast<ca::u32>(Op::ILOAD_0);
    constexpr ca::u32 kShorthandHi = static_cast<ca::u32>(Op::ALOAD_3);
    if (op < kShorthandLo || op > kShorthandHi) return false;
    slot            = (op - kShorthandLo) % 4;
    const ca::u32 c = (op - kShorthandLo) / 4;
    kind            = static_cast<TrackKind>(c);   ///< Int/Long/Float/Double/Null 序一致
    return true;
}

/// 控制流边界：直线段在此终结，追踪整体清空。旧栈清空集合的载体重述
/// （LABEL 边界经标签锚定判据另行处理，FRAME/LINE 新载体不进指令序）。
bool isFlowBoundary(const Insn& in) noexcept
{
    switch (in.op) {
    case Op::GOTO:
    case Op::GOTO_W:
    case Op::JSR:
    case Op::JSR_W:
    case Op::RET:
    case Op::ATHROW:
    case Op::TABLESWITCH:
    case Op::LOOKUPSWITCH:
    case Op::IFNULL:
    case Op::IFNONNULL:
    case Op::IF_ACMPEQ:
    case Op::IF_ACMPNE:
    case Op::IRETURN:
    case Op::LRETURN:
    case Op::FRETURN:
    case Op::DRETURN:
    case Op::ARETURN:
    case Op::RETURN: return true;
    default: return (in.op >= Op::IFEQ && in.op <= Op::IFLE)
                || (in.op >= Op::IF_ICMPEQ && in.op <= Op::IF_ICMPLE);
    }
}

/// store 紧邻前驱是否为匹配槽族的常量装载；命中时产出追踪值。
bool constProducerOf(const jbc::ir::ClassSession& session, const Insn& prev, Op storeOp,
                     Track& out)
{
    switch (storeOp) {
    case Op::ISTORE:
    case Op::ISTORE_0:
    case Op::ISTORE_1:
    case Op::ISTORE_2:
    case Op::ISTORE_3:
        out.kind = TrackKind::Int;
        return method_rewrite_util::intValueOf(session, prev, out.i);
    case Op::LSTORE:
    case Op::LSTORE_0:
    case Op::LSTORE_1:
    case Op::LSTORE_2:
    case Op::LSTORE_3:
        out.kind = TrackKind::Long;
        return method_rewrite_util::longValueOf(session, prev, out.l);
    case Op::FSTORE:
    case Op::FSTORE_0:
    case Op::FSTORE_1:
    case Op::FSTORE_2:
    case Op::FSTORE_3:
        out.kind = TrackKind::Float;
        return method_rewrite_util::floatValueOf(session, prev, out.f);
    case Op::DSTORE:
    case Op::DSTORE_0:
    case Op::DSTORE_1:
    case Op::DSTORE_2:
    case Op::DSTORE_3:
        out.kind = TrackKind::Double;
        return method_rewrite_util::doubleValueOf(session, prev, out.d);
    case Op::ASTORE:
    case Op::ASTORE_0:
    case Op::ASTORE_1:
    case Op::ASTORE_2:
    case Op::ASTORE_3:
        if (prev.op != Op::ACONST_NULL) return false;
        out.kind = TrackKind::Null;
        return true;
    default: return false;
    }
}

/// 把装载指令原位改写为按值物化的常量生产者（srcPc 影线保持原位）。
void rewriteToConst(jbc::ir::ClassSession& session, Insn* in, const Track& t)
{
    Insn next;
    switch (t.kind) {
    case TrackKind::Int: next = method_rewrite_util::materializeIntConst(session, t.i); break;
    case TrackKind::Long: next = method_rewrite_util::materializeLongConst(session, t.l); break;
    case TrackKind::Float:
        next = method_rewrite_util::materializeFloatConst(session, t.f);
        break;
    case TrackKind::Double:
        next = method_rewrite_util::materializeDoubleConst(session, t.d);
        break;
    case TrackKind::Null: next = jbc::ir::CodeEditor::simple(Op::ACONST_NULL); break;
    }
    const ca::u32 srcPc = in->srcPc;
    *in                 = next;
    in->srcPc           = srcPc;
}

}   // namespace

mjbase::spi::PreservedAnalyses LocalConstantPropagation::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::ClassSession& session = scope.session;
    jbc::ir::CodeDom&      dom     = scope.code.dom;

    // 单遍直线扫描即不动点：改写只把装载变为常量生产者（只加强后续追踪），
    // 槽值变化仅由 store/IINC 驱动；快照句柄全程有效（原位改写不动结构）。
    const std::vector<InsnId> ids = dom.order();

    std::unordered_map<ca::u32, Track> tracked;
    bool                               modified = false;
    for (ca::usize i = 0; i < ids.size(); ++i) {
        // segment 断裂（旧栈 LabelNode/FRAME 前置的等价判据）：整体清空。
        if (method_rewrite_util::labelsAnchorAt(dom, ids[i])) {
            tracked.clear();
            continue;
        }
        const Insn* in = dom.peek(ids[i]);
        if (in == nullptr) continue;

        if (isFlowBoundary(*in)) {
            tracked.clear();
            continue;
        }

        if (in->op == Op::IINC) {
            const auto it = tracked.find(static_cast<ca::u32>(in->a));
            if (it != tracked.end() && it->second.kind == TrackKind::Int) {
                // JVM iinc 语义：32 位定宽回绕加法（无符号域计算规避 UB）。
                const ca::u32 sum = static_cast<ca::u32>(it->second.i)
                                    + static_cast<ca::u32>(in->b);
                it->second.i = static_cast<ca::i32>(sum);
            }
            else {
                tracked.erase(static_cast<ca::u32>(in->a));
            }
            continue;
        }

        ca::u32 base = 0;
        ca::u32 span = 0;
        if (method_rewrite_util::storeLocalSpan(*in, base, span)) {
            tracked.erase(base);
            if (span == 2) tracked.erase(base + 1);
            // 追踪登记：紧邻前驱是匹配槽族的常量装载（i>0 且当前边界无标签
            // 才能到达此处——标签锚定已在本轮头部清空并跳过）。
            if (i > 0) {
                const Insn* prev = dom.peek(ids[i - 1]);
                if (prev != nullptr) {
                    Track t;
                    if (constProducerOf(session, *prev, in->op, t)) tracked.emplace(base, t);
                }
            }
            continue;
        }

        ca::u32   slot = 0;
        TrackKind kind = TrackKind::Int;
        if (!classifyLoad(*in, slot, kind)) continue;
        const auto it = tracked.find(slot);
        if (it == tracked.end() || it->second.kind != kind) continue;
        auto got = dom.get(ids[i]);
        if (got.is_err()) continue;
        rewriteToConst(session, std::move(got).unwrap(), it->second);
        modified = true;
    }

    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
