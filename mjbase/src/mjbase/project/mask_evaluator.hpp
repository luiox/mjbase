/// @file        mask_evaluator.hpp
/// @brief       mask 出口求值器：规则匹配 + suspect 门 +
///              信任级别默认的统一点。
///
/// 三个文本出口（MCP 工具返回、TSV 导出、反编译文本）共用本求值器，不
/// 各写一套。核心是纯函数判定面：build() 一次性编译规则、装载信任级别，
/// evaluate() 对单条文本产出决定——无 IO、无库连接；规则加载（数据面）
/// 归 AnalysisDb（mask_rules 表），出口接线（消费决定、渲染占位符）归
/// 上层（mjt）。
///
/// 判定序（优先级/信任级别默认/suspect 压制，阈值细节测试钉死）：
///   1. suspect 扫描先行（scanSuspect，旗标进决定留痕）；
///   2. 长度门：文本超过 kMaskMaxEvaluatedBytes 不进任何正则，直接按
///      default_action（信任级别）判定，via=LengthGate——对抗性超长
///      输入的灾难回溯防线（mask_regex_engine 的长度门纪律）；
///   3. 规则按 (priority, ruleId) 升序逐条匹配，先匹配先生效：mask 命中
///      即遮蔽；allow 命中且文本无 suspect 旗标即放行；
///   4. suspect 压过 allow：allow 命中但文本带 suspect 旗标时该命中作废
///      （记首条被压 allow 的 ruleId 留痕），继续匹配后续规则——后续
///      mask 命中则按规则遮蔽，全部未命中则按 default_action 判定，
///      via=SuspectGate、overrideBlocked=true；
///   5. 无规则命中：default_action，via=DefaultAction。
///
/// 命中计数归因：规则命中（含被 suspect 压制的 allow 命中）都算该规则的
/// hit——复盘要看见"allow 规则试图放行可疑内容"这一信号；持久化由调用
/// 方经 AnalysisDb::recordMaskRuleHits 落库。
///
/// 线程模型：build() 之后的 MaskEvaluator 不可变，多线程并发 evaluate()
/// 安全；跨线程共享走 shared_ptr<const MaskEvaluator>。
///
/// 红线：本模块及其测试不出现混淆器家族名。
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/project/mask_types.hpp"

namespace mjbase::project {

/// @brief 决定来源。区分规则生效与各类兜底，出口据此渲染与留痕。
enum class MaskDecisionVia : ca::u8
{
    RuleMatch    = 0, ///< 规则命中生效（mask 或 allow）
    SuspectGate  = 1, ///< suspect 压过 allow 命中后的 default_action 兜底
    DefaultAction = 2, ///< 无规则命中，按信任级别默认
    LengthGate   = 3, ///< 超长度门，不进正则，按信任级别默认
};

/// @brief 单条文本的遮蔽决定。
struct MaskDecision final
{
    MaskAction      action = MaskAction::Allow;          ///< 最终动作
    ca::i64         ruleId = 0;                          ///< 涉及规则 id；0 = 无规则
    MaskDecisionVia via    = MaskDecisionVia::DefaultAction; ///< 决定来源
    bool            suspect = false;                     ///< 输入带任一 suspect 旗标
    /// @brief allow 规则命中被 suspect 压下（override 留痕钩子：出口侧
    ///        计数/审计日志以此字段为触发条件）。
    bool suspectOverrideBlocked = false;
};

/// @brief 求值器构建输入（从 AnalysisDb 的 mask_rules 行投影，或测试直构）。
///
/// pattern 是借用视图：build() 在返回前完成编译，视图随调用结束失效；
/// ruleId 只用于排序并列与命中归因，可重复（归因取排序序位对应的 id）。
struct MaskRuleSpec final
{
    ca::i64          ruleId   = 0;
    ca::i32          priority = 0; ///< 数字越小越先匹配
    std::string_view pattern;      ///< 正则字面量（纪律见 mask_regex_engine.hpp）
    MaskAction       action   = MaskAction::Mask;
};

/// @brief 求值器构建诊断（含序号上下文）。
struct MaskEvaluatorError
{
    ca::str::Utf8String message;
};

/// @brief mask 出口求值器（不可变判定面；move-only）。
class MaskEvaluator final
{
public:
    /// @brief 编译并装载规则集与信任级别。规则按 (priority, ruleId) 稳定
    ///        升序；任一模式编译失败 → `Err`（消息含 0 基序号与原因），
    ///        整批不产出。规则条数上限 kMaximumMaskRules（与数据面一致）。
    static ca::Result<MaskEvaluator, MaskEvaluatorError> build(
        const std::vector<MaskRuleSpec>& rules, MaskTrustLevel trust);

    MaskEvaluator(MaskEvaluator&&) noexcept;
    MaskEvaluator& operator=(MaskEvaluator&&) noexcept;
    MaskEvaluator(const MaskEvaluator&)            = delete;
    MaskEvaluator& operator=(const MaskEvaluator&) = delete;
    ~MaskEvaluator();

    /// @brief 求值单条文本：suspect 扫描 + 长度门 + 规则匹配 + 信任级别
    ///        默认（判定序见文件头）。
    MaskDecision evaluate(std::string_view text) const;

    /// @brief 装载的信任级别（default_action 的来源）。
    MaskTrustLevel trust() const noexcept;

    /// @brief 已装载的规则条数。
    ca::usize ruleCount() const noexcept;

private:
    struct Impl;
    explicit MaskEvaluator(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

/// @brief 遮蔽占位符（ASCII 恒安全，原文不出求值面）：规则生效且动作为
///        遮蔽时带规则 id（"[masked:<ruleId>]"）便于复盘归因；suspect 门
///        与 default_action 兜底的遮蔽为无 id 占位（"[masked]"）——被压
///        制的 allow 规则不是遮蔽来源，不得显形为归因。
ca::str::Utf8String maskedPlaceholderText(const MaskDecision& decision);

}   // namespace mjbase::project
