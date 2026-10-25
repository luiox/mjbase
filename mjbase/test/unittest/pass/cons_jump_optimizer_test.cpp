// ConsJumpOptimizer：常量条件跳转折叠（单槽/双槽/空窗 × 恒真改 GOTO/
// 恒假删窗）+ 窗口内部边界守卫（头标签放行）+ 类型失配拒绝 + PA 纪律。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/cons_jump_optimizer.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器（可指定描述子——折叠产物常含 ireturn）。
Bytes oneMethodI(std::string_view desc, Bytes code)
{
    FixtureBuilder b("t/C");
    b.addMethod({"m", desc, std::move(code)});
    return b.assemble();
}

}   // namespace

// ── 单槽恒真：[ICONST_0; IFEQ] → 头改 GOTO（复用原目标），条件跳删除 ──
TEST(ConsJumpOptimizerTest, oneSlotTakenRewritesHeadToGoto)
{
    // ICONST_0(0) IFEQ→pc6(1) ICONST_5(4) IRETURN(5) BIPUSH 7(6) IRETURN(8)。
    // 0==0 恒真 → GOTO；死落径 ICONST_5/IRETURN 保留（结构级清理另归）。
    const MethodPassRun r =
        runMethodPass(oneMethodI("()I", {0x03, 0x99, 0x00, 0x05, 0x08, 0xac,
                                         0x10, 0x07, 0xac}),
                      mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::GOTO);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::IFEQ), 0);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ICONST_0), 0);
    // 幂等：产物再跑无变化。
    const MethodPassRun r2 = runMethodPass(r.out, mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── 单槽恒假：[ICONST_1; IFEQ] → 整窗删除（直落 fallthrough）──
TEST(ConsJumpOptimizerTest, oneSlotNotTakenDeletesWindow)
{
    // ICONST_1(0) IFEQ→pc4(1) ICONST_5(4) IRETURN(5)。1==0 恒假 → 两删。
    const MethodPassRun r =
        runMethodPass(oneMethodI("()I", {0x04, 0x99, 0x00, 0x03, 0x08, 0xac}),
                      mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_5);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

// ── 空窗恒真：[ACONST_NULL; IFNULL] → GOTO ──
TEST(ConsJumpOptimizerTest, nullTakenRewritesToGoto)
{
    // ACONST_NULL(0) IFNULL→pc5(1) RETURN(4) RETURN(5)。
    const MethodPassRun r =
        runMethodPass(oneMethodI("()V", {0x01, 0xc6, 0x00, 0x04, 0xb1, 0xb1}),
                      mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::GOTO);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::IFNULL), 0);
}

// ── 空窗恒假：[ACONST_NULL; IFNONNULL] → 整窗删除 ──
TEST(ConsJumpOptimizerTest, nullNotTakenDeletesWindow)
{
    const MethodPassRun r =
        runMethodPass(oneMethodI("()V", {0x01, 0xc7, 0x00, 0x04, 0xb1, 0xb1}),
                      mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::RETURN);
    EXPECT_EQ(ops[1], Op::RETURN);
}

// ── 双槽恒真：[ICONST_2; ICONST_3; IF_ICMPLT] → 首常量改 GOTO ──
TEST(ConsJumpOptimizerTest, twoSlotTakenRewritesHeadToGoto)
{
    // ICONST_2(0) ICONST_3(1) IF_ICMPLT→pc6(2) RETURN(5) RETURN(6)。2<3 恒真。
    const MethodPassRun r =
        runMethodPass(oneMethodI("()V", {0x05, 0x06, 0xa1, 0x00, 0x04, 0xb1, 0xb1}),
                      mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::GOTO);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::IF_ICMPLT), 0);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ICONST_2), 0);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ICONST_3), 0);
}

// ── 双槽恒假：[ICONST_3; ICONST_2; IF_ICMPLT] → 三指令全删 ──
TEST(ConsJumpOptimizerTest, twoSlotNotTakenDeletesWindow)
{
    const MethodPassRun r =
        runMethodPass(oneMethodI("()V", {0x06, 0x05, 0xa1, 0x00, 0x04, 0xb1, 0xb1}),
                      mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::RETURN);
    EXPECT_EQ(ops[1], Op::RETURN);
}

// ── 边界守卫：条件跳转自身是分支目标（内部边界锚标签）→ 整窗跳过 ──
TEST(ConsJumpOptimizerTest, interiorLabelBlocksFold)
{
    // ICONST_0(0) IFEQ→pc4(1) RETURN(4) GOTO→pc1(5)。外部跳转落在条件跳转
    // 上：其栈值等待这次消费，折叠拒收。
    const Bytes in = oneMethodI("()V", {0x03, 0x99, 0x00, 0x03, 0xb1, 0xa7, 0xff, 0xfc});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 窗口头是分支目标：放行折叠（从头进入重放整窗，净栈效应为零）──
TEST(ConsJumpOptimizerTest, headLabelFoldsThrough)
{
    // ICONST_1(0) IFEQ→pc4(1) RETURN(4) GOTO→pc0(5)。1==0 恒假 → 两删，
    // 头标签经 fallthrough 链重定向到 RETURN。
    const MethodPassRun r =
        runMethodPass(oneMethodI("()V", {0x04, 0x99, 0x00, 0x03, 0xb1, 0xa7, 0xff, 0xfb}),
                      mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    // 窗口在序列头部删除后，指向头标签的外部 GOTO（原位于窗口后方）
    // 留在 RETURN 之后，其目标经 fallthrough 链重定向到 RETURN。
    EXPECT_EQ(ops[0], Op::RETURN);
    EXPECT_EQ(ops[1], Op::GOTO);
}

// ── 类型失配拒绝：ACONST_NULL 配数值谓词（IFEQ）不入矩阵 ──
TEST(ConsJumpOptimizerTest, nullProducerWithIntPredicateNotFolded)
{
    // Verifier 非法形态，钉防御面：不做跨类型语义假设。
    const Bytes in = oneMethodI("()V", {0x01, 0x99, 0x00, 0x03, 0xb1, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 类型失配拒绝：int 常量配空谓词（IFNULL）不入矩阵 ──
TEST(ConsJumpOptimizerTest, intProducerWithNullPredicateNotFolded)
{
    const Bytes in = oneMethodI("()V", {0x03, 0xc6, 0x00, 0x04, 0xb1, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 引用比较谓词（IF_ACMPEQ）不入矩阵：双 int 常量配 ACMP 不折叠 ──
TEST(ConsJumpOptimizerTest, acmpPredicateNotInMatrix)
{
    // 旧栈对这类 Verifier 非法形态按谓词表假值整窗删除（防御怪癖），本
    // 实现显式拒绝。
    const Bytes in = oneMethodI("()V", {0x03, 0x04, 0xa5, 0x00, 0x04, 0xb1, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 注册面 ──
TEST(ConsJumpOptimizerTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("ConsJumpOptimizer"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("ConsJumpOptimizer");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
