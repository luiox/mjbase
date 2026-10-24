#include "edit_ledger_store.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "libca/fs/file_util.hpp"

namespace mjbase::project {
namespace {

using ca::str::Utf8String;
using ca::str::Utf8StringRef;

Utf8StringRef refOf(const char* text)
{
    return Utf8StringRef::from_cstr(text);
}

std::string toStd(const Utf8StringRef& value)
{
    return value.to_std_string();
}

Utf8String copyOf(const std::string& value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

Utf8String copyOf(std::string_view value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

Utf8String copyOf(const char* value)
{
    return copyOf(std::string_view(value));
}

ProjectStoreError makeError(ProjectStoreErrorKind kind, const std::string& message)
{
    ProjectStoreError error;
    error.kind    = kind;
    error.message = copyOf(message);
    return error;
}

/// 引擎名族 section 的代际后缀解析（"edit-ledger-12" → 12）。
std::optional<ca::u32> generationSuffix(const Utf8StringRef& name, const char* prefix)
{
    const std::string text  = toStd(name);
    const std::string head  = prefix;
    if (text.size() <= head.size() || text.compare(0, head.size(), head) != 0) return std::nullopt;
    const std::string tail = text.substr(head.size());
    if (tail.empty() || tail.size() > 10) return std::nullopt;
    for (const char c : tail) {
        if (c < '0' || c > '9') return std::nullopt;
    }
    const unsigned long long parsed = std::stoull(tail);
    if (parsed > 0xffffffffULL) return std::nullopt;
    return static_cast<ca::u32>(parsed);
}

/// 引擎名族前缀判定（overlay-<txn>-<seq> 无纯数字后缀，走前缀判定）。
bool hasEnginePrefix(const Utf8StringRef& name)
{
    const std::string text = toStd(name);
    for (const char* prefix : {kLedgerSectionPrefix, kBaselineSectionPrefix, kOverlaySectionPrefix}) {
        const std::string head = prefix;
        if (text.size() > head.size() && text.compare(0, head.size(), head) == 0) return true;
    }
    return false;
}

/// 清单条目装配（引擎自产 section；时间戳由调用方给定）。
StoreSectionEntry makeSectionEntry(const Utf8StringRef& name, SectionFormat format,
                                   const SectionWriteResult& written, ca::u64 recordCount,
                                   std::string_view createdAt)
{
    StoreSectionEntry entry;
    entry.name          = copyOf(name);
    entry.format        = format;
    entry.formatTag     = Utf8String::from_cstr(
        format == SectionFormat::Jsonl ? "jsonl" : "opaque");
    entry.sectionVersion = kLedgerRowVersion;
    entry.contentHash    = written.contentHash.clone();
    entry.byteSize       = written.byteSize;
    entry.hasRecordCount = format == SectionFormat::Jsonl;
    entry.recordCount    = recordCount;
    entry.producedBy     = Utf8String::from_cstr(kLedgerEngineName);
    entry.createdAt      = copyOf(createdAt);
    entry.updatedAt      = copyOf(createdAt);
    return entry;
}

}   // namespace

//-- 生命周期 -------------------------------------------------------------

EditLedgerStore::~EditLedgerStore() = default;

EditLedgerStore::EditLedgerStore(EditLedgerStore&& other) noexcept
    = default;

EditLedgerStore& EditLedgerStore::operator=(EditLedgerStore&& other) noexcept = default;

ca::Result<EditLedgerStore, ProjectStoreError> EditLedgerStore::createInMemory(
    const Utf8StringRef& baseContainerDigest, const LedgerEngineConfig& config)
{
    if (!isValidLedgerDigest(toStd(baseContainerDigest))) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "base container digest must be \"sha256:\" + 64 hex"));
    }
    EditLedgerStore store;
    store.config_              = config;
    store.baseContainerDigest_ = copyOf(baseContainerDigest);
    return ca::Ok(std::move(store));
}

ca::Result<EditLedgerStore, ProjectStoreError> EditLedgerStore::openBundle(
    const Utf8StringRef& bundleDir, const Utf8StringRef& baseContainerDigest,
    const LedgerEngineConfig& config)
{
    if (!isValidLedgerDigest(toStd(baseContainerDigest))) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "base container digest must be \"sha256:\" + 64 hex"));
    }
    auto locked = acquireStoreLock(bundleDir);
    if (locked.is_err()) return ca::Err(std::move(locked).unwrap_err());
    auto loaded = readStoreManifestFile(bundleDir);
    if (loaded.is_err()) return ca::Err(std::move(loaded).unwrap_err());
    auto manifest = std::move(loaded).unwrap();

    EditLedgerStore store;
    store.config_              = manifest.ledgerConfig.has_value() ? *manifest.ledgerConfig : config;
    store.baseContainerDigest_ = copyOf(baseContainerDigest);
    store.bundleDir_           = copyOf(bundleDir);
    store.lock_                = std::make_unique<StoreLock>(std::move(locked).unwrap());

    //-- 定位活动代 -----------------------------------------------------------
    std::optional<ca::u32>   active;
    const StoreSectionEntry* activeLedger = nullptr;
    for (const auto& entry : manifest.sections) {
        auto generation = generationSuffix(entry.name.ref(), kLedgerSectionPrefix);
        if (generation.has_value() && (!active.has_value() || *generation > *active)) {
            active      = generation;
            activeLedger = &entry;
        }
    }

    if (activeLedger != nullptr) {
        const ca::u32 gen = *active;
        // 活动代台账整体校验读：hash 失配/行违约 = Section 拒开（fail-closed，
        // 台账损毁不静默降级为空史——基线与工程实体不受影响）。
        auto ledgerBytesRead = readSectionBytes(bundleDir, *activeLedger);
        if (ledgerBytesRead.is_err()) return ca::Err(std::move(ledgerBytesRead).unwrap_err());
        auto rowsParsed = parseLedgerRows(std::move(ledgerBytesRead).unwrap());
        if (rowsParsed.is_err()) return ca::Err(std::move(rowsParsed).unwrap_err());
        auto rows = std::move(rowsParsed).unwrap();
        auto validated = validateLedgerChain(rows);
        if (validated.is_err()) {
            auto       chainError = std::move(validated).unwrap_err();
            ca::str::Utf8StringBuilder builder;
            builder.append("active edit-ledger is inconsistent: ").append(chainError.message.ref());
            chainError.message = builder.build_or_empty();
            return ca::Err(std::move(chainError));
        }

        std::optional<LedgerAnchor> anchor;
        for (const auto& row : rows) {
            if (row.kind == LedgerRow::Kind::Anchor) anchor = row.anchor.clone();
        }
        if (anchor.has_value()) {
            // 锚的基线 section 必须已登记（基线在后续 flush 代间保持同名引用，
            // 直到下一次压缩才换代——名字不再与本代号强绑定，完整性由清单
            // 登记 + content_hash 核验双门保证，见下方读路径）。
            const StoreSectionEntry* baselineEntry = nullptr;
            for (const auto& entry : manifest.sections) {
                if (entry.name.ref().equals(anchor->baselineSection.ref())) {
                    baselineEntry = &entry;
                }
            }
            if (baselineEntry == nullptr) {
                return ca::Err(makeError(ProjectStoreErrorKind::Section,
                                         "anchor baseline section missing from manifest: " +
                                             toStd(anchor->baselineSection.ref())));
            }
            // 一口径两处验证：基线 section 的 content_hash（readSectionBytes 已
            // 验）的 sha256 即锚 container。
            auto baseline = readSectionBytes(bundleDir, *baselineEntry);
            if (baseline.is_err()) return ca::Err(std::move(baseline).unwrap_err());
            auto baselineContent = std::move(baseline).unwrap();
            const auto digest = sectionContentHash(
                reinterpret_cast<const ca::u8*>(baselineContent.data()),
                baselineContent.byte_length());
            if (!digest.ref().equals(anchor->container.ref())) {
                return ca::Err(makeError(
                    ProjectStoreErrorKind::Section,
                    "baseline digest mismatch for '" + toStd(anchor->baselineSection.ref()) +
                        "': anchor says " + toStd(anchor->container.ref()) + ", content is " +
                        toStd(digest.ref())));
            }
            store.anchor_              = std::move(anchor);
            store.baseContainerDigest_ = store.anchor_->container.clone();
        }

        for (auto& row : rows) {
            if (row.kind != LedgerRow::Kind::Transaction) continue;
            store.txns_.push_back(std::move(row.transaction));
        }
        // 文件序即链序（txn 升序）；装载后显式定序，链不变量不依赖写方顺序。
        std::sort(store.txns_.begin(), store.txns_.end(),
                  [](const LedgerTransaction& a, const LedgerTransaction& b) {
                      return a.txn < b.txn;
                  });
        for (auto& row : rows) {
            if (row.kind == LedgerRow::Kind::Bookmark) {
                store.bookmarkOrder_.push_back(std::move(row.bookmark));
            }
        }
        store.head_ = store.txns_.empty() ? store.baseTxn() : store.txns_.back().txn;
        store.nextTxnId_ = store.head_ + 1;
        for (const auto& txn : store.txns_) {
            store.nextTxnId_ = std::max(store.nextTxnId_, txn.txn + 1);
        }
        store.generation_ = gen;
    }

    //-- 待释放清点（§3.3）：引擎名族下不被活动代引用的已登记 section ----------
    store.reclaimEngineSections(manifest);

    return ca::Ok(std::move(store));
}

void EditLedgerStore::reclaimEngineSections(const StoreManifest& manifest)
{
    std::set<std::string> live;
    for (const auto& txn : txns_) {
        for (const auto& entry : txn.entries) {
            if (!entry.payloadRef.is_empty()) live.insert(toStd(entry.payloadRef.ref()));
        }
    }
    // 活动代台账恒活（空链会话亦然——它是当前链的落盘载体，重启代只发生在
    // 下一次 flush/compact 的事务里）。
    if (generation_ != 0) {
        live.insert(toStd(ledgerSectionName(generation_)));
    }
    if (anchor_.has_value()) {
        live.insert(toStd(anchor_->baselineSection.ref()));
    }
    for (const auto& entry : manifest.sections) {
        const std::string name = toStd(entry.name.ref());
        if (!hasEnginePrefix(entry.name.ref())) continue;
        if (live.count(name) > 0) continue;
        pendingReleases_.push_back(copyOf(name));
    }
}

//-- 链查询 ---------------------------------------------------------------

ca::u64 EditLedgerStore::undoDepth() const
{
    const ca::u64 base = baseTxn();
    ca::u64       depth = 0;
    for (const auto& txn : txns_) {
        if (txn.txn > base && txn.txn <= head_) ++depth;
    }
    return depth;
}

const LedgerTransaction* EditLedgerStore::transaction(ca::u64 txn) const
{
    for (const auto& candidate : txns_) {
        if (candidate.txn == txn) return &candidate;
    }
    return nullptr;
}

std::vector<ca::u64> EditLedgerStore::chainTo(ca::u64 txn) const
{
    std::vector<ca::u64> chain;
    ca::u64              cursor = txn;
    const ca::u64        base   = baseTxn();
    while (cursor != base) {
        const auto* row = transaction(cursor);
        if (row == nullptr) break;  // 调用方契约：txn 在活动链上
        chain.push_back(cursor);
        cursor = row->parent;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

ca::u64 EditLedgerStore::lastTouchOf(std::string_view uri, ca::u64 atOrBefore) const
{
    for (auto it = txns_.rbegin(); it != txns_.rend(); ++it) {
        if (it->txn > atOrBefore) continue;
        for (const auto& entry : it->entries) {
            if (toStd(entry.uri.ref()) == uri) return it->txn;
        }
    }
    return 0;
}

ca::u64 EditLedgerStore::transactionCount() const
{
    return static_cast<ca::u64>(txns_.size());
}

ca::u64 EditLedgerStore::ledgerBytes() const
{
    std::vector<LedgerRow> rows;
    if (anchor_.has_value()) {
        LedgerRow anchorRow;
        anchorRow.kind   = LedgerRow::Kind::Anchor;
        anchorRow.anchor = anchor_->clone();
        rows.push_back(std::move(anchorRow));
    }
    for (const auto& txn : txns_) {
        LedgerRow row;
        row.kind        = LedgerRow::Kind::Transaction;
        row.transaction = txn.clone();
        rows.push_back(std::move(row));
    }
    for (const auto& bookmark : bookmarkOrder_) {
        LedgerRow row;
        row.kind     = LedgerRow::Kind::Bookmark;
        row.bookmark = bookmark.clone();
        rows.push_back(std::move(row));
    }
    auto serialized = serializeLedgerRows(rows);
    if (serialized.is_err()) return 0;
    return std::move(serialized).unwrap().byte_length();
}

ca::Result<std::vector<ca::u8>, ProjectStoreError> EditLedgerStore::payloadBytes(
    const Utf8StringRef& ref) const
{
    const auto  pending = pendingOverlays_.find(toStd(ref));
    if (pending != pendingOverlays_.end()) return ca::Ok(pending->second);
    if (!bound()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Section,
                                 "overlay payload not in memory and store is unbound: " +
                                     toStd(ref)));
    }
    auto manifest = readStoreManifestFile(bundleDir_);
    if (manifest.is_err()) return ca::Err(std::move(manifest).unwrap_err());
    auto loadedManifest = std::move(manifest).unwrap();
    for (const auto& entry : loadedManifest.sections) {
        if (!entry.name.ref().equals(ref)) continue;
        auto verified = readSectionBytes(bundleDir_, entry);
        if (verified.is_err()) return ca::Err(std::move(verified).unwrap_err());
        const auto  body = std::move(verified).unwrap();
        const auto* data = reinterpret_cast<const ca::u8*>(body.data());
        return ca::Ok(std::vector<ca::u8>(data, data + body.byte_length()));
    }
    return ca::Err(makeError(ProjectStoreErrorKind::Section,
                             "overlay payload not registered: " + toStd(ref)));
}

ca::Result<std::vector<ca::u8>, ProjectStoreError> EditLedgerStore::baselineBytes() const
{
    if (!anchor_.has_value()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Section,
                                 "no baseline section (baseline is the original container)"));
    }
    auto manifest = readStoreManifestFile(bundleDir_);
    if (manifest.is_err()) return ca::Err(std::move(manifest).unwrap_err());
    auto loadedManifest = std::move(manifest).unwrap();
    for (const auto& entry : loadedManifest.sections) {
        if (!entry.name.ref().equals(anchor_->baselineSection.ref())) continue;
        auto verified = readSectionBytes(bundleDir_, entry);
        if (verified.is_err()) return ca::Err(std::move(verified).unwrap_err());
        const auto  body = std::move(verified).unwrap();
        const auto* data = reinterpret_cast<const ca::u8*>(body.data());
        return ca::Ok(std::vector<ca::u8>(data, data + body.byte_length()));
    }
    return ca::Err(makeError(ProjectStoreErrorKind::Section,
                             "baseline section missing from manifest: " +
                                 toStd(anchor_->baselineSection.ref())));
}

//-- 变更 -----------------------------------------------------------------

void EditLedgerStore::resetSession(const Utf8StringRef& baseContainerDigest)
{
    baseContainerDigest_ = copyOf(baseContainerDigest);
    anchor_.reset();
    txns_.clear();
    head_       = 0;
    nextTxnId_  = 1;
    bookmarkOrder_.clear();
    pendingOverlays_.clear();
    pendingTxns_ = 0;
}

ca::Result<LedgerTransaction, ProjectStoreError> EditLedgerStore::append(
    const LedgerAppendInput& input)
{
    if (input.producedBy.empty() || input.createdAt.empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "append: produced_by and created_at must be non-empty"));
    }
    if (input.entries.empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "append: transaction must carry at least one entry"));
    }
    //-- 条目纪律：uri 严格字典序、digest 形状、op 相容 ------------------------
    for (ca::usize index = 0; index < input.entries.size(); ++index) {
        const auto& entry = input.entries[index];
        const std::string context = "entries[" + std::to_string(index) + "]: ";
        if (entry.uri.is_empty()) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema, context + "uri must be non-empty"));
        }
        if (index > 0 &&
            !(toStd(input.entries[index - 1].uri.ref()) < toStd(entry.uri.ref()))) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     context + "entries must be strictly uri-ascending"));
        }
        if (entry.op != LedgerEntryOp::Add && !isValidLedgerDigest(toStd(entry.before.ref()))) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     context + "before must be \"sha256:\" + 64 hex"));
        }
        if (entry.op != LedgerEntryOp::Remove &&
            !isValidLedgerDigest(toStd(entry.after.ref()))) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     context + "after must be \"sha256:\" + 64 hex"));
        }
        if (entry.op != LedgerEntryOp::Remove && entry.payloadRef.is_empty()) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     context + "payload ref is required for replace/add"));
        }
        if (entry.op == LedgerEntryOp::Remove && !entry.payloadRef.is_empty()) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     context + "remove entry must not carry payload"));
        }
    }
    //-- 载荷覆盖：引用与交付一一对应 ------------------------------------------
    std::set<std::string> referenced;
    for (const auto& entry : input.entries) {
        if (!entry.payloadRef.is_empty()) referenced.insert(toStd(entry.payloadRef.ref()));
    }
    std::map<std::string, std::vector<ca::u8>> delivered;
    for (const auto& payload : input.payloads) {
        const std::string key = toStd(payload.ref.ref());
        if (referenced.count(key) == 0) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     "append: payload not referenced by any entry: " + key));
        }
        if (!delivered.emplace(key, payload.bytes).second) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     "append: duplicate payload delivery: " + key));
        }
    }
    for (const auto& ref : referenced) {
        if (delivered.count(ref) == 0) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     "append: missing payload delivery: " + ref));
        }
    }
    //-- 链上可判校验：before == 上一次 touch 的 after -------------------------
    for (const auto& entry : input.entries) {
        const auto last = lastTouchOf(toStd(entry.uri.ref()), head_);
        if (last == 0) continue;  // 基线侧由消费方重放时以实际字节核对
        const auto* previous = transaction(last);
        for (const auto& priorEntry : previous->entries) {
            if (priorEntry.uri.ref().equals(entry.uri.ref())) {
                if (!priorEntry.after.ref().equals(entry.before.ref())) {
                    return ca::Err(makeError(
                        ProjectStoreErrorKind::Section,
                        "append: digest chain broken for '" + toStd(entry.uri.ref()) +
                            "': txn " + std::to_string(last) + " recorded after " +
                            toStd(priorEntry.after.ref()) + ", append claims before " +
                            toStd(entry.before.ref())));
                }
                break;
            }
        }
    }

    //-- 提交：放弃头之后的尾分支及其书签（对齐 commit 清 redo 栈）--------------
    ca::u64               assigned = nextTxnId_;
    std::set<std::string> droppedRefs;
    for (const auto& txn : txns_) {
        if (txn.txn > head_) {
            for (const auto& entry : txn.entries) {
                if (!entry.payloadRef.is_empty()) droppedRefs.insert(toStd(entry.payloadRef.ref()));
            }
        }
    }
    txns_.erase(std::remove_if(txns_.begin(), txns_.end(),
                               [this](const LedgerTransaction& txn) { return txn.txn > head_; }),
                txns_.end());
    bookmarkOrder_.erase(std::remove_if(bookmarkOrder_.begin(), bookmarkOrder_.end(),
                                        [this](const LedgerBookmark& bookmark) {
                                            return bookmark.txn > head_;
                                        }),
                         bookmarkOrder_.end());
    for (const auto& ref : droppedRefs) {
        auto it = pendingOverlays_.find(ref);
        if (it != pendingOverlays_.end()) pendingOverlays_.erase(it);
    }

    LedgerTransaction txn;
    txn.txn          = assigned;
    txn.parent       = head_;
    txn.label        = copyOf(input.label);
    txn.baseContainer = baseContainerDigest_.clone();
    for (const auto& entry : input.entries) {
        LedgerEntry copy;
        copy.uri        = entry.uri.clone();
        copy.before     = entry.before.clone();
        copy.after      = entry.after.clone();
        copy.op         = entry.op;
        copy.payloadRef = entry.payloadRef.clone();
        txn.entries.push_back(std::move(copy));
    }
    txn.producedBy = copyOf(input.producedBy);
    txn.createdAt  = copyOf(input.createdAt);

    for (const auto& [ref, payload] : delivered) {
        pendingOverlays_.emplace(ref, payload);
    }
    txns_.push_back(std::move(txn));
    head_      = assigned;
    nextTxnId_ = assigned + 1;
    ++pendingTxns_;
    return ca::Ok(txns_.back().clone());
}

ca::Result<void, ProjectStoreError> EditLedgerStore::moveHead(ca::u64 txn)
{
    if (txn != baseTxn() && transaction(txn) == nullptr) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "moveHead: txn " + std::to_string(txn) +
                                     " is not on the active chain"));
    }
    head_ = txn;
    return ca::Ok();
}

ca::Result<void, ProjectStoreError> EditLedgerStore::setBookmark(std::string_view name,
                                                                 std::string_view createdAt)
{
    if (name.empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema, "bookmark name must be non-empty"));
    }
    if (createdAt.empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "bookmark created_at must be non-empty"));
    }
    for (auto& existing : bookmarkOrder_) {
        if (toStd(existing.name.ref()) == name) {
            existing.txn       = head_;
            existing.createdAt = copyOf(createdAt);
            return ca::Ok();
        }
    }
    LedgerBookmark bookmark;
    bookmark.name       = copyOf(name);
    bookmark.txn        = head_;
    bookmark.createdAt  = copyOf(createdAt);
    bookmarkOrder_.push_back(std::move(bookmark));
    return ca::Ok();
}

std::vector<LedgerBookmark> EditLedgerStore::bookmarks() const
{
    std::vector<LedgerBookmark> copies;
    copies.reserve(bookmarkOrder_.size());
    for (const auto& bookmark : bookmarkOrder_) copies.push_back(bookmark.clone());
    return copies;
}

const LedgerBookmark* EditLedgerStore::bookmark(std::string_view name) const
{
    for (const auto& existing : bookmarkOrder_) {
        if (toStd(existing.name.ref()) == name) return &existing;
    }
    return nullptr;
}

bool EditLedgerStore::dropBookmark(std::string_view name)
{
    for (auto it = bookmarkOrder_.begin(); it != bookmarkOrder_.end(); ++it) {
        if (toStd(it->name.ref()) == name) {
            bookmarkOrder_.erase(it);
            return true;
        }
    }
    return false;
}

//-- 落盘 -----------------------------------------------------------------

ca::str::Utf8String EditLedgerStore::ledgerSectionName(ca::u32 generation) const
{
    return copyOf(std::string(kLedgerSectionPrefix) + std::to_string(generation));
}

ca::str::Utf8String EditLedgerStore::baselineSectionNameOf(ca::u32 generation) const
{
    return copyOf(std::string(kBaselineSectionPrefix) + std::to_string(generation));
}

ca::Result<LedgerFlushReport, ProjectStoreError> EditLedgerStore::flush(
    std::string_view createdAt)
{
    LedgerFlushReport report;
    if (!bound()) return ca::Ok(std::move(report));
    if (pendingTxns_ == 0 && pendingReleases_.empty()) return ca::Ok(std::move(report));

    const ca::u32 nextGeneration = generation_ + 1;

    //-- 1. pending overlay 原子落盘（新名，孤儿安全） --------------------------
    StoreManifestMutation mutation;
    for (const auto& [ref, payload] : pendingOverlays_) {
        auto written = writeSectionBytes(bundleDir_, copyOf(ref), SectionFormat::Opaque,
                                         payload.data(), payload.size());
        if (written.is_err()) return ca::Err(std::move(written).unwrap_err());
        auto overlayWritten = std::move(written).unwrap();
        mutation.upserts.push_back(makeSectionEntry(copyOf(ref), SectionFormat::Opaque,
                                                    overlayWritten, 0, createdAt));
        report.published.push_back(copyOf(ref));
    }

    //-- 2. 新代台账 section 全量写出（锚 + 活动链 + 书签） ---------------------
    std::vector<LedgerRow> rows;
    if (anchor_.has_value()) {
        LedgerRow anchorRow;
        anchorRow.kind   = LedgerRow::Kind::Anchor;
        anchorRow.anchor = anchor_->clone();
        rows.push_back(std::move(anchorRow));
    }
    for (const auto& txn : txns_) {
        LedgerRow row;
        row.kind        = LedgerRow::Kind::Transaction;
        row.transaction = txn.clone();
        rows.push_back(std::move(row));
    }
    for (const auto& bookmark : bookmarkOrder_) {
        LedgerRow row;
        row.kind     = LedgerRow::Kind::Bookmark;
        row.bookmark = bookmark.clone();
        rows.push_back(std::move(row));
    }
    auto ledgerText = serializeLedgerRows(rows);
    if (ledgerText.is_err()) return ca::Err(std::move(ledgerText).unwrap_err());
    const auto ledgerBody = std::move(ledgerText).unwrap();
    const auto ledgerName = ledgerSectionName(nextGeneration);
    auto ledgerWritten = writeSectionBytes(bundleDir_, ledgerName.ref(), SectionFormat::Jsonl,
                                           ledgerBody.ref());
    if (ledgerWritten.is_err()) return ca::Err(std::move(ledgerWritten).unwrap_err());
    auto ledgerPublished = std::move(ledgerWritten).unwrap();
    mutation.upserts.push_back(makeSectionEntry(ledgerName.ref(), SectionFormat::Jsonl,
                                                ledgerPublished,
                                                static_cast<ca::u64>(rows.size()), createdAt));
    report.published.push_back(ledgerName.clone());

    //-- 3. manifest 事务（upsert 新 + 删待释放；删除只指非活动代，§3.4） --------
    // 待释放名字若与本批 upsert 相交（新会话事务号自 1 重新生长，overlay 名
    // 复用仍待释放的旧名），以 upsert 重写收场、不再墓碑——同一事务又删又写
    // 是矛盾变异，清单事务会拒绝。
    std::set<std::string> upsertNames;
    for (const auto& entry : mutation.upserts) {
        upsertNames.insert(toStd(entry.name.ref()));
    }
    for (const auto& name : pendingReleases_) {
        if (upsertNames.count(toStd(name.ref())) == 0) {
            mutation.removals.push_back(name.clone());
        }
    }
    auto committed = commitStoreManifestTransaction(bundleDir_, mutation);
    if (committed.is_err()) return ca::Err(std::move(committed).unwrap_err());
    // 待释放名单已随本事务退役（墓碑删除或 upsert 重写覆盖），清账。
    for (const auto& name : pendingReleases_) report.released.push_back(name.clone());
    pendingReleases_.clear();

    //-- 4. 换代成功：旧代台账入待释放（基线不动），随即清理 --------------------
    report.flushed     = true;
    report.generation  = nextGeneration;
    report.transactions = static_cast<ca::u64>(txns_.size());
    report.ledgerBytes  = ledgerBody.byte_length();
    if (generation_ >= 1) {
        pendingReleases_.push_back(ledgerSectionName(generation_));
    }
    pendingOverlays_.clear();
    pendingTxns_  = 0;
    generation_   = nextGeneration;

    auto cleaned = cleanupSuperseded(&report.released, createdAt);
    if (cleaned.is_err()) return ca::Err(std::move(cleaned).unwrap_err());
    return ca::Ok(std::move(report));
}

//-- 压缩 -----------------------------------------------------------------

bool EditLedgerStore::shouldAutoCompact() const
{
    if (!bound() || !config_.autoCompact) return false;
    if (transactionCount() >= config_.compactMaxTransactions) return true;
    if (ledgerBytes() >= config_.compactMaxLedgerBytes) return true;
    if (undoDepth() >= config_.compactMaxDistance) return true;
    return false;
}

ca::u64 EditLedgerStore::retentionPoint(const LedgerCompactRequest& request, bool automatic,
                                        std::vector<ca::str::Utf8String>* droppedBookmarks) const
{
    std::vector<LedgerBookmark> surviving;
    surviving.reserve(bookmarkOrder_.size());
    for (const auto& bookmark : bookmarkOrder_) surviving.push_back(bookmark.clone());
    if (!automatic && request.keepRecentBookmarks < surviving.size()) {
        const ca::usize keep = static_cast<ca::usize>(request.keepRecentBookmarks);
        if (droppedBookmarks != nullptr) {
            for (ca::usize index = 0; index + keep < surviving.size(); ++index) {
                droppedBookmarks->push_back(surviving[index].name.clone());
            }
        }
        surviving.erase(surviving.begin(),
                        surviving.end() - static_cast<std::ptrdiff_t>(keep));
    }
    ca::u64 oldestBookmark = 0;
    for (const auto& bookmark : surviving) {
        if (oldestBookmark == 0 || bookmark.txn < oldestBookmark) oldestBookmark = bookmark.txn;
    }
    const ca::u64 keepDepth = automatic ? config_.minUndoDepth : request.keepUndoDepth;
    const ca::u64 newestAllowed = head_ >= keepDepth ? head_ - keepDepth : 0;
    ca::u64 upper = newestAllowed;
    if (oldestBookmark != 0) upper = std::min(upper, oldestBookmark);
    upper = std::min(upper, head_);
    return std::max(baseTxn(), upper);
}

ca::Result<LedgerCompactReport, ProjectStoreError> EditLedgerStore::compact(
    const LedgerCompactRequest& request, const std::vector<ca::u8>& baselineCanonicalBytes,
    std::string_view createdAt, bool automatic)
{
    LedgerCompactReport report;
    if (!bound()) {
        report.skippedReason = copyOf("ledger store is not bound to a bundle");
        return ca::Ok(std::move(report));
    }
    if (pendingTxns_ != 0) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "compact: flush pending transactions first (pending " +
                                     std::to_string(pendingTxns_) + ")"));
    }
    std::vector<Utf8String> droppedBookmarks;
    const ca::u64           point = retentionPoint(request, automatic, &droppedBookmarks);
    if (point <= baseTxn()) {
        report.skippedReason = automatic
                                   ? copyOf("retention point cannot advance past anchor")
                                   : copyOf("retention point " + std::to_string(point) +
                                            " does not advance past anchor " +
                                            std::to_string(baseTxn()));
        return ca::Ok(std::move(report));
    }

    //-- 1/2. 新基线 + 新代台账原子写（孤儿安全；任一步崩溃旧基线旧台账完整） ----
    const ca::u32 nextGeneration = generation_ + 1;
    const auto    baselineName   = baselineSectionNameOf(nextGeneration);
    auto baselineWritten = writeSectionBytes(
        bundleDir_, baselineName.ref(), SectionFormat::Opaque,
        baselineCanonicalBytes.data(),
        baselineCanonicalBytes.size());
    if (baselineWritten.is_err()) return ca::Err(std::move(baselineWritten).unwrap_err());
    auto baselinePublished = std::move(baselineWritten).unwrap();
    const auto baselineDigest = baselinePublished.contentHash.clone();

    ca::u64 droppedTxns = 0;
    std::vector<LedgerRow> rows;
    LedgerRow anchorRow;
    anchorRow.kind               = LedgerRow::Kind::Anchor;
    anchorRow.anchor.txn             = point;
    anchorRow.anchor.baselineSection = baselineName.clone();
    anchorRow.anchor.container       = baselineDigest.clone();
    anchorRow.anchor.previousTxn     = baseTxn();
    anchorRow.anchor.previousContainer = baseTxn() != 0 ? baseContainerDigest_.clone() : Utf8String();
    for (const auto& txn : txns_) {
        if (txn.txn <= point) ++droppedTxns;
    }
    anchorRow.anchor.compactedTxns = droppedTxns;
    anchorRow.anchor.createdAt     = copyOf(createdAt);
    rows.push_back(std::move(anchorRow));
    for (const auto& txn : txns_) {
        if (txn.txn <= point) continue;
        LedgerRow row;
        row.kind        = LedgerRow::Kind::Transaction;
        row.transaction = txn.clone();
        rows.push_back(std::move(row));
    }
    //-- 存活书签：自动路径全保留；手动路径按 keepRecentBookmarks 裁剪 ------------
    std::vector<LedgerBookmark> surviving;
    surviving.reserve(bookmarkOrder_.size());
    for (const auto& bookmark : bookmarkOrder_) surviving.push_back(bookmark.clone());
    if (!automatic && request.keepRecentBookmarks < surviving.size()) {
        const ca::usize keep = static_cast<ca::usize>(request.keepRecentBookmarks);
        surviving.erase(surviving.begin(), surviving.end() - static_cast<std::ptrdiff_t>(keep));
    }
    //-- 保留点以内的书签失效（手动丢弃列表之外的唯一失效源：state(P) = 基线） --
    for (auto it = surviving.begin(); it != surviving.end();) {
        if (it->txn < point) {
            droppedBookmarks.push_back(it->name.clone());
            it = surviving.erase(it);
        }
        else {
            ++it;
        }
    }
    for (const auto& bookmark : surviving) {
        LedgerRow row;
        row.kind     = LedgerRow::Kind::Bookmark;
        row.bookmark = bookmark.clone();
        rows.push_back(std::move(row));
    }
    auto ledgerText = serializeLedgerRows(rows);
    if (ledgerText.is_err()) return ca::Err(std::move(ledgerText).unwrap_err());
    const auto ledgerBody = std::move(ledgerText).unwrap();
    const auto ledgerName = ledgerSectionName(nextGeneration);
    auto ledgerWritten = writeSectionBytes(bundleDir_, ledgerName.ref(), SectionFormat::Jsonl,
                                           ledgerBody.ref());
    if (ledgerWritten.is_err()) return ca::Err(std::move(ledgerWritten).unwrap_err());
    auto ledgerPublished = std::move(ledgerWritten).unwrap();

    //-- 3. manifest 换代（仅 upsert 新对；旧对在提交后入待释放，§3.4） ----------
    StoreManifestMutation mutation;
    mutation.upserts.push_back(makeSectionEntry(baselineName.ref(), SectionFormat::Opaque,
                                                baselinePublished, 0, createdAt));
    mutation.upserts.push_back(makeSectionEntry(ledgerName.ref(), SectionFormat::Jsonl,
                                                ledgerPublished,
                                                static_cast<ca::u64>(rows.size()), createdAt));
    // 待释放名单随换代事务一并退役（upsert 相交的名字以重写收场，见 flush）。
    std::set<std::string> compactUpsertNames;
    for (const auto& entry : mutation.upserts) {
        compactUpsertNames.insert(toStd(entry.name.ref()));
    }
    for (const auto& name : pendingReleases_) {
        if (compactUpsertNames.count(toStd(name.ref())) == 0) {
            mutation.removals.push_back(name.clone());
        }
    }
    auto committed = commitStoreManifestTransaction(bundleDir_, mutation);
    if (committed.is_err()) return ca::Err(std::move(committed).unwrap_err());
    for (const auto& name : pendingReleases_) report.released.push_back(name.clone());
    pendingReleases_.clear();

    //-- 4. 内存换锚换代；旧台账/旧基线/失引 overlay 入待释放 -------------------
    std::set<std::string> survivingRefs;
    for (const auto& txn : txns_) {
        if (txn.txn <= point) continue;
        for (const auto& entry : txn.entries) {
            if (!entry.payloadRef.is_empty()) {
                survivingRefs.insert(toStd(entry.payloadRef.ref()));
            }
        }
    }
    for (const auto& txn : txns_) {
        if (txn.txn > point) continue;
        for (const auto& entry : txn.entries) {
            if (!entry.payloadRef.is_empty() &&
                survivingRefs.count(toStd(entry.payloadRef.ref())) == 0) {
                pendingReleases_.push_back(entry.payloadRef.clone());
            }
        }
    }
    if (generation_ >= 1) pendingReleases_.push_back(ledgerSectionName(generation_));
    if (anchor_.has_value()) {
        pendingReleases_.push_back(anchor_->baselineSection.clone());
    }

    report.executed           = true;
    report.retentionTxn       = point;
    report.droppedTransactions = droppedTxns;
    report.baselineSection    = baselineName.clone();
    report.baselineDigest     = baselineDigest.clone();
    report.droppedBookmarks   = std::move(droppedBookmarks);

    anchor_              = rows[0].anchor.clone();
    baseContainerDigest_ = baselineDigest.clone();
    txns_.erase(std::remove_if(txns_.begin(), txns_.end(),
                               [point](const LedgerTransaction& txn) { return txn.txn <= point; }),
                txns_.end());
    bookmarkOrder_ = std::move(surviving);
    generation_    = nextGeneration;

    auto cleaned = cleanupSuperseded(&report.released, createdAt);
    if (cleaned.is_err()) return ca::Err(std::move(cleaned).unwrap_err());
    return ca::Ok(std::move(report));
}

ca::Result<void, ProjectStoreError> EditLedgerStore::cleanupSuperseded(
    std::vector<Utf8String>* released, std::string_view createdAt)
{
    (void)createdAt;
    if (pendingReleases_.empty()) return ca::Ok();
    StoreManifestMutation mutation;
    for (const auto& name : pendingReleases_) mutation.removals.push_back(name.clone());
    auto committed = commitStoreManifestTransaction(bundleDir_, mutation);
    if (committed.is_err()) return ca::Err(std::move(committed).unwrap_err());
    if (released != nullptr) {
        for (const auto& name : pendingReleases_) released->push_back(name.clone());
    }
    pendingReleases_.clear();
    return ca::Ok();
}

}   // namespace mjbase::project
