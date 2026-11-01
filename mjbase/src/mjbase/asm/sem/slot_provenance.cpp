/// @file        slot_provenance.cpp
/// @brief       SlotProvenanceView 构建：def_use 复用 + 保真帧 + DUP 族补充边。
#include "mjbase/asm/sem/slot_provenance.hpp"

#include "jbc/core/opcode.hpp"

namespace mjbase::asm_sem {

namespace {

/// DUP 族判定与被复制槽位数（1 或 2）；SWAP 只重排不复制，不入族。
/// 返回 0 = 非 DUP 族。
ca::u32 dupSlotCount(jbc::core::Op op) noexcept
{
    switch (op) {
    case jbc::core::Op::DUP:
    case jbc::core::Op::DUP_X1:
    case jbc::core::Op::DUP_X2:
        return 1;
    case jbc::core::Op::DUP2:
    case jbc::core::Op::DUP2_X1:
    case jbc::core::Op::DUP2_X2:
        return 2;
    default:
        return 0;
    }
}

ca::u64 defKey(mjbase::asm_analyzer::DefId d) noexcept
{
    return (static_cast<ca::u64>(d.kind) << 32) | d.id;
}

}   // namespace

SlotProvenanceView SlotProvenanceView::build(const jbc::core::ConstantPool& cp,
                                             const jbc::ir::CodeUnit& u,
                                             const jbc::analysis::MethodSig& sig)
{
    SlotProvenanceView view;

    const mjbase::asm_analyzer::MethodDefUse base = mjbase::asm_analyzer::analyzeDefUse(cp, u, sig);
    view.summary_ = base.summary();

    mjbase::asm_analyzer::DefUseDomain dom;
    jbc::analysis::DomainEngine<mjbase::asm_analyzer::DefUseValue> engine(cp, dom);
    const jbc::analysis::DomainResult<mjbase::asm_analyzer::DefUseValue> r = engine.analyze(u, sig);

    const std::vector<jbc::ir::InsnId>& order = u.dom.order();
    if (!base.ok() || !r.summary.ok || r.frames.size() != order.size()) {
        return view;   /// ok()=false：边/访问/帧全部保持空，不产部分事实。
    }

    view.analyzed_    = true;
    view.truncated_   = base.truncated();
    view.frames_      = r.frames;
    view.edges_       = base.edges();
    view.localAccesses_ = base.localAccesses();

    view.posOf_.reserve(order.size() * 2);
    view.insnBySlot_.reserve(order.size());
    for (ca::usize i = 0; i < order.size(); ++i) {
        view.posOf_.emplace(order[i].slot, i);
        view.insnBySlot_.emplace(order[i].slot, order[i]);
    }


    view.appendDupEdges(u, view.frames_);

    for (ca::u32 i = 0; i < view.edges_.size(); ++i) {
        view.edgesByDef_[defKey(view.edges_[i].def)].push_back(i);
        view.edgesByUse_[view.edges_[i].use.slot].push_back(i);
    }
    return view;
}

void SlotProvenanceView::appendDupEdges(
    const jbc::ir::CodeUnit& u, const std::vector<jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>>& frames)
{
    const std::vector<jbc::ir::InsnId>& order = u.dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) {
        const jbc::ir::InsnId id = order[i];
        const jbc::ir::Insn* in = u.dom.peek(id);
        const jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>& f = frames[i];
        if (in == nullptr || !f.reachable) {
            continue;
        }
        const ca::u32 slots = dupSlotCount(in->op);
        if (slots == 0) {
            continue;
        }

        /// 被复制的槽位 = 顶部 slots 个槽行（引擎栈布局 [垫底行, 值行]，
        /// 值行恒在 back）；垫底行不承载定义也不计值深度（与边 index 的
        /// 值计口径一致）。DUP2 的两种形态（单 cat2 / 双 cat1）由此统一。
        ca::u32 depth = 0;
        for (ca::u32 taken = 0; taken < slots; ++taken) {
            const ca::usize r = f.stack.size() - 1 - taken;
            if (f.stack.size() <= taken || r >= f.stack.size()) {
                break;
            }
            const jbc::analysis::SlotV<mjbase::asm_analyzer::DefUseValue>& row = f.stack[r];
            if (row.bottom) {
                continue;
            }
            const mjbase::asm_analyzer::DefSet& defs = row.val.defs;
            for (ca::u32 k = 0; k < defs.size(); ++k) {
                edges_.push_back(
                    mjbase::asm_analyzer::DefUseEdge{defs.at(k), id, mjbase::asm_analyzer::DefUseSlot::Stack, depth, defs.truncated()});
            }
            if (defs.truncated()) {
                truncated_ = true;
            }
            ++depth;
        }
    }
}

const jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>*
SlotProvenanceView::frameOf(jbc::ir::InsnId id) const noexcept
{
    const auto it = posOf_.find(id.slot);
    if (it == posOf_.end() || it->second >= frames_.size()) {
        return nullptr;
    }
    const jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>& f = frames_[it->second];
    return f.reachable ? &f : nullptr;
}

ca::u32 SlotProvenanceView::stackValueCount(jbc::ir::InsnId id) const noexcept
{
    const jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>* f = frameOf(id);
    if (f == nullptr) {
        return 0;
    }
    ca::u32 count = 0;
    for (const jbc::analysis::SlotV<mjbase::asm_analyzer::DefUseValue>& row : f->stack) {
        if (!row.bottom) {
            ++count;
        }
    }
    return count;
}

mjbase::asm_analyzer::DefSet SlotProvenanceView::stackDefsAt(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept
{
    const jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>* f = frameOf(id);
    if (f == nullptr) {
        return mjbase::asm_analyzer::DefSet{};
    }
    ca::u32 seen = 0;
    for (ca::usize i = f->stack.size(); i-- > 0;) {
        if (f->stack[i].bottom) {
            continue;
        }
        if (seen == fromTop) {
            return f->stack[i].val.defs;
        }
        ++seen;
    }
    return mjbase::asm_analyzer::DefSet{};
}

mjbase::asm_analyzer::DefSet SlotProvenanceView::localDefsAt(jbc::ir::InsnId id, ca::u32 local) const noexcept
{
    const jbc::analysis::FrameV<mjbase::asm_analyzer::DefUseValue>* f = frameOf(id);
    if (f == nullptr || local >= f->locals.size() || f->locals[local].bottom) {
        return mjbase::asm_analyzer::DefSet{};
    }
    return f->locals[local].val.defs;
}

bool SlotProvenanceView::stackSlotClean(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept
{
    const mjbase::asm_analyzer::DefSet defs = stackDefsAt(id, fromTop);
    return defs.size() == 1 && !defs.truncated();
}

bool SlotProvenanceView::localSlotClean(jbc::ir::InsnId id, ca::u32 local) const noexcept
{
    const mjbase::asm_analyzer::DefSet defs = localDefsAt(id, local);
    return defs.size() == 1 && !defs.truncated();
}

std::vector<const mjbase::asm_analyzer::DefUseEdge*> SlotProvenanceView::usesOf(mjbase::asm_analyzer::DefId def) const
{
    std::vector<const mjbase::asm_analyzer::DefUseEdge*> out;
    const auto it = edgesByDef_.find(defKey(def));
    if (it == edgesByDef_.end()) {
        return out;
    }
    out.reserve(it->second.size());
    for (const ca::u32 i : it->second) {
        out.push_back(&edges_[i]);
    }
    return out;
}

std::vector<const mjbase::asm_analyzer::DefUseEdge*> SlotProvenanceView::usesAt(jbc::ir::InsnId use) const
{
    std::vector<const mjbase::asm_analyzer::DefUseEdge*> out;
    const auto it = edgesByUse_.find(use.slot);
    if (it == edgesByUse_.end()) {
        return out;
    }
    out.reserve(it->second.size());
    for (const ca::u32 i : it->second) {
        out.push_back(&edges_[i]);
    }
    return out;
}

const jbc::ir::InsnId* SlotProvenanceView::instructionOf(mjbase::asm_analyzer::DefId def) const
{
    if (def.kind != mjbase::asm_analyzer::DefId::Kind::Instruction) {
        return nullptr;
    }
    const auto it = insnBySlot_.find(def.id);
    return it != insnBySlot_.end() ? &it->second : nullptr;
}

}   // namespace mjbase::asm_sem
