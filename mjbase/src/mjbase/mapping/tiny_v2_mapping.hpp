#pragma once
#include "mapping_file_format.hpp"

namespace mjbase::mapping {

class TinyV2Mapping : public MappingFileFormat
{
public:
    ca::str::Utf8StringRef getFormatName() const override;

    ca::Result<IntermediateMapping, MappingError> parse(
        ca::str::Utf8StringRef mappingText) override;

    ca::Result<ca::str::Utf8String, MappingError> format(
        const IntermediateMapping& mappings) override;
};

}   // namespace mjbase::mapping
