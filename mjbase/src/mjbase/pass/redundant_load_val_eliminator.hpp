#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 冗余装载值消除器：删除「常量装载 + 紧随 POP/POP2」的相邻指令对。
///
/// 匹配矩阵：
///   - POP：前驱是 int / long / string 常量装载（ICONST/BIPUSH/SIPUSH/LDC(Int)/
///     LCONST/LDC2_W(Long)/LDC(String)）；
///   - POP2：前驱是 long / double 常量装载（LCONST_0/1、DCONST_0/1、
///     LDC2_W(Long/Double)）；以 classfile 可观测面为准，覆盖上述全集。
///
/// 迭代到不动点：前半段删除暴露出的新相邻对
/// 在下一轮继续消除（如 [LDC;POP][LDC;POP] 连续两对一轮即清，交错形态
/// [ICONST;ICONST;POP;POP] 第二轮清）。
///
/// 边界守卫：POP 的前置边界有
/// 存活标签锚定（分支目标/异常表边界）时跳过该对。首成员（常量装载）锚定
/// 标签时照常处理：fallthrough 删除把引用重定向到 POP。行号边界在 CodeDom
/// 不可见（LNT 是 backing 属性），跨行号边界的相邻对会消除——见
/// method_rewrite_util.hpp 头注。
///
/// 发生消除返回 none()，否则 all()。maxStack/maxLocals 不变（消除只降不升）。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class RedundantLoadValEliminator : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
