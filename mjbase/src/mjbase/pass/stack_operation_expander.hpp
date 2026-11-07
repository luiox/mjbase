#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief DUP 族展开：常量 producer 上的 dup/dup2/dup_x1/dup_x2/dup2_x1/
///        dup2_x2 按帧溯源还原 producer 复制语义——复制出的槽位改由原常量
///        装载的克隆独立供给，DUP 指令本身消解。
///
/// 语义门（全部满足才展开，任一不满足保守不动）：DUP 指令可达且入口帧
/// 可得；其全部输入槽位的来源收敛为单一指令（分支汇合多来源退化为未知
/// 即拒收），且来源是纯常量装载（含 ldc 元数据常量；condy 不认领）。
/// dup/dup2 原位消解（克隆在 DUP 位置重发输入槽）；X 族整窗重排（生产者
/// 与 DUP 构成的连续窗按输出形重发克隆，窗内生产者随之删除）。
///
/// 纪律对齐 stack_operation 套件：最长不重叠规划、自右向左批式落盘、
/// 首条豁免（控制流/异常表边界不得锚定窗口中部，labelsAnchorAt 覆盖
/// 分支与异常表两角色）、锚定拒收按窗口粒度跳过；数据依赖窗（帧溯源
/// 给出宽度与来源）不走 tmatcher 形状规则面，纪律门在同等位置重实现。
///
/// maxStack/maxLocals 不变（展开前后各点栈深逐一相同）；发生展开返回
/// none()，否则 all()。帧分析失败/截断/不可达一律整方法保守不动。
///
/// 适用形态、拒绝面、X 族窗口判据与前后对照见 doc:stack-op-expander-spec。
///
/// @par 标签
/// - `scope:method`
/// - `action:expand`
/// - `stability:opt-in`
class StackOperationExpander : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
