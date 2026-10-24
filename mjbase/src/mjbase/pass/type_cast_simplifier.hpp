#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 原始类型转换与已知 helper 常量折叠（legacy TypeCastSimplifier 移植）
///
/// ## 规则集
///
/// - 15 条 JVM primitive cast：`i2l/i2f/i2d/i2b/i2c/i2s/l2i/l2f/l2d/
///   f2i/f2l/f2d/d2i/d2l/d2f`，源必须为常量。
/// - `Float.intBitsToFloat(I)F`：int 位图 -> float 常量。
/// - `Double.longBitsToDouble(J)D`：long 位图 -> double 常量。
/// - `Integer.parseInt(String)I`：仅当字符串严格符合 Java 十进制语法且
///   在 int 范围内。
/// - `Long.parseUnsignedLong(String,I)J`：仅当 radix 为常量 2..36、字符串
///   严格符合无符号语法且不超过 u64 范围。
///
/// ## JVM 语义
///
/// - 浮点 -> 整数：NaN -> 0；越界 -> MIN/MAX；范围内向零截断。
/// - `i2b/i2c/i2s` 按 JVM 定义的 signed byte / unsigned char / signed short
///   位截取后再扩展为 int。
/// - float/double 常量按原始位图物化（保留 NaN payload 与 -0.0）。
/// - 未知 helper、非法字符串或无法解析的常量一律不折叠。
///
/// 所有规则通过 `window_rule::replaceWindow` 写入，标签边界与 srcPc 契约
/// 与 FixPattern/Expression 一致。
///
/// @par 标签
/// - `scope:method`
/// - `action:simplify`
/// - `stability:opt-in`
class TypeCastSimplifier : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
