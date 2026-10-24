/// @file        resource_helper.cpp
/// @brief       资源操作辅助工具实现。
#include "resource_helper.hpp"

#include <string>
#include <utility>

#include "libca/core/tag_cast.hpp"
#include "libca/str/utf8_string.hpp"

#include "resource_container.hpp"
#include "resource_io.hpp"

namespace mjbase::model {

// 从 class 文件导入：创建导入器 → 委托容器导入
ModelResult<void> ResourceHelper::importFromClassFile(ResourceContainer& container,
                                                      std::string_view    filePath)
{
    SimpleClassFileImporter importer(filePath);
    return container.importResource(&importer);
}

// 从 jar 导入：默认使用 JvmJarReader（兼容 data descriptor），支持自定义读取器。
// 路径按调用方 string_view 借用转 Utf8StringRef（构造器内单次复制驻留）。
ModelResult<void> ResourceHelper::importFromJar(ResourceContainer& container,
                                                std::string_view   filePath,
                                                std::unique_ptr<mjbase::jar::IJarReader> reader)
{
    if (!reader)
        reader = std::make_unique<mjbase::jar::JvmJarReader>(
            ca::str::Utf8StringRef::from_string_view(filePath));
    JarFileImporter importer(*reader);
    return container.importResource(&importer);
}

// #1110 族解码注入面：自建 JvmJarReader 并注入条目名宽容解码回调（空 = 原始
// 字节直通零漂移）。与 reader 重载分离：decoder（std::function）不与 reader
// （unique_ptr）混排默认参，避免既有 3 参 reader 调用点变义。
ModelResult<void> ResourceHelper::importFromJar(ResourceContainer& container,
                                                std::string_view   filePath,
                                                const mjbase::jar::IJarReader::EntryNameDecoder& decoder)
{
    auto reader = std::make_unique<mjbase::jar::JvmJarReader>(
        ca::str::Utf8StringRef::from_string_view(filePath));
    if (decoder) reader->setEntryNameDecoder(decoder);
    return importFromJar(container, filePath, std::move(reader));
}

// 带进度的 jar 导入：经 readWithProgress 单次解析，每个 entry 回调 progress。
ModelResult<void> ResourceHelper::importFromJar(ResourceContainer&      container,
                                                std::string_view        filePath,
                                                const ProgressCallback& progress,
                                                std::unique_ptr<mjbase::jar::IJarReader> reader)
{
    if (!reader)
        reader = std::make_unique<mjbase::jar::JvmJarReader>(
            ca::str::Utf8StringRef::from_string_view(filePath));
    JarFileImporter importer(*reader);
    // 进度路径不经 ResourceContainer::importResource，须手动开始一轮新导入，
    // 否则 duplicateResourceUris() 会累积上一轮的陈旧记录。
    container.beginImport();
    return importer.importResourceWithProgress(container, progress);
}

// 仅解析条目元数据（不解压），委托 mjbase::jar::listJarEntries（JarError → ModelError）。
ModelResult<std::vector<mjbase::jar::JarEntryInfo>> ResourceHelper::listJarEntries(
    std::string_view path)
{
    auto entries = mjbase::jar::listJarEntries(ca::str::Utf8StringRef::from_string_view(path));
    if (entries.is_err()) return Err(toModelError(std::move(entries).unwrap_err()));
    return Ok(std::move(entries).unwrap());
}

// 导出到 jar：默认使用 JarWriter，支持自定义写入器
ModelResult<void> ResourceHelper::exportToJar(ResourceContainer& container,
                                              std::string_view   filePath,
                                              std::unique_ptr<mjbase::jar::IJarWriter> writer)
{
    // Multi-Release 守护：版本化 class 条目在场时确保主属性在场（幂等，无
    // 版本化条目时零改动）。缺属性时 JVM 9+ 忽略整棵 versions 树，导出面是
    // 保证产出 jar 可用的最后收口点。
    ensureMultiReleaseManifest(container);
    if (!writer)
        writer = std::make_unique<mjbase::jar::JarWriter>(
            ca::str::Utf8StringRef::from_string_view(filePath));
    JarFileExporter exporter(*writer);
    return container.exportResource(&exporter);
}

// 导出单个 class 文件到磁盘
ModelResult<void> ResourceHelper::exportToClassFile(ResourceContainer& container,
                                                    std::string_view   filePath,
                                                    std::string_view   resourcePath)
{
    SimpleClassFileExporter exporter(resourcePath, filePath);
    return container.exportResource(&exporter);
}

// ── Multi-Release 主属性守护 ────────────────────────────────────────────────

bool ResourceHelper::ensureMultiReleaseManifest(ResourceContainer& container)
{
    static constexpr const char* kMultiReleaseKey   = "Multi-Release";
    static constexpr const char* kMultiReleaseValue = "true";

    bool                       hasVersionedClass = false;
    const ManifestResource*    manifestResource  = nullptr;
    container.visitResources([&](std::string_view uri, const IResource& resource) {
        if (resource.getType() == ResourceType::Class &&
            multiReleaseVersionOfClassUri(uri) >= 9) {
            hasVersionedClass = true;
        }
        if (manifestResource == nullptr) {
            manifestResource =
                ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource);
        }
    });
    if (!hasVersionedClass) return false;

    if (manifestResource == nullptr) {
        // 无 Manifest 资源：新增最小 Manifest（Manifest-Version + Multi-Release），
        // 落在 jar 规范键上。
        mjbase::jar::Manifest manifest;
        manifest.setMainAttribute(ca::str::Utf8StringRef::from_cstr("Manifest-Version"),
                                  ca::str::Utf8StringRef::from_cstr("1.0"));
        manifest.setMainAttribute(ca::str::Utf8StringRef::from_cstr(kMultiReleaseKey),
                                  ca::str::Utf8StringRef::from_cstr(kMultiReleaseValue));
        container.replaceResource(
            ca::str::Utf8StringRef::from_cstr(mjbase::jar::JarUtil::kManifestFileName),
            std::make_unique<ManifestResource>(std::move(manifest)));
        return true;
    }

    // 既有 Manifest：属性已是 "true" 则零改动；否则原位覆写（既有键不改变行
    // 位置）或追加尾部（新键）——行序字节级保序由 Manifest::setMainAttribute
    // 与 toBytes 的插入序序列化保证。
    mjbase::jar::Manifest updated = manifestResource->get();
    const auto            key     = ca::str::Utf8StringRef::from_cstr(kMultiReleaseKey);
    if (updated.getMainAttribute(key).to_std_string() == kMultiReleaseValue) return false;
    updated.setMainAttribute(key, ca::str::Utf8StringRef::from_cstr(kMultiReleaseValue));
    // replaceResource 先把键视图驻留进容器 arena、再替换条目，借用 location
    // 视图跨不过旧资源死亡点也安全。
    container.replaceResource(manifestResource->getLocation(),
                              std::make_unique<ManifestResource>(std::move(updated)));
    return true;
}

}   // namespace mjbase::model
