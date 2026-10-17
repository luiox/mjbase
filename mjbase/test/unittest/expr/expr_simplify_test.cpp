#include <gtest/gtest.h>

#include "mjbase/expr/expr_simplify.hpp"
#include "mjbase/expr/expr_eval.hpp"
#include "mjbase/expr/expr_util.hpp"

using namespace mjbase::expr;

// ── Constant folding ──

TEST(MjExprSimplifyTest, constantFoldAdd)
{
    auto expr   = exprAdd(exprConst(10), exprConst(20));
    auto result = simplify(expr);
    ASSERT_TRUE(result.converged);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), 30);
}

TEST(MjExprSimplifyTest, constantFoldNested)
{
    // (1 + 2) * (3 + 4) → 3 * 7 → 21
    auto expr   = exprMul(exprAdd(exprConst(1), exprConst(2)), exprAdd(exprConst(3), exprConst(4)));
    auto result = simplify(expr);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), 21);
}

TEST(MjExprSimplifyTest, constantFoldMixed)
{
    // (1 + x) * (3 + 4) → (1 + x) * 7  (partial fold)
    auto expr   = exprMul(exprAdd(exprConst(1), exprVar("x")), exprAdd(exprConst(3), exprConst(4)));
    auto result = simplify(expr);
    EXPECT_EQ(nodeCount(*result.simplified), 5);   // mul(add(1,x), const(7))
    EXPECT_TRUE(result.converged);
}

TEST(MjExprSimplifyTest, constantFoldNeg)
{
    auto expr   = exprNeg(exprConst(42));
    auto result = simplify(expr);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), -42);
}

// ── Identity simplification ──

TEST(MjExprSimplifyTest, identityXPlus0)
{
    auto expr   = exprAdd(exprVar("x"), exprConst(0));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identity0PlusX)
{
    auto expr   = exprAdd(exprConst(0), exprVar("x"));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityXMinus0)
{
    auto expr   = exprSub(exprVar("x"), exprConst(0));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityXMul1)
{
    auto expr   = exprMul(exprVar("x"), exprConst(1));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identity1MulX)
{
    auto expr   = exprMul(exprConst(1), exprVar("x"));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityXMul0)
{
    auto expr   = exprMul(exprVar("x"), exprConst(0));
    auto result = simplify(expr);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), 0);
}

TEST(MjExprSimplifyTest, identityXDiv1)
{
    auto expr   = exprDiv(exprVar("x"), exprConst(1));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityXor0)
{
    auto expr   = exprXor(exprVar("x"), exprConst(0));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityAndMinus1)
{
    auto expr   = exprAnd(exprVar("x"), exprConst(-1));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityAnd0)
{
    auto expr   = exprAnd(exprVar("x"), exprConst(0));
    auto result = simplify(expr);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), 0);
}

TEST(MjExprSimplifyTest, identityOr0)
{
    auto expr   = exprOr(exprVar("x"), exprConst(0));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityOrMinus1)
{
    auto expr   = exprOr(exprVar("x"), exprConst(-1));
    auto result = simplify(expr);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), -1);
}

TEST(MjExprSimplifyTest, identityNegNeg)
{
    auto expr   = exprNeg(exprNeg(exprVar("x")));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityNotNot)
{
    auto expr   = exprNot(exprNot(exprVar("x")));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityShl0)
{
    auto expr   = exprShl(exprVar("x"), exprConst(0));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityShr0)
{
    auto expr   = exprShr(exprVar("x"), exprConst(0));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

// ── Combined: constant fold + identity ──

TEST(MjExprSimplifyTest, combinedSimplify)
{
    // (x + 0) + (5 + 10) → x + 15
    auto expr = exprAdd(exprAdd(exprVar("x"), exprConst(0)), exprAdd(exprConst(5), exprConst(10)));
    auto result = simplify(expr);
    EXPECT_EQ(nodeCount(*result.simplified), 3);   // add(x, 15)
    EXPECT_TRUE(result.simplified->structuralEqual(*exprAdd(exprVar("x"), exprConst(15))));
}

TEST(MjExprSimplifyTest, deepSimplify)
{
    // (((x * 1) + 0) * (y * 0)) + (10 - 0)
    auto expr   = exprAdd(exprMul(exprAdd(exprMul(exprVar("x"), exprConst(1)), exprConst(0)),
                                exprMul(exprVar("y"), exprConst(0))),
                        exprSub(exprConst(10), exprConst(0)));
    auto result = simplify(expr);
    // Expected: (x * 0) + 10 → 0 + 10 → 10
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), 10);
}

// ── SolveResult fields ──

TEST(MjExprSimplifyTest, solveResultFields)
{
    auto expr   = exprAdd(exprConst(1), exprConst(2));
    auto result = simplify(expr);
    EXPECT_GT(result.nodesBefore, 0);
    EXPECT_GT(result.nodesAfter, 0);
    EXPECT_TRUE(result.converged);
    EXPECT_GE(result.passesApplied, 1);
}

// ── Max nodes guard ──

TEST(MjExprSimplifyTest, maxNodesGuard)
{
    // Build a large tree to test maxNodes
    ExprRef big = exprConst(0);
    for (int i = 0; i < 100; i++) {
        big = exprAdd(big, exprConst(i));
    }
    SolveConfig cfg;
    cfg.maxNodes = 10;
    auto result  = simplify(big, cfg);
    // Should stop early without crash
    EXPECT_GT(result.nodesBefore, 0);
}

// ── Same-capture equality rules ──

TEST(MjExprSimplifyTest, identityXSubX)
{
    auto expr   = exprSub(exprVar("x"), exprVar("x"));
    auto result = simplify(expr);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), 0);
}

TEST(MjExprSimplifyTest, identityXorX)
{
    auto expr   = exprXor(exprVar("x"), exprVar("x"));
    auto result = simplify(expr);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), 0);
}

TEST(MjExprSimplifyTest, identityXPlusX)
{
    // x + x → x * 2 → x << 1
    auto expr   = exprAdd(exprVar("x"), exprVar("x"));
    auto result = simplify(expr);
    // Either x * 2 or x << 1 is acceptable (semantically equivalent)
    bool ok = result.simplified->structuralEqual(*exprMul(exprVar("x"), exprConst(2))) ||
              result.simplified->structuralEqual(*exprShl(exprVar("x"), exprConst(1)));
    EXPECT_TRUE(ok);
}

TEST(MjExprSimplifyTest, identityAndSelf)
{
    auto expr   = exprAnd(exprVar("x"), exprVar("x"));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityOrSelf)
{
    auto expr   = exprOr(exprVar("x"), exprVar("x"));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, sameCaptureDeep)
{
    // (a + b) - (a + b) → 0
    auto common = exprAdd(exprVar("a"), exprVar("b"));
    auto expr   = exprSub(common, common);
    auto result = simplify(expr);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), 0);
}

// ── MBA reverse simplify ──

TEST(MjExprSimplifyTest, mbaReverseXorShl)
{
    // (x ^ y) + ((x & y) << 1) → x + y
    auto expr   = exprAdd(exprXor(exprVar("x"), exprVar("y")),
                        exprShl(exprAnd(exprVar("x"), exprVar("y")), exprConst(1)));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprAdd(exprVar("x"), exprVar("y"))));
}

TEST(MjExprSimplifyTest, mbaReverseAndOr)
{
    // (x & y) + (x | y) → x + y
    auto expr   = exprAdd(exprAnd(exprVar("x"), exprVar("y")), exprOr(exprVar("x"), exprVar("y")));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprAdd(exprVar("x"), exprVar("y"))));
}

TEST(MjExprSimplifyTest, mbaReverseAndNot)
{
    // (x & y) | (x & ~y) → x
    auto expr =
        exprOr(exprAnd(exprVar("x"), exprVar("y")), exprAnd(exprVar("x"), exprNot(exprVar("y"))));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, mbaReverseDeep)
{
    // Nested MBA: ((x ^ y) + ((x & y) << 1)) * 1 + 0 → x + y
    auto mba    = exprAdd(exprXor(exprVar("x"), exprVar("y")),
                       exprShl(exprAnd(exprVar("x"), exprVar("y")), exprConst(1)));
    auto expr   = exprAdd(exprMul(mba, exprConst(1)), exprConst(0));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprAdd(exprVar("x"), exprVar("y"))));
}

// ── New algebraic identities ──

TEST(MjExprSimplifyTest, identityXorMinus1)
{
    auto expr   = exprXor(exprVar("x"), exprConst(-1));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprNot(exprVar("x"))));
}

TEST(MjExprSimplifyTest, identityXPlusNotX)
{
    auto expr   = exprAdd(exprVar("x"), exprNot(exprVar("x")));
    auto result = simplify(expr);
    ASSERT_EQ(result.simplified->kind(), NodeKind::Const);
    EXPECT_EQ(static_cast<const ConstNode&>(*result.simplified).value(), -1);
}

TEST(MjExprSimplifyTest, identityXMulMinus1)
{
    auto expr   = exprMul(exprVar("x"), exprConst(-1));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprNeg(exprVar("x"))));
}

TEST(MjExprSimplifyTest, identity0SubX)
{
    auto expr   = exprSub(exprConst(0), exprVar("x"));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprNeg(exprVar("x"))));
}

TEST(MjExprSimplifyTest, identity0ShiftX)
{
    EXPECT_TRUE(
        simplify(exprShl(exprConst(0), exprVar("x"))).simplified->structuralEqual(*exprConst(0)));
    EXPECT_TRUE(
        simplify(exprShr(exprConst(0), exprVar("x"))).simplified->structuralEqual(*exprConst(0)));
    EXPECT_TRUE(
        simplify(exprUShr(exprConst(0), exprVar("x"))).simplified->structuralEqual(*exprConst(0)));
}

TEST(MjExprSimplifyTest, identityXUShr0)
{
    auto expr   = exprUShr(exprVar("x"), exprConst(0));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityXPlusYSubX)
{
    auto expr   = exprSub(exprAdd(exprVar("x"), exprVar("y")), exprVar("x"));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("y")));
}

TEST(MjExprSimplifyTest, identityXorYxorY)
{
    auto expr   = exprXor(exprXor(exprVar("x"), exprVar("y")), exprVar("y"));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityXorYxorX)
{
    auto expr   = exprXor(exprXor(exprVar("x"), exprVar("y")), exprVar("x"));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("y")));
}

TEST(MjExprSimplifyTest, identityXAndXorY)
{
    auto expr   = exprAnd(exprVar("x"), exprXor(exprVar("x"), exprVar("y")));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprAnd(exprVar("x"), exprNot(exprVar("y")))));
}

TEST(MjExprSimplifyTest, identityXOrXAndY)
{
    auto expr   = exprOr(exprVar("x"), exprAnd(exprVar("x"), exprVar("y")));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(*exprVar("x")));
}

TEST(MjExprSimplifyTest, identityXPlusYTimes2)
{
    auto expr   = exprMul(exprAdd(exprVar("x"), exprVar("y")), exprConst(2));
    auto result = simplify(expr);
    EXPECT_TRUE(result.simplified->structuralEqual(
        *exprShl(exprAdd(exprVar("x"), exprVar("y")), exprConst(1))));
}
