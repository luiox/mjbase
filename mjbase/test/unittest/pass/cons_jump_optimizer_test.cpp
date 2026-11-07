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
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;
namespace jan = jbc::analysis;

namespace {

/// 单方法容器（可指定描述子——折叠产物常含 ireturn）。jasm 文本装配，
/// 指令体片段 R"JASM 定界。
Bytes oneMethodI(std::string_view desc, std::string_view codeBody)
{
    return assembleMethodClass("m", desc, codeBody, "t/C");
}

}   // namespace

// ── 单槽恒真：[ICONST_0; IFEQ] → 头改 GOTO（复用原目标），条件跳删除 ──
TEST(ConsJumpOptimizerTest, oneSlotTakenRewritesHeadToGoto)
{
    // ICONST_0(0) IFEQ→pc6(1) ICONST_5(4) IRETURN(5) BIPUSH 7(6) IRETURN(8)。
    // 0==0 恒真 → GOTO；死落径 ICONST_5/IRETURN 保留（结构级清理另归）。
    const MethodPassRun r = runMethodPass(
        oneMethodI(
            "()I",
            R"JASM(      iconst_0
      ifeq L
      iconst_5
      ireturn
      L:
      bipush 7
      ireturn
)JASM"),
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
    const MethodPassRun r = runMethodPass(
        oneMethodI(
            "()I",
            R"JASM(      iconst_1
      ifeq L
      L:
      iconst_5
      ireturn
)JASM"),
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
    const MethodPassRun r = runMethodPass(
        oneMethodI(
            "()V",
            R"JASM(      aconst_null
      ifnull L
      return
      L:
      return
)JASM"),
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
    const MethodPassRun r = runMethodPass(
        oneMethodI(
            "()V",
            R"JASM(      aconst_null
      ifnonnull L
      return
      L:
      return
)JASM"),
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
    const MethodPassRun r = runMethodPass(
        oneMethodI(
            "()V",
            R"JASM(      iconst_2
      iconst_3
      if_icmplt L
      return
      L:
      return
)JASM"),
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
    const MethodPassRun r = runMethodPass(
        oneMethodI(
            "()V",
            R"JASM(      iconst_3
      iconst_2
      if_icmplt L
      return
      L:
      return
)JASM"),
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
    const Bytes in = oneMethodI(
        "()V",
        R"JASM(      iconst_0
      Lb:
      ifeq L
      L:
      return
      goto Lb
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 窗口头是分支目标：放行折叠（从头进入重放整窗，净栈效应为零）──
TEST(ConsJumpOptimizerTest, headLabelFoldsThrough)
{
    // ICONST_1(0) IFEQ→pc4(1) RETURN(4) GOTO→pc0(5)。1==0 恒假 → 两删，
    // 头标签经 fallthrough 链重定向到 RETURN。
    const MethodPassRun r = runMethodPass(
        oneMethodI(
            "()V",
            R"JASM(      Lh:
      iconst_1
      ifeq L
      L:
      return
      goto Lh
)JASM"),
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
    // Verifier 非法形态（ifeq 消费 null 引用），钉防御面：不做跨类型语义
    // 假设。装配器对该形态放行（relaxed 面收），直接装配。
    const Bytes in = oneMethodI(
        "()V",
        R"JASM(      aconst_null
      ifeq L
      return
      L:
      return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 类型失配拒绝：int 常量配空谓词（IFNULL）不入矩阵 ──
TEST(ConsJumpOptimizerTest, intProducerWithNullPredicateNotFolded)
{
    const Bytes in = oneMethodI(
        "()V",
        R"JASM(      iconst_0
      ifnull L
      return
      L:
      return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ConsJumpOptimizer{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 引用比较谓词（IF_ACMPEQ）不入矩阵：双 int 常量配 ACMP 不折叠 ──
TEST(ConsJumpOptimizerTest, acmpPredicateNotInMatrix)
{
    // 旧栈对这类 Verifier 非法形态（双 int 配引用比较谓词）按谓词表假值
    // 整窗删除（防御怪癖），本实现显式拒绝。装配器放行，直接装配。
    const Bytes in = oneMethodI(
        "()V",
        R"JASM(      iconst_0
      iconst_1
      if_acmpeq L
      return
      L:
      return
)JASM");
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
    const Bytes in = oneMethodI(
        "([Ljava/lang/Object;)Z",
        R"JASM(      aload_0
      ifnull L2
      aload_0
      arraylength
      ifne L0
      iconst_1
      goto L1
      L0:
      iconst_0
      L1:
      ifeq L3
      L2:
      iconst_1
      goto L4
      L3:
      iconst_0
      L4:
      ireturn
)JASM");
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
