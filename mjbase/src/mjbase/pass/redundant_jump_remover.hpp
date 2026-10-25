#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 冗余跳转消除器：删除/替换「目标落在顺序后继前置边界」的跳转
///        （对齐旧栈 mbase pass/opt/jump_label_layout 的
///        RedundantJumpRemover 可观测面；标签布局族首件）。
///
/// 适配形态：跳转（GOTO/GOTO_W 与全部条件跳转族）的分支目标标签锚定在
/// 紧邻的顺序后继前置边界——分支边与 fallthrough 边汇合同一点，跳转本身
/// 不产生分流作用。典型来自反混淆改写后的顺序残段：常量折叠把条件跳变
/// 成恒真 GOTO 后，目标块恰被顺序铺在后方；块重排/直线化后的邻接汇合。
///
/// 改写矩阵：
///   - GOTO/GOTO_W → 整条删除（fallthrough 语义等价）；
///   - 单值条件跳（IFEQ/IFNE/IFLT/IFGE/IFGT/IFLE/IFNULL/IFNONNULL）→
///     原位替换为 POP（两路径同点，条件值被丢弃，弹宽对齐单值谓词）；
///   - 双值条件跳（IF_ICMPxx / IF_ACMPEQ / IF_ACMPNE）→ POP; POP
///     （两值都被丢弃，插一条 POP 在后）。
///
/// 栈平衡：替换只发生在「分支与汇合重合」形态上——原跳转无论走哪条边
/// 都要弹掉条件值，POP 序列等价复现这一消费。
///
/// 方法末指令没有顺序后继，天然不匹配（GOTO 删除也没有承接者问题）。
///
/// 标签语义：跳转自身前置边界锚有标签时不阻断——GOTO 删除经
/// fallthrough 重定向承接，条件跳替换是原位改写（标签原地有效）。目标
/// 标签保持原位（可能仍有其他引用者；失去全部引用者则留为无引用标签，
/// 不物化进字节码）。
///
/// 每轮快照全扫 + 外层不动点。
///
/// 发生改写返回 none()，否则 all()。maxStack 不升。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class RedundantJumpRemover : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
