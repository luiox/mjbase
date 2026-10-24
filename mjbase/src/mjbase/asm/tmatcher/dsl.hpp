/// @file        dsl.hpp
/// @brief       DSL 运算符层：- (seq) / | (alt) / >> (规则产出)。
///
/// 运算子集；有意不迁的部分：
///   - CaptureVar<T>/load()：依赖 InsnUtil 的常量解释；新 DSL 的
///     捕获是 InsnId，取值由产出器经 ReplaceContext::capturedInsn 完成；
///   - makeSeq/makeReplaceSeq 大括号糖：initializer_list 持 move-only
///     句柄需 const_cast 挪动，属于将就写法，不迁。
#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "jbc/ir/code_dom.hpp"

#include "combinators.hpp"
#include "match_step.hpp"
#include "replace_rule.hpp"

namespace mjbase::asm_tmatcher {

/// @brief a - b = seq(a, b)。
inline StepPtr operator-(StepPtr a, StepPtr b)
{
    std::vector<StepPtr> nodes;
    nodes.push_back(std::move(a));
    nodes.push_back(std::move(b));
    return combinators::seq(std::move(nodes));
}

/// @brief a | b = alt(a, b)。
inline StepPtr operator|(StepPtr a, StepPtr b)
{
    std::vector<StepPtr> branches;
    branches.push_back(std::move(a));
    branches.push_back(std::move(b));
    return combinators::alt(std::move(branches));
}

/// @brief step >> fn → ReplaceRule：fn 接收命中视图，返回替换 Insn 序列。
///
/// 规则名默认 "unnamed"；需要命名检索时用 ReplaceRule
/// 构造函数显式给名。
template<typename Fn>
ReplaceRule operator>>(StepPtr step, Fn&& producer)
{
    return ReplaceRule("unnamed",
                       std::move(step),
                       ReplaceProducer(std::forward<Fn>(producer)));
}

/// @brief step >> fixed → ReplaceRule：命中窗口替换为固定指令序列。
///
/// 等价 `step >> ReplaceSequence` 的静态场景；需要读捕获表的
/// 动态产出用 lambda 重载。
inline ReplaceRule operator>>(StepPtr step, std::vector<jbc::ir::Insn> fixed)
{
    return ReplaceRule(
        "unnamed",
        std::move(step),
        [fixed = std::move(fixed)](const ReplaceContext&) { return fixed; });
}

}   // namespace mjbase::asm_tmatcher
