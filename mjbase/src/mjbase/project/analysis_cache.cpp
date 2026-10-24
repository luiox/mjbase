#include "analysis_cache.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include "libca/crypto/sha256.hpp"
#include "libca/json/json_reader.hpp"
#include "libca/json/json_value.hpp"
#include "libca/json/json_writer.hpp"

#include "mjbase/project/edit_ledger.hpp"

namespace mjbase::project {
namespace {

using ca::str::Utf8String;
using ca::str::Utf8StringRef;

using DecompileLookupResult = std::optional<Utf8String>;

Utf8StringRef refOf(const char* text)
{
    return Utf8StringRef::from_cstr(text);
}

std::string toStd(const Utf8StringRef& value)
{
    return value.to_std_string();
}

Utf8String copyOf(std::string_view value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

Utf8String copyOf(const Utf8StringRef& value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                 value.byte_length());
}

ProjectStoreError makeError(ProjectStoreErrorKind kind, const std::string& message)
{
    ProjectStoreError error;
    error.kind    = kind;
    error.message = copyOf(message);
    return error;
}

/// 短 digest：字节序列 SHA-256 的前 8 位十六进制小写（槽名的空间紧凑化；
/// 命中判定仍做全值复核，短 digest 只作槽定位）。
std::string shortDigest(const std::string& bytes)
{
    ca::crypto::SHA256 digest;
    if (!bytes.empty()) {
        digest.add(reinterpret_cast<const ca::u8*>(bytes.data()), bytes.size());
    }
    std::string hex = digest.get_hash();
    for (char& c : hex) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return hex.substr(0, 8);
}

/// u64 小端拼入 canonical 序列。
void appendLength(std::string& body, ca::usize value)
{
    const auto wide = static_cast<ca::u64>(value);
    for (int index = 0; index < 8; ++index) {
        body.push_back(static_cast<char>((wide >> (index * 8)) & 0xffU));
    }
}

/// depends 全集的 canonical 序列（按 path 字典序；与清单写出同序纪律）。
/// SectionDep 持 move-only 串，逐条 clone 装配工作副本。
std::string canonicalDependsBody(const std::vector<SectionDep>& depends)
{
    std::vector<std::pair<std::string, std::string>> sorted;
    sorted.reserve(depends.size());
    for (const auto& dep : depends) {
        sorted.emplace_back(toStd(dep.path.ref()), toStd(dep.fingerprint.ref()));
    }
    std::sort(sorted.begin(), sorted.end());
    std::string body;
    for (const auto& [path, fingerprint] : sorted) {
        appendLength(body, path.size());
        body.append(path);
        appendLength(body, fingerprint.size());
        body.append(fingerprint);
    }
    return body;
}

/// 版本后缀（-v<n>；缓存格式版本进名字：新旧版本各占一槽互不覆写）。
std::string versionSuffix(ca::u32 sectionVersion)
{
    return "-v" + std::to_string(sectionVersion);
}

/// profile 空即未设置的清单口径（R52/CP-3）下，槽键 profile 与清单条目一致判定。
bool profileMatches(const std::optional<Utf8String>& entryProfile, const std::string& expected)
{
    if (expected.empty()) return !entryProfile.has_value();
    return entryProfile.has_value() && toStd(entryProfile->ref()) == expected;
}

/// depends 全集一致判定（逐对 path+fingerprint；序不敏感，canonical 序比对）。
bool dependsMatch(const std::vector<SectionDep>& entryDeps,
                  const std::vector<SectionDep>& expected)
{
    if (entryDeps.size() != expected.size()) return false;
    std::vector<std::pair<std::string, std::string>> left;
    std::vector<std::pair<std::string, std::string>> right;
    left.reserve(entryDeps.size());
    right.reserve(expected.size());
    for (const auto& dep : entryDeps) {
        left.emplace_back(toStd(dep.path.ref()), toStd(dep.fingerprint.ref()));
    }
    for (const auto& dep : expected) {
        right.emplace_back(toStd(dep.path.ref()), toStd(dep.fingerprint.ref()));
    }
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

/// xref 缓存键的清单条目全值复核（名字槽定位之后的防撞门）。
bool xrefEntryMatches(const StoreSectionEntry& entry, const XrefCacheKey& key)
{
    if (entry.sectionVersion != key.sectionVersion) return false;
    if (toStd(entry.producedBy.ref()) != toStd(key.producer.ref())) return false;
    if (!profileMatches(entry.profile, toStd(key.profile.ref()))) return false;
    return dependsMatch(entry.depends, key.depends);
}

/// decompile 缓存槽键的清单条目全值复核。
bool decompileEntryMatches(const StoreSectionEntry& entry, const DecompileCacheSlot& slot)
{
    if (entry.sectionVersion != slot.sectionVersion) return false;
    if (toStd(entry.producedBy.ref()) != toStd(slot.producer.ref())) return false;
    if (!profileMatches(entry.profile, toStd(slot.profile.ref()))) return false;
    if (entry.depends.size() != 1) return false;
    return toStd(entry.depends[0].path.ref()) == toStd(slot.containerPath.ref()) &&
           toStd(entry.depends[0].fingerprint.ref()) == toStd(slot.containerDigest.ref());
}

/// 槽键深拷贝（Utf8String move-only，逐字段 clone）。
DecompileCacheSlot cloneSlot(const DecompileCacheSlot& slot)
{
    DecompileCacheSlot copy;
    copy.containerDigest = slot.containerDigest.clone();
    copy.containerPath   = slot.containerPath.clone();
    copy.producer        = slot.producer.clone();
    copy.profile         = slot.profile.clone();
    copy.sectionVersion  = slot.sectionVersion;
    return copy;
}

/// 由槽键装配清单条目（时间戳由调用方给定；depends = 单容器登记对）。
StoreSectionEntry makeDecompileEntry(const DecompileCacheSlot& slot, const std::string& name,
                                     const SectionWriteResult& written, ca::u64 recordCount,
                                     std::string_view createdAt)
{
    StoreSectionEntry entry;
    entry.name           = copyOf(name);
    entry.format         = SectionFormat::Jsonl;
    entry.formatTag      = Utf8String::from_cstr("jsonl");
    entry.sectionVersion = slot.sectionVersion;
    entry.contentHash    = written.contentHash.clone();
    entry.byteSize       = written.byteSize;
    entry.hasRecordCount = true;
    entry.recordCount    = recordCount;
    if (!slot.profile.is_empty()) entry.profile = slot.profile.clone();
    SectionDep dep;
    dep.path        = slot.containerPath.clone();
    dep.fingerprint = slot.containerDigest.clone();
    entry.depends.push_back(std::move(dep));
    entry.producedBy = slot.producer.clone();
    entry.createdAt  = copyOf(createdAt);
    entry.updatedAt  = copyOf(createdAt);
    return entry;
}

/// 严格解析一条 decompile 缓存行（字段集 {v, class, entry, source}，未知/
/// 重复字段、类型错、digest 形状错即拒）。
ca::Result<DecompileCacheRecord, ProjectStoreError> parseDecompileRow(std::string_view line,
                                                                      ca::usize lineNumber)
{
    const auto fail = [lineNumber](const std::string& message) {
        return ca::Err(makeError(ProjectStoreErrorKind::Section,
                                 "decompile cache line " + std::to_string(lineNumber) + ": " +
                                     message));
    };
    auto document = ca::json::JsonReader::read(Utf8StringRef::from_string_view(line));
    if (document.is_err()) return fail("not valid JSON");
    auto        parsedDocument = std::move(document).unwrap();
    const auto& root           = parsedDocument.root();
    if (!root.is_object()) return fail("row must be a JSON object");

    const ca::json::JsonValue* version     = nullptr;
    const ca::json::JsonValue* className   = nullptr;
    const ca::json::JsonValue* entryDigest = nullptr;
    const ca::json::JsonValue* source      = nullptr;
    for (const auto& [key, member] : root.as_object()) {
        const ca::json::JsonValue** slot  = nullptr;
        const char*                 known = nullptr;
        if (key.equals(refOf("v"))) {
            slot  = &version;
            known = "v";
        }
        else if (key.equals(refOf("class"))) {
            slot  = &className;
            known = "class";
        }
        else if (key.equals(refOf("entry"))) {
            slot  = &entryDigest;
            known = "entry";
        }
        else if (key.equals(refOf("source"))) {
            slot  = &source;
            known = "source";
        }
        else {
            return fail("unknown field: " + toStd(key));
        }
        if (*slot != nullptr) return fail(std::string("duplicate field: ") + known);
        *slot = &member;
    }
    if (version == nullptr || !version->is_int()) return fail("missing row version v");
    if (version->as_int() != static_cast<ca::i64>(kDecompileCacheRowVersion)) {
        return fail("unsupported row version " + std::to_string(version->as_int()));
    }
    DecompileCacheRecord record;
    if (className == nullptr || !className->is_string() || className->as_string().is_empty()) {
        return fail("class must be a non-empty string");
    }
    record.className = copyOf(toStd(className->as_string()));
    if (entryDigest == nullptr || !entryDigest->is_string()) {
        return fail("missing entry digest");
    }
    const std::string digestText = toStd(entryDigest->as_string());
    if (!isValidLedgerDigest(digestText)) return fail("entry must be \"sha256:\" + 64 hex");
    record.entryDigest = copyOf(digestText);
    if (source == nullptr || !source->is_string()) return fail("source must be a string");
    record.source = copyOf(toStd(source->as_string()));
    return ca::Ok(std::move(record));
}

/// 确定性序列化记录集（按类名字典序；每行尾 LF）。SectionDep/记录持
/// move-only 串，按值接手并原位排序。
ca::Result<Utf8String, ProjectStoreError> serializeDecompileRows(
    std::vector<DecompileCacheRecord> records)
{
    std::sort(records.begin(), records.end(),
              [](const DecompileCacheRecord& a, const DecompileCacheRecord& b) {
                  return toStd(a.className.ref()) < toStd(b.className.ref());
              });
    std::string body;
    for (const auto& record : records) {
        ca::json::JsonValue::ObjectStorage members;
        members.emplace_back(refOf("v"), ca::json::JsonValue::make_int(
                                              static_cast<ca::i64>(kDecompileCacheRowVersion)));
        members.emplace_back(refOf("class"),
                             ca::json::JsonValue::make_string(record.className.ref()));
        members.emplace_back(refOf("entry"),
                             ca::json::JsonValue::make_string(record.entryDigest.ref()));
        members.emplace_back(refOf("source"),
                             ca::json::JsonValue::make_string(record.source.ref()));
        ca::json::JsonDocument document;
        document.root() = ca::json::JsonValue::make_object_from(std::move(members));
        ca::json::JsonWriterOptions options;
        options.pretty = false;
        auto line      = ca::json::JsonWriter::write(document, options);
        body.append(reinterpret_cast<const char*>(line.data()), line.byte_length());
        body.push_back('\n');
    }
    return ca::Ok(copyOf(body));
}

/// 解析整段 decompile 缓存 jsonl（空行跳过；任一行违约 = 段不可解释）。
ca::Result<std::vector<DecompileCacheRecord>, ProjectStoreError> parseDecompileRows(
    const std::string& body)
{
    std::vector<DecompileCacheRecord> records;
    ca::usize                         lineNumber = 0;
    ca::usize                         start      = 0;
    while (start <= body.size()) {
        ca::usize end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        ++lineNumber;
        if (end > start) {
            auto parsed = parseDecompileRow(body.substr(start, end - start), lineNumber);
            if (parsed.is_err()) return ca::Err(std::move(parsed).unwrap_err());
            records.push_back(std::move(parsed).unwrap());
        }
        if (end == body.size()) break;
        start = end + 1;
    }
    return ca::Ok(std::move(records));
}

/// 读已登记 decompile 槽的全部记录；Damaged/不可解释 = 空集（缓存自愈：
/// 未命中重算 + 同名重写，§8.2-3 同名恢复口径）。
std::vector<DecompileCacheRecord> readExistingRecords(const Utf8StringRef&      bundleDir,
                                                      const StoreSectionEntry& entry)
{
    auto bytes = readSectionBytes(bundleDir, entry);
    if (bytes.is_err()) return {};
    const auto  content = std::move(bytes).unwrap();
    const auto  body    = toStd(content.ref());
    auto        parsed  = parseDecompileRows(body);
    if (parsed.is_err()) return {};
    return std::move(parsed).unwrap();
}

/// 前缀命中判定（名字族词根；std::string 面）。
bool hasPrefix(const StoreSectionEntry& entry, const std::string& head)
{
    const std::string name = toStd(entry.name.ref());
    return name.size() > head.size() && name.compare(0, head.size(), head) == 0;
}

}   // namespace

//-- section 名推导 ---------------------------------------------------------

ca::str::Utf8String xrefCacheSectionName(const std::vector<SectionDep>& depends,
                                         ca::u32                        sectionVersion,
                                         std::string_view               profile)
{
    const std::string setDigest     = shortDigest(canonicalDependsBody(depends));
    const std::string profileDigest = shortDigest(std::string(profile));
    return copyOf(std::string(kXrefCacheSectionPrefix) + setDigest + "-" + profileDigest +
                  versionSuffix(sectionVersion));
}

ca::str::Utf8String decompileCacheSectionName(const DecompileCacheSlot& slot)
{
    const std::string digestText = toStd(slot.containerDigest.ref());
    if (!isValidLedgerDigest(digestText)) return Utf8String();
    const std::string containerHex  = digestText.substr(7);
    const std::string engineDigest  = shortDigest(toStd(slot.producer.ref()));
    const std::string profileDigest = shortDigest(toStd(slot.profile.ref()));
    return copyOf(std::string(kDecompileCacheSectionPrefix) + containerHex.substr(0, 8) + "-" +
                  engineDigest + "-" + profileDigest + versionSuffix(slot.sectionVersion));
}

//-- 生命周期 ---------------------------------------------------------------

ca::Result<AnalysisCacheStore, ProjectStoreError> AnalysisCacheStore::open(
    const Utf8StringRef& bundleDir)
{
    auto loaded = readStoreManifestFile(bundleDir);
    if (loaded.is_err()) return ca::Err(std::move(loaded).unwrap_err());
    AnalysisCacheStore store;
    store.bundleDir_ = copyOf(bundleDir);
    return ca::Ok(std::move(store));
}

ca::Result<StoreManifest, ProjectStoreError> AnalysisCacheStore::loadManifest() const
{
    return readStoreManifestFile(bundleDir_);
}

//-- xref 缓存 ---------------------------------------------------------------

ca::Result<std::optional<std::vector<ca::u8>>, ProjectStoreError> AnalysisCacheStore::lookupXref(
    const XrefCacheKey& key) const
{
    if (!bound()) return ca::Ok(std::optional<std::vector<ca::u8>>{});
    auto loaded = loadManifest();
    if (loaded.is_err()) return ca::Err(std::move(loaded).unwrap_err());
    auto manifest   = std::move(loaded).unwrap();
    const auto name = xrefCacheSectionName(key.depends, key.sectionVersion,
                                           toStd(key.profile.ref()));
    const std::string nameKey = toStd(name.ref());
    for (const auto& entry : manifest.sections) {
        if (toStd(entry.name.ref()) != nameKey) continue;
        if (!xrefEntryMatches(entry, key)) return ca::Ok(std::optional<std::vector<ca::u8>>{});
        // 内容层失配（Damaged）按未命中——缓存可重算，不为它报错；下次同名
        // 重写即恢复（§8.2-3）。
        auto bytes = readSectionBytes(bundleDir_, entry);
        if (bytes.is_err()) return ca::Ok(std::optional<std::vector<ca::u8>>{});
        auto        content = std::move(bytes).unwrap();
        const auto* data    = reinterpret_cast<const ca::u8*>(content.data());
        return ca::Ok(
            std::make_optional(std::vector<ca::u8>(data, data + content.byte_length())));
    }
    return ca::Ok(std::optional<std::vector<ca::u8>>{});
}

ca::Result<void, ProjectStoreError> AnalysisCacheStore::storeXref(const XrefCacheKey& key,
                                                                  const ca::u8* payload,
                                                                  ca::usize     payloadSize,
                                                                  std::string_view createdAt)
{
    if (!bound()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Open, "cache store is not bound"));
    }
    if (key.producer.is_empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "xref cache producer must be non-empty"));
    }
    const auto name    = xrefCacheSectionName(key.depends, key.sectionVersion,
                                              toStd(key.profile.ref()));
    auto       nameValid = validateSectionName(name.ref());
    if (nameValid.is_err()) return ca::Err(std::move(nameValid).unwrap_err());
    auto written =
        writeSectionBytes(bundleDir_, name.ref(), SectionFormat::Opaque, payload, payloadSize);
    if (written.is_err()) return ca::Err(std::move(written).unwrap_err());
    const auto writeResult = std::move(written).unwrap();

    auto loaded = loadManifest();
    if (loaded.is_err()) return ca::Err(std::move(loaded).unwrap_err());
    auto manifest = std::move(loaded).unwrap();

    StoreSectionEntry entry;
    entry.name           = copyOf(name);
    entry.format         = SectionFormat::Opaque;
    entry.formatTag      = Utf8String::from_cstr("opaque");
    entry.sectionVersion = key.sectionVersion;
    entry.contentHash    = writeResult.contentHash.clone();
    entry.byteSize       = writeResult.byteSize;
    if (!key.profile.is_empty()) entry.profile = key.profile.clone();
    for (const auto& dep : key.depends) {
        SectionDep cloned;
        cloned.path        = dep.path.clone();
        cloned.fingerprint = dep.fingerprint.clone();
        entry.depends.push_back(std::move(cloned));
    }
    entry.producedBy = key.producer.clone();
    entry.createdAt  = copyOf(createdAt);
    entry.updatedAt  = copyOf(createdAt);

    StoreManifestMutation mutation;
    mutation.upserts.push_back(std::move(entry));
    // 同引擎陈旧槽退役：xref- 词根 + 同 produced_by 下非当前名的已登记槽
    // （容器集/profile/版本换槽后的旧槽不可再命中，留作死重；异引擎槽互不
    // 干涉）。缓存段删除不伤事实源。
    const std::string producerKey = toStd(key.producer.ref());
    const std::string nameKey     = toStd(name.ref());
    for (const auto& existing : manifest.sections) {
        if (!hasPrefix(existing, kXrefCacheSectionPrefix)) continue;
        if (toStd(existing.name.ref()) == nameKey) continue;
        if (toStd(existing.producedBy.ref()) != producerKey) continue;
        mutation.removals.push_back(existing.name.clone());
    }
    auto committed = commitStoreManifestTransaction(bundleDir_, mutation);
    if (committed.is_err()) return ca::Err(std::move(committed).unwrap_err());
    return ca::Ok();
}

ca::Result<void, ProjectStoreError> AnalysisCacheStore::storeXref(
    const XrefCacheKey& key, const Utf8StringRef& payload, std::string_view createdAt)
{
    return storeXref(key, reinterpret_cast<const ca::u8*>(payload.data()), payload.byte_length(),
                     createdAt);
}

//-- 条目级 decompile 缓存 ----------------------------------------------------

ca::Result<std::optional<Utf8String>, ProjectStoreError> AnalysisCacheStore::lookupDecompile(
    const DecompileCacheSlot& slot, std::string_view className,
    const Utf8StringRef& entryDigest) const
{
    if (!bound()) return ca::Ok(DecompileLookupResult{});
    if (!isValidLedgerDigest(toStd(entryDigest))) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "entry digest must be \"sha256:\" + 64 hex"));
    }
    auto loaded = loadManifest();
    if (loaded.is_err()) return ca::Err(std::move(loaded).unwrap_err());
    auto manifest   = std::move(loaded).unwrap();
    const auto name = decompileCacheSectionName(slot);
    if (name.is_empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "slot container digest must be \"sha256:\" + 64 hex"));
    }
    const std::string nameKey = toStd(name.ref());
    for (const auto& entry : manifest.sections) {
        if (toStd(entry.name.ref()) != nameKey) continue;
        if (!decompileEntryMatches(entry, slot)) return ca::Ok(DecompileLookupResult{});
        for (const auto& record : readExistingRecords(bundleDir_, entry)) {
            if (toStd(record.className.ref()) != className) continue;
            // 条目 digest 不同 = 类已编辑（条目级失效粒度的判定点）。
            if (toStd(record.entryDigest.ref()) != toStd(entryDigest)) {
                return ca::Ok(DecompileLookupResult{});
            }
            return ca::Ok(std::make_optional(record.source.clone()));
        }
        return ca::Ok(DecompileLookupResult{});
    }
    return ca::Ok(DecompileLookupResult{});
}

ca::Result<void, ProjectStoreError> AnalysisCacheStore::putDecompile(
    const DecompileCacheSlot& slot, std::string_view className, const Utf8StringRef& entryDigest,
    std::string_view source)
{
    const auto name = decompileCacheSectionName(slot);
    if (name.is_empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "slot container digest must be \"sha256:\" + 64 hex"));
    }
    if (slot.producer.is_empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "decompile cache producer must be non-empty"));
    }
    if (className.empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema, "class name must be non-empty"));
    }
    if (!isValidLedgerDigest(toStd(entryDigest))) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "entry digest must be \"sha256:\" + 64 hex"));
    }
    const std::string nameKey = toStd(name.ref());
    auto              slotIt  = pending_.find(nameKey);
    if (slotIt == pending_.end()) {
        PendingSlot group;
        group.slot = cloneSlot(slot);
        slotIt     = pending_.emplace(nameKey, std::move(group)).first;
    }
    PendingRecord record;
    record.entryDigest = copyOf(entryDigest);
    record.source      = copyOf(source);
    auto inserted      = slotIt->second.records.insert_or_assign(std::string(className),
                                                                 std::move(record));
    if (inserted.second) ++pendingCount_;
    return ca::Ok();
}

ca::Result<AnalysisCacheFlushReport, ProjectStoreError> AnalysisCacheStore::flush(
    std::string_view createdAt)
{
    AnalysisCacheFlushReport report;
    if (!bound() || pending_.empty()) return ca::Ok(std::move(report));

    auto loaded = loadManifest();
    if (loaded.is_err()) return ca::Err(std::move(loaded).unwrap_err());
    auto manifest = std::move(loaded).unwrap();

    //-- 1. 按槽分组落盘：pending 并入已登记槽的旧记录，全部经 writeSectionBytes
    //      原子写（孤儿安全）；槽元数据 = 槽键四字段。 ------------------------
    StoreManifestMutation mutation;
    std::set<std::string> upsertNames;
    for (auto& [nameKey, group] : pending_) {
        const auto name = decompileCacheSectionName(group.slot);
        auto       nameValid = validateSectionName(name.ref());
        if (nameValid.is_err()) return ca::Err(std::move(nameValid).unwrap_err());

        //-- 合并面：已登记槽旧记录先入，pending 后入移动覆盖（pending 同名
        //     覆盖旧记录；map 无拷贝——PendingRecord 持 move-only 串）。--------
        std::map<std::string, PendingRecord> merged;
        for (const auto& existing : manifest.sections) {
            if (toStd(existing.name.ref()) != nameKey) continue;
            for (auto&& record : readExistingRecords(bundleDir_, existing)) {
                const std::string className = toStd(record.className.ref());
                if (group.records.count(className) != 0) continue;
                PendingRecord carried;
                carried.entryDigest = record.entryDigest.clone();
                carried.source      = record.source.clone();
                merged.emplace(std::move(className), std::move(carried));
            }
            break;
        }
        for (auto& [className, record] : group.records) {
            merged.insert_or_assign(className, std::move(record));
        }

        std::vector<DecompileCacheRecord> records;
        records.reserve(merged.size());
        for (auto& [className, record] : merged) {
            DecompileCacheRecord row;
            row.className   = copyOf(className);
            row.entryDigest = record.entryDigest.clone();
            row.source      = record.source.clone();
            records.push_back(std::move(row));
        }
        const auto recordCount = static_cast<ca::u64>(records.size());
        auto serialized = serializeDecompileRows(std::move(records));
        if (serialized.is_err()) return ca::Err(std::move(serialized).unwrap_err());
        auto body     = std::move(serialized).unwrap();
        auto written  = writeSectionBytes(bundleDir_, name.ref(), SectionFormat::Jsonl,
                                          reinterpret_cast<const ca::u8*>(body.data()),
                                          body.byte_length());
        if (written.is_err()) return ca::Err(std::move(written).unwrap_err());
        const auto writeResult = std::move(written).unwrap();
        mutation.upserts.push_back(makeDecompileEntry(group.slot, nameKey, writeResult,
                                                      recordCount, createdAt));
        upsertNames.insert(nameKey);
        report.published.push_back(name.clone());
        report.records += recordCount;

        //-- 同族陈旧槽：同容器 + 同引擎（decompile-<cd8>-<e8>- 词根）下非当前
        //    名的已登记槽（profile/版本换槽后的旧槽）。跨容器/跨引擎的槽各归
        //    各的词根，不互删。 --------------------------------------------
        const std::string family      = nameKey;
        const std::size_t versionMark = family.rfind("-v");
        const std::size_t profileDash = versionMark == std::string::npos
                                            ? std::string::npos
                                            : family.rfind('-', versionMark - 1);
        const std::string stem        = profileDash == std::string::npos
                                            ? family
                                            : family.substr(0, profileDash + 1);
        for (const auto& existing : manifest.sections) {
            const std::string existingName = toStd(existing.name.ref());
            if (existingName.compare(0, stem.size(), stem) != 0) continue;
            if (existingName.compare(0, family.size(), family) == 0) continue;
            mutation.removals.push_back(existing.name.clone());
        }
    }

    //-- 2. 一笔 manifest 事务提交全部槽（manifest last；RFC 风险 6 的合并
    //      flush——N 槽/记录只付一次清单事务）。 ------------------------------
    auto committed = commitStoreManifestTransaction(bundleDir_, mutation);
    if (committed.is_err()) return ca::Err(std::move(committed).unwrap_err());
    report.released = std::move(mutation.removals);
    pending_.clear();
    pendingCount_ = 0;
    report.flushed = true;
    return ca::Ok(std::move(report));
}

}   // namespace mjbase::project
