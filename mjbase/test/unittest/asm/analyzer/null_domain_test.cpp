/// @file        null_domain_test.cpp
/// @brief       nullness 域测试：四态合并代数、产点事实（aconst_null/ldc
///        字符串）、分支合并与 MethodSig 播种（含双槽）。
///
/// 合并代数为 NullabilityUtil::merge（NotNull+Null→Nullable 族）；
/// 产点事实为 NullabilityInterpreter::newOperation。类型载荷
/// 与 IFNULL/IFNONNULL 分支细化未迁（理由见域头注释）。
#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "jbc/jbc.hpp"

#include "mjbase/asm/analyzer/null_domain.hpp"

using jbc::analysis::MethodSig;
using jbc::core::ConstantPool;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_analyzer::NullDomain;
using mjbase::asm_analyzer::NullResult;
using mjbase::asm_analyzer::NullState;
using mjbase::asm_analyzer::NullValue;
using mjbase::asm_analyzer::analyzeNullness;

namespace {

InsnId appendOp(CodeDom& d, Op o)
{
    jbc::ir::Insn in;
    in.op = o;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 追加 CP 操作数指令（new/ldc 等）。
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
// 合并代数（直接对域钩子断言，移植旧 NullabilityUtil::merge）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, NullMergeAlgebraFollowsJoinSemantics)
{
    ConstantPool cp;
    NullDomain dom(cp);

    // 同事实保持。
    EXPECT_EQ(dom.merge(NullValue::of(NullState::Null), NullValue::of(NullState::Null)).s,
              NullState::Null);
    EXPECT_EQ(dom.merge(NullValue::of(NullState::NotNull), NullValue::of(NullState::NotNull)).s,
              NullState::NotNull);

    // NotNull 与 Null 汇合 → Nullable（旧代数核心）。
    EXPECT_EQ(dom.merge(NullValue::of(NullState::NotNull), NullValue::of(NullState::Null)).s,
              NullState::Nullable);
    EXPECT_EQ(dom.merge(NullValue::of(NullState::Null), NullValue::of(NullState::NotNull)).s,
              NullState::Nullable);

    // Nullable 吸收事实。
    EXPECT_EQ(dom.merge(NullValue::of(NullState::Nullable), NullValue::of(NullState::Null)).s,
              NullState::Nullable);
    EXPECT_EQ(dom.merge(NullValue::of(NullState::NotNull), NullValue::of(NullState::Nullable)).s,
              NullState::Nullable);

    // Unknown 是顶：未知产点吞掉一切事实（对旧 UNKNOWN 单位元语义的显式
    // 重排——引擎约定 top 必须吸收，数据流层面可靠）。
    EXPECT_EQ(dom.merge(NullValue::of(NullState::Unknown), NullValue::of(NullState::Null)).s,
              NullState::Unknown);
    EXPECT_EQ(dom.merge(NullValue::of(NullState::Nullable), NullValue::of(NullState::Unknown)).s,
              NullState::Unknown);

    // 宽度并入 join：cat2 冲突后双槽宽保留。
    const NullValue wide = dom.merge(NullValue::of(NullState::Null, 2),
                                     NullValue::of(NullState::NotNull, 2));
    EXPECT_EQ(wide.s, NullState::Nullable);
    EXPECT_EQ(wide.slots, 2);
}

// ---------------------------------------------------------------------------
// 方法体级事实
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, NullAconstNullGivesNullFact)
{
    // aconst_null; astore_1; aload_1; ifnull L; L: return
    // → ifnull 入口栈顶/local1 = Null（事实穿透 store→load）。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ACONST_NULL);
    const LabelId end = newLbl(dom, i0);
    appendOp(dom, Op::ASTORE_1);
    const InsnId ld = appendOp(dom, Op::ALOAD_1);
    const InsnId ifn = appendBr(dom, Op::IFNULL, end);
    const InsnId ret = appendOp(dom, Op::RETURN);
    bindLbl(dom, end, ret);

    ConstantPool cp;
    NullResult r = analyzeNullness(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());

    const NullValue* v = r.stackFromTop(ifn, 0);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->s, NullState::Null);
    const NullValue* l1 = r.localAt(ld, 1);
    ASSERT_NE(l1, nullptr);
    EXPECT_EQ(l1->s, NullState::Null);
}

TEST(MjAsmAnalyzerTest, NullLdcStringGivesNotNullFact)
{
    // ldc "s"; ifnonnull L; L: return → 栈顶 = NotNull（唯一 NotNull 产点）。
    ConstantPool cp;
    const ca::u32 str = cp.addString(cp.addUtf8("s"));
    CodeDom dom;
    const InsnId i0 = appendCp(dom, Op::LDC, str);
    const LabelId end = newLbl(dom, i0);
    const InsnId ifnn = appendBr(dom, Op::IFNONNULL, end);
    const InsnId ret = appendOp(dom, Op::RETURN);
    bindLbl(dom, end, ret);

    NullResult r = analyzeNullness(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const NullValue* v = r.stackFromTop(ifnn, 0);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->s, NullState::NotNull);
}

TEST(MjAsmAnalyzerTest, NullConflictingBranchMergeGivesNullable)
{
    // 两臂 aconst_null vs ldc "s" → 汇合点（pop 入口）= Nullable。
    ConstantPool cp;
    const ca::u32 str = cp.addString(cp.addUtf8("s"));
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_0);
    const LabelId lElse = newLbl(dom, i0);
    const LabelId lJoin = newLbl(dom, i0);
    appendBr(dom, Op::IFEQ, lElse);
    appendOp(dom, Op::ACONST_NULL);
    appendBr(dom, Op::GOTO, lJoin);
    const InsnId el = appendCp(dom, Op::LDC, str);
    const InsnId join = appendOp(dom, Op::POP);
    appendOp(dom, Op::RETURN);
    bindLbl(dom, lElse, el);
    bindLbl(dom, lJoin, join);

    NullResult r = analyzeNullness(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const NullValue* v = r.stackFromTop(join, 0);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->s, NullState::Nullable);
}

TEST(MjAsmAnalyzerTest, NullSameFactBothBranchesSurvives)
{
    // 两臂都 aconst_null → 汇合后仍是 Null。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_0);
    const LabelId lElse = newLbl(dom, i0);
    const LabelId lJoin = newLbl(dom, i0);
    appendBr(dom, Op::IFEQ, lElse);
    appendOp(dom, Op::ACONST_NULL);
    appendBr(dom, Op::GOTO, lJoin);
    const InsnId el = appendOp(dom, Op::ACONST_NULL);
    const InsnId join = appendOp(dom, Op::POP);
    appendOp(dom, Op::RETURN);
    bindLbl(dom, lElse, el);
    bindLbl(dom, lJoin, join);

    ConstantPool cp;
    NullResult r = analyzeNullness(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const NullValue* v = r.stackFromTop(join, 0);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->s, NullState::Null);
}

TEST(MjAsmAnalyzerTest, NullUnknownProducerAbsorbsFact)
{
    // 一臂 aconst_null（Null），另一臂 new（旧语义：无事实）→ 汇合 = Unknown。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_0);
    const LabelId lElse = newLbl(dom, i0);
    const LabelId lJoin = newLbl(dom, i0);
    appendBr(dom, Op::IFEQ, lElse);
    appendOp(dom, Op::ACONST_NULL);
    appendBr(dom, Op::GOTO, lJoin);
    const InsnId el = appendCp(dom, Op::NEW, 0);
    const InsnId join = appendOp(dom, Op::POP);
    appendOp(dom, Op::RETURN);
    bindLbl(dom, lElse, el);
    bindLbl(dom, lJoin, join);

    ConstantPool cp;
    NullResult r = analyzeNullness(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const NullValue* v = r.stackFromTop(join, 0);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->s, NullState::Unknown);
}

TEST(MjAsmAnalyzerTest, NullParameterSeedingLayoutsDualSlots)
{
    // 静态 (J)V：[long 参数（未知，双槽）, 底槽]。
    ConstantPool cp;
    CodeDom dom;
    const InsnId ret = appendOp(dom, Op::RETURN);
    NullResult r = analyzeNullness(cp, unitOf(std::move(dom)), MethodSig{true, "(J)V"});
    ASSERT_TRUE(r.ok());
    const NullValue* p0 = r.localAt(ret, 0);
    ASSERT_NE(p0, nullptr);
    EXPECT_EQ(p0->s, NullState::Unknown);
    EXPECT_EQ(p0->slots, 2);
    EXPECT_EQ(r.localAt(ret, 1), nullptr);   // cat2 底槽
}
