/// @file        mini_dsl_test.cpp
/// @brief       mini DSL 解析/编译/应用语义测试（纯 CodeDom 构造）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mjbase/asm/matcher/mini_dsl_compiler.hpp"
#include "mjbase/asm/matcher/mini_dsl_parser.hpp"
#include "mjbase/asm/matcher/pattern_matcher.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

using ca::core::Result;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using mjbase::asm_matcher::MatchError;
using mjbase::asm_matcher::PatternMatcher;
using mjbase::asm_matcher::dsl::MiniDslCompiler;
using mjbase::asm_matcher::dsl::MiniDslParser;

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

std::vector<Op> opsOf(const CodeDom& dom)
{
    std::vector<Op> out;
    for (const jbc::ir::InsnId& id : dom.order()) {
        if (const Insn* p = dom.peek(id)) out.push_back(p->op);
    }
    return out;
}

/// 编译 + 一次 apply 的便捷入口。
Result<bool, mjbase::asm_matcher::MatcherError> applyDsl(CodeDom& dom, const char* dsl)
{
    PatternMatcher matcher;
    auto added = MiniDslCompiler::compileAndAdd(matcher, dsl);
    if (added.is_err()) return ca::core::Err(added.unwrap_err());
    return matcher.apply(dom);
}

}   // namespace

TEST(MjAsmMatcherTest, DslParseRuleShape)
{
    auto rule = MiniDslParser::parseRule("iconst_1 - iconst_2 - iadd => iconst_3");
    ASSERT_TRUE(rule.is_ok());
    const auto& r = rule.unwrap();
    ASSERT_EQ(r.matchSteps.size(), 3u);
    ASSERT_EQ(r.replaceSteps.size(), 1u);
    const auto* m0 = std::get_if<mjbase::asm_matcher::dsl::PatternStep::Opcode>(
        &r.matchSteps[0].data);
    ASSERT_NE(m0, nullptr);
    EXPECT_EQ(m0->name, "iconst_1");
}

TEST(MjAsmMatcherTest, DslParseMultiRuleBlocks)
{
    auto rules = MiniDslParser::parseRules("nop - pop => nothing\n---\ndup => swap");
    ASSERT_TRUE(rules.is_ok());
    EXPECT_EQ(rules.unwrap().size(), 2u);
}

TEST(MjAsmMatcherTest, DslParseErrors)
{
    auto noArrow = MiniDslParser::parseRule("iconst_1 iconst_2");
    ASSERT_TRUE(noArrow.is_err());
    EXPECT_EQ(noArrow.unwrap_err().kind, MatchError::DslSyntax);

    auto emptyMatch = MiniDslParser::parseRule(" => nop");
    ASSERT_TRUE(emptyMatch.is_err());
    EXPECT_EQ(emptyMatch.unwrap_err().kind, MatchError::DslSyntax);

    // 表达式尾随垃圾。
    auto trailing = MiniDslCompiler::compile("nop => load(I: 1 x)");
    ASSERT_TRUE(trailing.is_err());
    EXPECT_EQ(trailing.unwrap_err().kind, MatchError::DslSyntax);

    // 数字溢出 i64。
    auto overflow = MiniDslCompiler::compile("nop => load(I: 99999999999999999999)");
    ASSERT_TRUE(overflow.is_err());
    EXPECT_EQ(overflow.unwrap_err().kind, MatchError::DslSyntax);
}

TEST(MjAsmMatcherTest, DslUnknownOpcodeRejectedAtCompile)
{
    auto compiled = MiniDslCompiler::compile("frobnicate => nop");
    ASSERT_TRUE(compiled.is_err());
    EXPECT_EQ(compiled.unwrap_err().kind, MatchError::UnknownOpcode);

    auto badReplace = MiniDslCompiler::compile("nop => frobnicate");
    ASSERT_TRUE(badReplace.is_err());
    EXPECT_EQ(badReplace.unwrap_err().kind, MatchError::UnknownOpcode);
}

TEST(MjAsmMatcherTest, DslMethodFacesRejectedWithoutConstantPool)
{
    auto pattern = MiniDslCompiler::compile(
        "invokestatic java/lang/Math abs (I)I => nothing");
    ASSERT_TRUE(pattern.is_err());
    EXPECT_EQ(pattern.unwrap_err().kind, MatchError::DslSemantic);

    auto replace = MiniDslCompiler::compile("nop => invokestatic a b c");
    ASSERT_TRUE(replace.is_err());
    EXPECT_EQ(replace.unwrap_err().kind, MatchError::DslSemantic);
}

TEST(MjAsmMatcherTest, DslClassicConstFold)
{
    CodeDom dom;
    (void)dom.append(simple(Op::ICONST_1));
    (void)dom.append(simple(Op::ICONST_2));
    (void)dom.append(simple(Op::IADD));
    (void)dom.append(simple(Op::IRETURN));

    auto applied = applyDsl(dom, "iconst_1 - iconst_2 - iadd => iconst_3");
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());
    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_3);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjAsmMatcherTest, DslBoundExpressionFold)
{
    CodeDom dom;
    (void)dom.append(imm(Op::BIPUSH, 20));
    (void)dom.append(imm(Op::BIPUSH, 22));
    (void)dom.append(simple(Op::IADD));
    (void)dom.append(simple(Op::IRETURN));

    auto applied = applyDsl(dom, "load(I:a) - load(I:b) - iadd => load(I: a + b)");
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());
    ASSERT_EQ(dom.order().size(), 2u);
    const Insn* folded = dom.peek(dom.order()[0]);
    ASSERT_NE(folded, nullptr);
    EXPECT_EQ(folded->op, Op::BIPUSH);
    EXPECT_EQ(folded->a, 42);
}

TEST(MjAsmMatcherTest, DslWideIntFoldEmitsSipush)
{
    CodeDom dom;
    (void)dom.append(imm(Op::SIPUSH, 100));
    (void)dom.append(imm(Op::SIPUSH, 200));
    (void)dom.append(simple(Op::IADD));
    (void)dom.append(simple(Op::IRETURN));

    auto applied = applyDsl(dom, "load(I:a) - load(I:b) - iadd => load(I: a + b)");
    ASSERT_TRUE(applied.is_ok());
    ASSERT_EQ(dom.order().size(), 2u);
    const Insn* folded = dom.peek(dom.order()[0]);
    ASSERT_NE(folded, nullptr);
    EXPECT_EQ(folded->op, Op::SIPUSH);
    EXPECT_EQ(folded->a, 300);
}

TEST(MjAsmMatcherTest, DslTypedOpsShiftAndCast)
{
    {
        CodeDom dom;
        (void)dom.append(imm(Op::BIPUSH, 3));
        (void)dom.append(simple(Op::POP));
        auto applied = applyDsl(dom, "load(I:a) - pop => load(I: (a << 2) + 1)");
        ASSERT_TRUE(applied.is_ok());
        const Insn* folded = dom.peek(dom.order()[0]);
        ASSERT_NE(folded, nullptr);
        EXPECT_EQ(folded->op, Op::BIPUSH);
        EXPECT_EQ(folded->a, 13);
    }
    {
        // J 域 cast 后回 I 域：lconst_1 绑定为 long，cast J 再 +1。
        CodeDom dom;
        (void)dom.append(simple(Op::LCONST_1));
        (void)dom.append(simple(Op::POP));
        auto applied = applyDsl(dom, "load(J:a) - pop => load(I: J(a) + 1)");
        ASSERT_TRUE(applied.is_ok());
        const Insn* folded = dom.peek(dom.order()[0]);
        ASSERT_NE(folded, nullptr);
        EXPECT_EQ(folded->op, Op::ICONST_2);
    }
    {
        CodeDom dom;
        (void)dom.append(simple(Op::FCONST_1));
        (void)dom.append(simple(Op::POP));
        auto applied = applyDsl(dom, "load(F:a) - pop => load(F: a + 1)");
        ASSERT_TRUE(applied.is_ok());
        const Insn* folded = dom.peek(dom.order()[0]);
        ASSERT_NE(folded, nullptr);
        EXPECT_EQ(folded->op, Op::FCONST_2);
    }
}

TEST(MjAsmMatcherTest, DslDivisionByZeroYieldsZero)
{
    CodeDom dom;
    (void)dom.append(imm(Op::BIPUSH, 9));
    (void)dom.append(simple(Op::POP));

    auto applied = applyDsl(dom, "load(I:a) - pop => load(I: a / 0 + a % 0)");
    ASSERT_TRUE(applied.is_ok());
    const Insn* folded = dom.peek(dom.order()[0]);
    ASSERT_NE(folded, nullptr);
    EXPECT_EQ(folded->op, Op::ICONST_0);
}

TEST(MjAsmMatcherTest, DslNothingDeletesWindow)
{
    CodeDom dom;
    (void)dom.append(simple(Op::ICONST_1));
    (void)dom.append(simple(Op::POP));
    (void)dom.append(simple(Op::RETURN));

    auto applied = applyDsl(dom, "iconst_1 - pop => nothing");
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());
    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(MjAsmMatcherTest, DslOriginClonesMatchedInsns)
{
    CodeDom dom;
    (void)dom.append(imm(Op::BIPUSH, 3));
    (void)dom.append(simple(Op::POP));

    auto applied = applyDsl(dom, "bipush - pop => origin[0] - origin[0]");
    ASSERT_TRUE(applied.is_ok());
    ASSERT_EQ(dom.order().size(), 2u);
    const Insn* a = dom.peek(dom.order()[0]);
    const Insn* b = dom.peek(dom.order()[1]);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->op, Op::BIPUSH);
    EXPECT_EQ(a->a, 3);
    EXPECT_EQ(b->op, Op::BIPUSH);
    EXPECT_EQ(b->a, 3);
}

TEST(MjAsmMatcherTest, DslOriginRangeAndOutOfRange)
{
    {
        CodeDom dom;
        (void)dom.append(imm(Op::BIPUSH, 3));
        (void)dom.append(simple(Op::POP));
        auto applied = applyDsl(dom, "bipush - pop => origin[0..1]");
        ASSERT_TRUE(applied.is_ok());
        ASSERT_EQ(dom.order().size(), 2u);
    }
    {
        CodeDom dom;
        (void)dom.append(simple(Op::NOP));
        auto applied = applyDsl(dom, "nop => origin[3]");
        ASSERT_TRUE(applied.is_err());
        EXPECT_EQ(applied.unwrap_err().kind, MatchError::DslSemantic);
        EXPECT_EQ(dom.order().size(), 1u);   // 策略失败不改 DOM
    }
}

TEST(MjAsmMatcherTest, DslConstValueConstraintFiltersWindows)
{
    CodeDom dom;
    (void)dom.append(imm(Op::BIPUSH, 5));
    (void)dom.append(simple(Op::POP));
    (void)dom.append(imm(Op::BIPUSH, 6));
    (void)dom.append(simple(Op::POP));

    auto applied = applyDsl(dom, "load(I=5) - pop => nothing");
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());
    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    const Insn* left = dom.peek(dom.order()[0]);
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->a, 6);
}

TEST(MjAsmMatcherTest, DslVarLoadAndStoreAreAlwaysTrueSteps)
{
    // 照旧语义：vload/vstore 编译为恒真步骤。
    CodeDom dom;
    (void)dom.append(simple(Op::NOP));
    (void)dom.append(simple(Op::POP));
    (void)dom.append(simple(Op::RETURN));

    auto applied = applyDsl(dom, "vload(I:v) - pop => nothing");
    ASSERT_TRUE(applied.is_ok());
    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);

    CodeDom dom2;
    (void)dom2.append(simple(Op::ISTORE_0));
    (void)dom2.append(simple(Op::POP));
    (void)dom2.append(simple(Op::RETURN));
    auto applied2 = applyDsl(dom2, "vstore(I:x) - pop => nothing");
    ASSERT_TRUE(applied2.is_ok());
    ASSERT_EQ(opsOf(dom2).size(), 1u);
}

TEST(MjAsmMatcherTest, DslBindConsistencyFailureKeepsDom)
{
    CodeDom dom;
    (void)dom.append(imm(Op::BIPUSH, 3));
    (void)dom.append(imm(Op::BIPUSH, 4));

    auto applied = applyDsl(dom, "load(I:a) - load(I:a) => nothing");
    ASSERT_TRUE(applied.is_err());
    EXPECT_EQ(applied.unwrap_err().kind, MatchError::DslSemantic);
    EXPECT_EQ(dom.order().size(), 2u);
}

TEST(MjAsmMatcherTest, DslUnboundVariableFailsApply)
{
    CodeDom dom;
    (void)dom.append(simple(Op::IADD));

    auto applied = applyDsl(dom, "iadd => load(I: x + 1)");
    ASSERT_TRUE(applied.is_err());
    EXPECT_EQ(applied.unwrap_err().kind, MatchError::UnboundVariable);
    EXPECT_EQ(dom.order().size(), 1u);
}

TEST(MjAsmMatcherTest, DslEmitBeyondConstFormsFails)
{
    // long 常量超出 lconst_0/1 需要 ldc2_w + CP：纯 DOM 层明确失败。
    CodeDom dom;
    (void)dom.append(simple(Op::LCONST_1));
    (void)dom.append(simple(Op::POP));

    auto applied = applyDsl(dom, "lconst_1 - pop => load(J: 1 + 5)");
    ASSERT_TRUE(applied.is_err());
    EXPECT_EQ(applied.unwrap_err().kind, MatchError::EmitUnsupported);
    EXPECT_EQ(dom.order().size(), 2u);   // 窗口未动
}

TEST(MjAsmMatcherTest, DslMultiRuleTextAppliesAllRules)
{
    CodeDom dom;
    (void)dom.append(simple(Op::ICONST_1));
    (void)dom.append(simple(Op::POP));
    (void)dom.append(simple(Op::ICONST_2));
    (void)dom.append(simple(Op::POP));
    (void)dom.append(simple(Op::RETURN));

    PatternMatcher matcher;
    auto added = MiniDslCompiler::compileAllAndAdd(
        matcher,
        "iconst_1 - pop => nothing\n---\niconst_2 - pop => nothing");
    ASSERT_TRUE(added.is_ok());

    auto applied = matcher.apply(dom);
    ASSERT_TRUE(applied.is_ok());
    EXPECT_TRUE(applied.unwrap());
    const std::vector<Op> ops = opsOf(dom);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(MjAsmMatcherTest, DslOpcodeLookupIsCaseInsensitive)
{
    CodeDom dom;
    (void)dom.append(simple(Op::NOP));

    auto applied = applyDsl(dom, "NOP => POP");
    ASSERT_TRUE(applied.is_ok());
    EXPECT_EQ(dom.peek(dom.order()[0])->op, Op::POP);
}
