/// @file        insn_factory.hpp
/// @brief       typed 指令工厂：校验 op 族籍与操作数形状，产出 jbc::ir::Insn 值。
///
/// 定位：jbc::ir::CodeEditor 的 simple/local/branch/cpInsn/loadConst/iinc 是
/// 「不做校验的形状便利」；本工厂族是它们的校验扩展面——op 与操作数形状
/// 不一致时统一返回 Err（Result 值语义，不抛异常、不静默修正），产物由
/// 调用方经 CodeDom::append 入库（句柄归属校验仍由 CodeDom 负责）。
/// 的 typed 工厂方法；CP 操作数在新底座是池索引（u32），不再是 (owner,name,desc)
/// 三段文本——intern 由 ClassSession 负责，工厂只收索引。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

namespace mjbase::asm_insn {

/// @brief newarray 的数组元素类型（JVMS atype 编码值）。
enum class ArrayType : ca::u8 {
    Boolean = 4,
    Char = 5,
    Float = 6,
    Double = 7,
    Byte = 8,
    Short = 9,
    Int = 10,
    Long = 11,
};

/// @brief 工厂失败原因（全部工厂统一 Err 通道）。
enum class BuildError {
    Ok = 0,
    ShapeMismatch,   ///< op 不属于该工厂族（形状/族籍不符）
    BadOperand,      ///< 操作数值域不符（cp=0、局部变量号超界、dims 越界、空标签等）
    CountMismatch,   ///< switch 的表长与 low/high/keys 约束不一致
};

const char* to_string(BuildError e) noexcept;

using BuildResult = ca::core::Result<jbc::ir::Insn, BuildError>;

// ---- 无操作数族 ----

/// @brief Shape::None 的单字节指令（常量/算术/转换/比较/栈操作/监视器/
///        arraylength/athrow/紧凑 load-store 等）。
///
/// 拒绝 WIDE（前缀不是指令）与一切带操作数形状（iinc/局部变量/分支/CP 族
/// 走各自的 typed 工厂）。
BuildResult simpleInsn(jbc::core::Op op);

// ---- 局部变量族 ----

/// @brief Local1 形状族：iload..aload / istore..astore / ret。
/// @param idx 局部变量槽号，值域 [0, 0xFFFF]（wide 编码上限）。
BuildResult localInsn(jbc::core::Op op, ca::u32 idx);

/// @brief iinc（槽号 [0,0xFFFF]；增量 i16 值域）。
BuildResult iincInsn(ca::u32 idx, ca::i32 delta);

// ---- 分支族 ----

/// @brief Branch2/Branch4 形状族：if* / goto / jsr（含 *_w 宽形态）。
/// @param target 目标标签；kNoLabel 返回 Err（空标签不表达任何边界）。
BuildResult branchInsn(jbc::core::Op op, jbc::ir::LabelId target);

/// @brief 条件跳转的取反版本：branchInsn(invertIf(branch.op), branch.target)。
///
/// 用于「反转分支极性」的控制流重写；输入不是条件跳转返回 Err。
BuildResult invertedBranch(const jbc::ir::Insn& branch);

// ---- CP 操作数族 ----

/// @brief 一切 CP 形状（CP1/CP2/CP2Count/CP2Zero2/CP2Dims）的通用入口。
///
/// @param cp 常量池索引，值域 [1, 0xFFFF]；LDC 另受 u1 域约束（> 0xFF 时
///        返回 Err——升格 LDC_W 是调用方或 CodeEditor::loadConst 的职责）。
BuildResult cpInsn(jbc::core::Op op, ca::u32 cp);

/// @brief invoke 家族（invokevirtual/special/static/interface/dynamic）。
BuildResult invokeInsn(jbc::core::Op op, ca::u32 cp);

/// @brief 字段访问族（getstatic/putstatic/getfield/putfield）。
BuildResult fieldInsn(jbc::core::Op op, ca::u32 cp);

/// @brief 类操作数族（new/anewarray/checkcast/instanceof）。
BuildResult classInsn(jbc::core::Op op, ca::u32 cp);

// ---- 数组创建 ----

/// @brief newarray（primitive 元素数组；atype 为 JVMS 编码）。
BuildResult newArrayInsn(ArrayType atype);

/// @brief multianewarray。
/// @param cp   类描述符的池索引，[1, 0xFFFF]。
/// @param dims 维数，[1, 255]（u8 操作数域）。
BuildResult multiANewArrayInsn(ca::u32 cp, ca::u32 dims);

// ---- switch ----

/// @brief tableswitch：targets[i] 对应 key = low + i。
/// @param def     默认目标标签（非空）。
/// @param targets case 目标；长度必须等于 high - low + 1，且每个非空。
BuildResult tableSwitchInsn(jbc::ir::LabelId def, ca::i32 low, ca::i32 high,
                            std::vector<jbc::ir::LabelId> targets);

/// @brief lookupswitch（稀疏 case 表）。
/// @param keys    match 键；须严格递增（JVMS 编码要求），无重复。
/// @param targets 与 keys 等长；每个（含 def）非空。
BuildResult lookupSwitchInsn(jbc::ir::LabelId def, std::vector<ca::i32> keys,
                             std::vector<jbc::ir::LabelId> targets);

// ---- 最短常量装载（无 CP 池参与） ----
//
// 语义照旧 InsnUtil::getIntInsn/getLongInsn/getFloatInsn/getDoubleInsn 的
// 「短形态优先」策略；超短形态覆盖范围的值返回 Err（旧实现回落 LdcInsnNode，
// 新底座 ldc 需要池索引——由调用方 intern 后走 cpInsn(LDC…)/CodeEditor::loadConst）。

/// @brief int 常量：[-1,5]→iconst；[-128,127]→bipush；[-32768,32767]→sipush。
BuildResult intConstInsn(ca::i32 value);

/// @brief long 常量：0/1→lconst；其余 Err（需 ldc2_w + 池）。
BuildResult longConstInsn(ca::i64 value);

/// @brief float 常量：+0.0/1.0/2.0→fconst；其余（含 -0.0f/NaN）Err。
BuildResult floatConstInsn(float value);

/// @brief double 常量：+0.0/1.0→dconst；其余（含 -0.0/NaN）Err。
BuildResult doubleConstInsn(double value);

}   // namespace mjbase::asm_insn
