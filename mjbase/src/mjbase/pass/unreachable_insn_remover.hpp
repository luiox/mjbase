/// @file        unreachable_insn_remover.hpp
/// @brief       结构级死代码清理：删除帧可达性证明不可达的普通指令。
///
/// ## 适配场景
///
/// 方法体内「无条件跳转/终止指令之后不会再被正常控制流到达」的指令岛：
/// 混淆处理链尾部的死分发块与死状态链、混淆器插入的干扰死块、以及任何
/// 前序改写（分支解析/折叠）留下的不可达残留。只按可达性删除，不做任何
/// 「看起来没用到」的值级推断（死存/死调用归局部存储清理家族，另批迁移）。
///
/// ## 算法
///
/// 1. 帧引擎一次（BestEffort 形状档）：`frames[i].reachable == false` 的
///    指令进入待删集；计算失败或帧数与指令数不一致 → 全方法放弃（fail
///    closed）。入口指令恒可达（帧引擎种子），方法体不会被删空。
/// 2. 异常边保守门：帧引擎默认把异常表项 handler 入口按可达种子（无论
///    保护区是否可达），故 handler 体永不为待删集成员；但保护区 start/end
///    标签可能锚在待删指令上——删指令会破表项边界，v1 对锚有任何异常表
///    标签的待删指令整条保留（不删、不改表项、不重锚）。死保护区连同
///    表项的清理归异常面清理批次。
/// 3. 标签政策：待删指令上锚定的其余标签，若被待删集之外的分支/switch
///    引用则保留该指令（可达分支指向的指令按帧传播必可达，此为防御门）；
///    集内互引标签随批删原语一并消解；无引用标签随指令销毁（无引用 =
///    无语义影响，不物化进字节码）。
/// 4. 批删：全部待删指令一次性 eraseBatch（kRejectIfUsed 纯批外引用校验；
///    批内互引被批删原语忽略，批外引用在前两步已排除——不需要承接者重
///    定向，顺带正确处理死块延伸到方法末尾的形态）。批删被拒时方法指令面
///    不变（已销毁的无引用标签不物化，写回字节等价），返回全保留。
///
/// ## 放弃清单
///
/// - 帧计算失败 / 帧数不一致（fail closed，方法原样）；
/// - 锚有异常表标签的不可达指令（保守保留，交异常面清理批次）；
/// - 锚有分支引用标签的不可达指令（防御门，按分析不可能出现）。
///
/// ## 幂等性
///
/// 删除后存活指令全部可达，二轮待删集为空 → 全保留；无增量副作用。
/// maxStack 只降不升，现值保持合法。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`（默认链不收，CLI -p 显式挂点）
#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// 结构级死代码清理 pass（帧可达性证明删除）。
class UnreachableInsnRemover : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
