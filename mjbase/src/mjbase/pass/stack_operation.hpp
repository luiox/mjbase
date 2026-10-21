#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 冗余栈操作折叠：`dup;pop`→∅、`pop;pop`→`pop2`、常量装载复制展开等
///        25 条窗口规则迭代到不动点。
///
/// 语义：只匹配连续的栈操作形状，不跨 label/frame/异常边；
/// 至少一次替换算 changed；匹配窗口外的栈值、未知 producer 保守保持原样。
/// StackOperationExpander（帧分析展开 DUP 族）属帧链接线批次，不在本件。
///
/// 载体形态：asm/matcher 值语义窗口匹配（窗口纪律由 MatchRule 统一执行：
/// 内部不得有分支/switch 目标，首条豁免）；命中指令以值拷贝参与替换（cp 索引
/// 随值携带，本 pass 只复制既有装载、不新增 CP 条目）；替换以 replacement 值
/// 列表表达，空替换 = 整窗删除（apply 落盘语义，窗口外分支引用对首条的重
/// 定向到顺序后继）。
///
/// 宽语义声明：纯 DOM 层 ldc 载荷类型不可判别，
/// Steps::loadInt/loadFloat/loadStr 对 ldc 族一致宽匹配——被匹配序列的栈形状
/// 本身保证折叠栈正确（cat1 与 pop/cat2 与 pop2 对齐），触发面按栈形状
/// 宽判；golden 差异按声明流程落 manifest。
///
/// 收敛语义声明：apply 返回 Err（窗口被 DOM 拒绝，典型为异常表边界锚定落进
/// 窗口内部）时保守终止本方法的收敛循环——该窗口重试必然复败，继续扫描的
/// 增益不及语义复杂度；挂账：按窗口粒度跳过 Err 继续扫描。
///
/// maxStack/maxLocals 不变（折叠不改变栈深峰值）；发生替换返回 none()，
/// 否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:simplify`
/// - `stability:default`
class StackOperationSimplifier : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
