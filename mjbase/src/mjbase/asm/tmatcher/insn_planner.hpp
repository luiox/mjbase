/// @file        insn_planner.hpp
/// @brief       规则驱动器：在规则上滑窗匹配，产出替换计划。
///
/// DSL 只产出计划、不执行编辑：
///   - plansFor：匹配 → 有产出器的命中变成 ReplacePlan；无产出器的
///     命中仅跳过窗口。扫描纪律：Longest 裁决、区间不重叠、命中后
///     游标跳到 endExclusive。
///   - matchAll：纯匹配收集命中，不产出计划、不触碰 DOM。
/// 纯匹配器由 insn_matcher.hpp 提供。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/code_dom.hpp"

#include "insn_matcher.hpp"
#include "match_sequence.hpp"
#include "match_tracer.hpp"
#include "replace_rule.hpp"

namespace mjbase::asm_tmatcher {

class InsnPlanner
{
    std::vector<ReplaceRule> rules_;
    bool                     traceEnabled_ = false;
    MatchTrace               lastTrace_;

public:
    InsnPlanner() = default;
    ~InsnPlanner() = default;

    InsnPlanner(const InsnPlanner&)            = delete;
    InsnPlanner& operator=(const InsnPlanner&) = delete;
    InsnPlanner(InsnPlanner&&) noexcept        = default;
    InsnPlanner& operator=(InsnPlanner&&) noexcept = default;

    /// @brief 添加规则（step 树 + 可选产出器）。
    void addRule(ReplaceRule rule) { rules_.push_back(std::move(rule)); }

    void         setTraceEnabled(bool enabled) { traceEnabled_ = enabled; }
    bool         isTraceEnabled() const { return traceEnabled_; }
    const MatchTrace& lastTrace() const { return lastTrace_; }

    /// @brief 匹配并产出替换计划（窗口基于当前 order() 快照，见 ReplacePlan）。
    /// @note 方法非 const：开启 trace 时会刷新 lastTrace_ 调试快照。
    std::vector<ReplacePlan> plansFor(const jbc::ir::CodeDom& dom);

    /// @brief 纯匹配：在规则上收集命中，不产出计划。
    MatchResult matchAll(const jbc::ir::CodeDom& dom);
};

}   // namespace mjbase::asm_tmatcher
