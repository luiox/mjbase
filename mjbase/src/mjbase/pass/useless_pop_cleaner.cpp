#include "mjbase/pass/useless_pop_cleaner.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::ir::CodeDom;
using jbc::ir::InsnId;

/// 一对（局部装载, POP/POP2）能否安全消除。
bool matchPair(CodeDom& dom, const InsnId loadId, const InsnId popId, bool pop2)
{
    const jbc::ir::Insn* load = dom.peek(loadId);
    const jbc::ir::Insn* pop  = dom.peek(popId);
    if (load == nullptr || pop == nullptr) return false;
    if (pop->op != (pop2 ? jbc::core::Op::POP2 : jbc::core::Op::POP)) return false;
    const bool foldable = pop2 ? method_rewrite_util::isPop2FoldableLocalLoad(*load)
                               : method_rewrite_util::isPopFoldableLocalLoad(*load);
    if (!foldable) return false;
    // 边界守卫：POP 前置边界有标签锚定（分支目标/异常表边界）→ 不消除。
    if (method_rewrite_util::labelsAnchorAt(dom, popId)) return false;
    // POP 必须有顺序后继：两次 fallthrough 删除都需要后继承接引用。
    const std::vector<InsnId>& order = dom.order();
    return !order.empty() && order.back() != popId;
}

}   // namespace

mjbase::spi::PreservedAnalyses UselessPopCleaner::run(mjbase::spi::MethodScope&       scope,
                                                      mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeDom&            dom = scope.code.dom;
    jbc::ir::CodeEditor editor(scope.code);

    bool modified  = false;
    bool anyChange = true;
    while (anyChange) {
        anyChange = false;
        // 每轮快照：本轮内删除只发生在游标之前的窗口，快照句柄全程有效。
        const std::vector<InsnId> ids = dom.order();
        ca::usize                 i   = 0;
        while (i + 1 < ids.size()) {
            const bool pop2 = matchPair(dom, ids[i], ids[i + 1], /*pop2*/ true);
            if (!pop2 && !matchPair(dom, ids[i], ids[i + 1], /*pop2*/ false)) {
                ++i;
                continue;
            }
            // 匹配成功，顺序删除：先装载（fallthrough 重定向其引用到 POP），
            // 再 POP（引用随之落到原 POP 的后继）。两步的前置条件已由
            // matchPair 保证（POP 有边界标签时不匹配、POP 有后继）；任一步
            // 意外失败即停止本轮（已删除部分保留），下一轮快照重扫。
            const auto first = editor.erase(ids[i],
                                            jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
            if (first.is_err()) break;
            const auto second = editor.erase(ids[i + 1],
                                             jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
            modified  = true;
            anyChange = true;
            if (second.is_err()) break;
            i += 2;
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
