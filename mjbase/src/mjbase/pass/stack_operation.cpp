#include "mjbase/pass/stack_operation.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

#include "mjbase/asm/matcher/match_step.hpp"
#include "mjbase/asm/matcher/pattern_matcher.hpp"
#include "mjbase/asm/matcher/step_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using mjbase::asm_matcher::MatchContext;
using mjbase::asm_matcher::MatchRule;
using mjbase::asm_matcher::MatchStep;
using mjbase::asm_matcher::MatcherError;
using mjbase::asm_matcher::PatternMatcher;
using mjbase::asm_matcher::Steps;
using ca::core::Ok;

/// 无操作数指令值（POP/POP2/DUP 族重建用）。
jbc::ir::Insn plain(Op op)
{
    jbc::ir::Insn in;
    in.op = op;
    return in;
}

/// 命中步骤首指令的值拷贝。
void appendCloned(MatchContext& ctx, ca::usize stepIndex)
{
    ctx.replacement.push_back(ctx.matchedInsns[stepIndex]);
}

/// 策略收口：向 replacement 追加若干指令后返回 Ok。
template<typename... InsnT>
ca::core::Result<void, MatcherError> emit(MatchContext& ctx, InsnT... insns)
{
    (ctx.replacement.push_back(std::move(insns)), ...);
    return Ok();
}

/// 消除规则：显式空替换策略 = 整窗删除。无策略规则在 apply 里是「止扫且不
/// 报告改动」的查询语义，不会删除窗口——消除必须走这条helper。
MatchRule eliminate(std::vector<MatchStep> steps)
{
    return MatchRule(std::move(steps), [](MatchContext&) { return Ok(); });
}

PatternMatcher& stackMatcher()
{
    static PatternMatcher matcher = [] {
        PatternMatcher m;

        // ── 消除（空替换 = 整窗删除）──
        m.addRule(eliminate({MatchStep::op(Op::DUP), MatchStep::op(Op::POP)}));
        m.addRule(eliminate({MatchStep::op(Op::SWAP), MatchStep::op(Op::SWAP)}));
        m.addRule(eliminate({MatchStep::op(Op::DUP2), MatchStep::op(Op::POP2)}));

        // cat1 装载 - pop => ∅
        m.addRule(eliminate({Steps::loadInt(), MatchStep::op(Op::POP)}));
        m.addRule(eliminate({Steps::loadFloat(), MatchStep::op(Op::POP)}));
        m.addRule(eliminate({Steps::loadStr(), MatchStep::op(Op::POP)}));
        // cat2 装载 - pop2 => ∅
        m.addRule(eliminate({Steps::loadLong(), MatchStep::op(Op::POP2)}));
        m.addRule(eliminate({Steps::loadDouble(), MatchStep::op(Op::POP2)}));
        // cat1 装载 ×2 - pop2 => ∅
        m.addRule(eliminate({Steps::loadInt(), Steps::loadInt(), MatchStep::op(Op::POP2)}));
        m.addRule(eliminate({Steps::loadFloat(), Steps::loadFloat(), MatchStep::op(Op::POP2)}));

        // ── 合并 ──
        m.addRule(MatchRule({MatchStep::op(Op::POP), MatchStep::op(Op::POP)},
                            [](MatchContext& ctx) { return emit(ctx, plain(Op::POP2)); }));
        m.addRule(MatchRule({MatchStep::op(Op::DUP), MatchStep::op(Op::POP2)},
                            [](MatchContext& ctx) { return emit(ctx, plain(Op::POP)); }));
        m.addRule(MatchRule({Steps::loadInt(), MatchStep::op(Op::POP2)},
                            [](MatchContext& ctx) { return emit(ctx, plain(Op::POP)); }));
        m.addRule(MatchRule({Steps::loadFloat(), MatchStep::op(Op::POP2)},
                            [](MatchContext& ctx) { return emit(ctx, plain(Op::POP)); }));
        m.addRule(MatchRule({Steps::loadStr(), MatchStep::op(Op::POP2)},
                            [](MatchContext& ctx) { return emit(ctx, plain(Op::POP)); }));
        m.addRule(MatchRule({MatchStep::op(Op::DUP_X1), MatchStep::op(Op::SWAP)},
                            [](MatchContext& ctx) { return emit(ctx, plain(Op::DUP)); }));

        // cat1 装载 - dup => 装载 ×2
        m.addRule(MatchRule({Steps::loadInt(), MatchStep::op(Op::DUP)},
                            [](MatchContext& ctx) {
                                appendCloned(ctx, 0);
                                return emit(ctx, ctx.matchedInsns[0]);
                            }));
        m.addRule(MatchRule({Steps::loadFloat(), MatchStep::op(Op::DUP)},
                            [](MatchContext& ctx) {
                                appendCloned(ctx, 0);
                                return emit(ctx, ctx.matchedInsns[0]);
                            }));
        m.addRule(MatchRule({Steps::loadStr(), MatchStep::op(Op::DUP)},
                            [](MatchContext& ctx) {
                                appendCloned(ctx, 0);
                                return emit(ctx, ctx.matchedInsns[0]);
                            }));
        // cat2 装载 - dup2 => 装载 ×2
        m.addRule(MatchRule({Steps::loadLong(), MatchStep::op(Op::DUP2)},
                            [](MatchContext& ctx) {
                                appendCloned(ctx, 0);
                                return emit(ctx, ctx.matchedInsns[0]);
                            }));
        m.addRule(MatchRule({Steps::loadDouble(), MatchStep::op(Op::DUP2)},
                            [](MatchContext& ctx) {
                                appendCloned(ctx, 0);
                                return emit(ctx, ctx.matchedInsns[0]);
                            }));
        // cat1 装载 - swap - pop => pop - 装载
        m.addRule(MatchRule({Steps::loadInt(), MatchStep::op(Op::SWAP), MatchStep::op(Op::POP)},
                            [](MatchContext& ctx) {
                                return emit(ctx, plain(Op::POP), ctx.matchedInsns[0]);
                            }));
        m.addRule(MatchRule({Steps::loadFloat(), MatchStep::op(Op::SWAP), MatchStep::op(Op::POP)},
                            [](MatchContext& ctx) {
                                return emit(ctx, plain(Op::POP), ctx.matchedInsns[0]);
                            }));
        m.addRule(MatchRule({Steps::loadStr(), MatchStep::op(Op::SWAP), MatchStep::op(Op::POP)},
                            [](MatchContext& ctx) {
                                return emit(ctx, plain(Op::POP), ctx.matchedInsns[0]);
                            }));

        // cat1 装载(1) - 装载(2) - swap => 装载(2) - 装载(1)
        m.addRule(MatchRule({Steps::loadInt(), Steps::loadInt(), MatchStep::op(Op::SWAP)},
                            [](MatchContext& ctx) {
                                return emit(ctx, ctx.matchedInsns[1], ctx.matchedInsns[0]);
                            }));
        m.addRule(MatchRule({Steps::loadFloat(), Steps::loadFloat(), MatchStep::op(Op::SWAP)},
                            [](MatchContext& ctx) {
                                return emit(ctx, ctx.matchedInsns[1], ctx.matchedInsns[0]);
                            }));

        // 装载(1) - 装载(2) - dup_x1 - pop2 => 装载(2)
        m.addRule(MatchRule(
            {Steps::loadInt(), Steps::loadInt(), MatchStep::op(Op::DUP_X1), MatchStep::op(Op::POP2)},
            [](MatchContext& ctx) { return emit(ctx, ctx.matchedInsns[1]); }));
        m.addRule(MatchRule(
            {Steps::loadFloat(), Steps::loadFloat(), MatchStep::op(Op::DUP_X1),
             MatchStep::op(Op::POP2)},
            [](MatchContext& ctx) { return emit(ctx, ctx.matchedInsns[1]); }));

        // cat2 装载(1) - 装载(2) - dup2_x2 - pop2 - pop2 => 装载(2)
        m.addRule(MatchRule(
            {Steps::loadLong(), Steps::loadLong(), MatchStep::op(Op::DUP2_X2),
             MatchStep::op(Op::POP2), MatchStep::op(Op::POP2)},
            [](MatchContext& ctx) { return emit(ctx, ctx.matchedInsns[1]); }));
        m.addRule(MatchRule(
            {Steps::loadDouble(), Steps::loadDouble(), MatchStep::op(Op::DUP2_X2),
             MatchStep::op(Op::POP2), MatchStep::op(Op::POP2)},
            [](MatchContext& ctx) { return emit(ctx, ctx.matchedInsns[1]); }));

        // 装载(1) - 装载(2) - dup_x1 - pop => 装载(2) - 装载(1)
        m.addRule(MatchRule(
            {Steps::loadInt(), Steps::loadInt(), MatchStep::op(Op::DUP_X1), MatchStep::op(Op::POP)},
            [](MatchContext& ctx) {
                return emit(ctx, ctx.matchedInsns[1], ctx.matchedInsns[0]);
            }));
        m.addRule(MatchRule(
            {Steps::loadFloat(), Steps::loadFloat(), MatchStep::op(Op::DUP_X1),
             MatchStep::op(Op::POP)},
            [](MatchContext& ctx) {
                return emit(ctx, ctx.matchedInsns[1], ctx.matchedInsns[0]);
            }));

        return m;
    }();
    return matcher;
}

}   // namespace

mjbase::spi::PreservedAnalyses StackOperationSimplifier::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeDom& dom = scope.code.dom;
    if (dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();

    bool changed = false;
    for (;;) {
        // Err（窗口被 DOM 拒绝，典型为异常表边界锚定）保守终止收敛；声明见头注。
        auto applied = stackMatcher().apply(dom);
        if (!applied.is_ok() || !applied.unwrap()) break;
        changed = true;
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
