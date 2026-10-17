#pragma once

#include <string>
#include <unordered_map>

#include "expr_node.hpp"
#include "expr_value.hpp"

namespace mjbase::expr {

/// Variable bindings: name -> value
using VarBindings = std::unordered_map<std::string, ExprValue>;

/// Evaluation result
struct EvalResult
{
    ExprValue   value;
    bool        ok;
    std::string error;

    static EvalResult val(ExprValue v) { return {v, true, {}}; }
    static EvalResult err(std::string msg)
    {
        return {ExprValue(0, BitWidth::B32), false, std::move(msg)};
    }
};

/// Iterative evaluation — explicit stack, no recursion.
/// @param node     Root of expression tree.
/// @param vars     Variable bindings.
/// @param maxDepth Safety limit; returns error if exceeded.
EvalResult eval(const ExprNode& node, const VarBindings& vars = {}, ca::u32 maxDepth = 1024);

}   // namespace mjbase::expr
