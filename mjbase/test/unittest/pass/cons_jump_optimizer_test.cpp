// ConsJumpOptimizer：常量条件跳转折叠（单槽/双槽/空窗 × 恒真改 GOTO/
// 恒假删窗）+ 窗口内部边界守卫（头标签放行）+ 类型失配拒绝 + PA 纪律。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "jbc/core/opcode.hpp"

#include "mjbase/pass/cons_jump_optimizer.hpp"
#include "mjbase/pass/constant_folder.hpp"
#include "mjbase/pass/fix_pattern_simplifier.hpp"
#include "mjbase/pass/nop_remover.hpp"
#include "mjbase/pass/redundant_load_val_eliminator.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;
namespace jan = jbc::analysis;

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

// ── 布尔物化菱形 + 外部 goto 直指条件跳转：阶段清理 pass 集零改动（守护
// 回归钉）。形态（javac `a == null || a.length == 0` 同构）：
//   ifnull L2 / aload arraylength ifne L0 / iconst_1 goto L1 /
//   L0: iconst_0 / L1: ifeq L3 / L2: iconst_1 goto L4 / L3: iconst_0 /
//   L4: ireturn
// L1 的内部锚（外部 goto 的目标标签落在条件跳转上）是本 pass 拒折窗口
// 的判据；若守护被弱化，恒真折叠会把外部跳边重定向到物化常量之前，制
// 造汇合点入边栈形不一致（弹空边 vs 携值边）。此测试钉：pass 序列跑完
// 字节逐位不变，且 Strict 帧数据流对结果 ok。
TEST(ConsJumpOptimizerTest, booleanDiamondWithGotoIntoConditionUnchanged)
{
    const Bytes in = oneMethodI("([Ljava/lang/Object;)Z", {
        0x2a,               // 0  aload_0
        0xc6, 0x00, 0x10,   // 1  ifnull -> 17（空栈边进 L2）
        0x2a,               // 4  aload_0
        0xbe,               // 5  arraylength
        0x9a, 0x00, 0x07,   // 6  ifne -> 13
        0x04,               // 9  iconst_1
        0xa7, 0x00, 0x04,   // 10 goto -> 14（携值边直指条件跳转）
        0x03,               // 13 iconst_0（L0）
        0x99, 0x00, 0x07,   // 14 ifeq -> 21（L1，窗口内部锚）
        0x04,               // 17 iconst_1（L2）
        0xa7, 0x00, 0x04,   // 18 goto -> 22
        0x03,               // 21 iconst_0（L3）
        0xac,               // 22 ireturn（L4）
    });
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::ConsJumpOptimizer{}, mjbase::pass::ConstantFolder{},
                      mjbase::pass::FixPatternSimplifier{}, mjbase::pass::NopRemover{},
                      mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());   // 全集拒折/无匹配：方法零改动
    EXPECT_EQ(r.out, in);

    // Strict 帧数据流对（未被改动的）合法菱形 ok——守护不破不坏帧。
    auto              ins = inspect(r.out);
    jan::ShapeResolver resolver;
    jan::FrameEngine   fe(ins.session->backing().cp, resolver, jan::FramePolicy::Strict);
    jan::MethodSig     sig;
    sig.isStatic  = true;
    sig.name      = "m";
    sig.desc      = "([Ljava/lang/Object;)Z";
    sig.thisClass = "t/C";
    const auto sum = fe.compute(*ins.cu, sig);
    EXPECT_TRUE(sum.ok) << jan::to_string(sum.fail);
}
