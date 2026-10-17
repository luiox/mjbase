#include "expr_match.hpp"
#include "expr_print.hpp"

#include <unordered_set>

namespace mjbase::expr {

// ── Pattern factory ──

Pattern Pattern::Wild()
{
    return Pattern(Kind::Wild);
}
Pattern Pattern::Capture(std::string n)
{
    Pattern p(Kind::Capture);
    p.name_ = std::move(n);
    return p;
}
Pattern Pattern::Const(ca::i64 v)
{
    Pattern p(Kind::Const);
    p.constVal_ = v;
    return p;
}
Pattern Pattern::Var(std::string n)
{
    Pattern p(Kind::Var);
    p.name_ = std::move(n);
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
        if (name_.empty()) return true;
        auto it = bindings.find(name_);
        if (it != bindings.end()) {
            return it->second->structuralEqual(*node);
        }
        bindings[name_] = node;
        return true;
    }

    case Kind::Const:
        if (node->kind() != NodeKind::Const) return false;
        return static_cast<const ConstNode&>(*node).value() == constVal_;

    case Kind::Var:
        if (node->kind() != NodeKind::Var) return false;
        return static_cast<const VarNode&>(*node).name() == name_;

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

ExprRef applyPass(ExprRef expr, const std::vector<PrioritizedRule>& rules, ca::u32 depth,
                  ca::u32 maxDepth)
{
    if (!expr || depth > maxDepth) return expr;

    ExprRef rewritten;
    switch (expr->kind()) {
    case NodeKind::BinaryOp:
    {
        auto& bn       = static_cast<const BinaryNode&>(*expr);
        auto  newLeft  = applyPass(bn.left(), rules, depth + 1, maxDepth);
        auto  newRight = applyPass(bn.right(), rules, depth + 1, maxDepth);
        if (newLeft.get() == bn.left().get() && newRight.get() == bn.right().get())
            rewritten = expr;
        else
            rewritten = exprBinary(bn.op(), std::move(newLeft), std::move(newRight));
        break;
    }
    case NodeKind::UnaryOp:
    {
        auto& un       = static_cast<const UnaryNode&>(*expr);
        auto  newChild = applyPass(un.child(), rules, depth + 1, maxDepth);
        if (newChild.get() == un.child().get())
            rewritten = expr;
        else
            rewritten = exprUnary(un.op(), std::move(newChild));
        break;
    }
    default: rewritten = expr; break;
    }

    for (const auto& pr : rules) {
        MatchBindings bindings;
        if (pr.rule.lhs.match(rewritten, bindings)) {
            return pr.rule.rhs(bindings);
        }
    }

    return rewritten;
}

}   // anonymous namespace

ExprRef rewrite(ExprRef expr, const std::vector<PrioritizedRule>& rules, ca::u32 maxIter,
                ca::u32 maxDepth)
{
    std::map<RulePriority, std::vector<PrioritizedRule>> groups;
    for (const auto& r : rules) groups[r.priority].push_back(r);

    std::vector<std::vector<PrioritizedRule>> ordered;
    for (auto& [_, grp] : groups) ordered.push_back(std::move(grp));

    ExprRef                     current = expr;
    std::unordered_set<ca::u64> fingerprints;

    for (ca::u32 iter = 0; iter < maxIter; ++iter) {
        ExprRef before = current;

        for (const auto& group : ordered) {
            current = applyPass(current, group, 0, maxDepth);
        }

        if (current.get() == before.get()) break;

        auto fp = std::hash<std::string>{}(toSExpr(*current));
        if (!fingerprints.insert(fp).second) break;
    }

    return current;
}

}   // namespace mjbase::expr
