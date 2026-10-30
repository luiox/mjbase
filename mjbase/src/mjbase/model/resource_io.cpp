/// @file        resource_io.cpp
/// @brief       资源导入导出实现（jar 分拣、class 文件 IO）。
#include "resource_io.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::model {

// ── JarFileImporter ──────────────────────────────────────────

// JarError → ModelError 归一（见 resource_io.hpp 声明）。jar 层错误在 mbase
// 以异常穿透到调用方；本模块以码表收口，文件缺失细分为 NotFound。
ModelError toModelError(const mjbase::jar::JarError& error) noexcept
{
    if (error.kind == mjbase::jar::JarError::Kind::FileNotFound) return ModelError::NotFound;
    return ModelError::Io;
}

namespace {

// 按 entry 名/内容分拣为 Manifest / Class / Unknown（import 的两个入口共用）。
// entry 名在 mjbase::jar 边界上是 Utf8String（原始字节直通，零解码口径不变），
// 跨入容器时经借用视图零拷贝转换。
void classifyAndAdd(IResourceContainer& container, const ca::str::Utf8String& entryName,
                    const std::vector<ca::u8>& data)
{
    const auto name = entryName.ref();
    if (name == mjbase::jar::JarUtil::kManifestFileName) {
        container.addResource(
            name, std::make_unique<ManifestResource>(ManifestResource::fromBytes(data)));
        return;
    }
    if (mjbase::jar::JarUtil::isClassFile(name) && mjbase::jar::JarUtil::isClassFile(data)) {
        // 混淆器可能把类 entry 命名为 *.class/（尾斜杠），剥掉后再用作 URI/location，
        // 避免尾斜杠泄漏到类名派生与容器 key。
        if (name.byte_length() >= 7 &&
            name.slice(name.byte_length() - 7, name.byte_length()) == ".class/") {
            const auto normalized = name.slice(0, name.byte_length() - 1);
            container.addResource(normalized, std::make_unique<ClassResource>(normalized, data));
            return;
        }
        container.addResource(name, std::make_unique<ClassResource>(name, data));
        return;
    }
    container.addResource(name, std::make_unique<UnknownResource>(name, data));
}

}   // namespace

// 遍历 jar 条目，根据类型分拣为 ManifestResource / ClassResource / UnknownResource
ModelResult<void> JarFileImporter::importResource(IResourceContainer& container)
{
    // jar 模块错误走 Result<void, JarError>（mbase 为异常穿透），此处归一映射。
    auto r = reader_->read([&](const ca::str::Utf8String& entryName,
                               const std::vector<ca::u8>& data) {
        classifyAndAdd(container, entryName, data);
    });
    if (r.is_err()) return Err(toModelError(std::move(r).unwrap_err()));
    return Ok();
}

// 带进度的导入：每个条目回调 progress，单次解析无重复 IO（见 jar_reader readWithProgress）。
ModelResult<void> JarFileImporter::importResourceWithProgress(IResourceContainer&     container,
                                                              const ProgressCallback& progress)
{
    auto r = reader_->readWithProgress(
        [&](int index, int total, const ca::str::Utf8String& entryName,
            const std::vector<ca::u8>& data) {
            classifyAndAdd(container, entryName, data);
            if (progress) progress(index, total, entryName.ref());
        });
    if (r.is_err()) return Err(toModelError(std::move(r).unwrap_err()));
    return Ok();
}

// ── JarFileExporter ──────────────────────────────────────────

// 遍历容器中所有资源，按类型写入 jar 条目
ModelResult<void> JarFileExporter::exportResource(IResourceContainer& container)
{
    auto r = writer_->write([&](mjbase::jar::IEntryWriter& entryWriter) {
        container.visitResources([&](std::string_view, const IResource& resource) {
            if (auto* cls = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource)) {
                entryWriter.writeEntry(cls->getLocation(), cls->get());
            }
            else if (auto* mf = ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource)) {
                entryWriter.writeEntry(
                    ca::str::Utf8StringRef::from_cstr(mjbase::jar::JarUtil::kManifestFileName),
                    mf->get().toBytes());
            }
            else if (auto* unk = ca::core::tag_cast::dyn_cast<const UnknownResource>(&resource)) {
                entryWriter.writeEntry(unk->getLocation(), unk->get());
            }
        });
    });
    if (r.is_err()) return Err(toModelError(std::move(r).unwrap_err()));
    return Ok();
}

// ── SimpleClassFileExporter ──────────────────────────────────

SimpleClassFileExporter::SimpleClassFileExporter(std::string_view uri, std::string_view path)
    : uri_(ownText(uri))
    , path_(ownText(path))
{}

// 按 URI 查找 ClassResource 并写出到磁盘；空 URI 时使用容器中唯一的资源
ModelResult<void> SimpleClassFileExporter::exportResource(IResourceContainer& container)
{
    const ClassResource* classRes = nullptr;
    if (uri_.ref().is_empty()) {
        // 未指定 URI，取容器中唯一的资源（必须恰好一个且为 ClassResource）
        if (container.size() != 1)
            return Err(ModelError::BadArgument);   // mbase: "uri is empty and container size is not 1"
        container.visitResources([&](std::string_view, const IResource& res) {
            classRes = ca::core::tag_cast::dyn_cast<const ClassResource>(&res);
        });
        if (!classRes)
            return Err(ModelError::TypeMismatch);   // mbase: "the only resource is not ClassResource"
    }
    else {
        auto* res = container.getResource(uri_.ref());
        if (!res) return Err(ModelError::NotFound);   // mbase: "uri not found in container: <uri>"
        classRes = ca::core::tag_cast::dyn_cast<const ClassResource>(res);
        if (!classRes)
            return Err(ModelError::TypeMismatch);   // mbase: "resource is not ClassResource: <uri>"
    }
    const std::string pathText(path_.ref());
    std::ofstream     out(std::filesystem::u8path(pathText), std::ios::binary);
    if (!out) return Err(ModelError::Io);   // mbase: "failed to open: <path>"
    const auto& data = classRes->get();
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) return Err(ModelError::Io);   // mbase: "failed to flush: <path>"
    out.close();
    if (!out) return Err(ModelError::Io);   // mbase: "failed to close: <path>"
    return Ok();
}

// ── SimpleClassFileImporter ──────────────────────────────────

// 从磁盘读取 class 文件字节并作为 ClassResource 加入容器
ModelResult<void> SimpleClassFileImporter::importResource(IResourceContainer& container)
{
    std::ifstream in(std::filesystem::u8path(std::string(path_.ref())), std::ios::binary);
    if (!in) return Err(ModelError::Io);   // mbase: "failed to open: <path>"
    in.seekg(0, std::ios::end);
    auto size = static_cast<ca::usize>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<ca::u8> bytes(size);
    if (size > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    container.addResource(path_.ref(),
                          std::make_unique<ClassResource>(path_.ref(), std::move(bytes)));
    return Ok();
}

}   // namespace mjbase::model
