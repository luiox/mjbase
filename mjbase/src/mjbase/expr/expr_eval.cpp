#include "expr_eval.hpp"

#include <stack>
#include <vector>
#include <unordered_set>

namespace mjbase::expr {

namespace {

enum class VisitState
{
    Enter,
    Process
};

struct Frame
{
    const ExprNode* node;
    VisitState      state;
    ca::u32         depth;
};

bool isConstNode(const ExprNode* n)
{
    return n && n->kind() == NodeKind::Const;
}

ca::i64 constValue(const ExprNode* n)
{
    return static_cast<const ConstNode*>(n)->value();
}

BitWidth constBw(const ExprNode* n)
{
    return static_cast<const ConstNode*>(n)->bitWidth();
}

}   // anonymous namespace

EvalResult eval(const ExprNode& node, const VarBindings& vars, ca::u32 maxDepth)
{
    std::stack<Frame>     nodeStack;
    std::stack<ExprValue> valueStack;

    nodeStack.push({&node, VisitState::Enter, 0});

    while (!nodeStack.empty()) {
        auto [n, state, depth] = nodeStack.top();
        nodeStack.pop();

        if (state == VisitState::Process) {
            // All children processed; pop results and compute
            switch (n->kind()) {
            case NodeKind::Const:
            {
                auto* cn = static_cast<const ConstNode*>(n);
                valueStack.push(ExprValue(cn->value(), cn->bitWidth()));
                break;
            }
            case NodeKind::Var:
            {
                auto* vn = static_cast<const VarNode*>(n);
                auto  it = vars.find(vn->name());
                if (it == vars.end()) {
                    return EvalResult::err("unbound variable: " + vn->name());
                }
                valueStack.push(it->second);
                break;
            }
            case NodeKind::UnaryOp:
            {
                auto* un    = static_cast<const UnaryNode*>(n);
                auto  child = valueStack.top();
                valueStack.pop();
                ExprValue result(0, child.bitWidth());
                switch (un->op()) {
                case UnaryOpKind::Neg: result = -child; break;
                case UnaryOpKind::Not: result = ~child; break;
                }
                valueStack.push(result);
                break;
            }
            case NodeKind::BinaryOp:
            {
                auto* bn    = static_cast<const BinaryNode*>(n);
                auto  right = valueStack.top();
                valueStack.pop();
                auto left = valueStack.top();
                valueStack.pop();
                ExprValue result(0, ExprValue::wider(left.bitWidth(), right.bitWidth()));
                switch (bn->op()) {
                case BinOpKind::Add: result = left + right; break;
                case BinOpKind::Sub: result = left - right; break;
                case BinOpKind::Mul: result = left * right; break;
                case BinOpKind::Div: result = left / right; break;
                case BinOpKind::Rem: result = left % right; break;
                case BinOpKind::And: result = left & right; break;
                case BinOpKind::Or: result = left | right; break;
                case BinOpKind::Xor: result = left ^ right; break;
                case BinOpKind::Shl: result = ExprValue::shl(left, right); break;
                case BinOpKind::Shr: result = ExprValue::shr(left, right); break;
                case BinOpKind::UShr: result = ExprValue::ushr(left, right); break;
                }
                valueStack.push(result);
                break;
            }
            }
            continue;
        }

        // Enter state: check depth, then push Process marker + children
        if (depth > maxDepth) {
            return EvalResult::err("max eval depth exceeded");
        }

        nodeStack.push({n, VisitState::Process, depth});

        // Push children in reverse order so left is processed first
        switch (n->kind()) {
        case NodeKind::BinaryOp:
        {
            auto* bn = static_cast<const BinaryNode*>(n);
            nodeStack.push({bn->right().get(), VisitState::Enter, depth + 1});
            nodeStack.push({bn->left().get(), VisitState::Enter, depth + 1});
            break;
        }
        case NodeKind::UnaryOp:
        {
            auto* un = static_cast<const UnaryNode*>(n);
            nodeStack.push({un->child().get(), VisitState::Enter, depth + 1});
            break;
        }
        default:
            // Const/Var: no children
            break;
        }
    }

    if (valueStack.empty()) {
        return EvalResult::err("empty evaluation stack");
    }

    return EvalResult::val(valueStack.top());
}

}   // namespace mjbase::expr
