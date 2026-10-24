#include "name_provider.hpp"

#include <utility>

#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/hierarchy/summary.hpp"

namespace mjbase::renamer {

using ca::core::Err;
using ca::core::Ok;
using mjbase::ClassKey;

// ─────────────────────────── RenameContext ───────────────────────────

Result<std::unique_ptr<RenameContext>, std::string> RenameContext::build(
    const mjbase::model::ResourceContainer& container)
{
    // 堆上构造：ClassHierarchy 借用本对象 pool_ 的地址（查询按需 intern 的
    // 文本全部先经显式 intern），构造后对象地址不得再变——unique_ptr 转移不
    // 移动对象本身。
    std::unique_ptr<RenameContext> context(new RenameContext());

    // 类会话扫描（畸形类跳过，fail-open 与 reference_index 同口径）→ 骨架值
    // 建层级图。扫描集合只在本构建期存活；摘要拷贝的是池驻留 id。
    const auto scanned = mjbase::analysis::scanClassResources(container);
    for (const auto& scannedClass : scanned) {
        const hierarchy::ClassSummary summary = hierarchy::fillSummary(*scannedClass.session,
                                                                        context->pool_);
        auto added = context->hierarchy_.addClass(summary);
        if (added.is_err()) {
            return Err("build rename context: class '" +
                       std::string(context->pool_.str(summary.self.id)) + "': " +
                       hierarchy::to_string(std::move(added).unwrap_err()));
        }
    }
    context->methods_.build(context->hierarchy_);
    return Ok(std::move(context));
}

bool RenameContext::classExists(std::string_view internalName) const
{
    return hierarchy_.findClass(internalName).valid();
}

bool RenameContext::isSubType(std::string_view sub, std::string_view super) const
{
    const ClassKey subKey   = hierarchy_.findClass(sub);
    const ClassKey superKey = hierarchy_.findClass(super);
    if (!subKey.valid() || !superKey.valid()) return false;
    return hierarchy_.isSubtype(subKey, superKey);
}

std::string_view RenameContext::superClassOf(std::string_view internalName) const
{
    const ClassKey key = hierarchy_.findClass(internalName);
    if (!key.valid()) return {};
    const ClassKey superKey = hierarchy_.superClass(key);
    if (!superKey.valid()) return {};
    return hierarchy_.nameOf(superKey);
}

MemberKey RenameContext::keyOf(std::string_view owner, std::string_view name,
                               std::string_view desc) const
{
    const ClassKey ownerKey = hierarchy_.findClass(owner);
    return memberKeyOf(pool_, ownerKey, name, desc);
}

bool RenameContext::classDeclaresMethod(const hierarchy::ClassSummary& summary,
                                        std::string_view name, std::string_view desc) const
{
    const ca::u32 nameId = pool_.intern(name);
    const ca::u32 descId = pool_.intern(desc);
    for (const auto& method : summary.methods) {
        if (method.name == nameId && method.desc == descId) return true;
    }
    return false;
}

bool RenameContext::classDeclaresField(const hierarchy::ClassSummary& summary,
                                       std::string_view name, std::string_view desc) const
{
    const ca::u32 nameId = pool_.intern(name);
    const ca::u32 descId = pool_.intern(desc);
    for (const auto& field : summary.fields) {
        if (field.name == nameId && field.desc == descId) return true;
    }
    return false;
}

bool RenameContext::methodExists(std::string_view owner, std::string_view name,
                                 std::string_view desc) const
{
    const ClassKey ownerKey = hierarchy_.findClass(owner);
    if (!ownerKey.valid()) return false;
    const auto* ownerSummary = hierarchy_.summary(ownerKey);
    return ownerSummary != nullptr && classDeclaresMethod(*ownerSummary, name, desc);
}

Result<std::vector<OverrideComponentMember>, std::string> RenameContext::overrideComponentOf(
    std::string_view owner, std::string_view name, std::string_view desc) const
{
    const ClassKey ownerKey = hierarchy_.findClass(owner);
    if (!ownerKey.valid()) {
        return Err("override component: owner not in container: " + std::string(owner));
    }
    const std::vector<MemberKey> family = methods_.overrideFamily(keyOf(owner, name, desc));
    if (family.empty()) {
        return Err("override component: method not declared: " + std::string(owner) + "." +
                   std::string(name) + std::string(desc));
    }
    std::vector<OverrideComponentMember> members;
    members.reserve(family.size());
    for (const MemberKey key : family) {
        members.push_back({pool_.str(key.cls), pool_.str(key.name), pool_.str(key.desc)});
    }
    return Ok(std::move(members));
}

Result<MemberKey, std::string> RenameContext::overrideComponentRootOf(
    std::string_view owner, std::string_view name, std::string_view desc) const
{
    const ClassKey ownerKey = hierarchy_.findClass(owner);
    if (!ownerKey.valid()) {
        return Err("override component root: owner not in container: " + std::string(owner));
    }
    // 族内最浅声明是族的规范根：同族任意成员求根恒等（method_hierarchy 决胜序
    // 跨平台确定），可直接替代并查集根下标作去重键。
    const MemberKey root = methods_.sourceDeclaration(keyOf(owner, name, desc));
    if (root.cls == 0) {
        return Err("override component root: method not declared: " + std::string(owner) +
                   "." + std::string(name) + std::string(desc));
    }
    return Ok(root);
}

ClassKey RenameContext::resolveInstanceFieldOwner(ClassKey start, std::string_view name,
                                                  std::string_view desc) const
{
    // ancestors 自反（含自身）、悬挂键计入后截断——while(current!=INVALID)
    // 上溯逐类查声明的语义等值。
    for (const ClassKey current : hierarchy_.ancestors(start)) {
        const auto* summary = hierarchy_.summary(current);
        if (summary != nullptr && classDeclaresField(*summary, name, desc)) return current;
    }
    return ClassKey{};
}

bool RenameContext::hierarchyClosedForVirtualRename(ClassKey start) const
{
    // 闭包 = super 链（ancestors，含悬挂键）∪ 接口闭包（含传递超接口，悬挂键
    // 计入后截断）。每个可达键要么已登记（有摘要），要么豁免 java/lang/Object。
    if (!hierarchy_.contains(start)) return false;
    const auto check = [this](ClassKey key) {
        if (hierarchy_.summary(key) != nullptr) return true;
        return hierarchy_.nameOf(key) == "java/lang/Object";
    };
    for (const ClassKey key : hierarchy_.ancestors(start)) {
        if (!check(key)) return false;
    }
    for (const ClassKey key : hierarchy_.interfaceClosure(start)) {
        if (!check(key)) return false;
    }
    return true;
}

}   // namespace mjbase::renamer
