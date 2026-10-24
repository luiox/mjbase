/// @file        step_util.cpp
/// @brief       语义谓词步骤与常量读写助手实现。
#include "step_util.hpp"

namespace mjbase::asm_matcher {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

constexpr ca::u8 opVal(jbc::core::Op op) noexcept
{
    return static_cast<ca::u8>(op);
}

/// [lo, hi] 闭区间内的 opcode（字节值单调的指令族专用）。
bool opInRange(jbc::core::Op op, jbc::core::Op lo, jbc::core::Op hi) noexcept
{
    return opVal(op) >= opVal(lo) && opVal(op) <= opVal(hi);
}

char normalizeTypeTag(char t) noexcept
{
    if (t >= 'a' && t <= 'z') t = static_cast<char>(t - 'a' + 'A');
    if (t == 'L') t = 'J';
    return t;
}

}   // namespace

MatchStep Steps::loadInt()
{
    return MatchStep::anyIf([](const jbc::ir::Insn& in) {
        using jbc::core::Op;
        return in.op == Op::ILOAD || in.op == Op::BIPUSH || in.op == Op::SIPUSH ||
               in.op == Op::LDC || in.op == Op::LDC_W ||
               opInRange(in.op, Op::ILOAD_0, Op::ILOAD_3) ||
               opInRange(in.op, Op::ICONST_M1, Op::ICONST_5);
    });
}

MatchStep Steps::loadFloat()
{
    return MatchStep::anyIf([](const jbc::ir::Insn& in) {
        using jbc::core::Op;
        return in.op == Op::FLOAD || in.op == Op::LDC || in.op == Op::LDC_W ||
               opInRange(in.op, Op::FLOAD_0, Op::FLOAD_3) ||
               opInRange(in.op, Op::FCONST_0, Op::FCONST_2);
    });
}

MatchStep Steps::loadDouble()
{
    return MatchStep::anyIf([](const jbc::ir::Insn& in) {
        using jbc::core::Op;
        // double 只能经 ldc2_w 装载，同样未计入；族内只剩 dload/dconst。
        return in.op == Op::DLOAD || opInRange(in.op, Op::DLOAD_0, Op::DLOAD_3) ||
               in.op == Op::DCONST_0 || in.op == Op::DCONST_1;
    });
}

MatchStep Steps::loadLong()
{
    return MatchStep::anyIf([](const jbc::ir::Insn& in) {
        using jbc::core::Op;
        return in.op == Op::LLOAD || opInRange(in.op, Op::LLOAD_0, Op::LLOAD_3) ||
               in.op == Op::LCONST_0 || in.op == Op::LCONST_1;
    });
}

MatchStep Steps::loadStr()
{
    return MatchStep::anyIf([](const jbc::ir::Insn& in) {
        // 字符串/类常量只能经 ldc 族；载荷类型在纯 DOM 层不可判别。
        return in.op == jbc::core::Op::LDC || in.op == jbc::core::Op::LDC_W;
    });
}

bool Steps::isConstLoadOfType(char typeTag, const jbc::ir::Insn& in) noexcept
{
    using jbc::core::Op;
    const char t = normalizeTypeTag(typeTag);
    switch (t) {
    case 'I':
        return in.op == Op::BIPUSH || in.op == Op::SIPUSH || in.op == Op::LDC ||
               opInRange(in.op, Op::ICONST_M1, Op::ICONST_5);
    case 'J': return in.op == Op::LCONST_0 || in.op == Op::LCONST_1;
    case 'F':
        return in.op == Op::LDC || opInRange(in.op, Op::FCONST_0, Op::FCONST_2);
    case 'D': return in.op == Op::DCONST_0 || in.op == Op::DCONST_1;
    default: return false;
    }
}

MatchStep Steps::constLoad(char typeTag)
{
    const char t = normalizeTypeTag(typeTag);
    return MatchStep::anyIf(
        [t](const jbc::ir::Insn& in) { return isConstLoadOfType(t, in); });
}

bool Steps::getIntValue(const jbc::ir::Insn& in, ca::i32& out) noexcept
{
    using jbc::core::Op;
    if (opInRange(in.op, Op::ICONST_M1, Op::ICONST_5)) {
        out = static_cast<ca::i32>(opVal(in.op)) - static_cast<ca::i32>(opVal(Op::ICONST_0));
        return true;
    }
    if (in.op == Op::BIPUSH || in.op == Op::SIPUSH) {
        out = in.a;
        return true;
    }
    return false;   // ldc 族载荷是 CP 索引，纯 DOM 层不可读
}

bool Steps::getLongValue(const jbc::ir::Insn& in, ca::i64& out) noexcept
{
    using jbc::core::Op;
    if (in.op == Op::LCONST_0 || in.op == Op::LCONST_1) {
        out = opVal(in.op) - opVal(Op::LCONST_0);
        return true;
    }
    return false;
}

bool Steps::getFloatValue(const jbc::ir::Insn& in, double& out) noexcept
{
    using jbc::core::Op;
    if (opInRange(in.op, Op::FCONST_0, Op::FCONST_2)) {
        out = static_cast<double>(opVal(in.op) - opVal(Op::FCONST_0));
        return true;
    }
    return false;
}

bool Steps::getDoubleValue(const jbc::ir::Insn& in, double& out) noexcept
{
    using jbc::core::Op;
    if (in.op == Op::DCONST_0 || in.op == Op::DCONST_1) {
        out = static_cast<double>(opVal(in.op) - opVal(Op::DCONST_0));
        return true;
    }
    return false;
}

Result<jbc::ir::Insn, MatcherError> Steps::makeIntInsn(ca::i32 v) noexcept
{
    using jbc::core::Op;
    jbc::ir::Insn in;
    if (v >= -1 && v <= 5) {
        // 先整型求和再窄化：v=-1 时目标字节 0x02(iconst_m1)，u8 回绕不可依赖。
        const int raw = static_cast<int>(opVal(Op::ICONST_0)) + v;
        in.op = static_cast<Op>(static_cast<ca::u8>(raw));
        return Ok(in);
    }
    if (v >= -128 && v <= 127) {
        in.op = Op::BIPUSH;
        in.a = v;
        return Ok(in);
    }
    if (v >= -32768 && v <= 32767) {
        in.op = Op::SIPUSH;
        in.a = v;
        return Ok(in);
    }
    return Err(MatcherError{MatchError::EmitUnsupported, 0});   // 更宽需要 ldc + CP
}

Result<jbc::ir::Insn, MatcherError> Steps::makeLongInsn(ca::i64 v) noexcept
{
    jbc::ir::Insn in;
    if (v == 0) {
        in.op = jbc::core::Op::LCONST_0;
        return Ok(in);
    }
    if (v == 1) {
        in.op = jbc::core::Op::LCONST_1;
        return Ok(in);
    }
    return Err(MatcherError{MatchError::EmitUnsupported, 0});   // 需要 ldc2_w + CP
}

Result<jbc::ir::Insn, MatcherError> Steps::makeFloatInsn(double v) noexcept
{
    jbc::ir::Insn in;
    const float f = static_cast<float>(v);
    if (f == 0.0f) {
        in.op = jbc::core::Op::FCONST_0;
        return Ok(in);
    }
    if (f == 1.0f) {
        in.op = jbc::core::Op::FCONST_1;
        return Ok(in);
    }
    if (f == 2.0f) {
        in.op = jbc::core::Op::FCONST_2;
        return Ok(in);
    }
    return Err(MatcherError{MatchError::EmitUnsupported, 0});   // 需要 ldc + CP
}

Result<jbc::ir::Insn, MatcherError> Steps::makeDoubleInsn(double v) noexcept
{
    jbc::ir::Insn in;
    if (v == 0.0) {
        in.op = jbc::core::Op::DCONST_0;
        return Ok(in);
    }
    if (v == 1.0) {
        in.op = jbc::core::Op::DCONST_1;
        return Ok(in);
    }
    return Err(MatcherError{MatchError::EmitUnsupported, 0});   // 需要 ldc2_w + CP
}

}   // namespace mjbase::asm_matcher
