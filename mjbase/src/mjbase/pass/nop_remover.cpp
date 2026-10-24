#include "mjbase/pass/nop_remover.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::pass {

mjbase::spi::PreservedAnalyses NopRemover::run(mjbase::spi::MethodScope&       scope,
                                               mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeDom& dom = scope.code.dom;

    // order() 快照防遍历失稳：删除只影响序列，槽位句柄在快照里仍然有效。
    const std::vector<jbc::ir::InsnId> ids = dom.order();
    jbc::ir::CodeEditor                editor(scope.code);

    bool modified = false;
    for (const jbc::ir::InsnId id : ids) {
        const jbc::ir::Insn* in = dom.peek(id);
        if (in == nullptr || in->op != jbc::core::Op::NOP) continue;
        // fallthrough 策略：分支目标 NOP 先重定向到顺序后继再删；异常表边界
        // 锚定或末条无后继的 NOP 删除被拒（StillUsed），按声明的偏离保留。
        const auto erased = editor.erase(id, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
        if (erased.is_ok()) {
            modified = true;
            continue;
        }
        // 无引用的 NOP（含末条 NOP）无 use 可迁移，直接删（无条件删除）。
        if (editor.erase(id, jbc::ir::CodeEditor::ErasePolicy::kRejectIfUsed).is_ok()) {
            modified = true;
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
