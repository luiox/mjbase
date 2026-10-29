#include "mjbase/pass/stack_operation.hpp"

#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

#include "mjbase/asm/tmatcher/combinators.hpp"
#include "mjbase/asm/tmatcher/insn_planner.hpp"
#include "mjbase/asm/tmatcher/match_step.hpp"
#include "mjbase/asm/tmatcher/match_state.hpp"
#include "mjbase/asm/tmatcher/replace_rule.hpp"

#include "method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

namespace tm = mjbase::asm_tmatcher;

using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;

constexpr ca::u8 opVal(Op op) noexcept
{
    return static_cast<ca::u8>(op);
}

bool opInRange(Op op, Op lo, Op hi) noexcept
{
    return opVal(op) >= opVal(lo) && opVal(op) <= opVal(hi);
}

// ── 装载族叶子谓词（纯 DOM 宽语义；op 集与旧 asm/matcher step_util 一致）──
//
// ldc/ldc_w 载荷类型在纯 DOM 层不可判别，按宽语义计入 cat1 族——被匹配
// 序列的栈形状本身保证折叠栈正确（cat1 与 pop / cat2 与 pop2 对齐）；
// ldc2_w 不入任何族（cat2 装载只认 lconst/dconst 家族）。

bool isLoadInt(const Insn& in) noexcept
{
    return in.op == Op::ILOAD || in.op == Op::BIPUSH || in.op == Op::SIPUSH
           || in.op == Op::LDC || in.op == Op::LDC_W
           || opInRange(in.op, Op::ILOAD_0, Op::ILOAD_3)
           || opInRange(in.op, Op::ICONST_M1, Op::ICONST_5);
}

bool isLoadFloat(const Insn& in) noexcept
{
    return in.op == Op::FLOAD || in.op == Op::LDC || in.op == Op::LDC_W
           || opInRange(in.op, Op::FLOAD_0, Op::FLOAD_3)
           || opInRange(in.op, Op::FCONST_0, Op::FCONST_2);
}

bool isLoadLong(const Insn& in) noexcept
{
    return in.op == Op::LLOAD || opInRange(in.op, Op::LLOAD_0, Op::LLOAD_3)
           || in.op == Op::LCONST_0 || in.op == Op::LCONST_1;
}

bool isLoadDouble(const Insn& in) noexcept
{
    return in.op == Op::DLOAD || opInRange(in.op, Op::DLOAD_0, Op::DLOAD_3)
           || in.op == Op::DCONST_0 || in.op == Op::DCONST_1;
}

bool isLoadStr(const Insn& in) noexcept
{
    return in.op == Op::LDC || in.op == Op::LDC_W;
}

template<bool (*PredFn)(const Insn&) noexcept>
tm::StepPtr loadLeaf(std::string name)
{
    return tm::combinators::pred(name, [](const tm::InsnCursorView& view, const tm::CaptureStore&) {
        return view.valid() && PredFn(*view.insn);
    });
}

/// 无操作数指令值（POP/POP2/DUP 族重建用）。
Insn plain(Op op)
{
    Insn in;
    in.op = op;
    return in;
}

/// 窗口内第 i 条指令的值拷贝（cp 索引随值携带）；越界即规则定义缺陷。
/// 变参步列表（initializer_list 会拷贝 unique_ptr，不能直用花括号列表）。
template<typename... Steps>
std::vector<tm::StepPtr> makeSteps(Steps... steps)
{
    std::vector<tm::StepPtr> out;
    (out.push_back(std::move(steps)), ...);
    return out;
}

Insn cloneAt(const tm::ReplaceContext& ctx, std::size_t i)
{
    const Insn* in = ctx.origin(i);
    return in != nullptr ? *in : plain(Op::NOP);
}

tm::InsnPlanner& stackPlanner()
{
    static tm::InsnPlanner planner = [] {
        tm::InsnPlanner p;
        const auto op = [](Op o) { return tm::combinators::opcode(o); };
        const auto li = [] { return loadLeaf<isLoadInt>("loadInt"); };
        const auto lf = [] { return loadLeaf<isLoadFloat>("loadFloat"); };
        const auto lj = [] { return loadLeaf<isLoadLong>("loadLong"); };
        const auto ld = [] { return loadLeaf<isLoadDouble>("loadDouble"); };
        const auto ls = [] { return loadLeaf<isLoadStr>("loadStr"); };
        const auto elim = [&p](std::string_view name, std::vector<tm::StepPtr> steps) {
            // 空替换 = 整窗删除（消除必须显式走空组，无产出器规则不产出计划）。
            p.addRule(tm::ReplaceRule(name, tm::combinators::seq(std::move(steps)),
                                      [](const tm::ReplaceContext&) {
                                          return std::vector<Insn>{};
                                      }));
        };
        const auto rewrite = [&p](std::string_view name, std::vector<tm::StepPtr> steps,
                                  tm::ReplaceProducer producer) {
            p.addRule(tm::ReplaceRule(name, tm::combinators::seq(std::move(steps)), std::move(producer)));
        };

        // ── 消除（空替换 = 整窗删除）──
        elim("dup-pop", makeSteps(op(Op::DUP), op(Op::POP)));
        elim("swap-swap", makeSteps(op(Op::SWAP), op(Op::SWAP)));
        elim("dup2-pop2", makeSteps(op(Op::DUP2), op(Op::POP2)));

        // cat1 装载 - pop => ∅
        elim("load-int-pop", makeSteps(li(), op(Op::POP)));
        elim("load-float-pop", makeSteps(lf(), op(Op::POP)));
        elim("load-str-pop", makeSteps(ls(), op(Op::POP)));
        // cat2 装载 - pop2 => ∅
        elim("load-long-pop2", makeSteps(lj(), op(Op::POP2)));
        elim("load-double-pop2", makeSteps(ld(), op(Op::POP2)));
        // cat1 装载 ×2 - pop2 => ∅（Longest 裁决：双装载形状整组消费，
        // 不给单装载短窗留抢先命中面——旧规则序纪律在双装载形状上会
        // 吃掉短窗留下死装载，见头注偏离申报）
        elim("load-int-load-int-pop2", makeSteps(li(), li(), op(Op::POP2)));
        elim("load-float-load-float-pop2", makeSteps(lf(), lf(), op(Op::POP2)));

        // ── 合并 ──
        rewrite("pop-pop-pop2", makeSteps(op(Op::POP), op(Op::POP)),
                [](const tm::ReplaceContext&) { return std::vector<Insn>{plain(Op::POP2)}; });
        rewrite("dup-pop2-pop", makeSteps(op(Op::DUP), op(Op::POP2)),
                [](const tm::ReplaceContext&) { return std::vector<Insn>{plain(Op::POP)}; });
        rewrite("load-int-pop2-pop", makeSteps(li(), op(Op::POP2)),
                [](const tm::ReplaceContext&) { return std::vector<Insn>{plain(Op::POP)}; });
        rewrite("load-float-pop2-pop", makeSteps(lf(), op(Op::POP2)),
                [](const tm::ReplaceContext&) { return std::vector<Insn>{plain(Op::POP)}; });
        rewrite("load-str-pop2-pop", makeSteps(ls(), op(Op::POP2)),
                [](const tm::ReplaceContext&) { return std::vector<Insn>{plain(Op::POP)}; });
        // （`[dup_x1;swap]=>[dup]` 退役：栈面不等价，见头注规则面退役申报。）

        // cat1 装载 - dup => 装载 ×2
        const auto cloneTwice = [](const tm::ReplaceContext& ctx) {
            return std::vector<Insn>{cloneAt(ctx, 0), cloneAt(ctx, 0)};
        };
        rewrite("load-int-dup", makeSteps(li(), op(Op::DUP)), cloneTwice);
        rewrite("load-float-dup", makeSteps(lf(), op(Op::DUP)), cloneTwice);
        rewrite("load-str-dup", makeSteps(ls(), op(Op::DUP)), cloneTwice);
        // cat2 装载 - dup2 => 装载 ×2
        rewrite("load-long-dup2", makeSteps(lj(), op(Op::DUP2)), cloneTwice);
        rewrite("load-double-dup2", makeSteps(ld(), op(Op::DUP2)), cloneTwice);

        // cat1 装载 - swap - pop => pop - 装载（弹走被换上的旧栈顶，装载存活）
        const auto popThenLoad = [](const tm::ReplaceContext& ctx) {
            return std::vector<Insn>{plain(Op::POP), cloneAt(ctx, 0)};
        };
        rewrite("load-int-swap-pop", makeSteps(li(), op(Op::SWAP), op(Op::POP)), popThenLoad);
        rewrite("load-float-swap-pop", makeSteps(lf(), op(Op::SWAP), op(Op::POP)), popThenLoad);
        rewrite("load-str-swap-pop", makeSteps(ls(), op(Op::SWAP), op(Op::POP)), popThenLoad);

        // cat1 装载(1) - 装载(2) - swap => 装载(2) - 装载(1)
        const auto swapLoads = [](const tm::ReplaceContext& ctx) {
            return std::vector<Insn>{cloneAt(ctx, 1), cloneAt(ctx, 0)};
        };
        rewrite("load-load-swap", makeSteps(li(), li(), op(Op::SWAP)), swapLoads);
        rewrite("loadf-loadf-swap", makeSteps(lf(), lf(), op(Op::SWAP)), swapLoads);

        // 装载(1) - 装载(2) - dup_x1 - pop2 => 装载(2)
        const auto keepSecond = [](const tm::ReplaceContext& ctx) {
            return std::vector<Insn>{cloneAt(ctx, 1)};
        };
        rewrite("load-load-dup_x1-pop2", makeSteps(li(), li(), op(Op::DUP_X1), op(Op::POP2)),
                keepSecond);
        rewrite("loadf-loadf-dup_x1-pop2", makeSteps(lf(), lf(), op(Op::DUP_X1), op(Op::POP2)),
                keepSecond);

        // cat2 装载(1) - 装载(2) - dup2_x2 - pop2 - pop2 => 装载(2)
        rewrite("loadl-loadl-dup2_x2-pop2-pop2",
                makeSteps(lj(), lj(), op(Op::DUP2_X2), op(Op::POP2), op(Op::POP2)), keepSecond);
        rewrite("loadd-loadd-dup2_x2-pop2-pop2",
                makeSteps(ld(), ld(), op(Op::DUP2_X2), op(Op::POP2), op(Op::POP2)), keepSecond);

        // 装载(1) - 装载(2) - dup_x1 - pop => 装载(2) - 装载(1)
        rewrite("load-load-dup_x1-pop", makeSteps(li(), li(), op(Op::DUP_X1), op(Op::POP)),
                swapLoads);
        rewrite("loadf-loadf-dup_x1-pop", makeSteps(lf(), lf(), op(Op::DUP_X1), op(Op::POP)),
                swapLoads);

        return p;
    }();
    return planner;
}

/// 落盘一个替换计划（旧 applyWindow 语义对齐）：
///
/// 替换非空：覆写窗口首条（保留首边界标签语义），倒序擦除窗口其余，
/// 链式插回剩余替换。替换为空：倒序擦除窗口其余，窗口外分支引用重定向
/// 顺序后继（label 落点语义），再擦首条。替换指令统一剥 srcPc。
/// 替换面全为装载克隆/无操作数栈操作，不携带分支目标，无标签引用面。
/// 返回 false = DOM 拒绝（纪律门之后仅剩 DOM 内部错误族），交调用方
/// 保守终止收敛。
bool applyPlan(CodeDom& dom, const tm::ReplacePlan& plan)
{
    // 计划基于本轮 order() 快照；计划自右向左执行，右侧编辑不动左侧
    // 下标，此处重取 order() 即得窗口成员当前句柄。
    const std::vector<InsnId>& order = dom.order();
    if (plan.start < 0 || plan.endExclusive > static_cast<ca::i32>(order.size())) {
        return false;
    }
    std::vector<InsnId> ids;
    ids.reserve(static_cast<ca::usize>(plan.length()));
    for (ca::i32 i = plan.start; i < plan.endExclusive; ++i) ids.push_back(order[i]);

    std::vector<Insn> replacement = plan.replacement;
    for (Insn& r : replacement) r.srcPc = jbc::ir::kNoPc;

    if (!replacement.empty()) {
        auto got = dom.get(ids.front());
        if (got.is_err()) return false;
        *got.unwrap() = replacement.front();
        for (ca::usize k = ids.size(); k > 1;) {
            --k;
            if (dom.erase(ids[k]).is_err()) return false;
        }
        InsnId cursor = ids.front();
        for (ca::usize r = 1; r < replacement.size(); ++r) {
            auto ins = dom.insertAfter(cursor, replacement[r]);
            if (ins.is_err()) return false;
            cursor = ins.unwrap();
        }
        return true;
    }

    for (ca::usize k = ids.size(); k > 1;) {
        --k;
        if (dom.erase(ids[k]).is_err()) return false;
    }
    const std::vector<InsnId>& fresh    = dom.order();
    const ca::i32              nextIndex = plan.endExclusive;
    const bool                 nextValid =
        nextIndex >= 0 && nextIndex < static_cast<ca::i32>(fresh.size());
    if (nextValid && !dom.usersOf(ids.front()).empty()) {
        if (dom.replaceAllUses(ids.front(), fresh[nextIndex]).is_err()) return false;
    }
    return dom.erase(ids.front()).is_ok();
}

}   // namespace

mjbase::spi::PreservedAnalyses StackOperationSimplifier::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeDom& dom = scope.code.dom;
    if (dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();

    tm::InsnPlanner& planner = stackPlanner();
    bool             changed = false;
    for (;;) {
        const std::vector<tm::ReplacePlan> plans = planner.plansFor(dom);
        if (plans.empty()) break;

        bool applied = false;
        bool errored = false;
        // 计划区间不重叠（Longest 裁决），自右向左执行免疫左侧下标移动。
        for (ca::usize pi = plans.size(); pi-- > 0;) {
            const tm::ReplacePlan& plan = plans[pi];

            // 窗口纪律（首条豁免）：控制流/异常边界不得锚定窗口中部——
            // 分支目标进入窗内在擦除后语义不保，按窗口粒度跳过（头注
            // 挂账兑现：不再整方法保守终止）。
            const std::vector<InsnId>& order = dom.order();
            bool                       anchored = false;
            for (ca::i32 q = plan.start + 1; q < plan.endExclusive; ++q) {
                if (method_rewrite_util::labelsAnchorAt(dom, order[q])) {
                    anchored = true;
                    break;
                }
            }
            if (anchored) continue;

            if (!applyPlan(dom, plan)) {
                errored = true;
                break;
            }
            applied = true;
        }
        if (errored) break;   // DOM 拒绝：保守终止本方法收敛（纪律门外仅剩内部错误族）
        if (!applied) break;  // 全部被纪律拒收：无进展，防死循环
        changed = true;
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
