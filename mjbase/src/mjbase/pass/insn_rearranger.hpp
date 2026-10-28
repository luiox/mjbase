#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 算术赋值块重排器：连续的 `xLOAD s; CONST; 可交换算术; xSTORE s`
///        四元块按变量槽首现序分组重排（旧栈 mbase pass/opt/
///        insn_rearranger 的 ArithmeticInsnRearranger 重推导）。
///
/// 适配形态：把散落在相邻赋值块里的同槽算术归拢到一起，让后续的
/// 常量折叠 / 表达式化简在同一窗口内命中更多形态。块内顺序不变，只整块
/// 搬移；同槽块保持原相对序（跨槽搬移安全：每块只读写自身槽位且栈进
/// 出自洽，槽间无干扰）。
///
/// 判据：
/// 1. 块匹配：四条连续指令 = 槽 s 的 i/l 族 load + 常量装载 + 同族可交换
///    算术（i: ADD/MUL/AND/OR/XOR；l: 同）+ 槽 s 的同族 store。快捷形
///    （ILOAD_n/ISTORE_n/LLOAD_n/LSTORE_n）与泛形同判（旧栈 tree 模型把
///    快捷形规范化为泛形，此处等价覆盖）。
/// 2. 窗口：连续块收集，< 2 块不动（单块重排无意义）。
/// 3. 标签守卫：窗口起点或任一内部边界锚标签（分支目标/异常表边界）→
///    整窗拒收——锚点指令随重排漂移会改变入边落点，对齐全家「不重锚
///    异常表边界」惯例，fail-closed。
///
/// 相对旧实现偏离（逐条申报）：
/// 1. 载体换 CodeDom 快照 + 原位内容置换（旧栈 clone 重建整列；两者可
///    观测指令序一致），srcPc 随内容搬移。
/// 2. 「≥8 条」入口门槛按指令计数（旧栈按含标签/行号的列表项计数）。
/// 3. 常量装载面补 LDC_W/LDC2_W（旧栈读取器折叠为 LDC，classfile 可观
///    测面等价）。
///
/// 整数溢出保持 JVM 补码环绕语义（只搬移不改值）。
///
/// 发生重排返回 none()，否则 all()。方法/帧分析失效。
///
/// @par 标签
/// - `scope:method`
/// - `action:reorder`
/// - `stability:opt-in`
class ArithmeticInsnRearranger : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&            scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

/// @brief 交换律链常量后移：连续的 int/long 交换律运算链内把常量操作数
///        移到链尾，使常量相邻、常量折叠可命中（旧栈 mbase pass/opt/
///        insn_rearranger 的 CommutativeOperatorReorder 重推导）。
///
/// 链形态：[操作数×(n+1)][同一可交换算术×n]，操作数 = 同宽常量装载
/// （intValueOf/longValueOf 覆盖面）或同宽局部装载（iload/lload 族含快
/// 捷形）。重排 = 操作数稳定划分（非常量在前、常量在后），算术指令原位
/// 不动；仅当划分改变顺序才改写。交换律保证结果值不变（溢出环绕语义下
/// 仍成立——加/乘/与/或/异或对环绕对称）。
///
/// 判据与守卫：
/// 1. 链起点或内部任一边界锚标签 → 该起点不成链，前进一条重扫（旧栈标
///    签节点入列即断链；新栈等价判据）。链尾截断于标签锚点（旧栈 ops
///    运行遇标签节点收尾，同形）。
/// 2. 操作数与算术宽度一致（int 链拒绝 aconst_null/long 操作数，long
///    链反之）。
/// 3. 形状不齐（操作数数 ≠ 算术数 + 1）不成链。
///
/// 相对旧实现偏离（逐条申报）：
/// 1. 载体换 CodeDom 快照 + 原位内容置换，srcPc 随内容搬移。
/// 2. 链起点锚标签的形态旧栈借列表语义隐式重锚（标签跟随列表位置留在
///    链入口）；新栈标签锚定具体指令，重排会把锚点拖进链中改变入边落
///    点 → 该形态保守拒收（fail-closed）。
/// 3. 旧栈头注宣称支持 getstatic I/J 操作数，实现未覆盖（isIntOperand
///    仅常量 + iload）——以旧实现代码为准，不补声称面。
/// 4. 快捷形 load 与 LDC_W/LDC2_W 覆盖（读取器折叠等价面）。
///
/// 发生重排返回 none()，否则 all()。方法/帧分析失效。
///
/// @par 标签
/// - `scope:method`
/// - `action:reorder`
/// - `stability:opt-in`
class CommutativeOperatorReorder : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&            scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
