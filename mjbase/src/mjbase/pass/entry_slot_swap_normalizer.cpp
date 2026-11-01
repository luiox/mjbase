#include "mjbase/pass/entry_slot_swap_normalizer.hpp"

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <cstddef>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/local_op.hpp"

namespace mjbase::pass {
namespace {

using jbc::core::Op;
using jbc::ir::Insn;
using jbc::ir::InsnId;

/// 序言上限：观测语料固定 4 条（2 load + 2 store），留余量给更宽的置换。
constexpr ca::usize kMaxPrologueInsns = 16;

struct Plan
{
    std::vector<InsnId>                  prologue;
    std::unordered_map<ca::u32, ca::u32> remap;   ///< 序言后槽位 → 序言前来源槽
    bool                                 alias = false;   ///< 二指令 this 别名形态
};

/// 紧凑局部操作码（槽位编码在 opcode 里，改写槽位必须换成显式形态）。
bool isCompactLocal(Op op) noexcept
{
    const ca::u32 raw = static_cast<ca::u32>(op);
    return (raw >= 0x1a && raw <= 0x2d) || (raw >= 0x3b && raw <= 0x4e);
}

Op explicitOp(char kind, LocalUse use) noexcept
{
    switch (kind) {
    case 'I': return use == LocalUse::Load ? Op::ILOAD : Op::ISTORE;
    case 'L': return use == LocalUse::Load ? Op::LLOAD : Op::LSTORE;
    case 'F': return use == LocalUse::Load ? Op::FLOAD : Op::FSTORE;
    case 'D': return use == LocalUse::Load ? Op::DLOAD : Op::DSTORE;
    default: return use == LocalUse::Load ? Op::ALOAD : Op::ASTORE;
    }
}

/// 形态一：入口「搬移置换」——连续 load 段后接等长 store 段。
std::optional<Plan> detectPermutation(const jbc::ir::CodeUnit& code)
{
    const std::vector<InsnId>& order = code.dom.order();
    if (order.size() < 4) return std::nullopt;

    ca::usize i = 0;
    while (i < order.size() && i < kMaxPrologueInsns) {
        const Insn* in = code.dom.peek(order[i]);
        if (in == nullptr) return std::nullopt;
        if (localOpOf(*in).use != LocalUse::Load) break;
        ++i;
    }
    const ca::usize loadEnd = i;
    if (loadEnd < 2) return std::nullopt;

    while (i < order.size() && i < kMaxPrologueInsns) {
        const Insn* in = code.dom.peek(order[i]);
        if (in == nullptr) return std::nullopt;
        if (localOpOf(*in).use != LocalUse::Store) break;
        ++i;
    }
    const ca::usize prologueEnd = i;
    if (prologueEnd - loadEnd != loadEnd) return std::nullopt;

    // 分支目标/异常表边界落在序言内：删除会跳跃或改变保护区间，保守放弃。
    for (ca::usize k = 0; k < prologueEnd; ++k) {
        if (!code.dom.labelsAt(order[k]).empty()) return std::nullopt;
    }

    // 符号执行：栈元素 = 值的来源槽；prov[槽] = 该槽当前值的来源槽。
    std::unordered_map<ca::u32, ca::u32> prov;
    std::vector<ca::u32>                 stack;
    for (ca::usize k = 0; k < loadEnd; ++k) {
        const Insn*   in = code.dom.peek(order[k]);
        const LocalOp op = localOpOf(*in);
        const ca::u32 w  = localWidth(op.kind);
        prov.try_emplace(op.slot, op.slot);
        if (w == 2) {
            prov.try_emplace(op.slot + 1, op.slot + 1);
            if (prov[op.slot + 1] != prov[op.slot] + 1) return std::nullopt;
        }
        stack.push_back(prov[op.slot]);
    }
    for (ca::usize k = loadEnd; k < prologueEnd; ++k) {
        const Insn*   in = code.dom.peek(order[k]);
        const LocalOp op = localOpOf(*in);
        const ca::u32 w  = localWidth(op.kind);
        if (stack.empty()) return std::nullopt;
        const ca::u32 src = stack.back();
        stack.pop_back();
        prov[op.slot] = src;
        if (w == 2) prov[op.slot + 1] = src + 1;
    }
    if (!stack.empty()) return std::nullopt;

    // 目的槽与来源槽必须是同一集合上的双射；普通「拷到新局部」会失败。
    std::unordered_set<ca::u32> sources;
    bool                         identity = true;
    for (const auto& [slot, src] : prov) {
        if (!sources.insert(src).second) return std::nullopt;
        if (slot != src) identity = false;
    }
    if (identity) return std::nullopt;
    for (const ca::u32 src : sources) {
        if (prov.find(src) == prov.end()) return std::nullopt;
    }

    Plan plan;
    plan.prologue.assign(order.begin(),
                         order.begin() + static_cast<std::ptrdiff_t>(prologueEnd));
    plan.remap = std::move(prov);
    return plan;
}

/// 解析标签到指令序号；方法末尾返回 order.size()。
std::optional<ca::usize>
    labelIndex(const jbc::ir::CodeUnit& code, jbc::ir::LabelId label,
               const std::unordered_map<ca::u32, ca::usize>& bySlot)
{
    const auto resolved = code.dom.resolveLabel(label);
    if (resolved.is_err()) return std::nullopt;
    const jbc::ir::LabelTarget target = resolved.unwrap();
    if (target.isEnd()) return code.dom.order().size();
    const auto it = bySlot.find(target.insn.slot);
    if (it == bySlot.end()) return std::nullopt;
    return it->second;
}

bool isUnconditionalLeave(Op op) noexcept
{
    const ca::u32 raw = static_cast<ca::u32>(op);
    return (raw >= 0xac && raw <= 0xb1) || raw == 0xbf;   // *return / athrow
}

bool isConditionalBranch(Op op) noexcept
{
    const ca::u32 raw = static_cast<ca::u32>(op);
    return (raw >= 0x99 && raw <= 0xa6) || raw == 0xc6 || raw == 0xc7;
}

/// 全路径「slot0 已由方法体写过」判定。删除 this 别名前缀后槽 k 不再保留
/// this 副本，只有每条路径都先写槽 0 再读，0↔k 互换才不丢值。
bool slotZeroWrittenBeforeEveryRead(const jbc::ir::CodeUnit& code, ca::usize bodyStart)
{
    const std::vector<InsnId>& order = code.dom.order();
    const ca::usize            n     = order.size();

    std::unordered_map<ca::u32, ca::usize> bySlot;
    for (ca::usize i = 0; i < n; ++i) {
        const Insn* in = code.dom.peek(order[i]);
        if (in == nullptr) return false;
        bySlot.emplace(order[i].slot, i);
    }

    std::vector<std::vector<ca::usize>> succ(n);
    for (ca::usize i = 0; i < n; ++i) {
        const Insn*   in  = code.dom.peek(order[i]);
        const ca::u32 raw = static_cast<ca::u32>(in->op);
        if (isUnconditionalLeave(in->op)) continue;
        if (raw == 0xa7 || raw == 0xc8) {   // goto / goto_w
            const auto target = labelIndex(code, in->target, bySlot);
            if (!target.has_value() || *target >= n) return false;
            succ[i].push_back(*target);
            continue;
        }
        if (isConditionalBranch(in->op)) {
            const auto target = labelIndex(code, in->target, bySlot);
            if (!target.has_value() || *target >= n) return false;
            succ[i].push_back(*target);
            if (i + 1 < n) succ[i].push_back(i + 1);
            continue;
        }
        if (in->op == Op::TABLESWITCH || in->op == Op::LOOKUPSWITCH) {
            const auto dflt = labelIndex(code, in->target, bySlot);
            if (!dflt.has_value() || *dflt >= n) return false;
            succ[i].push_back(*dflt);
            for (const jbc::ir::LabelId target : in->targets) {
                const auto targetIdx = labelIndex(code, target, bySlot);
                if (!targetIdx.has_value() || *targetIdx >= n) return false;
                succ[i].push_back(*targetIdx);
            }
            continue;
        }
        if (i + 1 < n) succ[i].push_back(i + 1);
    }

    for (const jbc::ir::CodeUnit::Exc& exc : code.exc) {
        const auto start   = labelIndex(code, exc.start, bySlot);
        const auto end     = labelIndex(code, exc.end, bySlot);
        const auto handler = labelIndex(code, exc.handler, bySlot);
        if (!start.has_value() || !end.has_value() || !handler.has_value()) return false;
        if (*handler >= n) return false;
        for (ca::usize i = *start; i < *end && i < n; ++i) {
            succ[i].push_back(*handler);
        }
    }

    std::vector<std::vector<ca::usize>> preds(n);
    for (ca::usize i = 0; i < n; ++i) {
        for (const ca::usize s : succ[i]) preds[s].push_back(i);
    }

    // 前向 must 分析：进入状态从「未写」开始，汇合取 AND；不可达节点保持
    // true（不检查其读取）。
    std::vector<bool> in(n, true);
    std::vector<bool> out(n, true);
    bool              changed = true;
    while (changed) {
        changed = false;
        for (ca::usize i = bodyStart; i < n; ++i) {
            bool incoming = true;
            if (i == bodyStart) {
                incoming = false;
            } else if (preds[i].empty()) {
                incoming = true;
            } else {
                for (const ca::usize p : preds[i]) incoming = incoming && out[p];
            }
            if (in[i] != incoming) {
                in[i]  = incoming;
                changed = true;
            }
            const Insn*   st = code.dom.peek(order[i]);
            const LocalOp op = st != nullptr ? localOpOf(*st) : LocalOp{};
            const bool    written = incoming
                                 || (op.use == LocalUse::Store && op.slot == 0);
            if (out[i] != written) {
                out[i]  = written;
                changed = true;
            }
        }
    }

    for (ca::usize i = bodyStart; i < n; ++i) {
        const Insn*   in_ = code.dom.peek(order[i]);
        const LocalOp op  = in_ != nullptr ? localOpOf(*in_) : LocalOp{};
        if (op.slot == 0 && op.use == LocalUse::Load && !in[i]) return false;
    }
    return true;
}

/// 同槽混用引用与基本类型：Strict 帧在异常边/汇合点会把这类槽判成类型冲突
/// （原类靠自带 SMT 或旧校验器容忍）。本 pass 不修这类槽，别名还原会触发
/// 帧重算失败，保守跳过。
bool hasCrossKindSlotReuse(const jbc::ir::CodeUnit& code)
{
    std::unordered_map<ca::u32, ca::u8> kinds;
    for (const InsnId id : code.dom.order()) {
        const Insn* in = code.dom.peek(id);
        if (in == nullptr) return true;
        const LocalOp op = localOpOf(*in);
        if (op.use == LocalUse::None) continue;
        const ca::u8 bit = op.kind == 'A' ? 1u : 2u;
        const ca::u8 prev = kinds[op.slot];
        if (prev != 0 && prev != bit) return true;
        kinds[op.slot] = static_cast<ca::u8>(prev | bit);
        if (localWidth(op.kind) == 2) {
            const ca::u8 prevHi = kinds[op.slot + 1];
            if (prevHi != 0 && prevHi != bit) return true;
            kinds[op.slot + 1] = static_cast<ca::u8>(prevHi | bit);
        }
    }
    return false;
}

/// 形态二：`load 0; store k` 两指令前缀——先保存 this（或静态方法的首个
/// 引用参数），随后 slot 0 被复用为局部变量。
///
/// 这不是普通拷贝：方法体里 slot k 承担 this 的读取，slot 0 承担原局部变量，
/// 等价于整个方法只对 0 与 k 做了置换。删除前缀并把两条槽互换即可还原；
/// 删除后 slot k 不再持有 this 副本，因此要求方法体对 slot 0 的首次访问
/// 必须是写（否则会读到一个已不存在的 this 副本）。
std::optional<Plan> detectThisAlias(const jbc::ir::CodeUnit& code)
{
    const std::vector<InsnId>& order = code.dom.order();
    if (order.size() < 3) return std::nullopt;

    const Insn* first = code.dom.peek(order[0]);
    const Insn* second = code.dom.peek(order[1]);
    if (first == nullptr || second == nullptr) return std::nullopt;
    const LocalOp load = localOpOf(*first);
    const LocalOp store = localOpOf(*second);
    if (load.use != LocalUse::Load || load.kind != 'A' || load.slot != 0) {
        return std::nullopt;
    }
    if (store.use != LocalUse::Store || store.kind != 'A' || store.slot == 0) {
        return std::nullopt;
    }
    const ca::u32 aliasSlot = store.slot;

    for (ca::usize k = 0; k < 2; ++k) {
        if (!code.dom.labelsAt(order[k]).empty()) return std::nullopt;
    }
    for (const InsnId id : order) {
        const Insn* in = code.dom.peek(id);
        if (in == nullptr) return std::nullopt;
        if (in->op == Op::JSR || in->op == Op::JSR_W || in->op == Op::RET) {
            return std::nullopt;
        }
    }
    if (hasCrossKindSlotReuse(code)) return std::nullopt;

    bool hasSlotZeroAccess = false;
    for (ca::usize i = 2; i < order.size(); ++i) {
        const Insn* in = code.dom.peek(order[i]);
        if (in == nullptr) return std::nullopt;
        const LocalOp op = localOpOf(*in);
        if (op.use == LocalUse::None) continue;

        if (op.slot == 0 || op.slot == aliasSlot) {
            // 置换只搬单槽引用值；int/long/double 占用或 iinc 原地读写都不认。
            if (op.kind != 'A') return std::nullopt;
            if (op.slot == 0) hasSlotZeroAccess = true;
        }
        // 宽值以 [slot, slot+1] 占槽，任一半落在 aliasSlot 就不能互换。
        if ((op.kind == 'L' || op.kind == 'D')
            && (op.slot == aliasSlot || op.slot + 1 == aliasSlot)) {
            return std::nullopt;
        }
    }

    // 方法体读槽 0 之前必须每条路径都已写过槽 0；否则读到的 this 副本在
    // 删除前缀后不再存在于槽 k。
    if (hasSlotZeroAccess && !slotZeroWrittenBeforeEveryRead(code, 2)) {
        return std::nullopt;
    }

    Plan plan;
    plan.prologue.assign(order.begin(), order.begin() + 2);
    plan.remap = {{0, aliasSlot}, {aliasSlot, 0}};
    plan.alias = true;
    return plan;
}

std::optional<Plan> detect(const jbc::ir::CodeUnit& code)
{
    if (auto plan = detectPermutation(code); plan.has_value()) return plan;
    return detectThisAlias(code);
}

struct Rewrite
{
    InsnId  id;
    Op      op;
    ca::u32 slot;
    bool    wide;
};

/// 序言之后的局部访问 → 规范化槽位。宽值两半必须同向映射；返回 false 表示
/// 代码形态与置换不一致（放弃整方法）。
bool collectRewrites(const jbc::ir::CodeUnit& code, const Plan& plan,
                     std::vector<Rewrite>& out)
{
    const std::vector<InsnId>& order = code.dom.order();
    for (ca::usize k = plan.prologue.size(); k < order.size(); ++k) {
        const Insn* in = code.dom.peek(order[k]);
        if (in == nullptr) return false;
        const LocalOp op = localOpOf(*in);
        if (op.use == LocalUse::None) continue;

        const auto it = plan.remap.find(op.slot);
        if (it == plan.remap.end()) continue;
        const ca::u32 target = it->second;
        if (target == op.slot && !isCompactLocal(in->op)) continue;

        if (op.use != LocalUse::Inc) {
            const ca::u32 w = localWidth(op.kind);
            if (w == 2) {
                const auto hi = plan.remap.find(op.slot + 1);
                if (hi == plan.remap.end() || hi->second != target + 1) return false;
            }
        }

        Rewrite rw;
        rw.id   = order[k];
        rw.op   = op.use == LocalUse::Inc ? Op::IINC : explicitOp(op.kind, op.use);
        rw.slot = target;
        rw.wide = in->wide || target > 0xFF;
        out.push_back(rw);
    }
    return true;
}

}   // namespace

mjbase::spi::PreservedAnalyses EntrySlotSwapNormalizer::run(
    mjbase::spi::MethodScope& scope, mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;

    const std::optional<Plan> plan = detect(scope.code);
    if (!plan.has_value()) return mjbase::spi::PreservedAnalyses::all();

    std::vector<Rewrite> rewrites;
    if (!collectRewrites(scope.code, *plan, rewrites)) {
        return mjbase::spi::PreservedAnalyses::all();
    }

    jbc::ir::CodeEditor editor(scope.code);
    if (editor.eraseBatch(plan->prologue,
                          jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
            .is_err()) {
        ++stats_.skippedErase;
        return mjbase::spi::PreservedAnalyses::all();
    }

    // 序言删除不影响方法体内指令的句柄；改写前已全量校验。
    for (const Rewrite& rw : rewrites) {
        auto got = scope.code.dom.get(rw.id);
        if (got.is_err()) continue;
        Insn* in = got.unwrap();
        in->op   = rw.op;
        in->a    = static_cast<ca::i32>(rw.slot);
        in->wide = rw.wide;
    }

    ++stats_.normalized;
    if (plan->alias) ++stats_.normalizedAliases;
    return mjbase::spi::PreservedAnalyses::none();
}

}   // namespace mjbase::pass
