#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 冗余栈操作折叠：`dup;pop`→∅、`pop;pop`→`pop2`、常量装载复制展开等
///        31 条窗口规则迭代到不动点。
///
/// 语义：只匹配连续的栈操作形状，不跨 label/frame/异常边；
/// 至少一次替换算 changed；匹配窗口外的栈值保守保持原样。
/// StackOperationExpander（帧分析展开 DUP 族）属帧链接线批次，不在本件。
///
/// 载体（asm/tmatcher）：ReplaceRule 组合子规则 + InsnPlanner 计划面——
/// 匹配产出 ReplacePlan（窗口 + 替换 Insn 值列表），编辑由本 pass 经
/// CodeDom 原语落盘；cloneMatchedNode 的对应物是命中指令的值拷贝（cp
/// 索引随值携带，本 pass 只复制既有装载、不新增 CP 条目）；空替换 =
/// 整窗删除（窗口外分支引用重定向到顺序后继）。
///
/// 规则胜出纪律（相对旧 asm/matcher 载体的偏离申报）：从「规则注册序
/// 优先、窗内倒序首中、每规则每轮至多一处」改为「位置优先扫描、Longest
/// 裁决、命中区间不重叠、整轮批式落盘」。修复旧纪律的语义缺陷：
/// `[装载;装载;pop2]` 双装载形状上，先注册的 `[装载;pop2]` 短窗在倒序
/// 首中下抢吃后两条、留下死装载（栈面 +1）——Longest 使双装载整组消费。
///
/// 规则面退役申报：旧表 `[dup_x1;swap]=>[dup]` 在栈面不等价——`[a,b]` 上
/// LHS 终形 `[b,b,a]`（顶为 a）、RHS 终形 `[a,b,b]`（顶为 b），非死值尾部
/// 可观测；旧夹具以 `return` 死值尾部遮蔽了差异。整条退役，`dup_x1` 面
/// 保留 sound 的 `[装载×2;dup_x1;pop2]=>[装载2]` 与
/// `[装载×2;dup_x1;pop]=>[装载2;装载1]` 两窗。
///
/// 宽语义声明：纯 DOM 层 ldc 载荷类型不可判别，loadInt/loadFloat/loadStr
/// 叶子对 ldc 族一致宽匹配——被匹配序列的栈形状本身保证折叠栈正确
/// （cat1 与 pop / cat2 与 pop2 对齐），触发面比 CP 判别宽；ldc2_w 不入
/// 装载族（cat2 只认 lconst/dconst 家族）。
///
/// 收敛语义声明：窗口纪律（首条豁免：控制流/异常表边界不得锚定窗口
/// 中部，labelsAnchorAt 覆盖分支与异常表两角色）拒收的窗口按窗口粒度
/// 跳过、其余计划继续（原挂账兑现；旧载体对异常表锚定整方法保守终止）；
/// 纪律门外的 DOM 拒绝仅剩内部错误族，保守终止本方法收敛。全部计划被
/// 拒收即无进展，防死循环。
///
/// maxStack/maxLocals 不变（折叠不改变栈深峰值）；发生替换返回 none()，
/// 否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:simplify`
/// - `stability:default`
class StackOperationSimplifier : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
