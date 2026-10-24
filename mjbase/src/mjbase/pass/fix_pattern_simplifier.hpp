#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 固定模式简化器：用更简单的等价字节码替换已登记的固定混淆模式
///        （移植自 mbase pass/opt/fix_pattern_simplifier）。
///
/// 定位：`ArithmeticSimplifier` 折叠直接常量算术、`ExpressionSimplifier` 处理
/// 可复用代数恒等式，本 pass 处理**命名的字节码惯用模式**——每条规则匹配一个
/// 短指令窗口并产出显式的替换序列，刻意不做通用抽象。
///
/// ## 规则集（与旧实现逐条同序）
///
/// 位运算固定模式（栈效应逐一核对，替换前后净效应一致）：
///
/// 1. `ineg; iconst_m1; iadd` → `iconst_m1; ixor`（补码恒等式 -n-1 == ~n）
/// 2. `swap; dup_x1; iand; isub` → `iconst_m1; ixor; iand`
/// 3. `dup_x1; ior; swap; isub` → `iconst_m1; ixor; iand`
/// 4. `dup_x1; ior; isub; ineg` → `iconst_m1; ixor; iand`
/// 5. `swap; isub; iconst_1; isub` → `isub; iconst_m1; ixor`
/// 6. `swap; iconst_m1; ixor; iadd` → `isub; iconst_m1; ixor`
/// 7. `dup_x1; iconst_m1; ixor; iand; iadd` → `ior`（~a & ~b 配 iadd）
/// 8. `loadInt; dup; ixor` → `iconst_0`（x ^ x == 0）
///
/// 死代码清理（legacy 固定序列，匹配即整组删除）：
///
/// 9. `iconst_1; dup; iadd; i2l; l2i; pop`、`iconst_1; pop`、
///    `iconst_1; dup; iadd; dup; i2l; l2i; swap; pop; pop`、
///    `iconst_m1; dup; imul; pop`、`iconst_2; dup; imul; i2l; l2i; dup; swap; pop`、
///    `lconst_1; pop2`、`lconst_0; pop2` → 空序列
///
/// 整数比较折叠（规则 10）：`constInt; iadd; constInt; if_icmp*` →
/// `push(c2 - c1); if_icmp*`。等值/不等值比较按 JVM int 环绕安全；有符号大小
/// 比较仅左侧常量为 0 时变换（非零偏移的 INT_MAX+1 环绕会破坏大小关系）。比较
/// 操作码与分支目标从 matched jump 克隆，六种 if_icmp* 的分支边全部保留。
///
/// 同 slot store/load 中转消除（规则 11）：`loadInt; istore s; loadInt;
/// iload s; if_icmp*` → `P; istore s; C; P; if_icmp*`——在 reload 位 clone 原始
/// producer，istore 保留（slot 窗口外活跃性未知，保守不删）。仅通用 ISTORE/ILOAD
/// 形态参与（旧栈 step 谓词逐字对齐）；短形态（istore_n/iload_n）不匹配。
///
/// ## 写回与边界纪律
///
/// - 迭代至不动点；每轮每条规则至多一次替换（各自右界命中）——复刻旧栈
///   PatternMatcher::apply 的轮次结构，保证规则交互的终态一致。
/// - 边界守卫（旧栈 LabelNode/LineNumberNode/FrameNode 阻断窗口匹配的 CodeDom
///   等价）：窗口内部成员前置边界有存活标签锚定（分支目标 / 异常表 / 调试
///   边界）即跳过该窗口。首成员的标签随替换迁移到替换头（空替换时为窗口
///   后继 / 方法末尾）——等价旧栈标签留原位指向新序列头的落点。
/// - **已声明的保守偏离**（与 A 批方法体改写族同一类目）：行号边界在 CodeDom
///   不可见（LNT 是 backing 属性），跨行号边界的窗口照常变换——旧栈不折叠。
///   SMT 帧目标悬垂由管线帧链接线显式失败（不产出缺帧类），不静默。
/// - 替换头继承窗口首成员的来源 pc（srcPc）：LNT 等 pc 索引属性经 pcmap
///   重定向时，锚在旧首成员 pc 上的条目落到替换头。reload 位的第二个 producer
///   clone 不继承 pc（LNT 锚只归窗口头）。
/// - 节点所有权：替换全部由新构造或 clone 产生，绝不复用被删成员（CodeDom
///   句柄在 erase 后即失效）。
/// - 窗口替换的编辑器协议（头插 + 标签迁移 + 顺序删除）收敛在本文件
///   replaceWindow 一处；libjbc CodeEditor 缺一等 replaceWindow 原语的缺口
///   见模块地图挂账。
///
/// ## 终止性
///
/// 已知自匹配形态已由规则内守卫排除：规则 11 的 producer 为通用 ILOAD 且 slot
/// 与 ISTORE 相同时，替换式与窗口同形（`[ILOAD s; ISTORE s; C; ILOAD s; JMP]`），
/// 旧栈在此形态上无限自匹配（apply-until-converge 挂死）——本实现显式放弃该
/// 匹配（替换为恒等，放弃不损失语义），属对旧栈缺陷的显式修正。此外设不动点
/// 轮次熔断（越界抛 logic_error），把不可预见的发散从挂死转为显式失败。
///
/// 发生替换返回 none()，否则 all()。maxStack/maxLocals 不变（替换栈效应守恒，
/// 死代码族只降不升）。
///
/// @par 标签
/// - `scope:method`
/// - `action:simplify`
/// - `stability:opt-in`
class FixPatternSimplifier : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
