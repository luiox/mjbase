#include <gtest/gtest.h>

#include "mjbase/pass/expression_simplifier.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

#include "fix_pattern_fixture.hpp"

namespace mjbase::test {

namespace {

using jbc::core::Op;
using mjbase::pass::ExpressionSimplifier;

const jbc::ir::Insn* insnAt(const DomFixture& fx, ca::usize index)
{
    const auto& order = fx.session->unit().methods[0].code->dom.order();
    if (index >= order.size()) return nullptr;
    return fx.session->unit().methods[0].code->dom.peek(order[index]);
}

}   // namespace

TEST(MjExpressionSimplifierTest, FoldsIdentityAddZero)
{
    DomFixture fx("ExpressionIdentity", "run", "()I", 2, 1, [](Body& b) {
        b.iload(0).op(Op::ICONST_0).op(Op::IADD).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjExpressionSimplifierTest, FoldsZeroAndToZero)
{
    DomFixture fx("ExpressionZeroAnd", "run", "()I", 2, 1, [](Body& b) {
        b.iload(0).op(Op::ICONST_0).op(Op::IAND).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_0);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjExpressionSimplifierTest, RemovesMinusOneAnd)
{
    DomFixture fx("ExpressionMinusOneAnd", "run", "()I", 2, 1, [](Body& b) {
        b.iload(0).op(Op::ICONST_M1).op(Op::IAND).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjExpressionSimplifierTest, FoldsSwapThenAdd)
{
    DomFixture fx("ExpressionSwapAdd", "run", "()I", 2, 2, [](Body& b) {
        b.iload(0).iload(1).op(Op::SWAP).op(Op::IADD).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::ILOAD);
    EXPECT_EQ(ops[2], Op::IADD);
    EXPECT_EQ(ops[3], Op::IRETURN);
}

TEST(MjExpressionSimplifierTest, FoldsAssociatedConstants)
{
    DomFixture fx("ExpressionAssoc", "run", "()I", 3, 1, [](Body& b) {
        b.iload(0).bipush(3).op(Op::IADD).bipush(5).op(Op::IADD).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::BIPUSH);
    EXPECT_EQ(ops[2], Op::IADD);
    EXPECT_EQ(ops[3], Op::IRETURN);

    const jbc::ir::Insn* folded = insnAt(fx, 1);
    ASSERT_NE(folded, nullptr);
    EXPECT_EQ(folded->a, 8);
}

TEST(MjExpressionSimplifierTest, FoldsSelfXor)
{
    DomFixture fx("ExpressionSelfXor", "run", "()I", 2, 1, [](Body& b) {
        b.iload(0).op(Op::DUP).op(Op::IXOR).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_0);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjExpressionSimplifierTest, RewritesShiftLeftOneToMultiply)
{
    DomFixture fx("ExpressionShift", "run", "()I", 2, 1, [](Body& b) {
        b.iload(0).op(Op::ICONST_1).op(Op::ISHL).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::ICONST_2);
    EXPECT_EQ(ops[2], Op::IMUL);
    EXPECT_EQ(ops[3], Op::IRETURN);
}

TEST(MjExpressionSimplifierTest, RemovesDoubleNegation)
{
    DomFixture fx("ExpressionDoubleNeg", "run", "()I", 2, 1, [](Body& b) {
        b.iload(0).op(Op::INEG).op(Op::INEG).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjExpressionSimplifierTest, FoldsOrWithAndNot)
{
    DomFixture fx("ExpressionOrAndNot", "run", "()I", 3, 0, [](Body& b) {
        b.bipush(3).bipush(5).bipush(3).op(Op::ICONST_M1).op(Op::IXOR).op(Op::IAND).op(
            Op::IADD).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    EXPECT_EQ(ops[1], Op::BIPUSH);
    EXPECT_EQ(ops[2], Op::IOR);
    EXPECT_EQ(ops[3], Op::IRETURN);
}

TEST(MjExpressionSimplifierTest, SkipsWindowWithInteriorLabel)
{
    DomFixture fx("ExpressionLabelGuard", "run", "()I", 2, 1, [](Body& b) {
        b.iload(0);
        const jbc::ir::LabelId label = b.label();
        b.op(Op::ICONST_0);
        b.bindHere(label);
        b.op(Op::IADD).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::ICONST_0);
    EXPECT_EQ(ops[2], Op::IADD);
    EXPECT_EQ(ops[3], Op::IRETURN);
}

TEST(MjExpressionSimplifierTest, LeavesNonMatchingExpressionUntouched)
{
    DomFixture fx("ExpressionNoMatch", "run", "()I", 2, 2, [](Body& b) {
        b.iload(0).iload(1).op(Op::IMUL).op(Op::IRETURN);
    });

    fx.run(ExpressionSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::ILOAD);
    EXPECT_EQ(ops[2], Op::IMUL);
    EXPECT_EQ(ops[3], Op::IRETURN);
}

}   // namespace mjbase::test
