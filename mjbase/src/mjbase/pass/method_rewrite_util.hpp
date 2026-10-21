#pragma once

#include <string_view>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::pass {

/// @brief 方法级指令改写族共享的只读探测面（本批 A 组四 pass 内部使用）。
///
/// 只放「读指令操作数语义 + 标签边界守卫」两件事；不做任何编辑，也不持有
/// 会话状态。
namespace method_rewrite_util {

/// @brief int 常量装载判定。
///
/// 覆盖 ICONST_M1..ICONST_5 / BIPUSH / SIPUSH / LDC(Int)。LDC_W 与 LDC 同义
/// 装载（JVMS 允许 ldc_w 装载 int）。
bool intValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i32& out);

/// @brief long 常量装载判定。
///
/// 覆盖：LCONST_0/1 + ldc 族（LDC/LDC_W/LDC2_W）装载 Long。
bool longValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in, ca::i64& out);

/// @brief double 常量装载判定。
///
/// 同 long：DCONST_0/1 + ldc 族装载 Double。
bool doubleValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                   double& out);

/// @brief string 常量装载判定。
bool stringValueOf(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                   std::string_view& out);

/// @brief POP 可消除的常量判定：
///        int / long / string 常量装载。
bool isPopFoldableConstant(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in);

/// @brief POP2 可消除的常量判定：
///        long / double 常量装载（category-2 两类都参与）。
bool isPop2FoldableConstant(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in);

/// @brief 标签边界守卫：anchor 前置边界是否有存活标签锚定。
///
/// CodeDom 没有标签节点，本谓词即相邻性判据：标签锚定在后一成员的前置
/// 边界 ⟺ 两成员不直接相邻。分支目标标签（ControlFlow）与异常表/调试
/// 边界（Boundary）都算。
bool labelsAnchorAt(const jbc::ir::CodeDom& dom, jbc::ir::InsnId anchor);

}   // namespace method_rewrite_util

}   // namespace mjbase::pass
