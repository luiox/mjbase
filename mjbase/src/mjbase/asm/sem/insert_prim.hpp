/// @file        insert_prim.hpp
/// @brief       插入原语：outline/merge 落地路径共用的字节码插入机械。
///
/// 定位：apply 路径的插入机械单点在此——标签重锚、参数装载序幕、栈形
/// 重排（返回值插回原位）、DUP/紧凑栈操作发射；OutlinePlan::apply
/// （调用点重写）与 MergePlan::apply（回迁）都消费这些原语，其他设施
/// 直接用原语而非各自内嵌。
///
/// 能力面（四轴，全部有直接单测 insert_prim_test）：
///   1. 标签重锚（insertAnchoredAtLabel）：序列落标签边界、标签重锚到
///      首条——goto 入边形态下参数装载块随入边走（首条装载锚定）。
///      CodeEditor::insertAtLabel 语义
///      （标签恒指新插入指令）要求按执行序逆序逐条插入，终序 = 程序序。
///   2. fallthrough 批量插入（insertBeforeInsn）：锚指令前正序插入。
///   3. 带参数处理的前导装载（localLoadInsn / copySetLoadPreamble）：
///      描述符 → 显式局部装载指令（族轴 loadOpOf），落局部/栈序列消费
///      ——N 体栈参序幕（描述符落局部）与调用点局部入参 preamble 同源。
///   4. DUP 复位发射（entryDupResetInsn，调用点侧栈形重排）：
///      复制入口栈顶（副本作实参、原值留栈下、返回值落顶插回原位）。
///      临时局部过渡（dup_x1 族不可表达的形态）仍由策略层拒绝。
///
/// 值形态原语（descSlots/ValueFamily/loadOpOf/…）为两路径共用的
/// 辅助。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"

#include "mjbase/asm/insn/insn_op.hpp"
#include "mjbase/asm/sem/outline_plan.hpp"

namespace jbc::ir {
class CodeEditor;   ///< 批量插入原语的编辑面（引用形参，前置声明免拉全头）
}

namespace mjbase::asm_sem {

// ---------------------------------------------------------------------------
// 值形态原语（描述符/族/局部操作数形态；装载与发射指令的选型轴）
// ---------------------------------------------------------------------------

/// 描述符首字符的槽位宽（J/D = 2）。
inline ca::u32 descSlots(char head) noexcept
{
    return (head == 'J' || head == 'D') ? 2u : 1u;
}

/// 值类型族：装载/返回指令的 opcode 选择轴。
enum class ValueFamily : ca::u8 { Int, Long, Float, Double, Ref };

inline ValueFamily familyOfDesc(char head) noexcept
{
    switch (head) {
    case 'J': return ValueFamily::Long;
    case 'F': return ValueFamily::Float;
    case 'D': return ValueFamily::Double;
    case 'L':
    case '[': return ValueFamily::Ref;
    default:  return ValueFamily::Int;   /// I/Z/C/S/B
    }
}

inline jbc::core::Op loadOpOf(ValueFamily f) noexcept
{
    switch (f) {
    case ValueFamily::Long:   return jbc::core::Op::LLOAD;
    case ValueFamily::Float:  return jbc::core::Op::FLOAD;
    case ValueFamily::Double: return jbc::core::Op::DLOAD;
    case ValueFamily::Ref:    return jbc::core::Op::ALOAD;
    default:                  return jbc::core::Op::ILOAD;
    }
}

inline jbc::core::Op returnOpOf(ValueFamily f) noexcept
{
    switch (f) {
    case ValueFamily::Long:   return jbc::core::Op::LRETURN;
    case ValueFamily::Float:  return jbc::core::Op::FRETURN;
    case ValueFamily::Double: return jbc::core::Op::DRETURN;
    case ValueFamily::Ref:    return jbc::core::Op::ARETURN;
    default:                  return jbc::core::Op::IRETURN;
    }
}

/// 紧凑 load/store 形态 → 显式形态（重编号后统一显式编码，a = 新槽号）。
inline jbc::core::Op explicitLocalOp(jbc::core::Op op) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(op);
    if (v >= static_cast<ca::u8>(jbc::core::Op::ILOAD_0) && v <= static_cast<ca::u8>(jbc::core::Op::ILOAD_3)) return jbc::core::Op::ILOAD;
    if (v >= static_cast<ca::u8>(jbc::core::Op::LLOAD_0) && v <= static_cast<ca::u8>(jbc::core::Op::LLOAD_3)) return jbc::core::Op::LLOAD;
    if (v >= static_cast<ca::u8>(jbc::core::Op::FLOAD_0) && v <= static_cast<ca::u8>(jbc::core::Op::FLOAD_3)) return jbc::core::Op::FLOAD;
    if (v >= static_cast<ca::u8>(jbc::core::Op::DLOAD_0) && v <= static_cast<ca::u8>(jbc::core::Op::DLOAD_3)) return jbc::core::Op::DLOAD;
    if (v >= static_cast<ca::u8>(jbc::core::Op::ALOAD_0) && v <= static_cast<ca::u8>(jbc::core::Op::ALOAD_3)) return jbc::core::Op::ALOAD;
    if (v >= static_cast<ca::u8>(jbc::core::Op::ISTORE_0) && v <= static_cast<ca::u8>(jbc::core::Op::ISTORE_3)) return jbc::core::Op::ISTORE;
    if (v >= static_cast<ca::u8>(jbc::core::Op::LSTORE_0) && v <= static_cast<ca::u8>(jbc::core::Op::LSTORE_3)) return jbc::core::Op::LSTORE;
    if (v >= static_cast<ca::u8>(jbc::core::Op::FSTORE_0) && v <= static_cast<ca::u8>(jbc::core::Op::FSTORE_3)) return jbc::core::Op::FSTORE;
    if (v >= static_cast<ca::u8>(jbc::core::Op::DSTORE_0) && v <= static_cast<ca::u8>(jbc::core::Op::DSTORE_3)) return jbc::core::Op::DSTORE;
    if (v >= static_cast<ca::u8>(jbc::core::Op::ASTORE_0) && v <= static_cast<ca::u8>(jbc::core::Op::ASTORE_3)) return jbc::core::Op::ASTORE;
    return op;
}

/// 局部操作数指令的槽号（紧凑形态按 opcode 解出，与 asm/analyzer 同口径）。
inline ca::u32 localOperandSlot(const jbc::ir::Insn& in) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(in.op);
    const bool compact = (v >= static_cast<ca::u8>(jbc::core::Op::ILOAD_0)
                          && v <= static_cast<ca::u8>(jbc::core::Op::ALOAD_3))
                         || (v >= static_cast<ca::u8>(jbc::core::Op::ISTORE_0)
                             && v <= static_cast<ca::u8>(jbc::core::Op::ASTORE_3));
    if (compact) {
        const ca::u8 base = v <= static_cast<ca::u8>(jbc::core::Op::ALOAD_3)
                                ? static_cast<ca::u8>(jbc::core::Op::ILOAD_0)
                                : static_cast<ca::u8>(jbc::core::Op::ISTORE_0);
        return static_cast<ca::u32>(v - base) % 4u;
    }
    return static_cast<ca::u32>(in.a);
}

/// 局部槽宽（自足槽排布/fresh 分配步进用：L/D 族占 2 槽）。
inline ca::u32 localOpSlotWidth(jbc::core::Op op) noexcept
{
    using jbc::core::Op;
    return (op == Op::LLOAD || op == Op::DLOAD || op == Op::LSTORE || op == Op::DSTORE) ? 2u : 1u;
}

/// 值语义快照的分支族检查：直线/回迁体内禁止一切控制转移。
inline bool isControlTransfer(jbc::core::Op op) noexcept
{
    using jbc::core::Op;
    return asm_insn::isBranch(op) || asm_insn::isSwitch(op) || asm_insn::isReturn(op)
           || asm_insn::isThrow(op) || op == Op::RET;
}

// ---------------------------------------------------------------------------
// 插入原语
// ---------------------------------------------------------------------------

/// @brief 描述符 → 显式局部装载指令（前导装载原语；族轴 loadOpOf）。
jbc::ir::Insn localLoadInsn(char descHead, ca::u32 slot) noexcept;

/// @brief copy-set 前导装载序列：按 kind 过滤参数、描述符落指定槽选择。
///
/// slotNew=false 取 oldLocal（调用点局部入参装载），true 取 newSlot（N 体
/// 栈参序幕——描述符落局部、区域体自栈消费）。槽选择按栈序自底向上 =
/// 参数序。栈切片参数（Kind::Stack）在调用点免装载（值已在
/// 栈上），由本函数 kind 过滤承担。
std::vector<jbc::ir::Insn> copySetLoadPreamble(const std::vector<SlotCopy>& params,
                                               SlotCopy::Kind kind, bool slotNew);

/// @brief 标签重锚批量插入：insns（程序序）落标签边界，标签重锚到首条。
///
/// CodeEditor::insertAtLabel 语义（标签恒指新插入指令）下按执行序逆序逐条
/// 插入，终序 = 程序序；返回值 = 插入指令句柄（程序序）。goto 入边形态下
/// 首条装载锚定标签 = 参数装载随入边走。任一条失败即
/// Err(EditFailed)。
ca::core::Result<std::vector<jbc::ir::InsnId>, OutlineError>
insertAnchoredAtLabel(jbc::ir::CodeEditor& ed, jbc::ir::LabelId label,
                      const std::vector<jbc::ir::Insn>& insns);

/// @brief fallthrough 批量插入：insns（程序序）正序插锚指令之前。
ca::core::Result<std::vector<jbc::ir::InsnId>, OutlineError>
insertBeforeInsn(jbc::ir::CodeEditor& ed, jbc::ir::InsnId anchor,
                 const std::vector<jbc::ir::Insn>& insns);

/// @brief DUP 复位发射原语（调用点侧栈形重排）：dupOp ∈
///        {DUP, DUP2}，复制入口栈顶（副本作实参、原值留栈下、返回值落顶
///        插回原位）。
jbc::ir::Insn entryDupResetInsn(jbc::core::Op dupOp) noexcept;

}   // namespace mjbase::asm_sem
