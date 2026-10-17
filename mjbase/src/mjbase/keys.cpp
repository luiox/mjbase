/// @file        keys.cpp
/// @brief       跨类身份原语实现。
#include "mjbase/keys.hpp"

namespace mjbase {

KeyPool::KeyPool() noexcept
{
    // 烧掉 0 号槽：空串恒驻留为 0，真实内容从 1 起，id 0 = 未设置。
    slots_.emplace_back();
    byStr_.emplace(std::string_view{}, 0u);
}

ca::u32 KeyPool::intern(std::string_view s)
{
    const auto it = byStr_.find(s);
    if (it != byStr_.end()) {
        return it->second;
    }
    const auto ref = arena_.intern_raw(reinterpret_cast<const ca::u8*>(s.data()),
                                       static_cast<ca::u32>(s.size()));
    const ca::u32 slot = static_cast<ca::u32>(slots_.size());
    slots_.push_back(ref);
    // key 指向 arena 内部（arena 只增不移），视图长期有效。
    byStr_.emplace(std::string_view(reinterpret_cast<const char*>(ref.data()),
                                    ref.byte_length()),
                   slot);
    return slot;
}

std::string_view KeyPool::str(ca::u32 id) const noexcept
{
    if (id == 0 || id >= slots_.size()) {
        return {};
    }
    const auto& r = slots_[id];
    return std::string_view(reinterpret_cast<const char*>(r.data()),
                            static_cast<std::size_t>(r.byte_length()));
}

std::size_t MemberKeyHash::operator()(const MemberKey& k) const noexcept
{
    // 三段 u32 混合（FNV-1a over bytes），足够打散且与顺序无关替代方案简单。
    std::size_t h = 1469598103934665603ull;
    const auto mix = [&h](ca::u32 v) {
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<std::size_t>((v >> (i * 8)) & 0xFF);
            h *= 1099511628211ull;
        }
    };
    mix(k.cls);
    mix(k.name);
    mix(k.desc);
    return h;
}

ClassKey classKeyOf(KeyPool& pool, std::string_view internalName)
{
    return ClassKey{pool.intern(internalName)};
}

MemberKey memberKeyOf(KeyPool& pool,
                      ClassKey owner,
                      std::string_view name,
                      std::string_view desc)
{
    return MemberKey{owner.id, pool.intern(name), pool.intern(desc)};
}

void formatMember(const KeyPool& pool, MemberKey key, std::string& out)
{
    out.append(pool.str(key.cls));
    out.push_back('.');
    out.append(pool.str(key.name));
    out.push_back(' ');
    out.append(pool.str(key.desc));
}

}   // namespace mjbase
