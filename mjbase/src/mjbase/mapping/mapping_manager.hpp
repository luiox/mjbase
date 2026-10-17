#pragma once
#include <string>
#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "intermediate_mapping.hpp"

namespace mjbase::mapping {

class MappingManager
{
public:
    static ca::str::Utf8String formatClassMap(
        const std::unordered_map<ca::str::Utf8StringRef, ClassMapping>& classes);

    static ca::str::Utf8String formatMethodMap(
        const std::unordered_map<ca::str::Utf8StringRef, std::vector<MethodMapping>>& methods);

    static ca::str::Utf8String formatFieldMap(
        const std::unordered_map<ca::str::Utf8StringRef, std::vector<FieldMapping>>& fields);

    static bool writeToFile(ca::str::Utf8StringRef path, ca::str::Utf8StringRef content);

private:
    MappingManager() = default;
};

}   // namespace mjbase::mapping
