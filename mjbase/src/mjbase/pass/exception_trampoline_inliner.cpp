#include "mjbase/pass/exception_trampoline_inliner.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "jbc/analysis/frame.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::pass {
namespace {

using jbc::core::Op;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

/// 标签解析结果：ok=false 表示解析失败（放弃全方法）；End 边界用
/// pos = order.size() 参与区间运算。
struct PosResolve
{
    bool     ok    = false;
    bool     isEnd = false;
    ca::usize pos  = 0;
};

ca::usize posOfInsn(const std::vector<InsnId>& order, InsnId id)
{
    return static_cast<ca::usize>(std::find(order.begin(), order.end(), id) - order.begin());
}

PosResolve resolvePos(const jbc::ir::CodeDom& dom, const std::vector<InsnId>& order, LabelId label)
{
    auto resolved = dom.resolveLabel(label);
    if (resolved.is_err()) return {};
    const jbc::ir::LabelTarget t = std::move(resolved).unwrap();
    if (t.isEnd()) return {true, true, order.size()};
    return {true, false, posOfInsn(order, t.insn)};
}

/// 单个命中的跳板：表项下标 + 三条待删指令句柄。
struct Trampoline
{
    ca::usize excIdx;
    InsnId    aconstId;
    InsnId    athrowId;
    InsnId    popId;
};

}   // namespace

mjbase::spi::PreservedAnalyses ExceptionTrampolineInliner::run(mjbase::spi::MethodScope&          scope,
                                                               mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit& cu = scope.code;
    if (cu.exc.empty()) return mjbase::spi::PreservedAnalyses::all();

    const std::vector<InsnId> order = cu.dom.order();
    const ca::usize           n     = order.size();

    // 表项边界全量解析：任一失败即整体放弃（交叠推理的前提）。
    struct EntryPos
    {
        ca::usize s, e, h;
    };
    std::vector<EntryPos> entryPos;
    entryPos.reserve(cu.exc.size());
    for (const jbc::ir::CodeUnit::Exc& e : cu.exc) {
        const PosResolve rs = resolvePos(cu.dom, order, e.start);
        const PosResolve re = resolvePos(cu.dom, order, e.end);
        const PosResolve rh = resolvePos(cu.dom, order, e.handler);
        if (!rs.ok || !re.ok || !rh.ok) return mjbase::spi::PreservedAnalyses::all();
        entryPos.push_back({rs.pos, re.pos, rh.pos});
    }

    // 分支目标标签集合与目标位置计数（athrow/pop 禁入 + 标签预检共用）。
    std::vector<ca::u32> targetCount(n, 0);
    std::vector<LabelId> branchLabels;
    for (const InsnId id : order) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) continue;
        const auto addTarget = [&](LabelId l) {
            if (l == jbc::ir::kNoLabel) return;
            branchLabels.push_back(l);
            const PosResolve r = resolvePos(cu.dom, order, l);
            if (r.ok && !r.isEnd) ++targetCount[r.pos];
        };
        addTarget(in->target);
        for (const LabelId l : in->targets) addTarget(l);
    }
    const auto isBranchLabel = [&branchLabels](LabelId l) {
        return std::find(branchLabels.begin(), branchLabels.end(), l) != branchLabels.end();
    };

    // 帧引擎一次（BestEffort 形状档，只消费深度）；失败即全方法放弃。
    jbc::analysis::FrameEngine          engine(scope.session.backing().cp);
    std::vector<jbc::analysis::FrameAt> frames;
    const jbc::analysis::FrameSummary   summary = engine.compute(cu, frames);
    if (!summary.ok || frames.size() != n) return mjbase::spi::PreservedAnalyses::all();

    std::vector<Trampoline> hits;
    for (ca::usize i = 0; i < cu.exc.size(); ++i) {
        const jbc::ir::CodeUnit::Exc& e = cu.exc[i];
        const EntryPos&               p = entryPos[i];
        if (e.catchCp != 0) continue;                  // 只认 catch-all
        if (p.h != p.e) continue;                      // handler 必须紧贴保护区末尾
        if (p.s + 2 != p.e) continue;                  // 保护区恰两条
        if (p.h + 1 >= n) continue;                    // POP 需有顺序后继
        const jbc::ir::Insn* aconst = cu.dom.peek(order[p.s]);
        const jbc::ir::Insn* athrow = cu.dom.peek(order[p.s + 1]);
        const jbc::ir::Insn* pop    = cu.dom.peek(order[p.h]);
        if (aconst == nullptr || aconst->op != Op::ACONST_NULL) continue;
        if (athrow == nullptr || athrow->op != Op::ATHROW) continue;
        if (pop == nullptr || pop->op != Op::POP) continue;
        // 其他表项：区间不相交、handler 头独占、无 handler 锚入本保护区/POP。
        bool clean = true;
        for (ca::usize j = 0; j < entryPos.size() && clean; ++j) {
            if (j == i) continue;
            const EntryPos& q = entryPos[j];
            const bool rangeIntersects = !(q.e <= p.s || q.s >= p.e);
            const bool handlerClash    = q.h == p.h || q.h == p.s || q.h == p.s + 1 ||
                                         (q.s <= p.h && p.h < q.e);
            if (rangeIntersects || handlerClash) clean = false;
        }
        if (!clean) continue;
        // athrow 与 pop 不得为分支目标（绕过 aconst 的路径栈形不符）。
        if (targetCount[p.s + 1] != 0 || targetCount[p.h] != 0) continue;
        // 入口到达帧栈深必须为 0：athrow 的清栈语义由直落替代后不可残留。
        const jbc::analysis::FrameAt& f = frames[p.s];
        if (!f.reachable || !f.stack.empty()) continue;
        // 标签预检：批内三条指令上锚定的存活标签只允许本表项边界（随后
        // 销毁）与分支目标标签（eraseBatch 重定向）；其余锚定（调试范围等
        // Boundary）不可机械迁移，整组跳过。
        const bool labelsClean =
            [&]() {
                const auto anchorClean = [&](InsnId anchor) {
                    for (const LabelId l : cu.dom.labelsAt(anchor)) {
                        if (l == e.start || l == e.end || l == e.handler) continue;
                        if (isBranchLabel(l)) continue;
                        return false;
                    }
                    return true;
                };
                return anchorClean(order[p.s]) && anchorClean(order[p.s + 1]) &&
                       anchorClean(order[p.h]);
            }();
        if (!labelsClean) continue;
        hits.push_back({i, order[p.s], order[p.s + 1], order[p.h]});
    }
    if (hits.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 表项下标降序应用：摘表项 → 销毁边界标签 → 批量删除。批删被拒不落
    // 指令变更，表项以重建标签原位回插（锚点不变，语义等价）。
    jbc::ir::CodeEditor editor(cu);
    bool                modified = false;
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        const auto                   idx   = static_cast<std::ptrdiff_t>(it->excIdx);
        const jbc::ir::CodeUnit::Exc saved = cu.exc[static_cast<ca::usize>(it->excIdx)];
        cu.exc.erase(cu.exc.begin() + idx);
        (void)cu.dom.destroyLabel(saved.start);
        (void)cu.dom.destroyLabel(saved.end);
        (void)cu.dom.destroyLabel(saved.handler);
        if (editor.eraseBatch({it->aconstId, it->athrowId, it->popId},
                              jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                .is_ok()) {
            modified = true;
            continue;
        }
        jbc::ir::CodeUnit::Exc restored = saved;
        const auto             relabel  = [&](LabelId& field, InsnId anchor) {
            auto fresh = cu.dom.createLabel(anchor);
            if (fresh.is_ok()) field = std::move(fresh).unwrap();
        };
        relabel(restored.start, it->aconstId);
        relabel(restored.end, it->popId);
        relabel(restored.handler, it->popId);
        cu.exc.insert(cu.exc.begin() + idx, restored);
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
