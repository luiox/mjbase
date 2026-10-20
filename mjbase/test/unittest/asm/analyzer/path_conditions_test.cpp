/// @file        path_conditions_test.cpp
/// @brief       路径条件收集测试：常量分支、路径事实传播、汇合/回边保守性、
///       等式绑定判定与死分支修剪的栈效应保持。
///
/// 覆盖 issue #4 的验收面：恒真/恒假分支样例剔除；无路径条件（参数未播种、
/// 汇合稀释、回边稀释）时退回 Unknown 的回归门。
#include <gtest/gtest.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/analyzer/path_conditions.hpp"

using jbc::analysis::MethodSig;
using jbc::core::ConstantPool;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_analyzer::collectPathConditions;
using mjbase::asm_analyzer::PathConditionResult;
using mjbase::asm_analyzer::PathCmp;
using mjbase::asm_analyzer::PathConstraint;
using mjbase::asm_analyzer::PathTruth;
using mjbase::asm_analyzer::pruneDecidedBranches;

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

InsnId appendBr(CodeDom& d, Op o, LabelId t)
{
    jbc::ir::Insn in;
    in.op = o;
    in.target = t;
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

/// 静态方法 (I)V 的签名：local0 播种为符号参数 p0。
MethodSig intParamSig()
{
    MethodSig sig;
    sig.isStatic = true;
    sig.desc = "(I)V";
    sig.name = "f";
    return sig;
}

/// 方法体 + 路径事实；seed=false 走不播种重载（参数无符号身份）。
struct PathAnalysis
{
    ConstantPool cp;
    CodeUnit unit;
    PathConditionResult facts;

    explicit PathAnalysis(CodeDom dom, bool seed = true)
        : unit(unitOf(std::move(dom)))
    {
        if (seed) {
            const MethodSig sig = intParamSig();
            facts = collectPathConditions(cp, unit, &sig);
        }
        else {
            facts = collectPathConditions(cp, unit);
        }
    }
};

}   // namespace

// ---------------------------------------------------------------------------
// 常量分支：直接判定与不可行边剔除
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, PathConditionsDecideConstantTakenBranch)
{
    // iconst_0; ifeq L → 恒真；顺序侧块在事实分析中不可达。
    CodeDom dom;
    const InsnId c0 = appendOp(dom, Op::ICONST_0);
    const LabelId taken = newLbl(dom, c0);
    const InsnId branch = appendBr(dom, Op::IFEQ, taken);
    const InsnId fall = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    const InsnId target = appendOp(dom, Op::ICONST_2);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, taken, target);

    PathAnalysis a(std::move(dom));
    ASSERT_TRUE(a.facts.ok()) << mjbase::asm_analyzer::to_string(a.facts.error());
    EXPECT_EQ(a.facts.truthOf(branch), PathTruth::AlwaysTrue);
    EXPECT_TRUE(a.facts.blockReachable(a.facts.blockOf(target)));
    EXPECT_FALSE(a.facts.blockReachable(a.facts.blockOf(fall)));
}

TEST(MjAsmAnalyzerTest, PathConditionsDecideConstantFallThroughBranch)
{
    // iconst_1; ifeq L → 恒假。
    CodeDom dom;
    const InsnId c1 = appendOp(dom, Op::ICONST_1);
    const LabelId taken = newLbl(dom, c1);
    const InsnId branch = appendBr(dom, Op::IFEQ, taken);
    const InsnId fall = appendOp(dom, Op::ICONST_3);
    appendOp(dom, Op::IRETURN);
    const InsnId target = appendOp(dom, Op::ICONST_4);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, taken, target);

    PathAnalysis a(std::move(dom));
    ASSERT_TRUE(a.facts.ok());
    EXPECT_EQ(a.facts.truthOf(branch), PathTruth::AlwaysFalse);
    EXPECT_TRUE(a.facts.blockReachable(a.facts.blockOf(fall)));
    EXPECT_FALSE(a.facts.blockReachable(a.facts.blockOf(target)));
}

// ---------------------------------------------------------------------------
// 路径事实传播：同参数条件在分支体内可判定
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, PathConditionsRefineNestedBranchOnSameParam)
{
    // if (x == 1) { if (x == 1) ... } else { if (x == 1) ... }
    // 外层两臂把等式/不等式贴到边上，同一内层条件在两臂分别恒真/恒假。
    CodeDom dom;
    const InsnId l0 = appendOp(dom, Op::ILOAD_0);
    const InsnId c1Outer = appendOp(dom, Op::ICONST_1);
    const LabelId lEq1 = newLbl(dom, c1Outer);
    const InsnId brOuter = appendBr(dom, Op::IF_ICMPEQ, lEq1);

    appendOp(dom, Op::ILOAD_0);
    const InsnId c1Fall = appendOp(dom, Op::ICONST_1);
    const LabelId lEq1Fall = newLbl(dom, c1Fall);
    const InsnId brInnerFall = appendBr(dom, Op::IF_ICMPEQ, lEq1Fall);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);
    const InsnId deadTarget = appendImm(dom, Op::BIPUSH, 9);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, lEq1Fall, deadTarget);

    const InsnId l0Taken = appendOp(dom, Op::ILOAD_0);
    const InsnId c1Taken = appendOp(dom, Op::ICONST_1);
    const LabelId lEq1Taken = newLbl(dom, c1Taken);
    const InsnId brInnerTaken = appendBr(dom, Op::IF_ICMPEQ, lEq1Taken);
    appendOp(dom, Op::ICONST_2);
    appendOp(dom, Op::IRETURN);
    const InsnId liveTarget = appendOp(dom, Op::ICONST_3);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, lEq1Taken, liveTarget);
    bindLbl(dom, lEq1, l0Taken);

    PathAnalysis a(std::move(dom));
    ASSERT_TRUE(a.facts.ok());
    EXPECT_EQ(a.facts.truthOf(brOuter), PathTruth::Unknown);
    EXPECT_EQ(a.facts.truthOf(brInnerFall), PathTruth::AlwaysFalse);
    EXPECT_EQ(a.facts.truthOf(brInnerTaken), PathTruth::AlwaysTrue);
}

TEST(MjAsmAnalyzerTest, PathConditionsExposeTakenEdgeConstraint)
{
    // taken 边的约束可在目标块入口查询到：p0 == 1。
    CodeDom dom;
    const InsnId l0 = appendOp(dom, Op::ILOAD_0);
    const InsnId c1 = appendOp(dom, Op::ICONST_1);
    const LabelId lEq1 = newLbl(dom, c1);
    appendBr(dom, Op::IF_ICMPEQ, lEq1);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);
    const InsnId target = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, lEq1, target);

    PathAnalysis a(std::move(dom));
    ASSERT_TRUE(a.facts.ok());
    const std::vector<PathConstraint>& facts =
        a.facts.constraintsAt(a.facts.blockOf(target));
    ASSERT_EQ(facts.size(), 1u);
    EXPECT_EQ(facts[0].cmp, PathCmp::Eq);
    ASSERT_NE(facts[0].lhs, nullptr);
    ASSERT_NE(facts[0].rhs, nullptr);
    EXPECT_EQ(facts[0].lhs->kind(), mjbase::expr::NodeKind::Var);
    EXPECT_EQ(facts[0].rhs->kind(), mjbase::expr::NodeKind::Const);
}

// ---------------------------------------------------------------------------
// 回归门：域信息不足时必须 Unknown
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, PathConditionsKeepMergeBranchUnknown)
{
    // 菱形汇合：一侧 x=5，另一侧 x 未变；汇合后 x 的符号值退化为未知，
    // 再度比较 x==0 不可判定。
    CodeDom dom;
    const InsnId l0 = appendOp(dom, Op::ILOAD_0);
    const LabelId lZero = newLbl(dom, l0);
    const LabelId lMerge = newLbl(dom, l0);
    const LabelId lHit = newLbl(dom, l0);
    appendBr(dom, Op::IFEQ, lZero);

    appendOp(dom, Op::ICONST_5);
    appendOp(dom, Op::ISTORE_0);
    appendBr(dom, Op::GOTO, lMerge);

    const InsnId nop = appendOp(dom, Op::NOP);
    bindLbl(dom, lZero, nop);

    const InsnId mergeLoad = appendOp(dom, Op::ILOAD_0);
    bindLbl(dom, lMerge, mergeLoad);
    const InsnId brMerge = appendBr(dom, Op::IFEQ, lHit);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);
    const InsnId hit = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, lHit, hit);

    PathAnalysis a(std::move(dom));
    ASSERT_TRUE(a.facts.ok());
    EXPECT_EQ(a.facts.truthOf(brMerge), PathTruth::Unknown);
}

TEST(MjAsmAnalyzerTest, PathConditionsKeepLoopHeaderUnknown)
{
    // 回边事实不得反哺循环头：header 的 x!=0 在首轮到达时不成立，交集为空。
    // 循环体内同一条件则恒真。
    CodeDom dom;
    const InsnId headLoad = appendOp(dom, Op::ILOAD_0);
    const LabelId loopTop = newLbl(dom, headLoad);
    const LabelId body = newLbl(dom, headLoad);
    const InsnId brHeader = appendBr(dom, Op::IFNE, body);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);

    const InsnId bodyLoad = appendOp(dom, Op::ILOAD_0);
    bindLbl(dom, body, bodyLoad);
    const InsnId brBody = appendBr(dom, Op::IFNE, loopTop);
    const InsnId dead = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);

    PathAnalysis a(std::move(dom));
    ASSERT_TRUE(a.facts.ok());
    EXPECT_EQ(a.facts.truthOf(brHeader), PathTruth::Unknown);
    EXPECT_EQ(a.facts.truthOf(brBody), PathTruth::AlwaysTrue);
    EXPECT_FALSE(a.facts.blockReachable(a.facts.blockOf(dead)));
}

TEST(MjAsmAnalyzerTest, PathConditionsKeepUnseededParamBranchUnknown)
{
    // 不播种签名：局部槽没有稳定符号身份，任何路径都不产生事实。
    CodeDom dom;
    const InsnId l0 = appendOp(dom, Op::ILOAD_0);
    const LabelId taken = newLbl(dom, l0);
    const InsnId branch = appendBr(dom, Op::IFEQ, taken);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);
    const InsnId target = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, taken, target);

    PathAnalysis a(std::move(dom), false);
    ASSERT_TRUE(a.facts.ok());
    EXPECT_EQ(a.facts.truthOf(branch), PathTruth::Unknown);
}

// ---------------------------------------------------------------------------
// 等式绑定：x==5 推出 x+1==6
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, PathConditionsEvaluatePredicateUnderEquality)
{
    CodeDom dom;
    const InsnId l0 = appendOp(dom, Op::ILOAD_0);
    const LabelId lEq5 = newLbl(dom, l0);
    const LabelId lEnd = newLbl(dom, l0);
    const LabelId lEq6 = newLbl(dom, l0);
    appendOp(dom, Op::ICONST_5);
    appendBr(dom, Op::IF_ICMPEQ, lEq5);
    appendBr(dom, Op::GOTO, lEnd);

    const InsnId take5 = appendOp(dom, Op::ILOAD_0);
    bindLbl(dom, lEq5, take5);
    appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IADD);
    appendImm(dom, Op::BIPUSH, 6);
    const InsnId brShifted = appendBr(dom, Op::IF_ICMPEQ, lEq6);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);
    const InsnId hit6 = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, lEq6, hit6);

    const InsnId end = appendOp(dom, Op::ICONST_2);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, lEnd, end);

    PathAnalysis a(std::move(dom));
    ASSERT_TRUE(a.facts.ok());
    EXPECT_EQ(a.facts.truthOf(brShifted), PathTruth::AlwaysTrue);
}

// ---------------------------------------------------------------------------
// 死分支修剪：栈效应保持
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, PathConditionsPruneConstantFalseBranch)
{
    // iconst_1; ifeq L → 恒假，分支改写为 POP（仍弹出一槽）。
    CodeDom dom;
    const InsnId c1 = appendOp(dom, Op::ICONST_1);
    const LabelId taken = newLbl(dom, c1);
    const InsnId branch = appendBr(dom, Op::IFEQ, taken);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);
    const InsnId target = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, taken, target);

    PathAnalysis a(std::move(dom));
    ASSERT_TRUE(a.facts.ok());
    const std::vector<InsnId> rewritten = pruneDecidedBranches(a.unit, a.facts);
    ASSERT_EQ(rewritten.size(), 1u);
    EXPECT_EQ(rewritten[0], branch);
    const jbc::ir::Insn* in = a.unit.dom.peek(branch);
    ASSERT_NE(in, nullptr);
    EXPECT_EQ(in->op, Op::POP);
    EXPECT_EQ(in->target, jbc::ir::kNoLabel);
}

TEST(MjAsmAnalyzerTest, PathConditionsPruneConstantTrueBranch)
{
    // iconst_1; iconst_1; if_icmpeq L → 恒真：前插 POP2，分支改 GOTO_W。
    CodeDom dom;
    const InsnId c1 = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::ICONST_1);
    const LabelId taken = newLbl(dom, c1);
    const InsnId branch = appendBr(dom, Op::IF_ICMPEQ, taken);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);
    const InsnId target = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, taken, target);

    PathAnalysis a(std::move(dom));
    ASSERT_TRUE(a.facts.ok());
    const std::vector<InsnId> rewritten = pruneDecidedBranches(a.unit, a.facts);
    ASSERT_EQ(rewritten.size(), 1u);
    const jbc::ir::Insn* in = a.unit.dom.peek(branch);
    ASSERT_NE(in, nullptr);
    EXPECT_EQ(in->op, Op::GOTO_W);
    EXPECT_EQ(in->target, taken);

    const std::vector<InsnId>& order = a.unit.dom.order();
    const auto it = std::find(order.begin(), order.end(), branch);
    ASSERT_NE(it, order.end());
    ASSERT_NE(it, order.begin());
    const jbc::ir::Insn* pop = a.unit.dom.peek(*(it - 1));
    ASSERT_NE(pop, nullptr);
    EXPECT_EQ(pop->op, Op::POP2);
}

TEST(MjAsmAnalyzerTest, PathConditionsPruneLeavesUnknownBranchUntouched)
{
    CodeDom dom;
    const InsnId l0 = appendOp(dom, Op::ILOAD_0);
    const LabelId taken = newLbl(dom, l0);
    const InsnId branch = appendBr(dom, Op::IFEQ, taken);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::IRETURN);
    const InsnId target = appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::IRETURN);
    bindLbl(dom, taken, target);

    PathAnalysis a(std::move(dom), false);
    ASSERT_TRUE(a.facts.ok());
    const std::vector<InsnId> rewritten = pruneDecidedBranches(a.unit, a.facts);
    EXPECT_TRUE(rewritten.empty());
    const jbc::ir::Insn* in = a.unit.dom.peek(branch);
    ASSERT_NE(in, nullptr);
    EXPECT_EQ(in->op, Op::IFEQ);
}
