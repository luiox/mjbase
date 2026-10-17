#pragma once

#include <string>

#include "expr_node.hpp"

namespace mjbase::expr {

/// Infix notation with minimal parentheses, matching expr_parser precedence.
/// The output can be re-parsed by parseExpression losslessly at the value
/// level; bit widths are not represented in the output.
std::string toInfix(const ExprNode& node);

/// Graphviz DOT format (debug / visualization)
std::string toDot(const ExprNode& node);

/// S-expression: (add (var x) (const 3))
std::string toSExpr(const ExprNode& node);

}   // namespace mjbase::expr
