// GotoRearranger：GOTO 目标尾段 moveRange 搬移 + 冗余跳转消除——正例
// （基础消除/不动点多候选链/GOTO_W/单指令尾段/GOTO 边界外部引用 retarget
// 承接）+ 保守门反例（多前驱/环形回边/异常表跨段/异常边界触 GOTO）+
// 邻接目标不重收（家族姊妹件领地）+ 后向目标不动 + 注册面 + PA 纪律。
//
// 全部夹具按栈面一致构造（GOTO 源点空栈或载荷可被目标段消费），经生产
// 管线 lower + Strict 帧重算全链验证；M 段（被跳过的中间段）均为不可达
// 死代码，帧引擎按 reachable=false 建模。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService；装配器自带
// frame 分析与验证），固定片段与逐用例指令体 / 异常表文本全部 R"JASM
// 定界，变量插入处保留 + 拼接。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/goto_rearranger.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法夹具：desc 定返回/参数面，codeBody 为指令体片段，excEntries 为
/// 异常表条目片段（可空 = 无异常表）。
Bytes oneMethodEx(std::string_view desc, std::string_view codeBody,
                  std::string_view excEntries = "")
{
    std::string source;
    if (excEntries.empty()) {
        source = std::string{R"JASM(.super java/lang/Object
.class public super t/G {
  .method public static m )JASM"} + std::string(desc) + R"JASM( {
    code: {
)JASM" + std::string(codeBody) + R"JASM(    }
  }
}
)JASM";
    }
    else {
        source = std::string{R"JASM(.super java/lang/Object
.class public super t/G {
  .method public static m )JASM"} + std::string(desc) + R"JASM( {
    exceptions: {
)JASM" + std::string(excEntries) + R"JASM(    },
    code: {
)JASM" + std::string(codeBody) + R"JASM(    }
  }
}
)JASM";
    }
    return assembleJasm(source);
}

}   // namespace

// ── 基础消除：GOTO 前向跳过 M 段到尾段 → 尾段提前、GOTO 删除 ──
TEST(GotoRearrangerTest, gotoTailSegmentMovedAndDeleted)
{
    // GOTO→L(0) | M: ICONST_0 IRETURN | L: ICONST_2 IRETURN。
    const MethodPassRun r =
        runMethodPass(oneMethodEx("()I",
                                  R"JASM(      goto L
      iconst_0
      ireturn
      L:
      iconst_2
      ireturn
)JASM"),
                      mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IRETURN, Op::ICONST_0, Op::IRETURN}));
}

// ── 不动点：链式两候选各处理一轮（首轮搬移把第二候选从邻接暴露为
//    非邻接形态，第二轮继续消除）──
TEST(GotoRearrangerTest, twoGotoFixpointChain)
{
    // G1→L1(0) | M1: ICONST_0 IRETURN | L1: ICONST_2 IRETURN
    // G2→L2(7) | ATHROW | L2: ICONST_3 IRETURN。
    const MethodPassRun r =
        runMethodPass(oneMethodEx("()I",
                                  R"JASM(      goto L1
      iconst_0
      ireturn
      L1:
      iconst_2
      ireturn
      goto L2
      athrow
      L2:
      iconst_3
      ireturn
)JASM"),
                      mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IRETURN, Op::ICONST_3, Op::IRETURN,
                                    Op::ICONST_0, Op::IRETURN, Op::ATHROW}));
}

// ── GOTO_W 与三指令中间段：尾段哨兵形态 ──
TEST(GotoRearrangerTest, gotoWTailSegmentMoved)
{
    // GOTO_W→L(0) | M: ICONST_0 NOP IRETURN | L: ICONST_2 IRETURN。
    const MethodPassRun r =
        runMethodPass(oneMethodEx("()I",
                                  R"JASM(      goto_w L
      iconst_0
      nop
      ireturn
      L:
      iconst_2
      ireturn
)JASM"),
                      mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IRETURN, Op::ICONST_0, Op::NOP,
                                    Op::IRETURN}));
}

// ── 非零候选位：前缀块 + GOTO_W ──
TEST(GotoRearrangerTest, gotoWAfterPrefixBlock)
{
    // ACONST_NULL(0) POP(1) GOTO_W→L(2) | M: ICONST_0 IRETURN
    // | L: ICONST_2 IRETURN。
    const MethodPassRun r =
        runMethodPass(oneMethodEx("()I",
                                  R"JASM(      aconst_null
      pop
      goto_w L
      iconst_0
      ireturn
      L:
      iconst_2
      ireturn
)JASM"),
                      mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ACONST_NULL, Op::POP, Op::ICONST_2, Op::IRETURN,
                                    Op::ICONST_0, Op::IRETURN}));
}

// ── 单指令尾段（段 = 最后一条指令）──
TEST(GotoRearrangerTest, singleInsnSegmentMoved)
{
    // ICONST_1(0) GOTO→L(1) | M: ICONST_0 IRETURN | L: IRETURN。
    const MethodPassRun r =
        runMethodPass(oneMethodEx("()I",
                                  R"JASM(      iconst_1
      goto L
      iconst_0
      ireturn
      L:
      ireturn
)JASM"),
                      mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::IRETURN, Op::ICONST_0, Op::IRETURN}));
}

// ── GOTO 自身边界被外部分支引用：erase 的 kRetargetToFallthrough 把该
//    引用承接到来来的顺序后继（RedundantJumpRemover 同款不阻断）──
TEST(GotoRearrangerTest, externalBranchIntoGotoBoundaryRetargeted)
{
    // ICONST_2(0) IFNE→G(1) | ICONST_1(4) IRETURN(5) | G: GOTO→L(6)
    // | M: ATHROW(9) | L: ICONST_3(10) IRETURN(11)。IFNE 的目标锚 GOTO
    // 前置边界，GOTO 删除后承接为搬入的 ICONST_3。
    const MethodPassRun r =
        runMethodPass(oneMethodEx("()I",
                                  R"JASM(      iconst_2
      ifne G
      iconst_1
      ireturn
      G:
      goto L
      athrow
      L:
      iconst_3
      ireturn
)JASM"),
                      mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IFNE, Op::ICONST_1, Op::IRETURN,
                                    Op::ICONST_3, Op::IRETURN, Op::ATHROW}));
}

// ── 保守门：目标多前驱（显式分支 + GOTO 同指）→ 拒动 ──
TEST(GotoRearrangerTest, multiplePredecessorsRejected)
{
    // GOTO→L(0) IFNE→L(3，不可达) | M: ICONST_0 IRETURN
    // | L: ICONST_2 IRETURN。L 前驱 = GOTO + IFNE 双入边。
    const Bytes in = oneMethodEx(
        "()I",
        R"JASM(      goto L
      ifne L
      iconst_0
      ireturn
      L:
      iconst_2
      ireturn
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 保守门：段内回边跳出段外（环形结构）→ 拒动 ──
TEST(GotoRearrangerTest, cycleBackEdgeRejected)
{
    // GOTO→L(0) | M: IRETURN(3)（回边落点，段外） | L: ICONST_2(4)
    // GOTO→M(5) IRETURN(8)。段内 GOTO 回跳 M 头（段外），跨段循环首件不证。
    // M 仅由 L 侧回边携 [int] 进入，IRETURN 弹之，帧面收敛（M 前是无条件
    // GOTO，无 fallthrough 空栈入口）；段尾 IRETURN 为回边后的不可达残留，
    // 装配器按不可达段放行。
    const Bytes in = oneMethodEx(
        "()I",
        R"JASM(      goto L
      M:
      ireturn
      L:
      iconst_2
      goto M
      ireturn
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 保守门：异常表保护区跨界（end 锚定段内 L 头）→ 拒动 ──
TEST(GotoRearrangerTest, excEndInsideSegmentRejected)
{
    // NOP(0) GOTO→L(1) | M: ICONST_0 IRETURN | L: ICONST_2 IRETURN
    // | H: POP ATHROW。try [M, L) 的 end 恰锚段头。M 段是死码（goto L
    // 越过，无跳入）：异常出口边发射域是「范围内可达指令」（JVM 校验器
    // §4.10.1.4 保守口径），死范围无边可发 → handler 不可达不分析，
    // POP+ATHROW 尾不构成入口下溢，装配器放行。
    const Bytes in = oneMethodEx(
        "()I",
        R"JASM(      nop
      goto L
      Ms:
      iconst_0
      ireturn
      L:
      iconst_2
      ireturn
      H:
      pop
      athrow
)JASM",
        R"JASM(      { Ms, L, H, * }
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 保守门：异常表边界锚定 GOTO 自身（erase retarget 必 StillUsed，
//    先验证后动手）→ 拒动 ──
TEST(GotoRearrangerTest, excBoundaryOnGotoRejected)
{
    // 同上布局，try [NOP, GOTO) 的 end 锚 GOTO 前置边界（end 标签 G 绑
    // GOTO 指令自身，end_pc = pc(GOTO)，区间覆盖 NOP 单指令）。H 体为
    // ATHROW 单指令（入口栈异常值直接重抛）：try 覆盖面含可达指令，
    // handler 入口被帧分析实达——原夹具 POP+ATHROW 尾对入口 [exc] 弹后
    // 空栈 athrow 下溢，装配器拒收；门判定只看 end 锚位，H 体内容不参与。
    const Bytes in = oneMethodEx(
        "()I",
        R"JASM(      S:
      nop
      G:
      goto L
      Ms:
      iconst_0
      ireturn
      L:
      iconst_2
      ireturn
      H:
      athrow
)JASM",
        R"JASM(      { S, G, H, * }
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 邻接目标不重收（RedundantJumpRemover 的既有窗口）──
TEST(GotoRearrangerTest, adjacentTargetNotClaimed)
{
    // ICONST_1(0) IRETURN(1) GOTO→L(2) L: ICONST_0 IRETURN：目标在
    // order 意义下紧邻（p == i + 1），留给家族姊妹件。
    const Bytes in = oneMethodEx(
        "()I",
        R"JASM(      iconst_1
      ireturn
      goto L
      L:
      iconst_0
      ireturn
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::GotoRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 后向目标（回边）不动 ──
TEST(GotoRearrangerTest, backwardTargetUntouched)
{
    // GOTO→Ltop 回跳常量装载，目标在候选之前。
    const Bytes in = oneMethodEx(
        "()I",
        R"JASM(      Ltop:
      iconst_1
      ireturn
      goto Ltop
      iconst_0
      ireturn
)JASM");
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
