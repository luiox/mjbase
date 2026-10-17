#include <gtest/gtest.h>

#include "mjbase/expr/expr_parser.hpp"
#include "mjbase/expr/expr_eval.hpp"

using namespace mjbase::expr;

TEST(MjExprParserTest, parseNumber)
{
    auto e = parseExpression("42");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 42);
}

TEST(MjExprParserTest, parseHex)
{
    auto e = parseExpression("0xFF");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 255);
}

/// 回归：超界字面量按 u64 累积（无符号回绕，定义良好），词法期不再有 i64
/// 有符号溢出 UB。2^64+3 回绕为 3，再按默认 B32 宽度截断后仍为 3。
TEST(MjExprParserTest, parseHugeLiteralWrapsLikeEval)
{
    auto e = parseExpression("18446744073709551619");   // 2^64 + 3
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 3);
}

TEST(MjExprParserTest, parseAdd)
{
    auto e = parseExpression("1 + 2");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 3);
}

TEST(MjExprParserTest, parsePrecedence)
{
    auto e = parseExpression("1 + 2 * 3");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 7);   // 1 + (2*3), not (1+2)*3
}

TEST(MjExprParserTest, parseParens)
{
    auto e = parseExpression("(1 + 2) * 3");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 9);
}

TEST(MjExprParserTest, parseSub)
{
    auto e = parseExpression("10 - 3");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 7);
}

TEST(MjExprParserTest, parseMulDiv)
{
    auto e = parseExpression("12 / 3 * 2");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 8);   // (12/3)*2
}

TEST(MjExprParserTest, parseUnaryNeg)
{
    auto e = parseExpression("-5");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), -5);
}

TEST(MjExprParserTest, parseUnaryNot)
{
    auto e = parseExpression("~0");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.truncated(), -1);
}

TEST(MjExprParserTest, parseBitwise)
{
    auto e = parseExpression("0xFF & 0x0F");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 0x0F);
}

TEST(MjExprParserTest, parseXor)
{
    auto e = parseExpression("0xFF ^ 0x0F");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 0xF0);
}

TEST(MjExprParserTest, parseOr)
{
    auto e = parseExpression("0xF0 | 0x0F");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 0xFF);
}

TEST(MjExprParserTest, parseShift)
{
    auto e = parseExpression("1 << 3");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 8);
}

TEST(MjExprParserTest, parseVar)
{
    auto e = parseExpression("x + 1");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e, {{"x", ExprValue(5)}});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 6);
}

TEST(MjExprParserTest, parseComplex)
{
    // (1 + 3) * (5 + 2) ^ 0xFF
    auto e = parseExpression("(1 + 3) * (5 + 2) ^ 0xFF");
    ASSERT_NE(e, nullptr);
    auto r = eval(*e);
    ASSERT_TRUE(r.ok);
    // 4 * 7 = 28, 28 ^ 255 = 227
    EXPECT_EQ(r.value.rawValue(), 28 ^ 255);
}

TEST(MjExprParserTest, parseError)
{
    std::string err;
    auto        e = parseExpression("1 + + 2", &err);
    EXPECT_EQ(e, nullptr);
    EXPECT_FALSE(err.empty());
}

TEST(MjExprParserTest, parseEmptyError)
{
    std::string err;
    auto        e = parseExpression("", &err);
    EXPECT_EQ(e, nullptr);
}
