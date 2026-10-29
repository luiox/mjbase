#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 同值纯链复写摘除（GVN/CSE 第一切片）：同一直线段内两条结构
///        全同的纯计算链先后写同一局部槽，第二条链连同其存储整组摘除
///        ——第一条已把同值写入槽内，重算不可观测。
///
/// 识别形（链内逐指令相邻 + 区间无标签锚定）：
///   [CHAIN; xSTORE t] ... [CHAIN; xSTORE t]     两条全同链写同一槽 t
/// CHAIN 为自包含纯计算序列（自存储点反向需值抽取）：局部装载（记录
/// 槽号）、常量推入（LDC/LDC2_W 非 condy，记录 CP 槽）、算术/移位/位
/// 运算、取负、数值转换、比较——任何调用/字段/数组/栈操作/控制流不
/// 入链。链结构全同 = 逐条同操作码、装载同槽号、LDC 同 CP 条目。
///
/// 语义论证：区间 [链1 头 .. 链2 存储) 内无标签锚定与控制流（同一基本
/// 块直线段，链1 必然先于链2 执行完毕）、无对链操作数槽的写入（链为
/// 操作数纯函数，两次求值同值）、无对 t 的写入（链1 存储点除外，全程
/// t 持同值，区间内任何读点两种世界同值）——擦除链2 及其存储后局部
/// 状态与可观测行为逐点不变。IDIV/LDIV/IREM/LREM 入链：链1 已用同值
/// 完成求值，除零异常若可能已在链1 抛出，链2 的重算不可达异常面。
///
/// 安全门（fail-closed）：需值抽取遇链外指令（含外层栈依赖、任何存储）
/// 整对拒收；condy 不认领（bootstrap 可抛）；区间控制流/标签/iinc 或
/// 存储触操作数槽或 t（链1 存储点除外）拒收。maxStack 只降不抬。
///
/// 后续精化挂账：外层栈供源的链形、getstatic 纯面（需字段写推断）、
/// 跨基本块的支配关系泛化。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class CseChainEliminater : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
