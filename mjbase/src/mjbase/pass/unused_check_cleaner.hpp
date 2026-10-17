#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 冗余空检查清除器：删除「ALOAD 0 + INVOKESTATIC
///        java/util/Objects.requireNonNull (Ljava/lang/Object;)Ljava/lang/Object;
///        + POP」三指令组（移植自 mbase pass/opt/dead_code_remover 的
///        UnusedCheckCleaner）。
///
/// 语义（与旧实现逐条对齐）：
///   - 仅实例方法参与（静态方法无 this，ALOAD 0 装载的是参数，删引用探测
///     不成立）；
///   - 三成员必须严格程序相邻，引用固定为 Objects.requireNonNull 的标准
///     三段（owner/name/desc 逐字节）；
///   - 匹配即整组删除（this 的 VM 隐式非空保证删除是行为保持的）。
///
/// 边界守卫（旧栈 LabelNode 阻断语义的 CodeDom 等价）：第 2/3 成员
/// （INVOKESTATIC/POP）前置边界有存活标签锚定时跳过该组。首成员的分支目标
/// 标签随删除链重定向到组后继（等价旧栈标签留原位的落点）；其异常表边界
/// 锚定（Boundary）会使首步删除被拒，整组跳过（保守偏离，javac 不产此形态）。
///
/// 发生删除返回 none()，否则 all()。maxStack/maxLocals 不变。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class UnusedCheckCleaner : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
