/// @file        resource_container.cpp
/// @brief       资源容器默认实现 + Multi-Release 族视图。
#include "resource_container.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "resource_io.hpp"

#include "libca/log/log_macros.hpp"

namespace mjbase::model {

// 委托给底层 unordered_map 的常见操作
void ResourceContainer::addResource(std::string_view uri, std::unique_ptr<IResource> resource)
{
    auto it = resources_.find(uri);
    // 重复条目：按 JVM 类加载实测口径 last-wins 保留末条目。mini jar 对照实验
    // 钉死（JDK 17.0.8）：同名条目垃圾在前读真实类、垃圾在后读垃圾，两次都是
    // CEN 末现生效——推翻 issue #962 时的 first-wins 误判。实证罐（SSTO-Core，
    // 每个被 Dup 类名挂「垃圾 → 垃圾 → 真实类」三条同面条目）在 first-wins 下
    // 整条丢失真实类；末现保留让「工具分析的对象」与「JVM 实际加载的对象」
    // 一致。记录 + 日志告警（宿主入口未注册日志 target 时门面静默丢弃），导入
    // 方可用 duplicateResourceUris() 上报；有意静默替换（候选重建、事务预览、
    // undo/redo 回滚）请走 replaceResource。
    if (it != resources_.end()) {
        duplicateResourceUris_.push_back(it->first);
        CA_LOG_WARN("[mjbase] warning: duplicate jar entry '{}' (kept last occurrence, aligned with JVM classloading semantics)",
                    std::string(uri).c_str());
        it->second = std::move(resource);
        return;
    }
    resources_.emplace(internUri(uri), std::move(resource));
}

bool ResourceContainer::hasResource(std::string_view uri) const
{
    return resources_.find(uri) != resources_.end();
}

IResource* ResourceContainer::getResource(std::string_view uri) const
{
    auto it = resources_.find(uri);
    return it != resources_.end() ? it->second.get() : nullptr;
}

void ResourceContainer::visitResources(
    const std::function<void(std::string_view, const IResource&)>& visitor) const
{
    for (const auto& [uri, res] : resources_) visitor(uri, *res);
}

// 薄委托：导入/导出实际由 IResourceImporter / IResourceExporter 实现
ModelResult<void> ResourceContainer::importResource(IResourceImporter* importer)
{
    beginImport();
    return importer->importResource(*this);
}

ModelResult<void> ResourceContainer::exportResource(IResourceExporter* exporter)
{
    return exporter->exportResource(*this);
}

// URI 驻留：arena 只增不移、内容去重，返回的视图随容器存活（keys 同款用法）。
std::string_view ResourceContainer::internUri(std::string_view uri)
{
    const auto ref = arena_.intern_raw(reinterpret_cast<const ca::u8*>(uri.data()), uri.size());
    return std::string_view(reinterpret_cast<const char*>(ref.data()), ref.byte_length());
}

// ── Multi-Release 族视图 ─────────────────────────────────────────────────────

ca::i64 multiReleaseVersionOfClassUri(std::string_view uri)
{
    constexpr std::string_view kPrefix = "META-INF/versions/";
    if (uri.size() <= kPrefix.size() || uri.substr(0, kPrefix.size()) != kPrefix) return -1;
    const auto versionEnd = uri.find('/', kPrefix.size());
    // 版本段后必须还有内容（"META-INF/versions/<N>/" 至少）。
    if (versionEnd == std::string_view::npos || versionEnd + 1 >= uri.size()) return -1;
    const std::string_view version = uri.substr(kPrefix.size(), versionEnd - kPrefix.size());
    if (version.empty()) return -1;
    ca::i64 value = 0;
    for (const char ch : version) {
        if (ch < '0' || ch > '9') return -1;
        if (value > (std::numeric_limits<ca::i64>::max() - (ch - '0')) / 10) return -1;
        value = value * 10 + (ch - '0');
    }
    return value;
}

std::string internalNameFromClassUri(std::string_view uri)
{
    constexpr std::string_view kPrefix    = "META-INF/versions/";
    constexpr std::string_view kExtension = ".class";
    std::string_view           path       = uri;
    // 版本化前缀下的条目永远不会是 base 条目：JEP 238 口径（≥ 9 的纯数字版本
    // 段）剥前缀取内名；其余形状（N < 9、非数字段）归非法（空内名），不落回
    // base 形推导。
    if (uri.size() > kPrefix.size() && uri.compare(0, kPrefix.size(), kPrefix) == 0) {
        if (multiReleaseVersionOfClassUri(uri) < 9) return {};
        path = uri.substr(uri.find('/', kPrefix.size()) + 1);
    }
    if (path.size() <= kExtension.size() ||
        path.substr(path.size() - kExtension.size()) != kExtension) {
        return {};
    }
    return std::string(path.substr(0, path.size() - kExtension.size()));
}

std::unordered_map<ca::str::Utf8String, ClassFamilyView> buildClassFamilyIndex(
    const IResourceContainer& container)
{
    std::unordered_map<ca::str::Utf8String, ClassFamilyView> index;
    container.visitResources([&](std::string_view uri, const IResource& resource) {
        if (resource.getType() != ResourceType::Class) return;
        const std::string name = internalNameFromClassUri(uri);
        if (name.empty()) return;
        // 内部名来源为 URI 推导（ASCII 域），字节保真收进作索引键。
        ClassFamilyView& family = index[ca::str::Utf8String::from_data_unchecked(
            reinterpret_cast<const ca::u8*>(name.data()), name.size())];
        const ca::i64    version = multiReleaseVersionOfClassUri(uri);
        if (version < 9) {
            family.baseUri = uri;
        }
        else {
            family.variants.push_back({version, uri});
        }
    });
    for (auto& [name, family] : index) {
        std::sort(family.variants.begin(), family.variants.end(),
                  [](const ClassFamilyView::Variant& lhs, const ClassFamilyView::Variant& rhs) {
                      if (lhs.version != rhs.version) return lhs.version < rhs.version;
                      return lhs.uri < rhs.uri;
                  });
        // base 缺失而仅存版本化变体的族：并存状态合法（JVM 忽略该树，base 兜
        // 底缺位而已），本层只告警不改写 URI 布局——「最小 N 变体升格 base」
        // 属输出布局重构，须另批裁决。
        if (family.baseUri.empty()) {
            CA_LOG_WARN("[mjbase] warning: multi-release family '{}' has versioned variants but no base entry (kept as-is)",
                        name.c_str());
        }
    }
    return index;
}

}   // namespace mjbase::model
