/// @file        def_use.cpp
/// @brief       slot 级 def-use 实现：域钩子 + 帧后处理（使用点/字段事实）。
#include "def_use.hpp"

#include <algorithm>
#include <utility>

#include "jbc/core/opcode.hpp"

#include "mjbase/asm/insn/insn_op.hpp"

namespace mjbase::asm_analyzer {

namespace {

using jbc::analysis::DomainEngine;
using jbc::analysis::DomainResult;
using jbc::analysis::FrameSummary;
using jbc::analysis::FrameV;
using jbc::analysis::MethodSig;
using jbc::analysis::VKind;
using jbc::core::ConstantPool;
using jbc::core::Op;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;

ca::u8 slotsOfShape(VKind shape) noexcept
{
    return shape == VKind::Long || shape == VKind::Double ? 2 : 1;
}

/// store 的槽宽以 opcode 为准（lstore/dstore 收双槽，与传入值的历史宽度无关）。
ca::u8 storeSlots(Op op) noexcept
{
    switch (op) {
    case Op::LSTORE:
    case Op::LSTORE_0:
    case Op::LSTORE_1:
    case Op::LSTORE_2:
    case Op::LSTORE_3:
    case Op::DSTORE:
    case Op::DSTORE_0:
    case Op::DSTORE_1:
    case Op::DSTORE_2:
    case Op::DSTORE_3: return 2;
    default: return 1;
    }
}

/// 局部读写指令的槽号（紧凑 load/store 形态与显式形态分开解码）。
ca::u32 localIndexOf(const Insn& in) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(in.op);
    const bool compact = (v >= static_cast<ca::u8>(Op::ILOAD_0) &&
                          v <= static_cast<ca::u8>(Op::ALOAD_3)) ||
                         (v >= static_cast<ca::u8>(Op::ISTORE_0) &&
                          v <= static_cast<ca::u8>(Op::ASTORE_3));
    return compact ? jbc::analysis::detail::compactLocalIdx(in.op)
                   : static_cast<ca::u32>(in.a);
}

/// 读局部的指令：load 族、iinc（读后写）、ret。
bool localReadOf(const Insn& in, ca::u32& idx) noexcept
{
    if (mjbase::asm_insn::isLoad(in.op)) {
        idx = localIndexOf(in);
        return true;
    }
    if (in.op == Op::IINC || in.op == Op::RET) {
        idx = static_cast<ca::u32>(in.a);
        return true;
    }
    return false;
}

/// 写局部的指令：store 族与 iinc。
bool localWriteOf(const Insn& in, ca::u32& idx) noexcept
{
    if (mjbase::asm_insn::isStore(in.op)) {
        idx = localIndexOf(in);
        return true;
    }
    if (in.op == Op::IINC) {
        idx = static_cast<ca::u32>(in.a);
        return true;
    }
    return false;
}

/// 指令执行前弹出的槽位数；未知（CP 畸形/枚举外）返回 false。
bool stackPopSlots(const ConstantPool& cp, const Insn& in, ca::u32& out)
{
    using namespace mjbase::asm_insn;
    const ca::i32 pop = stackEffectOf(in.op).pop;
    if (pop >= 0) {
        out = static_cast<ca::u32>(pop);
        return true;
    }
    if (isFieldAccess(in.op)) {
        bool ok = false;
        const std::string_view desc = jbc::analysis::detail::memberDesc(cp, in.cp, ok);
        if (!ok) {
            return false;
        }
        const auto effect = fieldStackEffect(in.op, desc);
        if (effect.is_err()) {
            return false;
        }
        out = static_cast<ca::u32>(effect.unwrap().pop);
        return true;
    }
    if (isInvoke(in.op)) {
        bool ok = false;
        const std::string_view desc = jbc::analysis::detail::memberDesc(cp, in.cp, ok);
        if (!ok) {
            return false;
        }
        const auto effect = invokeStackEffect(in.op, desc);
        if (effect.is_err()) {
            return false;
        }
        out = static_cast<ca::u32>(effect.unwrap().pop);
        return true;
    }
    if (in.op == Op::MULTIANEWARRAY && in.a >= 0) {
        out = static_cast<ca::u32>(in.a);
        return true;
    }
    return false;
}

/// 字段访问三分类；非字段指令返回 false。
bool fieldAccessKind(Op op, bool& write, bool& isStatic) noexcept
{
    switch (op) {
    case Op::GETSTATIC: isStatic = true;  write = false; return true;
    case Op::PUTSTATIC: isStatic = true;  write = true;  return true;
    case Op::GETFIELD:  isStatic = false; write = false; return true;
    case Op::PUTFIELD:  isStatic = false; write = true;  return true;
    default: return false;
    }
}

/// 栈顶第 depth 个**值**的到达定义；depth 按值计（cat2 底槽不计）。
DefSet stackDefsAt(const FrameV<DefUseValue>& f, ca::u32 depth)
{
    ca::usize row = f.stack.size();
    for (ca::u32 i = 0; i <= depth; ++i) {
        if (row == 0) {
            return DefSet{};
        }
        const ca::usize vrow = row - 1;
        if (f.stack[vrow].bottom) {
            return DefSet{};
        }
        if (i == depth) {
            return f.stack[vrow].val.defs;
        }
        const ca::u8 w = (vrow > 0 && f.stack[vrow - 1].bottom) ? 2 : 1;
        row = vrow - (w - 1);
    }
    return DefSet{};
}

}   // namespace

// ---------------------------------------------------------------------------
// DefSet
// ---------------------------------------------------------------------------

bool DefSet::contains(DefId d) const noexcept
{
    for (ca::u32 i = 0; i < size_; ++i) {
        if (defs_[i] == d) {
            return true;
        }
    }
    return false;
}

void DefSet::markTruncated() noexcept
{
    truncated_ = true;
}

bool DefSet::add(DefId d) noexcept
{
    if (!d.valid()) {
        return false;
    }
    ca::u32 pos = 0;
    while (pos < size_ && defs_[pos] < d) {
        ++pos;
    }
    if (pos < size_ && defs_[pos] == d) {
        return false;
    }
    if (size_ < kMaxDefs) {
        for (ca::u32 i = size_; i > pos; --i) {
            defs_[i] = defs_[i - 1];
        }
        defs_[pos] = d;
        ++size_;
        return true;
    }
    // 已满：保留字典序最小的 kMaxDefs 个，保证不同合并顺序得到同一集合。
    truncated_ = true;
    if (!(d < defs_[kMaxDefs - 1])) {
        return false;
    }
    for (ca::u32 i = kMaxDefs - 1; i > pos; --i) {
        defs_[i] = defs_[i - 1];
    }
    defs_[pos] = d;
    return true;
}

bool DefSet::operator==(const DefSet& o) const noexcept
{
    if (size_ != o.size_ || truncated_ != o.truncated_) {
        return false;
    }
    for (ca::u32 i = 0; i < size_; ++i) {
        if (!(defs_[i] == o.defs_[i])) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// DefUseDomain
// ---------------------------------------------------------------------------

DefUseValue DefUseDomain::merge(const DefUseValue& a, const DefUseValue& b)
{
    DefUseValue out = a;
    for (ca::u32 i = 0; i < b.defs.size(); ++i) {
        out.defs.add(b.defs.at(i));
    }
    if (b.defs.truncated()) {
        out.defs.markTruncated();
    }
    out.slots = a.slots > b.slots ? a.slots : b.slots;
    return out;
}

DefUseValue DefUseDomain::newValue(const Insn& producer, InsnId at, VKind shape)
{
    const ca::u8 w = slotsOfShape(shape);
    // 未写过的局部 load：没有可追踪的定义，不把 load 自身伪装成定义。
    if (mjbase::asm_insn::isLoad(producer.op)) {
        return DefUseValue::unknownOf(w);
    }
    return DefUseValue::ofInsn(at, w);
}

DefUseValue DefUseDomain::newParameter(ca::u32 localIdx, VKind shape)
{
    return DefUseValue::ofParam(localIdx, slotsOfShape(shape));
}

DefUseValue DefUseDomain::newException(const CodeUnit::Exc&)
{
    return DefUseValue::unknownOf(1);
}

DefUseValue DefUseDomain::storeLocal(const Insn& store, InsnId id, const DefUseValue&,
                                     ca::u32)
{
    return DefUseValue::ofInsn(id, storeSlots(store.op));
}

bool DefUseDomain::equal(const DefUseValue& a, const DefUseValue& b)
{
    return a.slots == b.slots && a.defs == b.defs;
}

// ---------------------------------------------------------------------------
// MethodDefUse
// ---------------------------------------------------------------------------

MethodDefUse::MethodDefUse(const ConstantPool& cp, const CodeUnit& u, const MethodSig* sig)
{
    DefUseDomain dom;
    DomainEngine<DefUseValue> engine(cp, dom);
    const DomainResult<DefUseValue> result =
        sig != nullptr ? engine.analyze(u, *sig) : engine.analyze(u);
    summary_ = result.summary;
    if (!summary_.ok || result.frames.size() != u.dom.order().size()) {
        return;
    }
    analyzed_ = true;

    const auto& order = u.dom.order();
    for (const InsnId& id : order) {
        insnBySlot_.emplace(id.slot, id);
    }

    for (ca::usize i = 0; i < order.size(); ++i) {
        const InsnId id = order[i];
        const Insn* in  = u.dom.peek(id);
        const FrameV<DefUseValue>& f = result.frames[i];
        if (in == nullptr || !f.reachable) {
            continue;
        }

        ca::u32 pop = 0;
        if (stackPopSlots(cp, *in, pop)) {
            ca::u32 consumed = 0;
            ca::u32 depth    = 0;
            ca::usize row    = f.stack.size();
            while (consumed < pop && row > 0) {
                const ca::usize vrow = row - 1;
                if (f.stack[vrow].bottom) {
                    break;
                }
                const ca::u8 w = (vrow > 0 && f.stack[vrow - 1].bottom) ? 2 : 1;
                if (consumed + w > pop) {
                    break;
                }
                const DefUseValue& v = f.stack[vrow].val;
                for (ca::u32 k = 0; k < v.defs.size(); ++k) {
                    edges_.push_back(
                        DefUseEdge{v.defs.at(k), id, DefUseSlot::Stack, depth,
                                   v.defs.truncated()});
                }
                if (v.defs.truncated()) {
                    truncated_ = true;
                }
                consumed += w;
                ++depth;
                row = vrow - (w - 1);
            }
        }

        ca::u32 local = 0;
        if (localReadOf(*in, local)) {
            DefSet defs;
            if (local < f.locals.size() && !f.locals[local].bottom) {
                defs = f.locals[local].val.defs;
                for (ca::u32 k = 0; k < defs.size(); ++k) {
                    edges_.push_back(
                        DefUseEdge{defs.at(k), id, DefUseSlot::Local, local,
                                   defs.truncated()});
                }
                if (defs.truncated()) {
                    truncated_ = true;
                }
            }
            localAccesses_.push_back(LocalAccess{id, local, LocalAccess::Kind::Read, defs});
        }
        if (localWriteOf(*in, local)) {
            DefSet defs;
            defs.add(DefId::instruction(id));
            localAccesses_.push_back(LocalAccess{id, local, LocalAccess::Kind::Write, defs});
        }

        bool write = false;
        bool isStatic = false;
        if (fieldAccessKind(in->op, write, isStatic)) {
            bool ok = false;
            const std::string_view owner =
                jbc::analysis::detail::refOwnerOf(cp, in->cp, ok);
            if (!ok) {
                unresolvedFieldRefs_ = true;
                continue;
            }
            bool nameOk = false;
            const std::string_view name =
                jbc::analysis::detail::memberName(cp, in->cp, nameOk);
            bool descOk = false;
            const std::string_view desc =
                jbc::analysis::detail::memberDesc(cp, in->cp, descOk);
            if (!nameOk || !descOk) {
                unresolvedFieldRefs_ = true;
                continue;
            }

            FieldAccess fa;
            fa.insn     = id;
            fa.owner    = pool_.str(pool_.intern(owner));
            fa.name     = pool_.str(pool_.intern(name));
            fa.desc     = pool_.str(pool_.intern(desc));
            fa.write    = write;
            fa.isStatic = isStatic;
            if (isStatic) {
                if (write) {
                    fa.valueDefs = stackDefsAt(f, 0);
                }
            } else if (write) {
                fa.valueDefs  = stackDefsAt(f, 0);
                fa.objectDefs = stackDefsAt(f, 1);
            } else {
                fa.objectDefs = stackDefsAt(f, 0);
            }
            fieldAccesses_.push_back(std::move(fa));
        }
    }

    auto defKey = [](DefId d) noexcept {
        return (static_cast<ca::u64>(d.kind) << 32) | d.id;
    };
    for (ca::u32 i = 0; i < edges_.size(); ++i) {
        edgesByDef_[defKey(edges_[i].def)].push_back(i);
        edgesByUse_[edges_[i].use.slot].push_back(i);
    }
}

std::vector<const DefUseEdge*> MethodDefUse::usesOf(DefId def) const
{
    std::vector<const DefUseEdge*> out;
    const auto it = edgesByDef_.find((static_cast<ca::u64>(def.kind) << 32) | def.id);
    if (it == edgesByDef_.end()) {
        return out;
    }
    out.reserve(it->second.size());
    for (ca::u32 i : it->second) {
        out.push_back(&edges_[i]);
    }
    return out;
}

std::vector<const DefUseEdge*> MethodDefUse::usesAt(InsnId use) const
{
    std::vector<const DefUseEdge*> out;
    const auto it = edgesByUse_.find(use.slot);
    if (it == edgesByUse_.end()) {
        return out;
    }
    out.reserve(it->second.size());
    for (ca::u32 i : it->second) {
        out.push_back(&edges_[i]);
    }
    return out;
}

std::vector<const LocalAccess*> MethodDefUse::readsOfLocal(ca::u32 local) const
{
    std::vector<const LocalAccess*> out;
    for (const LocalAccess& a : localAccesses_) {
        if (a.kind == LocalAccess::Kind::Read && a.local == local) {
            out.push_back(&a);
        }
    }
    return out;
}

const InsnId* MethodDefUse::instructionOf(DefId def) const
{
    if (def.kind != DefId::Kind::Instruction) {
        return nullptr;
    }
    const auto it = insnBySlot_.find(def.id);
    return it != insnBySlot_.end() ? &it->second : nullptr;
}

MethodDefUse analyzeDefUse(const ConstantPool& cp, const CodeUnit& u)
{
    return MethodDefUse(cp, u, nullptr);
}

MethodDefUse analyzeDefUse(const ConstantPool& cp, const CodeUnit& u, const MethodSig& sig)
{
    return MethodDefUse(cp, u, &sig);
}

}   // namespace mjbase::asm_analyzer
