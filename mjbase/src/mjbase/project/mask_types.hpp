/// @file        mask_types.hpp
/// @brief       mask 出口层共享类型：动作与信任级别（#121 §4.3/§4.5）。
///
/// mask 与 rename 分界（#121 §3.2）：rename 是有稳定身份实体的显示名覆盖，
/// 进映射文件；mask 是无身份的内容遮蔽（字符串常量、注解值），防不可信
/// 内容流出，不进映射文件。本头只放两机制共享的判定词表，不含逻辑。
#pragma once

#include "libca/core/datatype.hpp"

namespace mjbase::project {

/// @brief 出口文本动作：放行或遮蔽。
enum class MaskAction : ca::u8
{
    Allow = 0, ///< 放行（原文出口）
    Mask   = 1, ///< 遮蔽（占位符出口，原文不出求值面）
};

/// @brief 信任级别：项目级 default_action 的翻转开关。
///
/// 可信库默认 allow（规则抓已知坏模式）；不可信/obf 库默认 mask（规则做
/// 选择性放行）。suspect 旗标压过 allow 规则（见 mask_evaluator）。
enum class MaskTrustLevel : ca::u8
{
    Trusted   = 0, ///< 可信：default_action = Allow
    Untrusted = 1, ///< 不可信/obf：default_action = Mask
};

/// @brief 信任级别对应的 default_action。
inline MaskAction defaultActionFor(MaskTrustLevel level) noexcept
{
    return level == MaskTrustLevel::Untrusted ? MaskAction::Mask : MaskAction::Allow;
}

/// @brief 求值器装载的规则条数上限（数据面 replaceMaskRules 与求值器
///        MaskEvaluator::build 同值同源）。
inline constexpr ca::usize kMaximumMaskRules = 1024;

}   // namespace mjbase::project
