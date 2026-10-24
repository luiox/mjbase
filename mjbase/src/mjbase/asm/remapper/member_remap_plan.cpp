#include "mjbase/asm/remapper/member_remap_plan.hpp"

namespace mjbase::asm_remapper {
namespace {

/// @brief 字段描述符单类型重写：'['* 前缀保留，对象类型 `Lname;` 内部名重映射。
///
/// jbc::core::Type 的 mapDesc/mapType 语义：数组维度原样保留、
/// 元素类名随类映射同步改写、基本类型原样。畸形输入（未闭合/越界）原样返回，
/// 不抛错（经 Type 解析会抛异常，此处改值语义 fail-closed）。
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
        // 基本类型单字符（或畸形字符）原样。
        out.push_back(desc[i]);
        return out;
    }
    const ca::usize start = ++i;
    while (i < desc.size() && desc[i] != ';') i++;
    if (i >= desc.size()) return std::string(desc);   // 未闭合：原样保留
    const std::string_view oldName = desc.substr(start, i - start);
    const std::string_view newName = mapName(oldName);
    out.push_back('L');
    out.append(newName);
    out.push_back(';');
    return out;
}

/// @brief 方法描述符重写：逐参数类型 + 返回类型按字段描述符口径重拼。
///
/// mapMethodDesc 语义（括号结构与参数次序保持）。
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

void MemberRemapPlan::mapClass(std::string_view oldName, std::string_view newName)
{
    classMappings_[pool_.intern(oldName)] = pool_.intern(newName);
    // 类映射是描述符重映射的唯一输入面：映射面变化即既有缓存失效。
    // mapField/mapMethod 只增精确成员规则、不经描述符映射路径，不触缓存。
    descCache_.clear();
}

void MemberRemapPlan::mapField(const FieldRemapRule& rule)
{
    fieldRules_.push_back(rule);
}

void MemberRemapPlan::mapMethod(const MethodRemapRule& rule)
{
    methodRules_.push_back(rule);
}

void MemberRemapPlan::mapField(std::string_view owner, std::string_view name,
                               std::string_view desc, std::string_view newOwner,
                               std::string_view newName, std::string_view newDesc)
{
    FieldRemapRule rule;
    rule.oldKey = memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc);
    rule.newKey = memberKeyOf(pool_, classKeyOf(pool_, newOwner), newName, newDesc);
    mapField(rule);
}

void MemberRemapPlan::mapMethod(std::string_view owner, std::string_view name,
                                std::string_view desc, std::string_view newOwner,
                                std::string_view newName, std::string_view newDesc)
{
    MethodRemapRule rule;
    rule.oldKey = memberKeyOf(pool_, classKeyOf(pool_, owner), name, desc);
    rule.newKey = memberKeyOf(pool_, classKeyOf(pool_, newOwner), newName, newDesc);
    mapMethod(rule);
}

std::string_view MemberRemapPlan::mapInternalName(std::string_view name) const
{
    const auto it = classMappings_.find(pool_.intern(name));
    return it == classMappings_.end() ? name : pool_.str(it->second);
}

std::string_view MemberRemapPlan::mapOwnerRef(std::string_view owner) const
{
    if (!owner.empty() && owner.front() == '[') return mapFieldDescriptor(owner);
    return mapInternalName(owner);
}

std::string_view MemberRemapPlan::remapDescriptorCached(std::string_view desc, bool method) const
{
    if (desc.empty()) return desc;
    // 恒等快路：classMappings_ 为空时描述符内容必恒等，直接返回输入视图。
    if (classMappings_.empty()) return desc;
    const ca::u32 key = pool_.intern(desc);
    const auto    hit = descCache_.find(key);
    if (hit != descCache_.end()) return pool_.str(hit->second);

    const auto mapper = [this](std::string_view name) { return mapInternalName(name); };
    const std::string rewritten = method ? rewriteMethodDescriptorText(desc, mapper)
                                         : rewriteFieldDescriptorText(desc, mapper);
    const ca::u32 result = pool_.intern(rewritten);
    descCache_.emplace(key, result);
    return pool_.str(result);
}

std::string_view MemberRemapPlan::mapFieldDescriptor(std::string_view desc) const
{
    return remapDescriptorCached(desc, false);
}

std::string_view MemberRemapPlan::mapMethodDescriptor(std::string_view desc) const
{
    return remapDescriptorCached(desc, true);
}

std::string_view MemberRemapPlan::mapSignature(std::string_view signature) const
{
    if (signature.empty()) return signature;
    // 恒等快路同描述符口径。
    if (classMappings_.empty()) return signature;
    const ca::u32 key = pool_.intern(signature);
    const auto    hit = descCache_.find(key);
    if (hit != descCache_.end()) return pool_.str(hit->second);

    std::string out;
    out.reserve(signature.size());
    for (ca::usize i = 0; i < signature.size(); ++i) {
        const char c = signature[i];
        out.push_back(c);
        if (c != 'L') continue;
        const ca::usize start = i + 1;
        ca::usize       end   = start;
        while (end < signature.size() && signature[end] != ';' && signature[end] != '<') end++;
        if (end <= start) continue;
        out.append(mapInternalName(signature.substr(start, end - start)));
        i = end - 1;
        // 泛型嵌套（`Lx<T;>` 形态）的类名已终止于 '<'；'>' 与参数段原样保留，
        // 后续 `L...;` 由主循环继续处理。
    }
    const ca::u32 result = pool_.intern(out);
    descCache_.emplace(key, result);
    return pool_.str(result);
}

const FieldRemapRule* MemberRemapPlan::findField(std::string_view owner, std::string_view name,
                                                 std::string_view desc) const
{
    const FieldRemapRule* descriptorless = nullptr;
    for (const auto& rule : fieldRules_) {
        if (pool_.str(rule.oldKey.cls) != owner || pool_.str(rule.oldKey.name) != name) continue;
        if (pool_.str(rule.oldKey.desc) == desc) return &rule;
        if (rule.oldKey.desc == 0 && descriptorless == nullptr) descriptorless = &rule;
    }
    return descriptorless;
}

const MethodRemapRule* MemberRemapPlan::findMethod(std::string_view owner, std::string_view name,
                                                   std::string_view desc) const
{
    for (const auto& rule : methodRules_) {
        if (pool_.str(rule.oldKey.cls) == owner && pool_.str(rule.oldKey.name) == name &&
            pool_.str(rule.oldKey.desc) == desc) {
            return &rule;
        }
    }
    return nullptr;
}

const FieldRemapRule* MemberRemapPlan::findFieldByName(std::string_view owner,
                                                       std::string_view name) const
{
    const FieldRemapRule* match = nullptr;
    for (const auto& rule : fieldRules_) {
        if (pool_.str(rule.oldKey.cls) != owner || pool_.str(rule.oldKey.name) != name) continue;
        if (match != nullptr && pool_.str(match->newKey.name) != pool_.str(rule.newKey.name)) {
            return nullptr;
        }
        match = &rule;
    }
    return match;
}

const MethodRemapRule* MemberRemapPlan::findMethodByName(std::string_view owner,
                                                         std::string_view name) const
{
    const MethodRemapRule* match = nullptr;
    for (const auto& rule : methodRules_) {
        if (pool_.str(rule.oldKey.cls) != owner || pool_.str(rule.oldKey.name) != name) continue;
        if (match != nullptr && pool_.str(match->newKey.name) != pool_.str(rule.newKey.name)) {
            return nullptr;
        }
        match = &rule;
    }
    return match;
}

bool MemberRemapPlan::empty() const
{
    return classMappings_.empty() && fieldRules_.empty() && methodRules_.empty();
}

}   // namespace mjbase::asm_remapper
