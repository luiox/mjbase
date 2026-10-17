/// @file        mini_dsl_parser.hpp
/// @brief       mini DSL 文本 → Rule（语法层，不接触字节码语义）。
#pragma once

#include <string_view>
#include <vector>

#include "libca/core/result.hpp"

#include "match_error.hpp"
#include "mini_dsl_ast.hpp"

namespace mjbase::asm_matcher::dsl {

struct MiniDslParser
{
    /// 解析单条规则文本（"match…=> replace…"）；语法错误 Err(DslSyntax)。
    static ca::core::Result<Rule, MatcherError> parseRule(std::string_view dsl);
    /// 解析多规则文本：'---'（三个及以上连续短横线）整行分隔的规则块。
    static ca::core::Result<std::vector<Rule>, MatcherError> parseRules(
        std::string_view input);
};

}   // namespace mjbase::asm_matcher::dsl
