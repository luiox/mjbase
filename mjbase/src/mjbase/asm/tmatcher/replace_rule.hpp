/// @file        replace_rule.hpp
/// @brief       替换规则骨架：命中视图 + 替换序列产出器 + 替换计划。
///
/// DSL 只产出**计划**，不执行编辑。
/// ReplacePlan 携带 order() 下标窗口与一组替换 Insn 值；编辑由调用方经
/// CodeDom 原语执行。推荐执行配方（对单个计划，且计划基于当前 order()）：
///
///   1. 先收集窗口句柄：window = order()[start .. endExclusive)（值拷贝）；
///   2. 对 replacement 逐条 `insertBefore(window[0], in)`（锚点用新句柄
///      继续前插可保持顺序：首插在 window[0] 前，其后插在前一条新句柄后
///      也等价于继续 insertBefore(window[0])）；
///   3. 对 window 内句柄逐条 erase；若外部分支仍指向窗口内指令，
///      erase 返回 StillUsed，调用方须先 replaceAllUses 归零引用。
///
/// 替换 Insn 是值：branch/switch 目标携带的 LabelId 必须属于目标
/// CodeDom 且仍有效（append 时校验）；标签重定向/remap 由调用方负责。
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/code_dom.hpp"

#include "match_state.hpp"
#include "match_step.hpp"

namespace mjbase::asm_tmatcher {

/// @brief 一次命中的只读视图（替换产出器的输入）。
///
/// 产出器只读命中窗口与捕获表，不原地收集节点。
struct ReplaceContext
{
    const jbc::ir::CodeDom* dom = nullptr;         ///< 被匹配的 DOM
    ca::i32                 start = -1;            ///< 命中窗口起点（含，order() 下标）
    ca::i32                 endExclusive = -1;     ///< 命中窗口终点（不含）
    const CaptureStore*     captures = nullptr;    ///< 本次命中的捕获表

    /// @brief 取捕获绑定的指令句柄；未绑定返回 kNoInsn。
    jbc::ir::InsnId captured(std::string_view name) const
    {
        return captures != nullptr ? captures->get(name) : jbc::ir::kNoInsn;
    }

    /// @brief 取捕获绑定指令的只读视图；未绑定/失效返回 nullptr。
    const jbc::ir::Insn* capturedInsn(std::string_view name) const
    {
        const jbc::ir::InsnId id = captured(name);
        if (id == jbc::ir::kNoInsn || dom == nullptr) {
            return nullptr;
        }
        return dom->peek(id);
    }

    /// @brief 命中窗口内第 i 条（0-based）指令的只读视图；越界返回 nullptr。
    const jbc::ir::Insn* origin(std::size_t i) const
    {
        if (dom == nullptr || start < 0 || endExclusive < start) {
            return nullptr;
        }
        const ca::i32 index = start + static_cast<ca::i32>(i);
        if (index < start || index >= endExclusive ||
            index >= static_cast<ca::i32>(dom->size())) {
            return nullptr;
        }
        return dom->peek(dom->order()[static_cast<ca::usize>(index)]);
    }

    /// @brief 拷贝命中窗口内 [begin, end)（0-based，相对窗口）的指令值。
    std::vector<jbc::ir::Insn> originRange(std::size_t begin, std::size_t end) const
    {
        std::vector<jbc::ir::Insn> out;
        for (std::size_t i = begin; i < end; i++) {
            const jbc::ir::Insn* in = origin(i);
            if (in != nullptr) {
                out.push_back(*in);
            }
        }
        return out;
    }
};

/// @brief 替换产出器：由命中视图产出一组替换 Insn 值（空组 = 删除窗口）。
using ReplaceProducer = std::function<std::vector<jbc::ir::Insn>(const ReplaceContext&)>;

/// @brief 替换计划：一个命中窗口 + 产出器给出的替换序列。
///
/// start/endExclusive 是产出计划时 order() 的下标；执行前若发生结构
/// 变更，下标即失效——计划应在产出后立即按序执行（或按窗口从右往左
/// 执行以免疫左移）。
struct ReplacePlan
{
    ca::i32                      start = -1;
    ca::i32                      endExclusive = -1;
    std::vector<jbc::ir::Insn>   replacement;

    /// @brief 窗口长度（被替换的原指令条数）。
    ca::i32 length() const noexcept { return endExclusive - start; }
};

/// @brief 替换规则 = 命名 step 树 + 可选替换产出器。
///
/// producer 为空时该规则仅参与匹配（命中区间被跳过，不产出计划）。
class ReplaceRule
{
    std::string_view name_;
    StepPtr          step_;
    ReplaceProducer  producer_;

public:
    ReplaceRule(std::string_view name, StepPtr step, ReplaceProducer producer = nullptr)
        : name_(name)
        , step_(std::move(step))
        , producer_(std::move(producer))
    {}

    ReplaceRule(const ReplaceRule&)            = delete;
    ReplaceRule& operator=(const ReplaceRule&) = delete;
    ReplaceRule(ReplaceRule&&) noexcept        = default;
    ReplaceRule& operator=(ReplaceRule&&) noexcept = default;

    std::string_view name() const { return name_; }
    IMatchStep&      step() const { return *step_; }

    bool                 hasProducer() const { return producer_ != nullptr; }
    const ReplaceProducer& producer() const { return producer_; }
};

}   // namespace mjbase::asm_tmatcher
