#include "mjbase/renamer/shadow_alias_store.hpp"

#include <string_view>
#include <utility>
#include <vector>

namespace mjbase::renamer {
namespace {

ca::str::Utf8String utf8Of(std::string_view text)
{
    if (text.empty()) return ca::str::Utf8String();
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(text.data()), text.size());
}

}   // namespace

ca::Result<void, project::AnalysisDbError> importShadowAliases(project::AnalysisDb& db,
                                                               const ShadowAliasTable& table)
{
    std::vector<project::DbRenameRecord> records;
    records.reserve(table.size());

    for (const auto& entry : table.entries()) {
        project::DbRenameRecord record;
        record.shadow = utf8Of(entry.shadowName);
        record.token  = utf8Of(entry.token8);

        if (entry.kind == ShadowKind::Class) {
            record.target = project::RenameTargetKind::Class;
            record.owner  = utf8Of(entry.rawName);
        }
        else {
            record.target     = project::RenameTargetKind::Member;
            record.memberKind = entry.kind == ShadowKind::Method
                                    ? project::DbMemberKind::Method
                                    : project::DbMemberKind::Field;
            record.owner = utf8Of(entry.owner);
            record.name  = utf8Of(entry.rawName);
            record.desc  = utf8Of(entry.desc);
        }
        records.push_back(std::move(record));
    }

    return db.replaceRenames(std::move(records));
}

ca::Result<ShadowAliasTable, project::AnalysisDbError> loadShadowAliases(
    const project::AnalysisDb& db)
{
    auto listed = db.listRenames();
    if (listed.is_err()) return ca::Err(std::move(listed).unwrap_err());
    const auto records = std::move(listed).unwrap();

    mapping::IntermediateMapping mappings;
    for (const auto& record : records) {
        const auto owner  = ca::str::Utf8StringRef(record.owner);
        const auto shadow = ca::str::Utf8StringRef(record.shadow);
        const auto token  = ca::str::Utf8StringRef(record.token);

        if (record.target == project::RenameTargetKind::Class) {
            mappings.addClass(owner, shadow, token);
        }
        else if (record.memberKind == project::DbMemberKind::Method) {
            mappings.addMethod(owner, ca::str::Utf8StringRef(record.desc),
                               ca::str::Utf8StringRef(record.name), shadow, token);
        }
        else {
            mappings.addField(owner, ca::str::Utf8StringRef(record.desc),
                              ca::str::Utf8StringRef(record.name), shadow, token);
        }
    }

    return ca::Ok(ShadowAliasTable::fromMapping(mappings));
}

}   // namespace mjbase::renamer
