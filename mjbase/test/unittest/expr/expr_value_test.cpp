#include <gtest/gtest.h>

#include "mjbase/expr/expr_value.hpp"

using namespace mjbase::expr;

// ── Construction ──

TEST(MjExprValueTest, construction)
{
    ExprValue v(42);
    EXPECT_EQ(v.rawValue(), 42);
    EXPECT_EQ(v.bitWidth(), BitWidth::B32);
    EXPECT_EQ(v.truncated(), 42);
}

TEST(MjExprValueTest, truncationOnConstruction)
{
    ExprValue v(0x1FF, BitWidth::B8);
    EXPECT_EQ(v.truncated(), -1);   // 0xFF sign-ext
}

// ── Arithmetic ──

TEST(MjExprValueTest, add)
{
    EXPECT_EQ((ExprValue(10) + ExprValue(20)).truncated(), 30);
}

TEST(MjExprValueTest, addOverflow)
{
    // 2^31-1 + 1 should wrap to -2^31 for i32
    ExprValue max(0x7FFFFFFF, BitWidth::B32);
    ExprValue one(1, BitWidth::B32);
    auto      result = max + one;
    EXPECT_EQ(result.truncated(), static_cast<ca::i64>(INT32_MIN));
}

TEST(MjExprValueTest, sub)
{
    EXPECT_EQ((ExprValue(20) - ExprValue(10)).truncated(), 10);
}

TEST(MjExprValueTest, mul)
{
    EXPECT_EQ((ExprValue(6) * ExprValue(7)).truncated(), 42);
}

TEST(MjExprValueTest, div)
{
    EXPECT_EQ((ExprValue(42) / ExprValue(6)).truncated(), 7);
}

TEST(MjExprValueTest, divByZero)
{
    EXPECT_EQ((ExprValue(42) / ExprValue(0)).truncated(), 0);
}

TEST(MjExprValueTest, rem)
{
    EXPECT_EQ((ExprValue(42) % ExprValue(5)).truncated(), 2);
}

TEST(MjExprValueTest, remByZero)
{
    EXPECT_EQ((ExprValue(42) % ExprValue(0)).truncated(), 0);
}

/// 回归：B64 下 INT64_MIN / -1 在 i64 上溢出，x86 直接触发除法异常（SIGFPE）。
/// 修复后经 jstd::arith::div 按 Java 语义回绕为 INT64_MIN；rem 为 0。
TEST(MjExprValueTest, divRemInt64MinByMinusOne)
{
    ExprValue min(INT64_MIN, BitWidth::B64);
    ExprValue minusOne(-1, BitWidth::B64);
    EXPECT_EQ((min / minusOne).truncated(), INT64_MIN);
    EXPECT_EQ((min % minusOne).truncated(), 0);
}

/// 回归：B64 加减乘与取负在 INT64 边界必须回绕而非有符号溢出 UB。
TEST(MjExprValueTest, int64BoundaryArithmeticWraps)
{
    ExprValue max(INT64_MAX, BitWidth::B64);
    ExprValue min(INT64_MIN, BitWidth::B64);
    ExprValue one(1, BitWidth::B64);
    ExprValue two(2, BitWidth::B64);
    EXPECT_EQ((max + one).truncated(), INT64_MIN);
    EXPECT_EQ((min - one).truncated(), INT64_MAX);
    EXPECT_EQ((max * two).truncated(), -2);   // 2*(2^63-1) mod 2^64
    EXPECT_EQ((-min).truncated(), INT64_MIN);
}

/// 回归：shl 移入符号位（1 << 63）走 u64 位移，结果为 INT64_MIN 而非 UB。
TEST(MjExprValueTest, shlIntoSignBitWraps)
{
    EXPECT_EQ(ExprValue::shl(ExprValue(1, BitWidth::B64), ExprValue(63, BitWidth::B64))
                  .truncated(),
              INT64_MIN);
    EXPECT_EQ((-ExprValue(INT64_MIN, BitWidth::B64)).truncated(), INT64_MIN);
}

// ── Bitwise ──

TEST(MjExprValueTest, xor_)
{
    ExprValue a(0xFF, BitWidth::B32);
    ExprValue b(0x0F, BitWidth::B32);
    EXPECT_EQ((a ^ b).truncated(), 0xF0);
}

TEST(MjExprValueTest, and_)
{
    ExprValue a(0xFF, BitWidth::B32);
    ExprValue b(0x0F, BitWidth::B32);
    EXPECT_EQ((a & b).truncated(), 0x0F);
}

TEST(MjExprValueTest, or_)
{
    ExprValue a(0xF0, BitWidth::B32);
    ExprValue b(0x0F, BitWidth::B32);
    EXPECT_EQ((a | b).truncated(), 0xFF);
}

// ── Unary ──

TEST(MjExprValueTest, negate)
{
    EXPECT_EQ((-ExprValue(42)).truncated(), -42);
}

TEST(MjExprValueTest, bitwiseNot)
{
    EXPECT_EQ((~ExprValue(0, BitWidth::B32)).truncated(), -1);
    EXPECT_EQ((~ExprValue(0xFFFFFFFFull, BitWidth::B32)).truncated(), 0);
}

// ── Shifts ──

TEST(MjExprValueTest, shl)
{
    EXPECT_EQ(ExprValue::shl(ExprValue(1), ExprValue(3)).truncated(), 8);
}

TEST(MjExprValueTest, shr)
{
    EXPECT_EQ(ExprValue::shr(ExprValue(16), ExprValue(3)).truncated(), 2);
}

TEST(MjExprValueTest, ushr)
{
    ExprValue neg(-1, BitWidth::B32);
    EXPECT_EQ(ExprValue::ushr(neg, ExprValue(1)).truncated(), 0x7FFFFFFF);
}

// ── Bit width mixing ──

TEST(MjExprValueTest, widerBitWidth)
{
    ExprValue a(100, BitWidth::B32);
    ExprValue b(200, BitWidth::B64);
    auto      r = a + b;
    EXPECT_EQ(r.bitWidth(), BitWidth::B64);
}

// ── Comparison ──

TEST(MjExprValueTest, equality)
{
    EXPECT_TRUE(ExprValue(42) == ExprValue(42));
    EXPECT_FALSE(ExprValue(42) != ExprValue(42));
    EXPECT_TRUE(ExprValue(42) != ExprValue(43));
}
