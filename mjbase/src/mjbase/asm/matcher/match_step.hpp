/// @file        match_step.hpp
/// @brief       线性指令序列模式的单步约束：具体 Op / 通配 / 贪婪多步 / 组合子。
///
/// 步骤是虚接口 + 裸指针 +
/// 进程级所有权登记表；本实现是值语义：一步是一棵可自由拷贝/移动的
/// 组合子树，随 MatchRule 一起存活，无生命周期负担。谓词只读单条指令
/// （@ref jbc::ir::Insn 值），不再接触节点指针与列表游标。
#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

#include "libca/core/datatype.hpp"

namespace mjbase::asm_matcher {

/// 单指令谓词：借用视图；匹配期间不得修改 DOM。
using InsnPredicate = std::function<bool(const jbc::ir::Insn&)>;

/// @brief 窗口内当前位置的一步约束，消耗一或多条指令。
///
/// 步骤种类（对齐旧 SimpleStep/AnyStep/AnyMultiStep/AndStep/OrStep/NotStep）：
///   - op / opIf ：具体 Op（可带操作数谓词），恒消耗 1 条；
///   - any / anyIf：通配（可带谓词），恒消耗 1 条；
///   - anyMulti  ：贪婪吞掉序列内全部剩余指令，消耗数动态（跳过
///                 Label/Line/Frame 伪节点后吞到表尾；新栈 order 内全是
///                 真指令，等价于吞到末尾，窗口边界纪律由 MatchRule 统一执行）；
///   - allOf / anyOf / notOf：同一条指令上的逻辑组合，恒消耗 1 条。
class MatchStep
{
public:
    /// 具体 Op。
    static MatchStep op(jbc::core::Op opcode);
    /// 具体 Op + 操作数谓词（约束 a/b/cp 等字段）。
    static MatchStep opIf(jbc::core::Op opcode, InsnPredicate pred);
    /// 任意指令。
    static MatchStep any();
    /// 任意满足谓词的指令。
    static MatchStep anyIf(InsnPredicate pred);
    /// 贪婪多步：吞掉全部剩余指令。
    static MatchStep anyMulti();
    /// 全部子步骤须对同一条指令命中。
    static MatchStep allOf(std::vector<MatchStep> steps);
    /// 第一个命中的子步骤胜出。
    static MatchStep anyOf(std::vector<MatchStep> steps);
    /// 取反。
    static MatchStep notOf(MatchStep inner);

    /// 命名捕获该步命中的指令（仅 MatchRule 的顶层步骤生效；名字借用
    /// 调用方存储，存活期须覆盖匹配）。链式用法：
    /// `MatchStep::opIf(...).captureAs("x")`。
    MatchStep captureAs(std::string_view name) &&;

    /// @brief 对当前位置指令求值；remaining 是包含当前指令的剩余条数
    ///        （anyMulti 据此确定贪婪消耗量）。
    bool tryMatch(const jbc::ir::Insn& insn, ca::usize remaining) const;

    /// 上次 tryMatch 成功时消耗的指令数。
    ca::usize matchedSize() const noexcept { return matchedSize_; }

    bool hasCapture() const noexcept { return !capture_.empty(); }
    /// 捕获名（借用视图）。
    std::string_view capture() const noexcept { return capture_; }

private:
    enum class Kind : ca::u8 {
        Op = 0,
        Any,
        AnyMulti,
        All,
        Or,
        Not,
    };

    Kind kind_ = Kind::Any;
    jbc::core::Op op_ = jbc::core::Op::NOP;
    InsnPredicate pred_;
    std::vector<MatchStep> children_;   // All/Or/Not 的子步骤（Not 恰一个）
    std::string_view capture_;
    mutable ca::usize matchedSize_ = 1;   // tryMatch 的输出槽（const 语义）
};

}   // namespace mjbase::asm_matcher
