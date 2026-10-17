// NopRemover：NOP 删除/分支目标重定向/边界锚定保留 + PA 纪律 + 注册面。
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/nop_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

// ── 顺序 NOP 与末条 NOP 全删 ──
TEST(NopRemoverTest, removesLeadingMiddleAndTrailingNops)
{
    FixtureBuilder b("t/N");
    b.addMethod({"m", "()V", {0x00, 0x04, 0x57, 0x00, 0xb1, 0x00}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::NopRemover{});

    EXPECT_TRUE(r.pa.changed());
    EXPECT_FALSE(r.pa.areAllPreserved());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── 无 NOP：PA=all，写回字节与输入逐字节一致（copy-through 保真）──
TEST(NopRemoverTest, noNopMeansAllPreservedAndByteIdentical)
{
    FixtureBuilder b("t/N");
    b.addMethod({"m", "()V", {0x04, 0x57, 0xb1}});
    const Bytes           in = b.assemble();
    const MethodPassRun   r  = runMethodPass(in, mjbase::pass::NopRemover{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_FALSE(r.pa.changed());
    EXPECT_EQ(r.out, in);
}

// ── 分支目标 NOP：删除后 GOTO 重定向到顺序后继（fallthrough 策略）──
TEST(NopRemoverTest, branchTargetNopRetargetsToFallthrough)
{
    // GOTO +3; NOP; RETURN —— GOTO 原目标 = NOP。
    FixtureBuilder b("t/N");
    b.addMethod({"m", "()V", {0xa8, 0x00, 0x03, 0x00, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::NopRemover{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                   ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 2u);   // GOTO; RETURN
    const jbc::ir::Insn* branch = ins.cu->dom.peek(ids[0]);
    const jbc::ir::Insn* ret    = ins.cu->dom.peek(ids[1]);
    ASSERT_NE(branch, nullptr);
    ASSERT_NE(ret, nullptr);
    // GOTO 的新目标锚定在 RETURN 上（fallthrough 重定向）。
    const auto target = ins.cu->dom.resolveLabel(branch->target);
    ASSERT_TRUE(target.is_ok());
    EXPECT_EQ(target.unwrap().kind, jbc::ir::LabelTargetKind::Instruction);
    EXPECT_EQ(target.unwrap().insn, ids[1]);
}

// ── 异常表边界锚定的 NOP：保留（声明的显式偏离，文档化在头文件）──
TEST(NopRemoverTest, handlerBoundaryNopIsKept)
{
    // NOP; RETURN；handler 指向 pc 0（NOP 上）。
    FixtureBuilder b("t/N");
    FixtureMethod  m;
    m.name = "m";
    m.desc = "()V";
    m.code = {0x00, 0xb1};
    m.exc  = {ExcSpec{0, 1, 0, 0}};
    b.addMethod(m);
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::NopRemover{});

    // 删除被拒 → 视为未改动（PA=all，字节保真）。
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::NOP);
}

// ── 注册面：种子名可查可构造 ──
TEST(NopRemoverTest, registeredAsNopRemover)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("NopRemover"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("NopRemover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
