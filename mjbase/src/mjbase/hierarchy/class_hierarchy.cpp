/// @file        class_hierarchy.cpp
/// @brief       类层次内存图实现：登记环检查 + super 链/接口闭包查询。
#include "class_hierarchy.hpp"

#include <unordered_set>
#include <utility>

#include "libca/core/datatype.hpp"

namespace mjbase::hierarchy {

using ca::core::Err;
using ca::core::Ok;

namespace {

constexpr const char* kObjectName = "java/lang/Object";

/// 未知类的 interfaces() 共享空表（返回引用的哨兵）。
const std::vector<ClassKey>& emptyKeys() noexcept
{
    static const std::vector<ClassKey> kEmpty;
    return kEmpty;
}

}   // namespace

const char* to_string(HierarchyError e) noexcept
{
    switch (e) {
    case HierarchyError::BadKey: return "bad key";
    case HierarchyError::DuplicateClass: return "duplicate class";
    case HierarchyError::Cycle: return "cycle";
    }
    return "?";
}

// ── 构建 ──

Result<void, HierarchyError> ClassHierarchy::addClass(const ClassSummary& s)
{
    if (!s.self.valid() || pool_->str(s.self.id).empty()) {
        return Err(HierarchyError::BadKey);
    }
    if (classes_.count(s.self.id) != 0) {
        return Err(HierarchyError::DuplicateClass);
    }

    // 父类边：自指，或本类已在（未来的）父类链上 → 环。
    // superChainOf 走悬挂键（未登记但有名字），因此「先 A(super=B悬挂)，
    // 后 B(super=A)」这类乱序闭合也能在此刻检出。
    if (s.super.valid()) {
        if (s.super == s.self) {
            return Err(HierarchyError::Cycle);
        }
        if (chainContains(superChainOf(s.super), s.self)) {
            return Err(HierarchyError::Cycle);
        }
    }

    // 接口边：自指，或本类已在接口目标的闭包中 → 环。
    for (const ClassKey iface : s.interfaces) {
        if (!iface.valid()) continue;
        if (iface == s.self) {
            return Err(HierarchyError::Cycle);
        }
        const ClassSummary* target = summary(iface);
        if (target != nullptr && chainContains(interfaceClosure(iface), s.self)) {
            return Err(HierarchyError::Cycle);
        }
    }

    byName_.emplace(pool_->str(s.self.id), s.self);
    order_.push_back(s.self);
    classes_.emplace(s.self.id, s);
    return Ok();
}

Result<void, HierarchyError> ClassHierarchy::validate() const
{
    for (const ClassKey key : order_) {
        const ClassSummary* s = summary(key);
        if (s == nullptr) continue;
        if (s->super.valid()
            && (s->super == s->self || chainContains(superChainOf(s->super), s->self))) {
            return Err(HierarchyError::Cycle);
        }
        for (const ClassKey iface : s->interfaces) {
            if (!iface.valid()) continue;
            if (iface == s->self
                || (summary(iface) != nullptr
                    && chainContains(interfaceClosure(iface), s->self))) {
                return Err(HierarchyError::Cycle);
            }
        }
    }
    return Ok();
}

// ── 基本查询 ──

bool ClassHierarchy::contains(ClassKey cls) const noexcept
{
    return cls.valid() && classes_.count(cls.id) != 0;
}

ClassKey ClassHierarchy::findClass(std::string_view internalName) const noexcept
{
    const auto it = byName_.find(internalName);
    return it != byName_.end() ? it->second : ClassKey{};
}

ClassKey ClassHierarchy::objectClass() const noexcept
{
    return findClass(kObjectName);
}

std::string_view ClassHierarchy::nameOf(ClassKey cls) const noexcept
{
    return pool_->str(cls.id);
}

const ClassSummary* ClassHierarchy::summary(ClassKey cls) const noexcept
{
    const auto it = classes_.find(cls.id);
    return it != classes_.end() ? &it->second : nullptr;
}

// ── 边查询 ──

ClassKey ClassHierarchy::superClass(ClassKey cls) const noexcept
{
    const ClassSummary* s = summary(cls);
    return s != nullptr ? s->super : ClassKey{};
}

const std::vector<ClassKey>& ClassHierarchy::interfaces(ClassKey cls) const noexcept
{
    const ClassSummary* s = summary(cls);
    return s != nullptr ? s->interfaces : emptyKeys();
}

std::vector<ClassKey> ClassHierarchy::superChainOf(ClassKey start) const
{
    std::vector<ClassKey> chain;
    if (!start.valid()) return chain;
    std::unordered_set<ca::u32> seen;
    ClassKey cur = start;
    while (cur.valid() && seen.insert(cur.id).second) {
        chain.push_back(cur);
        const ClassSummary* s = summary(cur);
        if (s == nullptr || !s->super.valid()) break;   // 悬挂键/无父类：截断
        cur = s->super;
    }
    return chain;
}

bool ClassHierarchy::chainContains(const std::vector<ClassKey>& chain,
                                   ClassKey key) noexcept
{
    for (const ClassKey c : chain) {
        if (c == key) return true;
    }
    return false;
}

std::vector<ClassKey> ClassHierarchy::ancestors(ClassKey cls) const
{
    if (!contains(cls)) return {};
    return superChainOf(cls);
}

std::vector<ClassKey> ClassHierarchy::descendants(ClassKey cls) const
{
    if (!contains(cls)) return {};
    std::vector<ClassKey> out;
    for (const ClassKey key : order_) {
        if (key == cls) continue;
        if (chainContains(superChainOf(key), cls)) out.push_back(key);
    }
    return out;
}

bool ClassHierarchy::isSubclass(ClassKey sub, ClassKey super) const
{
    if (!super.valid() || !contains(sub)) return false;
    return chainContains(superChainOf(sub), super);
}

bool ClassHierarchy::isSubtype(ClassKey sub, ClassKey super) const
{
    if (isSubclass(sub, super)) return true;
    if (!super.valid() || !contains(sub)) return false;
    return chainContains(interfaceClosure(sub), super);
}

ClassKey ClassHierarchy::commonAncestor(ClassKey a, ClassKey b) const
{
    if (!contains(a) || !contains(b)) return ClassKey{};
    const std::vector<ClassKey> chainA = superChainOf(a);
    const std::vector<ClassKey> chainB = superChainOf(b);
    // b 链自近及远扫，命中 a 链成员集合的第一个即最近公共祖先。
    for (const ClassKey cb : chainB) {
        if (chainContains(chainA, cb)) return cb;
    }
    return ClassKey{};
}

std::vector<ClassKey> ClassHierarchy::interfaceClosure(ClassKey cls) const
{
    std::vector<ClassKey> out;
    if (!contains(cls)) return out;

    std::vector<ClassKey> queue;
    for (const ClassKey c : superChainOf(cls)) {
        const ClassSummary* s = summary(c);
        if (s == nullptr) continue;
        for (const ClassKey i : s->interfaces) {
            if (i.valid()) queue.push_back(i);
        }
    }
    std::unordered_set<ca::u32> seen;
    for (ca::usize qi = 0; qi < queue.size(); ++qi) {
        const ClassKey i = queue[qi];
        if (!seen.insert(i.id).second) continue;
        out.push_back(i);
        const ClassSummary* s = summary(i);
        if (s == nullptr) continue;   // 悬挂接口：计入后不展开
        for (const ClassKey j : s->interfaces) {
            if (j.valid()) queue.push_back(j);
        }
    }
    return out;
}

bool ClassHierarchy::isInterface(ClassKey cls) const noexcept
{
    const ClassSummary* s = summary(cls);
    return s != nullptr && (s->access & kAccInterface) != 0;
}

bool ClassHierarchy::isEnum(ClassKey cls) const noexcept
{
    const ClassSummary* s = summary(cls);
    return s != nullptr && (s->access & kAccEnum) != 0;
}

}   // namespace mjbase::hierarchy
