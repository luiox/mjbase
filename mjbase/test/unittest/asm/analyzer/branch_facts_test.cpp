/// @file        branch_facts_test.cpp
/// @brief       分支预测事实测试：一元/二元 int 条件、null 系、引用相等、
///        switch 键命中与未知操作数保守回退。
///
/// 行为断言移植自旧栈 reanalyzer_test 的分支修剪族（prunesTaken /
/// prunesFallthrough / keepsBothSuccessors / prunesKnownBinaryIntegerBranch /
/// prunesKnownNullBranch / prunesKnownLookupSwitchTarget /
/// keepsAllTableSwitchTargetsForUnknownKey / OutOfRangeSwitchIndex /
/// prunesBranchAfterJavaOverflowArithmetic）。旧栈断言的是引擎级可达性修剪
/// （predictor 返回 nullopt 时引擎保守入队双后继）；DomainEngine 无分支条件
/// 钩子，本套件对拍的是被移植语义的查询面——predictBranch 的走向事实。
#include <gtest/gtest.h>

#include <limits>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/analyzer/branch_facts.hpp"
#include "mjbase/asm/analyzer/const_domain.hpp"

using jbc::core::ConstantPool;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_analyzer::BranchFact;
using mjbase::asm_analyzer::propagateConstants;

namespace {

InsnId appendOp(CodeDom& d, Op o)
{
    jbc::ir::Insn in;
    in.op = o;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

InsnId appendImm(CodeDom& d, Op o, ca::i32 a)
{
    jbc::ir::Insn in;
    in.op = o;
    in.a = a;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

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

/// 追加 TABLESWITCH（[low, high] = case 键区间，target = default，
/// targets.size() 须等于 high - low + 1——append 按 b-a+1 校验）。
InsnId appendTableSwitch(CodeDom& d, ca::i32 low, ca::i32 high, LabelId dflt,
                         std::vector<LabelId> targets)
{
    jbc::ir::Insn in;
    in.op = Op::TABLESWITCH;
    in.a = low;
    in.b = high;
    in.target = dflt;
    in.targets = std::move(targets);
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 追加 LOOKUPSWITCH（target = default，keys/targets 平行数组）。
InsnId appendLookupSwitch(CodeDom& d, LabelId dflt, std::vector<ca::i32> keys,
                          std::vector<LabelId> targets)
{
    jbc::ir::Insn in;
    in.op = Op::LOOKUPSWITCH;
    in.target = dflt;
    in.keys = std::move(keys);
    in.targets = std::move(targets);
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

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

/// 构建方法体并跑常量传播，返回（pool + unit + 结果）供 predictBranch 查询。
///
/// pool 须随结果存活：Str 格值的内容比较（ACMP 判等）借用常量池，LDC 的
/// 载荷校验同样读池——带 CP 操作数的方法体须经 pool 重载把条目建进同一个
/// 池。成员声明顺序即初始化顺序：cp/unit 先于 frames。
struct ConstAnalysis
{
    ConstantPool cp;
    CodeUnit unit;
    mjbase::asm_analyzer::ConstResult frames;

    ConstAnalysis(ConstantPool pool, CodeDom dom)
        : cp(std::move(pool))
        , unit(unitOf(std::move(dom)))
        , frames(propagateConstants(cp, unit))
    {}
};

/// 无 CP 操作数的方法体：空池传播。
ConstAnalysis analyzeConst(CodeDom dom)
{
    return ConstAnalysis(ConstantPool{}, std::move(dom));
}

/// 带 CP 操作数的方法体：pool 即方法体指令引用的池。
ConstAnalysis analyzeConst(ConstantPool pool, CodeDom dom)
{
    return ConstAnalysis(std::move(pool), std::move(dom));
}

}   // namespace

// ---------------------------------------------------------------------------
// 一元 int 条件（移植 prunesTaken / prunesFallthrough / keepsBothSuccessors）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, BranchPredictsTakenUnaryWhenOperandZero)
{
    // iconst_0; ifeq L → 条件成立（0 == 0）：走跳转目标，顺序侧被剪。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_0);
    const LabelId taken = newLbl(dom, i0);
    const InsnId br = appendBr(dom, Op::IFEQ, taken);
    appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    const InsnId tl = appendOp(dom, Op::ICONST_2);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, taken, tl);

    ConstAnalysis a = analyzeConst(std::move(dom));
    ASSERT_TRUE(a.frames.ok());
    const mjbase::asm_analyzer::BranchPrediction p = predictBranch(a.unit, a.frames, br);
    EXPECT_EQ(p.fact, BranchFact::Taken);
}

TEST(MjAsmAnalyzerTest, BranchPredictsFallThroughUnaryWhenOperandNonZero)
{
    // iconst_1; ifeq L → 条件不成立：走顺序后继，跳转目标侧被剪。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_1);
    const LabelId taken = newLbl(dom, i0);
    const InsnId br = appendBr(dom, Op::IFEQ, taken);
    appendOp(dom, Op::ICONST_3);
    appendOp(dom, Op::IRETURN);
    const InsnId tl = appendOp(dom, Op::ICONST_4);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, taken, tl);

    ConstAnalysis a = analyzeConst(std::move(dom));
    ASSERT_TRUE(a.frames.ok());
    const auto p = predictBranch(a.unit, a.frames, br);
    EXPECT_EQ(p.fact, BranchFact::FallThrough);
}

TEST(MjAsmAnalyzerTest, BranchKeepsBothSuccessorsForUnknownOperand)
{
    // iload_0（无签名播种 → 空槽回退 Top）; ifeq L → 未知：保守双后继。
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ILOAD_0);
    const LabelId taken = newLbl(dom, i0);
    const InsnId br = appendBr(dom, Op::IFEQ, taken);
    appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    const InsnId tl = appendOp(dom, Op::ICONST_2);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, taken, tl);

    ConstAnalysis a = analyzeConst(std::move(dom));
    ASSERT_TRUE(a.frames.ok());
    const auto p = predictBranch(a.unit, a.frames, br);
    EXPECT_EQ(p.fact, BranchFact::Unknown);
}

// ---------------------------------------------------------------------------
// 二元 int 条件（移植 prunesKnownBinaryIntegerBranch）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, BranchPredictsBinaryIntComparison)
{
    ConstantPool cp;
    // 2, 2 if_icmpeq → Taken。
    {
        CodeDom dom;
        const InsnId i0 = appendOp(dom, Op::ICONST_2);
        const LabelId eq = newLbl(dom, i0);
        appendOp(dom, Op::ICONST_2);
        const InsnId br = appendBr(dom, Op::IF_ICMPEQ, eq);
        appendOp(dom, Op::ICONST_0);
        appendOp(dom, Op::IRETURN);
        const InsnId el = appendOp(dom, Op::ICONST_1);
        appendOp(dom, Op::IRETURN);
        bindLbl(dom, eq, el);

        ConstAnalysis a = analyzeConst(std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::Taken);
    }
    // 2, 1 if_icmpeq → FallThrough；1, 2 if_icmplt → Taken。
    {
        CodeDom dom;
        const InsnId i0 = appendOp(dom, Op::ICONST_2);
        const LabelId eq = newLbl(dom, i0);
        appendOp(dom, Op::ICONST_1);
        const InsnId br = appendBr(dom, Op::IF_ICMPEQ, eq);
        appendOp(dom, Op::ICONST_0);
        appendOp(dom, Op::IRETURN);
        const InsnId el = appendOp(dom, Op::ICONST_1);
        appendOp(dom, Op::IRETURN);
        bindLbl(dom, eq, el);

        ConstAnalysis a = analyzeConst(std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::FallThrough);
    }
    {
        CodeDom dom;
        const InsnId i0 = appendOp(dom, Op::ICONST_1);
        const LabelId lt = newLbl(dom, i0);
        appendOp(dom, Op::ICONST_2);
        const InsnId br = appendBr(dom, Op::IF_ICMPLT, lt);
        appendOp(dom, Op::ICONST_0);
        appendOp(dom, Op::IRETURN);
        const InsnId ll = appendOp(dom, Op::ICONST_1);
        appendOp(dom, Op::IRETURN);
        bindLbl(dom, lt, ll);

        ConstAnalysis a = analyzeConst(std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::Taken);
    }
}

// ---------------------------------------------------------------------------
// null 系（移植 prunesKnownNullBranch；String 常量为非空引用事实）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, BranchPredictsNullChecksOnReferenceConstants)
{
    // aconst_null; ifnull L; L: return → Taken；ifnonnull → FallThrough。
    {
        CodeDom dom;
        const InsnId i0 = appendOp(dom, Op::ACONST_NULL);
        const LabelId l = newLbl(dom, i0);
        const InsnId br = appendBr(dom, Op::IFNULL, l);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, l, ret);
        ConstAnalysis a = analyzeConst(std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::Taken);
    }
    {
        CodeDom dom;
        const InsnId i0 = appendOp(dom, Op::ACONST_NULL);
        const LabelId l = newLbl(dom, i0);
        const InsnId br = appendBr(dom, Op::IFNONNULL, l);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, l, ret);
        ConstAnalysis a = analyzeConst(std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::FallThrough);
    }
    // ldc "s"; ifnull L; L: return → FallThrough；ifnonnull → Taken
    // （字符串常量是非空引用事实）。CP 条目建进分析自持的池。
    {
        ConstantPool pool;
        const ca::u32 str = pool.addString(pool.addUtf8("s"));
        CodeDom dom;
        const InsnId i0 = appendCp(dom, Op::LDC, str);
        const LabelId l = newLbl(dom, i0);
        const InsnId br = appendBr(dom, Op::IFNULL, l);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, l, ret);
        ConstAnalysis a = analyzeConst(std::move(pool), std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::FallThrough);
    }
    {
        ConstantPool pool;
        const ca::u32 str = pool.addString(pool.addUtf8("s"));
        CodeDom dom;
        const InsnId i0 = appendCp(dom, Op::LDC, str);
        const LabelId l = newLbl(dom, i0);
        const InsnId br = appendBr(dom, Op::IFNONNULL, l);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, l, ret);
        ConstAnalysis a = analyzeConst(std::move(pool), std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::Taken);
    }
}

// ---------------------------------------------------------------------------
// switch（移植 prunesKnownLookupSwitchTarget / keepsAllTableSwitchTargets /
// OutOfRangeSwitchIndex）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, BranchPredictsLookupSwitchCase)
{
    // bipush 7; lookupswitch {1,7} → Case(1)；bipush 9 → Case(-1)（default）。
    // 目标块均为纯 RETURN（switch 弹键后栈空）。
    const auto build = [](ca::i32 key, InsnId* swOut) {
        CodeDom dom;
        const InsnId k = appendImm(dom, Op::BIPUSH, key);
        const LabelId dflt = newLbl(dom, k);
        const LabelId one = newLbl(dom, k);
        const LabelId seven = newLbl(dom, k);
        const InsnId sw = appendLookupSwitch(dom, dflt, {1, 7}, {one, seven});
        const InsnId dl = appendOp(dom, Op::RETURN);
        bindLbl(dom, dflt, dl);
        const InsnId ol = appendOp(dom, Op::RETURN);
        bindLbl(dom, one, ol);
        const InsnId sl = appendOp(dom, Op::RETURN);
        bindLbl(dom, seven, sl);
        *swOut = sw;
        return dom;
    };

    InsnId sw7{};
    ConstAnalysis a = analyzeConst(build(7, &sw7));
    ASSERT_TRUE(a.frames.ok());
    const auto hit = predictBranch(a.unit, a.frames, sw7);
    EXPECT_EQ(hit.fact, BranchFact::Case);
    EXPECT_EQ(hit.caseIndex, 1);

    InsnId sw9{};
    ConstAnalysis b = analyzeConst(build(9, &sw9));
    ASSERT_TRUE(b.frames.ok());
    const auto miss = predictBranch(b.unit, b.frames, sw9);
    EXPECT_EQ(miss.fact, BranchFact::Case);
    EXPECT_EQ(miss.caseIndex, -1);
}

TEST(MjAsmAnalyzerTest, BranchKeepsAllSwitchTargetsForUnknownKey)
{
    // iload_0（未知键）; tableswitch → Unknown：全部目标保守展开。
    CodeDom dom;
    const InsnId ld = appendOp(dom, Op::ILOAD_0);
    const LabelId dflt = newLbl(dom, ld);
    const LabelId zero = newLbl(dom, ld);
    const LabelId one = newLbl(dom, ld);
    const InsnId sw = appendTableSwitch(dom, 0, 1, dflt, {zero, one});
    const InsnId dl = appendOp(dom, Op::RETURN);
    bindLbl(dom, dflt, dl);
    const InsnId zl = appendOp(dom, Op::RETURN);
    bindLbl(dom, zero, zl);
    const InsnId ol = appendOp(dom, Op::RETURN);
    bindLbl(dom, one, ol);

    ConstAnalysis a = analyzeConst(std::move(dom));
    ASSERT_TRUE(a.frames.ok());
    EXPECT_EQ(predictBranch(a.unit, a.frames, sw).fact, BranchFact::Unknown);
}

TEST(MjAsmAnalyzerTest, BranchPredictsDefaultForOutOfRangeTableKey)
{
    // bipush 9; tableswitch [0,1] → 越界键走 default（Case(-1)）。
    CodeDom dom;
    const InsnId k = appendImm(dom, Op::BIPUSH, 9);
    const LabelId dflt = newLbl(dom, k);
    const LabelId zero = newLbl(dom, k);
    const LabelId one = newLbl(dom, k);
    const InsnId sw = appendTableSwitch(dom, 0, 1, dflt, {zero, one});
    const InsnId dl = appendOp(dom, Op::RETURN);
    bindLbl(dom, dflt, dl);
    const InsnId zl = appendOp(dom, Op::RETURN);
    bindLbl(dom, zero, zl);
    const InsnId ol = appendOp(dom, Op::RETURN);
    bindLbl(dom, one, ol);

    ConstAnalysis a = analyzeConst(std::move(dom));
    ASSERT_TRUE(a.frames.ok());
    const auto p = predictBranch(a.unit, a.frames, sw);
    EXPECT_EQ(p.fact, BranchFact::Case);
    EXPECT_EQ(p.caseIndex, -1);
}

// ---------------------------------------------------------------------------
// 溢出算术喂分支（移植 prunesBranchAfterJavaOverflowArithmetic）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, BranchDecidesAfterOverflowArithmetic)
{
    // ldc MAX; iconst_1; iadd（环绕 → MIN）; iflt → Taken。
    ConstantPool pool;
    const ca::u32 maxI = pool.addInteger((std::numeric_limits<ca::i32>::max)());
    CodeDom dom;
    const InsnId i0 = appendCp(dom, Op::LDC, maxI);
    const LabelId neg = newLbl(dom, i0);
    appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IADD);
    const InsnId br = appendBr(dom, Op::IFLT, neg);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);
    const InsnId nl = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, neg, nl);

    ConstAnalysis a = analyzeConst(std::move(pool), std::move(dom));
    ASSERT_TRUE(a.frames.ok());
    EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::Taken);
}

// ---------------------------------------------------------------------------
// 引用相等（ACMP 家族：Null/String 常量按 intern 语义判等）
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, BranchFoldsReferenceEqualityOnConstants)
{
    // null vs null：acmpeq → Taken。
    {
        CodeDom dom;
        appendOp(dom, Op::ACONST_NULL);
        appendOp(dom, Op::ACONST_NULL);
        const LabelId end = newLbl(dom, dom.first());
        const InsnId br = appendBr(dom, Op::IF_ACMPEQ, end);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, end, ret);
        ConstAnalysis a = analyzeConst(std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::Taken);
    }
    // null vs "x"：acmpeq → FallThrough。
    {
        ConstantPool pool;
        const ca::u32 sx = pool.addString(pool.addUtf8("x"));
        CodeDom dom;
        appendOp(dom, Op::ACONST_NULL);
        appendCp(dom, Op::LDC, sx);
        const LabelId end = newLbl(dom, dom.first());
        const InsnId br = appendBr(dom, Op::IF_ACMPEQ, end);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, end, ret);
        ConstAnalysis a = analyzeConst(std::move(pool), std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::FallThrough);
    }
    // "x" vs "x"（同内容异 CP 条目）：acmpeq → Taken（JVMS 5.1 intern 语义）。
    {
        ConstantPool pool;
        const ca::u32 sx = pool.addString(pool.addUtf8("x"));
        const ca::u32 sx2 = pool.addString(pool.addUtf8("x"));
        CodeDom dom;
        appendCp(dom, Op::LDC, sx);
        appendCp(dom, Op::LDC, sx2);
        const LabelId end = newLbl(dom, dom.first());
        const InsnId br = appendBr(dom, Op::IF_ACMPEQ, end);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, end, ret);
        ConstAnalysis a = analyzeConst(std::move(pool), std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::Taken);
    }
    // "x" vs "y"：acmpeq → FallThrough；acmpne → Taken。
    {
        ConstantPool pool;
        const ca::u32 sx = pool.addString(pool.addUtf8("x"));
        const ca::u32 sy = pool.addString(pool.addUtf8("y"));
        CodeDom dom;
        appendCp(dom, Op::LDC, sx);
        appendCp(dom, Op::LDC, sy);
        const LabelId end = newLbl(dom, dom.first());
        const InsnId brEq = appendBr(dom, Op::IF_ACMPEQ, end);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, end, ret);
        ConstAnalysis a = analyzeConst(std::move(pool), std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, brEq).fact, BranchFact::FallThrough);
    }
    {
        ConstantPool pool;
        const ca::u32 sx = pool.addString(pool.addUtf8("x"));
        const ca::u32 sy = pool.addString(pool.addUtf8("y"));
        CodeDom dom;
        appendCp(dom, Op::LDC, sx);
        appendCp(dom, Op::LDC, sy);
        const LabelId end = newLbl(dom, dom.first());
        const InsnId brNe = appendBr(dom, Op::IF_ACMPNE, end);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, end, ret);
        ConstAnalysis a = analyzeConst(std::move(pool), std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, brNe).fact, BranchFact::Taken);
    }
    // 未知操作数（aload_0 无签名播种 → Top）→ Unknown。
    {
        CodeDom dom;
        appendOp(dom, Op::ALOAD_0);
        appendOp(dom, Op::ACONST_NULL);
        const LabelId end = newLbl(dom, dom.first());
        const InsnId br = appendBr(dom, Op::IF_ACMPEQ, end);
        const InsnId ret = appendOp(dom, Op::RETURN);
        bindLbl(dom, end, ret);
        ConstAnalysis a = analyzeConst(std::move(dom));
        ASSERT_TRUE(a.frames.ok());
        EXPECT_EQ(predictBranch(a.unit, a.frames, br).fact, BranchFact::Unknown);
    }
}

// ---------------------------------------------------------------------------
// 非预测形态
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, BranchUnknownForUnconditionalAndForeignHandles)
{
    CodeDom dom;
    const InsnId i0 = appendOp(dom, Op::ICONST_1);
    const LabelId l = newLbl(dom, i0);
    const InsnId go = appendBr(dom, Op::GOTO, l);
    appendOp(dom, Op::RETURN);
    bindLbl(dom, l, appendOp(dom, Op::RETURN));

    ConstAnalysis a = analyzeConst(std::move(dom));
    ASSERT_TRUE(a.frames.ok());
    // GOTO 是无条件控制流，不是预测问题。
    EXPECT_EQ(predictBranch(a.unit, a.frames, go).fact, BranchFact::Unknown);
    // 无效句柄同样 Unknown。
    EXPECT_EQ(predictBranch(a.unit, a.frames, InsnId{}).fact, BranchFact::Unknown);
}
