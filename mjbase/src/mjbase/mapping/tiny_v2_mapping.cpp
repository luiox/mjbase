#include "tiny_v2_mapping.hpp"
#include <vector>
#include <algorithm>

namespace mjbase::mapping {

using Utf8StringRef     = ca::str::Utf8StringRef;
using Utf8StringBuilder = ca::str::Utf8StringBuilder;

ca::str::Utf8StringRef TinyV2Mapping::getFormatName() const
{
    return ca::str::Utf8StringRef::from_cstr("Tiny-V2");
}

ca::Result<IntermediateMapping, MappingError> TinyV2Mapping::parse(Utf8StringRef mappingText)
{
    IntermediateMapping mappings;
    auto                lines = mappingText.split(Utf8StringRef::from_cstr("\n"));

    Utf8StringRef currentClass;
    auto          tabRef  = Utf8StringRef::from_cstr("\t");
    auto          hashRef = Utf8StringRef::from_cstr("#");
    auto          cRef    = Utf8StringRef::from_cstr("c");
    auto          fRef    = Utf8StringRef::from_cstr("f");
    auto          mRef    = Utf8StringRef::from_cstr("m");

    for (const auto& line : lines) {
        if (line.is_empty() || line.starts_with(hashRef)) continue;

        if (line.starts_with(tabRef)) {
            auto body = line;
            while (body.starts_with(tabRef)) body = body.slice(1, body.byte_length());

            auto parts = body.split(tabRef);
            if (parts.size() < 3) continue;

            auto          kind = parts[0];
            Utf8StringRef desc;
            Utf8StringRef oldName;
            Utf8StringRef newName;

            if (parts.size() >= 4 && (parts[1].starts_with(Utf8StringRef::from_cstr("(")) ||
                                      parts[1].starts_with(Utf8StringRef::from_cstr("[")) ||
                                      parts[1].starts_with(Utf8StringRef::from_cstr("L")) ||
                                      parts[1].byte_length() == 1)) {
                desc    = parts[1];
                oldName = parts[2];
                newName = parts[3];
            }
            else {
                oldName = parts[1];
                newName = parts[2];
                if (parts.size() >= 4) desc = parts[3];
            }

            if (kind.equals(fRef) && !currentClass.is_empty()) {
                mappings.addField(currentClass, desc, oldName, newName);
            }
            else if (kind.equals(mRef) && !currentClass.is_empty()) {
                mappings.addMethod(currentClass, desc, oldName, newName);
            }
        }
        else {
            auto parts = line.split(tabRef);
            if (parts.size() >= 3 && parts[0].equals(cRef)) {
                currentClass = parts[1];
                mappings.addClass(parts[1], parts[2]);
            }
        }
    }

    return ca::Ok(std::move(mappings));
}

ca::Result<ca::str::Utf8String, MappingError> TinyV2Mapping::format(
    const IntermediateMapping& mappings)
{
    Utf8StringBuilder sb;
    sb.append(Utf8StringRef::from_cstr("tiny\t2\t0\tintermediary\tnamed\n"));

    auto sortedClasses = std::vector<ca::str::Utf8StringRef>{};
    for (const auto& [k, v] : mappings.getClasses()) sortedClasses.push_back(k);
    std::sort(sortedClasses.begin(), sortedClasses.end(), [](Utf8StringRef a, Utf8StringRef b) {
        return a.compare(b) < 0;
    });

    for (const auto& className : sortedClasses) {
        auto* cm = mappings.getClassMapping(className);
        if (!cm) continue;

        sb.append_code_point('c');
        sb.append_code_point('\t');
        sb.append(cm->oldName);
        sb.append_code_point('\t');
        sb.append(cm->newName);
        sb.append_code_point('\n');

        auto* fields = mappings.getClassFieldMappings(className);
        if (fields) {
            for (const auto& fm : *fields) {
                sb.append_code_point('\t');
                sb.append_code_point('f');
                sb.append_code_point('\t');
                if (!fm.desc.is_empty()) {
                    sb.append(fm.desc);
                }
                sb.append_code_point('\t');
                sb.append(fm.oldName);
                sb.append_code_point('\t');
                sb.append(fm.newName);
                sb.append_code_point('\n');
            }
        }

        auto* methods = mappings.getClassMethodMappings(className);
        if (methods) {
            for (const auto& mm : *methods) {
                sb.append_code_point('\t');
                sb.append_code_point('m');
                sb.append_code_point('\t');
                if (!mm.desc.is_empty()) {
                    sb.append(mm.desc);
                }
                sb.append_code_point('\t');
                sb.append(mm.oldName);
                sb.append_code_point('\t');
                sb.append(mm.newName);
                sb.append_code_point('\n');
            }
        }
    }

    return ca::Ok(sb.build());
}

}   // namespace mjbase::mapping
