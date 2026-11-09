#include "mjbase/spi/pass_registry.hpp"

#include <algorithm>
#include <utility>

namespace mjbase::spi {

namespace {

/// 查询/注册名视图 → 临时驻留 key（key 为 Utf8String，无异构查找；空视图安全）。
/// 名字来源是内部注册字面量，按字节收进，不做错误通道回传。
ca::str::Utf8String utf8Key(std::string_view name)
{
    if (name.empty()) return ca::str::Utf8String{};
    return ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(name.data()),
                                          name.size());
}

}   // namespace

const char* to_string(PassStability stability) noexcept
{
    switch (stability) {
    case PassStability::Default: return "Default";
    case PassStability::OptIn: return "OptIn";
    case PassStability::Experimental: return "Experimental";
    case PassStability::Stub: return "Stub";
    }
    return "?";
}

// ── PassRegistry ─────────────────────────────────────────────

PassRegistry& PassRegistry::instance()
{
    static PassRegistry registry;
    return registry;
}

PassRegistry& PassRegistry::registerPipeline(std::string_view name, std::string_view description,
                                             PipelineFactory    factory, PassStability stability)
{
    Entry entry;
    entry.meta   = PassPipelineInfo{utf8Key(name), utf8Key(description), stability};
    entry.factory = std::move(factory);
    entries_[utf8Key(name)] = std::move(entry);
    return *this;
}

bool PassRegistry::hasPipeline(std::string_view name) const
{
    return entries_.find(utf8Key(name)) != entries_.end();
}

PipelineBuilder PassRegistry::getPipeline(std::string_view name) const
{
    const auto it = entries_.find(utf8Key(name));
    if (it == entries_.end()) return PipelineBuilder{};
    return it->second.factory();
}

const PassPipelineInfo* PassRegistry::pipelineInfo(std::string_view name) const
{
    const auto it = entries_.find(utf8Key(name));
    return it == entries_.end() ? nullptr : &it->second.meta;
}

std::vector<PassPipelineInfo> PassRegistry::listPipelines() const
{
    std::vector<PassPipelineInfo> out;
    out.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) out.push_back(entry.meta.clone());
    std::sort(out.begin(), out.end(),
              [](const PassPipelineInfo& lhs, const PassPipelineInfo& rhs) {
                  return lhs.name.compare(rhs.name) < 0;
              });
    return out;
}

}   // namespace mjbase::spi
