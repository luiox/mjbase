/// @file        pattern_matcher.hpp
/// @brief       旧代 pattern matcher：线性窗口匹配 + 原子替换（值语义重建）。
///
/// 语义来源 asm/matcher/pattern_matcher.hpp，按 libjbc 底座重写：
///   - 匹配目标是 @ref jbc::ir::CodeDom 的 order 序列（旧栈是 InsnList 链表）。
///   - 旧栈「不跨越 Label/Line/Frame 边界」在新栈的对应物是窗口纪律：
///     窗口内部（首条豁免）不得有分支/switch 目标（jumpTargetMask 可判部分）。
///     Line/Frame 是 classfile 嵌套属性，不在 DOM 层出现；异常表等
///     Boundary 标签锚定不可枚举，落到 apply 时由 erase 拒绝并整体回滚。
///   - 上下文/替换全部是值拷贝：旧栈的 builder InsnList、cloneMatchedNode、
///     Utf8StringArena 注入与节点 arena 在值语义下不存在。
#pragma once

#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "jbc/ir/code_dom.hpp"

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "match_error.hpp"
#include "match_step.hpp"

namespace mjbase::asm_matcher {

/// 命名捕获的记录：命中指令的 order 下标 + 值拷贝。
struct CapturedInsn
{
    ca::usize orderIndex = 0;
    jbc::ir::Insn insn;
};

/// @brief 一次匹配的上下文：窗口信息、命中指令（值拷贝）与替换构造器。
///
/// 生命周期：只在一次策略回调期间有效；策略不得缓存其中引用。
/// 策略只看到只读面（original 是 const），改写只能通过向 replacement
/// 追加新指令值表达，由 PatternMatcher::apply 统一落盘。
class MatchContext
{
public:
    const jbc::ir::CodeDom* original = nullptr;   ///< 匹配目标（只读借用）
    ca::usize startIndex = 0;                     ///< 窗口首条 order 下标
    ca::usize endIndex = 0;                       ///< 窗口末条 order 下标（开区间）

    /// 替换指令值列表（策略写入；apply 落盘并归一化 srcPc）。
    std::vector<jbc::ir::Insn> replacement;

    /// 每个顶层步骤命中的首条指令（值拷贝）；anyMulti 只记窗口内首条。
    std::vector<jbc::ir::Insn> matchedInsns;

    /// 命中句柄（apply 落盘前有效；回滚后失效）。
    std::vector<jbc::ir::InsnId> matchedIds;

    /// 命名捕获（键借用 MatchStep::captureAs 的调用方存储）。
    std::unordered_map<std::string_view, CapturedInsn> captures;

    /// 放弃当前窗口的替换：原指令保持不变。
    void cancelReplacement() noexcept { replacementCancelled_ = true; }
    /// 查询策略是否已放弃当前匹配。
    bool isReplacementCancelled() const noexcept { return replacementCancelled_; }
    /// 每次执行策略前重置替换决策（仅供 PatternMatcher 调用）。
    void resetReplacementDecision() noexcept { replacementCancelled_ = false; }

private:
    bool replacementCancelled_ = false;
};

/// 单条规则的匹配窗口（order 下标，开区间）。
struct MatchResult
{
    ca::usize start = 0;
    ca::usize end = 0;

    bool valid() const noexcept { return end > start; }
};

/// 替换策略：读 matchedInsns/captures，向 replacement 写新指令值。
/// 返回 Err 时该窗口保持不变。
using ModifyStrategy =
    std::function<ca::core::Result<void, MatcherError>(MatchContext&)>;

/// @brief 有序步骤 + 可选替换策略；匹配失败时不改 DOM。
class MatchRule
{
public:
    MatchRule() = default;
    MatchRule(std::vector<MatchStep> steps, ModifyStrategy strategy = nullptr)
        : steps_(std::move(steps))
        , strategy_(std::move(strategy))
    {}

    /// @brief 从 order 下标 startIndex 起尝试整窗匹配。
    ///
    /// 命中时填充 ctx（窗口、matchedInsns/matchedIds、captures）并返回 true；
    /// 窗口纪律（内部无跳转目标，首条豁免）不满足视为未命中。
    bool tryMatch(const jbc::ir::CodeDom& dom, ca::usize startIndex,
                  MatchContext& ctx) const;

    void setStrategy(ModifyStrategy s) { strategy_ = std::move(s); }
    const ModifyStrategy& strategy() const noexcept { return strategy_; }
    bool hasStrategy() const noexcept { return static_cast<bool>(strategy_); }

private:
    friend class PatternMatcher;

    bool tryMatchWithMask(const jbc::ir::CodeDom& dom, ca::usize startIndex,
                          MatchContext& ctx, const std::vector<ca::u8>& targetMask) const;

    std::vector<MatchStep> steps_;
    ModifyStrategy strategy_;
};

/// order 下标 → 是否有分支/switch 目标落在该指令前置边界（窗口纪律数据）。
std::vector<ca::u8> jumpTargetMask(const jbc::ir::CodeDom& dom);

/// @brief 规则集合上的匹配与应用入口。
///
/// apply 语义照旧：每条规则从后往前定位最后一个可替换窗口，每调用一次
/// 至多替换一处；仅匹配（无策略）的规则止扫且不报告改动。调用方循环
/// apply 到 Ok(false) 即得不动点。
class PatternMatcher
{
public:
    void addRule(MatchRule rule) { rules_.push_back(std::move(rule)); }
    ca::usize ruleCount() const noexcept { return rules_.size(); }

    /// 每条规则给出首个（最小起点）命中窗口；未命中的元素 invalid。
    std::vector<MatchResult> match(const jbc::ir::CodeDom& dom) const;

    /// @brief 执行规则匹配与原子替换。
    ///
    /// 返回：Ok(true) 发生了替换；Ok(false) 没有；Err 窗口应用被 DOM
    /// 拒绝（该窗口内容已整体回滚，但指令句柄可能因回滚重插而改变；
    /// 此前已成功的其它窗口不回退）。整窗删除且首指令被窗口外分支
    /// 引用时按旧栈语义把引用重定向到顺序后继，该重定向不参与回滚。
    ca::core::Result<bool, MatcherError> apply(jbc::ir::CodeDom& dom) const;

private:
    std::vector<MatchRule> rules_;
};

}   // namespace mjbase::asm_matcher
