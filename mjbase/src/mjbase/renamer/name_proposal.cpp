#include "name_proposal.hpp"

#include "mjbase/asm/analyzer/alias_analysis.hpp"

namespace mjbase::renamer {

namespace {

/// 构造入口统一经进程级别名池驻留（内容寻址；等值即内容等值）。
std::string_view interned(std::string_view value)
{
    return asm_analyzer::aliasInterned(value);
}

}   // namespace

std::string_view rejectionReasonName(RejectionReason reason)
{
    switch (reason) {
    case RejectionReason::None: return "none";
    case RejectionReason::Schema: return "schema";
    case RejectionReason::IllegalIdentifier: return "illegal_identifier";
    case RejectionReason::Keyword: return "keyword";
    case RejectionReason::ReservedName: return "reserved_name";
    case RejectionReason::NotFound: return "not_found";
    case RejectionReason::Conflict: return "conflict";
    case RejectionReason::ProviderError: return "provider_error";
    case RejectionReason::ApplyFailed: return "apply_failed";
    }
    return "unknown";
}

NameProposal::NameProposal(ProposalKind kindIn,
                           std::string_view ownerIn,
                           std::string_view oldNameIn,
                           std::string_view descIn,
                           std::string_view newNameIn,
                           double           confidenceIn,
                           std::string_view sourceIn)
    : kind(kindIn)
    , owner(interned(ownerIn))
    , oldName(interned(oldNameIn))
    , desc(interned(descIn))
    , newName(interned(newNameIn))
    , confidence(confidenceIn)
    , source(interned(sourceIn))
{}

NameProposal::NameProposal(ProposalKind kindIn,
                           std::string_view ownerIn,
                           std::string_view oldNameIn,
                           std::string_view descIn,
                           std::string_view newNameIn,
                           double           confidenceIn,
                           std::string_view sourceIn,
                           std::string_view rationaleIn,
                           std::string_view evidenceIn)
    : kind(kindIn)
    , owner(interned(ownerIn))
    , oldName(interned(oldNameIn))
    , desc(interned(descIn))
    , newName(interned(newNameIn))
    , confidence(confidenceIn)
    , source(interned(sourceIn))
    , rationale(interned(rationaleIn))
    , evidence(interned(evidenceIn))
{}

}   // namespace mjbase::renamer
