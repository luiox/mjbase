/// @file        field_hierarchy.cpp
/// @brief       字段层次实现：沿 super 链的最近声明解析与实例字段遮蔽判定。
#include "field_hierarchy.hpp"

#include <utility>

namespace mjbase::hierarchy {

void FieldHierarchy::build(const ClassHierarchy& ch)
{
    ch_ = &ch;
    table_.clear();
    count_ = 0;

    for (const ClassKey key : ch.registrationOrder()) {
        const ClassSummary* s = ch.summary(key);
        if (s == nullptr) continue;
        auto& perClass = table_[key.id];
        for (const MemberSummary& f : s->fields) {
            Decl d;
            d.key    = MemberKey{key.id, f.name, f.desc};
            d.access = f.access;
            perClass[packSig(f.name, f.desc)] = d;
            ++count_;
        }
    }
}

const FieldHierarchy::Decl* FieldHierarchy::findDecl(ClassKey cls, ca::u32 name,
                                                     ca::u32 desc) const noexcept
{
    const auto it = table_.find(cls.id);
    if (it == table_.end()) return nullptr;
    const auto jt = it->second.find(packSig(name, desc));
    return jt != it->second.end() ? &jt->second : nullptr;
}

FieldHierarchy::FieldResolution FieldHierarchy::resolveField(ClassKey receiver,
                                                             ca::u32 name,
                                                             ca::u32 desc) const
{
    FieldResolution r;
    if (ch_ == nullptr) return r;
    // ancestors 自近及远（含 receiver 自身）；首个命中即定义类。
    for (const ClassKey c : ch_->ancestors(receiver)) {
        const Decl* d = findDecl(c, name, desc);
        if (d != nullptr) {
            r.status = FieldResolution::Status::Found;
            r.target = d->key;
            return r;
        }
    }
    return r;
}

bool FieldHierarchy::shadowsAncestor(MemberKey field) const
{
    if (ch_ == nullptr) return false;
    const Decl* own = findDecl(ClassKey{field.cls}, field.name, field.desc);
    if (own == nullptr || (own->access & kAccStatic) != 0) return false;

    // 跳过链首自身，向上找同名「实例」字段（static 不构成遮蔽）。
    const std::vector<ClassKey> chain = ch_->ancestors(ClassKey{field.cls});
    for (ca::usize i = 1; i < chain.size(); ++i) {
        const Decl* d = findDecl(chain[i], field.name, field.desc);
        if (d != nullptr && (d->access & kAccStatic) == 0) return true;
    }
    return false;
}

}   // namespace mjbase::hierarchy
