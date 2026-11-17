#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 方法内跨块常量传播。共享 AST 对可分析方法汇合所有到达路径上的
///        整数值，多个定义在值一致时也可证明局部读取是常量。
///
/// 分支上的已知条件不剪边，传播仍检查分支的两条路径。JVM 适配器使用
/// 提升时的指令句柄将已证实的结果写回，并保留原来源 PC。
///
/// 方法提升或共享分析失败时使用字节码 def-use；混合方法的非整数槽也沿用
/// 此路径。字节码路径使用到达定义集合：
///   - 装载点到达定义恰一条且非截断（截断集合上的「唯一定义」不可靠，
///     整件跳过——负向结论纪律同 def_use 契约）；
///   - 该定义是 store（iinc 定义不产常量，跳过）；
///   - store 弹值（def-use 栈使用边 index 0）恰一定义且该指令是匹配槽族
///     的常量生产者（int/long/float/double 按族取值；引用槽只认
///     ACONST_NULL——null 单值无引用同一性歧义）；
///   - 装载与 store 槽族严格匹配（int/long/float/double/ref 分族校验）。
///
/// 命中后装载原位改写为按值物化的常量生产者（materialize 共享面，srcPc
/// 影线保持原位）。
///
/// 生产者指令不改动（其栈值可能还有其他消费者）；maxStack 不升。
///
/// 发生改写返回 none()，否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class ConstantPropagator : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
