/// @file        class_source.cpp
/// @brief       E3 类源实现：内存 map 版与 ResourceContainer 适配版。
#include "mjbase/consumer/class_source.hpp"

#include <algorithm>

#include "jbc/core/error.hpp"
#include "jbc/core/reader.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjbase::consumer {

const char* to_string(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::BadClassFile: return "BadClassFile";
    }
    return "?";
}

namespace {

/// @brief 名字字典序比较（经驻留池展开；稳定排序的键函数）。
struct NameOrderLess
{
    const KeyPool* pool;
    bool operator()(ca::u32 lhs, ca::u32 rhs) const noexcept
    {
        return pool->str(lhs) < pool->str(rhs);
    }
};

/// @brief 纯扫描读选项：不保留源字节副本（读侧只出骨架事实）。
jbc::core::ReadOptions scanReadOptions()
{
    jbc::core::ReadOptions options;
    options.preserveSourceBytes = false;
    return options;
}

}   // namespace

// ── MemoryClassSource ────────────────────────────────────────────────────────

void MemoryClassSource::add(std::string_view internalName, std::vector<ca::u8> bytes)
{
    const ca::u32 id   = pool_.intern(internalName);
    classes_[id]       = std::move(bytes);
    const auto it      = std::lower_bound(sortedIds_.begin(), sortedIds_.end(), id,
                                          NameOrderLess{&pool_});
    if (it != sortedIds_.end() && *it == id) return;
    sortedIds_.insert(it, id);
}

ConsumerResult<jbc::core::ByteView> MemoryClassSource::bytes(std::string_view internalName) const
{
    const ca::u32 id = pool_.intern(internalName);
    const auto    it = classes_.find(id);
    if (it == classes_.end()) {
        return ca::core::Err(ConsumerError{
            ErrorCode::NotFound, "class source: not found: " + std::string(internalName)});
    }
    return ca::core::Ok(jbc::core::ByteView{it->second});
}

void MemoryClassSource::listClasses(std::vector<std::string_view>& out) const
{
    out.reserve(out.size() + sortedIds_.size());
    for (const ca::u32 id : sortedIds_) out.push_back(pool_.str(id));
}

// ── ContainerClassSource ─────────────────────────────────────────────────────

ConsumerResult<ContainerClassSource> ContainerClassSource::make(
    const model::ResourceContainer& container)
{
    ContainerClassSource source;
    ConsumerError        failure{};
    bool                 failed = false;
    container.visitResources([&](const std::string_view uri, const model::IResource& resource) {
        if (failed) return;
        const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (cls == nullptr) return;   // 非 Class 资源忽略

        auto read = jbc::core::readClass(jbc::core::ByteView{cls->get()}, scanReadOptions());
        if (read.is_err()) {
            failed          = true;
            failure.code    = ErrorCode::BadClassFile;
            failure.message = "class source: read " + std::string{uri} + ": " +
                              jbc::core::to_string(std::move(read).unwrap_err());
            return;
        }
        const std::string_view name = std::move(read).unwrap().file->thisName();

        const ca::u32 id = source.pool_.intern(name);
        if (!source.resources_.emplace(id, cls).second) {
            // first-wins 保留首条（JVM 类加载语义）；重复记录供调用方告警。
            source.duplicates_.push_back(source.pool_.str(id));
            return;
        }
        const auto pos = std::lower_bound(source.sortedIds_.begin(), source.sortedIds_.end(), id,
                                          NameOrderLess{&source.pool_});
        source.sortedIds_.insert(pos, id);
    });
    if (failed) return ca::core::Err(std::move(failure));

    // 重复项按名字序报告（与枚举序一致，跨容器确定性）。
    std::sort(source.duplicates_.begin(), source.duplicates_.end());
    return ca::core::Ok(std::move(source));
}

ConsumerResult<jbc::core::ByteView> ContainerClassSource::bytes(std::string_view internalName) const
{
    const ca::u32 id = pool_.intern(internalName);
    const auto    it = resources_.find(id);
    if (it == resources_.end()) {
        return ca::core::Err(ConsumerError{
            ErrorCode::NotFound, "class source: not found: " + std::string(internalName)});
    }
    return ca::core::Ok(jbc::core::ByteView{it->second->get()});
}

void ContainerClassSource::listClasses(std::vector<std::string_view>& out) const
{
    out.reserve(out.size() + sortedIds_.size());
    for (const ca::u32 id : sortedIds_) out.push_back(pool_.str(id));
}

}   // namespace mjbase::consumer
