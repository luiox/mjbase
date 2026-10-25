/// @file        pop_unused_local_variables.hpp
/// @brief       局部存储清理：把「槽位在方法内无任何读取」的 store 降级为
///              pop/pop2。
///
/// ## 适配场景
///
/// 混淆处理尾部残留的纯噪声存储链：状态机分发被静态解析后，状态变量与
/// 中转变量再无读取点，但对它们的 store 仍在执行（`ldc K; istore s` 这类
/// 死写）。本件把这类 store 改写为等宽 pop——弹掉生产值、栈形不变，把
/// 「删不删生产者」的决定留给常量装载/装载清理件。
///
/// ## 算法
///
/// 1. 读点审计（方法级槽位粒度，按指令全集做——不可达代码中的读点同样
///    计入，保守口径）：任何局部装载（含短形态 _n 族）覆盖的槽位、两槽
///    装载（lload/dload）覆盖的基槽与高半槽、iinc 与 ret 的操作槽，全部
///    计入读集合。
/// 2. store 降级：遍历全部 store 指令（istore/lstore/fstore/dstore/astore
///    含 _n 族与 wide 形态），其覆盖槽位（两槽 store 覆盖基槽与高半槽）
///    全部不在读集合 → 改写为 pop（cat1 族）/ pop2（lstore/dstore 族），
///    操作数字段清零。
///
///    宽度由 store 族操作码自身决定（JVMS 为每族 store 定死弹槽数），
///    不依赖帧溯源——本载体不需要旧栈的 OriginalSource 帧定宽前提。
///
/// 3. 语义等价：store 无副作用，槽位无读取则写入不可观测；pop 弹掉同宽
///    值后栈形不变。改写只动操作码字段，不删指令、不动属性面。
///
/// ## 放弃清单
///
/// - 槽位存在任何读点（含仅被不可达代码读取、被 iinc/ret 触碰、作为两槽
///   值高半槽被读取）→ 该槽位的全部 store 保持原样；
/// - 不删生产者（常量装载清理归 RedundantLoadValEliminator，局部装载
///   清理归后续 UselessPopCleaner 切片）；
/// - 不做活跃区间分析（跨分支的「先写后读」不区分，读点在方法内任何
///   位置都保护整个槽位）。
///
/// ## 幂等性
///
/// 改写产物是 pop 族（非 store），二轮无可降级 store → 全保留。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`（默认链不收，CLI -p 显式挂点）
#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// 无读取槽位的 store 降级 pop/pop2。
class PopUnUsedLocalVariables : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
