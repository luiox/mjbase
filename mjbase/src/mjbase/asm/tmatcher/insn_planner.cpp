/// @file        insn_planner.cpp
/// @brief       规则驱动器实现：滑窗扫描产出计划。
#include "insn_planner.hpp"

namespace mjbase::asm_tmatcher {

std::vector<ReplacePlan> InsnPlanner::plansFor(const jbc::ir::CodeDom& dom)
{
    std::vector<ReplacePlan> plans;
    const ca::i32            size = static_cast<ca::i32>(dom.size());
    for (ca::i32 i = 0; i < size;) {
        ca::usize    winnerIndex = 0;
        auto fw = findWinner(rules_,
                             dom,
                             i,
                             MatchPreference::Longest,
                             traceEnabled_,
                             traceEnabled_ ? &lastTrace_ : nullptr,
                             &winnerIndex);
        if (!fw.has_value()) {
            i++;
            continue;
        }
        const ca::i32 end = fw->endExclusive;

        // 有产出器的命中变成计划；无产出器仅跳过窗口（对齐旧 noHandler）。
        const ReplaceRule& rule = rules_[winnerIndex];
        if (rule.hasProducer()) {
            ReplaceContext ctx;
            ctx.dom          = &dom;
            ctx.start        = fw->start;
            ctx.endExclusive = fw->endExclusive;
            ctx.captures     = &fw->captures;
            plans.push_back(ReplacePlan{fw->start, fw->endExclusive, rule.producer()(ctx)});
        }
        i = end;
    }
    return plans;
}

MatchResult InsnPlanner::matchAll(const jbc::ir::CodeDom& dom)
{
    MatchResult   result;
    const ca::i32 size = static_cast<ca::i32>(dom.size());
    for (ca::i32 i = 0; i < size;) {
        auto fw = findWinner(rules_,
                             dom,
                             i,
                             MatchPreference::Longest,
                             traceEnabled_,
                             traceEnabled_ ? &lastTrace_ : nullptr);
        if (!fw.has_value()) {
            i++;
            continue;
        }
        const ca::i32 end = fw->endExclusive;
        result.add(std::move(*fw));
        i = end;
    }
    return result;
}

}   // namespace mjbase::asm_tmatcher
