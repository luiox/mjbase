#include "expr_value.hpp"

#include <cstdint>
#include <climits>

#include "jstd/arith.hpp"

namespace mjbase::expr {

// 整数运算统一走 jstd::arith（仓内 Java 32/64 位整数语义唯一权威实现）：
// 有符号溢出在 C++ 是 UB，这里用无符号位运算回绕规避；INT64_MIN / -1 在
// x86 上会触发除法异常（SIGFPE），div/rem 内部已按 Java 语义回绕处理。

static ca::i64 doShl(ca::i64 a, ca::i64 b)
{
    return jstd::arith::shl(a, static_cast<int>(b & 0x3F));
}

static ca::i64 doShr(ca::i64 a, ca::i64 b)
{
    return jstd::arith::shr(a, static_cast<int>(b & 0x3F));
}

static ca::i64 doUShr(ca::i64 a, ca::i64 b)
{
    return jstd::arith::ushr(a, static_cast<int>(b & 0x3F));
}

// ── Arithmetic operators ──

ExprValue operator+(const ExprValue& a, const ExprValue& b)
{
    return ExprValue::apply(a, b, [](ca::i64 x, ca::i64 y) { return jstd::arith::add(x, y); });
}

ExprValue operator-(const ExprValue& a, const ExprValue& b)
{
    return ExprValue::apply(a, b, [](ca::i64 x, ca::i64 y) { return jstd::arith::sub(x, y); });
}

ExprValue operator*(const ExprValue& a, const ExprValue& b)
{
    return ExprValue::apply(a, b, [](ca::i64 x, ca::i64 y) { return jstd::arith::mul(x, y); });
}

ExprValue operator/(const ExprValue& a, const ExprValue& b)
{
    return ExprValue::apply(a, b, [](ca::i64 x, ca::i64 y) {
        if (y == 0) return static_cast<ca::i64>(0);   // 本类型既有约定：除 0 得 0
        return jstd::arith::div(x, y);                // INT64_MIN / -1 回绕，不触发 SIGFPE
    });
}

ExprValue operator%(const ExprValue& a, const ExprValue& b)
{
    return ExprValue::apply(a, b, [](ca::i64 x, ca::i64 y) {
        if (y == 0) return static_cast<ca::i64>(0);   // 本类型既有约定：模 0 得 0
        return jstd::arith::rem(x, y);                // INT64_MIN % -1 == 0
    });
}

ExprValue operator^(const ExprValue& a, const ExprValue& b)
{
    return ExprValue::apply(a, b, [](ca::i64 x, ca::i64 y) { return x ^ y; });
}

ExprValue operator&(const ExprValue& a, const ExprValue& b)
{
    return ExprValue::apply(a, b, [](ca::i64 x, ca::i64 y) { return x & y; });
}

ExprValue operator|(const ExprValue& a, const ExprValue& b)
{
    return ExprValue::apply(a, b, [](ca::i64 x, ca::i64 y) { return x | y; });
}

ExprValue ExprValue::operator-() const noexcept
{
    return ExprValue(jstd::arith::neg(val_), bw_);
}

ExprValue ExprValue::operator~() const noexcept
{
    return ExprValue(~val_, bw_);
}

ExprValue ExprValue::shl(const ExprValue& a, const ExprValue& b)
{
    // Truncate left operand to its bit width first, then shift
    auto truncated = a.truncated();
    return ExprValue(doShl(truncated, b.val_), a.bw_, UnsignedTag{});
}

ExprValue ExprValue::shr(const ExprValue& a, const ExprValue& b)
{
    auto truncated = a.truncated();
    return ExprValue(doShr(truncated, b.val_), a.bw_, UnsignedTag{});
}

ExprValue ExprValue::ushr(const ExprValue& a, const ExprValue& b)
{
    // Treat as unsigned N-bit value: truncate to bit width, shift, mask
    ca::u64 raw = static_cast<ca::u64>(a.truncated()) & static_cast<ca::u64>(bitWidthMask(a.bw_));
    auto    shifted = raw >> static_cast<unsigned>(b.val_ & 0x3F);
    return ExprValue(static_cast<ca::i64>(shifted), a.bw_, UnsignedTag{});
}

bool operator==(const ExprValue& a, const ExprValue& b)
{
    return a.truncated() == b.truncated();
}

bool operator!=(const ExprValue& a, const ExprValue& b)
{
    return !(a == b);
}

}   // namespace mjbase::expr
