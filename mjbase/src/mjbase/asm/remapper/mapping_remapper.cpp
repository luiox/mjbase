#include "mjbase/asm/remapper/mapping_remapper.hpp"

namespace mjbase::asm_remapper {
namespace {

/// @brief 「命中才改写」判定：映射查询空视图 = 未命中。
inline bool mappedNonEmpty(std::string_view mapped) noexcept { return !mapped.empty(); }

template <typename MapName>
std::string rewriteFieldDescriptorText(std::string_view desc, MapName&& mapName)
{
    std::string out;
    out.reserve(desc.size());
    ca::usize i = 0;
    while (i < desc.size() && desc[i] == '[') {
        out.push_back('[');
        i++;
    }
    if (i >= desc.size()) return std::string(desc);
    if (desc[i] != 'L') {
        out.push_back(desc[i]);
        return out;
    }
    const ca::usize start = ++i;
    while (i < desc.size() && desc[i] != ';') i++;
    if (i >= desc.size()) return std::string(desc);
    const std::string_view oldName = desc.substr(start, i - start);
    const std::string_view mapped  = mapName(oldName);
    if (mappedNonEmpty(mapped)) {
        out.push_back('L');
        out.append(mapped);
        out.push_back(';');
    }
    else {
        out.push_back('L');
        out.append(oldName);
        out.push_back(';');
    }
    return out;
}

template <typename MapName>
std::string rewriteMethodDescriptorText(std::string_view desc, MapName&& mapName)
{
    if (desc.empty() || desc.front() != '(') return std::string(desc);
    const ca::usize close = desc.find(')');
    if (close == std::string_view::npos) return std::string(desc);
    std::string out;
    out.reserve(desc.size());
    out.push_back('(');
    ca::usize i = 1;
    while (i < close) {
        const ca::usize paramStart = i;
        ca::usize       j          = i;
        while (j < close && desc[j] == '[') j++;
        if (j >= close) return std::string(desc);
        if (desc[j] == 'L') {
            while (j < close && desc[j] != ';') j++;
            if (j >= close) return std::string(desc);
            j++;
        }
        else {
            j++;
        }
        out.append(rewriteFieldDescriptorText(desc.substr(paramStart, j - paramStart), mapName));
        i = j;
    }
    out.push_back(')');
    out.append(rewriteFieldDescriptorText(desc.substr(close + 1), mapName));
    return out;
}

}   // namespace

std::string_view MappingRemapper::remapTextCached(std::string_view input, bool method,
                                                  bool signature) const
{
    if (input.empty() || !hasMappings()) return input;
    const ca::u32 key = pool_.intern(input);
    const auto    hit = cache_.find(key);
    if (hit != cache_.end()) return pool_.str(hit->second);

    const auto mapper = [this](std::string_view name) { return map(name); };
    std::string rewritten;
    bool changed = false;
    if (signature) {
        rewritten.reserve(input.size());
        for (ca::usize i = 0; i < input.size(); ++i) {
            const char c = input[i];
            rewritten.push_back(c);
            if (c != 'L') continue;
            const ca::usize start = i + 1;
            ca::usize       end   = start;
            while (end < input.size() && input[end] != ';' && input[end] != '<') end++;
            if (end <= start) continue;
            const std::string_view mapped = map(input.substr(start, end - start));
            if (mappedNonEmpty(mapped)) {
                rewritten.append(mapped);
                changed = true;
            }
            else {
                rewritten.append(input.substr(start, end - start));
            }
            i = end - 1;
        }
    }
    else if (method) {
        rewritten = rewriteMethodDescriptorText(input, mapper);
        changed   = rewritten != input;
    }
    else {
        rewritten = rewriteFieldDescriptorText(input, mapper);
        changed   = rewritten != input;
    }
    const ca::u32 result = changed ? pool_.intern(rewritten) : key;
    cache_.emplace(key, result);
    return pool_.str(result);
}

std::string_view MappingRemapper::mapFieldDescriptor(std::string_view desc) const
{
    return remapTextCached(desc, false, false);
}

std::string_view MappingRemapper::mapMethodDescriptor(std::string_view desc) const
{
    return remapTextCached(desc, true, false);
}

std::string_view MappingRemapper::mapSignature(std::string_view signature) const
{
    return remapTextCached(signature, false, true);
}

}   // namespace mjbase::asm_remapper
