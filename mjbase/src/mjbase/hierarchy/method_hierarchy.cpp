/// @file        method_hierarchy.cpp
/// @brief       方法层次实现：声明索引、override 等价族（并查集）、虚分派。
#include "method_hierarchy.hpp"

#include <utility>

namespace mjbase::hierarchy {

namespace {

/// 并查集查找（路径减半）；parent 与 decls_ 等长，-1 表示自根。
ca::u32 ufFind(std::vector<ca::u32>& parent, ca::u32 x) noexcept
{
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x         = parent[x];
    }
    return x;
}

void ufUnite(std::vector<ca::u32>& parent, ca::u32 a, ca::u32 b) noexcept
{
    const ca::u32 ra = ufFind(parent, a);
    const ca::u32 rb = ufFind(parent, b);
    if (ra != rb) parent[ra] = rb;
}

}   // namespace

void MethodHierarchy::build(const ClassHierarchy& ch)
{
    ch_   = &ch;
    pool_ = &ch.pool();
    decls_.clear();
    comps_.clear();
    table_.clear();

    // Phase 1：登记全部方法（登记序 × 类内声明序，族输出的确定性来源）。
    for (const ClassKey key : ch.registrationOrder()) {
        const ClassSummary* s = ch.summary(key);
        if (s == nullptr) continue;
        auto& perClass = table_[key.id];
        for (ca::usize i = 0; i < s->methods.size(); ++i) {
            const MemberSummary& m = s->methods[i];
            Decl d;
            d.key    = MemberKey{key.id, m.name, m.desc};
            d.access = m.access;
            d.index  = static_cast<ca::u32>(i);
            perClass[packSig(m.name, m.desc)] = static_cast<ca::u32>(decls_.size());
            decls_.push_back(d);
        }
    }

    // Phase 2：override 连通（旧并查集语义）。
    // 每个虚声明与「最近一个声明同签名的父类」 unite（传递闭包由各对
    // 相邻覆盖），再与接口闭包上全部同签名虚声明 unite。
    std::vector<ca::u32> parent(decls_.size());
    for (ca::u32 i = 0; i < parent.size(); ++i) parent[i] = i;

    for (ca::u32 i = 0; i < decls_.size(); ++i) {
        const Decl& d = decls_[i];
        if (!isVirtualDecl(d)) continue;

        const std::vector<ClassKey> chain = ch.ancestors(ClassKey{d.key.cls});
        for (ca::usize k = 1; k < chain.size(); ++k) {   // 跳过自身
            const auto it = table_.find(chain[k].id);
            if (it == table_.end()) continue;
            const auto jt = it->second.find(packSig(d.key.name, d.key.desc));
            if (jt == it->second.end()) continue;
            const Decl& super = decls_[jt->second];
            // 最近的「虚」声明成链；中间的非虚同名声明不截断链（旧 virtMap
            // 只含虚方法，等价于越过非虚声明继续上行）。
            if (isVirtualDecl(super)) {
                ufUnite(parent, i, jt->second);
                break;
            }
        }
        for (const ClassKey iface : ch.interfaceClosure(ClassKey{d.key.cls})) {
            const auto it = table_.find(iface.id);
            if (it == table_.end()) continue;
            const auto jt = it->second.find(packSig(d.key.name, d.key.desc));
            if (jt == it->second.end()) continue;
            if (isVirtualDecl(decls_[jt->second])) ufUnite(parent, i, jt->second);
        }
    }

    // Phase 3：根重编号为紧凑族 id，回填成员表。
    std::unordered_map<ca::u32, ca::u32> rootToComp;
    for (ca::u32 i = 0; i < decls_.size(); ++i) {
        const ca::u32 root = ufFind(parent, i);
        const auto [it, fresh] = rootToComp.emplace(root, 0);
        if (fresh) {
            it->second = static_cast<ca::u32>(comps_.size());
            comps_.emplace_back();
        }
        decls_[i].component = it->second;
        comps_[it->second].push_back(i);
    }
}

const MethodHierarchy::Decl* MethodHierarchy::findDecl(ClassKey cls, ca::u32 name,
                                                       ca::u32 desc) const noexcept
{
    const auto it = table_.find(cls.id);
    if (it == table_.end()) return nullptr;
    const auto jt = it->second.find(packSig(name, desc));
    return jt != it->second.end() ? &decls_[jt->second] : nullptr;
}

bool MethodHierarchy::isVirtualDecl(const Decl& d) const
{
    if (d.access & kAccPrivate) return false;
    if (d.access & kAccStatic) return false;
    const std::string_view n = pool_->str(d.key.name);
    return !(n.size() > 0 && n.front() == '<');
}

bool MethodHierarchy::isVirtual(MemberKey m) const
{
    if (ch_ == nullptr) return false;
    const Decl* d = findDecl(ClassKey{m.cls}, m.name, m.desc);
    return d != nullptr && isVirtualDecl(*d);
}

std::vector<MemberKey> MethodHierarchy::overrideFamily(MemberKey m) const
{
    std::vector<MemberKey> out;
    const Decl* self = findDecl(ClassKey{m.cls}, m.name, m.desc);
    if (self == nullptr) return out;
    for (const ca::u32 fi : comps_[self->component]) {
        out.push_back(decls_[fi].key);
    }
    return out;
}

ca::u32 MethodHierarchy::depthOf(ClassKey cls) const
{
    const std::vector<ClassKey> chain = ch_->ancestors(cls);
    return chain.empty() ? 0 : static_cast<ca::u32>(chain.size() - 1);
}

bool MethodHierarchy::shallowerOrStable(MemberKey a, MemberKey b) const
{
    const ca::u32 da = depthOf(ClassKey{a.cls});
    const ca::u32 db = depthOf(ClassKey{b.cls});
    if (da != db) return da < db;
    const bool ia = ch_->isInterface(ClassKey{a.cls});
    const bool ib = ch_->isInterface(ClassKey{b.cls});
    if (ia != ib) return ia;   // 平级时接口所有者优先
    const std::string_view na = pool_->str(a.cls);
    const std::string_view nb = pool_->str(b.cls);
    if (na != nb) return na < nb;
    const Decl* daDecl = findDecl(ClassKey{a.cls}, a.name, a.desc);
    const Decl* dbDecl = findDecl(ClassKey{b.cls}, b.name, b.desc);
    if (daDecl == nullptr || dbDecl == nullptr) return false;
    return daDecl->index < dbDecl->index;
}

MemberKey MethodHierarchy::sourceDeclaration(MemberKey m) const
{
    const Decl* self = findDecl(ClassKey{m.cls}, m.name, m.desc);
    if (self == nullptr) return MemberKey{};
    MemberKey best = self->key;
    for (const ca::u32 fi : comps_[self->component]) {
        if (shallowerOrStable(decls_[fi].key, best)) best = decls_[fi].key;
    }
    return best;
}

DispatchResult MethodHierarchy::virtualDispatch(ClassKey receiver, MemberKey sig) const
{
    DispatchResult r;
    if (ch_ == nullptr || !ch_->contains(receiver)) return r;

    // 类链优先（classes-win）：最近的虚声明直接定局——concrete → Resolved，
    // abstract → Abstract（重抽象遮蔽父类实现，JVM 就近不回退）。
    for (const ClassKey c : ch_->ancestors(receiver)) {
        const Decl* d = findDecl(c, sig.name, sig.desc);
        if (d == nullptr) continue;
        if (!isVirtualDecl(*d)) continue;   // static/private/'<' 不参与，继续上行
        if (d->access & kAccAbstract) {
            r.status = DispatchResult::Status::Abstract;
        } else {
            r.status = DispatchResult::Status::Resolved;
        }
        r.target = d->key;
        return r;
    }

    // 类链无声明 → 接口闭包。default（非 abstract）方法承载实现的场景
    // （含多继承冲突）单继承链不裁决，明确 Unresolved，不猜。
    MemberKey firstAbstract{};
    for (const ClassKey i : ch_->interfaceClosure(receiver)) {
        const Decl* d = findDecl(i, sig.name, sig.desc);
        if (d == nullptr) continue;
        if (!isVirtualDecl(*d)) continue;
        if (d->access & kAccAbstract) {
            if (firstAbstract == MemberKey{}) firstAbstract = d->key;
            continue;
        }
        r.status = DispatchResult::Status::Unresolved;
        r.target = d->key;
        return r;
    }
    if (firstAbstract != MemberKey{}) {
        r.status = DispatchResult::Status::Abstract;
        r.target = firstAbstract;
    }
    return r;
}

}   // namespace mjbase::hierarchy
