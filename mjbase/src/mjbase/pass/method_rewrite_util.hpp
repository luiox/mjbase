#pragma once

#include <string_view>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::pass {

/// @brief 方法级指令改写族共享的只读探测面（本批 A 组四 pass 内部使用）。
///
/// 只放「读指令操作数语义 + 标签边界守卫」两件事；不做任何编辑，也不持有
/// 会话状态。常量装载语义与旧栈 InsnUtil::getIntValue/getLongValue/
/// getStringValue 逐条对齐（含旧实现的准入怪癖，见各函数注）。
namespace method_rewrite_util {

/// @brief int 常量装载判定（旧栈 InsnUtil::getIntValue 对齐）。
///
/// 覆盖 ICONST_M1..ICONST_5 / BIPUSH / SIPUSH / LDC(Int)。LDC_W 与 LDC 同义
/// 装载（JVMS 允许 ldc_w 装载 int；旧栈 tree 模型把两者折叠为 LDC 节点，
/// 新栈按操作码区分，此处对齐旧语义的覆盖面：LDC 形态即可）。
bool intValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i32& out);

/// @brief long 常量装载判定（旧栈 InsnUtil::getLongValue 可观测行为对齐）。
///
/// 旧栈 tree 域只写 LCONST_0/1 与「LDC 节点装载 long」；其 classfile 可观测
/// 行为是 LDC2_W(long) 同样参与——旧读取器把 LDC2_W/LDC_W 一律折叠为 tree
/// LDC 节点（visitLdcInsn），写回时按常量类别再展开。本判定覆盖两栈共同
/// 可观测面：LCONST_0/1 + ldc 族（LDC/LDC_W/LDC2_W）装载 Long。
bool longValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i64& out);

/// @brief double 常量装载判定（旧栈 InsnUtil::getDoubleValue 可观测行为对齐）。
///
/// 同 long：DCONST_0/1 + ldc 族装载 Double（旧栈 tree 域 LDC 节点持 double，
/// classfile 面即 LDC2_W(double)）。
bool doubleValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                   double& out);

/// @brief string 常量装载判定（旧栈 InsnUtil::getStringValue 对齐：仅 LDC）。
bool stringValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                   std::string_view& out);

/// @brief POP 可消除的常量判定（旧栈 isRedundantPopConstant 对齐）：
///        int / long / string 常量装载。
bool isPopFoldableConstant(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in);

/// @brief POP2 可消除的常量判定（旧栈 isRedundantPop2Constant 对齐）：
///        long / double 常量装载（category-2 两类都参与）。
bool isPop2FoldableConstant(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in);

/// @brief 标签边界守卫：anchor 前置边界是否有存活标签锚定。
///
/// 旧栈 tree 模型里 LabelNode/LineNumberNode 是指令序列的一员，任何「相邻
/// 两指令」模式在两者之间隔有标签节点时天然不匹配。CodeDom 没有标签节点，
/// 本谓词是等价判据：标签锚定在后一成员的前置边界 ⟺ 旧栈两成员之间有
/// LabelNode。分支目标标签（ControlFlow）与异常表/调试边界（Boundary）
/// 都算——旧栈两种节点都阻断相邻匹配。
bool labelsAnchorAt(const jbc::ir::CodeDom& dom, jbc::ir::InsnId anchor);

}   // namespace method_rewrite_util

}   // namespace mjbase::pass
