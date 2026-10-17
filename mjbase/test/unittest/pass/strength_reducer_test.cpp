// StrengthReducer：2^k 乘除强度削减（imul/lmul→shl；idiv/ldiv→dup+符号
// 修正序列）+ 保守停止 + 负被除数差分求值矩阵 + 幂等 + maxStack 抬升 +
// A1 帧链接线（resolver 注入 → SMT 再生被读侧接受）。
//
// 语义面用 asm_evaluator 做差分验证：同一入参分别求值原始方法体与重写后
// 方法体，结果必须逐位一致（int 全矩阵；long 的 k=1 形态 LCONST_1 mask 无
// LDC2_W，可求值，对照 C++ 向零取整参考值）。
#include <gtest/gtest.h>

#include <limits>
#include <utility>
#include <vector>

#include "jbc/analysis/smt_read.hpp"
#include "jbc/core/opcode.hpp"

#include "mjbase/asm/evaluator/evaluator.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/pass/strength_reducer.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

using mjbase::asm_evaluator::MethodSig;
using mjbase::asm_evaluator::Value;

/// 字节片段拼接（测试组装用）。
Bytes cat(Bytes a, Bytes b)
{
    appendBytes(a, std::move(b));
    return a;
}

/// LDC（int 常量）指令字节：操作数是 u1，cp 须 <= 0xFF。
Bytes ldcInt(ca::u16 cp)
{
    return {0x12, static_cast<ca::u8>(cp)};
}

/// LDC_W（int 常量）指令字节：操作数是 u2。
Bytes ldcWInt(ca::u16 cp)
{
    return {0x13, static_cast<ca::u8>(cp >> 8), static_cast<ca::u8>(cp)};
}

/// LDC2_W（long 常量）指令字节：操作数是 u2。
Bytes ldc2Long(ca::u16 cp)
{
    return {0x14, static_cast<ca::u8>(cp >> 8), static_cast<ca::u8>(cp)};
}

/// int 立即数推送字节（按值选 BIPUSH/SIPUSH；调用方保证值在域内）。
Bytes pushInt(ca::i32 v)
{
    if (v >= -128 && v <= 127) {
        return {0x10, static_cast<ca::u8>(v)};
    }
    return {0x11, static_cast<ca::u8>((v >> 8) & 0xFF), static_cast<ca::u8>(v & 0xFF)};
}

/// 在 builder 上组装 static int m(int)：ILOAD_0; body; IRETURN。
Bytes intMethodOn(FixtureBuilder& b, Bytes body, ca::u16 maxStack = 2)
{
    Bytes code;
    code.push_back(0x1a);   // ILOAD_0
    appendBytes(code, std::move(body));
    code.push_back(0xac);   // IRETURN
    b.addMethod({"m", "(I)I", std::move(code), 0x0009, {}, maxStack, 1});
    return b.assemble();
}

/// 在 builder 上组装 static long m(long)：LLOAD_0; body; LRETURN（long 局部双槽）。
Bytes longMethodOn(FixtureBuilder& b, Bytes body, ca::u16 maxStack = 4)
{
    Bytes code;
    code.push_back(0x1e);   // LLOAD_0
    appendBytes(code, std::move(body));
    code.push_back(0xad);   // LRETURN
    b.addMethod({"m", "(J)J", std::move(code), 0x0009, {}, maxStack, 2});
    return b.assemble();
}

/// ldc 族指令装载的 int 常量值（constOf 解析失败即测试失败）。
ca::i32 ldcIntValueOf(const Inspection& ins, const jbc::ir::Insn* in)
{
    jbc::ir::ClassSession::ConstSem c;
    EXPECT_TRUE(ins.session->constOf(*in, c));
    return static_cast<ca::i32>(c.num);
}

/// ldc 族指令装载的 long 常量值（constOf 解析失败即测试失败）。
ca::i64 ldcLongValueOf(const Inspection& ins, const jbc::ir::Insn* in)
{
    jbc::ir::ClassSession::ConstSem c;
    EXPECT_TRUE(ins.session->constOf(*in, c));
    return c.num;
}

/// 求值静态 (I)I 方法体（入参 x）；失败返回 false（out 无效）。
bool evalIntBody(const Inspection& ins, ca::i32 x, ca::i32& out)
{
    const auto sig = MethodSig::parse("(I)I", true);
    if (sig.is_err()) return false;
    const auto r = mjbase::asm_evaluator::evaluate(ins.cu->dom, sig.unwrap(),
                                                  std::vector<Value>{Value::ofI32(x)});
    if (r.is_err()) return false;
    out = r.unwrap().value.i32;
    return true;
}

/// 求值静态 (J)J 方法体（入参 x）；失败返回 false（out 无效）。
bool evalLongBody(const Inspection& ins, ca::i64 x, ca::i64& out)
{
    const auto sig = MethodSig::parse("(J)J", true);
    if (sig.is_err()) return false;
    const auto r = mjbase::asm_evaluator::evaluate(ins.cu->dom, sig.unwrap(),
                                                  std::vector<Value>{Value::ofI64(x)});
    if (r.is_err()) return false;
    out = r.unwrap().value.i64;
    return true;
}

constexpr ca::i32 kIntMin = (std::numeric_limits<ca::i32>::min)();
constexpr ca::i32 kIntMax = (std::numeric_limits<ca::i32>::max)();
constexpr ca::i64 kLongMin = (std::numeric_limits<ca::i64>::min)();
constexpr ca::i64 kLongMax = (std::numeric_limits<ca::i64>::max)();

// ── imul → ishl：happy path（BIPUSH/ICONST/LDC/LDC_W 常量来源）──
TEST(StrengthReducerTest, imulByPowerOfTwoRewritesToShl)
{
    {
        // k=1（ICONST_2）：ILOAD_0; ICONST_2; IMUL → ILOAD_0; BIPUSH 1; ISHL。
        FixtureBuilder b("t/S");
        const Bytes    in = intMethodOn(b, {0x05, 0x68});
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        EXPECT_TRUE(r.pa.changed());
        const Inspection      ins = inspect(r.out);
        const std::vector<Op> ops = opsOf(ins);
        const std::vector<Op> expect = {Op::ILOAD_0, Op::BIPUSH, Op::ISHL, Op::IRETURN};
        ASSERT_EQ(ops, expect);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[1])->a, 1);
        // 栈形不变：maxStack 不动。
        EXPECT_EQ(maxStackOf(ins), 2);
    }
    {
        // k=6（BIPUSH 64）。
        FixtureBuilder    b("t/S");
        const Bytes       in = intMethodOn(b, cat(pushInt(64), {0x68}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        const std::vector<Op> expect = {Op::ILOAD_0, Op::BIPUSH, Op::ISHL, Op::IRETURN};
        ASSERT_EQ(ops, expect);
        const Inspection ins = inspect(r.out);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[1])->a, 6);
    }
    {
        // k=20（LDC 大常量 2^20）与 LDC_W 同义形态。
        FixtureBuilder    b("t/S");
        const ca::u16     c  = b.intConst(1048576);
        const Bytes       in = intMethodOn(b, cat(ldcInt(c), {0x68}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection      ins = inspect(r.out);
        const std::vector<Op> ops = opsOf(ins);
        const std::vector<Op> expect = {Op::ILOAD_0, Op::BIPUSH, Op::ISHL, Op::IRETURN};
        ASSERT_EQ(ops, expect);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[1])->a, 20);

        FixtureBuilder    b2("t/S");
        const ca::u16     c2  = b2.intConst(1048576);
        const Bytes       in2 = intMethodOn(b2, cat(ldcWInt(c2), {0x68}));
        const MethodPassRun r2 = runMethodPass(in2, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r2.pa.changed());
        const std::vector<Op> ops2 = opsOf(inspect(r2.out));
        ASSERT_EQ(ops2, expect);
        const Inspection ins2 = inspect(r2.out);
        EXPECT_EQ(ins2.cu->dom.peek(ins2.cu->dom.order()[1])->a, 20);
    }
}

// ── imul → ishl：差分求值（负值/边界/溢出矩阵，与原始语义逐位一致）──
TEST(StrengthReducerTest, imulRewritePreservesSemantics)
{
    const ca::i32 xs[] = {kIntMin, -7, -1, 0, 1, 7, kIntMax};
    const ca::i32 ks[] = {1, 2, 3, 7, 14};
    for (const ca::i32 k : ks) {
        FixtureBuilder    b("t/S");
        const Bytes       in = intMethodOn(b, cat(pushInt(1 << k), {0x68}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed()) << "k=" << k;
        const Inspection before = inspect(in);
        const Inspection after  = inspect(r.out);
        for (const ca::i32 x : xs) {
            ca::i32 want = 0;
            ca::i32 got  = 0;
            ASSERT_TRUE(evalIntBody(before, x, want)) << "k=" << k << " x=" << x;
            ASSERT_TRUE(evalIntBody(after, x, got)) << "k=" << k << " x=" << x;
            EXPECT_EQ(got, want) << "k=" << k << " x=" << x;
        }
    }
}

// ── lmul → lshl：happy path（LDC2_W 常量来源；k=62 为可达极值）──
TEST(StrengthReducerTest, lmulByPowerOfTwoRewritesToLshl)
{
    {
        FixtureBuilder    b("t/S");
        const ca::u16     c  = b.longConst(4);
        const Bytes       in = longMethodOn(b, cat(ldc2Long(c), {0x69}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection      ins = inspect(r.out);
        const std::vector<Op> ops = opsOf(ins);
        const std::vector<Op> expect = {Op::LLOAD_0, Op::BIPUSH, Op::LSHL, Op::LRETURN};
        ASSERT_EQ(ops, expect);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[1])->a, 2);
        EXPECT_EQ(maxStackOf(ins), 4);
    }
    {
        // k=62（2^62，long 侧可达的最大 2 幂指数）。
        FixtureBuilder    b("t/S");
        const ca::u16     c  = b.longConst(static_cast<ca::i64>(1) << 62);
        const Bytes       in = longMethodOn(b, cat(ldc2Long(c), {0x69}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection      ins = inspect(r.out);
        const std::vector<Op> ops = opsOf(ins);
        const std::vector<Op> expect = {Op::LLOAD_0, Op::BIPUSH, Op::LSHL, Op::LRETURN};
        ASSERT_EQ(ops, expect);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[1])->a, 62);
    }
}

// ── idiv → 符号修正序列：happy path（k=1/k=2/k=3/k=8/k=16/k=30，
//    mask 物化覆盖 ICONST/BIPUSH/SIPUSH/LDC 全选路）──
TEST(StrengthReducerTest, idivByPowerOfTwoRewritesToSequence)
{
    const auto expectDivShape = [](const Inspection& ins, ca::i32 signShift, ca::i32 k) {
        const std::vector<Op> ops = opsOf(ins);
        ASSERT_EQ(ops.size(), 10u);
        EXPECT_EQ(ops[0], Op::ILOAD_0);
        EXPECT_EQ(ops[1], Op::DUP);
        EXPECT_EQ(ops[2], Op::BIPUSH);
        EXPECT_EQ(ops[3], Op::ISHR);
        // ops[4] = mask 推送（ICONST/BIPUSH/SIPUSH/LDC 按值选路，各用例分验）。
        EXPECT_EQ(ops[5], Op::IAND);
        EXPECT_EQ(ops[6], Op::IADD);
        EXPECT_EQ(ops[7], Op::BIPUSH);
        EXPECT_EQ(ops[8], Op::ISHR);
        EXPECT_EQ(ops[9], Op::IRETURN);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[2])->a, signShift);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[7])->a, k);
    };

    {
        // k=1（BIPUSH 2）：mask=1 → ICONST_1。
        FixtureBuilder    b("t/S");
        const Bytes       in = intMethodOn(b, cat(pushInt(2), {0x6c}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 1);
        EXPECT_EQ(opsOf(ins)[4], Op::ICONST_1);
        // div 序列峰值 +1：输入 maxStack=2 → 输出 3。
        EXPECT_EQ(maxStackOf(ins), 3);
    }
    {
        // k=2（ICONST_4）：mask=3 → ICONST_3。
        FixtureBuilder    b("t/S");
        const Bytes       in = intMethodOn(b, {0x07, 0x6c}); // ICONST_4（除 4）
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 2);
        EXPECT_EQ(opsOf(ins)[4], Op::ICONST_3);
    }
    {
        // k=3（BIPUSH 8）：mask=7 → BIPUSH 7。
        FixtureBuilder    b("t/S");
        const Bytes       in = intMethodOn(b, cat(pushInt(8), {0x6c}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 3);
        EXPECT_EQ(opsOf(ins)[4], Op::BIPUSH);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[4])->a, 7);
    }
    {
        // k=8（SIPUSH 256）：mask=255 → SIPUSH 255。
        FixtureBuilder    b("t/S");
        const Bytes       in = intMethodOn(b, cat(pushInt(256), {0x6c}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 8);
        EXPECT_EQ(opsOf(ins)[4], Op::SIPUSH);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[4])->a, 255);
    }
    {
        // k=16（LDC 65536）：mask=65535 → LDC（constOf 读回值）。
        FixtureBuilder    b("t/S");
        const ca::u16     c  = b.intConst(65536);
        const Bytes       in = intMethodOn(b, cat(ldcInt(c), {0x6c}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 16);
        EXPECT_EQ(opsOf(ins)[4], Op::LDC);
        EXPECT_EQ(ldcIntValueOf(ins, ins.cu->dom.peek(ins.cu->dom.order()[4])), 65535);
    }
    {
        // k=30（LDC 2^30，int 侧可达极值）：mask=2^30-1 → LDC。
        // 注：k=31 不可达——+2^31 不是正 i32 常量（见头文件保守停止清单）。
        FixtureBuilder    b("t/S");
        const ca::u16     c  = b.intConst(1 << 30);
        const Bytes       in = intMethodOn(b, cat(ldcInt(c), {0x6c}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 30);
        EXPECT_EQ(opsOf(ins)[4], Op::LDC);
        EXPECT_EQ(ldcIntValueOf(ins, ins.cu->dom.peek(ins.cu->dom.order()[4])), (1 << 30) - 1);
    }
}

// ── idiv：差分求值（负被除数矩阵，向零取整逐位保持）──
TEST(StrengthReducerTest, idivRewritePreservesSemantics)
{
    const ca::i32 xs[] = {kIntMin, kIntMin + 1, -8, -7, -1, 0, 1, 7, 8, kIntMax};
    const ca::i32 ks[] = {1, 2, 3, 7, 14};
    for (const ca::i32 k : ks) {
        FixtureBuilder    b("t/S");
        const Bytes       in = intMethodOn(b, cat(pushInt(1 << k), {0x6c}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed()) << "k=" << k;
        const Inspection before = inspect(in);
        const Inspection after  = inspect(r.out);
        for (const ca::i32 x : xs) {
            ca::i32 want = 0;
            ca::i32 got  = 0;
            ASSERT_TRUE(evalIntBody(before, x, want)) << "k=" << k << " x=" << x;
            ASSERT_TRUE(evalIntBody(after, x, got)) << "k=" << k << " x=" << x;
            EXPECT_EQ(got, want) << "k=" << k << " x=" << x;
        }
    }
}

// ── ldiv → 符号修正序列：happy path（DUP2/BIPUSH 63/LSHR/mask/LAND/LADD）──
TEST(StrengthReducerTest, ldivByPowerOfTwoRewritesToSequence)
{
    const auto expectLongDivShape = [](const Inspection& ins, ca::i32 k) {
        const std::vector<Op> ops = opsOf(ins);
        ASSERT_EQ(ops.size(), 10u);
        EXPECT_EQ(ops[0], Op::LLOAD_0);
        EXPECT_EQ(ops[1], Op::DUP2);
        EXPECT_EQ(ops[2], Op::BIPUSH);
        EXPECT_EQ(ops[3], Op::LSHR);
        EXPECT_EQ(ops[5], Op::LAND);
        EXPECT_EQ(ops[6], Op::LADD);
        EXPECT_EQ(ops[7], Op::BIPUSH);
        EXPECT_EQ(ops[8], Op::LSHR);
        EXPECT_EQ(ops[9], Op::LRETURN);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[2])->a, 63);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[7])->a, k);
    };

    {
        // k=1（LDC2_W 2）：mask=1 → LCONST_1（无 CP 追加）。
        FixtureBuilder    b("t/S");
        const ca::u16     c  = b.longConst(2);
        const Bytes       in = longMethodOn(b, cat(ldc2Long(c), {0x6d}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectLongDivShape(ins, 1);
        EXPECT_EQ(opsOf(ins)[4], Op::LCONST_1);
        // long div 序列峰值 +2（DUP2 4 槽 → BIPUSH 5 槽 → mask long 6 槽）：
        // 输入 maxStack=4（原 [x long][c long] 顶格）→ 输出 6 恰覆盖新峰。
        EXPECT_EQ(maxStackOf(ins), 6);
    }
    {
        // k=2（LDC2_W 4）：mask=3 → LDC2_W（constOf 读回值）。
        FixtureBuilder    b("t/S");
        const ca::u16     c  = b.longConst(4);
        const Bytes       in = longMethodOn(b, cat(ldc2Long(c), {0x6d}));
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectLongDivShape(ins, 2);
        EXPECT_EQ(opsOf(ins)[4], Op::LDC2_W);
        EXPECT_EQ(ldcLongValueOf(ins, ins.cu->dom.peek(ins.cu->dom.order()[4])), 3);
    }
}

// ── ldiv k=1 重写序列：按 Java 向零取整求值（负被除数矩阵）──
TEST(StrengthReducerTest, ldivRewrittenSequenceComputesJavaDivision)
{
    FixtureBuilder    b("t/S");
    const ca::u16     c  = b.longConst(2);
    const Bytes       in = longMethodOn(b, cat(ldc2Long(c), {0x6d}));
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
    ASSERT_TRUE(r.pa.changed());

    const Inspection after = inspect(r.out);
    const ca::i64    xs[]  = {kLongMin, kLongMin / 2, -8, -7, -1, 0, 1, 7, 8, kLongMax};
    for (const ca::i64 x : xs) {
        ca::i64 got = 0;
        ASSERT_TRUE(evalLongBody(after, x, got)) << "x=" << x;
        EXPECT_EQ(got, x / 2) << "x=" << x;   // C++11 整数除法即向零取整
    }
}

// ── 保守停止：除数 0 / 负数 / 非 2 幂 / 乘 1 / 非常量 / 其他二元运算 ──
TEST(StrengthReducerTest, conservativeStopsLeaveSequenceIntact)
{
    const auto untouched = [](Bytes in) {
        const MethodPassRun r = runMethodPass(std::move(in), mjbase::pass::StrengthReducer{});
        EXPECT_TRUE(r.pa.areAllPreserved());
        return r;
    };
    {
        // 除数 0（ICONST_0）：保留运行期 ArithmeticException 语义。
        FixtureBuilder b("t/S");
        const Bytes    in = intMethodOn(b, {0x03, 0x6c});
        EXPECT_EQ(untouched(in).out, in);
    }
    {
        // 非 2 幂（3 / 6 / -2）。
        FixtureBuilder b("t/S");
        const Bytes    in = intMethodOn(b, cat(pushInt(3), {0x6c}));
        EXPECT_EQ(untouched(in).out, in);
        FixtureBuilder b2("t/S");
        const Bytes    in2 = intMethodOn(b2, cat(pushInt(6), {0x6c}));
        EXPECT_EQ(untouched(in2).out, in2);
        FixtureBuilder b3("t/S");
        const Bytes    in3 = intMethodOn(b3, cat(pushInt(-2), {0x6c}));
        EXPECT_EQ(untouched(in3).out, in3);
    }
    {
        // 乘 1（k==0）：留给 nop 清理家族。
        FixtureBuilder b("t/S");
        const Bytes    in = intMethodOn(b, {0x04, 0x68});
        EXPECT_EQ(untouched(in).out, in);
    }
    {
        // 除数非常量（ILOAD_1）。
        FixtureBuilder b("t/S");
        b.addMethod({"m", "(II)I", {0x1a, 0x1b, 0x6c, 0xac}, 0x0009, {}, 2, 2});
        const Bytes in = b.assemble();
        EXPECT_EQ(untouched(in).out, in);
    }
    {
        // 其他二元运算（IREM / ISHL / IADD）不匹配：只认 mul/div 四形态。
        FixtureBuilder b("t/S");
        const Bytes    in = intMethodOn(b, cat(pushInt(2), {0x70}));   // IREM
        EXPECT_EQ(untouched(in).out, in);
        FixtureBuilder b2("t/S");
        const Bytes    in2 = intMethodOn(b2, cat(pushInt(2), {0x78}));   // ISHL
        EXPECT_EQ(untouched(in2).out, in2);
        FixtureBuilder b3("t/S");
        const Bytes    in3 = intMethodOn(b3, cat(pushInt(2), {0x60}));   // IADD
        EXPECT_EQ(untouched(in3).out, in3);
    }
    {
        // float / double 不做（IEEE 形态不同，留后续）：FMUL 2.0f、DDIV 2.0。
        FixtureBuilder b("t/S");
        b.addMethod({"m", "(F)F", {0x22, 0x0d, 0x6a, 0xae}, 0x0009, {}, 2, 1});
        const Bytes in = b.assemble();
        EXPECT_EQ(untouched(in).out, in);
        FixtureBuilder b2("t/S");
        b2.addMethod({"m", "(D)D", {0x26, 0x0f, 0x6f, 0xaf}, 0x0009, {}, 4, 2});
        const Bytes in2 = b2.assemble();
        EXPECT_EQ(untouched(in2).out, in2);
    }
}

// ── 边界守卫：分支目标锚定除数常量或运算指令 → 整模式不动 ──
TEST(StrengthReducerTest, labelAnchoredMemberBlocksRewrite)
{
    {
        // 标签锚在除数常量（BIPUSH, pc1）上：尾部 GOTO 回跳 pc1。
        // ILOAD_0(0) BIPUSH 2(1) IDIV(3) IRETURN(4) GOTO(5) offset -4 → pc1。
        FixtureBuilder b("t/S");
        b.addMethod({"m", "(I)I", {0x1a, 0x10, 0x02, 0x6c, 0xac, 0xa7, 0xff, 0xfc},
                     0x0009,
                     {},
                     2,
                     1});
        const Bytes       in = b.assemble();
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        EXPECT_TRUE(r.pa.areAllPreserved());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        // ILOAD_0; BIPUSH 2; IDIV; IRETURN; GOTO = 5 条（GOTO 回跳被守卫挡下）。
        ASSERT_EQ(ops.size(), 5u);
        EXPECT_EQ(ops[1], Op::BIPUSH);
        EXPECT_EQ(ops[2], Op::IDIV);
    }
    {
        // 标签锚在运算指令（IDIV, pc3）上：尾部 GOTO 回跳 pc3（offset -2）。
        FixtureBuilder b("t/S");
        b.addMethod({"m", "(I)I", {0x1a, 0x10, 0x02, 0x6c, 0xac, 0xa7, 0xff, 0xfe},
                     0x0009,
                     {},
                     2,
                     1});
        const Bytes       in = b.assemble();
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        EXPECT_TRUE(r.pa.areAllPreserved());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        ASSERT_EQ(ops.size(), 5u);
        EXPECT_EQ(ops[1], Op::BIPUSH);
        EXPECT_EQ(ops[2], Op::IDIV);
    }
}

// ── 一趟多改：div 与 mul 模式各一处，单次运行双改写 ──
TEST(StrengthReducerTest, rewritesEveryMatchInOneSweep)
{
    // ILOAD_0; BIPUSH 2; IDIV; ILOAD_0; BIPUSH 4; IMUL; IADD; IRETURN。
    FixtureBuilder    b("t/S");
    const Bytes       in = intMethodOn(b, {0x10, 0x02, 0x6c, 0x1a, 0x10, 0x04, 0x68, 0x60});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
    ASSERT_TRUE(r.pa.changed());

    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    // ILOAD_0; DUP; BIPUSH 31; ISHR; ICONST_1; IAND; IADD; BIPUSH 1; ISHR;
    // ILOAD_0; BIPUSH 2; ISHL; IADD; IRETURN。
    const std::vector<Op> expect = {Op::ILOAD_0, Op::DUP,     Op::BIPUSH,  Op::ISHR,
                                    Op::ICONST_1, Op::IAND,  Op::IADD,    Op::BIPUSH,
                                    Op::ISHR,    Op::ILOAD_0, Op::BIPUSH, Op::ISHL,
                                    Op::IADD,    Op::IRETURN};
    ASSERT_EQ(ops, expect);
    EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[10])->a, 2);
    EXPECT_EQ(maxStackOf(ins), 3);
}

// ── 幂等：第二次运行 no-op（字节级不变）──
TEST(StrengthReducerTest, secondRunIsNoOp)
{
    FixtureBuilder    b("t/S");
    const Bytes       in = intMethodOn(b, cat(pushInt(2), {0x6c}));
    const MethodPassRun first = runMethodPass(in, mjbase::pass::StrengthReducer{});
    ASSERT_TRUE(first.pa.changed());
    const MethodPassRun second = runMethodPass(first.out, mjbase::pass::StrengthReducer{});
    EXPECT_TRUE(second.pa.areAllPreserved());
    EXPECT_EQ(second.out, first.out);
}

// ── A1 帧链接线：resolver 注入 → SMT 再生且被读侧接受 ──
TEST(StrengthReducerTest, resolverWiredPipelineRegeneratesSmt)
{
    mjbase::KeyPool                        pool;
    mjbase::hierarchy::ClassHierarchy      hierarchy{pool};
    mjbase::spi::CommonSuperClassResolver  resolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{resolver};

    mjbase::spi::PassPipelineOptions options;
    options.typeResolver = &typeResolver;

    // 带分支夹具（直线方法无分支目标 → SMT 恒空，测不到再生）：
    //   ILOAD_0; IFEQ +10 → pc11; ILOAD_0; BIPUSH 2; IDIV; GOTO +4 → pc12;
    //   ICONST_1(pc11, IFEQ 目标); IRETURN(pc12, GOTO 目标)。
    // 偏移基准 = 分支操作码自身 pc（code_dom.cpp resolve：dst = r.pc + off）。
    // div 模式（pc4..pc7 三指令）改写后扩为 8 条，模式区外分支偏移由 DOM
    // 写回按标签位置重编码；resolver 接线后 Strict 重算两个分支目标帧。
    FixtureBuilder b("t/S");
    b.addMethod({"m", "(I)I",
                 {0x1a, 0x99, 0x00, 0x0a, 0x1a, 0x10, 0x02, 0x6c, 0xa7, 0x00, 0x04, 0x04, 0xac},
                 0x0009,
                 {},
                 2,
                 1});
    const Bytes          in = b.assemble();
    const MethodPassRun  r  = runMethodPassOpts(in, options, mjbase::pass::StrengthReducer{});
    EXPECT_TRUE(r.pa.changed());

    // 写回产物再升格：SMT 存在、读侧（readFramesFromSmt）防御全绿且非空；
    // maxes 被重算覆盖后仍 ≥ 序列的栈需求（峰值 3）。
    const Inspection ins = inspect(r.out);
    auto             frames = jbc::analysis::readFramesFromSmt(*ins.session, *ins.cu);
    ASSERT_TRUE(frames.is_ok());
    const std::vector<jbc::analysis::SmtFrameView> views = std::move(frames).unwrap();
    EXPECT_FALSE(views.empty());
    EXPECT_GE(ins.cu->maxStack, 3);
}

}   // namespace
