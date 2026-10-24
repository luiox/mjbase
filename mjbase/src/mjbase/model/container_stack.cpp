/// @file        container_stack.cpp
/// @brief       角色化容器栈实现。
#include "container_stack.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

namespace mjbase::model {

using ca::str::Utf8String;
using ca::str::Utf8StringRef;

std::string_view to_string(ContainerRole role) noexcept
{
    switch (role) {
    case ContainerRole::Primary: return "primary";
    case ContainerRole::Library: return "library";
    case ContainerRole::Runtime: return "runtime";
    }
    return "primary";
}

namespace {

std::string toStd(const Utf8StringRef& value)
{
    return value.to_std_string();
}

Utf8String copyOf(std::string_view value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

/// 解析序秩：primary < library < runtime（数值小者先解析）。
int roleRank(ContainerRole role)
{
    switch (role) {
    case ContainerRole::Primary: return 0;
    case ContainerRole::Library: return 1;
    case ContainerRole::Runtime: return 2;
    }
    return 3;
}

/// 借用位置视图 → std::string（与 workspace locationText 同口径）。
std::string locationOf(const IResource& resource)
{
    const auto location = resource.getLocation();
    return std::string(reinterpret_cast<const char*>(location.data()), location.byte_length());
}

/// 资源克隆（值语义拷贝；未知资源类型返回空——mergedView 的全量交付门）。
std::unique_ptr<IResource> cloneResourceOf(const IResource& resource)
{
    if (const auto* classResource =
            ca::core::tag_cast::dyn_cast<const ClassResource>(&resource)) {
        return std::make_unique<ClassResource>(locationOf(resource), classResource->get());
    }
    if (const auto* manifestResource =
            ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource)) {
        return std::make_unique<ManifestResource>(manifestResource->get());
    }
    if (const auto* unknownResource =
            ca::core::tag_cast::dyn_cast<const UnknownResource>(&resource)) {
        return std::make_unique<UnknownResource>(locationOf(resource), unknownResource->get());
    }
    return nullptr;
}

/// 解析序下标序列：按（角色秩，挂载序）稳定排序。
std::vector<ca::usize> resolutionOrder(const std::vector<MountedContainer>& containers)
{
    std::vector<ca::usize> order;
    order.reserve(containers.size());
    for (ca::usize index = 0; index < containers.size(); ++index) order.push_back(index);
    std::stable_sort(order.begin(), order.end(),
                     [&](ca::usize a, ca::usize b) {
                         return roleRank(containers[a].role) < roleRank(containers[b].role);
                     });
    return order;
}

}   // namespace

ModelResult<void> MountedContainerSet::mount(std::string_view id, ContainerRole role,
                                             IResourceContainer* container)
{
    if (id.empty() || container == nullptr) return ca::Err(ModelError::BadArgument);
    const Utf8StringRef idRef = Utf8StringRef::from_string_view(id);
    for (const auto& existing : containers_) {
        if (existing.id.ref().equals(idRef)) return ca::Err(ModelError::BadArgument);
        // 一容器一记录：同一容器对象不得以不同 id/角色重复注册——双注册
        // 是 project_info 双条目与 detach 半边泄漏的模型层根源。
        if (existing.container == container) return ca::Err(ModelError::BadArgument);
    }
    MountedContainer mounted;
    mounted.id        = copyOf(id);
    mounted.role      = role;
    mounted.container = container;
    containers_.push_back(std::move(mounted));
    rebuildShadowedEntries();
    return ca::Ok();
}

bool MountedContainerSet::unmount(std::string_view id)
{
    for (auto it = containers_.begin(); it != containers_.end(); ++it) {
        if (it->id.ref().equals(Utf8StringRef::from_string_view(id))) {
            containers_.erase(it);
            rebuildShadowedEntries();
            return true;
        }
    }
    return false;
}

const MountedContainer* MountedContainerSet::find(std::string_view id) const
{
    for (const auto& mounted : containers_) {
        if (mounted.id.ref().equals(Utf8StringRef::from_string_view(id))) return &mounted;
    }
    return nullptr;
}

IResource* MountedContainerSet::resolve(std::string_view uri) const
{
    for (const ca::usize index : resolutionOrder(containers_)) {
        IResource* resource = containers_[index].container->getResource(uri);
        if (resource != nullptr) return resource;
    }
    return nullptr;
}

void MountedContainerSet::visitRole(
    ContainerRole role, const std::function<void(std::string_view, const IResource&)>& visitor) const
{
    for (const auto& mounted : containers_) {
        if (mounted.role != role) continue;
        mounted.container->visitResources(visitor);
    }
}

ModelResult<std::unique_ptr<ResourceContainer>> MountedContainerSet::mergedView() const
{
    auto merged = std::make_unique<ResourceContainer>();
    for (const ca::usize index : resolutionOrder(containers_)) {
        const IResourceContainer* source = containers_[index].container;
        ModelResult<void>         failure = ca::Ok();
        source->visitResources([&](std::string_view uri, const IResource& resource) {
            if (failure.is_err()) return;
            auto copied = cloneResourceOf(resource);
            if (!copied) {
                failure = ca::Err(ModelError::BadArgument);
                return;
            }
            // first-wins + 重复记录由容器自身承载（resource_container.hpp 现状
            // 语义）；跨容器遮蔽在栈侧 shadowedEntries 另有容器 id 记录。
            merged->addResource(uri, std::move(copied));
        });
        if (failure.is_err()) return ca::Err(ModelError::BadArgument);
    }
    return ca::Ok(std::move(merged));
}

ca::str::Utf8String MountedContainerSet::mountAddress(std::string_view containerId,
                                                      std::string_view entryUri)
{
    std::string address;
    address.append(containerId);
    address.append(kAddressSeparator);
    address.append(entryUri);
    return copyOf(address);
}

ModelResult<MountedContainerSet::ResolvedAddress> MountedContainerSet::resolveAddress(
    std::string_view address) const
{
    constexpr std::string_view separator = kAddressSeparator;
    const std::size_t          split     = address.find(separator);
    if (split == std::string_view::npos || split == 0 ||
        split + separator.size() >= address.size()) {
        return ca::Err(ModelError::BadArgument);
    }
    const auto containerId = address.substr(0, split);
    auto       entryUri    = address.substr(split + separator.size());
    // 复合寻址扩展位（RFC §5.3）：entryUri 残留 `!/` = 嵌套容器钻井（WAR 内
    // JAR / fat jar 嵌套 / JMOD），本期只落接口不实现解包；嵌套成员作为派生
    // 容器缓存于 sections/ 的方案见 doc_ai/analysis-cache-and-container-stack.md。
    // TODO(composite-addressing): 按 bang 链逐段下钻（每段解析为子容器，末端
    // 段为条目），multi-release 条目按 JVM 规则归并版本桶。
    if (entryUri.find(separator) != std::string_view::npos) {
        return ca::Err(ModelError::BadArgument);
    }
    const MountedContainer* mounted = find(containerId);
    if (mounted == nullptr) return ca::Err(ModelError::NotFound);
    // 地址钉死容器：条目必须在该容器内命中（不做跨栈 first-wins；未命中 =
    // NotFound）。
    if (!mounted->container->hasResource(entryUri)) return ca::Err(ModelError::NotFound);
    ResolvedAddress resolved;
    resolved.container = mounted;
    resolved.entryUri  = copyOf(entryUri);
    return ca::Ok(std::move(resolved));
}

void MountedContainerSet::rebuildShadowedEntries()
{
    shadowed_.clear();
    std::map<std::string, std::string> winnerOf;   ///< uri → 解析序首位容器 id
    for (const ca::usize index : resolutionOrder(containers_)) {
        const MountedContainer& mounted = containers_[index];
        const std::string       id      = toStd(mounted.id.ref());
        mounted.container->visitResources([&](std::string_view uri, const IResource&) {
            const std::string key(uri);
            auto              seen = winnerOf.find(key);
            if (seen == winnerOf.end()) {
                winnerOf.emplace(key, id);
                return;
            }
            ShadowedEntryRecord record;
            record.uri        = copyOf(uri);
            record.winnerId   = copyOf(seen->second);
            record.shadowedId = copyOf(id);
            shadowed_.push_back(std::move(record));
        });
    }
    // (uri, 发现序) 稳定排列：同 uri 的输家按解析序（发现序）相邻且有序。
    std::stable_sort(shadowed_.begin(), shadowed_.end(),
                     [](const ShadowedEntryRecord& a, const ShadowedEntryRecord& b) {
                         return toStd(a.uri.ref()) < toStd(b.uri.ref());
                     });
}

}   // namespace mjbase::model
