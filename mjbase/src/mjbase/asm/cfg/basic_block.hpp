/// @file        basic_block.hpp
/// @brief       控制流基本块与边的值语义视图类型。
///
/// 块与边都是纯值：BlockId 是 ControlFlowGraph 块表的密集下标（块表按
/// 程序序排列，0 号块即方法入口），指令身份沿用 jbc::ir::InsnId。CFG
/// 不持有任何指令所有权——CodeDom 仍是唯一属主，本模块只做只读分析
/// 视图；块/边里保存的是句柄副本，CodeUnit 存活期间句柄可继续解析。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/ir/code_dom.hpp"

namespace mjbase::asm_cfg {

/// @brief 块句柄：ControlFlowGraph 块表的密集下标（程序序，0 = 入口块）。
struct BlockId
{
    static constexpr ca::u32 kInvalid = 0xFFFFFFFFu;

    ca::u32 index = kInvalid;

    bool valid() const noexcept { return index != kInvalid; }
    bool operator==(const BlockId& o) const noexcept { return index == o.index; }
    bool operator!=(const BlockId& o) const noexcept { return index != o.index; }
    bool operator<(const BlockId& o) const noexcept { return index < o.index; }
};

/// @brief 「无此块」哨兵：空方法入口、未命中查询的返回值。
inline constexpr BlockId kNoBlock{};

/// @brief 控制流边种类。
enum class EdgeKind : ca::u8 {
    FallThrough = 0,  ///< 顺序落入下一块；条件分支的 not-taken 边同为此类
    Branch = 1,       ///< 无条件跳转的 taken 边（goto/goto_w/jsr/jsr_w）
    Condition = 2,    ///< 条件分支的 taken 边（not-taken 是 FallThrough 边）
    Switch = 3,       ///< switch 的 default/case 边（同一目标块去重为一条）
    Exception = 4,    ///< 异常边：try 覆盖区间内的块 → handler 块
};

/// @brief 控制流边：source → target。
///
/// excIndex 仅在 kind == Exception 时有意义，指向 CodeUnit::exc 的表项
/// 下标（catch 类型等细节回查该表项）；其余边该字段保持默认值。
struct Edge
{
    BlockId source;
    BlockId target;
    EdgeKind kind = EdgeKind::FallThrough;
    ca::u32 excIndex = 0;
};

/// @brief 基本块：一段无内部分支的连续指令。
///
/// entry 是块首指令（leader），同时是 insns.front()；insns 按 CodeDom
/// 程序序排列。块末条指令的形状决定出边（见 control_flow_graph.hpp）。
struct BasicBlock
{
    BlockId id;
    jbc::ir::InsnId entry;
    std::vector<jbc::ir::InsnId> insns;   ///< 顺序成员（含 entry）
};

/// @brief jbc::ir::InsnId 的折叠哈希（owner/slot/gen 三字段都参与相等性）。
struct InsnIdHash
{
    ca::usize operator()(const jbc::ir::InsnId& id) const noexcept
    {
        ca::usize h = id.owner;
        h = h * 1000003u ^ id.slot;
        h = h * 1000003u ^ id.gen;
        return h;
    }
};

}   // namespace mjbase::asm_cfg
