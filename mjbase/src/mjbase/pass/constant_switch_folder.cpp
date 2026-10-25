#include "mjbase/pass/constant_switch_folder.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

mjbase::spi::PreservedAnalyses ConstantSwitchFolder::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
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
        const std::vector<jbc::ir::InsnId> ids = dom.order();
        for (ca::usize i = 0; i + 1 < ids.size(); ++i) {
            const jbc::ir::Insn* head = dom.peek(ids[i]);
            const jbc::ir::Insn* sw   = dom.peek(ids[i + 1]);
            if (head == nullptr || sw == nullptr) continue;
            if (sw->op != jbc::core::Op::LOOKUPSWITCH
                && sw->op != jbc::core::Op::TABLESWITCH) {
                continue;
            }
            ca::i32 value = 0;
            if (!method_rewrite_util::intValueOf(session, *head, value)) continue;
            // switch 前置边界锚有存活标签（外部路径直接落到 switch 上，
            // 其栈顶 selector 正等待这次弹消费）→ 整窗跳过。头前置边界
            // 不查：从头进入重放整窗，净栈效应为零。
            if (!dom.labelsAt(ids[i + 1]).empty()) continue;

            const jbc::ir::LabelId target =
                method_rewrite_util::switchCaseLabelOf(*sw, value);
            if (target == jbc::ir::kNoLabel) continue;   // 畸形 default，拒收
            if (!dom.resolveLabel(target).is_ok()) continue;   // 悬空目标，拒收

            // 匹配成功。「先删尾、后改头」落笔：switch 删除在前，失败即
            // 中止本轮（顺序末尾无承接者的形态保守跳过），头部改写最后
            // 发生，不会留下半折叠态。目标标签在删除前已拷出。
            const auto delSw = editor.erase(
                ids[i + 1], jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
            if (delSw.is_err()) break;
            auto got = dom.get(ids[i]);
            if (got.is_err()) break;
            jbc::ir::Insn* g = std::move(got).unwrap();
            g->op     = jbc::core::Op::GOTO;
            g->target = target;
            g->a      = 0;
            g->b      = 0;
            g->cp     = 0;
            g->wide   = false;

            modified  = true;
            anyChange = true;
            break;   // 重开一轮：折叠暴露的新相邻窗口由下一轮快照接续
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
