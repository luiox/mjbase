// ConstantFolder：操作数栈常量链折叠。op 族全覆盖 + 回绕/除零/MIN/-1 红线 +
// 负操作数矩阵 + 链识别边界（链长 1 / 类型混用 / 标签守卫）+ 幂等 + PA 纪律。
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

#include "mjbase/pass/constant_folder.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

// ---------------------------------------------------------------------------
// 夹具辅助
// ---------------------------------------------------------------------------

/// 单方法容器（其余字段缺省；maxStack 由链深决定，用例按需给）。
Bytes oneMethod(Bytes code, ca::u16 maxStack = 2, ca::u16 maxLocals = 1)
{
    FixtureBuilder b("t/R");
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, maxStack, maxLocals});
    return b.assemble();
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

/// @brief FixtureBuilder 的 float/double 补充面：共享夹具只到 int/long/
///        string（structural_pass_util.hpp 头注明示「仅用单槽 CP 条目」），
///        float(tag4)/double(tag6 双槽) 条目在本文件本地组装。仅支持本文件
///        用例所需的最小面：单方法 + float/double 常量条目。
class FloatDoubleFixture
{
public:
    explicit FloatDoubleFixture(std::string_view thisClass)
    {
        thisIdx_  = cls(thisClass);
        superIdx_ = cls("java/lang/Object");
        codeName_ = utf8("Code");
    }

    ca::u16 floatConst(ca::f32 v)
    {
        const std::string key = "f:" + std::to_string(f32Bits(v));
        const auto        it  = floats_.find(key);
        if (it != floats_.end()) return it->second;
        putU8(cp_, 0x04);
        putU32(cp_, f32Bits(v));
        const ca::u16 idx = next_++;
        floats_.emplace(key, idx);
        return idx;
    }

    ca::u16 doubleConst(ca::f64 v)
    {
        const std::string key = "d:" + std::to_string(f64Bits(v));
        const auto        it  = doubles_.find(key);
        if (it != doubles_.end()) return it->second;
        putU8(cp_, 0x06);
        putU32(cp_, static_cast<ca::u32>(f64Bits(v) >> 32));
        putU32(cp_, static_cast<ca::u32>(f64Bits(v)));
        const ca::u16 idx = next_;
        next_ += 2;   // 双槽条目：下一 CP 下标跳过 placeholder
        doubles_.emplace(key, idx);
        return idx;
    }

    void addMethod(Bytes code, ca::u16 maxStack = 2, ca::u16 maxLocals = 1)
    {
        methods_.push_back({"m", "()V", std::move(code), 0x0009, {}, maxStack, maxLocals});
    }

    Bytes assemble()
    {
        const NameDesc method{utf8("m"), utf8("()V")};
        Bytes          out;
        putU32(out, 0xCAFEBABEu);
        putU16(out, 0x00);
        putU16(out, 0x34);
        putU16(out, next_);
        out.insert(out.end(), cp_.begin(), cp_.end());
        putU16(out, 0x0021);
        putU16(out, thisIdx_);
        putU16(out, superIdx_);
        putU16(out, 0);   // interfaces
        putU16(out, 0);   // fields
        putU16(out, 1);   // methods
        putU16(out, 0x0009);
        putU16(out, method.name);
        putU16(out, method.desc);
        Bytes attr;
        putU16(attr, methods_[0].maxStack);
        putU16(attr, methods_[0].maxLocals);
        putU32(attr, static_cast<ca::u32>(methods_[0].code.size()));
        attr.insert(attr.end(), methods_[0].code.begin(), methods_[0].code.end());
        putU16(attr, 0);   // exc
        putU16(attr, 0);   // Code 嵌套属性
        putU16(out, 1);
        putU16(out, codeName_);
        putU32(out, static_cast<ca::u32>(attr.size()));
        out.insert(out.end(), attr.begin(), attr.end());
        putU16(out, 0);   // class attrs
        return out;
    }

private:
    struct NameDesc
    {
        ca::u16 name;
        ca::u16 desc;
    };

    ca::u16 utf8(std::string_view s)
    {
        const std::string key(s);
        const auto        it = utf8s_.find(key);
        if (it != utf8s_.end()) return it->second;
        putU8(cp_, 0x01);
        putU16(cp_, static_cast<ca::u16>(s.size()));
        putRaw(cp_, s);
        const ca::u16 idx = next_++;
        utf8s_.emplace(key, idx);
        return idx;
    }

    ca::u16 cls(std::string_view n)
    {
        const std::string key(n);
        const auto        it = classes_.find(key);
        if (it != classes_.end()) return it->second;
        const ca::u16 u = utf8(n);
        putU8(cp_, 0x07);
        putU16(cp_, u);
        const ca::u16 idx = next_++;
        classes_.emplace(key, idx);
        return idx;
    }

    Bytes                                    cp_;
    ca::u16                                  next_ = 1;
    std::unordered_map<std::string, ca::u16> utf8s_;
    std::unordered_map<std::string, ca::u16> classes_;
    std::unordered_map<std::string, ca::u16> floats_;
    std::unordered_map<std::string, ca::u16> doubles_;
    ca::u16                                  thisIdx_  = 0;
    ca::u16                                  superIdx_ = 0;
    ca::u16                                  codeName_ = 0;
    std::vector<FixtureMethod>               methods_;
};

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

// 常用立即数助记（字节值即 JVMS 操作码；读取处一律走 Op 枚举）。
constexpr ca::u8 kBipush = 0x10;
constexpr ca::u8 kSipush = 0x11;
constexpr ca::u8 kLdc    = 0x12;
constexpr ca::u8 kLdc2W  = 0x14;
constexpr ca::u8 kPop    = 0x57;
constexpr ca::u8 kReturn = 0xb1;
constexpr ca::u8 kGoto   = 0xa7;

/// 值 → FCONST 形态（用例只用 0.0/1.0/2.0 三档）。
Op fconstOp(ca::f32 v)
{
    return v == 0.0f ? Op::FCONST_0 : v == 1.0f ? Op::FCONST_1 : Op::FCONST_2;
}

/// 值 → DCONST 形态（用例只用 0.0/1.0 两档）。
Op dconstOp(ca::f64 v)
{
    return v == 0.0 ? Op::DCONST_0 : Op::DCONST_1;
}

/// bipush v（v ∈ [-128,127]）。
void bipush(Bytes& b, ca::i32 v)
{
    b.push_back(kBipush);
    b.push_back(static_cast<ca::u8>(v));
}

/// sipush v（v ∈ [-32768,32767]）。
void sipush(Bytes& b, ca::i32 v)
{
    b.push_back(kSipush);
    b.push_back(static_cast<ca::u8>(static_cast<ca::u16>(v) >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

/// ldc idx（u1 操作数；idx ≤ 0xFF）。
void ldc(Bytes& b, ca::u16 idx)
{
    b.push_back(kLdc);
    b.push_back(static_cast<ca::u8>(idx));
}

/// ldc2_w idx（u2 操作数；long/double）。
void ldc2w(Bytes& b, ca::u16 idx)
{
    b.push_back(kLdc2W);
    b.push_back(static_cast<ca::u8>(idx >> 8));
    b.push_back(static_cast<ca::u8>(idx));
}

/// goto offset（2 字节有符号偏移）。
void goto16(Bytes& b, ca::i32 offset)
{
    b.push_back(kGoto);
    b.push_back(static_cast<ca::u8>(static_cast<ca::u16>(offset) >> 8));
    b.push_back(static_cast<ca::u8>(offset));
}

}   // namespace

// ── int 运算族 happy path（11 个 op 全覆盖）──
TEST(ConstantFolderTest, intAddFolds)
{
    Bytes code;
    bipush(code, 20);
    bipush(code, 22);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 42);
}

TEST(ConstantFolderTest, intSubFolds)
{
    Bytes code;
    bipush(code, 50);
    bipush(code, 8);
    code.push_back(static_cast<ca::u8>(Op::ISUB));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 42);
}

TEST(ConstantFolderTest, intMulFolds)
{
    Bytes code;
    bipush(code, 6);
    bipush(code, 7);
    code.push_back(static_cast<ca::u8>(Op::IMUL));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 42);
}

TEST(ConstantFolderTest, intDivFoldsWithTruncation)
{
    // -43 / 10 = -4（向零截断，JLS 15.17.2）。
    Bytes code;
    bipush(code, -43);
    bipush(code, 10);
    code.push_back(static_cast<ca::u8>(Op::IDIV));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, -4);
}

TEST(ConstantFolderTest, intRemFolds)
{
    Bytes code;
    bipush(code, 43);
    bipush(code, 10);
    code.push_back(static_cast<ca::u8>(Op::IREM));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 3);
}

TEST(ConstantFolderTest, intAndFolds)
{
    Bytes code;
    bipush(code, 12);
    bipush(code, 10);
    code.push_back(static_cast<ca::u8>(Op::IAND));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 8);
}

TEST(ConstantFolderTest, intOrFolds)
{
    Bytes code;
    bipush(code, 12);
    bipush(code, 10);
    code.push_back(static_cast<ca::u8>(Op::IOR));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 14);
}

TEST(ConstantFolderTest, intXorFolds)
{
    Bytes code;
    bipush(code, 12);
    bipush(code, 10);
    code.push_back(static_cast<ca::u8>(Op::IXOR));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 6);
}

TEST(ConstantFolderTest, intShlFolds)
{
    Bytes code;
    bipush(code, 1);
    bipush(code, 4);
    code.push_back(static_cast<ca::u8>(Op::ISHL));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 16);
}

TEST(ConstantFolderTest, intShrNegativeFolds)
{
    // -16 >> 2 = -4（算术右移，符号扩展）。
    Bytes code;
    bipush(code, -16);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::ISHR));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, -4);
}

TEST(ConstantFolderTest, intUshrNegativeFolds)
{
    // -16 >>> 2 = 0x3FFFFFFC = 1073741820（逻辑右移，零扩展）。
    Bytes code;
    bipush(code, -16);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IUSHR));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 1073741820);
}

// ── 移位掩码：int 移 5 位（JVMS §6.5 ishl）──
TEST(ConstantFolderTest, intShiftMaskedToFiveBits)
{
    // 1 << 33 == 1 << (33 & 31) == 2。
    Bytes code;
    bipush(code, 1);
    bipush(code, 33);
    code.push_back(static_cast<ca::u8>(Op::ISHL));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 2);
}

// ── 回绕边界：INT_MAX + 1 → INT_MIN（u32 回绕后按位读回）──
TEST(ConstantFolderTest, intAddWrapsAtIntMax)
{
    FixtureBuilder b("t/R");
    const ca::u16  max = b.intConst((std::numeric_limits<ca::i32>::max)());
    Bytes          code;
    ldc(code, max);
    code.push_back(static_cast<ca::u8>(Op::ICONST_1));
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 2, 1});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, (std::numeric_limits<ca::i32>::min)());
}

// ── 物化分级：结果 1023 落 SIPUSH 形态 ──
TEST(ConstantFolderTest, intLdcInputFoldsToSipush)
{
    FixtureBuilder b("t/R");
    const ca::u16  a = b.intConst(1000);
    const ca::u16  c = b.intConst(23);
    Bytes          code;
    ldc(code, a);
    ldc(code, c);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 2, 1});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(opsOf(ins).front(), Op::SIPUSH);
    expectIntFold(r, 1023);
}

// ── 物化分级：结果 -1 落 ICONST_M1 ──
TEST(ConstantFolderTest, intRemNegativeFoldsToIconstM1)
{
    // -7 % 3 = -1（余数符号跟随被除数，JLS 15.17.3）。
    Bytes code;
    bipush(code, -7);
    bipush(code, 3);
    code.push_back(static_cast<ca::u8>(Op::IREM));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
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
        Bytes code;
        bipush(code, c.a);
        bipush(code, c.b);
        code.push_back(static_cast<ca::u8>(Op::IREM));
        code.push_back(kReturn);
        const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
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
        Bytes code;
        // -256 超出 bipush 值域 → SIPUSH（截断会静默变 0）。
        if (c.v >= -128 && c.v <= 127) {
            bipush(code, c.v);
        }
        else {
            sipush(code, c.v);
        }
        bipush(code, c.d);
        code.push_back(static_cast<ca::u8>(Op::ISHR));
        code.push_back(kReturn);
        const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                              mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        expectIntFold(r, c.want);
    }
}

// ── 回绕边界：LONG_MIN - 1 → LONG_MAX ──
TEST(ConstantFolderTest, longSubWrapsAtLongMin)
{
    FixtureBuilder b("t/R");
    const ca::u16  min = b.longConst((std::numeric_limits<ca::i64>::min)());
    Bytes          code;
    ldc2w(code, min);
    code.push_back(static_cast<ca::u8>(Op::LCONST_1));
    code.push_back(static_cast<ca::u8>(Op::LSUB));
    code.push_back(kReturn);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 3, 1});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectLongFold(r, (std::numeric_limits<ca::i64>::max)());
}

// ── 红线：INT_MIN / -1 的 idiv/irem 保留不折 ──
TEST(ConstantFolderTest, intDivMinMinusOneNotFolded)
{
    FixtureBuilder b("t/R");
    const ca::u16  min = b.intConst((std::numeric_limits<ca::i32>::min)());
    Bytes          code;
    ldc(code, min);
    code.push_back(static_cast<ca::u8>(Op::ICONST_M1));
    code.push_back(static_cast<ca::u8>(Op::IDIV));
    code.push_back(kReturn);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 2, 1});
    const Bytes in = b.assemble();
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

TEST(ConstantFolderTest, intRemMinMinusOneNotFolded)
{
    FixtureBuilder b("t/R");
    const ca::u16  min = b.intConst((std::numeric_limits<ca::i32>::min)());
    Bytes          code;
    ldc(code, min);
    code.push_back(static_cast<ca::u8>(Op::ICONST_M1));
    code.push_back(static_cast<ca::u8>(Op::IREM));
    code.push_back(kReturn);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 2, 1});
    const Bytes in = b.assemble();
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 红线：除数 0 保留 ArithmeticException（不折）──
TEST(ConstantFolderTest, intDivZeroNotFolded)
{
    Bytes code;
    bipush(code, 1);
    code.push_back(static_cast<ca::u8>(Op::ICONST_0));
    code.push_back(static_cast<ca::u8>(Op::IDIV));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code));
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

TEST(ConstantFolderTest, intRemZeroNotFolded)
{
    Bytes code;
    bipush(code, 1);
    code.push_back(static_cast<ca::u8>(Op::ICONST_0));
    code.push_back(static_cast<ca::u8>(Op::IREM));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code));
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 链长 1（无运算可消费）：不折 ──
TEST(ConstantFolderTest, singlePushNotFolded)
{
    Bytes code;
    bipush(code, 5);
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code));
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

TEST(ConstantFolderTest, purePushSequenceNotFolded)
{
    // 两条装载无运算：虚拟栈深 2 回不到 1 → 不折。
    Bytes code;
    bipush(code, 5);
    bipush(code, 6);
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code));
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 常量 + 变量混合：运算消费链外值 → 保守停 ──
TEST(ConstantFolderTest, mixedWithVariableNotFolded)
{
    Bytes code;
    bipush(code, 1);
    code.push_back(static_cast<ca::u8>(Op::ILOAD_0));
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code));
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 类型混用：int 链遇 long 装载 → 停（该字节码本身是验证错误，pass 不得折叠）──
TEST(ConstantFolderTest, typeMixIntLongNotFolded)
{
    Bytes code;
    bipush(code, 1);
    code.push_back(static_cast<ca::u8>(Op::LCONST_1));
    code.push_back(static_cast<ca::u8>(Op::LADD));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code), 4);
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── int 链遇 long 移位（操作数形状 (long,int) 与 (int,int) 不符）→ 停 ──
TEST(ConstantFolderTest, intChainLongShiftNotFolded)
{
    Bytes code;
    code.push_back(static_cast<ca::u8>(Op::ICONST_1));
    code.push_back(static_cast<ca::u8>(Op::ICONST_2));
    code.push_back(static_cast<ca::u8>(Op::LSHL));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code));
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 一元 op（INEG）不在支持集：链止于其前，无折叠点 → 不折 ──
TEST(ConstantFolderTest, unaryOpNotFolded)
{
    Bytes code;
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::INEG));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code));
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 运算消费链外值（栈深 1 处的 IADD）→ 不折 ──
TEST(ConstantFolderTest, opUnderflowsVirtualStackNotFolded)
{
    Bytes code;
    bipush(code, 1);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code));
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 极大链：贪心消费到最后一个深度 1 点 ──
TEST(ConstantFolderTest, maximalChainFolds)
{
    // ((1 + 2) + 3) * 4 = 24。
    Bytes code;
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    bipush(code, 3);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    bipush(code, 4);
    code.push_back(static_cast<ca::u8>(Op::IMUL));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code), 3),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectIntFold(r, 24);
}

// ── 链尾后续装载：折叠点之后的 push 原样保留 ──
TEST(ConstantFolderTest, trailingPushAfterFoldKept)
{
    // BIPUSH 1; BIPUSH 2; IADD; BIPUSH 3 → ICONST_3; BIPUSH 3。
    // 产物 3 ∈ [-1,5] 落 ICONST_3 短形态（intConstInsn 分级）；链尾 push 不经折叠，原样保留 BIPUSH 3。
    Bytes code;
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    bipush(code, 3);
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ICONST_3);
    EXPECT_EQ(ops[1], Op::BIPUSH);
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
    FixtureBuilder b("t/R");
    const ca::u16  s = b.strConst("x");
    Bytes          code;
    ldc(code, s);
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 3, 1});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ConstantFolder{});
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
    // pc0 BIPUSH 1 / pc2 BIPUSH 2 / pc4 IADD / pc5 RETURN / pc6 GOTO→pc2
    // （锚定链中段成员）。
    Bytes code;
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    goto16(code, 2 - 6);
    const Bytes in = oneMethod(std::move(code));
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 首成员标签：分支跳入链头 → 折叠 + 标签迁移到替换头 ──
TEST(ConstantFolderTest, branchToChainHeadFoldsWithLabelMigration)
{
    // pc6 GOTO→pc0（链头）：折叠后 GOTO 随标签落到替换头。
    Bytes code;
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    goto16(code, 0 - 6);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
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
    // BIPUSH 1 / BIPUSH 2 / IADD / POP / RETURN；try 覆盖 [0,5)（仅链），
    // handler 在 RETURN。
    Bytes code;
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kPop);
    code.push_back(kReturn);
    FixtureBuilder b("t/R");
    b.addMethod({"m", "()V", std::move(code), 0x0009, {{0, 5, 6, 0}}, 2, 1});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ConstantFolder{});
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
    Bytes code;
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kPop);
    code.push_back(kReturn);
    FixtureBuilder b("t/R");
    b.addMethod({"m", "()V", std::move(code), 0x0009, {{2, 5, 6, 0}}, 2, 1});
    const Bytes in = b.assemble();
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 幂等：二次运行 no-op ──
TEST(ConstantFolderTest, secondRunIsNoOp)
{
    Bytes code;
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    const MethodPassRun first = runMethodPass(oneMethod(std::move(code)),
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
    Bytes code;
    bipush(code, 1);
    bipush(code, 2);
    code.push_back(static_cast<ca::u8>(Op::IADD));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code), 2),
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
        ca::i64     a;
        ca::i64     b;
        ca::i64     want;
        const char* name;
    };
    const Case cases[] = {
        {Op::LADD, 1, 1, 2, "ladd"},
        {Op::LSUB, 1, 0, 1, "lsub"},
        {Op::LMUL, 6, 7, 42, "lmul"},
        {Op::LDIV, -7, 3, -2, "ldiv-truncation"},
        {Op::LREM, -7, 3, -1, "lrem-sign"},
        {Op::LAND, 12, 10, 8, "land"},
        {Op::LOR, 12, 10, 14, "lor"},
        {Op::LXOR, 12, 10, 6, "lxor"},
    };
    for (const Case& c : cases) {
        FixtureBuilder b("t/R");
        const ca::u16  ia = b.longConst(c.a);
        const ca::u16  ib = b.longConst(c.b);
        Bytes          code;
        ldc2w(code, ia);
        ldc2w(code, ib);
        code.push_back(static_cast<ca::u8>(c.op));
        code.push_back(kReturn);
        b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 4, 1});
        const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ConstantFolder{});
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
        ca::i64 v;
        ca::i32 d;
        ca::i64 want;
    };
    const Case cases[] = {
        {Op::LSHL, 1, 3, 8},
        {Op::LSHR, -8, 1, -4},
        {Op::LUSHR, -8, 1, 0x7FFFFFFFFFFFFFFCLL},
        {Op::LSHL, 1, 65, 2},        // 掩 6 位：65 & 63 == 1
    };
    for (const Case& c : cases) {
        FixtureBuilder b("t/R");
        const ca::u16  lv = b.longConst(c.v);
        Bytes          code;
        ldc2w(code, lv);
        if (c.d >= -128 && c.d <= 127) {
            bipush(code, c.d);
        }
        else {
            sipush(code, c.d);
        }
        code.push_back(static_cast<ca::u8>(c.op));
        code.push_back(kReturn);
        b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 3, 1});
        const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        expectLongFold(r, c.want);
    }
}

// ── long 移位链：多级位移连续折叠 ──
TEST(ConstantFolderTest, longShiftChainFolds)
{
    // (1 << 3) << 1 == 16。
    Bytes code;
    code.push_back(static_cast<ca::u8>(Op::LCONST_1));
    bipush(code, 3);
    code.push_back(static_cast<ca::u8>(Op::LSHL));
    bipush(code, 1);
    code.push_back(static_cast<ca::u8>(Op::LSHL));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code), 4),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectLongFold(r, 16);
}

// ── long 移位 int 操作数非常量：停 ──
TEST(ConstantFolderTest, longShiftNonConstDistanceStops)
{
    Bytes code;
    code.push_back(static_cast<ca::u8>(Op::LCONST_1));
    code.push_back(static_cast<ca::u8>(Op::ILOAD_0));
    code.push_back(static_cast<ca::u8>(Op::LSHL));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code), 4);
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── long 移位 int 操作数是装载但后随非移位 op：停 ──
TEST(ConstantFolderTest, longChainIntPushNotFollowedByShiftStops)
{
    Bytes code;
    code.push_back(static_cast<ca::u8>(Op::LCONST_1));
    bipush(code, 2);
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code), 4);
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── long 红线：除 0 与 MIN/-1 保留不折 ──
TEST(ConstantFolderTest, longDivZeroNotFolded)
{
    FixtureBuilder b("t/R");
    const ca::u16  one = b.longConst(1);
    Bytes          code;
    ldc2w(code, one);
    code.push_back(static_cast<ca::u8>(Op::LCONST_0));
    code.push_back(static_cast<ca::u8>(Op::LDIV));
    code.push_back(kReturn);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 3, 1});
    const Bytes in = b.assemble();
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

TEST(ConstantFolderTest, longDivMinMinusOneNotFolded)
{
    FixtureBuilder b("t/R");
    const ca::u16  min = b.longConst((std::numeric_limits<ca::i64>::min)());
    const ca::u16  m1  = b.longConst(-1);
    Bytes          code;
    ldc2w(code, min);
    ldc2w(code, m1);
    code.push_back(static_cast<ca::u8>(Op::LDIV));
    code.push_back(kReturn);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 4, 1});
    const Bytes in = b.assemble();
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── long 回绕：LONG_MAX + 1 → LONG_MIN ──
TEST(ConstantFolderTest, longAddWrapsAtLongMax)
{
    FixtureBuilder b("t/R");
    const ca::u16  max = b.longConst((std::numeric_limits<ca::i64>::max)());
    Bytes          code;
    ldc2w(code, max);
    code.push_back(static_cast<ca::u8>(Op::LCONST_1));
    code.push_back(static_cast<ca::u8>(Op::LADD));
    code.push_back(kReturn);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 3, 1});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ConstantFolder{});
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
        ca::f32 a;
        ca::f32 b;
        ca::u32 want;
    };
    const Case cases[] = {
        {Op::FADD, 1.0f, 2.0f, f32Bits(3.0f)},
        {Op::FSUB, 2.0f, 1.0f, f32Bits(1.0f)},
        {Op::FMUL, 2.0f, 2.0f, f32Bits(4.0f)},
        {Op::FDIV, 2.0f, 1.0f, f32Bits(2.0f)},
        {Op::FDIV, 1.0f, 0.0f, f32Bits(inf)},    // IEEE：除 0 不抛异常
        {Op::FSUB, 1.0f, 2.0f, f32Bits(-1.0f)},
    };
    for (const Case& c : cases) {
        Bytes code;
        code.push_back(static_cast<ca::u8>(fconstOp(c.a)));
        code.push_back(static_cast<ca::u8>(fconstOp(c.b)));
        code.push_back(static_cast<ca::u8>(c.op));
        code.push_back(kReturn);
        const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                              mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        expectFloatBitsFold(r, c.want);
    }
}

// ── float 0/0 → NaN 物化（位图案不钉死，见 expectFloatIsNan 注）──
TEST(ConstantFolderTest, floatZeroDivZeroFoldsToNan)
{
    Bytes code;
    code.push_back(static_cast<ca::u8>(Op::FCONST_0));
    code.push_back(static_cast<ca::u8>(Op::FCONST_0));
    code.push_back(static_cast<ca::u8>(Op::FDIV));
    code.push_back(kReturn);
    const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                          mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectFloatIsNan(r);
}

// ── float 物化：+0.0 → FCONST_0；-0.0 → LDC（signbit 检查）──
TEST(ConstantFolderTest, floatMaterializesZeroSigns)
{
    // +0.0：FCONST_0 - FCONST_0 → FCONST_0。
    {
        Bytes code;
        code.push_back(static_cast<ca::u8>(Op::FCONST_0));
        code.push_back(static_cast<ca::u8>(Op::FCONST_0));
        code.push_back(static_cast<ca::u8>(Op::FSUB));
        code.push_back(kReturn);
        const MethodPassRun r = runMethodPass(oneMethod(std::move(code)),
                                              mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        EXPECT_EQ(opsOf(ins).front(), Op::FCONST_0);
        expectFloatBitsFold(r, f32Bits(0.0f));
    }
    // -0.0：LDC(-1.0) * FCONST_0 → LDC(-0.0)（0x80000000，不走 FCONST_0）。
    {
        FloatDoubleFixture f("t/R");
        const ca::u16      negOne = f.floatConst(-1.0f);
        Bytes              code;
        ldc(code, negOne);
        code.push_back(static_cast<ca::u8>(Op::FCONST_0));
        code.push_back(static_cast<ca::u8>(Op::FMUL));
        code.push_back(kReturn);
        f.addMethod(std::move(code));
        const MethodPassRun r = runMethodPass(f.assemble(), mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        EXPECT_EQ(opsOf(ins).front(), Op::LDC);
        expectFloatBitsFold(r, 0x80000000u);
    }
}

// ── float LDC 输入（util 探测面外的 ldc 族 Float）──
TEST(ConstantFolderTest, floatLdcInputFolds)
{
    FloatDoubleFixture f("t/R");
    const ca::u16      a = f.floatConst(2.5f);
    const ca::u16      b = f.floatConst(4.0f);
    Bytes              code;
    ldc(code, a);
    ldc(code, b);
    code.push_back(static_cast<ca::u8>(Op::FADD));
    code.push_back(kReturn);
    f.addMethod(std::move(code));
    const MethodPassRun r = runMethodPass(f.assemble(), mjbase::pass::ConstantFolder{});
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
        ca::f64 a;
        ca::f64 b;
        ca::u64 want;
    };
    const Case cases[] = {
        {Op::DADD, 1.0, 1.0, f64Bits(2.0)},
        {Op::DSUB, 0.0, 1.0, f64Bits(-1.0)},
        {Op::DMUL, 1.0, 1.0, f64Bits(1.0)},
        {Op::DDIV, 1.0, 1.0, f64Bits(1.0)},
        {Op::DDIV, 1.0, 0.0, f64Bits(inf)},
    };
    for (const Case& c : cases) {
        FloatDoubleFixture f("t/R");
        Bytes              code;
        code.push_back(static_cast<ca::u8>(dconstOp(c.a)));
        code.push_back(static_cast<ca::u8>(dconstOp(c.b)));
        code.push_back(static_cast<ca::u8>(c.op));
        code.push_back(kReturn);
        f.addMethod(std::move(code), 4);
        const MethodPassRun r = runMethodPass(f.assemble(), mjbase::pass::ConstantFolder{});
        EXPECT_TRUE(r.pa.changed());
        expectDoubleBitsFold(r, c.want);
    }
}

// ── double 0/0 → NaN 物化 ──
TEST(ConstantFolderTest, doubleZeroDivZeroFoldsToNan)
{
    FloatDoubleFixture f("t/R");
    Bytes              code;
    code.push_back(static_cast<ca::u8>(Op::DCONST_0));
    code.push_back(static_cast<ca::u8>(Op::DCONST_0));
    code.push_back(static_cast<ca::u8>(Op::DDIV));
    code.push_back(kReturn);
    f.addMethod(std::move(code), 4);
    const MethodPassRun r = runMethodPass(f.assemble(), mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectDoubleIsNan(r);
}

// ── double LDC2_W 输入与输出（CP 双槽追加路径）──
TEST(ConstantFolderTest, doubleLdc2wInputFolds)
{
    FloatDoubleFixture f("t/R");
    const ca::u16      a = f.doubleConst(1.5);
    const ca::u16      b = f.doubleConst(2.5);
    Bytes              code;
    ldc2w(code, a);
    ldc2w(code, b);
    code.push_back(static_cast<ca::u8>(Op::DMUL));
    code.push_back(kReturn);
    f.addMethod(std::move(code), 4);
    const MethodPassRun r = runMethodPass(f.assemble(), mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(r.pa.changed());
    expectDoubleBitsFold(r, f64Bits(3.75));
}

// ── double 链长 1：不折 ──
TEST(ConstantFolderTest, doubleSinglePushNotFolded)
{
    Bytes code;
    code.push_back(static_cast<ca::u8>(Op::DCONST_1));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code), 2);
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── float/double 类型混用：停 ──
TEST(ConstantFolderTest, floatDoubleMixNotFolded)
{
    Bytes code;
    code.push_back(static_cast<ca::u8>(Op::FCONST_1));
    code.push_back(static_cast<ca::u8>(Op::DCONST_1));
    code.push_back(static_cast<ca::u8>(Op::DADD));
    code.push_back(kReturn);
    const Bytes in = oneMethod(std::move(code), 4);
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}

// ── 空方法体（仅 RETURN）：不折 ──
TEST(ConstantFolderTest, emptyBodyUntouched)
{
    const Bytes in = oneMethod({kReturn});
    expectUnchanged(in, runMethodPass(in, mjbase::pass::ConstantFolder{}));
}
