#include "rename_plan.hpp"

namespace mjbase::renamer {

void RenamePlan::mapClass(std::string_view oldName, std::string_view newName)
{
    const auto oldView = pool_.str(pool_.intern(oldName));
    const auto newView = pool_.str(pool_.intern(newName));
    // 同一旧名重复登记保持首条（引擎侧 old-slot 裁决已保证唯一，防御位）。
    if (classByOld_.find(oldView) != classByOld_.end()) return;
    classByOld_.emplace(oldView, newView);
    classMappings_.emplace_back(oldView, newView);
}

void RenamePlan::mapField(const RenameMemberRule& rule)
{
    const ca::u32 ownerId = pool_.intern(rule.owner);
    const ca::u32 nameId  = pool_.intern(rule.oldName);
    const ca::u32 descId  = pool_.intern(rule.desc);
    const MemberKey key{ownerId, nameId, descId};
    if (fieldByKey_.find(key) != fieldByKey_.end()) return;
    fieldByKey_.emplace(key, fieldRules_.size());
    fieldRules_.push_back(RenameMemberRule{pool_.str(ownerId),
                                           pool_.str(nameId),
                                           pool_.str(descId),
                                           pool_.str(pool_.intern(rule.newOwner)),
                                           pool_.str(pool_.intern(rule.newName)),
                                           pool_.str(pool_.intern(rule.newDesc))});
}

void RenamePlan::mapMethod(const RenameMemberRule& rule)
{
    const ca::u32 ownerId = pool_.intern(rule.owner);
    const ca::u32 nameId  = pool_.intern(rule.oldName);
    const ca::u32 descId  = pool_.intern(rule.desc);
    const MemberKey key{ownerId, nameId, descId};
    if (methodByKey_.find(key) != methodByKey_.end()) return;
    methodByKey_.emplace(key, methodRules_.size());
    methodRules_.push_back(RenameMemberRule{pool_.str(ownerId),
                                            pool_.str(nameId),
                                            pool_.str(descId),
                                            pool_.str(pool_.intern(rule.newOwner)),
                                            pool_.str(pool_.intern(rule.newName)),
                                            pool_.str(pool_.intern(rule.newDesc))});
}

bool RenamePlan::empty() const noexcept
{
    return classMappings_.empty() && fieldRules_.empty() && methodRules_.empty();
}

const RenameMemberRule* RenamePlan::findField(std::string_view owner, std::string_view oldName,
                                              std::string_view desc) const
{
    const auto it = fieldByKey_.find(
        MemberKey{pool_.intern(owner), pool_.intern(oldName), pool_.intern(desc)});
    return it == fieldByKey_.end() ? nullptr : &fieldRules_[it->second];
}

const RenameMemberRule* RenamePlan::findMethod(std::string_view owner, std::string_view oldName,
                                               std::string_view desc) const
{
    const auto it = methodByKey_.find(
        MemberKey{pool_.intern(owner), pool_.intern(oldName), pool_.intern(desc)});
    return it == methodByKey_.end() ? nullptr : &methodRules_[it->second];
}

std::string_view RenamePlan::mapInternalName(std::string_view internalName) const
{
    const auto it = classByOld_.find(internalName);
    return it == classByOld_.end() ? internalName : it->second;
}

void RenamePlan::mapDescriptorInto(std::string_view desc, std::string& out) const
{
    ca::usize i = 0;
    while (i < desc.size()) {
        // 数组前缀与基本类型原样复制。
        if (desc[i] != 'L') {
            out.push_back(desc[i]);
            ++i;
            continue;
        }
        const ca::usize start = i + 1;
        ca::usize       end   = start;
        while (end < desc.size() && desc[end] != ';') ++end;
        if (end >= desc.size()) {
            // 畸形描述符（无收尾分号）：剩余字节原样透传，不猜。
            out.append(desc.substr(i));
            return;
        }
        out.push_back('L');   // 对象段 L 前缀原样保留，只改写段内类名
        out.append(mapInternalName(desc.substr(start, end - start)));
        out.push_back(';');
        i = end + 1;
    }
}

std::string RenamePlan::mapFieldDescriptor(std::string_view desc) const
{
    std::string out;
    out.reserve(desc.size());
    mapDescriptorInto(desc, out);
    return out;
}

std::string RenamePlan::mapMethodDescriptor(std::string_view desc) const
{
    std::string out;
    out.reserve(desc.size());
    mapDescriptorInto(desc, out);
    return out;
}

}   // namespace mjbase::renamer
