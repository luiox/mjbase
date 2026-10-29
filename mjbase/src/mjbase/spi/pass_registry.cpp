#include "mjbase/spi/pass_registry.hpp"

#include <algorithm>
#include <utility>

namespace mjbase::spi {

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

PassRegistry& PassRegistry::registerPipeline(const std::string& name,
                                             const std::string& description,
                                             PipelineFactory    factory, PassStability stability)
{
    Entry entry;
    entry.meta   = PassPipelineInfo{name, description, stability};
    entry.factory = std::move(factory);
    entries_[name] = std::move(entry);
    return *this;
}

bool PassRegistry::hasPipeline(const std::string& name) const
{
    return entries_.find(name) != entries_.end();
}

PipelineBuilder PassRegistry::getPipeline(const std::string& name) const
{
    const auto it = entries_.find(name);
    if (it == entries_.end()) return PipelineBuilder{};
    return it->second.factory();
}

const PassPipelineInfo* PassRegistry::pipelineInfo(const std::string& name) const
{
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second.meta;
}

std::vector<PassPipelineInfo> PassRegistry::listPipelines() const
{
    std::vector<PassPipelineInfo> out;
    out.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) out.push_back(entry.meta);
    std::sort(out.begin(), out.end(),
              [](const PassPipelineInfo& lhs, const PassPipelineInfo& rhs) {
                  return lhs.name < rhs.name;
              });
    return out;
}

}   // namespace mjbase::spi
