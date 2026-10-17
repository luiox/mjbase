#pragma once

#include <memory>
#include <vector>

#include "expr_match.hpp"
#include "expr_node.hpp"
#include "expr_value.hpp"

namespace mjbase::expr {

// ── Simplify pass interface ──

class SimplifyPass
{
public:
    virtual ~SimplifyPass()                       = default;
    virtual std::string name() const              = 0;
    virtual ExprRef     apply(ExprRef expr) const = 0;
};

// ── Solve config & result ──

struct SolveConfig
{
    ca::u32 maxPasses = 10;
    ca::u32 maxNodes  = 2000;
    ca::u32 maxDepth  = 64;
    bool    verbose   = false;

    /// Which identity group to use for rule-based passes
    IdentityGroup identityGroup = IdentityGroup::Simplify;
};

struct SolveResult
{
    ExprRef simplified;
    ca::u32 passesApplied = 0;
    ca::u32 nodesBefore   = 0;
    ca::u32 nodesAfter    = 0;
    bool    converged     = false;
};

/// Main entry: simplify an expression through the pass pipeline.
SolveResult simplify(ExprRef expr, const SolveConfig& config = {});

/// Default pass list (constant fold → identity simplify → basic algebra)
std::vector<std::unique_ptr<SimplifyPass>> defaultPasses();

/// Build rewrite rules from the simplify identity group
std::vector<PrioritizedRule> buildSimplifyRules();

/// Build rewrite rules from the obfuscate identity group
std::vector<PrioritizedRule> buildObfuscateRules();

}   // namespace mjbase::expr
