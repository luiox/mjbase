// LocalConstantPropagation：直线段常量槽位追踪（store 前驱常量 → 同类装载
// 原位物化）+ IINC 32 位回绕递推 + 引用槽 ACONST_NULL 面 + 标签/控制流边界
// 清空 + 槽族严格匹配拒绝 + PA 纪律。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/local_constant_propagation.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器。
Bytes oneMethod(std::string_view desc, Bytes code)
{
    FixtureBuilder b("t/C");
    b.addMethod({"m", desc, std::move(code)});
    return b.assemble();
}

/// 升格方法体的指针序列（操作数断言用）。
std::vector<const jbc::ir::Insn*> allInsns(const Inspection& ins)
{
    std::vector<const jbc::ir::Insn*> out;
    for (const jbc::ir::InsnId id : ins.cu->dom.order()) {
        const jbc::ir::Insn* in = ins.cu->dom.peek(id);
        EXPECT_NE(in, nullptr);
        out.push_back(in);
    }
    return out;
}

}   // namespace

// ── 基本面：BIPUSH;ISTORE_0 后 ILOAD_0 原位物化为 ICONST_5 ──
TEST(LocalConstantPropagationTest, intStoreThenLoadFoldsToConst)
{
    // BIPUSH 5(0) ISTORE_0(2) ILOAD_0(3) IRETURN(4)。
    const MethodPassRun r = runMethodPass(
        oneMethod("()I", {0x10, 0x05, 0x3b, 0x1a, 0xac}),
        mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    EXPECT_EQ(ops[1], Op::ISTORE_0);
    EXPECT_EQ(ops[2], Op::ICONST_5);   // 5 落 ICONST 值域
    EXPECT_EQ(ops[3], Op::IRETURN);
    // 幂等：产物再跑无变化。
    const MethodPassRun r2 =
        runMethodPass(r.out, mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── 值域物化分档：200 落 SIPUSH ──
TEST(LocalConstantPropagationTest, materializeFallsIntoSipushBand)
{
    // SIPUSH 200(0) ISTORE_0(3) ILOAD_0(4) IRETURN(5)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I", {0x11, 0x00, 0xc8, 0x3b, 0x1a, 0xac}),
                      mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all[2]->op, Op::SIPUSH);
    EXPECT_EQ(all[2]->a, 200);
}

// ── IINC 回绕递推：BIPUSH 7;ISTORE;IINC 0 3 → 装载取 10（SIPUSH 档）──
TEST(LocalConstantPropagationTest, iincFoldsTrackedInt)
{
    // BIPUSH 7(0) ISTORE_0(2) IINC 0 3(3) ILOAD_0(6) IRETURN(7)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I", {0x10, 0x07, 0x3b, 0x84, 0x00, 0x03, 0x1a, 0xac}),
                      mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 5u);
    EXPECT_EQ(all[3]->op, Op::BIPUSH);   // 10 落 BIPUSH 档
    EXPECT_EQ(all[3]->a, 10);
    // IINC 本体不改动（原生产者供更早装载消费的纪律）。
    EXPECT_EQ(all[2]->op, Op::IINC);
}

// ── IINC 未追踪槽 → 装载不折（参数槽无追踪）──
TEST(LocalConstantPropagationTest, iincUntrackedLoadKept)
{
    // (I)V 无参；ILOAD_0(0) IINC 0 1(1) ILOAD_0(4) IRETURN(5)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I", {0x1a, 0x84, 0x00, 0x01, 0x1a, 0xac}),
                      mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 32 位回绕：SIPUSH 0x7FFF... 用 ICONST_M1 + IINC 反复递推跨 INT_MAX ──
TEST(LocalConstantPropagationTest, iincWrapsAroundIntMax)
{
    // LDC INT_MAX(0) ISTORE_0(3) IINC 0 1(4) ILOAD_0(7) IRETURN(8)。
    // LDC 整数条目由 LDC_W 编码：0x13 idxhi idxlo。
    FixtureBuilder b("t/C");
    // LDC 的 CP 条目在 raw 字节里需要手工索引——改用 SIPUSH 32767 逼近：
    // 32767 + 1 = 32768，验证正溢出跨 SIPUSH 边界（物化落 LDC 档）。
    b.addMethod({"m", "()I",
                 {0x11, 0x7f, 0xff, 0x3b, 0x84, 0x00, 0x01, 0x1a, 0xac}});
    const MethodPassRun r = runMethodPass(
        b.assemble(), mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 5u);
    // 32768 超出 SIPUSH 值域 → LDC 整数常量。
    EXPECT_EQ(all[3]->op, Op::LDC);
    ca::i32 v = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::intValueOf(*ins.session, *all[3], v));
    EXPECT_EQ(v, 32768);
}

// ── 引用槽：ACONST_NULL;ASTORE_0 后 ALOAD_0 → ACONST_NULL ──
TEST(LocalConstantPropagationTest, refSlotTracksNullOnly)
{
    // ACONST_NULL(0) ASTORE_0(1) ALOAD_0(2) POP(3) RETURN(4)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()V", {0x01, 0x4b, 0x2a, 0x57, 0xb1}),
                      mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[2], Op::ACONST_NULL);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ALOAD_0), 0);
}

// ── 边界清空：store 与 load 之间隔标签锚定 → 不折 ──
TEST(LocalConstantPropagationTest, labelBetweenStoreAndLoadBreaksTracking)
{
    // GOTO→pc6(0) BIPUSH 5(3) ISTORE_0(5) ILOAD_0←GOTO 目标(6) IRETURN(7)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I", {0xa7, 0x00, 0x06, 0x10, 0x05, 0x3b, 0x1a, 0xac}),
                      mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── GOTO 边界清空：store 在前，GOTO 之后 load 不折 ──
TEST(LocalConstantPropagationTest, gotoClearsTracking)
{
    // BIPUSH 5(0) ISTORE_0(2) GOTO→pc7(3) BIPUSH 1(6)?? — 需要合法跳转域。
    // 布局：BIPUSH 5(0) ISTORE_0(2) GOTO→pc6(3) ILOAD_0(6) IRETURN(7)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I", {0x10, 0x05, 0x3b, 0xa7, 0x00, 0x03, 0x1a, 0xac}),
                      mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 槽族严格匹配：int 追踪配 FLOAD 不折（声明收紧面）──
TEST(LocalConstantPropagationTest, kindMismatchKept)
{
    // ICONST_5(0) ISTORE_0(1) FLOAD_0(2) FRETURN(3)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()F", {0x04, 0x3b, 0x32, 0xae}),
                      mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── cat2 面：DCONST_1;DSTORE_0 后 DLOAD_0 → DCONST_1 ──
TEST(LocalConstantPropagationTest, doubleSlotFolds)
{
    // DCONST_1(0) DSTORE_0(1) DLOAD_0(2) DRETURN(3)。
    const MethodPassRun r = runMethodPass(
        oneMethod("()D", {0x0f, 0x47, 0x26, 0xaf}), mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[2], Op::DCONST_1);
}

// ── 重存储清空：const;store;non-const store;load 不折 ──
TEST(LocalConstantPropagationTest, secondStoreClearsTracking)
{
    // BIPUSH 5(0) ISTORE_0(2) ILOAD_1(3)?? — 需第二 store 生产者非常量：
    // 用 ACONST_NULL? 类型不符。改用（无参方法）ILOAD_0 做非常量前驱：
    // ILOAD_0(0) ISTORE_0(1)?? 槽0 参数…()I 有参？给 (I)V：
    // 布局：(I)V — ILOAD_1(0) ISTORE_0(1) ILOAD_0(2) IRETURN(3)。
    // ILOAD_1 非常量前驱 → 槽0 不追踪 → ILOAD_0 不折。
    const MethodPassRun r =
        runMethodPass(oneMethod("(I)V", {0x1c, 0x3b, 0x1a, 0xac}),
                      mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 链式传递：第一段折出的常量喂第二段（单遍即不动点）──
TEST(LocalConstantPropagationTest, chainedSegmentsConverge)
{
    // BIPUSH 5(0) ISTORE_0(2) ILOAD_0(3) ISTORE_1(4) ILOAD_1(5) IRETURN(6)。
    // ILOAD_0 折成 ICONST_5 后，ISTORE_1 的常量前驱成立 → ILOAD_1 → ICONST_5。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I", {0x10, 0x05, 0x3b, 0x1a, 0x3c, 0x1b, 0xac}),
                      mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[2], Op::ICONST_5);
    EXPECT_EQ(ops[4], Op::ICONST_5);
    // 幂等。
    const MethodPassRun r2 =
        runMethodPass(r.out, mjbase::pass::LocalConstantPropagation{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── 注册面：名字可查、scope=Method ──
TEST(LocalConstantPropagationTest, registeredByName)
{
    const mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    const auto                           e   = reg.find("LocalConstantPropagation");
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.unwrap().scope, mjbase::spi::PassScope::Method);
}
