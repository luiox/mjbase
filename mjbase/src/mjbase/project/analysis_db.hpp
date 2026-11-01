#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

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
/// 批量写入口（`ensureInput` / `ingestEntries` / `ingestClasses` /
/// `replaceReferences` / `setRenames`）都走单事务 + 预编译语句复用；任一行
/// 失败整批回滚，不留半截事实。
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
/// v3：新增 `inputs` / `entries` 条目身份表与 `classes.entry_id` 外键。
/// v2：classes 增加 super/access/version，新增 interfaces 与 refs 表，
/// members 增加 access。v1 未发布，版本号仅用于识别本分支早期的临时库。
inline constexpr ca::u32 kAnalysisDbSchemaVersion = 3;

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
    bool                regular = true; ///< 常规条目旗标（形态判定由登记方定义）
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
    ca::Result<void, AnalysisDbError> setClassRename(
        const ca::str::Utf8StringRef& owner,
        const ca::str::Utf8StringRef& shadow,
        const ca::str::Utf8StringRef& token = {});

    /// @brief 设置成员重命名；成员身份不存在时先登记。已存在则整体覆盖。
    ca::Result<void, AnalysisDbError> setMemberRename(
        DbMemberKind                  kind,
        const ca::str::Utf8StringRef& owner,
        const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& desc,
        const ca::str::Utf8StringRef& shadow,
        const ca::str::Utf8StringRef& token = {});

    /// @brief 批量设置重命名：单事务、语句预编译复用，未提及的既有覆盖保持不变。
    ///
    /// 输入先整体校验（重复目标、重复非空 token、非法字段 → `Invalid`），
    /// 校验失败不写库；执行中任何一行失败（如 token 与库内既有行冲突）整批回滚。
    /// AI 批量重命名脚本走这个入口，不要循环调用单条接口。
    ca::Result<void, AnalysisDbError> setRenames(std::vector<DbRenameRecord> records);

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
    ca::Result<void, AnalysisDbError> replaceRenames(std::vector<DbRenameRecord> records);

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
    /// - 输入不存在、批内重复 path、path/kind 为空或含 NUL、sha256 非 64 位
    ///   小写 hex、size 为负 → `Invalid`，整批不写库。
    ca::Result<DbEntryIngestStats, AnalysisDbError> ingestEntries(
        ca::i64 inputId, std::vector<DbEntryInput> entries);

    /// @brief 全部已登记输入容器，按 path 升序。
    ca::Result<std::vector<DbInputInfo>, AnalysisDbError> listInputs() const;

    /// @brief 指定输入的全部条目（含墓碑行），按 path 升序。
    ///        输入不存在返回 `Invalid`；未登记条目返回空表。
    ca::Result<std::vector<DbEntryInfo>, AnalysisDbError> listEntries(ca::i64 inputId) const;

    //-- 分析事实层（类/成员/继承/直接引用） ------------------------------

    /// @brief 批量登记或更新类、接口与成员（单事务，预编译语句复用）。
    ///
    /// - 身份键为 internal_name；已存在则更新 super/access/version，ID 不变。
    /// - interfaces 整表替换（删除该类的旧行后按声明顺序重插）。
    /// - 成员按 (kind, name, descriptor) upsert access；库内已有但本批未提及的
    ///   成员保留（身份只增不删）。
    /// - 批内类名重复、接口重复、成员重复或字段非法 → `Invalid`，不写库。
    ///
    /// @return 处理的类数。
    ca::Result<ca::usize, AnalysisDbError> ingestClasses(std::vector<DbClassInput> classes);

    /// @brief 原子替换全部直接引用事实（单事务：清空 refs 后批量插入）。
    ///
    /// 来源类/方法必须已在库中（先 `ingestClasses`），否则整批回滚并保留旧
    /// refs。适合整库重建；方法体局部变更后的增量替换由上层切片。
    ///
    /// @return 写入的引用条数。
    ca::Result<ca::usize, AnalysisDbError> replaceReferences(
        std::vector<DbReferenceRecord> references);

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

    /// @brief 接口的实现类：接口自身闭包（含子接口）+ 实现者的全部后代。
    ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> implementors(
        const ca::str::Utf8StringRef& interfaceName) const;

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

    /// @brief 库内直接引用总数。
    ca::Result<ca::usize, AnalysisDbError> referenceCount() const;

private:
    struct Impl;
    explicit AnalysisDb(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}   // namespace mjbase::project
