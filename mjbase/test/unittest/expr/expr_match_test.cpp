#include <gtest/gtest.h>

#include "mjbase/expr/expr_match.hpp"
#include "mjbase/expr/expr_print.hpp"

using namespace mjbase::expr;

// ── Pattern wild ──

TEST(MjExprMatchTest, wildMatchesAnything)
{
    auto          p = Pattern::Wild();
    MatchBindings b;
    EXPECT_TRUE(p.match(exprConst(42), b));
    EXPECT_TRUE(b.empty());
    EXPECT_TRUE(p.match(exprVar("x"), b));
    EXPECT_TRUE(p.match(exprAdd(exprConst(1), exprConst(2)), b));
}

// ── Pattern capture ──

TEST(MjExprMatchTest, captureBinds)
{
    auto          p = Pattern::Capture("x");
    MatchBindings b;
    auto          n = exprConst(42);
    EXPECT_TRUE(p.match(n, b));
    ASSERT_TRUE(b.count("x"));
    EXPECT_TRUE(b["x"]->structuralEqual(*n));
}

// ── Pattern const ──

TEST(MjExprMatchTest, constMatch)
{
    auto          p = Pattern::Const(42);
    MatchBindings b;
    EXPECT_TRUE(p.match(exprConst(42), b));
    EXPECT_FALSE(p.match(exprConst(99), b));
    EXPECT_FALSE(p.match(exprVar("x"), b));
}

// ── Pattern var ──

TEST(MjExprMatchTest, varMatch)
{
    auto          p = Pattern::Var("x");
    MatchBindings b;
    EXPECT_TRUE(p.match(exprVar("x"), b));
    EXPECT_FALSE(p.match(exprVar("y"), b));
    EXPECT_FALSE(p.match(exprConst(42), b));
}

// ── Pattern unary op ──

TEST(MjExprMatchTest, unaryOpMatch)
{
    auto          p = Pattern::UnaryOp(UnaryOpKind::Neg, Pattern::Capture("x"));
    MatchBindings b;
    auto          n = exprNeg(exprConst(5));
    EXPECT_TRUE(p.match(n, b));
    ASSERT_TRUE(b.count("x"));
    EXPECT_TRUE(b["x"]->structuralEqual(*exprConst(5)));
}

TEST(MjExprMatchTest, unaryOpWrongOp)
{
    auto          p = Pattern::UnaryOp(UnaryOpKind::Neg, Pattern::Wild());
    MatchBindings b;
    EXPECT_FALSE(p.match(exprNot(exprConst(5)), b));
}

// ── Pattern binary op ──

TEST(MjExprMatchTest, binaryOpMatch)
{
    auto          p = Pattern::BinOp(BinOpKind::Add, Pattern::Capture("l"), Pattern::Capture("r"));
    MatchBindings b;
    auto          n = exprAdd(exprConst(1), exprConst(2));
    EXPECT_TRUE(p.match(n, b));
    ASSERT_TRUE(b.count("l"));
    ASSERT_TRUE(b.count("r"));
    EXPECT_TRUE(b["l"]->structuralEqual(*exprConst(1)));
    EXPECT_TRUE(b["r"]->structuralEqual(*exprConst(2)));
}

TEST(MjExprMatchTest, binaryOpWrongOp)
{
    auto          p = Pattern::BinOp(BinOpKind::Add, Pattern::Wild(), Pattern::Wild());
    MatchBindings b;
    EXPECT_FALSE(p.match(exprMul(exprConst(1), exprConst(2)), b));
}

// ── Pattern nested ──

TEST(MjExprMatchTest, nestedPattern)
{
    // Match (x + 5) * 3
    auto p =
        Pattern::BinOp(BinOpKind::Mul,
                       Pattern::BinOp(BinOpKind::Add, Pattern::Capture("x"), Pattern::Const(5)),
                       Pattern::Const(3));
    MatchBindings b;
    auto          n = exprMul(exprAdd(exprVar("a"), exprConst(5)), exprConst(3));
    EXPECT_TRUE(p.match(n, b));
    ASSERT_TRUE(b.count("x"));
    EXPECT_TRUE(b["x"]->structuralEqual(*exprVar("a")));
}

// ── Rewrite simple ──

TEST(MjExprMatchTest, rewriteAddZero)
{
    // Rule: x + 0 → x
    std::vector<PrioritizedRule> rules;
    rules.push_back({0,
                     {Pattern::BinOp(BinOpKind::Add, Pattern::Capture("x"), Pattern::Const(0)),
                      [](const MatchBindings& b) { return b.at("x"); },
                      "x+0->x"}});

    auto expr   = exprAdd(exprConst(42), exprConst(0));
    auto result = rewrite(expr, rules);
    EXPECT_TRUE(result->structuralEqual(*exprConst(42)));
}

TEST(MjExprMatchTest, rewriteZeroAdd)
{
    // Rule: 0 + x → x
    std::vector<PrioritizedRule> rules;
    rules.push_back({0,
                     {Pattern::BinOp(BinOpKind::Add, Pattern::Const(0), Pattern::Capture("x")),
                      [](const MatchBindings& b) { return b.at("x"); },
                      "0+x->x"}});

    auto expr   = exprAdd(exprConst(0), exprConst(99));
    auto result = rewrite(expr, rules);
    EXPECT_TRUE(result->structuralEqual(*exprConst(99)));
}

TEST(MjExprMatchTest, rewriteMulByZero)
{
    std::vector<PrioritizedRule> rules;
    rules.push_back({0,
                     {Pattern::BinOp(BinOpKind::Mul, Pattern::Capture("x"), Pattern::Const(0)),
                      [](const MatchBindings&) { return exprConst(0); },
                      "x*0->0"}});

    auto expr   = exprMul(exprVar("x"), exprConst(0));
    auto result = rewrite(expr, rules);
    EXPECT_TRUE(result->structuralEqual(*exprConst(0)));
}

TEST(MjExprMatchTest, rewriteDeep)
{
    // Apply x+0 → x deeply: ((x+0) + 0) → x
    std::vector<PrioritizedRule> rules;
    rules.push_back({0,
                     {Pattern::BinOp(BinOpKind::Add, Pattern::Capture("x"), Pattern::Const(0)),
                      [](const MatchBindings& b) { return b.at("x"); },
                      "x+0->x"}});

    auto expr   = exprAdd(exprAdd(exprVar("x"), exprConst(0)), exprConst(0));
    auto result = rewrite(expr, rules);
    EXPECT_TRUE(result->structuralEqual(*exprVar("x")));
}

TEST(MjExprMatchTest, rewritePriority)
{
    // Two rules, higher priority wins
    std::vector<PrioritizedRule> rules;
    // Priority 1: x + 0 → x + 99 (weaker)
    rules.push_back({1,
                     {Pattern::BinOp(BinOpKind::Add, Pattern::Capture("x"), Pattern::Const(0)),
                      [](const MatchBindings& b) { return exprAdd(b.at("x"), exprConst(99)); },
                      "x+0->x+99"}});
    // Priority 0: x + 0 → x (stronger — should be tried first)
    rules.push_back({0,
                     {Pattern::BinOp(BinOpKind::Add, Pattern::Capture("x"), Pattern::Const(0)),
                      [](const MatchBindings& b) { return b.at("x"); },
                      "x+0->x"}});

    auto expr   = exprAdd(exprConst(42), exprConst(0));
    auto result = rewrite(expr, rules);
    EXPECT_TRUE(result->structuralEqual(*exprConst(42)));
}

TEST(MjExprMatchTest, rewriteNoMatch)
{
    std::vector<PrioritizedRule> rules;
    rules.push_back({0,
                     {Pattern::BinOp(BinOpKind::Add, Pattern::Capture("x"), Pattern::Const(0)),
                      [](const MatchBindings& b) { return b.at("x"); },
                      "x+0->x"}});

    auto expr   = exprAdd(exprConst(42), exprConst(1));
    auto result = rewrite(expr, rules);
    EXPECT_TRUE(result->structuralEqual(*expr));   // unchanged
}

// ── Same-capture-name equality ──

TEST(MjExprMatchTest, sameCaptureMatch)
{
    // Pattern: x - x → bind once, second occurrence must equal first
    auto          p = Pattern::BinOp(BinOpKind::Sub, Pattern::Capture("x"), Pattern::Capture("x"));
    MatchBindings b;

    // Same expression in both positions → match
    auto n = exprSub(exprVar("a"), exprVar("a"));
    EXPECT_TRUE(p.match(n, b));
    ASSERT_TRUE(b.count("x"));
    EXPECT_TRUE(b["x"]->structuralEqual(*exprVar("a")));
}

TEST(MjExprMatchTest, sameCaptureMismatch)
{
    auto          p = Pattern::BinOp(BinOpKind::Sub, Pattern::Capture("x"), Pattern::Capture("x"));
    MatchBindings b;

    // Different expressions → no match
    auto n = exprSub(exprVar("a"), exprVar("b"));
    EXPECT_FALSE(p.match(n, b));
}

TEST(MjExprMatchTest, sameCaptureDeepMatch)
{
    // Pattern: (a+b) - (a+b)
    auto p = Pattern::BinOp(
        BinOpKind::Sub,
        Pattern::BinOp(BinOpKind::Add, Pattern::Capture("x"), Pattern::Capture("y")),
        Pattern::BinOp(BinOpKind::Add, Pattern::Capture("x"), Pattern::Capture("y")));
    MatchBindings b;

    auto common = exprAdd(exprVar("a"), exprVar("b"));
    auto n      = exprSub(common, common);
    EXPECT_TRUE(p.match(n, b));
}
