#pragma once

#include <string_view>

#include "expr_node.hpp"

#include "libca/str/utf8_string.hpp"

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
ExprRef parseExpression(std::string_view input, ca::str::Utf8String* errorMsg = nullptr);

}   // namespace mjbase::expr
