/// @file        combinators.hpp
/// @brief       组合子库：叶子匹配节点 + 序列组合子 + 捕获。
///
/// 语义对齐 asm/tmatcher 的 combinators.hpp，按新底座裁剪：
///   - 叶子按 `jbc::core::Op` 与操作数条件（cp 索引 / local 索引 /
///     立即数 / 分支目标标签解析结果）组合成谓词。
///   - 组合子覆盖任务清单：seq / any（任意单条）/ opt / repeat（贪婪与
///     懒惰）/ alt；旧 and_/not_/guard 由通用叶子谓词覆盖（谓词签名携带
///     捕获表与 DOM，可表达守卫与否定条件），不再单列节点。
///   - 捕获绑定「捕获名 → 窗口入口指令句柄」（InsnId），提取操作数是
///     调用方经 CodeDom::peek 的事。
///   - 候选排序即优先级：贪婪降序、懒惰升序；与旧实现的差异见各步骤
///     注释。
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

#include "match_state.hpp"
#include "match_step.hpp"
#include "match_tracer.hpp"

namespace mjbase::asm_tmatcher {

// ============================================================================
// 原子 Step：单指令匹配
// ============================================================================

/// @brief 通用叶子 step：谓词判定当前指令。所有具体原子 step 的基类。
class LeafStep : public IMatchStep
{
    std::string   name_;
    InsnPredicate pred_;

public:
    LeafStep(std::string name, InsnPredicate pred)
        : name_(std::move(name))
        , pred_(std::move(pred))
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(const jbc::ir::CodeDom& dom,
                                  const MatchState&       state,
                                  MatchTracer&            tracer,
                                  ca::i32                 depth) override
    {
        tracer.enter(name_, depth, state.cursor);
        InsnCursorView view{&dom, state.cursor, nullptr};
        if (state.cursor >= 0 && state.cursor < static_cast<ca::i32>(dom.size())) {
            view.insn = dom.peek(dom.order()[static_cast<ca::usize>(state.cursor)]);
        }
        if (!view.valid() || !pred_(view, state.captures)) {
            tracer.fail(name_, depth, state.cursor, "predicate failed");
            return {};
        }
        const MatchState next = state.withCursor(state.cursor + 1);
        tracer.success(name_, depth, state.cursor, next.cursor);
        return {next};
    }
};

/// @brief 匹配指定 opcode 的单条指令。
class OpcodeStep : public IMatchStep
{
    jbc::core::Op op_;
    std::string   name_;

public:
    explicit OpcodeStep(jbc::core::Op op)
        : op_(op)
        , name_(std::string("opcode(") + jbc::core::mnemonicOf(op) + ")")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(const jbc::ir::CodeDom& dom,
                                  const MatchState&       state,
                                  MatchTracer&            tracer,
                                  ca::i32                 depth) override
    {
        tracer.enter(name_, depth, state.cursor);
        if (state.cursor < 0 || state.cursor >= static_cast<ca::i32>(dom.size())) {
            tracer.fail(name_, depth, state.cursor, "cursor out of bounds");
            return {};
        }
        const jbc::ir::Insn* now = dom.peek(dom.order()[static_cast<ca::usize>(state.cursor)]);
        if (now == nullptr || now->op != op_) {
            tracer.fail(name_, depth, state.cursor, "opcode mismatch");
            return {};
        }
        const MatchState next = state.withCursor(state.cursor + 1);
        tracer.success(name_, depth, state.cursor, next.cursor);
        return {next};
    }
};

/// @brief 匹配任意一条指令（任务清单中的 any：任意单条）。
class AnyOneStep : public IMatchStep
{
public:
    std::string name() const override { return "any"; }

    std::vector<MatchState> match(const jbc::ir::CodeDom& dom,
                                  const MatchState&       state,
                                  MatchTracer&            tracer,
                                  ca::i32                 depth) override
    {
        tracer.enter(name(), depth, state.cursor);
        if (state.cursor < 0 || state.cursor >= static_cast<ca::i32>(dom.size())) {
            tracer.fail(name(), depth, state.cursor, "cursor out of bounds");
            return {};
        }
        const MatchState next = state.withCursor(state.cursor + 1);
        tracer.success(name(), depth, state.cursor, next.cursor);
        return {next};
    }
};

/// @brief 永不匹配的 step：非法构造参数（如 repeat 区间不合法）的显式失败态。
///
/// 旧实现在构造函数抛异常；新约定不抛错，改为「构造成功、匹配恒败」，
/// 失败原因写进 name() 与 trace detail，调用方可观察。
class NeverMatchStep : public IMatchStep
{
    std::string name_;

public:
    explicit NeverMatchStep(std::string name)
        : name_(std::move(name))
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(const jbc::ir::CodeDom& /*dom*/,
                                  const MatchState&       state,
                                  MatchTracer&            tracer,
                                  ca::i32                 depth) override
    {
        tracer.enter(name_, depth, state.cursor, "invalid pattern");
        tracer.fail(name_, depth, state.cursor, "invalid pattern");
        return {};
    }
};

// ============================================================================
// 序列组合子：seq / alt / opt / repeat
// ============================================================================

/// @brief 顺序组合：依次匹配所有子 step，前一个的候选状态喂给后一个。
///
/// 候选保持子 step 的优先级排列（不做全局排序）：贪婪子树自然按游标降序
/// 传播，懒惰子树保持升序；Longest 裁决由引擎的 findWinner 完成。
class SeqStep : public IMatchStep
{
    std::vector<StepPtr> nodes_;

public:
    explicit SeqStep(std::vector<StepPtr> nodes)
        : nodes_(std::move(nodes))
    {}

    std::string name() const override { return "seq"; }

    std::vector<MatchState> match(const jbc::ir::CodeDom& dom,
                                  const MatchState&       state,
                                  MatchTracer&            tracer,
                                  ca::i32                 depth) override
    {
        tracer.enter(name(), depth, state.cursor,
                     "size=" + std::to_string(nodes_.size()));
        std::vector<MatchState> states = {state};
        for (std::size_t i = 0; i < nodes_.size(); i++) {
            std::vector<MatchState> next;
            for (const auto& s : states) {
                auto branch = nodes_[i]->match(dom, s, tracer, depth + 1);
                next.insert(next.end(),
                            std::make_move_iterator(branch.begin()),
                            std::make_move_iterator(branch.end()));
            }
            if (next.empty()) {
                tracer.fail(name(), depth, state.cursor,
                            "child " + std::to_string(i) + " failed");
                return {};
            }
            states = std::move(next);
        }
        tracer.success(name(), depth, state.cursor, states.front().cursor,
                       "candidates=" + std::to_string(states.size()));
        return states;
    }
};

/// @brief 选择组合（任务清单中的 alt）：任一子 step 成功即成功，合并所有候选。
///
/// 候选按分支注册顺序拼接（不排序）：Longest 裁决在引擎层。
class AltStep : public IMatchStep
{
    std::vector<StepPtr> branches_;

public:
    explicit AltStep(std::vector<StepPtr> branches)
        : branches_(std::move(branches))
    {}

    std::string name() const override { return "alt"; }

    std::vector<MatchState> match(const jbc::ir::CodeDom& dom,
                                  const MatchState&       state,
                                  MatchTracer&            tracer,
                                  ca::i32                 depth) override
    {
        tracer.enter(name(), depth, state.cursor,
                     "branches=" + std::to_string(branches_.size()));
        std::vector<MatchState> results;
        for (auto& branch : branches_) {
            auto matched = branch->match(dom, state, tracer, depth + 1);
            results.insert(results.end(),
                           std::make_move_iterator(matched.begin()),
                           std::make_move_iterator(matched.end()));
        }
        if (results.empty()) {
            tracer.fail(name(), depth, state.cursor, "all branches failed");
            return {};
        }
        tracer.success(name(), depth, state.cursor, results.front().cursor,
                       "candidates=" + std::to_string(results.size()));
        return results;
    }
};

/// @brief 可选组合（任务清单中的 opt）：匹配 0 或 1 次，贪婪优先。
///
/// 等价 repeat(node, 0, 1) 的贪婪形态；需要懒惰语义时用
/// `repeatLazy(node, 0, 1)`。候选按游标降序（先尝试匹配，再回退不匹配）。
class OptionalStep : public IMatchStep
{
    StepPtr node_;

public:
    explicit OptionalStep(StepPtr node)
        : node_(std::move(node))
    {}

    std::string name() const override { return "opt"; }

    std::vector<MatchState> match(const jbc::ir::CodeDom& dom,
                                  const MatchState&       state,
                                  MatchTracer&            tracer,
                                  ca::i32                 depth) override
    {
        tracer.enter(name(), depth, state.cursor);
        auto matched = node_->match(dom, state, tracer, depth + 1);
        matched.push_back(state);
        std::sort(matched.begin(), matched.end(),
                  [](const MatchState& a, const MatchState& b) { return a.cursor > b.cursor; });
        tracer.success(name(), depth, state.cursor, matched.front().cursor);
        return matched;
    }
};

/// @brief 重复组合（任务清单中的 repeat）：重复匹配 [min, max] 次。
///
/// max < 0 表示无上限。候选排列即优先级：
///   - 贪婪（greedy=true）：游标降序，最长优先；
///   - 懒惰（greedy=false）：游标升序，最短优先。
/// 子 step 必须推进游标（零宽循环体视为该候选失败），防死循环。
class RepeatStep : public IMatchStep
{
    StepPtr   node_;
    ca::i32   min_;
    ca::i32   max_;      ///< < 0 表示无上限
    bool      greedy_;
    std::string name_;

public:
    RepeatStep(StepPtr node, ca::i32 min, ca::i32 max, bool greedy)
        : node_(std::move(node))
        , min_(min)
        , max_(max)
        , greedy_(greedy)
        , name_("repeat(min=" + std::to_string(min) + ",max=" +
                (max < 0 ? std::string("inf") : std::to_string(max)) +
                (greedy ? "" : ",lazy") + ")")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(const jbc::ir::CodeDom& dom,
                                  const MatchState&       state,
                                  MatchTracer&            tracer,
                                  ca::i32                 depth) override
    {
        tracer.enter(name_, depth, state.cursor);
        std::vector<MatchState> accepted;
        std::vector<MatchState> current = {state};
        if (min_ == 0) {
            accepted.push_back(state);
        }
        const ca::i32 upperBound =
            max_ < 0 ? static_cast<ca::i32>(dom.size()) - state.cursor : max_;
        for (ca::i32 count = 1; count <= upperBound; count++) {
            std::vector<MatchState> next;
            for (const auto& candidate : current) {
                for (auto& out : node_->match(dom, candidate, tracer, depth + 1)) {
                    if (out.cursor <= candidate.cursor) {
                        tracer.fail(name_, depth, candidate.cursor, "child did not advance");
                        continue;
                    }
                    next.push_back(std::move(out));
                }
            }
            if (next.empty()) {
                break;
            }
            current = std::move(next);
            if (count >= min_) {
                accepted.insert(accepted.end(),
                                std::make_move_iterator(current.begin()),
                                std::make_move_iterator(current.end()));
            }
        }
        if (accepted.empty()) {
            tracer.fail(name_, depth, state.cursor, "min bound not met");
            return {};
        }
        std::sort(accepted.begin(), accepted.end(),
                  [this](const MatchState& a, const MatchState& b) {
                      return greedy_ ? a.cursor > b.cursor : a.cursor < b.cursor;
                  });
        tracer.success(name_, depth, state.cursor, accepted.front().cursor,
                       "candidates=" + std::to_string(accepted.size()));
        return accepted;
    }
};

// ============================================================================
// 捕获组合子
// ============================================================================

/// @brief 捕获组合：匹配子 step，把「窗口入口指令句柄」绑定到捕获名。
///
/// 绑定语义：捕获值 = 子窗口第一条指令的 InsnId（子窗口必须非空；零宽
/// 子匹配的候选被拒绝——空窗口没有可指认的指令）。同名已绑定且句柄
/// 不同的候选同样被拒绝（捕获冲突）。候选保持子 step 的优先级排列。
class CaptureStep : public IMatchStep
{
    std::string_view name_;
    StepPtr          node_;

public:
    CaptureStep(std::string_view name, StepPtr node)
        : name_(name)
        , node_(std::move(node))
    {}

    std::string name() const override
    {
        return "capture(" + std::string(name_) + ")";
    }

    std::vector<MatchState> match(const jbc::ir::CodeDom& dom,
                                  const MatchState&       state,
                                  MatchTracer&            tracer,
                                  ca::i32                 depth) override
    {
        const std::string label = name();
        tracer.enter(label, depth, state.cursor);
        const ca::i32 start    = state.cursor;
        auto          matched  = node_->match(dom, state, tracer, depth + 1);
        if (matched.empty()) {
            tracer.fail(label, depth, state.cursor, "child failed");
            return {};
        }
        std::vector<MatchState> out;
        for (auto& candidate : matched) {
            if (candidate.cursor <= start) {
                tracer.fail(label, depth, start, "zero-width capture rejected");
                continue;
            }
            const jbc::ir::InsnId id =
                dom.order()[static_cast<ca::usize>(start)];
            auto bound = candidate.captures.bind(name_, id);
            if (!bound) {
                tracer.fail(label, depth, start, "capture conflict");
                continue;
            }
            out.push_back(candidate.withCaptures(*bound));
        }
        if (out.empty()) {
            tracer.fail(label, depth, state.cursor, "all candidates rejected");
            return {};
        }
        tracer.success(label, depth, state.cursor, out.front().cursor);
        return out;
    }
};

// ============================================================================
// 工厂函数
// ============================================================================

namespace combinators {

// ---------- 原子 step ----------

/// @brief 通用谓词叶子：name 仅作调试标识。
inline StepPtr pred(std::string name, InsnPredicate p)
{
    return std::make_unique<LeafStep>(std::move(name), std::move(p));
}

/// @brief 匹配指定 opcode 的单条指令。
inline StepPtr opcode(jbc::core::Op op)
{
    return std::make_unique<OpcodeStep>(op);
}

/// @brief 匹配任意一条指令（任意单条 any）。
inline StepPtr any()
{
    return std::make_unique<AnyOneStep>();
}

/// @brief 匹配「任意单条」重复 [0, 无上限)。
inline StepPtr anyRepeat()
{
    return std::make_unique<RepeatStep>(std::make_unique<AnyOneStep>(), 0, -1, true);
}

/// @brief 匹配 CP 形状（CP1/CP2/CP2Count/CP2Zero2/CP2Dims）且 cp 索引相等的指令。
///
/// 新底座的 CodeDom 不解析常量池内容（cp 只是索引）；按 owner/name/desc
/// 匹配调用点的调用方需先在池侧解出索引再传入。
inline StepPtr cpIndex(ca::u32 index)
{
    return pred("cp(" + std::to_string(index) + ")",
                [index](const InsnCursorView& v, const CaptureStore&) {
                    const jbc::core::Shape sh = jbc::core::shapeOf(v.insn->op);
                    switch (sh) {
                    case jbc::core::Shape::CP1:
                    case jbc::core::Shape::CP2:
                    case jbc::core::Shape::CP2Count:
                    case jbc::core::Shape::CP2Zero2:
                    case jbc::core::Shape::CP2Dims:
                        return v.insn->cp == index;
                    default:
                        return false;
                    }
                });
}

/// @brief 匹配局部变量访问形状（Local1：iload/lload/…/astore/ret）且索引相等。
///
/// `iinc`（Iinc1）与 `_n` 短形式不在此列：前者是读改写，后者编码在 opcode
/// 里，归类归 asm/insn 模块；本模块只做形状层的诚实匹配。
inline StepPtr localIndex(ca::i32 index)
{
    return pred("local(" + std::to_string(index) + ")",
                [index](const InsnCursorView& v, const CaptureStore&) {
                    return jbc::core::shapeOf(v.insn->op) == jbc::core::Shape::Local1 &&
                           v.insn->a == index;
                });
}

/// @brief 匹配立即数形状（Imm1/Imm2：bipush/sipush）且值相等的指令。
inline StepPtr imm(ca::i32 value)
{
    return pred("imm(" + std::to_string(value) + ")",
                [value](const InsnCursorView& v, const CaptureStore&) {
                    const jbc::core::Shape sh = jbc::core::shapeOf(v.insn->op);
                    return (sh == jbc::core::Shape::Imm1 || sh == jbc::core::Shape::Imm2) &&
                           v.insn->a == value;
                });
}

/// @brief 匹配分支形状（Branch2/Branch4）且目标标签解析结果满足谓词的指令。
///
/// 解析失败（悬垂标签）视为不匹配；switch 形状不在本谓词覆盖内。
inline StepPtr branchTo(LabelTargetPredicate p)
{
    return pred("branchTo",
                [p = std::move(p)](const InsnCursorView& v, const CaptureStore&) {
                    const jbc::core::Shape sh = jbc::core::shapeOf(v.insn->op);
                    if (sh != jbc::core::Shape::Branch2 && sh != jbc::core::Shape::Branch4) {
                        return false;
                    }
                    auto resolved = v.dom->resolveLabel(v.insn->target);
                    if (resolved.is_err()) {
                        return false;
                    }
                    return p(resolved.unwrap());
                });
}

/// @brief 匹配分支指令，且其解析目标 == 捕获 name 绑定的指令句柄。
///
/// 未绑定或绑定句柄无效（kNoInsn）时不匹配。
inline StepPtr branchTargetsCapture(std::string_view name)
{
    return pred("branchTargets(" + std::string(name) + ")",
                [name](const InsnCursorView& v, const CaptureStore& captures) {
                    const jbc::ir::InsnId bound = captures.get(name);
                    if (bound == jbc::ir::kNoInsn) {
                        return false;
                    }
                    const jbc::core::Shape sh = jbc::core::shapeOf(v.insn->op);
                    if (sh != jbc::core::Shape::Branch2 && sh != jbc::core::Shape::Branch4) {
                        return false;
                    }
                    auto resolved = v.dom->resolveLabel(v.insn->target);
                    if (resolved.is_err()) {
                        return false;
                    }
                    const jbc::ir::LabelTarget t = resolved.unwrap();
                    return !t.isEnd() && t.insn == bound;
                });
}

/// @brief 匹配 Local1 形状指令，且其局部索引 == 捕获 name 绑定指令的局部索引。
///
/// 绑定指令也必须是 Local1 形状，否则不匹配（旧实现的 guardEquals 槽位
/// 相等场景在此表达）。
inline StepPtr sameLocalIndexAs(std::string_view name)
{
    return pred("sameLocalAs(" + std::string(name) + ")",
                [name](const InsnCursorView& v, const CaptureStore& captures) {
                    if (jbc::core::shapeOf(v.insn->op) != jbc::core::Shape::Local1) {
                        return false;
                    }
                    const jbc::ir::InsnId bound = captures.get(name);
                    if (bound == jbc::ir::kNoInsn) {
                        return false;
                    }
                    const jbc::ir::Insn* other = v.dom->peek(bound);
                    if (other == nullptr ||
                        jbc::core::shapeOf(other->op) != jbc::core::Shape::Local1) {
                        return false;
                    }
                    return v.insn->a == other->a;
                });
}

// ---------- 序列组合子 ----------

inline StepPtr seq(std::vector<StepPtr> nodes)
{
    return std::make_unique<SeqStep>(std::move(nodes));
}

template<typename... Args, typename = std::enable_if_t<(sizeof...(Args) > 1)>>
StepPtr seq(Args&&... args)
{
    std::vector<StepPtr> nodes;
    (nodes.push_back(std::forward<Args>(args)), ...);
    return seq(std::move(nodes));
}

inline StepPtr alt(std::vector<StepPtr> branches)
{
    return std::make_unique<AltStep>(std::move(branches));
}

template<typename... Args, typename = std::enable_if_t<(sizeof...(Args) > 1)>>
StepPtr alt(Args&&... args)
{
    std::vector<StepPtr> branches;
    (branches.push_back(std::forward<Args>(args)), ...);
    return alt(std::move(branches));
}

inline StepPtr opt(StepPtr node)
{
    return std::make_unique<OptionalStep>(std::move(node));
}

/// @brief 重复 [min, max] 次（贪婪）；max < 0 表示无上限。
///
/// 区间不合法（min < 0，或 max >= 0 且 max < min）时返回永不匹配 step，
/// 失败原因可在 name()/trace 中观察。
inline StepPtr repeat(StepPtr node, ca::i32 min, ca::i32 max)
{
    if (min < 0 || (max >= 0 && max < min)) {
        return std::make_unique<NeverMatchStep>(
            "repeat(min=" + std::to_string(min) + ",max=" +
            (max < 0 ? std::string("inf") : std::to_string(max)) + ",invalid)");
    }
    return std::make_unique<RepeatStep>(std::move(node), min, max, true);
}

/// @brief 重复精确 n 次（贪婪）。
inline StepPtr repeat(StepPtr node, ca::i32 n)
{
    return repeat(std::move(node), n, n);
}

/// @brief 重复 [min, max] 次（懒惰，最短优先）；max < 0 表示无上限。
inline StepPtr repeatLazy(StepPtr node, ca::i32 min, ca::i32 max)
{
    if (min < 0 || (max >= 0 && max < min)) {
        return std::make_unique<NeverMatchStep>(
            "repeat(min=" + std::to_string(min) + ",max=" +
            (max < 0 ? std::string("inf") : std::to_string(max)) + ",lazy,invalid)");
    }
    return std::make_unique<RepeatStep>(std::move(node), min, max, false);
}

// ---------- 捕获 ----------

/// @brief 捕获子窗口入口指令句柄到 name（见 CaptureStep 的绑定语义）。
inline StepPtr capture(std::string_view name, StepPtr node)
{
    return std::make_unique<CaptureStep>(name, std::move(node));
}

}   // namespace combinators

}   // namespace mjbase::asm_tmatcher
