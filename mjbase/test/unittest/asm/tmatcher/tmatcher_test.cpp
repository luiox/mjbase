/// @file        tmatcher_test.cpp
/// @brief       新代指令模式匹配 DSL 测试：组合子、捕获、匹配入口、替换计划骨架。
///
/// 验收清单移植自 mbase 的 unittest/asm/tmatcher（tmatcher_test /
/// tmatcher_scenarios_test / tmatcher_layer_test / tmatcher_demo_test），
/// 底座换成 jbc::ir::CodeDom（append + createLabel 构造，不经 classfile）。
#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

#include "mjbase/asm/tmatcher/combinators.hpp"
#include "mjbase/asm/tmatcher/dsl.hpp"
#include "mjbase/asm/tmatcher/insn_matcher.hpp"
#include "mjbase/asm/tmatcher/insn_planner.hpp"
#include "mjbase/asm/tmatcher/match_sequence.hpp"
#include "mjbase/asm/tmatcher/match_state.hpp"
#include "mjbase/asm/tmatcher/match_tracer.hpp"
#include "mjbase/asm/tmatcher/replace_rule.hpp"

namespace {

namespace tmatcher = mjbase::asm_tmatcher;
using tmatcher::combinators::alt;
using tmatcher::combinators::any;
using tmatcher::combinators::branchTargetsCapture;
using tmatcher::combinators::branchTo;
using tmatcher::combinators::capture;
using tmatcher::combinators::cpIndex;
using tmatcher::combinators::imm;
using tmatcher::combinators::localIndex;
using tmatcher::combinators::opcode;
using tmatcher::combinators::opt;
using tmatcher::combinators::pred;
using tmatcher::combinators::repeat;
using tmatcher::combinators::repeatLazy;
using tmatcher::combinators::sameLocalIndexAs;
using tmatcher::combinators::seq;

using jbc::core::Op;
using jbc::core::Shape;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;

/// 构造最小指令值（其余字段默认）。
Insn insnOf(Op op)
{
    Insn in;
    in.op = op;
    return in;
}

/// 追加并解包（测试内构造失败即断言）。
InsnId appendOk(CodeDom& dom, Insn in)
{
    auto r = dom.append(std::move(in));
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

/// 直接驱动一个 step（单点诊断用）。
std::vector<tmatcher::MatchState> runMatch(tmatcher::IMatchStep&     node,
                                     const CodeDom&      dom,
                                     ca::i32             start = 0)
{
    tmatcher::MatchTracer tracer = tmatcher::MatchTracer::noop();
    return node.match(dom, tmatcher::MatchState::initial(start), tracer, 0);
}

/// 调用方执行计划的示范配方：先收集窗口句柄，再前插替换，最后删除窗口。
/// （对齐 replace_rule.hpp 文档中的推荐执行序。）
void applyPlan(CodeDom& dom, const tmatcher::ReplacePlan& plan)
{
    const auto& ord = dom.order();
    ASSERT_GE(plan.start, 0);
    ASSERT_LE(plan.endExclusive, static_cast<ca::i32>(ord.size()));
    std::vector<InsnId> window(ord.begin() + plan.start, ord.begin() + plan.endExclusive);
    for (const Insn& in : plan.replacement) {
        auto r = dom.insertBefore(window.front(), in);
        ASSERT_TRUE(r.is_ok());
    }
    for (InsnId id : window) {
        auto r = dom.erase(id);
        ASSERT_TRUE(r.is_ok());
    }
}

/// 数值常量载入选择：-1..5 用 iconst_n，否则 sipush（测试内的产出器逻辑）。
Insn intInsn(ca::i32 v)
{
    Insn in;
    switch (v) {
    case -1: in.op = Op::ICONST_M1; break;
    case 0: in.op = Op::ICONST_0; break;
    case 1: in.op = Op::ICONST_1; break;
    case 2: in.op = Op::ICONST_2; break;
    case 3: in.op = Op::ICONST_3; break;
    case 4: in.op = Op::ICONST_4; break;
    case 5: in.op = Op::ICONST_5; break;
    default:
        in.op = Op::SIPUSH;
        in.a  = v;
        break;
    }
    return in;
}

}   // namespace

// ========== CaptureStore ==========

TEST(MjAsmTmatcherTest, CaptureStoreEmpty)
{
    tmatcher::CaptureStore cs = tmatcher::CaptureStore::empty();
    EXPECT_FALSE(cs.contains("x"));
    EXPECT_EQ(jbc::ir::kNoInsn, cs.get("x"));
}

TEST(MjAsmTmatcherTest, CaptureStoreBindAndGet)
{
    CodeDom                dom;
    const InsnId           id = appendOk(dom, insnOf(Op::NOP));
    tmatcher::CaptureStore       cs = tmatcher::CaptureStore::empty();
    auto                   b  = cs.bind("x", id);
    ASSERT_TRUE(b.has_value());
    EXPECT_TRUE(b->contains("x"));
    EXPECT_EQ(id, b->get("x"));
}

TEST(MjAsmTmatcherTest, CaptureStoreBindConflictRejects)
{
    CodeDom dom;
    const InsnId a = appendOk(dom, insnOf(Op::NOP));
    const InsnId c = appendOk(dom, insnOf(Op::POP));
    auto b1 = tmatcher::CaptureStore::empty().bind("x", a);
    ASSERT_TRUE(b1.has_value());
    auto b2 = b1->bind("x", c);
    EXPECT_FALSE(b2.has_value());
}

TEST(MjAsmTmatcherTest, CaptureStoreBindSameHandleSucceeds)
{
    CodeDom dom;
    const InsnId a = appendOk(dom, insnOf(Op::NOP));
    auto b1 = tmatcher::CaptureStore::empty().bind("x", a);
    ASSERT_TRUE(b1.has_value());
    auto b2 = b1->bind("x", a);
    EXPECT_TRUE(b2.has_value());
}

TEST(MjAsmTmatcherTest, CaptureStoreRejectsNoInsnBinding)
{
    auto b = tmatcher::CaptureStore::empty().bind("x", jbc::ir::kNoInsn);
    EXPECT_FALSE(b.has_value());
}

TEST(MjAsmTmatcherTest, CaptureStoreMergeRules)
{
    CodeDom dom;
    const InsnId a = appendOk(dom, insnOf(Op::NOP));
    const InsnId b = appendOk(dom, insnOf(Op::POP));
    const InsnId c = appendOk(dom, insnOf(Op::DUP));

    auto cs1 = tmatcher::CaptureStore::empty().bind("x", a);
    auto cs2 = tmatcher::CaptureStore::empty().bind("y", b);
    ASSERT_TRUE(cs1.has_value());
    ASSERT_TRUE(cs2.has_value());
    auto merged = cs1->merge(*cs2);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(a, merged->get("x"));
    EXPECT_EQ(b, merged->get("y"));

    auto conflictOther = tmatcher::CaptureStore::empty().bind("x", c);
    ASSERT_TRUE(conflictOther.has_value());
    auto conflict = cs1->merge(*conflictOther);
    EXPECT_FALSE(conflict.has_value());

    auto sameOther = tmatcher::CaptureStore::empty().bind("x", a);
    ASSERT_TRUE(sameOther.has_value());
    auto same = cs1->merge(*sameOther);
    EXPECT_TRUE(same.has_value());
}

TEST(MjAsmTmatcherTest, CaptureStoreImmutability)
{
    CodeDom dom;
    const InsnId a = appendOk(dom, insnOf(Op::NOP));
    tmatcher::CaptureStore cs1 = tmatcher::CaptureStore::empty();
    auto             cs2 = cs1.bind("x", a);
    ASSERT_TRUE(cs2.has_value());
    EXPECT_FALSE(cs1.contains("x"));
    EXPECT_TRUE(cs2->contains("x"));
}

// ========== MatchState ==========

TEST(MjAsmTmatcherTest, MatchStateTransitions)
{
    const tmatcher::MatchState s = tmatcher::MatchState::initial(5);
    EXPECT_EQ(5, s.cursor);
    const tmatcher::MatchState s2 = s.withCursor(7);
    EXPECT_EQ(5, s.cursor);
    EXPECT_EQ(7, s2.cursor);

    CodeDom dom;
    const InsnId a = appendOk(dom, insnOf(Op::NOP));
    auto bound = s2.captures.bind("k", a);
    ASSERT_TRUE(bound.has_value());
    const tmatcher::MatchState s3 = s2.withCaptures(*bound);
    EXPECT_EQ(7, s3.cursor);
    EXPECT_EQ(a, s3.captures.get("k"));
}

// ========== MatchTracer ==========

TEST(MjAsmTmatcherTest, TracerNoopDoesNotRecord)
{
    auto t = tmatcher::MatchTracer::noop();
    t.enter("test", 0, 0);
    t.success("test", 0, 0, 1);
    EXPECT_TRUE(t.snapshot().events().empty());
}

TEST(MjAsmTmatcherTest, TracerRecordingAndFurthestCursor)
{
    auto t = tmatcher::MatchTracer::recording();
    t.enter("a", 0, 0);
    t.fail("a", 0, 0, "no");
    t.enter("b", 0, 0);
    t.success("b", 0, 0, 5);
    const tmatcher::MatchTrace trace = t.snapshot();
    ASSERT_EQ(4u, trace.events().size());
    EXPECT_EQ(tmatcher::TracePhase::Enter, trace.events()[0].phase);
    EXPECT_EQ(tmatcher::TracePhase::Fail, trace.events()[1].phase);
    EXPECT_EQ(5, trace.events()[3].nextCursor);
    EXPECT_EQ(5, trace.furthestCursor());
    const std::string pretty = trace.toPrettyString();
    EXPECT_NE(std::string::npos, pretty.find("FAIL"));
    EXPECT_NE(std::string::npos, pretty.find("b"));
}

// ========== 叶子匹配节点 ==========

TEST(MjAsmTmatcherTest, OpcodeLeaf)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));
    appendOk(dom, insnOf(Op::POP));

    auto hit = opcode(Op::NOP);
    auto r   = runMatch(*hit, dom);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(1, r[0].cursor);

    auto miss = opcode(Op::DUP);
    EXPECT_TRUE(runMatch(*miss, dom).empty());
}

TEST(MjAsmTmatcherTest, AnySingleLeaf)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));
    appendOk(dom, insnOf(Op::POP));

    auto p = any();
    auto r = runMatch(*p, dom, 1);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(2, r[0].cursor);

    // 游标越界 / 空 DOM 均失败。
    EXPECT_TRUE(runMatch(*p, dom, 2).empty());
    CodeDom empty;
    EXPECT_TRUE(runMatch(*p, empty, 0).empty());
}

TEST(MjAsmTmatcherTest, CpIndexLeaf)
{
    CodeDom dom;
    Insn    news = insnOf(Op::NEW);
    news.cp      = 7;
    appendOk(dom, news);
    appendOk(dom, insnOf(Op::NOP));

    auto hit = cpIndex(7);
    auto r   = runMatch(*hit, dom);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(1, r[0].cursor);

    EXPECT_TRUE(runMatch(*cpIndex(8), dom).empty());
    EXPECT_TRUE(runMatch(*cpIndex(7), dom, 1).empty());   // NOP 非 CP 形状
}

TEST(MjAsmTmatcherTest, LocalIndexLeaf)
{
    CodeDom dom;
    Insn    load = insnOf(Op::ILOAD);
    load.a       = 2;
    appendOk(dom, load);
    appendOk(dom, insnOf(Op::ICONST_0));

    auto r = runMatch(*localIndex(2), dom);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(1, r[0].cursor);

    EXPECT_TRUE(runMatch(*localIndex(3), dom).empty());
    EXPECT_TRUE(runMatch(*localIndex(2), dom, 1).empty());   // ICONST_0 非 Local1 形状
}

TEST(MjAsmTmatcherTest, ImmLeaf)
{
    CodeDom dom;
    Insn    bipush = insnOf(Op::BIPUSH);
    bipush.a       = 3;
    appendOk(dom, bipush);
    Insn sipush = insnOf(Op::SIPUSH);
    sipush.a    = 1000;
    appendOk(dom, sipush);

    auto r = runMatch(*imm(3), dom);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(1, r[0].cursor);

    auto r2 = runMatch(*imm(1000), dom, 1);
    ASSERT_EQ(1u, r2.size());
    EXPECT_EQ(2, r2[0].cursor);

    EXPECT_TRUE(runMatch(*imm(4), dom).empty());
}

TEST(MjAsmTmatcherTest, PredLeaf)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::IADD));
    auto p = pred("isBinaryAdd", [](const tmatcher::InsnCursorView& v, const tmatcher::CaptureStore&) {
        return v.insn->op == Op::IADD;
    });
    auto r = runMatch(*p, dom);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(1, r[0].cursor);
    EXPECT_NE(std::string::npos, p->name().find("isBinaryAdd"));
}

// ========== 分支目标条件（createLabel 造分支目标） ==========

TEST(MjAsmTmatcherTest, BranchToResolvedTarget)
{
    CodeDom dom;
    const InsnId retId = appendOk(dom, insnOf(Op::RETURN));
    auto lbl = dom.createLabel(retId);
    ASSERT_TRUE(lbl.is_ok());
    Insn got = insnOf(Op::GOTO);
    got.target = lbl.unwrap();
    appendOk(dom, got);

    // GOTO 在下标 1，目标解析为 RETURN 句柄。
    auto p = branchTo([retId](const jbc::ir::LabelTarget& t) {
        return !t.isEnd() && t.insn == retId;
    });
    auto r = runMatch(*p, dom, 1);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(2, r[0].cursor);

    // 目标谓词不满足（要求指向别的句柄）→ 失败。
    auto wrong = branchTo([](const jbc::ir::LabelTarget& t) { return t.isEnd(); });
    EXPECT_TRUE(runMatch(*wrong, dom, 1).empty());
    // 非分支形状不走分支谓词。
    EXPECT_TRUE(runMatch(*p, dom, 0).empty());
}

TEST(MjAsmTmatcherTest, BranchTargetsCapture)
{
    CodeDom dom;
    const InsnId retId = appendOk(dom, insnOf(Op::RETURN));
    auto lbl = dom.createLabel(retId);
    ASSERT_TRUE(lbl.is_ok());
    Insn got = insnOf(Op::GOTO);
    got.target = lbl.unwrap();
    appendOk(dom, got);
    appendOk(dom, insnOf(Op::ICONST_1));

    // 捕获 RETURN，再要求 GOTO 的解析目标 == 捕获句柄。
    auto p = seq(capture("t", opcode(Op::RETURN)), branchTargetsCapture("t"));
    auto r = runMatch(*p, dom);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(2, r[0].cursor);
    EXPECT_EQ(retId, r[0].captures.get("t"));

    // 捕获 ICONST_1 时目标不匹配（GOTO 指向 RETURN）→ 整体失败。
    auto q = seq(capture("u", any()), any(), branchTargetsCapture("u"));
    EXPECT_TRUE(runMatch(*q, dom).empty());
}

TEST(MjAsmTmatcherTest, SameLocalIndexAsCapture)
{
    auto pattern = seq(capture("x", any()), sameLocalIndexAs("x"), opcode(Op::IADD));

    CodeDom same;
    Insn    l1 = insnOf(Op::ILOAD);
    l1.a       = 1;
    appendOk(same, l1);
    appendOk(same, l1);
    appendOk(same, insnOf(Op::IADD));
    auto r = runMatch(*pattern, same);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(3, r[0].cursor);

    CodeDom diff;
    Insn    a = insnOf(Op::ILOAD);
    a.a       = 1;
    Insn    b = insnOf(Op::ILOAD);
    b.a       = 2;
    appendOk(diff, a);
    appendOk(diff, b);
    appendOk(diff, insnOf(Op::IADD));
    EXPECT_TRUE(runMatch(*pattern, diff).empty());
}

// ========== 序列组合子 ==========

TEST(MjAsmTmatcherTest, SeqCombinator)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::DUP));

    auto p = seq(opcode(Op::NOP), opcode(Op::POP));
    auto r = runMatch(*p, dom);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(2, r[0].cursor);

    auto p3 = seq(opcode(Op::NOP), opcode(Op::POP), opcode(Op::DUP));
    auto r3 = runMatch(*p3, dom);
    ASSERT_EQ(1u, r3.size());
    EXPECT_EQ(3, r3[0].cursor);

    // 首步失败 / 中间步失败。
    EXPECT_TRUE(runMatch(*seq(opcode(Op::DUP), opcode(Op::POP)), dom).empty());
    EXPECT_TRUE(runMatch(*seq(opcode(Op::NOP), opcode(Op::NOP), opcode(Op::DUP)), dom).empty());
}

TEST(MjAsmTmatcherTest, AltCombinator)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::POP));

    auto first  = alt(opcode(Op::POP), opcode(Op::NOP));
    auto second = alt(opcode(Op::NOP), opcode(Op::POP));
    ASSERT_EQ(1u, runMatch(*first, dom).size());
    ASSERT_EQ(1u, runMatch(*second, dom).size());
    EXPECT_TRUE(runMatch(*alt(opcode(Op::NOP), opcode(Op::DUP)), dom).empty());
}

TEST(MjAsmTmatcherTest, OptCombinator)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::NOP));

    auto p = opt(opcode(Op::POP));
    auto r = runMatch(*p, dom);
    ASSERT_EQ(2u, r.size());
    EXPECT_EQ(1, r[0].cursor);   // 贪婪：先取「匹配了」的候选
    EXPECT_EQ(0, r[1].cursor);

    auto q = opt(opcode(Op::DUP));
    auto r2 = runMatch(*q, dom);
    ASSERT_EQ(1u, r2.size());
    EXPECT_EQ(0, r2[0].cursor);
}

TEST(MjAsmTmatcherTest, RepeatGreedy)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));
    appendOk(dom, insnOf(Op::NOP));
    appendOk(dom, insnOf(Op::POP));

    auto p = repeat(opcode(Op::NOP), 0, -1);
    auto r = runMatch(*p, dom);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(2, r[0].cursor);   // 贪婪：吃满 2 个 NOP

    auto exact = repeat(opcode(Op::NOP), 2, 2);
    auto r2    = runMatch(*exact, dom);
    ASSERT_FALSE(r2.empty());
    EXPECT_EQ(2, r2[0].cursor);

    CodeDom noNop;
    appendOk(noNop, insnOf(Op::POP));
    EXPECT_TRUE(runMatch(*repeat(opcode(Op::NOP), 1, -1), noNop).empty());
}

TEST(MjAsmTmatcherTest, RepeatLazyPrefersShortest)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::ICONST_1));
    appendOk(dom, insnOf(Op::ICONST_2));

    auto lazy = repeatLazy(any(), 1, -1);
    auto r    = runMatch(*lazy, dom);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(1, r[0].cursor);   // 懒惰：最短候选排最前

    auto greedy = repeat(any(), 1, -1);
    auto r2     = runMatch(*greedy, dom);
    ASSERT_FALSE(r2.empty());
    EXPECT_EQ(3, r2[0].cursor);   // 贪婪：最长候选排最前
}

TEST(MjAsmTmatcherTest, RepeatInvalidRangeNeverMatches)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));
    auto p = repeat(any(), 3, 2);
    EXPECT_NE(std::string::npos, p->name().find("invalid"));
    EXPECT_TRUE(runMatch(*p, dom).empty());
    EXPECT_TRUE(runMatch(*repeat(any(), -1, 5), dom).empty());
}

TEST(MjAsmTmatcherTest, NestedQuantifierInsideSeq)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));
    appendOk(dom, insnOf(Op::NOP));
    Insn load = insnOf(Op::ILOAD);
    load.a    = 1;
    appendOk(dom, load);
    appendOk(dom, insnOf(Op::IADD));

    auto pattern = seq(repeat(opcode(Op::NOP), 0, -1), any(), opcode(Op::IADD));
    auto r       = runMatch(*pattern, dom);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(4, r[0].cursor);   // 2 NOP + ILOAD + IADD
}

TEST(MjAsmTmatcherTest, AnyMaxBoundedByUpperLimit)
{
    auto pattern = seq(repeat(any(), 0, 2), opcode(Op::IADD));

    CodeDom dom;
    for (int i = 0; i < 3; i++) {
        Insn load = insnOf(Op::ILOAD);
        load.a    = i;
        appendOk(dom, load);
    }
    appendOk(dom, insnOf(Op::IADD));
    EXPECT_TRUE(runMatch(*pattern, dom).empty());

    CodeDom dom2;
    Insn    load = insnOf(Op::ILOAD);
    load.a       = 1;
    appendOk(dom2, load);
    appendOk(dom2, load);
    appendOk(dom2, insnOf(Op::IADD));
    auto r = runMatch(*pattern, dom2);
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(3, r[0].cursor);
}

// ========== 捕获 ==========

TEST(MjAsmTmatcherTest, CaptureBindsWindowEntryInsn)
{
    CodeDom dom;
    const InsnId id = appendOk(dom, insnOf(Op::ICONST_0));

    auto p = capture("val", opcode(Op::ICONST_0));
    auto r = runMatch(*p, dom);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(1, r[0].cursor);
    EXPECT_EQ(id, r[0].captures.get("val"));
}

TEST(MjAsmTmatcherTest, CaptureConflictRejects)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));
    appendOk(dom, insnOf(Op::POP));

    // 两个不相邻窗口捕获同名 → 句柄不同 → 冲突拒绝。
    auto p = seq(capture("x", any()), capture("x", any()));
    EXPECT_TRUE(runMatch(*p, dom).empty());
}

TEST(MjAsmTmatcherTest, CaptureRejectsZeroWidthSubmatch)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));

    auto p = capture("x", repeat(any(), 0, 0));
    EXPECT_TRUE(runMatch(*p, dom).empty());
}

// ========== DSL 运算符 ==========

TEST(MjAsmTmatcherTest, OperatorSugar)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::DUP));

    auto seqOp = opcode(Op::NOP) - opcode(Op::POP);
    auto r     = runMatch(*seqOp, dom);
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(2, r[0].cursor);

    auto altOp = opcode(Op::NOP) | opcode(Op::DUP);
    auto r2    = runMatch(*altOp, dom);
    ASSERT_EQ(1u, r2.size());
    EXPECT_EQ(1, r2[0].cursor);

    auto chained = opcode(Op::NOP) - opcode(Op::POP) - opcode(Op::DUP);
    auto r3      = runMatch(*chained, dom);
    ASSERT_EQ(1u, r3.size());
    EXPECT_EQ(3, r3[0].cursor);

    auto mixed = opcode(Op::NOP) - (opcode(Op::POP) | opcode(Op::DUP));
    auto r4    = runMatch(*mixed, dom);
    ASSERT_EQ(1u, r4.size());
    EXPECT_EQ(2, r4[0].cursor);
}

// ========== 匹配入口：InsnMatcher ==========

TEST(MjAsmTmatcherTest, FindAllCollectsNonOverlappingAreas)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));
    Insn load = insnOf(Op::ILOAD);
    load.a    = 1;
    appendOk(dom, load);
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));
    ASSERT_EQ(5u, dom.size());

    tmatcher::InsnMatcher matcher;
    matcher.addSequence(tmatcher::MatchSequence{"pop-iconst",
                                          opcode(Op::ICONST_0) - opcode(Op::POP)});
    tmatcher::MatchResult result = matcher.findAll(dom);

    ASSERT_EQ(5u, dom.size());   // 纯匹配：DOM 未被触碰（const 入口）
    const auto& areas = result.areasOf("pop-iconst");
    ASSERT_EQ(2u, areas.size());
    EXPECT_EQ(0, areas[0].start);
    EXPECT_EQ(2, areas[0].endExclusive);
    EXPECT_EQ(3, areas[1].start);
    EXPECT_EQ(5, areas[1].endExclusive);
    EXPECT_EQ(2u, result.totalMatches());
}

TEST(MjAsmTmatcherTest, FindAllMultipleNamedSequences)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::DUP));
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::INEG));
    appendOk(dom, insnOf(Op::INEG));

    tmatcher::InsnMatcher matcher;
    matcher.addSequence(tmatcher::MatchSequence{"dup-pop", opcode(Op::DUP) - opcode(Op::POP)});
    matcher.addSequence(
        tmatcher::MatchSequence{"double-neg", opcode(Op::INEG) - opcode(Op::INEG)});

    tmatcher::MatchResult result = matcher.findAll(dom);
    ASSERT_EQ(1u, result.areasOf("dup-pop").size());
    EXPECT_EQ(0, result.areasOf("dup-pop")[0].start);
    ASSERT_EQ(1u, result.areasOf("double-neg").size());
    EXPECT_EQ(2, result.areasOf("double-neg")[0].start);
}

TEST(MjAsmTmatcherTest, FindFirstRespectsPreference)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::ICONST_1));
    appendOk(dom, insnOf(Op::ICONST_2));

    tmatcher::InsnMatcher matcher;
    matcher.addSequence(
        tmatcher::MatchSequence{"lazy-any-tail", seq(repeatLazy(any(), 1, -1), any())});

    // 懒惰 + First：repeat 吃 1 条 + any 吃 1 条 → 窗口 [0,2)。
    auto first = matcher.findFirst(dom, tmatcher::MatchPreference::First);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(0, first->start);
    EXPECT_EQ(2, first->endExclusive);

    // Longest 裁决：同一序列取最长候选 → 窗口 [0,3)。
    auto longest = matcher.findFirst(dom, tmatcher::MatchPreference::Longest);
    ASSERT_TRUE(longest.has_value());
    EXPECT_EQ(0, longest->start);
    EXPECT_EQ(3, longest->endExclusive);
}

TEST(MjAsmTmatcherTest, FindFirstNoMatchAndEmptyDom)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::NOP));
    tmatcher::InsnMatcher matcher;
    matcher.addSequence(tmatcher::MatchSequence{"never", opcode(Op::RETURN)});
    EXPECT_FALSE(matcher.findFirst(dom, tmatcher::MatchPreference::First).has_value());

    CodeDom empty;
    EXPECT_FALSE(matcher.findFirst(empty, tmatcher::MatchPreference::Longest).has_value());
    EXPECT_TRUE(matcher.findAll(empty).empty());
}

TEST(MjAsmTmatcherTest, MatchIsIdempotent)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));
    Insn load = insnOf(Op::ILOAD);
    load.a    = 1;
    appendOk(dom, load);
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));

    tmatcher::InsnMatcher matcher;
    matcher.addSequence(tmatcher::MatchSequence{"p", opcode(Op::ICONST_0) - opcode(Op::POP)});

    const tmatcher::MatchResult r1 = matcher.findAll(dom);
    const tmatcher::MatchResult r2 = matcher.findAll(dom);
    ASSERT_EQ(r1.totalMatches(), r2.totalMatches());
    ASSERT_EQ(2u, r1.totalMatches());
    for (std::size_t i = 0; i < r1.areasOf("p").size(); i++) {
        EXPECT_EQ(r1.areasOf("p")[i].start, r2.areasOf("p")[i].start);
        EXPECT_EQ(r1.areasOf("p")[i].endExclusive, r2.areasOf("p")[i].endExclusive);
    }
}

// ========== 替换规则骨架：InsnPlanner ==========

TEST(MjAsmTmatcherTest, PlansForProducesWindowAndReplacement)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::RETURN));

    tmatcher::InsnPlanner planner;
    auto rule = opcode(Op::ICONST_0) - opcode(Op::POP);
    planner.addRule(std::move(rule) >> [](const tmatcher::ReplaceContext&) {
        return std::vector<Insn>{insnOf(Op::NOP)};
    });

    std::vector<tmatcher::ReplacePlan> plans = planner.plansFor(dom);
    ASSERT_EQ(1u, plans.size());
    EXPECT_EQ(0, plans[0].start);
    EXPECT_EQ(2, plans[0].endExclusive);
    ASSERT_EQ(1u, plans[0].replacement.size());
    EXPECT_EQ(Op::NOP, plans[0].replacement[0].op);
    EXPECT_EQ(3u, dom.size());   // 产出计划不改 DOM
}

TEST(MjAsmTmatcherTest, LongestRuleWinsAcrossRules)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::POP));

    tmatcher::InsnPlanner planner;
    auto shortRule = opcode(Op::ICONST_0) - opcode(Op::POP);
    planner.addRule(std::move(shortRule) >> [](const tmatcher::ReplaceContext&) {
        return std::vector<Insn>{insnOf(Op::SWAP)};
    });
    auto longRule = seq(opcode(Op::ICONST_0), opcode(Op::POP), opcode(Op::POP));
    planner.addRule(std::move(longRule) >> [](const tmatcher::ReplaceContext&) {
        return std::vector<Insn>{insnOf(Op::ACONST_NULL)};
    });

    std::vector<tmatcher::ReplacePlan> plans = planner.plansFor(dom);
    ASSERT_EQ(1u, plans.size());
    EXPECT_EQ(0, plans[0].start);
    EXPECT_EQ(3, plans[0].endExclusive);   // 长规则胜出
    ASSERT_EQ(1u, plans[0].replacement.size());
    EXPECT_EQ(Op::ACONST_NULL, plans[0].replacement[0].op);
}

TEST(MjAsmTmatcherTest, MultipleDisjointPlans)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));
    Insn load = insnOf(Op::ILOAD);
    load.a    = 1;
    appendOk(dom, load);
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));

    tmatcher::InsnPlanner planner;
    auto rule = opcode(Op::ICONST_0) - opcode(Op::POP);
    planner.addRule(std::move(rule) >> [](const tmatcher::ReplaceContext&) {
        return std::vector<Insn>{insnOf(Op::NOP)};
    });

    std::vector<tmatcher::ReplacePlan> plans = planner.plansFor(dom);
    ASSERT_EQ(2u, plans.size());
    EXPECT_EQ(0, plans[0].start);
    EXPECT_EQ(2, plans[0].endExclusive);
    EXPECT_EQ(3, plans[1].start);
    EXPECT_EQ(5, plans[1].endExclusive);
}

TEST(MjAsmTmatcherTest, ProducerlessRuleSkipsWindowButMatches)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::RETURN));

    tmatcher::InsnPlanner planner;
    planner.addRule(tmatcher::ReplaceRule{"observe", opcode(Op::ICONST_0) - opcode(Op::POP)});

    EXPECT_TRUE(planner.plansFor(dom).empty());   // 无产出器：不产计划
    tmatcher::MatchResult result = planner.matchAll(dom);
    ASSERT_EQ(1u, result.areasOf("observe").size());
    EXPECT_EQ(3u, dom.size());
}

TEST(MjAsmTmatcherTest, ProducerNotCalledOnNoMatch)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::POP));

    bool called = false;
    tmatcher::InsnPlanner planner;
    auto rule = opcode(Op::GOTO);
    planner.addRule(std::move(rule) >> [&](const tmatcher::ReplaceContext&) {
        called = true;
        return std::vector<Insn>{};
    });
    EXPECT_TRUE(planner.plansFor(dom).empty());
    EXPECT_FALSE(called);
}

TEST(MjAsmTmatcherTest, EmptyReplacementDeletesWindow)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::DUP));
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::RETURN));

    tmatcher::InsnPlanner planner;
    auto rule = opcode(Op::DUP) - opcode(Op::POP);
    planner.addRule(std::move(rule) >>
                    [](const tmatcher::ReplaceContext&) { return std::vector<Insn>{}; });

    std::vector<tmatcher::ReplacePlan> plans = planner.plansFor(dom);
    ASSERT_EQ(1u, plans.size());
    EXPECT_EQ(0, plans[0].start);
    EXPECT_EQ(2, plans[0].endExclusive);
    EXPECT_TRUE(plans[0].replacement.empty());
}

TEST(MjAsmTmatcherTest, FixedSequenceRuleSugar)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::POP));

    tmatcher::InsnPlanner planner;
    auto rule = opcode(Op::POP) - opcode(Op::POP);
    planner.addRule(std::move(rule) >> std::vector<Insn>{insnOf(Op::POP2)});

    std::vector<tmatcher::ReplacePlan> plans = planner.plansFor(dom);
    ASSERT_EQ(1u, plans.size());
    ASSERT_EQ(1u, plans[0].replacement.size());
    EXPECT_EQ(Op::POP2, plans[0].replacement[0].op);
}

TEST(MjAsmTmatcherTest, TraceRecordsWhenEnabled)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::POP));
    appendOk(dom, insnOf(Op::RETURN));

    tmatcher::InsnPlanner planner;
    planner.setTraceEnabled(true);
    auto rule = opcode(Op::ICONST_0) - opcode(Op::POP);
    planner.addRule(std::move(rule) >>
                    [](const tmatcher::ReplaceContext&) { return std::vector<Insn>{}; });

    (void)planner.plansFor(dom);
    EXPECT_FALSE(planner.lastTrace().events().empty());
}

// ========== ReplaceContext 视图 ==========

TEST(MjAsmTmatcherTest, ReplaceContextOriginBounds)
{
    CodeDom dom;
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::RETURN));

    tmatcher::ReplaceContext ctx;
    ctx.dom          = &dom;
    ctx.start        = 0;
    ctx.endExclusive = 1;
    EXPECT_EQ(Op::ICONST_0, ctx.origin(0)->op);
    EXPECT_EQ(nullptr, ctx.origin(1));
    EXPECT_TRUE(ctx.originRange(0, 5).size() == 1u);   // 越界部分被裁剪
    EXPECT_TRUE(ctx.originRange(3, 6).empty());

    const tmatcher::CaptureStore empty;
    ctx.captures = &empty;
    EXPECT_EQ(jbc::ir::kNoInsn, ctx.captured("missing"));
    EXPECT_EQ(nullptr, ctx.capturedInsn("missing"));
}

// ========== 端到端：计划由调用方经 CodeDom 执行 ==========

TEST(MjAsmTmatcherTest, ConstantFoldPlanExecutedByCaller)
{
    // bipush 2 + bipush 3 + iadd → iconst_5（捕获两处立即数，产出折叠指令）。
    CodeDom dom;
    Insn    b2 = insnOf(Op::BIPUSH);
    b2.a       = 2;
    appendOk(dom, b2);
    Insn b3 = insnOf(Op::BIPUSH);
    b3.a    = 3;
    appendOk(dom, b3);
    appendOk(dom, insnOf(Op::IADD));
    appendOk(dom, insnOf(Op::IRETURN));

    // 立即数形状叶子工厂（StepPtr 只移不移，两处捕获各造一个）。
    auto immLeaf = [] {
        return pred("imm", [](const tmatcher::InsnCursorView& v,
                              const tmatcher::CaptureStore&) {
            const Shape sh = jbc::core::shapeOf(v.insn->op);
            return sh == Shape::Imm1 || sh == Shape::Imm2;
        });
    };

    tmatcher::InsnPlanner planner;
    auto foldStep = capture("a", immLeaf()) - capture("b", immLeaf()) - opcode(Op::IADD);
    planner.addRule(std::move(foldStep) >>
                    [](const tmatcher::ReplaceContext& ctx) -> std::vector<Insn> {
                        const Insn* a = ctx.capturedInsn("a");
                        const Insn* b = ctx.capturedInsn("b");
                        if (a == nullptr || b == nullptr) {
                            return {};
                        }
                        return std::vector<Insn>{intInsn(a->a + b->a)};
                    });

    // 纯匹配先验证捕获绑定正确。
    tmatcher::MatchResult matches = planner.matchAll(dom);
    ASSERT_EQ(1u, matches.areasOf("unnamed").size());
    const tmatcher::MatchedArea& area = matches.areasOf("unnamed")[0];
    EXPECT_EQ(dom.order()[0], area.captures.get("a"));
    EXPECT_EQ(dom.order()[1], area.captures.get("b"));
    EXPECT_EQ(0, area.start);
    EXPECT_EQ(3, area.endExclusive);

    // 产出计划并由调用方执行（先插后删配方）。
    std::vector<tmatcher::ReplacePlan> plans = planner.plansFor(dom);
    ASSERT_EQ(1u, plans.size());
    applyPlan(dom, plans[0]);

    ASSERT_EQ(2u, dom.size());
    EXPECT_EQ(Op::ICONST_5, dom.peek(dom.order()[0])->op);
    EXPECT_EQ(Op::IRETURN, dom.peek(dom.order()[1])->op);
}

TEST(MjAsmTmatcherTest, TwoFoldSitesExecutedRightToLeft)
{
    CodeDom dom;
    Insn    b2 = insnOf(Op::BIPUSH);
    b2.a       = 2;
    appendOk(dom, b2);
    Insn b3 = insnOf(Op::BIPUSH);
    b3.a    = 3;
    appendOk(dom, b3);
    appendOk(dom, insnOf(Op::IADD));
    Insn load = insnOf(Op::ILOAD);
    load.a    = 1;
    appendOk(dom, load);
    Insn b1a = insnOf(Op::BIPUSH);
    b1a.a    = 1;
    appendOk(dom, b1a);
    appendOk(dom, b1a);
    appendOk(dom, insnOf(Op::IADD));

    auto immLeaf = [] {
        return pred("imm", [](const tmatcher::InsnCursorView& v,
                              const tmatcher::CaptureStore&) {
            const Shape sh = jbc::core::shapeOf(v.insn->op);
            return sh == Shape::Imm1 || sh == Shape::Imm2;
        });
    };

    tmatcher::InsnPlanner planner;
    auto foldStep = capture("a", immLeaf()) - capture("b", immLeaf()) - opcode(Op::IADD);
    planner.addRule(std::move(foldStep) >>
                    [](const tmatcher::ReplaceContext& ctx) -> std::vector<Insn> {
                        const Insn* a = ctx.capturedInsn("a");
                        const Insn* b = ctx.capturedInsn("b");
                        return std::vector<Insn>{intInsn(a->a + b->a)};
                    });

    std::vector<tmatcher::ReplacePlan> plans = planner.plansFor(dom);
    ASSERT_EQ(2u, plans.size());
    EXPECT_EQ(0, plans[0].start);
    EXPECT_EQ(3, plans[0].endExclusive);
    EXPECT_EQ(4, plans[1].start);
    EXPECT_EQ(7, plans[1].endExclusive);

    // 多计划执行序：从右往左（右侧窗口先改，左侧下标不受影响）。
    for (auto it = plans.rbegin(); it != plans.rend(); ++it) {
        applyPlan(dom, *it);
    }

    ASSERT_EQ(3u, dom.size());
    EXPECT_EQ(Op::ICONST_5, dom.peek(dom.order()[0])->op);   // 2+3
    EXPECT_EQ(Op::ILOAD, dom.peek(dom.order()[1])->op);
    EXPECT_EQ(Op::ICONST_2, dom.peek(dom.order()[2])->op);   // 1+1
}

TEST(MjAsmTmatcherTest, PlanKeepsOriginInsnValues)
{
    // 噪声流（对齐旧 multiplePatternsInNoisyStream 场景）：ILOAD 是噪声，
    // 模式 any-ICONST_0-IADD 从 ISTORE 起命中 [1,4)；替换为保留区间第一条。
    CodeDom dom;
    Insn    load = insnOf(Op::ILOAD);
    load.a       = 1;
    appendOk(dom, load);
    Insn    store = insnOf(Op::ISTORE);
    store.a       = 2;
    appendOk(dom, store);
    appendOk(dom, insnOf(Op::ICONST_0));
    appendOk(dom, insnOf(Op::IADD));
    appendOk(dom, insnOf(Op::RETURN));

    tmatcher::InsnPlanner planner;
    auto keepFirst = any() - opcode(Op::ICONST_0) - opcode(Op::IADD);
    planner.addRule(std::move(keepFirst) >>
                    [](const tmatcher::ReplaceContext& ctx) -> std::vector<Insn> {
                        const Insn* origin = ctx.origin(0);
                        return origin != nullptr ? std::vector<Insn>{*origin}
                                                 : std::vector<Insn>{};
                    });

    std::vector<tmatcher::ReplacePlan> plans = planner.plansFor(dom);
    ASSERT_EQ(1u, plans.size());
    EXPECT_EQ(1, plans[0].start);   // 从 ISTORE 起命中 [1,4)
    applyPlan(dom, plans[0]);

    ASSERT_EQ(3u, dom.size());
    EXPECT_EQ(Op::ILOAD, dom.peek(dom.order()[0])->op);
    EXPECT_EQ(Op::ISTORE, dom.peek(dom.order()[1])->op);
    EXPECT_EQ(2, dom.peek(dom.order()[1])->a);   // origin 拷贝保留操作数
    EXPECT_EQ(Op::RETURN, dom.peek(dom.order()[2])->op);
}
