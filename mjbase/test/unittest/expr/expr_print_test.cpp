#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "mjbase/expr/expr_eval.hpp"
#include "mjbase/expr/expr_parser.hpp"
#include "mjbase/expr/expr_print.hpp"

using namespace mjbase::expr;

TEST(MjExprPrintTest, constSExpr)
{
    auto n = exprConst(42);
    auto s = toSExpr(*n);
    EXPECT_EQ(s, "(const 42:i32)");
}

TEST(MjExprPrintTest, varSExpr)
{
    auto n = exprVar("x", BitWidth::B64);
    auto s = toSExpr(*n);
    EXPECT_EQ(s, "(var x:i64)");
}

TEST(MjExprPrintTest, unarySExpr)
{
    auto n = exprNeg(exprConst(5));
    auto s = toSExpr(*n);
    EXPECT_EQ(s, "(- (const 5:i32))");
}

TEST(MjExprPrintTest, binarySExpr)
{
    auto n = exprAdd(exprConst(1), exprConst(2));
    auto s = toSExpr(*n);
    EXPECT_EQ(s, "(+ (const 1:i32) (const 2:i32))");
}

TEST(MjExprPrintTest, complexSExpr)
{
    // (x + 3) * (y - 1)
    auto n = exprMul(exprAdd(exprVar("x"), exprConst(3)), exprSub(exprVar("y"), exprConst(1)));
    auto s = toSExpr(*n);
    EXPECT_EQ(s, "(* (+ (var x:i32) (const 3:i32)) (- (var y:i32) (const 1:i32)))");
}

TEST(MjExprPrintTest, dotFormat)
{
    auto n   = exprAdd(exprConst(1), exprConst(2));
    auto dot = toDot(*n);
    EXPECT_NE(dot.find("digraph expr"), std::string::npos);
    EXPECT_NE(dot.find("const 1:i32"), std::string::npos);
    EXPECT_NE(dot.find("const 2:i32"), std::string::npos);
    EXPECT_NE(dot.find("+"), std::string::npos);
    EXPECT_NE(dot.find("->"), std::string::npos);
}

// ── toInfix：中缀 + 最小括号（与 expr_parser 优先级一致）─────

namespace {

/// parse → toInfix → parse → toInfix：第二次打印应与第一次一致（固定点），
/// 且重解析前后在给定变量绑定下求值相等。
void checkInfixRoundtrip(const std::string& input,
                         const std::vector<std::pair<std::string, ca::i64>>& vars = {})
{
    auto first = parseExpression(input);
    ASSERT_NE(first, nullptr) << "parse failed: " << input;
    std::string once = toInfix(*first);

    auto second = parseExpression(once);
    ASSERT_NE(second, nullptr) << "reparse failed for toInfix output: " << once;
    std::string twice = toInfix(*second);

    EXPECT_EQ(once, twice) << "print not a fixed point for: " << input;

    VarBindings bindings;
    for (const auto& [name, value] : vars) bindings.emplace(name, ExprValue(value));
    auto v1 = eval(*first, bindings);
    auto v2 = eval(*second, bindings);
    ASSERT_TRUE(v1.ok) << input;
    ASSERT_TRUE(v2.ok) << once;
    EXPECT_EQ(v1.value.rawValue(), v2.value.rawValue()) << "value changed for: " << input;
}

}   // namespace

TEST(MjExprPrintTest, infixRespectsPrecedenceWithoutExtraParens)
{
    auto e = parseExpression("1 + 2 * 3");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(toInfix(*e), "1 + 2 * 3");
}

TEST(MjExprPrintTest, infixParenthesizesLowerPrecedenceLeftChild)
{
    auto e = parseExpression("(1 + 2) * 3");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(toInfix(*e), "(1 + 2) * 3");
}

TEST(MjExprPrintTest, infixParenthesizesEqualPrecedenceRightChild)
{
    auto e = parseExpression("1 - (2 - 3)");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(toInfix(*e), "1 - (2 - 3)");
}

TEST(MjExprPrintTest, infixKeepsLeftAssocChainFlat)
{
    auto e = parseExpression("10 - 3 - 2");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(toInfix(*e), "10 - 3 - 2");
}

TEST(MjExprPrintTest, infixShiftsBindLooserThanAdd)
{
    auto e = parseExpression("1 << 2 + 3");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(toInfix(*e), "1 << 2 + 3");

    auto e2 = parseExpression("(1 << 2) + 3");
    ASSERT_NE(e2, nullptr);
    EXPECT_EQ(toInfix(*e2), "(1 << 2) + 3");
}

TEST(MjExprPrintTest, infixBitwiseFamilyOrdering)
{
    // 解析器优先级：& > ^ > |。
    auto e = parseExpression("a | b ^ c & d");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(toInfix(*e), "a | b ^ c & d");

    auto e2 = parseExpression("(a | b) ^ (c & d)");
    ASSERT_NE(e2, nullptr);
    // & 优先级高于 ^，右操作数 c & d 无需括号即可无损重解析。
    EXPECT_EQ(toInfix(*e2), "(a | b) ^ c & d");
}

TEST(MjExprPrintTest, infixUnaryAndVars)
{
    auto e = parseExpression("~x & -(y + 1)");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(toInfix(*e), "~x & -(y + 1)");
}

TEST(MjExprPrintTest, infixUnsignedShift)
{
    auto e = parseExpression("x >>> 2");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(toInfix(*e), "x >>> 2");
}

TEST(MjExprPrintTest, parserPrinterRoundtripBattery)
{
    checkInfixRoundtrip("1 + 2 * 3");
    checkInfixRoundtrip("(1 + 2) * 3");
    checkInfixRoundtrip("1 - (2 - 3)");
    checkInfixRoundtrip("10 - 3 - 2");
    checkInfixRoundtrip("1 << 2 + 3");
    checkInfixRoundtrip("(1 << 2) + 3");
    checkInfixRoundtrip("a | b ^ c & d", {{"a", 12}, {"b", 7}, {"c", 250}, {"d", 5}});
    checkInfixRoundtrip("~x & -(y + 1)", {{"x", 0x5A}, {"y", 3}});
    checkInfixRoundtrip("x >>> 2", {{"x", -64}});
    checkInfixRoundtrip("(x ^ 0xFF) % 7 + y * -2", {{"x", 1234}, {"y", -9}});
    checkInfixRoundtrip("-(-(-x))", {{"x", 5}});
}
