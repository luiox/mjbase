#include "mjbase/reference/keep_roots.hpp"

#include "mjbase/hierarchy/summary.hpp"

namespace mjbase::reference {

namespace {

using mjbase::hierarchy::kAccProtected;
using mjbase::hierarchy::kAccPublic;

/// 档位是否要把该类入根：public 类在 Public/PublicProtected 两档都入根
/// （类的访问标志里 ACC_PROTECTED 非法，两档类侧相同）。
bool tierKeepsClass(KeepTier tier, ca::u32 access)
{
    if (tier == KeepTier::None) return false;
    return (access & kAccPublic) != 0;
}

/// 档位是否要把该成员入根：Public 只认 public；PublicProtected 连带
/// protected（包私有与私有在两档都不入根）。
///
/// JVMS 保证成员最多带一个可见性位，故两个位独立判即可，无需先做互斥归一。
bool tierKeepsMember(KeepTier tier, ca::u32 access)
{
    if (tier == KeepTier::None) return false;
    if ((access & kAccPublic) != 0) return true;
    return tier == KeepTier::PublicProtected && (access & kAccProtected) != 0;
}

/// 档位成员根的宿主门槛：宿主类必须是 public。
///
/// 包私有类的 public 成员对外不可链接，入根会经 `enqueueClass(owner)` 把整个
/// 内部类拉回闭包，档位随之退化成「保留一切带 public 成员的类」。显式 member
/// 规则不走这里——用户点名的成员（含私有）必须保得住。
bool tierKeepsMethod(const ReferenceIndex& index, KeepTier tier, const MethodRefKey& method)
{
    if (tier == KeepTier::None) return false;
    const ClassDeclaration* owner = index.declaredClass(method.owner);
    if (owner == nullptr) return false;   // 无声明事实 → 不凭空造根
    if (!tierKeepsClass(tier, owner->access)) return false;   // 宿主门槛
    const MethodDeclaration* decl = index.declaredMethod(method.owner, method.name, method.desc);
    return decl != nullptr && tierKeepsMember(tier, decl->access);
}

bool tierKeepsField(const ReferenceIndex& index, KeepTier tier, const FieldRefKey& field)
{
    if (tier == KeepTier::None) return false;
    const ClassDeclaration* owner = index.declaredClass(field.owner);
    if (owner == nullptr) return false;
    if (!tierKeepsClass(tier, owner->access)) return false;
    const FieldDeclaration* decl = index.declaredField(field.owner, field.name, field.desc);
    return decl != nullptr && tierKeepsMember(tier, decl->access);
}

}   // namespace

void applyKeepRoots(const KeepRootSpec& spec, const ReferenceIndex& index,
                    GlobalReachabilityOptions& options)
{
    const KeepRuleSet* rules = spec.rules;

    // 类根：入根的是声明名（索引池内视图），不是规则的 glob 文本。
    for (const ClassDeclaration& decl : index.classDeclarations()) {
        if (decl.name.empty()) continue;
        if (rules != nullptr && rules->matchesClass(decl.name)) {
            options.rootClasses.emplace_back(decl.name);
            continue;
        }
        if (tierKeepsClass(spec.tier, decl.access)) {
            options.rootClasses.emplace_back(decl.name);
        }
    }

    // 方法根：规则命中（三段）或档位命中（访问标志）。
    for (const MethodDeclaration& decl : index.methodDeclarations()) {
        if (rules != nullptr &&
            rules->matchesMember(decl.method.owner, decl.method.name, decl.method.desc)) {
            options.rootMethods.push_back(decl.method);
            continue;
        }
        if (tierKeepsMethod(index, spec.tier, decl.method)) {
            options.rootMethods.push_back(decl.method);
        }
    }

    // 字段根：同上。
    for (const FieldDeclaration& decl : index.fieldDeclarations()) {
        if (rules != nullptr &&
            rules->matchesMember(decl.field.owner, decl.field.name, decl.field.desc)) {
            options.rootFields.push_back(decl.field);
            continue;
        }
        if (tierKeepsField(index, spec.tier, decl.field)) {
            options.rootFields.push_back(decl.field);
        }
    }
}

void applyRtaInstantiatedRoots(const mjbase::asm_classpath::ResolvedCallGraph& rtaGraph,
                               const ReferenceIndex& index,
                               GlobalReachabilityOptions& options)
{
    // 入根的是 RTA 图自有池内的已实例化声明类名（视图随图存活，见头注契约）；
    // 集合中的外部类型（JDK/库）无声明事实，无从保留，跳过——集合本身不去重
    // （发现序唯一），可达性 build 的 markClass 为集合语义，重复无害。
    for (const std::string_view name : rtaGraph.instantiatedClasses()) {
        if (name.empty()) continue;
        if (index.declaredClass(name) == nullptr) continue;
        options.rootClasses.push_back(name);
    }
}

}   // namespace mjbase::reference
