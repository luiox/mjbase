#pragma once

#include <string_view>
#include <unordered_map>

#include "expr_node.hpp"
#include "expr_value.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::expr {

/// Variable bindings: name -> value
using VarBindings = std::unordered_map<ca::str::Utf8String, ExprValue>;

/// Evaluation result
struct EvalResult
{
    ExprValue          value;
    bool               ok;
    ca::str::Utf8String error;

    static EvalResult val(ExprValue v) { return {v, true, {}}; }
    /// 错误文本经字节保真收进（消息为内部拼装的诊断串，不做编码裁决）。
    static EvalResult err(std::string_view msg)
    {
        return {ExprValue(0, BitWidth::B32), false,
                ca::str::Utf8String::from_data_unchecked(
                    reinterpret_cast<const ca::u8*>(msg.data()), msg.size())};
    }
};

/// Iterative evaluation — explicit stack, no recursion.
/// @param node     Root of expression tree.
/// @param vars     Variable bindings.
/// @param maxDepth Safety limit; returns error if exceeded.
EvalResult eval(const ExprNode& node, const VarBindings& vars = {}, ca::u32 maxDepth = 1024);

}   // namespace mjbase::expr
