// UselessPopCleaner：局部装载 + POP/POP2 相邻对的不动点消除 + 弹宽/矩阵
// 边界（常量对、运算生产者不入矩阵）+ 边界守卫 + PA 纪律。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/useless_pop_cleaner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器。
Bytes oneMethod(std::string_view name, Bytes code)
{
    FixtureBuilder b("t/U");
    b.addMethod({name, "()V", std::move(code)});
    return b.assemble();
}

}   // namespace

// ── POP 吸收 category-1 局部装载（紧凑形 ILOAD_1）──
TEST(UselessPopCleanerTest, localIntLoadPairFolded)
{
    // ILOAD_1; POP; RETURN → RETURN。配对删除恒栈中性，不读槽内容。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x1b, 0x57, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── POP 吸收引用装载（ALOAD_0）──
TEST(UselessPopCleanerTest, localRefLoadPairFolded)
{
    // ALOAD_0; POP; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x2a, 0x57, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 矩阵边界：常量装载对不入本矩阵（常量装载面已另立清理件）──
TEST(UselessPopCleanerTest, constantPairNotInMatrix)
{
    // ICONST_1; POP; RETURN——保持不动。
    const Bytes in = oneMethod("m", {0x04, 0x57, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 运算生产者不入矩阵：IADD; POP 不动（弹值生产者是运算，非纯装载）──
TEST(UselessPopCleanerTest, nonFoldableOperandUntouched)
{
    // 形态本身非 Verifier 合法（IADD 弹两推一），仅钉防御面：不匹配不动。
    const Bytes in = oneMethod("m", {0x60, 0x57, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── POP2 吸收 category-2 局部装载（LLOAD_2 / DLOAD_0）──
TEST(UselessPopCleanerTest, wideLocalLoadPairFoldedByPop2)
{
    // LLOAD_2; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x20, 0x58, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(UselessPopCleanerTest, wideLocalDoubleLoadPairFoldedByPop2)
{
    // DLOAD_0; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x26, 0x58, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 弹宽失配：cat1 装载配 POP2 不动（POP2 还要吃更深一槽，删两指令
//    非栈中性；双 cat1 半删归帧面批次）──
TEST(UselessPopCleanerTest, widthMismatchLocalLoadKept)
{
    // ILOAD_1; POP2; RETURN——保持不动。
    const Bytes in = oneMethod("m", {0x1b, 0x58, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 不动点：连续局部装载 + 连续 POP 两轮清空（交错形态）──
TEST(UselessPopCleanerTest, nestedLocalLoadPairsFixpoint)
{
    // ILOAD_1; ILOAD_2; POP; POP; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x1b, 0x1c, 0x57, 0x57, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 首成员（装载）是分支目标：照常消除，引用经 fallthrough 重定向落到
//    原 POP 的后继 ──
TEST(UselessPopCleanerTest, branchTargetLoadRedirectsThroughPair)
{
    // GOTO +3（落在 ILOAD_1）; ILOAD_1; POP; RETURN → GOTO; RETURN（GOTO
    // 重定向到 RETURN）。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", {0xa7, 0x00, 0x03, 0x1b, 0x57, 0xb1}),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::GOTO);
    EXPECT_EQ(ops[1], Op::RETURN);
}

// ── 边界守卫：POP 是分支目标（有标签锚定）时整对跳过 ──
TEST(UselessPopCleanerTest, branchTargetPopBlocksPair)
{
    // ILOAD_1(0) POP(1) RETURN(2) GOTO(3)——offset -2 落回 pc1。
    const Bytes in = oneMethod("m", {0x1b, 0x57, 0xb1, 0xa8, 0xff, 0xfe});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 注册面 ──
TEST(UselessPopCleanerTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("UselessPopCleaner"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("UselessPopCleaner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
