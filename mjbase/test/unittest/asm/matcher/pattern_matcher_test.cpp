/// @file        pattern_matcher_test.cpp
/// @brief       旧代 pattern matcher 语义测试：窗口匹配、捕获、原子替换、回滚。
///
/// 用例意图移植自 mbase/unittest/asm/matcher/pattern_matcher_test.cpp，
/// 底座换为纯 jbc::ir::CodeDom 构造。
#include <gtest/gtest.h>

#include <vector>

#include "mjbase/asm/matcher/pattern_matcher.hpp"
#include "mjbase/asm/matcher/step_util.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

using ca::core::Ok;
using ca::core::Result;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_matcher::MatchContext;
using mjbase::asm_matcher::MatchError;
using mjbase::asm_matcher::MatchRule;
using mjbase::asm_matcher::MatchStep;
using mjbase::asm_matcher::MatcherError;
using mjbase::asm_matcher::PatternMatcher;
using mjbase::asm_matcher::Steps;

namespace {

Insn simple(Op op)
{
    Insn in;
    in.op = op;
    return in;
}

Insn imm(Op op, ca::i32 a)
{
    Insn in;
    in.op = op;
    in.a = a;
    return in;
}

Insn cpInsn(ca::u32 cp)
{
    Insn in;
    in.op = Op::LDC;
    in.cp = cp;
    return in;
}

std::vector<Op> opsOf(const CodeDom& dom)
{
    std::vector<Op> out;
    for (const InsnId& id : dom.order()) {
        if (const Insn* p = dom.peek(id)) out.push_back(p->op);
    }
    return out;
}

Result<void, MatcherError> okStrategy(MatchContext&)
{
    return Ok();
}

}   // namespace

TEST(MjAsmMatcherTest, SimpleMatchFindsWindow)
{
    CodeDom dom;
    (void)dom.append(simple(Op::NOP));
    (void)dom.append(cpInsn(1));
    (void)dom.append(cpInsn(2));
    (void)dom.append(simple(Op::SWAP));
    (void)dom.append(simple(Op::NOP));

    PatternMatcher matcher;
    matcher.addRule(MatchRule(
        {MatchStep::op(Op::LDC), MatchStep::op(Op::LDC), MatchStep::op(Op::SWAP)}));
    const std::vector<mjbase::asm_matcher::MatchResult> results = matcher.match(dom);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].valid());
    EXPECT_EQ(results[0].start, 1u);
    EXPECT_EQ(results[0].end, 4u);   // 开区间：命中 ldc,ldc,swap
}

TEST(MjAsmMatcherTest, MatchNopAtFirstIndex)
{
    CodeDom dom;
    (void)dom.append(simple(Op::NOP));
    (void)dom.append(simple(Op::POP));

    PatternMatcher matcher;
    matcher.addRule(MatchRule({MatchStep::op(Op::NOP)}));
    const auto results = matcher.match(dom);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].valid());
    EXPECT_EQ(results[0].start, 0u);
}

TEST(MjAsmMatcherTest, MatchPopPop)
{
    CodeDom dom;
    (void)dom.append(simple(Op::POP));
    (void)dom.append(simple(Op::POP));

    PatternMatcher matcher;
    matcher.addRule(MatchRule({MatchStep::op(Op::POP), MatchStep::op(Op::POP)}));
    const auto results = matcher.match(dom);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].start, 0u);
    EXPECT_EQ(results[0].end, 2u);
}

TEST(MjAsmMatcherTest, NoMatchWhenSequenceAbsent)
{
    CodeDom dom;
    (void)dom.append(simple(Op::SWAP));
    (void)dom.append(simple(Op::NOP));

    PatternMatcher matcher;
    matcher.addRule(
        MatchRule({MatchStep::op(Op::SWAP), MatchStep::op(Op::SWAP)}));
    const auto results = matcher.match(dom);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_FALSE(results[0].valid());
}

TEST(MjAsmMatcherTest, ApplyMatchOnlyRuleDoesNotReportChange)
{
    CodeDom dom;
    (void)dom.append(simple(Op::NOP));
    (void)dom.append(simple(Op::RETURN));

    PatternMatcher matcher;
    matcher.addRule(MatchRule({MatchStep::op(Op::NOP)}));   // 无策略

    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_ok());
    EXPECT_FALSE(applied.unwrap());
    ASSERT_EQ(dom.order().size(), 2u);
    EXPECT_EQ(dom.peek(dom.order()[0])->op, Op::NOP);
    EXPECT_EQ(dom.peek(dom.order()[1])->op, Op::RETURN);
}

TEST(MjAsmMatcherTest, CancelledReplacementKeepsOriginalInstructions)
{
    MatchRule rule(
        {MatchStep::op(Op::ICONST_1), MatchStep::op(Op::ICONST_2), MatchStep::op(Op::IADD)},
        [](MatchContext& ctx) -> Result<void, MatcherError> {
            ctx.replacement.push_back(simple(Op::ICONST_3));
            ctx.cancelReplacement();
            return Ok();
        });

    CodeDom dom;
    (void)dom.append(simple(Op::ICONST_1));
    (void)dom.append(simple(Op::ICONST_2));
    (void)dom.append(simple(Op::IADD));
    (void)dom.append(simple(Op::IRETURN));

    PatternMatcher matcher;
    matcher.addRule(std::move(rule));

    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_ok());
    EXPECT_FALSE(applied.unwrap());
    ASSERT_EQ(dom.order().size(), 4u);
    EXPECT_EQ(dom.peek(dom.order()[0])->op, Op::ICONST_1);
    EXPECT_EQ(dom.peek(dom.order()[1])->op, Op::ICONST_2);
    EXPECT_EQ(dom.peek(dom.order()[2])->op, Op::IADD);
    EXPECT_EQ(dom.peek(dom.order()[3])->op, Op::IRETURN);
}

TEST(MjAsmMatcherTest, ApplyReplacesWindowWithValueList)
{
    MatchRule rule(
        {MatchStep::op(Op::ICONST_1), MatchStep::op(Op::ICONST_2), MatchStep::op(Op::IADD)},
        [](MatchContext& ctx) -> Result<void, MatcherError> {
            ctx.replacement.push_back(simple(Op::ICONST_3));
            return Ok();
        });

    CodeDom dom;
    (void)dom.append(simple(Op::ICONST_1));
    (void)dom.append(simple(Op::ICONST_2));
    (void)dom.append(simple(Op::IADD));
    (void)dom.append(simple(Op::IRETURN));

    PatternMatcher matcher;
    matcher.addRule(std::move(rule));

    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());

    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_3);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjAsmMatcherTest, ApplyEmptyReplacementDeletesWindow)
{
    CodeDom dom;
    (void)dom.append(simple(Op::ICONST_1));
    (void)dom.append(simple(Op::POP));
    (void)dom.append(simple(Op::RETURN));

    PatternMatcher matcher;
    matcher.addRule(MatchRule(
        {MatchStep::op(Op::ICONST_1), MatchStep::op(Op::POP)}, okStrategy));

    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());
    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(MjAsmMatcherTest, ApplyMultiInstructionReplacement)
{
    CodeDom dom;
    (void)dom.append(simple(Op::ACONST_NULL));
    (void)dom.append(simple(Op::POP));
    (void)dom.append(simple(Op::RETURN));

    MatchRule rule({MatchStep::op(Op::ACONST_NULL), MatchStep::op(Op::POP)},
                   [](MatchContext& ctx) -> Result<void, MatcherError> {
                       ctx.replacement.push_back(simple(Op::ICONST_1));
                       ctx.replacement.push_back(simple(Op::ICONST_2));
                       ctx.replacement.push_back(simple(Op::POP));
                       return Ok();
                   });
    PatternMatcher matcher;
    matcher.addRule(std::move(rule));

    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());
    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::ICONST_2);
    EXPECT_EQ(ops[2], Op::POP);
    EXPECT_EQ(ops[3], Op::RETURN);
}

TEST(MjAsmMatcherTest, ApplyUsesLastWindowAndReachesFixpoint)
{
    CodeDom dom;
    for (int i = 0; i < 2; ++i) {
        (void)dom.append(simple(Op::ICONST_1));
        (void)dom.append(simple(Op::ICONST_2));
        (void)dom.append(simple(Op::IADD));
        (void)dom.append(simple(Op::IRETURN));
    }

    MatchRule rule(
        {MatchStep::op(Op::ICONST_1), MatchStep::op(Op::ICONST_2), MatchStep::op(Op::IADD)},
        [](MatchContext& ctx) -> Result<void, MatcherError> {
            ctx.replacement.push_back(simple(Op::ICONST_3));
            return Ok();
        });
    PatternMatcher matcher;
    matcher.addRule(std::move(rule));

    // 第一次 apply 替换最后一个窗口（旧栈从后往前扫描语义）。
    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());
    std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[4], Op::ICONST_3);

    // 循环到不动点：每次调用每规则至多替换一处。
    int rounds = 1;
    for (;;) {
        auto again = matcher.apply(dom);
        ASSERT_TRUE(again.is_ok());
        if (!again.unwrap()) break;
        ++rounds;
    }
    EXPECT_EQ(rounds, 2);
    ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ICONST_3);
    EXPECT_EQ(ops[2], Op::ICONST_3);
}

TEST(MjAsmMatcherTest, PredicateConstrainsOperands)
{
    CodeDom dom;
    (void)dom.append(imm(Op::BIPUSH, 9));
    (void)dom.append(simple(Op::POP));
    (void)dom.append(imm(Op::BIPUSH, 7));
    (void)dom.append(simple(Op::POP));

    PatternMatcher matcher;
    matcher.addRule(MatchRule({MatchStep::opIf(
        Op::BIPUSH, [](const Insn& in) { return in.a == 7; }),
        MatchStep::op(Op::POP)}));
    const auto results = matcher.match(dom);

    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].valid());
    EXPECT_EQ(results[0].start, 2u);
    EXPECT_EQ(results[0].end, 4u);
}

TEST(MjAsmMatcherTest, NamedCaptureRecordsInsn)
{
    CodeDom dom;
    (void)dom.append(imm(Op::BIPUSH, 9));
    (void)dom.append(simple(Op::POP));
    (void)dom.append(imm(Op::BIPUSH, 7));
    (void)dom.append(simple(Op::POP));

    MatchRule rule({MatchStep::opIf(Op::BIPUSH, [](const Insn& in) { return in.a == 7; })
                        .captureAs("x"),
                    MatchStep::op(Op::POP)});
    MatchContext ctx;
    EXPECT_TRUE(rule.tryMatch(dom, 2, ctx));
    const auto it = ctx.captures.find("x");
    ASSERT_NE(it, ctx.captures.end());
    EXPECT_EQ(it->second.orderIndex, 2u);
    ASSERT_NE(it->second.insn.op, Op::NOP);
    EXPECT_EQ(it->second.insn.op, Op::BIPUSH);
    EXPECT_EQ(it->second.insn.a, 7);
}

TEST(MjAsmMatcherTest, CombinatorsAllOfAnyOfNotOf)
{
    CodeDom dom;
    (void)dom.append(simple(Op::DUP));

    PatternMatcher anyOfMatcher;
    anyOfMatcher.addRule(MatchRule(
        {MatchStep::anyOf({MatchStep::op(Op::SWAP), MatchStep::op(Op::DUP)})}));
    const auto r1 = anyOfMatcher.match(dom);
    ASSERT_EQ(r1.size(), 1u);
    EXPECT_TRUE(r1[0].valid());

    PatternMatcher notOfMatcher;
    notOfMatcher.addRule(MatchRule({MatchStep::notOf(MatchStep::op(Op::NOP))}));
    const auto r2 = notOfMatcher.match(dom);
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_TRUE(r2[0].valid());

    PatternMatcher notOfMiss;
    notOfMiss.addRule(MatchRule({MatchStep::notOf(MatchStep::op(Op::DUP))}));
    const auto r3 = notOfMiss.match(dom);
    ASSERT_EQ(r3.size(), 1u);
    EXPECT_FALSE(r3[0].valid());

    CodeDom withImm;
    (void)withImm.append(imm(Op::BIPUSH, 3));
    PatternMatcher allOfMatcher;
    allOfMatcher.addRule(MatchRule({MatchStep::allOf(
        {MatchStep::anyIf([](const Insn& in) { return in.op == Op::BIPUSH; }),
         MatchStep::opIf(Op::BIPUSH, [](const Insn& in) { return in.a == 3; })})}));
    const auto r4 = allOfMatcher.match(withImm);
    ASSERT_EQ(r4.size(), 1u);
    EXPECT_TRUE(r4[0].valid());
}

TEST(MjAsmMatcherTest, AnyMultiConsumesTail)
{
    CodeDom dom;
    (void)dom.append(simple(Op::ICONST_1));
    (void)dom.append(simple(Op::IADD));
    (void)dom.append(simple(Op::POP));

    PatternMatcher matcher;
    matcher.addRule(MatchRule({MatchStep::op(Op::ICONST_1), MatchStep::anyMulti()}));
    const auto results = matcher.match(dom);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].valid());
    EXPECT_EQ(results[0].start, 0u);
    EXPECT_EQ(results[0].end, 3u);   // 吞到序列尾
}

TEST(MjAsmMatcherTest, BranchTargetInsideWindowBlocksMatch)
{
    CodeDom dom;
    (void)dom.append(simple(Op::ICONST_1));
    const InsnId iadd = dom.append(simple(Op::IADD)).unwrap();
    (void)dom.append(simple(Op::IRETURN));
    const LabelId target = dom.createLabel(iadd).unwrap();
    Insn gotoInsn;
    gotoInsn.op = Op::GOTO;
    gotoInsn.target = target;
    (void)dom.append(gotoInsn);   // [iconst_1, iadd, ireturn, goto→iadd]

    // 目标在窗口内部：跨边界窗口不命中。
    PatternMatcher cross;
    cross.addRule(MatchRule({MatchStep::op(Op::ICONST_1), MatchStep::op(Op::IADD)}));
    const auto r1 = cross.match(dom);
    ASSERT_EQ(r1.size(), 1u);
    EXPECT_FALSE(r1[0].valid());

    // 目标恰为窗口首条：豁免（等价旧栈 label 在窗口首位不挡匹配）。
    PatternMatcher atHead;
    atHead.addRule(MatchRule({MatchStep::op(Op::IADD)}));
    const auto r2 = atHead.match(dom);
    ASSERT_EQ(r2.size(), 1u);
    EXPECT_TRUE(r2[0].valid());
    EXPECT_EQ(r2[0].start, 1u);
}

TEST(MjAsmMatcherTest, ApplyRollsBackWhenBoundaryLabelAnchorsMidWindow)
{
    CodeDom dom;
    (void)dom.append(simple(Op::ICONST_1));
    const InsnId i2 = dom.append(simple(Op::ICONST_2)).unwrap();
    (void)dom.append(simple(Op::IADD));
    (void)dom.append(simple(Op::IRETURN));
    const LabelId guard = dom.createLabel(i2).unwrap();   // Boundary 标签锚定窗口中部
    (void)guard;

    MatchRule rule(
        {MatchStep::op(Op::ICONST_1), MatchStep::op(Op::ICONST_2), MatchStep::op(Op::IADD)},
        [](MatchContext& ctx) -> Result<void, MatcherError> {
            ctx.replacement.push_back(simple(Op::ICONST_3));
            return Ok();
        });
    PatternMatcher matcher;
    matcher.addRule(std::move(rule));

    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_err());
    EXPECT_EQ(applied.unwrap_err().kind, MatchError::EditRejected);

    // 内容级回滚：指令序列原样恢复（句柄允许因重插而变化）。
    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::ICONST_2);
    EXPECT_EQ(ops[2], Op::IADD);
    EXPECT_EQ(ops[3], Op::IRETURN);
}

TEST(MjAsmMatcherTest, DeletionUnderExternalBranchRetargetsToFallthrough)
{
    // 手工字节：iconst_1(0x04) pop return goto -3（goto 回跳到首条指令）。
    // importFrom 产生的控制流标签是 ControlFlow 角色：窗口整删时外部
    // 引用重定向到顺序后继（旧栈 label 停在窗口原位置的等价语义）。
    const std::vector<ca::u8> bytes = {0x04, 0x57, 0xb1, 0xa7, 0xff, 0xfd};
    auto imported = CodeDom::importFrom(jbc::core::ByteView(bytes));
    ASSERT_TRUE(imported.is_ok());
    CodeDom dom = std::move(imported).unwrap();
    ASSERT_EQ(dom.order().size(), 4u);

    PatternMatcher matcher;
    matcher.addRule(MatchRule(
        {MatchStep::op(Op::ICONST_1), MatchStep::op(Op::POP)}, okStrategy));

    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());

    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::RETURN);
    EXPECT_EQ(ops[1], Op::GOTO);
    EXPECT_TRUE(dom.exportRaw().is_ok());   // 结构自洽可导出
}

TEST(MjAsmMatcherTest, StepsLoadFamiliesMatchCorrespondingInsns)
{
    CodeDom fresh;
    (void)fresh.append([] {
        Insn in;
        in.op = Op::ILOAD;
        in.a = 1;
        return in;
    }());
    (void)fresh.append(simple(Op::LCONST_1));
    (void)fresh.append(simple(Op::FCONST_2));
    (void)fresh.append(simple(Op::DCONST_1));
    (void)fresh.append(cpInsn(7));

    PatternMatcher matcher;
    matcher.addRule(MatchRule({Steps::loadInt(), Steps::loadLong(), Steps::loadFloat(),
                               Steps::loadDouble(), Steps::loadStr()}));
    const auto results = matcher.match(fresh);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].valid());
    EXPECT_EQ(results[0].start, 0u);
    EXPECT_EQ(results[0].end, 5u);
}

TEST(MjAsmMatcherTest, MatchResultIsPerRuleInOrder)
{
    CodeDom dom;
    (void)dom.append(simple(Op::NOP));
    (void)dom.append(simple(Op::POP));

    PatternMatcher matcher;
    matcher.addRule(MatchRule({MatchStep::op(Op::POP)}));
    matcher.addRule(MatchRule({MatchStep::op(Op::SWAP)}));
    const auto results = matcher.match(dom);

    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].valid());
    EXPECT_EQ(results[0].start, 1u);
    EXPECT_FALSE(results[1].valid());
}

TEST(MjAsmMatcherTest, EmptyMatcherApplyReturnsFalse)
{
    CodeDom dom;
    (void)dom.append(simple(Op::NOP));
    PatternMatcher matcher;
    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_ok());
    EXPECT_FALSE(applied.unwrap());
}
