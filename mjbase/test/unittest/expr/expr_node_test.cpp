#include <gtest/gtest.h>

#include "mjbase/expr/expr_node.hpp"

using namespace mjbase::expr;

// ── ConstNode ──

TEST(MjExprNodeTest, constNode)
{
    auto n = exprConst(42, BitWidth::B32);
    EXPECT_EQ(n->kind(), NodeKind::Const);
    auto* cn = static_cast<const ConstNode*>(n.get());
    EXPECT_EQ(cn->value(), 42);
    EXPECT_EQ(cn->bitWidth(), BitWidth::B32);
    EXPECT_EQ(cn->truncatedValue(), 42);
}

TEST(MjExprNodeTest, constNodeTruncate)
{
    auto  n  = exprConst(0x1FF, BitWidth::B8);   // 0x1FF -> -1 (sign-extend)
    auto* cn = static_cast<const ConstNode*>(n.get());
    EXPECT_EQ(cn->truncatedValue(), -1);   // 0xFF sign-extended to 64-bit
}

TEST(MjExprNodeTest, constNodeNegative)
{
    auto  n  = exprConst(-1, BitWidth::B32);
    auto* cn = static_cast<const ConstNode*>(n.get());
    EXPECT_EQ(cn->truncatedValue(), -1);
}

// ── VarNode ──

TEST(MjExprNodeTest, varNode)
{
    auto n = exprVar("x", BitWidth::B64);
    EXPECT_EQ(n->kind(), NodeKind::Var);
    auto* vn = static_cast<const VarNode*>(n.get());
    EXPECT_EQ(vn->name(), "x");
    EXPECT_EQ(vn->bitWidth(), BitWidth::B64);
}

// ── UnaryNode ──

TEST(MjExprNodeTest, unaryNeg)
{
    auto c = exprConst(5);
    auto n = exprNeg(c);
    EXPECT_EQ(n->kind(), NodeKind::UnaryOp);
    auto* un = static_cast<const UnaryNode*>(n.get());
    EXPECT_EQ(un->op(), UnaryOpKind::Neg);
    EXPECT_EQ(un->child()->kind(), NodeKind::Const);
}

TEST(MjExprNodeTest, unaryNot)
{
    auto c = exprConst(0xFF);
    auto n = exprNot(c);
    EXPECT_EQ(n->kind(), NodeKind::UnaryOp);
    auto* un = static_cast<const UnaryNode*>(n.get());
    EXPECT_EQ(un->op(), UnaryOpKind::Not);
}

// ── BinaryNode ──

TEST(MjExprNodeTest, binaryOp)
{
    auto l = exprConst(1);
    auto r = exprConst(2);
    auto n = exprAdd(l, r);
    EXPECT_EQ(n->kind(), NodeKind::BinaryOp);
    auto* bn = static_cast<const BinaryNode*>(n.get());
    EXPECT_EQ(bn->op(), BinOpKind::Add);
    EXPECT_TRUE(bn->left().get() == l.get());
    EXPECT_TRUE(bn->right().get() == r.get());
}

TEST(MjExprNodeTest, allBinaryOps)
{
    auto l = exprConst(1);
    auto r = exprConst(2);
    EXPECT_EQ(exprAdd(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprSub(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprMul(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprDiv(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprRem(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprAnd(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprOr(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprXor(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprShl(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprShr(l, r)->kind(), NodeKind::BinaryOp);
    EXPECT_EQ(exprUShr(l, r)->kind(), NodeKind::BinaryOp);
}

// ── Structural equality ──

TEST(MjExprNodeTest, structuralEqualSame)
{
    auto a = exprAdd(exprConst(1), exprConst(2));
    auto b = exprAdd(exprConst(1), exprConst(2));
    EXPECT_TRUE(a->structuralEqual(*b));
    EXPECT_TRUE(*a == *b);
}

TEST(MjExprNodeTest, structuralEqualDifferent)
{
    auto a = exprAdd(exprConst(1), exprConst(2));
    auto b = exprAdd(exprConst(1), exprConst(3));
    EXPECT_FALSE(a->structuralEqual(*b));
    EXPECT_TRUE(*a != *b);
}

TEST(MjExprNodeTest, structuralEqualDifferentKind)
{
    auto a = exprConst(1);
    auto b = exprVar("x");
    EXPECT_FALSE(a->structuralEqual(*b));
}

TEST(MjExprNodeTest, structuralEqualDifferentOp)
{
    auto a = exprAdd(exprConst(1), exprConst(2));
    auto b = exprSub(exprConst(1), exprConst(2));
    EXPECT_FALSE(a->structuralEqual(*b));
}

// ── Complex tree ──

TEST(MjExprNodeTest, complexTree)
{
    // (x + 3) * (y - 1)
    auto expr = exprMul(exprAdd(exprVar("x"), exprConst(3)), exprSub(exprVar("y"), exprConst(1)));
    EXPECT_EQ(expr->kind(), NodeKind::BinaryOp);
    auto* bn = static_cast<const BinaryNode*>(expr.get());
    EXPECT_EQ(bn->op(), BinOpKind::Mul);
}

// ── Shared sub-expressions (immutable) ──

TEST(MjExprNodeTest, sharedSubExpr)
{
    auto common = exprConst(42);
    auto a      = exprAdd(common, exprConst(1));
    auto b      = exprAdd(common, exprConst(2));
    // Both trees share the same ConstNode(42) instance
    auto* bn_a = static_cast<const BinaryNode*>(a.get());
    auto* bn_b = static_cast<const BinaryNode*>(b.get());
    EXPECT_TRUE(bn_a->left().get() == bn_b->left().get());
}

// ── BitWidth helpers ──

TEST(MjExprNodeTest, bitWidthMask)
{
    EXPECT_EQ(bitWidthMask(BitWidth::B8), 0xFF);
    EXPECT_EQ(bitWidthMask(BitWidth::B16), 0xFFFF);
    EXPECT_EQ(bitWidthMask(BitWidth::B32), 0xFFFFFFFFull);
    EXPECT_EQ(bitWidthMask(BitWidth::B64), -1);
}

TEST(MjExprNodeTest, truncateToBitWidth)
{
    EXPECT_EQ(truncateToBitWidth(0x1FF, BitWidth::B8), -1);   // 0xFF sign-ext
    EXPECT_EQ(truncateToBitWidth(0x100, BitWidth::B8), 0);    // 0x00
    EXPECT_EQ(truncateToBitWidth(0x7F, BitWidth::B8), 127);
    EXPECT_EQ(truncateToBitWidth(0x80, BitWidth::B8), -128);   // sign-ext
    EXPECT_EQ(truncateToBitWidth(0x10000, BitWidth::B16), 0);
    EXPECT_EQ(truncateToBitWidth(0x1FFFFFFFF, BitWidth::B32), -1);   // 0xFFFFFFFF sign-ext
}
