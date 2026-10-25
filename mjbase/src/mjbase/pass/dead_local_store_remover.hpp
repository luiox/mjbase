#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 死局部存储清理器：槽位值链不可达有意义汇点的 store 整删/降级
///        （旧栈 mbase pass/opt/dead_code_remover 的 DeadLocalStoreRemover
///        按 def-use 载体重推导）。
///
/// 适配形态：混淆展开后遗留的状态槽暂存——槽位虽被装载，但装载值沿全部
/// 传递链（运算转发、槽位间转存）都到不了任何可观测指令，即「写而不养」
/// 的死槽；直接喂常量的死 store 连常量生产者一起消失。
///
/// 载体：analyzeDefUse（按方法签名播种入参——参数值有定义边，参数槽的
/// 存储不会被误判死槽）。槽位有意义 ⟺ 某次读的到达定义沿 def→use 边
/// 闭包能到达有意义汇点（invoke 族 / 分支与 switch / putfield / putstatic /
/// monitorenter / monitorexit / athrow / xreturn / xastore）。store 与 iinc
/// 作为中间定义续走（槽位间转存穿透）；dup/swap 复制透明——消费边直连
/// 原定义。读点到达集合溢出或无定义边（看不见来源）→ 保守记有意义。
///
/// 处置（联合不动点，两约束迭代互约）：
///   - 死槽 store 整删 ⟺ 其弹值的全部到达定义都是纯常量生产者
///     （int/long/float/double/string 常量装载 + ACONST_NULL）且属最终
///     整删集；
///   - 常量属整删集 ⟺ 其全部使用边都指向最终整删的 store（降级为 POP
///     的 store 仍在消费弹值，生产者必须保留）。
///   不满足整删的死 store 原位降级 POP/POP2（宽度按 store 族操作码，
///     弹宽不变恒栈中性）。弹值定义含运算/装载转发/参数时不整删——
///     装载是恒等搬运、定义归 store，隔距离的装载对由装载清理件与管线
///     下一轮窗口面收敛。
///
/// 与 PopUnUsedLocalVariables 的分工：后者按「方法内零读点」降级（不需
/// 数据流）；本件按「读点全不可达有意义汇点」处置，覆盖更广且能整删
/// 常量链，两者在管线中互补、幂等收敛。
///
/// 安全门：帧数据流失败或任一到达集合溢出（truncated）时整件跳过——
/// 负向结论（「读点不可达汇点」）不建立在截断集合上；无输入边的 store
/// （不可达/无帧事实）只降级不整删。
///
/// 发生整删/降级返回 none()，否则 all()。maxStack/maxLocals 不变。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class DeadLocalStoreRemover : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
