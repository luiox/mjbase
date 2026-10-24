/// @file        insn_matcher.hpp
/// @brief       匹配引擎：单游标裁决 findWinner + 纯匹配器 InsnMatcher。
///
/// findWinner 是 InsnMatcher 与替换规则骨架（insn_planner）共享的调度
/// 逻辑（findLongestMatch），模板化以同时接受
/// vector<MatchSequence> 与 vector<ReplaceRule>——两者都须提供
/// name()/step()。胜出条目的容器下标经 outIndex 透出（供 planner 取回
/// 规则本体；名字可重复，不能按名字找回）。
///
/// 匹配入口语义（任务清单）：
///   - findFirst：对 order() 做滑窗扫描，返回首个命中（首个有命中的
///     起点位置；该位置上取哪个候选由 MatchPreference 决定）；
///   - findAll：全量扫描、命中区间不重叠（命中后游标跳到 endExclusive，
///     Longest 裁决），结果按序列名分组。
#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/code_dom.hpp"

#include "match_sequence.hpp"
#include "match_state.hpp"
#include "match_step.hpp"
#include "match_tracer.hpp"

namespace mjbase::asm_tmatcher {

/// @brief 命中候选的取舍偏好。
enum class MatchPreference : ca::u8 {
    Longest = 0,   ///< 同一起点上取游标最远的候选（旧 findLongestMatch 语义）
    First   = 1,   ///< 取候选列表的第一个（尊重贪婪/懒惰排列顺序）
};

/// @brief 在一组序列中找出从 start 出发的一次命中。
///
/// Longest：最长命中优先，同长按注册顺序；
/// First：第一个有命中的序列的第一个候选（懒惰量词在此生效）。
/// 零长命中（纯守卫类序列）被忽略——命中窗口必须至少覆盖一条指令。
/// tracerEnabled 时 *outTrace 接收胜出序列的匹配轨迹；
/// outIndex 非空时接收胜出条目在容器中的下标。
template<typename SeqContainer>
std::optional<MatchedArea> findWinner(const SeqContainer&      sequences,
                                      const jbc::ir::CodeDom& dom,
                                      ca::i32                 start,
                                      MatchPreference         pref,
                                      bool                    tracerEnabled,
                                      MatchTrace*             outTrace,
                                      ca::usize*              outIndex = nullptr)
{
    std::optional<MatchedArea> best;
    ca::i32                    bestLength = -1;
    MatchTrace                 bestTrace;

    ca::usize idx = 0;
    for (const auto& seq : sequences) {
        MatchTracer tracer = tracerEnabled ? MatchTracer::recording() : MatchTracer::noop();
        MatchState  init   = MatchState::initial(start);
        auto        states = seq.step().match(dom, init, tracer, 0);
        ++idx;

        if (states.empty()) {
            continue;
        }
        if (pref == MatchPreference::Longest) {
            // 防御性降序：组合子已按优先级排列，这里保证 Longest 裁决全局成立。
            std::sort(states.begin(), states.end(),
                      [](const MatchState& a, const MatchState& b) {
                          return a.cursor > b.cursor;
                      });
        }
        const MatchState& pick = states.front();
        const ca::i32     len  = pick.cursor - start;
        if (len <= 0) {
            continue;
        }

        if (!best.has_value() || (pref == MatchPreference::Longest && len > bestLength)) {
            bestLength = len;
            if (tracerEnabled) {
                bestTrace = tracer.snapshot();
            }
            if (outIndex != nullptr) {
                *outIndex = idx - 1;
            }
            best = MatchedArea{seq.name(), start, pick.cursor, pick.captures};
        }
    }

    if (best.has_value() && tracerEnabled && outTrace != nullptr) {
        *outTrace = std::move(bestTrace);
    }
    return best;
}

/// @brief 纯匹配器：只持有 MatchSequence，唯一职责是滑窗匹配返回命中区域。
///
/// 不含任何替换概念；需要「匹配 + 替换计划」的场景用 insn_planner。
/// 方法非 const：开启 trace 时会刷新 lastTrace_ 调试快照。
class InsnMatcher
{
    std::vector<MatchSequence> sequences_;
    bool                       traceEnabled_ = false;
    MatchTrace                 lastTrace_;

public:
    InsnMatcher() = default;
    ~InsnMatcher() = default;

    InsnMatcher(const InsnMatcher&)            = delete;
    InsnMatcher& operator=(const InsnMatcher&) = delete;
    InsnMatcher(InsnMatcher&&) noexcept        = default;
    InsnMatcher& operator=(InsnMatcher&&) noexcept = default;

    /// @brief 添加匹配序列。
    void addSequence(MatchSequence seq) { sequences_.push_back(std::move(seq)); }

    void         setTraceEnabled(bool enabled) { traceEnabled_ = enabled; }
    bool         isTraceEnabled() const { return traceEnabled_; }
    const MatchTrace& lastTrace() const { return lastTrace_; }

    /// @brief 滑窗扫描返回首个命中；无命中返回 nullopt。
    ///
    /// pref 决定同一起点上的候选取舍（Longest / First）；起点本身按
    /// order() 从左到右推进。
    std::optional<MatchedArea> findFirst(const jbc::ir::CodeDom& dom,
                                         MatchPreference         pref);

    /// @brief 全量匹配：遍历所有位置收集命中，区间不重叠（Longest 裁决）。
    MatchResult findAll(const jbc::ir::CodeDom& dom);
};

}   // namespace mjbase::asm_tmatcher
