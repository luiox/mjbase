#include "expr_mba_gen.hpp"
#include "expr_util.hpp"

#include <cstdint>

namespace mjbase::expr {

// ── xorshift64 RNG ──

double MBAState::nextDouble() noexcept
{
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return static_cast<double>(state_ >> 11) / 9007199254740992.0;
}

ca::i64 MBAState::nextInt(ca::i64 lo, ca::i64 hi) noexcept
{
    if (lo >= hi) return lo;
    ca::u64 range = static_cast<ca::u64>(hi - lo) + 1;
    return lo + static_cast<ca::i64>(nextDouble() * static_cast<double>(range));
}

// ── Identity factories ──

namespace {

ExprRef freshVar(MBAState& rng)
{
    return exprVar("mba_" + std::to_string(rng.nextInt(0, 999999)));
}

ExprRef freshConst(MBAState& rng)
{
    return exprConst(rng.nextInt(-10000, 10000));
}

ExprRef freshR(MBAState& rng)
{
    return rng.nextDouble() < 0.5 ? freshVar(rng) : freshConst(rng);
}

}   // anonymous namespace

static std::vector<MBAIdentity> makeObfuscateIdentities()
{
    std::vector<MBAIdentity> ids;

    // T → T + 0
    ids.push_back({"T->T+0", IdentityGroup::Obfuscate, [](ExprRef x, MBAState&) {
                       return exprAdd(x, exprConst(0));
                   }});

    // T → T * 1
    ids.push_back({"T->T*1", IdentityGroup::Obfuscate, [](ExprRef x, MBAState&) {
                       return exprMul(x, exprConst(1));
                   }});

    // T → T ^ 0
    ids.push_back({"T->T^0", IdentityGroup::Obfuscate, [](ExprRef x, MBAState&) {
                       return exprXor(x, exprConst(0));
                   }});

    // T → T | 0
    ids.push_back({"T->T|0", IdentityGroup::Obfuscate, [](ExprRef x, MBAState&) {
                       return exprOr(x, exprConst(0));
                   }});

    // T → T & -1
    ids.push_back({"T->T&-1", IdentityGroup::Obfuscate, [](ExprRef x, MBAState&) {
                       return exprAnd(x, exprConst(-1));
                   }});

    // T → T + R - R
    ids.push_back({"T->T+R-R", IdentityGroup::Obfuscate, [](ExprRef x, MBAState& rng) {
                       auto r = freshR(rng);
                       return exprAdd(x, exprSub(r, r));
                   }});

    // T → T ^ R ^ R
    ids.push_back({"T->T^R^R", IdentityGroup::Obfuscate, [](ExprRef x, MBAState& rng) {
                       auto r = freshR(rng);
                       return exprXor(x, exprXor(r, r));
                   }});

    // T → T & R | (T & ~R)   (identity: x = (x & y) | (x & ~y))
    ids.push_back({"T->(T&R)|(T&~R)", IdentityGroup::Obfuscate, [](ExprRef x, MBAState& rng) {
                       auto r = freshR(rng);
                       return exprOr(exprAnd(x, r), exprAnd(x, exprNot(r)));
                   }});

    // T → (T ^ R) + ((T & R) << 1)   MBA: x + y = (x ^ y) + (x & y) * 2
    ids.push_back({"T->(T^R)+((T&R)<<1)", IdentityGroup::Obfuscate, [](ExprRef x, MBAState& rng) {
                       auto r = freshR(rng);
                       return exprAdd(exprXor(x, r), exprShl(exprAnd(x, r), exprConst(1)));
                   }});

    // T → (T & R) + (T | R)   MBA: x + y = (x & y) + (x | y)
    ids.push_back({"T->(T&R)+(T|R)", IdentityGroup::Obfuscate, [](ExprRef x, MBAState& rng) {
                       auto r = freshR(rng);
                       return exprAdd(exprAnd(x, r), exprOr(x, r));
                   }});

    return ids;
}

static const std::vector<MBAIdentity>& obfuscateIdentities()
{
    static const auto ids = makeObfuscateIdentities();
    return ids;
}

const std::vector<MBAIdentity>& getIdentitiesByGroup(IdentityGroup group)
{
    if (group == IdentityGroup::Obfuscate || group == IdentityGroup::Both)
        return obfuscateIdentities();
    static const std::vector<MBAIdentity> empty;
    return empty;
}

// ── generateMBA ──
//
// Strategy: at each depth level, pick a random obfuscation identity and
// apply it to the current root. This produces a chain:
//   id_n(...id_2(id_1(target)))
// Each identity preserves semantic equivalence, and the chain creates
// a deeply nested MBA expression.

ExprRef generateMBA(ExprRef target, const MBAConfig& config, MBAState& state)
{
    auto& identities = obfuscateIdentities();
    if (identities.empty()) return target;

    ExprRef current = target;

    for (ca::u32 d = 0; d < config.targetDepth; ++d) {
        // Explosion guard
        if (nodeCount(*current) > config.maxNodes) break;

        // Pick a random identity
        auto idx =
            static_cast<ca::usize>(state.nextInt(0, static_cast<ca::i64>(identities.size()) - 1));

        // Apply at root
        current = identities[idx].transform(current, state);
    }

    return current;
}

// ── applyMBAObfuscation ──
//
// Apply obfuscation rounds to an existing expression.
// Each round picks a random identity and applies it at the root.

ExprRef applyMBAObfuscation(ExprRef expr, ca::u32 rounds, MBAState& state)
{
    auto& identities = obfuscateIdentities();
    if (identities.empty()) return expr;

    ExprRef current = expr;

    for (ca::u32 r = 0; r < rounds; ++r) {
        if (nodeCount(*current) > 500) break;

        auto idx =
            static_cast<ca::usize>(state.nextInt(0, static_cast<ca::i64>(identities.size()) - 1));
        current = identities[idx].transform(current, state);
    }

    return current;
}

}   // namespace mjbase::expr
