#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief NOP 规范化器：删除方法体里的 NOP 指令（移植自 mbase pass/opt/
///        dead_code_remover 的 NopRemover，Java 原件 cleaner/NopRemover.java）。
///
/// 语义（与旧实现逐条对齐）：只删 NOP，不碰任何其他指令；不删标签/行号/
/// 异常表边界等元数据。mjt opt 默认链（PassStability::Default）的收尾清理件。
///
/// 载体重定型：旧栈 NOP 是指令序列成员，LabelNode/LineNumberNode 与 NOP
/// 平级共存；新栈 CodeDom 无标签节点，标签是边界句柄。删除策略：
///   - NOP 是分支/switch 目标：先重定向到顺序后继再删（fallthrough 策略，
///     等价旧栈「LabelNode 留在原位、NOP 摘除」的落点语义）；
///   - NOP 被异常表边界（Boundary 标签）锚定：保留该 NOP（显式偏离：旧栈
///     无条件删除、标签自然落到后继；新栈边界标签保护锚点且 pass 无权迁移
///     异常表语义。javac 不产此形态，触发面为手工畸形样本）；
///   - NOP 是末条指令且被引用：无顺序后继可承接，保留。
///
/// maxStack/maxLocals 不变（NOP 无栈效应）；发生删除返回 none()，否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:default`
class NopRemover : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
