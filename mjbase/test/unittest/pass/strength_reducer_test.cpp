// StrengthReducer：2^k 乘除强度削减（imul/lmul→shl；idiv/ldiv→dup+符号
// 修正序列）+ 保守停止 + 负被除数差分矩阵 + 幂等 + maxStack 抬升 +
// A1 帧链接线（resolver 注入 → SMT 再生被读侧接受）。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）；bipush/
// sipush 窄常量装配器保形落地，`iload 0` 规范化为 ILOAD_0，maxStack 由
// 装配器栈分析按真实峰值写入（与原字节级夹具手填值同实）。差分面：
// asm_evaluator 已摘除（libjsim 接位，未落包前不引测试依赖），矩阵验证改为
// 「发射形状逐操作数读取 + C++ 位精确模拟」——k/mask/符号移位量全部从
// 重写后字节读出，按 JVM i32/i64 环绕与算术移位语义在 C++ 侧重放发射序列，
// 对照原始体的闭式语义（wrap 乘 / 向零取整除）。
#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/analysis/smt_read.hpp"
#include "jbc/core/opcode.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/pass/strength_reducer.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// static int m(int)：ILOAD_0; body; IRETURN（jasm 文本装配；帧less 49 面）。
Bytes intMethod(std::string_view body)
{
    return assembleMethodClass("m", "(I)I", "iload 0\n" + std::string(body) + "\nireturn\n");
}

/// static long m(long)：LLOAD_0; body; LRETURN（long 局部双槽，帧less 49 面）。
Bytes longMethod(std::string_view body)
{
    return assembleMethodClass("m", "(J)J", "lload 0\n" + std::string(body) + "\nlreturn\n");
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

// ── 发射序列模拟助手（位精确，JVM i32/i64 语义）──

/// JVM i32 乘法：u32 环绕。
ca::i32 wrapMul(ca::i32 a, ca::i32 b)
{
    return static_cast<ca::i32>(static_cast<ca::u32>(a) * static_cast<ca::u32>(b));
}

/// JVM ISHL：u32 环绕左移。
ca::i32 wrapShl(ca::i32 v, ca::i32 n)
{
    return static_cast<ca::i32>(static_cast<ca::u32>(v) << n);
}

/// 符号修正除法序列重放：(x + ((x >>s signShift) & mask)) >> k。
/// 负值的 >> 即算术右移（JVM ISHR/LSHR 语义，主流编译器一致）。
template<typename T>
T signCorrectedDiv(T x, ca::i32 signShift, T mask, ca::i32 k)
{
    return (x + ((x >> signShift) & mask)) >> k;
}

/// 重写体发射的 int 操作数读出：ICONST_1..5 / BIPUSH / SIPUSH / LDC。
bool emittedIntConstOf(const Inspection& ins, ca::usize idx, ca::i32& out)
{
    const jbc::ir::Insn* in = ins.cu->dom.peek(ins.cu->dom.order()[idx]);
    if (in == nullptr) return false;
    if (in->op >= Op::ICONST_1 && in->op <= Op::ICONST_5) {
        out = static_cast<ca::i32>(in->op) - static_cast<ca::i32>(Op::ICONST_0);
        return true;
    }
    if (in->op == Op::BIPUSH || in->op == Op::SIPUSH) {
        out = in->a;
        return true;
    }
    if (in->op == Op::LDC) {
        out = ldcIntValueOf(ins, in);
        return true;
    }
    return false;
}

/// 重写体发射的 long 操作数读出：LCONST_1 / LDC2_W。
bool emittedLongConstOf(const Inspection& ins, ca::usize idx, ca::i64& out)
{
    const jbc::ir::Insn* in = ins.cu->dom.peek(ins.cu->dom.order()[idx]);
    if (in == nullptr) return false;
    if (in->op == Op::LCONST_1) {
        out = 1;
        return true;
    }
    if (in->op == Op::LDC2_W) {
        out = ldcLongValueOf(ins, in);
        return true;
    }
    return false;
}

constexpr ca::i32 kIntMin = (std::numeric_limits<ca::i32>::min)();
constexpr ca::i32 kIntMax = (std::numeric_limits<ca::i32>::max)();
constexpr ca::i64 kLongMin = (std::numeric_limits<ca::i64>::min)();
constexpr ca::i64 kLongMax = (std::numeric_limits<ca::i64>::max)();

// ── imul → ishl：happy path（ICONST/BIPUSH/LDC 常量来源；LDC_W 同义形由
//    装配器按 CP 下标自选，双臂不再区分）──
TEST(StrengthReducerTest, imulByPowerOfTwoRewritesToShl)
{
    {
        // k=1（ICONST_2）：ILOAD_0; ICONST_2; IMUL → ILOAD_0; BIPUSH 1; ISHL。
        const Bytes in = intMethod("iconst_2\nimul");
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
        const Bytes in = intMethod("bipush 64\nimul");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection      ins = inspect(r.out);
        const std::vector<Op> ops = opsOf(ins);
        const std::vector<Op> expect = {Op::ILOAD_0, Op::BIPUSH, Op::ISHL, Op::IRETURN};
        ASSERT_EQ(ops, expect);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[1])->a, 6);
    }
    {
        // k=20（LDC 大常量 2^20）。
        const Bytes in = intMethod("ldc 1048576\nimul");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection      ins = inspect(r.out);
        const std::vector<Op> ops = opsOf(ins);
        const std::vector<Op> expect = {Op::ILOAD_0, Op::BIPUSH, Op::ISHL, Op::IRETURN};
        ASSERT_EQ(ops, expect);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[1])->a, 20);
    }
}

// ── imul → ishl：差分求值（负值/边界/溢出矩阵，与原始语义逐位一致）──
TEST(StrengthReducerTest, imulRewritePreservesSemantics)
{
    const ca::i32 xs[] = {kIntMin, -7, -1, 0, 1, 7, kIntMax};
    const ca::i32 ks[] = {1, 2, 3, 7, 14};
    for (const ca::i32 k : ks) {
        // 常量载体统一 ldc 文本（装配器窄常量规范化为 LDC，入参值域不受
        // bipush 限制；产出断言钉的是 pass 发射形，与输入载体无关）。
        const Bytes in = intMethod("ldc " + std::to_string(1 << k) + "\nimul");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed()) << "k=" << k;
        const Inspection after = inspect(r.out);
        // 发射形状：ILOAD_0; BIPUSH k; ISHL; IRETURN —— k 逐位读出重放。
        ASSERT_EQ(opsOf(after),
                  (std::vector<Op>{Op::ILOAD_0, Op::BIPUSH, Op::ISHL, Op::IRETURN}))
            << "k=" << k;
        ca::i32 emittedK = 0;
        ASSERT_TRUE(emittedIntConstOf(after, 1, emittedK)) << "k=" << k;
        ASSERT_EQ(emittedK, k) << "k=" << k;
        for (const ca::i32 x : xs) {
            const ca::i32 want = wrapMul(x, 1 << k);   // 原始体 <1<<k>; IMUL
            const ca::i32 got  = wrapShl(x, emittedK); // 重写体 ILOAD_0;BIPUSH;ISHL
            EXPECT_EQ(got, want) << "k=" << k << " x=" << x;
        }
    }
}

// ── lmul → lshl：happy path（LDC2_W 常量来源；k=62 为可达极值）──
TEST(StrengthReducerTest, lmulByPowerOfTwoRewritesToLshl)
{
    {
        const Bytes in = longMethod("ldc2_w 4L\nlmul");
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
        const Bytes in = longMethod("ldc2_w 4611686018427387904L\nlmul");
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
        const Bytes in = intMethod("bipush 2\nidiv");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 1);
        EXPECT_EQ(opsOf(ins)[4], Op::ICONST_1);
        // div 序列峰值 +1：输入 maxStack=2（装配器真值）→ 输出 3。
        EXPECT_EQ(maxStackOf(ins), 3);
    }
    {
        // k=2（ICONST_4）：mask=3 → ICONST_3。
        const Bytes in = intMethod("iconst_4\nidiv");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 2);
        EXPECT_EQ(opsOf(ins)[4], Op::ICONST_3);
    }
    {
        // k=3（BIPUSH 8）：mask=7 → BIPUSH 7。
        const Bytes in = intMethod("bipush 8\nidiv");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 3);
        EXPECT_EQ(opsOf(ins)[4], Op::BIPUSH);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[4])->a, 7);
    }
    {
        // k=8（SIPUSH 256）：mask=255 → SIPUSH 255。
        const Bytes in = intMethod("sipush 256\nidiv");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectDivShape(ins, 31, 8);
        EXPECT_EQ(opsOf(ins)[4], Op::SIPUSH);
        EXPECT_EQ(ins.cu->dom.peek(ins.cu->dom.order()[4])->a, 255);
    }
    {
        // k=16（LDC 65536）：mask=65535 → LDC（constOf 读回值）。
        const Bytes in = intMethod("ldc 65536\nidiv");
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
        const Bytes in = intMethod("ldc 1073741824\nidiv");
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
        // 常量载体统一 ldc 文本（同 imul 矩阵注）。
        const Bytes in = intMethod("ldc " + std::to_string(1 << k) + "\nidiv");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed()) << "k=" << k;
        const Inspection after = inspect(r.out);
        // 发射形状（形状套件逐槽钉死）：ILOAD_0;DUP;BIPUSH 31;ISHR;MASK;
        // IAND;IADD;BIPUSH k;ISHR;IRETURN —— mask/k/符号移位量逐位读出重放。
        ASSERT_EQ(opsOf(after).size(), 10u) << "k=" << k;
        ca::i32 signShift = 0;
        ca::i32 emittedK  = 0;
        ca::i32 mask      = 0;
        ASSERT_TRUE(emittedIntConstOf(after, 2, signShift)) << "k=" << k;
        ASSERT_TRUE(emittedIntConstOf(after, 4, mask)) << "k=" << k;
        ASSERT_TRUE(emittedIntConstOf(after, 7, emittedK)) << "k=" << k;
        ASSERT_EQ(signShift, 31) << "k=" << k;
        ASSERT_EQ(emittedK, k) << "k=" << k;
        ASSERT_EQ(mask, (1 << k) - 1) << "k=" << k;
        for (const ca::i32 x : xs) {
            const ca::i32 want = x / (1 << k);   // 原始体 IDIV：向零取整
            const ca::i32 got  = signCorrectedDiv(x, signShift, mask, emittedK);
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
        const Bytes in = longMethod("ldc2_w 2L\nldiv");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        ASSERT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        expectLongDivShape(ins, 1);
        EXPECT_EQ(opsOf(ins)[4], Op::LCONST_1);
        // long div 序列峰值 +2（DUP2 4 槽 → BIPUSH 5 槽 → mask long 6 槽）：
        // 输入 maxStack=4（装配器真值，原 [x long][c long] 顶格）→ 输出 6 恰覆盖新峰。
        EXPECT_EQ(maxStackOf(ins), 6);
    }
    {
        // k=2（LDC2_W 4）：mask=3 → LDC2_W（constOf 读回值）。
        const Bytes in = longMethod("ldc2_w 4L\nldiv");
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
    const Bytes in = longMethod("ldc2_w 2L\nldiv");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
    ASSERT_TRUE(r.pa.changed());

    const Inspection after = inspect(r.out);
    // 发射形状（形状套件逐槽钉死）：LLOAD_0;DUP2;BIPUSH 63;LSHR;MASK;
    // LAND;LADD;BIPUSH k;LSHR;LRETURN —— mask/k/符号移位量逐位读出重放。
    ASSERT_EQ(opsOf(after).size(), 10u);
    ca::i32 signShift = 0;
    ca::i32 emittedK  = 0;
    ca::i64 mask      = 0;
    ASSERT_TRUE(emittedIntConstOf(after, 2, signShift));
    ASSERT_TRUE(emittedLongConstOf(after, 4, mask));
    ASSERT_TRUE(emittedIntConstOf(after, 7, emittedK));
    ASSERT_EQ(signShift, 63);
    ASSERT_EQ(emittedK, 1);
    ASSERT_EQ(mask, 1);
    const ca::i64 xs[] = {kLongMin, kLongMin / 2, -8, -7, -1, 0, 1, 7, 8, kLongMax};
    for (const ca::i64 x : xs) {
        const ca::i64 got = signCorrectedDiv(x, signShift, mask, emittedK);
        EXPECT_EQ(got, x / 2) << "x=" << x;   // C++ 整数除法即向零取整
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
        const Bytes in = intMethod("iconst_0\nidiv");
        EXPECT_EQ(untouched(in).out, in);
    }
    {
        // 非 2 幂（3 / 6 / -2）。
        const Bytes in = intMethod("bipush 3\nidiv");
        EXPECT_EQ(untouched(in).out, in);
        const Bytes in2 = intMethod("bipush 6\nidiv");
        EXPECT_EQ(untouched(in2).out, in2);
        const Bytes in3 = intMethod("bipush -2\nidiv");
        EXPECT_EQ(untouched(in3).out, in3);
    }
    {
        // 乘 1（k==0）：留给 nop 清理家族。
        const Bytes in = intMethod("iconst_1\nimul");
        EXPECT_EQ(untouched(in).out, in);
    }
    {
        // 除数非常量（ILOAD_1）。
        const Bytes in = assembleMethodClass("m", "(II)I", "iload 0\niload 1\nidiv\nireturn\n");
        EXPECT_EQ(untouched(in).out, in);
    }
    {
        // 其他二元运算（IREM / ISHL / IADD）不匹配：只认 mul/div 四形态。
        const Bytes in = intMethod("bipush 2\nirem");
        EXPECT_EQ(untouched(in).out, in);
        const Bytes in2 = intMethod("bipush 2\nishl");
        EXPECT_EQ(untouched(in2).out, in2);
        const Bytes in3 = intMethod("bipush 2\niadd");
        EXPECT_EQ(untouched(in3).out, in3);
    }
    {
        // float / double 不做（IEEE 形态不同，留后续）：FMUL 2.0f、DDIV 1.0。
        const Bytes in =
            assembleMethodClass("m", "(F)F", "fload 0\nfconst_2\nfmul\nfreturn\n");
        EXPECT_EQ(untouched(in).out, in);
        const Bytes in2 =
            assembleMethodClass("m", "(D)D", "dload 0\ndconst_1\nddiv\ndreturn\n");
        EXPECT_EQ(untouched(in2).out, in2);
    }
}

// ── 边界守卫：分支目标锚定除数常量或运算指令 → 整模式不动 ──
TEST(StrengthReducerTest, labelAnchoredMemberBlocksRewrite)
{
    {
        // 标签锚在除数常量上：尾部 GOTO 回跳（死段回边，装配器放行——探针
        // 实证；goto 落点无栈断言面）。注：窄常量经装配器规范化落 LDC。
        const Bytes in = assembleMethodClass(
            "m", "(I)I", "iload 0\nLd:\nbipush 2\nidiv\nireturn\ngoto Ld\n");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        EXPECT_TRUE(r.pa.areAllPreserved());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        // ILOAD_0; LDC; IDIV; IRETURN; GOTO = 5 条（GOTO 回跳被守卫挡下）。
        ASSERT_EQ(ops.size(), 5u);
        EXPECT_EQ(ops[1], Op::LDC);
        EXPECT_EQ(ops[2], Op::IDIV);
    }
    {
        // 标签锚在运算指令（IDIV）上：尾部 GOTO 回跳同位。
        const Bytes in = assembleMethodClass(
            "m", "(I)I", "iload 0\nbipush 2\nLd:\nidiv\nireturn\ngoto Ld\n");
        const MethodPassRun r = runMethodPass(in, mjbase::pass::StrengthReducer{});
        EXPECT_TRUE(r.pa.areAllPreserved());
        const std::vector<Op> ops = opsOf(inspect(r.out));
        ASSERT_EQ(ops.size(), 5u);
        EXPECT_EQ(ops[1], Op::LDC);
        EXPECT_EQ(ops[2], Op::IDIV);
    }
}

// ── 一趟多改：div 与 mul 模式各一处，单次运行双改写 ──
TEST(StrengthReducerTest, rewritesEveryMatchInOneSweep)
{
    // ILOAD_0; BIPUSH 2; IDIV; ILOAD_0; BIPUSH 4; IMUL; IADD; IRETURN。
    const Bytes in = intMethod("bipush 2\nidiv\niload 0\nbipush 4\nimul\niadd");
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
    // maxStack 只升不降按重写增量抬（int div 增 +1）：装配器真值 3 → 4，
    // 上界语义（≥ 重写序列真峰 3 即合法）。
    EXPECT_EQ(maxStackOf(ins), 4);
}

// ── 幂等：第二次运行 no-op（字节级不变）──
TEST(StrengthReducerTest, secondRunIsNoOp)
{
    const Bytes in = intMethod("bipush 2\nidiv");
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

    // 带分支夹具（直线方法无分支目标 → SMT 恒空，测不到再生）——52 帧面
    //（jasm ≥50 自带 SMT，装配器直出 L1/L2 目标帧）：
    //   ILOAD_0; IFEQ L1; ILOAD_0; BIPUSH 2; IDIV; GOTO L2;
    //   L1: ICONST_1; L2: IRETURN。
    // div 模式（ILOAD_0..IDIV 三指令）改写后扩为 8 条，模式区外分支偏移由
    // DOM 写回按标签位置重编码；resolver 接线后 Strict 重算两个分支目标帧。
    const Bytes in = assembleMethodClass("m",
                                         "(I)I",
                                         "iload 0\n"
                                         "ifeq L1\n"
                                         "iload 0\n"
                                         "bipush 2\n"
                                         "idiv\n"
                                         "goto L2\n"
                                         "L1:\n"
                                         "iconst_1\n"
                                         "L2:\n"
                                         "ireturn\n",
                                         "t/S",
                                         52);
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
