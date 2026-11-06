/// @file        region_select.cpp
/// @brief       RegionSelect 实现：v1 默认策略（预检/排序/冲突/探针）。
#include "mjbase/asm/sem/region_select.hpp"

#include <algorithm>
#include <set>

#include "jbc/core/opcode.hpp"

#include "mjbase/asm/insn/insn_op.hpp"
#include "mjbase/asm/sem/insert_prim.hpp"

namespace mjbase::asm_sem {

const char* to_string(RegionSelectNote note) noexcept
{
    switch (note) {
    case RegionSelectNote::Ok:                return "ok";
    case RegionSelectNote::NoCandidates:      return "no-candidates";
    case RegionSelectNote::MethodSubroutine:  return "method-subroutine";
    case RegionSelectNote::AllFiltered:       return "all-filtered";
    case RegionSelectNote::ConflictAmbiguous: return "conflict-ambiguous";
    case RegionSelectNote::NoneAdmitted:      return "none-admitted";
    case RegionSelectNote::AllExcluded:       return "all-excluded";
    }
    return "unknown";
}

std::vector<ca::usize>
rankRegions(const RegionSelectPolicy& policy, const std::vector<RegionCandidate>& candidates,
            const std::vector<ca::usize>& passing)
{
    std::vector<ca::usize> order = passing;
    if (!policy.preferSmallest) {
        return order;   /// 枚举序（调用方传入序）。
    }
    /// 规模升序（最小切口保守）；平局以首成员块号（程序序）破平——候选
    /// members 升序交付，front() 即最小块。稳定排序保同键枚举序。
    std::stable_sort(order.begin(), order.end(),
                     [&candidates](ca::usize a, ca::usize b) {
                         const RegionCandidate& ra = candidates[a];
                         const RegionCandidate& rb = candidates[b];
                         if (ra.members.size() != rb.members.size()) {
                             return ra.members.size() < rb.members.size();
                         }
                         const ca::u32 headA = ra.members.empty() ? 0u : ra.members.front().index;
                         const ca::u32 headB = rb.members.empty() ? 0u : rb.members.front().index;
                         return headA < headB;
                     });
    return order;
}

namespace {

/// 成员集是否相交（候选 members 升序；双指针单遍）。
bool membersIntersect(const RegionCandidate& a, const RegionCandidate& b) noexcept
{
    ca::usize i = 0;
    ca::usize j = 0;
    while (i < a.members.size() && j < b.members.size()) {
        if (a.members[i].index == b.members[j].index) {
            return true;
        }
        if (a.members[i].index < b.members[j].index) {
            ++i;
        } else {
            ++j;
        }
    }
    return false;
}

/// 成员集恒等（等尺寸前置下逐块比较；升序交付保证按位对应）。
bool membersEqual(const RegionCandidate& a, const RegionCandidate& b) noexcept
{
    if (a.members.size() != b.members.size()) {
        return false;
    }
    for (ca::usize i = 0; i < a.members.size(); ++i) {
        if (a.members[i].index != b.members[i].index) {
            return false;
        }
    }
    return true;
}

/// 冲突保守拒绝的形状判定：存在等尺寸部分重叠对（相交且互异——既非嵌套
/// 亦非重复）。嵌套（⊆）与重复（=）不构成冲突（见 region_select.hpp 注）。
bool hasOverlapTie(const std::vector<RegionCandidate>& candidates,
                   const std::vector<ca::usize>& passing) noexcept
{
    for (ca::usize i = 0; i < passing.size(); ++i) {
        for (ca::usize j = i + 1; j < passing.size(); ++j) {
            const RegionCandidate& a = candidates[passing[i]];
            const RegionCandidate& b = candidates[passing[j]];
            if (a.members.size() != b.members.size()) {
                continue;   /// 异尺寸：交叠也只是嵌套（⊆），取小者即保守解。
            }
            if (membersEqual(a, b)) {
                continue;   /// 重复候选（并行入口边同形）：程序序破平即可。
            }
            if (membersIntersect(a, b)) {
                return true;   /// 等尺寸相交且互异：部分重叠，挑选无结构依据。
            }
        }
    }
    return false;
}

/// v2 排除档：候选是否为「合成接力块」——apply 调用点块形态（接力候选：
/// apply 后调用点块自身满足全部预检与出口形，排
/// 除档开启时驱动器跳过它，候选耗尽正常收敛）。判定规则见 region_select.hpp
/// selectRegion 注：单块 + 转发 goto + lone invokestatic（身份事实与
/// planInline 的 NotOutlineSynthetic 同源）+ 其余指令限装载/复位族。
/// 事实面不足或任一事实不符 → false（不排除；误排除方向安全但宁漏勿误）。
bool isOutlineRelayBlock(const RegionSelectRequest& req, const RegionCandidate& c)
{
    if (req.session == nullptr || req.view == nullptr || req.unit == nullptr) {
        return false;   /// 排除档事实面不足：无从判定身份，保守不排除。
    }
    if (c.members.size() != 1) {
        return false;   /// 接力块恒单成员（apply 调用点块）。
    }
    const mjbase::asm_cfg::BasicBlock* b = req.view->cfg().block(c.members.front());
    if (b == nullptr || b->insns.size() < 2) {
        return false;   /// 至少 [invokestatic, 转发 goto] 两条。
    }
    const jbc::ir::ClassUnit& cu       = req.session->unit();
    const std::string_view thisClass = cu.symbols().str(cu.thisClass);
    const ca::usize n                = b->insns.size();
    const jbc::ir::Insn* term        = req.unit->dom.peek(b->insns[n - 1]);
    if (term == nullptr || !mjbase::asm_insn::isGoto(term->op)) {
        return false;   /// 单转发形态：末条 = goto。直落出口不在本档判定
                        /// 范围（requireExitGoto 档下接力块恒带转发 goto）。
    }
    ca::usize callCount = 0;
    for (ca::usize i = 0; i + 1 < n; ++i) {
        const jbc::ir::Insn* in = req.unit->dom.peek(b->insns[i]);
        if (in == nullptr) {
            return false;
        }
        if (in->op == jbc::core::Op::INVOKESTATIC) {
            if (++callCount > 1) {
                return false;   /// lone：恰一个 invokestatic。
            }
            /// 身份事实（与 planInline NotOutlineSynthetic 同源三元）：同类 +
            /// outline$ 名前缀 + access 恒等 private|static|synthetic。
            jbc::ir::ClassSession::MemberSem sem;
            if (!req.session->memberOf(*in, sem)
                || sem.kind != jbc::ir::ClassSession::MemberSem::Kind::Method
                || sem.owner != thisClass) {
                return false;
            }
            const std::string_view prefix("outline$");
            if (sem.name.size() <= prefix.size()
                || sem.name.substr(0, prefix.size()) != prefix) {
                return false;
            }
            const jbc::ir::MethodUnit* cm = cu.method(cu.findMethodId(sem.name, sem.desc));
            if (cm == nullptr || cm->access != (0x0002u | 0x0008u | 0x1000u)) {
                return false;   /// 悬空/身份不符 = 非本设施产物，不排除。
            }
            continue;
        }
        /// 其余指令限 apply preamble 发射族：局部读（含紧凑形态）+ DUP/DUP2
        /// 复位（entryDupResetInsn）。常量/算术/字段/其他调用一概不匹配。
        if (in->op == jbc::core::Op::DUP || in->op == jbc::core::Op::DUP2
            || mjbase::asm_insn::isLoad(in->op)) {
            continue;
        }
        return false;
    }
    return callCount == 1;
}

/// 候选级事实面预检（v1 规则 F2–F6；返回 false = 该候选被过滤）。
bool passesFactPreflight(const RegionSelectPolicy& policy, const RegionSelectRequest& req,
                         const RegionCandidate& c)
{
    if (c.members.empty()) {
        return false;   /// 防御：空成员不交付（RegionView 口径，手构面兜底）。
    }
    if (policy.requireStraightLine && !c.straightLine) {
        return false;   /// F2 判据 1：v1 只开直线档。
    }
    if (policy.requireNoHandlers && (!c.exceptionHomogeneous || !c.handlers.empty())) {
        return false;   /// F3 判据 4：同质且 handler 集为空（非同质
                        ///  候选 handlers 恒空，以旗标同拒）。
    }
    const bool haveView = req.view != nullptr && req.unit != nullptr;
    if (haveView) {
        const mjbase::asm_cfg::BasicBlock* tail =
            req.view->cfg().block(c.members.back());
        const jbc::ir::Insn* last =
            tail == nullptr || tail->insns.empty()
                ? nullptr
                : req.unit->dom.peek(tail->insns.back());
        if (policy.requireExitGoto
            && (last == nullptr || last->op != jbc::core::Op::GOTO)) {
            return false;   /// F4 v1 出口形态：末条指令 goto（goto_w 同拒，
                            ///  与 planOutline LastInsnNotGoto 同口径）。
        }
        if (req.frame != nullptr) {
            for (const mjbase::asm_cfg::BlockId m : c.members) {
                const mjbase::asm_cfg::BasicBlock* b = req.view->cfg().block(m);
                if (b == nullptr) {
                    return false;
                }
                for (const jbc::ir::InsnId id : b->insns) {
                    if (req.frame->hasUninitThis(id)) {
                        return false;   /// F5 判据 6：UninitThis 区域内存活。
                    }
                }
            }
        }
        if (req.liveness != nullptr) {
            /// F6 判据 3（与 planOutline 写回集同向）：区域内 store/iinc
            ///  写槽 ∩ 出口锚（末成员末条 = exit goto）后活跃 = 空。
            std::set<ca::u32> written;
            for (const mjbase::asm_cfg::BlockId m : c.members) {
                const mjbase::asm_cfg::BasicBlock* b = req.view->cfg().block(m);
                if (b == nullptr) {
                    return false;
                }
                for (const jbc::ir::InsnId id : b->insns) {
                    const jbc::ir::Insn* in = req.unit->dom.peek(id);
                    if (in == nullptr) {
                        return false;
                    }
                    if (mjbase::asm_insn::isStore(in->op) || in->op == jbc::core::Op::IINC) {
                        written.insert(mjbase::asm_sem::localOperandSlot(*in));
                    }
                }
            }
            const mjbase::asm_cfg::BasicBlock* tail = req.view->cfg().block(c.members.back());
            const jbc::ir::InsnId exitAnchor = tail->insns.back();
            for (const ca::u32 slot : written) {
                if (req.liveness->localLiveAfter(exitAnchor, slot)) {
                    return false;   /// 写回集非空：需多值返回通道，v1 拒。
                }
            }
        }
    }
    return true;
}

}   // namespace

RegionDecision selectRegion(const RegionSelectPolicy& policy,
                            const RegionSelectRequest& req)
{
    RegionDecision d;
    /// 方法级预检（判据 5）先于候选集检查：jsr/ret 是方法体语形事实，
    /// 逐候选过滤无意义——方法不可开档与候选集为空是两回事。
    if ((req.view != nullptr && req.view->hasSubroutine())
        || (req.frame != nullptr && req.frame->hasSubroutine())) {
        d.note = RegionSelectNote::MethodSubroutine;
        return d;
    }
    if (req.candidates.empty()) {
        d.note = RegionSelectNote::NoCandidates;
        return d;
    }

    std::vector<ca::usize> passing;
    ca::usize excludedRelay = 0;
    for (ca::usize i = 0; i < req.candidates.size(); ++i) {
        /// v2 排除档先于事实面预检：接力块跳过（不进优先序、不进探针）；
        /// 档位关闭时零开销零行为差（v1 口径）。
        if (policy.excludeOutlineRelay && isOutlineRelayBlock(req, req.candidates[i])) {
            ++excludedRelay;
            continue;
        }
        if (passesFactPreflight(policy, req, req.candidates[i])) {
            passing.push_back(i);
        }
    }
    d.order = rankRegions(policy, req.candidates, passing);
    if (d.order.empty()) {
        /// 排除档参与了全拒 → AllExcluded（批量驱动据此判候选耗尽正常收敛）；
        /// 纯 v1 预检全滤仍记 AllFiltered（口径不变）。
        d.note = excludedRelay > 0 ? RegionSelectNote::AllExcluded
                                   : RegionSelectNote::AllFiltered;
        return d;
    }
    if (policy.rejectOverlapTie && hasOverlapTie(req.candidates, d.order)) {
        d.note = RegionSelectNote::ConflictAmbiguous;
        return d;
    }

    if (req.admit == nullptr) {
        d.accepted = true;
        d.chosen  = d.order.front();
        d.note    = RegionSelectNote::Ok;
        return d;
    }
    for (const ca::usize idx : d.order) {
        if (req.admit(req.candidates[idx])) {
            d.accepted = true;
            d.chosen   = idx;
            d.note     = RegionSelectNote::Ok;
            return d;
        }
    }
    d.note = RegionSelectNote::NoneAdmitted;
    return d;
}

}   // namespace mjbase::asm_sem
