#include "mjbase/asm/remapper/member_remap_plan.hpp"

#include <algorithm>

#include "jbc/type/signature.hpp"

namespace mjbase::asm_remapper {
namespace {

// 命名空间别名置匿名命名空间作用域：函数体内别名在 MSVC 泛型 lambda 延迟
// 实例化里查找失败（sig::X 被归解为 jbc::sig）。
namespace jbsig = jbc::type;

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

/// @brief 结构化签名重写：libjbc jbc::type::signature 消费面（B8 8.3 收口）。
///
/// 解析树定界全部类类型出现（parseSignature 自动分派三形态），逐类类型按
/// **完整内部名**（首段全名 + 后缀段按 '$' 连接，对齐 libjbc
/// internalNameOf 口径）查类映射，命中以段级 span 补丁拼装新串——逐字节
/// 边界来自解析树，杜绝扫描式改写的边界误读。形态纪律对齐 libjbc
/// renameClassInSignature 契约（v1 段链形态轴）并延伸一个改名语义分支：
///   - **扁平段链**（单段、无 '.' 后缀段）：全名命中整段替换，新名原文写满
///     本段（跨 '$' 形态替换，跨混淆器映射还原的载体）；
///   - **真嵌套段链（部件对齐）**：新名按 '$' 部件对齐替换（每段消费
///     dollarCount(段名)+1 个部件），段链结构与链上实参原样保留；
///   - **真嵌套段链（部件数不一致）**：改名 pass 给嵌套类配独立新名
///     （a/B$C → p/Q），签名引用的二元名整体易主——整链坍缩为扁平新名
///     （ASM SignatureRemapper 同语义），链间 '.' 与链上实参随旧链结构
///     一并消失（libjbc 单对手术 API 无法表达此语义，计划驱动重写可以）；
///   - 类型实参/通配符/数组维度/类型变量等其余字节原样保留。
/// 解析失败原样返回 false（fail-closed 保真，畸形签名的清洗归
/// IllegalSignatureRemover；不做扫描式兜底）。单遍同时替换语义：每个类类型
/// 出现只查一次映射，新名不会被后续映射再吃（A→B、B→C 并存时不串链）。
///
/// @returns 发生改写时 true 并把新串写入 @p out；未命中/解析失败 false。
template <typename MapName>
bool rewriteSignatureText(std::string_view signature, MapName&& mapName, std::string& out)
{
    // 三形态显式解析（与 parseSignature 自动分派等价，见下方分派序注记）。

    /// 一个段级改写坐标（offset 相对输入串；text 为替换字节）。
    struct SigPatch
    {
        ca::u32     offset = 0;
        ca::u32     length = 0;
        std::string text;
    };

    std::vector<SigPatch> patches;
    bool                  changed = false;

    /// 段名里的 '$' 个数（该段消费的部件数 = 个数 + 1，与 libjbc 同口径）。
    const auto dollarCount = [](std::string_view name) {
        ca::usize n = 0;
        for (const char c : name) {
            if (c == '$') ++n;
        }
        return n;
    };
    /// 按 '$' 切内部名为部件（供真嵌套段链的部件对齐）。
    const auto splitParts = [](std::string_view name) {
        std::vector<std::string_view> parts;
        ca::usize                     begin = 0;
        for (ca::usize i = 0; i <= name.size(); ++i) {
            if (i == name.size() || name[i] == '$') {
                parts.push_back(name.substr(begin, i - begin));
                begin = i + 1;
            }
        }
        return parts;
    };

    /// 一个类类型的映射改写（命中挂段级补丁；类型实参随后由调用方递归）。
    /// @returns 整链坍缩时 true（链上实参随旧链结构一并消失，调用方不得
    /// 再递归——否则会产生与坍缩补丁重叠的实参补丁）。
    const auto mapClassType = [&](const jbsig::SigClassType& ct) -> bool {
        const std::string_view prefix = ct.package_prefix;
        std::string            full(prefix);
        for (ca::usize i = 0; i < ct.segments.size(); ++i) {
            if (i > 0) full.push_back('$');
            full.append(ct.segments[i].name);
        }
        const std::string_view mapped = mapName(full);
        if (mapped == full) return false;

        const auto base = static_cast<ca::u32>(prefix.data() - signature.data());
        if (ct.segments.size() == 1) {
            // 扁平段链：全名命中整段替换（新名须为合法内部名）。
            if (!jbsig::isValidSigInternalName(mapped)) return false;
            patches.push_back(
                SigPatch{base,
                         static_cast<ca::u32>(prefix.size() + ct.segments[0].name.size()),
                         std::string(mapped)});
            changed = true;
            return false;
        }
        // 真嵌套段链（canonical '.' 形态），全名命中后按新旧部件结构分派：
        const auto oldParts = splitParts(full);
        const auto newParts = splitParts(mapped);
        if (newParts.size() == oldParts.size()) {
            // 部件对齐：逐段替换，段链结构与链上类型实参原样保留。
            ca::usize       part    = 0;
            const ca::usize partEnd = patches.size();
            for (ca::usize i = 0; i < ct.segments.size(); ++i) {
                const jbsig::SigClassSegment& seg  = ct.segments[i];
                const ca::usize             need = dollarCount(seg.name) + 1;
                if (part + need > newParts.size()) {
                    patches.resize(partEnd);
                    return false;
                }
                std::string replacement(newParts[part]);
                for (ca::usize k = 1; k < need; ++k) {
                    replacement.push_back('$');
                    replacement.append(newParts[part + k]);
                }
                const bool valid = (i == 0) ? jbsig::isValidSigInternalName(replacement)
                                            : jbsig::isValidSigSimpleName(replacement);
                if (!valid) {
                    patches.resize(partEnd);
                    return false;
                }
                if (i == 0) {
                    patches.push_back(
                        SigPatch{base,
                                 static_cast<ca::u32>(prefix.size() + seg.name.size()),
                                 std::move(replacement)});
                }
                else {
                    patches.push_back(
                        SigPatch{static_cast<ca::u32>(seg.name.data() - signature.data()),
                                 static_cast<ca::u32>(seg.name.size()),
                                 std::move(replacement)});
                }
                part += need;
            }
            changed = true;
            return false;
        }
        // 部件数不一致：改名 pass 给嵌套类配的是独立新名（如 a/B$C → p/Q，
        // 新类是扁平顶级类）。签名引用的二元名已整体易主——整链坍缩为扁平
        // 新名（ASM SignatureRemapper 同语义）：补丁盖住前缀起到末段名止的
        // 全部字节，链间 '.' 与各段实参（旧链外围参数化，扁平新类不再需要）
        // 一并收编。新名须为合法内部名。
        if (!jbsig::isValidSigInternalName(mapped)) return false;
        const jbsig::SigClassSegment& last = ct.segments.back();
        patches.push_back(
            SigPatch{base,
                     static_cast<ca::u32>((last.name.data() - signature.data()) +
                                          last.name.size()) -
                         base,
                     std::string(mapped)});
        changed = true;
        return true;
    };

    /// 树遍历：类类型 → 映射 + 逐段实参递归（span 序与 libjbc 同款字节序）。
    const auto walkType = [&](const jbsig::SigType& t, auto&& self) -> void {
        if (t.kind != jbsig::SigType::Kind::Class) return;
        if (mapClassType(t.class_type)) return;   // 整链坍缩：实参随旧链消失
        for (const jbsig::SigClassSegment& seg : t.class_type.segments) {
            for (const jbsig::SigTypeArg& arg : seg.type_args) {
                if (arg.bound != nullptr) self(*arg.bound, self);
            }
        }
    };

    // 三形态显式解析 + move-unwrap（树含 unique_ptr 成员不可拷贝）。分派序
    // 类 → 方法 → 字段与 parseSignature 自动分派等价（'(' 与 '['/'T' 起始
    // 只可能是一种形态；'L' 起先试类、失败落字段；'<' 起类失败落方法）。
    {
        auto classSig = jbsig::parseClassSignature(signature);
        if (classSig.is_ok()) {
            auto cs = std::move(classSig).unwrap();
            for (const jbsig::SigTypeParam& tp : cs.type_params) {
                if (tp.class_bound != nullptr) walkType(*tp.class_bound, walkType);
                for (const jbsig::SigType& ib : tp.interface_bounds) walkType(ib, walkType);
            }
            walkType(cs.superclass, walkType);
            for (const jbsig::SigType& itf : cs.super_interfaces) walkType(itf, walkType);
        }
        else {
            auto methodSig = jbsig::parseMethodSignature(signature);
            if (methodSig.is_ok()) {
                auto ms = std::move(methodSig).unwrap();
                for (const jbsig::SigTypeParam& tp : ms.type_params) {
                    if (tp.class_bound != nullptr) walkType(*tp.class_bound, walkType);
                    for (const jbsig::SigType& ib : tp.interface_bounds) {
                        walkType(ib, walkType);
                    }
                }
                for (const jbsig::SigType& param : ms.params) walkType(param, walkType);
                if (!ms.ret_void) walkType(ms.ret, walkType);
                for (const jbsig::SigType& thrown : ms.throws_list) walkType(thrown, walkType);
            }
            else {
                auto fieldSig = jbsig::parseFieldSignature(signature);
                if (fieldSig.is_err()) return false;
                walkType(std::move(fieldSig).unwrap(), walkType);
            }
        }
    }

    if (!changed) return false;
    // 补丁按字节序前向拼装（坐标两两不相交：段名与其实参在字节上互不重叠）。
    std::sort(patches.begin(), patches.end(),
              [](const SigPatch& a, const SigPatch& b) { return a.offset < b.offset; });
    out.clear();
    out.reserve(signature.size());
    ca::usize copied = 0;
    for (const SigPatch& patch : patches) {
        out.append(signature.substr(copied, patch.offset - copied));
        out.append(patch.text);
        copied = patch.offset + patch.length;
    }
    out.append(signature.substr(copied));
    return true;
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

    // 结构化重写（libjbc jbc::type::signature 消费面，见 rewriteSignatureText）；
    // 解析失败/未命中原样驻留返回（fail-closed 保真）。
    std::string rewritten;
    if (rewriteSignatureText(signature,
                             [this](std::string_view name) { return mapInternalName(name); },
                             rewritten)) {
        const ca::u32 result = pool_.intern(rewritten);
        descCache_.emplace(key, result);
        return pool_.str(result);
    }
    const ca::u32 result = pool_.intern(signature);
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
