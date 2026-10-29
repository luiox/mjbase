#include "mjbase/pass/uncontended_monitor_eliminator.hpp"

#include <algorithm>
#include <set>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/asm/insn/insn_op.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::DefUseSlot;
using mjbase::asm_analyzer::MethodDefUse;

/// 局部读写指令的槽号（紧凑形态经分析器同款展开）。
bool localIndexOf(const Insn& in, ca::u32& idx)
{
    const ca::u8 v      = static_cast<ca::u8>(in.op);
    const bool   compact = (v >= static_cast<ca::u8>(Op::ILOAD_0)
                         && v <= static_cast<ca::u8>(Op::ALOAD_3))
                        || (v >= static_cast<ca::u8>(Op::ISTORE_0)
                            && v <= static_cast<ca::u8>(Op::ASTORE_3));
    if (!compact && !mjbase::asm_insn::isLoad(in.op) && !mjbase::asm_insn::isStore(in.op)) {
        return false;
    }
    idx = compact ? jbc::analysis::detail::compactLocalIdx(in.op)
                  : static_cast<ca::u32>(in.a);
    return true;
}

/// 顺序位（找不到 = order.size()）。
ca::usize posIn(const std::vector<InsnId>& ids, InsnId id)
{
    for (ca::usize i = 0; i < ids.size(); ++i) {
        if (ids[i] == id) return i;
    }
    return ids.size();
}

}   // namespace

mjbase::spi::PreservedAnalyses UncontendedMonitorEliminater::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeDom&                  dom   = scope.code.dom;
    const std::vector<InsnId> order = dom.order();
    if (order.size() < 8) return mjbase::spi::PreservedAnalyses::all();

    const MethodDefUse du =
        mjbase::asm_analyzer::analyzeDefUse(scope.session.backing().cp, scope.code);
    if (!du.ok() || du.truncated()) return mjbase::spi::PreservedAnalyses::all();

    std::vector<std::vector<InsnId>> removals;
    std::set<ca::u32>                claimed;
    const auto                       claimedInsn = [&](InsnId id) {
        return claimed.count(id.slot) != 0;
    };

    for (ca::usize i = 0; i + 3 < order.size(); ++i) {
        const Insn* newInsn = dom.peek(order[i]);
        if (newInsn == nullptr || newInsn->op != Op::NEW) continue;
        const std::string_view newClass = scope.session.classOf(*newInsn);
        if (newClass.empty()) continue;
        const Insn* dup = dom.peek(order[i + 1]);
        if (dup == nullptr || dup->op != Op::DUP) continue;
        const Insn* init = dom.peek(order[i + 2]);
        if (init == nullptr || init->op != Op::INVOKESPECIAL) continue;
        ClassSession::MemberSem m;
        if (!scope.session.memberOf(*init, m) || m.owner != newClass || m.name != "<init>"
            || m.desc != "()V") {
            continue;
        }
        const Insn* st = dom.peek(order[i + 3]);
        if (st == nullptr) continue;
        ca::u32 slot = 0;
        if (!localIndexOf(*st, slot) || !mjbase::asm_insn::isStore(st->op)) continue;

        // 组内定义：本 ASTORE。接收者读点 = 该定义的 Local 使用边（同一
        // 槽位可被多组先后复用，按定义而非按槽位圈组）。
        const DefId storeDef = DefId::instruction(order[i + 3]);
        std::vector<InsnId> reads;
        for (const mjbase::asm_analyzer::DefUseEdge* e : du.usesOf(storeDef)) {
            if (e->slot == DefUseSlot::Local) reads.push_back(e->use);
        }
        std::sort(reads.begin(), reads.end(), [&](InsnId a, InsnId b) {
            return posIn(order, a) < posIn(order, b);
        });
        reads.erase(std::unique(reads.begin(), reads.end(),
                                [](InsnId a, InsnId b) { return a.slot == b.slot; }),
                    reads.end());
        // 恰两个接收者装载，先后对应 enter/exit。
        if (reads.size() != 2) continue;

        // 读点各带一个监控点：先读先 enter，后读后 exit。
        const ca::usize p1 = posIn(order, reads[0]);
        const ca::usize p2 = posIn(order, reads[1]);
        if (p1 >= order.size() || p2 >= order.size() || p1 <= i + 3 || p2 <= p1 + 1) {
            continue;
        }
        const Insn* e1 = dom.peek(order[p1 + 1]);
        const Insn* e2 = dom.peek(order[p2 + 1]);
        if (e1 == nullptr || e2 == nullptr || e1->op != Op::MONITORENTER
            || e2->op != Op::MONITOREXIT) {
            continue;
        }

        // 定义互证：两读的到达定义恰为唯一 ASTORE（流入读点的 Local 边
        // 全部来自本定义）；监控点的栈定义边全部汇于它。
        bool defsOk = true;
        for (const InsnId rIn : reads) {
            for (const mjbase::asm_analyzer::DefUseEdge* e : du.usesAt(rIn)) {
                if (e->slot != DefUseSlot::Local) continue;
                if (!(e->def == storeDef)) {
                    defsOk = false;
                    break;
                }
            }
            if (!defsOk) break;
        }
        if (!defsOk) continue;
        bool edgeOk = true;
        for (const InsnId mon : {order[p1 + 1], order[p2 + 1]}) {
            bool hasEdge = false;
            for (const mjbase::asm_analyzer::DefUseEdge* e : du.usesAt(mon)) {
                if (e->slot != DefUseSlot::Stack) continue;
                hasEdge = true;
                if (!(e->def == storeDef)) {
                    edgeOk = false;
                    break;
                }
            }
            if (!hasEdge) edgeOk = false;
            if (!edgeOk) break;
        }
        if (!edgeOk) continue;
        // usesOf 覆盖两类边：Local 读边（两 ALOAD）与转发后的监控点栈边。
        // 全集恰为这四点，多出任何其他消费边（额外栈使用等）→ 拒收。
        for (const mjbase::asm_analyzer::DefUseEdge* u : du.usesOf(storeDef)) {
            if (u->use.slot != reads[0].slot && u->use.slot != reads[1].slot
                && u->use.slot != order[p1 + 1].slot
                && u->use.slot != order[p2 + 1].slot) {
                edgeOk = false;
                break;
            }
        }
        if (!edgeOk) continue;

        // 成员集：头 4 + 两个接收装载 + 两个监控点。
        std::vector<InsnId> group{order[i],     order[i + 1],   order[i + 2],
                                  order[i + 3], reads[0],       order[p1 + 1],
                                  reads[1],     order[p2 + 1]};

        // 边界守卫：组成员或 enter..exit 区间内部锚定存活标签（分支/异常
        // 表）→ 整组拒收——跳入体内绕过 enter 的路径在原类触发 IMSE，摘除
        // 后不可观测语义变化，fail-closed。
        bool labelsClean = true;
        for (const InsnId id : group) {
            if (method_rewrite_util::labelsAnchorAt(dom, id)) {
                labelsClean = false;
                break;
            }
        }
        for (ca::usize p = p1 + 2; labelsClean && p <= p2; ++p) {
            if (method_rewrite_util::labelsAnchorAt(dom, order[p])) {
                labelsClean = false;
            }
        }
        if (!labelsClean) continue;

        bool overlap = false;
        for (const InsnId id : group) overlap = overlap || claimedInsn(id);
        if (overlap) continue;

        for (const InsnId id : group) claimed.insert(id.slot);
        removals.push_back(std::move(group));
    }

    if (removals.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 应用：逐组 eraseBatch（组级原子——异常表边界锚定任一成员由编辑器
    // 前置校验整组拒绝；标签门已过时重定向为空操作）。
    bool                modified = false;
    jbc::ir::CodeEditor editor(scope.code);
    for (const std::vector<InsnId>& group : removals) {
        if (editor
                .eraseBatch(group,
                            jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                .is_err()) {
            continue;
        }
        modified = true;
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
