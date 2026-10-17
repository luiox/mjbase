#pragma once

#include <string>

#include "expr_node.hpp"

namespace mjbase::expr {

/// Parse an infix expression string into an expr AST.
/// Supports:
///   - Integer literals (decimal: 42, hex: 0xFF)
///   - Variables (identifiers: x, abc)
///   - Binary: + - * / % & | ^ << >> >>>
///   - Unary:  - ~
///   - Parentheses for grouping
///   - Standard C operator precedence
///
/// On parse error, returns nullptr and sets errorMsg.
ExprRef parseExpression(const std::string& input, std::string* errorMsg = nullptr);

}   // namespace mjbase::expr
