#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/project/edit_ledger.hpp"
#include "mjbase/project/project_store.hpp"

namespace mjbase::project {

/// @file edit_ledger_store.hpp
/// @brief 编辑台账引擎：活动链 + 书签 + pending overlay 的内存态，及其在
///        ProjectStore bundle 上的代际化落盘（flush）、压缩（手动/自动阈值）
///        与崩溃安全（manifest last、只删非活动代）。
///
/// 设计定稿见 doc_ai/analysis-project-schema-v2.md §2..§5（上游 RFC 0001 §6.2/
/// §6.4）。引擎工作在 uri + digest + 字节面：不知道容器对象，重放与状态物料化
/// 由消费方在其载体上执行；引擎负责链完整性、载荷寻址、落盘协议与压缩不变量。
/// 未绑定 bundle 时引擎是纯内存台账（flush 为 no-op），供无工程会话使用。

/// @brief 事务载荷字节（pending overlay 的一道交付；ref 由 append 分配）。
struct LedgerOverlayPayload
{
    ca::str::Utf8String    ref;    ///< overlay section 名（flush 后即 section 名）
    std::vector<ca::u8>    bytes;  ///< 条目 after 字节
};

/// @brief 追加事务的输入面（digest 链由调用方从工作副本实际字节算出；引擎做
///        链上可判部分的校验，见 append）。字符串字段 owning：调用方常以临时
///        量传入（如 rfc3339UtcNow() 的返回值），view 会在分号处悬垂，linux
///        实证为栈复用后的 invalid UTF-8。
struct LedgerAppendInput
{
    std::vector<LedgerEntry>          entries;    ///< 按 uri 字典序
    std::vector<LedgerOverlayPayload> payloads;  ///< 覆盖 entries 引用的全部 ref
    std::string                       label;      ///< 可空
    std::string                       producedBy; ///< 非空
    std::string                       createdAt;  ///< 非空
};

/// @brief flush 报告。
struct LedgerFlushReport
{
    bool                    flushed = false;      ///< false = 无 pending，零动作
    ca::u32                 generation = 0;       ///< 落盘后的活动代
    ca::u64                 transactions = 0;     ///< 本次落盘的活动链事务条数
    ca::u64                 ledgerBytes = 0;      ///< 台账 section 字节数
    std::vector<ca::str::Utf8String> published;   ///< 本次发布的 section 名（含台账）
    std::vector<ca::str::Utf8String> released;    ///< 本次随清理事务删除的 section 名
};

/// @brief 手动压缩参数（设计文档 §5.2）。
struct LedgerCompactRequest
{
    /// 保留最近 N 个书签（按创建序）；上限值 = 全保留。被裁书签列名进报告。
    ca::u64 keepRecentBookmarks = static_cast<ca::u64>(-1);
    /// 压缩后承诺保留的 undo 深度；0 = 压到书签约束允许的最晚处。
    ca::u64 keepUndoDepth = 0;
};

/// @brief 压缩报告（手动/自动共用；skippedReason 非空 = 未执行）。
struct LedgerCompactReport
{
    bool                    executed = false;
    ca::str::Utf8String     skippedReason;       ///< executed=false 时非空
    ca::u64                 retentionTxn = 0;    ///< 保留点（新基线对应事务号）
    ca::u64                 droppedTransactions = 0;  ///< 被压缩区间事务条数
    ca::str::Utf8String     baselineSection;     ///< 新基线 section 名
    ca::str::Utf8String     baselineDigest;      ///< 新基线容器 digest
    std::vector<ca::str::Utf8String> droppedBookmarks;  ///< 显式丢弃的书签名（手动路径）
    std::vector<ca::str::Utf8String> released;          ///< 旧台账/旧基线/失引 overlay
};

/// @brief 编辑台账引擎（move-only 值语义；绑定时持有 StoreLock 句柄生命线）。
class EditLedgerStore
{
public:
    /// @brief 缺省空引擎（仅作持有人成员缺省态；基线 digest 为空、链为空）。
    ///
    /// 链操作前必须经 createInMemory/openBundle 赋值换底——消费方（workspace）
    /// 在打开主资源时即重置；bound()/baseContainerDigest() 空即可辨此态。
    EditLedgerStore() = default;
    ~EditLedgerStore();
    EditLedgerStore(EditLedgerStore&& other) noexcept;
    EditLedgerStore& operator=(EditLedgerStore&& other) noexcept;
    EditLedgerStore(const EditLedgerStore&)            = delete;
    EditLedgerStore& operator=(const EditLedgerStore&) = delete;

    /// @brief 纯内存台账（flush/自动压缩 no-op；baseContainerDigest 非空且合法）。
    static ca::Result<EditLedgerStore, ProjectStoreError> createInMemory(
        const ca::str::Utf8StringRef& baseContainerDigest,
        const LedgerEngineConfig&     config = {});

    /// @brief 绑定既有 bundle 并装载活动代台账（单写者锁随对象持有）。
    ///
    /// bundle 缺 store.json 等"清单不可载"态按既有纪律拒绝（台账引擎不落任何
    /// 新清单）。无台账 section = 全新会话（活动链为空、基线取 baseContainer
    ///Digest 所指的原始容器）。配置取 manifest.ledgerConfig（工程配置持久面），
    /// 缺省回落 @p config。基线 section 存在时核对其 content_hash 与锚行
    /// container（一口径两处验证，§1.6）。
    static ca::Result<EditLedgerStore, ProjectStoreError> openBundle(
        const ca::str::Utf8StringRef& bundleDir, const ca::str::Utf8StringRef& baseContainerDigest,
        const LedgerEngineConfig& config = {});

    //-- 链查询 ---------------------------------------------------------------

    /// 当前基线容器 digest（压缩后随锚推进）。
    const ca::str::Utf8String& baseContainerDigest() const { return baseContainerDigest_; }

    /// 链基点事务号（锚事务或 0）。
    ca::u64 baseTxn() const { return anchor_.has_value() ? anchor_->txn : 0; }

    /// 头事务号（0/baseTxn = 基线态）。
    ca::u64 headTxn() const { return head_; }

    /// 下一个将分配的事务号（消费方装配 overlay 引用名时需要：ref 含事务号，
    /// 分配发生在 append）。
    ca::u64 nextTxnId() const { return nextTxnId_; }

    /// @brief 会话重置（打开新主资源）：活动链/书签/头/锚清零，换基线 digest。
    ///
    /// 绑定态保留 bundle 目录、锁与代际计数——旧容器的已落盘台账历史仍在磁盘
    /// 上（更早代），下一次 flush 以新链写新代；pendingReleases_ 保留待清理。
    /// 未落盘的旧会话事务随重置丢弃（调用方须先 flush，见 workspace 换底契约）。
    void resetSession(const ca::str::Utf8StringRef& baseContainerDigest);

    /// 能否 undo（头不在基点）。
    bool canUndo() const { return head_ != baseTxn(); }

    /// undo 深度 = 活动链上 (基点, 头] 的事务条数。
    ca::u64 undoDepth() const;

    /// 事务行查询；不在活动链返回 nullptr。
    const LedgerTransaction* transaction(ca::u64 txn) const;

    /// 基点到 @p txn（含）的事务号序列（链序；txn == 基点 → 空）。
    std::vector<ca::u64> chainTo(ca::u64 txn) const;

    /// @p uri 在 @p atOrBefore（含）之前最近一次 touch 的事务号（0 = 无）。
    ca::u64 lastTouchOf(std::string_view uri, ca::u64 atOrBefore) const;

    /// 活动链事务行总数（台账体量指标）。
    ca::u64 transactionCount() const;

    /// 活动链行的序列化字节数（阈值指标）。
    ca::u64 ledgerBytes() const;

    /// @brief 载荷字节：内存 pending 优先，已落盘走清单校验读（§5.2 先 size 后
    ///        hash）。未绑定且非 pending = NotFound。
    ca::Result<std::vector<ca::u8>, ProjectStoreError> payloadBytes(
        const ca::str::Utf8StringRef& ref) const;

    /// 是否存在基线 section（压缩过）。
    bool hasBaselineSection() const { return anchor_.has_value(); }

    /// 基线 section 字节（canonical 序列化；未压缩 = NotFound——基线取原始容器，
    /// 由消费方回落）。
    ca::Result<std::vector<ca::u8>, ProjectStoreError> baselineBytes() const;

    const ca::str::Utf8String& baselineSectionName() const
    {
        return anchor_.has_value() ? anchor_->baselineSection : empty_;
    }

    //-- 变更 -----------------------------------------------------------------

    /// @brief 追加事务（parent = 当前头，放弃头之后的尾分支及其书签——对齐
    ///        旧实现 commit 清 redo 栈；事务号单调不复用）。
    ///
    /// 引擎校验（Section 错当场拒绝）：entries 按 uri 字典序且不重复；digest
    /// 形状与 op 相容（行级纪律）；链上可判部分——条目 before 必须等于该 uri
    /// 上一次 touch 的 after（无 touch 的基线侧由消费方重放时以实际字节核对）；
    /// payloads 覆盖全部引用的 ref。
    ca::Result<LedgerTransaction, ProjectStoreError> append(const LedgerAppendInput& input);

    /// @brief 头指针回退/前进（undo/redo 用；不记新事务）。
    ///
    /// @p txn 必须是活动链上的事务号或基点。前进侧由消费方保证只走到其 redo
    /// 目标（被放弃分支的行在 append 时已删除，前进目标天然只剩存活链）。
    ca::Result<void, ProjectStoreError> moveHead(ca::u64 txn);

    /// @brief 设书签（savepoint 换底形态）；重名覆盖，txn = 当前头。
    ///        createdAt 按引用收临时量（生命周期延展到调用结束）。
    ca::Result<void, ProjectStoreError> setBookmark(std::string_view    name,
                                                    const std::string&  createdAt);

    /// 书签查询（创建序；重名覆盖后保持原位）。
    std::vector<LedgerBookmark> bookmarks() const;

    /// 按名查书签；未命中 nullptr。
    const LedgerBookmark* bookmark(std::string_view name) const;

    /// 摘除书签；未命中 false。
    bool dropBookmark(std::string_view name);

    //-- 落盘 -----------------------------------------------------------------

    /// 是否绑定 bundle。
    bool bound() const { return !bundleDir_.is_empty(); }

    /// 是否有未落盘事务。
    bool hasPending() const { return pendingTxns_ > 0; }

    /// 当前活动代（0 = 尚无落盘台账）。
    ca::u32 generation() const { return generation_; }

    /// @brief flush（设计文档 §3.2，manifest last）：落 overlay sections → 新代
    ///        台账 section → manifest 事务 upsert → 清理事务删除待释放（只删
    ///        非活动代）→ 释放内存 pending。未绑定 / 无 pending 且无待释放 =
    ///        no-op。崩溃任一点旧代完整（§3.2/§3.4）；重试同字节幂等。
    ca::Result<LedgerFlushReport, ProjectStoreError> flush(const std::string& createdAt);

    //-- 压缩 -----------------------------------------------------------------

    /// 自动压缩阈值判定（flush/savepoint 时机查视；§5.3）。
    bool shouldAutoCompact() const;

    /// @brief 保留点计算（§5.1）：clamp(head − keepUndoDepth, 基点,
    ///        最老存活书签事务号)；自动路径 keepUndoDepth 取配置 min_undo_depth、
    ///        书签全保留。返回值 ≤ 基点 = 不可推进（自动路径跳过）。
    ca::u64 retentionPoint(const LedgerCompactRequest& request, bool automatic,
                           std::vector<ca::str::Utf8String>* droppedBookmarks) const;

    /// @brief 压缩（§5.4）：物化基线由消费方按 retentionPoint 预先完成并以
    ///        canonical 字节交入；引擎原子写新基线 + 新代台账 → manifest 换代
    ///        → 旧代/失引 overlay 入待释放并随即清理。
    ///
    /// 自动路径书签零失效（保留点不越过最老存活书签）；手动路径可显式丢弃更
    /// 早书签（列名进报告）。undo 深度承诺按 §5.1 形式化。
    ca::Result<LedgerCompactReport, ProjectStoreError> compact(
        const LedgerCompactRequest& request, const std::vector<ca::u8>& baselineCanonicalBytes,
        std::string_view createdAt, bool automatic);

    const LedgerEngineConfig& config() const { return config_; }

private:
    ca::Result<void, ProjectStoreError> cleanupSuperseded(std::vector<ca::str::Utf8String>* released,
                                                          std::string_view createdAt);
    /// 引擎名族 section 的活动代登记与待释放清点（open 期）。
    void reclaimEngineSections(const StoreManifest& manifest);

    ca::str::Utf8String ledgerSectionName(ca::u32 generation) const;
    ca::str::Utf8String baselineSectionNameOf(ca::u32 generation) const;

    ca::str::Utf8String    bundleDir_;       ///< 空 = 纯内存
    LedgerEngineConfig     config_;
    ca::str::Utf8String    baseContainerDigest_;  ///< 当前基线 digest（随锚推进）
    std::optional<LedgerAnchor> anchor_;         ///< 压缩锚（无 = 基线取原始容器）
    std::vector<LedgerTransaction> txns_;        ///< 活动链（txn 升序 = 链序）
    ca::u64                head_ = 0;            ///< 头事务号
    ca::u64                nextTxnId_ = 1;       ///< 事务号单调分配
    std::vector<LedgerBookmark> bookmarkOrder_;  ///< 创建序（重名覆盖保持原位）
    std::map<std::string, std::vector<ca::u8>> pendingOverlays_;  ///< ref → after 字节
    ca::u64                pendingTxns_ = 0;     ///< 未落盘事务计数
    ca::u32                generation_ = 0;      ///< 活动代（0 = 尚无落盘台账）
    std::vector<ca::str::Utf8String> pendingReleases_;  ///< 待删除的非活动代 section
    std::unique_ptr<StoreLock> lock_;       ///< 绑定期单写者锁生命线
    ca::str::Utf8String    empty_;
};

}   // namespace mjbase::project
