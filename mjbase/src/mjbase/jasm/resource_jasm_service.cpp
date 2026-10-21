#include "resource_jasm_service.hpp"

#include "jbc/core/reader.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"

#include <exception>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace mjbase::jasm {
namespace {

using ca::core::Err;
using ca::core::Ok;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string_view view(ca::str::Utf8StringRef value)
{
    return std::string_view(reinterpret_cast<const char*>(value.data()), value.byte_length());
}

template<typename T>
Result<T> resourceError(const std::string& message)
{
    Result<T> result;
    result.diagnostics.push_back(
        {DiagnosticSeverity::Error, DiagnosticStage::Resource, utf16(message), std::nullopt});
    return result;
}

// 读取 class internal name（jbc 骨架解析）；字节不可解析返回 Err(ReadError)。
// 与 jvm_jasm_service.cpp 的同名 helper 保持同一语义。
ca::core::Result<ca::str::Utf8String, jbc::core::ReadError>
readInternalName(const std::vector<ca::u8>& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) return Err(std::move(read).unwrap_err());
    auto                parsed = std::move(read).unwrap();   // ReadResult 持 unique_ptr，只能移出
    const std::string_view name = parsed.file->thisName();
    return Ok(ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(name.data()),
                                             name.size()));
}

// 字节不可解析时的稳定 Error 诊断（落在 Resource 阶段）。
template<typename T>
Result<T> unreadableClassResult(jbc::core::ReadError error)
{
    return resourceError<T>(std::string("invalid class bytes: ") + jbc::core::to_string(error));
}

void appendDiagnostics(std::vector<Diagnostic>& target, std::vector<Diagnostic> source)
{
    target.insert(target.end(),
                  std::make_move_iterator(source.begin()),
                  std::make_move_iterator(source.end()));
}

// 复制容器中全部 Class 资源，目标 URI 的字节换成候选字节——事务预览的工作区。
model::ResourceContainer candidateContainer(const model::ResourceContainer& container,
                                            const std::string&              targetUri,
                                            const std::vector<ca::u8>&      candidateBytes)
{
    model::ResourceContainer candidate;
    container.visitResources([&](std::string_view uri, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (!classResource) return;
        candidate.addResource(uri,
                              std::make_unique<model::ClassResource>(
                                  view(classResource->getLocation()),
                                  std::string_view(targetUri) == uri ? candidateBytes
                                                                     : classResource->get()));
    });
    return candidate;
}

// 容器 → 层次图即时重建：只读 Class 资源的 this/super/接口骨架（jbc 骨架解析）。
// 畸形条目跳过，不进图。帧计算（InheritanceChecker）只依赖类图，成员摘要不参与；
// 层次错误（DuplicateClass/Cycle）按跳过处理——服务这里只取「可用的类图」。
mjbase::hierarchy::ClassHierarchy buildHierarchy(const model::ResourceContainer& container,
                                                 mjbase::KeyPool&                pool)
{
    mjbase::hierarchy::ClassHierarchy hierarchy(pool);
    container.visitResources([&](std::string_view, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (!classResource) return;
        const auto& bytes = classResource->get();
        auto        read  = jbc::core::readClass(jbc::core::ByteView(bytes));
        if (read.is_err()) return;
        auto        parsed = std::move(read).unwrap();   // ReadResult 持 unique_ptr，只能移出
        const auto& file   = *parsed.file;

        mjbase::hierarchy::ClassSummary summary;
        summary.self = mjbase::classKeyOf(pool, file.thisName());
        const std::string_view superName = file.superName();
        summary.super = superName.empty() ? mjbase::ClassKey{} : mjbase::classKeyOf(pool, superName);
        for (const ca::u16 ifaceCp : file.interfaces) {
            summary.interfaces.push_back(mjbase::classKeyOf(pool, file.className(ifaceCp)));
        }
        (void)hierarchy.addClass(summary);
    });
    return hierarchy;
}

}   // namespace

Result<JvmJasmDisassembly> ResourceJasmService::disassemble(const std::string& uri) const
{
    const auto* resource =
        ca::core::tag_cast::dyn_cast<const model::ClassResource>(container_->getResource(uri));
    if (!resource) return resourceError<JvmJasmDisassembly>("Class resource not found: " + uri);
    return jasm_.disassemble(resource->get());
}

Result<JvmJasmClass> ResourceJasmService::assemble(const std::string&      uri,
                                                   ca::str::Utf16StringRef sourceName,
                                                   ca::str::Utf16StringRef source,
                                                   JvmJasmContext          context)
{
    model::IResource* rawResource = container_->getResource(uri);
    if (!ca::core::tag_cast::isa<model::ClassResource>(rawResource))
        return resourceError<JvmJasmClass>("Class resource not found: " + uri);
    auto* resource = ca::core::tag_cast::cast<model::ClassResource>(rawResource);

    try {
        auto expected = readInternalName(resource->get());
        if (expected.is_err()) return unreadableClassResult<JvmJasmClass>(expected.unwrap_err());
        const ca::str::Utf8String expectedName = std::move(expected).unwrap();

        // 当前容器的层次图（每次调用即时重建，无跨调用缓存）。
        mjbase::KeyPool               pool;
        mjbase::hierarchy::ClassHierarchy currentHierarchy = buildHierarchy(*container_, pool);

        context.hierarchy = &currentHierarchy;
        auto preliminary  = jasm_.assemble(sourceName, source, context);
        if (!preliminary.isOk()) return preliminary;
        if (preliminary.value->internalName != expectedName) {
            auto result = resourceError<JvmJasmClass>(
                "Class name mismatch for " + uri + ": expected " + expectedName.to_std_string() +
                ", got " + preliminary.value->internalName.to_std_string());
            appendDiagnostics(result.diagnostics, std::move(preliminary.diagnostics));
            return result;
        }

        // 候选工作区：候选容器的层次图全部用新字节重建后复验。
        auto candidate                   = candidateContainer(*container_, uri,
                                                               preliminary.value->bytes);
        mjbase::hierarchy::ClassHierarchy candidateHierarchy = buildHierarchy(candidate, pool);

        context.hierarchy = &candidateHierarchy;
        auto compiled     = jasm_.assemble(sourceName, source, context);
        if (!compiled.isOk()) return compiled;
        if (compiled.value->internalName != expectedName)
            return resourceError<JvmJasmClass>("Class name changed between compilation passes");

        auto validated = jasm_.validate(compiled.value->bytes, context);
        if (!validated.isOk()) {
            appendDiagnostics(validated.diagnostics, std::move(compiled.diagnostics));
            return validated;
        }

        JvmJasmClass            output            = std::move(*compiled.value);
        std::vector<Diagnostic> outputDiagnostics = std::move(compiled.diagnostics);
        appendDiagnostics(outputDiagnostics, std::move(validated.diagnostics));
        resource->set(output.bytes);
        return {std::move(output), std::move(outputDiagnostics)};
    }
    catch (const std::exception& exception) {
        return resourceError<JvmJasmClass>(exception.what());
    }
}

}   // namespace mjbase::jasm
