#include "mjbase/pass/flattening_reverser.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
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

// 案表命中解析（lookup 按 keys 精确匹配，table 按 low + i 连续区间；未命中
// 一律走 default 目标——回落语义，default 是合法状态）提升至
// method_rewrite_util::switchCaseLabelOf 共享。

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

/// 相对状态更新语义：s' = s op D（iinc 是 +D 的紧凑形）。
struct RelUpdate
{
    Op      op = Op::NOP;
    ca::i32 d  = 0;
};

/// 一个状态槽写点：绝对形态（形态表解析出常量 K）或相对形态（s += D）。
struct StateWrite
{
    ca::usize pos = 0;
    bool      rel = false;
    ca::i32   k   = 0;   ///< 绝对形态的常量 K。
    RelUpdate relupd{};
};

/// 相对状态更新形态解析（写点 w 是对 slot 的 istore；s' = s op D）：
///   s 在左：[iload s; intConst D; binop]，binop ∈ {IADD, ISUB, IXOR}；
///   D 在左：[intConst D; iload s; binop]——IADD/IXOR 交换律下与 s 左同值
///   放行；ISUB 的 D 在左是 D - s 形（非 s += D 族），fail-closed。
/// 命中返回 true 并写出更新语义与 s 读点位置（读点审计豁免用）。
bool matchRelUpdate(const jbc::ir::ClassSession& session, const jbc::ir::CodeDom& dom,
                    const std::vector<InsnId>& order, ca::usize w, ca::u32 slot,
                    RelUpdate& out, ca::usize& readPos)
{
    if (w < 3) return false;
    const jbc::ir::Insn* binop = dom.peek(order[w - 1]);
    if (binop == nullptr) return false;
    if (binop->op != Op::IADD && binop->op != Op::ISUB && binop->op != Op::IXOR) return false;
    const jbc::ir::Insn* mid = dom.peek(order[w - 2]);
    const jbc::ir::Insn* low = dom.peek(order[w - 3]);
    if (mid == nullptr || low == nullptr) return false;
    ca::i32 d = 0;
    if (isIntLoadOp(low->op) && slotOfIntLoad(*low) == slot
        && isIntConstInsn(session, *mid, d)) {
        out.op   = binop->op;   // s 在左：s op D
        out.d    = d;
        readPos  = w - 3;
        return true;
    }
    if (binop->op == Op::ISUB) return false;   // D - s 形不认
    if (isIntConstInsn(session, *low, d) && isIntLoadOp(mid->op)
        && slotOfIntLoad(*mid) == slot) {
        out.op   = binop->op;   // D 在左：D op s（IADD/IXOR 与 s 左同值）
        out.d    = d;
        readPos  = w - 2;
        return true;
    }
    return false;
}

/// 控制转移指令（状态迹直线供源扫描的停点）：if 族/goto/switch/return/
/// athrow/jsr/ret。直线供源越过控制转移不可静态成立。
bool isControlTransfer(Op op)
{
    if (isGotoOp(op) || op == Op::ATHROW || op == Op::LOOKUPSWITCH || op == Op::TABLESWITCH
        || op == Op::JSR || op == Op::JSR_W || op == Op::RET) {
        return true;
    }
    if (op >= Op::IFEQ && op <= Op::IF_ACMPNE) return true;
    return op == Op::IFNULL || op == Op::IFNONNULL
           || (op >= Op::IRETURN && op <= Op::RETURN);
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
    //    形态 A（常量在右，既有）：[iload s; (intConst C; binop)*; switch]，
    //      v = ((K op C₁) op C₂)…；
    //    形态 B（镜像，常量在左）：[intConst 列; iload s; binop 链; switch]，
    //      常量列在装载之下，近载端常量先应用、与 binop 链远端配对：
    //      v = C₂ op₂ (C₁ op₁ K)。
    //    两形态各自整链一致，混合链（部分步常量在左部分在右）拒收（挂账）。──
    ca::usize             iloadPos  = n;
    ca::usize             blockHead = n;   // 分发块头（入边守卫用）
    bool                  mirrored  = false;
    std::vector<FoldStep> steps;           // 应用序：先应用的在前
    {
        // 形态 A：从 switch 向上吞 (intConst C; binop) 对到 iload。
        ca::usize             p = swPos;
        std::vector<FoldStep> scanned;   // 近 switch → 远
        while (scanned.size() < dispatch_.maxSteps && p >= 2) {
            const jbc::ir::Insn* binop   = cu.dom.peek(order[p - 1]);
            const jbc::ir::Insn* constIn = cu.dom.peek(order[p - 2]);
            ca::i32              c       = 0;
            if (binop == nullptr || constIn == nullptr) break;
            const bool binopOk = std::find(dispatch_.binops.begin(), dispatch_.binops.end(),
                                           binop->op) != dispatch_.binops.end();
            if (!binopOk || !isIntConstInsn(scope.session, *constIn, c)) break;
            scanned.push_back({c, binop->op});
            p -= 2;
        }
        if (p >= 1) {
            const jbc::ir::Insn* load = cu.dom.peek(order[p - 1]);
            if (load != nullptr && isIntLoadOp(load->op)) {
                iloadPos  = p - 1;
                blockHead = iloadPos;   // 形态 A 块头即装载
                steps.assign(scanned.rbegin(), scanned.rend());
            }
        }
    }
    if (iloadPos == n) {
        // 形态 B：binop 链紧邻 switch、iload 在链下、常量列在 iload 之下。
        ca::usize       p = swPos;
        std::vector<Op> ops;   // 近 switch → 远
        while (ops.size() < dispatch_.maxSteps && p >= 1) {
            const jbc::ir::Insn* binop = cu.dom.peek(order[p - 1]);
            if (binop == nullptr
                || std::find(dispatch_.binops.begin(), dispatch_.binops.end(), binop->op)
                       == dispatch_.binops.end()) {
                break;
            }
            ops.push_back(binop->op);
            --p;
        }
        if (!ops.empty() && p >= 1) {
            const jbc::ir::Insn* load = cu.dom.peek(order[p - 1]);
            if (load != nullptr && isIntLoadOp(load->op)) {
                const ca::usize m = ops.size();
                iloadPos = p - 1;
                if (iloadPos < m) return abandon();
                bool ok = true;
                for (ca::usize j = 0; j < m && ok; ++j) {
                    const jbc::ir::Insn* constIn = cu.dom.peek(order[iloadPos - 1 - j]);
                    ca::i32              c       = 0;
                    if (constIn == nullptr || !isIntConstInsn(scope.session, *constIn, c)) {
                        ok = false;
                        break;
                    }
                    steps.push_back({c, ops[m - 1 - j]});
                }
                if (ok) {
                    blockHead = iloadPos - m;
                    mirrored  = true;
                } else {
                    steps.clear();
                }
            }
        }
    }
    if (iloadPos == n || blockHead == 0) return abandon();   // 无形态 / 入口边不可静态解析
    const ca::u32 slot = slotOfIntLoad(*cu.dom.peek(order[iloadPos]));

    // ── 引擎步 3：状态槽写点分类（绝对 K / 相对 s += D）+ 读点审计。──
    //    写点先分类：形态表命中 = 绝对 K；否则尝试相对更新形态（[iload s;
    //    intConst D; binop; istore s] 与 iinc 紧凑形）；两者皆不命中 = 不可
    //    静态求值的前驱，整族拒收。iinc 是 s += D 的紧凑形，读改写建模为
    //    相对写（原先的整族禁用解除，闭环交由相对模式的覆盖审计承担）。
    std::vector<StateWrite> writes;
    std::vector<ca::usize>  updateReads;   // 相对更新的 s 读点（审计豁免）
    bool                    anyRelative = false;
    for (ca::usize i = 0; i < n; ++i) {
        const jbc::ir::Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) continue;
        if (isIntStoreOp(in->op) && slotOfIntStore(*in) == slot) {
            StateWrite w;
            w.pos     = i;
            bool   matched = false;
            ca::i32 k      = 0;
            for (const PredFormFn& form : predForms_) {
                k = 0;
                if (form != nullptr && form(scope.session, cu.dom, order, i, k)) {
                    matched = true;
                    break;
                }
            }
            if (matched) {
                w.k = k;
            } else {
                RelUpdate ru;
                ca::usize readPos = 0;
                if (!matchRelUpdate(scope.session, cu.dom, order, i, slot, ru, readPos)) {
                    return abandon();
                }
                w.rel    = true;
                w.relupd = ru;
                updateReads.push_back(readPos);
                anyRelative = true;
            }
            writes.push_back(w);
            continue;
        }
        if (in->op == Op::IINC && static_cast<ca::u32>(in->a) == slot) {
            StateWrite w;
            w.pos    = i;
            w.rel    = true;
            w.relupd = RelUpdate{Op::IADD, in->b};
            writes.push_back(w);
            anyRelative = true;
            continue;
        }
        // 两槽值（long/double）高半槽是隐式覆盖：不在引擎建模面。
        ca::u32 base = 0;
        if (twoSlotLocalBase(*in, base) && base + 1 == slot) return abandon();
    }
    if (writes.empty()) return abandon();

    // 读点审计：分发装载之外，相对更新的 s 装载（s op D 的操作数）是读改
    // 写的一部分，按登记位置豁免；其余读点 = 被读作他用，整族拒收。
    for (ca::usize i = 0; i < n; ++i) {
        const jbc::ir::Insn* in = cu.dom.peek(order[i]);
        if (in == nullptr) continue;
        if (isIntLoadOp(in->op) && slotOfIntLoad(*in) == slot) {
            if (i == iloadPos) continue;
            if (std::find(updateReads.begin(), updateReads.end(), i) != updateReads.end()) {
                continue;
            }
            return abandon();
        }
    }

    // 分发折叠：应用序复合——形态 A 常量在右 v = v op C（v 从 K 起），形态
    // B 常量在左 v = C op v。u32 环绕，无符号 UB；op 不在规格内拒收。
    const auto foldDispatch = [&](ca::i32 k) -> std::optional<ca::i32> {
        ca::u32 v = static_cast<ca::u32>(k);
        for (const FoldStep& st : steps) {
            const ca::u32 c = static_cast<ca::u32>(st.c);
            if (st.op == Op::IADD) {
                v = v + c;
            } else if (st.op == Op::ISUB) {
                v = mirrored ? c - v : v - c;
            } else if (st.op == Op::IXOR) {
                v = v ^ c;
            } else {
                return std::nullopt;
            }
        }
        return static_cast<ca::i32>(v);
    };

    // 相对更新求值：s' = s op D（u32 环绕与分发折叠同口径）。
    const auto applyRel = [](const RelUpdate& ru, ca::i32 prev) -> ca::i32 {
        const ca::u32 s = static_cast<ca::u32>(prev);
        const ca::u32 d = static_cast<ca::u32>(ru.d);
        ca::u32       v = s;
        if (ru.op == Op::IADD) {
            v = s + d;
        } else if (ru.op == Op::ISUB) {
            v = s - d;
        } else if (ru.op == Op::IXOR) {
            v = s ^ d;
        }
        return static_cast<ca::i32>(v);
    };

    // ── 引擎步 3b：相对模式状态迹模拟（s += D 分发形态的闭环求解）。
    //    绝对写点自解析；相对写点的当前值由「入口锚 → 案表 → 直线供源写
    //    点」的状态迹模拟定值：
    //      案表供源：每个案表目标（含 default）从目标起直线扫描到首个状态
    //        写点；控制转移/分发块头/方法尾 = 无直线供源（终态块）。
    //      入口锚：不被任何案表供源的绝对写点，唯一才可锚定（入口边必经
    //        方法头侧进入，不取道分发块）。
    //      迹：v = fold(s) → 案表目标 → 供源写点 w → s' = valueAfter(w)
    //        （绝对 = K，相对 = s op D）；同写点双值（路径敏感）→ 拒收；
    //        环闭合即停。
    //    模拟后任一相对写点未定值 → 拒收（不可静态求解的前驱）。
    std::vector<ca::i32> assigned(writes.size(), 0);
    std::vector<char>    hasValue(writes.size(), 0);
    if (anyRelative) {
        const auto straightFeed = [&](LabelId l) -> std::optional<ca::usize> {
            auto resolved = cu.dom.resolveLabel(l);
            if (resolved.is_err()) return std::nullopt;
            const jbc::ir::LabelTarget t = std::move(resolved).unwrap();
            if (t.isEnd()) return std::nullopt;
            for (ca::usize p = posOfInsn(order, t.insn); p < n; ++p) {
                if (p == blockHead) return std::nullopt;
                const jbc::ir::Insn* in = cu.dom.peek(order[p]);
                if (in == nullptr) return std::nullopt;
                for (ca::usize j = 0; j < writes.size(); ++j) {
                    if (writes[j].pos == p) return j;
                }
                if (isControlTransfer(in->op)) return std::nullopt;
            }
            return std::nullopt;
        };

        // 案表目标 → 供源写点（无直线供源的终态块不入表）。
        std::unordered_map<ca::u32, ca::usize> feedOfTarget;
        std::vector<char>                      fedByCase(writes.size(), 0);
        const auto noteCase = [&](LabelId l) {
            if (l == jbc::ir::kNoLabel) return;
            const std::optional<ca::usize> feed = straightFeed(l);
            if (!feed.has_value()) return;
            auto resolved = cu.dom.resolveLabel(l);
            if (resolved.is_err()) return;
            const jbc::ir::LabelTarget t = std::move(resolved).unwrap();
            if (t.isEnd()) return;
            feedOfTarget.emplace(t.insn.slot, *feed);
            fedByCase[*feed] = 1;
        };
        noteCase(sw->target);
        for (const LabelId l : sw->targets) noteCase(l);

        // 入口锚：不被案表供源的绝对写点，唯一。
        ca::i64 entry = -1;
        for (ca::usize j = 0; j < writes.size(); ++j) {
            if (writes[j].rel || fedByCase[j] != 0) continue;
            if (entry >= 0) return abandon();
            entry = static_cast<ca::i64>(j);
        }
        if (entry < 0) return abandon();

        hasValue[static_cast<ca::usize>(entry)] = 1;
        assigned[static_cast<ca::usize>(entry)] = writes[static_cast<ca::usize>(entry)].k;
        ca::usize cur = static_cast<ca::usize>(entry);
        for (ca::usize bound = 0; bound <= writes.size(); ++bound) {
            const std::optional<ca::i32> v = foldDispatch(assigned[cur]);
            if (!v.has_value()) return abandon();
            const LabelId caseLabel = method_rewrite_util::switchCaseLabelOf(*sw, *v);
            if (caseLabel == jbc::ir::kNoLabel) return abandon();
            auto resolved = cu.dom.resolveLabel(caseLabel);
            if (resolved.is_err()) return abandon();
            const jbc::ir::LabelTarget t = std::move(resolved).unwrap();
            if (t.isEnd()) break;   // 迹终止：案表指向方法尾
            const auto it = feedOfTarget.find(t.insn.slot);
            if (it == feedOfTarget.end()) break;   // 终态块：迹结束
            const ca::usize next = it->second;
            const ca::i32   nv   = writes[next].rel
                                       ? applyRel(writes[next].relupd, assigned[cur])
                                       : writes[next].k;
            if (hasValue[next] != 0) {
                if (assigned[next] != nv) return abandon();   // 同写点双值：路径敏感
                break;                                        // 环闭合
            }
            hasValue[next] = 1;
            assigned[next] = nv;
            cur            = next;
        }
        // 覆盖审计：相对写点必须全部定值。
        for (ca::usize j = 0; j < writes.size(); ++j) {
            if (writes[j].rel && hasValue[j] == 0) return abandon();
        }
    }

    // ── 引擎步 4：逐写点解析分发值并查案表；后继必须 goto 分发块头——
    //    交叠定值/回环断裂整族拒收。绝对写用形态表 K，相对写用模拟定值。──
    struct ResolvedPred
    {
        InsnId  gotoId;
        LabelId newTarget;
    };
    std::vector<ResolvedPred> preds;
    preds.reserve(writes.size());
    for (ca::usize j = 0; j < writes.size(); ++j) {
        const ca::usize w = writes[j].pos;
        if (w + 1 >= n) return abandon();
        const jbc::ir::Insn* gotoIn = cu.dom.peek(order[w + 1]);
        if (gotoIn == nullptr || !isGotoOp(gotoIn->op)) return abandon();
        auto resolved = cu.dom.resolveLabel(gotoIn->target);
        if (resolved.is_err()) return abandon();
        const jbc::ir::LabelTarget t = std::move(resolved).unwrap();
        if (t.isEnd() || posOfInsn(order, t.insn) != blockHead) return abandon();

        const std::optional<ca::i32> v = foldDispatch(writes[j].rel ? assigned[j] : writes[j].k);
        if (!v.has_value()) return abandon();
        const LabelId caseLabel = method_rewrite_util::switchCaseLabelOf(*sw, *v);
        if (caseLabel == jbc::ir::kNoLabel) return abandon();
        preds.push_back({order[w + 1], caseLabel});
    }

    // ── 引擎步 5a：入边守卫——非前驱分支/switch 目标锚入分发块区间
    //    (blockHead, swPos] 或块头本身 → 拒收；switch 案表自引用由同一条
    //    扫描覆盖（switch 不是前驱 goto）。──
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
                if (pos > blockHead && pos <= swPos) return false;   // 锚入区间
                if (pos == blockHead &&
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
        const jbc::ir::Insn* prev = cu.dom.peek(order[blockHead - 1]);
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
