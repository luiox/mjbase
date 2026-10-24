/// @file        match_sequence.hpp
/// @brief       命名匹配序列与命中结果。
///
/// 语义：
///   - MatchSequence = 名字 + 一个 step 树，作为匹配入口的最小单元；
///   - MatchedArea 用 order() 的整数下标窗口表达命中（与后续编辑解耦），
///     捕获表给出「捕获名 → InsnId」；
///   - MatchResult 按序列名分组收集全部命中。
#pragma once

#include <cstddef>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/code_dom.hpp"

#include "match_state.hpp"
#include "match_step.hpp"

namespace mjbase::asm_tmatcher {

/// @brief 命名匹配序列：一个带名字的 step 树。
///
/// 名字是借用视图（通常字面量），调用方须保证规则存活期间有效。
class MatchSequence
{
    std::string_view name_;
    StepPtr          step_;

public:
    MatchSequence(std::string_view name, StepPtr step)
        : name_(name)
        , step_(std::move(step))
    {}

    MatchSequence(const MatchSequence&)            = delete;
    MatchSequence& operator=(const MatchSequence&) = delete;
    MatchSequence(MatchSequence&&) noexcept        = default;
    MatchSequence& operator=(MatchSequence&&) noexcept = default;

    std::string_view name() const { return name_; }
    IMatchStep&      step() const { return *step_; }
};

/// @brief 命中区域：order() 下标窗口 [start, endExclusive) + 捕获表。
///
/// 下标只对匹配时的 order() 快照有意义；窗口内指令句柄可经
/// `dom.order()[start + i]` 取回（结构变更前）。
struct MatchedArea
{
    std::string_view name;          ///< 所属 MatchSequence 的名字
    ca::i32          start = -1;    ///< 命中起点（含）
    ca::i32          endExclusive = -1;   ///< 命中终点（不含）
    CaptureStore     captures;      ///< 本次命中的捕获表（捕获名 → InsnId）

    /// @brief 窗口长度（指令条数）。
    ca::i32 length() const noexcept { return endExclusive - start; }
};

/// @brief 匹配结果：每个序列名字对应其全部命中区域列表。
class MatchResult
{
    std::map<std::string_view, std::vector<MatchedArea>> areas_;

public:
    void add(MatchedArea area) { areas_[area.name].push_back(std::move(area)); }

    /// @brief 取某个序列名字下的全部命中；不存在则返回空。
    const std::vector<MatchedArea>& areasOf(std::string_view name) const
    {
        static const std::vector<MatchedArea> empty;
        auto it = areas_.find(name);
        return it != areas_.end() ? it->second : empty;
    }

    /// @brief 全部命中（按序列名分组）。
    const std::map<std::string_view, std::vector<MatchedArea>>& all() const
    {
        return areas_;
    }

    /// @brief 总命中数。
    std::size_t totalMatches() const
    {
        std::size_t n = 0;
        for (const auto& entry : areas_) {
            n += entry.second.size();
        }
        return n;
    }

    bool empty() const { return areas_.empty(); }
};

}   // namespace mjbase::asm_tmatcher
