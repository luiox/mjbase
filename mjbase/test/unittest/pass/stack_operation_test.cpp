// StackOperationSimplifier：窗口折叠规则面 + 不动点收敛 + 边界保守停 + 注册面。
//
// 窗口语义记账基准：窗口 [S1..Sn] → replacement R，产出 = 窗口前指令 + R +
// 窗口后指令（整窗替换，窗口内指令不保留）；消除规则的 R 为空。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/asm/matcher/step_util.hpp"
#include "mjbase/pass/stack_operation.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 写回产物里按序取操作码，断言快捷。
std::vector<Op> expectOps(const MethodPassRun& r)
{
    return opsOf(inspect(r.out));
}

}   // namespace

// ── 消除族 ──

TEST(StackOperationSimplifierTest, dupPopEliminated)
{
    FixtureBuilder b("t/S");
    // BIPUSH 7; DUP; POP; RETURN —— [dup,pop] 整窗删除。
    b.addMethod({"m", "()V", {0x10, 0x07, 0x59, 0x57, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    EXPECT_EQ(ops[1], Op::RETURN);
}

TEST(StackOperationSimplifierTest, swapSwapEliminated)
{
    FixtureBuilder b("t/S");
    // ACONST_NULL; ICONST_0; SWAP; SWAP; RETURN
    b.addMethod({"m", "()V", {0x01, 0x03, 0x5f, 0x5f, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::ICONST_0);
    EXPECT_EQ(ops[2], Op::RETURN);
}

TEST(StackOperationSimplifierTest, cat1LoadPopEliminated)
{
    FixtureBuilder b("t/S");
    // BIPUSH 7; POP; RETURN —— 装载进窗，随窗消除。
    b.addMethod({"m", "()V", {0x10, 0x07, 0x57, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(StackOperationSimplifierTest, cat2LoadPop2Eliminated)
{
    FixtureBuilder b("t/S");
    // LCONST_0; POP2; RETURN
    b.addMethod({"m", "()V", {0x09, 0x58, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 合并族 ──

TEST(StackOperationSimplifierTest, popPopMergedToPop2)
{
    FixtureBuilder b("t/S");
    // ACONST_NULL; ACONST_NULL; POP; POP; RETURN —— [pop,pop] => [pop2]。
    b.addMethod({"m", "()V", {0x01, 0x01, 0x57, 0x57, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::ACONST_NULL);
    EXPECT_EQ(ops[2], Op::POP2);
    EXPECT_EQ(ops[3], Op::RETURN);
}

TEST(StackOperationSimplifierTest, dupPop2FoldedToPop)
{
    FixtureBuilder b("t/S");
    // ACONST_NULL; DUP; POP2; RETURN —— [dup,pop2] => [pop]。
    b.addMethod({"m", "()V", {0x01, 0x59, 0x58, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

TEST(StackOperationSimplifierTest, cat1LoadPop2FoldedToPop)
{
    FixtureBuilder b("t/S");
    // BIPUSH 7; POP2; RETURN —— [loadInt,pop2] => [pop]，装载随窗消费。
    b.addMethod({"m", "()V", {0x10, 0x07, 0x58, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::POP);
    EXPECT_EQ(ops[1], Op::RETURN);
}

TEST(StackOperationSimplifierTest, dupX1SwapFoldedToDup)
{
    FixtureBuilder b("t/S");
    // ACONST_NULL; ALOAD_1; DUP_X1; SWAP; RETURN —— [dup_x1,swap] => [dup]。
    // 第二条指令刻意用 ALOAD_1（不在 loadInt/Float/Str 族内）：若用 ICONST_0，
    // 产出的 DUP 会在下一轮 fixpoint 被 loadInt;dup 规则吃掉级联成 2×ICONST，
    // 无法隔离验证本规则（级联收敛行为见 nestedShapesConvergeToFixpoint）。
    b.addMethod({"m", "()V", {0x01, 0x2b, 0x5a, 0x5f, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::ALOAD_1);
    EXPECT_EQ(ops[2], Op::DUP);
    EXPECT_EQ(ops[3], Op::RETURN);
}

// ── 展开族（常量装载复制）──

TEST(StackOperationSimplifierTest, constLoadDupDuplicated)
{
    FixtureBuilder b("t/S");
    // BIPUSH 7; DUP; RETURN —— [loadInt,dup] => [loadInt,loadInt]（含 RETURN 共 3 条）。
    b.addMethod({"m", "()V", {0x10, 0x07, 0x59, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                 ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 3u);
    ca::i32 v0 = 0, v1 = 0;
    ASSERT_TRUE(mjbase::asm_matcher::Steps::getIntValue(*ins.cu->dom.peek(ids[0]), v0));
    ASSERT_TRUE(mjbase::asm_matcher::Steps::getIntValue(*ins.cu->dom.peek(ids[1]), v1));
    EXPECT_EQ(v0, 7);
    EXPECT_EQ(v1, 7);
    EXPECT_EQ(ins.cu->dom.peek(ids[2])->op, Op::RETURN);
}

TEST(StackOperationSimplifierTest, cat2LoadDup2Duplicated)
{
    FixtureBuilder b("t/S");
    // LCONST_0; DUP2; RETURN —— [loadLong,dup2] => [loadLong,loadLong]。
    b.addMethod({"m", "()V", {0x09, 0x5c, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::LCONST_0);
    EXPECT_EQ(ops[1], Op::LCONST_0);
    EXPECT_EQ(ops[2], Op::RETURN);
}

TEST(StackOperationSimplifierTest, loadSwapPopReordered)
{
    FixtureBuilder b("t/S");
    // SIPUSH 0x0102; SWAP; POP; RETURN —— [loadInt,swap,pop] => [pop,loadInt]。
    b.addMethod({"m", "()V", {0x11, 0x01, 0x02, 0x5f, 0x57, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::POP);
    EXPECT_EQ(ops[1], Op::SIPUSH);
    EXPECT_EQ(ops[2], Op::RETURN);
}

TEST(StackOperationSimplifierTest, twoLoadSwapSwapped)
{
    FixtureBuilder b("t/S");
    // BIPUSH 1; BIPUSH 2; SWAP; RETURN —— [loadInt,loadInt,swap] => [装载2,装载1]。
    b.addMethod({"m", "()V", {0x10, 0x01, 0x10, 0x02, 0x5f, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                 ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 3u);
    ca::i32 v0 = 0, v1 = 0;
    ASSERT_TRUE(mjbase::asm_matcher::Steps::getIntValue(*ins.cu->dom.peek(ids[0]), v0));
    ASSERT_TRUE(mjbase::asm_matcher::Steps::getIntValue(*ins.cu->dom.peek(ids[1]), v1));
    EXPECT_EQ(v0, 2);
    EXPECT_EQ(v1, 1);
    EXPECT_EQ(ins.cu->dom.peek(ids[2])->op, Op::RETURN);
}

TEST(StackOperationSimplifierTest, dupX1Pop2KeepsSecond)
{
    FixtureBuilder b("t/S");
    // BIPUSH 1; BIPUSH 2; DUP_X1; POP2; RETURN —— 4 条窗 => [装载2]。
    b.addMethod({"m", "()V", {0x10, 0x01, 0x10, 0x02, 0x5a, 0x58, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                 ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 2u);
    ca::i32 v0 = 0;
    ASSERT_TRUE(mjbase::asm_matcher::Steps::getIntValue(*ins.cu->dom.peek(ids[0]), v0));
    EXPECT_EQ(v0, 2);
    EXPECT_EQ(ins.cu->dom.peek(ids[1])->op, Op::RETURN);
}

TEST(StackOperationSimplifierTest, dupX1PopReorders)
{
    FixtureBuilder b("t/S");
    // BIPUSH 1; BIPUSH 2; DUP_X1; POP; RETURN —— 4 条窗 => [装载2,装载1]。
    b.addMethod({"m", "()V", {0x10, 0x01, 0x10, 0x02, 0x5a, 0x57, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                 ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 3u);
    ca::i32 v0 = 0, v1 = 0;
    ASSERT_TRUE(mjbase::asm_matcher::Steps::getIntValue(*ins.cu->dom.peek(ids[0]), v0));
    ASSERT_TRUE(mjbase::asm_matcher::Steps::getIntValue(*ins.cu->dom.peek(ids[1]), v1));
    EXPECT_EQ(v0, 2);
    EXPECT_EQ(v1, 1);
    EXPECT_EQ(ins.cu->dom.peek(ids[2])->op, Op::RETURN);
}

TEST(StackOperationSimplifierTest, cat2Dup2X2ChainKeepsSecond)
{
    FixtureBuilder b("t/S");
    // LCONST_0; LCONST_1; DUP2_X2; POP2; POP2; RETURN —— 5 条窗 => [装载2]。
    b.addMethod({"m", "()V", {0x09, 0x0a, 0x5e, 0x58, 0x58, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LCONST_1);
    EXPECT_EQ(ops[1], Op::RETURN);
}

// ── 不动点收敛 ──

TEST(StackOperationSimplifierTest, nestedShapesConvergeToFixpoint)
{
    FixtureBuilder b("t/S");
    // BIPUSH 7; DUP; POP; POP; RETURN —— 第一轮 [dup,pop] 消除，第二轮
    // [bipush,pop] 消除：不动点迭代后只剩 RETURN。
    b.addMethod({"m", "()V", {0x10, 0x07, 0x59, 0x57, 0x57, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 保守面 ──

TEST(StackOperationSimplifierTest, noMatchMeansAllPreservedAndByteIdentical)
{
    FixtureBuilder b("t/S");
    // ACONST_NULL; POP; RETURN —— 无规则命中（loadInt 不含 aconst_null）。
    b.addMethod({"m", "()V", {0x01, 0x57, 0xb1}});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_FALSE(r.pa.changed());
    EXPECT_EQ(r.out, in);
}

TEST(StackOperationSimplifierTest, boundaryAnchoredWindowStopsConservatively)
{
    FixtureBuilder b("t/S");
    // BIPUSH; DUP; POP; RETURN，异常表 handler 锚定在 POP（pc 3）上：
    // 窗口 [dup,pop] 命中但 erase 被 DOM 拒绝 → Err → 保守终止收敛，
    // 声明语义 = 整方法未改动、字节保真。
    FixtureMethod m;
    m.name = "m";
    m.desc = "()V";
    m.code = {0x10, 0x07, 0x59, 0x57, 0xb1};
    m.exc  = {ExcSpec{3, 4, 3, 0}};
    b.addMethod(m);
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 注册面 ──

TEST(StackOperationSimplifierTest, registeredAsStackOperationSimplifier)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("StackOperationSimplifier"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("StackOperationSimplifier");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
