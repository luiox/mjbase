#include "mjbase/pass/store_load_relay_eliminator.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::DefUseSlot;

/// 装载族槽位/宽度（store 家族的对偶）：五族泛形与 _0..3 快捷形同权。
bool loadLocalSpan(const jbc::ir::Insn& in, ca::u32& base, ca::u32& width)
{
    if (in.op == Op::ILOAD || in.op == Op::FLOAD || in.op == Op::ALOAD) {
        base  = static_cast<ca::u32>(in.a);
        width = 1;
        return true;
    }
    if (in.op == Op::LLOAD || in.op == Op::DLOAD) {
        base  = static_cast<ca::u32>(in.a);
        width = 2;
        return true;
    }
    // 快捷形区段 ILOAD_0..ALOAD_3 连续：每 4 个一族，L/D 族（第 2、4 段）
    // 双槽。
    if (in.op >= Op::ILOAD_0 && in.op <= Op::ALOAD_3) {
        const ca::u32 raw = static_cast<ca::u32>(in.op)
                            - static_cast<ca::u32>(Op::ILOAD_0);
        base  = raw % 4;
        width = (raw / 4 == 1 || raw / 4 == 3) ? 2 : 1;
        return true;
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses StoreLoadRelayEliminator::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeDom&   dom    = scope.code.dom;
    jbc::ir::CodeEditor editor(scope.code);
    if (dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();

    bool modified  = false;
    bool anyChange = true;
    while (anyChange) {
        anyChange = false;
        // 单使用是负向结论：数据流失败或到达集合溢出时整件跳过。
        const jbc::analysis::MethodSig sig{
            (scope.access & mjbase::hierarchy::kAccStatic) != 0, scope.desc};
        const mjbase::asm_analyzer::MethodDefUse du =
            mjbase::asm_analyzer::analyzeDefUse(scope.session.backing().cp, scope.code, sig);
        if (!du.ok() || du.truncated()) break;

        // 每轮快照：本轮删除只发生在游标处相邻对，快照句柄全程有效。
        const std::vector<InsnId> ids = dom.order();
        ca::usize                 i   = 0;
        while (i + 1 < ids.size()) {
            const jbc::ir::Insn* store = dom.peek(ids[i]);
            const jbc::ir::Insn* load  = dom.peek(ids[i + 1]);
            ca::u32 sBase = 0, sWidth = 0, lBase = 0, lWidth = 0;
            if (store == nullptr || load == nullptr
                || !method_rewrite_util::storeLocalSpan(*store, sBase, sWidth)
                || !loadLocalSpan(*load, lBase, lWidth) || sBase != lBase
                || sWidth != lWidth
                // 双侧锚标签一律拒收（fail-closed）：reload 位锚入 = 分支
                // 跳入 reload 位期望弹值在栈，删对后栈形破坏；store 位锚入
                // （分支目标/异常表边界）涉及边界重锚，对齐全家「不重锚边
                // 界」惯例整对保留。
                || method_rewrite_util::labelsAnchorAt(dom, ids[i])
                || method_rewrite_util::labelsAnchorAt(dom, ids[i + 1])) {
                ++i;
                continue;
            }
            // 单使用证明：store 定义的局部读边（slot=Local）恰为紧邻
            // reload。栈消费边是 reload 恒等搬运的转发（对是删后值留栈、
            // 转发面自然保持），不计入；handler 读点（异常边合并）、iinc
            // 读改写、后续任何局部读都在 Local 边上现形。
            const std::vector<const mjbase::asm_analyzer::DefUseEdge*> uses =
                du.usesOf(DefId::instruction(ids[i]));
            ca::usize                               localReads = 0;
            const mjbase::asm_analyzer::DefUseEdge* only       = nullptr;
            for (const mjbase::asm_analyzer::DefUseEdge* e : uses) {
                if (e->slot != DefUseSlot::Local) continue;
                ++localReads;
                only = e;
            }
            if (localReads != 1 || only->use != ids[i + 1]) {
                ++i;
                continue;
            }
            // 顺序删除：先 store（锚引用链式落到 reload），再 reload（引用
            // 随之落到对后继）。双侧已由锚标签守卫保证无锚入；任一步意外
            // 失败即停止本轮，下一轮快照重扫。
            const auto first = editor.erase(
                ids[i], jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
            if (first.is_err()) {
                ++i;
                continue;
            }
            const auto second = editor.erase(
                ids[i + 1], jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
            if (second.is_err()) break;
            modified  = true;
            anyChange = true;
            i += 2;
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
