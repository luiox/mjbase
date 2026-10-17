#include "mjbase/pass/junk_code_inserter.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;

/// 插入步距（确定性策略：每第 k 条指令一个候选点）。
constexpr ca::usize kStride = 8;

/// 前驱是否顺序 fallthrough（无条件转移之后不插——死代码无验证价值）。
bool fallsThrough(const Insn& in)
{
    switch (in.op) {
    case Op::GOTO:
    case Op::GOTO_W:
    case Op::ATHROW:
    case Op::RET:
    case Op::RETURN:
    case Op::IRETURN:
    case Op::LRETURN:
    case Op::FRETURN:
    case Op::DRETURN:
    case Op::ARETURN:
        return false;
    default:
        return true;
    }
}

/// 目标指令是否异常 handler 入口（保守跳过，不改 handler 帧验证面）。
bool isHandlerEntry(const jbc::ir::CodeUnit& code, const InsnId id, CodeDom& dom)
{
    for (const jbc::ir::CodeUnit::Exc& e : code.exc) {
        const auto target = dom.resolveLabel(e.handler);
        if (target.is_ok() && !target.unwrap().isEnd() && target.unwrap().insn == id) {
            return true;
        }
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses JunkCodeInserter::run(mjbase::spi::MethodScope&       scope,
                                                     mjbase::spi::MethodAnalysisManager& mam)
{
    CodeDom&            dom = scope.code.dom;
    jbc::ir::CodeEditor editor(scope.code);

    const std::vector<InsnId> ids = dom.order();
    if (ids.size() < 2) return mjbase::spi::PreservedAnalyses::all();

    bool modified = false;
    for (ca::usize i = kStride; i < ids.size(); i += kStride) {
        const Insn* target = dom.peek(ids[i]);
        if (target == nullptr) continue;
        const Insn* prev = dom.peek(ids[i - 1]);
        if (prev == nullptr || !fallsThrough(*prev)) continue;
        if (isHandlerEntry(scope.code, ids[i], dom)) continue;

        // 垃圾分支块插到目标指令之前；End 标签锚定目标指令，块尾 IFEQ 的
        // not-taken 臂与块外前驱都汇入同一目标（标签留在目标上，目标自身
        // 的既有入边不被遮挡）。
        auto endLabel = editor.createLabel(ids[i]);
        if (endLabel.is_err()) continue;

        Insn push;
        push.op = Op::ICONST_0;
        auto pushId = editor.insertBefore(ids[i], push);
        if (pushId.is_err()) continue;
        Insn branch;
        branch.op     = Op::IFEQ;
        branch.target = endLabel.unwrap();
        auto branchId = editor.insertAfter(pushId.unwrap(), branch);
        if (branchId.is_err()) continue;
        Insn deadPush;
        deadPush.op = Op::ICONST_1;
        auto deadId = editor.insertAfter(branchId.unwrap(), deadPush);
        if (deadId.is_err()) continue;
        Insn drop;
        drop.op = Op::POP;
        if (editor.insertAfter(deadId.unwrap(), drop).is_err()) continue;

        modified = true;
    }
    if (!modified) return mjbase::spi::PreservedAnalyses::all();

    // 垃圾块占栈 1（ICONST_0 → IFEQ 弹出；not-taken 臂 ICONST_1 → POP 弹出）。
    if (scope.code.maxStack < 1) {
        scope.code.maxStack = 1;
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
