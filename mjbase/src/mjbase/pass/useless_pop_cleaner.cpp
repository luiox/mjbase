#include "mjbase/pass/useless_pop_cleaner.hpp"

#include <optional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::ir::CodeDom;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::DefUseEdge;
using mjbase::asm_analyzer::DefUseSlot;
using mjbase::asm_analyzer::MethodDefUse;

/// 一对（局部装载, POP/POP2）能否安全消除。
bool matchPair(CodeDom& dom, const InsnId loadId, const InsnId popId, bool pop2)
{
    const jbc::ir::Insn* load = dom.peek(loadId);
    const jbc::ir::Insn* pop  = dom.peek(popId);
    if (load == nullptr || pop == nullptr) return false;
    if (pop->op != (pop2 ? jbc::core::Op::POP2 : jbc::core::Op::POP)) return false;
    const bool foldable = pop2 ? method_rewrite_util::isPop2FoldableLocalLoad(*load)
                               : method_rewrite_util::isPopFoldableLocalLoad(*load);
    if (!foldable) return false;
    // 边界守卫：POP 前置边界有标签锚定（分支目标/异常表边界）→ 不消除。
    if (method_rewrite_util::labelsAnchorAt(dom, popId)) return false;
    // POP 必须有顺序后继：两次 fallthrough 删除都需要后继承接引用。
    const std::vector<InsnId>& order = dom.order();
    return !order.empty() && order.back() != popId;
}

/// 栈中继操作码（DUP/DUP2 族与 SWAP）。def-use 载体对复制类是透明的：
/// 帧引擎的 dup 槽复制沿用了原定义行（拷贝不带中继自身的定义），中继的
/// 输入消费按净效应计不产边——删除中继下的生产者会留下栈元数破坏的悬空
/// 中继（如 [ICONST_1; DUP; POP2]）。逐中继细化见 traceRelayChain /
/// certifyRelayPlans。
bool isStackRelayOp(jbc::core::Op op)
{
    switch (op) {
    case jbc::core::Op::DUP:
    case jbc::core::Op::DUP_X1:
    case jbc::core::Op::DUP_X2:
    case jbc::core::Op::DUP2:
    case jbc::core::Op::DUP2_X1:
    case jbc::core::Op::DUP2_X2:
    case jbc::core::Op::SWAP: return true;
    default: return false;
    }
}

/// 帧面可删生产者：纯常量装载——int/long/float/double/string 常量、
/// ACONST_NULL 与 ldc Class/MethodType/MethodHandle（元数据常量无求值
/// 副作用；Dynamic/condy 不认领——bootstrap 链有求值副作用）。
/// 覆盖面按旧栈帧载体（InsnUtil::isConstant 的纯装载子集）收窄：局部
/// 装载不作帧面生产者（def-use 里装载是恒等搬运、定义归 store，归
/// DeadLocalStoreRemover 协同，见头文件挂账）。
bool isFrameRemovableConstProducer(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in)
{
    ca::i32         iv = 0;
    ca::i64         lv = 0;
    float           fv = 0.0F;
    double          dv = 0.0;
    std::string_view sv;
    if (in.op == jbc::core::Op::ACONST_NULL
        || method_rewrite_util::intValueOf(session, in, iv)
        || method_rewrite_util::longValueOf(session, in, lv)
        || method_rewrite_util::floatValueOf(session, in, fv)
        || method_rewrite_util::doubleValueOf(session, in, dv)
        || method_rewrite_util::stringValueOf(session, in, sv)) {
        return true;
    }
    jbc::ir::ClassSession::ConstSem cs;
    if (session.constOf(in, cs)) {
        using K = jbc::ir::ClassSession::ConstSem::Kind;
        return cs.kind == K::Class || cs.kind == K::MethodType || cs.kind == K::MethodHandle;
    }
    return false;
}

/// 生产者是否 category-2（long/double 常量）。同一栈值槽在合法输入下
/// 各路径宽度一致（Verifier 汇合形状约束），取首个到达定义判定即可。
bool isCat2ConstProducer(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in)
{
    ca::i64 lv = 0;
    double dv = 0.0;
    return method_rewrite_util::longValueOf(session, in, lv)
           || method_rewrite_util::doubleValueOf(session, in, dv);
}

/// 指令在快照序列中的位置（找不到返回 ids.size()）。
ca::usize posIn(const std::vector<InsnId>& ids, InsnId id)
{
    for (ca::usize i = 0; i < ids.size(); ++i) {
        if (ids[i] == id) return i;
    }
    return ids.size();
}

/// 帧面单条 pop 的删除计划。downgrade=true 时 POP2 降级 POP（半删），
/// 只删 defs 里的生产者；否则生产者与 pop 一并删除。relays 是直线溯源
/// 收集的可吸收中继（仅中继在场路；认证通过的整删计划专属）。
struct PopPlan
{
    InsnId               pop;
    bool                 downgrade = false;
    std::vector<DefId>   defs;
    std::vector<InsnId>  relays;
};

/// 中继在场路的直线溯源：从 pop 位置向前回溯，输入槽需求模拟——NOP
/// 跳过、纯常量生产者扣减（cat2 双槽；先认领上方存量）、已计划 pop 穿透
/// （其消费槽在需求区上方，计入 above）、DUP/DUP2 中继把拷贝别名折叠为
/// 一份原件（输入需求 = 原件槽宽，DUP 1 槽 / DUP2 2 槽）。收集链上常量
/// 与可吸收中继。以下形态 fail-closed 返回 false：
///   - X 族（DUP_X1/DUP_X2/DUP2_X1/DUP2_X2）与 SWAP 的重排/插底形态
///     （拷贝与在场值交错，删除面不闭合）——挂账；
///   - DUP2 的 long/double 双槽直接输入（第二份拷贝天然离场）——挂账；
///   - 链上任何标签锚定（分支汇合：直线供源不成立）；
///   - 未计划消费穿插（弹值逃逸出认证面）、复合生产者（运算/调用/字段）、
///     cat2 常量部分消费（半吃双槽）。
bool traceRelayChain(const CodeDom& dom, const jbc::ir::ClassSession& session,
                     const std::vector<InsnId>& ids,
                     const std::unordered_set<ca::u32>& planPops, ca::usize popPos,
                     bool pop2, std::vector<InsnId>& consts, std::vector<InsnId>& relays)
{
    ca::u32 above = 0;   // 需求区上方、归上方消费认领的槽位
    ca::i64 need  = pop2 ? 2 : 1;
    bool    dup2Input = false;   // DUP2 的直接输入待定（禁 cat2）
    ca::usize p = popPos;
    while (need > 0) {
        if (p == 0) return false;
        --p;
        const jbc::ir::Insn* in = dom.peek(ids[p]);
        if (in == nullptr) return false;
        // 标签锚定 = 分支汇合边界：链上直线供源不成立。
        if (method_rewrite_util::labelsAnchorAt(dom, ids[p])) return false;
        if (in->op == jbc::core::Op::NOP) continue;
        if (isStackRelayOp(in->op)) {
            // 仅 DUP/DUP2 可吸收；重排/插底形态挂账。
            if (in->op != jbc::core::Op::DUP && in->op != jbc::core::Op::DUP2) return false;
            // 拷贝别名折叠：上方存量 + 窗口需求全部落在中继输出拷贝上，
            // 向下追溯一份原件（unit 槽）；窗口深入原件之下的余量并入。
            const ca::u32 unit     = in->op == jbc::core::Op::DUP2 ? 2u : 1u;
            const ca::u64 capacity = 2 * static_cast<ca::u64>(unit);
            const ca::u64 total    = static_cast<ca::u64>(above) + static_cast<ca::u64>(need);
            above = 0;
            need  = static_cast<ca::i64>(unit)
                 + (total > capacity ? static_cast<ca::i64>(total - capacity) : 0);
            relays.push_back(ids[p]);
            dup2Input = in->op == jbc::core::Op::DUP2;
            continue;
        }
        if (in->op == jbc::core::Op::POP || in->op == jbc::core::Op::POP2) {
            // 穿透仅限本批计划 pop（消费自有溯源认证）；未计划消费 = 弹值
            // 逃逸出认证面。
            if (planPops.find(ids[p].slot) == planPops.end()) return false;
            above += in->op == jbc::core::Op::POP ? 1u : 2u;
            continue;
        }
        // 纯常量生产者扣减；cat2 直接进 DUP2 输入 = 双槽形态挂账。
        if (!isFrameRemovableConstProducer(session, *in)) return false;
        const bool  cat2  = isCat2ConstProducer(session, *in);
        const ca::u32 width = cat2 ? 2u : 1u;
        if (above >= width) {
            above -= width;   // 上方消费认领，不入本链。
            continue;
        }
        if (above > 0) return false;   // cat2 半吃：跨消费分裂，不可推演。
        need -= static_cast<ca::i64>(width);
        if (need < 0) return false;   // DUP 遇 cat2 输入等超宽形态。
        if (dup2Input) {
            if (cat2) return false;   // DUP2 对 long/double 双槽：挂账。
            dup2Input = false;
        }
        consts.push_back(ids[p]);
    }
    return true;
}

/// 中继在场路的计划认证：每个计划从其 pop 直线回溯溯源，溯源常量须与
/// def 侧互证一致；删除常量的全部使用边必须落在本批计划 pop（逃逸消费
/// 者 = 删值破坏他人）；共享常量/中继的计划连通分组，组内删除面槽平衡
/// 非零（复制逃逸，如 [ICONST_1; DUP; POP] 的第二拷贝离场）整组弃。弃证
/// 收缩计划集后重算穿透许可，至不动点。返回存活计划（relays 已回填）。
std::vector<PopPlan> certifyRelayPlans(CodeDom& dom, const jbc::ir::ClassSession& session,
                                       const MethodDefUse&        du,
                                       const std::vector<InsnId>& ids,
                                       std::vector<PopPlan>       plans)
{
    std::vector<char> alive(plans.size(), 1);
    for (ca::usize round = 0; round <= plans.size() + 1; ++round) {
        std::unordered_set<ca::u32> planPops;
        for (ca::usize i = 0; i < plans.size(); ++i) {
            if (alive[i] != 0) planPops.insert(plans[i].pop.slot);
        }
        bool killed = false;

        // 溯源 + 互证。
        for (ca::usize i = 0; i < plans.size(); ++i) {
            if (alive[i] == 0) continue;
            const jbc::ir::Insn* pop = dom.peek(plans[i].pop);
            if (pop == nullptr) {
                alive[i] = 0;
                killed   = true;
                continue;
            }
            std::vector<InsnId> consts;
            std::vector<InsnId> relays;
            const ca::usize     pos = posIn(ids, plans[i].pop);
            if (pos >= ids.size()
                || !traceRelayChain(dom, session, ids, planPops, pos,
                                    pop->op == jbc::core::Op::POP2, consts, relays)) {
                alive[i] = 0;
                killed   = true;
                continue;
            }
            // def-use 侧与物理链互证：溯源常量必须就是计划定义集（半删的
            // 双侧不一致、分支汇合多定义在此失配弃证）。
            std::set<DefId> walked;
            for (const InsnId& c : consts) walked.insert(DefId::instruction(c));
            const std::set<DefId> planned(plans[i].defs.begin(), plans[i].defs.end());
            if (walked != planned) {
                alive[i] = 0;
                killed   = true;
                continue;
            }
            plans[i].relays = std::move(relays);
        }

        // 删除常量的使用边逃逸检查（中继在场路放宽了逐计划唯一消费，
        // 闭环改由本检查 + 组平衡承担）。
        std::unordered_set<ca::u32> badConsts;
        {
            std::unordered_set<ca::u32> seenConsts;
            for (ca::usize i = 0; i < plans.size(); ++i) {
                if (alive[i] == 0) continue;
                for (const DefId& d : plans[i].defs) {
                    if (!seenConsts.insert(d.id).second) continue;
                    for (const DefUseEdge* e : du.usesOf(d)) {
                        if (planPops.find(e->use.slot) == planPops.end()) {
                            badConsts.insert(d.id);
                            break;
                        }
                    }
                }
            }
        }
        if (!badConsts.empty()) {
            for (ca::usize i = 0; i < plans.size(); ++i) {
                if (alive[i] == 0) continue;
                for (const DefId& d : plans[i].defs) {
                    if (badConsts.count(d.id) != 0) {
                        alive[i] = 0;
                        killed   = true;
                        break;
                    }
                }
            }
        }

        // 连通分组（共享常量/中继）+ 组栈平衡（成员唯一化后计）。
        std::vector<ca::usize> parent(plans.size());
        for (ca::usize i = 0; i < parent.size(); ++i) parent[i] = i;
        const auto find = [&](ca::usize x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        {
            std::unordered_map<ca::u32, ca::usize> owner;   // 共享成员 slot → 首见计划
            for (ca::usize i = 0; i < plans.size(); ++i) {
                if (alive[i] == 0) continue;
                const auto touch = [&](ca::u32 slot) {
                    const auto it = owner.find(slot);
                    if (it == owner.end()) {
                        owner.emplace(slot, i);
                    } else {
                        parent[find(it->second)] = find(i);
                    }
                };
                for (const DefId& d : plans[i].defs) touch(d.id);
                for (const InsnId& r : plans[i].relays) touch(r.slot);
            }
        }
        {
            // root → 成员集合（槽号去重，句柄取首见计划的真实值）。
            std::map<ca::usize, std::unordered_set<ca::u32>>            constSlots;
            std::map<ca::usize, std::unordered_map<ca::u32, InsnId>>    relaySlots;
            std::map<ca::usize, std::unordered_map<ca::u32, InsnId>>    popSlots;
            for (ca::usize i = 0; i < plans.size(); ++i) {
                if (alive[i] == 0) continue;
                const ca::usize root = find(i);
                for (const DefId& d : plans[i].defs) constSlots[root].insert(d.id);
                for (const InsnId& r : plans[i].relays) relaySlots[root].emplace(r.slot, r);
                popSlots[root].emplace(plans[i].pop.slot, plans[i].pop);
            }
            for (const auto& [root, consts] : constSlots) {
                ca::i64 balance = 0;
                for (const ca::u32 slot : consts) {
                    const InsnId* iid = du.instructionOf(DefId{DefId::Kind::Instruction, slot});
                    const jbc::ir::Insn* in = iid != nullptr ? dom.peek(*iid) : nullptr;
                    if (in != nullptr) balance += isCat2ConstProducer(session, *in) ? 2 : 1;
                }
                const auto rit = relaySlots.find(root);
                if (rit != relaySlots.end()) {
                    for (const auto& [slot, rid] : rit->second) {
                        const jbc::ir::Insn* in = dom.peek(rid);
                        if (in != nullptr) balance += in->op == jbc::core::Op::DUP2 ? 2 : 1;
                    }
                }
                const auto pit = popSlots.find(root);
                if (pit != popSlots.end()) {
                    for (const auto& [slot, pid] : pit->second) {
                        const jbc::ir::Insn* in = dom.peek(pid);
                        if (in != nullptr) balance += in->op == jbc::core::Op::POP2 ? -2 : -1;
                    }
                }
                if (balance != 0) {
                    for (ca::usize i = 0; i < plans.size(); ++i) {
                        if (alive[i] != 0 && find(i) == root) {
                            alive[i] = 0;
                            killed   = true;
                        }
                    }
                }
            }
        }
        if (!killed) break;
    }
    std::vector<PopPlan> out;
    for (ca::usize i = 0; i < plans.size(); ++i) {
        if (alive[i] != 0) out.push_back(std::move(plans[i]));
    }
    return out;
}

}   // namespace

mjbase::spi::PreservedAnalyses UselessPopCleaner::run(mjbase::spi::MethodScope&       scope,
                                                      mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeDom&            dom = scope.code.dom;
    jbc::ir::CodeEditor editor(scope.code);

    bool modified = false;
    bool anyChange = true;
    while (anyChange) {
        anyChange = false;
        // 每轮快照：本轮内删除只发生在游标之前的窗口，快照句柄全程有效。
        const std::vector<InsnId> ids = dom.order();
        ca::usize                 i   = 0;
        while (i + 1 < ids.size()) {
            const bool pop2 = matchPair(dom, ids[i], ids[i + 1], /*pop2*/ true);
            if (!pop2 && !matchPair(dom, ids[i], ids[i + 1], /*pop2*/ false)) {
                ++i;
                continue;
            }
            // 匹配成功，顺序删除：先装载（fallthrough 重定向其引用到 POP），
            // 再 POP（引用随之落到原 POP 的后继）。两步的前置条件已由
            // matchPair 保证（POP 有边界标签时不匹配、POP 有后继）；任一步
            // 意外失败即停止本轮（已删除部分保留），下一轮快照重扫。
            const auto first = editor.erase(ids[i],
                                            jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
            if (first.is_err()) break;
            const auto second = editor.erase(ids[i + 1],
                                             jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);
            modified  = true;
            anyChange = true;
            if (second.is_err()) break;
            i += 2;
        }
    }

    // ── 帧溯源面 ──
    // 载体是 def-use 到达定义：合并所有路径后「弹值的全部到达定义都是纯
    // 常量生产者」→ 生产者组与 pop 一并删除（分支汇合的多定义同组处理）；
    // POP2 双 category-1 半删降 POP。窗口面只在单一路径相邻窗口上推理，
    // 跨距离/跨路径对归本面。
    //
    // 栈中继逐中继细化：方法内无中继时保持纯 def 路（逐计划唯一消费判据
    // 不变）；有中继时每个计划经直线溯源认证（traceRelayChain），收集链上
    // 可吸收的 DUP/DUP2，共享常量/中继的计划连通分组，组栈平衡非零（复制
    // 逃逸）或常量使用边逃出计划 pop 集整组弃（certifyRelayPlans）。X 族/
    // SWAP 重排、DUP2 的 long/double 双槽输入、链上标签汇合、未计划消费
    // 穿插保持 fail-closed。
    //
    // 边界守卫差异：本面不需要窗口面的标签锚定守卫——def-use 边覆盖进入
    // pop 的全部路径，标签进入点携带的到达定义已在合并集合里；删除仍走
    // fallthrough 重定向，引用链式落到 pop 后继。中继链上的标签锚定按直线
    // 供源不成立拒收（traceRelayChain）。
    bool                      frameChanged = false;
    const std::vector<InsnId> ids          = dom.order();
    if (!ids.empty()) {
        const MethodDefUse du =
            mjbase::asm_analyzer::analyzeDefUse(scope.session.backing().cp, scope.code);
        if (du.ok() && !du.truncated()) {
            bool relayPresent = false;
            for (const InsnId id : ids) {
                const jbc::ir::Insn* in = dom.peek(id);
                if (in != nullptr && isStackRelayOp(in->op)) {
                    relayPresent = true;
                    break;
                }
            }

            // 单侧弹值的可删定义集：该侧全部到达定义均为纯常量生产者。
            // 返回 nullopt = 该侧不可删（含非指令定义、无到达定义边两种
            // 看不见定义的形态）。唯一消费判据仅用于无中继的纯 def 路；
            // 中继在场时复制经中继别名沿用原定义，同一常量的使用边可落
            // 在多个计划 pop 上，闭环改由溯源认证 + 边检查 + 组平衡承担。
            const auto removableSide = [&](InsnId pop, ca::u32 index)
                -> std::optional<std::vector<DefId>> {
                std::vector<DefId> defs;
                for (const DefUseEdge* e : du.usesAt(pop)) {
                    if (e->slot != DefUseSlot::Stack || e->index != index) continue;
                    if (e->def.kind != DefId::Kind::Instruction) return std::nullopt;
                    const InsnId* iid = du.instructionOf(e->def);
                    if (iid == nullptr) return std::nullopt;
                    const jbc::ir::Insn* producer = dom.peek(*iid);
                    if (producer == nullptr
                        || !isFrameRemovableConstProducer(scope.session, *producer)) {
                        return std::nullopt;
                    }
                    if (!relayPresent) {
                        // 唯一消费者：该定义的全部使用边（含局部读转发）都
                        // 指向本 pop，跨计划共享在此天然被拒。
                        for (const DefUseEdge* u : du.usesOf(e->def)) {
                            if (!(u->use == pop)) return std::nullopt;
                        }
                    }
                    defs.push_back(e->def);
                }
                if (defs.empty()) return std::nullopt;
                // POP2 两侧可共享同一定义（中继拷贝透明），按 DefId 去重；
                // 删除面按指令只删一次。
                std::vector<DefId> unique;
                for (const DefId& d : defs) {
                    bool seen = false;
                    for (const DefId& u : unique) seen = seen || u == d;
                    if (!seen) unique.push_back(d);
                }
                return unique;
            };

            std::vector<PopPlan> plans;
            for (const InsnId popId : ids) {
                const jbc::ir::Insn* pop = dom.peek(popId);
                if (pop == nullptr) continue;
                if (pop->op != jbc::core::Op::POP && pop->op != jbc::core::Op::POP2) continue;
                const std::optional<std::vector<DefId>> top = removableSide(popId, 0);
                if (pop->op == jbc::core::Op::POP) {
                    if (top) plans.push_back(PopPlan{popId, false, std::move(*top), {}});
                    continue;
                }
                // POP2 顶值生产者是 cat2 → 单 cat2 弹值形态，整删；否则是
                // 双 cat1 形态（第二值 index=1）：两侧都可删整删，单侧可删
                // 半删降 POP。顶侧无定义集时 topCat2 取 false，走双 cat1
                // 分支（cat2 底槽不计值，index=1 无边 → 第二侧空 → 不动）。
                bool topCat2 = false;
                if (top && !top->empty()) {
                    const InsnId* iid = du.instructionOf(top->front());
                    const jbc::ir::Insn* producer = iid != nullptr ? dom.peek(*iid) : nullptr;
                    topCat2 = producer != nullptr && isCat2ConstProducer(scope.session, *producer);
                }
                if (topCat2) {
                    plans.push_back(PopPlan{popId, false, std::move(*top), {}});
                    continue;
                }
                const std::optional<std::vector<DefId>> second = removableSide(popId, 1);
                if (top && second) {
                    PopPlan plan{popId, false, {}, {}};
                    plan.defs = std::move(*top);
                    for (const DefId& d : *second) plan.defs.push_back(d);
                    plans.push_back(std::move(plan));
                } else if (top || second) {
                    plans.push_back(
                        PopPlan{popId, true, std::move(top ? *top : *second), {}});
                }
            }

            if (relayPresent && !plans.empty()) {
                plans = certifyRelayPlans(dom, scope.session, du, ids, std::move(plans));
            }

            if (!plans.empty()) {
                // 应用阶段一：删除全部计划生产者与吸收中继（全局按指令去
                // 重——共享中继/共享常量只删一次）。fallthrough 重定向把
                // 删除边界标签链式后移；中继链 [常量; DUP; POP2] 逐个 erase
                // 时引用接力落到最终后继。可达生产者必有顺序后继（弹值消费
                // 在其后），失败属防御路径——任一步失败停止全部后续应用，
                // 保部分一致。
                std::vector<InsnId>         eraseInsns;
                std::unordered_set<ca::u32> eraseSeen;
                const auto pushErase = [&](const DefId& d) {
                    const InsnId* iid = du.instructionOf(d);
                    if (iid != nullptr && eraseSeen.insert(iid->slot).second) {
                        eraseInsns.push_back(*iid);
                    }
                };
                for (const PopPlan& plan : plans) {
                    for (const DefId& d : plan.defs) pushErase(d);
                    for (const InsnId& r : plan.relays) {
                        if (eraseSeen.insert(r.slot).second) eraseInsns.push_back(r);
                    }
                }
                bool applyOk = true;
                for (const InsnId& id : eraseInsns) {
                    if (editor.erase(id,
                                     jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                            .is_err()) {
                        applyOk = false;
                        break;
                    }
                }
                // 应用阶段二：删/降级 pop。半删走原位改写（POP 无操作数，
                // 清空残留字段），整删走 fallthrough 重定向。中继路的半删
                // 计划已在溯源互证阶段弃证（双侧不一致），此处 downgrade
                // 计划只出现在无中继路。
                if (applyOk) {
                    for (const PopPlan& plan : plans) {
                        if (plan.downgrade) {
                            auto got = dom.get(plan.pop);
                            if (got.is_err()) break;
                            jbc::ir::Insn* p = std::move(got).unwrap();
                            p->op    = jbc::core::Op::POP;
                            p->target = jbc::ir::kNoLabel;
                            p->a     = 0;
                            p->b     = 0;
                            p->cp    = 0;
                            p->wide  = false;
                        } else if (editor.erase(plan.pop,
                                                jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                                       .is_err()) {
                            break;
                        }
                    }
                    frameChanged = true;
                }
            }
        }
    }
    return (modified || frameChanged) ? mjbase::spi::PreservedAnalyses::none()
                                      : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
