#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/core/result.hpp"
#include "libca/crypto/sha256.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/project/project_store.hpp"

namespace mjbase::project {

/// @file edit_ledger.hpp
/// @brief 编辑台账的行级数据模型：三种行（事务/锚/书签）的严格解析与确定性
///        写出、条目 digest 与容器 canonical digest 口径、活动链校验。
///
/// 设计定稿见 doc_ai/analysis-project-schema-v2.md §2/§3（上游 RFC 0001 §6.2/
/// §6.4）。行模型是纯数据层：不知道容器对象，只认 uri + digest + overlay 引用；
/// 重放与状态物料化由消费方（workspace）在其容器载体上执行。本模块与
/// project_store 同为 libca 叶子，不依赖 model。

/// @brief 台账行版本（行级自描述；行级演进不触清单版本，RFC 风险 1 缓解）。
inline constexpr ca::u32 kLedgerRowVersion = 1;

/// @brief 台账引擎自述串（section 的 produced_by；仅诊断用）。
inline constexpr const char* kLedgerEngineName = "mjbase-edit-ledger/1";

/// @brief 台账 section 名前缀与代际命名（设计文档 §3.1）。
///
/// 活动代 = 清单中 edit-ledger-<n> 的最大 n；edit-baseline-<n> 与同号 ledger
/// 配对（缺配对基线 = 基线取原始容器）。每次重写台账都换新代号，永不覆写
/// 已登记 section 的字节。
inline constexpr const char* kLedgerSectionPrefix   = "edit-ledger-";
inline constexpr const char* kBaselineSectionPrefix = "edit-baseline-";
inline constexpr const char* kOverlaySectionPrefix = "overlay-";

/// @brief 台账条目的操作类别。
enum class LedgerEntryOp : ca::u8
{
    Replace,  ///< uri 已存在，字节整条替换
    Add,      ///< uri 原不存在，新增（before 恒空串）
    Remove,   ///< uri 存在，整条删除（after 恒空串，无 payload）
};

/// @brief 事务内单条目的前后 digest 链记录（RFC §6.2 entries[]）。
struct LedgerEntry
{
    ca::str::Utf8String uri;       ///< 条目 URI（非空）
    ca::str::Utf8String before;    ///< 前态 digest；Add 恒空串
    ca::str::Utf8String after;     ///< 后态 digest；Remove 恒空串
    LedgerEntryOp       op = LedgerEntryOp::Replace;
    ca::str::Utf8String payloadRef;  ///< overlay section 名（overlay-<txn>-<seq>）；
                                     ///< Remove 恒空串

    /// @brief 深拷贝（行模型含 move-only 串，值拷贝走 clone；move 语义照常）。
    LedgerEntry clone() const
    {
        LedgerEntry copy;
        copy.uri        = uri.clone();
        copy.before     = before.clone();
        copy.after      = after.clone();
        copy.op         = op;
        copy.payloadRef = payloadRef.clone();
        return copy;
    }
};

/// @brief 一条已提交编辑事务（row=txn）。
struct LedgerTransaction
{
    ca::u64             txn = 0;       ///< 事务号 ≥ 1，全局单调不复用
    ca::u64             parent = 0;    ///< 父事务号；0 = 链根（基线态）
    ca::str::Utf8String label;         ///< 编辑来源标签（可空）
    ca::str::Utf8String baseContainer; ///< 编辑基线容器 digest（"sha256:"+64hex）
    std::vector<LedgerEntry> entries;  ///< 按 uri 字典序（写出定序）
    ca::str::Utf8String producedBy;    ///< 生产者自述串（非空）
    ca::str::Utf8String createdAt;     ///< ISO-8601 串（非空；格式不校验）

    /// @brief 深拷贝。
    LedgerTransaction clone() const
    {
        LedgerTransaction copy;
        copy.txn          = txn;
        copy.parent       = parent;
        copy.label        = label.clone();
        copy.baseContainer = baseContainer.clone();
        copy.entries.reserve(entries.size());
        for (const auto& entry : entries) copy.entries.push_back(entry.clone());
        copy.producedBy = producedBy.clone();
        copy.createdAt  = createdAt.clone();
        return copy;
    }
};

/// @brief 压缩锚行（row=anchor）：基线推进记录与补链（设计文档 §5.5）。
struct LedgerAnchor
{
    ca::u64             txn = 0;             ///< 基线对应的事务号（state(txn) = 基线）
    ca::str::Utf8String baselineSection;     ///< 基线 section 名（edit-baseline-<g>）
    ca::str::Utf8String container;           ///< 新基线容器 digest（= 基线 section
                                             ///< content_hash，一口径两处验证）
    ca::u64             previousTxn = 0;     ///< 上一锚事务号（0 = 首次压缩，无 previous）
    ca::str::Utf8String previousContainer;   ///< 上一基线 digest（补链；首次压缩空串）
    ca::u64             compactedTxns = 0;   ///< 被压缩区间的事务条数
    ca::str::Utf8String createdAt;           ///< ISO-8601 串（非空）

    /// @brief 深拷贝。
    LedgerAnchor clone() const
    {
        LedgerAnchor copy;
        copy.txn               = txn;
        copy.baselineSection   = baselineSection.clone();
        copy.container         = container.clone();
        copy.previousTxn       = previousTxn;
        copy.previousContainer = previousContainer.clone();
        copy.compactedTxns     = compactedTxns;
        copy.createdAt         = createdAt.clone();
        return copy;
    }
};

/// @brief 书签行（row=bookmark）：命名指针指向某事务（savepoint 换底形态）。
struct LedgerBookmark
{
    ca::str::Utf8String name;      ///< 书签名（非空）
    ca::u64             txn = 0;   ///< 指向的事务号
    ca::str::Utf8String createdAt; ///< ISO-8601 串（非空）

    /// @brief 深拷贝。
    LedgerBookmark clone() const
    {
        LedgerBookmark copy;
        copy.name      = name.clone();
        copy.txn       = txn;
        copy.createdAt = createdAt.clone();
        return copy;
    }
};

/// @brief 台账行（三型判别联合；同时只有一型有效）。
struct LedgerRow
{
    enum class Kind : ca::u8
    {
        Transaction,
        Anchor,
        Bookmark,
    };
    Kind              kind = Kind::Transaction;
    LedgerTransaction transaction;
    LedgerAnchor      anchor;
    LedgerBookmark    bookmark;
};

/// @brief 严格解析单行台账 JSON（行级字段严格：未知/重复字段、类型错、digest
///        形状错、未知 payload kind 即拒载）。
ca::Result<LedgerRow, ProjectStoreError> parseLedgerRow(std::string_view line,
                                                        ca::usize        lineNumber);

/// @brief 确定性序列化单行（紧凑单行 JSON；字段序固定）。
ca::Result<ca::str::Utf8String, ProjectStoreError> serializeLedgerRow(const LedgerRow& row);

/// @brief 解析整段台账 jsonl 文本（空行跳过；任一行违约即整段拒载，零半登记）。
ca::Result<std::vector<LedgerRow>, ProjectStoreError> parseLedgerRows(
    const ca::str::Utf8StringRef& text);

/// @brief 确定性序列化行集合为 jsonl 文本（每行尾 LF；行序 = 输入序）。
ca::Result<ca::str::Utf8String, ProjectStoreError> serializeLedgerRows(
    const std::vector<LedgerRow>& rows);

/// @brief 活动链结构校验（装载期）：锚行（若有）必须在行首且 txn 为最小；事务
///        parent 必须指向段内更早事务或 0；txn 无重复；书签 txn 必须指向段内
///        事务。违反即 Section 错（带行定位）。
ca::Result<void, ProjectStoreError> validateLedgerChain(const std::vector<LedgerRow>& rows);

/// @brief 段内行集合的确定性排序：锚行恒在行首，事务行按 txn 升序，书签行按
///        名字典序排其后。装载后的内存序与写出序由此统一。
void sortLedgerRows(std::vector<LedgerRow>& rows);

/// @brief overlay section 名（overlay-<txn>-<seq>；事务号 + 段内序号）。
ca::str::Utf8String overlaySectionName(ca::u64 txn, ca::u32 seq);

/// @brief UTF-8 文本 → 引擎自有串（std::string/string_view 入口；消费者换型用）。
ca::str::Utf8String copyLedgerText(std::string_view text);

/// @brief digest 形状校验："sha256:" + 64 位十六进制（大小写不限，载入归一）。
bool isValidLedgerDigest(std::string_view text);

//-- digest 口径 -----------------------------------------------------------

/// @brief 条目字节 digest：精确字节 SHA-256（"sha256:"+64hex；与 section
///        content_hash 同一口径）。
inline ca::str::Utf8String ledgerDigestOfBytes(const ca::u8* data, ca::usize size)
{
    return sectionContentHash(data, size);
}

/// @brief 容器 canonical 序列化器（设计文档 §1.6）。
///
/// 口径：条目按 uri 字典序逐项拼接 len(uri, u64 小端) + uri 字节 +
/// len(payload, u64 小端) + payload 字节。容器 digest = 对累计字节求
/// SHA-256 加前缀；基线 section 字节 = 同一序列本身，故基线 digest 恒等于其
/// section 的 content_hash。调用方负责按字典序投喂（乱序投喂 digest 不同——
/// 口径不含排序容错，写出层定序）。
class ContainerDigestBuilder
{
public:
    /// @brief 投喂一个条目（uri + 载荷字节；uri 应按调用方排序保证字典序）。
    void add(std::string_view uri, const ca::u8* payload, ca::usize payloadSize);

    /// @brief 结束并产出容器 digest（"sha256:"+64hex；不可续喂）。
    ca::str::Utf8String finish();

private:
    void feed(const ca::u8* data, ca::usize size);
    void feedLength(ca::usize value);

    ca::crypto::SHA256 digest_;
    ca::usize          entries_ = 0;
};

/// @brief 容器 canonical 序列化字节（一次性装配；基线 section 落盘形态）。
///
/// 返回字节向量而非文本串：载荷字节任意（opaque），canonical 序列化不是
/// UTF-8 文本。
std::vector<ca::u8> serializeContainerEntries(
    const std::vector<std::pair<ca::str::Utf8String, std::vector<ca::u8>>>& sortedEntries);

/// @brief 遍历 canonical 容器字节（§1.6 口径；visitor 返回 false 提前终止）。
///        截断/长度越界 = Section 错（基线字节已过 content_hash 校验，违约即
///        写方 bug 或存储损坏，fail-closed）。
ca::Result<void, ProjectStoreError> forEachContainerEntry(
    const ca::u8* data, ca::usize size,
    const std::function<bool(std::string_view uri, const ca::u8* payload,
                             ca::usize payloadSize)>& visitor);

/// @brief 定位 canonical 容器字节中的单条目；命中写入 @p payload 返回 true，
///        未命中返回 false（格式违约 = Section 错）。
ca::Result<bool, ProjectStoreError> findContainerEntry(const ca::u8* data, ca::usize size,
                                                       std::string_view             uri,
                                                       std::vector<ca::u8>& payload);

}   // namespace mjbase::project
