/// @file        cfg_analysis.cpp
/// @brief       将 JVM CFG 投影给共享块图分析，保留旧块句柄接口。
#include "cfg_analysis.hpp"

#include <utility>

#include "mjbase/ast/control_flow.hpp"

namespace mjbase::asm_cfg {
namespace {

ca::core::Result<ast::ControlFlowGraph, ast::ControlFlowError>
sharedGraphOf(const ControlFlowGraph& cfg)
{
    ast::ControlFlowGraph::Adjacency adjacency(cfg.blockCount());
    for (const auto& block : cfg.blocks()) {
        auto& targets = adjacency[block.id.index];
        const auto& edges = cfg.succEdgesOf(block.id);
        targets.reserve(edges.size());
        for (const auto& edge : edges) {
            targets.push_back(ast::BlockId{edge.target.index});
        }
    }
    return ast::ControlFlowGraph::fromSuccessors(std::move(adjacency));
}

std::vector<BlockId> jvmBlocks(const std::vector<ast::BlockId>& blocks)
{
    std::vector<BlockId> result;
    result.reserve(blocks.size());
    for (ast::BlockId block : blocks) result.push_back(BlockId{static_cast<ca::u32>(block.index)});
    return result;
}

}   // namespace

CfgTraversal::CfgTraversal(const ControlFlowGraph& cfg)
    : reachable_(cfg.blockCount(), 0)
{
    auto graph = sharedGraphOf(cfg);
    if (graph.is_err()) return;
    const ast::CfgTraversal traversal(std::move(graph).unwrap());
    preorder_ = jvmBlocks(traversal.preorder());
    postorder_ = jvmBlocks(traversal.postorder());
    rpo_ = jvmBlocks(traversal.reversePostorder());
    for (BlockId block : preorder_) reachable_[block.index] = 1;
}

bool CfgTraversal::isReachable(BlockId block) const noexcept
{
    return block.valid() && block.index < reachable_.size() && reachable_[block.index] != 0;
}

void DominatorTree::compute(const ControlFlowGraph& cfg)
{
    const ca::usize n = cfg.blockCount();
    reachable_.assign(n, 0);
    idoms_.assign(n, kNoBlock);
    frontiers_.assign(n, {});
    auto graph = sharedGraphOf(cfg);
    if (graph.is_err()) return;
    const ast::DominatorTree tree(std::move(graph).unwrap());
    for (ca::usize i = 0; i < n; ++i) {
        const ast::BlockId block{i};
        const auto parent = tree.idom(block);
        if (!parent) continue;
        reachable_[i] = 1;
        idoms_[i] = BlockId{static_cast<ca::u32>(parent->index)};
        frontiers_[i] = jvmBlocks(tree.frontier(block));
    }
}

bool DominatorTree::isReachable(BlockId block) const noexcept
{
    return block.valid() && block.index < reachable_.size() && reachable_[block.index] != 0;
}

BlockId DominatorTree::idom(BlockId block) const noexcept
{
    return isReachable(block) ? idoms_[block.index] : kNoBlock;
}

bool DominatorTree::dominates(BlockId dominator, BlockId block) const noexcept
{
    if (!isReachable(dominator) || !isReachable(block)) return false;
    while (block != dominator) {
        const auto parent = idoms_[block.index];
        if (!parent.valid() || parent == block) return false;
        block = parent;
    }
    return true;
}

const std::vector<BlockId>& DominatorTree::frontier(BlockId block) const noexcept
{
    static const std::vector<BlockId> kEmpty;
    return block.valid() && block.index < frontiers_.size() ? frontiers_[block.index] : kEmpty;
}

}   // namespace mjbase::asm_cfg
