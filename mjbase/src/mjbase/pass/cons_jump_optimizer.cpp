#include "mjbase/pass/cons_jump_optimizer.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::InsnId;

/// 数值单槽谓词（栈顶 int 常量对 0 的有符号比较）。
bool intPredicate(Op op, ca::i32 v)
{
    switch (op) {
    case Op::IFEQ: return v == 0;
    case Op::IFNE: return v != 0;
    case Op::IFLT: return v < 0;
    case Op::IFGE: return v >= 0;
    case Op::IFGT: return v > 0;
    case Op::IFLE: return v <= 0;
    default:       return false;   // 调用方已按族过滤
    }
}

/// 双槽比较谓词（两个 int 常量的有符号比较）。
bool icmpPredicate(Op op, ca::i32 l, ca::i32 r)
{
    switch (op) {
    case Op::IF_ICMPEQ: return l == r;
    case Op::IF_ICMPNE: return l != r;
    case Op::IF_ICMPLT: return l < r;
    case Op::IF_ICMPGE: return l >= r;
    case Op::IF_ICMPGT: return l > r;
    case Op::IF_ICMPLE: return l <= r;
    default:            return false;   // 调用方已按族过滤
    }
}

bool isIntPredicate(Op op) noexcept
{
    return op >= Op::IFEQ && op <= Op::IFLE;
}

bool isIcmpPredicate(Op op) noexcept
{
    return op >= Op::IF_ICMPEQ && op <= Op::IF_ICMPLE;
}

/// 窗口内部边界守卫：任一内部成员（双槽窗的中位装载/条件跳转、单槽窗
/// 的条件跳转）的前置边界锚有存活标签即拒收——外部路径从窗口中部进入
/// 时，其栈值正等待窗口内的消费，折叠会让它失去消费者。窗口头前置边界
/// 不查：从头进入重放整窗，净栈效应为零。
bool anyInteriorAnchored(jbc::ir::CodeDom& dom, const InsnId* interior, ca::usize n)
{
    for (ca::usize k = 0; k < n; ++k) {
        if (!dom.labelsAt(interior[k]).empty()) return true;
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses ConsJumpOptimizer::run(mjbase::spi::MethodScope&       scope,
                                                      mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    const jbc::ir::ClassSession& session = scope.session;
    jbc::ir::CodeDom&            dom     = scope.code.dom;
    jbc::ir::CodeEditor          editor(scope.code);

    bool modified  = false;
    bool anyChange = true;
    while (anyChange) {
        anyChange = false;
        // 每轮快照：折叠只发生在游标之前的窗口，快照句柄全程有效。
        const std::vector<InsnId> ids = dom.order();
        for (ca::usize i = 0; i + 1 < ids.size(); ++i) {
            const jbc::ir::Insn* head = dom.peek(ids[i]);
            const jbc::ir::Insn* jump = dom.peek(ids[i + 1]);
            if (head == nullptr || jump == nullptr) continue;

            bool twoSlot = false;
            bool taken   = false;
            if (i + 2 < ids.size()) {
                // 双槽窗 [int 常量; int 常量; IF_ICMPxx]。
                const jbc::ir::Insn* mid = jump;
                const jbc::ir::Insn* j2  = dom.peek(ids[i + 2]);
                ca::i32              l   = 0;
                ca::i32              r   = 0;
                if (j2 != nullptr && isIcmpPredicate(j2->op)
                    && method_rewrite_util::intValueOf(session, *head, l)
                    && method_rewrite_util::intValueOf(session, *mid, r)) {
                    const InsnId interior[2] = {ids[i + 1], ids[i + 2]};
                    if (anyInteriorAnchored(dom, interior, 2)) continue;
                    twoSlot = true;
                    taken   = icmpPredicate(j2->op, l, r);
                    jump    = j2;
                }
            }
            if (!twoSlot) {
                // 单槽窗 [int 常量; 数值谓词] / [ACONST_NULL; IFNULL/IFNONNULL]。
                ca::i32 v = 0;
                if (isIntPredicate(jump->op)
                    && method_rewrite_util::intValueOf(session, *head, v)) {
                    taken = intPredicate(jump->op, v);
                }
                else if ((jump->op == Op::IFNULL || jump->op == Op::IFNONNULL)
                         && head->op == Op::ACONST_NULL) {
                    taken = jump->op == Op::IFNULL;
                }
                else {
                    continue;
                }
                const InsnId interior[1] = {ids[i + 1]};
                if (anyInteriorAnchored(dom, interior, 1)) continue;
            }
            if (jump->target == jbc::ir::kNoLabel) continue;

            // 匹配成功。改写按「先删尾、后改头」落笔：条件跳转删除在前，
            // 失败即中止本轮（顺序末尾无承接者的形态保守跳过），头部改写
            // 最后发生——首删成功后中位/头部的承接者已由该删除保证，后续
            // 步骤不会再失败，不会留下半折叠态。目标标签在删除前拷贝。
            const jbc::ir::LabelId target = jump->target;
            const InsnId           jumpId = twoSlot ? ids[i + 2] : ids[i + 1];
            const auto             delJump =
                editor.erase(jumpId, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
            if (delJump.is_err()) break;
            if (twoSlot) {
                const auto delMid = editor.erase(ids[i + 1],
                                                 jbc::ir::CodeEditor::ErasePolicy::
                                                     kRetargetToFallthrough);
                if (delMid.is_err()) break;
            }
            if (taken) {
                auto got = dom.get(ids[i]);
                if (got.is_err()) break;
                jbc::ir::Insn* g = std::move(got).unwrap();
                g->op     = Op::GOTO;
                g->target = target;
                g->a      = 0;
                g->b      = 0;
                g->cp     = 0;
                g->wide   = false;
            }
            else {
                const auto delHead = editor.erase(ids[i],
                                                  jbc::ir::CodeEditor::ErasePolicy::
                                                      kRetargetToFallthrough);
                if (delHead.is_err()) break;
            }
            modified  = true;
            anyChange = true;
            break;   // 重开一轮：折叠暴露的新相邻窗口由下一轮快照接续
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
