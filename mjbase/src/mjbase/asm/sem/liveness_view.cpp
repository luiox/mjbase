/// @file        liveness_view.cpp
/// @brief       LivenessView 构建：块级后向 may-liveness 不动点。
#include "mjbase/asm/sem/liveness_view.hpp"

#include <algorithm>
#include <utility>

#include "jbc/core/opcode.hpp"

#include "mjbase/asm/insn/insn_op.hpp"

namespace mjbase::asm_sem {

using jbc::core::Op;

namespace {

/// 局部操作数槽号（紧凑形态按 opcode 解出，与 outline_plan 同口径）。
ca::u32 localOperandSlot(const jbc::ir::Insn& in) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(in.op);
    const bool compact = (v >= static_cast<ca::u8>(Op::ILOAD_0)
                          && v <= static_cast<ca::u8>(Op::ALOAD_3))
                         || (v >= static_cast<ca::u8>(Op::ISTORE_0)
                             && v <= static_cast<ca::u8>(Op::ASTORE_3));
    if (compact) {
        const ca::u8 base = v <= static_cast<ca::u8>(Op::ALOAD_3)
                                ? static_cast<ca::u8>(Op::ILOAD_0)
                                : static_cast<ca::u8>(Op::ISTORE_0);
        return static_cast<ca::u32>(v - base) % 4u;
    }
    return static_cast<ca::u32>(in.a);
}

/// long/double（cat2）局部形态：store 连带杀高半槽。
bool isCat2LocalOp(Op op) noexcept
{
    return op == Op::LLOAD || op == Op::DLOAD || op == Op::LSTORE || op == Op::DSTORE;
}

}   // namespace

LivenessView::LocalFx LivenessView::localFxOf(const jbc::ir::Insn& in) noexcept
{
    LocalFx fx;
    fx.slot    = localOperandSlot(in);
    fx.cat2    = isCat2LocalOp(in.op);
    fx.isLoad  = asm_insn::isLoad(in.op);
    fx.isStore = asm_insn::isStore(in.op);
    fx.isInc   = in.op == Op::IINC;
    fx.isRet   = in.op == Op::RET;
    return fx;
}

ca::core::Result<LivenessView, mjbase::asm_cfg::CfgError> LivenessView::build(jbc::ir::CodeUnit& unit)
{
    auto built = mjbase::asm_cfg::ControlFlowGraph::build(unit);
    if (built.is_err()) {
        return ca::core::Err(built.unwrap_err());
    }
    LivenessView view;
    view.cfg_ = std::move(built).unwrap();

    const jbc::ir::CodeDom& dom = unit.dom;
    const std::vector<jbc::ir::InsnId>& order = dom.order();

    /// 逐指令局部 def/use 事实 + 槽位上界（含 cat2 高半槽余量）。
    ca::u32 maxSlot = 0;
    view.fx_.reserve(order.size());
    for (const jbc::ir::InsnId id : order) {
        const jbc::ir::Insn* in = dom.peek(id);
        const LocalFx fx = in != nullptr ? localFxOf(*in) : LocalFx{};
        if (in != nullptr && (fx.isLoad || fx.isStore || fx.isInc || fx.isRet)) {
            maxSlot = std::max(maxSlot, fx.slot + (fx.cat2 ? 2u : 1u));
        }
        view.fx_.push_back(fx);
        view.posOf_.emplace(id.slot, view.fx_.size() - 1);
    }
    view.slotCount_ = maxSlot;
    view.liveOut_.assign(order.size(), std::vector<bool>(maxSlot, false));
    if (maxSlot == 0u) {
        return ca::core::Ok(std::move(view));
    }

    /// 块内逆序迁移：in(i) = (out(i) − def) ∪ use；out(i) 顺手挂表，
    /// 返回块入状态。迁移只依赖后继入状态，块内一次逆扫即精确。
    const auto transferBackward = [&](ca::u32 blockIdx, std::vector<bool> out) {
        const mjbase::asm_cfg::BasicBlock* b =
            view.cfg_.block(mjbase::asm_cfg::BlockId{blockIdx});
        for (ca::usize i = b->insns.size(); i-- > 0;) {
            const auto it = view.posOf_.find(b->insns[i].slot);
            if (it == view.posOf_.end()) {
                continue;
            }
            view.liveOut_[it->second] = out;
            const LocalFx& fx = view.fx_[it->second];
            if (fx.isStore || fx.isInc) {
                if (fx.slot < out.size()) {
                    out[fx.slot] = false;
                }
                if (fx.cat2 && fx.slot + 1u < out.size()) {
                    out[fx.slot + 1u] = false;
                }
            }
            if (fx.isLoad || fx.isInc || fx.isRet) {
                if (fx.slot < out.size()) {
                    out[fx.slot] = true;
                }
            }
        }
        return out;
    };

    /// 不动点：逆块序迭代（may-liveness 单调，环上收敛）。
    const ca::u32 n = static_cast<ca::u32>(view.cfg_.blockCount());
    std::vector<std::vector<bool>> blockIn(n, std::vector<bool>(maxSlot, false));
    bool changed = true;
    while (changed) {
        changed = false;
        for (ca::u32 bi = n; bi-- > 0;) {
            const mjbase::asm_cfg::BasicBlock* b =
                view.cfg_.block(mjbase::asm_cfg::BlockId{bi});
            if (b == nullptr || b->insns.empty()) {
                continue;
            }
            std::vector<bool> out(maxSlot, false);
            for (const mjbase::asm_cfg::BlockId succ :
                 view.cfg_.successors(mjbase::asm_cfg::BlockId{bi})) {
                if (succ.valid() && succ.index < n) {
                    for (ca::u32 s = 0; s < maxSlot; ++s) {
                        out[s] = out[s] || blockIn[succ.index][s];
                    }
                }
            }
            std::vector<bool> in = transferBackward(bi, std::move(out));
            if (in != blockIn[bi]) {
                blockIn[bi] = std::move(in);
                changed = true;
            }
        }
    }

    return ca::core::Ok(std::move(view));
}

bool LivenessView::localLiveAfter(jbc::ir::InsnId id, ca::u32 slot) const noexcept
{
    const auto it = posOf_.find(id.slot);
    if (it == posOf_.end() || slot >= slotCount_) {
        return false;
    }
    const std::vector<bool>& out = liveOut_[it->second];
    return slot < out.size() && out[slot];
}

bool LivenessView::localLiveBefore(jbc::ir::InsnId id, ca::u32 slot) const noexcept
{
    const auto it = posOf_.find(id.slot);
    if (it == posOf_.end() || slot >= slotCount_) {
        return false;
    }
    /// in = (out − def) ∪ use：out 集过本指令 fx 迁移即得。
    const LocalFx& fx = fx_[it->second];
    if (fx.isLoad || fx.isInc || fx.isRet) {
        return fx.slot == slot;
    }
    const std::vector<bool>& out = liveOut_[it->second];
    const bool outLive = slot < out.size() && out[slot];
    if (fx.isStore) {
        return false;   /// store 定义即杀：in 不含本槽（cat2 高半槽同杀）
    }
    return outLive;     /// 无局部效应指令：in = out
}

std::vector<ca::u32> LivenessView::liveAfterSlots(jbc::ir::InsnId id) const
{
    std::vector<ca::u32> slots;
    const auto it = posOf_.find(id.slot);
    if (it == posOf_.end()) {
        return slots;
    }
    const std::vector<bool>& out = liveOut_[it->second];
    for (ca::u32 s = 0; s < out.size(); ++s) {
        if (out[s]) {
            slots.push_back(s);
        }
    }
    return slots;
}

}   // namespace mjbase::asm_sem
