#include <gtest/gtest.h>

#include "mjbase/expr/expr_eval.hpp"
#include "mjbase/expr/expr_print.hpp"

using namespace mjbase::expr;

TEST(MjExprEvalTest, constEval)
{
    auto r = eval(*exprConst(42));
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 42);
}

TEST(MjExprEvalTest, varEval)
{
    auto        expr = exprVar("x");
    VarBindings vars = {{"x", ExprValue(10)}};
    auto        r    = eval(*expr, vars);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 10);
}

TEST(MjExprEvalTest, varUnbound)
{
    auto expr = exprVar("x");
    auto r    = eval(*expr);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("unbound"), std::string::npos);
}

TEST(MjExprEvalTest, addEval)
{
    auto expr = exprAdd(exprConst(10), exprConst(20));
    auto r    = eval(*expr);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 30);
}

TEST(MjExprEvalTest, subEval)
{
    auto expr = exprSub(exprConst(20), exprConst(10));
    auto r    = eval(*expr);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 10);
}

TEST(MjExprEvalTest, mulEval)
{
    auto expr = exprMul(exprConst(6), exprConst(7));
    auto r    = eval(*expr);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 42);
}

TEST(MjExprEvalTest, divEval)
{
    auto expr = exprDiv(exprConst(42), exprConst(6));
    auto r    = eval(*expr);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 7);
}

TEST(MjExprEvalTest, xorEval)
{
    auto expr = exprXor(exprConst(0xFF), exprConst(0x0F));
    auto r    = eval(*expr);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 0xF0);
}

TEST(MjExprEvalTest, complexEval)
{
    // (x + 3) * 2
    auto        expr = exprMul(exprAdd(exprVar("x"), exprConst(3)), exprConst(2));
    VarBindings vars = {{"x", ExprValue(5)}};
    auto        r    = eval(*expr, vars);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 16);
}

TEST(MjExprEvalTest, negEval)
{
    auto expr = exprNeg(exprConst(42));
    auto r    = eval(*expr);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), -42);
}

TEST(MjExprEvalTest, notEval)
{
    auto expr = exprNot(exprConst(0, BitWidth::B32));
    auto r    = eval(*expr, {}, 1024);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.truncated(), -1);
}

TEST(MjExprEvalTest, divByZeroEval)
{
    auto expr = exprDiv(exprConst(42), exprConst(0));
    auto r    = eval(*expr);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 0);
}

TEST(MjExprEvalTest, maxDepthExceeded)
{
    // Build a deep tree: (((1+2)+3)+4)...
    ExprRef expr = exprConst(1);
    for (int i = 2; i <= 100; i++) {
        expr = exprAdd(expr, exprConst(i));
    }
    // Should fail with small depth limit
    auto r = eval(*expr, {}, 5);
    EXPECT_FALSE(r.ok);
}

TEST(MjExprEvalTest, deepTreeWithinLimit)
{
    ExprRef expr = exprConst(1);
    for (int i = 2; i <= 10; i++) {
        expr = exprAdd(expr, exprConst(i));
    }
    auto r = eval(*expr, {}, 128);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 55);   // sum 1..10
}

TEST(MjExprEvalTest, shiftEval)
{
    auto expr = exprShl(exprConst(1), exprConst(3));
    auto r    = eval(*expr);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.value.rawValue(), 8);

    auto expr2 = exprShr(exprConst(16), exprConst(3));
    auto r2    = eval(*expr2);
    EXPECT_TRUE(r2.ok);
    EXPECT_EQ(r2.value.rawValue(), 2);
}
