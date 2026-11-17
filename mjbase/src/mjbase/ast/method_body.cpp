#include "method_body.hpp"

namespace mjbase::ast {

std::vector<CallSite> directCalls(const MethodBody& body)
{
    std::vector<CallSite> sites;
    for (ca::usize block = 0; block < body.blocks.size(); ++block) {
        const auto& statements = body.blocks[block].statements;
        for (ca::usize index = 0; index < statements.size(); ++index) {
            const Statement& statement = statements[index];
            const Call* call = nullptr;
            if (const auto* let = std::get_if<Let>(&statement)) {
                call = std::get_if<Call>(&let->expression);
            } else if (const auto* invoke = std::get_if<CallStatement>(&statement)) {
                call = &invoke->call;
            }
            if (call) sites.push_back({BlockId{block}, index, call->target, call->dispatch});
        }
    }
    return sites;
}

}   // namespace mjbase::ast
