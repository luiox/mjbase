#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 冗余存储消除器：值定义无任何使用边的 store 原位降级 pop/pop2
///        （旧栈 mbase pass/opt/dead_code_remover 的 RedundantStore
///        Eliminater 按 def-use 载体重推导）。
///
/// 适配形态：状态槽反复覆写——同一槽位连续 store 而中间无读取，前一次
/// 写入的值在所有路径上都被覆写（或直到方法结束都无人读），是死写。
///
/// 载体：analyzeDefUse 到达定义（签名播种入参）。store 定义的使用边为空
/// ⟺ 该次写入的值在任何路径上都不被读取（后续 store 覆盖在数据流里天然
/// 杀死前定义；异常路径经精确异常边合并，handler 读取同样产生使用边）。
///
/// 与旧栈线性序载体（store/load 下标区间推理）的差异：旧栈按位置区间
/// 判定，控制流从区间内跳出到区间后的读取会漏判（误删活存储）；到达
/// 定义按路径追踪，该形态正确保留——这是修正而非偏离，单测钉死。iinc
/// 是读改写，读取产生使用边，旧栈的 iinc 槽整槽排除在此自然内化。
///
/// 与家族的分工：PopUnUsedLocalVariables 判「方法内零读点的槽」（不跑
/// 数据流）；DeadLocalStoreRemover 判「读点全不可达有意义汇点的槽」并
/// 可整删常量链；本件判「单次 store 定义的零使用」（到边粒度，槽内其他
/// 定义不受牵连）。三者互补、幂等。
///
/// 处置：原位降级（宽度按 store 族操作码——LSTORE/DSTORE 弹双槽 →
/// POP2），不删指令、不动结构，弹宽不变恒栈中性。
///
/// 安全门：帧数据流失败或截断整件跳过（零使用是负向结论）。
///
/// 发生降级返回 none()，否则 all()。maxStack/maxLocals 不变。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class RedundantStoreEliminater : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
