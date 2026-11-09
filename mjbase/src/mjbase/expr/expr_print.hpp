#pragma once

#include "expr_node.hpp"

#include "libca/str/utf8_string.hpp"

namespace mjbase::expr {

/// Infix notation with minimal parentheses, matching expr_parser precedence.
/// The output can be re-parsed by parseExpression losslessly at the value
/// level; bit widths are not represented in the output.
ca::str::Utf8String toInfix(const ExprNode& node);

/// Graphviz DOT format (debug / visualization)
ca::str::Utf8String toDot(const ExprNode& node);

/// S-expression: (add (var x) (const 3))
ca::str::Utf8String toSExpr(const ExprNode& node);

}   // namespace mjbase::expr
