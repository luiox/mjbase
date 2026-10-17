#include "expr_simplify.hpp"
#include "expr_eval.hpp"
#include "expr_util.hpp"

#include <sstream>

namespace mjbase::expr {

// ── Constant Folding Pass ──

namespace {

class ConstantFoldPass : public SimplifyPass
{
public:
    std::string name() const override { return "constant-fold"; }

    ExprRef apply(ExprRef expr) const override
    {
        // Recursive post-order traversal with depth guard
        return fold(expr, 0);
    }

private:
    ExprRef fold(ExprRef expr, ca::u32 depth) const
    {
        if (!expr || depth > 128) return expr;

        switch (expr->kind()) {
        case NodeKind::BinaryOp:
        {
            auto& bn       = static_cast<const BinaryNode&>(*expr);
            auto  newLeft  = fold(bn.left(), depth + 1);
            auto  newRight = fold(bn.right(), depth + 1);

            // If both children are constants, evaluate now
            if (newLeft->kind() == NodeKind::Const && newRight->kind() == NodeKind::Const) {
                auto result = eval(*exprBinary(bn.op(), newLeft, newRight));
                if (result.ok) {
                    return exprConst(result.value.rawValue(), result.value.bitWidth());
                }
            }

            // Rebuild if children changed
            if (newLeft.get() != bn.left().get() || newRight.get() != bn.right().get()) {
                return exprBinary(bn.op(), std::move(newLeft), std::move(newRight));
            }
            return expr;
        }
        case NodeKind::UnaryOp:
        {
            auto& un       = static_cast<const UnaryNode&>(*expr);
            auto  newChild = fold(un.child(), depth + 1);

            if (newChild->kind() == NodeKind::Const) {
                auto result = eval(*exprUnary(un.op(), newChild));
                if (result.ok) {
                    return exprConst(result.value.rawValue(), result.value.bitWidth());
                }
            }

            if (newChild.get() != un.child().get()) {
                return exprUnary(un.op(), std::move(newChild));
            }
            return expr;
        }
        default: return expr;
        }
    }
};

// ── Identity Simplify Pass (rule-based) ──

class IdentitySimplifyPass : public SimplifyPass
{
public:
    explicit IdentitySimplifyPass(std::vector<PrioritizedRule> rules)
        : rules_(std::move(rules))
    {}

    std::string name() const override { return "identity-simplify"; }

    ExprRef apply(ExprRef expr) const override { return rewrite(expr, rules_, 5, 128); }

private:
    std::vector<PrioritizedRule> rules_;
};

}   // anonymous namespace

// ── Build identity rules ──

static std::vector<PrioritizedRule> buildRulesForGroup(IdentityGroup target)
{
    std::vector<PrioritizedRule> rules;

    auto add = [&](RulePriority prio,
                   std::string  name,
                   Pattern      lhs,
                   std::function<ExprRef(const MatchBindings&)>
                       rhs) {
        rules.push_back({prio, {std::move(lhs), std::move(rhs), std::move(name)}});
    };

    auto cap  = [](const char* n) { return Pattern::Capture(n); };
    auto cnst = [](ca::i64 v) { return Pattern::Const(v); };
    auto var  = [](const char* n) { return Pattern::Var(n); };
    auto bop  = [](BinOpKind o, auto l, auto r) { return Pattern::BinOp(o, l, r); };
    auto uop  = [](UnaryOpKind o, auto c) { return Pattern::UnaryOp(o, c); };

    auto get = [](const MatchBindings& b, const char* n) -> ExprRef {
        auto it = b.find(n);
        return it != b.end() ? it->second : nullptr;
    };

    // ── Simplify rules (priority 0: most aggressive) ──

    if (target == IdentityGroup::Simplify || target == IdentityGroup::Both) {

        // x + 0 → x
        add(0, "x+0->x", bop(BinOpKind::Add, cap("x"), cnst(0)), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // 0 + x → x
        add(0, "0+x->x", bop(BinOpKind::Add, cnst(0), cap("x")), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x - 0 → x
        add(0, "x-0->x", bop(BinOpKind::Sub, cap("x"), cnst(0)), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x - x → 0 (same capture name = structural equality)
        add(0, "x-x->0", bop(BinOpKind::Sub, cap("x"), cap("x")), [](const MatchBindings&) {
            return exprConst(0);
        });
    }

    if (target == IdentityGroup::Simplify || target == IdentityGroup::Both) {

        // x * 0 → 0
        add(0, "x*0->0", bop(BinOpKind::Mul, cap("x"), cnst(0)), [](const MatchBindings&) {
            return exprConst(0);
        });
        add(0, "0*x->0", bop(BinOpKind::Mul, cnst(0), cap("x")), [](const MatchBindings&) {
            return exprConst(0);
        });

        // x * 1 → x
        add(0, "x*1->x", bop(BinOpKind::Mul, cap("x"), cnst(1)), [&](const MatchBindings& b) {
            return get(b, "x");
        });
        add(0, "1*x->x", bop(BinOpKind::Mul, cnst(1), cap("x")), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x / 1 → x
        add(0, "x/1->x", bop(BinOpKind::Div, cap("x"), cnst(1)), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x ^ 0 → x
        add(0, "x^0->x", bop(BinOpKind::Xor, cap("x"), cnst(0)), [&](const MatchBindings& b) {
            return get(b, "x");
        });
        add(0, "0^x->x", bop(BinOpKind::Xor, cnst(0), cap("x")), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x & -1 → x (for B32: -1 = 0xFFFFFFFF)
        add(0, "x&-1->x", bop(BinOpKind::And, cap("x"), cnst(-1)), [&](const MatchBindings& b) {
            return get(b, "x");
        });
        add(0, "-1&x->x", bop(BinOpKind::And, cnst(-1), cap("x")), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x & 0 → 0
        add(0, "x&0->0", bop(BinOpKind::And, cap("x"), cnst(0)), [](const MatchBindings&) {
            return exprConst(0);
        });
        add(0, "0&x->0", bop(BinOpKind::And, cnst(0), cap("x")), [](const MatchBindings&) {
            return exprConst(0);
        });

        // x | 0 → x
        add(0, "x|0->x", bop(BinOpKind::Or, cap("x"), cnst(0)), [&](const MatchBindings& b) {
            return get(b, "x");
        });
        add(0, "0|x->x", bop(BinOpKind::Or, cnst(0), cap("x")), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x | -1 → -1
        add(0, "x|-1->-1", bop(BinOpKind::Or, cap("x"), cnst(-1)), [](const MatchBindings&) {
            return exprConst(-1);
        });
        add(0, "-1|x->-1", bop(BinOpKind::Or, cnst(-1), cap("x")), [](const MatchBindings&) {
            return exprConst(-1);
        });

        // x << 0 → x
        add(1, "x<<0->x", bop(BinOpKind::Shl, cap("x"), cnst(0)), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x >> 0 → x
        add(1, "x>>0->x", bop(BinOpKind::Shr, cap("x"), cnst(0)), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // -(-x) → x
        add(1,
            "--x->x",
            uop(UnaryOpKind::Neg, uop(UnaryOpKind::Neg, cap("x"))),
            [&](const MatchBindings& b) { return get(b, "x"); });

        // ~(~x) → x
        add(1,
            "~~x->x",
            uop(UnaryOpKind::Not, uop(UnaryOpKind::Not, cap("x"))),
            [&](const MatchBindings& b) { return get(b, "x"); });

        // x + x → x * 2
        add(1, "x+x->x*2", bop(BinOpKind::Add, cap("x"), cap("x")), [](const MatchBindings& b) {
            return exprMul(b.at("x"), exprConst(2));
        });

        // x ^ x → 0
        add(1, "x^x->0", bop(BinOpKind::Xor, cap("x"), cap("x")), [](const MatchBindings&) {
            return exprConst(0);
        });

        // x & x → x
        add(1, "x&x->x", bop(BinOpKind::And, cap("x"), cap("x")), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x | x → x
        add(1, "x|x->x", bop(BinOpKind::Or, cap("x"), cap("x")), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // x * 2 → x << 1  (normalize for MBA reverse matching)
        add(1, "x*2->x<<1", bop(BinOpKind::Mul, cap("x"), cnst(2)), [](const MatchBindings& b) {
            return exprShl(b.at("x"), exprConst(1));
        });
        // 2 * x → x << 1
        add(1, "2*x->x<<1", bop(BinOpKind::Mul, cnst(2), cap("x")), [](const MatchBindings& b) {
            return exprShl(b.at("x"), exprConst(1));
        });

        // ── MBA reverse identities (priority 2) ──

        // (x ^ y) + ((x & y) * 2) → x + y
        add(2,
            "(x^y)+(x&y)*2->x+y",
            bop(BinOpKind::Add,
                bop(BinOpKind::Xor, cap("x"), cap("y")),
                bop(BinOpKind::Mul, bop(BinOpKind::And, cap("x"), cap("y")), cnst(2))),
            [](const MatchBindings& b) { return exprAdd(b.at("x"), b.at("y")); });
        // 2 * (x & y) + (x ^ y) → x + y
        add(2,
            "2*(x&y)+(x^y)->x+y",
            bop(BinOpKind::Add,
                bop(BinOpKind::Mul, cnst(2), bop(BinOpKind::And, cap("x"), cap("y"))),
                bop(BinOpKind::Xor, cap("x"), cap("y"))),
            [](const MatchBindings& b) { return exprAdd(b.at("x"), b.at("y")); });

        // (x ^ y) + ((x & y) << 1) → x + y
        // Pattern: add(xor(x,y), shl(and(x,y), const(1)))
        add(2,
            "(x^y)+((x&y)<<1)->x+y",
            bop(BinOpKind::Add,
                bop(BinOpKind::Xor, cap("x"), cap("y")),
                bop(BinOpKind::Shl, bop(BinOpKind::And, cap("x"), cap("y")), cnst(1))),
            [](const MatchBindings& b) { return exprAdd(b.at("x"), b.at("y")); });

        // (x & y) + (x | y) → x + y
        add(2,
            "(x&y)+(x|y)->x+y",
            bop(BinOpKind::Add,
                bop(BinOpKind::And, cap("x"), cap("y")),
                bop(BinOpKind::Or, cap("x"), cap("y"))),
            [](const MatchBindings& b) { return exprAdd(b.at("x"), b.at("y")); });

        // (x & y) | (x & ~y) → x
        add(2,
            "(x&y)|(x&~y)->x",
            bop(BinOpKind::Or,
                bop(BinOpKind::And, cap("x"), cap("y")),
                bop(BinOpKind::And, cap("x"), uop(UnaryOpKind::Not, cap("y")))),
            [&](const MatchBindings& b) { return get(b, "x"); });

        // ── More algebraic identities (priority 2) ──

        // x ^ -1 → ~x
        add(2, "x^-1->~x", bop(BinOpKind::Xor, cap("x"), cnst(-1)), [](const MatchBindings& b) {
            return exprNot(b.at("x"));
        });
        add(2, "-1^x->~x", bop(BinOpKind::Xor, cnst(-1), cap("x")), [](const MatchBindings& b) {
            return exprNot(b.at("x"));
        });

        // x + (~x) → -1
        add(2,
            "x+~x->-1",
            bop(BinOpKind::Add, cap("x"), uop(UnaryOpKind::Not, cap("x"))),
            [](const MatchBindings&) { return exprConst(-1); });
        add(2,
            "~x+x->-1",
            bop(BinOpKind::Add, uop(UnaryOpKind::Not, cap("x")), cap("x")),
            [](const MatchBindings&) { return exprConst(-1); });

        // -(x) → ~x + 1  (two's complement negation)
        // Already represented as UnaryOp::Neg, no conversion needed.
        // But: ~x = -x - 1, so: -x → ~x + 1
        // Already handled as Neg/Not which are separate ops.

        // x * -1 → -x
        add(2, "x*-1->-x", bop(BinOpKind::Mul, cap("x"), cnst(-1)), [](const MatchBindings& b) {
            return exprNeg(b.at("x"));
        });
        add(2, "-1*x->-x", bop(BinOpKind::Mul, cnst(-1), cap("x")), [](const MatchBindings& b) {
            return exprNeg(b.at("x"));
        });

        // 0 - x → -x
        add(2, "0-x->-x", bop(BinOpKind::Sub, cnst(0), cap("x")), [](const MatchBindings& b) {
            return exprNeg(b.at("x"));
        });

        // 0 >> x → 0, 0 << x → 0
        add(2, "0<<x->0", bop(BinOpKind::Shl, cnst(0), cap("x")), [](const MatchBindings&) {
            return exprConst(0);
        });
        add(2, "0>>x->0", bop(BinOpKind::Shr, cnst(0), cap("x")), [](const MatchBindings&) {
            return exprConst(0);
        });
        add(2, "0>>>x->0", bop(BinOpKind::UShr, cnst(0), cap("x")), [](const MatchBindings&) {
            return exprConst(0);
        });

        // x >>> 0 → x
        add(2, "x>>>0->x", bop(BinOpKind::UShr, cap("x"), cnst(0)), [&](const MatchBindings& b) {
            return get(b, "x");
        });

        // (x + y) - x → y
        add(2,
            "(x+y)-x->y",
            bop(BinOpKind::Sub, bop(BinOpKind::Add, cap("x"), cap("y")), cap("x")),
            [&](const MatchBindings& b) { return get(b, "y"); });
        add(2,
            "(x+y)-y->x",
            bop(BinOpKind::Sub, bop(BinOpKind::Add, cap("x"), cap("y")), cap("y")),
            [&](const MatchBindings& b) { return get(b, "x"); });

        // (x ^ y) ^ y → x
        add(2,
            "(x^y)^y->x",
            bop(BinOpKind::Xor, bop(BinOpKind::Xor, cap("x"), cap("y")), cap("y")),
            [&](const MatchBindings& b) { return get(b, "x"); });
        // (x ^ y) ^ x → y
        add(2,
            "(x^y)^x->y",
            bop(BinOpKind::Xor, bop(BinOpKind::Xor, cap("x"), cap("y")), cap("x")),
            [&](const MatchBindings& b) { return get(b, "y"); });

        // x & (x ^ y) → x & ~y
        add(2,
            "x&(x^y)->x&~y",
            bop(BinOpKind::And, cap("x"), bop(BinOpKind::Xor, cap("x"), cap("y"))),
            [](const MatchBindings& b) { return exprAnd(b.at("x"), exprNot(b.at("y"))); });

        // x | (x & y) → x
        add(2,
            "x|(x&y)->x",
            bop(BinOpKind::Or, cap("x"), bop(BinOpKind::And, cap("x"), cap("y"))),
            [&](const MatchBindings& b) { return get(b, "x"); });

        // (x + y) * 2 → (x + y) << 1
        add(2,
            "(x+y)*2->(x+y)<<1",
            bop(BinOpKind::Mul, bop(BinOpKind::Add, cap("x"), cap("y")), cnst(2)),
            [](const MatchBindings& b) {
                return exprShl(exprAdd(b.at("x"), b.at("y")), exprConst(1));
            });
        add(2,
            "2*(x+y)->(x+y)<<1",
            bop(BinOpKind::Mul, cnst(2), bop(BinOpKind::Add, cap("x"), cap("y"))),
            [](const MatchBindings& b) {
                return exprShl(exprAdd(b.at("x"), b.at("y")), exprConst(1));
            });

    }   // Simplify group

    // ── Obfuscate rules (priority 10+) ──

    if (target == IdentityGroup::Obfuscate || target == IdentityGroup::Both) {
        add(10, "x->x+0", cap("x"), [](const MatchBindings& b) {
            return exprAdd(b.at("x"), exprConst(0));
        });
        add(10, "x->x^0", cap("x"), [](const MatchBindings& b) {
            return exprXor(b.at("x"), exprConst(0));
        });
        add(10, "x->x*1", cap("x"), [](const MatchBindings& b) {
            return exprMul(b.at("x"), exprConst(1));
        });
        add(10, "x->x|0", cap("x"), [](const MatchBindings& b) {
            return exprOr(b.at("x"), exprConst(0));
        });
    }

    return rules;
}

std::vector<PrioritizedRule> buildSimplifyRules()
{
    return buildRulesForGroup(IdentityGroup::Simplify);
}

std::vector<PrioritizedRule> buildObfuscateRules()
{
    return buildRulesForGroup(IdentityGroup::Obfuscate);
}

// ── Simplify entry point ──

SolveResult simplify(ExprRef expr, const SolveConfig& config)
{
    SolveResult result;
    result.nodesBefore = nodeCount(*expr);

    auto    passes  = defaultPasses();
    ExprRef current = expr;

    for (ca::u32 i = 0; i < config.maxPasses; ++i) {
        ExprRef before = current;
        for (const auto& pass : passes) {
            current = pass->apply(current);
        }
        result.passesApplied = i + 1;

        if (current.get() == before.get()) {
            result.converged = true;
            break;
        }

        // Node count explosion guard
        auto count = nodeCount(*current);
        if (count > config.maxNodes) break;
    }

    result.simplified = current;
    result.nodesAfter = nodeCount(*current);
    return result;
}

// ── Default passes ──

std::vector<std::unique_ptr<SimplifyPass>> defaultPasses()
{
    std::vector<std::unique_ptr<SimplifyPass>> passes;
    passes.push_back(std::make_unique<ConstantFoldPass>());
    passes.push_back(std::make_unique<IdentitySimplifyPass>(buildSimplifyRules()));
    return passes;
}

}   // namespace mjbase::expr
