#pragma once

#include <functional>
#include <string>
#include <vector>

#include "expr_node.hpp"
#include "expr_match.hpp"

namespace mjbase::expr {

// ── Deterministic RNG (xorshift64) ──

/// Seed-based PRNG — the ONLY source of randomness for MBA generation.
class MBAState
{
public:
    explicit MBAState(ca::u64 seed) noexcept
        : state_(seed ? seed : 1)
    {}

    /// [0, 1) double
    double nextDouble() noexcept;

    /// [lo, hi] inclusive integer
    ca::i64 nextInt(ca::i64 lo, ca::i64 hi) noexcept;

    /// Current seed (for serialization)
    ca::u64 seed() const noexcept { return state_; }

private:
    ca::u64 state_;
};

// ── Generator config ──

struct MBAConfig
{
    ca::u32  targetDepth = 3;     // obfuscation depth
    ca::u32  maxNodes    = 500;   // explosion guard
    BitWidth bitWidth    = BitWidth::B32;
    double   varProb     = 0.3;   // probability to insert a variable vs constant
};

// ── Identity entry ──

struct MBAIdentity
{
    std::string                                      name;
    IdentityGroup                                    group;
    std::function<ExprRef(ExprRef x, MBAState& rng)> transform;
};

// ── Core generator API ──

/// Generate a semantically equivalent but obfuscated MBA expression.
ExprRef generateMBA(ExprRef target, const MBAConfig& config, MBAState& state);

/// Apply obfuscation passes to deepen an existing expression.
ExprRef applyMBAObfuscation(ExprRef expr, ca::u32 rounds, MBAState& state);

/// Get identities by group.
const std::vector<MBAIdentity>& getIdentitiesByGroup(IdentityGroup group);

}   // namespace mjbase::expr
