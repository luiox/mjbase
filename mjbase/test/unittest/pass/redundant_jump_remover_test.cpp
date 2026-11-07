// RedundantJumpRemover：目标落顺序后继前置边界的跳转消解（GOTO 删除 /
// 条件跳换 POP 序列保栈平衡）+ 远目标不动 + 方法末形态（条件跳替换、
// GOTO 保守跳过）+ PA 纪律。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService；装配器自带
// frame 分析），指令体片段 R"JASM 定界。
#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/redundant_jump_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法夹具（t/R.m，指令体为 jasm 文本片段）。
Bytes oneMethod(std::string_view desc, std::string_view codeBody)
{
    return assembleMethodClass("m", desc, codeBody, "t/R");
}

}   // namespace

// ── GOTO 落到紧邻后继前置边界 → 删除 ──
TEST(RedundantJumpRemoverTest, gotoToSuccessorDeleted)
{
    // ICONST_1(0) IRETURN(1) GOTO→L(2) L: ICONST_0 IRETURN。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I",
                                R"JASM(      iconst_1
      ireturn
      goto L
      L:
      iconst_0
      ireturn
)JASM"),
                      mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::IRETURN, Op::ICONST_0, Op::IRETURN}));
}

// ── 单值条件跳落到紧邻后继 → 原位替换为 POP ──
TEST(RedundantJumpRemoverTest, oneValueCondReplacedWithPop)
{
    // ICONST_1(0) IFEQ→L(1) L: ICONST_0 IRETURN。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I",
                                R"JASM(      iconst_1
      ifeq L
      L:
      iconst_0
      ireturn
)JASM"),
                      mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::POP, Op::ICONST_0, Op::IRETURN}));
}

// ── 双值条件跳落到紧邻后继 → POP; POP（两条件值都被丢弃）──
TEST(RedundantJumpRemoverTest, twoValueCondReplacedWithTwoPops)
{
    // ICONST_1(0) ICONST_2(1) IF_ICMPEQ→L(2) L: ICONST_0 IRETURN。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I",
                                R"JASM(      iconst_1
      iconst_2
      if_icmpeq L
      L:
      iconst_0
      ireturn
)JASM"),
                      mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::ICONST_2, Op::POP, Op::POP,
                                    Op::ICONST_0, Op::IRETURN}));
}

// ── 目标非紧邻后继 → 不动 ──
TEST(RedundantJumpRemoverTest, gotoToFarLabelKept)
{
    // GOTO→Ltop（回跳常量装载），顺序后继在别处——不匹配。
    const Bytes in = oneMethod(
        "()I",
        R"JASM(      Ltop:
      iconst_1
      ireturn
      goto Ltop
      iconst_0
      ireturn
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 空谓词（单值）→ POP ──
TEST(RedundantJumpRemoverTest, nullCondReplacedWithPop)
{
    // ACONST_NULL(0) IFNONNULL→L(1) L: RETURN(4) RETURN(5)（尾 return 不可
    // 达死代码，装配器帧分析按不可达建模收下）。
    const MethodPassRun r =
        runMethodPass(oneMethod("()V",
                                R"JASM(      aconst_null
      ifnonnull L
      L:
      return
      return
)JASM"),
                      mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ACONST_NULL, Op::POP, Op::RETURN, Op::RETURN}));
}

// ── 引用比较（双值）→ POP; POP ──
TEST(RedundantJumpRemoverTest, acmpCondReplacedWithTwoPops)
{
    // ACONST_NULL(0) ACONST_NULL(1) IF_ACMPNE→L(2) L: RETURN(5) RETURN(6)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()V",
                                R"JASM(      aconst_null
      aconst_null
      if_acmpne L
      L:
      return
      return
)JASM"),
                      mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ACONST_NULL, Op::ACONST_NULL, Op::POP, Op::POP,
                                    Op::RETURN, Op::RETURN}));
}

// ── 注册面 ──
TEST(RedundantJumpRemoverTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("RedundantJumpRemover"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("RedundantJumpRemover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
