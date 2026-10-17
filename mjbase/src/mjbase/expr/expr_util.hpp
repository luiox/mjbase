#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "expr_node.hpp"

namespace mjbase::expr {

/// Tree depth (longest path root-to-leaf)
ca::u32 depth(const ExprNode& node);

/// Total node count
ca::u32 nodeCount(const ExprNode& node);

/// Collect all variable names
std::unordered_set<std::string> collectVars(const ExprNode& node);

/// Sub-expression at the given path (indices into children: 0=left, 1=right, etc.)
/// Empty path returns the node itself.
ExprRef subExprAt(const ExprNode& node, const std::vector<ca::u32>& path);

/// Replace sub-expression at path with the replacement.
/// Returns the new tree root (original is unchanged — immutable model).
ExprRef replaceAt(ExprRef node, const std::vector<ca::u32>& path, ExprRef replacement);

}   // namespace mjbase::expr
