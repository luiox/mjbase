#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 异常边界清理器：对「删除/改写后可观察行为逐位等价」的异常表
///        条目做收口（对齐旧栈 mbase pass/opt/try_catch_cleanup 五件的
///        可观测面；RedundantTryCatchRemover / UselessTryCatchRemover /
///        InvalidTryCatchRemover / SelfCaughtThrowRewriter / TryCatchRepair
///        中语义成立的面收拢为单 pass 四规则）。
///
/// 规则面（全部只动异常表 + 个别改写，fail-closed：任一不确定整条保留）：
///   - 修复面：start/handler 标签无法锚定（悬空）或保护区间为空的条目
///     删除（结构上无法参与合法派发）。
///   - 透明重抛删除：handler 体只把捕获异常原样重抛——`athrow` /
///     `astore X; aload X; athrow` 同槽恒等 / `invokestatic M; athrow` 且
///     M 为当前类恒等静态包装（(LX;)LX;，体恰为 `aload 0; areturn`，无
///     异常表、非 synchronized）——且 handler 不被其他条目共享、handler
///     及其体在正常控制流下不可达（无来自可达代码的普通流入）、无其他
///     条目与保护区重叠或覆盖 handler 体时，删除条目（捕获后立即重抛与
///     不捕获逐位等价）。
///   - 恒不抛收口：保护区间内不存在任何「域分析可达且可能抛出异常」的
///     指令时删除条目。可达性取自域引擎到达帧（帧不可达 = 不可执行），
///     可能抛出集合对齐旧栈保守口径（调用/分配/类型解析/字段与数组访问/
///     监视器/除法，含普通局部 store 区间的整段保守计入）。
///   - NPE 形改写：保护区恰为 `aconst_null; athrow`（无其他标签锚入）、
///     catch 类型为 any 或 NPE、handler 形如 `pop` 后接续点（下一指令的
///     锚定标签或下一指令 GOTO 的目标）、保护区栈深为零、无其他条目重叠
///     时——`aconst_null` 原位改写为 `goto 续点`、`athrow` 删除、条目删除
///     （必然抛出 + handler 立即吞掉跳续点 ≡ 直跳续点）。
///   - 自捕抛改写：保护区恰为 `new E; dup; invokespecial E.<init>()V;
///     athrow`、catch 类型为 any 或 E、athrow 处栈深恰为 1、athrow 无其他
///     标签锚入、handler 在区外、无其他条目重叠时——`athrow` 原位改写为
///     `goto handler`（构造完成的 E 留在栈顶，恰为异常派发进入 handler
///     的栈形状）、条目删除。
///
/// 域分析（可达性 + 栈深）失败、JSR/RET 等结构不支持、描述符/标签解析
/// 不齐一律放弃对应规则，保守保留。
///
/// 与旧栈的偏差（修正面非劣化，逐条申报）：
///   1. 旧栈五件合并为单 pass 四规则；InvalidTryCatchRemover 的「handler
///      首指令 athrow 即删条目」无守卫版本不复制（handler 可被正常流到达
///      或被共享时删条目会改行为）——该面由带完整守卫的透明重抛规则承担；
///   2. 旧栈 TryCatchRepair 按「handler 地址序必须后于保护区」删除乱序
///      条目不复制（合法类文件的 handler 允许在地址序上先于保护区）；
///   3. NPE 形改写与自捕抛改写补入旧栈缺失的锚入标签守卫与栈深门槛
///      （旧栈无栈深检查的 NPE 形在保护区入口有栈积压时会产出 Verifier
///      非法跳转）；
///   4. throws 子句（Exceptions 属性）修剪面不在本 pass（属性级元数据，
///      与异常表派发无关）。
///
/// 发生删除/改写返回 none()，否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class TryCatchCleanup : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
