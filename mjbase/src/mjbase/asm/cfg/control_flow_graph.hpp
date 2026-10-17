/// @file        control_flow_graph.hpp
/// @brief       方法级控制流图：基本块表、后继/前驱边与异常边。
///
/// 语义来源是 mbase 的 asm/cfg（冻结参考），在 libjbc 底座上重写为只读
/// 值视图：
///   - 切块复用 CodeEditor::blocks() 的 leader 分析（顺序头、跳转/switch
///     目标、return/athrow/ret 的顺序后继、异常表三个边界）；
///   - 普通边按块末条指令形状分类（Branch2/Branch4/switch/终止/顺序）；
///   - 异常边建模为「覆盖区间的块 → handler 块」：CodeUnit::exc 的
///     [start, end) 区间按块首位置对齐（切分保证边界都是块首），区间内
///     每个块获得一条指向 handler 的 Exception 边。
///
/// 与旧实现的差异：CFG 不再接管/改写指令（旧 preProcess 会把 InsnList
/// 摘链再交还），CodeDom 始终是唯一属主；jsr 除 taken 边外还补一条顺序
/// FallThrough 边（子程序返回点），旧实现只有 fallthrough、丢失跳转目标。
#pragma once

#include <unordered_map>
#include <vector>

#include "libca/core/result.hpp"

#include "basic_block.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::asm_cfg {

/// @brief CFG 构建失败原因。
enum class CfgError : ca::u8 {
    Ok = 0,
    BadTarget,    ///< 分支/switch 目标标签无效，或解析到方法末尾边界
    BadExcEntry,  ///< 异常表条目标签无效（start/handler 必须落在指令边界）
};

/// @brief 错误码的稳定文本（诊断输出用）。
const char* to_string(CfgError e) noexcept;

/// @brief 控制流图：程序序块表 + 每块出边/入边表。
///
/// 构建后自包含（只存句柄值，不持有 CodeUnit 引用），可自由拷贝/移动；
/// 句柄的解析仍需原 CodeDom 存活。默认构造是 0 块的空图（空方法形态）。
class ControlFlowGraph
{
public:
    ControlFlowGraph() = default;

    /// @brief 从方法体构建 CFG（读 dom 顺序 + exc 表；不修改 unit）。
    ///
    /// 前置条件：unit.dom / unit.exc 处于一致状态（句柄有效、switch
    /// 操作数与形状匹配——append 校验已保证）。分支/switch 目标或异常表
    /// 边界无法解析时返回 Err，不做静默跳过。
    static ca::core::Result<ControlFlowGraph, CfgError> build(jbc::ir::CodeUnit& unit);

    /// @brief 块数。
    ca::usize blockCount() const noexcept { return blocks_.size(); }

    /// @brief 块表（程序序；blocks()[0] 即入口块）。
    const std::vector<BasicBlock>& blocks() const noexcept { return blocks_; }

    /// @brief 按 id 取块；无效 id 返回 nullptr（探测式，不抛错）。
    const BasicBlock* block(BlockId id) const noexcept;

    /// @brief 入口块；空方法返回 kNoBlock。
    BlockId entry() const noexcept { return blocks_.empty() ? kNoBlock : BlockId{0}; }

    /// @brief 指令所在块；不属于任何块（含无效/过期句柄）返回 kNoBlock。
    BlockId blockOfInsn(jbc::ir::InsnId insn) const noexcept;

    /// @brief 标签所在块（解析到其锚定指令的所属块）；End 边界或无效
    ///        标签返回 kNoBlock。
    BlockId blockOfLabel(const jbc::ir::CodeDom& dom, jbc::ir::LabelId label) const noexcept;

    /// @brief 块的出边（普通边在前、异常边在后，均按构建序）；
    ///        无效 id 返回空表。
    const std::vector<Edge>& succEdgesOf(BlockId id) const noexcept;

    /// @brief 块的入边（普通边在前、异常边在后）；无效 id 返回空表。
    const std::vector<Edge>& predEdgesOf(BlockId id) const noexcept;

    /// @brief 去重后的后继块（按块下标升序）；异常 handler 也算后继。
    std::vector<BlockId> successors(BlockId id) const;

    /// @brief 去重后的前驱块（按块下标升序）。
    std::vector<BlockId> predecessors(BlockId id) const;

    /// @brief 全图按边种类统计边数（succ 边表条数，含异常边）。
    ca::usize edgeCount(EdgeKind kind) const noexcept;

    /// @brief 块末条指令是 return 族或 athrow（终止块，无普通后继）。
    bool isTerminated(BlockId id) const noexcept;

private:
    std::vector<BasicBlock> blocks_;
    std::vector<std::vector<Edge>> succ_;
    std::vector<std::vector<Edge>> pred_;
    std::vector<ca::u8> terminated_;
    std::unordered_map<jbc::ir::InsnId, ca::u32, InsnIdHash> blockOf_;
};

}   // namespace mjbase::asm_cfg
