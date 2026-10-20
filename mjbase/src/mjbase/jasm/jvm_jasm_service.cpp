#include "jvm_jasm_service.hpp"

#include "jhasm/jhasm.hpp"
#include "jblw/version/java_version.hpp"
#include "jbc/core/reader.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace mjbase::jasm {
namespace {

using ca::core::Err;
using ca::core::Ok;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

ca::str::Utf8String own(ca::str::Utf8StringRef value)
{
    return ca::str::Utf8String::from_data(value.data(), value.byte_length(), value.length());
}

ca::str::Utf8String ownBytes(std::string_view value)
{
    return ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                          value.size());
}

std::string_view view(ca::str::Utf8StringRef value)
{
    return std::string_view(reinterpret_cast<const char*>(value.data()), value.byte_length());
}

// mjbase::hierarchy::ClassHierarchy → jhasm InheritanceChecker 适配器。
// 语义照 旧栈 ClassHierarchy 适配：任一侧未登记时 isSubclassOf 退化为
// 名字相等，commonSuperclass 退化为 java/lang/Object。
class HierarchyInheritanceChecker final : public ::jhasm::analysis::InheritanceChecker
{
public:
    explicit HierarchyInheritanceChecker(const mjbase::hierarchy::ClassHierarchy& hierarchy)
        : hierarchy_(&hierarchy)
    {}

    bool isSubclassOf(ca::str::Utf8StringRef child, ca::str::Utf8StringRef parent) const override
    {
        const mjbase::ClassKey childKey  = hierarchy_->findClass(view(child));
        const mjbase::ClassKey parentKey = hierarchy_->findClass(view(parent));
        if (!childKey.valid() || !parentKey.valid()) return child == parent;
        return hierarchy_->isSubtype(childKey, parentKey);
    }

    ca::str::Utf8String commonSuperclass(ca::str::Utf8StringRef first,
                                         ca::str::Utf8StringRef second) const override
    {
        if (first == second) return own(first);
        const mjbase::ClassKey firstKey  = hierarchy_->findClass(view(first));
        const mjbase::ClassKey secondKey = hierarchy_->findClass(view(second));
        if (!firstKey.valid() || !secondKey.valid())
            return ca::str::Utf8String::from_cstr("java/lang/Object");
        const mjbase::ClassKey commonKey = hierarchy_->commonAncestor(firstKey, secondKey);
        if (!commonKey.valid()) return ca::str::Utf8String::from_cstr("java/lang/Object");
        return ownBytes(hierarchy_->nameOf(commonKey));
    }

private:
    const mjbase::hierarchy::ClassHierarchy* hierarchy_;
};

SourceLocation sourceLocation(const ::jhasm::util::Location& location)
{
    SourceLocation result;
    result.line   = location.line();
    result.column = location.column();
    result.length = location.length();
    if (location.has_source()) result.source = location.source().to_string();
    return result;
}

Diagnostic diagnostic(const ::jhasm::error::Error& error, DiagnosticSeverity severity,
                      DiagnosticStage stage)
{
    std::optional<SourceLocation> location;
    if (error.location()) location = sourceLocation(*error.location());
    return {severity, stage, error.message().to_string(), std::move(location)};
}

template<typename T>
std::vector<Diagnostic> diagnostics(const ::jhasm::error::Result<T>& result,
                                    DiagnosticStage                  stage)
{
    std::vector<Diagnostic> output;
    output.reserve(result.errors().size() + result.warnings().size());
    for (const auto& error : result.errors())
        output.push_back(diagnostic(error, DiagnosticSeverity::Error, stage));
    for (const auto& warning : result.warnings())
        output.push_back(diagnostic(warning, DiagnosticSeverity::Warning, stage));
    return output;
}

template<typename T>
Result<T> exceptionResult(DiagnosticStage stage, const std::exception& exception)
{
    Result<T> result;
    result.diagnostics.push_back(
        {DiagnosticSeverity::Error, stage, utf16(exception.what()), std::nullopt});
    return result;
}

// 字节不可解析时的稳定 Error 诊断（旧栈 侧由 ClassReader 异常路径产生）。
template<typename T>
Result<T> unreadableClassResult(DiagnosticStage stage, jbc::core::ReadError error)
{
    Result<T> result;
    result.diagnostics.push_back(
        {DiagnosticSeverity::Error, stage,
         utf16(std::string("invalid class bytes: ") + jbc::core::to_string(error)),
         std::nullopt});
    return result;
}

// 读取 class internal name（jbc 骨架解析，Recovered 状态下骨架仍可读）。
// 字节不可解析时返回 Err(ReadError)。
ca::core::Result<ca::str::Utf8String, jbc::core::ReadError>
readInternalName(const std::vector<ca::u8>& classBytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(classBytes));
    if (read.is_err()) return Err(std::move(read).unwrap_err());
    auto parsed = std::move(read).unwrap();   // ReadResult 持 unique_ptr，只能移出
    return Ok(ownBytes(parsed.file->thisName()));
}

}   // namespace

Result<JvmJasmClass> JvmJasmService::assemble(ca::str::Utf16StringRef sourceName,
                                              ca::str::Utf16StringRef source,
                                              const JvmJasmContext&   context) const
{
    try {
        ::jhasm::compile::JvmCompilerOptions options;
        options.version      = jblw::version::JavaVersion::classVersion(context.classMajorVersion,
                                                                   context.classMinorVersion);
        options.maxStackHint = context.maxStackHint;

        std::optional<HierarchyInheritanceChecker> checker;
        if (context.hierarchy) {
            checker.emplace(*context.hierarchy);
            options.inheritance = &*checker;
        }

        auto                 compiled = ::jhasm::assemble(sourceName, source, options);
        Result<JvmJasmClass> result;
        result.diagnostics = diagnostics(compiled, DiagnosticStage::Assemble);
        if (!compiled.is_ok()) return result;

        auto value = compiled.take_value();
        auto name  = readInternalName(value->bytes);
        if (name.is_err())
            return unreadableClassResult<JvmJasmClass>(DiagnosticStage::Assemble,
                                                       name.unwrap_err());
        JvmJasmClass output;
        output.internalName = std::move(name).unwrap();
        output.bytes        = std::move(value->bytes);
        result.value        = std::move(output);
        return result;
    }
    catch (const std::exception& exception) {
        return exceptionResult<JvmJasmClass>(DiagnosticStage::Assemble, exception);
    }
}

Result<JvmJasmDisassembly> JvmJasmService::disassemble(const std::vector<ca::u8>& classBytes) const
{
    try {
        auto name = readInternalName(classBytes);
        if (name.is_err())
            return unreadableClassResult<JvmJasmDisassembly>(DiagnosticStage::Disassemble,
                                                             name.unwrap_err());
        // libjdasm 是 libjhasm disassemble 的纯转发门面；mjbase 未依赖
        // libjdasm，直接取同一实现（语义逐位一致）。
        auto                       disassembled = ::jhasm::disassemble(classBytes);
        Result<JvmJasmDisassembly> result;
        result.diagnostics = diagnostics(disassembled, DiagnosticStage::Disassemble);
        if (!disassembled.is_ok()) return result;

        auto source  = disassembled.take_value();
        result.value = JvmJasmDisassembly{std::move(name).unwrap(), std::move(*source)};
        return result;
    }
    catch (const std::exception& exception) {
        return exceptionResult<JvmJasmDisassembly>(DiagnosticStage::Disassemble, exception);
    }
}

Result<JvmJasmMethodDisassembly> JvmJasmService::disassembleMethod(
    const std::vector<ca::u8>& classBytes, ca::str::Utf8StringRef name,
    ca::str::Utf8StringRef descriptor) const
{
    try {
        auto internalName = readInternalName(classBytes);
        if (internalName.is_err())
            return unreadableClassResult<JvmJasmMethodDisassembly>(DiagnosticStage::Disassemble,
                                                                   internalName.unwrap_err());
        auto disassembled = ::jhasm::disassembleMethod(classBytes, name, descriptor);
        Result<JvmJasmMethodDisassembly> result;
        result.diagnostics = diagnostics(disassembled, DiagnosticStage::Disassemble);
        if (!disassembled.is_ok()) return result;

        auto source = disassembled.take_value();
        result.value = JvmJasmMethodDisassembly{std::move(internalName).unwrap(), own(name),
                                                own(descriptor), std::move(*source)};
        return result;
    }
    catch (const std::exception& exception) {
        return exceptionResult<JvmJasmMethodDisassembly>(DiagnosticStage::Disassemble, exception);
    }
}

Result<JvmJasmClass> JvmJasmService::validate(const std::vector<ca::u8>& classBytes,
                                              const JvmJasmContext&      context) const
{
    try {
        auto name = readInternalName(classBytes);
        if (name.is_err())
            return unreadableClassResult<JvmJasmClass>(DiagnosticStage::Validate,
                                                       name.unwrap_err());
        std::optional<HierarchyInheritanceChecker>   checker;
        const ::jhasm::analysis::InheritanceChecker* inheritance =
            &::jhasm::analysis::EmptyInheritanceChecker::instance();
        if (context.hierarchy) {
            checker.emplace(*context.hierarchy);
            inheritance = &*checker;
        }

        auto                 analyzed = ::jhasm::analyze(classBytes, *inheritance);
        Result<JvmJasmClass> result;
        result.diagnostics = diagnostics(analyzed, DiagnosticStage::Validate);
        if (!analyzed.is_ok()) return result;
        result.value = JvmJasmClass{std::move(name).unwrap(), classBytes};
        return result;
    }
    catch (const std::exception& exception) {
        return exceptionResult<JvmJasmClass>(DiagnosticStage::Validate, exception);
    }
}

}   // namespace mjbase::jasm
