#include <gtest/gtest.h>

#include "mjbase/expr/expr_mba_gen.hpp"
#include "mjbase/expr/expr_eval.hpp"
#include "mjbase/expr/expr_simplify.hpp"
#include "mjbase/expr/expr_util.hpp"

using namespace mjbase::expr;

// ── MBAState RNG ──

TEST(MjExprMbaGenTest, rngDeterministic)
{
    MBAState s1(42), s2(42);
    for (int i = 0; i < 100; ++i) {
        EXPECT_DOUBLE_EQ(s1.nextDouble(), s2.nextDouble());
    }
}

TEST(MjExprMbaGenTest, rngNextInt)
{
    MBAState s(123);
    for (int i = 0; i < 100; ++i) {
        auto v = s.nextInt(-10, 10);
        EXPECT_GE(v, -10);
        EXPECT_LE(v, 10);
    }
}

TEST(MjExprMbaGenTest, rngDifferentSeedDifferent)
{
    MBAState s1(42), s2(99);
    bool     same = true;
    for (int i = 0; i < 10; ++i) {
        if (s1.nextDouble() != s2.nextDouble()) {
            same = false;
            break;
        }
    }
    EXPECT_FALSE(same);
}

// ── generateMBA basic ──

TEST(MjExprMbaGenTest, generateConst)
{
    MBAState  state(42);
    MBAConfig cfg;
    cfg.targetDepth = 2;
    cfg.maxNodes    = 50;

    auto target = exprConst(0x12345678);
    auto obf    = generateMBA(target, cfg, state);

    // Should be more complex than a single const
    EXPECT_GT(nodeCount(*obf), 1);

    // Should evaluate to the same value
    auto result = eval(*obf);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.value.rawValue(), 0x12345678);
}

TEST(MjExprMbaGenTest, generateDeterministic)
{
    MBAState  s1(42), s2(42);
    MBAConfig cfg;
    cfg.targetDepth = 3;

    auto a = generateMBA(exprConst(100), cfg, s1);
    auto b = generateMBA(exprConst(100), cfg, s2);

    // Structural equality (same seed → same tree)
    EXPECT_TRUE(a->structuralEqual(*b));
}

TEST(MjExprMbaGenTest, generateDifferentSeedDifferent)
{
    MBAState  s1(42), s2(999);
    MBAConfig cfg;
    cfg.targetDepth = 3;

    auto a = generateMBA(exprConst(100), cfg, s1);
    auto b = generateMBA(exprConst(100), cfg, s2);

    // Different seeds → different trees (with high probability)
    // Count how many nodes differ
    EXPECT_NE(nodeCount(*a), nodeCount(*b));
}

// ── generateMBA with var target ──

TEST(MjExprMbaGenTest, generateVarTarget)
{
    MBAState  state(7);
    MBAConfig cfg;
    cfg.targetDepth = 2;

    auto target = exprVar("x");
    auto obf    = generateMBA(target, cfg, state);

    EXPECT_GT(nodeCount(*obf), 1);

    // Evaluate with x=5 should get 5
    VarBindings vars   = {{"x", ExprValue(5)}};
    auto        result = eval(*obf, vars);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.value.rawValue(), 5);
}

// ── generateMBA maxNodes guard ──

TEST(MjExprMbaGenTest, generateMaxNodesGuard)
{
    MBAState  state(1);
    MBAConfig cfg;
    cfg.targetDepth = 100;   // would be huge
    cfg.maxNodes    = 10;    // cut off early

    auto obf = generateMBA(exprConst(42), cfg, state);
    // Should not explode — the guard kicks in but multiple identities may
    // create more nodes than maxNodes allows in one pass, so be generous
    EXPECT_LE(nodeCount(*obf), 30);
}

// ── generateMBA with complex expression ──

TEST(MjExprMbaGenTest, generateComplexExpr)
{
    MBAState  state(99);
    MBAConfig cfg;
    cfg.targetDepth = 4;
    cfg.maxNodes    = 100;

    // Target: (x + 3) * 2
    auto target = exprMul(exprAdd(exprVar("x"), exprConst(3)), exprConst(2));
    auto obf    = generateMBA(target, cfg, state);

    EXPECT_GT(nodeCount(*obf), nodeCount(*target));

    VarBindings vars   = {{"x", ExprValue(7)}};
    auto        result = eval(*obf, vars);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.value.rawValue(), 20);   // (7+3)*2 == 20
}

// ── applyMBAObfuscation ──

TEST(MjExprMbaGenTest, applyObfuscation)
{
    MBAState state(42);
    auto     expr = exprAdd(exprVar("x"), exprConst(5));
    auto     obf  = applyMBAObfuscation(expr, 3, state);

    EXPECT_GT(nodeCount(*obf), nodeCount(*expr));

    VarBindings vars   = {{"x", ExprValue(10)}};
    auto        result = eval(*obf, vars);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.value.rawValue(), 15);
}

// ── MBA reverse simplify ──

TEST(MjExprMbaGenTest, mbaSimplifyReverse)
{
    // Generate an MBA expression, then simplify it back
    MBAState  state(42);
    MBAConfig cfg;
    cfg.targetDepth = 5;
    cfg.maxNodes    = 100;

    auto target = exprConst(0xABCD);
    auto obf    = generateMBA(target, cfg, state);

    // Now simplify
    SolveConfig scfg;
    auto        result = simplify(obf, scfg);

    // Should converge to const(0xABCD) or something equivalent
    auto simplifiedResult = eval(*result.simplified);
    ASSERT_TRUE(simplifiedResult.ok);
    EXPECT_EQ(simplifiedResult.value.rawValue(), 0xABCD);
}

// ── Identities list ──

TEST(MjExprMbaGenTest, identitiesNotEmpty)
{
    auto& ids = getIdentitiesByGroup(IdentityGroup::Obfuscate);
    EXPECT_FALSE(ids.empty());

    auto& simplifyIds = getIdentitiesByGroup(IdentityGroup::Simplify);
    EXPECT_TRUE(simplifyIds.empty());   // simplify uses RewriteRules, not identities
}
