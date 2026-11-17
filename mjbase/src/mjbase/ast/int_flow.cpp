#include "int_flow.hpp"

#include <array>
#include <deque>
#include <utility>

#include "constant_facts.hpp"

namespace mjbase::ast {
namespace {

using Constant = std::optional<KnownConstant>;
using detail::ConstantFacts;
using detail::LocalConstants;

/// 缺席的局部或参数值代表未知，整个状态缺席代表入口尚不可达。
struct Entry
{
    LocalConstants locals;
    ConstantFacts parameters;
};

struct NormalEdges
{
    std::array<BlockId, 2> targets{};
    ca::usize count = 0;
};

/// 把投影大小、每次块传递和合流都计入工作上限。
struct WorkBudget
{
    bool charge(ca::usize work)
    {
        if (work > remaining) return false;
        remaining -= work;
        return true;
    }

    ca::usize remaining = 1u << 20;
};

std::optional<NormalEdges> normalEdges(const Terminator& terminator)
{
    if (const auto* jump = std::get_if<Jump>(&terminator)) {
        return NormalEdges{{jump->target, {}}, 1};
    }
    if (const auto* branch = std::get_if<Branch>(&terminator)) {
        return NormalEdges{{branch->whenTrue, branch->whenFalse},
                           branch->whenTrue.index == branch->whenFalse.index ? 1u : 2u};
    }
    if (std::holds_alternative<Return>(terminator) || std::holds_alternative<Throw>(terminator)) {
        return NormalEdges{};
    }
    return std::nullopt;
}

bool isValue(const Block& block, ValueId id)
{
    return id.index < block.statements.size()
        && std::holds_alternative<Let>(block.statements[id.index]);
}

std::optional<std::vector<NormalEdges>> checkGraph(const MethodBody& body, WorkBudget& budget)
{
    if (body.blocks.empty() || !body.blocks.front().parameters.empty()
        || !budget.charge(body.blocks.size())) {
        return std::nullopt;
    }

    std::vector<NormalEdges> edges;
    edges.reserve(body.blocks.size());
    for (const auto& block : body.blocks) {
        if (!block.handlers.empty() || !budget.charge(block.statements.size())
            || !budget.charge(block.parameters.size()) || !budget.charge(block.outgoingArguments.size())) {
            return std::nullopt;
        }
        const auto normal = normalEdges(block.terminator);
        if (!normal || (normal->count == 0 && !block.outgoingArguments.empty())) {
            return std::nullopt;
        }
        for (ca::usize i = 0; i < normal->count; ++i) {
            const auto target = normal->targets[i].index;
            if (target >= body.blocks.size()
                || body.blocks[target].parameters.size() != block.outgoingArguments.size()) {
                return std::nullopt;
            }
        }
        for (auto id : block.outgoingArguments) {
            if (!isValue(block, id)) return std::nullopt;
        }
        if (const auto* branch = std::get_if<Branch>(&block.terminator)) {
            if (!isValue(block, branch->left) || !isValue(block, branch->right)) {
                return std::nullopt;
            }
        }
        edges.push_back(*normal);
    }
    return edges;
}

bool sameConstant(const KnownConstant& left, const KnownConstant& right)
{
    if (left.index() != right.index()) return false;
    if (const auto* integer = std::get_if<ca::i32>(&left)) {
        return *integer == std::get<ca::i32>(right);
    }
    if (const auto* bits = std::get_if<Bits32>(&left)) {
        return bits->bits == std::get<Bits32>(right).bits;
    }
    return true;
}

bool mergeInto(std::optional<Entry>& entry, const LocalConstants& locals,
               const ConstantFacts& parameters)
{
    if (!entry) {
        entry.emplace(Entry{locals, parameters});
        return true;
    }

    bool changed = false;
    for (auto it = entry->locals.begin(); it != entry->locals.end();) {
        const auto incoming = locals.find(it->first);
        if (incoming == locals.end() || !sameConstant(it->second, incoming->second)) {
            it = entry->locals.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    for (ca::usize i = 0; i < parameters.size(); ++i) {
        auto& old = entry->parameters[i];
        const auto& next = parameters[i];
        if (old && (!next || !sameConstant(*old, *next))) {
            old.reset();
            changed = true;
        }
    }
    return changed;
}

std::optional<ca::i32> intValue(const Constant& value, bool typedUse = false)
{
    if (!value) return std::nullopt;
    if (const auto* number = std::get_if<ca::i32>(&*value)) return *number;
    if (typedUse) {
        if (const auto* bits = std::get_if<Bits32>(&*value)) {
            return static_cast<ca::i32>(bits->bits);
        }
    }
    return std::nullopt;
}

std::optional<bool> branchValue(const Branch& branch, const ConstantFacts& facts)
{
    if (branch.kind != LocalKind::Unknown && branch.kind != LocalKind::Int) {
        return std::nullopt;
    }
    const bool typedUse = branch.kind == LocalKind::Int;
    const auto lhs = intValue(facts[branch.left.index], typedUse);
    const auto rhs = intValue(facts[branch.right.index], typedUse);
    if (!lhs || !rhs) return std::nullopt;
    switch (branch.comparison) {
    case Comparison::Equal: return *lhs == *rhs;
    case Comparison::NotEqual: return *lhs != *rhs;
    case Comparison::Less: return *lhs < *rhs;
    case Comparison::GreaterEqual: return *lhs >= *rhs;
    case Comparison::Greater: return *lhs > *rhs;
    case Comparison::LessEqual: return *lhs <= *rhs;
    }
    return std::nullopt;
}

}   // namespace

std::optional<MethodIntFlow> analyzeIntFlow(const MethodBody& body)
{
    WorkBudget budget;
    auto checkedEdges = checkGraph(body, budget);
    if (!checkedEdges) return std::nullopt;
    const auto& edges = *checkedEdges;

    const auto count = body.blocks.size();
    std::vector<std::optional<Entry>> entries(count);
    entries.front().emplace();
    std::deque<ca::usize> worklist{0};
    std::vector<bool> queued(count, false);
    queued.front() = true;
    while (!worklist.empty()) {
        const auto current = worklist.front();
        worklist.pop_front();
        queued[current] = false;
        if (edges[current].count == 0) continue;

        const auto& block = body.blocks[current];
        const auto& entry = *entries[current];
        if (!budget.charge(1) || !budget.charge(entry.locals.size())
            || !budget.charge(block.statements.size()) || !budget.charge(block.outgoingArguments.size())) {
            return std::nullopt;
        }
        auto locals = entry.locals;
        const auto facts = detail::transferBlockConstants(block, locals, entry.parameters);
        ConstantFacts arguments;
        arguments.reserve(block.outgoingArguments.size());
        for (auto argument : block.outgoingArguments) arguments.push_back(facts[argument.index]);

        for (ca::usize i = 0; i < edges[current].count; ++i) {
            const auto target = edges[current].targets[i].index;
            auto& next = entries[target];
            const auto joinCost = next ? next->locals.size() : locals.size();
            if (!budget.charge(joinCost) || !budget.charge(arguments.size())) return std::nullopt;
            if (mergeInto(next, locals, arguments) && !queued[target]) {
                worklist.push_back(target);
                queued[target] = true;
            }
        }
    }

    MethodIntFlow result;
    result.values.resize(count);
    result.branches.resize(count);
    for (ca::usize i = 0; i < count; ++i) {
        const auto& block = body.blocks[i];
        auto& values = result.values[i];
        values.resize(block.statements.size());
        if (!entries[i]) continue;
        auto& entry = *entries[i];
        if (!budget.charge(entry.locals.size()) || !budget.charge(block.statements.size())) {
            return std::nullopt;
        }
        const auto facts = detail::transferBlockConstants(block, entry.locals, entry.parameters);
        for (ca::usize j = 0; j < facts.size(); ++j) values[j] = intValue(facts[j]);
        if (const auto* branch = std::get_if<Branch>(&block.terminator)) {
            result.branches[i] = branchValue(*branch, facts);
        }
    }
    return result;
}

}   // namespace mjbase::ast
