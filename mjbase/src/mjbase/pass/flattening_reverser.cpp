#include "mjbase/pass/flattening_reverser.hpp"

#include <algorithm>
#include <vector>

#include "jbc/analysis/frame.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {
namespace {

using jbc::core::Op;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

bool isIntLoadOp(Op op)
{
    return op == Op::ILOAD || (op >= Op::ILOAD_0 && op <= Op::ILOAD_3);
}

ca::u32 slotOfIntLoad(const jbc::ir::Insn& in)
{
    if (in.op == Op::ILOAD) return static_cast<ca::u32>(in.a);
    return static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::ILOAD_0);
}

bool isIntStoreOp(Op op)
{
    return op == Op::ISTORE || (op >= Op::ISTORE_0 && op <= Op::ISTORE_3);
}

ca::u32 slotOfIntStore(const jbc::ir::Insn& in)
{
    if (in.op == Op::ISTORE) return static_cast<ca::u32>(in.a);
    return static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::ISTORE_0);
}

bool isIntConstInsn(const jbc::ir::ClassSession& session, const jbc::ir::Insn& in,
                    ca::i32& value)
{
    return method_rewrite_util::intValueOf(session, in, value);
}

/// 两槽局部（long/double）的基槽装载/存储：命中返回基槽，用于状态槽
/// 高半槽占用审计（LSTORE q 写隐式覆盖 q+1）。
bool twoSlotLocalBase(const jbc::ir::Insn& in, ca::u32& slot)
{
    const auto inRange = [](Op op, Op first, Op last) { return op >= first && op <= last; };
    if (in.op == Op::LLOAD || in.op == Op::DLOAD || in.op == Op::LSTORE ||
        in.op == Op::DSTORE) {
        slot = static_cast<ca::u32>(in.a);
        return true;
    }
    if (inRange(in.op, Op::LLOAD_0, Op::LLOAD_3)) {
        slot = static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::LLOAD_0);
        return true;
    }
    if (inRange(in.op, Op::DLOAD_0, Op::DLOAD_3)) {
        slot = static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::DLOAD_0);
        return true;
    }
    if (inRange(in.op, Op::LSTORE_0, Op::LSTORE_3)) {
        slot = static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::LSTORE_0);
        return true;
    }
    if (inRange(in.op, Op::DSTORE_0, Op::DSTORE_3)) {
        slot = static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::DSTORE_0);
        return true;
    }
    return false;
}

bool isGotoOp(Op op)
{
    return op == Op::GOTO || op == Op::GOTO_W;
}

/// 无直落的指令（goto/athrow/return/switch 族）：物理前邻是其中之一时，
/// 分发块头没有直落入边。IF*/JSR 等有直落路径的形态不算。
bool neverFallsThrough(Op op)
{
    return isGotoOp(op) || op == Op::ATHROW || op == Op::LOOKUPSWITCH ||
           op == Op::TABLESWITCH ||
           (op >= Op::IRETURN && op <= Op::RETURN);
}

ca::usize posOfInsn(const std::vector<InsnId>& order, InsnId id)
{
    return static_cast<ca::usize>(std::find(order.begin(), order.end(), id) - order.begin());
}

/// 一个折叠步：常量 C 与二元 op（求值时按记录逆序复合）。
struct FoldStep
{
    ca::i32 c = 0;
    Op      op = Op::NOP;
};

/// 案表命中：lookup 按 keys 精确匹配，table 按 low + i 连续区间；未命中
/// 一律走 default 目标（回落语义，default 是合法状态）。
LabelId caseLabelFor(const jbc::ir::Insn& sw, ca::u32 value)
{
    const ca::i32 key = static_cast<ca::i32>(value);
    if (sw.op == Op::LOOKUPSWITCH) {
        for (ca::usize i = 0; i < sw.keys.size() && i < sw.targets.size(); ++i) {
            if (sw.keys[i] == key) return sw.targets[i];
        }
        return sw.target;
    }
    // TABLESWITCH：targets[i] 对应 key = a + i。
    const ca::i64 idx = static_cast<ca::i64>(key) - static_cast<ca::i64>(sw.a);
    if (idx >= 0 && idx < static_cast<ca::i64>(sw.targets.size())) {
        return sw.targets[static_cast<ca::usize>(idx)];
    }
    return sw.target;
}

/// 默认前驱形态行 1：intConst K; istore s（直存）。
bool directStorePred(const jbc::ir::ClassSession& session, const jbc::ir::CodeDom& dom,
                     const std::vector<InsnId>& order, ca::usize w, ca::i32& k)
{
    if (w < 1) return false;
    const jbc::ir::Insn* prev = dom.peek(order[w - 1]);
    return prev != nullptr && isIntConstInsn(session, *prev, k);
}

/// 默认前驱形态行 2：intConst K; istore t; iload t; istore s（单级中转
/// 弹跳——混淆器的状态写噪声形态；中转 store/load 本 pass 不动，属
/// store→load 转发清理面）。
bool bounceStorePred(const jbc::ir::ClassSession& session, const jbc::ir::CodeDom& dom,
                     const std::vector<InsnId>& order, ca::usize w, ca::i32& k)
{
    if (w < 3) return false;
    // 从写点（istore s）向前：iload t → istore t → intConst K。
    const jbc::ir::Insn* iloadT  = dom.peek(order[w - 1]);
    const jbc::ir::Insn* istoreT = dom.peek(order[w - 2]);
    const jbc::ir::Insn* kIn     = dom.peek(order[w - 3]);
    return iloadT != nullptr && istoreT != nullptr && kIn != nullptr &&
           isIntLoadOp(iloadT->op) && isIntStoreOp(istoreT->op) &&
           slotOfIntLoad(*iloadT) == slotOfIntStore(*istoreT) &&
           isIntConstInsn(session, *kIn, k);
}

}   // namespace

std::vector<FlatteningReverser::PredFormFn> FlatteningReverser::defaultPredForms()
{
    return {&directStorePred, &bounceStorePred};
}

FlatteningReverser::DispatchSpec FlatteningReverser::defaultDispatchSpec()
{
    return DispatchSpec{1, {Op::IADD, Op::ISUB, Op::IXOR}};
}

FlatteningReverser::FlatteningReverser(std::vector<PredFormFn> predForms,
                                       DispatchSpec            dispatch)
    : predForms_(std::move(predForms)), dispatch_(std::move(dispatch))
{
}

mjbase::spi::PreservedAnalyses FlatteningReverser::run(mjbase::spi::MethodScope&          scope,
                                                       mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::CodeUnit& cu = scope.code;

    const std::vector<InsnId> order = cu.dom.order();
    const ca::usize           n     = order.size();

    const auto abandon = [] { return mjbase::spi::PreservedAnalyses::all(); };

    // ── 引擎步 1：全方法 switch 唯一（多汇合点无法单值传播）。──
    ca::usize swPos = n;
    for (ca::usize i = 0; i < n; ++i) {
        const jbc::ir::Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) continue;
        if (in->op == Op::LOOKUPSWITCH || in->op == Op::TABLESWITCH) {
            if (swPos != n) return abandon();
            swPos = i;
        }
    }
    if (swPos == n || swPos == 0) return abandon();
    const jbc::ir::Insn* sw = cu.dom.peek(order[swPos]);

    // ── 引擎步 2：分发操作数解析（形态表：折叠步规格）。
    //    从 switch 向上吞掉至多 maxSteps 个 (intConst C; binop) 对，随后
    //    必须恰到一个 iload（状态槽），区间再无其他指令。──
    ca::usize           headPos = swPos;
    std::vector<FoldStep> steps;
    while (steps.size() < dispatch_.maxSteps && headPos >= 2) {
        const jbc::ir::Insn* binop  = cu.dom.peek(order[headPos - 1]);
        const jbc::ir::Insn* constIn = cu.dom.peek(order[headPos - 2]);
        ca::i32             c = 0;
        if (binop == nullptr || constIn == nullptr) break;
        const bool binopOk = std::find(dispatch_.binops.begin(), dispatch_.binops.end(),
                                       binop->op) != dispatch_.binops.end();
        if (!binopOk || !isIntConstInsn(scope.session, *constIn, c)) break;
        steps.push_back({c, binop->op});
        headPos -= 2;
    }
    if (headPos < 1) return abandon();
    {
        const jbc::ir::Insn* load = cu.dom.peek(order[headPos - 1]);
        if (load == nullptr || !isIntLoadOp(load->op)) return abandon();
        --headPos;   // headPos 现指向 iload 本身
    }
    const ca::u32 slot = slotOfIntLoad(*cu.dom.peek(order[headPos]));
    if (headPos == 0) return abandon();   // 入口边不可静态解析

    // ── 引擎步 3：状态槽读写全量审计。──
    std::vector<ca::usize> writes;
    for (ca::usize i = 0; i < n; ++i) {
        const jbc::ir::Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) continue;
        if (isIntLoadOp(in->op) && slotOfIntLoad(*in) == slot) {
            if (i != headPos) return abandon();   // 第二读点：被读作他用
        }
        if (isIntStoreOp(in->op) && slotOfIntStore(*in) == slot) writes.push_back(i);
        // iinc 是读改写，两槽值高半槽是隐式覆盖：两者都不在引擎建模面。
        if (in->op == Op::IINC && static_cast<ca::u32>(in->a) == slot) {
            return abandon();
        }
        ca::u32 base = 0;
        if (twoSlotLocalBase(*in, base) && base + 1 == slot) return abandon();
    }
    if (writes.empty()) return abandon();

    // ── 引擎步 4：逐写点用前驱形态表解析常量状态；任一写点无行命中或
    //    后继不是 goto D → 交叠定值/不可静态求值，整族拒收。──
    struct ResolvedPred
    {
        InsnId  gotoId;
        LabelId newTarget;
    };
    std::vector<ResolvedPred> preds;
    preds.reserve(writes.size());
    for (const ca::usize w : writes) {
        if (w + 1 >= n) return abandon();
        const jbc::ir::Insn* gotoIn = cu.dom.peek(order[w + 1]);
        if (gotoIn == nullptr || !isGotoOp(gotoIn->op)) return abandon();
        auto resolved = cu.dom.resolveLabel(gotoIn->target);
        if (resolved.is_err()) return abandon();
        const jbc::ir::LabelTarget t = std::move(resolved).unwrap();
        if (t.isEnd() || posOfInsn(order, t.insn) != headPos) return abandon();

        ca::i32 k = 0;
        bool   matched = false;
        for (const PredFormFn& form : predForms_) {
            if (form != nullptr && form(scope.session, cu.dom, order, w, k)) {
                matched = true;
                break;
            }
        }
        if (!matched) return abandon();

        // v = ((K op1 C1) op2 C2)…：折叠步按距 switch 由近及远记录，
        // 求值逆序复合（先复合最靠近 iload 的步）。u32 环绕，无符号 UB。
        ca::u32 v = static_cast<ca::u32>(k);
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            const ca::u32 c = static_cast<ca::u32>(it->c);
            if (it->op == Op::IADD) v += c;
            else if (it->op == Op::ISUB) v -= c;
            else if (it->op == Op::IXOR) v ^= c;
            else return abandon();
        }
        const LabelId caseLabel = caseLabelFor(*sw, v);
        if (caseLabel == jbc::ir::kNoLabel) return abandon();
        preds.push_back({order[w + 1], caseLabel});
    }

    // ── 引擎步 5a：入边守卫——非前驱分支/switch 目标锚入分发块区间
    //    [headPos, swPos]（含块头）→ 拒收；switch 案表自引用由同一条扫描
    //    覆盖（switch 不是前驱 goto）。──
    {
        std::vector<InsnId> predGotos;
        predGotos.reserve(preds.size());
        for (const ResolvedPred& p : preds) predGotos.push_back(p.gotoId);
        for (ca::usize i = 0; i < n; ++i) {
            const InsnId             id = order[i];
            const jbc::ir::Insn* in = cu.dom.peek(id);
            if (in == nullptr) continue;
            const auto checkTarget = [&](LabelId l) {
                if (l == jbc::ir::kNoLabel) return true;
                auto resolved = cu.dom.resolveLabel(l);
                if (resolved.is_err()) return false;
                const jbc::ir::LabelTarget t = std::move(resolved).unwrap();
                if (t.isEnd()) return true;
                const ca::usize pos = posOfInsn(order, t.insn);
                if (pos > headPos && pos <= swPos) return false;   // 锚入区间
                if (pos == headPos &&
                    std::find(predGotos.begin(), predGotos.end(), id) == predGotos.end()) {
                    return false;   // 指向 D 头但不是已识别前驱
                }
                return true;
            };
            if (!checkTarget(in->target)) return abandon();
            for (const LabelId l : in->targets) {
                if (!checkTarget(l)) return abandon();
            }
        }
    }

    // ── 引擎步 5b：物理直落进分发块头——前邻必须是无直落指令。──
    {
        const jbc::ir::Insn* prev = cu.dom.peek(order[headPos - 1]);
        if (prev == nullptr || !neverFallsThrough(prev->op)) return abandon();
    }

    // ── 引擎步 5c：帧深等价门——前驱 goto 点与 case 入口的到达栈深相等
    //    （状态链净栈效应 0、switch 弹掉派发值后的入口栈深同源）。──
    {
        jbc::analysis::FrameEngine          engine(scope.session.backing().cp);
        std::vector<jbc::analysis::FrameAt> frames;
        const jbc::analysis::FrameSummary   summary = engine.compute(cu, frames);
        if (!summary.ok || frames.size() != n) return abandon();
        for (const ResolvedPred& p : preds) {
            auto resolved = cu.dom.resolveLabel(p.newTarget);
            if (resolved.is_err()) return abandon();
            const jbc::ir::LabelTarget t = std::move(resolved).unwrap();
            if (t.isEnd()) return abandon();
            const jbc::analysis::FrameAt& atGoto = frames[posOfInsn(order, p.gotoId)];
            const jbc::analysis::FrameAt& atCase = frames[posOfInsn(order, t.insn)];
            if (!atGoto.reachable || !atCase.reachable) return abandon();
            if (atGoto.stack.size() != atCase.stack.size()) return abandon();
        }
    }

    // ── 引擎步 6：事务性统一改写——goto 目标直接复用 switch 的 case 标签
    //    句柄；先全量取可变句柄再落笔，任一失败零副作用返回。──
    std::vector<jbc::ir::Insn*> handles;
    handles.reserve(preds.size());
    for (const ResolvedPred& p : preds) {
        auto got = cu.dom.get(p.gotoId);
        if (got.is_err()) return abandon();
        handles.push_back(std::move(got).unwrap());
    }
    for (ca::usize i = 0; i < preds.size(); ++i) {
        handles[i]->target = preds[i].newTarget;
    }
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
