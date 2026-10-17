/// @file        mini_dsl_compiler.hpp
/// @brief       mini DSL Rule → MatchRule（可执行匹配/替换规则）。
#pragma once

#include <string_view>

#include "libca/core/result.hpp"

#include "match_error.hpp"
#include "mini_dsl_ast.hpp"
#include "pattern_matcher.hpp"

namespace mjbase::asm_matcher::dsl {

struct MiniDslCompiler
{
    /// AST 规则 → MatchRule；opcode 名在编译期解析，Method 面编译期拒绝。
    static ca::core::Result<MatchRule, MatcherError> compile(const Rule& astRule);
    /// DSL 文本 → MatchRule（parseRule + compile）。
    static ca::core::Result<MatchRule, MatcherError> compile(std::string_view dslRule);
    static ca::core::Result<void, MatcherError> compileAndAdd(PatternMatcher& matcher,
                                                              std::string_view dslRule);
    /// 多规则文本（'---' 分隔）逐条编译并登记。
    static ca::core::Result<void, MatcherError> compileAllAndAdd(PatternMatcher& matcher,
                                                                 std::string_view dslRules);
};

}   // namespace mjbase::asm_matcher::dsl
