/// @file        insert_prim.cpp
/// @brief       插入原语落地：标签重锚/fallthrough 批量插入与 copy-set
///              前导装载（A5）。
///
/// 机械语义与 A2–A4 apply 内嵌实现逐位一致（提炼即等价，见文件头注）。
#include "mjbase/asm/sem/insert_prim.hpp"

#include <algorithm>
#include <utility>

#include "jbc/ir/code_editor.hpp"

namespace mjbase::asm_sem {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

jbc::ir::Insn localLoadInsn(char descHead, ca::u32 slot) noexcept
{
    jbc::ir::Insn load;
    load.op = loadOpOf(familyOfDesc(descHead));
    load.a  = static_cast<ca::i32>(slot);
    return load;
}

std::vector<jbc::ir::Insn> copySetLoadPreamble(const std::vector<SlotCopy>& params,
                                               SlotCopy::Kind kind, bool slotNew)
{
    std::vector<jbc::ir::Insn> out;
    for (const SlotCopy& p : params) {
        if (p.kind != kind) {
            continue;
        }
        out.push_back(localLoadInsn(std::string_view(p.desc)[0],
                                    slotNew ? p.newSlot : p.oldLocal));
    }
    return out;
}

Result<std::vector<jbc::ir::InsnId>, OutlineError>
insertAnchoredAtLabel(jbc::ir::CodeEditor& ed, jbc::ir::LabelId label,
                      const std::vector<jbc::ir::Insn>& insns)
{
    std::vector<jbc::ir::InsnId> ids;
    ids.reserve(insns.size());
    /// 逆序插入：每条落标签边界、标签重锚到新指令 → 终序 = 程序序。
    for (auto it = insns.rbegin(); it != insns.rend(); ++it) {
        auto ins = ed.insertAtLabel(label, *it);
        if (ins.is_err()) {
            return Err(OutlineError::EditFailed);
        }
        ids.push_back(ins.unwrap());
    }
    std::reverse(ids.begin(), ids.end());
    return Ok(std::move(ids));
}

Result<std::vector<jbc::ir::InsnId>, OutlineError>
insertBeforeInsn(jbc::ir::CodeEditor& ed, jbc::ir::InsnId anchor,
                 const std::vector<jbc::ir::Insn>& insns)
{
    std::vector<jbc::ir::InsnId> ids;
    ids.reserve(insns.size());
    for (const jbc::ir::Insn& in : insns) {
        auto ins = ed.insertBefore(anchor, in);
        if (ins.is_err()) {
            return Err(OutlineError::EditFailed);
        }
        ids.push_back(ins.unwrap());
    }
    return Ok(std::move(ids));
}

jbc::ir::Insn entryDupResetInsn(jbc::core::Op dupOp) noexcept
{
    jbc::ir::Insn dupInsn;
    dupInsn.op = dupOp;
    return dupInsn;
}

}   // namespace mjbase::asm_sem
