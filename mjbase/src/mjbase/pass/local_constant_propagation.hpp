#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 局部常量传播：直线段内的「常量装载 + store」槽位追踪，把后续
///        同类装载原位改写为常量生产者（对齐旧栈 mbase pass/opt/
///        local_constant_propagation 的可观测面）。
///
/// 适配形态（混淆器常量内联后的残段——槽位被写入常量后反复装载）：
/// 单遍直线扫描维护 slot → 常量追踪表：
///   - 追踪登记：store 的顺序紧邻前驱是数值常量装载（int/long/float/
///     double 按槽族匹配）时登记该槽；引用槽只在生产者恰为 ACONST_NULL
///     时登记（null 单值无引用同一性歧义，load → aconst_null 逐位等价；
///     ldc 字符串/getstatic 等非 null 引用来源不入追踪——引用别名与
///     可变性面保守不碰）。
///   - 装载改写：load 族命中追踪且槽族匹配时原位替换为按值物化的常量
///     生产者（materializeIntConst 等共享面，值域落 iconst/bipush/sipush/
///     ldc 与 ldc2_w），srcPc 影线保持原位。
///   - IINC 原地递推：槽位追踪中且为 int 常量时按 JVM iinc 语义求新值
///     （32 位定宽回绕，规避宿主有符号溢出 UB），后续装载取新值；原
///     生产者指令不改动（更早的装载已消费旧值或不在追踪面）。
///   - 清空纪律：任一成员前置边界锚有标签（segment 断裂）、GOTO/GOTO_W/
///     JSR/JSR_W/RET、条件跳转族、switch 族、return 族、ATHROW 即整体
///     清空——槽值在这些点之外不可静态延续，宁可漏折不可折错。其余
///     直线指令不触碰追踪（槽值只经 store/IINC 改变）。
///
/// 语义面差异（相对旧栈，均收紧或等价）：load 与追踪的槽族必须严格
/// 匹配（旧栈把 ILOAD_0..ALOAD_3 快捷形一并按追踪表替换，畸形输入可
/// 产出 Verifier 非法形态；本实现按 int/long/float/double/ref 分族校验）；
/// FRAME/LINE 元数据节点在新载体不进指令序（CodeDom 无标签/帧节点），
/// 旧栈对它们的清空纪律等价收敛为标签锚定判据。
///
/// 单遍即不动点：改写只把装载变为常量生产者（只会加强后续追踪，不会
/// 削弱），槽值变化仅由 store/IINC 驱动，一遍扫描与旧栈 while(changed)
/// 外层不动点收敛到同一结果。
///
/// maxStack 不升（同宽替换）；局部槽表/LVT 不动（不删槽位）。
///
/// 发生改写返回 none()，否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class LocalConstantPropagation : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
