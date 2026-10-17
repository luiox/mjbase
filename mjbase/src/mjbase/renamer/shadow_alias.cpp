#include "shadow_alias.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>

#include "jhasm/util/escape_util.hpp"
#include "libca/crypto/random.hpp"

#include "mjbase/analysis/alias_analysis.hpp"
#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/mapping/identifier_util.hpp"

namespace mjbase::renamer {
namespace {

using analysis::aliasInterned;
using analysis::aliasMemberKey;

std::string toStd(std::string_view value)
{
    return std::string(value);
}

ca::str::Utf8StringRef utf8Ref(std::string_view value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// 池驻留：空视图得空视图（条目字段空 = 无），其余内容寻址驻留进表私有池。
std::string_view internName(mjbase::KeyPool& pool, std::string_view value)
{
    return value.empty() ? std::string_view{} : pool.str(pool.intern(value));
}

/// (name, desc) 字典序比较器（#1072 钉死：类内稳定序）。
bool memberPairLess(const std::pair<std::string, std::string>& a,
                    const std::pair<std::string, std::string>& b)
{
    if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
}

/// Utf8StringRef → 裸字节借用视图（池驻留入参口径）。
std::string_view viewOf(const ca::str::Utf8StringRef& value)
{
    return std::string_view(reinterpret_cast<const char*>(value.data()), value.byte_length());
}

/// jasm 显示 token 的转义形式（与 libjhasm printer 的 literal() 同一函数，
/// 保证改写映射的 key 与反汇编文本里的 token 逐字节一致）。
std::string jasmEscaped(std::string_view value)
{
    const auto utf16   = ca::str::Utf16String::from_utf8_string(utf8Ref(value));
    const auto escaped = jhasm::util::EscapeUtil::escape_literal(utf16.ref());
    return escaped.to_utf8_string().to_std_string();
}

/// #1156 引用 token：4 字节 CSPRNG → 8 位小写 hex（uint32 %08x 语义）；
/// 与 taken 撞号则重生成（表内唯一）。随机源 = libca secure_random_bytes
/// （系统 CSPRNG）；系统 RNG 失败属环境灾难，报 Err 而非自造随机源降级。
ca::core::Result<std::string, std::string> freshUniqueToken8(
    std::unordered_set<std::string>& taken)
{
    for (;;) {
        auto bytes = ca::crypto::secure_random_bytes(4);
        if (bytes.is_err()) {
            return ca::core::Err(std::string("secure_random_bytes failed: system RNG unavailable"));
        }
        auto       data = std::move(bytes).unwrap();
        const auto raw  = data.as_ptr();
        char       buf[9]  = {};
        std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", raw[0], raw[1], raw[2], raw[3]);
        if (taken.insert(std::string(buf, 8)).second) return ca::core::Ok(std::string(buf, 8));
    }
}

std::string formatIndexName(const char* prefix, ca::usize index)
{
    return std::string(prefix) + std::to_string(index);
}

/// jasm 文本扫描的分隔符：其余字符（含 / . $ < > \ 与转义序列）均为 token 字符。
bool isJasmDelimiter(char c)
{
    switch (c) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
    case '{':
    case '}':
    case '(':
    case ')':
    case '[':
    case ']':
    case '"':
    case '\'':
    case ',':
    case ';': return true;
    default: return false;
    }
}

}   // namespace

// ─────────────────────────── ShadowAliasTable ───────────────────────────

mjbase::MemberKey ShadowAliasTable::memberKeyOf(std::string_view owner, std::string_view raw,
                                                std::string_view desc)
{
    // 别名进程级驻留池建键（内容相同 ⇔ 键等值）；与 alias/remapper 消费面
    // 同一契约，不另造拼接串键。
    return aliasMemberKey(owner, raw, desc);
}

ShadowAliasTable ShadowAliasTable::fromMapping(
    const mjbase::mapping::IntermediateMapping& mappings)
{
    ShadowAliasTable table;
    for (const auto& [key, value] : mappings.getClasses()) {
        table.addClass(viewOf(value.oldName), viewOf(value.newName), viewOf(value.token));
    }
    for (const auto& [owner, fields] : mappings.getFields()) {
        for (const auto& fm : fields) {
            table.addField(viewOf(fm.ownerName),
                           viewOf(fm.desc),
                           viewOf(fm.oldName),
                           viewOf(fm.newName),
                           viewOf(fm.token));
        }
    }
    for (const auto& [owner, methods] : mappings.getMethods()) {
        for (const auto& mm : methods) {
            table.addMethod(viewOf(mm.ownerName),
                            viewOf(mm.desc),
                            viewOf(mm.oldName),
                            viewOf(mm.newName),
                            viewOf(mm.token));
        }
    }
    return table;
}

mjbase::mapping::IntermediateMapping ShadowAliasTable::toMapping() const
{
    mjbase::mapping::IntermediateMapping mappings;
    for (const auto& entry : entries_) {
        const auto ownerRef  = utf8Ref(entry.owner);
        const auto descRef   = utf8Ref(entry.desc);
        const auto rawRef    = utf8Ref(entry.rawName);
        const auto shadowRef = utf8Ref(entry.shadowName);
        const auto tokenRef  = utf8Ref(entry.token8);
        switch (entry.kind) {
        case ShadowKind::Class: mappings.addClass(rawRef, shadowRef, tokenRef); break;
        case ShadowKind::Field:
            mappings.addField(ownerRef, descRef, rawRef, shadowRef, tokenRef);
            break;
        case ShadowKind::Method:
            mappings.addMethod(ownerRef, descRef, rawRef, shadowRef, tokenRef);
            break;
        }
    }
    return mappings;
}

bool ShadowAliasTable::addClass(std::string_view raw, std::string_view shadow,
                                std::string_view token8)
{
    if (raw.empty() || shadow.empty()) return false;
    const auto rawId = pool_.intern(raw);
    if (classByRaw_.find(rawId) != classByRaw_.end()) return false;

    const auto rawView    = pool_.str(rawId);
    const auto shadowView = internName(pool_, shadow);
    const auto tokenView  = internName(pool_, token8);
    const auto index      = entries_.size();
    entries_.push_back({ShadowKind::Class, {}, {}, rawView, shadowView, tokenView});
    classByRaw_.emplace(rawId, index);
    byShadowName_[shadowView].push_back(index);
    byRawName_[rawView].push_back(index);
    return true;
}

bool ShadowAliasTable::addField(std::string_view owner, std::string_view desc,
                                std::string_view raw, std::string_view shadow,
                                std::string_view token8)
{
    if (raw.empty() || shadow.empty()) return false;
    const auto key = memberKeyOf(owner, raw, desc);
    if (memberByOwnerKey_.find(key) != memberByOwnerKey_.end()) return false;

    const auto ownerView  = internName(pool_, owner);
    const auto descView   = internName(pool_, desc);
    const auto rawView    = internName(pool_, raw);
    const auto shadowView = internName(pool_, shadow);
    const auto tokenView  = internName(pool_, token8);
    const auto index      = entries_.size();
    entries_.push_back({ShadowKind::Field, ownerView, descView, rawView, shadowView, tokenView});
    memberByOwnerKey_.emplace(key, index);
    byShadowName_[shadowView].push_back(index);
    byRawName_[rawView].push_back(index);
    return true;
}

bool ShadowAliasTable::addMethod(std::string_view owner, std::string_view desc,
                                 std::string_view raw, std::string_view shadow,
                                 std::string_view token8)
{
    if (raw.empty() || shadow.empty()) return false;
    const auto key = memberKeyOf(owner, raw, desc);
    if (memberByOwnerKey_.find(key) != memberByOwnerKey_.end()) return false;

    const auto ownerView  = internName(pool_, owner);
    const auto descView   = internName(pool_, desc);
    const auto rawView    = internName(pool_, raw);
    const auto shadowView = internName(pool_, shadow);
    const auto tokenView  = internName(pool_, token8);
    const auto index      = entries_.size();
    entries_.push_back({ShadowKind::Method, ownerView, descView, rawView, shadowView, tokenView});
    memberByOwnerKey_.emplace(key, index);
    byShadowName_[shadowView].push_back(index);
    byRawName_[rawView].push_back(index);
    return true;
}

std::string_view ShadowAliasTable::classShadow(std::string_view raw) const
{
    const auto it = classByRaw_.find(pool_.intern(raw));
    return it == classByRaw_.end() ? std::string_view{} : entries_[it->second].shadowName;
}

std::string_view ShadowAliasTable::fieldShadow(std::string_view owner, std::string_view raw,
                                               std::string_view desc) const
{
    const auto it = memberByOwnerKey_.find(memberKeyOf(owner, raw, desc));
    if (it == memberByOwnerKey_.end()) return {};
    const auto& entry = entries_[it->second];
    return entry.kind == ShadowKind::Field ? entry.shadowName : std::string_view{};
}

std::string_view ShadowAliasTable::methodShadow(std::string_view owner, std::string_view raw,
                                                std::string_view desc) const
{
    const auto it = memberByOwnerKey_.find(memberKeyOf(owner, raw, desc));
    if (it == memberByOwnerKey_.end()) return {};
    const auto& entry = entries_[it->second];
    return entry.kind == ShadowKind::Method ? entry.shadowName : std::string_view{};
}

std::string ShadowAliasTable::renderClass(std::string_view raw) const
{
    const auto shadow = classShadow(raw);
    const auto rawEsc = mjbase::mapping::IdentifierUtil::printReadable(toStd(raw));
    if (shadow.empty()) return rawEsc;
    return mjbase::mapping::IdentifierUtil::printReadable(toStd(shadow)) + " (" + rawEsc + ")";
}

std::string ShadowAliasTable::renderMemberName(std::string_view owner, std::string_view raw,
                                               std::string_view desc) const
{
    auto shadow = methodShadow(owner, raw, desc);
    if (shadow.empty()) shadow = fieldShadow(owner, raw, desc);
    const auto rawEsc = mjbase::mapping::IdentifierUtil::printReadable(toStd(raw));
    if (shadow.empty()) return rawEsc;
    return mjbase::mapping::IdentifierUtil::printReadable(toStd(shadow)) + " (" + rawEsc + ")";
}

std::string ShadowAliasTable::renderMember(std::string_view owner, std::string_view name,
                                           std::string_view desc) const
{
    const auto ownerEsc = mjbase::mapping::IdentifierUtil::printReadable(toStd(owner));
    const auto nameEsc  = mjbase::mapping::IdentifierUtil::printReadable(toStd(name));
    const auto descEsc  = mjbase::mapping::IdentifierUtil::printReadable(toStd(desc));
    const auto rawForm  = ownerEsc + "." + nameEsc + ":" + descEsc;

    const auto ownerShadow = classShadow(owner);
    auto       nameShadow  = methodShadow(owner, name, desc);
    if (nameShadow.empty()) nameShadow = fieldShadow(owner, name, desc);
    if (ownerShadow.empty() && nameShadow.empty()) return rawForm;

    auto ownerPart = ownerEsc;
    if (!ownerShadow.empty()) {
        ownerPart = mjbase::mapping::IdentifierUtil::printReadable(toStd(ownerShadow)) +
                    " (" + ownerEsc + ")";
    }
    auto namePart = nameEsc;
    if (!nameShadow.empty()) {
        namePart = mjbase::mapping::IdentifierUtil::printReadable(toStd(nameShadow)) + " (" +
                   nameEsc + ")";
    }

    // P0-2（luiox/morpher#1106）：尾随 ` (完整原始形式)` 属信息删除，必须逐字段
    // 核对尾段与前段完全重合才可去。当前格式下 owner/name 在前段必然出现
    // （直接或 `shadow (raw)` 括号内；结构性后缀比较，不用子串搜索，防名字
    // 本身含 `(`/`)` 时误判），desc 恒以 `:` + descEsc 收尾 → 三字段全部重合，
    // 尾段为纯重复，恒去；若未来格式变动致任一字段仅尾段可见，则保留尾段。
    const auto covered = [](const std::string& part, const std::string& rawEsc) {
        if (part == rawEsc) return true;
        const std::string paren = " (" + rawEsc + ")";
        return part.size() >= paren.size() &&
               part.compare(part.size() - paren.size(), paren.size(), paren) == 0;
    };
    std::string text = ownerPart + "." + namePart + ":" + descEsc;
    if (!(covered(ownerPart, ownerEsc) && covered(namePart, nameEsc))) {
        text += " (" + rawForm + ")";
    }
    return text;
}

std::vector<ShadowAliasEntry> ShadowAliasTable::findByShadowName(std::string_view shadow) const
{
    std::vector<ShadowAliasEntry> hits;
    const auto                    it = byShadowName_.find(pool_.str(pool_.intern(shadow)));
    if (it == byShadowName_.end()) return hits;
    for (const auto index : it->second) hits.push_back(entries_[index]);
    return hits;
}

std::vector<ShadowAliasEntry> ShadowAliasTable::findByRawName(std::string_view raw) const
{
    std::vector<ShadowAliasEntry> hits;
    const auto                    it = byRawName_.find(pool_.str(pool_.intern(raw)));
    if (it == byRawName_.end()) return hits;
    for (const auto index : it->second) hits.push_back(entries_[index]);
    return hits;
}

std::vector<ShadowAliasEntry> ShadowAliasTable::entriesOfClass(std::string_view owner) const
{
    std::vector<ShadowAliasEntry> hits;
    if (const auto* entry = classEntry(owner)) hits.push_back(*entry);
    const auto ownerView = pool_.intern(owner);
    for (const auto& entry : entries_) {
        if (entry.kind != ShadowKind::Class &&
            entry.owner == pool_.str(ownerView)) {
            hits.push_back(entry);
        }
    }
    return hits;
}

const ShadowAliasEntry* ShadowAliasTable::classEntry(std::string_view raw) const
{
    const auto it = classByRaw_.find(pool_.intern(raw));
    return it == classByRaw_.end() ? nullptr : &entries_[it->second];
}

ca::core::Result<ca::usize, std::string> ShadowAliasTable::backfillTokens()
{
    // 已占用 token 集合（含历史条目）；新号在此域内唯一（碰撞重生成）。
    std::unordered_set<std::string> taken;
    taken.reserve(entries_.size() * 2);
    for (const auto& entry : entries_) {
        if (!entry.token8.empty()) taken.insert(toStd(entry.token8));
    }

    ca::usize assigned = 0;
    for (auto& entry : entries_) {
        if (!entry.token8.empty()) continue;
        auto fresh = freshUniqueToken8(taken);
        if (fresh.is_err()) return ca::core::Err(std::move(fresh).unwrap_err());
        entry.token8 = internName(pool_, std::move(fresh).unwrap());
        ++assigned;
    }
    return ca::core::Ok(assigned);
}

bool ShadowAliasTable::classShadowTaken(std::string_view shadow,
                                        std::string_view exceptRaw) const
{
    const auto it = byShadowName_.find(pool_.str(pool_.intern(shadow)));
    if (it == byShadowName_.end()) return false;
    const auto exceptView = pool_.intern(exceptRaw);
    for (const auto index : it->second) {
        const auto& entry = entries_[index];
        if (entry.kind == ShadowKind::Class && entry.rawName != pool_.str(exceptView)) {
            return true;
        }
    }
    return false;
}

bool ShadowAliasTable::memberShadowTaken(std::string_view owner, std::string_view shadow) const
{
    const auto it = byShadowName_.find(pool_.str(pool_.intern(shadow)));
    if (it == byShadowName_.end()) return false;
    const auto ownerView = pool_.intern(owner);
    for (const auto index : it->second) {
        const auto& entry = entries_[index];
        if (entry.kind != ShadowKind::Class && entry.owner == pool_.str(ownerView)) return true;
    }
    return false;
}

void ShadowAliasTable::clear()
{
    pool_ = mjbase::KeyPool();
    entries_.clear();
    classByRaw_.clear();
    memberByOwnerKey_.clear();
    byShadowName_.clear();
    byRawName_.clear();
}

// ─────────────────────────── 批量机械占位名生成 ───────────────────────────

namespace {

/// 容器内一个 class 的成员采样（生成器排序输入；std::string 便于稳定字典序）。
struct ClassSample
{
    std::string                                      name;
    std::vector<std::pair<std::string, std::string>> methods;   // (name, desc)
    std::vector<std::pair<std::string, std::string>> fields;    // (name, desc)
};

std::vector<ClassSample> sampleContainerClasses(const mjbase::model::ResourceContainer& container)
{
    std::vector<ClassSample> samples;
    // 逐类独立池：解析产物只活到采样完成（class_scan fail-open，畸形类跳过）。
    for (const auto& scanned : mjbase::analysis::scanClassResources(container)) {
        mjbase::KeyPool            localPool;
        const hierarchy::ClassSummary summary = hierarchy::fillSummary(*scanned.session,
                                                                        localPool);
        ClassSample sample;
        sample.name = toStd(localPool.str(summary.self.id));
        for (const auto& method : summary.methods) {
            sample.methods.emplace_back(toStd(localPool.str(method.name)),
                                        toStd(localPool.str(method.desc)));
        }
        for (const auto& field : summary.fields) {
            sample.fields.emplace_back(toStd(localPool.str(field.name)),
                                       toStd(localPool.str(field.desc)));
        }
        samples.push_back(std::move(sample));
    }
    return samples;
}

}   // namespace

ca::core::Result<ShadowGenStats, std::string> generateMechanicalShadowNames(
    const mjbase::model::ResourceContainer& container, ShadowAliasTable& table)
{
    ShadowGenStats stats;
    stats.keptEntries = table.size();

    auto samples = sampleContainerClasses(container);
    std::sort(samples.begin(), samples.end(),
              [](const ClassSample& a, const ClassSample& b) { return a.name < b.name; });

    // 类内成员按 (name, desc) 字典序；N 为"分配计数器"（从 1 递增，只在实际
    // 分配时前进）——已有 shadow（手工名/上一轮批号）的符号跳过不占号，撞名
    // 递增跳过；jar 不变重跑零改写（幂等），容器遍历序不影响输出（先排序）。
    ca::usize classCounter = 1;
    for (const auto& sample : samples) {
        const std::string owner = sample.name;

        if (table.classEntry(owner) == nullptr) {
            ca::usize n = classCounter;
            while (table.classShadowTaken(formatIndexName("Class_", n), owner)) {
                ++n;
            }
            table.addClass(owner, formatIndexName("Class_", n));
            classCounter = n + 1;
            ++stats.assignedClasses;
        }

        auto sortedMethods = sample.methods;
        std::sort(sortedMethods.begin(), sortedMethods.end(), memberPairLess);
        ca::usize methodCounter = 1;
        for (const auto& [methodName, methodDesc] : sortedMethods) {
            if (!table.methodShadow(owner, methodName, methodDesc).empty()) continue;
            ca::usize n = methodCounter;
            while (table.memberShadowTaken(owner, formatIndexName("Method_", n))) {
                ++n;
            }
            table.addMethod(owner, methodDesc, methodName, formatIndexName("Method_", n));
            methodCounter = n + 1;
            ++stats.assignedMethods;
        }

        auto sortedFields = sample.fields;
        std::sort(sortedFields.begin(), sortedFields.end(), memberPairLess);
        ca::usize fieldCounter = 1;
        for (const auto& [fieldName, fieldDesc] : sortedFields) {
            if (!table.fieldShadow(owner, fieldName, fieldDesc).empty()) continue;
            ca::usize n = fieldCounter;
            while (table.memberShadowTaken(owner, formatIndexName("Field_", n))) {
                ++n;
            }
            table.addField(owner, fieldDesc, fieldName, formatIndexName("Field_", n));
            fieldCounter = n + 1;
            ++stats.assignedFields;
        }
    }

    // #1156：token 列收尾——新条目与历史缺号条目（v1 sidecar 遗留）一律补
    // CSPRNG token8（表内唯一；已有 token 原样保留，幂等）。随机列不参与
    // 上面的编号与排序，编号确定性不受影响。随机源失败 → Err（已分配编号
    // 保留在表，重跑幂等补号）。
    auto tokens = table.backfillTokens();
    if (tokens.is_err()) {
        return ca::core::Err(std::move(tokens).unwrap_err());
    }
    return ca::core::Ok(stats);
}

ca::usize pruneShadowAliases(const mjbase::model::ResourceContainer& container,
                             ShadowAliasTable&                       table)
{
    // 存在性索引：class 集合 + 每 owner 的成员 (kind, desc, name) 集合。
    std::unordered_set<std::string> classes;
    std::unordered_map<std::string, std::unordered_set<std::string>> membersByOwner;
    for (const auto& sample : sampleContainerClasses(container)) {
        classes.insert(sample.name);
        auto& members = membersByOwner[sample.name];
        for (const auto& [methodName, methodDesc] : sample.methods) {
            members.insert(std::string("m") + '\x1f' + methodName + '\x1f' + methodDesc);
        }
        for (const auto& [fieldName, fieldDesc] : sample.fields) {
            members.insert(std::string("f") + '\x1f' + fieldName + '\x1f' + fieldDesc);
        }
    }

    // 先收集存活条目（owned 拷贝），再原地重建——clear() 使旧视图失效，
    // 重建期不得再引用旧条目。token8 随行（#1156：prune 不得丢 token）。
    struct KeptEntry
    {
        ShadowKind  kind;
        std::string owner;
        std::string desc;
        std::string raw;
        std::string shadow;
        std::string token8;
    };
    std::vector<KeptEntry> kept;
    for (const auto& entry : table.entries()) {
        const auto owner = toStd(entry.owner);
        const auto desc  = toStd(entry.desc);
        const auto raw   = toStd(entry.rawName);
        bool       alive = false;
        if (entry.kind == ShadowKind::Class) {
            alive = classes.count(raw) > 0;
        }
        else if (classes.count(owner) > 0) {
            const auto& members = membersByOwner[owner];
            const std::string key =
                std::string(entry.kind == ShadowKind::Field ? "f" : "m") + '\x1f' + raw +
                '\x1f' + desc;
            alive = members.count(key) > 0;
        }
        if (alive) {
            kept.push_back({entry.kind, owner, desc, raw, toStd(entry.shadowName),
                            toStd(entry.token8)});
        }
    }
    const ca::usize removed = table.size() - kept.size();
    if (removed == 0) return 0;

    table.clear();
    for (const auto& entry : kept) {
        switch (entry.kind) {
        case ShadowKind::Class: table.addClass(entry.raw, entry.shadow, entry.token8); break;
        case ShadowKind::Field:
            table.addField(entry.owner, entry.desc, entry.raw, entry.shadow, entry.token8);
            break;
        case ShadowKind::Method:
            table.addMethod(entry.owner, entry.desc, entry.raw, entry.shadow, entry.token8);
            break;
        }
    }
    return removed;
}

// ─────────────────────────── jasm 显示层改写 ───────────────────────────

std::string rewriteJasmForDisplay(std::string_view jasmText, const ShadowAliasTable& table,
                                  std::string_view ownerClass)
{
    // 替换映射（key = jasm 转义后的 token 形式，与 printer literal() 同源）：
    // - class 条目全 jar 生效（.class/.super/描述符内的引用）；
    // - 成员条目仅 ownerClass 生效：裸名 token（.field/.method 声明行）与
    //   `owner.name` 复合 token（invoke 操作数 / handle 字面量）。
    // field 与 method 在 JVM 里允许同名（混淆器会造）：raw 名相同时按上下文
    // 区分——名字 token 后随 `(` 开头的描述符即方法位，否则字段位；仅登记了
    // 一种 kind 时不区分上下文（声明行/操作数/同名局部注解一律同显）。
    std::unordered_map<std::string, std::string> classMap;
    std::unordered_map<std::string, std::string> methodMap;
    std::unordered_map<std::string, std::string> fieldMap;
    std::unordered_map<std::string, std::string> compositeMethodMap;
    std::unordered_map<std::string, std::string> compositeFieldMap;
    for (const auto& entry : table.entries()) {
        if (entry.kind == ShadowKind::Class) {
            classMap[jasmEscaped(entry.rawName)] = jasmEscaped(entry.shadowName);
            continue;
        }
        if (entry.owner != ownerClass) continue;
        const auto rawKey     = jasmEscaped(entry.rawName);
        const auto shadowText = jasmEscaped(entry.shadowName);
        if (entry.kind == ShadowKind::Method) {
            methodMap[rawKey] = shadowText;
            compositeMethodMap[jasmEscaped(entry.owner) + "." + rawKey] = shadowText;
        }
        else {
            fieldMap[rawKey] = shadowText;
            compositeFieldMap[jasmEscaped(entry.owner) + "." + rawKey] = shadowText;
        }
    }
    if (classMap.empty() && methodMap.empty() && fieldMap.empty()) {
        return std::string(jasmText);
    }

    const char* text   = jasmText.data();
    const auto  length = jasmText.size();
    std::string out;
    out.reserve(length + length / 8);

    bool        inQuote   = false;   // "..."
    bool        inLiteral = false;   // '...'
    bool        escaped   = false;   // 引号内反斜杠转义
    std::string token;

    // 名字 token 后（同行、跳过空格）首个字符为 `(` ⇒ 方法上下文。
    auto nextTokenIsDescriptor = [&](ca::usize delimiterIndex) {
        auto j = delimiterIndex;
        while (j < length && (text[j] == ' ' || text[j] == '\t')) ++j;
        return j < length && text[j] == '(';
    };
    // 双 kind 同名时按上下文取映射；仅一种 kind 时不区分上下文。
    auto memberShadowFor = [&](const std::string& key, bool methodContext)
        -> const std::string* {
        const bool hasMethod = methodMap.count(key) > 0;
        const bool hasField  = fieldMap.count(key) > 0;
        if (hasMethod && hasField) {
            return methodContext ? &methodMap[key] : &fieldMap[key];
        }
        if (hasMethod) return &methodMap[key];
        if (hasField) return &fieldMap[key];
        return nullptr;
    };
    auto compositeShadowFor = [&](const std::string& key, bool methodContext)
        -> const std::string* {
        const bool hasMethod = compositeMethodMap.count(key) > 0;
        const bool hasField  = compositeFieldMap.count(key) > 0;
        if (hasMethod && hasField) {
            return methodContext ? &compositeMethodMap[key] : &compositeFieldMap[key];
        }
        if (hasMethod) return &compositeMethodMap[key];
        if (hasField) return &compositeFieldMap[key];
        return nullptr;
    };

    auto flushToken = [&](ca::usize delimiterIndex) {
        if (token.empty()) return;
        auto replace = [&](const std::string& with) {
            out += with;
            token.clear();
        };
        const bool methodContext = nextTokenIsDescriptor(delimiterIndex);
        if (const auto* shadow = compositeShadowFor(token, methodContext)) {
            replace(*shadow);
            return;
        }
        if (const auto* shadow = memberShadowFor(token, methodContext)) {
            replace(*shadow);
            return;
        }
        if (const auto it = classMap.find(token); it != classMap.end()) {
            replace(it->second);
            return;
        }
        // 描述符对象类型段 `L<name>;`（[ 与 ; 是分隔符，token 即 L<name>）：
        // 剥 L 前缀连带替换，未命中原样。
        if (token.size() > 1 && token[0] == 'L') {
            if (const auto it = classMap.find(token.substr(1)); it != classMap.end()) {
                replace("L" + it->second);
                return;
            }
        }
        replace(token);
    };

    for (ca::usize i = 0; i < length; ++i) {
        const char c = text[i];
        if (inQuote || inLiteral) {
            out += c;
            if (escaped) {
                escaped = false;
            }
            else if (c == '\\') {
                escaped = true;
            }
            else if ((inQuote && c == '"') || (inLiteral && c == '\'')) {
                inQuote = inLiteral = false;
            }
            continue;
        }
        if (isJasmDelimiter(c)) {
            flushToken(i);
            out += c;
            if (c == '"') inQuote = true;
            else if (c == '\'') inLiteral = true;
            continue;
        }
        token += c;
    }
    flushToken(length);
    return out;
}

}   // namespace mjbase::renamer
