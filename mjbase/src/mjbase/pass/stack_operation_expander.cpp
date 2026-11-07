#include "mjbase/pass/stack_operation_expander.hpp"

#include <algorithm>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/analyzer/origin_domain.hpp"

#include "method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using mjbase::asm_analyzer::OriginResult;
using mjbase::asm_analyzer::OriginValue;
using mjbase::asm_analyzer::traceOrigins;

/// DUP 族操作码（SWAP 不在族内：双值换位没有「复制」语义，见
/// doc:stack-op-expander-spec 挂账节）。
bool isDupFamilyOp(Op op) noexcept
{
    switch (op) {
    case Op::DUP:
    case Op::DUP_X1:
    case Op::DUP_X2:
    case Op::DUP2:
    case Op::DUP2_X1:
    case Op::DUP2_X2: return true;
    default: return false;
    }
}

/// DUP 槽预算：本指令复制区的槽位数（X 族的插入区另计）。
ca::u32 dupSlotBudget(Op op) noexcept
{
    switch (op) {
    case Op::DUP:
    case Op::DUP_X1:
    case Op::DUP_X2: return 1;
    case Op::DUP2:
    case Op::DUP2_X1:
    case Op::DUP2_X2: return 2;
    default: return 0;
    }
}

/// 展开需要读取的输入槽预算（复制区 + 插入区总宽）。
ca::u32 inputSlotBudget(Op op) noexcept
{
    switch (op) {
    case Op::DUP: return 1;
    case Op::DUP_X1: return 2;
    case Op::DUP_X2: return 3;
    case Op::DUP2: return 2;
    case Op::DUP2_X1: return 3;
    case Op::DUP2_X2: return 4;
    default: return 0;
    }
}

/// 帧面可克隆常量生产者：纯常量装载——int/long/float/double/string 常量、
/// ACONST_NULL 与 ldc Class/MethodType/MethodHandle（元数据常量克隆重发无
/// 求值副作用；Dynamic/condy 不认领，与 UselessPopCleaner 帧面生产者判定
/// 同一覆盖面——那边管删、这边管克隆，口径一致）。
bool isExpandableConstProducer(const jbc::ir::ClassSession& session, const Insn& in)
{
    ca::i32         iv = 0;
    ca::i64         lv = 0;
    float           fv = 0.0F;
    double          dv = 0.0;
    std::string_view sv;
    if (in.op == Op::ACONST_NULL
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

/// 生产者是否 category-2（long/double 常量装载）；宽度互证用。
bool isCat2ConstProducer(const jbc::ir::ClassSession& session, const Insn& in)
{
    ca::i64  lv = 0;
    double   dv = 0.0;
    return method_rewrite_util::longValueOf(session, in, lv)
           || method_rewrite_util::doubleValueOf(session, in, dv);
}

/// DUP 入口帧上的一个输入槽值：来源句柄 + 槽宽。
struct SlotValue
{
    InsnId producer = jbc::ir::kNoInsn;
    ca::u8 slots   = 1;
};

/// 从栈顶读 `needSlots` 个槽的输入值序列（切片按栈顶在首位存放；cat2 值
/// 占两行，底槽行由 k 步进跳过）。要求恰好覆盖预算：越过栈深或末值跨出
/// 预算（cat2 被复制区/插入区边界劈开——VerifyError 形态）都拒收。
bool readInputSlice(const OriginResult& origins, InsnId at, ca::u32 needSlots,
                    std::vector<SlotValue>& out)
{
    ca::u32 got = 0;
    ca::u32 k   = 0;
    while (got < needSlots) {
        const OriginValue* v = origins.stackFromTop(at, k);
        if (v == nullptr) return false;                 // 越过栈深
        if (!v->isInsn()) return false;                 // Param/未知来源不在展开面
        if (got + static_cast<ca::u32>(v->slots) > needSlots) {
            return false;                               // 边界劈开 cat2
        }
        out.push_back(SlotValue{v->insn, v->slots});
        got += static_cast<ca::u32>(v->slots);
        k   += static_cast<ca::u32>(v->slots);
    }
    return true;
}

/// 一条展开计划：窗口 [start, endExclusive)（本轮 order() 快照下标）整窗
/// 替换为 replacement（恒非空；deep→top 次序的克隆序列）。
struct ExpandPlan
{
    ca::i32            start         = 0;
    ca::i32            endExclusive  = 0;
    std::vector<Insn>  replacement;
};

/// 生产者克隆（cp 索引随值携带，不新增 CP 条目；srcPc 剥离）。
Insn cloneProducer(const CodeDom& dom, InsnId producer)
{
    const Insn* in = dom.peek(producer);
    Insn        clone = in != nullptr ? *in : Insn{};
    clone.srcPc = jbc::ir::kNoPc;
    return clone;
}

/// 单条 DUP 的展开计划构造；拒收返回 false（形态外保守不动）。
///
/// 形 A（dup/dup2）：窗口 = DUP 自身，替换 = 输入切片的来源克隆（deep→top
/// 重发一遍输入）；来源生产者保留原位。X 族（形 B）：窗口 = 输入切片来源
/// 生产者与 DUP 构成的连续窗，替换 = 输出形克隆序列（复制区 ++ 插入区 ++
/// 复制区），窗内生产者随之删除。
bool planForDup(const CodeDom& dom, const jbc::ir::ClassSession& session,
                const OriginResult& origins, ca::i32 dIndex, std::vector<ExpandPlan>& out)
{
    const std::vector<InsnId>& order = dom.order();
    const InsnId               d     = order[static_cast<ca::usize>(dIndex)];
    const Insn*                dup   = dom.peek(d);
    if (dup == nullptr || !isDupFamilyOp(dup->op)) return false;
    const Op                op       = dup->op;
    const ca::u32           budget   = dupSlotBudget(op);
    const ca::u32           need     = inputSlotBudget(op);
    std::vector<SlotValue>  slice;   // 栈顶在首位
    if (!readInputSlice(origins, d, need, slice)) return false;

    // 宽度互证：来源生产者的常量类别必须与帧槽宽一致（合法输入由 Verifier
    // 保证，此处双查防畸形）。
    for (const SlotValue& sv : slice) {
        const Insn* producer = dom.peek(sv.producer);
        if (producer == nullptr) return false;
        if (!origins.reachable(sv.producer)) return false;
        if (!isExpandableConstProducer(session, *producer)) return false;
        if (isCat2ConstProducer(session, *producer) != (sv.slots == 2)) return false;
    }

    // 复制区（栈顶在首位的前缀）槽预算必须被完整值填满：预算 1 要求顶值
    // cat1；预算 2 要求顶值 cat2，或顶两值都 cat1。
    if (budget == 1) {
        if (slice.front().slots != 1) return false;
    }
    else {
        if (slice.front().slots == 2) {
            // cat2 形：复制区 = 顶值，插入区 = 其余。
        }
        else if (slice.size() >= 2 && slice[1].slots == 1) {
            // 双 cat1 形：复制区 = 顶两值。
        }
        else {
            return false;
        }
    }

    // 替换序列（deep→top 克隆次序）。slice 栈顶在首位，反转即 deep→top。
    const auto cloneRange = [&dom, &slice](ca::usize from, ca::usize to) {
        std::vector<Insn> seq;
        for (ca::usize i = to; i-- > from;) seq.push_back(cloneProducer(dom, slice[i].producer));
        return seq;
    };
    const ca::usize dupValues = (budget == 2 && slice.front().slots == 2) ? 1
                                : static_cast<ca::usize>(budget);
    if (op == Op::DUP || op == Op::DUP2) {
        // 形 A：窗口 = DUP 自身，替换 = 整个输入切片的来源克隆。
        out.push_back(ExpandPlan{dIndex, dIndex + 1, cloneRange(0, slice.size())});
        return true;
    }

    // 形 B（X 族）：整窗连续性——输入切片的来源生产者必须恰为 DUP 之前
    // slice.size() 条指令、且词法序与栈序（deep→top）一致；共享来源（同
    // 一生产者喂两槽）在位置核对上天然失配，一并拒收。
    const ca::i32 start = dIndex - static_cast<ca::i32>(slice.size());
    if (start < 0) return false;
    for (ca::usize j = 0; j < slice.size(); ++j) {
        // slice 栈顶在首位：最深输入值是 slice.back()，对应窗口首条。
        const SlotValue& sv = slice[slice.size() - 1 - j];
        if (order[static_cast<ca::usize>(start) + j] != sv.producer) return false;
    }

    // 窗口纪律（首条豁免）：控制流/异常表边界不得锚定窗口中部——窗首
    // 生产者被覆写保留首边界标签语义，内点（含 DUP 自身落点）锚定即
    // 整窗拒收。
    for (ca::i32 q = start + 1; q <= dIndex; ++q) {
        if (method_rewrite_util::labelsAnchorAt(dom, order[static_cast<ca::usize>(q)])) {
            return false;
        }
    }

    // X 族输出形：复制区 ++ 插入区 ++ 复制区（各区 deep→top）。
    std::vector<Insn> replacement = cloneRange(0, dupValues);
    {
        std::vector<Insn> inserted = cloneRange(dupValues, slice.size());
        replacement.insert(replacement.end(), inserted.begin(), inserted.end());
        std::vector<Insn> dupAgain = cloneRange(0, dupValues);
        replacement.insert(replacement.end(), dupAgain.begin(), dupAgain.end());
    }
    out.push_back(ExpandPlan{start, dIndex + 1, std::move(replacement)});
    return true;
}

/// 落盘一条展开计划（替换恒非空；机械与 StackOperationSimplifier::applyPlan
/// 非空路径一致）：覆写窗口首条（保留首边界标签语义），倒序擦除窗口其余，
/// 链式插回剩余克隆。返回 false = DOM 拒绝，交调用方保守终止收敛。
bool applyExpandPlan(CodeDom& dom, const ExpandPlan& plan)
{
    const std::vector<InsnId>& order = dom.order();
    if (plan.start < 0 || plan.endExclusive > static_cast<ca::i32>(order.size())) {
        return false;
    }
    std::vector<InsnId> ids;
    ids.reserve(static_cast<ca::usize>(plan.endExclusive - plan.start));
    for (ca::i32 i = plan.start; i < plan.endExclusive; ++i) {
        ids.push_back(order[static_cast<ca::usize>(i)]);
    }
    auto got = dom.get(ids.front());
    if (got.is_err()) return false;
    *got.unwrap() = plan.replacement.front();
    for (ca::usize k = ids.size(); k > 1;) {
        --k;
        if (dom.erase(ids[k]).is_err()) return false;
    }
    InsnId cursor = ids.front();
    for (ca::usize r = 1; r < plan.replacement.size(); ++r) {
        auto ins = dom.insertAfter(cursor, plan.replacement[r]);
        if (ins.is_err()) return false;
        cursor = ins.unwrap();
    }
    return true;
}

}   // namespace

mjbase::spi::PreservedAnalyses StackOperationExpander::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    CodeDom& dom = scope.code.dom;
    if (dom.order().empty()) return mjbase::spi::PreservedAnalyses::all();

    bool changed = false;
    for (;;) {
        // 每轮重跑帧溯源：上一轮落盘后句柄/布局已变。分析失败保守停机。
        const OriginResult origins = traceOrigins(scope.session.backing().cp, scope.code);
        if (!origins.ok()) break;

        std::vector<ExpandPlan> plans;
        const std::vector<InsnId>& order = dom.order();
        for (ca::i32 i = 0; i < static_cast<ca::i32>(order.size()); ++i) {
            const Insn* in = dom.peek(order[static_cast<ca::usize>(i)]);
            if (in == nullptr || !isDupFamilyOp(in->op)) continue;
            if (!origins.reachable(order[static_cast<ca::usize>(i)])) continue;
            planForDup(dom, scope.session, origins, i, plans);
        }
        if (plans.empty()) break;

        // 最长不重叠规划：起点降序、同起点长窗优先；自右向左执行，已应用
        // 区间右侧下标不受左侧编辑影响，与新计划区间重叠者按窗口粒度跳过。
        std::sort(plans.begin(), plans.end(),
                  [](const ExpandPlan& a, const ExpandPlan& b) {
                      return a.start != b.start ? a.start > b.start
                                                : a.endExclusive > b.endExclusive;
                  });
        bool   applied   = false;
        bool   errored   = false;
        ca::i32 rightmost = static_cast<ca::i32>(order.size());
        for (const ExpandPlan& plan : plans) {
            if (plan.endExclusive > rightmost) continue;
            if (!applyExpandPlan(dom, plan)) {
                errored = true;
                break;
            }
            rightmost = plan.start;
            applied   = true;
        }
        if (errored) break;   // DOM 拒绝：保守终止本方法收敛
        if (!applied) break;  // 全部被拒收：无进展，防死循环
        changed = true;
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
