/// @file        resource_container.cpp
/// @brief       资源容器默认实现。
#include "resource_container.hpp"

#include <string>

#include "resource_io.hpp"

#include "libca/log/log_macros.hpp"

namespace mjbase::model {

// 委托给底层 unordered_map 的常见操作
void ResourceContainer::addResource(std::string_view uri, std::unique_ptr<IResource> resource)
{
    auto it = resources_.find(uri);
    // 重复条目：按 JVM 类加载语义 first-wins 保留首条目（issue #962）。含重复
    // 条目的 jar 常为恶意构造——藏在后条目里的实现会被 JVM 忽略，last-wins 导入
    // 却让工具分析后条目，造成「工具分析的对象」与「实际加载的对象」错配。
    // 记录 + 日志告警（宿主入口未注册日志 target 时门面静默丢弃），导入方可
    // 用 duplicateResourceUris() 上报；有意替换（候选重建、事务预览、undo/redo
    // 回滚）请走 replaceResource。
    if (it != resources_.end()) {
        duplicateResourceUris_.push_back(it->first);
        CA_LOG_WARN("[mjbase] warning: duplicate jar entry '{}' (kept first occurrence, aligned with JVM first-wins class loading)",
                    std::string(uri).c_str());
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

}   // namespace mjbase::model
