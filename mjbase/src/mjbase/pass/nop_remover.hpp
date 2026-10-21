#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief NOP 规范化器：删除方法体里的 NOP 指令。
///
/// 语义：只删 NOP，不碰任何其他指令；不删标签/行号/
/// 异常表边界等元数据。
///
/// 载体形态：CodeDom 无标签节点，标签是边界句柄。删除策略：
///   - NOP 是分支/switch 目标：先重定向到顺序后继再删（fallthrough 策略）；
///   - NOP 被异常表边界（Boundary 标签）锚定：保留该 NOP；
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
