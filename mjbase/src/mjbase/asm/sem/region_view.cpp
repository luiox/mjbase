/// @file        region_view.cpp
/// @brief       RegionView 构建：后支配树（逆图 CHK）与 SESE 候选枚举。
#include "mjbase/asm/sem/region_view.hpp"

#include <algorithm>
#include <utility>

#include "jbc/core/opcode.hpp"

using ca::core::Err;
using ca::core::Ok;

namespace mjbase::asm_sem {

namespace {

/// @brief 普通边域的迭代后支配树（Cooper-Harvey-Kennedy 逆后序算法，
///        在反向图 + 虚拟根上运行）。
///
/// 虚拟根连接全部「普通出边为空」的块（return/athrow 尾块——含只挂异常
/// 边的块：普通边域下它们无后继）；从虚拟根不可达的块（普通边域下无法
/// 到达出口，如死循环体）后支配查询恒 false。异常边不入图（见
/// region_view.hpp 类注）。
class PostDominators
{
public:
    /// @param normalSuccs 每块的普通出边目标（可含重复；无害）。
    void compute(ca::u32 blockCount, const std::vector<std::vector<ca::u32>>& normalSuccs)
    {
        const ca::u32 n    = blockCount;
        const ca::u32 root = n;   /// 虚拟根
        reach_.assign(n + 1, 0);
        idom_.assign(n + 1, asm_cfg::BlockId::kInvalid);
        reach_[root] = 1;

        /// 反向邻接：radj[x] = 普通边指向 x 的块。虚拟根 → 全部出口块
        /// （普通出边为空——含只挂异常边的块）。
        std::vector<std::vector<ca::u32>> radj(n + 1);
        for (ca::u32 b = 0; b < n; ++b) {
            if (normalSuccs[b].empty()) {
                radj[root].push_back(b);
                continue;
            }
            for (const ca::u32 s : normalSuccs[b]) {
                radj[s].push_back(b);
            }
        }

        /// 反向图 DFS（迭代式）：root 起可达集 + 后序 → 逆后序。
        std::vector<ca::u32> rpoIndex(n + 1, 0);
        std::vector<ca::u32> post;
        post.reserve(n + 1);
        {
            std::vector<std::pair<ca::u32, ca::usize>> stack;
            stack.emplace_back(root, 0);
            while (!stack.empty()) {
                auto& top = stack.back();
                if (top.second < radj[top.first].size()) {
                    const ca::u32 next = radj[top.first][top.second++];
                    if (reach_[next] == 0) {
                        reach_[next] = 1;
                        stack.emplace_back(next, 0);
                    }
                    continue;
                }
                post.push_back(top.first);
                stack.pop_back();
            }
        }

        rpo_.reserve(post.size());
        for (ca::usize i = post.size(); i-- > 0;) {
            rpoIndex[post[i]] = static_cast<ca::u32>(rpo_.size());
            rpo_.push_back(post[i]);
        }

        /// CHK：逆后序迭代（root 自支配；无已处理反向后继的块挂 root）。
        idom_[root] = root;
        for (ca::usize i = 1; i < rpo_.size(); ++i) {
            const ca::u32 b = rpo_[i];
            ca::u32 newIdom = asm_cfg::BlockId::kInvalid;
            for (const ca::u32 s : normalSuccs[b]) {
                if (idom_[s] == asm_cfg::BlockId::kInvalid) {
                    continue;   /// 未处理（逆后序在其后）。
                }
                if (newIdom == asm_cfg::BlockId::kInvalid) {
                    newIdom = s;
                    continue;
                }
                newIdom = intersect(s, newIdom, idom_, rpoIndex);
            }
            idom_[b] = newIdom == asm_cfg::BlockId::kInvalid ? root : newIdom;
        }
    }

    /// b 在普通边域能到达出口（反向图上自 root 可达；root 本身恒真）。
    bool reachable(ca::u32 b) const noexcept
    {
        return b < reach_.size() && reach_[b] != 0;
    }

    /// a 后支配 b（自后支配成立；任一侧不可达为 false）。
    bool dominates(ca::u32 a, ca::u32 b) const noexcept
    {
        const ca::u32 root = static_cast<ca::u32>(idom_.size()) - 1;
        if (a >= root || b >= root || reach_[a] == 0 || reach_[b] == 0) {
            return false;
        }
        ca::u32 walk = b;
        while (walk != a) {
            if (walk == root) {
                return false;   /// 抵虚拟根仍未命中。
            }
            walk = idom_[walk];
        }
        return true;
    }

private:
    static ca::u32 intersect(ca::u32 a, ca::u32 b, const std::vector<ca::u32>& idom,
                             const std::vector<ca::u32>& rpoIndex)
    {
        while (a != b) {
            while (rpoIndex[a] > rpoIndex[b]) {
                a = idom[a];
            }
            while (rpoIndex[b] > rpoIndex[a]) {
                b = idom[b];
            }
        }
        return a;
    }

    std::vector<ca::u8>  reach_;   ///< 反向图 root 可达（含 root 自身）
    std::vector<ca::u32> idom_;    ///< 逆图直接支配者；root 自支配
    std::vector<ca::u32> rpo_;     ///< 逆图逆后序（root 在首）
};

/// 普通出边目标（去重升序）：后支配邻接与直线判定共用。
std::vector<ca::u32> normalSuccTargetsOf(const mjbase::asm_cfg::ControlFlowGraph& cfg,
                                         mjbase::asm_cfg::BlockId b)
{
    std::vector<ca::u32> out;
    for (const mjbase::asm_cfg::Edge& e : cfg.succEdgesOf(b)) {
        if (e.kind == mjbase::asm_cfg::EdgeKind::Exception) {
            continue;
        }
        if (std::find(out.begin(), out.end(), e.target.index) == out.end()) {
            out.push_back(e.target.index);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool isSubroutineOp(jbc::core::Op op) noexcept
{
    return op == jbc::core::Op::JSR || op == jbc::core::Op::JSR_W || op == jbc::core::Op::RET;
}

}   // namespace

ca::core::Result<RegionView, mjbase::asm_cfg::CfgError> RegionView::build(jbc::ir::CodeUnit& unit)
{
    RegionView view;

    auto built = mjbase::asm_cfg::ControlFlowGraph::build(unit);
    if (built.is_err()) {
        return Err(built.unwrap_err());
    }
    view.cfg_ = std::move(built).unwrap();
    view.dominators_.compute(view.cfg_);

    /// 块级 handler 事实：异常边自带 handler 块与 excIndex（catchCp 回查表项）。
    const ca::usize n = view.cfg_.blockCount();
    view.handlers_.assign(n, {});
    for (ca::usize b = 0; b < n; ++b) {
        const mjbase::asm_cfg::BlockId id{static_cast<ca::u32>(b)};
        for (const mjbase::asm_cfg::Edge& e : view.cfg_.succEdgesOf(id)) {
            if (e.kind != mjbase::asm_cfg::EdgeKind::Exception) {
                continue;
            }
            const ca::u32 catchCp =
                e.excIndex < unit.exc.size() ? unit.exc[e.excIndex].catchCp : 0;
            HandlerRef ref{e.target, catchCp};
            if (std::find(view.handlers_[b].begin(), view.handlers_[b].end(), ref) ==
                view.handlers_[b].end()) {
                view.handlers_[b].push_back(ref);
            }
        }
        std::sort(view.handlers_[b].begin(), view.handlers_[b].end());
    }

    /// 判据 5 语形扫描（RegionView 不依赖帧；FrameSummary.hasSubroutine 为
    /// 帧口径事实，两者一致性由 FrameView 侧消费时保证）。
    for (const jbc::ir::InsnId& id : unit.dom.order()) {
        const jbc::ir::Insn* in = unit.dom.peek(id);
        if (in != nullptr && isSubroutineOp(in->op)) {
            view.hasSubroutine_ = true;
            break;
        }
    }
    return Ok(std::move(view));
}

std::vector<HandlerRef> RegionView::handlerSetOf(mjbase::asm_cfg::BlockId id) const
{
    if (!id.valid() || id.index >= handlers_.size()) {
        return {};
    }
    return handlers_[id.index];
}

bool RegionView::exceptionHomogeneous(const std::vector<mjbase::asm_cfg::BlockId>& members) const
{
    if (members.empty()) {
        return true;
    }
    const std::vector<HandlerRef> first = handlerSetOf(members.front());
    for (ca::usize i = 1; i < members.size(); ++i) {
        if (handlerSetOf(members[i]) != first) {
            return false;
        }
    }
    return true;
}

bool RegionView::straightLine(const std::vector<mjbase::asm_cfg::BlockId>& members) const
{
    for (ca::usize i = 0; i < members.size(); ++i) {
        const std::vector<ca::u32> succs = normalSuccTargetsOf(cfg_, members[i]);
        if (i + 1 < members.size()) {
            if (succs.size() != 1 || succs.front() != members[i + 1].index) {
                return false;
            }
            continue;
        }
        if (succs.size() > 1) {
            return false;   /// 末成员带内部分支（回边/多越出）非直线。
        }
    }
    return true;
}

std::vector<RegionCandidate> RegionView::candidates() const
{
    std::vector<RegionCandidate> out;
    const ca::u32 n = static_cast<ca::u32>(cfg_.blockCount());
    if (n == 0) {
        return out;
    }

    std::vector<std::vector<ca::u32>> normalSuccs(n);
    for (ca::u32 b = 0; b < n; ++b) {
        normalSuccs[b] = normalSuccTargetsOf(cfg_, mjbase::asm_cfg::BlockId{b});
    }
    PostDominators pdom;
    pdom.compute(n, normalSuccs);

    std::vector<ca::u8> member(n, 0);
    std::vector<std::pair<ca::u32, ca::u32>> seenEntries;

    for (ca::u32 a = 0; a < n; ++a) {
        for (const mjbase::asm_cfg::Edge& e : cfg_.succEdgesOf(mjbase::asm_cfg::BlockId{a})) {
            if (e.kind == mjbase::asm_cfg::EdgeKind::Exception) {
                continue;   /// 异常边不作区域入口（进入点语义不成立）。
            }
            const ca::u32 head = e.target.index;
            const auto key     = std::make_pair(a, head);
            if (std::find(seenEntries.begin(), seenEntries.end(), key) != seenEntries.end()) {
                continue;   /// 并行同类边（Condition+FallThrough 同对）只枚举一次。
            }
            seenEntries.push_back(key);

            /// 入口支配集：入口边目标的支配闭包（asm_cfg 语义，含异常边
            /// 参与支配；不可达块天然排除）。
            std::vector<mjbase::asm_cfg::BlockId> dominated;
            for (ca::u32 x = 0; x < n; ++x) {
                if (dominators_.dominates(mjbase::asm_cfg::BlockId{head},
                                          mjbase::asm_cfg::BlockId{x})) {
                    dominated.push_back(mjbase::asm_cfg::BlockId{x});
                }
            }
            if (dominated.empty()) {
                continue;
            }

            /// 出口锚探测：支配集成员的普通后继（收敛检查钉定唯一越出
            /// 目标；锚可落在支配集内——循环形态的越出目标在集合内）。
            std::vector<ca::u32> probes;
            for (const mjbase::asm_cfg::BlockId m : dominated) {
                for (const ca::u32 t : normalSuccs[m.index]) {
                    if (std::find(probes.begin(), probes.end(), t) == probes.end()) {
                        probes.push_back(t);
                    }
                }
            }

            std::vector<RegionCandidate> perEdge;
            for (const ca::u32 anchor : probes) {
                /// R = { x ∈ 支配集 : 锚后支配 x } \ {锚}（口径
                /// 「入口支配集 ∩ 出口后支配集」；锚自排除——它是续延点）。
                std::vector<mjbase::asm_cfg::BlockId> members;
                for (const mjbase::asm_cfg::BlockId x : dominated) {
                    if (x.index == anchor) {
                        continue;
                    }
                    if (pdom.dominates(anchor, x.index)) {
                        members.push_back(x);
                    }
                }
                if (members.empty()) {
                    continue;
                }
                for (const mjbase::asm_cfg::BlockId m : members) {
                    member[m.index] = 1;
                }

                /// 单入口：成员全部入边（普通 + 异常）只允许来自成员内部
                /// 或入口边 (a, head)；并行同对边按 (源, 目标) 去重。
                bool singleEntry = true;
                {
                    std::vector<std::pair<ca::u32, ca::u32>> inPairs;
                    for (const mjbase::asm_cfg::BlockId m : members) {
                        for (const mjbase::asm_cfg::Edge& pe : cfg_.predEdgesOf(m)) {
                            const auto p = std::make_pair(pe.source.index, m.index);
                            if (std::find(inPairs.begin(), inPairs.end(), p) != inPairs.end()) {
                                continue;
                            }
                            inPairs.push_back(p);
                            if (member[pe.source.index]) {
                                continue;
                            }
                            if (!(pe.source.index == a && m.index == head)) {
                                singleEntry = false;
                                break;
                            }
                        }
                        if (!singleEntry) {
                            break;
                        }
                    }
                }

                /// 单出口：成员普通出边的越出目标收敛于唯一块（无越出 =
                /// 无续延边，v1 不交付）。
                std::vector<ca::u32> escapes;
                if (singleEntry) {
                    for (const mjbase::asm_cfg::BlockId m : members) {
                        for (const ca::u32 t : normalSuccs[m.index]) {
                            if (member[t]) {
                                continue;
                            }
                            if (std::find(escapes.begin(), escapes.end(), t) == escapes.end()) {
                                escapes.push_back(t);
                            }
                        }
                    }
                    if (escapes.size() != 1) {
                        singleEntry = false;
                    }
                }

                for (const mjbase::asm_cfg::BlockId m : members) {
                    member[m.index] = 0;
                }
                if (!singleEntry) {
                    continue;
                }

                RegionCandidate r;
                r.entryFrom  = mjbase::asm_cfg::BlockId{a};
                r.entryBlock = mjbase::asm_cfg::BlockId{head};
                r.exitTarget = mjbase::asm_cfg::BlockId{escapes.front()};
                r.members    = std::move(members);
                r.exceptionHomogeneous = exceptionHomogeneous(r.members);
                r.straightLine         = straightLine(r.members);
                if (r.exceptionHomogeneous) {
                    r.handlers = handlerSetOf(r.members.front());
                }
                perEdge.push_back(std::move(r));
            }

            /// 同入口边下被更大候选包含的次大候选不重复交付。
            for (const RegionCandidate& c : perEdge) {
                bool subsumed = false;
                for (const RegionCandidate& bigger : perEdge) {
                    if (&bigger != &c && bigger.members.size() > c.members.size() &&
                        std::includes(bigger.members.begin(), bigger.members.end(),
                                      c.members.begin(), c.members.end())) {
                        subsumed = true;
                        break;
                    }
                }
                if (!subsumed) {
                    out.push_back(c);
                }
            }
        }
    }
    return out;
}

}   // namespace mjbase::asm_sem
