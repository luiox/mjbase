/// @file        insn_factory.cpp
/// @brief       typed 指令工厂实现：族籍/值域校验 + Insn 值装配。
#include "insn_factory.hpp"

#include <cmath>

#include "insn_op.hpp"

namespace mjbase::asm_insn {

using ca::core::Err;
using ca::core::Ok;
using jbc::core::Op;
using jbc::core::Shape;
using jbc::ir::Insn;
using jbc::ir::LabelId;

namespace {

/// CP 索引合法域：[1, 0xFFFF]（0 是占位槽，u16 是编码上限）。
bool validCpIndex(ca::u32 cp) noexcept
{
    return cp >= 1 && cp <= 0xFFFF;
}

/// 局部变量槽号合法域（wide 编码上限）。
bool validLocalIdx(ca::u32 idx) noexcept
{
    return idx <= 0xFFFF;
}

/// 标签句柄非空（kNoLabel 不表达任何边界）。
bool validLabel(LabelId l) noexcept
{
    return l != jbc::ir::kNoLabel;
}

/// 分支形状（Branch2/Branch4）。
bool isBranchShape(Shape s) noexcept
{
    return s == Shape::Branch2 || s == Shape::Branch4;
}

/// CP 形状全集。
bool isCpShape(Shape s) noexcept
{
    return s == Shape::CP1 || s == Shape::CP2 || s == Shape::CP2Count ||
           s == Shape::CP2Zero2 || s == Shape::CP2Dims;
}

}   // namespace

const char* to_string(BuildError e) noexcept
{
    switch (e) {
    case BuildError::Ok: return "ok";
    case BuildError::ShapeMismatch: return "op does not belong to this factory family";
    case BuildError::BadOperand: return "operand out of range";
    case BuildError::CountMismatch: return "switch table size mismatch";
    }
    return "?";
}

BuildResult simpleInsn(Op op)
{
    if (op == Op::WIDE) {
        return Err(BuildError::ShapeMismatch);   // wide 是前缀，不是指令
    }
    if (jbc::core::shapeOf(op) != Shape::None) {
        return Err(BuildError::ShapeMismatch);
    }
    Insn i;
    i.op = op;
    return Ok(i);
}

BuildResult localInsn(Op op, ca::u32 idx)
{
    if (jbc::core::shapeOf(op) != Shape::Local1) {
        return Err(BuildError::ShapeMismatch);
    }
    if (!validLocalIdx(idx)) {
        return Err(BuildError::BadOperand);
    }
    Insn i;
    i.op = op;
    i.a  = static_cast<ca::i32>(idx);
    return Ok(i);
}

BuildResult iincInsn(ca::u32 idx, ca::i32 delta)
{
    if (!validLocalIdx(idx)) {
        return Err(BuildError::BadOperand);
    }
    if (delta < -32768 || delta > 32767) {
        return Err(BuildError::BadOperand);   // wide iinc 的 i16 常量域
    }
    Insn i;
    i.op = Op::IINC;
    i.a  = static_cast<ca::i32>(idx);
    i.b  = delta;
    return Ok(i);
}

BuildResult branchInsn(Op op, LabelId target)
{
    if (!isBranchShape(jbc::core::shapeOf(op))) {
        return Err(BuildError::ShapeMismatch);
    }
    if (!validLabel(target)) {
        return Err(BuildError::BadOperand);
    }
    Insn i;
    i.op     = op;
    i.target = target;
    return Ok(i);
}

BuildResult invertedBranch(const Insn& branch)
{
    if (!isConditionalBranch(branch.op)) {
        return Err(BuildError::ShapeMismatch);
    }
    return branchInsn(invertIf(branch.op), branch.target);
}

BuildResult cpInsn(Op op, ca::u32 cp)
{
    if (!isCpShape(jbc::core::shapeOf(op))) {
        return Err(BuildError::ShapeMismatch);
    }
    if (!validCpIndex(cp)) {
        return Err(BuildError::BadOperand);
    }
    if (op == Op::LDC && cp > 0xFF) {
        // LDC 操作数只有 u1 域；升格 LDC_W 由调用方/CodeEditor::loadConst 决定。
        return Err(BuildError::BadOperand);
    }
    Insn i;
    i.op = op;
    i.cp = cp;
    return Ok(i);
}

BuildResult invokeInsn(Op op, ca::u32 cp)
{
    if (!isInvoke(op)) {
        return Err(BuildError::ShapeMismatch);
    }
    return cpInsn(op, cp);
}

BuildResult fieldInsn(Op op, ca::u32 cp)
{
    if (!isFieldAccess(op)) {
        return Err(BuildError::ShapeMismatch);
    }
    return cpInsn(op, cp);
}

BuildResult classInsn(Op op, ca::u32 cp)
{
    switch (op) {
    case Op::NEW:
    case Op::ANEWARRAY:
    case Op::CHECKCAST:
    case Op::INSTANCEOF: break;
    default: return Err(BuildError::ShapeMismatch);
    }
    return cpInsn(op, cp);
}

BuildResult newArrayInsn(ArrayType atype)
{
    Insn i;
    i.op = Op::NEWARRAY;
    i.a  = static_cast<ca::i32>(atype);
    return Ok(i);
}

BuildResult multiANewArrayInsn(ca::u32 cp, ca::u32 dims)
{
    if (dims < 1 || dims > 255) {
        return Err(BuildError::BadOperand);   // u8 操作数域
    }
    Insn i;
    i.op = Op::MULTIANEWARRAY;
    i.cp = cp;
    i.a  = static_cast<ca::i32>(dims);
    return Ok(i);
}

BuildResult tableSwitchInsn(LabelId def, ca::i32 low, ca::i32 high,
                            std::vector<LabelId> targets)
{
    if (!validLabel(def)) {
        return Err(BuildError::BadOperand);
    }
    if (high < low) {
        return Err(BuildError::CountMismatch);
    }
    const ca::u64 count =
        static_cast<ca::u64>(high) - static_cast<ca::u64>(low) + 1ull;
    if (count != static_cast<ca::u64>(targets.size())) {
        return Err(BuildError::CountMismatch);
    }
    for (const LabelId& t : targets) {
        if (!validLabel(t)) {
            return Err(BuildError::BadOperand);
        }
    }
    Insn i;
    i.op      = Op::TABLESWITCH;
    i.target  = def;
    i.a       = low;
    i.b       = high;
    i.targets = std::move(targets);
    return Ok(i);
}

BuildResult lookupSwitchInsn(LabelId def, std::vector<ca::i32> keys,
                             std::vector<LabelId> targets)
{
    if (!validLabel(def)) {
        return Err(BuildError::BadOperand);
    }
    if (keys.size() != targets.size()) {
        return Err(BuildError::CountMismatch);
    }
    for (ca::usize i = 1; i < keys.size(); ++i) {
        if (keys[i] <= keys[i - 1]) {
            return Err(BuildError::CountMismatch);   // 须严格递增（无重复）
        }
    }
    for (const LabelId& t : targets) {
        if (!validLabel(t)) {
            return Err(BuildError::BadOperand);
        }
    }
    Insn i;
    i.op      = Op::LOOKUPSWITCH;
    i.target  = def;
    i.keys    = std::move(keys);
    i.targets = std::move(targets);
    return Ok(i);
}

BuildResult intConstInsn(ca::i32 value)
{
    if (value >= -1 && value <= 5) {
        // ICONST_M1=0x02 起，值 -1..5 线性映射。
        Insn i;
        i.op = static_cast<Op>(0x02 + (value + 1));
        return Ok(i);
    }
    if (value >= -128 && value <= 127) {
        Insn i;
        i.op = Op::BIPUSH;
        i.a  = value;
        return Ok(i);
    }
    if (value >= -32768 && value <= 32767) {
        Insn i;
        i.op = Op::SIPUSH;
        i.a  = value;
        return Ok(i);
    }
    return Err(BuildError::BadOperand);   // 超短形态范围：需 CP 池（ldc 路径）
}

BuildResult longConstInsn(ca::i64 value)
{
    Insn i;
    if (value == 0) {
        i.op = Op::LCONST_0;
        return Ok(i);
    }
    if (value == 1) {
        i.op = Op::LCONST_1;
        return Ok(i);
    }
    return Err(BuildError::BadOperand);   // 需 ldc2_w + 池
}

BuildResult floatConstInsn(float value)
{
    Insn i;
    if (value == 0.0f && !std::signbit(value)) {
        i.op = Op::FCONST_0;
        return Ok(i);
    }
    if (value == 1.0f) {
        i.op = Op::FCONST_1;
        return Ok(i);
    }
    if (value == 2.0f) {
        i.op = Op::FCONST_2;
        return Ok(i);
    }
    return Err(BuildError::BadOperand);   // 含 -0.0f/NaN/inf：需 ldc + 池
}

BuildResult doubleConstInsn(double value)
{
    Insn i;
    if (value == 0.0 && !std::signbit(value)) {
        i.op = Op::DCONST_0;
        return Ok(i);
    }
    if (value == 1.0) {
        i.op = Op::DCONST_1;
        return Ok(i);
    }
    return Err(BuildError::BadOperand);   // 含 -0.0/NaN/inf：需 ldc2_w + 池
}

}   // namespace mjbase::asm_insn
