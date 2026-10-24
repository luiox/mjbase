#pragma once

#include <cstdio>
#include <optional>
#include <vector>

#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::project {

/// @file project_store.hpp
/// @brief 分析工程存取层（ProjectStore）底层原语：bundle 常量/目录骨架、
///        section 命名校验、store.json 严格解析与确定性写出、section 内容
///        原子读写与 hash/size 校验原语。
///
/// 设计源：`doc_ai/analysis-project-store-design.md`（R45 评审通过稿），
/// 实现卡 **C1**（§12 卡表：bundle 常量/目录骨架、
/// section 命名校验、store.json 严格解析与确定性写出；验收 §11-1 骨架、
/// §11-7）+ **C2**（section 层可靠读写原语：单 section 原子写 staged tmp +
/// 换名、content_hash/byte_size 双登记、hash 校验 Damaged 判定材料；
/// R52/CP-1 降级触发预埋、CP-2/CP-3 顺路修）+ **C3**（store.json manifest
/// 事务：读-改-写事务语义、staged tmp + 单次换名 + 重试（对齐 §8.1 既有
/// 口径）、CP-1 钩子的第一个消费方、§9.3 收容段不可改写、R53-N1 顺路件）+
/// **C4**（§8.2 恢复扫描：sections/+store.json+tmp/ 全量盘点分类账
/// （Ok/Damaged/孤儿两档）、CP-1 终裁落地（Io 入降级触发面）、扫描只读——
/// 恢复动作默认只报告不改动）+ **C5**（§7.1 单写者锁 store.lock 全路径：
/// 独占创建获取、PID fail-closed 探测、死锁接管恰好一次重试、不可解析
/// fail-closed、stealLock 人工抢占；§12 卡表——ProjectStore::open 尚未
/// 落地，acquireStoreLock 即 open 的锁接缝/接管点，R45/P7）。后续卡在此
/// 之上叠加：实体接线/裸文件模式/upgradeFromFile、读路径
/// （readRecords/readJson/readBytes）、depends/stale/inputSetFingerprint
/// （C6）。R62（C4 T3 评审）两条非阻塞处方随本卡落地：**N1** = §8.2-4 flush
/// 后孤儿 prune 的动作面原语（pruneOrphanSections，工作清单现扫现验——
/// exists/可读验证失败即跳过并诊断，绝不带病删）；**N2** = 收容段词根匹配
/// 平台分叉（Windows 大小写不敏感对齐 Win32 路径语义，POSIX 保持敏感）。
///
/// 与 #1096 的零破坏边界（§4.1/§4.3，定案 D2 清单分治）：bundle 根下
/// `project.json`（实体事实源，#1096 冻结 v1）与 `store.json`（引擎清单，
/// 本模块私有 schema）分治——实体字节永远只经 analysis_project.hpp 的 API
/// 通行，#1096 旧读者读 bundle 内实体照常工作。

//-- bundle 常量（§3.2）----------------------------------------------------

/// @brief store.json kind 标记。与 project.json 的 "mjt-analysis-project"
///        互斥：双向喂错文件都拒载（§4.2，#1096 决策 1 同款自描述兜底）。
inline constexpr const char* kProjectStoreKind = "mjt-project-store";

/// @brief store.json 当前版本。不识别的版本整文件拒载（对齐 #1096 版本纪律；
///        打开期该拒载走 §8.2-2 的全孤儿降级，禁止自动 prune——P1 红线）。
inline constexpr ca::u32 kProjectStoreFileVersion = 1;

/// @brief 本引擎自述串（store.json 的 engine 字段；仅诊断用，读者不解释）。
inline constexpr const char* kProjectStoreEngine = "mbase-project-store/1";

/// @brief bundle 内文件/目录名（§3.2 布局）。目录后缀 `.mjt-project` 是 CLI
///        层约定，引擎不校验目录名（§3.2）。
inline constexpr const char* kProjectStoreEntityName      = "project.json";  ///< 实体事实源（#1096 冻结 v1）
inline constexpr const char* kProjectStoreManifestName    = "store.json";    ///< 引擎清单（本模块读写）
inline constexpr const char* kProjectStoreLockName        = "store.lock";    ///< 单写者锁（C5：§7.1）
inline constexpr const char* kProjectStoreSectionsDirName = "sections";      ///< section 内容文件
inline constexpr const char* kProjectStoreTmpDirName      = "tmp";           ///< 事务暂存区（恒在 bundle 内，同卷 MUST §8.1/R45-P6）

/// @brief store.lock 的缺省 owner 自述串（§7.1 内容示例口径；acquireStoreLock
///        可传自定义串覆盖——上层产品面用它写明开锁场景）。
inline constexpr const char* kProjectStoreLockOwnerDefault = "mjt project open";

//-- 错误模型（§6.1）-------------------------------------------------------

/// @brief ProjectStore 错误类别。
///
/// C1 的映射口径（C4 恢复扫描按"清单整笔不可载"路由降级，见下方
/// isManifestUnloadableKind——R52/CP-1：触发面 = Open ∪ 解析期 Schema
/// 整笔拒载，不能只认 Open）：
/// - Open：store.json 缺失 / JSON 语法不可读 / root 非对象 / kind·版本不识别
///   / bundle 结构缺件——即"清单整体不可载"；
/// - Schema：manifest/section 元数据违约（含命名非法、未知条目字段、
///   duplicate-key、类型/形状错、depends 形状错）——同样整笔拒载（零半登记
///   纪律下，解析期任一 Schema 错即整文件拒载），因此与 Open **同权**构成
///   §8.2-2 的降级触发（打开期降级为全孤儿：只报告 + 永久保留，禁止自动
///   删除）；
/// - Section：section 内容损坏（hash 失配/文件缺失，C2 判定原语；行解析
///   失败/版本过新，读路径落地）；
/// - Io：文件系统失败（含换名重试耗尽；清单**读取**失败同样属 Io——CP-1
///   **C4 终裁**：读取失败入 §8.2-2 降级触发面，见 isManifestUnloadableKind
///   的裁定理由）；Lock：锁被持有（C5 落地）。
enum class ProjectStoreErrorKind : ca::u8
{
    Open,    ///< 路径不存在/形态非法/kind·版本不识别/bundle 结构缺件
    Lock,    ///< 锁被活进程持有（§7.1；消息含 owner pid 与 created_at）或
             ///< 锁内容不可解析按活锁 fail-closed（R45/P5）
    Schema,  ///< manifest/section 元数据违约
    Section, ///< section 内容损坏：hash 失配/行解析失败/版本过新
    Io,      ///< 文件系统失败
};

/// @brief §8.2-2 降级触发判定（R52/CP-1 预埋；C3 manifest 事务首消费，
///        **C4 终裁在本注释落地**：Io 入触发面）。
///
/// 清单"整笔不可载"= Open ∪ 解析期 Schema 整笔拒载 ∪ **Io 读取失败**。
/// C2/C3 期的暂定口径（Io 排除："不可读 ≠ 不可载"）由 C4 终裁**反转**。
/// 裁定理由（§8.2 恢复扫描面对的不对称风险）：
/// - 把**可载**误判成不可载（本裁定的保守侧）：后果只是收紧——现存文件按
///   §8.2-2 走降级孤儿（只报告、永不自动 prune）；清单事后恢复可载即一切
///   如常，无任何数据损失面；
/// - 把**读不出**当成可载（被否决的排除侧）：现存 section 文件会被定性为
///   §8.2-4"本引擎半提交遗留"（首次成功 flush 后可自动 prune）——而此刻
///   清单字节根本没被读到，它登记的可能正是本引擎不可重算乃至用户仅有的
///   数据（§9.3）。**宁可错判不可误 prune**：与 §7.2 PID 探测同一 fail-closed
///   句式——证明不了对方已死就不得动它的锁；证明不了清单可载，就不得把
///   section 字节归入可自动删除的那一类。
///
/// 适用面（注释纪律）：本函数只对 store.json 的**读取/解析结果**判定
/// （readStoreManifestFile / parseStoreManifest* 返回的错误 kind）；其他
/// 场景的 Io（如提交期换名重试耗尽）不经过本判定，不因此获得降级语义。
/// 各消费点把错误 kind **原样上浮不吞并**——终裁合流的是处置政策面，
/// 不改写错误类别（Open/Schema/Io 各自可辨）。
///
/// 消费纪律（C3 已落，C4 沿用）：判定为"可载触发面"的状态下，任何写路径
/// 都**不得**换新清单——降级孤儿态下覆写 store.json 会把现存 section 文件
/// 重注册为"清单可载"场景的 §8.2-4 孤儿（flush 后可自动 prune），等于绕开
/// P1 红线。
inline bool isManifestUnloadableKind(ProjectStoreErrorKind kind)
{
    return kind == ProjectStoreErrorKind::Open || kind == ProjectStoreErrorKind::Schema ||
           kind == ProjectStoreErrorKind::Io;
}

/// @brief ProjectStore 诊断（形态对齐 AnalysisProjectError；错误消息自带
///        上下文：section 名、期望值 vs 实际值）。
struct ProjectStoreError
{
    ProjectStoreErrorKind kind = ProjectStoreErrorKind::Io;
    ca::str::Utf8String   message;
    ca::str::Utf8String   section;  ///< section 域错误时非空，否则空
};

//-- section 清单条目模型（§4.2 store.json v1）------------------------------

/// @brief section 内容格式（§5.1）。文件扩展名由 format 决定：
///        json → .json、jsonl → .jsonl、opaque → .bin。
enum class SectionFormat : ca::u8
{
    Json,   ///< 整文档型：单个 JSON 值，pretty 2 空格写出
    Jsonl,  ///< facts/事件型默认：每行一个 JSON 对象 + LF，末行必有换行
    Opaque, ///< 保留型：任意字节原样保存，引擎不解释内容
};

/// @brief section 对输入的依赖（§5.3）：{path, fingerprint} 从 project.json
///        inputs 拷贝；登记一致性校验（依赖 path ∈ inputs 等）在 C6/flush。
struct SectionDep
{
    ca::str::Utf8String path;         ///< 输入登记路径（非空）
    ca::str::Utf8String fingerprint;  ///< 生产时刻该 input 的登记指纹（非空）
};

/// @brief store.json sections[] 条目（§4.2 v1）。
///
/// 字段集**严格**：解析期未知字段即整文件拒载（引擎自产封闭记录，严格性
/// 当场抓住 writer bug；新增条目字段必须升 store 主版本，§4.2 演进规则）。
/// 与 §6.2 SectionInfo 的关系：C4 的读端视图在 C3/C4 落地时由本条目扩展
/// state（Ok/Damaged/Orphan）而来，字段命名保持同构。
struct StoreSectionEntry
{
    ca::str::Utf8String name;              ///< §3.3 命名语法，bundle 内唯一
    SectionFormat       format = SectionFormat::Jsonl;
    ca::str::Utf8String formatTag;         ///< 清单原载 format 串（收容场景原样回写，§9.3）
    bool                formatRetained = false;  ///< true = format 值本引擎不识别，按 opaque 收容（§4.2/§9.3）
    ca::u32             sectionVersion = 1;      ///< 生产者私有版本，≥ 1；读者拒读 > 自身支持值（C4）
    ca::str::Utf8String contentHash;              ///< "sha256:" + 64 hex，对 section 文件精确字节
    ca::u64             byteSize = 0;             ///< 同一字节口径的长度
    bool                hasRecordCount = false;   ///< jsonl 必有；json/opaque 禁止；收容条目透传保留
    ca::u64             recordCount = 0;          ///< 仅 jsonl 语义有效
    std::optional<ca::str::Utf8String> profile;   ///< 对内容有影响的构建选项串（引擎不解释
                                                  ///< 语义）。空串归一化为不携带（R52/CP-3）：
                                                  ///< 解析期 `"profile": ""` 视同未设置
                                                  ///< （optionalString 口径）、写出期不携带
                                                  ///< 该键——空串 ≡ 缺省，两侧同义
    std::vector<SectionDep> depends;              ///< 可空；写出按 path 字典序
    ca::str::Utf8String producedBy;               ///< 生产者自述串（非空）
    ca::str::Utf8String createdAt;                ///< ISO-8601 串（格式引擎不校验，非空）
    ca::str::Utf8String updatedAt;                ///< 同上
};

/// @brief store.json 清单（§4.2 v1 顶层）。
///
/// 演进规则：顶层字段只增不删不改义，读者**忽略**未知顶层字段；重复键一律
/// 拒载（ca::json 解析纪律）。diagnostics 是解析期非致命发现的**报告通道**
/// （§4.2"记 openDiagnostics"的 C1 承接点：未知 format 收容等；C4 的
/// openDiagnostics() 由它与恢复扫描发现汇流），不属于清单内容、不参与写出。
struct StoreManifest
{
    ca::u32                        version = kProjectStoreFileVersion;
    ca::str::Utf8String            engine;      ///< 产出引擎自述串（仅诊断用）。空串归一化
                                                ///< 为不携带（R52/CP-3）：解析期
                                                ///< `"engine": ""` 视同未设置、写出期不
                                                ///< 携带该键——空串 ≡ 缺省，两侧同义
    std::vector<StoreSectionEntry> sections;    ///< 解析期按原载顺序；写出按 name 字典序
    std::vector<ca::str::Utf8String> diagnostics;  ///< 解析期非致命发现（未知 format 收容等）
};

//-- section 命名校验（§3.3）------------------------------------------------

/// @brief 校验 section 名（§3.3，文件名的唯一来源）。
///
/// 语法 `^[a-z0-9][a-z0-9._-]{0,63}$` 且不以 '.' 结尾（Windows 尾点陷阱）；
/// 首个 '.' 之前的段不得为 Windows 保留设备名 {con, prn, aux, nul,
/// com1..com9, lpt1..lpt9}（大小写不敏感）。非法名返回 Schema 错
/// （section 域错误，error.section = 该名），**不静默改写**。
ca::Result<void, ProjectStoreError> validateSectionName(const ca::str::Utf8StringRef& name);

/// @brief format → 文件扩展名（含 '.'）：.json / .jsonl / .bin（§3.3）。
const char* sectionFileExtension(SectionFormat format);

/// @brief section 文件名：`<name>.<ext>`（§3.2，sections/ 目录内一层）。
ca::str::Utf8String sectionFileName(const ca::str::Utf8StringRef& name, SectionFormat format);

//-- 路径判别与目录骨架（§3.2/§3.3，create/open 的目录判别）------------------

/// @brief 存储路径形态判别（§9.1 行为表的入口）。
enum class StorePathKind : ca::u8
{
    Missing,   ///< 不存在
    Directory, ///< 目录（bundle 形态）
    BareFile,  ///< 普通文件（#1096 v1 裸文件；只读降级在 C2 落地）
    Other,     ///< 存在但既非目录也非普通文件（符号链接等，拒绝）
};

/// @brief 判别路径形态（error_code 重载，不抛异常）。
StorePathKind classifyStorePath(const ca::str::Utf8StringRef& path);

/// @brief 新建 bundle 目录骨架（§3.2；目录骨架本身不带锁——持锁生命线是
///        StoreLock 句柄（C5 的 acquireStoreLock），由 ProjectStore::create
///        接线时持有（§6.3 持锁即开门；该接线归实体接线卡）。
///
/// 前置：目录不存在，或存在且为空（§6.2 create 语义；非空即 Open 错，消息
/// 含首个现存条目）。成功后布局：sections/、tmp/ 目录 + store.json（v1 空
/// 清单，确定性字节）+ project.json（空实体，字节 = writeAnalysisProjectText
/// 产出，§4.3）。初始两文件走 plain 写（目录全新为空，无旧内容可破坏；不用
/// atomic_write_text 以免其同目录临时文件残留于 bundle 根——R45/P4 同一关切，
/// 崩溃残留收敛到 §8.2 既有处置）。
ca::Result<void, ProjectStoreError> createBundleSkeleton(const ca::str::Utf8StringRef& dir);

//-- store.json 严格解析与确定性写出（§4.2）----------------------------------

/// @brief 严格解析 store.json 文本（§4.2 v1）。
///
/// 纪律（§11-7 验收面）：
/// - kind 必须 "mjt-project-store"、version 必须识别——不识别整文件拒载
///   （Open kind，双向喂错文件都拒载）；
/// - 顶层未知字段**忽略**（加法演进）；条目未知字段**拒载**（Schema）；
///   重复键（任意层、含未知键重复）一律拒载；
/// - `format` 值不识别 → 该 section 按 opaque 收容保留：format=Opaque +
///   formatRetained，原载串存 formatTag 供原样回写（§9.3 只保留不解释），
///   并记入 diagnostics，**不拒载**；
/// - `record_count`：jsonl 必有；json/opaque 禁止（出现即 Schema 拒载）；
///   收容条目（未知 format）允许出现并透传——收容的立场是"无法解释的字节
///   只保留不删除"，整文件拒载会让新版引擎清单触发全孤儿降级，违背 §9.3。
/// - 解析零半登记：任一 Schema 错误即整文件拒载。
ca::Result<StoreManifest, ProjectStoreError> parseStoreManifestText(
    const ca::str::Utf8StringRef& text);

/// @brief 从 bundle 目录读取并解析 store.json（`<dir>/store.json`）。
/// 缺失/不可读 → Open/Io 错，消息含路径。
ca::Result<StoreManifest, ProjectStoreError> readStoreManifestFile(
    const ca::str::Utf8StringRef& bundleDir);

/// @brief 确定性写出 store.json 文本（§4.2：pretty、2 空格缩进，键序固定为
///        version/kind/engine/sections；sections 按 name 字典序、条目键序按
///        §4.2 表序、depends 按 path 字典序；同状态任意操作序逐字节一致）。
///
/// 写出前校验（Schema 错）：version 必须为当前版本、条目名合法且唯一、
/// content_hash 形状、record_count 与 format 的相容规则同解析、produced_by/
/// created_at/updated_at 非空。diagnostics 不写出。
ca::Result<ca::str::Utf8String, ProjectStoreError> writeStoreManifestText(
    const StoreManifest& manifest);

//-- section 内容读写原语（C2：§5.1/§5.2/§8.1 的 section 层切片）--------------
//
// 本节是单 section 的可靠读写原语：原子写（staged tmp + 换名）与读取校验
// （先 size 后 hash → Damaged 判定材料）。store.json manifest 的 flush 事务
// 编排（§8.1 manifest last）见下方 C3 事务（复用本节原语、不重写）；§8.2
// 恢复扫描（scanBundleRecovery，文末）在本组原语之上组装。

/// @brief section 原子写的双登记字段（§6.3/D3）：对发布到 sections/ 的
///        **精确字节**（含末尾 LF）算出，供清单条目登记（C3 写入 store.json；
///        C4 读回校验）。hash 与 size 双登记使截断/追加都能检出（§4.2）。
struct SectionWriteResult
{
    ca::str::Utf8String contentHash; ///< "sha256:" + 64 hex（小写），精确字节 SHA-256
    ca::u64             byteSize = 0; ///< 同一字节口径的长度
};

/// @brief 单 section 原子写（§8.1 序列 1-2-4-7 的 section 层切片）。
///
/// 字节**原样**写入 `tmp/txn-<pid>-<mono>/sections/<name>.<ext>`（staged，
/// 同卷 MUST §8.1/R45-P6：txn 恒在 bundle 内）→ 单次换名到
/// `sections/<name>.<ext>`（唯一提交点；Windows 撞读者句柄按 §8.1 重试
/// 3 次 × 50ms，耗尽 = Io）→ 清扫本次 txn。换名成功前旧字节完好、磁盘无
/// 残迹；换名成功后新字节整体可见（读者要么旧字节要么新字节，§7.3）。
///
/// - 字节透传：不做任何编码/序列化变换——jsonl 的紧凑记录与末行 LF、json
///   的 pretty 2 空格由生产者（C3 SectionWriter）在字节构造期保证（§5.1）；
///   同字节重复写幂等（逐字节覆盖，§8.1 重试语义的同款红利）。
/// - 非法名在任何触盘动作之前拒绝（Schema，零残迹，§3.3）。
/// - 成功返回双登记字段；manifest 登记归 C3（本原语不触 store.json）。
/// - record_count 不在此计算：它是记录层语义（jsonl 行数），由 C3 的
///   SectionWriter 在 finish 时按记录口径产出。
ca::Result<SectionWriteResult, ProjectStoreError> writeSectionBytes(
    const ca::str::Utf8StringRef& bundleDir, const ca::str::Utf8StringRef& name,
    SectionFormat format, const ca::u8* data, ca::usize size);

/// @brief section 文本字节便捷重载（json/jsonl 等文本内容的常规入口；
///        opaque 二进制请用指针重载——Utf8StringRef 构造会校验 UTF-8）。
ca::Result<SectionWriteResult, ProjectStoreError> writeSectionBytes(
    const ca::str::Utf8StringRef& bundleDir, const ca::str::Utf8StringRef& name,
    SectionFormat format, const ca::str::Utf8StringRef& bytes);

/// @brief section 内容校验原语（§5.2/D3：**先验 byte_size 再验 content_hash**；
///        §8.3 Damaged 判定材料；完整恢复扫描 §8.2-3 见文末
///        scanBundleRecovery）。
///
/// 预期值取自清单条目（C3 登记）。文件缺失同样按损坏口径报 `Section` 错
/// （§8.1 崩溃表：removeSection 墓碑中途/替换型半提交与真损坏同落"文件缺失
/// /hash 失配 → Damaged"一条路径）。失配消息自带期望 vs 实际（§6.1：hash
/// 失配含期望/实际 hash 前缀）。Ok = 精确字节与登记一致。
ca::Result<void, ProjectStoreError> verifySectionBytes(const ca::str::Utf8StringRef& bundleDir,
                                                       const StoreSectionEntry& entry);

/// @brief 读 section 精确字节：单次读入后按 §5.2 口径校验（先 size 后 hash），
///        通过即原样返回；失配报 `Section` 错（消息同 verifySectionBytes）。
///        不持久持句柄（§7.3 读者短句柄纪律）。
ca::Result<ca::str::Utf8String, ProjectStoreError> readSectionBytes(
    const ca::str::Utf8StringRef& bundleDir, const StoreSectionEntry& entry);

//-- store.json manifest 事务（C3：§6.3 flush 的 manifest 切片、§8.1 编排）------
//
// sections/ 的字节发布归 C2 的 writeSectionBytes（不重写只复用）；本节把与之
// 一致的清单登记以事务语义提交：读-改-写、staged tmp + 单次换名 + 重试、
// CP-1 消费。§8.2 恢复扫描已由 C4 以 scanBundleRecovery 落地（只读分类账，
// 见文末）；开期 txn 清扫与 flush 后孤儿 prune 是**动作面**：prune 半边已由
// R62/N1 的 pruneOrphanSections（文末）落地（消费扫描报告的 AfterFirstFlush
// 档，挂点 ProjectStore::flush 归实体接线卡），txn 清扫半边挂点
// ProjectStore::open（尚未落地），扫描不代行任何一侧。

/// @brief manifest 事务的变更集（读-改-写的"改"；§8.1 flush 编排的输入面）。
///
/// - upserts：写即替换（§6.3）——名字已登记则整体替换该条目，未登记则追加。
///   content_hash/byte_size 必须来自 writeSectionBytes 的 SectionWriteResult
///   （同字节发布在先）；提交前事务按 §5.2 口径（先 size 后 hash）对发布
///   文件逐笔复核，失配/缺失 = Damaged 判定（§8.3）当场拦截——清单是引擎
///   自产封闭记录，writer bug 在 flush 当场抓住，不落成明日 Damaged。
/// - formatRetained 条目（清单载有不识别 format 的收容段）**不可 upsert
///   覆写**（§9.3"可整体删除但不可改写"）——先显式 remove，再另行登记新段。
/// - removals：removeSection 墓碑（§6.3）——flush 时删 sections/<name>.<ext>
///   文件 + 删清单条目；对当前清单不存在的名字报 Schema 错；与 upserts 同名
///   相交 = Schema（一笔事务内又删又写，语义矛盾）。
/// - created_at/updated_at 语义归生产者：upsert 是整体替换，替换时时间戳由
///   调用方给定（本事务不做继承魔法）。
struct StoreManifestMutation
{
    std::vector<StoreSectionEntry>   upserts;  ///< 新增/替换条目（写即替换）
    std::vector<ca::str::Utf8String> removals; ///< removeSection 墓碑名（§6.3）
};

/// @brief store.json 读-改-写事务（C3：§8.1 序列的 manifest 层切片）。
///
/// 序列（对齐 §8.1 既有口径：txn 目录命名/同卷 MUST/重试/自清扫与 C2 三段式
/// 一致）：
///
/// 1. **读**当前清单——CP-1 消费点（R52 处方，本函数是钩子首个消费方）：
///    isManifestUnloadableKind 为真（Open ∪ 解析期 Schema 整笔拒载 ∪ Io
///    读取失败——C4 终裁）即**拒绝事务**。降级孤儿态下绝不换新清单：若在
///    该状态覆写 store.json，现存 section 文件会被新清单重注册为"清单可载"
///    场景的 §8.2-4 孤儿（flush 后可自动 prune），P1 红线（unloadable-manifest
///    孤儿永不自动 prune）即被绕过。错误 kind 原样上浮（终裁合流政策面，
///    不吞并类别）。拒绝时 store.json 原字节不动、零触盘。
/// 2. **校验面**（零触盘）：变更集自洽（§6.3 一名一 writer、墓碑必须已
///    登记、收容段不可覆写）+ upsert 与发布字节逐笔复核（§5.2 先 size 后
///    hash）。
/// 3. **删除墓碑文件**（先删文件后提清单）：中途崩溃落 §8.1 崩溃表"文件已
///    删、条目未提 → Damaged"路径（条目保留、可重写恢复），不会制造清单外
///    可 prune 孤儿。删除幂等（文件已缺 = 此前中断/损坏形态，视为已删——
///    Damaged 段的显式删除是设计认可的恢复路径）；删除失败 = Io，中止于
///    清单提交之前（旧清单完好）。
/// 4. **应用变更** → writeStoreManifestText 全量确定性写出（写出前校验
///    失败同样零触盘）。
/// 5. **提交**：staged tmp/txn-<pid>-<mono>/store.json → 单次换名到 bundle
///    根（唯一提交点；撞读者句柄重试 3×50ms 耗尽 = Io；重试 = 清扫本次
///    txn 后重跑全序列，同字节幂等，R45/P2——staged 残留不作为重试依据）
///    → 自清扫本次 txn（他人/开期残留归 open 期恢复扫描 §8.2-1 的动作面，
///    scanBundleRecovery 只盘点不代清）。bundle 根不走
///    atomic_write_text（R45/P4：其同目录临时残留游离于恢复扫描之外）。
///
/// 幂等：无变更时同状态重写逐字节一致（§4.2 确定性验收口径）；成功返回
/// 提交后的清单（含解析期 diagnostics，仅供诊断、不参与写出）。
ca::Result<StoreManifest, ProjectStoreError> commitStoreManifestTransaction(
    const ca::str::Utf8StringRef& bundleDir, const StoreManifestMutation& mutation);

//-- 恢复扫描（C4：§8.2 全量盘点分类账；§6.2 openDiagnostics 的数据源）--------
//
// 对 bundle 的 sections/ + store.json + tmp/ 做只读盘点，产出分类账：每份
// section 文件归入 Ok（清单已登记且校验通过）/ Damaged（清单已登记但
// size/hash 失配、文件缺失或读失败）/ Orphan（sections/ 现存、清单无）；孤儿
// 再按**清单可载性**分两档（R45/P1）：清单可载场景孤儿 = 本引擎半提交遗留
// （§8.2-4，首次成功 flush 后 prune）；unloadable-manifest 孤儿 = §8.2-2 降级
// （只报告、永久保留，零自动 prune 路径——CP-1 终裁：Io 读错同档）。
//
// 恢复动作语义（本卡口径，设计稿之外的动作为零）：扫描**只报告不改动**——
// §8.2-1 的 txn 清扫是 open 期动作、§8.2-4 的 flush 后 prune 是 flush 期动作、
// §8.3 的 Damaged 恢复 = 同名重写（writeSectionBytes + 清单 upsert 既有
// 原语）；prune 动作面已由 R62/N1 的 pruneOrphanSections（文末）以现扫现验
// 纪律落地，txn 清扫与 Damaged 重写的挂点（ProjectStore::open/flush）尚未
// 落地，本扫描一概不代行。

/// @brief 孤儿处置政策（R45/P1 两档；本扫描只定政策，不执行动作）。
///
/// 行状态词汇（§6.2 SectionState 的同构口径）由报告结构本身承载：登记且校验
/// 通过 = okSections 行、登记而失配 = damagedSections 行、清单不认 = orphans
/// 行；正式的 SectionInfo.state 归读路径卡落地时再定。
enum class OrphanPrunePolicy : ca::u8
{
    AfterFirstFlush, ///< 清单可载场景孤儿（§8.2-4）：本引擎半提交遗留，首次成功
                     ///< flush 后 prune——全引擎的自动 prune 仅限此档
    Never,           ///< unloadable-manifest 孤儿（§8.2-2）：只报告、永久保留，
                     ///< 零自动 prune 路径（P1 红线；CP-1 终裁：Io 读错同档）
};

/// @brief 清单载入态（§8.2-2/3 的分流依据；口径 = isManifestUnloadableKind，
///        CP-1 终裁后 Open/Schema/Io 全落 Unloadable）。
enum class ManifestLoadState : ca::u8
{
    Loadable,   ///< store.json 载入成功（含收容诊断）；清单外文件属 §8.2-4 档
    Unloadable, ///< store.json 缺失/拒载/读取失败（§8.2-2 降级）：sections/ 现存
                ///< 文件全部转孤儿且 prunePolicy 恒 Never
};

/// @brief 孤儿行：sections/ 现存而清单不认的文件（§8.2-2/§8.2-4）。
struct StoreOrphanRow
{
    ca::str::Utf8String fileName; ///< sections/ 下的文件名（含扩展名）
    OrphanPrunePolicy   prunePolicy = OrphanPrunePolicy::Never;
    ///< AfterFirstFlush 仅当 manifestState == Loadable（§8.2-4）；Unloadable
    ///< 时恒 Never（§8.2-2，P1 红线——专测断言的零 prune 路径即此字段）。
};

/// @brief §8.2 恢复扫描报告（分类账；纯只读产出，扫描零触盘）。
///
/// 行覆盖完备性：sections/ 下每份普通文件恰有一行——登记面（okSections ∪
/// damagedSections，行 = 清单条目）或孤儿面（orphans，行 = 文件名）；目录/
/// 符号链接等非普通文件不建行、记 diagnostics。各行均按字典序（Ok/Damaged
/// 按 section 名、孤儿/txn 按文件名）——同状态重扫逐字段一致。
struct StoreRecoveryReport
{
    ManifestLoadState manifestState = ManifestLoadState::Unloadable;
    ///< Unloadable 时 manifestError 原样携带（kind 不改写：Open/Schema/Io
    ///< 各自上浮——CP-1 终裁只在政策面合流，不吞并错误类别）。
    ProjectStoreError manifestError; ///< Loadable 时为空构造

    std::vector<StoreSectionEntry> okSections;      ///< 校验通过（清单条目原样）
    std::vector<StoreSectionEntry> damagedSections; ///< 失配/缺失（条目保留，§8.2-3）
    std::vector<ProjectStoreError> damagedReasons;
    ///< 与 damagedSections 一一对应（含期望 vs 实际）；登记文件的 Io 读失败
    ///< 同样归 Damaged 行（宁可错报不可漏报，CP-1 同款保守）。

    std::vector<StoreOrphanRow> orphans; ///< sections/ 清单外文件（文件名字典序）

    std::vector<ca::str::Utf8String> txnResidues;
    ///< tmp/ 下 `txn-*` 残留目录名（字典序；§8.2-1 只盘点不清扫——清扫挂点
    ///< ProjectStore::open）。tmp/ 下非 txn-* 条目记 diagnostics（设计布局
    ///< 之外，扫描不触碰）。

    std::vector<ca::str::Utf8String> diagnostics;
    ///< 恢复发现 + 解析期收容诊断汇流（§6.2 openDiagnostics 的数据源；人类
    ///< 可读，逐行自足）。open 本身不因 2-4 类发现失败（§8.2）——本报告即
    ///< "失败也进报告"的承载形态。
};

/// @brief §8.2 恢复扫描：sections/ + store.json + tmp/ 全量盘点 → 分类账。
///
/// 只读红线：任何路径不写、不删、不改（含 tmp/ 残留与降级孤儿——扫描只读
/// 性有专测断言）；不触 store.lock（锁原语 = acquireStoreLock，C5——扫描与
/// 锁互不越界有专测钉住）；project.json 不在盘点面
/// （§8.2-5 实体真源门归 ProjectStore::open，实体拒载 = 整体 Open 错，非
/// 报告行）。分类口径：
/// - **Loadable**：逐条目按 §5.2（先 size 后 hash）对发布文件校验 →
///   Ok/Damaged；现存文件不匹配任何登记文件名 → Orphan（AfterFirstFlush）。
///   收容条目（format 值不识别，§4.2/§9.3）按 `<name>.` 词根匹配现存文件——
///   新引擎的未知扩展名文件永不因本引擎不识扩展名而落入 prune 档（§9.3
///   同源保守；词根多匹配时首行受验、其余按 Never 孤儿上报）。
/// - **Unloadable**（缺失/拒载/读取失败——CP-1 终裁 Io 同权）：现存文件
///   全部转孤儿且恒 Never；不做条目重建（section_version/depends/
///   produced_by 不可信重建，宁孤儿不错报，§8.2-2）。
/// - 失败面：仅 sections/（或 tmp/）目录不可枚举 = Io 整笔失败（分类账做
///   不出来才失败）；§8.2-2/3/4 的发现一律进报告不失败。
ca::Result<StoreRecoveryReport, ProjectStoreError> scanBundleRecovery(
    const ca::str::Utf8StringRef& bundleDir);

//-- flush 后孤儿 prune（R62/N1：工作清单现扫现验的动作面原语）------------------
//
// §8.2-4 "首次成功 flush 后可自动 prune"的动作原语：输入 = 恢复扫描分类账
// （scanBundleRecovery 的产出），工作清单 = 其中 prunePolicy ==
// AfterFirstFlush 的孤儿行（全引擎自动 prune 仅限此档——R45/P1）。挂点
// ProjectStore::flush（实体接线卡）：首次成功 flush 后调用一次。
//
// **R62/N1 现扫现验**：工作清单是扫描时刻的快照，扫描到执行之间磁盘与清单
// 都可能变化（清单恢复、他人登记、权限边界漂移）；病态权限边界下 exists 的
// ec 曾被认领处忽略（R62 病灶），登记文件可能误落 AfterFirstFlush 档。因此
// 执行时对每个候选路径**重新**验证当前状态，任何一环证明不了安全即跳过并
// 诊断（诊断带路径与原因），**绝不带病删**：
//   1. 清单现读：store.json 此刻载入失败（isManifestUnloadableKind）→ 全部
//      跳过——清单读不出来，就没有任何可证明可删的东西（P1 红线；
//      CP-1 终裁同款 fail-closed 句式）；
//   2. 认领现验：候选此刻被当前清单认领（登记名 / 收容词根，平台语义同
//      N2——Windows 大小写不敏感）→ 跳过（登记字节永不该自动删）；
//   3. 路径现验：exists 的 ec 不再忽略——存在性检查失败 = 跳过（病态权限
//      边界的现症）；已缺 = 幂等（"文件已缺视为已删"的既有设计口径，诊断
//      注明）；非普通文件 = 跳过不触碰（§8.2 盘点面对齐）；可读验证失败
//      （只读句柄探针打不开）= 跳过；
//   4. 删除动作：remove 的 ec 上浮为跳过诊断，文件原样保留。
// **Never 档零触碰**：Never 行根本不进工作清单——不 stat、不诊断、不提及
// （unloadable-manifest 孤儿恒不 prune 的 P1 红线零变化）。锁互不越界（C5
// 边界延伸）：本原语唯一可删除的是 sections/ 下的候选文件，store.lock 永不
// 在工作清单。

/// @brief 单次孤儿 prune 的执行报告（动作面产出；pruned/skipped 均按文件名
///        字典序）。
struct StorePruneReport
{
    std::vector<ca::str::Utf8String> pruned;
    ///< 已删除（含"已缺 = 幂等视为已删"形态，诊断逐笔注明）。

    std::vector<ca::str::Utf8String> skipped;
    ///< 带病跳过：现认领命中 / 存在性或可读验证失败 / 删除失败（路径+原因
    ///< 见 diagnostics）。Never 档不产生任何行。

    std::vector<ca::str::Utf8String> diagnostics;
    ///< 每笔跳过/幂等事实的人读行（路径 + 原因，逐条自足）；干净删除不产生
    ///< 诊断（事实面即 pruned 行）。
};

/// @brief §8.2-4 flush 后孤儿 prune（R62/N1 现扫现验；动作语义见上方块注）。
///
/// 纯动作面：report 只读消费（本原语不改动输入报告）；全程零整体失败态——
/// 每个发现都在返回的报告内带路径与原因，不外抛不吞错。工作清单为空时零
/// 动作返回（连 sections/ 的存在性都不探测）。
StorePruneReport pruneOrphanSections(const ca::str::Utf8StringRef& bundleDir,
                                     const StoreRecoveryReport& report);

//-- 单写者锁（C5：§7.1 全路径 + §7.2 PID fail-closed 过期判定）----------------
//
// store.lock 的获取/探测/接管/stealLock（§12 卡表 C5；验收 §11-3）。锁以
// **句柄持有**为生命线（§7.1：独占创建 → 写内容 → fflush → 句柄持有到
// close；Windows 上该独占句柄同时阻止他人删除锁文件——活持有者的锁删不掉、
// 同路径独占创建必败，构成 PID 探测之外的硬防线）。锁的语义面与 C4 的
// prune 红线互不越界：锁路径唯一可删除的文件是 store.lock 本身，恢复扫描/
// 孤儿两档/txn 盘点一概不触（scanBundleRecovery 对 store.lock 零触碰有
// 专测钉住）。
//
// open() 接缝（R45/P7）：ProjectStore::open（实体接线卡落地）以
// acquireStoreLock 为其锁接管点——open 成功 = 持有 StoreLock，锁随对象
// 生存、close/析构释放（§6.3 持锁即开门；open 失败不留任何句柄）。本卡只
// 落锁原语与 §11-3 验收，不建 ProjectStore 类。
//
// 已知边界（§7.2）：裸文件模式无锁（只读，§9.1）；PID 复用是残余风险，由
// stealLock 人工兜底，v1 不加时间戳/启动时间启发式。

/// @brief 锁接管选项（§6.2 OpenOptions 的锁切片；ProjectStore::open 落地时
///        原样并入其 OpenOptions）。
struct StoreLockOptions
{
    bool stealLock = false; ///< true = 跳过 §7.2 探测，直接删旧锁重试独占创建
                            ///< 恰好一次（人工决策出口：用户明知对端活着仍要
                            ///< 强开，§7.1）；抢占事实记入锁诊断。
};

/// @brief store.lock 的持有句柄（move-only RAII；构造即已持锁，唯一合法
///        入口是 acquireStoreLock）。
///
/// 释放语义（析构 / release() 等价）：
/// - POSIX 删除前 fstat/stat inode 比对核验属主（名字已被并发 stealLock 抢
///   占者换成新 inode 时绝不删，与 Windows 内容比对对称），核验通过先删名
///   后关句柄（unlink 语义：名字消失后竞争者的独占创建落成新 inode，本句
///   柄 close 只释放旧 inode——教科书安全序）；
/// - Windows 先关句柄再删文件（自身独占句柄在持期间阻止删除），删除前重读
///   锁内容与本人写入字节逐字节比对，不是本人的字节绝不删——防 close→remove
///   窗口内 stealLock 抢占者的新锁被误删。
/// 释放期异常一律转锁诊断（diagnostics()），不外抛（析构纪律）；锁路径任何
/// 获取期失败不吞错误、原样上浮（acquireStoreLock 的 Result）。
class StoreLock
{
public:
    ~StoreLock();
    StoreLock(StoreLock&& other) noexcept;
    StoreLock& operator=(StoreLock&& other) noexcept;
    StoreLock(const StoreLock&)            = delete;
    StoreLock& operator=(const StoreLock&) = delete;

    /// 是否仍持锁（move 出源后的源对象为 false；release 后为 false）。
    bool holdsLock() const noexcept { return handle_ != nullptr; }

    /// 锁文件路径（诊断用）。
    const ca::str::Utf8String& lockFilePath() const { return lockFilePath_; }

    /// 锁诊断（非致命事实的报告通道，§4.2 store.json diagnostics 同款形态；
    /// 人类可读、逐条自足）：死锁接管事实、stealLock 抢占事实（§7.1 都必须
    /// 记录）与释放期异常。纯获取（无接管）成功时为空。
    const std::vector<ca::str::Utf8String>& diagnostics() const { return diagnostics_; }

    /// 释放锁（幂等；析构等价调用）。POSIX inode 比对核验后先删名后关句柄；
    /// Windows 先关句柄、内容比对后删文件（类注）。
    void release();

private:
    friend ca::Result<StoreLock, ProjectStoreError> acquireStoreLock(
        const ca::str::Utf8StringRef& bundleDir, const StoreLockOptions& options,
        const char* owner);

    StoreLock() = default;

    std::FILE*                       handle_ = nullptr; ///< 持有期生命线（§7.1）
    ca::str::Utf8String              lockFilePath_;
    ca::str::Utf8String              writtenContent_; ///< 本人写入的精确字节（释放期比对）
    std::vector<ca::str::Utf8String> diagnostics_;
};

/// @brief 获取 bundle 的单写者锁（§7.1 全路径；验收 §11-3）。
///
/// 锁文件 `<bundle>/store.lock`，内容单行 JSON
/// `{"pid":<pid>,"created_at":"<RFC3339 UTC>","owner":"<owner>"}`。获取序：
/// 独占创建（CRT `fopen("wbx")` 的 O_EXCL 语义，§6.4 处方：纯 CRT 无平台
/// 分支）→ 写内容 → fflush → 句柄持有到 close。
///
/// 失败路径（§7.1 逐条）：
/// - 已存在 → 读出 owner → §7.2 探测：**活** = `Lock` 错（消息含
///   pid/created_at/owner，IDA "database is locked by …" 同款体验）；
///   **死** = 接管：删旧锁文件后重试独占创建**恰好一次**（不循环）——再失败
///   = 抢锁竞态输，返回 `Lock` 且消息**重读当前锁文件**报新赢家（R45/P5）；
///   删旧失败 = `Io`（fail-closed，绝不破坏性绕过）。
/// - **锁内容不可解析/读不出（R45/P5）**= 按活锁 fail-closed：`Lock` 错注明
///   unparseable 并指路 stealLock；**禁止**把读不出 owner 的锁当作死锁自动
///   接管（独占创建成功与内容写完之间的崩溃窗会留下空/半截 lock 文件）。
/// - `options.stealLock = true` → 跳过探测（含不可解析/读不出的拒绝）直接
///   删旧锁重试一次，成功后的锁诊断**必须**记录抢占事实（旧 owner 全文，
///   读不出则注明）。
/// - 非 EEXIST 的创建失败 = `Io`（真实文件系统失败，不删任何东西）。
///
/// owner：锁的自述串（缺省 kProjectStoreLockOwnerDefault），仅诊断用。
/// 同进程二次获取同 bundle 同样被拒（自身 pid 探测为活）——单线程纪律下这
/// 就是重复 open 的正确失败面。
ca::Result<StoreLock, ProjectStoreError> acquireStoreLock(
    const ca::str::Utf8StringRef& bundleDir, const StoreLockOptions& options = {},
    const char* owner = kProjectStoreLockOwnerDefault);

/// @brief PID 存活探测（§7.2 fail-closed 过期判定；mjdbg daemon_state.cpp
///        :348-368 纪律平移——模式平移，不改 mjdbg）。
///
/// 返回 true = 活（或**证明不了死**）；false = 证明已死。Windows：
/// OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) + GetExitCodeProcess，**仅**
/// ERROR_INVALID_PARAMETER 证明 pid 已死，其余失败（含 ACCESS_DENIED 保护
/// 进程）一律按活——证明不了死就不得动它的锁；pid ≤ 0 或超出 Windows pid
/// 空间（DWORD）判死（不可能是真 pid）。POSIX：kill(pid, 0)——ESRCH = 死，
/// 其余（含 EPERM 活但无权限、其他瞬时错误）一律活。PID 复用是残余风险，
/// 由 stealLock 人工兜底（§7.2）。测试用：§11-3 死 pid 构造法的探测断言
/// （断言失败即 skip 的备选法依赖本函数）。
bool storeLockPidAlive(ca::i64 pid);

}   // namespace mjbase::project
