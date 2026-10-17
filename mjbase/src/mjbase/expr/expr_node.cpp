#include "expr_node.hpp"

namespace mjbase::expr {

// ── BitWidth helpers ──

ca::i64 bitWidthMask(BitWidth bw) noexcept
{
    switch (bw) {
    case BitWidth::B8: return 0xFF;
    case BitWidth::B16: return 0xFFFF;
    case BitWidth::B32: return 0xFFFFFFFFull;
    case BitWidth::B64: return -1;   // all bits set
    }
    return -1;
}

ca::i64 truncateToBitWidth(ca::i64 val, BitWidth bw) noexcept
{
    ca::u64 mask      = static_cast<ca::u64>(bitWidthMask(bw));
    ca::u64 truncated = static_cast<ca::u64>(val) & mask;
    switch (bw) {
    case BitWidth::B8:
    {
        // sign-extend from bit 7
        ca::i8 v = static_cast<ca::i8>(truncated);
        return static_cast<ca::i64>(v);
    }
    case BitWidth::B16:
    {
        ca::i16 v = static_cast<ca::i16>(truncated);
        return static_cast<ca::i64>(v);
    }
    case BitWidth::B32:
    {
        ca::i32 v = static_cast<ca::i32>(truncated);
        return static_cast<ca::i64>(v);
    }
    case BitWidth::B64: return static_cast<ca::i64>(truncated);
    }
    return val;
}

// ── ExprNode ──

bool ExprNode::structuralEqual(const ExprNode& other) const noexcept
{
    if (kind() != other.kind()) return false;
    switch (kind()) {
    case NodeKind::Const:
    {
        auto& a = static_cast<const ConstNode&>(*this);
        auto& b = static_cast<const ConstNode&>(other);
        return a.value() == b.value() && a.bitWidth() == b.bitWidth();
    }
    case NodeKind::Var:
    {
        auto& a = static_cast<const VarNode&>(*this);
        auto& b = static_cast<const VarNode&>(other);
        return a.name() == b.name() && a.bitWidth() == b.bitWidth();
    }
    case NodeKind::UnaryOp:
    {
        auto& a = static_cast<const UnaryNode&>(*this);
        auto& b = static_cast<const UnaryNode&>(other);
        return a.op() == b.op() && a.child()->structuralEqual(*b.child());
    }
    case NodeKind::BinaryOp:
    {
        auto& a = static_cast<const BinaryNode&>(*this);
        auto& b = static_cast<const BinaryNode&>(other);
        return a.op() == b.op() && a.left()->structuralEqual(*b.left()) &&
               a.right()->structuralEqual(*b.right());
    }
    }
    return false;
}

// ── ConstNode ──

ConstNode::ConstNode(ca::i64 value, BitWidth bw) noexcept
    : value_(value)
    , bw_(bw)
{}

ca::i64 ConstNode::truncatedValue() const noexcept
{
    return truncateToBitWidth(value_, bw_);
}

// ── VarNode ──

VarNode::VarNode(std::string name, BitWidth bw) noexcept
    : name_(std::move(name))
    , bw_(bw)
{}

// ── UnaryNode ──

UnaryNode::UnaryNode(UnaryOpKind op, ExprRef child) noexcept
    : op_(op)
    , child_(std::move(child))
{}

BitWidth UnaryNode::bitWidth() const noexcept
{
    return child_->kind() == NodeKind::Const
               ? static_cast<const ConstNode*>(child_.get())->bitWidth()
               : BitWidth::B32;   // default fallback
}

// ── BinaryNode ──

BinaryNode::BinaryNode(BinOpKind op, ExprRef left, ExprRef right) noexcept
    : op_(op)
    , left_(std::move(left))
    , right_(std::move(right))
{}

BitWidth BinaryNode::bitWidth() const noexcept
{
    auto getBw = [](const ExprRef& n) -> BitWidth {
        if (n->kind() == NodeKind::Const) return static_cast<const ConstNode*>(n.get())->bitWidth();
        if (n->kind() == NodeKind::Var) return static_cast<const VarNode*>(n.get())->bitWidth();
        if (n->kind() == NodeKind::UnaryOp)
            return static_cast<const UnaryNode*>(n.get())->bitWidth();
        if (n->kind() == NodeKind::BinaryOp)
            return static_cast<const BinaryNode*>(n.get())->bitWidth();
        return BitWidth::B32;
    };
    BitWidth lb = getBw(left_);
    BitWidth rb = getBw(right_);
    // For bit-width: take the wider of the two
    return static_cast<BitWidth>(static_cast<ca::u8>(lb) > static_cast<ca::u8>(rb) ? lb : rb);
}

// ── Factory functions ──

ExprRef exprConst(ca::i64 val, BitWidth bw)
{
    return std::make_shared<const ConstNode>(val, bw);
}

ExprRef exprVar(std::string name, BitWidth bw)
{
    return std::make_shared<const VarNode>(std::move(name), bw);
}

ExprRef exprUnary(UnaryOpKind op, ExprRef child)
{
    return std::make_shared<const UnaryNode>(op, std::move(child));
}

ExprRef exprBinary(BinOpKind op, ExprRef left, ExprRef right)
{
    return std::make_shared<const BinaryNode>(op, std::move(left), std::move(right));
}

// ── Operators ──

bool operator==(const ExprNode& a, const ExprNode& b)
{
    return a.structuralEqual(b);
}

bool operator!=(const ExprNode& a, const ExprNode& b)
{
    return !a.structuralEqual(b);
}

}   // namespace mjbase::expr
