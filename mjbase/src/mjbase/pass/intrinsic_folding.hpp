#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief intrinsic 常量折叠器：白名单纯函数在实参全常量时整体折叠为
///        常量结果（对齐旧栈 mbase pass/opt/intrinsic_folding 的三件
///        BoxUnboxEliminator / RandomSeedResolver / StdConsFunctionOptimizer
///        可观测面，三件合为一个 pass 的内部三清扫）。
///
/// 适配形态：
///   - 装箱/拆箱对：`Box.valueOf(p) + Box.pValue()`（8 种包装类型）相邻
///     窗整对删除（净栈效应零，valueOf 不产 null 故无 NPE 语义差）。
///   - 种子随机：`NEW java/util/Random; DUP; ldc2_w 种子; INVOKESPECIAL
///     <init>(J)V; INVOKEVIRTUAL nextXxx` 五窗折叠为结果常量（LCG 收敛于
///     jstd::Random，JDK java.util.Random 位模式权威实现）：nextInt()I /
///     nextLong()J / nextFloat()F / nextDouble()D / nextBoolean()Z。
///   - 白名单纯函数（实参全常量）：Math.abs/max/min（I/J/F/D，NaN 与
///     ±0 语义对齐 JDK）、String.length/isEmpty/substring(II)（常量串
///     接收者）、Integer/Long 的 signum / parseInt / parseLong / reverse /
///     reverseBytes / bitCount / numberOfLeadingZeros / numberOfTrailingZeros
///     / highestOneBit / lowestOneBit / rotateLeft / rotateRight 与
///     toBinaryString / toHexString / toOctalString / toString（含 toString
///     进制重载）、String.toCharArray + String.valueOf([C) 恒等对整对删除。
///
/// 窗口守卫：窗口内部边界（首成员之外全部成员的前置边界）锚有存活标签
/// 时整窗跳过——外部路径从窗口中部进入时其栈值正等待窗口内的消费，折叠
/// 会让它失去消费者。首成员前置边界不查：从头进入重放整窗，净栈效应
/// 为零（成对消费与生产同消）。toCharArray+valueOf 对与装箱对删除首成员
/// 时按 fallthrough 重定向锚定标签（接收者生产者留在窗前，从头进入的
/// 语义仍是整窗重放）。
///
/// 字符串面：常量串操作数按 CP MUTF-8 字节视图读取。length 按前导字节
/// 计数（MUTF-8/CESU-8 无续字节即可读码元，码元数与 String.length()
/// 一致）；isEmpty 按字节空判；substring 仅折纯 ASCII 常量串（字节号 ==
/// 码元号；非 ASCII 形态保守不折——旧栈按字节错折，属声明修正）。
/// 折叠结果字符串经 session.internString 物化为 LDC。
///
/// 事务性：改写按「先删尾、后改头」顺序落笔，删除失败即中止本轮，
/// 头部改写最后发生，不留半折叠态。顺序末尾窗口（fallthrough 无承接者）
/// 保守跳过——Verifier 合法代码的窗口尾必消费窗口产物，该形态不可达。
///
/// 每轮快照全扫 + 外层不动点：折叠暴露的新相邻窗口由下一轮快照接续。
///
/// 与旧栈的偏差（修正面非劣化，逐条申报）：
///   1. 旧栈三件（BoxUnbox / RandomSeed / StdConsFunction）合并为单 pass
///      内部三清扫，管线挂点面收敛，语义逐窗等价；
///   2. String.length 对非 ASCII 常量串按码元数折叠（旧栈按 MUTF-8 字节
///      数错折）、substring 仅折纯 ASCII（旧栈按字节错折非 ASCII 形态）；
///   3. Math.abs(int/long) 的 MIN_VALUE 溢出面与旧栈一致返回原值（仍折叠，
///      物化为同值常量）。
///
/// 发生折叠返回 none()，否则 all()。maxStack 不升（窗内峰值只降）。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class IntrinsicFolding : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
