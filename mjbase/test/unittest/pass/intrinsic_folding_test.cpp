// IntrinsicFolding：装箱/拆箱对整删、种子 Random 五窗 LCG 折叠、白名单
// 纯函数常量折叠（Math/String/Integer/Long）、toCharArray+valueOf 恒等对、
// 窗内边界锚标签拒收、不动点链折、非白名单形态保留。
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

using namespace mjbase::test;
using jbc::core::Op;

namespace {

void put16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v & 0xff));
}

void putSipush(Bytes& b, ca::i16 v) { put16(b, static_cast<ca::u16>(v)); }

/// 带 u16 CP 槽的 invoke/new/ldc2_w 形指令落笔（op + u16，共 3 字节）。
void putInvoke(Bytes& b, ca::u8 op, ca::u16 cp)
{
    b.push_back(op);
    put16(b, cp);
}

Bytes oneMethod(std::string_view desc, Bytes code, ca::u16 maxStack = 1)
{
    FixtureBuilder b("t/C");
    b.addMethod({"m", desc, std::move(code), 0x0009, {}, maxStack});
    return b.assemble();
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
    FixtureBuilder b("t/C");
    const ca::u16 valueOf =
        b.methodRef("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;");
    const ca::u16 intValue = b.methodRef("java/lang/Integer", "intValue", "()I");
    Bytes code = {0x10, 0x07};   // BIPUSH 7
    putInvoke(code, 0xb8, valueOf);
    putInvoke(code, 0xb6, intValue);
    code.push_back(0xac);   // IRETURN
    b.addMethod({"m", "()I", std::move(code)});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

// ── cat2 装箱对：Long.valueOf(J) + longValue() ──
TEST(IntrinsicFoldingTest, boxUnboxLongPairFolds)
{
    FixtureBuilder b("t/C");
    const ca::u16 seed = b.longConst(9);
    const ca::u16 valueOf =
        b.methodRef("java/lang/Long", "valueOf", "(J)Ljava/lang/Long;");
    const ca::u16 longValue = b.methodRef("java/lang/Long", "longValue", "()J");
    Bytes code;
    putInvoke(code, 0x14, seed);   // LDC2_W
    putInvoke(code, 0xb8, valueOf);
    putInvoke(code, 0xb6, longValue);
    code.push_back(0xad);   // LRETURN
    b.addMethod({"m", "()J", std::move(code), 0x0009, {}, 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LDC2_W);
    EXPECT_EQ(ops[1], Op::LRETURN);
}

// ── 族失配：valueOf 与他类拆箱混搭不折 ──
TEST(IntrinsicFoldingTest, boxUnboxMismatchKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 valueOf =
        b.methodRef("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;");
    const ca::u16 doubleValue = b.methodRef("java/lang/Double", "doubleValue", "()D");
    Bytes code = {0x10, 0x07};
    putInvoke(code, 0xb8, valueOf);
    putInvoke(code, 0xb6, doubleValue);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code), 0x0009, {}, 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 4u);
}

// ── 拆箱前置边界锚标签（外部路径持包装引用进入）→ 整对拒收 ──
TEST(IntrinsicFoldingTest, boxUnboxInteriorLabelKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 valueOf =
        b.methodRef("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;");
    const ca::u16 intValue = b.methodRef("java/lang/Integer", "intValue", "()I");
    Bytes code;
    code.push_back(0xa7);   // GOTO（pc0）→ pc8（intValue）
    putSipush(code, 8);
    code.push_back(0x10);   // BIPUSH 7（pc3）
    code.push_back(0x07);
    putInvoke(code, 0xb8, valueOf);   // valueOf（pc5）
    putInvoke(code, 0xb6, intValue);  // intValue（pc8，锚点）
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code)});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 5u);
}

// ── 种子 Random：new Random(0).nextInt() 折叠为 JDK 已知位模式 ──
TEST(IntrinsicFoldingTest, randomSeedNextIntFolds)
{
    FixtureBuilder b("t/C");
    const ca::u16 cls   = b.cls("java/util/Random");
    const ca::u16 seed  = b.longConst(0);
    const ca::u16 init  = b.methodRef("java/util/Random", "<init>", "(J)V");
    const ca::u16 nextI = b.methodRef("java/util/Random", "nextInt", "()I");
    Bytes code;
    putInvoke(code, 0xbb, cls);   // NEW
    code.push_back(0x59);   // DUP
    putInvoke(code, 0x14, seed);   // LDC2_W
    putInvoke(code, 0xb7, init);
    putInvoke(code, 0xb6, nextI);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code), 0x0009, {}, 4});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
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
    FixtureBuilder b("t/C");
    const ca::u16 cls   = b.cls("java/util/Random");
    const ca::u16 seed  = b.longConst(7);
    const ca::u16 init  = b.methodRef("java/util/Random", "<init>", "(J)V");
    const ca::u16 nextB = b.methodRef("java/util/Random", "nextBoolean", "()Z");
    Bytes code;
    putInvoke(code, 0xbb, cls);
    code.push_back(0x59);
    putInvoke(code, 0x14, seed);
    putInvoke(code, 0xb7, init);
    putInvoke(code, 0xb6, nextB);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code), 0x0009, {}, 4});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 2u);
    ca::i32 v = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::intValueOf(*ins.session, *all[0], v));
    EXPECT_EQ(v, jstd::Random(7).next(1));
}

// ── 种子装载前置边界锚标签 → 五窗拒收 ──
TEST(IntrinsicFoldingTest, randomSeedInteriorLabelKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 cls   = b.cls("java/util/Random");
    const ca::u16 seed  = b.longConst(0);
    const ca::u16 init  = b.methodRef("java/util/Random", "<init>", "(J)V");
    const ca::u16 nextI = b.methodRef("java/util/Random", "nextInt", "()I");
    Bytes code;
    code.push_back(0xa7);   // GOTO（pc0）→ pc7（LDC2_W 种子装载）
    putSipush(code, 7);
    putInvoke(code, 0xbb, cls);   // NEW（pc3）
    code.push_back(0x59);   // DUP（pc6）
    putInvoke(code, 0x14, seed);   // LDC2_W（pc7，锚点）
    putInvoke(code, 0xb7, init);
    putInvoke(code, 0xb6, nextI);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code), 0x0009, {}, 4});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 7u);
}

// ── Math.abs(-42) → BIPUSH 42 ──
TEST(IntrinsicFoldingTest, mathAbsFolds)
{
    FixtureBuilder b("t/C");
    const ca::u16 abs = b.methodRef("java/lang/Math", "abs", "(I)I");
    Bytes code = {0x11};
    putSipush(code, -42);
    putInvoke(code, 0xb8, abs);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code)});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

// ── Math.max(3, 9) → BIPUSH 9；幂等 ──
TEST(IntrinsicFoldingTest, mathMaxFolds)
{
    FixtureBuilder b("t/C");
    const ca::u16 max = b.methodRef("java/lang/Math", "max", "(II)I");
    Bytes code = {0x11};
    putSipush(code, 3);
    code.push_back(0x11);
    putSipush(code, 9);
    putInvoke(code, 0xb8, max);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code), 0x0009, {}, 2});
    const Bytes bytes = b.assemble();
    const MethodPassRun  r     = runMethodPass(bytes, mjbase::pass::IntrinsicFolding{});
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
    FixtureBuilder b("t/C");
    const ca::u16 vcp = b.intConst(-2147483647 - 1);
    const ca::u16 abs = b.methodRef("java/lang/Math", "abs", "(I)I");
    Bytes code = {0x12};   // LDC
    code.push_back(static_cast<ca::u8>(vcp));
    putInvoke(code, 0xb8, abs);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code)});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 2u);
    ca::i32 v = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::intValueOf(*ins.session, *all[0], v));
    EXPECT_EQ(v, -2147483647 - 1);
}

// ── Math.min(2.0f, 1.0f) → FCONST_1 ──
TEST(IntrinsicFoldingTest, mathMinFloatFolds)
{
    FixtureBuilder b("t/C");
    const ca::u16 min = b.methodRef("java/lang/Math", "min", "(FF)F");
    Bytes code = {0x0d, 0x0c};   // FCONST_2 FCONST_1
    putInvoke(code, 0xb8, min);
    code.push_back(0xae);   // FRETURN
    b.addMethod({"m", "()F", std::move(code), 0x0009, {}, 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::FCONST_1);
    EXPECT_EQ(ops[1], Op::FRETURN);
}

// ── String.length：常量接收者 → BIPUSH 5 ──
TEST(IntrinsicFoldingTest, stringLengthFolds)
{
    FixtureBuilder b("t/C");
    const ca::u16 s   = b.strConst("hello");
    const ca::u16 len = b.methodRef("java/lang/String", "length", "()I");
    Bytes code = {0x12};
    code.push_back(static_cast<ca::u8>(s));
    putInvoke(code, 0xb6, len);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code)});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_5);   // 5 落 iconst 档
}

// ── String.substring(1,3) → LDC "el" ──
TEST(IntrinsicFoldingTest, stringSubstringFolds)
{
    FixtureBuilder b("t/C");
    const ca::u16 s   = b.strConst("hello");
    const ca::u16 sub = b.methodRef("java/lang/String", "substring", "(II)Ljava/lang/String;");
    Bytes code = {0x12};
    code.push_back(static_cast<ca::u8>(s));
    code.push_back(0x11);
    putSipush(code, 1);
    code.push_back(0x11);
    putSipush(code, 3);
    putInvoke(code, 0xb6, sub);
    code.push_back(0xb0);   // ARETURN
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 3});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
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
    FixtureBuilder b("t/C");
    const ca::u16 s   = b.strConst("hello");
    const ca::u16 sub = b.methodRef("java/lang/String", "substring", "(II)Ljava/lang/String;");
    Bytes code = {0x12};
    code.push_back(static_cast<ca::u8>(s));
    code.push_back(0x11);
    putSipush(code, 3);
    code.push_back(0x11);
    putSipush(code, 1);
    putInvoke(code, 0xb6, sub);
    code.push_back(0xb0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 3});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 5u);
}

// ── Integer.parseInt("42") → BIPUSH 42；Long.parseLong("123") → LDC2_W ──
TEST(IntrinsicFoldingTest, parseIntAndParseLongFold)
{
    {
        FixtureBuilder b("t/C");
        const ca::u16 s  = b.strConst("42");
        const ca::u16 ps = b.methodRef("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I");
        Bytes code = {0x12};
        code.push_back(static_cast<ca::u8>(s));
        putInvoke(code, 0xb8, ps);
        code.push_back(0xac);
        b.addMethod({"m", "()I", std::move(code)});
        const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
        EXPECT_TRUE(r.pa.changed());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        ASSERT_EQ(ops.size(), 2u);
        EXPECT_EQ(ops[0], Op::BIPUSH);
    }
    {
        FixtureBuilder b("t/C");
        const ca::u16 s  = b.strConst("123");
        const ca::u16 pl = b.methodRef("java/lang/Long", "parseLong", "(Ljava/lang/String;)J");
        Bytes code = {0x12};
        code.push_back(static_cast<ca::u8>(s));
        putInvoke(code, 0xb8, pl);
        code.push_back(0xad);
        b.addMethod({"m", "()J", std::move(code), 0x0009, {}, 2});
        const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
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
    FixtureBuilder b("t/C");
    const ca::u16 s  = b.strConst("4x2");
    const ca::u16 ps = b.methodRef("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I");
    Bytes code = {0x12};
    code.push_back(static_cast<ca::u8>(s));
    putInvoke(code, 0xb8, ps);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code)});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 3u);
}

// ── Integer.bitCount / toHexString ──
TEST(IntrinsicFoldingTest, bitOpsFolds)
{
    {
        FixtureBuilder b("t/C");
        const ca::u16 bc = b.methodRef("java/lang/Integer", "bitCount", "(I)I");
        Bytes code = {0x11};
        putSipush(code, 255);
        putInvoke(code, 0xb8, bc);
        code.push_back(0xac);
        b.addMethod({"m", "()I", std::move(code)});
        const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
        EXPECT_TRUE(r.pa.changed());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        ASSERT_EQ(ops.size(), 2u);
        EXPECT_EQ(ops[0], Op::BIPUSH);   // 8 超 iconst 档，落 bipush
        const MethodPassRun r2 = runMethodPass(r.out, mjbase::pass::IntrinsicFolding{});
        EXPECT_TRUE(r2.pa.areAllPreserved());
    }
    {
        FixtureBuilder b("t/C");
        const ca::u16 hx =
            b.methodRef("java/lang/Integer", "toHexString", "(I)Ljava/lang/String;");
        Bytes code = {0x11};
        putSipush(code, 255);
        putInvoke(code, 0xb8, hx);
        code.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(code)});
        const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
        EXPECT_TRUE(r.pa.changed());
        const Inspection            ins = inspect(r.out);
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
    FixtureBuilder b("t/C");
    const ca::u16 s  = b.strConst("abc");
    const ca::u16 tc = b.methodRef("java/lang/String", "toCharArray", "()[C");
    const ca::u16 vo = b.methodRef("java/lang/String", "valueOf", "([C)Ljava/lang/String;");
    Bytes code = {0x12};
    code.push_back(static_cast<ca::u8>(s));
    putInvoke(code, 0xb6, tc);
    putInvoke(code, 0xb8, vo);
    code.push_back(0xb0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code)});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LDC);
    EXPECT_EQ(ops[1], Op::ARETURN);
}

// ── 非常量实参（ILOAD_0）不在白名单 → 保留 ──
TEST(IntrinsicFoldingTest, nonConstantArgKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 abs = b.methodRef("java/lang/Math", "abs", "(I)I");
    Bytes code = {0x1a};   // ILOAD_0
    putInvoke(code, 0xb8, abs);
    code.push_back(0xac);
    b.addMethod({"m", "(I)I", std::move(code)});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 3u);
}

// ── invoke 前置边界锚标签（外部路径持部分实参进入）→ 白名单窗拒收 ──
TEST(IntrinsicFoldingTest, stdConsInteriorLabelKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 max = b.methodRef("java/lang/Math", "max", "(II)I");
    Bytes code = {0x11};
    putSipush(code, 3);   // SIPUSH 3（pc0）
    code.push_back(0x11);
    putSipush(code, 9);   // SIPUSH 9（pc3）
    code.push_back(0xa7); // GOTO（pc6）→ pc9（invoke）
    putSipush(code, 3);
    putInvoke(code, 0xb8, max);   // max（pc9，锚点）
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code), 0x0009, {}, 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 5u);
}

// ── 不动点链折：装箱对折叠暴露 Math.abs 常量窗，再折为单一常量 ──
TEST(IntrinsicFoldingTest, chainedFoldsToFixedPoint)
{
    FixtureBuilder b("t/C");
    const ca::u16 valueOf =
        b.methodRef("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;");
    const ca::u16 intValue = b.methodRef("java/lang/Integer", "intValue", "()I");
    const ca::u16 abs      = b.methodRef("java/lang/Math", "abs", "(I)I");
    Bytes code = {0x10, 0x07};
    putInvoke(code, 0xb8, valueOf);
    putInvoke(code, 0xb6, intValue);
    putInvoke(code, 0xb8, abs);
    code.push_back(0xac);
    b.addMethod({"m", "()I", std::move(code)});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::IntrinsicFolding{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
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
