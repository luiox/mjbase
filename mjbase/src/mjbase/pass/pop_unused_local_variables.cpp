#include "mjbase/pass/pop_unused_local_variables.hpp"

#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {
namespace {

using jbc::core::Op;
using jbc::ir::InsnId;

/// 局部访问的槽位覆盖：base 起的 width 个槽。
struct SlotSpan
{
    ca::u32 base  = 0;
    ca::u32 width = 1;
};

const auto inRange = [](Op op, Op first, Op last) { return op >= first && op <= last; };

/// 短形态（_0.._3）的族内槽位：各族内部连续。
ca::u32 slotOfCompact(Op op, Op first)
{
    return static_cast<ca::u32>(op) - static_cast<ca::u32>(first);
}

/// 装载/iinc/ret 的读槽覆盖；非读点返回 false。
bool loadSpan(const jbc::ir::Insn& in, SlotSpan& span)
{
    switch (in.op) {
    case Op::ILOAD: case Op::FLOAD: case Op::ALOAD:
        span = {static_cast<ca::u32>(in.a), 1};
        return true;
    case Op::LLOAD: case Op::DLOAD:
        span = {static_cast<ca::u32>(in.a), 2};
        return true;
    case Op::IINC:
    case Op::RET:
        // iinc 是读改写、ret 读返回地址槽：均按读点计。
        span = {static_cast<ca::u32>(in.a), 1};
        return true;
    default: break;
    }
    if (inRange(in.op, Op::ILOAD_0, Op::ILOAD_3)) {
        span = {slotOfCompact(in.op, Op::ILOAD_0), 1};
        return true;
    }
    if (inRange(in.op, Op::LLOAD_0, Op::LLOAD_3)) {
        span = {slotOfCompact(in.op, Op::LLOAD_0), 2};
        return true;
    }
    if (inRange(in.op, Op::FLOAD_0, Op::FLOAD_3)) {
        span = {slotOfCompact(in.op, Op::FLOAD_0), 1};
        return true;
    }
    if (inRange(in.op, Op::DLOAD_0, Op::DLOAD_3)) {
        span = {slotOfCompact(in.op, Op::DLOAD_0), 2};
        return true;
    }
    if (inRange(in.op, Op::ALOAD_0, Op::ALOAD_3)) {
        span = {slotOfCompact(in.op, Op::ALOAD_0), 1};
        return true;
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses PopUnUsedLocalVariables::run(mjbase::spi::MethodScope&          scope,
                                                            mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit& cu = scope.code;

    const std::vector<InsnId> order = cu.dom.order();
    const ca::usize           n     = order.size();
    if (n == 0) return mjbase::spi::PreservedAnalyses::all();

    // 读点审计（方法级槽位粒度，指令全集口径）：装载/iinc/ret 覆盖的槽位
    // 全计入；越界即隐式未读（从未标记）。
    std::vector<char> readSlots;
    const auto markRead = [&](const SlotSpan& span) {
        if (readSlots.size() < span.base + span.width) {
            readSlots.resize(span.base + span.width, 0);
        }
        for (ca::u32 s = 0; s < span.width; ++s) readSlots[span.base + s] = 1;
    };
    for (const InsnId id : order) {
        const jbc::ir::Insn* in = cu.dom.peek(id);
        if (in == nullptr) continue;
        SlotSpan span;
        if (loadSpan(*in, span)) markRead(span);
    }
    const auto isRead = [&readSlots](ca::u32 slot) {
        return slot < readSlots.size() && readSlots[slot] != 0;
    };

    // store 降级：覆盖槽位全部无读点 → pop/pop2；只动操作码字段。
    bool modified = false;
    for (const InsnId id : order) {
        const jbc::ir::Insn* view = cu.dom.peek(id);
        if (view == nullptr) continue;
        ca::u32 base  = 0;
        ca::u32 width = 0;
        if (!method_rewrite_util::storeLocalSpan(*view, base, width)) continue;
        const SlotSpan span{base, width};
        bool read = false;
        for (ca::u32 s = 0; s < span.width; ++s) {
            if (isRead(span.base + s)) {
                read = true;
                break;
            }
        }
        if (read) continue;
        auto got = cu.dom.get(id);
        if (got.is_err()) continue;
        jbc::ir::Insn* in = std::move(got).unwrap();
        in->op   = span.width == 2 ? Op::POP2 : Op::POP;
        in->a    = 0;
        in->b    = 0;
        in->wide = false;
        modified = true;
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
