#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 冗余调用清理器（全局件）：容器内定义、调用结果被弹弃、方法体
///        纯（method_effect Pure 档）的方法，其可删调用点整组摘除（旧栈
///        mbase pass/opt/dead_code_remover 的 RedundantInvokeRemover 按
///        「普查 + 委派」载体重推导）。
///
/// 适配形态：混淆填充调用的结果只被 pop 吞掉——被调方是容器内可见的
/// 纯函数（典型常量返回探针），调用与弹弃一起消失。
///
/// 普查（候选提名，只读面）：遍历容器全部 class 资源建会话，凡某方法
/// 在任一调用点出现「调用 + 紧随宽度匹配的 POP/POP2」（返回 cat2 配
/// POP2、其余配 POP）、目标命中容器内定义集、且 MethodEffectModel 判
/// Pure（写堆/分配/同步/抛出/字段写一律不提名；ReadOnly 档不提名，与
/// 旧栈 isPure 档对齐）→ 提名该成员。
///
/// 应用（委派 MethodCallRemover 点名面）：提名成员装入 includeCalls，
/// 栈等价改写（可删形态 = pop/pop2/死局部消费；实参补弹宽；句柄钉死
/// 守卫；两阶段提交）全部复用。与旧栈「调用换 ICONST_1 留实参残段给
/// 后续管线」的差异：点名面按栈等价规则一次摘净调用与消费面——收敛
/// 结果等价且单件自洽；实参生产者残段由局部清理族后续收敛。
///
/// 本件只删调用点不删方法定义——悬垂方法交由 unused-member 族收尾。
///
/// 任一类资源不可解析时整件放弃（普查全量性前提，fail closed）。
///
/// 发生改写返回 none()，否则 all()。
///
/// @par 标签
/// - `scope:global`
/// - `action:eliminate`
/// - `stability:opt-in`
class RedundantInvokeRemover : public mjbase::spi::GlobalPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::model::ResourceContainer&     container,
                                       mjbase::spi::GlobalAnalysisManager& am) override;
};

}   // namespace mjbase::pass
