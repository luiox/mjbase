#include "mjbase/pass/unreachable_insn_remover.hpp"

#include <algorithm>
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

}   // namespace

mjbase::spi::PreservedAnalyses UnreachableInsnRemover::run(mjbase::spi::MethodScope&          scope,
                                                           mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit& cu = scope.code;

    const std::vector<InsnId> order = cu.dom.order();
    const ca::usize           n     = order.size();
    if (n == 0) return mjbase::spi::PreservedAnalyses::all();

    // 帧可达性一次：失败即全方法放弃（fail closed）。
    jbc::analysis::FrameEngine          engine(scope.session.backing().cp);
    std::vector<jbc::analysis::FrameAt> frames;
    const jbc::analysis::FrameSummary   summary = engine.compute(cu, frames);
    if (!summary.ok || frames.size() != n) return mjbase::spi::PreservedAnalyses::all();

    // 异常表标签全集（start/end/handler 任一身份都算）：锚到待删指令即保守
    // 保留该指令，不做边界重锚；死保护区连同表项的清理归异常面清理批次。
    std::vector<LabelId> excLabels;
    excLabels.reserve(cu.exc.size() * 3);
    for (const jbc::ir::CodeUnit::Exc& e : cu.exc) {
        excLabels.push_back(e.start);
        excLabels.push_back(e.end);
        excLabels.push_back(e.handler);
    }
    const auto isExcLabel = [&excLabels](LabelId l) {
        return std::find(excLabels.begin(), excLabels.end(), l) != excLabels.end();
    };

    // 分支/switch 目标标签 → 引用者（帧传播保证可达分支不会指向不可达指令；
    // 待删集内部的互引由批删原语忽略，只有集外引用才阻断）。
    std::vector<std::pair<LabelId, InsnId>> branchRefs;
    for (const InsnId id : order) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) continue;
        if (in->target != jbc::ir::kNoLabel) branchRefs.emplace_back(in->target, id);
        for (const LabelId l : in->targets) {
            if (l != jbc::ir::kNoLabel) branchRefs.emplace_back(l, id);
        }
    }

    // 两阶段选集（集外引用判定对完整候选集做，不依赖扫描顺序）：候选 =
    // 不可达且不锚异常表标签；过滤 = 不锚候选集之外的分支引用标签。
    std::vector<InsnId> doomed;
    {
        std::vector<InsnId> candidates;
        for (ca::usize i = 0; i < n; ++i) {
            if (frames[i].reachable) continue;
            const InsnId id = order[i];
            bool         excAnchored = false;
            for (const LabelId l : cu.dom.labelsAt(id)) {
                if (isExcLabel(l)) {
                    excAnchored = true;
                    break;
                }
            }
            if (!excAnchored) candidates.push_back(id);
        }
        const auto inCandidates = [&candidates](InsnId id) {
            return std::find(candidates.begin(), candidates.end(), id) != candidates.end();
        };
        for (const InsnId id : candidates) {
            bool externalRef = false;
            for (const LabelId l : cu.dom.labelsAt(id)) {
                for (const auto& [label, referrer] : branchRefs) {
                    if (label == l && !inCandidates(referrer)) {
                        externalRef = true;
                        break;
                    }
                }
                if (externalRef) break;
            }
            if (!externalRef) doomed.push_back(id);
        }
    }
    if (doomed.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 无引用标签（可能为 Boundary 角色）批删前销毁——live 标签锚在待删指令
    // 上会被批删以 StillUsed 拒收；集内互引的分支标签保持存活进批（批删
    // 原语忽略批内互引），成功后销毁。
    std::vector<LabelId> inBatchLabels;
    const auto isBranchRef = [&branchRefs](LabelId l) {
        for (const auto& ref : branchRefs) {
            if (ref.first == l) return true;
        }
        return false;
    };
    for (const InsnId id : doomed) {
        for (const LabelId l : cu.dom.labelsAt(id)) {
            if (isBranchRef(l)) inBatchLabels.push_back(l);
            else (void)cu.dom.destroyLabel(l);
        }
    }

    // kRejectIfUsed（纯批外引用校验）：待删集经异常标签门与集外引用门
    // 过滤后不存在存活引用者，不需要承接者重定向——顺带正确处理死块延伸
    // 到方法末尾（无「批后承接者」可用）的形态。
    jbc::ir::CodeEditor editor(cu);
    if (editor.eraseBatch(doomed, jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed)
            .is_err()) {
        // 批外引用被拒（防御门失效形状）：指令面未变（已销毁标签均无引用、
        // 不物化进字节码），写回字节等价，按全保留返回。
        return mjbase::spi::PreservedAnalyses::all();
    }
    for (const LabelId l : inBatchLabels) {
        (void)cu.dom.destroyLabel(l);
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
