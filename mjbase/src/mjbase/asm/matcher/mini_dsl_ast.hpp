/// @file        mini_dsl_ast.hpp
/// @brief       mini DSL 的解析产物（纯 ASCII 文本域，不接触字节码语义）。
///
/// DSL 的变量名/运算符/类型标签/JVM 名是 parser 的词法产物（构造期数据，
/// 编译后由策略闭包持有），沿用 std::string；模块对外 API 仍是
/// std::string_view 借用视图。
#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "libca/core/datatype.hpp"

namespace mjbase::asm_matcher::dsl {

struct ExprNode;
using ExprPtr = std::shared_ptr<ExprNode>;

/// 算术/位运算表达式树：整型字面量、变量引用、一元负、二元运算、cast。
struct ExprNode
{
    struct IntLit
    {
        ca::i64 value;
    };
    struct VarRef
    {
        std::string name;
    };
    struct Unary
    {
        std::string op;
        ExprPtr operand;
    };
    struct Binary
    {
        std::string op;
        ExprPtr left, right;
    };
    struct Cast
    {
        std::string targetType;
        ExprPtr inner;
    };

    using Var = std::variant<IntLit, VarRef, Unary, Binary, Cast>;
    Var data;

    template <typename T>
    explicit ExprNode(T&& v)
        : data(std::forward<T>(v))
    {}
};

inline ExprPtr makeExpr(ExprNode::Var&& v)
{
    return std::make_shared<ExprNode>(std::move(v));
}

/// 模式侧单步。
struct PatternStep
{
    struct Opcode
    {
        std::string name;
    };
    /// load(<type>[:=<const>][:<bind>])：类型常量装载，可带值约束与命名绑定。
    struct ConstLoad
    {
        std::string typeTag;
        ca::i64 constVal = 0;
        bool hasConst = false;
        std::string bind;
    };
    struct VarLoad
    {
        std::string typeTag;
        std::string slotName;
    };
    struct VarStore
    {
        std::string typeTag;
        std::string slotName;
    };
    /// 语法保留：owner/name/desc 匹配需要常量池语义，编译期明确拒绝。
    struct Method
    {
        std::string opcode, owner, name, desc;
    };

    using Var = std::variant<Opcode, ConstLoad, VarLoad, VarStore, Method>;
    Var data;

    PatternStep()
        : data(Opcode{})
    {}
    explicit PatternStep(Var v)
        : data(std::move(v))
    {}
};

/// 替换侧单步。
struct ReplaceStep
{
    struct Nothing
    {};
    struct Opcode
    {
        std::string name;
    };
    /// origin[i] / origin[a..b]：按窗口内步骤下标克隆原指令（闭区间含 b）。
    struct OriginRange
    {
        int start;
        int endExclusive;
    };
    /// 语法保留：同 PatternStep::Method，编译期明确拒绝。
    struct Method
    {
        std::string opcode, owner, name, desc;
    };

    using Var = std::variant<Nothing, Opcode, OriginRange, Method>;
    Var data;
    ExprPtr loadExpr;          // 非 null 时本步是常量装载发射
    std::string loadExprType;

    ReplaceStep()
        : data(Nothing{})
    {}
    explicit ReplaceStep(Var v)
        : data(std::move(v))
    {}

    static ReplaceStep makeLoadExpr(std::string typeTag, ExprPtr expr)
    {
        ReplaceStep s;
        s.loadExpr = std::move(expr);
        s.loadExprType = std::move(typeTag);
        return s;
    }

    bool isLoadExpr() const { return loadExpr != nullptr; }
};

/// 一条规则：模式序列 => 替换序列。
struct Rule
{
    std::vector<PatternStep> matchSteps;
    std::vector<ReplaceStep> replaceSteps;
};

}   // namespace mjbase::asm_matcher::dsl
