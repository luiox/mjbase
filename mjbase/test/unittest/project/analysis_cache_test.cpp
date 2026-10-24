// 分析缓存落盘引擎（RFC 0001 §7 失效键 + §4.1 section 布局 + §10-P3）：xref
// 整索引槽与条目级 decompile 缓存的二次打开命中、四字段失效（depends/
// produced_by/section_version/profile）、条目级粒度隔离（编辑一个类只失效该
// 类）、合并 flush（多槽一次清单事务，风险 6）、同族陈旧槽退役与崩溃残留
// 由既有恢复扫描兜底（AfterFirstFlush 孤儿 prune / Damaged 同名重写自愈）。
//
// 失效键完全沿用 store.json 既有字段（project_store.hpp §4.2），不自造体系。
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "mjbase/project/analysis_cache.hpp"
#include "mjbase/project/project_store.hpp"
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

Utf8String ownOf(const std::string& value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

/// 清掉上一次 run 的同名临时 bundle，返回干净路径。
std::filesystem::path freshTempDir(const std::string& name)
{
    const auto      path = test::getTempOutPath(name);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path;
}

/// 建空 bundle 骨架（store.json 空 v1 清单）。
std::filesystem::path freshBundle(const std::string& name)
{
    const auto dir     = freshTempDir(name);
    auto       created = createBundleSkeleton(refOf(dir.string()));
    EXPECT_FALSE(created.is_err());
    return dir;
}

SectionDep dep(const std::string& path, const std::string& fingerprint)
{
    SectionDep entry;
    entry.path        = ownOf(path);
    entry.fingerprint = ownOf(fingerprint);
    return entry;
}

SectionDep cloneDep(const SectionDep& source)
{
    SectionDep copy;
    copy.path        = source.path.clone();
    copy.fingerprint = source.fingerprint.clone();
    return copy;
}

/// 合法 sha256 形状的测试 digest（FNV 混入 tag 字节，任意不同 tag 的前 8
/// 字节十六进制必异，避免字面量 mod 16 撞车）。
std::string digestOf(char tag)
{
    ca::u64 state = 14695981039346656037ULL;
    for (int round = 0; round < 4; ++round) {
        state ^= static_cast<ca::u8>(tag);
        state *= 1099511628211ULL;
    }
    std::string hex;
    for (int index = 0; index < 8; ++index) {
        hex += "0123456789abcdef"[state % 16];
        state /= 16;
    }
    while (hex.size() < 64) hex.push_back('a');
    return "sha256:" + hex;
}

XrefCacheKey xrefKey(const std::string& fingerprint, const std::string& profile,
                     const std::string& producer, ca::u32 version)
{
    XrefCacheKey key;
    key.depends.push_back(dep("samples/app.jar", fingerprint));
    key.profile        = ownOf(profile);
    key.producer       = ownOf(producer);
    key.sectionVersion = version;
    return key;
}

DecompileCacheSlot slotOf(const std::string& containerDigest, const std::string& producer,
                          const std::string& profile)
{
    DecompileCacheSlot slot;
    slot.containerDigest = ownOf(containerDigest);
    slot.containerPath   = ownOf("samples/app.jar");
    slot.producer        = ownOf(producer);
    slot.profile         = ownOf(profile);
    return slot;
}

/// 打开失败即失败并返回绑定实例（gtest 宏早退语义下的取值样板）。
AnalysisCacheStore expectOpen(const std::filesystem::path& bundle)
{
    auto opened = AnalysisCacheStore::open(refOf(bundle.string()));
    if (opened.is_err()) {
        ADD_FAILURE() << "open failed: " << toStd(std::move(opened).unwrap_err().message);
        return AnalysisCacheStore();
    }
    return std::move(opened).unwrap();
}

/// 移出 Result 的成功值（gtest 宏早退语义下读取 optional/报告的统一出口；
/// unwrap() const& 会按值拷贝，move-only 载荷必须移动出）。
template <typename T>
T taken(ca::Result<T, ProjectStoreError>& result)
{
    EXPECT_FALSE(result.is_err());
    return result.is_err() ? T() : std::move(result).unwrap();
}

std::filesystem::path sectionFileOf(const std::filesystem::path& bundle, const std::string& name,
                                    SectionFormat format)
{
    return bundle / kProjectStoreSectionsDirName / toStd(sectionFileName(refOf(name), format));
}

/// 按清单定位 section 名（登记面的唯一查询）。
bool manifestHas(const std::filesystem::path& bundle, const std::string& name)
{
    auto loaded = readStoreManifestFile(refOf(bundle.string()));
    if (loaded.is_err()) return false;
    for (const auto& entry : std::move(loaded).unwrap().sections) {
        if (toStd(entry.name.ref()) == name) return true;
    }
    return false;
}

}   // namespace

//-- xref 缓存：命中与失效 ----------------------------------------------------

TEST(MjAnalysisCacheTest, xrefRoundTripAndSecondOpenHit)
{
    const auto        bundle  = freshBundle("mj_analysis_cache_xref_hit");
    const auto        key     = xrefKey(digestOf('f'), "keep-synth", "mjbase-xref-cache/1", 1);
    const std::string payload = "xref-fact-bytes";

    auto store = expectOpen(bundle);
    ASSERT_FALSE(store.storeXref(key, refOf(payload), "2026-10-23T00:00:00Z").is_err());

    // 二次打开同工程：同失效键命中且负载逐字节一致（命中率断言的前提语义）。
    auto reopened = expectOpen(bundle);
    auto lookup   = reopened.lookupXref(key);
    ASSERT_FALSE(lookup.is_err());
    auto hit = taken(lookup);
    ASSERT_TRUE(hit.has_value());
    const std::string seen(reinterpret_cast<const char*>(hit->data()), hit->size());
    EXPECT_EQ(seen, payload);
}

TEST(MjAnalysisCacheTest, xrefInvalidatedOnContainerDigestChange)
{
    const auto bundle = freshBundle("mj_analysis_cache_xref_dep");
    auto       store  = expectOpen(bundle);
    ASSERT_FALSE(
        store.storeXref(xrefKey(digestOf('o'), "", "mjbase-xref-cache/1", 1), refOf("stale"),
                        "2026-10-23T00:00:00Z")
            .is_err());

    // 任一参与容器 digest（登记 fingerprint）变化 → depends 失配 → 未命中。
    auto fresh = expectOpen(bundle);
    auto changed =
        fresh.lookupXref(xrefKey(digestOf('n'), "", "mjbase-xref-cache/1", 1));
    ASSERT_FALSE(changed.is_err());
    EXPECT_FALSE(taken(changed).has_value());
    // 同键仍在（失效 ≠ 删除，容器集未变时旧槽可用）。
    auto same = fresh.lookupXref(xrefKey(digestOf('o'), "", "mjbase-xref-cache/1", 1));
    ASSERT_FALSE(same.is_err());
    EXPECT_TRUE(taken(same).has_value());
}

TEST(MjAnalysisCacheTest, xrefInvalidatedOnProfileVersionAndProducerChange)
{
    const auto bundle = freshBundle("mj_analysis_cache_xref_fields");
    auto       store  = expectOpen(bundle);
    ASSERT_FALSE(
        store.storeXref(xrefKey(digestOf('f'), "profile-a", "engine/1", 1), refOf("payload"),
                        "2026-10-23T00:00:00Z")
            .is_err());
    auto fresh = expectOpen(bundle);

    // profile 变化 → 未命中（内容受 options 影响）。
    auto otherProfile = fresh.lookupXref(xrefKey(digestOf('f'), "profile-b", "engine/1", 1));
    ASSERT_FALSE(otherProfile.is_err());
    EXPECT_FALSE(taken(otherProfile).has_value());
    // section_version 变化 → 未命中（缓存格式版本门）。
    auto otherVersion = fresh.lookupXref(xrefKey(digestOf('f'), "profile-a", "engine/1", 2));
    ASSERT_FALSE(otherVersion.is_err());
    EXPECT_FALSE(taken(otherVersion).has_value());
    // produced_by（引擎 + 引擎版本自述）变化 → 未命中。
    auto otherProducer = fresh.lookupXref(xrefKey(digestOf('f'), "profile-a", "engine/2", 1));
    ASSERT_FALSE(otherProducer.is_err());
    EXPECT_FALSE(taken(otherProducer).has_value());
    // 全字段一致 → 命中。
    auto same = fresh.lookupXref(xrefKey(digestOf('f'), "profile-a", "engine/1", 1));
    ASSERT_FALSE(same.is_err());
    EXPECT_TRUE(taken(same).has_value());
}

TEST(MjAnalysisCacheTest, xrefDamagedSectionRecoversBySameNameRewrite)
{
    const auto bundle = freshBundle("mj_analysis_cache_xref_dmg");
    const auto key    = xrefKey(digestOf('f'), "", "mjbase-xref-cache/1", 1);
    auto       store  = expectOpen(bundle);
    const auto name   = toStd(xrefCacheSectionName(key.depends, key.sectionVersion, ""));
    ASSERT_FALSE(store.storeXref(key, refOf("good"), "2026-10-23T00:00:00Z").is_err());

    // 崩溃/位翻转：内容损坏 = Damaged → 未命中（缓存永不值得为它报错）。
    {
        std::error_code ec;
        std::filesystem::resize_file(sectionFileOf(bundle, name, SectionFormat::Opaque), 2, ec);
        ASSERT_FALSE(ec);
    }
    auto damaged = expectOpen(bundle);
    auto miss    = damaged.lookupXref(key);
    ASSERT_FALSE(miss.is_err());
    EXPECT_FALSE(taken(miss).has_value());

    // 同名重写即恢复（§8.2-3 既有恢复口径）。
    ASSERT_FALSE(damaged.storeXref(key, refOf("good-again"), "2026-10-23T00:00:10Z").is_err());
    auto after = expectOpen(bundle);
    auto hit   = after.lookupXref(key);
    ASSERT_FALSE(hit.is_err());
    auto payloadSeen = taken(hit);
    ASSERT_TRUE(payloadSeen.has_value());
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(payloadSeen->data()),
                          payloadSeen->size()),
              "good-again");
}

TEST(MjAnalysisCacheTest, xrefStaleSlotRetiredOnKeyChange)
{
    const auto bundle = freshBundle("mj_analysis_cache_xref_gc");
    auto       store  = expectOpen(bundle);
    ASSERT_FALSE(
        store.storeXref(xrefKey(digestOf('f'), "p1", "engine/1", 1), refOf("old"),
                        "2026-10-23T00:00:00Z")
            .is_err());
    ASSERT_FALSE(
        store.storeXref(xrefKey(digestOf('f'), "p2", "engine/1", 1), refOf("new"),
                        "2026-10-23T00:00:10Z")
            .is_err());

    // 同引擎陈旧槽随 storeXref 退役：清单只剩当前槽，旧 section 文件被墓碑删除。
    const auto oldName = toStd(xrefCacheSectionName(xrefKey(digestOf('f'), "p1", "engine/1", 1).depends,
                                                    1, "p1"));
    const auto newName = toStd(xrefCacheSectionName(xrefKey(digestOf('f'), "p2", "engine/1", 1).depends,
                                                    1, "p2"));
    EXPECT_FALSE(manifestHas(bundle, oldName));
    EXPECT_TRUE(manifestHas(bundle, newName));
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(sectionFileOf(bundle, oldName, SectionFormat::Opaque), ec));
    EXPECT_TRUE(std::filesystem::exists(sectionFileOf(bundle, newName, SectionFormat::Opaque), ec));
}

TEST(MjAnalysisCacheTest, xrefRefusedWhenManifestUnloadable)
{
    const auto bundle = freshBundle("mj_analysis_cache_xref_unload");
    const auto key    = xrefKey(digestOf('f'), "", "engine/1", 1);
    {
        auto store = expectOpen(bundle);
        ASSERT_FALSE(store.storeXref(key, refOf("p"), "2026-10-23T00:00:00Z").is_err());
    }
    // 降级孤儿态：清单不可载 → open 拒绝、未绑定实例写拒绝（不落新清单，CP-1）。
    std::error_code ec;
    std::filesystem::remove(bundle / kProjectStoreManifestName, ec);
    ASSERT_FALSE(ec);
    auto opened = AnalysisCacheStore::open(refOf(bundle.string()));
    EXPECT_TRUE(opened.is_err());
    AnalysisCacheStore unbound;
    auto               lookup = unbound.lookupXref(key);
    ASSERT_FALSE(lookup.is_err());
    EXPECT_FALSE(taken(lookup).has_value());
    auto stored = unbound.storeXref(key, refOf("p"), "2026-10-23T00:00:00Z");
    ASSERT_TRUE(stored.is_err());
    EXPECT_FALSE(std::filesystem::exists(bundle / kProjectStoreManifestName, ec));
}

//-- 条目级 decompile 缓存 ------------------------------------------------------

TEST(MjAnalysisCacheTest, decompileEntryLevelIsolationAfterOtherClassEdit)
{
    const auto bundle     = freshBundle("mj_analysis_cache_dec_entry");
    const auto slot       = slotOf(digestOf('c'), "mjt decompile quilt/2.15", "");
    const auto digestA1   = digestOf('A');
    const auto digestB1   = digestOf('B');
    const auto digestB2   = digestOf('2');

    auto store = expectOpen(bundle);
    ASSERT_FALSE(store.putDecompile(slot, "com/x/A", refOf(digestA1), "class A {}").is_err());
    ASSERT_FALSE(store.putDecompile(slot, "com/x/B", refOf(digestB1), "class B {}").is_err());
    EXPECT_EQ(store.pendingDecompileCount(), 2u);
    auto flushed = store.flush("2026-10-23T00:00:00Z");
    ASSERT_FALSE(flushed.is_err());
    auto flushedReport = taken(flushed);
    EXPECT_TRUE(flushedReport.flushed);
    EXPECT_EQ(flushedReport.records, 2u);

    auto fresh = expectOpen(bundle);
    auto hitA  = fresh.lookupDecompile(slot, "com/x/A", refOf(digestA1));
    ASSERT_FALSE(hitA.is_err());
    auto sourceA = taken(hitA);
    ASSERT_TRUE(sourceA.has_value());
    EXPECT_EQ(toStd(*sourceA), "class A {}");

    // 编辑 B（条目 digest 变化）→ 只有 B 失效，A 仍命中（条目级粒度）。
    auto missB = fresh.lookupDecompile(slot, "com/x/B", refOf(digestB2));
    ASSERT_FALSE(missB.is_err());
    EXPECT_FALSE(taken(missB).has_value());
    ASSERT_FALSE(fresh.putDecompile(slot, "com/x/B", refOf(digestB2), "class B2 {}").is_err());
    ASSERT_FALSE(fresh.flush("2026-10-23T00:00:10Z").is_err());

    auto reopened = expectOpen(bundle);
    auto hitA2    = reopened.lookupDecompile(slot, "com/x/A", refOf(digestA1));
    ASSERT_FALSE(hitA2.is_err());
    ASSERT_TRUE(taken(hitA2).has_value());
    auto hitB2 = reopened.lookupDecompile(slot, "com/x/B", refOf(digestB2));
    ASSERT_FALSE(hitB2.is_err());
    auto sourceB2 = taken(hitB2);
    ASSERT_TRUE(sourceB2.has_value());
    EXPECT_EQ(toStd(*sourceB2), "class B2 {}");
}

TEST(MjAnalysisCacheTest, decompileInvalidatedOnEngineProfileAndVersionChange)
{
    const auto bundle = freshBundle("mj_analysis_cache_dec_slot");
    const auto entry  = digestOf('A');
    auto       store  = expectOpen(bundle);
    ASSERT_FALSE(store.putDecompile(slotOf(digestOf('c'), "engine-a/1", ""), "com/x/A",
                                    refOf(entry), "source")
                     .is_err());
    ASSERT_FALSE(store.flush("2026-10-23T00:00:00Z").is_err());
    auto fresh = expectOpen(bundle);

    // 引擎（+版本自述）换 → 新槽未命中；options profile 换 → 新槽未命中；
    // section_version 换 → 新槽未命中；原键仍命中。
    auto otherEngine = fresh.lookupDecompile(slotOf(digestOf('c'), "engine-b/1", ""), "com/x/A",
                                             refOf(entry));
    ASSERT_FALSE(otherEngine.is_err());
    EXPECT_FALSE(taken(otherEngine).has_value());
    auto otherProfile = fresh.lookupDecompile(slotOf(digestOf('c'), "engine-a/1", "keep=aggressive"),
                                              "com/x/A", refOf(entry));
    ASSERT_FALSE(otherProfile.is_err());
    EXPECT_FALSE(taken(otherProfile).has_value());
    auto otherVersion = slotOf(digestOf('c'), "engine-a/1", "");
    otherVersion.sectionVersion = 2;
    auto versionMiss = fresh.lookupDecompile(otherVersion, "com/x/A", refOf(entry));
    ASSERT_FALSE(versionMiss.is_err());
    EXPECT_FALSE(taken(versionMiss).has_value());
    auto same = fresh.lookupDecompile(slotOf(digestOf('c'), "engine-a/1", ""), "com/x/A",
                                      refOf(entry));
    ASSERT_FALSE(same.is_err());
    EXPECT_TRUE(taken(same).has_value());
}

TEST(MjAnalysisCacheTest, decompileMergedFlushPreservesRegisteredRecords)
{
    const auto bundle = freshBundle("mj_analysis_cache_dec_merge");
    const auto slot   = slotOf(digestOf('c'), "engine/1", "");
    const auto name   = toStd(decompileCacheSectionName(slot));
    {
        auto store = expectOpen(bundle);
        ASSERT_FALSE(store.putDecompile(slot, "com/x/A", refOf(digestOf('A')), "class A {}").is_err());
        ASSERT_FALSE(store.flush("2026-10-23T00:00:00Z").is_err());
    }
    // 新会话只缓冲 B 再 flush：A 不丢（槽内已登记记录并写保留）。
    {
        auto store = expectOpen(bundle);
        ASSERT_FALSE(store.putDecompile(slot, "com/x/B", refOf(digestOf('B')), "class B {}").is_err());
        auto flushed = store.flush("2026-10-23T00:00:10Z");
        ASSERT_FALSE(flushed.is_err());
        auto flushedReport = taken(flushed);
        EXPECT_EQ(flushedReport.records, 2u);   // A + B 合并写出
        EXPECT_EQ(flushedReport.published.size(), 1u);
    }
    auto fresh = expectOpen(bundle);
    auto hitA  = fresh.lookupDecompile(slot, "com/x/A", refOf(digestOf('A')));
    ASSERT_FALSE(hitA.is_err());
    EXPECT_TRUE(taken(hitA).has_value());
    auto hitB = fresh.lookupDecompile(slot, "com/x/B", refOf(digestOf('B')));
    ASSERT_FALSE(hitB.is_err());
    EXPECT_TRUE(taken(hitB).has_value());
    EXPECT_TRUE(manifestHas(bundle, name));
}

TEST(MjAnalysisCacheTest, decompileStaleProfileSlotRetiredOnFlush)
{
    const auto bundle = freshBundle("mj_analysis_cache_dec_gc");
    const auto slotP1 = slotOf(digestOf('c'), "engine/1", "p1");
    const auto slotP2 = slotOf(digestOf('c'), "engine/1", "p2");
    const auto nameP1 = toStd(decompileCacheSectionName(slotP1));
    const auto nameP2 = toStd(decompileCacheSectionName(slotP2));
    ASSERT_NE(nameP1, nameP2);

    auto store = expectOpen(bundle);
    ASSERT_FALSE(store.putDecompile(slotP1, "com/x/A", refOf(digestOf('A')), "old").is_err());
    ASSERT_FALSE(store.flush("2026-10-23T00:00:00Z").is_err());
    EXPECT_TRUE(manifestHas(bundle, nameP1));
    ASSERT_FALSE(store.putDecompile(slotP2, "com/x/A", refOf(digestOf('A')), "new").is_err());
    auto flushed = store.flush("2026-10-23T00:00:10Z");
    ASSERT_FALSE(flushed.is_err());
    ASSERT_EQ(taken(flushed).released.size(), 1u);

    // 同容器同引擎换 profile：旧槽随 flush 退役（同一笔清单事务内墓碑）。
    EXPECT_FALSE(manifestHas(bundle, nameP1));
    EXPECT_TRUE(manifestHas(bundle, nameP2));
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(sectionFileOf(bundle, nameP1, SectionFormat::Jsonl), ec));
}

TEST(MjAnalysisCacheTest, decompilePutRejectsMalformedKeys)
{
    AnalysisCacheStore store;
    auto               badSlot = slotOf("not-a-digest", "engine/1", "");
    auto badDigest = store.putDecompile(badSlot, "com/x/A", refOf(digestOf('A')), "s");
    ASSERT_TRUE(badDigest.is_err());
    EXPECT_EQ(std::move(badDigest).unwrap_err().kind, ProjectStoreErrorKind::Schema);

    auto goodSlot = slotOf(digestOf('c'), "engine/1", "");
    auto badEntry = store.putDecompile(goodSlot, "com/x/A", refOf("sha256:zz"), "s");
    ASSERT_TRUE(badEntry.is_err());
    auto emptyClass = store.putDecompile(goodSlot, "", refOf(digestOf('A')), "s");
    ASSERT_TRUE(emptyClass.is_err());
    auto emptyProducer   = slotOf(digestOf('c'), "", "");
    auto badProducer   = store.putDecompile(emptyProducer, "com/x/A", refOf(digestOf('A')), "s");
    ASSERT_TRUE(badProducer.is_err());

    // 形状违约的槽名推导返回空串（可判不可写）。
    EXPECT_TRUE(decompileCacheSectionName(badSlot).is_empty());
    EXPECT_FALSE(decompileCacheSectionName(goodSlot).is_empty());
}

//-- 崩溃残留与恢复兜底 ---------------------------------------------------------

TEST(MjAnalysisCacheTest, crashResidueCoveredByRecoveryScan)
{
    const auto bundle = freshBundle("mj_analysis_cache_recovery");
    const auto slot   = slotOf(digestOf('c'), "engine/1", "");
    const auto name   = toStd(decompileCacheSectionName(slot));
    {
        auto store = expectOpen(bundle);
        ASSERT_FALSE(store.putDecompile(slot, "com/x/A", refOf(digestOf('A')), "class A {}").is_err());
        ASSERT_FALSE(store.flush("2026-10-23T00:00:00Z").is_err());
    }
    // 崩溃注入：section 字节已发布、清单未提（manifest last 的崩溃窗）。
    const auto orphanName = name + "9";
    auto orphanWritten = writeSectionBytes(refOf(bundle.string()), refOf(orphanName),
                                           SectionFormat::Jsonl, refOf("{\"v\":1}"));
    ASSERT_FALSE(orphanWritten.is_err());

    auto scanned = scanBundleRecovery(refOf(bundle.string()));
    ASSERT_FALSE(scanned.is_err());
    auto report = std::move(scanned).unwrap();
    ASSERT_EQ(report.manifestState, ManifestLoadState::Loadable);
    ASSERT_EQ(report.orphans.size(), 1u);
    EXPECT_EQ(toStd(report.orphans[0].fileName),
              toStd(sectionFileName(refOf(orphanName), SectionFormat::Jsonl)));
    EXPECT_EQ(report.orphans[0].prunePolicy, OrphanPrunePolicy::AfterFirstFlush);

    // 既有 prune 动作面收敛孤儿；缓存段本体仍是 Ok 行。
    auto pruned = pruneOrphanSections(refOf(bundle.string()), report);
    EXPECT_EQ(pruned.pruned.size(), 1u);
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(sectionFileOf(bundle, orphanName, SectionFormat::Jsonl), ec));

    auto rescanned = scanBundleRecovery(refOf(bundle.string()));
    ASSERT_FALSE(rescanned.is_err());
    auto clean = std::move(rescanned).unwrap();
    EXPECT_TRUE(clean.orphans.empty());
    EXPECT_EQ(clean.okSections.size(), 1u);
}

TEST(MjAnalysisCacheTest, decompileDamagedSectionMissesThenSelfHeals)
{
    const auto bundle = freshBundle("mj_analysis_cache_dec_dmg");
    const auto slot   = slotOf(digestOf('c'), "engine/1", "");
    const auto name   = toStd(decompileCacheSectionName(slot));
    {
        auto store = expectOpen(bundle);
        ASSERT_FALSE(store.putDecompile(slot, "com/x/A", refOf(digestOf('A')), "class A {}").is_err());
        ASSERT_FALSE(store.flush("2026-10-23T00:00:00Z").is_err());
    }
    // 行级不可解释（截断 = Damaged 路径）→ 未命中。
    {
        std::error_code ec;
        std::filesystem::resize_file(sectionFileOf(bundle, name, SectionFormat::Jsonl), 5, ec);
        ASSERT_FALSE(ec);
    }
    auto damaged = expectOpen(bundle);
    auto miss    = damaged.lookupDecompile(slot, "com/x/A", refOf(digestOf('A')));
    ASSERT_FALSE(miss.is_err());
    EXPECT_FALSE(taken(miss).has_value());

    // 重新 put + flush = 同名重写恢复（§8.2-3）。
    ASSERT_FALSE(damaged.putDecompile(slot, "com/x/A", refOf(digestOf('A')), "class A {}").is_err());
    ASSERT_FALSE(damaged.flush("2026-10-23T00:00:10Z").is_err());
    auto healed = expectOpen(bundle);
    auto hit    = healed.lookupDecompile(slot, "com/x/A", refOf(digestOf('A')));
    ASSERT_FALSE(hit.is_err());
    ASSERT_TRUE(taken(hit).has_value());
    // 恢复后恢复扫描回到 Ok 单行。
    auto rescanned = scanBundleRecovery(refOf(bundle.string()));
    ASSERT_FALSE(rescanned.is_err());
    EXPECT_EQ(std::move(rescanned).unwrap().okSections.size(), 1u);
}

//-- section 名推导 -------------------------------------------------------------

TEST(MjAnalysisCacheTest, sectionNamesDeterministicAndValid)
{
    // SectionDep 持 move-only 串：push_back 装配（initializer_list 要拷贝）。
    std::vector<SectionDep> deps;
    deps.push_back(dep("a.jar", digestOf('1')));
    deps.push_back(dep("b.jar", digestOf('2')));
    const auto name1  = xrefCacheSectionName(deps, 1, "profile");
    const auto name2  = xrefCacheSectionName(deps, 1, "profile");
    EXPECT_EQ(toStd(name1), toStd(name2));
    EXPECT_FALSE(validateSectionName(name1.ref()).is_err());
    EXPECT_LE(name1.byte_length(), 64u);

    // depends 序不敏感（canonical 序）；键任一字段变化 → 槽名变化。
    std::vector<SectionDep> reversed;
    reversed.push_back(cloneDep(deps[1]));
    reversed.push_back(cloneDep(deps[0]));
    EXPECT_EQ(toStd(xrefCacheSectionName(reversed, 1, "profile")), toStd(name1));
    EXPECT_NE(toStd(xrefCacheSectionName(deps, 1, "other")), toStd(name1));
    EXPECT_NE(toStd(xrefCacheSectionName(deps, 2, "profile")), toStd(name1));

    const auto slotName1 = decompileCacheSectionName(slotOf(digestOf('c'), "engine/1", ""));
    EXPECT_EQ(toStd(slotName1),
              toStd(decompileCacheSectionName(slotOf(digestOf('c'), "engine/1", ""))));
    EXPECT_NE(toStd(slotName1),
              toStd(decompileCacheSectionName(slotOf(digestOf('2'), "engine/1", ""))));
    EXPECT_NE(toStd(slotName1),
              toStd(decompileCacheSectionName(slotOf(digestOf('c'), "engine/2", ""))));
    EXPECT_FALSE(validateSectionName(slotName1.ref()).is_err());
    EXPECT_LE(slotName1.byte_length(), 64u);
}
