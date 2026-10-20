/// @file        statement_tree.hpp
/// @brief       保守结构化语句层：块级 if/循环恢复，语句与指令窗口双映射。
///
/// 定位是「可查询、可回写的最小语句层」，不是反编译器：只恢复基本块边界
/// 上形状完整的 if、while、for、do-while（体均为单个直线块），其余块保留
/// 为叶子。恢复结果同时保留每条语句覆盖的指令窗口，未恢复的控制流不会被
/// 丢弃或改写。
///
/// 写回只有一个通道：replaceLeaf 把叶子语句的替换序列经 CodeEditor 插回
/// 方法体，不新增 lower 路径。结构化节点本身不提供改写——if/循环的语义
/// 改写需要同时改边和补块，超出首版范围。
#pragma once

#include <memory>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/cfg/basic_block.hpp"
#include "mjbase/asm/cfg/control_flow_graph.hpp"

namespace mjbase::asm_stmt {

/// @brief 语句句柄：树节点密集下标（先子后父，根最后创建）。
struct StmtId
{
    static constexpr ca::u32 kInvalid = 0xFFFFFFFFu;

    ca::u32 index = kInvalid;

    bool valid() const noexcept { return index != kInvalid; }
    bool operator==(const StmtId& o) const noexcept { return index == o.index; }
    bool operator!=(const StmtId& o) const noexcept { return index != o.index; }
    bool operator<(const StmtId& o) const noexcept { return index < o.index; }
};

/// @brief 「无此语句」哨兵。
inline constexpr StmtId kNoStmt{};

/// @brief 语句种类。
enum class StmtKind : ca::u8 {
    Invalid = 0,   ///< 无效句柄（查询缺省值）
    Sequence = 1,  ///< 顺序列表；根节点固定是它
    Block = 2,     ///< 直线块：末尾允许 GOTO/return/athrow 或顺序落入
    Opaque = 3,    ///< 含条件/switch/jsr 等未恢复控制流的块
    If = 4,        ///< 条件分支 + 一或两个单块臂
    While = 5,     ///< 头部条件 + 单块体，体末尾回跳头部
    For = 6,       ///< 头部条件 + 单块体 + 单块更新，更新末尾回跳头部
    DoWhile = 7,   ///< 单块体 + 末尾条件块回跳体首
};

const char* to_string(StmtKind kind) noexcept;

/// @brief if 节点的分支事实。
///
/// 两个臂按程序序排在 children 里；哪个臂走 taken 边由 firstArmOnTaken 决定。
/// 单臂 if 只有 children[0]：它可能走 taken 边，也可能走 fall-through 边，
/// 另一边直接落到汇合块。
struct IfInfo
{
    jbc::ir::InsnId branch = jbc::ir::kNoInsn;
    bool firstArmOnTaken = true;
    bool hasSecondArm = false;
};

/// @brief 循环节点的条件事实。
///
/// bodyOnTaken 描述「继续循环的边」是否就是分支的 taken 边；循环退出走
/// 另一条边。For 的体执行完进入 update，再回跳头部。
struct LoopInfo
{
    jbc::ir::InsnId branch = jbc::ir::kNoInsn;
    bool bodyOnTaken = true;
};

/// @brief 收集失败原因。
enum class StatementError : ca::u8 {
    Ok = 0,
    CfgBuild,   ///< CFG 构建失败（分支/switch/异常表标签无法解析）
};

const char* to_string(StatementError error) noexcept;

/// @brief 方法体的结构化语句树（自持，move-only）。
///
/// 树不持有 CodeUnit；指令句柄在方法编辑后失效，需要重新 build。块序、
/// 边与指令窗口都是构建时的快照。异常边不参与结构化：try/catch 不会
/// 变成语句，handler 块按程序序作为普通块处理。
class StatementTree
{
public:
    StatementTree();
    ~StatementTree();

    StatementTree(StatementTree&& other) noexcept;
    StatementTree& operator=(StatementTree&& other) noexcept;
    StatementTree(const StatementTree&) = delete;
    StatementTree& operator=(const StatementTree&) = delete;

    /// @brief 从方法体恢复语句树（读 dom 顺序、exc 表；不修改 unit）。
    ///
    /// 空方法返回只有根 Sequence 的树。CFG 构建失败时返回 Err，不产出
    /// 部分结果。
    static ca::core::Result<StatementTree, StatementError> build(jbc::ir::CodeUnit& unit);

    /// @brief 构建时的 CFG 快照（块序与边）。
    const asm_cfg::ControlFlowGraph& cfg() const noexcept;

    /// @brief 根节点（永远是 Sequence）；空树构造的默认对象返回 kNoStmt。
    StmtId root() const noexcept;

    /// @brief 节点总数（含根）。
    ca::usize size() const noexcept;

    /// @brief 句柄是否属于本树。
    bool contains(StmtId id) const noexcept;

    /// @brief 节点种类；无效句柄返回 Invalid。
    StmtKind kindOf(StmtId id) const noexcept;

    /// @brief 父节点；根与无效句柄返回 kNoStmt。
    StmtId parentOf(StmtId id) const noexcept;

    /// @brief 子节点（程序序）；无效句柄返回空表。
    const std::vector<StmtId>& childrenOf(StmtId id) const noexcept;

    /// @brief 子树覆盖的全部指令（程序序）；无效句柄返回空表。
    const std::vector<jbc::ir::InsnId>& insnsOf(StmtId id) const noexcept;

    /// @brief 子树首/末条指令；空子树或无效句柄返回 kNoInsn。
    jbc::ir::InsnId firstInsnOf(StmtId id) const noexcept;
    jbc::ir::InsnId lastInsnOf(StmtId id) const noexcept;

    /// @brief 叶子节点所属基本块；结构节点与无效句柄返回 kNoBlock。
    asm_cfg::BlockId blockOf(StmtId id) const noexcept;

    /// @brief 指令的最内层归属：构造头/条件块属于结构节点，For 的更新块与
    ///        各循环体一样是结构节点的子叶。
    ///
    /// 无效句柄或不属于本方法的指令返回 kNoStmt。该查询是「语句 undo 到
    /// 指令窗口」的入口：撤销一条语句时按 insnsOf 取得窗口。
    StmtId stmtOf(jbc::ir::InsnId insn) const noexcept;

    /// @brief 块的直接归属节点；不属于本方法的块返回 kNoStmt。
    StmtId stmtOfBlock(asm_cfg::BlockId block) const noexcept;

    /// @brief If 节点的分支事实；非 If 或无效句柄返回 nullptr。
    const IfInfo* ifInfoOf(StmtId id) const noexcept;

    /// @brief 循环节点的条件事实；非循环或无效句柄返回 nullptr。
    const LoopInfo* loopInfoOf(StmtId id) const noexcept;

    /// @brief If 是否有第二个臂。
    bool hasSecondArm(StmtId ifStmt) const noexcept;

    /// @brief 走 taken 边的臂；该方向没有臂（单臂 if 的另一边直接汇合）返回 kNoStmt。
    StmtId takenArmOf(StmtId ifStmt) const noexcept;

    /// @brief 走 fall-through 边的臂；该方向没有臂返回 kNoStmt。
    StmtId fallThroughArmOf(StmtId ifStmt) const noexcept;

    /// @brief 循环体；非循环返回 kNoStmt。
    StmtId loopBodyOf(StmtId loop) const noexcept;

    /// @brief For 的更新块；While/DoWhile 或非循环返回 kNoStmt。
    StmtId loopUpdateOf(StmtId loop) const noexcept;

private:
    struct Impl;

    explicit StatementTree(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    friend struct StatementBuilder;
};

/// @brief 叶子语句替换失败原因。
enum class StmtEditError : ca::u8 {
    Ok = 0,
    InvalidStmt,           ///< 句柄不属于本树或不是叶子
    NotLeaf,               ///< 只支持 Block/Opaque 叶子
    StaleWindow,           ///< 窗口句柄已失效（树过期或方法已被编辑）
    ExceptionBoundary,     ///< 异常表边界落在窗口内
    ExternallyReferenced,  ///< 窗口内指令仍被分支/switch 引用
    LabelAttached,         ///< 窗口内指令仍挂着边界标签
    EditFailed,            ///< CodeEditor 拒绝插入或删除（已尽量回滚插入）
};

const char* to_string(StmtEditError error) noexcept;

/// @brief 用替换序列改写叶子语句，写回经 CodeEditor。
///
/// 前置条件（全部满足才落变更，否则什么都不改）：
/// - leaf 是 Block/Opaque 叶子，窗口等于整个基本块；
/// - 窗口内指令未被分支/switch 引用、未挂任何标签、不是异常表边界；
/// - 替换序列中的跳转目标标签已在本 DOM 锚定。
///
/// 替换序列整体插在窗口之前，随后删除原窗口；空序列等价于删除该语句。
/// 返回新语句首条指令，删除时返回 kNoInsn。成功返回后旧树与旧句柄失效，
/// 调用方必须重新 build，不能拿 old tree 继续查询。
ca::core::Result<jbc::ir::InsnId, StmtEditError>
replaceLeaf(jbc::ir::CodeUnit& unit, const StatementTree& tree, StmtId leaf,
            std::vector<jbc::ir::Insn> replacement);

}   // namespace mjbase::asm_stmt
