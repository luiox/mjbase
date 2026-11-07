/// @file        mask_suspect.hpp
/// @brief       字符串 suspect 旗标扫描（#121 §4.2）：控制字符、零宽字符、
///              异常 Unicode 段与注入形态正则。
///
/// 纯函数面：输入 UTF-8 文本字节，输出四类旗标。入库时按策略打标（v1 只
/// 加旗标，不建字符串实体表）与出口求值（mask_evaluator 的 suspect 门）
/// 共用本扫描。旗标是"内容可疑"的观察信号，不直接等于遮蔽决定；遮蔽
/// 判定归 mask_evaluator（规则 + 信任级别）。
///
/// 旗标语义（阈值细节测试钉死）：
///   - controlChars：C0（U+0000..U+001F）、DEL（U+007F）与 C1
///     （U+0080..U+009F）。合法文本格式控制（`\t`/`\n`/`\r` 除外意向上不特赦——
///     常量池字符串出现裸控制字节本身即异常，宁枉勿纵）；
///   - zeroWidthChars：零宽与方向控制字符（U+200B..U+200F、U+202A..U+202E、
///     U+2060..U+2064、U+2066..U+2069、U+FEFF）——对抗性文本用它做同形
///     异义与指令隐藏；
///   - anomalousUnicode：无效 UTF-8 序列、私有区（U+E000..U+F8FF、
///     U+F0000..U+FFFFD、U+100000..U+10FFFD）与非字符（U+FDD0..U+FDEF、
///     各面 U+xxFE/U+xxFF）；
///   - injectionPattern：内建注入形态模式命中（字面量集见实现；经
///     MaskRegex 适配面，仅 kMaskMaxEvaluatedBytes 内的输入参与——超长
///     输入跳过正则，字节级旗标照常计算）。
///
/// 红线：本模块及其测试不出现混淆器家族名；模式是通用协议/指令形态，
/// 中性命名。
#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"

namespace mjbase::project {

/// @brief 一次 suspect 扫描的旗标集。
struct SuspectFlags final
{
    bool controlChars     = false; ///< C0/C1 控制字符与 DEL
    bool zeroWidthChars   = false; ///< 零宽与方向控制字符
    bool anomalousUnicode = false; ///< 无效 UTF-8、私有区、非字符
    bool injectionPattern = false; ///< 内建注入形态模式命中

    /// @brief 任一旗标置位。
    bool any() const noexcept;
};

/// @brief 扫描文本的 suspect 旗标。
///
/// 字节级旗标（控制/零宽/异常 Unicode）对任意长度线性扫描；注入形态
/// 正则只对 ≤ kMaskMaxEvaluatedBytes 的输入求值（引擎纪律 §3 长度门），
/// 超长输入该旗标恒 false——超长本身交由出口长度门走 default_action。
SuspectFlags scanSuspect(std::string_view text);

}   // namespace mjbase::project
