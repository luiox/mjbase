/// @file        control_flow_graph.cpp
/// @brief       控制流图构建：切块、普通边分类与异常区间覆盖。
#include "control_flow_graph.hpp"

#include <algorithm>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"

namespace mjbase::asm_cfg {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

constexpr ca::usize kNoIndex = static_cast<ca::usize>(-1);

/// 条件分支（IFEQ..IF_ACMPNE / IFNULL / IFNONNULL）：taken 边走 Condition。
bool isConditionalBranchOp(jbc::core::Op op) noexcept
{
    return (op >= jbc::core::Op::IFEQ && op <= jbc::core::Op::IF_ACMPNE) ||
           op == jbc::core::Op::IFNULL || op == jbc::core::Op::IFNONNULL;
}

/// return 族（IRETURN..RETURN，opcode 值连续）。
bool isReturnOp(jbc::core::Op op) noexcept
{
    return op >= jbc::core::Op::IRETURN && op <= jbc::core::Op::RETURN;
}

/// 块末不再有普通顺序后继的指令：return 族、athrow、ret。
bool endsNormalFlow(jbc::core::Op op) noexcept
{
    return isReturnOp(op) || op == jbc::core::Op::ATHROW || op == jbc::core::Op::RET;
}

/// 终止块判定（对齐旧 isTerminatedBlock）：return 族 + athrow；ret 不算。
bool isTerminatingOp(jbc::core::Op op) noexcept
{
    return isReturnOp(op) || op == jbc::core::Op::ATHROW;
}

}   // namespace

const char* to_string(CfgError e) noexcept
{
    switch (e) {
    case CfgError::Ok:
        return "ok";
    case CfgError::BadTarget:
        return "cfg-bad-target";
    case CfgError::BadExcEntry:
        return "cfg-bad-exc-entry";
    }
    return "cfg-unknown";
}

Result<ControlFlowGraph, CfgError> ControlFlowGraph::build(jbc::ir::CodeUnit& unit)
{
    ControlFlowGraph cfg;

    // ---- 1) 切块：复用 CodeEditor 的 leader 分析（含异常表边界）----
    const jbc::ir::CodeEditor editor(unit);
    std::vector<jbc::ir::CodeEditor::Block> editorBlocks = editor.blocks();
    const auto& order = unit.dom.order();
    const ca::usize nBlocks = editorBlocks.size();

    cfg.blocks_.reserve(nBlocks);
    cfg.succ_.resize(nBlocks);
    cfg.pred_.resize(nBlocks);
    cfg.terminated_.assign(nBlocks, 0);

    // 块表（程序序）+ 指令→块索引 + 块首在 order 中的位置。
    // 块是 order 的连续切分，entryPos 按块成员数累计。
    std::vector<ca::usize> entryPos(nBlocks, 0);
    ca::usize pos = 0;
    for (ca::usize bi = 0; bi < nBlocks; ++bi) {
        BasicBlock blk;
        blk.id = BlockId{static_cast<ca::u32>(bi)};
        blk.entry = editorBlocks[bi].entry;
        blk.insns = std::move(editorBlocks[bi].insns);
        entryPos[bi] = pos;
        pos += blk.insns.size();
        for (const jbc::ir::InsnId id : blk.insns) {
            cfg.blockOf_.emplace(id, static_cast<ca::u32>(bi));
        }
        cfg.blocks_.push_back(std::move(blk));
    }

    const auto blockIndexOf = [&](jbc::ir::InsnId id) -> ca::usize {
        const auto it = cfg.blockOf_.find(id);
        return it == cfg.blockOf_.end() ? kNoIndex : it->second;
    };
    const auto orderPosOf = [&](jbc::ir::InsnId id) -> ca::usize {
        const ca::usize bi = blockIndexOf(id);
        if (bi == kNoIndex) {
            return kNoIndex;
        }
        const auto& insns = cfg.blocks_[bi].insns;
        for (ca::usize k = 0; k < insns.size(); ++k) {
            if (insns[k] == id) {
                return entryPos[bi] + k;
            }
        }
        return kNoIndex;
    };
    const auto addEdge = [&](ca::usize s, ca::usize t, EdgeKind kind, ca::u32 excIndex = 0) {
        const Edge e{BlockId{static_cast<ca::u32>(s)}, BlockId{static_cast<ca::u32>(t)}, kind,
                     excIndex};
        cfg.succ_[s].push_back(e);
        cfg.pred_[t].push_back(e);
    };
    // 控制流目标标签必须解析到本图内的现存指令（End 边界不是合法跳转目标）。
    const auto resolveTargetBlock = [&](jbc::ir::LabelId label) -> Result<ca::usize, CfgError> {
        const auto t = unit.dom.resolveLabel(label);
        if (t.is_err() || t.unwrap().isEnd()) {
            return Err(CfgError::BadTarget);
        }
        const ca::usize bi = blockIndexOf(t.unwrap().insn);
        if (bi == kNoIndex) {
            return Err(CfgError::BadTarget);
        }
        return Ok(bi);
    };
    const auto hasEdgeTo = [&](ca::usize s, ca::usize t, EdgeKind kind) {
        for (const Edge& e : cfg.succ_[s]) {
            if (e.target.index == t && e.kind == kind) {
                return true;
            }
        }
        return false;
    };

    // ---- 2) 普通边：按块末条指令形状分类 ----
    for (ca::usize bi = 0; bi < nBlocks; ++bi) {
        const BasicBlock& blk = cfg.blocks_[bi];
        if (blk.insns.empty()) {
            continue;
        }
        const jbc::ir::Insn* last = unit.dom.peek(blk.insns.back());
        if (last == nullptr) {
            continue;
        }
        const jbc::core::Shape sh = jbc::core::shapeOf(last->op);

        if (sh == jbc::core::Shape::Branch2 || sh == jbc::core::Shape::Branch4) {
            const auto target = resolveTargetBlock(last->target);
            if (target.is_err()) {
                return Err(target.unwrap_err());
            }
            const bool conditional = isConditionalBranchOp(last->op);
            addEdge(bi, target.unwrap(), conditional ? EdgeKind::Condition : EdgeKind::Branch);
            // 顺序后继边：条件分支的 not-taken 路径；jsr 的子程序返回点。
            const bool jsr = last->op == jbc::core::Op::JSR || last->op == jbc::core::Op::JSR_W;
            if ((conditional || jsr) && bi + 1 < nBlocks) {
                addEdge(bi, bi + 1, EdgeKind::FallThrough);
            }
        } else if (sh == jbc::core::Shape::TableSwitch || sh == jbc::core::Shape::LookupSwitch) {
            // switch 永远经 default/case 分发，无顺序落入；default 先记，
            // 同一目标块的重复 case 折叠为一条 Switch 边。
            const auto dflt = resolveTargetBlock(last->target);
            if (dflt.is_err()) {
                return Err(dflt.unwrap_err());
            }
            addEdge(bi, dflt.unwrap(), EdgeKind::Switch);
            for (const jbc::ir::LabelId label : last->targets) {
                const auto t = resolveTargetBlock(label);
                if (t.is_err()) {
                    return Err(t.unwrap_err());
                }
                if (!hasEdgeTo(bi, t.unwrap(), EdgeKind::Switch)) {
                    addEdge(bi, t.unwrap(), EdgeKind::Switch);
                }
            }
        } else {
            if (isTerminatingOp(last->op)) {
                cfg.terminated_[bi] = 1;
            }
            if (!endsNormalFlow(last->op) && bi + 1 < nBlocks) {
                addEdge(bi, bi + 1, EdgeKind::FallThrough);
            }
        }
    }

    // ---- 3) 异常边：覆盖区间 [startPos, endPos) 内的块 → handler ----
    // 切分已保证 exc 三个边界（非 End 时）都是块首，逐块比较块首位置
    // 即得覆盖块集；end 绑定方法末尾（End）时区间延伸至最后一块。
    for (ca::usize ei = 0; ei < unit.exc.size(); ++ei) {
        const jbc::ir::CodeUnit::Exc& e = unit.exc[ei];

        const auto start = unit.dom.resolveLabel(e.start);
        if (start.is_err() || start.unwrap().isEnd()) {
            return Err(CfgError::BadExcEntry);
        }
        const ca::usize startPos = orderPosOf(start.unwrap().insn);
        const auto handler = unit.dom.resolveLabel(e.handler);
        if (handler.is_err() || handler.unwrap().isEnd()) {
            return Err(CfgError::BadExcEntry);
        }
        const ca::usize h = blockIndexOf(handler.unwrap().insn);

        ca::usize endPos = order.size();
        const auto end = unit.dom.resolveLabel(e.end);
        if (end.is_err()) {
            return Err(CfgError::BadExcEntry);
        }
        if (!end.unwrap().isEnd()) {
            endPos = orderPosOf(end.unwrap().insn);
        }
        if (startPos == kNoIndex || h == kNoIndex || endPos == kNoIndex) {
            return Err(CfgError::BadExcEntry);
        }

        for (ca::usize bi = 0; bi < nBlocks; ++bi) {
            if (entryPos[bi] >= startPos && entryPos[bi] < endPos) {
                addEdge(bi, h, EdgeKind::Exception, static_cast<ca::u32>(ei));
            }
        }
    }

    return Ok(std::move(cfg));
}

const BasicBlock* ControlFlowGraph::block(BlockId id) const noexcept
{
    if (!id.valid() || id.index >= blocks_.size()) {
        return nullptr;
    }
    return &blocks_[id.index];
}

BlockId ControlFlowGraph::blockOfInsn(jbc::ir::InsnId insn) const noexcept
{
    const auto it = blockOf_.find(insn);
    return it == blockOf_.end() ? kNoBlock : BlockId{it->second};
}

BlockId ControlFlowGraph::blockOfLabel(const jbc::ir::CodeDom& dom,
                                        jbc::ir::LabelId label) const noexcept
{
    const auto t = dom.resolveLabel(label);
    if (t.is_err() || t.unwrap().isEnd()) {
        return kNoBlock;
    }
    return blockOfInsn(t.unwrap().insn);
}

const std::vector<Edge>& ControlFlowGraph::succEdgesOf(BlockId id) const noexcept
{
    static const std::vector<Edge> kEmpty;
    if (!id.valid() || id.index >= succ_.size()) {
        return kEmpty;
    }
    return succ_[id.index];
}

const std::vector<Edge>& ControlFlowGraph::predEdgesOf(BlockId id) const noexcept
{
    static const std::vector<Edge> kEmpty;
    if (!id.valid() || id.index >= pred_.size()) {
        return kEmpty;
    }
    return pred_[id.index];
}

std::vector<BlockId> ControlFlowGraph::successors(BlockId id) const
{
    std::vector<BlockId> out;
    if (!id.valid() || id.index >= succ_.size()) {
        return out;
    }
    for (const Edge& e : succ_[id.index]) {
        out.push_back(e.target);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<BlockId> ControlFlowGraph::predecessors(BlockId id) const
{
    std::vector<BlockId> out;
    if (!id.valid() || id.index >= pred_.size()) {
        return out;
    }
    for (const Edge& e : pred_[id.index]) {
        out.push_back(e.source);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

ca::usize ControlFlowGraph::edgeCount(EdgeKind kind) const noexcept
{
    ca::usize count = 0;
    for (const auto& edges : succ_) {
        for (const Edge& e : edges) {
            if (e.kind == kind) {
                ++count;
            }
        }
    }
    return count;
}

bool ControlFlowGraph::isTerminated(BlockId id) const noexcept
{
    return id.valid() && id.index < terminated_.size() && terminated_[id.index] != 0;
}

}   // namespace mjbase::asm_cfg
