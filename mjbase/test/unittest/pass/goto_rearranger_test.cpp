// GotoRearranger：GOTO 目标尾段 moveRange 搬移 + 冗余跳转消除——正例
// （基础消除/不动点多候选链/GOTO_W/单指令尾段/GOTO 边界外部引用 retarget
// 承接）+ 保守门反例（多前驱/环形回边/异常表跨段/异常边界触 GOTO）+
// 邻接目标不重收（家族姊妹件领地）+ 后向目标不动 + 注册面 + PA 纪律。
//
// 全部夹具按栈面一致构造（GOTO 源点空栈或载荷可被目标段消费），经生产
// 管线 lower + Strict 帧重算全链验证；M 段（被跳过的中间段）均为不可达
// 死代码，帧引擎按 reachable=false 建模。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/goto_rearranger.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器（透传异常表规格）。
Bytes oneMethodEx(std::string_view name, std::string_view desc, Bytes code,
                  std::vector<ExcSpec> exc = {})
{
    FixtureBuilder b("t/G");
    FixtureMethod m;
    m.name  = name;
    m.desc  = desc;
    m.code  = std::move(code);
    m.exc   = std::move(exc);
    b.addMethod(m);
    return b.assemble();
}

}   // namespace

// ── 基础消除：GOTO 前向跳过 M 段到尾段 → 尾段提前、GOTO 删除 ──
TEST(GotoRearrangerTest, gotoTailSegmentMovedAndDeleted)
{
    // GOTO→pc5(0) | M: ICONST_0(3) IRETURN(4) | L: ICONST_2(5) IRETURN(6)。
    const MethodPassRun r = runMethodPass(
        oneMethodEx("m", "()I", {0xa7, 0x00, 0x05, 0x03, 0xac, 0x05, 0xac}),
        mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IRETURN, Op::ICONST_0, Op::IRETURN}));
}

// ── 不动点：链式两候选各处理一轮（首轮搬移把第二候选从邻接暴露为
//    非邻接形态，第二轮继续消除）──
TEST(GotoRearrangerTest, twoGotoFixpointChain)
{
    // G1→pc5(0) | M1: ICONST_0(3) IRETURN(4) | L1: ICONST_2(5) IRETURN(6)
    // G2→pc11(7) | ATHROW(10) | L2: ICONST_3(11) IRETURN(12)。
    const MethodPassRun r =
        runMethodPass(oneMethodEx("m", "()I",
                                  {0xa7, 0x00, 0x05, 0x03, 0xac, 0x05, 0xac, 0xa7, 0x00,
                                   0x04, 0xbf, 0x06, 0xac}),
                      mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IRETURN, Op::ICONST_3, Op::IRETURN,
                                    Op::ICONST_0, Op::IRETURN, Op::ATHROW}));
}

// ── GOTO_W 与三指令中间段：尾段哨兵形态 ──
TEST(GotoRearrangerTest, gotoWTailSegmentMoved)
{
    // GOTO_W→pc8(0) | M: ICONST_0(5) NOP(6) IRETURN(7) | L: ICONST_2(8)
    // IRETURN(9)。
    const MethodPassRun r = runMethodPass(
        oneMethodEx("m", "()I",
                    {0xc8, 0x00, 0x00, 0x00, 0x08, 0x03, 0x00, 0xac, 0x05, 0xac}),
        mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IRETURN, Op::ICONST_0, Op::NOP,
                                    Op::IRETURN}));
}

// ── 非零候选位：前缀块 + GOTO_W ──
TEST(GotoRearrangerTest, gotoWAfterPrefixBlock)
{
    // ACONST_NULL(0) POP(1) GOTO_W→pc9(2) | M: ICONST_0(7) IRETURN(8)
    // | L: ICONST_2(9) IRETURN(10)。
    const MethodPassRun r = runMethodPass(
        oneMethodEx("m", "()I",
                    {0x01, 0x57, 0xc8, 0x00, 0x00, 0x00, 0x07, 0x03, 0xac, 0x05, 0xac}),
        mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ACONST_NULL, Op::POP, Op::ICONST_2, Op::IRETURN,
                                    Op::ICONST_0, Op::IRETURN}));
}

// ── 单指令尾段（段 = 最后一条指令）──
TEST(GotoRearrangerTest, singleInsnSegmentMoved)
{
    // ICONST_1(0) GOTO→pc6(1) | M: ICONST_0(4) IRETURN(5) | L: IRETURN(6)。
    const MethodPassRun r = runMethodPass(
        oneMethodEx("m", "()I", {0x04, 0xa7, 0x00, 0x05, 0x03, 0xac, 0xac}),
        mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::IRETURN, Op::ICONST_0, Op::IRETURN}));
}

// ── GOTO 自身边界被外部分支引用：erase 的 kRetargetToFallthrough 把该
//    引用承接到来来的顺序后继（RedundantJumpRemover 同款不阻断）──
TEST(GotoRearrangerTest, externalBranchIntoGotoBoundaryRetargeted)
{
    // ICONST_2(0) IFNE→pc6(1) | ICONST_1(3) IRETURN(4) | GOTO→pc9(5)
    // | M: ATHROW(8) | L: ICONST_3(9) IRETURN(10)。IFNE 的目标锚 GOTO
    // 前置边界，GOTO 删除后承接为搬入的 ICONST_3。
    const MethodPassRun r = runMethodPass(
        oneMethodEx("m", "()I",
                    {0x05, 0x9a, 0x00, 0x05, 0x04, 0xac, 0xa7, 0x00, 0x04, 0xbf, 0x06,
                     0xac}),
        mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IFNE, Op::ICONST_1, Op::IRETURN,
                                    Op::ICONST_3, Op::IRETURN, Op::ATHROW}));
}

// ── 保守门：目标多前驱（显式分支 + GOTO 同指）→ 拒动 ──
TEST(GotoRearrangerTest, multiplePredecessorsRejected)
{
    // GOTO→pc8(0) IFNE→pc8(3，不可达) | M: ICONST_0(6) IRETURN(7)
    // | L: ICONST_2(8) IRETURN(9)。L 前驱 = GOTO + IFNE 双入边。
    const Bytes in = oneMethodEx("m", "()I",
                                 {0xa7, 0x00, 0x08, 0x9a, 0x00, 0x05, 0x03, 0xac, 0x05,
                                  0xac});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 保守门：段内回边跳出段外（环形结构）→ 拒动 ──
TEST(GotoRearrangerTest, cycleBackEdgeRejected)
{
    // GOTO→pc4(0) | M: IRETURN(3) | L: ICONST_2(4) GOTO→pc3(5) IRETURN(8)。
    // 段内 GOTO 回跳 M 头（段外），跨段循环首件不证。
    const Bytes in       = oneMethodEx("m", "()I",
                                 {0xa7, 0x00, 0x04, 0xac, 0x05, 0xa7, 0xff, 0xfe, 0xac});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 保守门：异常表保护区跨界（end 锚定段内 L 头）→ 拒动 ──
TEST(GotoRearrangerTest, excEndInsideSegmentRejected)
{
    // NOP(0) GOTO→pc6(1) | M: ICONST_0(4) IRETURN(5) | L: ICONST_2(6)
    // IRETURN(7) | H: POP(8) ATHROW(9)。try [pc4, pc6) 的 end 恰锚段头。
    const Bytes in = oneMethodEx("m", "()I",
                                 {0x00, 0xa7, 0x00, 0x05, 0x03, 0xac, 0x05, 0xac, 0x57,
                                  0xbf},
                                 {{4, 6, 8, 0}});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 保守门：异常表边界锚定 GOTO 自身（erase retarget 必 StillUsed，
//    先验证后动手）→ 拒动 ──
TEST(GotoRearrangerTest, excBoundaryOnGotoRejected)
{
    // 同上布局，try [pc0, pc1) 的 end 锚 GOTO 前置边界。
    const Bytes in = oneMethodEx("m", "()I",
                                 {0x00, 0xa7, 0x00, 0x05, 0x03, 0xac, 0x05, 0xac, 0x57,
                                  0xbf},
                                 {{0, 1, 8, 0}});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 邻接目标不重收（RedundantJumpRemover 的既有窗口）──
TEST(GotoRearrangerTest, adjacentTargetNotClaimed)
{
    // ICONST_1(0) IRETURN(1) GOTO→pc5(2) ICONST_0(5) IRETURN(6)：目标在
    // order 意义下紧邻（p == i + 1），留给家族姊妹件。
    const Bytes in = oneMethodEx("m", "()I", {0x04, 0xac, 0xa7, 0x00, 0x03, 0x03, 0xac});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 后向目标（回边）不动 ──
TEST(GotoRearrangerTest, backwardTargetUntouched)
{
    // GOTO→pc0 回跳常量装载，目标在候选之前。
    const Bytes in = oneMethodEx("m", "()I", {0x04, 0xac, 0xa7, 0xff, 0xfe, 0x03, 0xac});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 注册面 ──
TEST(GotoRearrangerTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("GotoRearranger"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("GotoRearranger");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
