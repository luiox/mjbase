// RedundantLoadValEliminator：POP 吸收可折叠常量的不动点扫描 + 怪癖保留 +
// 边界守卫 + PA 纪律。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/redundant_load_val_eliminator.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器。
Bytes oneMethod(std::string_view name, Bytes code)
{
    FixtureBuilder b("t/R");
    b.addMethod({name, "()V", std::move(code)});
    return b.assemble();
}

}   // namespace

// ── int 常量对（ICONST 族）被 POP 吸收 ──
TEST(RedundantLoadValEliminatorTest, intPairFolded)
{
    // ICONST_5; POP; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x08, 0x57, 0xb1}),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── string 常量对被 POP 吸收 ──
TEST(RedundantLoadValEliminatorTest, stringPairFolded)
{
    FixtureBuilder b("t/R");
    const ca::u16  sc = b.strConst("x");
    b.addMethod({"m", "()V", {0x12, static_cast<ca::u8>(sc), 0x57, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── POP2 吸收 long 常量对（LCONST 与 LDC2_W 装载都在消除面）──
TEST(RedundantLoadValEliminatorTest, longPairFoldedByPop2)
{
    // LCONST_1; POP2(0x58); RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x0a, 0x58, 0xb1}),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 不动点：嵌套两层对一次跑净 ──
TEST(RedundantLoadValEliminatorTest, fixpointSweepsNestedPairs)
{
    // BIPUSH 9; BIPUSH 9; POP; POP; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x10, 9, 0x10, 9, 0x57, 0x57, 0xb1}),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── POP2 吸收 LDC2_W 装载的 long──
TEST(RedundantLoadValEliminatorTest, ldc2wLongFoldedByPop2)
{
    FixtureBuilder b("t/R");
    const ca::u16  lng = b.longConst(5);
    b.addMethod({"m",
                 "()V",
                 {0x14, static_cast<ca::u8>(lng >> 8), static_cast<ca::u8>(lng), 0x58, 0xb1},
                 0x0009,
                 {},
                 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── POP2 吸收 DCONST_0──
TEST(RedundantLoadValEliminatorTest, dconstPairFoldedByPop2)
{
    // DCONST_0; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x0e, 0x58, 0xb1}),
                                          mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── LDC2_W 指向非 cat2 常量：不折叠（防御面：消除只认 long/double）──
TEST(RedundantLoadValEliminatorTest, nonCat2Ldc2wNotFolded)
{
    // LDC2_W 指向 #1（Class 条目，非 long/double）——constOf 解析不出
    // Long/Double → 不折叠。合法 classfile 无此形态（long/double 恒走
    // LDC2_W），此处钉死本 pass 的防御面。
    const Bytes in = oneMethod("m", {0x14, 0x00, 0x01, 0x58, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 非常量被 POP：不动 ──
TEST(RedundantLoadValEliminatorTest, nonConstantOperandUntouched)
{
    // ALOAD_0; POP; RETURN（static 方法里 ALOAD_0 也照常不折叠——非常量）。
    const Bytes in = oneMethod("m", {0x2a, 0x57, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 边界守卫：POP 是分支目标（有标签锚定）时整对跳过 ──
TEST(RedundantLoadValEliminatorTest, branchTargetPopBlocksPair)
{
    // ICONST_1; POP; RETURN；GOTO 反向跳回 POP（pc 1）。
    // code: ICONST_1(0) POP(1) RETURN(2) GOTO(3)——offset -2 落回 pc1。
    FixtureBuilder b("t/R");
    b.addMethod({"m", "()V", {0x04, 0x57, 0xb1, 0xa8, 0xff, 0xfe}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::RedundantLoadValEliminator{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::POP);
}

// ── 注册面 ──
TEST(RedundantLoadValEliminatorTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("RedundantLoadValEliminator"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("RedundantLoadValEliminator");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
