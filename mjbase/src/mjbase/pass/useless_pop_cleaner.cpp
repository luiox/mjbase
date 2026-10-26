#include "mjbase/pass/useless_pop_cleaner.hpp"

#include <optional>
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

/// 栈中继操作码（DUP/DUP2 族与 SWAP）。def-use 把复制/重排视为非使用边，
/// 中继搬运下的生产者删除会留下栈元数破坏的悬空中继（如
/// [ICONST_1; DUP; POP2]——删 ICONST_1 后 DUP 在空栈上复制）；方法内任一
/// 中继在场即整体禁用帧面。逐中继细化见头文件挂账。
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
/// DeadLocalStoreRemover 协同，见头文件挂账）；DUP/DUP2 生产者由中继
/// 守卫整体取代。
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

/// 帧面单条 pop 的删除计划。downgrade=true 时 POP2 降级 POP（半删），
/// 只删 defs 里的生产者；否则生产者与 pop 一并删除。
struct PopPlan
{
    InsnId               pop;
    bool                 downgrade = false;
    std::vector<DefId>   defs;
};

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
    // 常量生产者、且每个定义的全部使用边都指向本 pop」→ 生产者组 + pop
    // 整删（分支汇合的多定义同组处理）；POP2 双 category-1 半删降 POP。
    // 窗口面只在单一路径相邻窗口上推理，跨距离/跨路径对归本面。
    //
    // 边界守卫差异：本面不需要窗口面的标签锚定守卫——def-use 边覆盖进入
    // pop 的全部路径，标签进入点携带的到达定义已在合并集合里；删除仍走
    // fallthrough 重定向，引用链式落到 pop 后继。
    bool frameChanged = false;
    bool relayPresent = false;
    for (const InsnId id : dom.order()) {
        const jbc::ir::Insn* in = dom.peek(id);
        if (in != nullptr && isStackRelayOp(in->op)) {
            relayPresent = true;
            break;
        }
    }
    if (!relayPresent && !dom.order().empty()) {
        // 窗口面删除后重算：帧面看到的是窗口收敛后的方法体。
        const MethodDefUse du =
            mjbase::asm_analyzer::analyzeDefUse(scope.session.backing().cp, scope.code);
        if (du.ok() && !du.truncated()) {
            // 单侧弹值的可删定义集：该侧全部到达定义均为纯常量生产者、
            // 唯一消费者是本 pop。返回 nullopt = 该侧不可删（含非指令
            // 定义、无到达定义边两种看不见定义的形态）。
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
                    // 唯一消费者：该定义的全部使用边（含局部读转发）都指向
                    // 本 pop，跨计划共享在此天然被拒。
                    for (const DefUseEdge* u : du.usesOf(e->def)) {
                        if (!(u->use == pop)) return std::nullopt;
                    }
                    defs.push_back(e->def);
                }
                if (defs.empty()) return std::nullopt;
                return defs;
            };

            std::vector<PopPlan> plans;
            for (const InsnId popId : dom.order()) {
                const jbc::ir::Insn* pop = dom.peek(popId);
                if (pop == nullptr) continue;
                if (pop->op != jbc::core::Op::POP && pop->op != jbc::core::Op::POP2) continue;
                const std::optional<std::vector<DefId>> top = removableSide(popId, 0);
                if (pop->op == jbc::core::Op::POP) {
                    if (top) plans.push_back(PopPlan{popId, false, std::move(*top)});
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
                    plans.push_back(PopPlan{popId, false, std::move(*top)});
                    continue;
                }
                const std::optional<std::vector<DefId>> second = removableSide(popId, 1);
                if (top && second) {
                    PopPlan plan{popId, false, {}};
                    plan.defs = std::move(*top);
                    for (const DefId& d : *second) plan.defs.push_back(d);
                    plans.push_back(std::move(plan));
                } else if (top || second) {
                    plans.push_back(
                        PopPlan{popId, true, std::move(top ? *top : *second)});
                }
            }

            if (!plans.empty()) {
                // 应用阶段一：先删全部计划生产者。计划间定义互斥（唯一消费
                // 者判据），删除互不依赖；fallthrough 重定向把生产者边界标签
                // 链式后移。可达生产者必有顺序后继（弹值消费在其后），失败
                // 属防御路径——任一步失败停止全部后续应用，保部分一致。
                bool applyOk = true;
                for (const PopPlan& plan : plans) {
                    for (const DefId& d : plan.defs) {
                        const InsnId* iid = du.instructionOf(d);
                        if (iid == nullptr
                            || editor.erase(*iid,
                                            jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                                   .is_err()) {
                            applyOk = false;
                            break;
                        }
                    }
                    if (!applyOk) break;
                }
                // 应用阶段二：删/降级 pop。半删走原位改写（POP 无操作数，
                // 清空残留字段），整删走 fallthrough 重定向。
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
