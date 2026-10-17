#include "expr_print.hpp"

#include <sstream>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mjbase::expr {

namespace {

const char* binOpName(BinOpKind op)
{
    switch (op) {
    case BinOpKind::Add: return "+";
    case BinOpKind::Sub: return "-";
    case BinOpKind::Mul: return "*";
    case BinOpKind::Div: return "/";
    case BinOpKind::Rem: return "%";
    case BinOpKind::And: return "&";
    case BinOpKind::Or: return "|";
    case BinOpKind::Xor: return "^";
    case BinOpKind::Shl: return "<<";
    case BinOpKind::Shr: return ">>";
    case BinOpKind::UShr: return ">>>";
    }
    return "?";
}

const char* unaryOpName(UnaryOpKind op)
{
    switch (op) {
    case UnaryOpKind::Neg: return "-";
    case UnaryOpKind::Not: return "~";
    }
    return "?";
}

const char* bitWidthSuffix(BitWidth bw)
{
    switch (bw) {
    case BitWidth::B8: return "i8";
    case BitWidth::B16: return "i16";
    case BitWidth::B32: return "i32";
    case BitWidth::B64: return "i64";
    }
    return "";
}

struct SExprFrame
{
    const ExprNode* node;
    bool            childrenDone;
};

// 与 expr_parser 的优先级表保持一致：Mul/Div/Rem > Add/Sub > 移位 > & > ^ > |。
// toInfix 的输出必须能被 parseExpression 原样重新解析（roundtrip 安全）。
constexpr int kPrecMul = 6;
constexpr int kPrecAdd = 5;
constexpr int kPrecShift = 4;
constexpr int kPrecAnd = 3;
constexpr int kPrecXor = 2;
constexpr int kPrecOr = 1;

int infixPrec(BinOpKind op)
{
    switch (op) {
    case BinOpKind::Mul:
    case BinOpKind::Div:
    case BinOpKind::Rem: return kPrecMul;
    case BinOpKind::Add:
    case BinOpKind::Sub: return kPrecAdd;
    case BinOpKind::Shl:
    case BinOpKind::Shr:
    case BinOpKind::UShr: return kPrecShift;
    case BinOpKind::And: return kPrecAnd;
    case BinOpKind::Xor: return kPrecXor;
    case BinOpKind::Or: return kPrecOr;
    }
    return 0;
}

void infixEmit(std::ostringstream& os, const ExprNode& node)
{
    switch (node.kind()) {
    case NodeKind::Const:
        os << static_cast<const ConstNode&>(node).value();
        return;
    case NodeKind::Var:
        os << static_cast<const VarNode&>(node).name();
        return;
    case NodeKind::UnaryOp:
    {
        auto& un    = static_cast<const UnaryNode&>(node);
        auto& child = *un.child();
        os << unaryOpName(un.op());
        const bool paren = (child.kind() == NodeKind::BinaryOp);
        if (paren) os << "(";
        infixEmit(os, child);
        if (paren) os << ")";
        return;
    }
    case NodeKind::BinaryOp:
    {
        auto& bn   = static_cast<const BinaryNode&>(node);
        int   prec = infixPrec(bn.op());
        auto& lhs  = *bn.left();
        auto& rhs  = *bn.right();
        // 左孩子：仅当优先级更低才加括号（同优先级依赖解析器左结合）；
        // 右孩子：同优先级也加括号，保证 a - (b - c) 这类结构不被吞掉。
        const bool lParen = (lhs.kind() == NodeKind::BinaryOp) && infixPrec(
            static_cast<const BinaryNode&>(lhs).op()) < prec;
        const bool rParen = (rhs.kind() == NodeKind::BinaryOp) && infixPrec(
            static_cast<const BinaryNode&>(rhs).op()) <= prec;
        if (lParen) os << "(";
        infixEmit(os, lhs);
        if (lParen) os << ")";
        os << " " << binOpName(bn.op()) << " ";
        if (rParen) os << "(";
        infixEmit(os, rhs);
        if (rParen) os << ")";
        return;
    }
    }
}

}   // anonymous namespace

std::string toSExpr(const ExprNode& node)
{
    std::stack<SExprFrame>  st;
    std::stack<std::string> result;
    st.push({&node, false});

    while (!st.empty()) {
        auto f = st.top();
        st.pop();
        auto n = f.node;

        if (f.childrenDone) {
            switch (n->kind()) {
            case NodeKind::Const:
            {
                auto*              cn = static_cast<const ConstNode*>(n);
                std::ostringstream os;
                os << "(const " << cn->value() << ":" << bitWidthSuffix(cn->bitWidth()) << ")";
                result.push(os.str());
                break;
            }
            case NodeKind::Var:
            {
                auto*              vn = static_cast<const VarNode*>(n);
                std::ostringstream os;
                os << "(var " << vn->name() << ":" << bitWidthSuffix(vn->bitWidth()) << ")";
                result.push(os.str());
                break;
            }
            case NodeKind::UnaryOp:
            {
                auto* un    = static_cast<const UnaryNode*>(n);
                auto  child = std::move(result.top());
                result.pop();
                std::ostringstream os;
                os << "(" << unaryOpName(un->op()) << " " << child << ")";
                result.push(os.str());
                break;
            }
            case NodeKind::BinaryOp:
            {
                auto* bn    = static_cast<const BinaryNode*>(n);
                auto  right = std::move(result.top());
                result.pop();
                auto left = std::move(result.top());
                result.pop();
                std::ostringstream os;
                os << "(" << binOpName(bn->op()) << " " << left << " " << right << ")";
                result.push(os.str());
                break;
            }
            }
            continue;
        }

        // Push children-done marker first, then children
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

    if (result.empty()) return "";
    auto r = result.top();
    result.pop();
    return r;
}

std::string toInfix(const ExprNode& node)
{
    // 中缀 + 最小括号：只在解析器优先级要求的位置加括号，输出可被
    // parseExpression 无损重解析（位宽不进入打印，重解析取解析器默认位宽）。
    std::ostringstream os;
    infixEmit(os, node);
    return os.str();
}

std::string toDot(const ExprNode& node)
{
    std::ostringstream os;
    os << "digraph expr {\n";
    os << "  node [shape=box, style=filled, fillcolor=lightyellow];\n";

    // Iterative DFS with explicit stack for node ID assignment
    std::stack<const ExprNode*>                  st;
    std::unordered_map<const ExprNode*, ca::u32> ids;
    ca::u32                                      nextId = 0;

    st.push(&node);
    while (!st.empty()) {
        auto n = st.top();
        st.pop();
        if (ids.count(n)) continue;
        ids[n] = nextId++;

        switch (n->kind()) {
        case NodeKind::Const:
        {
            auto* cn = static_cast<const ConstNode*>(n);
            os << "  n" << ids[n] << " [label=\"const " << cn->value() << ":"
               << bitWidthSuffix(cn->bitWidth()) << "\"];\n";
            break;
        }
        case NodeKind::Var:
        {
            auto* vn = static_cast<const VarNode*>(n);
            os << "  n" << ids[n] << " [label=\"var " << vn->name() << ":"
               << bitWidthSuffix(vn->bitWidth()) << "\"];\n";
            break;
        }
        case NodeKind::UnaryOp:
        {
            auto* un = static_cast<const UnaryNode*>(n);
            os << "  n" << ids[n] << " [label=\"" << unaryOpName(un->op())
               << "\", fillcolor=lightblue];\n";
            st.push(un->child().get());
            break;
        }
        case NodeKind::BinaryOp:
        {
            auto* bn = static_cast<const BinaryNode*>(n);
            os << "  n" << ids[n] << " [label=\"" << binOpName(bn->op())
               << "\", fillcolor=lightgreen];\n";
            st.push(bn->right().get());
            st.push(bn->left().get());
            break;
        }
        }
    }

    // Second pass: emit edges
    st.push(&node);
    std::unordered_set<const ExprNode*> visited;
    while (!st.empty()) {
        auto n = st.top();
        st.pop();
        if (visited.count(n)) continue;
        visited.insert(n);

        switch (n->kind()) {
        case NodeKind::UnaryOp:
        {
            auto* un = static_cast<const UnaryNode*>(n);
            os << "  n" << ids[n] << " -> n" << ids[un->child().get()] << ";\n";
            st.push(un->child().get());
            break;
        }
        case NodeKind::BinaryOp:
        {
            auto* bn = static_cast<const BinaryNode*>(n);
            os << "  n" << ids[n] << " -> n" << ids[bn->left().get()] << " [label=L];\n";
            os << "  n" << ids[n] << " -> n" << ids[bn->right().get()] << " [label=R];\n";
            st.push(bn->right().get());
            st.push(bn->left().get());
            break;
        }
        default: break;
        }
    }

    os << "}\n";
    return os.str();
}

}   // namespace mjbase::expr
