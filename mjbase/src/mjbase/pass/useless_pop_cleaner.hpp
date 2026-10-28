#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 无用 pop 清理器：消除弹值即被丢弃的死生产链（旧栈 mbase
///        pass/opt/dead_code_remover 的 UselessPopCleaner 迁移，分两个
///        相位各按自己的载体推理）。
///
/// 相位一（窗口面，单一路径相邻对）：
///   - POP：前驱是 category-1 局部装载（ILOAD/FLOAD/ALOAD 及 _0.._3
///     紧凑形；wide 前缀不改族属）；
///   - POP2：前驱是 category-2 局部装载（LLOAD/DLOAD 及 _0.._3 紧凑形）。
///
/// 配对删除恒栈中性（装载恰推 1/2 槽、POP/POP2 恰弹同宽），不读局部槽
/// 内容。迭代到不动点：前半段删除暴露出的新相邻对在下一轮继续消除。
///
/// 窗口面边界守卫：POP 的前置边界有存活标签锚定（分支目标/异常表边界）
/// 时跳过该对——分支可能在装载与 pop 之间进入，窗口推理看不见该路径的
/// 栈形。首成员（装载）锚定标签时照常处理：fallthrough 删除把引用重定向
/// 到 POP。行号边界在 CodeDom 不可见，跨行号边界的相邻对会消除。
///
/// 相位二（帧溯源面，def-use 到达定义）：对每个 POP/POP2，若弹值的全部
/// 到达定义都是纯常量生产者（int/long/float/double/string 常量装载与
/// ACONST_NULL——即「装载即无副作用且值确定」的生产族），且每个定义的
/// 全部使用边都指向本 pop，则生产者组与 pop 一并删除。分支汇合的多到达
/// 定义同组处理（各路径各推一个常量、汇合处统一弹弃的形态）。非相邻、
/// 跨距离的 [常量; …; POP] 对只有本面能看见。
///
/// 帧面的两类 POP2 处理：顶值生产者是 cat2 常量 → 单值形态整删；双
/// category-1 形态两侧都可删整删、仅一侧可删则半删（删该侧生产者，
/// POP2 原位降级 POP 弹剩余一侧）。
///
/// 帧面不需要窗口面的标签守卫：def-use 边覆盖进入 pop 的全部路径，标签
/// 进入点携带的到达定义已在合并集合内；删除仍走 fallthrough 重定向。
///
/// 帧面安全门：
///   - 栈中继逐中继细化：DUP/DUP2 族不再整族一票否决。def-use 载体对
///     复制类透明（帧引擎的 dup 槽复制沿用原定义行，中继输入消费按净
///     效应计不产边），方法内无中继时保持纯 def 路（逐计划唯一消费判据
///     不变）；有中继时每个计划经直线溯源认证——从 pop 回溯模拟输入槽
///     需求（NOP 跳过、纯常量扣减、已计划 pop 穿透、DUP/DUP2 把拷贝
///     别名折叠为一份原件），收集链上可吸收中继；溯源常量与 def 侧互
///     证一致，共享常量/中继的计划连通分组，组栈平衡非零（复制逃逸，
///     如 [ICONST_1; DUP; POP] 的第二拷贝离场）或常量使用边逃出计划
///     pop 集整组弃。吸收形态清单：DUP（cat1 输入，两份拷贝全部落进
///     计划 pop，如 [常量; DUP; POP2]、[常量; DUP; POP; POP]）、DUP2
///     （cat1 对输入，四份拷贝全部落进计划 pop，如 [常量; 常量; DUP2;
///     POP2; POP2]）。保持 fail-closed 的形态清单：X 族（DUP_X1/DUP_X2/
///     DUP2_X1/DUP2_X2）与 SWAP 的重排/插底形态（拷贝与在场值交错）；
///     DUP2 对 long/double 双槽（cat2 直接输入，第二份拷贝天然离场）；
///     中继链上的标签锚定（分支汇合，直线供源不成立）；未计划消费穿插
///     （弹值逃逸出认证面）；复合生产者与 cat2 半吃；中继叠链（组平衡
///     非零拦截）。中继路无半删（溯源互证对双侧不一致天然弃证）。
///   - 分析失败（帧数据流不成立）或到达集合溢出（truncated）时跳过帧面；
///     负向结论（「无其他消费者」）不建立在截断集合上。
///   - 未播种入参（分析不含 MethodSig）：参数值无到达定义边，弹参数装载
///     值的对保守不动。
///   - 局部装载不作帧面生产者：def-use 里装载是恒等搬运、定义归 store，
///     隔距离的 [装载; …; POP] 删除需连同 store 活跃性一起判，归
///     DeadLocalStoreRemover 的链式删除合同，不在本件重复。
///
/// 挂账（帧面余件）：X 族/SWAP 重排中继的吸收（需要复制面与重排面的
/// 联合栈推理，现 fail-closed）。LDC(class/MethodType/MethodHandle) 纯
/// 生产者已认领（Dynamic/condy 不认领——bootstrap 链有求值副作用）；
/// 帧面删除后窗口面的再收敛归管线重复驱动，本件单次运行各相位各走
/// 一遍。
///
/// 发生消除返回 none()，否则 all()。maxStack/maxLocals 不变（消除只降
/// 不升）。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class UselessPopCleaner : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
