// IntrinsicFolding：装箱/拆箱对整删、种子 Random 五窗 LCG 折叠、白名单
// 纯函数常量折叠（Math/String/Integer/Long）、toCharArray+valueOf 恒等对、
// 窗内边界锚标签拒收、不动点链折、非白名单形态保留。
//
// 夹具纪律：方法体一律 jasm 文本装配（JvmJasmService），原 FixtureBuilder
// 手拼载体退役；窄整数（bipush/sipush）装配器规范化为 ldc——折叠残留的
// 常量生产者期望值随之改记 LDC；long 形态走 `ldc <n>L`（装配器发
// LDC2_W，折 Long.parseLong 期望同面）。三处「窗内边界锚标签」守卫载体
// 以异常区间边界锚合法重表达——守卫面 labelsAnchorAt 对分支目标与异常
// 边界同源在 DOM 留锚，锚位（intValue 前置 / 种子装载 / invoke 自身）同
// 位触发；handler 收口为裸 athrow（吃掉异常栈，帧自洽），原字节级 goto
// 中腹进窗的栈形断裂形不再出现。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "jstd/random.hpp"

#include "mjbase/pass/intrinsic_folding.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 方法级夹具便捷形：t/C 单 public static m(desc)（excText 为 exceptions
/// 段条目文本，可空）。
Bytes asmM(std::string_view desc, std::string_view codeBody,
           std::string_view excText = "")
{
    const std::string excSeg = excText.empty()
        ? std::string()
        : "    exceptions: {\n" + std::string(excText) + "    },\n";
    const std::string src = R"JASM(.super java/lang/Object
.class public super t/C {
  .method public static m )JASM" + std::string(desc) + R"JASM( {
)JASM" + excSeg + "    code: {\n" + std::string(codeBody) + "    }\n  }\n}\n";
    return assembleJasm(src);
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

// ── 装箱/拆箱对：Integer.valueOf(I) + intValue() 整对删除 ──
TEST(IntrinsicFoldingTest, boxUnboxIntPairFolds)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      ldc 7\n"
             "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
             "      invokevirtual java/lang/Integer.intValue ()I\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LDC);   // 窄整数生产者经装配器规范化为 ldc
    EXPECT_EQ(ops[1], Op::IRETURN);
}

// ── cat2 装箱对：Long.valueOf(J) + longValue() ──
TEST(IntrinsicFoldingTest, boxUnboxLongPairFolds)
{
    const MethodPassRun r = runMethodPass(
        asmM("()J",
             "      ldc 9L\n"
             "      invokestatic java/lang/Long.valueOf (J)Ljava/lang/Long;\n"
             "      invokevirtual java/lang/Long.longValue ()J\n"
             "      lreturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LDC2_W);   // ldc <n>L 装配器发 LDC2_W
    EXPECT_EQ(ops[1], Op::LRETURN);
}

// ── 族失配：valueOf 与他类拆箱混搭不折 ──
TEST(IntrinsicFoldingTest, boxUnboxMismatchKept)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      ldc 7\n"
             "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
             "      invokevirtual java/lang/Double.doubleValue ()D\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 4u);
}

// ── 拆箱前置边界锚标签（异常区间起点锚 intValue）→ 整对拒收 ──
// 合法重表达：原字节形 goto 中腹进窗（栈形断裂不可装配）；守卫面
// labelsAnchorAt 对异常边界与分支目标同源留锚，起点锚 intValue 同位触发。
TEST(IntrinsicFoldingTest, boxUnboxInteriorLabelKept)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      ldc 7\n"
             "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
             "      Lb:\n"
             "      invokevirtual java/lang/Integer.intValue ()I\n"
             "      ireturn\n"
             "      Lr:\n"
             "      athrow\n",
             "      { Lb, Lr, Lr, * }\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 5u);
}

// ── 种子 Random：new Random(0).nextInt() 折叠为 JDK 已知位模式 ──
TEST(IntrinsicFoldingTest, randomSeedNextIntFolds)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      new java/util/Random\n"
             "      dup\n"
             "      ldc 0L\n"
             "      invokespecial java/util/Random.<init> (J)V\n"
             "      invokevirtual java/util/Random.nextInt ()I\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection                        ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 2u);
    ca::i32 v = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::intValueOf(*ins.session, *all[0], v));
    // java.util.Random(0).nextInt() 的 JDK 权威值，同时钉 jstd 对齐面。
    EXPECT_EQ(v, -1155484576);
    EXPECT_EQ(v, jstd::Random(0).nextInt());
    EXPECT_EQ(all[1]->op, Op::IRETURN);
}

// ── nextBoolean：消耗 1 位 ──
TEST(IntrinsicFoldingTest, randomSeedNextBooleanFolds)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      new java/util/Random\n"
             "      dup\n"
             "      ldc 7L\n"
             "      invokespecial java/util/Random.<init> (J)V\n"
             "      invokevirtual java/util/Random.nextBoolean ()Z\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection                        ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 2u);
    ca::i32 v = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::intValueOf(*ins.session, *all[0], v));
    EXPECT_EQ(v, jstd::Random(7).next(1));
}

// ── 种子装载前置边界锚标签 → 五窗拒收 ──
// 合法重表达：异常起点锚种子装载位（五窗 interior，守卫扫 DUP..消费同位
// 触发），handler 裸 athrow。
TEST(IntrinsicFoldingTest, randomSeedInteriorLabelKept)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      new java/util/Random\n"
             "      dup\n"
             "      Lb:\n"
             "      ldc 0L\n"
             "      invokespecial java/util/Random.<init> (J)V\n"
             "      invokevirtual java/util/Random.nextInt ()I\n"
             "      ireturn\n"
             "      Lr:\n"
             "      athrow\n",
             "      { Lb, Lr, Lr, * }\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 7u);
}

// ── Math.abs(-42) → BIPUSH 42 ──
TEST(IntrinsicFoldingTest, mathAbsFolds)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      ldc -42\n"
             "      invokestatic java/lang/Math.abs (I)I\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

// ── Math.max(3, 9) → BIPUSH 9；幂等 ──
TEST(IntrinsicFoldingTest, mathMaxFolds)
{
    const Bytes in = asmM("()I",
                          "      ldc 3\n"
                          "      ldc 9\n"
                          "      invokestatic java/lang/Math.max (II)I\n"
                          "      ireturn\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    const MethodPassRun r2 = runMethodPass(r.out, mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── Math.abs(Integer.MIN_VALUE) 溢出面：仍折叠、物化为同值 ──
TEST(IntrinsicFoldingTest, mathAbsIntMinFoldsToSameValue)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      ldc -2147483648\n"
             "      invokestatic java/lang/Math.abs (I)I\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection                        ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 2u);
    ca::i32 v = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::intValueOf(*ins.session, *all[0], v));
    EXPECT_EQ(v, -2147483647 - 1);
}

// ── Math.min(2.0f, 1.0f) → FCONST_1 ──
TEST(IntrinsicFoldingTest, mathMinFloatFolds)
{
    const MethodPassRun r = runMethodPass(
        asmM("()F",
             "      fconst_2\n"
             "      fconst_1\n"
             "      invokestatic java/lang/Math.min (FF)F\n"
             "      freturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::FCONST_1);
    EXPECT_EQ(ops[1], Op::FRETURN);
}

// ── String.length：常量接收者 → BIPUSH 5 ──
TEST(IntrinsicFoldingTest, stringLengthFolds)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      ldc \"hello\"\n"
             "      invokevirtual java/lang/String.length ()I\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_5);   // 5 落 iconst 档
}

// ── String.substring(1,3) → LDC "el" ──
TEST(IntrinsicFoldingTest, stringSubstringFolds)
{
    const MethodPassRun r = runMethodPass(
        asmM("()Ljava/lang/String;",
             "      ldc \"hello\"\n"
             "      ldc 1\n"
             "      ldc 3\n"
             "      invokevirtual java/lang/String.substring (II)Ljava/lang/String;\n"
             "      areturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection                        ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 2u);
    std::string_view v;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::stringValueOf(*ins.session, *all[0], v));
    EXPECT_EQ(v, "el");
    EXPECT_EQ(all[1]->op, Op::ARETURN);
}

// ── substring 界非法（end < start）→ 保留 ──
TEST(IntrinsicFoldingTest, stringSubstringInvalidKept)
{
    const MethodPassRun r = runMethodPass(
        asmM("()Ljava/lang/String;",
             "      ldc \"hello\"\n"
             "      ldc 3\n"
             "      ldc 1\n"
             "      invokevirtual java/lang/String.substring (II)Ljava/lang/String;\n"
             "      areturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 5u);
}

// ── Integer.parseInt("42") → BIPUSH 42；Long.parseLong("123") → LDC2_W ──
TEST(IntrinsicFoldingTest, parseIntAndParseLongFold)
{
    {
        const MethodPassRun r = runMethodPass(
            asmM("()I",
                 "      ldc \"42\"\n"
                 "      invokestatic java/lang/Integer.parseInt (Ljava/lang/String;)I\n"
                 "      ireturn\n"),
            mjbase::pass::IntrinsicFolding{});
        EXPECT_TRUE(r.pa.changed());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        ASSERT_EQ(ops.size(), 2u);
        EXPECT_EQ(ops[0], Op::BIPUSH);
    }
    {
        const MethodPassRun r = runMethodPass(
            asmM("()J",
                 "      ldc \"123\"\n"
                 "      invokestatic java/lang/Long.parseLong (Ljava/lang/String;)J\n"
                 "      lreturn\n"),
            mjbase::pass::IntrinsicFolding{});
        EXPECT_TRUE(r.pa.changed());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        ASSERT_EQ(ops.size(), 2u);
        EXPECT_EQ(ops[0], Op::LDC2_W);
        EXPECT_EQ(ops[1], Op::LRETURN);
    }
}

// ── parseInt 尾随杂质拒收 → 保留 ──
TEST(IntrinsicFoldingTest, parseInvalidKept)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      ldc \"4x2\"\n"
             "      invokestatic java/lang/Integer.parseInt (Ljava/lang/String;)I\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 3u);
}

// ── Integer.bitCount / toHexString ──
TEST(IntrinsicFoldingTest, bitOpsFolds)
{
    {
        const MethodPassRun r = runMethodPass(
            asmM("()I",
                 "      ldc 255\n"
                 "      invokestatic java/lang/Integer.bitCount (I)I\n"
                 "      ireturn\n"),
            mjbase::pass::IntrinsicFolding{});
        EXPECT_TRUE(r.pa.changed());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        ASSERT_EQ(ops.size(), 2u);
        EXPECT_EQ(ops[0], Op::BIPUSH);   // 8 超 iconst 档，落 bipush
        const MethodPassRun r2 = runMethodPass(r.out, mjbase::pass::IntrinsicFolding{});
        EXPECT_TRUE(r2.pa.areAllPreserved());
    }
    {
        const MethodPassRun r = runMethodPass(
            asmM("()Ljava/lang/String;",
                 "      ldc 255\n"
                 "      invokestatic java/lang/Integer.toHexString (I)Ljava/lang/String;\n"
                 "      areturn\n"),
            mjbase::pass::IntrinsicFolding{});
        EXPECT_TRUE(r.pa.changed());
        const Inspection                        ins = inspect(r.out);
        const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
        ASSERT_EQ(all.size(), 2u);
        std::string_view v;
        ASSERT_TRUE(
            mjbase::pass::method_rewrite_util::stringValueOf(*ins.session, *all[0], v));
        EXPECT_EQ(v, "ff");
    }
}

// ── toCharArray + valueOf([C) 恒等对：整对删除、接收者生产者保留 ──
TEST(IntrinsicFoldingTest, toCharArrayValueOfPairFolds)
{
    const MethodPassRun r = runMethodPass(
        asmM("()Ljava/lang/String;",
             "      ldc \"abc\"\n"
             "      invokevirtual java/lang/String.toCharArray ()[C\n"
             "      invokestatic java/lang/String.valueOf ([C)Ljava/lang/String;\n"
             "      areturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LDC);
    EXPECT_EQ(ops[1], Op::ARETURN);
}

// ── 非常量实参（ILOAD_0）不在白名单 → 保留 ──
TEST(IntrinsicFoldingTest, nonConstantArgKept)
{
    const MethodPassRun r = runMethodPass(
        asmM("(I)I",
             "      iload 0\n"
             "      invokestatic java/lang/Math.abs (I)I\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 3u);
}

// ── invoke 前置边界锚标签（异常起点锚 invoke，外部路径持部分实参进入的
// 守卫面）→ 白名单窗拒收 ──
TEST(IntrinsicFoldingTest, stdConsInteriorLabelKept)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      ldc 3\n"
             "      ldc 9\n"
             "      Lb:\n"
             "      invokestatic java/lang/Math.max (II)I\n"
             "      ireturn\n"
             "      Lr:\n"
             "      athrow\n",
             "      { Lb, Lr, Lr, * }\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 5u);
}

// ── 不动点链折：装箱对折叠暴露 Math.abs 常量窗，再折为单一常量 ──
TEST(IntrinsicFoldingTest, chainedFoldsToFixedPoint)
{
    const MethodPassRun r = runMethodPass(
        asmM("()I",
             "      ldc 7\n"
             "      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;\n"
             "      invokevirtual java/lang/Integer.intValue ()I\n"
             "      invokestatic java/lang/Math.abs (I)I\n"
             "      ireturn\n"),
        mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection                        ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 2u);
    ca::i32 v = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::intValueOf(*ins.session, *all[0], v));
    EXPECT_EQ(v, 7);
    EXPECT_EQ(all[1]->op, Op::IRETURN);
}

// ── 注册面：IntrinsicFolding 可由注册名取回 ──
TEST(IntrinsicFoldingTest, registeredInRegistry)
{
    const mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    const auto                           e   = reg.find("IntrinsicFolding");
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.unwrap().scope, mjbase::spi::PassScope::Method);
}
