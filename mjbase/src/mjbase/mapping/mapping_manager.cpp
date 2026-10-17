#include "mapping_manager.hpp"
#include "libca/fs/file_util.hpp"
#include <cstdio>
#include <string>

using ca::str::Utf8String;
using ca::str::Utf8StringRef;
using ca::str::Utf8StringBuilder;
using ca::fs::FileUtil;

namespace mjbase::mapping {

ca::str::Utf8String MappingManager::formatClassMap(
    const std::unordered_map<Utf8StringRef, ClassMapping>& classes)
{
    Utf8StringBuilder sb;
    sb.append(Utf8StringRef::from_cstr("oldname -> newname\n"));
    for (const auto& [key, cm] : classes) {
        sb.append(cm.oldName);
        sb.append(Utf8StringRef::from_cstr(" -> "));
        sb.append(cm.newName);
        sb.append_code_point('\n');
    }
    return sb.build();
}

ca::str::Utf8String MappingManager::formatMethodMap(
    const std::unordered_map<Utf8StringRef, std::vector<MethodMapping>>& methods)
{
    Utf8StringBuilder sb;
    sb.append(Utf8StringRef::from_cstr("owner desc oldname -> newname\n"));
    for (const auto& [key, mappings] : methods) {
        for (const auto& m : mappings) {
            sb.append(m.ownerName);
            sb.append_code_point(' ');
            sb.append(m.desc);
            sb.append_code_point(' ');
            sb.append(m.oldName);
            sb.append(Utf8StringRef::from_cstr(" -> "));
            sb.append(m.newName);
            sb.append_code_point('\n');
        }
    }
    return sb.build();
}

ca::str::Utf8String MappingManager::formatFieldMap(
    const std::unordered_map<Utf8StringRef, std::vector<FieldMapping>>& fields)
{
    Utf8StringBuilder sb;
    sb.append(Utf8StringRef::from_cstr("owner desc oldname -> newname\n"));
    for (const auto& [key, mappings] : fields) {
        for (const auto& f : mappings) {
            sb.append(f.ownerName);
            sb.append_code_point(' ');
            sb.append(f.desc);
            sb.append_code_point(' ');
            sb.append(f.oldName);
            sb.append(Utf8StringRef::from_cstr(" -> "));
            sb.append(f.newName);
            sb.append_code_point('\n');
        }
    }
    return sb.build();
}

bool MappingManager::writeToFile(Utf8StringRef path, Utf8StringRef content)
{
    return FileUtil::write_text(path.to_std_string(), content.to_std_string()).is_ok();
}

}   // namespace mjbase::mapping
