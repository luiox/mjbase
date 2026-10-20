/// @file        branch_facts.hpp
/// @brief       分支预测事实：常量操作数可判定时的条件分支/switch 走向。
///
/// 旧栈对应物：asm/analyzer 的 jump_predicting_analyzer（predictIf /
/// predictIfTwo / predictLookupSwitch / predictTableSwitch 的常量判定路径）与
/// reanalyzer 的 EvaluatorBranchPredictor（null 系判定）。迁移纪律是语义照旧、
/// 实现重写：预测是 ConstResult 到达帧上的纯函数查询——读分支指令入口帧的
/// 操作数栈顶常量，按 JVM 条件语义给走向；条件比较符与旧 MathHelper::condition
/// 的两版逐一对应。
///
/// 与旧实现的形态差（见迁移报告）：
/// - 旧栈把预测织进分析引擎（预测器返回 nullopt 时引擎按双后继入队）；本件
///   只答「走向事实」这个查询，不做控制流修剪——DomainEngine 无分支条件
///   钩子，修剪应用属引擎层能力，待裁决。
/// - 一元条件的 double 路径不迁（旧 predictIf 把 double 常量截断为 int 再
///   判定）：本域不折浮点，无此载荷。
/// - IF_ACMPEQ/IF_ACMPNE 判定合并了旧 ReAnalyzer 预测器「两操作数均已知即
///   可比」的语义：引用常量只有 Null 与 String（JVMS 5.1 保证 CP String
///   解析走 intern，同内容即同实例），内容相等即可安全判定引用相等。
#pragma once

#include "libca/core/datatype.hpp"

#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "const_domain.hpp"

namespace mjbase::asm_analyzer {

/// @brief 分支预测事实层级。
enum class BranchFact : ca::u8 {
    Unknown = 0,   ///< 操作数未知或指令不是可判定形状——保守双后继
    Taken,         ///< 条件成立：走跳转目标
    FallThrough,   ///< 条件不成立：走顺序后继
    Case,          ///< switch 命中：caseIndex 指向 targets 下标（-1 = default）
};

/// @brief 一条分支指令的预测结果。
struct BranchPrediction
{
    BranchFact fact = BranchFact::Unknown;
    /// 仅 Case 有意义：TABLESWITCH/LOOKUPSWITCH 的 targets 下标，-1 为
    /// default；其余形态恒 -1。
    ca::i32 caseIndex = -1;
};

/// @brief 预测一条分支指令的走向。
///
/// 读取 frames 在 branch 入口帧的操作数栈顶（二元条件读栈顶两行），覆盖：
/// 一元 int 条件（IFEQ..IFLE）、null 系（IFNULL/IFNONNULL，Null/String 常量
/// 可判定）、二元 int 条件（IF_ICMPxx）、引用相等（IF_ACMPEQ/IF_ACMPNE，
/// Null/String 常量间按 intern 语义判定）、TABLESWITCH/LOOKUPSWITCH（Int 键
/// 命中 case 或 default）。
///
/// @param u      被分析的方法体（读指令本体：opcode 与 switch 的 targets/keys；
///               须是 frames 对应的同一 CodeUnit）。
/// @param frames 常量传播结果（ok=false 时全部查询退回 Unknown）。
/// @param branch 分支指令句柄；非条件分支（GOTO/return 等无条件控制流）与
///               无效句柄返回 Unknown——无条件走向是引擎职责，不是预测问题。
BranchPrediction predictBranch(const jbc::ir::CodeUnit& u, const ConstResult& frames,
                               jbc::ir::InsnId branch);

}   // namespace mjbase::asm_analyzer
