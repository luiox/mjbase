/// @file        match_error.hpp
/// @brief       matcher 模块错误码：窗口应用、DSL 解析/编译/求值各阶段共用。
#pragma once

#include "libca/core/datatype.hpp"

namespace mjbase::asm_matcher {

/// 结构化失败原因。
enum class MatchError : ca::u8 {
    Ok = 0,
    DslSyntax,        ///< DSL 文本不合语法（缺 =>、括号不配对、数字越界等）
    DslSemantic,      ///< DSL 结构合法但语义不可编译（origin 下标越界、同名绑定冲突等）
    UnknownOpcode,    ///< 助记符无法解析为 jbc::core::Op
    UnboundVariable,  ///< 表达式引用了未捕获的变量
    UnsupportedExpr,  ///< 不支持的运算符 / 类型标签 / cast 目标
    EmitUnsupported,  ///< 常量超出无 CP 指令形态可表达范围（如超 i16 的 int）
    EditRejected,     ///< CodeDom 拒绝编辑（StillUsed/InvalidHandle 等），窗口已回滚
};

/// 全模块错误值：kind + DSL 文本内偏移（仅表达式错误可给，其余为 0）。
struct MatcherError {
    MatchError kind = MatchError::Ok;
    ca::usize pos = 0;

    bool operator==(const MatcherError& o) const noexcept
    {
        return kind == o.kind && pos == o.pos;
    }
    bool operator!=(const MatcherError& o) const noexcept { return !(*this == o); }
};

/// 错误类别名（诊断输出用）。
const char* to_string(MatchError e) noexcept;

}   // namespace mjbase::asm_matcher
