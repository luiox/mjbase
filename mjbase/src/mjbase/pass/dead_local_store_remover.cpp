#include "mjbase/pass/dead_local_store_remover.hpp"

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
using jbc::ir::CodeDom;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::DefUseEdge;
using mjbase::asm_analyzer::DefUseSlot;
using mjbase::asm_analyzer::MethodDefUse;

/// 有意义汇点：消费值且行为可观测的指令族（旧栈 isMeaningfulSink 对齐）。
/// invoke 族 / 条件分支与 goto / switch / 字段写 / monitor / athrow /
/// 带值返回 / 数组存。跳转类本身不消费栈值，列入只为与旧栈口径一致。
bool isMeaningfulSink(Op op)
{
    const auto inRange = [](Op v, Op first, Op last) { return v >= first && v <= last; };
    return inRange(op, Op::INVOKEVIRTUAL, Op::INVOKEDYNAMIC)
           || inRange(op, Op::IFEQ, Op::IF_ACMPNE) || op == Op::GOTO || op == Op::JSR
           || op == Op::IFNULL || op == Op::IFNONNULL || op == Op::GOTO_W || op == Op::JSR_W
           || op == Op::LOOKUPSWITCH || op == Op::TABLESWITCH
           || op == Op::PUTSTATIC || op == Op::PUTFIELD
           || op == Op::MONITORENTER || op == Op::MONITOREXIT || op == Op::ATHROW
           || inRange(op, Op::IRETURN, Op::ARETURN)
           || inRange(op, Op::IASTORE, Op::SASTORE);
}

/// 读点闭包能否到达有意义汇点：从读的到达定义出发沿 def→use 边 BFS，
/// use 指令是汇点即命中；use 自身产生新值（运算/store/iinc）时作为中间
/// 定义续走（槽位间转存穿透）。dup/swap 复制透明——消费边直连原定义，
/// BFS 无需感知。
bool readReachesMeaningfulSink(const CodeDom& dom, const MethodDefUse& du, const DefId& def)
{
    std::set<DefId> visited;
    std::vector<DefId> queue{def};
    visited.insert(def);
    for (ca::usize head = 0; head < queue.size(); ++head) {
        for (const DefUseEdge* e : du.usesOf(queue[head])) {
            const jbc::ir::Insn* u = dom.peek(e->use);
            if (u != nullptr && isMeaningfulSink(u->op)) return true;
            // use 指令的产物（若有）以它自身为定义，续走消费闭包；
            // 无产物指令（pop/跳转等）在此自然终止。
            const DefId next = DefId::instruction(e->use);
            if (visited.insert(next).second) queue.push_back(next);
        }
    }
    return false;
}

/// 弹值定义是否纯常量生产者（与 UselessPopCleaner 帧面同族口径）。
bool isConstProducer(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in)
{
    ca::i32         iv = 0;
    ca::i64         lv = 0;
    float           fv = 0.0F;
    double          dv = 0.0;
    std::string_view sv;
    return in.op == Op::ACONST_NULL
           || method_rewrite_util::intValueOf(session, in, iv)
           || method_rewrite_util::longValueOf(session, in, lv)
           || method_rewrite_util::floatValueOf(session, in, fv)
           || method_rewrite_util::doubleValueOf(session, in, dv)
           || method_rewrite_util::stringValueOf(session, in, sv);
}

}   // namespace

mjbase::spi::PreservedAnalyses DeadLocalStoreRemover::run(mjbase::spi::MethodScope&       scope,
                                                          mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeDom& dom = scope.code.dom;
    if (dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();

    // 签名播种：参数值有定义边，参数槽不会被误判死槽；截断集合上的
    // 负向结论（读点不可达汇点 ⇒ 可删）不可靠，整件跳过。
    const jbc::analysis::MethodSig sig{
        (scope.access & mjbase::hierarchy::kAccStatic) != 0, scope.desc};
    const MethodDefUse du =
        mjbase::asm_analyzer::analyzeDefUse(scope.session.backing().cp, scope.code, sig);
    if (!du.ok() || du.truncated()) return mjbase::spi::PreservedAnalyses::all();

    // 槽位有意义判定：任一读点的闭包命中汇点即有意义；到达集合溢出或
    // 无定义边（看不见来源）保守记有意义。
    std::vector<char> meaningful;
    const auto markMeaningful = [&](ca::u32 slot) {
        if (meaningful.size() <= slot) meaningful.resize(slot + 1, 0);
        meaningful[slot] = 1;
    };
    for (const auto& access : du.localAccesses()) {
        if (access.kind != mjbase::asm_analyzer::LocalAccess::Kind::Read) continue;
        if (access.defs.empty() || access.defs.truncated()) {
            markMeaningful(access.local);
            continue;
        }
        for (ca::u32 i = 0; i < access.defs.size(); ++i) {
            if (readReachesMeaningfulSink(dom, du, access.defs.at(i))) {
                markMeaningful(access.local);
                break;
            }
        }
    }
    const auto isMeaningful = [&meaningful](ca::u32 slot) {
        return slot < meaningful.size() && meaningful[slot] != 0;
    };

    // 死 store 收集：覆盖槽位全部无意义。
    struct DeadStore
    {
        InsnId   id;
        ca::u32  width = 1;
    };
    std::vector<DeadStore> deadStores;
    for (const InsnId id : dom.order()) {
        const jbc::ir::Insn* in = dom.peek(id);
        if (in == nullptr) continue;
        ca::u32 base  = 0;
        ca::u32 width = 0;
        if (!method_rewrite_util::storeLocalSpan(*in, base, width)) continue;
        bool dead = true;
        for (ca::u32 s = 0; s < width; ++s) {
            if (isMeaningful(base + s)) {
                dead = false;
                break;
            }
        }
        if (dead) deadStores.push_back(DeadStore{id, width});
    }
    if (deadStores.empty()) return mjbase::spi::PreservedAnalyses::all();

    // 联合不动点：整删 store 集 removed 与纯常量集 consts 互约——
    //   store ∈ removed ⟺ 至少一条输入边且全部输入定义 ∈ consts；
    //   常量 ∈ consts ⟺ 全部使用边指向 removed 里的 store。
    // 从乐观初值（全部入集）单调收缩到不动点。候选常量 = 死 store 的
    // 输入定义中的纯常量生产者。
    // store 键用 InsnId::slot（同一 CodeUnit 内唯一）——InsnId 无序比较。
    std::set<ca::u32> removed;
    std::set<DefId>   consts;
    for (const DeadStore& ds : deadStores) removed.insert(ds.id.slot);
    for (const DefUseEdge& e : du.edges()) {
        if (removed.count(e.use.slot) == 0) continue;
        if (e.def.kind != DefId::Kind::Instruction) continue;
        const InsnId* iid = du.instructionOf(e.def);
        const jbc::ir::Insn* in = iid != nullptr ? dom.peek(*iid) : nullptr;
        if (in != nullptr && isConstProducer(scope.session, *in)) consts.insert(e.def);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const DeadStore& ds : deadStores) {
            if (removed.count(ds.id.slot) == 0) continue;
            bool covered = false;
            for (const DefUseEdge* e : du.usesAt(ds.id)) {
                if (e->slot != DefUseSlot::Stack) continue;
                if (consts.count(e->def) == 0) {
                    covered = false;
                    break;
                }
                covered = true;
            }
            if (!covered) {
                removed.erase(ds.id.slot);
                changed = true;
            }
        }
        for (auto it = consts.begin(); it != consts.end();) {
            bool outside = false;
            for (const DefUseEdge* u : du.usesOf(*it)) {
                if (removed.count(u->use.slot) == 0) {
                    outside = true;
                    break;
                }
            }
            if (outside) {
                it = consts.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    // 应用：先删常量生产者，再整删/降级 store。候选常量可达且有消费
    // 后继，删除走 fallthrough 重定向；降级只动操作码字段（宽度按 store
    // 族，弹宽不变）。任一步失败停止后续应用（防御路径）。
    bool modified = false;
    bool applyOk  = true;
    jbc::ir::CodeEditor editor(scope.code);
    for (const DefId& d : consts) {
        const InsnId* iid = du.instructionOf(d);
        if (iid == nullptr
            || editor.erase(*iid, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                   .is_err()) {
            applyOk = false;
            break;
        }
        modified = true;
    }
    if (applyOk) {
        for (const DeadStore& ds : deadStores) {
            if (removed.count(ds.id.slot) != 0) {
                if (editor.erase(ds.id, jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                        .is_err()) {
                    break;
                }
            } else {
                auto got = dom.get(ds.id);
                if (got.is_err()) break;
                jbc::ir::Insn* in = std::move(got).unwrap();
                in->op   = ds.width == 2 ? Op::POP2 : Op::POP;
                in->a    = 0;
                in->b    = 0;
                in->wide = false;
            }
            modified = true;
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
