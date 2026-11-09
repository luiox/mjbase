#include "expr_match.hpp"
#include "expr_print.hpp"

#include <unordered_set>

namespace mjbase::expr {

// ── Pattern factory ──

Pattern Pattern::Wild()
{
    return Pattern(Kind::Wild);
}
Pattern Pattern::Capture(std::string_view n)
{
    Pattern p(Kind::Capture);
    p.name_ = ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(n.data()), n.size());
    return p;
}
Pattern Pattern::Const(ca::i64 v)
{
    Pattern p(Kind::Const);
    p.constVal_ = v;
    return p;
}
Pattern Pattern::Var(std::string_view n)
{
    Pattern p(Kind::Var);
    p.name_ = ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(n.data()), n.size());
    return p;
}
Pattern Pattern::UnaryOp(UnaryOpKind o, Pattern c)
{
    Pattern p(Kind::UnaryOp);
    p.unaryOp_   = o;
    p.leftChild_ = std::make_unique<Pattern>(std::move(c));
    return p;
}
Pattern Pattern::BinOp(BinOpKind o, Pattern l, Pattern r)
{
    Pattern p(Kind::BinaryOp);
    p.binaryOp_   = o;
    p.leftChild_  = std::make_unique<Pattern>(std::move(l));
    p.rightChild_ = std::make_unique<Pattern>(std::move(r));
    return p;
}

// ── Pattern::match ──

bool Pattern::match(ExprRef node, MatchBindings& bindings) const
{
    if (!node) return false;

    switch (kind_) {
    case Kind::Wild: return true;

    case Kind::Capture:
    {
        if (name_.is_empty()) return true;
        auto it = bindings.find(name_);
        if (it != bindings.end()) {
            return it->second->structuralEqual(*node);
        }
        bindings.emplace(name_.clone(), node);
        return true;
    }

    case Kind::Const:
        if (node->kind() != NodeKind::Const) return false;
        return static_cast<const ConstNode&>(*node).value() == constVal_;

    case Kind::Var:
        if (node->kind() != NodeKind::Var) return false;
        return static_cast<const VarNode&>(*node).name() == name_.ref();

    case Kind::UnaryOp:
        if (node->kind() != NodeKind::UnaryOp) return false;
        if (static_cast<const UnaryNode&>(*node).op() != unaryOp_) return false;
        return leftChild_ &&
               leftChild_->match(static_cast<const UnaryNode&>(*node).child(), bindings);

    case Kind::BinaryOp:
        if (node->kind() != NodeKind::BinaryOp) return false;
        if (static_cast<const BinaryNode&>(*node).op() != binaryOp_) return false;
        if (!leftChild_ ||
            !leftChild_->match(static_cast<const BinaryNode&>(*node).left(), bindings))
            return false;
        return rightChild_ &&
               rightChild_->match(static_cast<const BinaryNode&>(*node).right(), bindings);
    }
    return false;
}

// ── Rewrite ──

namespace {

/// 规则经指针借用分组（PrioritizedRule 携 Utf8String 名字，禁拷贝）。
using RuleGroup = std::vector<const PrioritizedRule*>;

ExprRef applyPass(ExprRef expr, const std::vector<RuleGroup>& groups, ca::u32 depth,
                  ca::u32 maxDepth)
{
    if (!expr || depth > maxDepth) return expr;

    ExprRef rewritten;
    switch (expr->kind()) {
    case NodeKind::BinaryOp:
    {
        auto& bn       = static_cast<const BinaryNode&>(*expr);
        auto  newLeft  = applyPass(bn.left(), groups, depth + 1, maxDepth);
        auto  newRight = applyPass(bn.right(), groups, depth + 1, maxDepth);
        if (newLeft.get() == bn.left().get() && newRight.get() == bn.right().get())
            rewritten = expr;
        else
            rewritten = exprBinary(bn.op(), std::move(newLeft), std::move(newRight));
        break;
    }
    case NodeKind::UnaryOp:
    {
        auto& un       = static_cast<const UnaryNode&>(*expr);
        auto  newChild = applyPass(un.child(), groups, depth + 1, maxDepth);
        if (newChild.get() == un.child().get())
            rewritten = expr;
        else
            rewritten = exprUnary(un.op(), std::move(newChild));
        break;
    }
    default: rewritten = expr; break;
    }

    for (const auto& group : groups) {
        for (const auto* pr : group) {
            MatchBindings bindings;
            if (pr->rule.lhs.match(rewritten, bindings)) {
                return pr->rule.rhs(bindings);
            }
        }
    }

    return rewritten;
}

}   // anonymous namespace

ExprRef rewrite(ExprRef expr, const std::vector<PrioritizedRule>& rules, ca::u32 maxIter,
                ca::u32 maxDepth)
{
    std::map<RulePriority, RuleGroup> groups;
    for (const auto& r : rules) groups[r.priority].push_back(&r);

    std::vector<RuleGroup> ordered;
    for (auto& [_, grp] : groups) ordered.push_back(std::move(grp));

    ExprRef                     current = expr;
    std::unordered_set<ca::u64> fingerprints;

    for (ca::u32 iter = 0; iter < maxIter; ++iter) {
        ExprRef before = current;

        current = applyPass(current, ordered, 0, maxDepth);

        if (current.get() == before.get()) break;

        const auto sexpr = toSExpr(*current);
        auto    fp   = std::hash<ca::str::Utf8String>{}(sexpr);
        if (!fingerprints.insert(fp).second) break;
    }

    return current;
}

}   // namespace mjbase::expr
