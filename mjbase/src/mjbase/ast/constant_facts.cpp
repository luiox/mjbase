#include "constant_facts.hpp"

#include <unordered_map>

#include "jstd/arith.hpp"

namespace mjbase::ast {
namespace {

using Constant = std::optional<KnownConstant>;
using Facts = std::vector<Constant>;
namespace arithmetic = jstd::arith;

Constant factOf(const Facts& facts, ValueId value)
{
    if (value.index >= facts.size()) return std::nullopt;
    return facts[value.index];
}

Constant literalFact(const Literal& literal)
{
    if (const auto* integer = std::get_if<ca::i32>(&literal)) return *integer;
    if (const auto* bits = std::get_if<Bits32>(&literal)) return *bits;
    if (std::holds_alternative<Null>(literal)) return Null{};
    return std::nullopt;
}

std::optional<ca::i32> asInt(const KnownConstant& value)
{
    if (const auto* integer = std::get_if<ca::i32>(&value)) return *integer;
    if (const auto* bits = std::get_if<Bits32>(&value)) return static_cast<ca::i32>(bits->bits);
    return std::nullopt;
}

Constant typedFact(const KnownConstant& value, LocalKind kind)
{
    switch (kind) {
    case LocalKind::Unknown: return value;
    case LocalKind::Int:
        if (const auto integer = asInt(value)) return *integer;
        break;
    case LocalKind::Reference:
        if (std::holds_alternative<Null>(value)) return Null{};
        if (const auto* bits = std::get_if<Bits32>(&value); bits && bits->bits == 0) return Null{};
        break;
    case LocalKind::Long:
    case LocalKind::Float:
    case LocalKind::Double:
        break;
    }
    return std::nullopt;
}

Constant binaryFact(const Facts& facts, const IntBinary& binary)
{
    const auto left = factOf(facts, binary.left);
    const auto right = factOf(facts, binary.right);
    if (!left || !right) return std::nullopt;
    const auto lhs = asInt(*left);
    const auto rhs = asInt(*right);
    if (!lhs || !rhs) return std::nullopt;

    const ca::i32 l = *lhs;
    const ca::i32 r = *rhs;
    switch (binary.op) {
    case IntOp::Add: return arithmetic::add(l, r);
    case IntOp::Subtract: return arithmetic::sub(l, r);
    case IntOp::Multiply: return arithmetic::mul(l, r);
    case IntOp::Divide:
        if (r != 0) return arithmetic::div(l, r);
        break;
    case IntOp::Remainder:
        if (r != 0) return arithmetic::rem(l, r);
        break;
    case IntOp::And: return l & r;
    case IntOp::Or: return l | r;
    case IntOp::Xor: return l ^ r;
    case IntOp::ShiftLeft: return arithmetic::shl(l, r);
    case IntOp::ShiftRight: return arithmetic::shr(l, r);
    case IntOp::UnsignedShiftRight: return arithmetic::ushr(l, r);
    }
    return std::nullopt;
}

}   // namespace

std::vector<std::optional<KnownConstant>> analyzeBlockConstants(const Block& block)
{
    Facts facts;
    facts.reserve(block.statements.size());
    std::unordered_map<ca::usize, KnownConstant> locals;
    for (const auto& statement : block.statements) {
        Constant fact;
        if (const auto* let = std::get_if<Let>(&statement)) {
            if (const auto* literal = std::get_if<Literal>(&let->expression)) {
                fact = literalFact(*literal);
            } else if (const auto* read = std::get_if<ReadLocal>(&let->expression)) {
                const auto local = locals.find(read->local.index);
                if (local != locals.end()) fact = typedFact(local->second, read->kind);
            } else if (const auto* binary = std::get_if<IntBinary>(&let->expression)) {
                fact = binaryFact(facts, *binary);
            }
        } else if (const auto* assign = std::get_if<Assign>(&statement)) {
            if (assign->clobbered) {
                locals.erase(assign->local.index);
                locals.erase(assign->clobbered->index);
            } else if (const auto rhs = factOf(facts, assign->value)) {
                locals.insert_or_assign(assign->local.index, *rhs);
            } else {
                locals.erase(assign->local.index);
            }
        }
        facts.push_back(fact);
    }
    return facts;
}

}   // namespace mjbase::ast
