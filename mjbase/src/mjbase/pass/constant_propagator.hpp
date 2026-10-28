#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 全域常量传播（方法内）：def-use 到达定义面——装载点的到达定义
///        唯一且其「store 弹值定义」为常量生产者时，装载原位物化为常量
///        （对齐旧栈 mbase pass/opt/constant_propagator 的 ConsLocalVar
///        Optimizer / LabelConsLocalVarPropagator 两面）。
///
/// 与 LocalConstantPropagation（直线段窗口面）的分工：本件跨块传播——
/// 分支/汇合不阻断，判据是路径精确的到达定义集合：
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
/// 语义面差异（相对旧栈，均收紧或修正）：
/// 1. **store 前装载不再误折**（修正）：旧栈 ConsLocalVarOptimizer 按整方
///    法「恰一 store」替换全部装载，store 执行前到达的装载读的是入参/初
///    值，被错误替换为常量；到达定义面天然不含不经过该 store 的路径。
/// 2. **标签区间启发面被路径精度覆盖**（等价超集）：旧栈
///    LabelConsLocalVarPropagator 的 label 区间 + 区间内无 iinc 启发，是
///    到达定义单值性的保守近似；iinc 经到达定义面天然拦截（其定义非常
///    量生产者）。
/// 3. **截断集合整件跳过**（收紧）：旧栈无到达定义面，无此失败模式。
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
