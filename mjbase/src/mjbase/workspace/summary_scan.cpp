#include "mjbase/workspace/summary_scan.hpp"

#include "jbc/core/reader.hpp"

namespace mjbase::workspace {

WorkspaceResult<ClassSummary> decodeClassSummary(const std::string&          uri,
                                                 const model::ClassResource& resource)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(resource.get()));
    if (read.is_err())
        return ca::core::Err(WorkspaceErrorInfo{
            WorkspaceError::DataLoss,
            "decode class summary " + uri + ": " +
                jbc::core::to_string(std::move(read).unwrap_err())});
    auto                        parsed = std::move(read).unwrap();
    const jbc::core::ClassFile& cf     = *parsed.file;

    ClassSummary output;
    output.resourceUri   = uri;
    output.internalName  = std::string{cf.thisName()};
    output.superName     = std::string{cf.superName()};
    output.access        = static_cast<ca::i32>(cf.access);
    // ASM 版本语义：version = major | (minor << 16)。
    output.version       = static_cast<ca::i32>(cf.major) | (static_cast<ca::i32>(cf.minor) << 16);
    output.interfaceCount = cf.interfaces.size();
    output.fieldCount     = cf.fields.size();
    output.methodCount    = cf.methods.size();
    return ca::core::Ok(std::move(output));
}

}   // namespace mjbase::workspace
