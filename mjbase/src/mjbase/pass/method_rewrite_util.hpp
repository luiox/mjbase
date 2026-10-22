#pragma once

#include <string_view>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::pass {

/// @brief 方法级指令改写族共享的只读探测面（本批 A 组四 pass 内部使用）。
///
/// 只放「读指令操作数语义 + 标签边界守卫」两件事；不做任何编辑，也不持有
/// 会话状态。常量装载语义见各函数注（含准入怪癖）。
namespace method_rewrite_util {

/// @brief int 常量装载判定。
///
/// 覆盖 ICONST_M1..ICONST_5 / BIPUSH / SIPUSH / LDC(Int)。LDC_W 与 LDC 同义
/// 装载（JVMS 允许 ldc_w 装载 int；按操作码区分时 LDC 形态即可）。
bool intValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i32& out);

/// @brief long 常量装载判定。
///
/// 覆盖 LCONST_0/1 + ldc 族（LDC/LDC_W/LDC2_W）装载 Long——LDC2_W(long)
/// 在 classfile 面同样参与。
bool longValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i64& out);

/// @brief double 常量装载判定。
///
/// 同 long：DCONST_0/1 + ldc 族装载 Double（classfile 面即 LDC2_W(double)）。
bool doubleValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                   double& out);

/// @brief string 常量装载判定（仅 LDC）。
bool stringValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                   std::string_view& out);

/// @brief POP 可消除的常量判定：int / long / string 常量装载。
bool isPopFoldableConstant(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in);

/// @brief POP2 可消除的常量判定：long / double 常量装载（category-2 两类都参与）。
bool isPop2FoldableConstant(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in);

/// @brief 标签边界守卫：anchor 前置边界是否有存活标签锚定。
///
/// CodeDom 没有标签节点；分支目标标签（ControlFlow）与异常表/调试边界
/// （Boundary）锚定在后一成员的前置边界，阻断「相邻两指令」模式匹配。
bool labelsAnchorAt(const jbc::ir::CodeDom& dom, jbc::ir::InsnId anchor);

}   // namespace method_rewrite_util

}   // namespace mjbase::pass
