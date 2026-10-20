/// @file        cfg_analysis.hpp
/// @brief       CFG 上的图分析：DFS 可达集、迭代支配树与支配边界。
///
/// 语义来源是 旧栈 asm/cfg 的 CfgTraversal / DominatorTree（冻结参考）。
/// 遍历与支配计算的后继集都包含异常边（handler 入口可达、参与支配），
/// 与旧实现一致。
#pragma once

#include <vector>

#include "control_flow_graph.hpp"

namespace mjbase::asm_cfg {

/// @brief 深度优先遍历：preorder / postorder / 逆后序与可达集。
///
/// 从入口块出发（successors 含异常 handler 后继）；空图各项为空。
/// 迭代式 DFS，深链方法不会递归爆栈。
class CfgTraversal
{
public:
    explicit CfgTraversal(const ControlFlowGraph& cfg);

    const std::vector<BlockId>& preorder() const noexcept { return preorder_; }
    const std::vector<BlockId>& postorder() const noexcept { return postorder_; }
    const std::vector<BlockId>& reversePostorder() const noexcept { return rpo_; }

    /// @brief 块是否从入口可达。
    bool isReachable(BlockId block) const noexcept;

private:
    std::vector<ca::u8> reachable_;
    std::vector<BlockId> preorder_;
    std::vector<BlockId> postorder_;
    std::vector<BlockId> rpo_;
};

/// @brief 迭代支配树（Cooper-Harvey-Kennedy 逆后序算法）+ 支配边界。
///
/// 仅对可达块给出结果：不可达块的 idom 为 kNoBlock、dominates 恒为
/// false、frontier 为空。入口块自支配（idom(entry) == entry）。
class DominatorTree
{
public:
    /// @brief 在 cfg 上计算支配关系；可重复调用（先清空旧状态）。
    void compute(const ControlFlowGraph& cfg);

    bool isReachable(BlockId block) const noexcept;

    /// @brief 直接支配者；不可达/无效块返回 kNoBlock。
    BlockId idom(BlockId block) const noexcept;

    /// @brief dominator 是否支配 block（自支配成立）；任一侧不可达为 false。
    bool dominates(BlockId dominator, BlockId block) const noexcept;

    /// @brief 支配边界（按块下标升序去重）；无效块返回空表。
    const std::vector<BlockId>& frontier(BlockId block) const noexcept;

private:
    std::vector<ca::u8> reachable_;
    std::vector<BlockId> idoms_;
    std::vector<std::vector<BlockId>> frontiers_;
};

}   // namespace mjbase::asm_cfg
