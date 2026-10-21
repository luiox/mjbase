/// @file        origin_domain_test.cpp
/// @brief       来源追踪域测试：store→load 穿透、分支合并退化、运算结果来源、
///        MethodSig 入参身份（含 long/double 双槽）与死代码探测。
///
/// 断言覆盖参数身份保持、冲突合并丢事实、经 store/load 的来源保持。
#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "jbc/jbc.hpp"

#include "mjbase/asm/analyzer/origin_domain.hpp"

using jbc::analysis::MethodSig;
using jbc::core::ConstantPool;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_analyzer::OriginDomain;
using mjbase::asm_analyzer::OriginResult;
using mjbase::asm_analyzer::OriginValue;
using mjbase::asm_analyzer::traceOrigins;

namespace {

InsnId appendOp(CodeDom& d, Op o)
{
    jbc::ir::Insn in;
    in.op = o;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 追加 CP 操作数指令；new 的类引用操作数本域不读（引擎亦不查），传 0 即可。
InsnId appendCp(CodeDom& d, Op o, ca::u32 cpIdx)
{
    jbc::ir::Insn in;
    in.op = o;
    in.cp = cpIdx;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

InsnId appendBr(CodeDom& d, Op o, LabelId t)
{
    jbc::ir::Insn in;
    in.op = o;
    in.target = t;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 新建标签：append 校验要求分支目标锚定在存活指令上，先锚到占位指令，
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

CodeUnit unitOf(CodeDom&& d)
{
    CodeUnit u;
    u.dom = std::move(d);
    return u;
}

}   // namespace

// ---------------------------------------------------------------------------
// 格代数（直接对域钩子断言）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, OriginMergeKeepsAgreeingFactOnly)
{
    OriginDomain dom;
    const jbc::ir::InsnId a{1, 0, 0};
    const jbc::ir::InsnId b{1, 1, 0};

    // 同句柄保持。
    const OriginValue m1 = dom.merge(OriginValue::ofInsn(a), OriginValue::ofInsn(a));
    ASSERT_TRUE(m1.isInsn());
    EXPECT_TRUE(m1.insn == a);

    // 不同句柄 → Unknown。
    const OriginValue m2 = dom.merge(OriginValue::ofInsn(a), OriginValue::ofInsn(b));
    EXPECT_TRUE(m2.isUnknown());

    // 同入参槽位保持；不同槽位 → Unknown。
    const OriginValue m3 = dom.merge(OriginValue::ofParam(1), OriginValue::ofParam(1));
    ASSERT_TRUE(m3.isParam());
    EXPECT_EQ(m3.local, 1u);
    EXPECT_TRUE(dom.merge(OriginValue::ofParam(0), OriginValue::ofParam(1)).isUnknown());

    // 事实与入参/未知汇合 → Unknown。
    EXPECT_TRUE(dom.merge(OriginValue::ofInsn(a), OriginValue::ofParam(0)).isUnknown());
    EXPECT_TRUE(dom.merge(OriginValue::ofInsn(a), OriginValue::unknownOf()).isUnknown());

    // 双槽宽在冲突后保留（cat2 槽布局不塌）。
    const OriginValue m4 =
        dom.merge(OriginValue::ofInsn(a, 2), OriginValue::ofInsn(b, 2));
    EXPECT_TRUE(m4.isUnknown());
    EXPECT_EQ(m4.slots, 2);
}

// ---------------------------------------------------------------------------
// 方法体级来源追踪
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, OriginSurvivesStoreLoad)
{
    // new; astore_1; aload_1; ifnonnull L; L: return
    // → ifnonnull 入口栈顶的来源 = new 指令（穿透 store→load）。
    CodeDom dom;
    const InsnId nw = appendCp(dom, Op::NEW, 0);
    const LabelId end = newLbl(dom, nw);
    appendOp(dom, Op::ASTORE_1);
    appendOp(dom, Op::ALOAD_1);
    const InsnId ifnn = appendBr(dom, Op::IFNONNULL, end);
    const InsnId ret = appendOp(dom, Op::RETURN);
    bindLbl(dom, end, ret);

    ConstantPool cp;
    OriginResult r = traceOrigins(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const OriginValue* v = r.stackFromTop(ifnn, 0);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->isInsn());
    EXPECT_TRUE(v->insn == nw);
}

TEST(MjAsmAnalyzerTest, OriginOfArithmeticResultIsTheInstruction)
{
    // iconst_2; iconst_3; iadd; istore_0 → istore 入口栈顶来源 = iadd 指令。
    CodeDom dom;
    appendOp(dom, Op::ICONST_2);
    appendOp(dom, Op::ICONST_3);
    const InsnId add = appendOp(dom, Op::IADD);
    const InsnId st = appendOp(dom, Op::ISTORE_0);

    ConstantPool cp;
    OriginResult r = traceOrigins(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const OriginValue* v = r.stackFromTop(st, 0);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->isInsn());
    EXPECT_TRUE(v->insn == add);
}

TEST(MjAsmAnalyzerTest, OriginDifferentProducersMergeToUnknown)
{
    // 菱形：两臂各 new 一个对象 → 汇合点来源未知。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_0);
    const LabelId lElse = newLbl(dom, i0);
    const LabelId lJoin = newLbl(dom, i0);
    appendBr(dom, Op::IFEQ, lElse);
    appendCp(dom, Op::NEW, 0);
    appendBr(dom, Op::GOTO, lJoin);
    const InsnId el = appendCp(dom, Op::NEW, 0);
    const InsnId join = appendOp(dom, Op::ASTORE_1);
    bindLbl(dom, lElse, el);
    bindLbl(dom, lJoin, join);

    ConstantPool cp;
    OriginResult r = traceOrigins(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const OriginValue* v = r.stackFromTop(join, 0);
    ASSERT_NE(v, nullptr);
    EXPECT_TRUE(v->isUnknown());
}

TEST(MjAsmAnalyzerTest, OriginSingleDefinitionSurvivesBranchJoin)
{
    // iconst_5; istore_1; iload_1; ifeq Ljoin; nop; Ljoin: iload_1; ifeq E; return
    // → 定义支配两臂：local1/栈顶来源仍是 iconst_5。
    CodeDom dom;
    const InsnId def = appendOp(dom, Op::ICONST_5);
    const LabelId lJoin = newLbl(dom, def);
    const LabelId lEnd = newLbl(dom, def);
    appendOp(dom, Op::ISTORE_1);
    appendOp(dom, Op::ILOAD_1);
    appendBr(dom, Op::IFEQ, lJoin);
    appendOp(dom, Op::NOP);
    const InsnId ld = appendOp(dom, Op::ILOAD_1);
    const InsnId ife = appendBr(dom, Op::IFEQ, lEnd);
    const InsnId ret = appendOp(dom, Op::RETURN);
    bindLbl(dom, lJoin, ld);
    bindLbl(dom, lEnd, ret);

    ConstantPool cp;
    OriginResult r = traceOrigins(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());

    const OriginValue* l1 = r.localAt(ife, 1);
    ASSERT_NE(l1, nullptr);
    ASSERT_TRUE(l1->isInsn());
    EXPECT_TRUE(l1->insn == def);

    const OriginValue* top = r.stackFromTop(ife, 0);
    ASSERT_NE(top, nullptr);
    ASSERT_TRUE(top->isInsn());
    EXPECT_TRUE(top->insn == def);   // store→load 穿透且跨汇合点保持
}

// ---------------------------------------------------------------------------
// MethodSig 播种（入参身份，含 long/double 双槽）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, OriginParametersKeepLocalIdentity)
{
    // 静态 (II)I：iload_0; iload_1; iadd → iadd 入口栈 = [Param0, Param1]。
    CodeDom dom;
    appendOp(dom, Op::ILOAD_0);
    appendOp(dom, Op::ILOAD_1);
    const InsnId add = appendOp(dom, Op::IADD);
    appendOp(dom, Op::IRETURN);

    ConstantPool cp;
    OriginResult r = traceOrigins(cp, unitOf(std::move(dom)), MethodSig{true, "(II)I"});
    ASSERT_TRUE(r.ok());

    const OriginValue* bottom = r.stackFromTop(add, 1);
    const OriginValue* top = r.stackFromTop(add, 0);
    ASSERT_NE(bottom, nullptr);
    ASSERT_NE(top, nullptr);
    ASSERT_TRUE(bottom->isParam());
    ASSERT_TRUE(top->isParam());
    EXPECT_EQ(bottom->local, 0u);
    EXPECT_EQ(top->local, 1u);
}

TEST(MjAsmAnalyzerTest, OriginParameterSeedingLayoutsDualSlots)
{
    ConstantPool cp;

    // 静态 (J)V：[long 参数（双槽）, 底槽]。
    {
        CodeDom dom;
        const InsnId ret = appendOp(dom, Op::RETURN);
        OriginResult r = traceOrigins(cp, unitOf(std::move(dom)), MethodSig{true, "(J)V"});
        ASSERT_TRUE(r.ok());
        const OriginValue* p0 = r.localAt(ret, 0);
        ASSERT_NE(p0, nullptr);
        ASSERT_TRUE(p0->isParam());
        EXPECT_EQ(p0->local, 0u);
        EXPECT_EQ(p0->slots, 2);
        EXPECT_EQ(r.localAt(ret, 1), nullptr);   // cat2 底槽不承载域值
    }

    // 实例 (Ljava/lang/Object;J)V：[this=Param0, arg=Param1, long=Param2（双槽）, 底槽]。
    {
        CodeDom dom;
        const InsnId ret = appendOp(dom, Op::RETURN);
        OriginResult r =
            traceOrigins(cp, unitOf(std::move(dom)), MethodSig{false, "(Ljava/lang/Object;J)V"});
        ASSERT_TRUE(r.ok());
        const OriginValue* p0 = r.localAt(ret, 0);
        const OriginValue* p1 = r.localAt(ret, 1);
        const OriginValue* p2 = r.localAt(ret, 2);
        ASSERT_NE(p0, nullptr);
        ASSERT_NE(p1, nullptr);
        ASSERT_NE(p2, nullptr);
        EXPECT_TRUE(p0->isParam());
        EXPECT_TRUE(p1->isParam());
        EXPECT_TRUE(p2->isParam());
        EXPECT_EQ(p0->local, 0u);
        EXPECT_EQ(p1->local, 1u);
        EXPECT_EQ(p2->local, 2u);
        EXPECT_EQ(p2->slots, 2);
        EXPECT_EQ(r.localAt(ret, 3), nullptr);
    }
}

TEST(MjAsmAnalyzerTest, OriginLongLoadKeepsWideOrigin)
{
    // 静态 (J)J：lload_0; lreturn → lreturn 入口栈顶 = Param0（双槽）。
    CodeDom dom;
    appendOp(dom, Op::LLOAD_0);
    const InsnId ret = appendOp(dom, Op::LRETURN);

    ConstantPool cp;
    OriginResult r = traceOrigins(cp, unitOf(std::move(dom)), MethodSig{true, "(J)J"});
    ASSERT_TRUE(r.ok());
    const OriginValue* v = r.stackFromTop(ret, 0);
    ASSERT_NE(v, nullptr);
    ASSERT_TRUE(v->isParam());
    EXPECT_EQ(v->local, 0u);
    EXPECT_EQ(v->slots, 2);   // cat2 载荷：栈上值行 + 底槽行
}

TEST(MjAsmAnalyzerTest, OriginDeadCodeFrameIsUnreachable)
{
    // goto L; iconst_1（死代码）; L: return。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::NOP);
    const LabelId l = newLbl(dom, i0);
    appendBr(dom, Op::GOTO, l);
    const InsnId dead = appendOp(dom, Op::ICONST_1);
    const InsnId ret = appendOp(dom, Op::RETURN);
    bindLbl(dom, l, ret);

    ConstantPool cp;
    OriginResult r = traceOrigins(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    EXPECT_FALSE(r.reachable(dead));
    EXPECT_EQ(r.stackFromTop(dead, 0), nullptr);
    EXPECT_EQ(r.localAt(dead, 0), nullptr);
}
