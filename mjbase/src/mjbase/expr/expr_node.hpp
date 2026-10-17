#pragma once

#include <memory>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

namespace mjbase::expr {

// ── Enums ──

enum class BitWidth : ca::u8
{
    B8  = 8,
    B16 = 16,
    B32 = 32,
    B64 = 64,
};

enum class BinOpKind
{
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    And,
    Or,
    Xor,
    Shl,
    Shr,
    UShr,
};

enum class UnaryOpKind
{
    Neg,
    Not,
};

enum class NodeKind
{
    Const,
    Var,
    UnaryOp,
    BinaryOp,
};

// ── Forward ──

class ExprNode;
using ExprRef = std::shared_ptr<const ExprNode>;

// ── Base node ──

class ExprNode
{
public:
    virtual ~ExprNode()                    = default;
    virtual NodeKind kind() const noexcept = 0;

    /// Structural equality (deep compare)
    bool structuralEqual(const ExprNode& other) const noexcept;

protected:
    ExprNode()                           = default;
    ExprNode(const ExprNode&)            = delete;
    ExprNode& operator=(const ExprNode&) = delete;
};

// ── Const node ──

class ConstNode final : public ExprNode
{
public:
    ConstNode(ca::i64 value, BitWidth bw) noexcept;

    NodeKind kind() const noexcept override { return NodeKind::Const; }
    ca::i64  value() const noexcept { return value_; }
    BitWidth bitWidth() const noexcept { return bw_; }

    /// Value truncated to bit width (two's complement)
    ca::i64 truncatedValue() const noexcept;

private:
    ca::i64  value_;
    BitWidth bw_;
};

// ── Var node ──

class VarNode final : public ExprNode
{
public:
    VarNode(std::string name, BitWidth bw) noexcept;

    NodeKind           kind() const noexcept override { return NodeKind::Var; }
    const std::string& name() const noexcept { return name_; }
    BitWidth           bitWidth() const noexcept { return bw_; }

private:
    std::string name_;
    BitWidth    bw_;
};

// ── Unary node ──

class UnaryNode final : public ExprNode
{
public:
    UnaryNode(UnaryOpKind op, ExprRef child) noexcept;

    NodeKind       kind() const noexcept override { return NodeKind::UnaryOp; }
    UnaryOpKind    op() const noexcept { return op_; }
    const ExprRef& child() const noexcept { return child_; }
    BitWidth       bitWidth() const noexcept;

private:
    UnaryOpKind op_;
    ExprRef     child_;
};

// ── Binary node ──

class BinaryNode final : public ExprNode
{
public:
    BinaryNode(BinOpKind op, ExprRef left, ExprRef right) noexcept;

    NodeKind       kind() const noexcept override { return NodeKind::BinaryOp; }
    BinOpKind      op() const noexcept { return op_; }
    const ExprRef& left() const noexcept { return left_; }
    const ExprRef& right() const noexcept { return right_; }
    BitWidth       bitWidth() const noexcept;

private:
    BinOpKind op_;
    ExprRef   left_;
    ExprRef   right_;
};

// ── Factory functions ──

ExprRef exprConst(ca::i64 val, BitWidth bw = BitWidth::B32);
ExprRef exprVar(std::string name, BitWidth bw = BitWidth::B32);
ExprRef exprUnary(UnaryOpKind op, ExprRef child);
ExprRef exprBinary(BinOpKind op, ExprRef left, ExprRef right);

// Semantic aliases
inline ExprRef exprAdd(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::Add, std::move(a), std::move(b));
}
inline ExprRef exprSub(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::Sub, std::move(a), std::move(b));
}
inline ExprRef exprMul(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::Mul, std::move(a), std::move(b));
}
inline ExprRef exprDiv(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::Div, std::move(a), std::move(b));
}
inline ExprRef exprRem(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::Rem, std::move(a), std::move(b));
}
inline ExprRef exprAnd(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::And, std::move(a), std::move(b));
}
inline ExprRef exprOr(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::Or, std::move(a), std::move(b));
}
inline ExprRef exprXor(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::Xor, std::move(a), std::move(b));
}
inline ExprRef exprShl(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::Shl, std::move(a), std::move(b));
}
inline ExprRef exprShr(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::Shr, std::move(a), std::move(b));
}
inline ExprRef exprUShr(ExprRef a, ExprRef b)
{
    return exprBinary(BinOpKind::UShr, std::move(a), std::move(b));
}
inline ExprRef exprNeg(ExprRef a)
{
    return exprUnary(UnaryOpKind::Neg, std::move(a));
}
inline ExprRef exprNot(ExprRef a)
{
    return exprUnary(UnaryOpKind::Not, std::move(a));
}

// ── Structural comparison ──

bool operator==(const ExprNode& a, const ExprNode& b);
bool operator!=(const ExprNode& a, const ExprNode& b);

// ── BitWidth helpers ──

/// Mask for the given bit width (e.g. B32 => 0xFFFFFFFF)
ca::i64 bitWidthMask(BitWidth bw) noexcept;

/// Truncate value to bit width (two's complement sign-extension)
ca::i64 truncateToBitWidth(ca::i64 val, BitWidth bw) noexcept;

}   // namespace mjbase::expr
