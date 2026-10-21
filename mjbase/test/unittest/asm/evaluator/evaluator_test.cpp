/// @file        evaluator_test.cpp
/// @brief       asm/evaluator 具体栈机测试：纯 CodeDom 构造（append/insertBefore/
///              createLabel），覆盖算术 Java 语义、分支真假两路、cat2 双槽、
///              步数熔断、不支持形状结构化报错。
///
/// 断言覆盖 overflow/shift/div/rem/cmp/switch/branch/step-budget 用例族。
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "mjbase/asm/evaluator/evaluator.hpp"

namespace {

using mjbase::asm_evaluator::EvalError;
using mjbase::asm_evaluator::EvalOutcome;
using mjbase::asm_evaluator::MethodSig;
using mjbase::asm_evaluator::Options;
using mjbase::asm_evaluator::RefFactory;
using mjbase::asm_evaluator::SigError;
using mjbase::asm_evaluator::SigKind;
using mjbase::asm_evaluator::Value;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

/// 无操作数指令。
Insn mk(Op op)
{
    Insn in;
    in.op = op;
    return in;
}

/// 带一个 i32 操作数（局部变量索引 / 立即数 / iinc 常量）的指令。
Insn mkA(Op op, int a)
{
    Insn in = mk(op);
    in.a    = a;
    return in;
}

/// iinc：a = 槽号，b = 增量。
Insn mkIinc(int index, int delta)
{
    Insn in = mk(Op::IINC);
    in.a    = index;
    in.b    = delta;
    return in;
}

InsnId add(CodeDom& dom, Insn in)
{
    const auto r = dom.append(std::move(in));
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

InsnId addOp(CodeDom& dom, Op op) { return add(dom, mk(op)); }

/// 在 at 前插入分支指令（目标标签须已锚定）。
InsnId insertBranch(CodeDom& dom, InsnId at, Op op, LabelId target)
{
    Insn in = mk(op);
    in.target = target;
    const auto r = dom.insertBefore(at, std::move(in));
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

LabelId labelAt(CodeDom& dom, InsnId anchor)
{
    const auto r = dom.createLabel(anchor);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

MethodSig sigOf(const char* descriptor, bool isStatic = true)
{
    const auto r = MethodSig::parse(descriptor, isStatic);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 快捷求值：静态方法 + 空入参。
mjbase::asm_evaluator::Result<EvalOutcome, EvalError> evalStatic(CodeDom& dom,
                                                                 const MethodSig& sig,
                                                                 const Options& options = Options{})
{
    return mjbase::asm_evaluator::evaluate(dom, sig, {}, options);
}

constexpr ca::i32 kIntMin = (std::numeric_limits<ca::i32>::min)();
constexpr ca::i32 kIntMax = (std::numeric_limits<ca::i32>::max)();
constexpr ca::i64 kLongMin = (std::numeric_limits<ca::i64>::min)();
constexpr ca::i64 kLongMax = (std::numeric_limits<ca::i64>::max)();

}   // namespace

// ---------------------------------------------------------------------------
// 算术：Java 溢出 / 移位 / 除法 / 取余语义
// ---------------------------------------------------------------------------

TEST(MjAsmEvaluatorTest, IntegerArithmeticMultiplyReturn)
{
    CodeDom dom;
    const MethodSig sig = sigOf("()I");
    addOp(dom, Op::ICONST_2);
    addOp(dom, Op::ICONST_3);
    addOp(dom, Op::IMUL);
    addOp(dom, Op::IRETURN);

    const auto result = evalStatic(dom, sig);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.unwrap().kind, EvalOutcome::Kind::Value);
    EXPECT_EQ(result.unwrap().value.kind, Value::Kind::I32);
    EXPECT_EQ(result.unwrap().value.i32, 6);
}

TEST(MjAsmEvaluatorTest, IntegerArithmeticUsesJavaOverflowSemantics)
{
    // INT_MAX = iconst_m1 << 31 ^ -1；(MAX+1) 取负、除 -1 均回落 INT_MIN。
    {
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        addOp(dom, Op::ICONST_M1);
        add(dom, mkA(Op::BIPUSH, 31));
        addOp(dom, Op::ISHL);
        addOp(dom, Op::ICONST_M1);
        addOp(dom, Op::IXOR);
        addOp(dom, Op::ICONST_1);
        addOp(dom, Op::IADD);
        addOp(dom, Op::INEG);
        addOp(dom, Op::ICONST_M1);
        addOp(dom, Op::IDIV);
        addOp(dom, Op::IRETURN);

        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i32, kIntMin);
    }
    {
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        addOp(dom, Op::ICONST_M1);
        addOp(dom, Op::ICONST_1);
        addOp(dom, Op::ISHL);
        addOp(dom, Op::IRETURN);

        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i32, -2);
    }
    {
        // ishr(-8,1) = -4（算术右移）；iushr(-8,1) = 0x7FFFFFFC（逻辑右移）。
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        add(dom, mkA(Op::BIPUSH, -8));
        addOp(dom, Op::ICONST_1);
        addOp(dom, Op::ISHR);
        addOp(dom, Op::IRETURN);

        const auto shrResult = evalStatic(dom, sig);
        ASSERT_TRUE(shrResult.is_ok());
        EXPECT_EQ(shrResult.unwrap().value.i32, -4);

        CodeDom dom2;
        add(dom2, mkA(Op::BIPUSH, -8));
        addOp(dom2, Op::ICONST_1);
        addOp(dom2, Op::IUSHR);
        addOp(dom2, Op::IRETURN);
        const auto ushrResult = evalStatic(dom2, sig);
        ASSERT_TRUE(ushrResult.is_ok());
        EXPECT_EQ(ushrResult.unwrap().value.i32, 0x7FFFFFFC);
    }
    {
        // irem：MIN % -1 = 0；普通 9 % 2 = 1。
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        addOp(dom, Op::ICONST_M1);
        add(dom, mkA(Op::BIPUSH, 31));
        addOp(dom, Op::ISHL);
        addOp(dom, Op::ICONST_M1);
        addOp(dom, Op::IREM);
        addOp(dom, Op::IRETURN);
        const auto minRem = evalStatic(dom, sig);
        ASSERT_TRUE(minRem.is_ok());
        EXPECT_EQ(minRem.unwrap().value.i32, 0);
    }
}

TEST(MjAsmEvaluatorTest, LongArithmeticOverflowShiftDivRem)
{
    const MethodSig sig = sigOf("()J");
    {
        // (1L << 63) = LONG_MIN；取负按补码环绕仍是 LONG_MIN；再 +1 得 LONG_MIN+1。
        CodeDom dom;
        addOp(dom, Op::LCONST_1);
        add(dom, mkA(Op::BIPUSH, 63));
        addOp(dom, Op::LSHL);
        addOp(dom, Op::LNEG);
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::LADD);
        addOp(dom, Op::LRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.kind, Value::Kind::I64);
        EXPECT_EQ(result.unwrap().value.i64, kLongMin + 1);
    }
    {
        // 1L << 4 = 16；-1L >>> 1 = LONG_MAX（移位数是 int 栈值）。
        CodeDom dom;
        addOp(dom, Op::LCONST_1);
        add(dom, mkA(Op::BIPUSH, 4));
        addOp(dom, Op::LSHL);
        addOp(dom, Op::LRETURN);
        const auto shl = evalStatic(dom, sig);
        ASSERT_TRUE(shl.is_ok());
        EXPECT_EQ(shl.unwrap().value.i64, 16);

        CodeDom dom2;
        addOp(dom2, Op::LCONST_1);
        addOp(dom2, Op::LNEG);
        addOp(dom2, Op::ICONST_1);
        addOp(dom2, Op::LUSHR);
        addOp(dom2, Op::LRETURN);
        const auto ushr = evalStatic(dom2, sig);
        ASSERT_TRUE(ushr.is_ok());
        EXPECT_EQ(ushr.unwrap().value.i64, kLongMax);
    }
    {
        // 9L / 2L = 4；9L % 2L = 1；LONG_MIN / -1 = LONG_MIN。
        CodeDom dom;
        add(dom, mkA(Op::BIPUSH, 9));
        addOp(dom, Op::I2L);
        add(dom, mkA(Op::BIPUSH, 2));
        addOp(dom, Op::I2L);
        addOp(dom, Op::LDIV);
        addOp(dom, Op::LRETURN);
        const auto div = evalStatic(dom, sig);
        ASSERT_TRUE(div.is_ok());
        EXPECT_EQ(div.unwrap().value.i64, 4);

        CodeDom dom2;
        add(dom2, mkA(Op::BIPUSH, 9));
        addOp(dom2, Op::I2L);
        add(dom2, mkA(Op::BIPUSH, 2));
        addOp(dom2, Op::I2L);
        addOp(dom2, Op::LREM);
        addOp(dom2, Op::LRETURN);
        const auto rem = evalStatic(dom2, sig);
        ASSERT_TRUE(rem.is_ok());
        EXPECT_EQ(rem.unwrap().value.i64, 1);

        CodeDom dom3;
        addOp(dom3, Op::LCONST_1);
        add(dom3, mkA(Op::BIPUSH, 63));
        addOp(dom3, Op::LSHL);
        addOp(dom3, Op::LCONST_1);
        addOp(dom3, Op::LNEG);
        addOp(dom3, Op::LDIV);
        addOp(dom3, Op::LRETURN);
        const auto minDiv = evalStatic(dom3, sig);
        ASSERT_TRUE(minDiv.is_ok());
        EXPECT_EQ(minDiv.unwrap().value.i64, kLongMin);
    }
    {
        // lcmp 三态：1<2 → -1；2>1 → 1；2=2 → 0。
        const MethodSig isig = sigOf("()I");
        CodeDom dom;
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::LADD);   // rhs = 2
        addOp(dom, Op::LCMP);
        addOp(dom, Op::IRETURN);
        const auto less = evalStatic(dom, isig);
        ASSERT_TRUE(less.is_ok());
        EXPECT_EQ(less.unwrap().value.i32, -1);

        CodeDom dom2;
        addOp(dom2, Op::LCONST_1);
        addOp(dom2, Op::LCONST_1);
        addOp(dom2, Op::LADD);   // lhs = 2
        addOp(dom2, Op::LCONST_1);
        addOp(dom2, Op::LCMP);
        addOp(dom2, Op::IRETURN);
        const auto greater = evalStatic(dom2, isig);
        ASSERT_TRUE(greater.is_ok());
        EXPECT_EQ(greater.unwrap().value.i32, 1);

        CodeDom dom3;
        addOp(dom3, Op::LCONST_1);
        addOp(dom3, Op::LCONST_1);
        addOp(dom3, Op::LADD);
        addOp(dom3, Op::LCONST_1);
        addOp(dom3, Op::LCONST_1);
        addOp(dom3, Op::LADD);
        addOp(dom3, Op::LCMP);
        addOp(dom3, Op::IRETURN);
        const auto equal = evalStatic(dom3, isig);
        ASSERT_TRUE(equal.is_ok());
        EXPECT_EQ(equal.unwrap().value.i32, 0);
    }
}

TEST(MjAsmEvaluatorTest, FloatDoubleArithmeticAndNanCompare)
{
    {
        CodeDom dom;
        const MethodSig sig = sigOf("()F");
        addOp(dom, Op::FCONST_1);
        addOp(dom, Op::FCONST_2);
        addOp(dom, Op::FADD);
        addOp(dom, Op::FRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.kind, Value::Kind::F32);
        EXPECT_FLOAT_EQ(result.unwrap().value.f32, 3.0f);
    }
    {
        // frem 与 IEEE remainder 一致：fmod(3,2) = 1。
        CodeDom dom;
        const MethodSig sig = sigOf("()F");
        addOp(dom, Op::FCONST_1);
        addOp(dom, Op::FCONST_2);
        addOp(dom, Op::FADD);
        addOp(dom, Op::FCONST_2);
        addOp(dom, Op::FREM);
        addOp(dom, Op::FRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_FLOAT_EQ(result.unwrap().value.f32, 1.0f);
    }
    {
        // NaN（0.0f/0.0f）：fcmpl → -1、fcmpg → +1。
        const MethodSig sig = sigOf("()I");
        CodeDom dom;
        addOp(dom, Op::FCONST_0);
        addOp(dom, Op::FCONST_0);
        addOp(dom, Op::FDIV);
        addOp(dom, Op::FCONST_1);
        addOp(dom, Op::FCMPL);
        addOp(dom, Op::IRETURN);
        const auto cmpl = evalStatic(dom, sig);
        ASSERT_TRUE(cmpl.is_ok());
        EXPECT_EQ(cmpl.unwrap().value.i32, -1);

        CodeDom dom2;
        addOp(dom2, Op::FCONST_0);
        addOp(dom2, Op::FCONST_0);
        addOp(dom2, Op::FDIV);
        addOp(dom2, Op::FCONST_1);
        addOp(dom2, Op::FCMPG);
        addOp(dom2, Op::IRETURN);
        const auto cmpg = evalStatic(dom2, sig);
        ASSERT_TRUE(cmpg.is_ok());
        EXPECT_EQ(cmpg.unwrap().value.i32, 1);
    }
    {
        // (dconst_1 + dconst_1) * (dconst_1 + dconst_1) = 4.0；NaN 的 dcmpl → -1。
        CodeDom dom;
        const MethodSig sig = sigOf("()D");
        addOp(dom, Op::DCONST_1);
        addOp(dom, Op::DCONST_1);
        addOp(dom, Op::DADD);
        addOp(dom, Op::DCONST_1);
        addOp(dom, Op::DCONST_1);
        addOp(dom, Op::DADD);
        addOp(dom, Op::DMUL);
        addOp(dom, Op::DRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_DOUBLE_EQ(result.unwrap().value.f64, 4.0);

        const MethodSig isig = sigOf("()I");
        CodeDom dom2;
        addOp(dom2, Op::DCONST_0);
        addOp(dom2, Op::DCONST_0);
        addOp(dom2, Op::DDIV);
        addOp(dom2, Op::DCONST_1);
        addOp(dom2, Op::DCMPL);
        addOp(dom2, Op::IRETURN);
        const auto cmpl = evalStatic(dom2, isig);
        ASSERT_TRUE(cmpl.is_ok());
        EXPECT_EQ(cmpl.unwrap().value.i32, -1);
    }
}

TEST(MjAsmEvaluatorTest, ConversionsClampTruncateAndNarrow)
{
    const MethodSig isig = sigOf("()I");
    {
        // f2i：NaN → 0；2^31f（越界）饱和到 INT_MAX。
        CodeDom dom;
        addOp(dom, Op::FCONST_0);
        addOp(dom, Op::FCONST_0);
        addOp(dom, Op::FDIV);
        addOp(dom, Op::F2I);
        addOp(dom, Op::IRETURN);
        const auto nan = evalStatic(dom, isig);
        ASSERT_TRUE(nan.is_ok());
        EXPECT_EQ(nan.unwrap().value.i32, 0);

        CodeDom dom2;
        addOp(dom2, Op::LCONST_1);
        add(dom2, mkA(Op::BIPUSH, 31));
        addOp(dom2, Op::LSHL);
        addOp(dom2, Op::L2F);
        addOp(dom2, Op::F2I);
        addOp(dom2, Op::IRETURN);
        const auto clamp = evalStatic(dom2, isig);
        ASSERT_TRUE(clamp.is_ok());
        EXPECT_EQ(clamp.unwrap().value.i32, kIntMax);
    }
    {
        // d2l：2^62 × 2.0 = 2^63 超出 LONG_MAX 饱和；(2^32+1) l2i 截断为 1。
        const MethodSig jsig = sigOf("()J");
        CodeDom dom;
        addOp(dom, Op::LCONST_1);
        add(dom, mkA(Op::BIPUSH, 62));
        addOp(dom, Op::LSHL);
        addOp(dom, Op::L2D);
        addOp(dom, Op::DCONST_1);
        addOp(dom, Op::DCONST_1);
        addOp(dom, Op::DADD);   // × 2
        addOp(dom, Op::DMUL);
        addOp(dom, Op::D2L);
        addOp(dom, Op::LRETURN);
        const auto clamp = evalStatic(dom, jsig);
        ASSERT_TRUE(clamp.is_ok());
        EXPECT_EQ(clamp.unwrap().value.i64, kLongMax);

        CodeDom dom2;
        addOp(dom2, Op::LCONST_1);
        add(dom2, mkA(Op::BIPUSH, 32));
        addOp(dom2, Op::LSHL);
        addOp(dom2, Op::LCONST_1);
        addOp(dom2, Op::LADD);
        addOp(dom2, Op::L2I);
        addOp(dom2, Op::IRETURN);
        const auto trunc = evalStatic(dom2, isig);
        ASSERT_TRUE(trunc.is_ok());
        EXPECT_EQ(trunc.unwrap().value.i32, 1);
    }
    {
        // i2b/i2c/i2s：截断 + 符号/零扩展。
        CodeDom dom;
        add(dom, mkA(Op::SIPUSH, 0x1234));
        addOp(dom, Op::I2B);
        addOp(dom, Op::IRETURN);
        const auto toB = evalStatic(dom, isig);
        ASSERT_TRUE(toB.is_ok());
        EXPECT_EQ(toB.unwrap().value.i32, 0x34);

        CodeDom dom2;
        add(dom2, mkA(Op::SIPUSH, -257));
        addOp(dom2, Op::I2B);
        addOp(dom2, Op::IRETURN);
        const auto toBneg = evalStatic(dom2, isig);
        ASSERT_TRUE(toBneg.is_ok());
        EXPECT_EQ(toBneg.unwrap().value.i32, -1);

        CodeDom dom3;
        addOp(dom3, Op::ICONST_M1);
        addOp(dom3, Op::I2C);
        addOp(dom3, Op::IRETURN);
        const auto toC = evalStatic(dom3, isig);
        ASSERT_TRUE(toC.is_ok());
        EXPECT_EQ(toC.unwrap().value.i32, 0xFFFF);

        CodeDom dom4;
        add(dom4, mkA(Op::SIPUSH, 0x1234));
        add(dom4, mkA(Op::BIPUSH, 16));
        addOp(dom4, Op::ISHL);
        addOp(dom4, Op::ICONST_5);
        addOp(dom4, Op::IADD);
        addOp(dom4, Op::I2S);
        addOp(dom4, Op::IRETURN);
        const auto toS = evalStatic(dom4, isig);
        ASSERT_TRUE(toS.is_ok());
        EXPECT_EQ(toS.unwrap().value.i32, 5);
    }
}

// ---------------------------------------------------------------------------
// 分支：真/假两路、goto、switch、循环
// ---------------------------------------------------------------------------

TEST(MjAsmEvaluatorTest, BranchFollowsTrueAndFalsePaths)
{
    // iload_0; ifeq Lf; iconst_1; goto Le; Lf: iconst_2; Le: ireturn
    const MethodSig sig = sigOf("(I)I");
    for (const auto& [input, expected] : std::vector<std::pair<ca::i32, ca::i32>>{
             {0, 2},   // 分支成立走 false 块
             {1, 1},   // 分支不成立顺序执行
             {-3, 1},
         }) {
        CodeDom dom;
        const InsnId load = addOp(dom, Op::ILOAD_0);
        const InsnId one = addOp(dom, Op::ICONST_1);
        const InsnId two = addOp(dom, Op::ICONST_2);
        const InsnId ret = addOp(dom, Op::IRETURN);
        const LabelId falseLabel = labelAt(dom, two);
        const LabelId endLabel = labelAt(dom, ret);
        insertBranch(dom, one, Op::IFEQ, falseLabel);
        insertBranch(dom, two, Op::GOTO, endLabel);
        EXPECT_EQ(dom.order().size(), 6u);
        (void)load;

        const auto result = mjbase::asm_evaluator::evaluate(
            dom, sig, std::vector<Value>{Value::ofI32(input)});
        ASSERT_TRUE(result.is_ok()) << "input=" << input;
        EXPECT_EQ(result.unwrap().value.i32, expected) << "input=" << input;
    }
}

TEST(MjAsmEvaluatorTest, IfIcmpBranchesBothDirections)
{
    // iload_0; bipush 3; if_icmpge Lge; iconst_m1; ireturn; Lge: iconst_1; ireturn
    const MethodSig sig = sigOf("(I)I");
    for (const auto& [input, expected] : std::vector<std::pair<ca::i32, ca::i32>>{
             {3, 1},
             {9, 1},
             {2, -1},
         }) {
        CodeDom dom;
        addOp(dom, Op::ILOAD_0);
        add(dom, mkA(Op::BIPUSH, 3));
        const InsnId miss = addOp(dom, Op::ICONST_M1);
        addOp(dom, Op::IRETURN);
        const InsnId hit = addOp(dom, Op::ICONST_1);
        addOp(dom, Op::IRETURN);
        insertBranch(dom, miss, Op::IF_ICMPGE, labelAt(dom, hit));

        const auto result = mjbase::asm_evaluator::evaluate(
            dom, sig, std::vector<Value>{Value::ofI32(input)});
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i32, expected);
    }
}

TEST(MjAsmEvaluatorTest, BackwardBranchLoopWithIinc)
{
    // acc=0; Lhead: acc+=n; iinc n,-1; if n>0 goto Lhead; return acc
    // n=4 → 4+3+2+1 = 10。
    CodeDom dom;
    const MethodSig sig = sigOf("(I)I");
    addOp(dom, Op::ICONST_0);
    addOp(dom, Op::ISTORE_1);
    const InsnId head = addOp(dom, Op::ILOAD_1);
    addOp(dom, Op::ILOAD_0);
    addOp(dom, Op::IADD);
    addOp(dom, Op::ISTORE_1);
    add(dom, mkIinc(0, -1));
    addOp(dom, Op::ILOAD_0);
    const LabelId headLabel = labelAt(dom, head);
    Insn loop = mk(Op::IFGT);
    loop.target = headLabel;
    add(dom, std::move(loop));
    addOp(dom, Op::ILOAD_1);
    addOp(dom, Op::IRETURN);

    const auto result = mjbase::asm_evaluator::evaluate(dom, sig,
                                                        std::vector<Value>{Value::ofI32(4)});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.unwrap().value.i32, 10);
}

TEST(MjAsmEvaluatorTest, LookupSwitchDispatchesKeyAndDefault)
{
    const MethodSig sig = sigOf("(I)I");
    for (const auto& [input, expected] : std::vector<std::pair<ca::i32, ca::i32>>{
             {7, 2},   // 命中 case
             {0, 1},   // 落 default
             {8, 1},
         }) {
        CodeDom dom;
        addOp(dom, Op::ILOAD_0);
        const InsnId defFirst = addOp(dom, Op::ICONST_1);
        addOp(dom, Op::IRETURN);
        const InsnId caseFirst = addOp(dom, Op::ICONST_2);
        addOp(dom, Op::IRETURN);

        Insn sw = mk(Op::LOOKUPSWITCH);
        sw.target = labelAt(dom, defFirst);
        sw.keys = {7};
        sw.targets = {labelAt(dom, caseFirst)};
        const auto inserted = dom.insertBefore(defFirst, std::move(sw));
        EXPECT_TRUE(inserted.is_ok());

        const auto result = mjbase::asm_evaluator::evaluate(
            dom, sig, std::vector<Value>{Value::ofI32(input)});
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i32, expected);
    }
}

TEST(MjAsmEvaluatorTest, TableSwitchCoversRangeEdgesAndDefault)
{
    // low=1..high=3 → 10/20/30；越界（0 与 9）落 default=99。
    const MethodSig sig = sigOf("(I)I");
    for (const auto& [input, expected] : std::vector<std::pair<ca::i32, ca::i32>>{
             {1, 10},
             {2, 20},
             {3, 30},
             {0, 99},
             {9, 99},
         }) {
        CodeDom dom;
        addOp(dom, Op::ILOAD_0);
        const InsnId c1 = add(dom, mkA(Op::BIPUSH, 10));
        addOp(dom, Op::IRETURN);
        const InsnId c2 = add(dom, mkA(Op::BIPUSH, 20));
        addOp(dom, Op::IRETURN);
        const InsnId c3 = add(dom, mkA(Op::BIPUSH, 30));
        addOp(dom, Op::IRETURN);
        const InsnId defFirst = add(dom, mkA(Op::BIPUSH, 99));
        addOp(dom, Op::IRETURN);

        Insn sw = mk(Op::TABLESWITCH);
        sw.a = 1;
        sw.b = 3;
        sw.target = labelAt(dom, defFirst);
        sw.targets = {labelAt(dom, c1), labelAt(dom, c2), labelAt(dom, c3)};
        const auto inserted = dom.insertBefore(c1, std::move(sw));
        EXPECT_TRUE(inserted.is_ok());

        const auto result = mjbase::asm_evaluator::evaluate(
            dom, sig, std::vector<Value>{Value::ofI32(input)});
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i32, expected);
    }
}

// ---------------------------------------------------------------------------
// cat2 双槽：注入布局、Top 高槽、存取往返
// ---------------------------------------------------------------------------

TEST(MjAsmEvaluatorTest, Cat2ParamsOccupyTwoLocalSlots)
{
    // (IJB)I：I@0、J@1-2、B@3；iload_0 + iload_3 证明 byte 参数在槽 3。
    CodeDom dom;
    const MethodSig sig = sigOf("(IJB)I");
    addOp(dom, Op::ILOAD_0);
    addOp(dom, Op::ILOAD_3);
    addOp(dom, Op::IADD);
    addOp(dom, Op::IRETURN);

    const auto result = mjbase::asm_evaluator::evaluate(
        dom, sig, std::vector<Value>{Value::ofI32(7), Value::ofI64(1000000), Value::ofI32(5)});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.unwrap().value.i32, 12);
}

TEST(MjAsmEvaluatorTest, ReadingCat2TopSlotIsBadState)
{
    {
        // (J)I：slot1 是 long 的高槽（Top），iload_1 读它是坏状态。
        CodeDom dom;
        const MethodSig sig = sigOf("(J)I");
        add(dom, mkA(Op::ILOAD, 1));
        addOp(dom, Op::IRETURN);
        const auto result = mjbase::asm_evaluator::evaluate(dom, sig,
                                                            std::vector<Value>{Value::ofI64(1)});
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
    {
        // ()I：lstore_0 后 slot1 为 Top。
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::LSTORE_0);
        add(dom, mkA(Op::ILOAD, 1));
        addOp(dom, Op::IRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
}

TEST(MjAsmEvaluatorTest, Cat2StoreLoadRoundtrip)
{
    // bipush 7 → i2l → lstore_0 → lload_0 → lreturn。
    CodeDom dom;
    const MethodSig sig = sigOf("()J");
    add(dom, mkA(Op::BIPUSH, 7));
    addOp(dom, Op::I2L);
    addOp(dom, Op::LSTORE_0);
    addOp(dom, Op::LLOAD_0);
    addOp(dom, Op::LRETURN);
    const auto result = evalStatic(dom, sig);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.unwrap().value.i64, 7);
}

TEST(MjAsmEvaluatorTest, WideLocalIndexUsesOperandA)
{
    // 显式索引（wide 解码后同样落在 a）：bipush 42 → istore 300 → iload 300。
    CodeDom dom;
    const MethodSig sig = sigOf("()I");
    add(dom, mkA(Op::BIPUSH, 42));
    Insn store = mkA(Op::ISTORE, 300);
    store.wide = true;
    add(dom, std::move(store));
    Insn load = mkA(Op::ILOAD, 300);
    load.wide = true;
    add(dom, std::move(load));
    addOp(dom, Op::IRETURN);

    const auto result = evalStatic(dom, sig);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.unwrap().value.i32, 42);
}

// ---------------------------------------------------------------------------
// 引用只读形状：aconst_null / arraylength / ifnull / 引用身份
// ---------------------------------------------------------------------------

TEST(MjAsmEvaluatorTest, ArrayLengthReadsInjectedArray)
{
    {
        CodeDom dom;
        const MethodSig sig = sigOf("([I)I");
        addOp(dom, Op::ALOAD_0);
        addOp(dom, Op::ARRAYLENGTH);
        addOp(dom, Op::IRETURN);
        RefFactory refs;
        const auto result = mjbase::asm_evaluator::evaluate(
            dom, sig, std::vector<Value>{refs.array(4)});
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i32, 4);
    }
    {
        // null 读长度：NPE 形状 → BadState。
        CodeDom dom;
        const MethodSig sig = sigOf("([I)I");
        addOp(dom, Op::ALOAD_0);
        addOp(dom, Op::ARRAYLENGTH);
        addOp(dom, Op::IRETURN);
        const auto result = mjbase::asm_evaluator::evaluate(dom, sig,
                                                            std::vector<Value>{Value::ofNull()});
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
}

TEST(MjAsmEvaluatorTest, IfNullBranchesOnNullness)
{
    // aload_0; ifnull Ln; iconst_0; ireturn; Ln: iconst_1; ireturn
    const MethodSig sig = sigOf("(Ljava/lang/Object;)I");
    const auto run = [&](bool isNull) -> ca::i32 {
        CodeDom dom;
        addOp(dom, Op::ALOAD_0);
        const InsnId miss = addOp(dom, Op::ICONST_0);
        addOp(dom, Op::IRETURN);
        const InsnId hit = addOp(dom, Op::ICONST_1);
        addOp(dom, Op::IRETURN);
        insertBranch(dom, miss, Op::IFNULL, labelAt(dom, hit));

        RefFactory refs;
        const std::vector<Value> args{isNull ? Value::ofNull() : refs.object()};
        const auto result = mjbase::asm_evaluator::evaluate(dom, sig, args);
        EXPECT_TRUE(result.is_ok());
        return result.is_ok() ? result.unwrap().value.i32 : -1;
    };
    EXPECT_EQ(run(true), 1);
    EXPECT_EQ(run(false), 0);
}

TEST(MjAsmEvaluatorTest, IfAcmpComparesByReferenceIdentity)
{
    // aload_0; aload_1; if_acmpeq Leq; iconst_0; ireturn; Leq: iconst_1; ireturn
    const MethodSig sig = sigOf("([I[I)I");
    const auto run = [&](const Value& a, const Value& b) -> ca::i32 {
        CodeDom dom;
        addOp(dom, Op::ALOAD_0);
        addOp(dom, Op::ALOAD_1);
        const InsnId miss = addOp(dom, Op::ICONST_0);
        addOp(dom, Op::IRETURN);
        const InsnId hit = addOp(dom, Op::ICONST_1);
        addOp(dom, Op::IRETURN);
        insertBranch(dom, miss, Op::IF_ACMPEQ, labelAt(dom, hit));

        const auto result = mjbase::asm_evaluator::evaluate(dom, sig, std::vector<Value>{a, b});
        EXPECT_TRUE(result.is_ok());
        return result.is_ok() ? result.unwrap().value.i32 : -1;
    };
    RefFactory refs;
    const Value a = refs.array(4);
    // 同长度不同身份：不相等。
    EXPECT_EQ(run(a, refs.array(4)), 0);
    // 同一引用（拷贝传播身份）：相等。
    EXPECT_EQ(run(a, a), 1);
    // null == null：相等；null vs 非空：不等。
    EXPECT_EQ(run(Value::ofNull(), Value::ofNull()), 1);
    EXPECT_EQ(run(Value::ofNull(), refs.array(1)), 0);
}

TEST(MjAsmEvaluatorTest, AconstNullComparesEqualToNull)
{
    // aconst_null; aconst_null; if_acmpeq Leq; iconst_0; ireturn; Leq: iconst_1; ireturn
    CodeDom dom;
    const MethodSig sig = sigOf("()I");
    addOp(dom, Op::ACONST_NULL);
    addOp(dom, Op::ACONST_NULL);
    const InsnId miss = addOp(dom, Op::ICONST_0);
    addOp(dom, Op::IRETURN);
    const InsnId hit = addOp(dom, Op::ICONST_1);
    addOp(dom, Op::IRETURN);
    insertBranch(dom, miss, Op::IF_ACMPEQ, labelAt(dom, hit));

    const auto result = evalStatic(dom, sig);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.unwrap().value.i32, 1);
}

TEST(MjAsmEvaluatorTest, AreturnYieldsInjectedReference)
{
    CodeDom dom;
    const MethodSig sig = sigOf("()[I", /*isStatic=*/false);
    addOp(dom, Op::ALOAD_0);
    addOp(dom, Op::ARETURN);
    RefFactory refs;
    const Value arr = refs.array(12);
    const auto result =
        mjbase::asm_evaluator::evaluate(dom, sig, std::vector<Value>{arr});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.unwrap().kind, EvalOutcome::Kind::Value);
    EXPECT_TRUE(result.unwrap().value.sameRef(arr));
    EXPECT_TRUE(result.unwrap().value.refIsArray);
    EXPECT_EQ(result.unwrap().value.refLength, 12);
}

TEST(MjAsmEvaluatorTest, InstanceMethodInjectsThisAtSlotZero)
{
    // aload_0; ifnull Ln; iload_1; ireturn; Ln: iconst_m1; ireturn
    // this 非空 → 顺序读到槽 1 的 int 参数；this 为 null → 走 Ln。
    const MethodSig sig = sigOf("(I)I", /*isStatic=*/false);
    const auto run = [&](const Value& self) -> ca::i32 {
        CodeDom dom;
        addOp(dom, Op::ALOAD_0);
        const InsnId miss = addOp(dom, Op::ILOAD_1);
        addOp(dom, Op::IRETURN);
        const InsnId hit = addOp(dom, Op::ICONST_M1);
        addOp(dom, Op::IRETURN);
        insertBranch(dom, miss, Op::IFNULL, labelAt(dom, hit));

        const auto result =
            mjbase::asm_evaluator::evaluate(dom, sig, std::vector<Value>{self, Value::ofI32(33)});
        EXPECT_TRUE(result.is_ok());
        return result.is_ok() ? result.unwrap().value.i32 : -100;
    };
    RefFactory refs;
    EXPECT_EQ(run(refs.object()), 33);
    EXPECT_EQ(run(Value::ofNull()), -1);
}

// ---------------------------------------------------------------------------
// 步数熔断
// ---------------------------------------------------------------------------

TEST(MjAsmEvaluatorTest, StepLimitFusesInfiniteLoop)
{
    // Lhead: nop; goto Lhead —— 永不返回。
    CodeDom dom;
    const MethodSig sig = sigOf("()I");
    const InsnId head = addOp(dom, Op::NOP);
    const LabelId headLabel = labelAt(dom, head);
    Insn loop = mk(Op::GOTO);
    loop.target = headLabel;
    add(dom, std::move(loop));
    addOp(dom, Op::IRETURN);

    Options options;
    options.maxSteps = 8;
    const auto result = evalStatic(dom, sig, options);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::StepLimit);
    // 熔断错误指向将执行的指令（nop 或 goto，二者的 owner 一致）。
    EXPECT_EQ(result.unwrap_err().insn.owner, dom.owner());
}

TEST(MjAsmEvaluatorTest, StepBudgetBoundaryIsExact)
{
    // BackwardBranchLoopWithIinc 在 n=1 时恰好 11 步。
    const auto build = [] {
        CodeDom dom;
        addOp(dom, Op::ICONST_0);
        addOp(dom, Op::ISTORE_1);
        const InsnId head = addOp(dom, Op::ILOAD_1);
        addOp(dom, Op::ILOAD_0);
        addOp(dom, Op::IADD);
        addOp(dom, Op::ISTORE_1);
        add(dom, mkIinc(0, -1));
        addOp(dom, Op::ILOAD_0);
        const LabelId headLabel = labelAt(dom, head);
        Insn loop = mk(Op::IFGT);
        loop.target = headLabel;
        add(dom, std::move(loop));
        addOp(dom, Op::ILOAD_1);
        addOp(dom, Op::IRETURN);
        return dom;
    };
    const MethodSig sig = sigOf("(I)I");
    const std::vector<Value> args{Value::ofI32(1)};

    Options enough;
    enough.maxSteps = 11;
    const auto ok = mjbase::asm_evaluator::evaluate(build(), sig, args, enough);
    ASSERT_TRUE(ok.is_ok());
    EXPECT_EQ(ok.unwrap().value.i32, 1);

    Options tight;
    tight.maxSteps = 10;
    const auto fused = mjbase::asm_evaluator::evaluate(build(), sig, args, tight);
    ASSERT_TRUE(fused.is_err());
    EXPECT_EQ(fused.unwrap_err().kind, EvalError::Kind::StepLimit);
}

// ---------------------------------------------------------------------------
// 不支持形状与坏状态
// ---------------------------------------------------------------------------

TEST(MjAsmEvaluatorTest, UnsupportedOpcodesReportStructuredError)
{
    EXPECT_TRUE(mjbase::asm_evaluator::isSupportedOp(Op::IADD));
    EXPECT_TRUE(mjbase::asm_evaluator::isSupportedOp(Op::GOTO_W));
    EXPECT_TRUE(mjbase::asm_evaluator::isSupportedOp(Op::ARRAYLENGTH));
    EXPECT_FALSE(mjbase::asm_evaluator::isSupportedOp(Op::NEW));
    EXPECT_FALSE(mjbase::asm_evaluator::isSupportedOp(Op::LDC));
    EXPECT_FALSE(mjbase::asm_evaluator::isSupportedOp(Op::LDC2_W));
    EXPECT_FALSE(mjbase::asm_evaluator::isSupportedOp(Op::GETSTATIC));
    EXPECT_FALSE(mjbase::asm_evaluator::isSupportedOp(Op::INVOKESTATIC));
    EXPECT_FALSE(mjbase::asm_evaluator::isSupportedOp(Op::JSR));
    EXPECT_FALSE(mjbase::asm_evaluator::isSupportedOp(Op::ATHROW));

    {
        // new：对象分配不在子集内，第一步即 Unsupported，错误锚定该指令。
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        Insn newInsn = mk(Op::NEW);
        newInsn.cp = 1;
        const InsnId newId = add(dom, std::move(newInsn));
        addOp(dom, Op::ICONST_1);
        addOp(dom, Op::IRETURN);

        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::Unsupported);
        EXPECT_EQ(result.unwrap_err().insn, newId);
    }
    {
        // ldc：依赖常量池，CodeDom 单方法视图无法解析。
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        Insn ldc = mk(Op::LDC);
        ldc.cp = 1;
        add(dom, std::move(ldc));
        addOp(dom, Op::IRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::Unsupported);
    }
}

TEST(MjAsmEvaluatorTest, IntegerDivisionByZeroIsBadState)
{
    const MethodSig sig = sigOf("()I");
    {
        CodeDom dom;
        addOp(dom, Op::ICONST_1);
        addOp(dom, Op::ICONST_0);
        addOp(dom, Op::IDIV);
        addOp(dom, Op::IRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
    {
        CodeDom dom;
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::LCONST_0);
        addOp(dom, Op::LDIV);
        addOp(dom, Op::LRETURN);
        const MethodSig jsig = sigOf("()J");
        const auto result = evalStatic(dom, jsig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
}

TEST(MjAsmEvaluatorTest, StackUnderflowAndKindMismatchAreBadState)
{
    const MethodSig sig = sigOf("()I");
    {
        // iadd 空栈。
        CodeDom dom;
        addOp(dom, Op::IADD);
        addOp(dom, Op::IRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
    {
        // lconst_1 + iadd：类别错。
        CodeDom dom;
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::ICONST_1);
        addOp(dom, Op::IADD);
        addOp(dom, Op::IRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
    {
        // 未初始化局部变量读。
        CodeDom dom;
        addOp(dom, Op::ILOAD_2);
        addOp(dom, Op::IRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
}

TEST(MjAsmEvaluatorTest, FallingOffEndOrReturnKindMismatchIsBadState)
{
    {
        // 无 return 落到末尾。
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        addOp(dom, Op::ICONST_1);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
        EXPECT_EQ(result.unwrap_err().insn, jbc::ir::kNoInsn);
    }
    {
        // 空方法体。
        CodeDom dom;
        const MethodSig sig = sigOf("()V");
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
    {
        // ()I 却 return（void）。
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        addOp(dom, Op::RETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
    {
        // ()V 却 ireturn。
        CodeDom dom;
        const MethodSig sig = sigOf("()V");
        addOp(dom, Op::ICONST_1);
        addOp(dom, Op::IRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
}

TEST(MjAsmEvaluatorTest, ArgumentMismatchIsBadState)
{
    const MethodSig sig = sigOf("(I)I");
    CodeDom dom;
    addOp(dom, Op::ILOAD_0);
    addOp(dom, Op::IRETURN);

    {
        const auto r = mjbase::asm_evaluator::evaluate(dom, sig, std::vector<Value>{});
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err().kind, EvalError::Kind::BadState);
    }
    {
        const auto r =
            mjbase::asm_evaluator::evaluate(dom, sig, std::vector<Value>{Value::ofI64(1)});
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err().kind, EvalError::Kind::BadState);
    }
    {
        const auto r =
            mjbase::asm_evaluator::evaluate(dom, sig, std::vector<Value>{Value::ofNull()});
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err().kind, EvalError::Kind::BadState);
    }
}

TEST(MjAsmEvaluatorTest, VoidMethodCompletesWithVoidOutcome)
{
    CodeDom dom;
    const MethodSig sig = sigOf("()V");
    addOp(dom, Op::ICONST_1);
    addOp(dom, Op::POP);
    addOp(dom, Op::RETURN);
    const auto result = evalStatic(dom, sig);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.unwrap().kind, EvalOutcome::Kind::Void);
}

// ---------------------------------------------------------------------------
// 栈整形（cat1/cat2 形式）
// ---------------------------------------------------------------------------

TEST(MjAsmEvaluatorTest, StackShapersHandleCat2Forms)
{
    {
        // dup2 复制 long 后相加：1L + 1L = 2L。
        CodeDom dom;
        const MethodSig sig = sigOf("()J");
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::DUP2);
        addOp(dom, Op::LADD);
        addOp(dom, Op::LRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i64, 2);
    }
    {
        // swap：[1,2] → [2,1]，isub 由 -1 变 +1。
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        addOp(dom, Op::ICONST_1);
        addOp(dom, Op::ICONST_2);
        addOp(dom, Op::SWAP);
        addOp(dom, Op::ISUB);
        addOp(dom, Op::IRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i32, 1);
    }
    {
        // dup_x1：[1,2] → [2,1,2]，两轮 iadd = 2+1+2 = 5。
        CodeDom dom;
        const MethodSig sig = sigOf("()I");
        addOp(dom, Op::ICONST_1);
        addOp(dom, Op::ICONST_2);
        addOp(dom, Op::DUP_X1);
        addOp(dom, Op::IADD);
        addOp(dom, Op::IADD);
        addOp(dom, Op::IRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i32, 5);
    }
    {
        // pop2 弹掉 double 后返回 long。
        CodeDom dom;
        const MethodSig sig = sigOf("()J");
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::DCONST_1);
        addOp(dom, Op::POP2);
        addOp(dom, Op::LRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.unwrap().value.i64, 1);
    }
    {
        // dup 顶是 cat2：形式非法 → BadState。
        CodeDom dom;
        const MethodSig sig = sigOf("()J");
        addOp(dom, Op::LCONST_1);
        addOp(dom, Op::DUP);
        addOp(dom, Op::LRETURN);
        const auto result = evalStatic(dom, sig);
        ASSERT_TRUE(result.is_err());
        EXPECT_EQ(result.unwrap_err().kind, EvalError::Kind::BadState);
    }
}

// ---------------------------------------------------------------------------
// MethodSig：描述符解析
// ---------------------------------------------------------------------------

TEST(MjAsmEvaluatorTest, ParsesDescriptorLayout)
{
    {
        const auto r = MethodSig::parse("(IJ[Ljava/lang/String;D)V", true);
        ASSERT_TRUE(r.is_ok());
        const MethodSig& sig = r.unwrap();
        EXPECT_TRUE(sig.isStatic());
        EXPECT_EQ(sig.paramCount(), 4u);
        EXPECT_EQ(sig.argCount(), 4u);
        EXPECT_EQ(sig.paramSlotCount(), 6u);   // 1 + 2 + 1 + 2
        EXPECT_EQ(sig.paramKind(0), SigKind::I32);
        EXPECT_EQ(sig.paramKind(1), SigKind::I64);
        EXPECT_EQ(sig.paramKind(2), SigKind::Ref);
        EXPECT_EQ(sig.paramKind(3), SigKind::F64);
        EXPECT_EQ(sig.returnKind(), SigKind::Void);
    }
    {
        const auto r = MethodSig::parse("(J)J", false);
        ASSERT_TRUE(r.is_ok());
        const MethodSig& sig = r.unwrap();
        EXPECT_FALSE(sig.isStatic());
        EXPECT_EQ(sig.argCount(), 2u);        // this + long
        EXPECT_EQ(sig.paramSlotCount(), 3u);  // this + 双槽
        EXPECT_EQ(sig.returnKind(), SigKind::I64);
    }
    {
        const auto r = MethodSig::parse("()[Ljava/lang/String;", true);
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.unwrap().returnKind(), SigKind::Ref);
    }
    for (const char* bad : {"I", "(I", "()", "(I)VV", "()X", "(Lx;", "(V)V"}) {
        const auto r = MethodSig::parse(bad, true);
        EXPECT_TRUE(r.is_err()) << bad;
        EXPECT_EQ(r.unwrap_err(), SigError::Malformed) << bad;
    }
    EXPECT_TRUE(mjbase::asm_evaluator::isCat2(SigKind::I64));
    EXPECT_TRUE(mjbase::asm_evaluator::isCat2(SigKind::F64));
    EXPECT_FALSE(mjbase::asm_evaluator::isCat2(SigKind::Ref));
}
