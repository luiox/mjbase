#include "mjbase/pass/unreachable_insn_remover.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "jbc/analysis/frame.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/ast/control_flow.hpp"
#include "mjbase/ast/jvm.hpp"

namespace mjbase::pass {
namespace {

using jbc::ir::InsnId;
using jbc::ir::LabelId;

std::optional<std::vector<ca::u8>> astReachability(spi::MethodScope& scope,
                                                const std::vector<InsnId>& order)
{
    // FrameEngine seeds every handler, even when its protected range is unreachable.
    if (order.empty() || !scope.code.exc.empty()) return std::nullopt;
    const auto* method = scope.session.unit().method(scope.method);
    if (!method || method->code.get() != &scope.code) return std::nullopt;
    KeyPool keys;
    auto lifted = ast::liftJvmMethodWithSources(scope.session, scope.method, keys);
    if (lifted.is_err()) return std::nullopt;
    const auto projection = std::move(lifted).unwrap();
    auto graph = ast::ControlFlowGraph::build(projection.body);
    if (graph.is_err() || projection.sources.empty()
        || projection.sources.front().entry != order.front()) {
        return std::nullopt;
    }

    const ast::CfgTraversal traversal(std::move(graph).unwrap());
    std::vector<ca::u8> reachable;
    reachable.reserve(order.size());
    ca::usize block = 0;
    for (InsnId insn : order) {
        if (block + 1 < projection.sources.size()
            && projection.sources[block + 1].entry == insn) {
            ++block;
        }
        reachable.push_back(traversal.isReachable(ast::BlockId{block}));
    }
    if (block + 1 != projection.sources.size()) return std::nullopt;
    return reachable;
}

}   // namespace

mjbase::spi::PreservedAnalyses UnreachableInsnRemover::run(mjbase::spi::MethodScope&          scope,
                                                           mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit& cu = scope.code;

    const std::vector<InsnId> order = cu.dom.order();
    const ca::usize           n     = order.size();
    if (n == 0) return mjbase::spi::PreservedAnalyses::all();

    auto reachable = astReachability(scope, order);
    if (reachable && std::all_of(reachable->begin(), reachable->end(), [](ca::u8 live) {
            return live != 0;
        })) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    // 发生实际删除时仍检查 JVM 帧。提升失败、异常方法或两种可达结果不一致时
    // 使用原有帧可达性。
    jbc::analysis::FrameEngine          engine(scope.session.backing().cp);
    std::vector<jbc::analysis::FrameAt> frames;
    const jbc::analysis::FrameSummary   summary = engine.compute(cu, frames);
    if (!summary.ok || frames.size() != n) return mjbase::spi::PreservedAnalyses::all();
    if (reachable && !std::equal(reachable->begin(), reachable->end(), frames.begin(),
                                 [](ca::u8 live, const jbc::analysis::FrameAt& frame) {
                                     return (live != 0) == frame.reachable;
                                 })) {
        reachable.reset();
    }

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
            if (reachable ? (*reachable)[i] != 0 : frames[i].reachable) continue;
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
