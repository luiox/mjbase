/// @file        cfg_analysis.cpp
/// @brief       CFG 遍历与支配树实现（CHK 逆后序迭代）。
#include "cfg_analysis.hpp"

#include <algorithm>

namespace mjbase::asm_cfg {

CfgTraversal::CfgTraversal(const ControlFlowGraph& cfg)
{
    const ca::usize n = cfg.blockCount();
    if (n == 0) {
        return;
    }
    reachable_.assign(n, 0);

    // 显式栈 DFS：入栈时记 preorder，弹栈时记 postorder。
    // 栈帧持有该块的后继快照（successors 已按块序去重）。
    struct Frame
    {
        BlockId node;
        std::vector<BlockId> succs;
        ca::usize next = 0;
    };
    std::vector<Frame> stack;

    const BlockId root = cfg.entry();
    reachable_[root.index] = 1;
    preorder_.push_back(root);
    stack.push_back(Frame{root, cfg.successors(root), 0});

    while (!stack.empty()) {
        Frame& top = stack.back();
        if (top.next < top.succs.size()) {
            const BlockId child = top.succs[top.next++];
            if (reachable_[child.index] == 0) {
                reachable_[child.index] = 1;
                preorder_.push_back(child);
                stack.push_back(Frame{child, cfg.successors(child), 0});
            }
            continue;
        }
        postorder_.push_back(top.node);
        stack.pop_back();
    }

    rpo_ = postorder_;
    std::reverse(rpo_.begin(), rpo_.end());
}

bool CfgTraversal::isReachable(BlockId block) const noexcept
{
    return block.valid() && block.index < reachable_.size() && reachable_[block.index] != 0;
}

void DominatorTree::compute(const ControlFlowGraph& cfg)
{
    const ca::usize n = cfg.blockCount();
    reachable_.assign(n, 0);
    idoms_.assign(n, kNoBlock);
    frontiers_.assign(n, {});
    if (n == 0) {
        return;
    }

    const CfgTraversal traversal(cfg);
    const std::vector<BlockId>& rpo = traversal.reversePostorder();

    std::vector<ca::u32> rpoIndex(n, BlockId::kInvalid);
    for (ca::usize i = 0; i < rpo.size(); ++i) {
        rpoIndex[rpo[i].index] = static_cast<ca::u32>(i);
        reachable_[rpo[i].index] = 1;
    }

    // 前驱表：successors 已按块序去重，按源块升序追加即天然有序无重。
    std::vector<std::vector<BlockId>> preds(n);
    for (ca::u32 p = 0; static_cast<ca::usize>(p) < n; ++p) {
        if (reachable_[p] == 0) {
            continue;
        }
        for (const BlockId t : cfg.successors(BlockId{p})) {
            if (reachable_[t.index] != 0) {
                preds[t.index].push_back(BlockId{p});
            }
        }
    }

    // 入口自支配；其余块的 idom 迭代到不动点（CHK）。
    const BlockId entryBlock = cfg.entry();
    idoms_[entryBlock.index] = entryBlock;

    // intersect：两个已定 idom 的块沿支配链上溯，直到 RPO 序较小者会合。
    const auto intersect = [&](BlockId lhs, BlockId rhs) {
        while (lhs != rhs) {
            while (rpoIndex[lhs.index] > rpoIndex[rhs.index]) {
                lhs = idoms_[lhs.index];
            }
            while (rpoIndex[rhs.index] > rpoIndex[lhs.index]) {
                rhs = idoms_[rhs.index];
            }
        }
        return lhs;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (ca::usize i = 1; i < rpo.size(); ++i) {
            const BlockId b = rpo[i];
            BlockId newIdom = kNoBlock;
            for (const BlockId p : preds[b.index]) {
                if (!idoms_[p.index].valid()) {
                    continue;   // 尚未定 idom 的前驱不参与本轮
                }
                newIdom = newIdom.valid() ? intersect(p, newIdom) : p;
            }
            if (newIdom.valid() && idoms_[b.index] != newIdom) {
                idoms_[b.index] = newIdom;
                changed = true;
            }
        }
    }

    // 支配边界：多前驱块的每个前驱沿 idom 链上溯至 idom[b] 为止，
    // 途经块（含起点）的边界都加入 b。
    for (ca::usize i = 0; i < rpo.size(); ++i) {
        const BlockId b = rpo[i];
        if (preds[b.index].size() < 2) {
            continue;
        }
        for (const BlockId p : preds[b.index]) {
            BlockId runner = p;
            while (runner != idoms_[b.index]) {
                frontiers_[runner.index].push_back(b);
                const BlockId up = idoms_[runner.index];
                if (!up.valid() || up == runner) {
                    break;   // 入口自支配兜底（正常不会走到）
                }
                runner = up;
            }
        }
    }
    for (auto& f : frontiers_) {
        std::sort(f.begin(), f.end());
        f.erase(std::unique(f.begin(), f.end()), f.end());
    }
}

bool DominatorTree::isReachable(BlockId block) const noexcept
{
    return block.valid() && block.index < reachable_.size() && reachable_[block.index] != 0;
}

BlockId DominatorTree::idom(BlockId block) const noexcept
{
    if (!isReachable(block)) {
        return kNoBlock;
    }
    return idoms_[block.index];
}

bool DominatorTree::dominates(BlockId dominator, BlockId block) const noexcept
{
    if (!isReachable(dominator) || !isReachable(block)) {
        return false;
    }
    BlockId cur = block;
    for (;;) {
        if (cur == dominator) {
            return true;
        }
        const BlockId parent = idoms_[cur.index];
        if (!parent.valid() || parent == cur) {
            return false;
        }
        cur = parent;
    }
}

const std::vector<BlockId>& DominatorTree::frontier(BlockId block) const noexcept
{
    static const std::vector<BlockId> kEmpty;
    if (!block.valid() || block.index >= frontiers_.size()) {
        return kEmpty;
    }
    return frontiers_[block.index];
}

}   // namespace mjbase::asm_cfg
