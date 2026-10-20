/// @file        path_conditions.hpp
/// @brief       路径条件收集：分支谓词贴边、全路径事实交集与分支走向查询。
///
/// 定位是「域条件化」，不是符号执行、更不是 SMT：在抽象域（const / branch
/// facts 族）之上，把每条条件分支的谓词贴到 CFG 的 taken / fall-through
/// 边上，用 must 分析（汇合取交集）求「所有到达路径都成立」的整数约束集。
/// 谓词表达式复用 expr 模块，常量折叠与恒等式化简复用 expr_simplify。
///
/// 事实的可靠性来自两条纪律：
/// - 边约束是路径假设：taken 边断言谓词，fall-through 边断言其否定；汇合
///   时只保留所有入边共有的原子（某条路径不成立的约束被交集丢弃）。
/// - 符号值的身份 = 产值点（参数按 local 槽，其余按指令句柄）；汇合时结构
///   不同的表达式退化为未知。产值点在方法内稳定，回边只会让 header 处的
///   事实被入口边稀释，不会把某次迭代的约束带给下一次。
///
/// 查询面回答「此分支在所有到达路径上恒真/恒假/未知」；pruneDecidedBranches
/// 是该查询的一个消费样例（恒真改写 GOTO_W、恒假改写 POP/POP2，不重构 CFG）。
#pragma once

#include <memory>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/cfg/basic_block.hpp"
#include "mjbase/expr/expr_node.hpp"

namespace mjbase::asm_analyzer {

/// @brief 整数比较关系（JVM 有符号语义）。
enum class PathCmp : ca::u8 {
    Eq = 0,   ///< lhs == rhs
    Ne = 1,   ///< lhs != rhs
    Lt = 2,   ///< lhs < rhs
    Le = 3,   ///< lhs <= rhs
    Gt = 4,   ///< lhs > rhs
    Ge = 5,   ///< lhs >= rhs
};

const char* to_string(PathCmp cmp) noexcept;

/// @brief 逻辑否定：Eq↔Ne，Lt↔Ge，Le↔Gt。
PathCmp negate(PathCmp cmp) noexcept;

/// @brief 一条整数约束 lhs <cmp> rhs。
///
/// 表达式是 expr 模块的不可变节点，不含位宽标注（位宽在常量节点上）；两侧
/// 都是常量时 constantValue 可直接判定。相同表达式的约束按结构相等比较。
struct PathConstraint
{
    PathCmp cmp = PathCmp::Eq;
    expr::ExprRef lhs;
    expr::ExprRef rhs;

    /// @brief 两侧均可折为常量时输出比较结果并返回 true；否则 false。
    bool constantValue(bool& value) const noexcept;
};

bool operator==(const PathConstraint& a, const PathConstraint& b) noexcept;
bool operator!=(const PathConstraint& a, const PathConstraint& b) noexcept;

/// @brief 分支条件在全路径上的真值。
enum class PathTruth : ca::u8 {
    Unknown = 0,    ///< 存在到达路径无法判定（或不适用）
    AlwaysTrue = 1,   ///< 所有到达路径上条件成立：恒走跳转目标
    AlwaysFalse = 2,  ///< 所有到达路径上条件不成立：恒走顺序后继
};

const char* to_string(PathTruth truth) noexcept;

/// @brief 收集失败原因。
enum class PathConditionError : ca::u8 {
    Ok = 0,
    CfgBuild,   ///< CFG 构建失败（分支/switch/异常表标签无法解析）
    Analysis,   ///< 符号域分析失败（形状/熔断），帧不可用
};

const char* to_string(PathConditionError error) noexcept;

/// @brief 路径条件结果（自持，move-only）。
///
/// 事实与查询在收集时算好，结果不借用 CodeUnit 或常量池。分析失败
/// （ok()==false）时事实为空、全部查询退回 Unknown——失败面 fail-closed。
class PathConditionResult
{
public:
    PathConditionResult();
    ~PathConditionResult();

    PathConditionResult(PathConditionResult&& other) noexcept;
    PathConditionResult& operator=(PathConditionResult&& other) noexcept;
    PathConditionResult(const PathConditionResult&) = delete;
    PathConditionResult& operator=(const PathConditionResult&) = delete;

    const jbc::analysis::FrameSummary& summary() const noexcept;
    bool ok() const noexcept;
    PathConditionError error() const noexcept;

    /// @brief 指令所在块；无效/不属于该方法返回 kNoBlock。
    asm_cfg::BlockId blockOf(jbc::ir::InsnId insn) const noexcept;

    /// @brief 该块是否被路径事实分析视为可达。
    ///
    /// 只在所有入边都被证明不可行时为 false；异常边照常传播。查询不适用
    /// 于无效块（返回 false）。
    bool blockReachable(asm_cfg::BlockId block) const noexcept;

    /// @brief 块入口在所有到达路径上均成立的约束（规范化序）。
    ///
    /// 不可达块返回空表。表内约束是 must 事实：每条在所有到达路径上成立，
    /// 但不同约束之间不必来自同一条路径（交集语义）。
    const std::vector<PathConstraint>& constraintsAt(asm_cfg::BlockId block) const noexcept;

    /// @brief 条件分支在全路径上的走向。
    ///
    /// 只覆盖整数条件分支（IFEQ..IFLE / IF_ICMPxx）；null 系与 switch 在
    /// 本层不做符号推理，返回 Unknown。不可达块内的分支同样返回 Unknown。
    PathTruth truthOf(jbc::ir::InsnId branch) const noexcept;

private:
    struct Impl;

    explicit PathConditionResult(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    friend PathConditionResult collectPathConditions(const jbc::core::ConstantPool& cp,
                                                     const jbc::ir::CodeUnit& unit,
                                                     const jbc::analysis::MethodSig* sig);
};

/// @brief 收集方法体的路径条件事实。
///
/// @param cp  方法所在常量池（借用；ldc 常量折叠需要，须活过本调用）。
/// @param unit 方法体（借用；不修改）。
/// @param sig 入参播种签名；非空时实例方法 this 占 0 号槽、参数按描述符
///            落槽并以符号变量身份参与约束。nullptr 表示不播种——未写过的
///            局部槽读值不可跨读点关联（保守退化为独立符号）。
PathConditionResult collectPathConditions(const jbc::core::ConstantPool& cp,
                                          const jbc::ir::CodeUnit& unit,
                                          const jbc::analysis::MethodSig* sig = nullptr);

/// @brief 按路径事实改写已判定的条件分支（死分支剔除样例）。
///
/// 恒真：在分支前插入 POP/POP2 复现操作数出栈，原分支改写为 GOTO_W；
/// 恒假：原分支改写为 POP/POP2（栈效应与原条件分支一致）。未知分支不动。
/// 只改指令不重构 CFG——调用方需要重建 CFG/分析视图。返回被改写的分支句柄
/// （恒真分支改写后仍是 GOTO_W；恒假分支句柄现在指向 POP/POP2）。
///
/// 事实必须来自同一方法体且收集后未编辑过；过期句柄按探测式跳过。
std::vector<jbc::ir::InsnId> pruneDecidedBranches(jbc::ir::CodeUnit& unit,
                                                  const PathConditionResult& facts);

}   // namespace mjbase::asm_analyzer
