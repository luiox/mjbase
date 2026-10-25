#include "mjbase/pass/dead_side_effect_free_call_remover.hpp"

#include <functional>
#include <map>
#include <set>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::DefUseEdge;
using mjbase::asm_analyzer::DefUseSlot;
using mjbase::asm_analyzer::MethodDefUse;

/// 白名单声明表（旧栈 issue #678 对齐）：零参、无可观测副作用、返回
/// category-2 的 JDK 静态调用；扩展只增条目，不改判定逻辑。
struct DiscardableCall
{
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
};

constexpr DiscardableCall kDiscardableCalls[] = {
    {"java/lang/System", "nanoTime", "()J"},
    {"java/lang/System", "currentTimeMillis", "()J"},
    {"java/lang/Math", "random", "()D"},
};

bool isDiscardableCall(const ClassSession& session, const Insn& in)
{
    if (in.op != Op::INVOKESTATIC) return false;
    ClassSession::MemberSem m;
    if (!session.memberOf(in, m)) return false;
    if (m.kind != ClassSession::MemberSem::Kind::Method
        && m.kind != ClassSession::MemberSem::Kind::InterfaceMethod) {
        return false;
    }
    for (const DiscardableCall& c : kDiscardableCalls) {
        if (m.owner == c.owner && m.name == c.name && m.desc == c.desc) return true;
    }
    return false;
}

/// 链内纯计算生产者的期望弹值个数（arity）；-1 = 不入链。
///
/// LDC 族常量类别需 CP 判定（class/MethodType/MethodHandle 不认领）。
/// arity 同时是缺失边守卫：输入使用边数少于期望即存在看不见的来源
/// （未写局部装载等），不可删。
ca::i32 chainOpArity(const ClassSession& session, const Insn& in)
{
    const auto inRange = [](Op v, Op first, Op last) { return v >= first && v <= last; };
    switch (in.op) {
    case Op::LDC: case Op::LDC_W: case Op::LDC2_W: {
        ca::i32         iv = 0;
        ca::i64         lv = 0;
        float           fv = 0.0F;
        double          dv = 0.0;
        std::string_view sv;
        return method_rewrite_util::intValueOf(session, in, iv)
                       || method_rewrite_util::longValueOf(session, in, lv)
                       || method_rewrite_util::floatValueOf(session, in, fv)
                       || method_rewrite_util::doubleValueOf(session, in, dv)
                       || method_rewrite_util::stringValueOf(session, in, sv)
                   ? 0
                   : -1;
    }
    default: break;
    }
    if (in.op == Op::ACONST_NULL || in.op == Op::BIPUSH || in.op == Op::SIPUSH
        || inRange(in.op, Op::ICONST_M1, Op::ICONST_5) || inRange(in.op, Op::LCONST_0, Op::LCONST_1)
        || inRange(in.op, Op::FCONST_0, Op::FCONST_2) || inRange(in.op, Op::DCONST_0, Op::DCONST_1)) {
        return 0;
    }
    if (inRange(in.op, Op::IADD, Op::DREM)) return 2;
    if (inRange(in.op, Op::INEG, Op::DNEG)) return 1;
    if (inRange(in.op, Op::ISHL, Op::LXOR)) return 2;
    if (inRange(in.op, Op::I2L, Op::I2S)) return 1;
    if (in.op == Op::ARRAYLENGTH || in.op == Op::INSTANCEOF) return 1;
    return -1;
}

/// 链闭包：从种子定义出发，白名单调用（叶子）与「纯计算且全部输入
/// 定义均入链」的生产者递归入链。纯栈上生产者不经局部回传，静态图
/// 无环；visiting 兜底环（防御）。返回定义 → 是否入链的备忘。
std::map<DefId, bool> closeChain(const ClassSession& session, const CodeDom& dom,
                                 const MethodDefUse& du, const std::vector<DefId>& seeds)
{
    std::map<DefId, bool> memo;
    std::set<DefId>       visiting;
    const std::function<bool(const DefId&)> visit = [&](const DefId& d) -> bool {
        const auto hit = memo.find(d);
        if (hit != memo.end()) return hit->second;
        if (!visiting.insert(d).second) return false;
        bool inChain = false;
        if (d.kind == DefId::Kind::Instruction) {
            const InsnId* iid = du.instructionOf(d);
            const Insn*   in  = iid != nullptr ? dom.peek(*iid) : nullptr;
            if (in != nullptr) {
                if (isDiscardableCall(session, *in)) {
                    inChain = true;   // 白名单叶子：零参无输入边
                } else {
                    const ca::i32 arity = chainOpArity(session, *in);
                    if (arity >= 0 && iid != nullptr) {
                        ca::usize edgeCount = 0;
                        inChain = true;
                        for (const DefUseEdge* e : du.usesAt(*iid)) {
                            if (e->slot != DefUseSlot::Stack) continue;
                            ++edgeCount;
                            if (!visit(e->def)) {
                                inChain = false;
                                break;
                            }
                        }
                        // 输入边少于期望 = 存在无定义来源（看不见即不可删）。
                        if (edgeCount != static_cast<ca::usize>(arity)) inChain = false;
                    }
                }
            }
        }
        visiting.erase(d);
        memo[d] = inChain;
        return inChain;
    };
    for (const DefId& d : seeds) visit(d);
    return memo;
}

}   // namespace

mjbase::spi::PreservedAnalyses DeadSideEffectFreeCallRemover::run(mjbase::spi::MethodScope& scope,
                                                                  mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeDom& dom = scope.code.dom;
    if (dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();
    // handler 入口值无定义边，含异常表的方法负向结论不可靠，整件跳过
    // （精细化挂账）。
    if (!scope.code.exc.empty()) return mjbase::spi::PreservedAnalyses::all();

    const jbc::analysis::MethodSig sig{
        (scope.access & mjbase::hierarchy::kAccStatic) != 0, scope.desc};
    const MethodDefUse du =
        mjbase::asm_analyzer::analyzeDefUse(scope.session.backing().cp, scope.code, sig);
    if (!du.ok() || du.truncated()) return mjbase::spi::PreservedAnalyses::all();

    const std::vector<InsnId> order = dom.order();
    std::map<ca::u32, ca::usize> idxOf;
    for (ca::usize i = 0; i < order.size(); ++i) idxOf[order[i].slot] = i;

    std::set<ca::u32>                 claimed;
    std::vector<std::vector<InsnId>>  removals;   // 每窗口一个删除组

    for (ca::usize i = 0; i < order.size(); ++i) {
        const InsnId popId = order[i];
        const Insn*  pop   = dom.peek(popId);
        if (pop == nullptr || (pop->op != Op::POP && pop->op != Op::POP2)) continue;
        if (claimed.count(popId.slot) != 0) continue;

        // 弹值定义种子：POP 取 index0；POP2 取 index0/1（cat2 顶值天然
        // 只有 index0 边）。无定义边（看不见来源）直接跳过。
        std::vector<DefId> seeds;
        bool               shapeOk = true;
        for (const DefUseEdge* e : du.usesAt(popId)) {
            if (e->slot != DefUseSlot::Stack) continue;
            if (e->index > 1) {
                shapeOk = false;
                break;
            }
            if (pop->op == Op::POP && e->index != 0) {
                shapeOk = false;
                break;
            }
            seeds.push_back(e->def);
        }
        if (!shapeOk || seeds.empty()) continue;

        std::map<DefId, bool> chain = closeChain(scope.session, dom, du, seeds);
        const auto allSeedsIn = [&]() {
            for (const DefId& d : seeds) {
                const auto hit = chain.find(d);
                if (hit == chain.end() || !hit->second) return false;
            }
            return true;
        };
        if (!allSeedsIn()) continue;

        // 逐出不动点：链成员的全部使用边须指向链内或本 pop；共享消费者
        // 级联逐出（成员被逐后，其输入生产者随之出现集外消费者）。
        std::set<ca::u32> inSlots;
        for (const auto& kv : chain) {
            if (!kv.first.valid()) continue;
            if (!kv.second) continue;
            const InsnId* iid = du.instructionOf(kv.first);
            if (iid != nullptr) inSlots.insert(iid->slot);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& kv : chain) {
                if (!kv.second) continue;
                const InsnId* iid = du.instructionOf(kv.first);
                if (iid == nullptr) continue;
                bool outside = false;
                for (const DefUseEdge* u : du.usesOf(kv.first)) {
                    if (u->use == popId) continue;
                    if (inSlots.count(u->use.slot) == 0) {
                        outside = true;
                        break;
                    }
                }
                if (outside) {
                    chain[kv.first] = false;
                    inSlots.erase(iid->slot);
                    changed = true;
                }
            }
        }
        if (!allSeedsIn()) continue;
        bool hasCall = false;
        for (const auto& kv : chain) {
            if (!kv.second) continue;
            const InsnId* iid = du.instructionOf(kv.first);
            const Insn*   in  = iid != nullptr ? dom.peek(*iid) : nullptr;
            if (in != nullptr && isDiscardableCall(scope.session, *in)) {
                hasCall = true;
                break;
            }
        }
        if (!hasCall) continue;

        // 区间门：[链首, pop] 顺序区间内只允许链成员与 pop，且区间内部
        // （含 pop 前边界）无存活标签——区间外分支不得中途进入链。链
        // 成员必须全部先于 pop（布局 wrap 形态保守拒收）。
        ca::usize headIdx = i;
        ca::usize tailIdx = i;
        bool       wrap    = false;
        for (const InsnId id : order) {
            if (inSlots.count(id.slot) == 0) continue;
            const auto hit = idxOf.find(id.slot);
            if (hit == idxOf.end()) continue;
            if (hit->second < headIdx) headIdx = hit->second;
            if (hit->second > tailIdx) tailIdx = hit->second;
        }
        if (tailIdx > i) wrap = true;
        bool rangeOk = !wrap;
        for (ca::usize j = headIdx; rangeOk && j <= i; ++j) {
            const bool member = j == i || inSlots.count(order[j].slot) != 0;
            if (!member) {
                rangeOk = false;
                break;
            }
            if (j > headIdx && method_rewrite_util::labelsAnchorAt(dom, order[j])) {
                rangeOk = false;
                break;
            }
        }
        if (!rangeOk) continue;

        std::vector<InsnId> group;
        for (ca::usize j = headIdx; j <= i; ++j) group.push_back(order[j]);
        removals.push_back(std::move(group));
        for (const InsnId id : removals.back()) claimed.insert(id.slot);
    }

    if (removals.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 应用：组内按顺序删除（fallthrough 重定向链式后移）；任一步失败
    // 停止后续（防御路径——链成员可达且有消费后继）。
    bool      modified = false;
    jbc::ir::CodeEditor editor(scope.code);
    for (const std::vector<InsnId>& group : removals) {
        for (const InsnId id : group) {
            if (editor.erase(id, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                    .is_err()) {
                return modified ? mjbase::spi::PreservedAnalyses::none()
                                : mjbase::spi::PreservedAnalyses::all();
            }
            modified = true;
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
