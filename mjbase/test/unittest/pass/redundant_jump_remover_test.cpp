// RedundantJumpRemover：目标落顺序后继前置边界的跳转消解（GOTO 删除 /
// 条件跳换 POP 序列保栈平衡）+ 远目标不动 + 方法末形态（条件跳替换、
// GOTO 保守跳过）+ PA 纪律。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/redundant_jump_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器。
Bytes oneMethod(std::string_view name, std::string_view desc, Bytes code)
{
    FixtureBuilder b("t/R");
    b.addMethod({name, desc, std::move(code)});
    return b.assemble();
}

}   // namespace

// ── GOTO 落到紧邻后继前置边界 → 删除 ──
TEST(RedundantJumpRemoverTest, gotoToSuccessorDeleted)
{
    // ICONST_1(0) IRETURN(1) GOTO→pc5(2) ICONST_0(5) IRETURN(6)。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", "()I", {0x04, 0xac, 0xa7, 0x00, 0x03, 0x03, 0xac}),
                      mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::IRETURN, Op::ICONST_0, Op::IRETURN}));
}

// ── 单值条件跳落到紧邻后继 → 原位替换为 POP ──
TEST(RedundantJumpRemoverTest, oneValueCondReplacedWithPop)
{
    // ICONST_1(0) IFEQ→pc4(1) ICONST_0(4) IRETURN(5)。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", "()I", {0x04, 0x99, 0x00, 0x03, 0x03, 0xac}),
                      mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::POP, Op::ICONST_0, Op::IRETURN}));
}

// ── 双值条件跳落到紧邻后继 → POP; POP（两条件值都被丢弃）──
TEST(RedundantJumpRemoverTest, twoValueCondReplacedWithTwoPops)
{
    // ICONST_1(0) ICONST_2(1) IF_ICMPEQ→pc5(2) ICONST_0(5) IRETURN(6)。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", "()I",
                                {0x04, 0x05, 0x9f, 0x00, 0x03, 0x03, 0xac}),
                      mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::ICONST_2, Op::POP, Op::POP,
                                    Op::ICONST_0, Op::IRETURN}));
}

// ── 目标非紧邻后继 → 不动 ──
TEST(RedundantJumpRemoverTest, gotoToFarLabelKept)
{
    // GOTO→pc0（回跳常量装载），顺序后继在 pc5——不匹配。
    const Bytes in = oneMethod("m", "()I", {0x04, 0xac, 0xa7, 0xff, 0xfe, 0x03, 0xac});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 空谓词（单值）→ POP ──
TEST(RedundantJumpRemoverTest, nullCondReplacedWithPop)
{
    // ACONST_NULL(0) IFNONNULL→pc4(1) RETURN(4) RETURN(5)。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", "()V", {0x01, 0xc7, 0x00, 0x03, 0xb1, 0xb1}),
                      mjbase::pass::RedundantJumpRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    EXPECT_EQ(ops, (std::vector<Op>{Op::ACONST_NULL, Op::POP, Op::RETURN, Op::RETURN}));
}

// ── 引用比较（双值）→ POP; POP ──
TEST(RedundantJumpRemoverTest, acmpCondReplacedWithTwoPops)
{
    // ACONST_NULL(0) ACONST_NULL(1) IF_ACMPNE→pc5(2) RETURN(5) RETURN(6)。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", "()V", {0x01, 0x01, 0xa6, 0x00, 0x03, 0xb1, 0xb1}),
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
