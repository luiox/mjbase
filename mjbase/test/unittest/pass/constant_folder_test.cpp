// ConstantFolder：操作数栈常量链折叠。op 族全覆盖 + 回绕/除零/MIN/-1 红线 +
// 负操作数矩阵 + 链识别边界（链长 1 / 类型混用 / 标签守卫）+ 幂等 + PA 纪律。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）；窄常量
//（bipush/sipush）装配器按自身口径规范化为 LDC，float/double 经 ldc/ldc2_w
// 字面直写（本地 FloatDoubleFixture 随字节级 builder 一并退役）。装配器栈
// 分析只约束可达代码（JVMS 口径）：验证「栈欠额/类型混用拒绝折叠」的输入
// 形以 goto 越段死代码表达（死段不进帧分析、pass 的虚拟栈照判失配），字节
// 语义与原手拼夹具同实。
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

#include "mjbase/pass/constant_folder.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

// ---------------------------------------------------------------------------
// 夹具辅助
// ---------------------------------------------------------------------------

/// 单方法容器：body 为 jasm 指令体文本（maxStack 由装配器按真值写入）。
Bytes oneMethod(std::string_view body)
{
    return assembleMethodClass("m", "()V", body);
}

/// 死段表达：goto 越段后 return，中段为 pass 扫描面（死段不进帧分析）。
/// 用于装配器活代码帧分析拒收的「欠额 / 类型混用」折叠失配输入。
Bytes deadBody(std::string_view deadInsns)
{
    return assembleMethodClass("m",
                               "()V",
                               "goto Lend\n" + std::string(deadInsns) + "Lend:\nreturn\n");
}

/// float/double CP 位图案。
ca::u32 f32Bits(ca::f32 v)
{
    ca::u32 b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}
ca::u64 f64Bits(ca::f64 v)
{
    ca::u64 b = 0;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}

// ---------------------------------------------------------------------------
// 断言辅助：折叠产物 = 「单条常量装载 + RETURN」，值经共享探测面读回
// ---------------------------------------------------------------------------

const jbc::ir::Insn* firstInsn(const Inspection& ins)
{
    const std::vector<jbc::ir::InsnId>& order = ins.cu->dom.order();
    if (order.empty()) return nullptr;
    return ins.cu->dom.peek(order.front());
}

/// 首条指令的 int 常量值（ICONST 族/BIPUSH/SIPUSH/LDC(Int)）。
bool firstIntOf(const Inspection& ins, ca::i32& out)
{
    const jbc::ir::Insn* in = firstInsn(ins);
    return in != nullptr && mjbase::pass::method_rewrite_util::intValueOf(*ins.session, *in, out);
}

/// 首条指令的 long 常量值（LCONST 族/LDC2_W(Long)）。
bool firstLongOf(const Inspection& ins, ca::i64& out)
{
    const jbc::ir::Insn* in = firstInsn(ins);
    return in != nullptr && mjbase::pass::method_rewrite_util::longValueOf(*ins.session, *in, out);
}

/// 首条指令的 float 常量位图案（FCONST 族/LDC(Float)；util 无 float 臂）。
bool firstFloatBitsOf(const Inspection& ins, ca::u32& out)
{
    const jbc::ir::Insn* in = firstInsn(ins);
    if (in == nullptr) return false;
    if (in->op == Op::FCONST_0) { out = f32Bits(0.0f); return true; }
    if (in->op == Op::FCONST_1) { out = f32Bits(1.0f); return true; }
    if (in->op == Op::FCONST_2) { out = f32Bits(2.0f); return true; }
    jbc::ir::ClassSession::ConstSem c;
    if (ins.session->constOf(*in, c) && c.kind == jbc::ir::ClassSession::ConstSem::Kind::Float) {
        out = static_cast<ca::u32>(c.bits);
        return true;
    }
    return false;
}

/// 首条指令的 double 常量位图案（DCONST 族/LDC2_W(Double)）。
bool firstDoubleBitsOf(const Inspection& ins, ca::u64& out)
{
    const jbc::ir::Insn* in = firstInsn(ins);
    if (in == nullptr) return false;
    if (in->op == Op::DCONST_0) { out = f64Bits(0.0); return true; }
    if (in->op == Op::DCONST_1) { out = f64Bits(1.0); return true; }
    jbc::ir::ClassSession::ConstSem c;
    if (ins.session->constOf(*in, c) && c.kind == jbc::ir::ClassSession::ConstSem::Kind::Double) {
        out = c.bits;
        return true;
    }
    return false;
}

/// 首条指令的 float 常量值（位图案还原）。
bool firstFloatValueOf(const Inspection& ins, ca::f32& out)
{
    ca::u32 bits = 0;
    if (!firstFloatBitsOf(ins, bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

/// 首条指令的 double 常量值（位图案还原）。
bool firstDoubleValueOf(const Inspection& ins, ca::f64& out)
{
    ca::u64 bits = 0;
    if (!firstDoubleBitsOf(ins, bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

void expectIntFold(const MethodPassRun& r, ca::i32 expected)
{
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], Op::RETURN);
    ca::i32 v = 0;
    ASSERT_TRUE(firstIntOf(ins, v));
    EXPECT_EQ(v, expected);
}

void expectLongFold(const MethodPassRun& r, ca::i64 expected)
{
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], Op::RETURN);
    ca::i64 v = 0;
    ASSERT_TRUE(firstLongOf(ins, v));
    EXPECT_EQ(v, expected);
}

void expectFloatBitsFold(const MethodPassRun& r, ca::u32 expected)
{
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], Op::RETURN);
    ca::u32 v = 0;
    ASSERT_TRUE(firstFloatBitsOf(ins, v));
    EXPECT_EQ(v, expected);
}

void expectDoubleBitsFold(const MethodPassRun& r, ca::u64 expected)
{
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], Op::RETURN);
    ca::u64 v = 0;
    ASSERT_TRUE(firstDoubleBitsOf(ins, v));
    EXPECT_EQ(v, expected);
}

/// NaN 的位图案由硬件定义（x86 SSE 的 real-indefinite 带符号位），只断言
/// 「物化出一个 NaN」——与 JVM 在同硬件的计算结果同类，不钉死载荷。
void expectFloatIsNan(const MethodPassRun& r)
{
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], Op::RETURN);
    ca::f32 v = 0.0f;
    ASSERT_TRUE(firstFloatValueOf(ins, v));
    EXPECT_TRUE(std::isnan(v));
}

void expectDoubleIsNan(const MethodPassRun& r)
{
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[1], Op::RETURN);
    ca::f64 v = 0.0;
    ASSERT_TRUE(firstDoubleValueOf(ins, v));
    EXPECT_TRUE(std::isnan(v));
}

void expectUnchanged(const Bytes& in, const MethodPassRun& r)
{
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

/// 值 → float 装载指令文本（0.0/1.0/2.0 走 FCONST 短形，其余 LDC 字面）。
std::string fconstText(ca::f32 v)
{
    if (v == 0.0f) return "fconst_0";
    if (v == 1.0f) return "fconst_1";
    if (v == 2.0f) return "fconst_2";
    return "ldc " + std::to_string(v) + "f";
}

/// 值 → double 装载指令文本（0.0/1.0 走 DCONST 短形，其余 ldc2_w 字面）。
std::string dconstText(ca::f64 v)
{
    if (v == 0.0) return "dconst_0";
    if (v == 1.0) return "dconst_1";
    return "ldc2_w " + std::to_string(v);
}

}   // namespace

// ── int 运算族 happy path（11 个 op 全覆盖）──
TEST(ConstantFolderTest, intAddFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("bipush 20\nbipush 22\niadd\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 42);
}

TEST(ConstantFolderTest, intSubFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("bipush 50\nbipush 8\nisub\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 42);
}

TEST(ConstantFolderTest, intMulFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("bipush 6\nbipush 7\nimul\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 42);
}

TEST(ConstantFolderTest, intDivFoldsWithTruncation)
{
    // -43 / 10 = -4（向零截断，JLS 15.17.2）。
    const MethodPassRun r = runMethodPass(oneMethod("bipush -43\nbipush 10\nidiv\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, -4);
}

TEST(ConstantFolderTest, intRemFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("bipush 43\nbipush 10\nirem\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 3);
}

TEST(ConstantFolderTest, intAndFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("bipush 12\nbipush 10\niand\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 8);
}

TEST(ConstantFolderTest, intOrFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("bipush 12\nbipush 10\nior\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 14);
}

TEST(ConstantFolderTest, intXorFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("bipush 12\nbipush 10\nixor\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 6);
}

TEST(ConstantFolderTest, intShlFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("bipush 1\nbipush 4\nishl\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 16);
}

TEST(ConstantFolderTest, intShrNegativeFolds)
{
    // -16 >> 2 = -4（算术右移，符号扩展）。
    const MethodPassRun r = runMethodPass(oneMethod("bipush -16\nbipush 2\nishr\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, -4);
}

TEST(ConstantFolderTest, intUshrNegativeFolds)
{
    // -16 >>> 2 = 0x3FFFFFFC = 1073741820（逻辑右移，零扩展）。
    const MethodPassRun r = runMethodPass(oneMethod("bipush -16\nbipush 2\niushr\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 1073741820);
}

// ── 移位掩码：int 移 5 位（JVMS §6.5 ishl）──
TEST(ConstantFolderTest, intShiftMaskedToFiveBits)
{
    // 1 << 33 == 1 << (33 & 31) == 2。
    const MethodPassRun r = runMethodPass(oneMethod("bipush 1\nbipush 33\nishl\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 2);
}

// ── 回绕边界：INT_MAX + 1 → INT_MIN（u32 回绕后按位读回）──
TEST(ConstantFolderTest, intAddWrapsAtIntMax)
{
    const MethodPassRun r =
        runMethodPass(oneMethod("ldc 2147483647\niconst_1\niadd\nreturn\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, (std::numeric_limits<ca::i32>::min)());
}

// ── 物化分级：结果 1023 落 SIPUSH 形态 ──
TEST(ConstantFolderTest, intLdcInputFoldsToSipush)
{
    const MethodPassRun r = runMethodPass(oneMethod("ldc 1000\nldc 23\niadd\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(opsOf(ins).front(), Op::SIPUSH);
    expectIntFold(r, 1023);
}

// ── 物化分级：结果 -1 落 ICONST_M1 ──
TEST(ConstantFolderTest, intRemNegativeFoldsToIconstM1)
{
    // -7 % 3 = -1（余数符号跟随被除数，JLS 15.17.3）。
    const MethodPassRun r = runMethodPass(oneMethod("bipush -7\nbipush 3\nirem\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, -1);
}

// ── 负操作数矩阵：irem / ishr 符号 ──
TEST(ConstantFolderTest, negativeRemSignMatrix)
{
    struct Case
    {
        ca::i32 a;
        ca::i32 b;
        ca::i32 want;
    };
    const Case cases[] = {
        {-7, 3, -1},   // 负被除数 → 负余数
        {7, -3, 1},    // 负除数 → 余数跟随被除数（正）
        {-7, -3, -1},  // 双负 → 负
        {-43, 10, -3}, // 截断取模
    };
    for (const Case& c : cases) {
        const MethodPassRun r =
            runMethodPass(oneMethod("ldc " + std::to_string(c.a) + "\nldc "
                                        + std::to_string(c.b) + "\nirem\nreturn\n"),
                          mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        expectIntFold(r, c.want);
    }
}

TEST(ConstantFolderTest, negativeShrSignMatrix)
{
    struct Case
    {
        ca::i32 v;
        ca::i32 d;
        ca::i32 want;
    };
    const Case cases[] = {
        {-1, 1, -1},           // 全 1 负值算术右移仍为 -1
        {-16, 2, -4},
        {-256, 4, -16},
        {-1, 31, -1},          // 掩码内最大位移
    };
    for (const Case& c : cases) {
        const MethodPassRun r =
            runMethodPass(oneMethod("ldc " + std::to_string(c.v) + "\nldc "
                                        + std::to_string(c.d) + "\nishr\nreturn\n"),
                          mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        expectIntFold(r, c.want);
    }
}

// ── 回绕边界：LONG_MIN - 1 → LONG_MAX ──
TEST(ConstantFolderTest, longSubWrapsAtLongMin)
{
    const MethodPassRun r =
        runMethodPass(oneMethod("ldc2_w -9223372036854775808L\nlconst_1\nlsub\nreturn\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectLongFold(r, (std::numeric_limits<ca::i64>::max)());
}

// ── 红线：INT_MIN / -1 的 idiv/irem 保留不折 ──
TEST(ConstantFolderTest, intDivMinMinusOneNotFolded)
{
    const Bytes in = oneMethod("ldc -2147483648\niconst_m1\nidiv\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

TEST(ConstantFolderTest, intRemMinMinusOneNotFolded)
{
    const Bytes in = oneMethod("ldc -2147483648\niconst_m1\nirem\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 红线：除数 0 保留 ArithmeticException（不折）──
TEST(ConstantFolderTest, intDivZeroNotFolded)
{
    const Bytes in = oneMethod("bipush 1\niconst_0\nidiv\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

TEST(ConstantFolderTest, intRemZeroNotFolded)
{
    const Bytes in = oneMethod("bipush 1\niconst_0\nirem\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 链长 1（无运算可消费）：不折 ──
TEST(ConstantFolderTest, singlePushNotFolded)
{
    const Bytes in = oneMethod("bipush 5\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

TEST(ConstantFolderTest, purePushSequenceNotFolded)
{
    // 两条装载无运算：虚拟栈深 2 回不到 1 → 不折。装载留栈在 return——
    // 活代码帧面拒收，按死段表达（pass 扫描面照见，语义同实）。
    const Bytes in = deadBody("bipush 5\nbipush 6\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 常量 + 变量混合：运算消费链外值 → 保守停 ──
TEST(ConstantFolderTest, mixedWithVariableNotFolded)
{
    const Bytes in = oneMethod("bipush 1\niload 0\niadd\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 类型混用：int 链遇 long 装载 → 停（该字节码本身是验证错误，pass 不得
//    折叠）——int 链折半遇 cat2 装载活代码帧面拒收，按死段表达 ──
TEST(ConstantFolderTest, typeMixIntLongNotFolded)
{
    const Bytes in = deadBody("bipush 1\nlconst_1\nladd\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── int 链遇 long 移位（操作数形状 (long,int) 与 (int,int) 不符）→ 停 ──
TEST(ConstantFolderTest, intChainLongShiftNotFolded)
{
    const Bytes in = deadBody("iconst_1\niconst_2\nlshl\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 一元 op（INEG）不在支持集：链止于其前，无折叠点 → 不折 ──
TEST(ConstantFolderTest, unaryOpNotFolded)
{
    const Bytes in = oneMethod("bipush 1\nbipush 2\nineg\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 运算消费链外值（栈深 1 处的 IADD）→ 不折（活代码帧面拒收，死段表达）──
TEST(ConstantFolderTest, opUnderflowsVirtualStackNotFolded)
{
    const Bytes in = deadBody("bipush 1\niadd\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 极大链：贪心消费到最后一个深度 1 点 ──
TEST(ConstantFolderTest, maximalChainFolds)
{
    // ((1 + 2) + 3) * 4 = 24。
    const MethodPassRun r =
        runMethodPass(oneMethod("bipush 1\nbipush 2\niadd\nbipush 3\niadd\nbipush 4\nimul\nreturn\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 24);
}

// ── 链尾后续装载：折叠点之后的 push 原样保留 ──
TEST(ConstantFolderTest, trailingPushAfterFoldKept)
{
    // LDC 1; LDC 2; IADD; LDC 3 → ICONST_3; LDC 3。
    // 产物 3 ∈ [-1,5] 落 ICONST_3 短形态（intConstInsn 分级，InsnUtil）；
    // 链尾 push 不经折叠，原样保留（装配器窄常量规范化落 LDC 3）。
    const MethodPassRun r =
        runMethodPass(oneMethod("bipush 1\nbipush 2\niadd\nbipush 3\nreturn\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ICONST_3);
    EXPECT_EQ(ops[1], Op::LDC);
    EXPECT_EQ(ops[2], Op::RETURN);
    const jbc::ir::Insn* first = firstInsn(ins);
    ASSERT_NE(first, nullptr);
    ca::i32 v = 0;
    ASSERT_TRUE(firstIntOf(ins, v));
    EXPECT_EQ(v, 3);
}

// ── 非数值装载（LDC String）不起始链，但其后的常量链照常折叠 ──
TEST(ConstantFolderTest, stringPushDoesNotStartChain)
{
    const MethodPassRun r =
        runMethodPass(oneMethod("ldc \"x\"\nbipush 1\nbipush 2\niadd\nreturn\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::LDC);
    EXPECT_EQ(ops[1], Op::ICONST_3);   // 产物 3 短形态分级
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── 边界守卫：分支跳入链中段 → 整链不动 ──
TEST(ConstantFolderTest, branchIntoChainMiddleBlocksFold)
{
    // LDC 1 / LDC 2 / Lmid: IADD / RETURN / GOTO→Lmid（锚定链中段成员；
    // goto 居 return 后死段，装配器放行）。
    const Bytes in = oneMethod("bipush 1\nbipush 2\nLmid:\niadd\nreturn\ngoto Lmid\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 首成员标签：分支跳入链头 → 折叠 + 标签迁移到替换头 ──
TEST(ConstantFolderTest, branchToChainHeadFoldsWithLabelMigration)
{
    // GOTO→链头：折叠后 GOTO 随标签落到替换头（goto 居尾，死段回边）。
    const MethodPassRun r =
        runMethodPass(oneMethod("Lh:\nbipush 1\nbipush 2\niadd\nreturn\ngoto Lh\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ICONST_3);   // 产物 3 短形态分级
    EXPECT_EQ(ops[1], Op::RETURN);
    EXPECT_EQ(ops[2], Op::GOTO);
    const jbc::ir::Insn* first = firstInsn(ins);
    ASSERT_NE(first, nullptr);
    ca::i32 v = 0;
    ASSERT_TRUE(firstIntOf(ins, v));
    EXPECT_EQ(v, 3);
}

// ── 异常表：start 边界在链头 → 折叠 + start 标签迁移 ──
TEST(ConstantFolderTest, excStartAtChainHeadFolds)
{
    // LDC 1 / LDC 2 / IADD / POP / RETURN；try 覆盖链头起（仅链），handler
    // 落 RETURN（jasm handler 须绑定实体指令，悬空面由 end 悬空语义承消）。
    const Bytes in = assembleJasm(R"JASM(
.super java/lang/Object
.class public super t/R {
  .method public static m ()V {
    exceptions: {
      { L0, Lend, Lr, * }
    },
    code: {
      L0:
      bipush 1
      bipush 2
      iadd
      pop
      Lr:
      return
    }
  }
}
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ICONST_3);   // 产物 3 短形态分级
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
    // start 标签迁移到替换头（异常区间起点语义守恒）。
    ASSERT_EQ(ins.cu->exc.size(), 1u);
    const auto start = ins.cu->dom.resolveLabel(ins.cu->exc[0].start);
    ASSERT_TRUE(start.is_ok());
    ASSERT_FALSE(start.unwrap().isEnd());
    EXPECT_EQ(start.unwrap().insn, ins.cu->dom.order().front());
}

// ── 异常表：start 边界在链中段 → 守卫拒绝 ──
TEST(ConstantFolderTest, excStartInChainMiddleBlocksFold)
{
    const Bytes in = assembleJasm(R"JASM(
.super java/lang/Object
.class public super t/R {
  .method public static m ()V {
    exceptions: {
      { Lmid, Lend, Lr, * }
    },
    code: {
      bipush 1
      Lmid:
      bipush 2
      iadd
      pop
      Lr:
      return
    }
  }
}
)JASM");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 幂等：二次运行 no-op ──
TEST(ConstantFolderTest, secondRunIsNoOp)
{
    const MethodPassRun first =
        runMethodPass(oneMethod("bipush 1\nbipush 2\niadd\nreturn\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(first.pa.changed());
    const MethodPassRun second =
        runMethodPass(first.out, mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(second.pa.areAllPreserved());
    EXPECT_EQ(second.out, first.out);
}

// ── maxStack 不升（折叠只降不升；管线未注入 resolver 时不重算）──
TEST(ConstantFolderTest, maxStackNotRaised)
{
    const MethodPassRun r =
        runMethodPass(oneMethod("bipush 1\nbipush 2\niadd\nreturn\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    EXPECT_EQ(maxStackOf(inspect(r.out)), 2);
}

// ── long 运算族 happy path（LADD/LSUB/LMUL/LDIV/LREM/LAND/LOR/LXOR）──
TEST(ConstantFolderTest, longOpFamilyFolds)
{
    struct Case
    {
        Op          op;
        const char* opText;
        ca::i64     a;
        ca::i64     b;
        ca::i64     want;
        const char* name;
    };
    const Case cases[] = {
        {Op::LADD, "ladd", 1, 1, 2, "ladd"},
        {Op::LSUB, "lsub", 1, 0, 1, "lsub"},
        {Op::LMUL, "lmul", 6, 7, 42, "lmul"},
        {Op::LDIV, "ldiv", -7, 3, -2, "ldiv-truncation"},
        {Op::LREM, "lrem", -7, 3, -1, "lrem-sign"},
        {Op::LAND, "land", 12, 10, 8, "land"},
        {Op::LOR, "lor", 12, 10, 14, "lor"},
        {Op::LXOR, "lxor", 12, 10, 6, "lxor"},
    };
    for (const Case& c : cases) {
        const MethodPassRun r =
            runMethodPass(oneMethod("ldc2_w " + std::to_string(c.a) + "L\nldc2_w "
                                        + std::to_string(c.b) + "L\n" + c.opText + "\nreturn\n"),
                          mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed()) << c.name;
        expectLongFold(r, c.want);
    }
}

// ── long 移位族：(long 常量, int 常量, LSHL/LSHR/LUSHR) 成对消费 ──
TEST(ConstantFolderTest, longShiftFamilyFolds)
{
    struct Case
    {
        Op      op;
        const char* opText;
        ca::i64 v;
        ca::i32 d;
        ca::i64 want;
    };
    const Case cases[] = {
        {Op::LSHL, "lshl", 1, 3, 8},
        {Op::LSHR, "lshr", -8, 1, -4},
        {Op::LUSHR, "lushr", -8, 1, 0x7FFFFFFFFFFFFFFCLL},
        {Op::LSHL, "lshl", 1, 65, 2},        // 掩 6 位：65 & 63 == 1
    };
    for (const Case& c : cases) {
        const MethodPassRun r =
            runMethodPass(oneMethod("ldc2_w " + std::to_string(c.v) + "L\nldc "
                                        + std::to_string(c.d) + "\n" + c.opText + "\nreturn\n"),
                          mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        expectLongFold(r, c.want);
    }
}

// ── long 移位链：多级位移连续折叠 ──
TEST(ConstantFolderTest, longShiftChainFolds)
{
    // (1 << 3) << 1 == 16。
    const MethodPassRun r =
        runMethodPass(oneMethod("lconst_1\nbipush 3\nlshl\nbipush 1\nlshl\nreturn\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectLongFold(r, 16);
}

// ── long 移位 int 操作数非常量：停 ──
TEST(ConstantFolderTest, longShiftNonConstDistanceStops)
{
    const Bytes in = oneMethod("lconst_1\niload 0\nlshl\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── long 移位 int 操作数是装载但后随非移位 op：停（装载留栈到 return，
//    活代码帧面拒收，按死段表达）──
TEST(ConstantFolderTest, longChainIntPushNotFollowedByShiftStops)
{
    const Bytes in = deadBody("lconst_1\nbipush 2\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── long 红线：除 0 与 MIN/-1 保留不折 ──
TEST(ConstantFolderTest, longDivZeroNotFolded)
{
    const Bytes in = oneMethod("ldc2_w 1L\nlconst_0\nldiv\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

TEST(ConstantFolderTest, longDivMinMinusOneNotFolded)
{
    const Bytes in =
        oneMethod("ldc2_w -9223372036854775808L\nldc2_w -1L\nldiv\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── long 回绕：LONG_MAX + 1 → LONG_MIN ──
TEST(ConstantFolderTest, longAddWrapsAtLongMax)
{
    const MethodPassRun r =
        runMethodPass(oneMethod("ldc2_w 9223372036854775807L\nlconst_1\nladd\nreturn\n"),
                      mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectLongFold(r, (std::numeric_limits<ca::i64>::min)());
}

// ── float 运算族 happy path（含 NaN/Inf 物化）──
TEST(ConstantFolderTest, floatOpFamilyFolds)
{
    const ca::f32 inf = std::numeric_limits<ca::f32>::infinity();
    struct Case
    {
        Op      op;
        const char* opText;
        ca::f32 a;
        ca::f32 b;
        ca::u32 want;
    };
    const Case cases[] = {
        {Op::FADD, "fadd", 1.0f, 2.0f, f32Bits(3.0f)},
        {Op::FSUB, "fsub", 2.0f, 1.0f, f32Bits(1.0f)},
        {Op::FMUL, "fmul", 2.0f, 2.0f, f32Bits(4.0f)},
        {Op::FDIV, "fdiv", 2.0f, 1.0f, f32Bits(2.0f)},
        {Op::FDIV, "fdiv", 1.0f, 0.0f, f32Bits(inf)},    // IEEE：除 0 不抛异常
        {Op::FSUB, "fsub", 1.0f, 2.0f, f32Bits(-1.0f)},
    };
    for (const Case& c : cases) {
        const MethodPassRun r =
            runMethodPass(oneMethod(std::string(fconstText(c.a)) + "\n"
                                        + fconstText(c.b) + "\n" + c.opText + "\nreturn\n"),
                          mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        expectFloatBitsFold(r, c.want);
    }
}

// ── float 0/0 → NaN 物化（位图案不钉死，见 expectFloatIsNan 注）──
TEST(ConstantFolderTest, floatZeroDivZeroFoldsToNan)
{
    const MethodPassRun r = runMethodPass(oneMethod("fconst_0\nfconst_0\nfdiv\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectFloatIsNan(r);
}

// ── float 物化：+0.0 → FCONST_0；-0.0 → LDC（signbit 检查）──
TEST(ConstantFolderTest, floatMaterializesZeroSigns)
{
    // +0.0：FCONST_0 - FCONST_0 → FCONST_0。
    {
        const MethodPassRun r =
            runMethodPass(oneMethod("fconst_0\nfconst_0\nfsub\nreturn\n"),
                          mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        EXPECT_EQ(opsOf(ins).front(), Op::FCONST_0);
        expectFloatBitsFold(r, f32Bits(0.0f));
    }
    // -0.0：LDC(-1.0) * FCONST_0 → LDC(-0.0)（0x80000000，不走 FCONST_0）。
    {
        const MethodPassRun r =
            runMethodPass(oneMethod("ldc -1.0f\nfconst_0\nfmul\nreturn\n"),
                          mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        EXPECT_EQ(opsOf(ins).front(), Op::LDC);
        expectFloatBitsFold(r, 0x80000000u);
    }
}

// ── float LDC 输入（util 探测面外的 ldc 族 Float）──
TEST(ConstantFolderTest, floatLdcInputFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("ldc 2.5f\nldc 4.0f\nfadd\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectFloatBitsFold(r, f32Bits(6.5f));
}

// ── double 运算族 happy path ──
TEST(ConstantFolderTest, doubleOpFamilyFolds)
{
    const ca::f64 inf = std::numeric_limits<ca::f64>::infinity();
    struct Case
    {
        Op      op;
        const char* opText;
        ca::f64 a;
        ca::f64 b;
        ca::u64 want;
    };
    const Case cases[] = {
        {Op::DADD, "dadd", 1.0, 1.0, f64Bits(2.0)},
        {Op::DSUB, "dsub", 0.0, 1.0, f64Bits(-1.0)},
        {Op::DMUL, "dmul", 1.0, 1.0, f64Bits(1.0)},
        {Op::DDIV, "ddiv", 1.0, 1.0, f64Bits(1.0)},
        {Op::DDIV, "ddiv", 1.0, 0.0, f64Bits(inf)},
    };
    for (const Case& c : cases) {
        const MethodPassRun r =
            runMethodPass(oneMethod(std::string(dconstText(c.a)) + "\n"
                                        + dconstText(c.b) + "\n" + c.opText + "\nreturn\n"),
                          mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        expectDoubleBitsFold(r, c.want);
    }
}

// ── double 0/0 → NaN 物化 ──
TEST(ConstantFolderTest, doubleZeroDivZeroFoldsToNan)
{
    const MethodPassRun r = runMethodPass(oneMethod("dconst_0\ndconst_0\nddiv\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectDoubleIsNan(r);
}

// ── double LDC2_W 输入与输出（CP 双槽追加路径）──
TEST(ConstantFolderTest, doubleLdc2wInputFolds)
{
    const MethodPassRun r = runMethodPass(oneMethod("ldc2_w 1.5\nldc2_w 2.5\ndmul\nreturn\n"),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectDoubleBitsFold(r, f64Bits(3.75));
}

// ── double 链长 1：不折 ──
TEST(ConstantFolderTest, doubleSinglePushNotFolded)
{
    const Bytes in = oneMethod("dconst_1\nreturn\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── float/double 类型混用：停（cat1+cat2 欠额活代码帧面拒收，死段表达）──
TEST(ConstantFolderTest, floatDoubleMixNotFolded)
{
    const Bytes in = deadBody("fconst_1\ndconst_1\ndadd\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 空方法体（仅 RETURN）：不折 ──
TEST(ConstantFolderTest, emptyBodyUntouched)
{
    const Bytes in = oneMethod("return\n");
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}
