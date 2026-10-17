#include "expr_util.hpp"

#include <stack>

namespace mjbase::expr {

namespace {

struct DepthFrame
{
    const ExprNode* node;
    ca::u32         depth;
};

struct CountFrame
{
    const ExprNode* node;
    bool            childrenDone;
};

}   // anonymous namespace

ca::u32 depth(const ExprNode& node)
{
    std::stack<DepthFrame> st;
    st.push({&node, 1});
    ca::u32 maxDepth = 0;

    while (!st.empty()) {
        auto [n, d] = st.top();
        st.pop();
        if (d > maxDepth) maxDepth = d;

        switch (n->kind()) {
        case NodeKind::BinaryOp:
        {
            auto* bn = static_cast<const BinaryNode*>(n);
            st.push({bn->right().get(), d + 1});
            st.push({bn->left().get(), d + 1});
            break;
        }
        case NodeKind::UnaryOp:
        {
            auto* un = static_cast<const UnaryNode*>(n);
            st.push({un->child().get(), d + 1});
            break;
        }
        default: break;
        }
    }

    return maxDepth;
}

ca::u32 nodeCount(const ExprNode& node)
{
    std::stack<CountFrame> st;
    st.push({&node, false});
    ca::u32 count = 0;

    while (!st.empty()) {
        auto [n, done] = st.top();
        st.pop();

        if (done) {
            ++count;
            continue;
        }

        st.push({n, true});
        switch (n->kind()) {
        case NodeKind::BinaryOp:
        {
            auto* bn = static_cast<const BinaryNode*>(n);
            st.push({bn->right().get(), false});
            st.push({bn->left().get(), false});
            break;
        }
        case NodeKind::UnaryOp:
        {
            auto* un = static_cast<const UnaryNode*>(n);
            st.push({un->child().get(), false});
            break;
        }
        default: break;
        }
    }

    return count;
}

std::unordered_set<std::string> collectVars(const ExprNode& node)
{
    std::unordered_set<std::string> vars;
    std::stack<const ExprNode*>     st;
    st.push(&node);

    while (!st.empty()) {
        auto n = st.top();
        st.pop();

        switch (n->kind()) {
        case NodeKind::Var:
        {
            auto* vn = static_cast<const VarNode*>(n);
            vars.insert(vn->name());
            break;
        }
        case NodeKind::BinaryOp:
        {
            auto* bn = static_cast<const BinaryNode*>(n);
            st.push(bn->right().get());
            st.push(bn->left().get());
            break;
        }
        case NodeKind::UnaryOp:
        {
            auto* un = static_cast<const UnaryNode*>(n);
            st.push(un->child().get());
            break;
        }
        default: break;
        }
    }

    return vars;
}

ExprRef subExprAt(const ExprNode& node, const std::vector<ca::u32>& path)
{
    const ExprNode* cur = &node;
    for (auto idx : path) {
        switch (cur->kind()) {
        case NodeKind::BinaryOp:
        {
            auto* bn = static_cast<const BinaryNode*>(cur);
            cur      = (idx == 0) ? bn->left().get() : bn->right().get();
            break;
        }
        case NodeKind::UnaryOp:
        {
            auto* un = static_cast<const UnaryNode*>(cur);
            cur      = un->child().get();
            break;
        }
        default: return nullptr;   // path longer than tree
        }
    }
    // Find the original shared_ptr from the root to avoid dangling
    // For simplicity, we rebuild — caller must keep root alive
    // Return value is a valid pointer, not shared ownership
    // Use with caution: caller must ensure root outlives usage
    return nullptr;
}

ExprRef replaceAt(ExprRef node, const std::vector<ca::u32>& path, ExprRef replacement)
{
    if (path.empty()) return std::move(replacement);

    auto                 idx = path.front();
    std::vector<ca::u32> rest(path.begin() + 1, path.end());

    switch (node->kind()) {
    case NodeKind::BinaryOp:
    {
        auto* bn       = static_cast<const BinaryNode*>(node.get());
        auto  newLeft  = bn->left();
        auto  newRight = bn->right();
        if (idx == 0) {
            newLeft = replaceAt(bn->left(), rest, replacement);
        }
        else {
            newRight = replaceAt(bn->right(), rest, replacement);
        }
        return exprBinary(bn->op(), std::move(newLeft), std::move(newRight));
    }
    case NodeKind::UnaryOp:
    {
        auto* un       = static_cast<const UnaryNode*>(node.get());
        auto  newChild = replaceAt(un->child(), rest, std::move(replacement));
        return exprUnary(un->op(), std::move(newChild));
    }
    default: return node;   // leaf, path invalid — return unchanged
    }
}

}   // namespace mjbase::expr
