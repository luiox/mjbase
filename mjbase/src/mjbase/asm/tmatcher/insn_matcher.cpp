/// @file        insn_matcher.cpp
/// @brief       匹配引擎实现：滑窗扫描入口。
#include "insn_matcher.hpp"

namespace mjbase::asm_tmatcher {

std::optional<MatchedArea> InsnMatcher::findFirst(const jbc::ir::CodeDom& dom,
                                                  MatchPreference         pref)
{
    const ca::i32 size = static_cast<ca::i32>(dom.size());
    for (ca::i32 start = 0; start < size; start++) {
        auto hit = findWinner(sequences_,
                              dom,
                              start,
                              pref,
                              traceEnabled_,
                              traceEnabled_ ? &lastTrace_ : nullptr);
        if (hit.has_value()) {
            return hit;
        }
    }
    return std::nullopt;
}

MatchResult InsnMatcher::findAll(const jbc::ir::CodeDom& dom)
{
    MatchResult   result;
    const ca::i32 size = static_cast<ca::i32>(dom.size());
    for (ca::i32 i = 0; i < size;) {
        auto fw = findWinner(sequences_,
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
