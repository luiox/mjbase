#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 常量条件跳转折叠器：把「常量装载 + 常量条件跳转」的相邻窗口
///        折叠成无条件跳转或整窗删除（对齐旧栈 mbase pass/opt/
///        constant_jump_optimizer 的 ConsJumpOptimizer 可观测面）。
///
/// 适配形态（混淆器留下的常量化条件分发残段——真值在静态期已定死的
/// 分支，典型来自不透明谓词常量内联后的产物）：
///   - 单槽窗 [int 常量装载; IFEQ/IFNE/IFLT/IFGE/IFGT/IFLE]：
///     谓词按常量求值，恒真 → 窗口头改写为 GOTO（复用原跳转目标标签），
///     条件跳转删除；恒假 → 整窗删除（直落 fallthrough）。
///   - 单槽空窗 [ACONST_NULL; IFNULL/IFNONNULL]：null 判定恒真/恒假，
///     同上折叠。
///   - 双槽窗 [int 常量装载; int 常量装载; IF_ICMPxx]：比较谓词按两常量
///     求值，恒真 → 首常量改写为 GOTO、其余两指令删除；恒假 → 三指令
///     全删。
///
/// 常量装载面 = method_rewrite_util::intValueOf 全集（ICONST/BIPUSH/
/// SIPUSH/LDC 与 LDC_W 装载 int）。空窗只认 ACONST_NULL 生产者；int
/// 常量配 IFNULL/IFNONNULL、ACONST_NULL 配数值谓词、双槽的 IF_ACMPEQ/
/// IF_ACMPNE 均不入矩阵——旧栈对这类 Verifier 非法形态也按谓词表折叠
/// （防御怪癖），本实现显式拒绝（类型不匹配的窗口不做语义假设）。
///
/// 栈安全守卫：折叠会取消窗口内的一次（双槽两次）栈消费。窗口内部
/// 边界（双槽窗中位装载与条件跳转的前置边界）锚有存活标签时整窗跳过
/// ——外部路径从窗口中部进入时，其栈上的值正等待这次消费，折叠会让
/// 它失去消费者（VerifyError）。窗口头前置边界的标签不阻断：从头进入
/// 重放整个窗口，净栈效应为零（装载与消费成对消失），改写后仍为零。
///
/// 事务性：改写按「先删尾、后改头」顺序落笔——条件跳转（及双槽中位
/// 装载）删除在前且失败即中止本轮，头部改写最后发生，任一步失败不会
/// 留下「头已改、尾未删」的半折叠态。
///
/// 每轮快照全扫 + 外层不动点：本轮折叠暴露出的新相邻窗口（前窗删除
/// 使后方窗口前移接续）下一轮继续。
///
/// 顺序末尾的条件跳转删除没有承接者（fallthrough 重定向无落点），该
/// 形态保守跳过——旧栈树模型无此约束照删，属载体重论证后的显式收紧。
///
/// 发生折叠返回 none()，否则 all()。maxStack 不升（窗口内峰值只降）。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class ConsJumpOptimizer : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
