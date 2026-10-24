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
    // 异常表 start 锚在窗口内部成员（ICONST_0）上：旧栈 LabelNode 阻断窗口，
    // CodeDom 等价即内部成员标签守卫——整窗跳过，x+0 不折叠。
    // （裸 bindHere 标签无引用会在 lower→promote 往返中丢失，必须用异常表
    // 边界锚定才能穿过管线。）
    DomFixture fx("ExpressionLabelGuard", "run", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::ICONST_0);
        const jbc::ir::InsnId atConst = b.last();
        b.op(Op::IADD);
        const jbc::ir::LabelId start = b.label();
        const jbc::ir::LabelId hit   = b.label();
        b.branch(Op::IFEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        const jbc::ir::InsnId atEnd = b.last();
        const jbc::ir::LabelId end = b.label();
        b.op(Op::ICONST_1);
        const jbc::ir::InsnId atHit = b.last();
        b.op(Op::IRETURN);
        b.bindAt(start, atConst);
        b.bindAt(end, atEnd);
        b.bindAt(hit, atHit);
        b.tryCatch(start, end, hit);
    });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(ExpressionSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
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
