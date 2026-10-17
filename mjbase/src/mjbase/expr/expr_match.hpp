#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "expr_node.hpp"

namespace mjbase::expr {

/// Match bindings: capture name -> matched sub-expression
using MatchBindings = std::unordered_map<std::string, ExprRef>;

/// Pattern node — describes a tree structure to match against.
class Pattern
{
public:
    enum class Kind
    {
        Wild,
        Capture,
        Const,
        Var,
        UnaryOp,
        BinaryOp,
    };

    // Factory methods
    static Pattern Wild();
    static Pattern Capture(std::string name);
    static Pattern Const(ca::i64 val);
    static Pattern Var(std::string name);
    static Pattern UnaryOp(UnaryOpKind op, Pattern child);
    static Pattern BinOp(BinOpKind op, Pattern left, Pattern right);

    Kind               kind() const noexcept { return kind_; }
    const std::string& name() const noexcept { return name_; }
    ca::i64            constVal() const noexcept { return constVal_; }
    UnaryOpKind        unaryOp() const noexcept { return unaryOp_; }
    BinOpKind          binaryOp() const noexcept { return binaryOp_; }
    const Pattern*     leftChild() const noexcept { return leftChild_.get(); }
    const Pattern*     rightChild() const noexcept { return rightChild_.get(); }

    /// Copy constructor (deep copy child patterns)
    Pattern(const Pattern& other)
        : kind_(other.kind_)
        , name_(other.name_)
        , constVal_(other.constVal_)
        , unaryOp_(other.unaryOp_)
        , binaryOp_(other.binaryOp_)
        , leftChild_(other.leftChild_ ? std::make_unique<Pattern>(*other.leftChild_) : nullptr)
        , rightChild_(other.rightChild_ ? std::make_unique<Pattern>(*other.rightChild_) : nullptr)
    {}

    Pattern& operator=(const Pattern& other)
    {
        if (this != &other) {
            kind_      = other.kind_;
            name_      = other.name_;
            constVal_  = other.constVal_;
            unaryOp_   = other.unaryOp_;
            binaryOp_  = other.binaryOp_;
            leftChild_ = other.leftChild_ ? std::make_unique<Pattern>(*other.leftChild_) : nullptr;
            rightChild_ =
                other.rightChild_ ? std::make_unique<Pattern>(*other.rightChild_) : nullptr;
        }
        return *this;
    }

    Pattern(Pattern&&)            = default;
    Pattern& operator=(Pattern&&) = default;

    /// Try to match. On success, bindings are populated with captures.
    bool match(ExprRef node, MatchBindings& bindings) const;

private:
    Pattern(Kind kind)
        : kind_(kind)
    {}

    Kind                     kind_;
    std::string              name_;           // for Capture / Var
    ca::i64                  constVal_ = 0;   // for Const
    UnaryOpKind              unaryOp_  = UnaryOpKind::Neg;
    BinOpKind                binaryOp_ = BinOpKind::Add;
    std::unique_ptr<Pattern> leftChild_;    // for UnaryOp / BinaryOp
    std::unique_ptr<Pattern> rightChild_;   // for BinaryOp
};

/// Rewrite rule: lhs pattern -> rhs builder
struct RewriteRule
{
    Pattern                                      lhs;
    std::function<ExprRef(const MatchBindings&)> rhs;
    std::string                                  name;
};

/// Priority — lower number = higher priority
using RulePriority = ca::i32;

/// Rule with priority
struct PrioritizedRule
{
    RulePriority priority = 0;
    RewriteRule  rule;
};

/// Rewrite engine: top-down recursive rewrite with depth guard.
ExprRef rewrite(ExprRef expr, const std::vector<PrioritizedRule>& rules, ca::u32 maxIter = 10,
                ca::u32 maxDepth = 1024);

// ── Identity group (shared with simplify) ──

enum class IdentityGroup
{
    Simplify,
    Obfuscate,
    Both
};

}   // namespace mjbase::expr
