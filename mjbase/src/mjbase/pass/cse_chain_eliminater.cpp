#include "mjbase/pass/cse_chain_eliminater.hpp"

#include <algorithm>
#include <set>
#include <vector>

#include "jbc/analysis/domain.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/insn/insn_op.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;

/// 链内纯推值指令：局部装载（含紧凑形）、常量推入。condy 不认领
/// （bootstrap 可抛）。
bool chainPushOp(const Insn& in, ClassSession& session)
{
    switch (in.op) {
    case Op::ILOAD: case Op::LLOAD: case Op::FLOAD: case Op::DLOAD:
    case Op::ALOAD:
    case Op::ILOAD_0: case Op::ILOAD_1: case Op::ILOAD_2: case Op::ILOAD_3:
    case Op::LLOAD_0: case Op::LLOAD_1: case Op::LLOAD_2: case Op::LLOAD_3:
    case Op::FLOAD_0: case Op::FLOAD_1: case Op::FLOAD_2: case Op::FLOAD_3:
    case Op::DLOAD_0: case Op::DLOAD_1: case Op::DLOAD_2: case Op::DLOAD_3:
    case Op::ALOAD_0: case Op::ALOAD_1: case Op::ALOAD_2: case Op::ALOAD_3:
    case Op::ICONST_M1: case Op::ICONST_0: case Op::ICONST_1:
    case Op::ICONST_2: case Op::ICONST_3: case Op::ICONST_4:
    case Op::ICONST_5:
    case Op::LCONST_0: case Op::LCONST_1:
    case Op::FCONST_0: case Op::FCONST_1: case Op::FCONST_2:
    case Op::DCONST_0: case Op::DCONST_1:
    case Op::BIPUSH: case Op::SIPUSH:
        return true;
    case Op::LDC:
    case Op::LDC2_W: {
        ClassSession::ConstSem cs;
        return session.constOf(in, cs)
            && cs.kind != ClassSession::ConstSem::Kind::Dynamic;
    }
    default: return false;
    }
}

/// 二元纯运算（算术/移位/位/比较）：两值进一出。
bool chainBinaryOp(Op op)
{
    switch (op) {
    case Op::IADD: case Op::ISUB: case Op::IMUL: case Op::IDIV:
    case Op::IREM: case Op::IAND: case Op::IOR: case Op::IXOR:
    case Op::ISHL: case Op::ISHR: case Op::IUSHR:
    case Op::LADD: case Op::LSUB: case Op::LMUL: case Op::LDIV:
    case Op::LREM: case Op::LAND: case Op::LOR: case Op::LXOR:
    case Op::LSHL: case Op::LSHR: case Op::LUSHR:
    case Op::FADD: case Op::FSUB: case Op::FMUL: case Op::FDIV:
    case Op::FREM:
    case Op::DADD: case Op::DSUB: case Op::DMUL: case Op::DDIV:
    case Op::DREM:
    case Op::LCMP: case Op::FCMPL: case Op::FCMPG: case Op::DCMPL:
    case Op::DCMPG:
        return true;
    default: return false;
    }
}

/// 一元纯运算（取负/数值转换）：一值进出。
bool chainUnaryOp(Op op)
{
    switch (op) {
    case Op::INEG: case Op::LNEG: case Op::FNEG: case Op::DNEG:
    case Op::I2L: case Op::I2F: case Op::I2D: case Op::I2B: case Op::I2C:
    case Op::I2S:
    case Op::L2I: case Op::L2F: case Op::L2D:
    case Op::F2I: case Op::F2L: case Op::F2D:
    case Op::D2I: case Op::D2L: case Op::D2F:
        return true;
    default: return false;
    }
}

/// 链内一步的值收支（cat2 记一个值）；非链指令 false。
bool chainStep(const Insn& in, ClassSession& session, int& consume, int& produce)
{
    if (chainBinaryOp(in.op)) {
        consume = 2;
        produce = 1;
        return true;
    }
    if (chainUnaryOp(in.op)) {
        consume = 1;
        produce = 1;
        return true;
    }
    if (chainPushOp(in, session)) {
        consume = 0;
        produce = 1;
        return true;
    }
    return false;
}

/// 区间控制流面：分支/开关/返回/抛出/ret 任一出现即非同一直线段。
bool controlFlowOp(Op op)
{
    return mjbase::asm_insn::isBranch(op) || mjbase::asm_insn::isSwitch(op)
        || mjbase::asm_insn::isReturn(op) || mjbase::asm_insn::isThrow(op)
        || op == Op::RET;
}

/// 存储目标槽号（含紧凑形；wide 同操作码取 a）。
bool storeSlot(const Insn& in, ca::u32& slot)
{
    if (!mjbase::asm_insn::isStore(in.op)) return false;
    const ca::u8 v      = static_cast<ca::u8>(in.op);
    const bool   compact = v >= static_cast<ca::u8>(Op::ISTORE_0)
                        && v <= static_cast<ca::u8>(Op::ASTORE_3);
    slot = compact ? jbc::analysis::detail::compactLocalIdx(in.op)
                   : static_cast<ca::u32>(in.a);
    return true;
}

/// 一条纯链：指令区间 [start .. storePos)，insns 不含收尾存储。
struct Chain
{
    ca::usize            start    = 0;
    ca::usize            storePos = 0;
    std::vector<InsnId>  insns;
    std::vector<ca::u32> operandSlots;
};

/// 自存储点反向需值抽取自包含纯链；遇链外指令（含任何存储）/需值越
/// 过方法头/超供返回 false。
bool extractChain(const CodeDom& dom, const std::vector<InsnId>& order,
                  ClassSession& session, ca::usize storePos, Chain& out)
{
    const Insn* st = dom.peek(order[storePos]);
    if (st == nullptr || !mjbase::asm_insn::isStore(st->op)) return false;
    out.start    = storePos;
    out.storePos = storePos;
    out.insns.clear();
    out.operandSlots.clear();
    int       needed = 1;
    ca::usize cur    = storePos;
    while (needed > 0) {
        if (cur == 0) return false;
        --cur;
        const Insn* in = dom.peek(order[cur]);
        if (in == nullptr) return false;
        int consume = 0;
        int produce = 0;
        if (!chainStep(*in, session, consume, produce)) return false;
        needed += consume - produce;
        if (needed < 0) return false;
        if (mjbase::asm_insn::isLoad(in->op)) {
            const ca::u8 v      = static_cast<ca::u8>(in->op);
            const bool   compact = v >= static_cast<ca::u8>(Op::ILOAD_0)
                                && v <= static_cast<ca::u8>(Op::ALOAD_3);
            out.operandSlots.push_back(
                compact ? jbc::analysis::detail::compactLocalIdx(in->op)
                        : static_cast<ca::u32>(in->a));
        }
        out.insns.push_back(order[cur]);
    }
    out.start = cur;
    std::reverse(out.insns.begin(), out.insns.end());
    return true;
}

/// 链结构全同：逐条同操作码、装载同槽号、LDC 同 CP 条目。
bool sameChain(const CodeDom& dom, const Chain& a, const Chain& b)
{
    if (a.insns.size() != b.insns.size()) return false;
    for (ca::usize i = 0; i < a.insns.size(); ++i) {
        const Insn* ia = dom.peek(a.insns[i]);
        const Insn* ib = dom.peek(b.insns[i]);
        if (ia == nullptr || ib == nullptr || ia->op != ib->op) return false;
        if (mjbase::asm_insn::isLoad(ia->op) && ia->a != ib->a) return false;
        if ((ia->op == Op::LDC || ia->op == Op::LDC2_W) && ia->cp != ib->cp) {
            return false;
        }
    }
    return true;
}

}   // namespace

mjbase::spi::PreservedAnalyses CseChainEliminater::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeDom&                  dom   = scope.code.dom;
    const std::vector<InsnId> order = dom.order();
    if (order.size() < 3) return mjbase::spi::PreservedAnalyses::all();

    std::vector<ca::usize> stores;
    for (ca::usize p = 0; p < order.size(); ++p) {
        const Insn* in = dom.peek(order[p]);
        if (in != nullptr && mjbase::asm_insn::isStore(in->op)) stores.push_back(p);
    }

    std::set<ca::u32> claimed;
    const auto        claimedPos = [&](ca::usize p) {
        return claimed.count(static_cast<ca::u32>(p)) != 0;
    };

    std::vector<std::vector<InsnId>> removals;
    for (const ca::usize q2 : stores) {
        if (claimedPos(q2)) continue;
        Chain c2;
        if (!extractChain(dom, order, scope.session, q2, c2)) continue;
        if (c2.insns.empty()) continue;
        // 就近取结构全同的链 1；区间门失败即放弃该链 2——更早的同值链
        // 区间更宽，只会撞上同一批破坏点。
        for (const ca::usize q1 : stores) {
            if (q1 >= c2.start) break;
            if (claimedPos(q1)) continue;
            ca::u32 t1 = 0;
            ca::u32 t2 = 0;
            if (!storeSlot(*dom.peek(order[q1]), t1)
                || !storeSlot(*dom.peek(order[q2]), t2) || t1 != t2) {
                continue;
            }
            Chain c1;
            if (!extractChain(dom, order, scope.session, q1, c1)) continue;
            if (!sameChain(dom, c1, c2)) continue;

            // 区间守卫：[链1 头 .. 链2 存储) 无标签锚定、无控制流；写入
            // 面仅允许链1 存储点触 t，任何触操作数槽即拒收（iinc 同存储
            // 计）；链2 存储位单独查标签（跳到存储点本身）。
            bool clean = true;
            for (ca::usize p = c1.start; clean && p < q2; ++p) {
                if (method_rewrite_util::labelsAnchorAt(dom, order[p])) {
                    clean = false;
                    break;
                }
                const Insn* in = dom.peek(order[p]);
                if (in == nullptr) {
                    clean = false;
                    break;
                }
                if (controlFlowOp(in->op)) {
                    clean = false;
                    break;
                }
                if (in->op == Op::IINC) {
                    if (static_cast<ca::u32>(in->a) == t1 && p != c1.storePos) {
                        clean = false;
                    }
                    for (const ca::u32 s : c2.operandSlots) {
                        if (static_cast<ca::u32>(in->a) == s) clean = false;
                    }
                    continue;
                }
                if (mjbase::asm_insn::isStore(in->op)) {
                    ca::u32 s = 0;
                    storeSlot(*in, s);
                    if (s == t1 && p != c1.storePos) clean = false;
                    for (const ca::u32 os : c2.operandSlots) {
                        if (s == os) clean = false;
                    }
                }
            }
            if (!clean) break;
            if (method_rewrite_util::labelsAnchorAt(dom, order[q2])) break;

            bool overlap = false;
            for (const InsnId id : c2.insns) {
                overlap = overlap || claimedPos(id.slot);
            }
            overlap = overlap || claimedPos(q2);
            for (const InsnId id : c1.insns) {
                overlap = overlap || claimedPos(id.slot);
            }
            overlap = overlap || claimedPos(q1);
            if (overlap) break;

            for (const InsnId id : c2.insns) claimed.insert(id.slot);
            claimed.insert(static_cast<ca::u32>(q2));
            std::vector<InsnId> group(c2.insns);
            group.push_back(order[q2]);
            removals.push_back(std::move(group));
            break;
        }
    }

    if (removals.empty()) return mjbase::spi::PreservedAnalyses::all();

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
