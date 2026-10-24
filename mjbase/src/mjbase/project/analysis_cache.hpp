#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/project/project_store.hpp"

namespace mjbase::project {

/// @file analysis_cache.hpp
/// @brief 分析缓存落盘引擎：xref 缓存（整索引一槽）与条目级 decompile 缓存
///        的 section 读写、失效键判定与合并 flush。
///
/// 上游依据：mjt 仓 RFC 0001 §4.1/§7/§10-P3/§11 风险 6。失效键**完全沿用
/// store.json 既有字段**（§2.2），不自造体系：
///
///     失效键 = depends[]（参与容器的 path+fingerprint 全集）
///            + produced_by（引擎 + 引擎版本自述串）
///            + section_version（缓存格式版本）
///            + profile（影响内容的 options 串；空 ≡ 未设置，R52/CP-3 归一）
///
/// 命中判定 = 名字槽定位（名字内嵌短 digest，不同键不共槽）+ 清单条目四字段
/// 全值复核（防短 digest 相撞）+ readSectionBytes 校验读（§5.2 先 size 后
/// hash）。任一环节失配按**未命中**处理（缓存内容可重算，永不值得为它报错）
/// ——Damaged 缓存段由「未命中重算 + 同名重写」自愈（§8.2-3 既有口径）。
/// 清单不可载（isManifestUnloadableKind）除外：降级孤儿态下读路径报错、写
/// 路径拒绝，与台账引擎同一 fail-closed 纪律。
///
/// 崩溃安全沿既有恢复语义：一切字节经 writeSectionBytes 原子写（staged tmp +
/// 单次换名），清单事务 manifest last——崩溃窗口留下的新文件是 AfterFirstFlush
/// 档孤儿（scanBundleRecovery + pruneOrphanSections 收敛），旧清单完整。
///
/// 锁纪律：本类**不获取** store.lock——单写者锁由工程会话（如 EditLedgerStore
/// 绑定）持有，写路径（storeXref/flush）前置条件是调用方已持锁；读路径无锁
/// 短句柄（§7.3 读者纪律）。

/// @brief xref 缓存引擎自述串（produced_by；消费者可携带自己的引擎串覆盖）。
inline constexpr const char* kXrefCacheEngineDefault = "mjbase-xref-cache/1";

/// @brief decompile 缓存引擎自述串基形。条目级键的「引擎 + 引擎版本」由消费
///        方自述（produced_by 承载，如 "mjt decompile quiltflower/2.15.1"），
///        本常量只作缺省兜底。
inline constexpr const char* kDecompileCacheEngineDefault = "mjbase-decompile-cache/1";

/// @brief decompile 缓存行级版本（行级自描述；同时是该 section 的
///        section_version——行格式与缓存格式同源演进）。
inline constexpr ca::u32 kDecompileCacheRowVersion = 1;

/// @brief decompile 缓存 section 名前缀（名族词根；同族陈旧槽随 flush 退役）。
inline constexpr const char* kDecompileCacheSectionPrefix = "decompile-";

/// @brief xref 缓存 section 名前缀（同上；陈旧槽随 storeXref 退役）。
inline constexpr const char* kXrefCacheSectionPrefix = "xref-";

/// @brief xref 缓存键（失效键四字段的原样投影；RFC §7）。
struct XrefCacheKey
{
    std::vector<SectionDep> depends;   ///< 参与容器全集（写出按 path 字典序复核）
    ca::u32                 sectionVersion = 1;  ///< 消费者私有缓存格式版本
    ca::str::Utf8String     producer;      ///< produced_by（非空；引擎 + 版本自述）
    ca::str::Utf8String     profile;       ///< options profile（空 ≡ 未设置）
};

/// @brief decompile 缓存的槽键（RFC §7 条目级键的槽级投影）。
///
/// 槽 = （容器 digest、引擎 + 引擎版本、options profile）三元组确定的
/// section；条目级粒度由槽内记录的条目 digest 承担——条目未变即命中，编辑
/// 一个类只失效该类（容器 digest 用登记口径的原始容器，原容器不可变红线
/// 保证槽名稳定；工作副本条目的 digest 进记录键）。
struct DecompileCacheSlot
{
    ca::str::Utf8String containerDigest;     ///< 登记 digest（"sha256:"+64hex）
    ca::str::Utf8String containerPath;       ///< 登记 path（depends 单条）
    ca::str::Utf8String producer;            ///< produced_by（引擎 + 引擎版本自述）
    ca::str::Utf8String profile;             ///< options profile（空 ≡ 未设置）
    ca::u32             sectionVersion = kDecompileCacheRowVersion;
};

/// @brief 一条条目级反编译缓存记录（jsonl 行；行字段严格，未知/重复字段、
///        digest 形状错即整段拒载——写方 bug 当场抓住，读方按未命中自愈）。
struct DecompileCacheRecord
{
    ca::str::Utf8String className;   ///< 类内部名（槽内唯一；写出按名字典序）
    ca::str::Utf8String entryDigest; ///< 反编译时刻类字节 digest（"sha256:"+64hex）
    ca::str::Utf8String source;      ///< 反编译产物文本（可空串）
};

/// @brief xref 缓存 section 名：`xref-<setd8>-<p8>-v<ver>`。
///
/// setd8 = depends 全集的 canonical 序（按 path 字典序，逐项
/// len(path,u64 LE)+path+len(fingerprint,u64 LE)+fingerprint）SHA-256 前 8 hex；
/// p8 = profile 字节 SHA-256 前 8 hex。名字只作槽定位，命中判定仍做全值复核。
ca::str::Utf8String xrefCacheSectionName(const std::vector<SectionDep>& depends,
                                         ca::u32 sectionVersion, std::string_view profile);

/// @brief decompile 缓存 section 名：`decompile-<cd8>-<e8>-<p8>-v<ver>`。
///
/// cd8 = 容器 digest 十六进制体前 8 位；e8 = producer 字节 SHA-256 前 8 hex；
/// p8 同上。slot.containerDigest 必须是合法 digest 形状，否则返回空串。
ca::str::Utf8String decompileCacheSectionName(const DecompileCacheSlot& slot);

/// @brief 合并 flush 报告。
struct AnalysisCacheFlushReport
{
    bool flushed = false;  ///< false = 无 pending，零动作
    std::vector<ca::str::Utf8String> published;  ///< 本次发布的 section 名
    std::vector<ca::str::Utf8String> released;   ///< 本次随事务退役的同族陈旧槽
    ca::u64 records = 0;                          ///< 本次落盘的条目级记录总数
};

/// @brief 分析缓存存取（绑定 bundle 目录；move-only 值语义）。
///
/// 一切查询现读清单（不缓存清单视图）——与外部写者（台账清理事务等）的
/// 一致性不建立在内存快照上；单机磁盘成本可接受，内存化索引归后续卡。
class AnalysisCacheStore
{
public:
    AnalysisCacheStore() = default;
    AnalysisCacheStore(AnalysisCacheStore&& other) noexcept = default;
    AnalysisCacheStore& operator=(AnalysisCacheStore&& other) noexcept = default;
    AnalysisCacheStore(const AnalysisCacheStore&)            = delete;
    AnalysisCacheStore& operator=(const AnalysisCacheStore&) = delete;

    /// @brief 绑定既有 bundle（不获取 store.lock，见文件注锁纪律）。
    ///
    /// bundle 缺 store.json 等"清单不可载"态拒绝（缓存引擎不落任何新清单，
    /// 与台账引擎同款纪律）。纯数据绑定：本步零触盘。
    static ca::Result<AnalysisCacheStore, ProjectStoreError> open(
        const ca::str::Utf8StringRef& bundleDir);

    /// 是否绑定 bundle。
    bool bound() const { return !bundleDir_.is_empty(); }

    //-- xref 缓存（整索引一槽）----------------------------------------------

    /// @brief 读 xref 缓存。命中返回负载字节；未命中（无登记/键失配/内容
    ///        Damaged/行不可解释）返回 nullopt。清单不可载 = Io 错上浮。
    ///
    /// 键复核四字段全值比对：section_version、produced_by、profile、depends
    /// 全集（path+fingerprint 逐对相等）。
    ca::Result<std::optional<std::vector<ca::u8>>, ProjectStoreError> lookupXref(
        const XrefCacheKey& key) const;

    /// @brief 写 xref 缓存（整索引重算是低频事件，单 section 单清单事务即时
    ///        落盘；高频批量场景归 flush 的合并事务面）。
    ///
    /// 序：writeSectionBytes 原子写 → 同族（xref- 词根）陈旧槽墓碑 → 一笔
    /// manifest 事务 upsert + removals。清单不可载拒绝事务（CP-1，零触盘）。
    ca::Result<void, ProjectStoreError> storeXref(const XrefCacheKey& key,
                                                  const ca::u8* payload, ca::usize payloadSize,
                                                  std::string_view createdAt);

    /// @brief 文本负载便捷重载。
    ca::Result<void, ProjectStoreError> storeXref(const XrefCacheKey&             key,
                                                  const ca::str::Utf8StringRef&   payload,
                                                  std::string_view                createdAt);

    //-- 条目级 decompile 缓存 ----------------------------------------------

    /// @brief 读一条条目级缓存。槽键 + 类名 + 条目 digest 全对上才算命中；
    ///        条目 digest 不同（类已编辑）按未命中。清单不可载 = Io 错上浮。
    ca::Result<std::optional<ca::str::Utf8String>, ProjectStoreError> lookupDecompile(
        const DecompileCacheSlot& slot, std::string_view className,
        const ca::str::Utf8StringRef& entryDigest) const;

    /// @brief 缓冲一条条目级反编译产物（驻内存 pending，随 flush 批量落盘；
    ///        RFC 风险 6 的合并 flush 形态——N 条目只付一次清单事务）。
    ///
    /// className 非空、entryDigest 合法形状即受理（同类名重复 put 以最后一次
    /// 为准）；未绑定 bundle 时 pending 仅内存（flush no-op）。槽键形状违约
    /// （digest 形状、producer 空）= Schema 错当场拒绝——写方 bug 不落盘。
    ca::Result<void, ProjectStoreError> putDecompile(const DecompileCacheSlot&    slot,
                                                     std::string_view             className,
                                                     const ca::str::Utf8StringRef& entryDigest,
                                                     std::string_view             source);

    /// pending 条目级记录数（flush 时机决策用）。
    ca::usize pendingDecompileCount() const { return pendingCount_; }

    /// @brief 合并 flush：按槽分组把 pending 记录并写进各自 section（已登记
    ///        槽先读旧记录合并、pending 同名覆盖），全部槽一笔 manifest 事务
    ///        提交；同族（同 cd8+e8 词根）陈旧槽随事务退役。未绑定/无 pending
    ///        = no-op。崩溃任一点旧清单完整，新文件归孤儿收敛（manifest last）。
    ca::Result<AnalysisCacheFlushReport, ProjectStoreError> flush(std::string_view createdAt);

private:
    struct PendingRecord
    {
        ca::str::Utf8String entryDigest;
        ca::str::Utf8String source;
    };
    struct PendingSlot
    {
        DecompileCacheSlot slot;
        /// 类名 → 记录（同名 put 以最后一次为准；flush 按类名字典序写出）。
        std::map<std::string, PendingRecord> records;
    };

    ca::Result<StoreManifest, ProjectStoreError> loadManifest() const;

    ca::str::Utf8String bundleDir_;  ///< 空 = 未绑定
    /// pending 分组：槽名 → 组。同槽 put 归并进同组（槽名由槽键确定性导出）。
    std::map<std::string, PendingSlot> pending_;
    ca::usize pendingCount_ = 0;
};

}   // namespace mjbase::project
