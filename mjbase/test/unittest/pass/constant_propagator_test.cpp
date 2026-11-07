// ConstantPropagator：def-use 到达定义面——跨块传播（GOTO 不阻断）、双路
// 汇合多定义保留、store 前装载不误折（旧栈正确性修正）、非相邻常量生产者
// （栈中间指令隔断）、iinc 定义跳过、引用槽 null 面、槽族严格匹配、PA 纪律。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService；装配器自带
// frame 分析），指令体片段 R"JASM 定界。槽族失配（ref 槽 int 装载）装配器
// 收下、relaxed validate 拒收——例外①申报：装配直接出形 + validate 拒收
// 自检在案。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/constant_propagator.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

Bytes oneMethod(std::string_view desc, std::string_view codeBody)
{
    return assembleMethodClass("m", desc, codeBody);
}

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

// ── 跨块传播：store 与 load 隔 GOTO → 照折（窗口面在此不折）──
TEST(ConstantPropagatorTest, crossesGotoBoundary)
{
    // BIPUSH 5(0) ISTORE_0(2) GOTO→Ld(3) Ld: ILOAD_0 IRETURN。
    // store 经 GOTO 仍到达 load → 折。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I",
                                R"JASM(      bipush 5
      istore_0
      goto Ld
      Ld:
      iload_0
      ireturn
)JASM"),
                      mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[3], Op::ICONST_5);
    // 幂等。
    const MethodPassRun r2 =
        runMethodPass(r.out, mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── 双路汇合：两个 store 都到达 load → 多定义不折 ──
TEST(ConstantPropagatorTest, mergeOfTwoStoresKept)
{
    // ICONST_1(0) ISTORE_0(1) ICONST_1(2) IFEQ→Lm(3..5) ICONST_3(6)
    // ISTORE_0(7) Lm: ILOAD_0(8) IRETURN(9)。真路径只经 store@1，假路径
    // 经两个 store → load 到达集合 = 2 定义。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I",
                                R"JASM(      iconst_1
      istore_0
      iconst_1
      ifeq Lm
      iconst_3
      istore_0
      Lm:
      iload_0
      ireturn
)JASM"),
                      mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── store 前装载不误折（旧栈 ConsLocalVarOptimizer 的正确性修正）──
TEST(ConstantPropagatorTest, loadBeforeStoreKept)
{
    // (I)V：ILOAD_0(0) POP(1) ICONST_5(2) ISTORE_0(3) ILOAD_0(4) IRETURN(5)。
    // 首个 ILOAD_0 到达定义 = 入参 → 保留；尾 ILOAD_0 → ICONST_5。
    const MethodPassRun r =
        runMethodPass(oneMethod("(I)V",
                                R"JASM(      iload_0
      pop
      iconst_5
      istore_0
      iload_0
      return
)JASM"),
                      mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[0], Op::ILOAD_0);   // store 前装载（入参）保留
    EXPECT_EQ(ops[4], Op::ICONST_5);
}

// ── 非相邻生产者：store 弹值定义隔一条 POP（栈中间指令）→ 照折 ──
TEST(ConstantPropagatorTest, nonAdjacentProducerFolds)
{
    // (II)V（静态，参数槽 0/1）：SIPUSH 5(0) ILOAD_1(3) POP(4) ISTORE_0(5)
    // ILOAD_0(6) IRETURN(7)。store 弹值 = SIPUSH 定义（iload 被 POP
    // 消费）——非紧邻生产者靠栈使用边定位。
    const MethodPassRun r =
        runMethodPass(oneMethod("(II)V",
                                R"JASM(      sipush 5
      iload_1
      pop
      istore_0
      iload_0
      ireturn
)JASM"),
                      mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[4], Op::ICONST_5);
}

// ── iinc 定义非常量生产者 → 不折 ──
TEST(ConstantPropagatorTest, iincDefSkipped)
{
    // (I)V：ILOAD_2(0) ISTORE_0(1) IINC 0 1(2) ILOAD_0(5) IRETURN(6)。
    const MethodPassRun r =
        runMethodPass(oneMethod("(I)V",
                                R"JASM(      iload_2
      istore_0
      iinc 0 1
      iload_0
      ireturn
)JASM"),
                      mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 引用槽：ACONST_NULL 经 GOTO 跨块到达 ALOAD_0 → ACONST_NULL ──
TEST(ConstantPropagatorTest, refNullAcrossBlocks)
{
    // ACONST_NULL(0) ASTORE_0(1) GOTO→Ld(2) Ld: ALOAD_0(5) POP(6) RETURN(7)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()V",
                                R"JASM(      aconst_null
      astore_0
      goto Ld
      Ld:
      aload_0
      pop
      return
)JASM"),
                      mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[3], Op::ACONST_NULL);
}

// ── 槽族严格匹配：int store 配 DLOAD 不折 ──
TEST(ConstantPropagatorTest, kindMismatchKept)
{
    // ACONST_NULL(0) ASTORE_0(1) GOTO→Ld(2) Ld: ILOAD_0(5) IRETURN(6)。
    // Ref store 配 int 装载——槽族严格匹配拒收。装配器收下（frame 分析按
    // store 定槽型不验读宽）、relaxed validate 拒收——纪律例外①申报，
    // 自检：validate 拒收在案。
    const Bytes in = oneMethod("()I",
                               R"JASM(      aconst_null
      astore_0
      goto Ld
      Ld:
      iload_0
      ireturn
)JASM");
    expectValidateFail(in);
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── cat2 跨块：DCONST_1;DSTORE_0 隔 GOTO → DLOAD_0 → DCONST_1 ──
TEST(ConstantPropagatorTest, doubleAcrossBlocks)
{
    // DCONST_1(0) DSTORE_0(1) GOTO→Ld(2..4) Ld: DLOAD_0(5) DRETURN(6)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()D",
                                R"JASM(      dconst_1
      dstore_0
      goto Ld
      Ld:
      dload_0
      dreturn
)JASM"),
                      mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[3], Op::DCONST_1);
}

// ── 值域物化：300 跨块折为 SIPUSH（LDC 档面在 LCP
// 回绕用例覆盖） ──
TEST(ConstantPropagatorTest, sipushBandAcrossBlocks)
{
    // SIPUSH(0..2) ISTORE_0(3) GOTO(4..6)→Ld Ld: ILOAD_0(7) IRETURN(8)。
    const MethodPassRun r =
        runMethodPass(oneMethod("()I",
                                R"JASM(      sipush 300
      istore_0
      goto Ld
      Ld:
      iload_0
      ireturn
)JASM"),
                      mjbase::pass::ConstantPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 5u);
    EXPECT_EQ(all[3]->op, Op::SIPUSH);
    ca::i32 v = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::intValueOf(*ins.session, *all[3], v));
    EXPECT_EQ(v, 300);
}

// ── 注册面：名字可查、scope=Method ──
TEST(ConstantPropagatorTest, registeredByName)
{
    const mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    const auto                           e   = reg.find("ConstantPropagator");
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.unwrap().scope, mjbase::spi::PassScope::Method);
}
