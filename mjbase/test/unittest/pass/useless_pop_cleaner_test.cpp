// UselessPopCleaner：相位一「局部装载 + POP/POP2 相邻对」不动点消除 +
// 弹宽/矩阵边界 + 边界守卫；相位二帧溯源面（def-use 到达定义）——纯常量
// 生产者 + 唯一消费者 pop 整删、POP2 半删降级、中继守卫、多定义汇合。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/useless_pop_cleaner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器（自定义描述符——帧面测试需要带参方法）。
Bytes oneMethodD(std::string_view name, std::string_view desc, Bytes code)
{
    FixtureBuilder b("t/U");
    b.addMethod({name, desc, std::move(code)});
    return b.assemble();
}

/// 单方法容器（默认 ()V）。
Bytes oneMethod(std::string_view name, Bytes code)
{
    return oneMethodD(name, "()V", std::move(code));
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

// ── 帧面：纯常量生产者 + POP 相邻对（窗口面不管常量，帧面接管）──
TEST(UselessPopCleanerTest, frameConstProducerAbsorbed)
{
    // ICONST_1; POP; RETURN → RETURN。生产者无副作用、唯一消费者是 pop。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x04, 0x57, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 帧面：ACONST_NULL 生产者 ──
TEST(UselessPopCleanerTest, frameAconstNullProducerAbsorbed)
{
    // ACONST_NULL; POP; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x01, 0x57, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 帧面：非相邻形态（窗口面够不着，帧面按到达定义删）──
TEST(UselessPopCleanerTest, frameNonAdjacentConstPopsFolded)
{
    // ICONST_1; ICONST_2; POP; POP; RETURN → RETURN。两个 pop 各自的弹值
    // 到达定义互异、各唯一消费。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", {0x04, 0x05, 0x57, 0x57, 0xb1}),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 运算/分析失败形态不动：IADD; POP 的帧数据流不成立（du.ok() 假，
//    空栈弹双值），帧面跳过；即便分析成立，运算生产者也不在纯常量族 ──
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

// ── 帧面：POP2 单 cat2 常量（long / double）整删 ──
TEST(UselessPopCleanerTest, framePop2LongConstRemoved)
{
    // LCONST_1; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x0a, 0x58, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(UselessPopCleanerTest, framePop2DoubleConstRemoved)
{
    // DCONST_1; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x0f, 0x58, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 帧面：POP2 双 cat1 两侧都可删 → 两个生产者 + POP2 整删 ──
TEST(UselessPopCleanerTest, framePop2TwoCat1BothRemoved)
{
    // ICONST_1; ICONST_2; POP2; RETURN → RETURN。index0=ICONST_2、
    // index1=ICONST_1，各自唯一消费。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x04, 0x05, 0x58, 0xb1}),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 帧面：POP2 半删（顶侧可删、底侧是未播种参数 → 降级 POP）──
TEST(UselessPopCleanerTest, framePop2HalfSplitTopRemoved)
{
    // (I)V：ICONST_1; ILOAD_0; POP2; RETURN → ILOAD_0; POP; RETURN。
    // 底侧弹值是参数装载（无到达定义边），不可删；删顶侧生产者后 POP2
    // 原位降级 POP 弹剩余底值。
    const MethodPassRun r =
        runMethodPass(oneMethodD("m", "(I)V", {0x04, 0x1a, 0x58, 0xb1}),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ILOAD_0);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── 帧面：POP2 半删（底侧可删、顶侧是 store 转发 → 降级 POP）──
TEST(UselessPopCleanerTest, framePop2HalfSplitBottomRemoved)
{
    // ICONST_1; ICONST_2; ISTORE_2; ILOAD_2; POP2; RETURN
    //   → ICONST_2; ISTORE_2; ILOAD_2; POP; RETURN。
    // 顶侧弹值定义经装载转发到 ISTORE_2（非纯常量生产者）不可删；底侧
    // ICONST_1 的值垫在栈底、唯一消费者是本 POP2 → 删 ICONST_1、降级。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", {0x04, 0x05, 0x3d, 0x1c, 0x58, 0xb1}),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::ICONST_2);
    EXPECT_EQ(ops[1], Op::ISTORE_2);
    EXPECT_EQ(ops[2], Op::ILOAD_2);
    EXPECT_EQ(ops[3], Op::POP);
    EXPECT_EQ(ops[4], Op::RETURN);
}

// ── 帧面中继守卫：DUP 在场即禁用帧面（def-use 复制类无使用边，删生产
//    者会留下栈元数破坏的悬空 DUP）──
TEST(UselessPopCleanerTest, frameRelayGuardDisablesFace)
{
    // ICONST_1; DUP; POP; POP; RETURN——整体不动。
    const Bytes in = oneMethod("m", {0x04, 0x59, 0x57, 0x57, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 帧面：多到达定义汇合——各路径各推一个常量、汇合 pop 统一弹弃 ──
TEST(UselessPopCleanerTest, frameMultiDefMergePopsFolded)
{
    // ICONST_1; ICONST_2; IF_ICMPNE→pc9; ICONST_2; GOTO→pc10; ICONST_1;
    // POP; RETURN。POP 的弹值到达定义 = {两条路径的常量生产者}，各自
    // 唯一消费 → 三个生产者/消费者组整删，跳转引用经 fallthrough 重定向
    // 链式落到 RETURN。
    const MethodPassRun r = runMethodPass(
        oneMethod("m", {0x04, 0x05, 0xa0, 0x00, 0x07, 0x05, 0xa7, 0x00, 0x04, 0x04, 0x57, 0xb1}),
        mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::ICONST_2);
    EXPECT_EQ(ops[2], Op::IF_ICMPNE);
    EXPECT_EQ(ops[3], Op::GOTO);
    EXPECT_EQ(ops[4], Op::RETURN);
}

// ── 注册面 ──
TEST(UselessPopCleanerTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("UselessPopCleaner"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("UselessPopCleaner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
