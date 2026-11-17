#include "control_flow.hpp"

#include <algorithm>
#include <utility>

namespace mjbase::ast {
namespace {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

void sortUnique(std::vector<BlockId>& blocks)
{
    std::sort(blocks.begin(), blocks.end(),
              [](BlockId lhs, BlockId rhs) { return lhs.index < rhs.index; });
    const auto end = std::unique(blocks.begin(), blocks.end(),
                                 [](BlockId lhs, BlockId rhs) { return lhs.index == rhs.index; });
    blocks.erase(end, blocks.end());
}

}   // namespace

Result<ControlFlowGraph, ControlFlowError> ControlFlowGraph::build(const MethodBody& body)
{
    Adjacency successors(body.blocks.size());
    for (ca::usize i = 0; i < body.blocks.size(); ++i) {
        const auto& block = body.blocks[i];
        auto& targets = successors[i];
        if (const auto* jump = std::get_if<Jump>(&block.terminator)) {
            targets.push_back(jump->target);
        } else if (const auto* branch = std::get_if<Branch>(&block.terminator)) {
            targets.push_back(branch->whenTrue);
            targets.push_back(branch->whenFalse);
        }
        for (const auto& handler : block.handlers) {
            targets.push_back(handler.target);
        }
    }
    return fromSuccessors(std::move(successors));
}

Result<ControlFlowGraph, ControlFlowError> ControlFlowGraph::fromSuccessors(Adjacency successors)
{
    for (auto& targets : successors) {
        for (BlockId target : targets) {
            if (target.index >= successors.size()) return Err(ControlFlowError::InvalidTarget);
        }
        sortUnique(targets);
    }
    return Ok(ControlFlowGraph(std::move(successors)));
}

const std::vector<BlockId>& ControlFlowGraph::successors(BlockId block) const noexcept
{
    static const std::vector<BlockId> empty;
    return block.index < successors_.size() ? successors_[block.index] : empty;
}

CfgTraversal::CfgTraversal(const ControlFlowGraph& graph)
    : reachable_(graph.blockCount(), 0)
{
    if (reachable_.empty()) return;

    struct Frame
    {
        BlockId block;
        ca::usize next = 0;
    };
    std::vector<Frame> stack;
    const auto visit = [&](BlockId block) {
        reachable_[block.index] = 1;
        preorder_.push_back(block);
        stack.push_back(Frame{block});
    };
    visit(BlockId{0});
    while (!stack.empty()) {
        auto& frame = stack.back();
        const auto& targets = graph.successors(frame.block);
        if (frame.next < targets.size()) {
            const BlockId child = targets[frame.next++];
            if (!isReachable(child)) visit(child);
        } else {
            postorder_.push_back(frame.block);
            stack.pop_back();
        }
    }
    reversePostorder_.assign(postorder_.rbegin(), postorder_.rend());
}

bool CfgTraversal::isReachable(BlockId block) const noexcept
{
    return block.index < reachable_.size() && reachable_[block.index] != 0;
}

DominatorTree::DominatorTree(const ControlFlowGraph& graph)
    : idoms_(graph.blockCount()), frontiers_(graph.blockCount())
{
    if (idoms_.empty()) return;
    const CfgTraversal traversal(graph);
    const auto& order = traversal.reversePostorder();

    std::vector<ca::usize> orderIndex(graph.blockCount());
    for (ca::usize i = 0; i < order.size(); ++i) {
        orderIndex[order[i].index] = i;
    }
    ControlFlowGraph::Adjacency predecessors(graph.blockCount());
    for (BlockId source : order) {
        for (BlockId target : graph.successors(source)) {
            predecessors[target.index].push_back(source);
        }
    }

    // Cooper–Harvey–Kennedy iteration. Only predecessor chains established by an
    // earlier visit are eligible for intersection, and every chain ends at entry.
    const BlockId entry{0};
    idoms_[entry.index] = entry;
    const auto intersect = [&](BlockId lhs, BlockId rhs) {
        while (lhs.index != rhs.index) {
            while (orderIndex[lhs.index] > orderIndex[rhs.index]) lhs = *idoms_[lhs.index];
            while (orderIndex[rhs.index] > orderIndex[lhs.index]) rhs = *idoms_[rhs.index];
        }
        return lhs;
    };
    bool changed;
    do {
        changed = false;
        for (ca::usize i = 1; i < order.size(); ++i) {
            const BlockId block = order[i];
            std::optional<BlockId> parent;
            for (BlockId predecessor : predecessors[block.index]) {
                if (!idoms_[predecessor.index]) continue;
                parent = parent ? intersect(*parent, predecessor) : predecessor;
            }
            auto& previous = idoms_[block.index];
            if (parent && (!previous || previous->index != parent->index)) {
                previous = parent;
                changed = true;
            }
        }
    } while (changed);

    for (BlockId merge : order) {
        const bool entryMerge = merge.index == entry.index;
        if (!entryMerge && predecessors[merge.index].size() < 2) continue;
        for (BlockId runner : predecessors[merge.index]) {
            while (entryMerge || runner.index != idoms_[merge.index]->index) {
                frontiers_[runner.index].push_back(merge);
                const auto parent = *idoms_[runner.index];
                if (runner.index == parent.index) break;
                runner = parent;
            }
        }
    }
    for (auto& frontier : frontiers_) sortUnique(frontier);
}

bool DominatorTree::isReachable(BlockId block) const noexcept
{
    return idom(block).has_value();
}

std::optional<BlockId> DominatorTree::idom(BlockId block) const noexcept
{
    return block.index < idoms_.size() ? idoms_[block.index] : std::nullopt;
}

bool DominatorTree::dominates(BlockId dominator, BlockId block) const noexcept
{
    if (!isReachable(dominator) || !isReachable(block)) return false;
    while (block.index != dominator.index) {
        const auto parent = *idoms_[block.index];
        if (parent.index == block.index) return false;
        block = parent;
    }
    return true;
}

const std::vector<BlockId>& DominatorTree::frontier(BlockId block) const noexcept
{
    static const std::vector<BlockId> empty;
    return block.index < frontiers_.size() ? frontiers_[block.index] : empty;
}

}   // namespace mjbase::ast
