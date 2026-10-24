/// @file        pattern_matcher.cpp
/// @brief       窗口匹配扫描与原子替换落盘。
#include <unordered_map>
#include <utility>

#include "pattern_matcher.hpp"

namespace mjbase::asm_matcher {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

/// 镜像 CodeDom::checkHandles 的公开面：替换指令若带分支/switch 操作数，
/// 目标标签必须属于本 DOM 且锚定到存活指令，switch 元数须自洽。
Result<void, MatcherError> validateReplacement(const jbc::ir::CodeDom& dom,
                                               const jbc::ir::Insn& in)
{
    using jbc::core::Shape;
    const Shape sh = jbc::core::shapeOf(in.op);
    const bool isBranch = sh == Shape::Branch2 || sh == Shape::Branch4;
    const bool isSwitch = sh == Shape::TableSwitch || sh == Shape::LookupSwitch;
    const auto validTarget = [&](jbc::ir::LabelId label) {
        if (!dom.validLabel(label)) return false;
        auto t = dom.resolveLabel(label);
        return t.is_ok() && !t.unwrap().isEnd();
    };
    const MatcherError rejected{MatchError::EditRejected, 0};
    if ((isBranch || isSwitch) && !validTarget(in.target)) return Err(rejected);
    if (sh == Shape::TableSwitch) {
        if (in.b < in.a) return Err(rejected);
        const ca::u64 count =
            static_cast<ca::u64>(static_cast<ca::i64>(in.b) - static_cast<ca::i64>(in.a)) + 1;
        if (count != in.targets.size()) return Err(rejected);
    } else if (sh == Shape::LookupSwitch && in.keys.size() != in.targets.size()) {
        return Err(rejected);
    }
    if (isSwitch) {
        for (const jbc::ir::LabelId& t : in.targets) {
            if (!validTarget(t)) return Err(rejected);
        }
    }
    return Ok();
}

/// @brief 落盘一个已匹配窗口的替换。
///
/// 替换非空：覆写窗口首条（保留首边界标签语义，label 留在窗口
/// 首位），擦除窗口其余指令，链式插入剩余替换。替换为空：整窗擦除；首条
/// 若仍被窗口外分支引用，先把引用重定向到顺序后继（label 落点语义）。
/// 任一步被 DOM 拒绝即回滚：恢复首条原值、按原顺序重插已擦除指令
/// （含原 srcPc/wide 全字段），返回 Err(EditRejected)。
Result<void, MatcherError> applyWindow(jbc::ir::CodeDom& dom, MatchContext& ctx)
{
    for (jbc::ir::Insn& r : ctx.replacement) r.srcPc = jbc::ir::kNoPc;
    for (const jbc::ir::Insn& r : ctx.replacement) {
        auto v = validateReplacement(dom, r);
        if (v.is_err()) return v;
    }

    // 窗口视图快照：回滚时 matchedIds 已因擦除失效，先拷值。
    const std::vector<jbc::ir::InsnId> ids = ctx.matchedIds;
    const std::vector<jbc::ir::Insn> originals = ctx.matchedInsns;
    const jbc::ir::InsnId firstId = ids.front();
    const bool hasNext = ctx.endIndex < dom.order().size();
    const jbc::ir::InsnId nextId =
        hasNext ? dom.order()[ctx.endIndex] : jbc::ir::kNoInsn;

    // 把 originals[fromIdx..] 按原顺序放回窗口尾部位置，并恢复首条覆写。
    const auto rollback = [&](ca::usize fromIdx) -> MatcherError {
        if (!ctx.replacement.empty()) {
            auto got = dom.get(firstId);
            if (got.is_ok()) *got.unwrap() = originals.front();
        }
        for (ca::usize k = fromIdx; k < ids.size(); ++k) {
            auto ins = hasNext ? dom.insertBefore(nextId, originals[k])
                               : dom.append(originals[k]);
            if (ins.is_err()) break;   // 尽力恢复；错误通道由返回值承载
            auto got = dom.get(ins.unwrap());
            if (got.is_ok()) *got.unwrap() = originals[k];   // 恢复 srcPc/wide
        }
        return MatcherError{MatchError::EditRejected, 0};
    };

    // 从最后一条倒序擦除窗口内除首条外的指令；失败即回滚已擦除的后缀。
    const auto eraseTail = [&]() -> Result<void, MatcherError> {
        for (ca::usize k = ids.size(); k > 1;) {
            --k;
            auto er = dom.erase(ids[k]);
            if (er.is_err()) return Err(rollback(k + 1));
        }
        return Ok();
    };

    if (!ctx.replacement.empty()) {
        auto got = dom.get(firstId);
        if (got.is_err()) return Err(MatcherError{MatchError::EditRejected, 0});
        *got.unwrap() = ctx.replacement.front();
        auto tail = eraseTail();
        if (tail.is_err()) return tail;

        jbc::ir::InsnId cursor = firstId;
        std::vector<jbc::ir::InsnId> inserted;
        for (ca::usize r = 1; r < ctx.replacement.size(); ++r) {
            auto ins = dom.insertAfter(cursor, ctx.replacement[r]);
            if (ins.is_err()) {
                for (const jbc::ir::InsnId& nid : inserted) (void)dom.erase(nid);
                return Err(rollback(1));
            }
            cursor = ins.unwrap();
            inserted.push_back(cursor);
        }
        return Ok();
    }

    auto tail = eraseTail();
    if (tail.is_err()) return tail;
    if (hasNext && !dom.usersOf(firstId).empty()) {
        auto rr = dom.replaceAllUses(firstId, nextId);
        if (rr.is_err()) return Err(rollback(1));
    }
    auto er = dom.erase(firstId);
    if (er.is_err()) return Err(rollback(1));   // Boundary 标签锚定：交调用方先移标签
    return Ok();
}

}   // namespace

std::vector<ca::u8> jumpTargetMask(const jbc::ir::CodeDom& dom)
{
    using jbc::core::Shape;
    const std::vector<jbc::ir::InsnId>& order = dom.order();
    std::vector<ca::u8> mask(order.size(), 0);
    std::unordered_map<ca::u32, ca::usize> indexOfSlot;
    indexOfSlot.reserve(order.size() * 2);
    for (ca::usize i = 0; i < order.size(); ++i) indexOfSlot.emplace(order[i].slot, i);

    const auto mark = [&](jbc::ir::LabelId label) {
        auto t = dom.resolveLabel(label);
        if (t.is_err()) return;
        const jbc::ir::InsnId anchor = t.unwrap().insn;
        if (anchor == jbc::ir::kNoInsn) return;
        const auto it = indexOfSlot.find(anchor.slot);
        if (it != indexOfSlot.end()) mask[it->second] = 1;
    };
    for (const jbc::ir::InsnId& id : order) {
        const jbc::ir::Insn* in = dom.peek(id);
        if (in == nullptr) continue;
        const Shape sh = jbc::core::shapeOf(in->op);
        const bool isBranch = sh == Shape::Branch2 || sh == Shape::Branch4;
        const bool isSwitch = sh == Shape::TableSwitch || sh == Shape::LookupSwitch;
        if (isBranch || isSwitch) mark(in->target);
        if (isSwitch) {
            for (const jbc::ir::LabelId& t : in->targets) mark(t);
        }
    }
    return mask;
}

bool MatchRule::tryMatch(const jbc::ir::CodeDom& dom, ca::usize startIndex,
                         MatchContext& ctx) const
{
    return tryMatchWithMask(dom, startIndex, ctx, jumpTargetMask(dom));
}

bool MatchRule::tryMatchWithMask(const jbc::ir::CodeDom& dom, ca::usize startIndex,
                                 MatchContext& ctx, const std::vector<ca::u8>& targetMask) const
{
    ctx.matchedInsns.clear();
    ctx.matchedIds.clear();
    ctx.captures.clear();

    if (steps_.empty()) return false;
    const std::vector<jbc::ir::InsnId>& order = dom.order();
    if (startIndex >= order.size()) return false;

    ca::usize offset = 0;
    for (const MatchStep& step : steps_) {
        const ca::usize pos = startIndex + offset;
        if (pos >= order.size()) return false;
        const jbc::ir::Insn* in = dom.peek(order[pos]);
        if (in == nullptr) return false;
        if (!step.tryMatch(*in, order.size() - pos)) return false;
        ctx.matchedInsns.push_back(*in);
        ctx.matchedIds.push_back(order[pos]);
        if (step.hasCapture()) ctx.captures[step.capture()] = CapturedInsn{pos, *in};
        offset += step.matchedSize();
    }

    ctx.original = &dom;
    ctx.startIndex = startIndex;
    ctx.endIndex = startIndex + offset;

    // 窗口纪律（首条豁免）：控制流不得从窗口中部进入——
    // 「匹配不跨越 Label 边界」的可检测部分。
    for (ca::usize p = startIndex + 1; p < ctx.endIndex; ++p) {
        if (targetMask[p] != 0) return false;
    }
    return true;
}

std::vector<MatchResult> PatternMatcher::match(const jbc::ir::CodeDom& dom) const
{
    std::vector<MatchResult> results(rules_.size());
    if (rules_.empty()) return results;
    const std::vector<ca::u8> mask = jumpTargetMask(dom);
    const ca::usize n = dom.order().size();
    for (ca::usize ri = 0; ri < rules_.size(); ++ri) {
        MatchContext ctx;
        for (ca::usize i = 0; i < n; ++i) {
            if (rules_[ri].tryMatchWithMask(dom, i, ctx, mask)) {
                results[ri] = MatchResult{ctx.startIndex, ctx.endIndex};
                break;
            }
        }
    }
    return results;
}

Result<bool, MatcherError> PatternMatcher::apply(jbc::ir::CodeDom& dom) const
{
    bool anyChanged = false;
    for (const MatchRule& rule : rules_) {
        // 规则可能改动序列与目标布局，窗口纪律数据逐规则重建。
        const std::vector<ca::u8> mask = jumpTargetMask(dom);
        const ca::usize n = dom.order().size();
        for (ca::usize u = n; u-- > 0;) {
            MatchContext ctx;
            if (!rule.tryMatchWithMask(dom, u, ctx, mask)) continue;
            if (!rule.hasStrategy()) break;   // 仅匹配规则：止扫，不改动（照旧）

            ctx.resetReplacementDecision();
            ctx.replacement.clear();
            Result<void, MatcherError> sr = rule.strategy()(ctx);
            if (sr.is_err()) return Err(sr.unwrap_err());
            if (ctx.isReplacementCancelled()) {
                ctx.replacement.clear();
                continue;
            }

            Result<void, MatcherError> applied = applyWindow(dom, ctx);
            if (applied.is_err()) return Err(applied.unwrap_err());
            anyChanged = true;
            break;   // 每条规则每次调用至多替换一处（照旧）
        }
    }
    return Ok(anyChanged);
}

}   // namespace mjbase::asm_matcher
