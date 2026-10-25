#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 常量 switch 折叠器：把「int 常量装载 + switch」的相邻窗口折叠
///        成无条件跳转（对齐旧栈 mbase pass/opt/constant_jump_optimizer
///        的 ConstantSwitchFolder 可观测面）。
///
/// 适配形态：selector 真值静态期已定死的 switch 分发——常量化不透明
/// 谓词的 switch 形态、常量状态机分发残段。窗口
/// [int 常量装载; LOOKUPSWITCH/TABLESWITCH]，按 selector 值解析命中分支
/// （method_rewrite_util::switchCaseLabelOf：lookup 按 keys 精确匹配，
/// table 按 low + i 连续区间，未命中走 default），常量头原位改写为 GOTO
/// 复用命中的目标标签，switch 删除。
///
/// 常量装载面 = method_rewrite_util::intValueOf 全集（ICONST/BIPUSH/
/// SIPUSH/LDC 与 LDC_W 装载 int）。非常量 selector（装载/运算结果）不动。
///
/// 栈安全守卫：折叠取消 switch 的一次 selector 弹消费。switch 前置边界
/// 锚有存活标签时整窗跳过——外部路径直接落到 switch 上时，其栈顶值正
/// 等待这次消费，折叠会让它失去消费者（VerifyError）。窗口头（常量
/// 装载）前置边界的标签不阻断：从头进入重放整个窗口，净栈效应为零；
/// 改写后头部仍在（GOTO），标签原地有效。
///
/// 事务性：改写按「先删尾、后改头」顺序落笔——switch 删除在前且失败
/// 即中止本轮，头部改写最后发生，不会留下半折叠态。顺序末尾的 switch
/// 删除没有承接者（fallthrough 重定向无落点），该形态保守跳过。目标
/// 标签解析失败（畸形 switch 的悬空目标）同样拒收。
///
/// 折叠后未被选中的 case 标签失去引用者，CodeDom 中留为无引用标签——
/// 不物化进字节码，无需显式销毁。
///
/// 发生折叠返回 none()，否则 all()。maxStack 不升（窗口内峰值只降）。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class ConstantSwitchFolder : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
