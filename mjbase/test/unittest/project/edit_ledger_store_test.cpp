// 编辑台账引擎（schema v2 交付 2 的落盘面）：纯内存链 + bundle 绑定装载、
// flush 的 manifest last 协议与重开等价、崩溃注入三态（staged 残留 / Damaged
// 台账拒开 / 孤儿档两路：清单外孤儿 prune、已登记旧代清理）、压缩不变量
// （压缩前后重放一致、书签零失效、补链、手动显式丢弃列名、阈值边界、压缩
// 中途崩溃可恢复）。
//
// 设计定稿见 doc_ai/analysis-project-schema-v2.md §3/§5（上游 RFC 0001 §6.2/
// §6.4）；崩溃语义对齐 project_store 既有恢复面（§8.1/§8.2）。
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "mjbase/project/edit_ledger_store.hpp"
#include "libca/fs/file_util.hpp"
#include "test_helper.hpp"

using namespace mjbase::project;
using ca::str::Utf8String;
using ca::str::Utf8StringRef;

namespace {

Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

std::string toStd(const Utf8StringRef& value)
{
    return value.to_std_string();
}

/// 清掉上一次 run 的同名临时 bundle，返回干净路径。
std::filesystem::path freshTempDir(const std::string& name)
{
    const auto  path = test::getTempOutPath(name);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path;
}

using Bytes = std::vector<ca::u8>;

Bytes bytesOf(const std::string& text)
{
    return Bytes(reinterpret_cast<const ca::u8*>(text.data()),
                 reinterpret_cast<const ca::u8*>(text.data()) + text.size());
}

std::string dig(const Bytes& payload)
{
    return ledgerDigestOfBytes(payload.data(), payload.size()).to_std_string();
}

using ContainerMap = std::map<std::string, Bytes>;

//-- 容器状态物料化（消费方重放职责的最小化身）------------------------------

ContainerMap canonicalToMap(const Bytes& canonicalBytes)
{
    ContainerMap state;
    auto walked  = forEachContainerEntry(canonicalBytes.data(), canonicalBytes.size(),
        [&](std::string_view uri, const ca::u8* payload, ca::usize payloadSize) {
            state.emplace(std::string(uri),
                          Bytes(payload, payload + payloadSize));
            return true;
        });
    EXPECT_FALSE(walked.is_err());
    return state;
}

Bytes mapToCanonical(const ContainerMap& state)
{
    std::vector<std::pair<Utf8String, Bytes>> sorted;
    for (const auto& [uri, payload] : state) {
        sorted.emplace_back(Utf8String::from_data(reinterpret_cast<const ca::u8*>(uri.data()),
                                                  uri.size()),
                            payload);
    }
    return serializeContainerEntries(sorted);
}

std::string digestOfMap(const ContainerMap& state)
{
    const auto  canonical = mapToCanonical(state);
    return sectionContentHash(canonical.data(), canonical.size()).to_std_string();
}

Utf8StringRef refOfBytes(const Bytes& payload)
{
    return Utf8StringRef::from_data(payload.data(), payload.size());
}

/// 压缩过引擎的基线态（锚 container 所指 canonical 字节的重读物化）。
ContainerMap baselineStateOf(const EditLedgerStore& store)
{
    auto baselineBytesRead = store.baselineBytes();
    EXPECT_FALSE(baselineBytesRead.is_err());
    return baselineBytesRead.is_err()
               ? ContainerMap()
               : canonicalToMap(std::move(baselineBytesRead).unwrap());
}


/// 打开失败即失败并打印错误消息（定位用）。
EditLedgerStore expectOpenOk(ca::Result<EditLedgerStore, ProjectStoreError> opened,
                             const char* what)
{
    if (opened.is_err()) {
        const auto  err = std::move(opened).unwrap_err();
        ADD_FAILURE() << what << ": kind=" << static_cast<int>(err.kind) << " "
                      << toStd(err.message);
        return EditLedgerStore();
    }
    return std::move(opened).unwrap();
}

/// 单事务重放：Remove 摘除，Add/Replace 以引擎载荷交付覆盖。
void applyTransaction(ContainerMap& state, const LedgerTransaction& txn,
                      const EditLedgerStore& store)
{
    for (const auto& entry : txn.entries) {
        const std::string uri = toStd(entry.uri.ref());
        if (entry.op == LedgerEntryOp::Remove) {
            state.erase(uri);
            continue;
        }
        auto payload = store.payloadBytes(entry.payloadRef.ref());
        EXPECT_FALSE(payload.is_err()) << uri;
        if (payload.is_ok()) state[uri] = std::move(payload).unwrap();
    }
}

ContainerMap replayTo(const EditLedgerStore& store, ca::u64 txn, const ContainerMap& baseline)
{
    ContainerMap state = baseline;
    for (const auto id : store.chainTo(txn)) {
        const auto* row = store.transaction(id);
        EXPECT_NE(nullptr, row);
        if (row != nullptr) applyTransaction(state, *row, store);
    }
    return state;
}

//-- 事务装配（digest 链由调用方从实际字节算出——workspace commit 的同款职责）--

struct EntryDraft
{
    std::string   uri;
    bool          remove  = false;
    bool          added   = false;  ///< true = Add（无 before）
    Bytes         before;
    Bytes         after;
};

LedgerAppendInput makeAppendInput(ca::u64 txnId, const std::vector<EntryDraft>& drafts,
                                  const std::string& label = "test edit")
{
    LedgerAppendInput input;
    input.label      = label;
    input.producedBy = "mjbase-unittest/1";
    input.createdAt  = "2026-10-23T00:00:00Z";
    ca::u32 seq      = 0;
    for (const auto& draft : drafts) {
        LedgerEntry entry;
        entry.uri = copyLedgerText(draft.uri);
        if (draft.remove) {
            entry.op     = LedgerEntryOp::Remove;
            entry.before = copyLedgerText(dig(draft.before));
        }
        else {
            entry.op = draft.added ? LedgerEntryOp::Add : LedgerEntryOp::Replace;
            if (!draft.added) entry.before = copyLedgerText(dig(draft.before));
            entry.after      = copyLedgerText(dig(draft.after));
            entry.payloadRef = overlaySectionName(txnId, seq++);
            LedgerOverlayPayload payload;
            payload.ref   = entry.payloadRef.clone();
            payload.bytes = draft.after;
            input.payloads.push_back(std::move(payload));
        }
        input.entries.push_back(std::move(entry));
    }
    return input;
}

/// 场景基线：{a.class: A0, b.class: B0}；返回其 canonical 字节与 digest。
struct BaselineFixture
{
    ContainerMap state;
    Bytes        canonical;
    std::string  digest;
};

BaselineFixture makeBaseline()
{
    BaselineFixture fixture;
    fixture.state["a.class"] = bytesOf("A0");
    fixture.state["b.class"] = bytesOf("B0");
    fixture.canonical        = mapToCanonical(fixture.state);
    fixture.digest           = digestOfMap(fixture.state);
    return fixture;
}

}   // namespace

//-- 纯内存链 ---------------------------------------------------------------

TEST(MjLedgerStoreTest, inMemoryChainAppendsUndoesAndAbandonsBranches)
{
    const auto  baseline = makeBaseline();
    auto created = EditLedgerStore::createInMemory(refOf(baseline.digest));
    ASSERT_FALSE(created.is_err());
    auto store = std::move(created).unwrap();

    EXPECT_EQ(0U, store.headTxn());
    EXPECT_FALSE(store.canUndo());
    EXPECT_EQ(0U, store.undoDepth());

    // t1：replace a.class（A0→A1）。
    auto t1 = store.append(makeAppendInput(store.nextTxnId(),
                                           {{"a.class", false, false, bytesOf("A0"), bytesOf("A1")}}));
    ASSERT_FALSE(t1.is_err());
    EXPECT_EQ(1U, std::move(t1).unwrap().txn);
    EXPECT_EQ(1U, store.headTxn());
    EXPECT_TRUE(store.canUndo());
    EXPECT_EQ(1U, store.undoDepth());
    EXPECT_EQ(1U, store.chainTo(1).size());

    // t2：add c.class。
    ASSERT_FALSE(
        store.append(makeAppendInput(store.nextTxnId(),
                                     {{"c.class", false, true, {}, bytesOf("C1")}}))
            .is_err());
    EXPECT_EQ(2U, store.undoDepth());
    EXPECT_EQ(2U, store.headTxn());
    ASSERT_NE(nullptr, store.transaction(2));
    EXPECT_EQ(1U, store.transaction(2)->parent);
    EXPECT_EQ(1U, store.lastTouchOf("a.class", 5));

    // 链上 digest 校验：t1 之后 a.class 的 before 与 t1.after 失配 → Section 拒绝。
    auto broken = store.append(makeAppendInput(store.nextTxnId(),
                                               {{"a.class", false, false, bytesOf("nope"),
                                                 bytesOf("A2")}}));
    ASSERT_TRUE(broken.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Section, std::move(broken).unwrap_err().kind);

    // 书签 + 回退 + 尾分支放弃（对齐 commit 清 redo 栈；事务号不复用）。
    ASSERT_FALSE(store.setBookmark("sp", "2026-10-23T00:00:01Z").is_err());
    ASSERT_FALSE(store.moveHead(1).is_err());
    ASSERT_NE(nullptr, store.bookmark("sp"));
    auto t3 = store.append(makeAppendInput(store.nextTxnId(),
                                           {{"a.class", false, false, bytesOf("A1"), bytesOf("A2")}}));
    ASSERT_FALSE(t3.is_err());
    EXPECT_EQ(3U, std::move(t3).unwrap().txn);
    EXPECT_EQ(nullptr, store.transaction(2));          // 被放弃分支行已删
    EXPECT_EQ(nullptr, store.bookmark("sp"));          // 分支书签随之失效
    EXPECT_EQ(4U, store.nextTxnId());                  // 事务号不复用（3 已发给 t3）
    EXPECT_EQ(2U, store.undoDepth());

    // moveHead 校验：非活动链事务号拒绝。
    auto stray = store.moveHead(2);
    ASSERT_TRUE(stray.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Schema, std::move(stray).unwrap_err().kind);
}

TEST(MjLedgerStoreTest, appendRejectsDisciplineViolations)
{
    const auto  baseline = makeBaseline();
    auto store =
        std::move(EditLedgerStore::createInMemory(refOf(baseline.digest)).unwrap());

    // 空 entries。
    auto empty = store.append(makeAppendInput(1, {}));
    ASSERT_TRUE(empty.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Schema, std::move(empty).unwrap_err().kind);

    // uri 非严格字典序（b 先于 a）。
    auto bDraft = makeAppendInput(1, {{"b.class", false, true, {}, bytesOf("B1")}});
    auto aDraft = makeAppendInput(1, {{"a.class", false, true, {}, bytesOf("A1")}});
    LedgerAppendInput unsorted;
    unsorted.label      = bDraft.label;
    unsorted.producedBy = bDraft.producedBy;
    unsorted.createdAt  = bDraft.createdAt;
    unsorted.entries.push_back(bDraft.entries[0].clone());
    unsorted.entries.push_back(aDraft.entries[0].clone());
    for (const auto& payload : bDraft.payloads) {
        LedgerOverlayPayload delivered;
        delivered.ref   = payload.ref.clone();
        delivered.bytes = payload.bytes;
        unsorted.payloads.push_back(std::move(delivered));
    }
    unsorted.payloads.push_back(std::move(aDraft.payloads[0]));
    auto disorder = store.append(unsorted);
    ASSERT_TRUE(disorder.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Schema, std::move(disorder).unwrap_err().kind);

    // 载荷缺交付。
    LedgerAppendInput missing      = makeAppendInput(1, {{"a.class", false, true, {}, bytesOf("A1")}});
    missing.payloads.clear();
    auto undelivered = store.append(missing);
    ASSERT_TRUE(undelivered.is_err());
    EXPECT_NE(std::string::npos,
              std::move(undelivered).unwrap_err().message.to_std_string().find("missing payload"));

    // digest 形状。
    LedgerAppendInput malformed = makeAppendInput(1, {{"a.class", false, true, {}, bytesOf("A1")}});
    malformed.entries[0].after  = copyLedgerText("sha256:short");
    auto shape = store.append(malformed);
    ASSERT_TRUE(shape.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Schema, std::move(shape).unwrap_err().kind);
}

//-- bundle 绑定 / flush / 重开 ----------------------------------------------

TEST(MjLedgerStoreTest, bundleFlushReopenRestoresChainBookmarksAndPayloads)
{
    const auto  dir      = freshTempDir("mjbase_ledger_store_reopen.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto  baseline = makeBaseline();

    {
        auto opened = EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest));
        ASSERT_FALSE(opened.is_err());
        auto store = std::move(opened).unwrap();
        EXPECT_TRUE(store.bound());
        EXPECT_EQ(0U, store.generation());
        EXPECT_FALSE(store.hasBaselineSection());

        ASSERT_FALSE(
            store.append(makeAppendInput(store.nextTxnId(),
                                         {{"a.class", false, false, bytesOf("A0"), bytesOf("A1")}}))
                .is_err());
        ASSERT_FALSE(
            store.append(makeAppendInput(store.nextTxnId(),
                                         {{"c.class", false, true, {}, bytesOf("C1")}}))
                .is_err());
        ASSERT_FALSE(store.setBookmark("checkpoint", "2026-10-23T00:00:02Z").is_err());
        ASSERT_TRUE(store.hasPending());

        auto flushed = store.flush("2026-10-23T00:00:03Z");
        ASSERT_FALSE(flushed.is_err());
        const auto  report = std::move(flushed).unwrap();
        EXPECT_TRUE(report.flushed);
        EXPECT_EQ(1U, report.generation);
        EXPECT_EQ(2U, report.transactions);
        EXPECT_GE(report.ledgerBytes, 1U);
        ASSERT_EQ(3U, report.published.size());        // 2 overlay + 1 台账
        EXPECT_FALSE(store.hasBaselineSection());      // 未压缩无基线 section
        EXPECT_FALSE(store.hasPending());

        // 无 pending 且无待释放 → flush no-op。
        auto idle = store.flush("2026-10-23T00:00:04Z");
        ASSERT_FALSE(idle.is_err());
        EXPECT_FALSE(std::move(idle).unwrap().flushed);
    }   // 锁生命线随 store 析构释放，重开才能获取

    // 重开等价：链/书签/载荷/代际全部从 bundle 恢复。
    auto reopened = EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest));
    ASSERT_FALSE(reopened.is_err());
    auto restored = std::move(reopened).unwrap();
    EXPECT_EQ(1U, restored.generation());
    EXPECT_EQ(2U, restored.headTxn());
    EXPECT_EQ(2U, restored.undoDepth());
    EXPECT_EQ(baseline.digest, toStd(restored.baseContainerDigest().ref()));
    ASSERT_NE(nullptr, restored.bookmark("checkpoint"));
    EXPECT_EQ(2U, restored.bookmark("checkpoint")->txn);

    const auto  expectedPayload = bytesOf("A1");
    auto payload = restored.payloadBytes(overlaySectionName(1, 0));
    ASSERT_FALSE(payload.is_err());
    EXPECT_EQ(expectedPayload, std::move(payload).unwrap());

    // 重放确定性：重开前后对同一事务位的重放逐字节一致。
    const auto  stateAfter = replayTo(restored, 2, baseline.state);
    EXPECT_EQ(bytesOf("A1"), stateAfter.at("a.class"));
    EXPECT_EQ(bytesOf("C1"), stateAfter.at("c.class"));
    EXPECT_EQ(0U, restored.lastTouchOf("missing.class", 9));
}

TEST(MjLedgerStoreTest, boundStoreExcludesConcurrentOpenAndSurvivesDamagedLedger)
{
    const auto  dir      = freshTempDir("mjbase_ledger_store_lock.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto  baseline = makeBaseline();

    auto first = EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest));
    ASSERT_FALSE(first.is_err());

    // 单写者锁随对象持有：第二个 open = Lock 错。
    auto second = EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest));
    ASSERT_TRUE(second.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Lock, std::move(second).unwrap_err().kind);

    // flush 落出台账后破坏其字节 → Damaged（hash 失配）→ 拒开 fail-closed。
    {
        auto store = std::move(first).unwrap();
        ASSERT_FALSE(
            store.append(makeAppendInput(store.nextTxnId(),
                                         {{"a.class", false, false, bytesOf("A0"), bytesOf("A1")}}))
                .is_err());
        ASSERT_FALSE(store.flush("2026-10-23T00:00:00Z").is_err());
    }   // 锁随析构释放
    const auto  ledgerFile = dir / "sections" / "edit-ledger-1.jsonl";
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text(ledgerFile.string(), "{\"v\":1,\"row\":\"txn\"}").is_err());

    auto damaged = EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest));
    ASSERT_TRUE(damaged.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Section, std::move(damaged).unwrap_err().kind);

    // 对齐既有恢复语义：扫描定性 Damaged（条目保留），而非孤儿。
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err());
    const auto  ledgerReport = std::move(scanned).unwrap();
    EXPECT_EQ(ManifestLoadState::Loadable, ledgerReport.manifestState);
    // write_text 写出的字节与登记 size/hash 失配 → 该 section 必须进 Damaged 行。
    bool ledgerDamaged = false;
    for (const auto& entry : ledgerReport.damagedSections) {
        if (toStd(entry.name.ref()) == "edit-ledger-1") ledgerDamaged = true;
    }
    EXPECT_TRUE(ledgerDamaged) << "damaged ledger must be classified, not orphaned";
}

TEST(MjLedgerStoreTest, crashResiduesReconcileWithExistingRecoverySemantics)
{
    const auto  dir      = freshTempDir("mjbase_ledger_store_crash.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto  baseline = makeBaseline();

    {
        auto opened = EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest));
        ASSERT_FALSE(opened.is_err());
        auto store = std::move(opened).unwrap();
        ASSERT_FALSE(
            store.append(makeAppendInput(store.nextTxnId(),
                                         {{"a.class", false, false, bytesOf("A0"), bytesOf("A1")}}))
                .is_err());
        ASSERT_FALSE(store.flush("2026-10-23T00:00:00Z").is_err());
    }   // 锁随析构释放

    //-- 注入 1：flush 中途崩溃的 staged 残留（tmp/txn-*）→ 只盘点不清扫 --------
    const auto  residue = dir / "tmp" / "txn-424242-7";
    std::error_code ec;
    std::filesystem::create_directories(residue, ec);
    ASSERT_FALSE(ca::fs::FileUtil::write_text((residue / "sections" / "overlay-9-0.bin").string(),
                                              "halfway")
                     .is_err());
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err());
    EXPECT_EQ(1U, std::move(scanned).unwrap().txnResidues.size());

    //-- 注入 2：清单外 overlay 孤儿（崩溃于 section 写出后、清单提交前）---------
    const auto  orphanBytes = bytesOf("orphan");
    ASSERT_FALSE(writeSectionBytes(refOf(dirStr), copyLedgerText("overlay-99-0"),
                                   SectionFormat::Opaque, orphanBytes.data(),
                                   orphanBytes.size())
                     .is_err());
    auto rescan = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(rescan.is_err());
    const auto  orphanReport = std::move(rescan).unwrap();
    ASSERT_EQ(1U, orphanReport.orphans.size());
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, orphanReport.orphans[0].prunePolicy);

    // 引擎打开不受孤儿影响；AfterFirstFlush 档经既有 prune 原语清扫。
    {
        auto restored = expectOpenOk(
            EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest)), "reopen");
        EXPECT_EQ(1U, restored.headTxn());
    }   // 锁随析构释放
    const auto  pruned = pruneOrphanSections(refOf(dirStr), orphanReport);
    ASSERT_EQ(1U, pruned.pruned.size());
    EXPECT_FALSE(std::filesystem::exists(dir / "sections" / "overlay-99-0.bin"));

    //-- 注入 3：新代已登记、旧代仍登记（崩溃于清单切换后、清理前）→ 活动代 ------
    // 手动写 edit-ledger-2（空台账 = 全新会话）并登记，模拟"新代 manifest
    // 已提交"的中间态。
    auto gen2Text = serializeLedgerRows({});
    ASSERT_FALSE(gen2Text.is_err());
    auto gen2Written = writeSectionBytes(refOf(dirStr), copyLedgerText("edit-ledger-2"),
                                         SectionFormat::Jsonl, std::move(gen2Text).unwrap().ref());
    ASSERT_FALSE(gen2Written.is_err());
    const auto  gen2Published = std::move(gen2Written).unwrap();
    StoreManifestMutation promote;
    {
        StoreSectionEntry entry;
        entry.name          = copyLedgerText("edit-ledger-2");
        entry.format        = SectionFormat::Jsonl;
        entry.formatTag     = Utf8String::from_cstr("jsonl");
        entry.sectionVersion = kLedgerRowVersion;
        entry.contentHash   = gen2Published.contentHash.clone();
        entry.byteSize      = gen2Published.byteSize;
        entry.hasRecordCount = true;
        entry.recordCount   = 0;
        entry.producedBy    = Utf8String::from_cstr(kLedgerEngineName);
        entry.createdAt     = Utf8String::from_cstr("2026-10-23T00:00:01Z");
        entry.updatedAt     = Utf8String::from_cstr("2026-10-23T00:00:01Z");
        promote.upserts.push_back(std::move(entry));
    }
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), promote).is_err());

    auto active = expectOpenOk(
        EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest)), "promote");
    // 注意：active 持锁直至测试结束；末段断言全部经由 active 完成。
    EXPECT_EQ(2U, active.generation());   // 活动代 = 最大已登记代
    EXPECT_EQ(0U, active.headTxn());      // 新代台账为空 = 全新会话
    // 旧代台账已登记但活动代不引用 → 入待释放，下一次 flush 随清理事务删除。
    ASSERT_FALSE(
        active.append(makeAppendInput(active.nextTxnId(),
                                      {{"b.class", false, false, bytesOf("B0"), bytesOf("B1")}}))
            .is_err());
    auto cleanup = active.flush("2026-10-23T00:00:02Z");
    if (cleanup.is_err()) {
        const auto  flushError = std::move(cleanup).unwrap_err();
        ADD_FAILURE() << "cleanup flush: kind=" << static_cast<int>(flushError.kind) << " "
                      << toStd(flushError.message);
        FAIL();
    }
    const auto  cleanupReport = std::move(cleanup).unwrap();
    bool releasedOldLedger = false;
    for (const auto& name : cleanupReport.released) {
        if (toStd(name.ref()) == "edit-ledger-1") releasedOldLedger = true;
    }
    EXPECT_TRUE(releasedOldLedger);
    EXPECT_FALSE(std::filesystem::exists(dir / "sections" / "edit-ledger-1.jsonl"));
}

//-- 压缩 ------------------------------------------------------------------

/// 压缩不变量场景：基线 {a:A0, b:B0} + 四个事务
/// t1 replace a(A0→A1)、t2 add c(C1)、t3 replace b(B0→B1)、t4 replace
/// a(A1→A1x)，书签 b2@t2、b3@t3，全部 flush 到代 1。
EditLedgerStore makeCompactScenario(const BaselineFixture& baseline, const std::string& dirStr)
{
    auto opened = EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest));
    EXPECT_FALSE(opened.is_err());
    auto store = std::move(opened).unwrap();

    EXPECT_FALSE(
        store.append(makeAppendInput(store.nextTxnId(),
                                     {{"a.class", false, false, bytesOf("A0"), bytesOf("A1")}}))
            .is_err());
    EXPECT_FALSE(
        store.append(makeAppendInput(store.nextTxnId(),
                                     {{"c.class", false, true, {}, bytesOf("C1")}}))
            .is_err());
    EXPECT_FALSE(store.setBookmark("b2", "2026-10-23T00:00:10Z").is_err());
    EXPECT_FALSE(
        store.append(makeAppendInput(store.nextTxnId(),
                                     {{"b.class", false, false, bytesOf("B0"), bytesOf("B1")}}))
            .is_err());
    EXPECT_FALSE(store.setBookmark("b3", "2026-10-23T00:00:11Z").is_err());
    EXPECT_FALSE(
        store.append(makeAppendInput(store.nextTxnId(),
                                     {{"a.class", false, false, bytesOf("A1"), bytesOf("A1x")}}))
            .is_err());
    EXPECT_EQ(4U, store.headTxn());
    auto flushed = store.flush("2026-10-23T00:00:12Z");
    EXPECT_FALSE(flushed.is_err());
    return store;
}

TEST(MjLedgerStoreTest, compactionPreservesReplayResultAndLiveBookmarks)
{
    const auto  dir      = freshTempDir("mjbase_ledger_compact_core.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto  baseline = makeBaseline();
    auto store = makeCompactScenario(baseline, dirStr);

    // 压缩前重放基準：state(4) 与 state(2)。
    const auto  before4 = replayTo(store, 4, baseline.state);
    EXPECT_EQ((ContainerMap{{"a.class", bytesOf("A1x")},
                            {"b.class", bytesOf("B1")},
                            {"c.class", bytesOf("C1")}}),
              before4);
    const auto  before2 = replayTo(store, 2, baseline.state);
    const auto  canonical2 = mapToCanonical(before2);

    // 手动压缩：undo 深度承诺 0、书签全保留 → 保留点 = 最老书签 = t2。
    LedgerCompactRequest request;
    auto compacted = store.compact(request, canonical2, "2026-10-23T00:00:20Z", false);
    ASSERT_FALSE(compacted.is_err());
    const auto  report = std::move(compacted).unwrap();
    EXPECT_TRUE(report.executed);
    EXPECT_EQ(2U, report.retentionTxn);
    EXPECT_EQ(2U, report.droppedTransactions);
    EXPECT_EQ("edit-baseline-2", toStd(report.baselineSection.ref()));
    EXPECT_TRUE(report.droppedBookmarks.empty());
    EXPECT_EQ(digestOfMap(before2), toStd(report.baselineDigest.ref()));

    //-- 不变量 1：书签零失效（保留点不早于最老存活书签）------------------------
    ASSERT_NE(nullptr, store.bookmark("b2"));
    EXPECT_EQ(2U, store.bookmark("b2")->txn);
    ASSERT_NE(nullptr, store.bookmark("b3"));
    EXPECT_EQ(3U, store.bookmark("b3")->txn);

    //-- 不变量 2：压缩后重放 = 压缩前重放（新基线 ⊕ 残链）----------------------
    EXPECT_EQ(2U, store.baseTxn());
    EXPECT_EQ(4U, store.headTxn());
    EXPECT_EQ(2U, store.undoDepth());
    EXPECT_TRUE(store.hasBaselineSection());
    {
        auto baselineBytesRead = store.baselineBytes();
        ASSERT_FALSE(baselineBytesRead.is_err());
        const auto  restoredBaselineBytes = std::move(baselineBytesRead).unwrap();
        ASSERT_EQ(canonical2.size(), restoredBaselineBytes.size());
        EXPECT_EQ(0, std::memcmp(restoredBaselineBytes.data(), canonical2.data(),
                                 canonical2.size()))
            << "baseline section must be the canonical serialization of state(P)";
    }
    const auto  after4 = replayTo(store, 4, canonicalToMap(canonical2));
    EXPECT_EQ(before4, after4);

    //-- 不变量 3：被压缩事务的行不在活动链--------------------------------------
    EXPECT_EQ(nullptr, store.transaction(1));
    EXPECT_EQ(nullptr, store.transaction(2));
    ASSERT_NE(nullptr, store.transaction(3));
    // 压缩后追加新事务：链在残链上继续，base_container = 新基线 digest。
    auto t5 = store.append(makeAppendInput(store.nextTxnId(),
                                           {{"c.class", false, false, bytesOf("C1"),
                                             bytesOf("C2")}}));
    ASSERT_FALSE(t5.is_err());
    EXPECT_EQ(5U, std::move(t5).unwrap().txn);
    EXPECT_EQ(toStd(report.baselineDigest.ref()),
              toStd(store.transaction(5)->baseContainer.ref()));
    ASSERT_FALSE(store.flush("2026-10-23T00:00:22Z").is_err());
    store = EditLedgerStore();   // 释放锁生命线

    //-- 重开等价：锚行补链 + 基线核验过 → 链/书签/基线一致----------------------
    auto restored = expectOpenOk(
        EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest)), "reopen compacted");
    EXPECT_EQ(2U, restored.baseTxn());
    EXPECT_EQ(5U, restored.headTxn());
    EXPECT_EQ(toStd(report.baselineDigest.ref()), toStd(restored.baseContainerDigest().ref()));
    ASSERT_NE(nullptr, restored.bookmark("b2"));
    const auto  afterReopen = replayTo(restored, 5, baselineStateOf(restored));
    ContainerMap expected = before4;
    expected["c.class"] = bytesOf("C2");
    EXPECT_EQ(expected, afterReopen);
}

TEST(MjLedgerStoreTest, manualCompactionMayDropOldBookmarksAndListsThem)
{
    const auto  dir      = freshTempDir("mjbase_ledger_compact_drop.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto  baseline = makeBaseline();
    auto store = makeCompactScenario(baseline, dirStr);

    // 显式只留最近 1 个书签 → b2 被丢弃且列名进报告；保留点越过 b2 至 b3。
    LedgerCompactRequest request;
    request.keepRecentBookmarks = 1;
    const auto  state3 = replayTo(store, 3, baseline.state);   // 保留点 = b3.txn = 3
    auto compacted = store.compact(request, mapToCanonical(state3),
                                   "2026-10-23T00:00:21Z", false);
    ASSERT_FALSE(compacted.is_err());
    const auto  report = std::move(compacted).unwrap();
    EXPECT_TRUE(report.executed);
    EXPECT_EQ(3U, report.retentionTxn);
    ASSERT_EQ(1U, report.droppedBookmarks.size());
    EXPECT_EQ("b2", toStd(report.droppedBookmarks[0].ref()));
    EXPECT_EQ(nullptr, store.bookmark("b2"));
    ASSERT_NE(nullptr, store.bookmark("b3"));

    // 保留点不越过最老存活书签时重放仍闭合：state(4) 前后一致。
    const auto  before4 = replayTo(store, 4, baselineStateOf(store));
    EXPECT_EQ((ContainerMap{{"a.class", bytesOf("A1x")},
                            {"b.class", bytesOf("B1")},
                            {"c.class", bytesOf("C1")}}),
              before4);
}

TEST(MjLedgerStoreTest, compactionThresholdsFollowEngineConfig)
{
    const auto  dir      = freshTempDir("mjbase_ledger_compact_auto.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto  baseline = makeBaseline();

    LedgerEngineConfig config;
    config.autoCompact            = true;
    config.compactMaxTransactions = 3;
    config.minUndoDepth           = 2;

    auto opened = EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest), config);
    ASSERT_FALSE(opened.is_err());
    auto store = std::move(opened).unwrap();
    EXPECT_EQ(3U, store.config().compactMaxTransactions);   // manifest 缺省回落传入配置

    const char* revisions[4] = {"A0", "A1", "A2", "A3"};
    for (int i = 0; i < 2; ++i) {
        ASSERT_FALSE(
            store.append(makeAppendInput(store.nextTxnId(),
                                         {{"a.class", false, false, bytesOf(revisions[i]),
                                           bytesOf(revisions[i + 1])}}))
                .is_err());
        // 阈值边界：事务条数未到 3 → 不触发（undo 距离/字节均未达阈）。
        EXPECT_FALSE(store.shouldAutoCompact()) << "after txn " << i + 1;
    }
    ASSERT_FALSE(
        store.append(makeAppendInput(store.nextTxnId(),
                                     {{"a.class", false, false, bytesOf(revisions[2]),
                                       bytesOf(revisions[3])}}))
            .is_err());
    EXPECT_TRUE(store.shouldAutoCompact());   // 第 3 条事务命中条数阈值

    // 自动保留点：书签全保留（无书签）+ min_undo_depth=2 → 保留点 = 3-2 = 1。
    EXPECT_EQ(1U, store.retentionPoint(LedgerCompactRequest{}, true, nullptr));

    // flush 后（压缩要求无 pending）执行自动压缩：undo 深度承诺兑现。
    ASSERT_FALSE(store.flush("2026-10-23T00:00:30Z").is_err());
    const auto  state1 = replayTo(store, 1, baseline.state);
    auto compacted = store.compact(LedgerCompactRequest{}, mapToCanonical(state1),
                                   "2026-10-23T00:00:31Z", true);
    ASSERT_FALSE(compacted.is_err());
    const auto  report = std::move(compacted).unwrap();
    EXPECT_TRUE(report.executed);
    EXPECT_EQ(1U, report.retentionTxn);
    EXPECT_EQ(2U, store.undoDepth());
    EXPECT_GE(store.undoDepth(), config.minUndoDepth);

    // 保留点不前移（undo 深度承诺过高）→ 自动路径跳过不报错。
    store = EditLedgerStore();   // 释放锁生命线
    LedgerEngineConfig greedy = config;
    greedy.minUndoDepth       = 64;
    std::string newDigest;
    {
        auto stingy = expectOpenOk(
            EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest), greedy),
            "refill a");
        ASSERT_FALSE(
            stingy.append(makeAppendInput(stingy.nextTxnId(),
                                          {{"c.class", false, true, {}, bytesOf("C1")}}))
                .is_err());
        ASSERT_FALSE(stingy.flush("2026-10-23T00:00:32Z").is_err());
        newDigest = toStd(stingy.baseContainerDigest().ref());
    }   // 锁随析构释放
    {
        auto stingy = expectOpenOk(
            EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest), greedy),
            "refill b");
        EXPECT_EQ(newDigest, toStd(stingy.baseContainerDigest().ref()));
        auto refused = stingy.compact(LedgerCompactRequest{}, mapToCanonical(baseline.state),
                                      "2026-10-23T00:00:33Z", true);
        ASSERT_FALSE(refused.is_err());
        EXPECT_FALSE(std::move(refused).unwrap().executed);
    }
    // autoCompact=false → 阈值判定恒假。
    LedgerEngineConfig manual = config;
    manual.autoCompact        = false;
    auto quietOpened = EditLedgerStore::createInMemory(refOf(baseline.digest), manual);
    ASSERT_FALSE(quietOpened.is_err());
    auto quiet = std::move(quietOpened).unwrap();
    EXPECT_FALSE(quiet.shouldAutoCompact());
}

TEST(MjLedgerStoreTest, compactionCrashMidwayKeepsOldGenerationAuthoritative)
{
    const auto  dir      = freshTempDir("mjbase_ledger_compact_crash.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto  baseline = makeBaseline();
    auto store = makeCompactScenario(baseline, dirStr);

    //-- 崩溃点 1：新基线 section 已写出、清单未切换（§5.4 步骤 1 后）-------------
    const auto  state2 = replayTo(store, 2, baseline.state);
    const auto  canonical2 = mapToCanonical(state2);
    store = EditLedgerStore();   // 显式释放锁生命线
    ASSERT_FALSE(writeSectionBytes(refOf(dirStr), copyLedgerText("edit-baseline-2"),
                                   SectionFormat::Opaque, canonical2.data(), canonical2.size())
                     .is_err());

    // 重开：活动代仍是 1，链完整，半截基线只是清单外孤儿。
    auto recovered = expectOpenOk(
        EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest)), "reopen crash1");
    EXPECT_EQ(1U, recovered.generation());
    EXPECT_EQ(0U, recovered.baseTxn());
    EXPECT_EQ(4U, recovered.headTxn());
    EXPECT_FALSE(recovered.hasBaselineSection());
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err());
    EXPECT_EQ(1U, std::move(scanned).unwrap().orphans.size());

    //-- 崩溃点 2：清单已换代、旧代未清理（§5.4 步骤 3 后）-----------------------
    // 正常压缩到保留点 2，随后把旧对重新登记回清单（模拟清理事务未及执行）。
    auto compacted = recovered.compact(LedgerCompactRequest{}, canonical2,
                                       "2026-10-23T00:00:40Z", false);
    if (compacted.is_err()) {
        const auto  compactError = std::move(compacted).unwrap_err();
        ADD_FAILURE() << "compact crash2: kind=" << static_cast<int>(compactError.kind) << " "
                      << toStd(compactError.message);
        FAIL();
    }
    EXPECT_TRUE(std::move(compacted).unwrap().executed);
    recovered = EditLedgerStore();   // compact 后释放锁
    const std::string staleLedger = "{\"v\":1,\"row\":\"anchor\"}\n";
    auto staleWritten = writeSectionBytes(refOf(dirStr), copyLedgerText("edit-ledger-1"),
                                          SectionFormat::Jsonl, refOf(staleLedger));
    ASSERT_FALSE(staleWritten.is_err());
    const auto  stalePublished = std::move(staleWritten).unwrap();
    StoreSectionEntry staleEntry;
    staleEntry.name          = copyLedgerText("edit-ledger-1");
    staleEntry.format        = SectionFormat::Jsonl;
    staleEntry.formatTag     = Utf8String::from_cstr("jsonl");
    staleEntry.sectionVersion = kLedgerRowVersion;
    staleEntry.contentHash   = stalePublished.contentHash.clone();
    staleEntry.byteSize      = stalePublished.byteSize;
    staleEntry.hasRecordCount = true;
    staleEntry.recordCount   = 1;
    staleEntry.producedBy    = Utf8String::from_cstr(kLedgerEngineName);
    staleEntry.createdAt     = Utf8String::from_cstr("2026-10-23T00:00:00Z");
    staleEntry.updatedAt     = staleEntry.createdAt.clone();
    StoreManifestMutation resurrect;
    resurrect.upserts.push_back(std::move(staleEntry));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), resurrect).is_err());

    auto authoritative = expectOpenOk(
        EditLedgerStore::openBundle(refOf(dirStr), refOf(baseline.digest)), "reopen resurrect");
    // 活动代仍取最大已登记代（2），旧代行不复活链。
    EXPECT_EQ(2U, authoritative.generation());
    EXPECT_EQ(2U, authoritative.baseTxn());
    EXPECT_EQ(4U, authoritative.headTxn());
    ASSERT_NE(nullptr, authoritative.bookmark("b2"));
    const auto  replayed = replayTo(authoritative, 4, baselineStateOf(authoritative));
    EXPECT_EQ((ContainerMap{{"a.class", bytesOf("A1x")},
                            {"b.class", bytesOf("B1")},
                            {"c.class", bytesOf("C1")}}),
              replayed);
}
