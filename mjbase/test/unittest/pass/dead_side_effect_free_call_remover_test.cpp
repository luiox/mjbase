// DeadSideEffectFreeCallRemover：白名单无副作用调用 + 纯计算链被 pop
// 丢弃时整链删除——直弃/隔运算/长整尾链、无调用纯链不动、非白名单调用
// 不动、外部消费逐出、区间门（外来指令/内部标签）、异常表跳过、幂等。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/dead_side_effect_free_call_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "fix_pattern_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

// ── 直弃形态：nanoTime 结果直接 POP2 ──
TEST(DeadSideEffectFreeCallRemoverTest, directDiscardRemoved)
{
    DomFixture fx("t/F", "m", "()V", 4, 1, [&](Body& b) {
        b.invokeStatic("java/lang/System", "nanoTime", "()J");
        b.pop2();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── Math.random ()D 白名单 ──
TEST(DeadSideEffectFreeCallRemoverTest, mathRandomDiscardRemoved)
{
    DomFixture fx("t/F", "m", "()V", 4, 1, [&](Body& b) {
        b.invokeStatic("java/lang/Math", "random", "()D");
        b.pop2();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 链尾运算中转：nanoTime + lconst 后 LADD 再 POP2，三件整删 ──
TEST(DeadSideEffectFreeCallRemoverTest, chainThroughArithRemoved)
{
    DomFixture fx("t/F", "m", "()V", 6, 1, [&](Body& b) {
        b.invokeStatic("java/lang/System", "nanoTime", "()J");
        b.ldcLong(111111111L);
        b.op(Op::LADD);
        b.pop2();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 长整尾链：currentTimeMillis → L2I → IADD(lconst) → POP ──
TEST(DeadSideEffectFreeCallRemoverTest, longTailNarrowChainRemoved)
{
    DomFixture fx("t/F", "m", "()V", 6, 1, [&](Body& b) {
        b.invokeStatic("java/lang/System", "currentTimeMillis", "()J");
        b.op(Op::L2I);
        b.iconst_1();
        b.iadd();
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 纯常量链（无白名单调用）不属本件：不动 ──
TEST(DeadSideEffectFreeCallRemoverTest, constOnlyChainKept)
{
    DomFixture fx("t/F", "m", "()V", 4, 1, [&](Body& b) {
        b.iconst_1();
        b.iconst_2();
        b.iadd();
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 5u);
}

// ── 非白名单调用不动 ──
TEST(DeadSideEffectFreeCallRemoverTest, nonWhitelistedCallKept)
{
    DomFixture fx("t/F", "m", "()V", 4, 1, [&](Body& b) {
        b.invokeStatic("java/util/Objects", "hashCode", "(Ljava/lang/Object;)I");
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 3u);
}

// ── 白名单调用结果同时喂 LSTORE（链外消费）→ 整体不动 ──
TEST(DeadSideEffectFreeCallRemoverTest, externalConsumerEvictsChain)
{
    DomFixture fx("t/F", "m", "()V", 6, 2, [&](Body& b) {
        b.invokeStatic("java/lang/System", "nanoTime", "()J");
        b.dup2();
        b.op(Op::LSTORE_2);
        b.pop2();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 5u);
}

// ── 区间门：链与 pop 之间有外来指令 → 不动 ──
TEST(DeadSideEffectFreeCallRemoverTest, foreignInsnInsideRangeKept)
{
    DomFixture fx("t/F", "m", "()V", 6, 1, [&](Body& b) {
        b.invokeStatic("java/lang/System", "nanoTime", "()J");
        b.nop();
        b.pop2();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 4u);
}

// ── 区间门：pop 前有标签锚定（分支中途进入）→ 不动 ──
TEST(DeadSideEffectFreeCallRemoverTest, interiorLabelBlocksRange)
{
    DomFixture fx("t/F", "m", "()V", 6, 1, [&](Body& b) {
        jbc::ir::LabelId tgt = b.label();
        b.iconst_1();
        b.branch(Op::IFEQ, tgt);
        b.vreturn();
        b.bindHere(tgt);
        b.invokeStatic("java/lang/System", "nanoTime", "()J");
        b.pop2();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[3], Op::INVOKESTATIC);
}

// ── 含异常表的方法整件跳过（handler 入口值无定义边）──
TEST(DeadSideEffectFreeCallRemoverTest, excTableSkips)
{
    DomFixture fx("t/F", "m", "()V", 6, 1, [&](Body& b) {
        jbc::ir::LabelId start = b.label();
        jbc::ir::LabelId end   = b.label();
        jbc::ir::LabelId hnd   = b.label();
        b.invokeStatic("java/lang/System", "nanoTime", "()J");
        b.bindHere(start);
        b.pop2();
        b.bindHere(end);
        b.vreturn();
        // handler 体（弹 throwable 后返回）。
        b.pop();
        b.bindHere(hnd);
        b.vreturn();
        b.tryCatch(start, end, hnd, 0);
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::INVOKESTATIC);
}

// ── 幂等：已收敛方法再跑不动 ──
TEST(DeadSideEffectFreeCallRemoverTest, idempotentAfterConvergence)
{
    DomFixture fx("t/F", "m", "()V", 4, 1, [&](Body& b) {
        b.invokeStatic("java/lang/System", "nanoTime", "()J");
        b.pop2();
        b.vreturn();
    });
    ASSERT_TRUE(fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{}).changed());
    const auto pa2 = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa2.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 1u);
}

// ── 写回一致性：链删除后类字节可写回 ──
TEST(DeadSideEffectFreeCallRemoverTest, writeBackConsistentAfterRemoval)
{
    DomFixture fx("t/F", "m", "()V", 4, 1, [&](Body& b) {
        b.invokeStatic("java/lang/System", "nanoTime", "()J");
        b.ldcLong(111111111L);
        b.op(Op::LADD);
        b.pop2();
        b.vreturn();
    });
    ASSERT_TRUE(fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{}).changed());
    const Bytes out = fx.writeBack();
    EXPECT_FALSE(out.empty());
}

// ── URCE 吸收面：常量字符串接收者 + String.length 弹弃整删 ──
TEST(DeadSideEffectFreeCallRemoverTest, stringLengthOnConstRemoved)
{
    DomFixture fx("t/F", "m", "()V", 4, 1, [&](Body& b) {
        const ca::u32 lenCp =
            b.session().internMethodRef("java/lang/String", "length", "()I");
        b.ldcString("abc");
        b.invoke(Op::INVOKEVIRTUAL, lenCp);
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── URCE 吸收面：Math.max 两常量实参 + 弹弃整删 ──
TEST(DeadSideEffectFreeCallRemoverTest, mathMaxConstArgsRemoved)
{
    DomFixture fx("t/F", "m", "()V", 4, 1, [&](Body& b) {
        b.iconst_1();
        b.iconst_2();
        b.invokeStatic("java/lang/Math", "max", "(II)I");
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── URCE 吸收面：cat2 返回宽（Math.abs (J)J → POP2）──
TEST(DeadSideEffectFreeCallRemoverTest, mathAbsLongPop2Removed)
{
    DomFixture fx("t/F", "m", "()V", 4, 3, [&](Body& b) {
        b.ldcLong(111111111L);
        b.invokeStatic("java/lang/Math", "abs", "(J)J");
        b.pop2();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 带参白名单的实参是参数装载（不在链内）→ 不动 ──
TEST(DeadSideEffectFreeCallRemoverTest, paramArgCallKept)
{
    DomFixture fx("t/F", "m", "(I)V", 4, 1, [&](Body& b) {
        b.iload(0);
        b.invokeStatic("java/lang/Math", "abs", "(I)I");
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[1], Op::INVOKESTATIC);
}

// ── String.length 接收者是参数装载（不在链内）→ 不动 ──
TEST(DeadSideEffectFreeCallRemoverTest, paramReceiverCallKept)
{
    DomFixture fx("t/F", "m", "(Ljava/lang/String;)V", 4, 1, [&](Body& b) {
        const ca::u32 lenCp =
            b.session().internMethodRef("java/lang/String", "length", "()I");
        b.op(Op::ALOAD_0);
        b.invoke(Op::INVOKEVIRTUAL, lenCp);
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[1], Op::INVOKEVIRTUAL);
}

// ── 安全扩展声明：纯计算实参（常量经 IADD）同样入链整删 ──
TEST(DeadSideEffectFreeCallRemoverTest, computedArgChainRemoved)
{
    DomFixture fx("t/F", "m", "()V", 4, 1, [&](Body& b) {
        b.iconst_1();
        b.iconst_2();
        b.iadd();
        b.invokeStatic("java/lang/Math", "abs", "(I)I");
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::DeadSideEffectFreeCallRemover{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 注册面 ──
TEST(DeadSideEffectFreeCallRemoverTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("DeadSideEffectFreeCallRemover"));
    auto created =
        mjbase::spi::defaultPassRegistry().createMethodPass("DeadSideEffectFreeCallRemover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
