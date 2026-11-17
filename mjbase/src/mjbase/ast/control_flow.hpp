/// @file control_flow.hpp
/// @brief Block-level reachability and dominance for lifted method bodies.
#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "libca/core/result.hpp"

#include "method_body.hpp"

namespace mjbase::ast {

enum class ControlFlowError : ca::u8 { InvalidTarget };

/// A block graph snapshot with the same block numbering as the source body.
class ControlFlowGraph
{
public:
    using Adjacency = std::vector<std::vector<BlockId>>;

    /// Includes all normal and handler edges. Targets outside the body are rejected.
    static ca::core::Result<ControlFlowGraph, ControlFlowError> build(const MethodBody& body);

    /// Allows bytecode CFGs to use the same graph analysis before full AST lifting is available.
    static ca::core::Result<ControlFlowGraph, ControlFlowError> fromSuccessors(Adjacency successors);

    ca::usize blockCount() const noexcept { return successors_.size(); }

    /// Successors in ascending block index order, without duplicate edges.
    const std::vector<BlockId>& successors(BlockId block) const noexcept;

private:
    explicit ControlFlowGraph(Adjacency successors) : successors_(std::move(successors)) {}
    Adjacency successors_;
};

/// Iterative depth-first traversal from block zero. An empty graph has no entry.
class CfgTraversal
{
public:
    explicit CfgTraversal(const ControlFlowGraph& graph);

    bool isReachable(BlockId block) const noexcept;
    const std::vector<BlockId>& preorder() const noexcept { return preorder_; }
    const std::vector<BlockId>& postorder() const noexcept { return postorder_; }
    const std::vector<BlockId>& reversePostorder() const noexcept { return reversePostorder_; }

private:
    std::vector<ca::u8> reachable_;
    std::vector<BlockId> preorder_;
    std::vector<BlockId> postorder_;
    std::vector<BlockId> reversePostorder_;
};

/// Immediate dominators and dominance frontiers for entry-reachable blocks.
/// Entry is its own immediate dominator and has an implicit incoming method edge.
/// Consequently, a backedge to entry is included in the dominance frontiers.
class DominatorTree
{
public:
    explicit DominatorTree(const ControlFlowGraph& graph);

    bool isReachable(BlockId block) const noexcept;
    std::optional<BlockId> idom(BlockId block) const noexcept;
    bool dominates(BlockId dominator, BlockId block) const noexcept;
    const std::vector<BlockId>& frontier(BlockId block) const noexcept;

private:
    std::vector<std::optional<BlockId>> idoms_;
    std::vector<std::vector<BlockId>> frontiers_;
};

}   // namespace mjbase::ast
