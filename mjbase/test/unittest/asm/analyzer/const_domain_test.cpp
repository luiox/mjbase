/// @file        const_domain_test.cpp
/// @brief       常量传播域测试：格代数、JVM 折叠语义、store→load 穿透、
///        分支合并、MethodSig 播种（含 long/double 双槽）与句柄查询面。
///
/// 行为断言覆盖 JVM 环绕/移位掩码/MIN÷-1
/// 特例、冲突合并丢常量）与 libjbc domain_test（穿透与播种布局）。
///
/// long 折叠语义在域钩子层直接断言：DomainEngine v0.3.8 的 popValue 对 cat2
/// 读到的是底槽行（默认构造 V），宽度 2 的载荷经运算/存储会丢失——引擎级
/// 方法体测试只覆盖当前可观察的行为（入栈行查询、入参播种），不锚定该缺陷。
#include <gtest/gtest.h>
#include <cstdio>

#include <limits>
#include <string_view>
#include <utility>

#include "jbc/jbc.hpp"

#include "mjbase/asm/analyzer/const_domain.hpp"

using jbc::analysis::MethodSig;
using jbc::core::ConstantPool;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_analyzer::ConstDomain;
using mjbase::asm_analyzer::ConstLattice;
using mjbase::asm_analyzer::ConstResult;
using mjbase::asm_analyzer::propagateConstants;

namespace {

/// 追加无操作数指令（iconst_x / iadd / istore_n / return 等）。
InsnId appendOp(CodeDom& d, Op o)
{
    jbc::ir::Insn in;
    in.op = o;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 追加单立即数指令（bipush/sipush 等）。
InsnId appendImm(CodeDom& d, Op o, ca::i32 a)
{
    jbc::ir::Insn in;
    in.op = o;
    in.a = a;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 追加 CP 操作数指令（ldc/ldc_w/ldc2_w/new 等）。
InsnId appendCp(CodeDom& d, Op o, ca::u32 cpIdx)
{
    jbc::ir::Insn in;
    in.op = o;
    in.cp = cpIdx;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 追加分支（ifeq/goto/ifnonnull 等）；标签须先锚定在存活指令上。
InsnId appendBr(CodeDom& d, Op o, LabelId t)
{
    jbc::ir::Insn in;
    in.op = o;
    in.target = t;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 新建标签。append 校验要求分支目标锚定在存活指令上：先锚到占位指令，
/// 序列成型后再 bindLbl 移到真实边界。
LabelId newLbl(CodeDom& d, InsnId anchor)
{
    const auto r = d.createLabel(anchor);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

void bindLbl(CodeDom& d, LabelId l, InsnId at)
{
    EXPECT_TRUE(d.bindLabel(l, at).is_ok());
}

/// CodeDom → 域分析入参形态。
CodeUnit unitOf(CodeDom&& d)
{
    CodeUnit u;
    u.dom = std::move(d);
    return u;
}

}   // namespace

// ---------------------------------------------------------------------------
// 格代数（直接对域钩子断言，移植旧 mergeConflictingConstantsDropsKnownValue）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, ConstMergeAlgebraFollowsJoinSemantics)
{
    ConstantPool cp;
    ConstDomain dom(cp);

    // 同常量保持。
    const ConstLattice m1 = dom.merge(ConstLattice::ofInt(7), ConstLattice::ofInt(7));
    ASSERT_TRUE(m1.isConst());
    EXPECT_EQ(m1.asI64(), 7);

    // 不同常量 → Top（冲突合并丢已知值）。
    const ConstLattice m2 = dom.merge(ConstLattice::ofInt(1), ConstLattice::ofInt(2));
    EXPECT_TRUE(m2.isTop());

    // Bottom 是单位元（不可达来源不施加约束）。
    const ConstLattice m3 = dom.merge(ConstLattice::bottomOf(), ConstLattice::ofInt(5));
    ASSERT_TRUE(m3.isConst());
    EXPECT_EQ(m3.asI64(), 5);

    // Top 是吸收元（未知来源吞掉已知）。
    const ConstLattice m4 = dom.merge(ConstLattice::topOf(), ConstLattice::ofInt(5));
    EXPECT_TRUE(m4.isTop());

    // cat2 未知与 long 常量汇合 → Top 但双槽宽保留（cat2 槽布局不塌）。
    const ConstLattice m5 =
        dom.merge(ConstLattice::unknownOf(jbc::analysis::VKind::Long), ConstLattice::ofLong(5));
    EXPECT_TRUE(m5.isTop());
    EXPECT_EQ(m5.slots, 2);
}

TEST(MjAsmAnalyzerTest, ConstEqualComparesPayloadNotIdentity)
{
    ConstantPool cp;
    ConstDomain dom(cp);
    EXPECT_TRUE(dom.equal(ConstLattice::ofInt(3), ConstLattice::ofInt(3)));
    EXPECT_FALSE(dom.equal(ConstLattice::ofInt(3), ConstLattice::ofInt(4)));
    EXPECT_FALSE(dom.equal(ConstLattice::ofInt(1), ConstLattice::ofLong(1)));
    EXPECT_TRUE(dom.equal(ConstLattice::ofNull(), ConstLattice::ofNull()));
    // 同值 String 走内容比较（CP 内同值异条目视为同一常量）。
    const ca::u32 a = cp.addString(cp.addUtf8("x"));
    const ca::u32 b = cp.addString(cp.addUtf8("x"));
    EXPECT_NE(a, b);
    EXPECT_TRUE(dom.equal(ConstLattice::ofStr(a), ConstLattice::ofStr(b)));
    EXPECT_TRUE(dom.merge(ConstLattice::ofStr(a), ConstLattice::ofStr(b)).isStr());
}

// ---------------------------------------------------------------------------
// 折叠语义（移植旧 integerArithmeticUsesJvmOverflowSemantics /
// integerShiftDistanceUsesJvmMask / longArithmeticAndShiftUseJvmSemantics）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, ConstFoldsIntegerArithmetic)
{
    // iconst_2; iconst_3; iadd; istore_0 → istore 入口栈顶 = 5。
    CodeDom dom;
    appendOp(dom, Op::ICONST_2);
    appendOp(dom, Op::ICONST_3);
    appendOp(dom, Op::IADD);
    const InsnId st = appendOp(dom, Op::ISTORE_0);

    ConstantPool cp;
    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const ConstLattice* v = r.stackFromTop(st, 0);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->isInt());
    EXPECT_EQ(v->asI64(), 5);
}

TEST(MjAsmAnalyzerTest, ConstPropagatesThroughLocal)
{
    // iconst_2; istore_1; iload_1; iconst_3; iadd; istore_2
    // → iadd 入口栈 = [2, 3]（load 穿透）；istore_2 入口栈顶 = 5；
    //   local 1 在 iadd 入口仍是常量 2。
    CodeDom dom;
    appendOp(dom, Op::ICONST_2);
    const InsnId st1 = appendOp(dom, Op::ISTORE_1);
    const InsnId ld = appendOp(dom, Op::ILOAD_1);
    appendOp(dom, Op::ICONST_3);
    const InsnId add = appendOp(dom, Op::IADD);
    const InsnId st2 = appendOp(dom, Op::ISTORE_2);

    ConstantPool cp;
    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());

    const ConstLattice* a0 = r.stackFromTop(add, 0);
    const ConstLattice* a1 = r.stackFromTop(add, 1);
    ASSERT_NE(a0, nullptr);
    ASSERT_NE(a1, nullptr);
    EXPECT_EQ(a0->asI64(), 3);
    EXPECT_EQ(a1->asI64(), 2);

    const ConstLattice* top = r.stackFromTop(st2, 0);
    ASSERT_NE(top, nullptr);
    ASSERT_TRUE(top->isInt());
    EXPECT_EQ(top->asI64(), 5);

    const ConstLattice* l1 = r.localAt(add, 1);
    ASSERT_NE(l1, nullptr);
    ASSERT_TRUE(l1->isInt());
    EXPECT_EQ(l1->asI64(), 2);
    // 到达帧视角一致：load 指令入口的 local 1 也是常量 2。
    const ConstLattice* l1b = r.localAt(ld, 1);
    ASSERT_NE(l1b, nullptr);
    EXPECT_EQ(l1b->asI64(), 2);
    EXPECT_EQ(r.localAt(st1, 1), nullptr);    // 存储前 local 1 未触达
}

TEST(MjAsmAnalyzerTest, ConstIntegerArithmeticUsesJvmOverflowSemantics)
{
    // IADD MAX+1 → MIN。
    {
        ConstantPool cp;
        const ca::u32 maxI = cp.addInteger((std::numeric_limits<ca::i32>::max)());
        CodeDom dom;
        appendCp(dom, Op::LDC, maxI);
        appendOp(dom, Op::ICONST_1);
        appendOp(dom, Op::IADD);
        const InsnId st = appendOp(dom, Op::ISTORE_0);
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
        ASSERT_TRUE(r.ok());
        const ConstLattice* v = r.stackFromTop(st, 0);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(v->asI64(), (std::numeric_limits<ca::i32>::min)());
    }
    // IDIV MIN/-1 → MIN（JVM 特例）。
    {
        ConstantPool cp;
        const ca::u32 minI = cp.addInteger((std::numeric_limits<ca::i32>::min)());
        CodeDom dom;
        appendCp(dom, Op::LDC, minI);
        appendOp(dom, Op::ICONST_M1);
        appendOp(dom, Op::IDIV);
        const InsnId st = appendOp(dom, Op::ISTORE_0);
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
        ASSERT_TRUE(r.ok());
        const ConstLattice* v = r.stackFromTop(st, 0);
        ASSERT_NE(v, nullptr);
        ASSERT_TRUE(v->isInt());
        EXPECT_EQ(v->asI64(), (std::numeric_limits<ca::i32>::min)());
    }
    // IREM MIN/-1 → 0（JVM 特例）。
    {
        ConstantPool cp;
        const ca::u32 minI = cp.addInteger((std::numeric_limits<ca::i32>::min)());
        CodeDom dom;
        appendCp(dom, Op::LDC, minI);
        appendOp(dom, Op::ICONST_M1);
        appendOp(dom, Op::IREM);
        const InsnId st = appendOp(dom, Op::ISTORE_0);
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
        ASSERT_TRUE(r.ok());
        const ConstLattice* v = r.stackFromTop(st, 0);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(v->asI64(), 0);
    }
}

TEST(MjAsmAnalyzerTest, ConstShiftDistanceUsesJvmMask)
{
    // 1 << 32 → 1（int 移位量按 5 位屏蔽）。
    {
        CodeDom dom;
        appendOp(dom, Op::ICONST_1);
        appendImm(dom, Op::BIPUSH, 32);
        appendOp(dom, Op::ISHL);
        const InsnId st = appendOp(dom, Op::ISTORE_0);
        ConstantPool cp;
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
        ASSERT_TRUE(r.ok());
        const ConstLattice* v = r.stackFromTop(st, 0);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(v->asI64(), 1);
    }
    // -1 >>> 31 → 1。
    {
        ConstantPool cp;
        const ca::u32 minus1 = cp.addInteger(-1);
        CodeDom dom;
        appendCp(dom, Op::LDC, minus1);
        appendImm(dom, Op::BIPUSH, 31);
        appendOp(dom, Op::IUSHR);
        const InsnId st = appendOp(dom, Op::ISTORE_0);
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
        ASSERT_TRUE(r.ok());
        const ConstLattice* v = r.stackFromTop(st, 0);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(v->asI64(), 1);
    }
}

TEST(MjAsmAnalyzerTest, ConstLongFoldJvmSemanticsAtDomainHooks)
{
    // long 折叠在域钩子层直接断言（引擎 cat2 popValue 载荷缺陷不影响本层语义；
    // 移植旧 longArithmeticAndShiftUseJvmSemantics 的期望值）。
    ConstantPool cp;
    ConstDomain dom(cp);
    const jbc::ir::Insn dummy;
    const jbc::ir::InsnId dummyId{};

    // LADD MAX+1 → MIN（long 环绕）。
    ConstLattice v = dom.binary(Op::LADD, ConstLattice::ofLong((std::numeric_limits<ca::i64>::max)()),
                                ConstLattice::ofLong(1), jbc::analysis::VKind::Long, dummy,
                                dummyId);
    ASSERT_TRUE(v.isLong());
    EXPECT_EQ(v.asI64(), (std::numeric_limits<ca::i64>::min)());

    // LDIV MIN/-1 → MIN；LREM MIN/-1 → 0（JVM 特例）。
    v = dom.binary(Op::LDIV, ConstLattice::ofLong((std::numeric_limits<ca::i64>::min)()),
                   ConstLattice::ofLong(-1), jbc::analysis::VKind::Long, dummy, dummyId);
    ASSERT_TRUE(v.isLong());
    EXPECT_EQ(v.asI64(), (std::numeric_limits<ca::i64>::min)());
    v = dom.binary(Op::LREM, ConstLattice::ofLong((std::numeric_limits<ca::i64>::min)()),
                   ConstLattice::ofLong(-1), jbc::analysis::VKind::Long, dummy, dummyId);
    ASSERT_TRUE(v.isLong());
    EXPECT_EQ(v.asI64(), 0);

    // LDIV x/0 → Top（除零不折）。
    v = dom.binary(Op::LDIV, ConstLattice::ofLong(5), ConstLattice::ofLong(0),
                   jbc::analysis::VKind::Long, dummy, dummyId);
    EXPECT_TRUE(v.isTop());

    // 1L << 64 → 1（long 移位量按 6 位屏蔽；移位量是 int 操作数）。
    v = dom.binary(Op::LSHL, ConstLattice::ofLong(1), ConstLattice::ofInt(64),
                   jbc::analysis::VKind::Long, dummy, dummyId);
    ASSERT_TRUE(v.isLong());
    EXPECT_EQ(v.asI64(), 1);

    // LCMP 折叠为 int 三值。
    v = dom.binary(Op::LCMP, ConstLattice::ofLong(-1), ConstLattice::ofLong(1),
                   jbc::analysis::VKind::Int, dummy, dummyId);
    ASSERT_TRUE(v.isInt());
    EXPECT_EQ(v.asI64(), -1);

    // L2I 截断：0x100000005 → 5。
    v = dom.unary(Op::L2I, ConstLattice::ofLong(static_cast<ca::i64>(0x100000005LL)),
                  jbc::analysis::VKind::Int, dummy, dummyId);
    ASSERT_TRUE(v.isInt());
    EXPECT_EQ(v.asI64(), 5);
}

TEST(MjAsmAnalyzerTest, ConstTracksLongConstantOnStack)
{
    // ldc2_w 5; lstore_0 → lstore 入口栈顶（值行）= Long 5；紧邻的下一行是
    // cat2 底槽（不承载域值）。
    ConstantPool cp;
    const ca::u32 five = cp.addLong(5);
    CodeDom dom;
    appendCp(dom, Op::LDC2_W, five);
    const InsnId st = appendOp(dom, Op::LSTORE_0);

    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const ConstLattice* v = r.stackFromTop(st, 0);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->isLong());
    EXPECT_EQ(v->asI64(), 5);
    EXPECT_EQ(v->slots, 2);
    EXPECT_EQ(r.stackFromTop(st, 1), nullptr);   // 底槽行
}

TEST(MjAsmAnalyzerTest, ConstDivisionByZeroStaysUnknown)
{
    // iconst_1; iconst_0; idiv → 不折（运行时抛 ArithmeticException）。
    CodeDom dom;
    appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IDIV);
    const InsnId st = appendOp(dom, Op::ISTORE_0);

    ConstantPool cp;
    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const ConstLattice* v = r.stackFromTop(st, 0);
    ASSERT_NE(v, nullptr);
    EXPECT_TRUE(v->isTop());
}

TEST(MjAsmAnalyzerTest, ConstNarrowingConversionsFold)
{
    // ldc 0x123ABCD：i2b → 0xCD；i2c → 0xABCD；i2s → (i16)0xABCD。
    ConstantPool cp;
    const ca::u32 big = cp.addInteger(0x123ABCD);
    CodeDom dom;
    appendCp(dom, Op::LDC, big);
    appendOp(dom, Op::I2B);
    const InsnId stb = appendOp(dom, Op::ISTORE_0);
    appendCp(dom, Op::LDC, big);   // 每段独立装载（前段 ISTORE 已清栈）
    appendOp(dom, Op::I2C);
    const InsnId stc = appendOp(dom, Op::ISTORE_0);
    appendCp(dom, Op::LDC, big);
    appendOp(dom, Op::I2S);
    const InsnId sts = appendOp(dom, Op::ISTORE_0);

    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const ConstLattice* b = r.stackFromTop(stb, 0);
    const ConstLattice* c = r.stackFromTop(stc, 0);
    const ConstLattice* s = r.stackFromTop(sts, 0);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(b->asI64(), static_cast<ca::i32>(static_cast<ca::i8>(0xCD)));
    EXPECT_EQ(c->asI64(), 0xABCD);
    EXPECT_EQ(s->asI64(), static_cast<ca::i32>(static_cast<ca::i16>(0xABCD)));
}

TEST(MjAsmAnalyzerTest, ConstFloatFamilyNotFolded)
{
    // fconst_1; fconst_2; fadd → Top（本域不折浮点）。
    CodeDom dom;
    appendOp(dom, Op::FCONST_1);
    appendOp(dom, Op::FCONST_2);
    appendOp(dom, Op::FADD);
    const InsnId st = appendOp(dom, Op::FSTORE_0);

    ConstantPool cp;
    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const ConstLattice* v = r.stackFromTop(st, 0);
    ASSERT_NE(v, nullptr);
    EXPECT_TRUE(v->isTop());
    EXPECT_EQ(v->slots, 1);
}

// ---------------------------------------------------------------------------
// 引用常量（null / String）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, ConstTracksStringReferenceConstant)
{
    // ldc "abc"; astore_1; aload_1; ifnonnull L; L: return
    // → ifnonnull 入口栈顶/local1 = Str("abc")。
    ConstantPool cp;
    const ca::u32 str = cp.addString(cp.addUtf8("abc"));
    CodeDom dom;
    const InsnId i0 = appendCp(dom, Op::LDC, str);
    const LabelId end = newLbl(dom, i0);
    const InsnId st1 = appendOp(dom, Op::ASTORE_1);
    const InsnId ld1 = appendOp(dom, Op::ALOAD_1);
    const InsnId ifnn = appendBr(dom, Op::IFNONNULL, end);
    const InsnId ret = appendOp(dom, Op::RETURN);
    bindLbl(dom, end, ret);

    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());

    const ConstLattice* sv = r.stackFromTop(ifnn, 0);
    ASSERT_NE(sv, nullptr);
    ASSERT_TRUE(sv->isStr());
    EXPECT_EQ(r.strOf(*sv), std::string_view{"abc"});

    const ConstLattice* sl = r.localAt(ld1, 1);
    ASSERT_NE(sl, nullptr);
    EXPECT_TRUE(sl->isStr());
    EXPECT_EQ(r.localAt(st1, 1), nullptr);   // 存储前 local 1 未触达
}

TEST(MjAsmAnalyzerTest, ConstTracksNullConstant)
{
    // aconst_null; astore_2; aload_2; ifnull L; L: return → 栈顶 = Null。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ACONST_NULL);
    const LabelId end = newLbl(dom, i0);
    appendOp(dom, Op::ASTORE_2);
    appendOp(dom, Op::ALOAD_2);
    const InsnId ifn = appendBr(dom, Op::IFNULL, end);
    const InsnId ret = appendOp(dom, Op::RETURN);
    bindLbl(dom, end, ret);

    ConstantPool cp;
    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const ConstLattice* v = r.stackFromTop(ifn, 0);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->isNull());
}

// ---------------------------------------------------------------------------
// 分支合并
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, ConstConflictingBranchMergeDropsConstant)
{
    // iconst_0; ifeq Lelse; iconst_1; goto Ljoin; Lelse: iconst_2; Ljoin: istore_1
    // → 汇合点栈顶 = Top（1 与 2 冲突）。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_0);
    const LabelId lElse = newLbl(dom, i0);
    const LabelId lJoin = newLbl(dom, i0);
    appendBr(dom, Op::IFEQ, lElse);
    appendOp(dom, Op::ICONST_1);
    appendBr(dom, Op::GOTO, lJoin);
    const InsnId el = appendOp(dom, Op::ICONST_2);
    const InsnId join = appendOp(dom, Op::ISTORE_1);
    bindLbl(dom, lElse, el);
    bindLbl(dom, lJoin, join);

    ConstantPool cp;
    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const ConstLattice* v = r.stackFromTop(join, 0);
    ASSERT_NE(v, nullptr);
    EXPECT_TRUE(v->isTop());
}

TEST(MjAsmAnalyzerTest, ConstSameConstantBothBranchesSurvivesMerge)
{
    // 两臂各装 7（不同指令：bipush 7 ×2）→ 汇合后仍是 7（按载荷合并，非按指令）。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_0);
    const LabelId lElse = newLbl(dom, i0);
    const LabelId lJoin = newLbl(dom, i0);
    appendBr(dom, Op::IFEQ, lElse);
    appendImm(dom, Op::BIPUSH, 7);
    appendBr(dom, Op::GOTO, lJoin);
    const InsnId el = appendImm(dom, Op::BIPUSH, 7);
    const InsnId join = appendOp(dom, Op::ISTORE_1);
    bindLbl(dom, lElse, el);
    bindLbl(dom, lJoin, join);

    ConstantPool cp;
    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const ConstLattice* v = r.stackFromTop(join, 0);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->isInt());
    EXPECT_EQ(v->asI64(), 7);
}

// ---------------------------------------------------------------------------
// MethodSig 播种（含 long/double 双槽）与查询面
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, ConstParameterSeedingLayoutsDualSlots)
{
    ConstantPool cp;

    // 静态 (IJ)V：[int, long(2), 底槽] —— long 占双槽，槽 2 是底槽。
    {
        CodeDom dom;
        const InsnId ret = appendOp(dom, Op::RETURN);
        ConstResult r =
            propagateConstants(cp, unitOf(std::move(dom)), MethodSig{true, "(IJ)V"});
        ASSERT_TRUE(r.ok());
        ASSERT_TRUE(r.reachable(ret));
        const ConstLattice* p0 = r.localAt(ret, 0);
        const ConstLattice* p1 = r.localAt(ret, 1);
        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);
        EXPECT_TRUE(p0->isTop());
        EXPECT_EQ(p0->slots, 1);
        EXPECT_TRUE(p1->isTop());
        EXPECT_EQ(p1->slots, 2);
        EXPECT_EQ(r.localAt(ret, 2), nullptr);   // cat2 底槽
        EXPECT_EQ(r.localAt(ret, 3), nullptr);   // 越界
    }

    // 实例 (Ljava/lang/String;J)V：[this, str, long(2), 底槽]。
    {
        CodeDom dom;
        const InsnId ret = appendOp(dom, Op::RETURN);
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)),
                                           MethodSig{false, "(Ljava/lang/String;J)V"});
        ASSERT_TRUE(r.ok());
        const ConstLattice* p0 = r.localAt(ret, 0);
        const ConstLattice* p1 = r.localAt(ret, 1);
        const ConstLattice* p2 = r.localAt(ret, 2);
        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);
        ASSERT_NE(p2, nullptr);
        EXPECT_EQ(p0->slots, 1);   // this
        EXPECT_EQ(p1->slots, 1);   // String
        EXPECT_EQ(p2->slots, 2);   // long
        EXPECT_EQ(r.localAt(ret, 3), nullptr);
    }

    // 无 sig 重载：不播种（local 0 未触达）。
    {
        CodeDom dom;
        const InsnId ret = appendOp(dom, Op::RETURN);
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
        ASSERT_TRUE(r.ok());
        EXPECT_EQ(r.localAt(ret, 0), nullptr);
    }
}

TEST(MjAsmAnalyzerTest, ConstDeadCodeFrameIsUnreachable)
{
    // iconst_1; istore_0; goto L; iconst_2（死代码）; L: return。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_1);
    const LabelId l = newLbl(dom, i0);
    appendOp(dom, Op::ISTORE_0);
    appendBr(dom, Op::GOTO, l);
    const InsnId dead = appendOp(dom, Op::ICONST_2);
    const InsnId ret = appendOp(dom, Op::RETURN);
    bindLbl(dom, l, ret);

    ConstantPool cp;
    ConstResult r = propagateConstants(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.reachable(dead));
    EXPECT_EQ(r.stackFromTop(dead, 0), nullptr);
    EXPECT_EQ(r.localAt(dead, 0), nullptr);
    EXPECT_TRUE(r.reachable(ret));
}

// ---------------------------------------------------------------------------
// 入参常量种子（移植旧 normalizesShortArgumentBeforeIntegerArithmetic /
// normalizesCharArgumentBeforeIntegerArithmetic：入参值参与 int 算术）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, ConstSeededArgumentFeedsIntegerArithmetic)
{
    // 移植旧 addOne 用例族：iload_0; iconst_1; iadd; istore_1，入参种子 41
    // → iadd 结果 42（IRETURN 前栈顶）。按 short/char 归一
    // （normalizeIntValue），新格层 int 宽度只有一类 Int 载荷，(S)/(C)/(I)
    // 三种描述符同一种子语义。
    for (const char* desc : {"(S)I", "(C)I", "(I)I"}) {
        CodeDom dom;
        appendOp(dom, Op::ILOAD_0);
        appendOp(dom, Op::ICONST_1);
        const InsnId add = appendOp(dom, Op::IADD);
        const InsnId st = appendOp(dom, Op::ISTORE_1);

        ConstantPool cp;
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)), MethodSig{true, desc},
                                           {ConstLattice::ofInt(41)});
        ASSERT_TRUE(r.ok()) << "desc=" << desc;

        // iadd 入口操作数序：栈顶 iconst_1，其下是入参种子（种子按 int 参与）。
        const ConstLattice* rhs = r.stackFromTop(add, 0);
        const ConstLattice* lhs = r.stackFromTop(add, 1);
        ASSERT_NE(rhs, nullptr) << "desc=" << desc;
        ASSERT_NE(lhs, nullptr) << "desc=" << desc;
        ASSERT_TRUE(rhs->isInt()) << "desc=" << desc;
        ASSERT_TRUE(lhs->isInt()) << "desc=" << desc;
        EXPECT_EQ(rhs->asI64(), 1) << "desc=" << desc;
        EXPECT_EQ(lhs->asI64(), 41) << "desc=" << desc;

        // istore_1 入口栈顶 = 和 42。
        const ConstLattice* v = r.stackFromTop(st, 0);
        ASSERT_NE(v, nullptr) << "desc=" << desc;
        ASSERT_TRUE(v->isInt()) << "desc=" << desc;
        EXPECT_EQ(v->asI64(), 42) << "desc=" << desc;
    }
}

TEST(MjAsmAnalyzerTest, ConstSeededArgumentsLayoutSlotsAndFallbackToTop)
{
    ConstantPool cp;

    // (IJ)V 种子 {Int 3, Long 7}：local0 = Int 3；local1 = Long 7 双槽；
    // local2 是 cat2 底槽（不承载域值）。
    {
        CodeDom dom;
        const InsnId ret = appendOp(dom, Op::RETURN);
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)), MethodSig{true, "(IJ)V"},
                                           {ConstLattice::ofInt(3), ConstLattice::ofLong(7)});
        ASSERT_TRUE(r.ok());
        const ConstLattice* p0 = r.localAt(ret, 0);
        const ConstLattice* p1 = r.localAt(ret, 1);
        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);
        ASSERT_TRUE(p0->isInt());
        EXPECT_EQ(p0->asI64(), 3);
        ASSERT_TRUE(p1->isLong());
        EXPECT_EQ(p1->asI64(), 7);
        EXPECT_EQ(p1->slots, 2);
        EXPECT_EQ(r.localAt(ret, 2), nullptr);   // cat2 底槽
    }

    // 种子不足：未给出的参数仍为 Top（(II)V 只给第一个参数种子）。
    {
        CodeDom dom;
        const InsnId ret = appendOp(dom, Op::RETURN);
        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)), MethodSig{true, "(II)V"},
                                           {ConstLattice::ofInt(3)});
        ASSERT_TRUE(r.ok());
        const ConstLattice* p0 = r.localAt(ret, 0);
        const ConstLattice* p1 = r.localAt(ret, 1);
        ASSERT_NE(p0, nullptr);
        ASSERT_TRUE(p0->isInt());
        EXPECT_EQ(p0->asI64(), 3);
        ASSERT_NE(p1, nullptr);
        EXPECT_TRUE(p1->isTop());
    }

    // 种子经 load 参与算术：iload_0; iload_0; iadd; istore_1
    // → iadd 入口两行都是种子 41；istore_1 入口栈顶 = 82。
    {
        CodeDom dom;
        appendOp(dom, Op::ILOAD_0);
        appendOp(dom, Op::ILOAD_0);
        const InsnId add = appendOp(dom, Op::IADD);
        const InsnId st = appendOp(dom, Op::ISTORE_1);

        ConstResult r = propagateConstants(cp, unitOf(std::move(dom)), MethodSig{true, "(I)V"},
                                           {ConstLattice::ofInt(41)});
        ASSERT_TRUE(r.ok());
        const ConstLattice* rhs = r.stackFromTop(add, 0);
        const ConstLattice* lhs = r.stackFromTop(add, 1);
        ASSERT_NE(rhs, nullptr);
        ASSERT_NE(lhs, nullptr);
        ASSERT_TRUE(rhs->isInt());
        ASSERT_TRUE(lhs->isInt());
        EXPECT_EQ(rhs->asI64(), 41);
        EXPECT_EQ(lhs->asI64(), 41);

        const ConstLattice* v = r.stackFromTop(st, 0);
        ASSERT_NE(v, nullptr);
        ASSERT_TRUE(v->isInt());
        EXPECT_EQ(v->asI64(), 82);
    }
}

// ---------------------------------------------------------------------------
// fake-try 处理器汇合（引擎 handler 种子时序回归）：handler 落在直链中部、
// 真实入边栈更深且晚于异常种子到达。种子抢先单跑的旧口径下，handler 体以
// [exc] 单槽残帧执行，iand 双操作数当场 Underflow，propagateConstants 整体
// 失败；待并参与者口径（verifier 语义：入口帧 = 全部入边 join）下种子与真
// 实边同权汇合——槽位布局保持、值被吸收为 Top，后续可折点照常出常量。
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, FakeTryHandlerMergeKeepsAnalysisAlive)
{
    ConstantPool cp;
    CodeDom dom;
    // 入口块：iconst_1; goto 深路径
    const InsnId i1 = appendOp(dom, Op::ICONST_1);
    const LabelId lDeep = newLbl(dom, i1);
    appendBr(dom, Op::GOTO, lDeep);
    // 深路径块：iconst_2; goto handler——正常边晚于种子处理到达 handler。
    const InsnId i2 = appendOp(dom, Op::ICONST_2);
    const LabelId lHandler = newLbl(dom, i2);
    appendBr(dom, Op::GOTO, lHandler);
    // handler 体：istore_0; iconst_m1; iand; istore_1; return。
    const InsnId st0 = appendOp(dom, Op::ISTORE_0);
    const InsnId pushM1 = appendOp(dom, Op::ICONST_M1);
    const InsnId andInsn = appendOp(dom, Op::IAND);
    appendOp(dom, Op::ISTORE_1);
    appendOp(dom, Op::RETURN);
    bindLbl(dom, lDeep, i2);
    bindLbl(dom, lHandler, st0);

    // try [入口, handler)；catch any。fake-try：覆盖范围无任何可抛指令。
    const LabelId lStart = newLbl(dom, i1);
    const LabelId lEnd = newLbl(dom, st0);

    CodeUnit u = unitOf(std::move(dom));
    u.exc.push_back({lStart, lEnd, lHandler, 0});

    ConstResult r = propagateConstants(cp, u);
    ASSERT_TRUE(r.ok());

    // handler 入口帧 = 异常种子(未知引用) ⊔ 正常边 [Int1, Int2]：两行槽位
    // 齐，值并成 Top（宽度 1 槽保持）。
    ASSERT_TRUE(r.reachable(st0));
    const ConstLattice* top = r.stackFromTop(st0, 0);
    ASSERT_NE(top, nullptr);
    EXPECT_TRUE(top->isTop());
    const ConstLattice* below = r.stackFromTop(st0, 1);
    ASSERT_NE(below, nullptr);
    EXPECT_TRUE(below->isTop());

    // 汇合吸收不阻断后续可折点：iand 入口栈顶仍是 iconst_m1 的 -1。
    ASSERT_TRUE(r.reachable(andInsn));
    const ConstLattice* rhs = r.stackFromTop(andInsn, 0);
    ASSERT_NE(rhs, nullptr);
    ASSERT_TRUE(rhs->isInt());
    EXPECT_EQ(rhs->asI64(), -1);
}
