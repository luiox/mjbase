#include "mjbase/spi/common_super_resolver.hpp"

#include <algorithm>

namespace mjbase::spi {

CommonSuperClassResolver::CommonSuperClassResolver(
    const mjbase::hierarchy::ClassHierarchy& hierarchy, std::string_view fallback)
    : hierarchy_(&hierarchy)
{
    // fallback 文本驻留进查询池（id 从 1 起，恒 valid）。
    fallback_ = mjbase::ClassKey{queryPool_.intern(fallback)};
}

void CommonSuperClassResolver::setFallback(std::string_view fallback)
{
    fallback_ = mjbase::ClassKey{queryPool_.intern(fallback)};
    cache_.clear();   // 回退语义变了，既有缓存全部作废
}

std::string_view CommonSuperClassResolver::getCommonSuperClass(std::string_view type1,
                                                               std::string_view type2)
{
    // 归一化 key：(t1,t2) 与 (t2,t1) 等价，按驻留 id 取 (min,max)。
    const mjbase::ClassKey t1{queryPool_.intern(type1)};
    const mjbase::ClassKey t2{queryPool_.intern(type2)};
    const auto             key = (t1.id < t2.id) ? std::make_pair(t1.id, t2.id)
                                                 : std::make_pair(t2.id, t1.id);
    if (auto it = cache_.find(key); it != cache_.end()) {
        const Cached& c = it->second;
        return c.fromFallback ? queryPool_.str(c.key.id) : hierarchy_->nameOf(c.key);
    }

    const Cached resolved = resolve(type1, type2);
    cache_[key]           = resolved;
    return resolved.fromFallback ? queryPool_.str(resolved.key.id)
                                 : hierarchy_->nameOf(resolved.key);
}

CommonSuperClassResolver::Cached CommonSuperClassResolver::resolve(std::string_view type1,
                                                                   std::string_view type2)
{
    // 不可判定回退第 1 条：任一类型未登记 → fallback。
    const mjbase::ClassKey a = hierarchy_->findClass(type1);
    const mjbase::ClassKey b = hierarchy_->findClass(type2);
    if (!a.valid() || !b.valid()) return Cached{fallback_, true};

    const mjbase::ClassKey common = hierarchy_->commonAncestor(a, b);
    // 不可判定回退第 2 条：两链无交（含悬挂键截断）→ fallback。
    if (!common.valid()) return Cached{fallback_, true};

    // 正常公共祖先是图内键，视图走层次图驻留池（nameOf）。
    return Cached{common, false};
}

}   // namespace mjbase::spi
