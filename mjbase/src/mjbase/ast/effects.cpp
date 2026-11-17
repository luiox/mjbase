#include "effects.hpp"

namespace mjbase::ast {

EffectSummary summarizeEffects(const MethodBody& body)
{
    EffectSummary summary;
    for (const auto& block : body.blocks) {
        if (std::holds_alternative<Throw>(block.terminator)) {
            summary.local = LocalEffect::Impure;
        }
        for (const auto& statement : block.statements) {
            if (const auto* let = std::get_if<Let>(&statement)) {
                if (const auto* call = std::get_if<Call>(&let->expression)) {
                    summary.callees.push_back(call->target);
                } else if (summary.local == LocalEffect::Pure
                           && std::holds_alternative<FieldRead>(let->expression)) {
                    summary.local = LocalEffect::ReadOnly;
                }
            } else if (const auto* call = std::get_if<CallStatement>(&statement)) {
                summary.callees.push_back(call->call.target);
            } else if (std::holds_alternative<FieldWrite>(statement)) {
                summary.local = LocalEffect::Impure;
            }
        }
    }
    return summary;
}

}   // namespace mjbase::ast
