/// @file        frame_view.cpp
/// @brief       FrameView 构建：Strict 域装配与句柄索引。
#include "mjbase/asm/sem/frame_view.hpp"

using ca::core::Err;
using ca::core::Ok;

namespace mjbase::asm_sem {

ca::core::Result<FrameView, jbc::analysis::FrameSummary>
FrameView::build(const jbc::core::ConstantPool& cp, const jbc::ir::CodeUnit& u,
                 const jbc::analysis::MethodSig& sig, jbc::analysis::ITypeResolver& resolver)
{
    jbc::analysis::StrictDomain dom(cp, resolver);
    jbc::analysis::DomainEngine<jbc::analysis::VType> engine(cp, dom);
    const jbc::analysis::DomainResult<jbc::analysis::VType> r = engine.analyze(u, sig);
    if (!r.summary.ok) {
        return Err(r.summary);   /// 不可算显式 Err：不交付部分帧，不伪造。
    }

    FrameView view;
    view.summary_ = r.summary;
    view.frames_  = r.frames;

    const std::vector<jbc::ir::InsnId>& order = u.dom.order();
    view.posOf_.reserve(order.size() * 2);
    for (ca::usize i = 0; i < order.size(); ++i) {
        view.posOf_.emplace(order[i].slot, i);
    }
    return Ok(std::move(view));
}

const jbc::analysis::FrameV<jbc::analysis::VType>*
FrameView::frameOf(jbc::ir::InsnId id) const noexcept
{
    const auto it = posOf_.find(id.slot);
    if (it == posOf_.end() || it->second >= frames_.size()) {
        return nullptr;   /// 句柄不属于本方法（slot 未建索引）。
    }
    const jbc::analysis::FrameV<jbc::analysis::VType>& f = frames_[it->second];
    return f.reachable ? &f : nullptr;
}

const jbc::analysis::VType*
FrameView::localAt(jbc::ir::InsnId id, ca::u32 idx) const noexcept
{
    const jbc::analysis::FrameV<jbc::analysis::VType>* f = frameOf(id);
    if (f == nullptr || idx >= f->locals.size() || f->locals[idx].bottom) {
        return nullptr;
    }
    return &f->locals[idx].val;
}

const jbc::analysis::VType*
FrameView::stackFromTop(jbc::ir::InsnId id, ca::u32 fromTop) const noexcept
{
    const jbc::analysis::FrameV<jbc::analysis::VType>* f = frameOf(id);
    if (f == nullptr) {
        return nullptr;
    }
    ca::u32 seen = 0;
    for (ca::usize i = f->stack.size(); i-- > 0;) {
        if (f->stack[i].bottom) {
            continue;   /// cat2 垫底行：不承载值，不计值索引。
        }
        if (seen == fromTop) {
            return &f->stack[i].val;
        }
        ++seen;
    }
    return nullptr;
}

ca::u32 FrameView::stackValueCount(jbc::ir::InsnId id) const noexcept
{
    const jbc::analysis::FrameV<jbc::analysis::VType>* f = frameOf(id);
    if (f == nullptr) {
        return 0;
    }
    ca::u32 count = 0;
    for (const jbc::analysis::SlotV<jbc::analysis::VType>& row : f->stack) {
        if (!row.bottom) {
            ++count;
        }
    }
    return count;
}

bool FrameView::hasUninitThis(jbc::ir::InsnId id) const noexcept
{
    const jbc::analysis::FrameV<jbc::analysis::VType>* f = frameOf(id);
    if (f == nullptr) {
        return false;
    }
    const auto isUninitThis = [](const jbc::analysis::VType& v) noexcept {
        return v.k == jbc::analysis::VKind::UninitThis;
    };
    for (const jbc::analysis::SlotV<jbc::analysis::VType>& row : f->locals) {
        if (!row.bottom && isUninitThis(row.val)) {
            return true;
        }
    }
    for (const jbc::analysis::SlotV<jbc::analysis::VType>& row : f->stack) {
        if (!row.bottom && isUninitThis(row.val)) {
            return true;
        }
    }
    return false;
}

}   // namespace mjbase::asm_sem
