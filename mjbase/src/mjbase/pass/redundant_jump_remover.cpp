#include "mjbase/pass/redundant_jump_remover.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

bool isGotoOpT(Op op) noexcept
{
    return op == Op::GOTO || op == Op::GOTO_W;
}

/// 单值条件跳（弹 1 个条件值）。
bool isOneValueCond(Op op) noexcept
{
    return (op >= Op::IFEQ && op <= Op::IFLE) || op == Op::IFNULL || op == Op::IFNONNULL;
}

/// 双值条件跳（弹 2 个条件值）。
bool isTwoValueCond(Op op) noexcept
{
    return (op >= Op::IF_ICMPEQ && op <= Op::IF_ICMPLE) || op == Op::IF_ACMPEQ
           || op == Op::IF_ACMPNE;
}

/// 跳转目标是否落在紧邻的顺序后继前置边界（末尾指令无顺序后继，天然
/// 不匹配——方法末形态不构成本件的输入面）。
bool targetsSuccessor(const jbc::ir::CodeDom& dom, const std::vector<InsnId>& order,
                      ca::usize i, LabelId target)
{
    if (target == jbc::ir::kNoLabel) return false;
    if (i + 1 >= order.size()) return false;
    for (const LabelId l : dom.labelsAt(order[i + 1])) {
        if (l == target) return true;
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses RedundantJumpRemover::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeDom&   dom    = scope.code.dom;
    jbc::ir::CodeEditor editor(scope.code);

    bool modified  = false;
    bool anyChange = true;
    while (anyChange) {
        anyChange = false;
        // 每轮快照：改写只发生在游标之前，快照句柄全程有效。
        const std::vector<InsnId> ids = dom.order();
        const ca::usize           n   = ids.size();
        for (ca::usize i = 0; i < n; ++i) {
            const jbc::ir::Insn* jump = dom.peek(ids[i]);
            if (jump == nullptr) continue;
            const bool isGoto = isGotoOpT(jump->op);
            const bool isCond = isOneValueCond(jump->op) || isTwoValueCond(jump->op);
            if (!isGoto && !isCond) continue;
            if (!targetsSuccessor(dom, ids, i, jump->target)) continue;

            if (isGoto) {
                // fallthrough 语义等价，删除（匹配保证有顺序后继承接引用；
                // 删除意外失败即中止本轮，防御性保守）。
                const auto erased = editor.erase(
                    ids[i], jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
                if (erased.is_err()) break;
            }
            else {
                // 条件跳 → POP 序列（原位改写 + 双值补插一条）。
                const bool twoValues = isTwoValueCond(jump->op);
                auto       got       = dom.get(ids[i]);
                if (got.is_err()) break;
                jbc::ir::Insn* p = std::move(got).unwrap();
                p->op     = Op::POP;
                p->target = jbc::ir::kNoLabel;
                p->a      = 0;
                p->b      = 0;
                p->cp     = 0;
                p->wide   = false;
                if (twoValues) {
                    const auto ins = editor.insertAfter(ids[i],
                                                        jbc::ir::CodeEditor::simple(Op::POP));
                    if (ins.is_err()) break;
                }
            }
            modified  = true;
            anyChange = true;
            break;   // 重开一轮：改写暴露的新形态由下一轮快照接续
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
