#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/analysis/semantic_fp.hpp"
#include "mjbase/project/class_kind.hpp"
#include "mjbase/project/library_origin.hpp"
#include "mjbase/project/mask_types.hpp"

namespace mjbase::project {

/// @file analysis_db.hpp
/// @brief 分析数据库：单文件 SQLite 工程库、稳定身份、重命名覆盖层与分析事实。
///
/// - 单文件 `.mjdb`，打开期间由 SQLite 维护 `-wal` / `-shm` 伴随文件；
///   `save()` / `close()` 做 checkpoint，收敛回单文件。
/// - `classes` / `members` 在首次登记时分配单调 ID，只增不删；重命名只动
///   `renames` 覆盖层，不改身份、不触发分析。
/// - 重命名目标用稳定 ID 键控（class_id / member_id），原始名字与描述符只是
///   身份登记属性。分析事实（父类/接口/直接引用）直接复用同一套 ID。
/// - 分析事实层是原始字节码的直接编码事实：类头（super/interfaces/access）、
///   成员清单、指令级与 class 级直接引用。解析、重写与增量失效由上层负责，
///   本层只做批量入库与索引查询。
/// - 条目身份层（schema v3）：`inputs` 登记输入容器（path 为身份键），
///   `entries` 登记 jar 内条目（entry_id 单调分配、只增不删，内容锚 =
///   size + sha256）。重导入按 (input_id, path, sha256) 匹配：锚命中复用
///   id（消失后重现的条目原地复活），未命中新建 id，消失条目打墓碑不删行。
///   `classes.entry_id` 记录类字节来自哪个条目。
///
/// - 出口遮蔽层（schema v4）：`mask_rules` 存出口遮蔽规则（#121 §4.3，
///   priority 有序、先匹配先生效、hit_count 复盘），meta 存项目信任级别
///   （default_action 翻转开关）。规则匹配与判定在 `mask_evaluator`（纯
///   函数面、无 IO），本层只做规则配置的数据面与命中记账。
///
/// - 引用目标 id 化（schema v5）：refs 目标侧复用身份层既有稳定 id（可空
///   外键 target_class_id / target_member_id），写入时已登记的目标整体落 id
///   并清空文本三列，未登记目标整体落文本；id 与名字互为冗余编码，查询输出
///   经 LEFT JOIN + COALESCE 重建文本三元组，DbReferenceRecord 契约不变。
///
/// - producer 注册表（schema v6，#143）：`producers` 登记向本库写事实的
///   身份（稳定 key 幂等、id 单调只增不删、trust_level 两档），`batches`
///   记录逻辑写入运行（一次整库导入、一次引用替换各一批；finished_at 空串
///   = 未收尾的残批，合法审计状态），`batch_scopes` 记录批次覆盖的事实
///   区间（作用域锚）。事实表不加归属列，逐行归属经作用域解析回答；
///   renames 是覆盖层与配置型小表，行级归属即语义，走行级
///   `renames.producer_id` 列（写入口尾参盖章，缺省 0 = 匿名写入落 NULL，
///   覆盖行为不变 last-write-wins，覆盖来源事后可查）。
///
/// - mask 配置面归属化（schema v7，#143 遗留回填）：mask_rules 与 renames
///   同为覆盖层与配置型小表，行级归属即语义，走行级 `mask_rules.producer_id`
///   列（`replaceMaskRules` 尾参盖章，口径同 renames）；meta 信任级别键的
///   写入口 `setMaskTrustLevel` 同样尾参盖章，归属落伴随 meta 键
///   `mask_trust_level_producer`（匿名写入时清除）。规则面写入挂批
///   （rules_write）属消费方职责，本层只做行级归属。
///
/// - 方法版本指纹层（schema v8，#132「怎么接进数据库」）：`method_versions`
///   存方法语义指纹的版本历史——行 = 一次语义落库（method_version_id 单调
///   只增不删；fingerprint 32 字节定长 BLOB + fp_version 算法版本字节 +
///   recorded_at）。「当前指针」是 `is_current` 列 + (member_id) 部分唯一
///   索引：同成员至多一行当前版本，不动 members 表（成员身份与指纹版本是
///   两条独立的轴，指针挤进身份登记面会让没有版本行的方法背空指针）。
///   「标脏」不另建脏表：当前指针变更本身可查询（旧指纹对不上当前行即脏），
///   is_current=0 的历史行就是审计面。指纹引擎（semanticFp，#152）在
///   analysis 层，本层只做版本事实的存储与查询，编辑流接线归上层。
///
/// - 全程序可达闭包（schema v9 #159 / v10 #159 后置批）：`closure_batches` /
///   `closure_members` 存可达方法成员集这份派生事实——批次元数据行挂既有
///   producer 注册表（kind = closure_write），记录输入图指纹（32 字节
///   SHA-256 + fp_version 算法版本字节）、精度档（precision：CHA/RTA）、
///   种子/覆盖度位词表与行数；closure_members 是结果本体（WITHOUT ROWID
///   主键即数据）。计算面在 `reachability_closure`（消费本层读取面与写入面），
///   指纹 = 变更敏感聚合（inputs 投影 + 四表聚合），指纹不匹配整体重算，
///   不增量修补；复用判定 = 指纹 + seed_mask + precision 三匹配。
///
/// - 普查投影层（#121 §5 B4）：census 是库上的只读 SELECT 投影（#39 类
///   普查事实面 + 容器形态汇总面；#148 成员普查事实面），不反向定义存储、
///   不重新扫描容器。形态判定核心（entry_shape 路径 facet、class_kind 类
///   形态）是纯字节码域知识，随本层落 mjbase；导出渲染（TSV、转义、版本
///   化表头）由上层工具出口负责。
///
/// 批量写入口（`ensureInput` / `ingestEntries` / `ingestClasses` /
/// `replaceReferences` / `replaceReferencesFromMember` / `setRenames`）都走
/// 单事务 + 预编译语句复用；任一行失败整批回滚，不留半截事实。
///
/// 存储引擎是 SQLite（xrepo `sqlite3`，静态链接）。sqlite3 C API 只出现在
/// analysis_db.cpp；本头不暴露 SQL、句柄或表结构。错误统一走
/// `ca::Result<T, AnalysisDbError>`，不抛异常。
///
/// 线程模型：一个 `AnalysisDb` 实例持有一个连接，不是线程安全的；写由单连接
/// 串行化。只读打开不获取写锁、不触发任何分析。

/// @brief 当前 schema 版本（`PRAGMA user_version`）。不识别的版本拒绝打开
///        （包括旧版本：不提供自动迁移，旧库由上层显式重建）。
///
/// v10：可达闭包精度档（#159 后置批）——closure_batches 末位追加 `precision`
///        列（0 = CHA 全族保守展开，1 = RTA 实例化集合收窄）；复用判定
///        随之升级为指纹 + seed_mask + precision 三匹配（精度档是计算输入，
///        同图不同档不得互复用）。v9：全程序可达闭包（#159）——新增
///        closure_batches（闭包批次元数据：输入图指纹 32B BLOB +
///        fp_version + 种子/覆盖度位词表 + 行数）与 closure_members
///        （可达成员集，(batch_id, member_id) 主键）两表与 batches.kind
///        新词 closure_write。v8：方法版本指纹层（#132）——新增
///        method_versions 表（行 = 方法语义指纹版本：32 字节定长 BLOB +
///        fp_version 算法版本字节；is_current 当前指针 + (member_id) 部分
///        唯一索引，版本历史累积即审计面）。
///        v7：mask 配置面归属化（#143 遗留回填）——mask_rules 增加可空外键
///        `producer_id`（列序末位追加，`replaceMaskRules` 尾参盖章）与
///        meta 信任级别写入口 `setMaskTrustLevel` 尾参盖章（伴随键
///        `mask_trust_level_producer`）。v6：producer 注册表——新增
///        producers / batches / batch_scopes 三表（#143：写入运行登记与
///        作用域锚，事实表不加归属列）与 renames.producer_id 行级归属
///        （可空外键，写入口尾参盖章）。v5：refs 目标侧 id 化——新增可空
///        外键 `refs.target_class_id` / `target_member_id`，目标整体二元化
///        （登记目标落 id 清文本，未登记目标整体落文本），目标反查走
///        refs_target_id / refs_target_text 两个部分索引两分支 UNION ALL。
///        v4：新增 mask_rules 遮蔽规则表与 meta 信任级别键（出口遮蔽层，
///        #121 §4.3）。v3：新增 `inputs` / `entries` 条目身份表与
///        `classes.entry_id` 外键。v2：classes 增加 super/access/version，
///        新增 interfaces 与 refs 表，members 增加 access。v1 未发布，
///        版本号仅用于识别本分支早期的临时库。
inline constexpr ca::u32 kAnalysisDbSchemaVersion = 10;

/// @brief SQLite 数据库头里的应用标识（ASCII "MJDB"）。
inline constexpr ca::u32 kAnalysisDbApplicationId = 0x4D4A4442u;

/// @brief `meta.kind` 自描述串，防止把别的 SQLite 库当分析库打开。
inline constexpr const char* kAnalysisDbKind = "mjbase-analysis-db";

/// @brief 打开方式。只读连接要求库已存在且已保存（WAL 已 checkpoint）。
enum class AnalysisDbOpenMode : ca::u8
{
    ReadWrite, ///< 读写；文件不存在时创建
    ReadOnly,  ///< 只读；不创建、不写、不触发分析
};

/// @brief 重命名目标类别。class 用 class_id，member 用 member_id。
enum class RenameTargetKind : ca::u8
{
    Class  = 0,
    Member = 1,
};

/// @brief 成员类别。字段与方法同名同描述符时仍是两个身份。
enum class DbMemberKind : ca::u8
{
    Field  = 0,
    Method = 1,
};

/// @brief 事实来源可信级别（producers.trust_level，两档）。
///
/// 词表对齐既有 MaskTrustLevel（trusted/untrusted），但不复用同一 C++ 枚举：
/// 两者语义不同（项目出口默认动作 vs 事实来源可信级别），绑死一个类型会让
/// 两个轴被迫同步演化。
enum class DbProducerTrustLevel : ca::u8
{
    Untrusted = 0, ///< 脚本来源等自报身份（处置策略由消费方决定）
    Trusted   = 1, ///< 产品内置管线（如 workspace 整库导入桥）
};

/// @brief 批次种类（batches.kind；SQL CHECK 词表同源）。
enum class DbBatchKind : ca::u8
{
    FullImport       = 0, ///< full_import：整库导入（条目 + 类事实 + 全库引用）
    RefsReplace      = 1, ///< refs_replace：整库引用替换（只动 refs）
    RefsSliceReplace = 2, ///< refs_slice_replace：单成员引用切片替换
    RenamesWrite     = 3, ///< renames_write：覆盖层批量写入（预留）
    RulesWrite       = 4, ///< rules_write：遮蔽规则配置写入（预留）
    ClosureWrite     = 5, ///< closure_write：可达闭包派生事实写入（schema v9，#159）
};

/// @brief 分析数据库错误类别。
enum class AnalysisDbErrorKind : ca::u8
{
    Open,     ///< 路径不可开、文件不是 SQLite 库或库尚未初始化
    Schema,   ///< application_id / user_version / meta.kind 不匹配
    Invalid,  ///< 调用参数非法，或唯一约束冲突（如 token 重复）
    ReadOnly, ///< 只读连接上尝试写入
    Io,       ///< 文件系统或 checkpoint 失败
    Sqlite,   ///< 其他 SQL 执行失败
};

/// @brief 分析数据库诊断（消息自带上下文：SQL 阶段、约束名等）。
struct AnalysisDbError
{
    AnalysisDbErrorKind kind = AnalysisDbErrorKind::Sqlite;
    ca::str::Utf8String message;
};

/// @brief 一条重命名覆盖记录（没有对应行时不存在）。
///
/// - class 记录：`target == Class`，`owner` = 类内部名，`name` / `desc` 为空。
/// - member 记录：`target == Member`，`owner` = 类内部名，`name` / `desc` 为
///   原始成员名与描述符，`memberKind` 区分字段与方法。
/// - `token` 为空表示未分配（旧 shadow 格式），非空时必须是 8 位小写 hex。
/// - `updatedAt` 由数据库写入时生成（ISO-8601 UTC）；作为 `replaceRenames`
///   输入时忽略。
/// - `producerId` 是行级归属（schema v6）：查询输出为盖章结果（0 = 匿名
///   写入或未归属）；作为写入输入时忽略，归属由写入口的 producerId 参数
///   决定，不经记录携带。
struct DbRenameRecord
{
    RenameTargetKind    target     = RenameTargetKind::Class;
    DbMemberKind        memberKind = DbMemberKind::Field; ///< 仅 target == Member 有效
    ca::str::Utf8String owner;
    ca::str::Utf8String name;
    ca::str::Utf8String desc;
    ca::str::Utf8String shadow;
    ca::str::Utf8String token;
    ca::str::Utf8String updatedAt;
    ca::i64             producerId = 0; ///< 覆盖来源生产者；0 = 匿名写入
};

/// @brief 直接引用的目标类别（与指令/class 头的编码事实一一对应）。
enum class DbReferenceKind : ca::u8
{
    Type   = 0, ///< new/checkcast/instanceof/LDC class/class 头声明
    Field  = 1, ///< get/put 家族
    Method = 2, ///< invoke 家族
};

/// @brief 入库的成员定义（`ingestClasses` 输入）。
struct DbMemberInput
{
    DbMemberKind        kind = DbMemberKind::Method;
    ca::str::Utf8String name;
    ca::str::Utf8String descriptor;
    ca::i32             access = 0;
};

/// @brief 入库的类定义（`ingestClasses` 输入）。
///
/// 身份键是 `internalName`；`superName` 可为空（java/lang/Object 或缺失）。
/// `interfaces` 按 classfile 声明顺序保存；`members` 只做登记/更新，不删除
/// 库内已有成员（稳定身份只增不删）。`entryId` 指向条目身份层的
/// `entries.entry_id`（类字节所属 jar 条目）；0 表示未指定——已登记类保持
/// 既有链接不动，新登记类存 NULL。
struct DbClassInput
{
    ca::str::Utf8String              internalName;
    ca::str::Utf8String              superName;
    ca::i32                          access  = 0;
    ca::i32                          version = 0;
    ca::i64                          entryId = 0;
    std::vector<ca::str::Utf8String> interfaces;
    std::vector<DbMemberInput>       members;
};

/// @brief 类事实（查询输出）。
struct DbClassInfo
{
    ca::i64                          classId = 0;
    ca::str::Utf8String              internalName;
    ca::str::Utf8String              superName;
    ca::i32                          access  = 0;
    ca::i32                          version = 0;
    ca::i64                          entryId = 0; ///< 类字节所属条目；0 = 未关联
    std::vector<ca::str::Utf8String> interfaces;
};

/// @brief 成员事实（查询输出）。
struct DbMemberInfo
{
    ca::i64             memberId = 0;
    DbMemberKind        kind     = DbMemberKind::Method;
    ca::str::Utf8String name;
    ca::str::Utf8String descriptor;
    ca::i32             access = 0;
};

/// @brief 一条直接引用事实（指令级或 class 头）。
///
/// 方法内引用：sourceMethod/sourceDescriptor 非空，insnIndex 为方法体内代码
/// 指令序号；class 级引用（super/interfaces）：sourceMethod 为空、insnIndex
/// 为 -1。Type 目标只填 targetClass；Field/Method 目标三个字段都填。
struct DbReferenceRecord
{
    ca::str::Utf8String sourceClass;
    ca::str::Utf8String sourceMethod;
    ca::str::Utf8String sourceDescriptor;
    ca::i64             insnIndex = -1;
    DbReferenceKind     kind      = DbReferenceKind::Type;
    ca::str::Utf8String targetClass;
    ca::str::Utf8String targetName;
    ca::str::Utf8String targetDescriptor;
};

/// @brief 已登记的输入容器（查询输出）。
struct DbInputInfo
{
    ca::i64             inputId = 0;
    ca::str::Utf8String path;
    ca::i64             size = 0;    ///< 登记方给定的容器规模
    ca::str::Utf8String sha256;      ///< 容器内容锚（64 位小写 hex）
};

/// @brief 待登记的 jar 条目（`ingestEntries` 输入）。
struct DbEntryInput
{
    ca::str::Utf8String path;         ///< 条目路径（jar 内路径，非空）
    ca::str::Utf8String kind;         ///< 形态类别（class/manifest/resource/...，开放文本）
    ca::i64             size = 0;     ///< 条目字节数
    ca::str::Utf8String sha256;       ///< 内容锚：精确字节的 SHA-256，64 位小写 hex
    bool                regular = true; ///< 常规条目旗标；登记方通常取
                                        ///< entry_shape.hpp 的 isRegularEntryPath
                                        ///< （B1 最小口径的公共面，#121 B4 上提）
};

/// @brief 已登记的 jar 条目事实（查询输出；墓碑行同样返回）。
struct DbEntryInfo
{
    ca::i64             entryId = 0;
    ca::i64             inputId = 0;
    ca::str::Utf8String path;
    ca::str::Utf8String kind;
    ca::i64             size = 0;
    ca::str::Utf8String sha256;
    bool                regular    = true;
    bool                tombstoned = false; ///< 墓碑：曾登记、本次重导入未出现
};

/// @brief 一次条目批量登记（`ingestEntries`）的计数。
struct DbEntryIngestStats
{
    ca::usize total       = 0; ///< 本批条目数
    ca::usize created     = 0; ///< 新分配 entry_id 的条目数
    ca::usize reused      = 0; ///< 锚命中且本就活跃（仅刷新元数据）的条目数
    ca::usize resurrected = 0; ///< 锚命中墓碑行、原地复活的条目数（entry_id 不变）
    ca::usize tombstoned  = 0; ///< 本次标记为消失的既有活跃条目数
};

//-- 普查投影层（census，#121 §5 B4） ------------------------------------------

/// @brief 类普查投影行（`censusClasses` 输出；#39 class_census 的库侧事实面）。
///
/// 除 `kind` 外全是库内编码事实；`kind` 是投影层派生（classifyClassKind，
/// access + super 现算，不存储）。行序 = internal_name 升序（SQL BINARY
/// 字节序，跨平台确定）。
struct DbClassCensusRow
{
    ca::str::Utf8String internalName;
    ca::i32             access = 0;
    ca::str::Utf8String superName;
    DbClassKind         kind = DbClassKind::Class; ///< 投影层派生形态
    LibraryOrigin       origin = LibraryOrigin::Unknown; ///< 投影层派生来源（前缀
                                                   ///< 分类，规则版本 kLibraryOriginRuleVersion）
    ca::i64             ifaceCount        = 0;     ///< classfile 声明序接口数
    ca::i64             methodCount       = 0;
    ca::i64             fieldCount        = 0;
    ca::i64             nativeMethodCount = 0;     ///< ACC_NATIVE 方法数
    ca::i64             entryId  = 0;              ///< 类条目身份
    ca::i64             byteSize = 0;              ///< 条目字节大小（条目 size 列）
    bool                entryRegular = true;       ///< 条目 regular 登记旗标（事实列；
                                                   ///< 登记方通常取 isRegularEntryPath）
};

/// @brief 成员普查投影行（`censusMembers` 输出；#148 member_census 的库侧
///        事实面）。
///
/// 除 `native` / `origin` 外全是库内编码事实：`native` 是投影层派生（方法
/// ACC_NATIVE 按位测试，字段恒 0）；`origin` 继承所属类分类（对 owner 内部
/// 名走 classifyLibraryOrigin，与 `censusClasses` 同源同果）。行序 = 复合键
/// BINARY 升序：owner 内部名 → kind（Field 在前，编码序）→ 名字 → 描述符。
struct DbMemberCensusRow
{
    ca::str::Utf8String ownerInternalName;
    DbMemberKind        kind       = DbMemberKind::Field;
    ca::str::Utf8String name;
    ca::str::Utf8String descriptor;
    ca::i32             access     = 0;
    bool                native     = false; ///< 方法 ACC_NATIVE 派生；字段恒 0
    ca::i64             entryId    = 0;     ///< 所属类关联条目（活跃/墓碑语义随类）
    LibraryOrigin       origin     = LibraryOrigin::Unknown; ///< 继承所属类
};

/// @brief 计数分布行（facet 稳定名 + 计数；census 汇总面通用行）。
struct DbCensusFacetCount
{
    ca::str::Utf8String facet;
    ca::i64             count = 0;
};

/// @brief 容器普查投影（`censusContainer` 输出；条目形态分布 + 分析事实计数）。
///
/// 条目面统计活跃行（墓碑行只计总数）；路径形态 facet 由 entry_shape 判定
/// 器在投影层现算（#121 B4 钩子：entries.kind 登记语义不变，细化切面走
/// 投影层）。类/成员/引用计数只统计关联本输入活跃条目的类。
struct DbContainerCensus
{
    ca::i64             inputId = 0;
    ca::str::Utf8String path;      ///< 输入容器身份（登记 path）
    ca::i64             size = 0;  ///< 登记的容器规模
    ca::str::Utf8String sha256;    ///< 容器内容锚

    ca::i64 entriesActive     = 0; ///< 活跃条目数
    ca::i64 entriesTombstoned = 0; ///< 墓碑条目数
    ca::i64 entriesRegular    = 0; ///< 活跃 ∧ regular 登记旗标（事实列）
    ca::i64 entriesIrregular  = 0; ///< 活跃 ∧ 非登记常规
    /// @brief entries.kind 存储值分布（facet 名升序；只含计数非零项）。
    std::vector<DbCensusFacetCount> entryKinds;
    /// @brief 路径形态 facet 分布（entry_shape 投影层现算，与登记旗标独立；
    ///        登记方取 isRegularEntryPath 时两者一致。声明序；只含非零项）。
    std::vector<DbCensusFacetCount> entryShapes;

    ca::i64 classes       = 0; ///< 关联本输入活跃条目的类数
    ca::i64 methods       = 0;
    ca::i64 fields        = 0;
    ca::i64 nativeMethods = 0; ///< ACC_NATIVE 方法数
    ca::i64 references    = 0; ///< 源类属本输入的引用行数
};

/// @brief 待写入的遮蔽规则（`replaceMaskRules` 输入；#121 §4.3）。
///
/// 规则是出口遮蔽的项目级配置：priority 数字越小越先匹配，先匹配先生效；
/// pattern 为正则字面量（引擎纪律见 mask_regex_engine.hpp，写入时校验
/// 编译性）；note 为人工备注，随行透传不参与匹配。
struct DbMaskRuleInput
{
    ca::i32             priority = 0;
    ca::str::Utf8String pattern;
    MaskAction          action = MaskAction::Mask;
    ca::str::Utf8String note;
};

/// @brief 已登记的遮蔽规则（`listMaskRules` 输出，(priority, rule_id) 升序）。
struct DbMaskRule
{
    ca::i64             ruleId     = 0;
    ca::i32             priority   = 0;
    ca::str::Utf8String pattern;
    MaskAction          action     = MaskAction::Mask;
    ca::str::Utf8String note;
    ca::i64             hitCount   = 0; ///< 命中计数（含被 suspect 压制的 allow 命中）
    ca::i64             producerId = 0; ///< 规则写入者（schema v7 行级归属）；
                                       ///< 0 = 匿名写入
};

/// @brief 一次遮蔽规则整体替换（`replaceMaskRules`）的计数。
struct DbMaskRuleReplaceStats
{
    ca::usize total   = 0; ///< 本批规则数（= 新分配 rule_id 数）
    ca::usize created = 0; ///< 新分配 rule_id 的规则数
    ca::usize retired = 0; ///< 被替换掉的既有规则行数（hit 计数随行清零）
};

/// @brief 待注册的生产者（`registerProducer` 输入）。
///
/// `key` 是稳定 id（点分 slug 是建议命名，注册后不可变）；`name` 为展示名；
/// `toolFingerprint` 由注册方自报（产品名 + 语义版本 + 构建标识的拼接是建议
/// 载荷），回答「哪个版本的工具写的」，与事实内容指纹正交。
struct DbProducerInput
{
    ca::str::Utf8String  key;
    ca::str::Utf8String  name;
    ca::str::Utf8String  toolFingerprint;
    DbProducerTrustLevel trustLevel = DbProducerTrustLevel::Trusted;
};

/// @brief 批次收尾（`closeBatch`）回填的写入计数。
///
/// 由批次发起方按各写入口的返回值累计；注册表不核对其与库内行数的一致性
/// （批次是逻辑运行记录，不是对账单）。
struct DbBatchCounts
{
    ca::i64 classCount  = 0;
    ca::i64 entryCount  = 0;
    ca::i64 memberCount = 0;
    ca::i64 refCount    = 0;
};

/// @brief 作用域种类（batch_scopes.scope_kind；CHECK IN (0,1,2,3) 同源）。
///
/// 0=refs_all（整库引用替换，scope_id 恒 0）、1=input（条目登记与重导入）、
/// 2=class（类事实 upsert 与 class 头引用切片）、3=member（成员引用切片）。
/// 成员登记的归属并入其所在类（ingestClasses 的 class 作用域行）。
enum class DbBatchScopeKind : ca::u8
{
    RefsAll = 0, ///< 整库引用替换锚；scope_id 恒 0，覆盖全部引用切片
    Input   = 1, ///< 条目登记与重导入锚；scope_id = input_id
    Class   = 2, ///< 类事实锚；scope_id = class_id
    Member  = 3, ///< 成员引用切片锚；scope_id = member_id
};

/// @brief 已登记的生产者（`listProducers` 输出，producer_id 升序）。
struct DbProducerInfo
{
    ca::i64              producerId = 0;
    ca::str::Utf8String  key;
    ca::str::Utf8String  name;
    ca::str::Utf8String  toolFingerprint;
    DbProducerTrustLevel trustLevel = DbProducerTrustLevel::Trusted;
    ca::str::Utf8String  createdAt;
    ca::str::Utf8String  updatedAt;
};

/// @brief 一次作用域锚登记（`batchScopes` 输出，(scope_kind, scope_id) 升序）。
struct DbBatchScope
{
    ca::i64          batchId = 0;
    DbBatchScopeKind kind    = DbBatchScopeKind::RefsAll;
    ca::i64          scopeId = 0; ///< refs_all 恒 0；input/class/member 对应 id
};

/// @brief 已登记的批次（`listBatches` 输出，batch_id 升序）。
///
/// `finishedAt` 为空串 = 未收尾的残批（失败/中断的写入运行，合法审计状态）。
/// `scopeCount` 是 batch_scopes 里本批的作用域行数。
struct DbBatchInfo
{
    ca::i64             batchId     = 0;
    ca::i64             producerId  = 0;
    ca::str::Utf8String producerKey;
    DbBatchKind         kind        = DbBatchKind::FullImport;
    ca::str::Utf8String startedAt;
    ca::str::Utf8String finishedAt;
    ca::i64             classCount  = 0;
    ca::i64             entryCount  = 0;
    ca::i64             memberCount = 0;
    ca::i64             refCount    = 0;
    ca::i64             scopeCount  = 0;
    ca::str::Utf8String note;
};

/// @brief 归属解析结果（`refSliceOwnership` / `classOwnership` 输出）。
///
/// 回答「这份事实是哪次运行写的」：批次 + 生产者身份。无归属（nullopt）
/// = 没有任何批次的作用域锚覆盖该事实。
struct DbOwnershipInfo
{
    ca::i64             batchId     = 0;
    ca::i64             producerId  = 0;
    ca::str::Utf8String producerKey;
    DbBatchKind         kind        = DbBatchKind::FullImport;
};

/// @brief 生产者聚合统计（`producerStats` 输出，producer_id 升序）。
///
/// 批次维度求和（含残批；从未收尾的批 finished_at 不计入 lastBatchAt）。
struct DbProducerStats
{
    ca::i64              producerId = 0;
    ca::str::Utf8String  key;
    ca::str::Utf8String  name;
    DbProducerTrustLevel trustLevel = DbProducerTrustLevel::Trusted;
    ca::i64              batchCount = 0;
    ca::i64              classCount  = 0; ///< 各批 class_count 求和
    ca::i64              entryCount  = 0;
    ca::i64              memberCount = 0;
    ca::i64              refCount    = 0;
    ca::str::Utf8String  firstBatchAt; ///< 各批 started_at 最小值；无批为空串
    ca::str::Utf8String  lastBatchAt;  ///< 各批 finished_at 最大值（残批不计）；无批为空串
};

/// @brief 待落库的方法版本指纹（`recordMethodVersions` 输入；schema v8，#132）。
struct DbMethodVersionInput
{
    ca::i64              classId = 0;  ///< 方法宿主类（须与成员登记归属一致）
    ca::i64              memberId = 0; ///< 已登记方法身份（字段无指纹）
    analysis::SemanticFp fingerprint;  ///< 方法语义指纹（semanticFp 引擎输出）
};

/// @brief 一条方法版本事实（`listMethodVersions` 输出，method_version_id
///        升序 = 记录序）。
///
/// fingerprint 的字节与 hex 都由库内 BLOB 重建（hex 是字节的小写文本派生
/// 形态，不单独存储）；`isCurrent` 为 true 的行是当前版本，退位行是审计面。
struct DbMethodVersionInfo
{
    ca::i64              methodVersionId = 0;
    ca::i64              classId         = 0;
    ca::i64              memberId        = 0;
    analysis::SemanticFp fingerprint;
    ca::u8               fpVersion = 0; ///< 记录时盖章的算法版本字节
                                        ///< （kSemanticFpVersion）
    bool                 isCurrent = false;
    ca::str::Utf8String  recordedAt;    ///< ISO-8601 UTC
};

/// @brief 一次方法版本批量落库（`recordMethodVersions`）的计数。
struct DbMethodVersionRecordStats
{
    ca::usize recorded  = 0; ///< 新落版本行数（语义有变化的成员）
    ca::usize unchanged = 0; ///< 指纹与当前行相同、幂等跳过的成员数
};

/// @brief 待落库的可达闭包批次（`storeClosureBatch` 输入；schema v9，#159）。
///
/// 批次生命周期由本面托管：按 @p producerId 开 closure_write 批 → 单事务落
/// 元数据行 + 成员集 + 类域作用域行（作用域从成员集派生，不经输入携带）→
/// 收尾批。@p memberIds 契约为升序去重（闭包工作表的自然输出序），落库前
/// 校验；`fingerprint` 必须是 32 字节（闭包输入图指纹，设计稿 §5）。
struct DbClosureBatchRecord
{
    ca::i64             producerId    = 0;
    std::vector<ca::u8> fingerprint;      ///< 输入图指纹（32 字节 SHA-256）
    ca::u32             fpVersion     = 1; ///< 闭包算法版本字节（kReachabilityClosureFpVersion）
    ca::u32             seedMask      = 0; ///< 种子档位位词表（ClosureSeedMask）
    ca::i64             seedCount     = 0;
    ca::u32             coverageMask  = 0; ///< 覆盖度位词表（ClosureCoverageMask）
    ca::i64             indyEdgeCount = 0; ///< indy 补充边数（coverage bit1 置位时 > 0）
    /// 精度档（ClosurePrecisionTier 词表：0 = CHA，1 = RTA；schema v10）。
    ca::u32             precision     = 0;
    std::vector<ca::i64> memberIds;        ///< 可达成员集（升序去重）
};

/// @brief 一条闭包批次事实（`listClosureBatches` / `freshClosureBatch` /
///        `storeClosureBatch` 输出，batch_id 升序）。
struct DbClosureBatchInfo
{
    ca::i64             batchId       = 0;
    std::vector<ca::u8> fingerprint;      ///< 输入图指纹（32 字节）
    ca::u32             fpVersion     = 1;
    ca::u32             seedMask      = 0;
    ca::i64             seedCount     = 0;
    ca::u32             coverageMask  = 0;
    ca::i64             indyEdgeCount = 0;
    ca::u32             precision     = 0; ///< 精度档（ClosurePrecisionTier 词表）
    ca::i64             memberCount   = 0;
    ca::str::Utf8String computedAt;       ///< ISO-8601 UTC
};

/// @brief 闭包图边快照（`closureGraphEdges` 输出）：方法边（id 分支）与
///        clinit 触发行（id/文本两分支）+ Type 触发行（RTA 实例化证据）。
///
/// 方法边只来自 method 引用的 id 分支行（target_member_id 已登记）；触发行
/// 携带「源方法 → 目标类」（refs 无 opcode 位，Type/Field/Method 引用一律
/// 按「可能首触」保守处理，设计稿 §3）；class 级引用行（source_member_id
/// 为空）不入快照——类头声明不触发 JVM 初始化。文本分支行只保留已登记
/// owner 类的触发（未登记目标无类节点，不入闭包域）。typeClassTriggers 是
/// id 分支 Type 引用行的子集（new/checkcast/instanceof/ldc class 全族，
/// refs 无 opcode 位按可能实例化保守处理）——CHA 闭包不用它，RTA 档取为
/// 实例化集合证据。行序 = ref_id 升序（确定性），同名边可能重复，消费方
/// 去重（闭包面 sort+unique）。
struct DbClosureGraphEdges
{
    /// (源方法 member_id, 目标方法 member_id)：id 分支 method 引用。
    std::vector<std::pair<ca::i64, ca::i64>> methodEdges;
    /// (源方法 member_id, 目标类 class_id)：id 分支触发行（target_class_id 在册）。
    std::vector<std::pair<ca::i64, ca::i64>> classTriggers;
    /// (源方法 member_id, 已登记 owner 类 class_id)：文本分支触发行
    /// （target_class 文本命中 classes.internal_name）。
    std::vector<std::pair<ca::i64, ca::i64>> textBranchClassTriggers;
    /// (源方法 member_id, 目标类 class_id)：id 分支 Type 引用行（classTriggers
    /// 的 kind=Type 子集；RTA 档实例化集合证据，schema v10 起提供）。
    std::vector<std::pair<ca::i64, ca::i64>> typeClassTriggers;
};

/// @brief 分析数据库连接（单文件、单连接、非线程安全）。
class AnalysisDb
{
public:
    /// @brief 打开或创建数据库。
    ///
    /// - `ReadWrite`：文件不存在时创建空库并写入 schema；
    /// - `ReadOnly`：文件必须已存在且是已保存的分析库，不创建、不写；
    /// - 已存在但 application_id / user_version / meta.kind 不匹配 → `Schema`；
    ///   非 SQLite 文件 → `Open`。
    ///
    /// 返回的连接析构时自动 checkpoint 并关闭；需要可观测错误时显式调用 `close()`。
    static ca::Result<AnalysisDb, AnalysisDbError> open(
        const ca::str::Utf8StringRef& path,
        AnalysisDbOpenMode            mode = AnalysisDbOpenMode::ReadWrite);

    AnalysisDb(AnalysisDb&&) noexcept;
    AnalysisDb& operator=(AnalysisDb&&) noexcept;
    AnalysisDb(const AnalysisDb&) = delete;
    AnalysisDb& operator=(const AnalysisDb&) = delete;
    ~AnalysisDb();

    /// @brief 把 WAL 内容 checkpoint 回主文件（`TRUNCATE`）。只读连接为空操作。
    ///        有其他连接持读快照导致无法完成时返回 `Io`，数据仍已在 WAL 中。
    ca::Result<void, AnalysisDbError> save();

    /// @brief checkpoint 并关闭连接；幂等。关闭后 WAL/SHM 由 SQLite 清理。
    ca::Result<void, AnalysisDbError> close();

    bool isOpen() const noexcept;
    bool isReadOnly() const noexcept;

    /// @brief 打开的连接返回 schema 版本，关闭后返回 0。
    ca::u32 schemaVersion() const noexcept;

    //-- 重命名覆盖层（原始身份 → 显示名） --------------------------------

    /// @brief 设置类重命名；类身份不存在时先登记。
    ///        `shadow` 非空；`token` 为空或 8 位小写 hex。已存在则整体覆盖。
    ///
    /// `producerId` 行级盖章（0 = 匿名写入，producer_id 落 NULL；非 0 必须
    /// 已注册，否则 `Invalid`）。覆盖来源 + updated_at 让「谁盖掉了谁」
    /// 事后可查。
    ca::Result<void, AnalysisDbError> setClassRename(
        const ca::str::Utf8StringRef& owner,
        const ca::str::Utf8StringRef& shadow,
        const ca::str::Utf8StringRef& token      = {},
        ca::i64                       producerId = 0);

    /// @brief 设置成员重命名；成员身份不存在时先登记。已存在则整体覆盖。
    ///        `producerId` 行级盖章，口径同 `setClassRename`。
    ca::Result<void, AnalysisDbError> setMemberRename(
        DbMemberKind                  kind,
        const ca::str::Utf8StringRef& owner,
        const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& desc,
        const ca::str::Utf8StringRef& shadow,
        const ca::str::Utf8StringRef& token      = {},
        ca::i64                       producerId = 0);

    /// @brief 批量设置重命名：单事务、语句预编译复用，未提及的既有覆盖保持不变。
    ///
    /// 输入先整体校验（重复目标、重复非空 token、非法字段 → `Invalid`），
    /// 校验失败不写库；执行中任何一行失败（如 token 与库内既有行冲突）整批回滚。
    /// AI 批量重命名脚本走这个入口，不要循环调用单条接口。
    /// `producerId` 对本批全部行级盖章，口径同 `setClassRename`。
    ca::Result<void, AnalysisDbError> setRenames(std::vector<DbRenameRecord> records,
                                                 ca::i64                     producerId = 0);

    /// @brief 删除类重命名；返回是否确有该行。身份登记保留。
    ca::Result<bool, AnalysisDbError> clearClassRename(const ca::str::Utf8StringRef& owner);

    /// @brief 删除成员重命名；返回是否确有该行。身份登记保留。
    ca::Result<bool, AnalysisDbError> clearMemberRename(
        DbMemberKind                  kind,
        const ca::str::Utf8StringRef& owner,
        const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& desc);

    /// @brief 查询类重命名；未登记返回 `std::nullopt`。
    ca::Result<std::optional<DbRenameRecord>, AnalysisDbError> findClassRename(
        const ca::str::Utf8StringRef& owner) const;

    /// @brief 查询成员重命名；未登记返回 `std::nullopt`。
    ca::Result<std::optional<DbRenameRecord>, AnalysisDbError> findMemberRename(
        DbMemberKind                  kind,
        const ca::str::Utf8StringRef& owner,
        const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& desc) const;

    /// @brief 全部重命名记录，按 (target, owner, member kind, name, desc) 稳定排序。
    ca::Result<std::vector<DbRenameRecord>, AnalysisDbError> listRenames() const;

    /// @brief 按 shadow 名反查；class/member 都可能命中，同名可有多条。
    ///
    /// class shadow 并不要求全局唯一（成员 shadow 只要求类内唯一），所以返回
    /// 列表而不是单条。排序与 `listRenames()` 一致。
    ca::Result<std::vector<DbRenameRecord>, AnalysisDbError> findRenamesByShadow(
        const ca::str::Utf8StringRef& shadow) const;

    /// @brief 批量反查：按输入顺序逐项查询并拼接结果（同一 shadow 重复出现会重复输出）。
    ///
    /// 预编译语句复用，供 AI 脚本一次性还原大量显示名。
    ca::Result<std::vector<DbRenameRecord>, AnalysisDbError> findRenamesByShadows(
        const std::vector<ca::str::Utf8StringRef>& shadows) const;

    /// @brief 原子替换全部重命名覆盖（单事务）。
    ///
    /// 输入先整体校验：重复目标、重复非空 token、非法字段一律 `Invalid`，
    /// 且不改动现有数据。身份登记（classes/members）保留，ID 不重排。
    /// `producerId` 对本批全部行级盖章，口径同 `setClassRename`（0 = 匿名
    /// 写入，重放后新行 producer_id 为 NULL）。
    ca::Result<void, AnalysisDbError> replaceRenames(std::vector<DbRenameRecord> records,
                                                     ca::i64                     producerId = 0);

    //-- 条目身份层（inputs / entries，schema v3） --------------------------

    /// @brief 登记输入容器；身份键为 path。同 path 重登记复用 input_id 并
    ///        刷新 size/sha256，不同 path 分配新 id（id 单调、只增不删）。
    ///        `sha256` 必须是 64 位小写 hex，`size` 非负。
    ca::Result<ca::i64, AnalysisDbError> ensureInput(
        const ca::str::Utf8StringRef& path, const ca::str::Utf8StringRef& sha256, ca::i64 size);

    /// @brief 批量登记一个输入的 jar 条目身份（单事务，完整集合语义）。
    ///
    /// - 匹配锚 = (input_id, path, sha256)：锚命中复用 entry_id——活跃行仅
    ///   刷新 kind/size/regular 等元数据，墓碑行原地复活（id 不漂移）；锚未
    ///   命中新建 entry_id。行只增不删，永不物理删除。
    /// - 处理顺序先墓碑后匹配：本批未出现的该输入既有活跃条目先标记
    ///   `tombstoned=1`，再逐条匹配/新建——同 path 内容变化必然让旧行进
    ///   墓碑、新内容拿新 id。
    /// - `batchId` 首参挂靠批次（#143 批 3）：0 = 无批写入（不落作用域行，
    ///   条目归属悬空）；非 0 必须是未收尾的已注册批次（否则 `Invalid`，
    ///   不静默匿名写入），命中后落 (1, input_id) 作用域行。
    /// - 输入不存在、批内重复 path、path/kind 为空或含 NUL、sha256 非 64 位
    ///   小写 hex、size 为负 → `Invalid`，整批不写库。
    ca::Result<DbEntryIngestStats, AnalysisDbError> ingestEntries(
        ca::i64 batchId, ca::i64 inputId, std::vector<DbEntryInput> entries);

    /// @brief 全部已登记输入容器，按 path 升序。
    ca::Result<std::vector<DbInputInfo>, AnalysisDbError> listInputs() const;

    /// @brief 指定输入的全部条目（含墓碑行），按 path 升序。
    ///        输入不存在返回 `Invalid`；未登记条目返回空表。
    ca::Result<std::vector<DbEntryInfo>, AnalysisDbError> listEntries(ca::i64 inputId) const;

    //-- 普查投影层（census，#121 §5 B4） ------------------------------------

    /// @brief 类普查投影：一行一类（#39 class_census 事实面）。
    ///
    /// 纯 SELECT 投影：类事实 JOIN 所属条目（byte_size / 形态旗标）+ 接口
    /// 与成员计数聚合，`ORDER BY internal_name`（BINARY 字节序，确定性）。
    /// 只统计关联 @p inputId 活跃条目的类——条目墓碑表示类已不在当前容器
    /// 内容，未关联条目的类（entry_id 为空）不属于任何输入的普查面。
    /// 输入不存在、连接未打开 → `Invalid`；只读连接可用。
    ca::Result<std::vector<DbClassCensusRow>, AnalysisDbError> censusClasses(
        ca::i64 inputId) const;

    /// @brief 成员普查投影：一行一成员（#148 member_census 事实面；v1 范围
    ///        = 全部成员，字段与方法都出）。
    ///
    /// 纯 SELECT 投影：members JOIN 所属类 JOIN 所属条目，行序复合键 BINARY
    /// 升序（owner 内部名 → kind 编码序 → 名字 → 描述符，键序定死投影层）。
    /// 可见域与 `censusClasses` 同契约：只含关联 @p inputId 活跃条目的类下的
    /// 成员——条目墓碑表示类已不在当前容器内容，墓碑类连带其成员一起退出
    /// 普查面；未关联条目的类（entry_id 为空）不属于任何输入的普查面。
    /// 输入不存在、连接未打开 → `Invalid`；只读连接可用。
    ca::Result<std::vector<DbMemberCensusRow>, AnalysisDbError> censusMembers(
        ca::i64 inputId) const;

    /// @brief 容器普查投影：条目形态分布 + 类/成员/引用计数（汇总面）。
    ///
    /// 路径形态 facet 由 entry_shape 判定器现算（#121 B4 钩子：不改
    /// entries.kind 登记语义）；facet 分布只含计数非零项。输入不存在、
    /// 连接未打开 → `Invalid`；只读连接可用。
    ca::Result<DbContainerCensus, AnalysisDbError> censusContainer(ca::i64 inputId) const;

    //-- 出口遮蔽层（mask_rules / 信任级别，schema v4） --------------------

    /// @brief 原子替换全部遮蔽规则（单事务；#121 §4.3）。
    ///
    /// 整体替换语义：既有行（连同 hit_count 计数）删除后按输入序插入，新
    /// rule_id 单调分配；空输入 = 清空规则面。规则匹配数据面的求值器
    /// （MaskEvaluator::build）同源校验：条数超 kMaximumMaskRules、pattern
    /// 空/含 NUL/超 kMaximumMaskPatternBytes/编译失败 → `Invalid`，整批不
    /// 写库——数据面不存编译不了的模式，求值器装载失败只剩实现缺陷一种
    /// 来源。
    ///
    /// `producerId` 行级盖章（schema v7，#143；口径同 `setClassRename`）：
    /// 0 = 匿名写入，producer_id 落 NULL；非 0 必须已注册，否则 `Invalid`。
    /// 整体替换后新行统一盖 @p producerId，覆盖来源事后可查。
    ca::Result<DbMaskRuleReplaceStats, AnalysisDbError> replaceMaskRules(
        std::vector<DbMaskRuleInput> rules, ca::i64 producerId = 0);

    /// @brief 全部遮蔽规则，按 (priority, rule_id) 升序（求值器同序）。
    ca::Result<std::vector<DbMaskRule>, AnalysisDbError> listMaskRules() const;

    /// @brief 设置项目信任级别（meta 键 `mask_trust_level`；default_action
    ///        的翻转开关，见 MaskTrustLevel）。重复设置同值幂等。
    ///
    /// `producerId` 配置面归属盖章（schema v7，#143；口径同
    /// `setClassRename`）：0 = 匿名写入（伴随键清除），非 0 必须已注册，
    /// 否则 `Invalid`；归属落伴随 meta 键 `mask_trust_level_producer`
    /// （裸 SQL 可查），随每次设置整体刷新。
    ca::Result<void, AnalysisDbError> setMaskTrustLevel(MaskTrustLevel level,
                                                        ca::i64        producerId = 0);

    /// @brief 项目信任级别；未设置（键缺省）按 `Trusted`。
    ca::Result<MaskTrustLevel, AnalysisDbError> maskTrustLevel() const;

    /// @brief 批量累加规则命中计数（单事务；#121 §4.3 hit_count 复盘面）。
    ///
    /// 输入按出现次数逐 id `hit_count + 1`（出口按行记账：一次查询命中同
    /// 一规则的多条文本各计一次）；未知 rule_id 静默跳过（规则替换与出口
    /// 记账并发的正常窗口，命中记账是尽力而为的审计面，不因竞态失败）。
    /// 空输入为空操作。
    ca::Result<void, AnalysisDbError> recordMaskRuleHits(const std::vector<ca::i64>& ruleIds);

    //-- producer 注册表（producers / batches / batch_scopes，schema v6） ----

    /// @brief 注册生产者（幂等）：稳定 key 命中即刷新元数据并复用 id。
    ///
    /// producer_id 单调分配，key 注册后不可变，无删除 API（与身份层只增不删
    /// 纪律同构）；改名或换工具指纹都是刷新元数据，历史批次仍指向登记行，
    /// 演化经 updated_at 可追溯。`key` / `name` 非空且不含 NUL，
    /// `toolFingerprint` 允许为空串但不含 NUL；违反 → `Invalid`。只读连接
    /// → `ReadOnly`。
    ///
    /// @return 生产者 id（既有 key 复用原 id）。
    ca::Result<ca::i64, AnalysisDbError> registerProducer(const DbProducerInput& input);

    /// @brief 开启一个写入批次：先落一行 started 批（finished_at 为空串），
    ///        随后各事实写入口以返回的 batchId 显式挂靠本批（写事实与作用域
    ///        行），最后 `closeBatch` 收尾。生产者未注册 → `Invalid`。
    ///
    /// 批次是逻辑写入运行，不是事务：openBatch 自带事务只落批次行，各写
    /// 入口在各自事务内写事实与作用域行，中途失败留下 finished_at 为空的
    /// 残批——合法审计状态（回答了「这次运行开始过、走到哪一段」），不算
    /// 脏数据。@p note 为人工备注（允许空）。
    ///
    /// @return 批次 id。
    ca::Result<ca::i64, AnalysisDbError> openBatch(ca::i64                       producerId,
                                                   DbBatchKind                   kind,
                                                   const ca::str::Utf8StringRef& note = {});

    /// @brief 收尾批次：写 finished_at 并回填计数（残批转终态的唯一出口）。
    ///
    /// 批次不存在或已收尾 → `Invalid`（重复收尾是调用方契约错误）；
    /// 计数为负 → `Invalid`。只读连接 → `ReadOnly`。
    ca::Result<void, AnalysisDbError> closeBatch(ca::i64 batchId, const DbBatchCounts& counts);

    /// @brief 全部已注册生产者，按 producer_id 升序。只读连接可用。
    ca::Result<std::vector<DbProducerInfo>, AnalysisDbError> listProducers() const;

    /// @brief 已登记批次，按 batch_id 升序（含计数、作用域数、生产者 key）。
    ///
    /// @p producerId 过滤单生产者；给出但未注册 → `Invalid`（与 listEntries
    /// 对未知输入同口径，不静默返回空表）。只读连接可用。
    ca::Result<std::vector<DbBatchInfo>, AnalysisDbError> listBatches(
        std::optional<ca::i64> producerId) const;

    /// @brief 批次的作用域锚行，按 (scope_kind, scope_id) 升序。
    ///        批次不存在 → `Invalid`；已登记但无作用域行返回空表。
    ca::Result<std::vector<DbBatchScope>, AnalysisDbError> batchScopes(ca::i64 batchId) const;

    /// @brief 成员引用切片的归属解析（#143 3.2）。
    ///
    /// 在 batch_scopes 探 (3, member_id) 与 (0, 0) 两个前缀、取 batch_id 较大
    /// 者（两次索引前缀探 + 取大，无全表扫；refs_all 锚覆盖全部引用切片，
    /// 整库替换之后此前成员级归属被更新的 refs_all 批盖过）。两个前缀都无行
    /// 返回 `std::nullopt`。@p memberId <= 0 → `Invalid`。只读连接可用。
    ca::Result<std::optional<DbOwnershipInfo>, AnalysisDbError> refSliceOwnership(
        DbMemberKind kind, ca::i64 memberId) const;

    /// @brief 类事实的归属解析（#143 3.2）：只探 (2, class_id)——整库引用
    ///        替换不触碰类行，refs_all 不回退参与类事实归属；full_import 缺
    ///        逐类作用域行时类归属为空（负例即契约）。无行返回 nullopt。
    ///        @p classId <= 0 → `Invalid`。只读连接可用。
    ca::Result<std::optional<DbOwnershipInfo>, AnalysisDbError> classOwnership(
        ca::i64 classId) const;

    /// @brief 生产者聚合统计（批次数、累计写入计数、首末批时间），按
    ///        producer_id 升序。只读连接可用。
    ca::Result<std::vector<DbProducerStats>, AnalysisDbError> producerStats() const;

    //-- 分析事实层（类/成员/继承/直接引用） ------------------------------

    /// @brief 批量登记或更新类、接口与成员（单事务，预编译语句复用）。
    ///
    /// - 身份键为 internal_name；已存在则更新 super/access/version，ID 不变。
    /// - interfaces 整表替换（删除该类的旧行后按声明顺序重插）。
    /// - 成员按 (kind, name, descriptor) upsert access；库内已有但本批未提及的
    ///   成员保留（身份只增不删）。
    /// - `batchId` 首参挂靠批次（#143 批 3）：0 = 无批写入（类事实归属悬空）；
    ///   非 0 必须是未收尾的已注册批次（否则 `Invalid`），命中后逐类落
    ///   (2, class_id) 作用域行——full_import 的逐类作用域行是 classOwnership
    ///   可答的必需项而非审计细节（PoC 263 行/次导入量级）。
    /// - 批内类名重复、接口重复、成员重复或字段非法 → `Invalid`，不写库。
    ///
    /// @return 处理的类数。
    ca::Result<ca::usize, AnalysisDbError> ingestClasses(
        ca::i64 batchId, std::vector<DbClassInput> classes);

    /// @brief 原子替换全部直接引用事实（单事务：清空 refs 后批量插入）。
    ///
    /// 来源类/方法必须已在库中（先 `ingestClasses`），否则整批回滚并保留旧
    /// refs。适合整库重建；方法体局部变更后的增量替换由上层切片。
    /// `batchId` 首参挂靠批次（#143 批 3）：0 = 无批写入；非 0 必须是未收尾
    /// 的已注册批次（否则 `Invalid`），命中后落 (0, 0) refs_all 作用域行——
    /// 该锚覆盖全部引用切片，此后此前一切成员级归属被本批盖过。
    ///
    /// @return 写入的引用条数。
    ca::Result<ca::usize, AnalysisDbError> replaceReferences(
        ca::i64 batchId, std::vector<DbReferenceRecord> references);

    /// @brief 原子替换单个源成员的直接引用事实（单事务切片替换）。
    ///
    /// 删除 @p sourceMemberId 名下的全部引用行后按输入序插入 @p references；
    /// 其他成员与 class 头的引用逐行不动。成员身份由参数携带，行内 source
    /// 字段照常解析写入，且必须与 @p sourceMemberId 指向的成员一致（不一致
    /// → `Invalid`）。@p sourceMemberId 不存在或不是方法、记录形状非法 →
    /// `Invalid`，旧切片保持不动；空输入 = 清空该成员的引用切片。适合方法
    /// 体编辑后的增量替换；整库重建用 `replaceReferences`。
    /// `batchId` 首参挂靠批次（#143 批 3）：0 = 无批写入；非 0 必须是未收尾
    /// 的已注册批次（否则 `Invalid`），命中后落 (3, member_id) 作用域行（类
    /// 事实未触碰，不落 class 作用域行）。
    ///
    /// @return 写入的引用条数。
    ca::Result<ca::usize, AnalysisDbError> replaceReferencesFromMember(
        ca::i64 batchId, ca::i64 sourceMemberId, std::vector<DbReferenceRecord> references);

    /// @brief 按内部名查类事实；未登记返回 `std::nullopt`。
    ca::Result<std::optional<DbClassInfo>, AnalysisDbError> findClass(
        const ca::str::Utf8StringRef& owner) const;

    /// @brief 全部已登记类事实，按 internal_name 升序。
    ca::Result<std::vector<DbClassInfo>, AnalysisDbError> listClasses() const;

    /// @brief 类的全部成员，按 (kind, name, descriptor) 升序。
    ca::Result<std::vector<DbMemberInfo>, AnalysisDbError> listMembers(
        const ca::str::Utf8StringRef& owner) const;

    /// @brief 精确成员查询；未登记返回 `std::nullopt`。
    ca::Result<std::optional<DbMemberInfo>, AnalysisDbError> findMember(
        DbMemberKind kind, const ca::str::Utf8StringRef& owner,
        const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc) const;

    /// @brief super 链：含自身、最近在前，未登记的父类名计入链尾后停止。
    ///
    /// 未登记的起点返回空表；父类成环时在首次重复处截断。
    ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> superChain(
        const ca::str::Utf8StringRef& owner) const;

    /// @brief 直接子类（仅 extends 边），按 internal_name 升序。
    ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> directSubclasses(
        const ca::str::Utf8StringRef& owner) const;

    /// @brief 全部后代（不含自身），按 internal_name 升序。
    ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> allSubclasses(
        const ca::str::Utf8StringRef& owner) const;

    /// @brief `allSubclasses` 的分页变体：闭包按 internal_name 升序，只返回
    ///        第 @p offset 行起的 @p limit 行（SQLite OFFSET 行偏移分页）。
    ///
    /// 分页契约（层次闭包分页查询共用）：
    /// - 全量结果 = 各页按 offset 升序拼接，跨页不重不漏（闭包行键唯一、
    ///   排序确定）；
    /// - @p limit 为 0 返回空页，超出剩余行数只返回剩余行；负值 → `Invalid`；
    /// - @p offset 超过闭包大小返回空页；负值 → `Invalid`；
    /// - 窗口在递归 CTE 全量展开、排序之后截取：分页只省行对象构造与返回
    ///   内存，遍历与排序成本不随窗口缩小；游标分页不在本层预设。
    ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> allSubclassesPaged(
        const ca::str::Utf8StringRef& owner, ca::i64 limit, ca::i64 offset) const;

    /// @brief 接口的实现类：接口自身闭包（含子接口）+ 实现者的全部后代。
    ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> implementors(
        const ca::str::Utf8StringRef& interfaceName) const;

    /// @brief `implementors` 的分页变体：实现类按 internal_name 升序，分页
    ///        契约（窗口语义、参数约束、遍历成本不随窗口缩小）与
    ///        `allSubclassesPaged` 完全相同。
    ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> implementorsPaged(
        const ca::str::Utf8StringRef& interfaceName, ca::i64 limit, ca::i64 offset) const;

    /// @brief 精确目标查询：编码目标为 owner/name/descriptor 的方法引用。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesToMethod(
        const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& descriptor) const;

    /// @brief 精确目标查询：编码目标为 owner/name/descriptor 的字段引用。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesToField(
        const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& descriptor) const;

    /// @brief 目标类查询：targetClass 为 owner 的全部引用（type/field/method）。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesToClass(
        const ca::str::Utf8StringRef& owner) const;

    /// @brief 来源成员查询：方法体发出的全部引用（owner+name+descriptor）。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesFromMember(
        const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& descriptor) const;

    /// @brief 来源类查询：class 头与全部方法体发出的引用。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesFromClass(
        const ca::str::Utf8StringRef& owner) const;

    //-- 引用查询分页变体（行偏移窗口；全量接口语义不变） ------------------

    /// @brief `referencesToClass` 的分页变体：命中行按全量版同一行序，只返回
    ///        第 @p offset 行起的 @p limit 行（SQLite OFFSET 行偏移分页）。
    ///
    /// 引用分页契约（本组查询共用）：
    /// - 全量结果 = 各页按 offset 升序拼接，跨页不重不漏（行序键含 ref_id，
    ///   排序全量确定）；
    /// - @p limit 为 0 返回空页，超出剩余行数只返回剩余行；负值 → `Invalid`；
    /// - @p offset 超过命中行数返回空页；负值 → `Invalid`；
    /// - 窗口在排序后截取：匹配与排序每次查询仍全量执行，分页省的是行对象
    ///   构造与返回内存；游标分页不在本层预设；
    /// - 相邻两次分页查询之间 refs 被整体替换（如 `replaceReferences`）会使
    ///   行序重排，跨页窗口以「同一事实状态」为前提。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesToClassPaged(
        const ca::str::Utf8StringRef& owner, ca::i64 limit, ca::i64 offset) const;

    /// @brief `referencesToMethod` 的分页变体：分页契约与
    ///        `referencesToClassPaged` 完全相同，命中目标为
    ///        owner/name/descriptor 的方法引用。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesToMethodPaged(
        const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& descriptor, ca::i64 limit, ca::i64 offset) const;

    /// @brief `referencesToField` 的分页变体：分页契约与
    ///        `referencesToClassPaged` 完全相同，命中目标为
    ///        owner/name/descriptor 的字段引用。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesToFieldPaged(
        const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& descriptor, ca::i64 limit, ca::i64 offset) const;

    /// @brief `referencesFromMember` 的分页变体：分页契约与
    ///        `referencesToClassPaged` 完全相同，命中来源为 owner/name/
    ///        descriptor 方法体的引用；成员未登记返回空页（非错误）。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesFromMemberPaged(
        const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& descriptor, ca::i64 limit, ca::i64 offset) const;

    /// @brief `referencesFromClass` 的分页变体：分页契约与
    ///        `referencesToClassPaged` 完全相同，命中来源为 owner 的
    ///        class 头与方法体引用；类未登记返回空页（非错误）。
    ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesFromClassPaged(
        const ca::str::Utf8StringRef& owner, ca::i64 limit, ca::i64 offset) const;

    /// @brief 库内直接引用总数。
    ca::Result<ca::usize, AnalysisDbError> referenceCount() const;

    //-- 方法版本指纹层（method_versions，schema v8，#132）----------------

    /// @brief 记录方法版本指纹（单方法口；编辑流在每次方法体变更后调用）。
    ///
    /// 一个事务三件事（#132「怎么接进数据库」）：存新版本（异指纹落新行，
    /// method_version_id 单调分配）、更新当前指针（旧行 is_current 退位、
    /// 新行接棒，部分唯一索引锁死同成员至多一行当前版本）、可查询即标脏
    /// （旧指纹对不上当前行即脏，不另建脏表）。指纹与当前行相同则幂等跳过
    /// ——语义没变不是新版本，重复 record 不产生伪版本行。
    ///
    /// @p classId / @p memberId 必须为正，@p memberId 必须是已登记方法
    /// （字段无指纹）且归属 @p classId，指纹不得为全零字节（未初始化的
    /// SemanticFp）；违反 → `Invalid`。只读连接 → `ReadOnly`。
    ///
    /// @return 调用后的当前版本行 id（新落行 id，或幂等跳过时既有当前行 id）。
    ca::Result<ca::i64, AnalysisDbError> recordMethodVersion(
        ca::i64 classId, ca::i64 memberId, const analysis::SemanticFp& fingerprint);

    /// @brief 方法的当前指纹；从未落过版本返回 `std::nullopt`。
    ///
    /// @p memberId 必须是已登记方法（未登记成员或字段 → `Invalid`，与
    /// listEntries / listBatches 对未知 id 同口径，不静默返回空）。
    /// 只读连接可用。
    ca::Result<std::optional<analysis::SemanticFp>, AnalysisDbError>
    currentMethodFingerprint(ca::i64 memberId) const;

    /// @brief 方法的全部版本历史（method_version_id 升序 = 记录序；当前行
    ///        与退位行都出，审计面）。@p memberId 口径同
    ///        `currentMethodFingerprint`。只读连接可用。
    ca::Result<std::vector<DbMethodVersionInfo>, AnalysisDbError> listMethodVersions(
        ca::i64 memberId) const;

    /// @brief 批量记录方法版本指纹（整类重写场景）：单事务，批内任一行失败
    ///        整批回滚，不留半截版本。
    ///
    /// 输入先整体校验（id 为正、指纹非全零、批内成员不重复——一次编辑只落
    /// 一个终态版本），校验失败不写库；成员存在性与归属一致性在事务内逐行
    /// 校验。幂等语义与 `recordMethodVersion` 一致：与当前行同指纹的成员
    /// 跳过不计新版本，整类重写后语义未动的成员不产生伪版本行。
    ///
    /// @return 计数（recorded = 新落行数，unchanged = 幂等跳过数）。
    ca::Result<DbMethodVersionRecordStats, AnalysisDbError> recordMethodVersions(
        std::vector<DbMethodVersionInput> inputs);

    //-- 全程序可达闭包（closure_batches / closure_members，schema v9 #159 /
    //-- v10 #159 后置批）-------------------------------------------------------------

    /// @brief 可达闭包批次落库：按 @p record.producerId 开 closure_write 批 →
    ///        单事务落元数据行 + 成员集 + 类域作用域行（作用域从成员集经
    ///        members.class_id 派生）→ 收尾批，返回批次事实。
    ///
    /// 三个事务段与既有写入面同纪律：批次是逻辑运行记录，事实段中途失败
    /// 留残批（合法审计状态）。输入校验：producer 未注册、fingerprint 非
    /// 32 字节、fp_version 为 0、seed_count/indy_edge_count 为负、precision
    /// 非 0/1、memberIds 非升序去重或含非正 id → `Invalid`（校验先于开批，
    /// 不产生残批垃圾）；成员 id 未登记 → FK 拒绝（事实段回滚留残批）。
    /// 只读连接 → `ReadOnly`。
    ca::Result<DbClosureBatchInfo, AnalysisDbError> storeClosureBatch(
        const DbClosureBatchRecord& record);

    /// @brief 全部闭包批次事实，按 batch_id 升序。只读连接可用。
    ca::Result<std::vector<DbClosureBatchInfo>, AnalysisDbError> listClosureBatches() const;

    /// @brief 复用判定：输入图指纹、种子档位与精度档都命中的「最新有效批」
    ///        （batch_id 最大且已收尾者；残批跳过——与注册表审计语义一致）。
    ///        指纹含 fp_version 载荷段（设计稿 §5），算法升级后旧指纹自然失配；
    ///        seed_mask 与 precision 同匹配——种子档位与精度档都是计算输入的
    ///        一部分，同图不同档位的闭包集不可互复用（schema v10 起三匹配）。
    ///        未命中返回 `std::nullopt`；fingerprint 非 32 字节 → `Invalid`。
    ///        只读连接可用。
    ca::Result<std::optional<DbClosureBatchInfo>, AnalysisDbError> freshClosureBatch(
        const std::vector<ca::u8>& fingerprint, ca::u32 seedMask, ca::u32 precision) const;

    /// @brief 闭包成员集分页：member_id 升序，只返回第 @p offset 行起的
    ///        @p limit 行。分页契约（窗口语义、参数约束、遍历成本不随窗口
    ///        缩小）与引用分页完全相同。批次不存在或不是闭包批 → `Invalid`
    ///        （与 listEntries / listBatches 对未知 id 同口径）。只读连接可用。
    ca::Result<std::vector<ca::i64>, AnalysisDbError> closureMembersPaged(
        ca::i64 batchId, ca::i64 limit, ca::i64 offset) const;

    /// @brief 成员可达判定（单批域）。批次不存在或不是闭包批 → `Invalid`；
    ///        @p memberId <= 0 → `Invalid`。只读连接可用。
    ca::Result<bool, AnalysisDbError> isClosureMember(ca::i64 batchId, ca::i64 memberId) const;

    /// @brief 闭包图边快照：refs 全量方法边与 clinit 触发行（形状见
    ///        `DbClosureGraphEdges`）。全量快照语义（PoC M2 裁决：批量装载
    ///        胜过 per-member 反查一个数量级），行序 ref_id 升序。只读连接可用。
    ca::Result<DbClosureGraphEdges, AnalysisDbError> closureGraphEdges() const;

    /// @brief 闭包输入图指纹载荷（canonical 序列化，设计稿 §5）：
    ///        fp_version 字节 + inputs 投影（input_id 升序：(input_id, size,
    ///        path 长度前缀, path, sha256)）+ classes/interfaces/members/refs
    ///        变更敏感聚合（COUNT/MAX/SUM；refs 聚合含 SUM(ref_id)——ref_id
    ///        单调不复用，任何整库/切片替换必改聚合）。聚合口径是变更敏感
    ///        而非内容寻址（假阳性失效 → 重算，fail-safe 方向）。@p fpVersion
    ///        由调用方盖章（kReachabilityClosureFpVersion）。只读连接可用。
    ca::Result<std::vector<ca::u8>, AnalysisDbError> closureFingerprintPayload(
        ca::u32 fpVersion) const;

private:
    struct Impl;
    explicit AnalysisDb(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}   // namespace mjbase::project
