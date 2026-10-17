#include <gtest/gtest.h>

#include "mjbase/expr/expr_util.hpp"

using namespace mjbase::expr;

// ── depth ──

TEST(MjExprUtilTest, depthConst)
{
    EXPECT_EQ(depth(*exprConst(42)), 1);
}

TEST(MjExprUtilTest, depthUnary)
{
    auto n = exprNeg(exprConst(5));
    EXPECT_EQ(depth(*n), 2);
}

TEST(MjExprUtilTest, depthBinary)
{
    auto n = exprAdd(exprConst(1), exprConst(2));
    EXPECT_EQ(depth(*n), 2);
}

TEST(MjExprUtilTest, depthDeep)
{
    // (((1+2)+3)+4)
    auto n = exprAdd(exprAdd(exprAdd(exprConst(1), exprConst(2)), exprConst(3)), exprConst(4));
    EXPECT_EQ(depth(*n), 4);
}

TEST(MjExprUtilTest, depthBalanced)
{
    // (1+2) * (3+4)  — depth 3
    auto n = exprMul(exprAdd(exprConst(1), exprConst(2)), exprAdd(exprConst(3), exprConst(4)));
    EXPECT_EQ(depth(*n), 3);
}

// ── nodeCount ──

TEST(MjExprUtilTest, nodeCountConst)
{
    EXPECT_EQ(nodeCount(*exprConst(42)), 1);
}

TEST(MjExprUtilTest, nodeCountUnary)
{
    EXPECT_EQ(nodeCount(*exprNeg(exprConst(5))), 2);
}

TEST(MjExprUtilTest, nodeCountBinary)
{
    EXPECT_EQ(nodeCount(*exprAdd(exprConst(1), exprConst(2))), 3);
}

TEST(MjExprUtilTest, nodeCountComplex)
{
    // (x + 3) * (y - 1)  → 6 nodes (1 mul, 1 add, 1 sub, 2 const, 2 var)
    auto n = exprMul(exprAdd(exprVar("x"), exprConst(3)), exprSub(exprVar("y"), exprConst(1)));
    EXPECT_EQ(nodeCount(*n), 7);
}

// ── collectVars ──

TEST(MjExprUtilTest, collectVarsNone)
{
    auto vars = collectVars(*exprConst(42));
    EXPECT_TRUE(vars.empty());
}

TEST(MjExprUtilTest, collectVarsSingle)
{
    auto vars = collectVars(*exprVar("x"));
    EXPECT_EQ(vars.size(), 1);
    EXPECT_TRUE(vars.count("x"));
}

TEST(MjExprUtilTest, collectVarsMultiple)
{
    auto n    = exprMul(exprVar("x"), exprAdd(exprVar("y"), exprVar("x")));
    auto vars = collectVars(*n);
    EXPECT_EQ(vars.size(), 2);
    EXPECT_TRUE(vars.count("x"));
    EXPECT_TRUE(vars.count("y"));
}

// ── replaceAt ──

TEST(MjExprUtilTest, replaceAtRoot)
{
    auto  r  = replaceAt(exprConst(42), {}, exprConst(99));
    auto* cn = static_cast<const ConstNode*>(r.get());
    EXPECT_EQ(cn->value(), 99);
}

TEST(MjExprUtilTest, replaceAtLeftLeaf)
{
    // (1 + 2) → replace left: (99 + 2)
    auto  orig     = exprAdd(exprConst(1), exprConst(2));
    auto  replaced = replaceAt(orig, {0}, exprConst(99));
    auto* bn       = static_cast<const BinaryNode*>(replaced.get());
    EXPECT_EQ(static_cast<const ConstNode*>(bn->left().get())->value(), 99);
    EXPECT_EQ(static_cast<const ConstNode*>(bn->right().get())->value(), 2);
}

TEST(MjExprUtilTest, replaceAtRightLeaf)
{
    auto  orig     = exprAdd(exprConst(1), exprConst(2));
    auto  replaced = replaceAt(orig, {1}, exprConst(99));
    auto* bn       = static_cast<const BinaryNode*>(replaced.get());
    EXPECT_EQ(static_cast<const ConstNode*>(bn->left().get())->value(), 1);
    EXPECT_EQ(static_cast<const ConstNode*>(bn->right().get())->value(), 99);
}

TEST(MjExprUtilTest, replaceAtDeep)
{
    // ((1 + 2) + 3) → replace /0/1 (right of left): (1 + 99) + 3)
    auto  orig     = exprAdd(exprAdd(exprConst(1), exprConst(2)), exprConst(3));
    auto  replaced = replaceAt(orig, {0, 1}, exprConst(99));
    auto* root     = static_cast<const BinaryNode*>(replaced.get());
    auto* left     = static_cast<const BinaryNode*>(root->left().get());
    EXPECT_EQ(static_cast<const ConstNode*>(left->right().get())->value(), 99);
}

TEST(MjExprUtilTest, replaceAtImmutable)
{
    // Original tree is unchanged
    auto  orig     = exprAdd(exprConst(1), exprConst(2));
    auto  replaced = replaceAt(orig, {0}, exprConst(99));
    auto* bn       = static_cast<const BinaryNode*>(orig.get());
    EXPECT_EQ(static_cast<const ConstNode*>(bn->left().get())->value(), 1);
}
