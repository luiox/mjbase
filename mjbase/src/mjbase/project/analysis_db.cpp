#include "mjbase/project/analysis_db.hpp"

#include "mjbase/analysis/semantic_fp.hpp"
#include "mjbase/project/library_origin.hpp"

#include <sqlite3.h>

#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "mjbase/project/entry_shape.hpp"
#include "mjbase/project/mask_regex_engine.hpp"

namespace mjbase::project {
namespace {

constexpr int kBusyTimeoutMs = 5000;

AnalysisDbError makeError(AnalysisDbErrorKind kind, std::string message)
{
    AnalysisDbError error;
    error.kind    = kind;
    error.message = ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(message.data()), message.size());
    return error;
}

AnalysisDbErrorKind kindOf(int sqliteCode)
{
    switch (sqliteCode & 0xff) {
    case SQLITE_CONSTRAINT: return AnalysisDbErrorKind::Invalid;
    case SQLITE_READONLY: return AnalysisDbErrorKind::ReadOnly;
    case SQLITE_CANTOPEN:
    case SQLITE_NOTADB: return AnalysisDbErrorKind::Open;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
    case SQLITE_IOERR: return AnalysisDbErrorKind::Io;
    default: return AnalysisDbErrorKind::Sqlite;
    }
}

AnalysisDbError sqliteFailure(sqlite3* db, const std::string& context)
{
    return makeError(kindOf(sqlite3_extended_errcode(db)),
                     context + ": " + sqlite3_errmsg(db));
}

template <typename T>
ca::core::types::Err<AnalysisDbError> errorOf(ca::Result<T, AnalysisDbError>&& result)
{
    return ca::Err(std::move(result).unwrap_err());
}

ca::Result<void, AnalysisDbError> exec(sqlite3* db, const char* sql)
{
    char*     message = nullptr;
    const int rc      = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::string detail = message != nullptr ? message : sqlite3_errmsg(db);
        sqlite3_free(message);
        return ca::Err(makeError(kindOf(rc), "exec failed: " + detail));
    }
    return ca::Ok();
}

struct Statement
{
    sqlite3_stmt* raw = nullptr;

    Statement() = default;
    explicit Statement(sqlite3_stmt* statement) : raw(statement) {}
    ~Statement()
    {
        if (raw != nullptr) sqlite3_finalize(raw);
    }
    Statement(Statement&& other) noexcept : raw(other.raw) { other.raw = nullptr; }
    Statement& operator=(Statement&& other) noexcept
    {
        if (this != &other) {
            if (raw != nullptr) sqlite3_finalize(raw);
            raw       = other.raw;
            other.raw = nullptr;
        }
        return *this;
    }
    Statement(const Statement&)            = delete;
    Statement& operator=(const Statement&) = delete;
};

ca::Result<Statement, AnalysisDbError> prepare(sqlite3* db, const char* sql)
{
    sqlite3_stmt* raw = nullptr;
    const int     rc  = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        if (raw != nullptr) sqlite3_finalize(raw);
        return ca::Err(sqliteFailure(db, "prepare failed"));
    }
    return ca::Ok(Statement(raw));
}

ca::Result<bool, AnalysisDbError> step(sqlite3* db, sqlite3_stmt* statement)
{
    const int rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) return ca::Ok(true);
    if (rc == SQLITE_DONE) return ca::Ok(false);
    return ca::Err(sqliteFailure(db, "step failed"));
}

ca::Result<void, AnalysisDbError> bindText(sqlite3* db, sqlite3_stmt* statement, int index,
                                           const ca::str::Utf8StringRef& value)
{
    const int rc = sqlite3_bind_text(statement, index,
                                     reinterpret_cast<const char*>(value.data()),
                                     static_cast<int>(value.byte_length()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) return ca::Err(sqliteFailure(db, "bind text failed"));
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> bindOptionalText(sqlite3* db, sqlite3_stmt* statement,
                                                   int index,
                                                   const ca::str::Utf8StringRef& value)
{
    if (value.is_empty()) {
        if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
            return ca::Err(sqliteFailure(db, "bind null failed"));
        }
        return ca::Ok();
    }
    return bindText(db, statement, index, value);
}

ca::Result<void, AnalysisDbError> bindNull(sqlite3* db, sqlite3_stmt* statement, int index)
{
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        return ca::Err(sqliteFailure(db, "bind null failed"));
    }
    return ca::Ok();
}

/// @brief 绑定零长度文本（''，非 NULL）——id 行清空的 target_name/target_desc。
ca::Result<void, AnalysisDbError> bindEmptyText(sqlite3* db, sqlite3_stmt* statement, int index)
{
    static const char kEmptyText[] = "";
    if (sqlite3_bind_text(statement, index, kEmptyText, 0, SQLITE_TRANSIENT) != SQLITE_OK) {
        return ca::Err(sqliteFailure(db, "bind text failed"));
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> bindInt(sqlite3* db, sqlite3_stmt* statement, int index,
                                          ca::i64 value)
{
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
        return ca::Err(sqliteFailure(db, "bind integer failed"));
    }
    return ca::Ok();
}

/// @brief 绑定定长 BLOB（方法版本指纹 32 字节；SQLITE_TRANSIENT 让 SQLite
///        在 step 前自持拷贝）。
ca::Result<void, AnalysisDbError> bindBlob(sqlite3* db, sqlite3_stmt* statement, int index,
                                           const ca::u8* data, int size)
{
    if (sqlite3_bind_blob(statement, index, data, size, SQLITE_TRANSIENT) != SQLITE_OK) {
        return ca::Err(sqliteFailure(db, "bind blob failed"));
    }
    return ca::Ok();
}

ca::Result<ca::i64, AnalysisDbError> queryScalarInt(sqlite3* db, const char* sql)
{
    auto prepared = prepare(db, sql);
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                 std::string("query returned no row: ") + sql));
    }
    return ca::Ok(static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 0)));
}

std::string textColumn(sqlite3_stmt* statement, int index)
{
    const auto* text = sqlite3_column_text(statement, index);
    if (text == nullptr) return std::string();
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, index)));
}

ca::str::Utf8String utf8Column(sqlite3_stmt* statement, int index)
{
    const auto* text  = sqlite3_column_text(statement, index);
    const int   bytes = sqlite3_column_bytes(statement, index);
    if (text == nullptr || bytes <= 0) return ca::str::Utf8String();
    return ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(text),
                                                    static_cast<ca::usize>(bytes));
}

bool hasNul(const ca::str::Utf8StringRef& value)
{
    return value.byte_length() != 0 && std::memchr(value.data(), 0, value.byte_length()) != nullptr;
}

ca::Result<void, AnalysisDbError> validateRequiredText(const ca::str::Utf8StringRef& value,
                                                       const char*                   field)
{
    if (value.is_empty()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 std::string(field) + " must not be empty"));
    }
    if (hasNul(value)) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 std::string(field) + " must not contain NUL"));
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> validateToken(const ca::str::Utf8StringRef& token)
{
    if (token.is_empty()) return ca::Ok();
    if (token.byte_length() != 8) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "token must be 8 lowercase hex characters"));
    }
    for (ca::usize i = 0; i < token.byte_length(); ++i) {
        const ca::u8 c = token.byte_at(i);
        const bool   digit = c >= '0' && c <= '9';
        const bool   lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "token must be 8 lowercase hex characters"));
        }
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> validateSha256Hex(const ca::str::Utf8StringRef& value,
                                                    const char*                   field)
{
    if (value.byte_length() != 64) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 std::string(field) + " must be 64 lowercase hex characters"));
    }
    for (ca::usize i = 0; i < value.byte_length(); ++i) {
        const ca::u8 c = value.byte_at(i);
        const bool   digit = c >= '0' && c <= '9';
        const bool   lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     std::string(field) + " must be 64 lowercase hex characters"));
        }
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> validateMemberKind(DbMemberKind kind)
{
    if (kind != DbMemberKind::Field && kind != DbMemberKind::Method) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "member kind is invalid"));
    }
    return ca::Ok();
}

/// @brief 校验指纹值：全零字节是未初始化的 SemanticFp（引擎输出是真实
///        SHA-256，不可能是全零），fail-loud 拒绝，不做静默近似。
ca::Result<void, AnalysisDbError> validateMethodFingerprint(const analysis::SemanticFp& fp)
{
    for (const ca::u8 byte : fp.bytes) {
        if (byte != 0) return ca::Ok();
    }
    return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                             "fingerprint must not be all zero (uninitialized SemanticFp?)"));
}

/// @brief 校验分页窗口参数：limit 与 offset 均须非负。
///
/// 0 页与超过命中行数的 offset 都是合法窗口（返回空页）；负值是调用方
/// 契约错误，统一 `Invalid` 拒绝。
ca::Result<void, AnalysisDbError> validatePageWindow(ca::i64 limit, ca::i64 offset)
{
    if (limit < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "page limit must not be negative"));
    }
    if (offset < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "page offset must not be negative"));
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> validateClassFields(const ca::str::Utf8StringRef& owner,
                                                      const ca::str::Utf8StringRef& shadow,
                                                      const ca::str::Utf8StringRef& token)
{
    if (auto result = validateRequiredText(owner, "owner"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(shadow, "shadow name"); result.is_err()) {
        return errorOf(std::move(result));
    }
    return validateToken(token);
}

ca::Result<void, AnalysisDbError> validateMemberFields(DbMemberKind kind,
                                                       const ca::str::Utf8StringRef& owner,
                                                       const ca::str::Utf8StringRef& name,
                                                       const ca::str::Utf8StringRef& desc,
                                                       const ca::str::Utf8StringRef& shadow,
                                                       const ca::str::Utf8StringRef& token)
{
    if (auto result = validateMemberKind(kind); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(owner, "owner"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(name, "member name"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(desc, "member descriptor"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(shadow, "shadow name"); result.is_err()) {
        return errorOf(std::move(result));
    }
    return validateToken(token);
}

ca::Result<void, AnalysisDbError> validateRenameRecord(const DbRenameRecord& record)
{
    if (record.target == RenameTargetKind::Class) {
        if (!record.name.is_empty() || !record.desc.is_empty()) {
            return ca::Err(makeError(
                AnalysisDbErrorKind::Invalid,
                "class rename must not carry a member name or descriptor"));
        }
        return validateClassFields(record.owner, record.shadow, record.token);
    }
    if (record.target == RenameTargetKind::Member) {
        return validateMemberFields(record.memberKind, record.owner, record.name, record.desc,
                                    record.shadow, record.token);
    }
    return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "rename target kind is invalid"));
}

ca::Result<void, AnalysisDbError> validateRenameSet(
    const std::vector<DbRenameRecord>& records)
{
    std::set<std::tuple<int, std::string, int, std::string, std::string>> targets;
    std::set<std::string>                                               tokens;
    for (const auto& record : records) {
        if (auto result = validateRenameRecord(record); result.is_err()) {
            return errorOf(std::move(result));
        }

        std::tuple<int, std::string, int, std::string, std::string> key;
        if (record.target == RenameTargetKind::Class) {
            key = std::make_tuple(static_cast<int>(record.target), record.owner.to_std_string(), 0,
                                  std::string(), std::string());
        }
        else {
            key = std::make_tuple(static_cast<int>(record.target), record.owner.to_std_string(),
                                  static_cast<int>(record.memberKind),
                                  record.name.to_std_string(), record.desc.to_std_string());
        }
        if (!targets.insert(std::move(key)).second) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "duplicate rename target in batch"));
        }

        if (!record.token.is_empty() && !tokens.insert(record.token.to_std_string()).second) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "duplicate token in batch"));
        }
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> validateMemberInput(const DbMemberInput& member)
{
    if (auto result = validateMemberKind(member.kind); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(member.name.ref(), "member name"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(member.descriptor.ref(), "member descriptor");
        result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> validateClassInput(const DbClassInput& input)
{
    if (auto result = validateRequiredText(input.internalName.ref(), "class internal name");
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (hasNul(input.superName.ref())) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "super class name must not contain NUL"));
    }

    std::set<std::string> interfaces;
    for (const auto& name : input.interfaces) {
        if (auto result = validateRequiredText(name.ref(), "interface name"); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (!interfaces.insert(name.to_std_string()).second) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "duplicate interface in class " +
                                         input.internalName.to_std_string()));
        }
    }

    std::set<std::tuple<int, std::string, std::string>> members;
    for (const auto& member : input.members) {
        if (auto result = validateMemberInput(member); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto key = std::make_tuple(static_cast<int>(member.kind), member.name.to_std_string(),
                                   member.descriptor.to_std_string());
        if (!members.insert(std::move(key)).second) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "duplicate member in class " +
                                         input.internalName.to_std_string()));
        }
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> validateClassInputs(const std::vector<DbClassInput>& inputs)
{
    std::set<std::string> names;
    for (const auto& input : inputs) {
        if (auto result = validateClassInput(input); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (!names.insert(input.internalName.to_std_string()).second) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "duplicate class in batch: " +
                                         input.internalName.to_std_string()));
        }
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> validateReference(const DbReferenceRecord& reference)
{
    switch (reference.kind) {
    case DbReferenceKind::Type:
    case DbReferenceKind::Field:
    case DbReferenceKind::Method: break;
    default:
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "reference kind is invalid"));
    }

    if (auto result = validateRequiredText(reference.sourceClass.ref(), "reference source class");
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(reference.targetClass.ref(), "reference target class");
        result.is_err()) {
        return errorOf(std::move(result));
    }
    const bool classLevel = reference.sourceMethod.is_empty();
    if (classLevel) {
        if (!reference.sourceDescriptor.is_empty()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "class-level reference must not carry a method descriptor"));
        }
        if (reference.insnIndex != -1) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "class-level reference must use instruction index -1"));
        }
        if (reference.kind != DbReferenceKind::Type) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "field/method reference must have a source method"));
        }
    }
    else {
        if (auto result =
                validateRequiredText(reference.sourceDescriptor.ref(), "source method descriptor");
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (reference.insnIndex < 0) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "method reference must have a non-negative instruction index"));
        }
    }

    if (reference.kind == DbReferenceKind::Type) {
        if (!reference.targetName.is_empty() || !reference.targetDescriptor.is_empty()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "type reference must not carry target name or descriptor"));
        }
        return ca::Ok();
    }
    if (auto result = validateRequiredText(reference.targetName.ref(), "reference target name");
        result.is_err()) {
        return errorOf(std::move(result));
    }
    return validateRequiredText(reference.targetDescriptor.ref(), "reference target descriptor");
}

ca::Result<void, AnalysisDbError> validateReferences(
    const std::vector<DbReferenceRecord>& references)
{
    for (const auto& reference : references) {
        if (auto result = validateReference(reference); result.is_err()) {
            return errorOf(std::move(result));
        }
    }
    return ca::Ok();
}

std::string nowIso8601()
{
    const std::time_t now = std::time(nullptr);
    std::tm           utc {};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
}

/// @brief 批次种类 → batches.kind 词表（与 DDL 的 CHECK 约束逐字同源）。
const char* batchKindToSql(DbBatchKind kind)
{
    switch (kind) {
    case DbBatchKind::FullImport: return "full_import";
    case DbBatchKind::RefsReplace: return "refs_replace";
    case DbBatchKind::RefsSliceReplace: return "refs_slice_replace";
    case DbBatchKind::RenamesWrite: return "renames_write";
    case DbBatchKind::RulesWrite: return "rules_write";
    case DbBatchKind::ClosureWrite: return "closure_write";
    }
    return nullptr;
}

/// @brief batches.kind 词表 → 批次种类；词表外视为库损坏（CHECK 门内不应出现）。
ca::Result<DbBatchKind, AnalysisDbError> batchKindFromSql(const std::string& text)
{
    if (text == "full_import") return ca::Ok(DbBatchKind::FullImport);
    if (text == "refs_replace") return ca::Ok(DbBatchKind::RefsReplace);
    if (text == "refs_slice_replace") return ca::Ok(DbBatchKind::RefsSliceReplace);
    if (text == "renames_write") return ca::Ok(DbBatchKind::RenamesWrite);
    if (text == "rules_write") return ca::Ok(DbBatchKind::RulesWrite);
    if (text == "closure_write") return ca::Ok(DbBatchKind::ClosureWrite);
    return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                             "batches row has an unknown kind: " + text));
}

class Transaction
{
public:
    static ca::Result<Transaction, AnalysisDbError> begin(sqlite3* db)
    {
        auto result = exec(db, "BEGIN IMMEDIATE");
        if (result.is_err()) return errorOf(std::move(result));
        return ca::Ok(Transaction(db));
    }

    Transaction(Transaction&& other) noexcept : db_(other.db_), active_(other.active_)
    {
        other.active_ = false;
    }
    Transaction& operator=(Transaction&&) = delete;
    Transaction(const Transaction&)      = delete;
    Transaction& operator=(const Transaction&) = delete;

    ~Transaction()
    {
        if (active_ && db_ != nullptr) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    ca::Result<void, AnalysisDbError> commit()
    {
        auto result = exec(db_, "COMMIT");
        if (result.is_ok()) active_ = false;
        return result;
    }

private:
    explicit Transaction(sqlite3* db) : db_(db), active_(true) {}

    sqlite3* db_     = nullptr;
    bool     active_ = false;
};

ca::Result<ca::i64, AnalysisDbError> ensureClass(sqlite3* db,
                                                 const ca::str::Utf8StringRef& internalName)
{
    {
        auto inserted = prepare(
            db,
            "INSERT INTO classes(internal_name) VALUES(?1) "
            "ON CONFLICT(internal_name) DO NOTHING");
        if (inserted.is_err()) return errorOf(std::move(inserted));
        auto statement = std::move(inserted).unwrap();
        if (auto result = bindText(db, statement.raw, 1, internalName); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto done = step(db, statement.raw);
        if (done.is_err()) return errorOf(std::move(done));
    }

    auto selected = prepare(db, "SELECT class_id FROM classes WHERE internal_name=?1");
    if (selected.is_err()) return errorOf(std::move(selected));
    auto statement = std::move(selected).unwrap();
    if (auto result = bindText(db, statement.raw, 1, internalName); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Sqlite,
                                 "class identity vanished after registration"));
    }
    return ca::Ok(static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 0)));
}

ca::Result<ca::i64, AnalysisDbError> ensureMember(sqlite3* db, ca::i64 classId, DbMemberKind kind,
                                                  const ca::str::Utf8StringRef& name,
                                                  const ca::str::Utf8StringRef& desc)
{
    {
        auto inserted = prepare(
            db,
            "INSERT INTO members(class_id, kind, name, descriptor) VALUES(?1, ?2, ?3, ?4) "
            "ON CONFLICT(class_id, kind, name, descriptor) DO NOTHING");
        if (inserted.is_err()) return errorOf(std::move(inserted));
        auto statement = std::move(inserted).unwrap();
        if (auto result = bindInt(db, statement.raw, 1, classId); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(db, statement.raw, 2, static_cast<ca::i64>(kind));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(db, statement.raw, 3, name); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(db, statement.raw, 4, desc); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto done = step(db, statement.raw);
        if (done.is_err()) return errorOf(std::move(done));
    }

    auto selected = prepare(
        db,
        "SELECT member_id FROM members "
        "WHERE class_id=?1 AND kind=?2 AND name=?3 AND descriptor=?4");
    if (selected.is_err()) return errorOf(std::move(selected));
    auto statement = std::move(selected).unwrap();
    if (auto result = bindInt(db, statement.raw, 1, classId); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement.raw, 2, static_cast<ca::i64>(kind)); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, statement.raw, 3, name); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, statement.raw, 4, desc); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Sqlite,
                                 "member identity vanished after registration"));
    }
    return ca::Ok(static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 0)));
}

ca::Result<void, AnalysisDbError> upsertRename(sqlite3* db, ca::i64 targetId,
                                                RenameTargetKind targetKind,
                                                const ca::str::Utf8StringRef& shadow,
                                                const ca::str::Utf8StringRef& token,
                                                const std::string&            updatedAt,
                                                ca::i64                       producerId)
{
    auto prepared = prepare(
        db,
        "INSERT INTO renames(target_kind, target_id, new_name, token, updated_at, producer_id) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(target_kind, target_id) DO UPDATE SET "
        "new_name=excluded.new_name, token=excluded.token, updated_at=excluded.updated_at, "
        "producer_id=excluded.producer_id");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    if (auto result = bindInt(db, statement.raw, 1, static_cast<ca::i64>(targetKind));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement.raw, 2, targetId); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, statement.raw, 3, shadow); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindOptionalText(db, statement.raw, 4, token); result.is_err()) {
        return errorOf(std::move(result));
    }
    const auto updated = ca::str::Utf8StringRef::from_string_view(updatedAt);
    if (auto result = bindText(db, statement.raw, 5, updated); result.is_err()) {
        return errorOf(std::move(result));
    }
    // 行级归属：0 = 匿名写入（producer_id 落 NULL），非 0 随覆盖整体刷新。
    if (auto result = producerId > 0 ? bindInt(db, statement.raw, 6, producerId)
                                     : bindNull(db, statement.raw, 6);
        result.is_err()) {
        return errorOf(std::move(result));
    }

    auto done = step(db, statement.raw);
    if (done.is_err()) return errorOf(std::move(done));
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> deleteRename(sqlite3* db, ca::i64 targetId,
                                                RenameTargetKind targetKind)
{
    auto prepared = prepare(
        db, "DELETE FROM renames WHERE target_kind=?1 AND target_id=?2");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(db, statement.raw, 1, static_cast<ca::i64>(targetKind));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement.raw, 2, targetId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto done = step(db, statement.raw);
    if (done.is_err()) return errorOf(std::move(done));
    return ca::Ok();
}

constexpr const char* kRenameSelectSql =
    "SELECT r.target_kind, r.new_name, r.token, r.updated_at, "
    "       c.internal_name, m.kind, m.name, m.descriptor, mc.internal_name, r.producer_id "
    "FROM renames r "
    "LEFT JOIN classes c ON r.target_kind=0 AND c.class_id=r.target_id "
    "LEFT JOIN members m ON r.target_kind=1 AND m.member_id=r.target_id "
    "LEFT JOIN classes mc ON m.class_id=mc.class_id ";

constexpr const char* kRenameOrderSql =
    "ORDER BY r.target_kind, COALESCE(c.internal_name, mc.internal_name), "
    "         m.kind, m.name, m.descriptor";

ca::Result<DbRenameRecord, AnalysisDbError> readRenameRow(sqlite3_stmt* statement)
{
    DbRenameRecord record;
    record.shadow    = utf8Column(statement, 1);
    record.token     = utf8Column(statement, 2);
    record.updatedAt = utf8Column(statement, 3);
    record.producerId = sqlite3_column_int64(statement, 9);

    const ca::i64 targetKind = sqlite3_column_int64(statement, 0);
    if (targetKind == static_cast<ca::i64>(RenameTargetKind::Class)) {
        record.target = RenameTargetKind::Class;
        record.owner  = utf8Column(statement, 4);
    }
    else if (targetKind == static_cast<ca::i64>(RenameTargetKind::Member)) {
        record.target     = RenameTargetKind::Member;
        record.memberKind = static_cast<DbMemberKind>(sqlite3_column_int(statement, 5));
        record.name       = utf8Column(statement, 6);
        record.desc       = utf8Column(statement, 7);
        record.owner      = utf8Column(statement, 8);
    }
    else {
        return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                 "renames row has an unknown target kind"));
    }
    return ca::Ok(std::move(record));
}

struct RenameBatchStatements
{
    Statement upsertClass;
    Statement selectClass;
    Statement upsertMember;
    Statement selectMember;
    Statement upsertRename;
};

ca::Result<RenameBatchStatements, AnalysisDbError> prepareRenameBatch(sqlite3* db)
{
    RenameBatchStatements statements;

    auto upsertClass = prepare(
        db,
        "INSERT INTO classes(internal_name) VALUES(?1) "
        "ON CONFLICT(internal_name) DO NOTHING");
    if (upsertClass.is_err()) return errorOf(std::move(upsertClass));
    statements.upsertClass = std::move(upsertClass).unwrap();

    auto selectClass = prepare(db, "SELECT class_id FROM classes WHERE internal_name=?1");
    if (selectClass.is_err()) return errorOf(std::move(selectClass));
    statements.selectClass = std::move(selectClass).unwrap();

    auto upsertMember = prepare(
        db,
        "INSERT INTO members(class_id, kind, name, descriptor) VALUES(?1, ?2, ?3, ?4) "
        "ON CONFLICT(class_id, kind, name, descriptor) DO NOTHING");
    if (upsertMember.is_err()) return errorOf(std::move(upsertMember));
    statements.upsertMember = std::move(upsertMember).unwrap();

    auto selectMember = prepare(
        db,
        "SELECT member_id FROM members "
        "WHERE class_id=?1 AND kind=?2 AND name=?3 AND descriptor=?4");
    if (selectMember.is_err()) return errorOf(std::move(selectMember));
    statements.selectMember = std::move(selectMember).unwrap();

    auto upsertRename = prepare(
        db,
        "INSERT INTO renames(target_kind, target_id, new_name, token, updated_at, producer_id) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(target_kind, target_id) DO UPDATE SET "
        "new_name=excluded.new_name, token=excluded.token, updated_at=excluded.updated_at, "
        "producer_id=excluded.producer_id");
    if (upsertRename.is_err()) return errorOf(std::move(upsertRename));
    statements.upsertRename = std::move(upsertRename).unwrap();

    return ca::Ok(std::move(statements));
}

void resetStatement(Statement& statement)
{
    sqlite3_reset(statement.raw);
    sqlite3_clear_bindings(statement.raw);
}

/// @brief 批量写入已校验的重命名记录；调用方负责事务边界。
///
/// 所有语句在批次内预编译复用，class ID 按 owner 缓存；这是批量重命名的
/// 热路径，不要退回逐条 prepare。@p producerId 对全部行级盖章（0 = 匿名）。
ca::Result<void, AnalysisDbError> applyRenameBatch(
    sqlite3* db, const std::vector<DbRenameRecord>& records, ca::i64 producerId)
{
    auto preparedBatch = prepareRenameBatch(db);
    if (preparedBatch.is_err()) return errorOf(std::move(preparedBatch));
    auto batch = std::move(preparedBatch).unwrap();

    std::unordered_map<std::string_view, ca::i64> classIds;
    const std::string                            updatedAt = nowIso8601();

    for (const auto& record : records) {
        const ca::str::Utf8StringRef owner(record.owner);
        const std::string_view       ownerView = owner;

        ca::i64 classId = 0;
        if (const auto it = classIds.find(ownerView); it != classIds.end()) {
            classId = it->second;
        }
        else {
            if (auto result = bindText(db, batch.upsertClass.raw, 1, owner); result.is_err()) {
                return errorOf(std::move(result));
            }
            auto inserted = step(db, batch.upsertClass.raw);
            resetStatement(batch.upsertClass);
            if (inserted.is_err()) return errorOf(std::move(inserted));

            if (auto result = bindText(db, batch.selectClass.raw, 1, owner); result.is_err()) {
                return errorOf(std::move(result));
            }
            auto hasRow = step(db, batch.selectClass.raw);
            if (hasRow.is_err()) return errorOf(std::move(hasRow));
            if (!hasRow.unwrap()) {
                return ca::Err(makeError(AnalysisDbErrorKind::Sqlite,
                                         "class identity vanished during batch registration"));
            }
            classId = sqlite3_column_int64(batch.selectClass.raw, 0);
            resetStatement(batch.selectClass);
            classIds.emplace(ownerView, classId);
        }

        ca::i64 targetId = classId;
        if (record.target == RenameTargetKind::Member) {
            if (auto result = bindInt(db, batch.upsertMember.raw, 1, classId); result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindInt(db, batch.upsertMember.raw, 2,
                                      static_cast<ca::i64>(record.memberKind));
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result =
                    bindText(db, batch.upsertMember.raw, 3, ca::str::Utf8StringRef(record.name));
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result =
                    bindText(db, batch.upsertMember.raw, 4, ca::str::Utf8StringRef(record.desc));
                result.is_err()) {
                return errorOf(std::move(result));
            }
            auto inserted = step(db, batch.upsertMember.raw);
            resetStatement(batch.upsertMember);
            if (inserted.is_err()) return errorOf(std::move(inserted));

            if (auto result = bindInt(db, batch.selectMember.raw, 1, classId); result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindInt(db, batch.selectMember.raw, 2,
                                      static_cast<ca::i64>(record.memberKind));
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result =
                    bindText(db, batch.selectMember.raw, 3, ca::str::Utf8StringRef(record.name));
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result =
                    bindText(db, batch.selectMember.raw, 4, ca::str::Utf8StringRef(record.desc));
                result.is_err()) {
                return errorOf(std::move(result));
            }
            auto hasRow = step(db, batch.selectMember.raw);
            if (hasRow.is_err()) return errorOf(std::move(hasRow));
            if (!hasRow.unwrap()) {
                return ca::Err(makeError(
                    AnalysisDbErrorKind::Sqlite,
                    "member identity vanished during batch registration"));
            }
            targetId = sqlite3_column_int64(batch.selectMember.raw, 0);
            resetStatement(batch.selectMember);
        }

        if (auto result =
                bindInt(db, batch.upsertRename.raw, 1, static_cast<ca::i64>(record.target));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(db, batch.upsertRename.raw, 2, targetId); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result =
                bindText(db, batch.upsertRename.raw, 3, ca::str::Utf8StringRef(record.shadow));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindOptionalText(db, batch.upsertRename.raw, 4,
                                           ca::str::Utf8StringRef(record.token));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result =
                bindText(db, batch.upsertRename.raw, 5,
                         ca::str::Utf8StringRef::from_string_view(updatedAt));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        // 行级归属：0 = 匿名写入（producer_id 落 NULL），非 0 随覆盖整体刷新。
        if (auto result = producerId > 0 ? bindInt(db, batch.upsertRename.raw, 6, producerId)
                                         : bindNull(db, batch.upsertRename.raw, 6);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto upserted = step(db, batch.upsertRename.raw);
        resetStatement(batch.upsertRename);
        if (upserted.is_err()) return errorOf(std::move(upserted));
    }
    return ca::Ok();
}

/// @brief 批量写入已校验的类输入；调用方负责事务边界。
///
/// @p classIdsOut 按输入序回收每个类的 class_id（作用域锚写入用；调用方
/// 传 batchId != 0 时必读）。
ca::Result<void, AnalysisDbError> applyClassBatch(sqlite3*                         db,
                                                  const std::vector<DbClassInput>& classes,
                                                  std::vector<ca::i64>&            classIdsOut)
{
    classIdsOut.clear();
    classIdsOut.reserve(classes.size());
    // entry_id 语义：>0 设置链接（FK 校验失败整批回滚），0 = 未指定——
    // 已登记类保持既有链接，新登记类存 NULL。
    auto upsertClass = prepare(
        db,
        "INSERT INTO classes(internal_name, super_name, access, version, entry_id) "
        "VALUES(?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(internal_name) DO UPDATE SET "
        "super_name=excluded.super_name, access=excluded.access, version=excluded.version, "
        "entry_id=CASE WHEN ?5 IS NULL THEN entry_id ELSE excluded.entry_id END");
    if (upsertClass.is_err()) return errorOf(std::move(upsertClass));
    auto upsertClassStatement = std::move(upsertClass).unwrap();

    auto selectClass = prepare(db, "SELECT class_id FROM classes WHERE internal_name=?1");
    if (selectClass.is_err()) return errorOf(std::move(selectClass));
    auto selectClassStatement = std::move(selectClass).unwrap();

    auto deleteInterfaces = prepare(db, "DELETE FROM interfaces WHERE class_id=?1");
    if (deleteInterfaces.is_err()) return errorOf(std::move(deleteInterfaces));
    auto deleteInterfacesStatement = std::move(deleteInterfaces).unwrap();

    auto insertInterface = prepare(
        db, "INSERT INTO interfaces(class_id, position, interface_name) VALUES(?1, ?2, ?3)");
    if (insertInterface.is_err()) return errorOf(std::move(insertInterface));
    auto insertInterfaceStatement = std::move(insertInterface).unwrap();

    auto upsertMember = prepare(
        db,
        "INSERT INTO members(class_id, kind, name, descriptor, access) "
        "VALUES(?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(class_id, kind, name, descriptor) DO UPDATE SET access=excluded.access");
    if (upsertMember.is_err()) return errorOf(std::move(upsertMember));
    auto upsertMemberStatement = std::move(upsertMember).unwrap();

    for (const DbClassInput& input : classes) {
        if (auto result = bindText(db, upsertClassStatement.raw, 1, input.internalName.ref());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(db, upsertClassStatement.raw, 2, input.superName.ref());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(db, upsertClassStatement.raw, 3, input.access); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(db, upsertClassStatement.raw, 4, input.version); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (input.entryId > 0) {
            if (auto result = bindInt(db, upsertClassStatement.raw, 5, input.entryId);
                result.is_err()) {
                return errorOf(std::move(result));
            }
        }
        else if (auto result = bindNull(db, upsertClassStatement.raw, 5); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto done = step(db, upsertClassStatement.raw);
        resetStatement(upsertClassStatement);
        if (done.is_err()) return errorOf(std::move(done));

        if (auto result = bindText(db, selectClassStatement.raw, 1, input.internalName.ref());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(db, selectClassStatement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            resetStatement(selectClassStatement);
            return ca::Err(makeError(AnalysisDbErrorKind::Sqlite,
                                     "class identity vanished after upsert: " +
                                         input.internalName.to_std_string()));
        }
        const ca::i64 classId = sqlite3_column_int64(selectClassStatement.raw, 0);
        resetStatement(selectClassStatement);
        classIdsOut.push_back(classId);

        if (auto result = bindInt(db, deleteInterfacesStatement.raw, 1, classId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        done = step(db, deleteInterfacesStatement.raw);
        resetStatement(deleteInterfacesStatement);
        if (done.is_err()) return errorOf(std::move(done));

        for (ca::usize position = 0; position < input.interfaces.size(); ++position) {
            if (auto result = bindInt(db, insertInterfaceStatement.raw, 1, classId);
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindInt(db, insertInterfaceStatement.raw, 2,
                                      static_cast<ca::i64>(position));
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result =
                    bindText(db, insertInterfaceStatement.raw, 3, input.interfaces[position].ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            done = step(db, insertInterfaceStatement.raw);
            resetStatement(insertInterfaceStatement);
            if (done.is_err()) return errorOf(std::move(done));
        }

        for (const DbMemberInput& member : input.members) {
            if (auto result = bindInt(db, upsertMemberStatement.raw, 1, classId); result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindInt(db, upsertMemberStatement.raw, 2,
                                      static_cast<ca::i64>(member.kind));
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(db, upsertMemberStatement.raw, 3, member.name.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(db, upsertMemberStatement.raw, 4, member.descriptor.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindInt(db, upsertMemberStatement.raw, 5, member.access);
                result.is_err()) {
                return errorOf(std::move(result));
            }
            done = step(db, upsertMemberStatement.raw);
            resetStatement(upsertMemberStatement);
            if (done.is_err()) return errorOf(std::move(done));
        }
    }
    return ca::Ok();
}

/// @brief 目标 owner 名 → class_id（批内 memo；未登记 memo 0）。
///
/// 与 source 解析共用同一条 select 语句（bind→step→reset 串行复用）；与
/// source 侧「未登记即报错」不同，目标自由是 replaceReferences 的合同，未
/// 登记是合法状态（落文本分支），memo 记 0。每个不同目标至多两次唯一索引
/// 探测，批内 memo 后总量受不同目标数约束。
ca::Result<ca::i64, AnalysisDbError> resolveTargetClassId(
    sqlite3* db, Statement& selectClass,
    std::unordered_map<std::string_view, ca::i64>& memo, const ca::str::Utf8StringRef& owner)
{
    const std::string_view ownerView = owner;
    auto                   it        = memo.find(ownerView);
    if (it != memo.end()) return ca::Ok(it->second);

    if (auto result = bindText(db, selectClass.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, selectClass.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    const ca::i64 classId = hasRow.unwrap() ? sqlite3_column_int64(selectClass.raw, 0) : 0;
    resetStatement(selectClass);
    memo.emplace(ownerView, classId);
    return ca::Ok(classId);
}

/// @brief (class_id, member kind, name, desc) → member_id（批内 memo；未登记
///        memo 0）。kind 区分字段与方法，与 members 的类内唯一键同形。
ca::Result<ca::i64, AnalysisDbError> resolveTargetMemberId(
    sqlite3* db, Statement& selectMember,
    std::map<std::tuple<ca::i64, ca::i64, std::string_view, std::string_view>, ca::i64>& memo,
    ca::i64 classId, DbMemberKind kind, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& desc)
{
    auto key = std::make_tuple(classId, static_cast<ca::i64>(kind), std::string_view(name),
                               std::string_view(desc));
    auto it  = memo.find(key);
    if (it != memo.end()) return ca::Ok(it->second);

    if (auto result = bindInt(db, selectMember.raw, 1, classId); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, selectMember.raw, 2, static_cast<ca::i64>(kind));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, selectMember.raw, 3, name); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, selectMember.raw, 4, desc); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, selectMember.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    const ca::i64 memberId = hasRow.unwrap() ? sqlite3_column_int64(selectMember.raw, 0) : 0;
    resetStatement(selectMember);
    memo.emplace(std::move(key), memberId);
    return ca::Ok(memberId);
}

/// @brief 批量写入已校验的引用记录；调用方负责事务边界与 refs 清理。
///
/// 目标整体二元化（v5）：owner 名与 (class_id, kind, name, desc) 成员各自
/// memo 化解析，共解析则整目标落 id（文本三列清空、target_class 为 NULL），
/// 任一未命中整体落文本（两个 id 列为 NULL）——无「owner 走 id、成员走文本」
/// 的混合行。目标文本列按字节语义绑定（带长度 bind_text，不做 UTF-8 校验），
/// 对抗名原样入库。
ca::Result<void, AnalysisDbError> applyReferenceBatch(
    sqlite3* db, const std::vector<DbReferenceRecord>& references)
{
    auto insert = prepare(
        db,
        "INSERT INTO refs(source_class_id, source_member_id, insn_index, ref_kind, "
        "                 target_class_id, target_member_id, "
        "                 target_class, target_name, target_desc) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
    if (insert.is_err()) return errorOf(std::move(insert));
    auto insertStatement = std::move(insert).unwrap();

    auto selectClass = prepare(db, "SELECT class_id FROM classes WHERE internal_name=?1");
    if (selectClass.is_err()) return errorOf(std::move(selectClass));
    auto selectClassStatement = std::move(selectClass).unwrap();

    auto selectMember = prepare(
        db, "SELECT member_id FROM members WHERE class_id=?1 AND kind=1 AND name=?2 AND descriptor=?3");
    if (selectMember.is_err()) return errorOf(std::move(selectMember));
    auto selectMemberStatement = std::move(selectMember).unwrap();

    auto selectTargetMember = prepare(
        db, "SELECT member_id FROM members WHERE class_id=?1 AND kind=?2 AND name=?3 AND descriptor=?4");
    if (selectTargetMember.is_err()) return errorOf(std::move(selectTargetMember));
    auto selectTargetMemberStatement = std::move(selectTargetMember).unwrap();

    std::unordered_map<std::string_view, ca::i64> classIds;
    std::map<std::tuple<ca::i64, std::string_view, std::string_view>, ca::i64> memberIds;
    // target memo：owner 名 → class_id（0 = 未登记），(class_id, kind, name,
    // desc) → member_id（0 = 未登记）。与 source memo 并列、各管各的。
    std::unordered_map<std::string_view, ca::i64> targetClassIds;
    std::map<std::tuple<ca::i64, ca::i64, std::string_view, std::string_view>, ca::i64>
        targetMemberIds;

    for (const DbReferenceRecord& reference : references) {
        const std::string_view sourceClass = reference.sourceClass.ref();
        auto                   classIt     = classIds.find(sourceClass);
        if (classIt == classIds.end()) {
            if (auto result = bindText(db, selectClassStatement.raw, 1, reference.sourceClass.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            auto hasRow = step(db, selectClassStatement.raw);
            if (hasRow.is_err()) return errorOf(std::move(hasRow));
            if (!hasRow.unwrap()) {
                resetStatement(selectClassStatement);
                return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                         "reference source class is not registered: " +
                                             reference.sourceClass.to_std_string()));
            }
            classIt = classIds.emplace(sourceClass,
                                       sqlite3_column_int64(selectClassStatement.raw, 0))
                          .first;
            resetStatement(selectClassStatement);
        }
        const ca::i64 classId = classIt->second;

        ca::i64 memberId = 0;
        if (!reference.sourceMethod.is_empty()) {
            auto key = std::make_tuple(classId, std::string_view(reference.sourceMethod.ref()),
                                       std::string_view(reference.sourceDescriptor.ref()));
            auto memberIt = memberIds.find(key);
            if (memberIt == memberIds.end()) {
                if (auto result = bindInt(db, selectMemberStatement.raw, 1, classId);
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                if (auto result =
                        bindText(db, selectMemberStatement.raw, 2, reference.sourceMethod.ref());
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                if (auto result = bindText(db, selectMemberStatement.raw, 3,
                                           reference.sourceDescriptor.ref());
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                auto hasRow = step(db, selectMemberStatement.raw);
                if (hasRow.is_err()) return errorOf(std::move(hasRow));
                if (!hasRow.unwrap()) {
                    resetStatement(selectMemberStatement);
                    return ca::Err(makeError(
                        AnalysisDbErrorKind::Invalid,
                        "reference source member is not registered: " +
                            reference.sourceClass.to_std_string() + "." +
                            reference.sourceMethod.to_std_string() + "(" +
                            reference.sourceDescriptor.to_std_string() + ")"));
                }
                memberIt = memberIds.emplace(key,
                                             sqlite3_column_int64(selectMemberStatement.raw, 0))
                               .first;
                resetStatement(selectMemberStatement);
            }
            memberId = memberIt->second;
        }

        // 目标解析：owner 与成员共解析则整目标落 id，任一未命中整体落文本。
        auto targetClassId = resolveTargetClassId(db, selectClassStatement, targetClassIds,
                                                  reference.targetClass.ref());
        if (targetClassId.is_err()) return errorOf(std::move(targetClassId));

        ca::i64 targetMemberId = 0;
        if (reference.kind != DbReferenceKind::Type && targetClassId.unwrap() != 0) {
            const auto memberKind = static_cast<DbMemberKind>(static_cast<int>(reference.kind) - 1);
            auto resolved = resolveTargetMemberId(db, selectTargetMemberStatement, targetMemberIds,
                                                  targetClassId.unwrap(), memberKind,
                                                  reference.targetName.ref(),
                                                  reference.targetDescriptor.ref());
            if (resolved.is_err()) return errorOf(std::move(resolved));
            targetMemberId = resolved.unwrap();
        }
        const bool targetOnIds = targetClassId.unwrap() != 0 &&
                                 (reference.kind == DbReferenceKind::Type || targetMemberId != 0);

        if (auto result = bindInt(db, insertStatement.raw, 1, classId); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (memberId == 0) {
            if (auto result = bindNull(db, insertStatement.raw, 2); result.is_err()) {
                return errorOf(std::move(result));
            }
        }
        else if (auto result = bindInt(db, insertStatement.raw, 2, memberId); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(db, insertStatement.raw, 3, reference.insnIndex);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(db, insertStatement.raw, 4,
                                  static_cast<ca::i64>(reference.kind));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (targetOnIds) {
            if (auto result = bindInt(db, insertStatement.raw, 5, targetClassId.unwrap());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (targetMemberId == 0) {
                if (auto result = bindNull(db, insertStatement.raw, 6); result.is_err()) {
                    return errorOf(std::move(result));
                }
            }
            else if (auto result = bindInt(db, insertStatement.raw, 6, targetMemberId);
                     result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindNull(db, insertStatement.raw, 7); result.is_err()) {
                return errorOf(std::move(result));
            }
            // 文本三列清空（''非 NULL）：成员 id 行不携带名字与描述符文本，
            // CHECK 拒绝混合行。
            if (auto result = bindEmptyText(db, insertStatement.raw, 8); result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindEmptyText(db, insertStatement.raw, 9); result.is_err()) {
                return errorOf(std::move(result));
            }
        }
        else {
            if (auto result = bindNull(db, insertStatement.raw, 5); result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindNull(db, insertStatement.raw, 6); result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(db, insertStatement.raw, 7, reference.targetClass.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(db, insertStatement.raw, 8, reference.targetName.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(db, insertStatement.raw, 9, reference.targetDescriptor.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
        }
        auto done = step(db, insertStatement.raw);
        resetStatement(insertStatement);
        if (done.is_err()) return errorOf(std::move(done));
    }
    return ca::Ok();
}

DbClassInfo readClassRow(sqlite3_stmt* statement)
{
    DbClassInfo info;
    info.classId      = sqlite3_column_int64(statement, 0);
    info.internalName = utf8Column(statement, 1);
    info.superName    = utf8Column(statement, 2);
    info.access       = sqlite3_column_int(statement, 3);
    info.version      = sqlite3_column_int(statement, 4);
    info.entryId      = sqlite3_column_int64(statement, 5);
    return info;
}

DbMemberInfo readMemberRow(sqlite3_stmt* statement)
{
    DbMemberInfo info;
    info.memberId   = sqlite3_column_int64(statement, 0);
    info.kind       = static_cast<DbMemberKind>(sqlite3_column_int(statement, 1));
    info.name       = utf8Column(statement, 2);
    info.descriptor = utf8Column(statement, 3);
    info.access     = sqlite3_column_int(statement, 4);
    return info;
}

DbReferenceRecord readReferenceRow(sqlite3_stmt* statement)
{
    DbReferenceRecord record;
    record.sourceClass      = utf8Column(statement, 0);
    record.sourceMethod     = utf8Column(statement, 1);
    record.sourceDescriptor = utf8Column(statement, 2);
    record.insnIndex        = sqlite3_column_int64(statement, 3);
    record.kind             = static_cast<DbReferenceKind>(sqlite3_column_int(statement, 4));
    record.targetClass      = utf8Column(statement, 5);
    record.targetName       = utf8Column(statement, 6);
    record.targetDescriptor = utf8Column(statement, 7);
    return record;
}

/// @brief 引用查询输出投影：显示列 8 个（DbReferenceRecord 字段序）。
///
/// 目标 id 化（v5）：目标文本经两个 LEFT JOIN 从身份层重建，COALESCE 回退
/// refs 内联文本列——id 行（文本三列清空）由 JOIN 补出名字，文本行（id 列
/// NULL，JOIN 不命中）回退原文本；目标文本按字节语义透传，不做 UTF-8 解码。
constexpr const char* kReferenceSelectSql =
    "SELECT c.internal_name, COALESCE(m.name, ''), COALESCE(m.descriptor, ''), "
    "       r.insn_index, r.ref_kind, "
    "       COALESCE(tc.internal_name, r.target_class), "
    "       COALESCE(tm.name, r.target_name), "
    "       COALESCE(tm.descriptor, r.target_desc) "
    "FROM refs r "
    "JOIN classes c ON c.class_id=r.source_class_id "
    "LEFT JOIN members m ON m.member_id=r.source_member_id "
    "LEFT JOIN classes tc ON tc.class_id=r.target_class_id "
    "LEFT JOIN members tm ON tm.member_id=r.target_member_id ";

constexpr const char* kReferenceOrderSql =
    "ORDER BY r.source_class_id, r.source_member_id, r.insn_index, r.ref_id";

/// @brief 目标侧两分支共用的分支 select/join 形状（UNION ALL 各带一份）。
///
/// 显示列与 kReferenceSelectSql 同序同义，尾随 4 个排序键列；别名供外层
/// 引用——复合 SELECT 的 ORDER BY 只能引用输出列，不能引用表别名限定名。
constexpr const char* kReferenceTargetBranchSql =
    "SELECT c.internal_name AS source_class, COALESCE(m.name, '') AS source_member, "
    "       COALESCE(m.descriptor, '') AS source_descriptor, "
    "       r.insn_index AS insn_index, r.ref_kind AS ref_kind, "
    "       COALESCE(tc.internal_name, r.target_class) AS target_class, "
    "       COALESCE(tm.name, r.target_name) AS target_name, "
    "       COALESCE(tm.descriptor, r.target_desc) AS target_desc, "
    "       r.source_class_id AS order_class, r.source_member_id AS order_member, "
    "       r.insn_index AS order_insn, r.ref_id AS order_ref "
    "FROM refs r "
    "JOIN classes c ON c.class_id=r.source_class_id "
    "LEFT JOIN members m ON m.member_id=r.source_member_id "
    "LEFT JOIN classes tc ON tc.class_id=r.target_class_id "
    "LEFT JOIN members tm ON tm.member_id=r.target_member_id ";

constexpr const char* kReferenceUnionHeadSql =
    "SELECT v.source_class, v.source_member, v.source_descriptor, v.insn_index, v.ref_kind, "
    "       v.target_class, v.target_name, v.target_desc FROM (";

constexpr const char* kReferenceUnionTailSql =
    ") v ORDER BY v.order_class, v.order_member, v.order_insn, v.order_ref ";

/// @brief 组装目标侧两分支 UNION ALL 查询：id 分支走 refs_target_id（解析
///        id 后双列/前缀下探），文本分支走 refs_target_text——两分支按
///        target_class_id 是否 NULL 互斥（任一行只会是 id 形状或文本形状），
///        UNION ALL 不重不漏；外层按源侧同键排序，分页窗口追加在其后。
std::string makeTargetReferenceSql(const char* idBranchWhere, const char* textBranchWhere)
{
    return std::string(kReferenceUnionHeadSql) + kReferenceTargetBranchSql + idBranchWhere +
           "UNION ALL " + kReferenceTargetBranchSql + textBranchWhere + kReferenceUnionTailSql;
}

/// @brief 执行引用查询并按行序收集；@p pageLimit 非负时在 SQL 末尾追加
///        LIMIT/OFFSET 窗口（参数号紧跟既有绑定之后，绑 @p pageLimit /
///        @p pageOffset）。@p pageLimit < 0 表示无窗口（@p pageOffset 被忽
///        略）——内部调用方约定：分页入口必须先过 `validatePageWindow`。
ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> runReferenceQuery(
    sqlite3* db, const std::string& sql, int bindCount, ca::i64 pageLimit, ca::i64 pageOffset,
    const std::function<ca::Result<void, AnalysisDbError>(sqlite3_stmt*)>& bind)
{
    std::string windowSql = sql;
    if (pageLimit >= 0) {
        windowSql += " LIMIT ?" + std::to_string(bindCount + 1) +
                     " OFFSET ?" + std::to_string(bindCount + 2);
    }
    auto prepared = prepare(db, windowSql.c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (bindCount > 0) {
        if (auto result = bind(statement.raw); result.is_err()) {
            return errorOf(std::move(result));
        }
    }
    if (pageLimit >= 0) {
        if (auto result = bindInt(db, statement.raw, bindCount + 1, pageLimit);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(db, statement.raw, bindCount + 2, pageOffset);
            result.is_err()) {
            return errorOf(std::move(result));
        }
    }

    std::vector<DbReferenceRecord> references;
    for (;;) {
        auto hasRow = step(db, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        references.push_back(readReferenceRow(statement.raw));
    }
    return ca::Ok(std::move(references));
}

/// @brief 内部名 → class_id；未登记返回 0。
///
/// 按源反查的两步走第一步：直接以 c.internal_name 过滤 refs 的 JOIN 形状
/// 会让规划器为免排序选「全索引扫 refs + 逐行回表探 classes」（B6 实测
/// rt.jar 2.3M 行 565ms）；先解析 id 再下探 refs_source_class 索引。
ca::Result<ca::i64, AnalysisDbError> classIdByName(sqlite3*                     db,
                                                   const ca::str::Utf8StringRef& owner)
{
    auto prepared = prepare(db, "SELECT class_id FROM classes WHERE internal_name=?1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(db, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(static_cast<ca::i64>(0));
    return ca::Ok(static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 0)));
}

/// @brief (owner, member kind, name, descriptor) → member_id；未登记返回 0。
///
/// 按源反查的两步走第一步（来源成员恒为方法）；目标侧 id 分支解析同用此
/// 形状（kind 与写入侧 target memo 同源），字段与方法同名同描述符的病态
/// 输入也解析到与引用 ref_kind 一致的成员行。
ca::Result<ca::i64, AnalysisDbError> memberIdByName(sqlite3*                     db,
                                                    DbMemberKind                 kind,
                                                    const ca::str::Utf8StringRef& owner,
                                                    const ca::str::Utf8StringRef& name,
                                                    const ca::str::Utf8StringRef& desc)
{
    auto prepared = prepare(
        db, "SELECT m.member_id FROM members m JOIN classes c ON c.class_id=m.class_id "
            "WHERE c.internal_name=?1 AND m.kind=?2 AND m.name=?3 AND m.descriptor=?4");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(db, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement.raw, 2, static_cast<ca::i64>(kind));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, statement.raw, 3, name); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, statement.raw, 4, desc); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(static_cast<ca::i64>(0));
    return ca::Ok(static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 0)));
}

constexpr const char* kSchemaDdl[] = {
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID",
    // 条目身份层（v3）：inputs 以 path 为身份键；entries 以 entry_id 单调
    // 分配、只增不删。同 (input_id, path) 同时至多一行活跃（partial unique
    // index），历史行靠墓碑保留。
    "CREATE TABLE inputs("
    "  input_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  path TEXT NOT NULL UNIQUE,"
    "  size INTEGER NOT NULL DEFAULT 0,"
    "  sha256 TEXT NOT NULL DEFAULT ''"
    ")",
    "CREATE TABLE entries("
    "  entry_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  input_id INTEGER NOT NULL REFERENCES inputs(input_id),"
    "  path TEXT NOT NULL,"
    "  kind TEXT NOT NULL,"
    "  size INTEGER NOT NULL DEFAULT 0,"
    "  sha256 TEXT NOT NULL,"
    "  regular INTEGER NOT NULL DEFAULT 1,"
    "  tombstoned INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(input_id, path, sha256)"
    ")",
    "CREATE UNIQUE INDEX entries_live ON entries(input_id, path) WHERE tombstoned=0",
    "CREATE TABLE classes("
    "  class_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  internal_name TEXT NOT NULL UNIQUE,"
    "  super_name TEXT NOT NULL DEFAULT '',"
    "  access INTEGER NOT NULL DEFAULT 0,"
    "  version INTEGER NOT NULL DEFAULT 0,"
    "  entry_id INTEGER REFERENCES entries(entry_id) ON DELETE SET NULL"
    ")",
    "CREATE TABLE interfaces("
    "  class_id INTEGER NOT NULL REFERENCES classes(class_id) ON DELETE CASCADE,"
    "  position INTEGER NOT NULL,"
    "  interface_name TEXT NOT NULL,"
    "  PRIMARY KEY(class_id, position)"
    ") WITHOUT ROWID",
    "CREATE INDEX interfaces_name ON interfaces(interface_name)",
    "CREATE TABLE members("
    "  member_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  class_id INTEGER NOT NULL REFERENCES classes(class_id) ON DELETE CASCADE,"
    "  kind INTEGER NOT NULL CHECK(kind IN (0,1)),"
    "  name TEXT NOT NULL,"
    "  descriptor TEXT NOT NULL,"
    "  access INTEGER NOT NULL DEFAULT 0,"
    "  UNIQUE(class_id, kind, name, descriptor)"
    ")",
    // 引用目标 id 化（v5）：目标整体二元化——写入时 owner 与成员共解析则整
    // 目标落 id（文本三列清空，target_class 为 NULL），任一未命中整体落文本
    // （两个 id 列为 NULL）；三条 CHECK 与代码校验同源，混合行拒写。target_
    // class_id / target_member_id 复用身份层既有稳定 id，不引入第三套 id。
    "CREATE TABLE refs("
    "  ref_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  source_class_id INTEGER NOT NULL REFERENCES classes(class_id) ON DELETE CASCADE,"
    "  source_member_id INTEGER REFERENCES members(member_id) ON DELETE CASCADE,"
    "  insn_index INTEGER NOT NULL DEFAULT -1,"
    "  ref_kind INTEGER NOT NULL CHECK(ref_kind IN (0,1,2)),"
    "  target_class_id INTEGER REFERENCES classes(class_id),"
    "  target_member_id INTEGER REFERENCES members(member_id),"
    "  target_class TEXT,"
    "  target_name TEXT NOT NULL DEFAULT '',"
    "  target_desc TEXT NOT NULL DEFAULT '',"
    "  CHECK((target_class_id IS NULL) != (target_class IS NULL)),"
    "  CHECK(target_member_id IS NULL OR ref_kind != 0),"
    "  CHECK(target_member_id IS NULL OR (target_name = '' AND target_desc = ''))"
    ")",
    // id 分支索引：目标登记行（基准语料稳态下的主体）按 (owner, member) 双列
    // 精确下探；文本分支索引只携带未登记行——外部符号与对抗文本集中于此，
    // 基准语料稳态下近似为空。
    "CREATE INDEX refs_target_id ON refs(target_class_id, target_member_id) "
    "  WHERE target_class_id IS NOT NULL",
    "CREATE INDEX refs_target_text ON refs(ref_kind, target_class, target_name, target_desc) "
    "  WHERE target_class_id IS NULL",
    "CREATE INDEX refs_source_member ON refs(source_member_id)",
    // 按源类反查（referencesFromClass）：无此索引时按 c.internal_name 驱动
    // 只能全表扫 refs（B6 实测 rt.jar 语料 2.3M 行 565ms）。重建 refs 表时
    // 两条 source 侧索引必须照抄——PoC 实测漏建后 referencesFromMember 从
    // 0.575ms 退化为 1.672ms 全表扫。
    "CREATE INDEX refs_source_class ON refs(source_class_id)",
    // renames 行级归属（v6 #143 3.4）：覆盖层与配置型小表，行级归属即语义
    // （「这条覆盖是谁、何时设置的」就是行问题）。可空外键追加在列序末位，
    // 既有列序与两条索引保持不动；NULL = 匿名写入（写入口 producerId 缺省
    // 0），覆盖行为不变 last-write-wins，producer_id 随覆盖整体刷新。
    "CREATE TABLE renames("
    "  target_kind INTEGER NOT NULL CHECK(target_kind IN (0,1)),"
    "  target_id INTEGER NOT NULL,"
    "  new_name TEXT NOT NULL,"
    "  token TEXT,"
    "  updated_at TEXT NOT NULL,"
    "  producer_id INTEGER REFERENCES producers(producer_id),"
    "  PRIMARY KEY(target_kind, target_id)"
    ") WITHOUT ROWID",
    "CREATE UNIQUE INDEX renames_token ON renames(token) WHERE token IS NOT NULL",
    "CREATE INDEX renames_new_name ON renames(new_name)",
    // 出口遮蔽层（v4）：规则是项目级配置（整体替换、rule_id 单调分配），
    // hit_count 是命中记账（含被 suspect 压制的 allow 命中），查询序与
    // 求值器装载序同源：(priority, rule_id) 升序。
    // 行级归属（v7 #143 遗留回填）：与 renames 同为覆盖层与配置型小表，
    // 行级归属即语义。可空外键追加在列序末位，既有列序与既有索引保持
    // 不动；NULL = 匿名写入（写入口 producerId 缺省 0），整体替换后新行
    // 统一盖章，覆盖来源事后可查。
    "CREATE TABLE mask_rules("
    "  rule_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  priority INTEGER NOT NULL DEFAULT 0,"
    "  pattern TEXT NOT NULL,"
    "  action INTEGER NOT NULL CHECK(action IN (0,1)),"
    "  note TEXT NOT NULL DEFAULT '',"
    "  hit_count INTEGER NOT NULL DEFAULT 0,"
    "  producer_id INTEGER REFERENCES producers(producer_id)"
    ")",
    "CREATE INDEX mask_rules_order ON mask_rules(priority, rule_id)",
    // producer 注册表（v6，#143）：登记向本库写事实的身份与逻辑写入运行。
    // producers 注册幂等（key 唯一）、id 单调只增不删；trust_level 两档，
    // 词表对齐 MaskTrustLevel 但语义独立（事实来源可信级别 vs 项目出口默认
    // 动作），不复用同一 C++ 枚举。tool_fingerprint 由注册方自报，mjbase
    // 不解析其内部结构。
    "CREATE TABLE producers("
    "  producer_id      INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  key              TEXT NOT NULL UNIQUE,"
    "  name             TEXT NOT NULL,"
    "  tool_fingerprint TEXT NOT NULL DEFAULT '',"
    "  trust_level      INTEGER NOT NULL CHECK(trust_level IN (0,1)),"
    "  created_at       TEXT NOT NULL,"
    "  updated_at       TEXT NOT NULL"
    ")",
    // 批次是逻辑写入运行，不是事务：openBatch 落 started 行，各写入口在各自
    // 事务内写事实与作用域行，closeBatch 收尾。finished_at 空串 = 未收尾的
    // 残批（失败/中断），是合法审计状态，可读可见。kind 词表与 DbBatchKind
    // 同源；renames_write / rules_write 为覆盖层与规则面消费方预留，
    // closure_write 为可达闭包派生事实写入（v9 #159）。
    "CREATE TABLE batches("
    "  batch_id     INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  producer_id  INTEGER NOT NULL REFERENCES producers(producer_id),"
    "  kind         TEXT NOT NULL CHECK(kind IN ('full_import','refs_replace',"
    "               'refs_slice_replace','renames_write','rules_write','closure_write')),"
    "  started_at   TEXT NOT NULL,"
    "  finished_at  TEXT NOT NULL DEFAULT '',"
    "  class_count  INTEGER NOT NULL DEFAULT 0,"
    "  entry_count  INTEGER NOT NULL DEFAULT 0,"
    "  member_count INTEGER NOT NULL DEFAULT 0,"
    "  ref_count    INTEGER NOT NULL DEFAULT 0,"
    "  note         TEXT NOT NULL DEFAULT ''"
    ")",
    // 作用域锚（v6 #143 3.2）：归属单位是写入运行，作用域锚记录这次运行
    // 覆盖的事实区间；逐行归属经作用域解析回答，事实表不加归属列。
    // scope_kind：0=refs_all（scope_id 恒 0）、1=input、2=class、3=member；
    // 成员登记的归属并入其所在类（ingestClasses 的 class 作用域行）。
    "CREATE TABLE batch_scopes("
    "  batch_id   INTEGER NOT NULL REFERENCES batches(batch_id),"
    "  scope_kind INTEGER NOT NULL CHECK(scope_kind IN (0,1,2,3)),"
    "  scope_id   INTEGER NOT NULL,"
    "  PRIMARY KEY(batch_id, scope_kind, scope_id)"
    ") WITHOUT ROWID",
    // 归属解析的反向探取索引：按 (scope_kind, scope_id) 前缀取最大 batch_id
    // （两次索引前缀探 + 取大，无全表扫；单条 OR 形态实测慢 2.8 倍）。
    "CREATE INDEX batch_scopes_owner ON batch_scopes(scope_kind, scope_id, batch_id)",
    // 方法版本指纹层（v8，#132「怎么接进数据库」）：行 = 方法语义面的一次
    // 落库版本（编辑流在方法体变更后 record），method_version_id 单调只增
    // 不删；fingerprint 是 32 字节 SHA-256 定长 BLOB（CHECK 锁定长度），
    // fp_version 盖 kSemanticFpVersion 算法版本字节（算法升级后旧行自证
    // 版本位），recorded_at 与 renames/producers 同为 ISO-8601 UTC 文本。
    // class_id 与 member_id 都是身份层既有稳定 id 的外键（member_id 已含
    // 宿主类，class_id 是规范行形状的冗余编码，写入时校验归属一致）。
    "CREATE TABLE method_versions("
    "  method_version_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  class_id INTEGER NOT NULL REFERENCES classes(class_id) ON DELETE CASCADE,"
    "  member_id INTEGER NOT NULL REFERENCES members(member_id) ON DELETE CASCADE,"
    "  fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32),"
    "  fp_version INTEGER NOT NULL CHECK(fp_version>=1),"
    "  is_current INTEGER NOT NULL DEFAULT 1 CHECK(is_current IN (0,1)),"
    "  recorded_at TEXT NOT NULL"
    ")",
    // 当前指针（推荐定案，不动 members 表）：is_current 列 + (member_id)
    // 部分唯一索引——同成员至多一行当前版本，存储层锁死（写路径退位在前、
    // 落行在后同事务完成，违例即实现缺陷或裸 SQL 破坏）。成员身份与指纹
    // 版本是两条独立的轴，指针挤进 members 会把身份登记面改造成版本面，
    // 且没有版本行的方法要背空指针列。「标脏」不另建脏表：当前指针变更
    // 本身可查询（旧指纹对不上当前行即脏），退位行就是审计面。
    "CREATE UNIQUE INDEX method_versions_current "
    "  ON method_versions(member_id) WHERE is_current=1",
    // 版本历史索引：listMethodVersions 按 method_version_id 升序（单调分配
    // 序 = 记录序）走索引扫描，不排序、不裸扫。
    "CREATE INDEX method_versions_member ON method_versions(member_id, method_version_id)",
    // 全程序可达闭包（v9，#159）：派生事实 = 可达方法成员集，批次挂既有
    // producer 注册表（kind=closure_write）。closure_batches 挂批次主键记
    // 元数据：graph_fingerprint 是闭包输入图指纹（32 字节 SHA-256，载荷见
    // closureFingerprintPayload；变更敏感聚合口径，非内容寻址），fp_version
    // 盖闭包算法版本字节（仿 method_versions fp_version 模式，旧行自证版本
    // 位），seed_mask/coverage_mask 是种子档位与输入覆盖度位词表，
    // computed_at 与 renames/producers 同为 ISO-8601 UTC 文本。
    "CREATE TABLE closure_batches("
    "  batch_id          INTEGER PRIMARY KEY REFERENCES batches(batch_id),"
    "  graph_fingerprint BLOB NOT NULL CHECK(length(graph_fingerprint)=32),"
    "  fp_version        INTEGER NOT NULL CHECK(fp_version>=1),"
    "  seed_mask         INTEGER NOT NULL DEFAULT 0,"
    "  seed_count        INTEGER NOT NULL DEFAULT 0,"
    "  coverage_mask     INTEGER NOT NULL DEFAULT 0,"
    "  indy_edge_count   INTEGER NOT NULL DEFAULT 0,"
    "  member_count      INTEGER NOT NULL DEFAULT 0,"
    "  computed_at       TEXT NOT NULL"
    ")",
    // 可达成员集（闭包结果本体）：(batch_id, member_id) 主键即数据（WITHOUT
    // ROWID，行 16B 量级）；删批级联与成员身份级联同既有事实表口径。
    "CREATE TABLE closure_members("
    "  batch_id  INTEGER NOT NULL REFERENCES batches(batch_id) ON DELETE CASCADE,"
    "  member_id INTEGER NOT NULL REFERENCES members(member_id) ON DELETE CASCADE,"
    "  PRIMARY KEY(batch_id, member_id)"
    ") WITHOUT ROWID",
    // member 反查索引：服务「哪些闭包含此成员」的反向查询；前向分页走主键。
    "CREATE INDEX closure_members_member ON closure_members(member_id)",
};

/// @brief 全部后代闭包 SQL（allSubclasses / allSubclassesPaged 共用）；
///        分页变体在末尾追加 LIMIT/OFFSET 窗口。
constexpr const char* kAllSubclassesSql =
    "WITH RECURSIVE subs(name) AS ("
    "  SELECT internal_name FROM classes WHERE super_name=?1"
    "  UNION"
    "  SELECT c.internal_name FROM classes c JOIN subs s ON c.super_name=s.name"
    ") SELECT name FROM subs ORDER BY name";

/// @brief 接口实现类闭包 SQL（implementors / implementorsPaged 共用）；
///        分页变体在末尾追加 LIMIT/OFFSET 窗口。
constexpr const char* kImplementorsSql =
    "WITH RECURSIVE ifaces(name) AS ("
    "  SELECT ?1"
    "  UNION"
    "  SELECT i.interface_name FROM interfaces i "
    "  JOIN classes c ON c.class_id=i.class_id "
    "  JOIN ifaces f ON c.internal_name=f.name"
    "), impls(name) AS ("
    "  SELECT c.internal_name FROM classes c "
    "  JOIN interfaces i ON i.class_id=c.class_id "
    "  JOIN ifaces f ON i.interface_name=f.name"
    "  UNION"
    "  SELECT c.internal_name FROM classes c JOIN impls p ON c.super_name=p.name"
    ") SELECT name FROM impls ORDER BY name";

//-- 方法版本指纹层（method_versions，v8）的共享助手 ---------------------------

/// @brief 32 字节哈希 → 64 位小写 hex 文本（与 analysis 层 hexOf 同构）。
///        存储面只存 BLOB 字节，hex 是读出时的派生文本形态，不单独存储。
ca::str::Utf8String fpHexOf(const ca::u8* bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    char                  text[analysis::kSemanticFpHashBytes * 2];
    for (ca::usize i = 0; i < analysis::kSemanticFpHashBytes; ++i) {
        text[2 * i]     = kDigits[bytes[i] >> 4];
        text[2 * i + 1] = kDigits[bytes[i] & 0x0F];
    }
    return ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(text),
                                                    sizeof(text));
}

/// @brief 从 BLOB 列读指纹值：字节逐位拷入，hex 派生重建。
analysis::SemanticFp fingerprintFromColumn(sqlite3_stmt* statement, int index)
{
    analysis::SemanticFp fp;
    const auto*          bytes =
        static_cast<const ca::u8*>(sqlite3_column_blob(statement, index));
    if (bytes != nullptr) {
        for (ca::usize i = 0; i < analysis::kSemanticFpHashBytes; ++i) {
            fp.bytes[i] = bytes[i];
        }
    }
    fp.hex = fpHexOf(fp.bytes.data());
    return fp;
}

/// @brief 方法成员存在性探取（方法版本读写口共用）：已登记方法返回 Ok(true)。
///        未登记成员或字段返回 Ok(false)——字段无指纹，调用方按口径报
///        `Invalid`。SELECT 1 走 member_id 主键，常数代价。
ca::Result<bool, AnalysisDbError> methodMemberExists(sqlite3* db, ca::i64 memberId)
{
    auto prepared = prepare(db, "SELECT 1 FROM members WHERE member_id=?1 AND kind=1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(db, statement.raw, 1, memberId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    return ca::Ok(hasRow.unwrap());
}

/// @brief 单个方法版本落库的共享实现（调用方持事务）。成员身份校验 →
///        当前指针同指纹幂等跳过 / 异指纹退位 + 落新行。
struct RecordedMethodVersion
{
    ca::i64 currentId = 0; ///< 落库后的当前版本行 id
    bool    inserted  = false;
};

ca::Result<RecordedMethodVersion, AnalysisDbError> recordOneMethodVersion(
    sqlite3* db, ca::i64 classId, ca::i64 memberId, const analysis::SemanticFp& fingerprint)
{
    // 成员身份先落定：必须是已登记方法（指纹只对方法存在），且归属类与
    // 入参 classId 一致——防止跨类错位写入（FK 只保证成员存在，不保证
    // 调用方拿对宿主类）。
    auto member = prepare(db, "SELECT class_id FROM members WHERE member_id=?1 AND kind=1");
    if (member.is_err()) return errorOf(std::move(member));
    auto memberStatement = std::move(member).unwrap();
    if (auto result = bindInt(db, memberStatement.raw, 1, memberId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasMember = step(db, memberStatement.raw);
    if (hasMember.is_err()) return errorOf(std::move(hasMember));
    if (!hasMember.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "method version target is not a registered method: " +
                                     std::to_string(memberId)));
    }
    const ca::i64 ownerClassId = sqlite3_column_int64(memberStatement.raw, 0);
    resetStatement(memberStatement);
    if (ownerClassId != classId) {
        return ca::Err(makeError(
            AnalysisDbErrorKind::Invalid,
            "member " + std::to_string(memberId) + " does not belong to class " +
                std::to_string(classId) + " (registered under class " +
                std::to_string(ownerClassId) + ")"));
    }

    // 当前指针：同指纹幂等跳过（语义没变不是新版本），异指纹退位 + 落新行。
    auto current = prepare(db,
                           "SELECT method_version_id, fingerprint FROM method_versions "
                           "WHERE member_id=?1 AND is_current=1");
    if (current.is_err()) return errorOf(std::move(current));
    auto currentStatement = std::move(current).unwrap();
    if (auto result = bindInt(db, currentStatement.raw, 1, memberId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasCurrent = step(db, currentStatement.raw);
    if (hasCurrent.is_err()) return errorOf(std::move(hasCurrent));
    const bool currentRow = hasCurrent.unwrap();
    ca::i64    currentId  = 0;
    bool       sameBytes  = false;
    if (currentRow) {
        currentId = sqlite3_column_int64(currentStatement.raw, 0);
        const auto* bytes =
            static_cast<const ca::u8*>(sqlite3_column_blob(currentStatement.raw, 1));
        sameBytes = bytes != nullptr &&
                    std::memcmp(bytes, fingerprint.bytes.data(),
                                analysis::kSemanticFpHashBytes) == 0;
        resetStatement(currentStatement);
    }

    if (currentRow && sameBytes) {
        return ca::Ok(RecordedMethodVersion{currentId, false});
    }

    if (currentRow) {
        auto retire = prepare(
            db, "UPDATE method_versions SET is_current=0 WHERE method_version_id=?1");
        if (retire.is_err()) return errorOf(std::move(retire));
        auto retireStatement = std::move(retire).unwrap();
        if (auto result = bindInt(db, retireStatement.raw, 1, currentId); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto retired = step(db, retireStatement.raw);
        if (retired.is_err()) return errorOf(std::move(retired));
    }

    auto insert = prepare(db,
                          "INSERT INTO method_versions"
                          "(class_id, member_id, fingerprint, fp_version, is_current, "
                          " recorded_at) VALUES(?1, ?2, ?3, ?4, 1, ?5)");
    if (insert.is_err()) return errorOf(std::move(insert));
    auto insertStatement = std::move(insert).unwrap();
    if (auto result = bindInt(db, insertStatement.raw, 1, classId); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, insertStatement.raw, 2, memberId); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindBlob(db, insertStatement.raw, 3,
                               fingerprint.bytes.data(),
                               static_cast<int>(analysis::kSemanticFpHashBytes));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, insertStatement.raw, 4,
                              static_cast<ca::i64>(analysis::kSemanticFpVersion));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, insertStatement.raw, 5,
                               ca::str::Utf8StringRef::from_string_view(nowIso8601()));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    auto inserted = step(db, insertStatement.raw);
    if (inserted.is_err()) return errorOf(std::move(inserted));

    return ca::Ok(RecordedMethodVersion{sqlite3_last_insert_rowid(db), true});
}

/// @brief 方法版本行读取（listMethodVersions；method_version_id 升序游标）。
DbMethodVersionInfo readMethodVersionRow(sqlite3_stmt* statement)
{
    DbMethodVersionInfo info;
    info.methodVersionId = sqlite3_column_int64(statement, 0);
    info.classId         = sqlite3_column_int64(statement, 1);
    info.memberId        = sqlite3_column_int64(statement, 2);
    info.fingerprint     = fingerprintFromColumn(statement, 3);
    info.fpVersion       = static_cast<ca::u8>(sqlite3_column_int64(statement, 4));
    info.isCurrent       = sqlite3_column_int64(statement, 5) != 0;
    info.recordedAt      = utf8Column(statement, 6);
    return info;
}

}   // namespace

struct AnalysisDb::Impl
{
    sqlite3*           handle        = nullptr;
    AnalysisDbOpenMode mode          = AnalysisDbOpenMode::ReadWrite;
    ca::u32            schemaVersion = 0;

    ~Impl()
    {
        if (handle != nullptr) sqlite3_close_v2(handle);
    }
};

AnalysisDb::AnalysisDb(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

AnalysisDb::AnalysisDb(AnalysisDb&&) noexcept            = default;
AnalysisDb& AnalysisDb::operator=(AnalysisDb&&) noexcept = default;
AnalysisDb::~AnalysisDb()                                = default;

ca::Result<AnalysisDb, AnalysisDbError> AnalysisDb::open(const ca::str::Utf8StringRef& path,
                                                         AnalysisDbOpenMode            mode)
{
    if (path.is_empty()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database path must not be empty"));
    }
    if (hasNul(path)) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "database path must not contain NUL"));
    }

    const std::string filePath = path.to_std_string();
    int               flags    = SQLITE_OPEN_FULLMUTEX;
    if (mode == AnalysisDbOpenMode::ReadOnly) {
        flags |= SQLITE_OPEN_READONLY;
    }
    else {
        flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    }

    sqlite3*  handle = nullptr;
    const int rc     = sqlite3_open_v2(filePath.c_str(), &handle, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string detail =
            handle != nullptr ? sqlite3_errmsg(handle) : std::string("out of memory");
        if (handle != nullptr) sqlite3_close_v2(handle);
        return ca::Err(makeError(kindOf(rc), "open '" + filePath + "' failed: " + detail));
    }

    auto impl       = std::make_unique<Impl>();
    impl->handle    = handle;
    impl->mode      = mode;

    if (sqlite3_busy_timeout(handle, kBusyTimeoutMs) != SQLITE_OK) {
        return ca::Err(sqliteFailure(handle, "set busy timeout failed"));
    }
    if (auto result = exec(handle, "PRAGMA foreign_keys=ON"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (mode == AnalysisDbOpenMode::ReadWrite) {
        if (auto result = exec(handle, "PRAGMA journal_mode=WAL"); result.is_err()) {
            return errorOf(std::move(result));
        }
        // NORMAL（WAL）：应用崩溃不丢已提交事务，断电最多丢最近 checkpoint
        // 之后的提交——分析工作库的正确权衡；FULL 的每次提交 fsync 在
        // 大批量导入场景无观测收益。
        if (auto result = exec(handle, "PRAGMA synchronous=NORMAL"); result.is_err()) {
            return errorOf(std::move(result));
        }
        // 默认 2MB page cache 在整库导入（数百万行引用、二级索引随机落页）
        // 下持续颠簸：B6 基线 ref_ingest 占整库导入 78-84%。给到 256MB 后
        // 索引维护基本全内存。temp_store 同理服务排序与临时 B-tree。
        if (auto result = exec(handle, "PRAGMA cache_size=-262144"); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = exec(handle, "PRAGMA temp_store=MEMORY"); result.is_err()) {
            return errorOf(std::move(result));
        }
    }

    auto applicationId = queryScalarInt(handle, "PRAGMA application_id");
    if (applicationId.is_err()) return errorOf(std::move(applicationId));
    auto userVersion = queryScalarInt(handle, "PRAGMA user_version");
    if (userVersion.is_err()) return errorOf(std::move(userVersion));

    const ca::i64 appId   = applicationId.unwrap();
    const ca::i64 version = userVersion.unwrap();

    if (appId == 0 && version == 0) {
        auto tableCount = queryScalarInt(
            handle,
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE type='table' AND name NOT LIKE 'sqlite_%'");
        if (tableCount.is_err()) return errorOf(std::move(tableCount));

        if (tableCount.unwrap() > 0) {
            return ca::Err(makeError(
                AnalysisDbErrorKind::Schema,
                "database has user tables but no mjbase analysis database marker"));
        }
        if (mode == AnalysisDbOpenMode::ReadOnly) {
            return ca::Err(makeError(AnalysisDbErrorKind::Open,
                                     "database is not initialized: " + filePath));
        }

        auto transaction = Transaction::begin(handle);
        if (transaction.is_err()) return errorOf(std::move(transaction));
        auto txn = std::move(transaction).unwrap();

        for (const char* ddl : kSchemaDdl) {
            if (auto result = exec(handle, ddl); result.is_err()) {
                return errorOf(std::move(result));
            }
        }
        if (auto result = exec(handle,
                               "INSERT INTO meta(key, value) VALUES('kind', 'mjbase-analysis-db')");
            result.is_err()) {
            return errorOf(std::move(result));
        }
        const std::string applicationIdSql =
            "PRAGMA application_id = " + std::to_string(kAnalysisDbApplicationId);
        if (auto result = exec(handle, applicationIdSql.c_str()); result.is_err()) {
            return errorOf(std::move(result));
        }
        const std::string userVersionSql =
            "PRAGMA user_version = " + std::to_string(kAnalysisDbSchemaVersion);
        if (auto result = exec(handle, userVersionSql.c_str()); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = txn.commit(); result.is_err()) {
            return errorOf(std::move(result));
        }
        impl->schemaVersion = kAnalysisDbSchemaVersion;
        return ca::Ok(AnalysisDb(std::move(impl)));
    }

    if (appId != static_cast<ca::i64>(kAnalysisDbApplicationId)) {
        return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                 "not an mjbase analysis database (application_id=" +
                                     std::to_string(appId) + ")"));
    }
    if (version != static_cast<ca::i64>(kAnalysisDbSchemaVersion)) {
        return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                 "unsupported analysis database schema version " +
                                     std::to_string(version) + " (supported " +
                                     std::to_string(kAnalysisDbSchemaVersion) + ")"));
    }

    auto kindStatement = prepare(handle, "SELECT value FROM meta WHERE key='kind'");
    if (kindStatement.is_err()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                 "analysis database meta table is missing"));
    }
    auto statement = std::move(kindStatement).unwrap();
    auto hasRow    = step(handle, statement.raw);
    if (hasRow.is_err()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                 "analysis database meta table is unreadable"));
    }
    if (!hasRow.unwrap() || textColumn(statement.raw, 0) != kAnalysisDbKind) {
        return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                 "analysis database kind marker is missing or invalid"));
    }

    impl->schemaVersion = kAnalysisDbSchemaVersion;
    return ca::Ok(AnalysisDb(std::move(impl)));
}

bool AnalysisDb::isOpen() const noexcept
{
    return impl_ != nullptr && impl_->handle != nullptr;
}

bool AnalysisDb::isReadOnly() const noexcept
{
    return impl_ != nullptr && impl_->mode == AnalysisDbOpenMode::ReadOnly;
}

ca::u32 AnalysisDb::schemaVersion() const noexcept
{
    return impl_ != nullptr ? impl_->schemaVersion : 0;
}

ca::Result<void, AnalysisDbError> AnalysisDb::save()
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) return ca::Ok();

    int       logFrames     = 0;
    int       checkpointed  = 0;
    const int rc = sqlite3_wal_checkpoint_v2(impl_->handle, nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                             &logFrames, &checkpointed);
    if (rc != SQLITE_OK) {
        return ca::Err(makeError(
            kindOf(rc), "WAL checkpoint failed: " + std::string(sqlite3_errmsg(impl_->handle)) +
                            " (log=" + std::to_string(logFrames) +
                            ", checkpointed=" + std::to_string(checkpointed) + ")"));
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> AnalysisDb::close()
{
    if (!isOpen()) return ca::Ok();

    ca::Result<void, AnalysisDbError> checkpoint = ca::Ok();
    if (!isReadOnly()) checkpoint = save();

    sqlite3*  handle = impl_->handle;
    const int rc     = sqlite3_close_v2(handle);
    if (rc != SQLITE_OK) {
        return ca::Err(sqliteFailure(handle, "close failed"));
    }
    impl_->handle        = nullptr;
    impl_->schemaVersion = 0;

    if (checkpoint.is_err()) return checkpoint;
    return ca::Ok();
}

namespace {

/// @brief 行级归属写入口共用的生产者门（renames 覆盖层与 mask 配置面）：
///        0 = 匿名写入放行；非 0 必须已注册（FK 也拦，但先查给出可诊断
///        错误）；负值是调用方契约错误。
ca::Result<void, AnalysisDbError> validateRegisteredProducer(sqlite3* db, ca::i64 producerId)
{
    if (producerId < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "producer id must not be negative"));
    }
    if (producerId == 0) return ca::Ok();

    auto selected = prepare(db, "SELECT 1 FROM producers WHERE producer_id=?1");
    if (selected.is_err()) return errorOf(std::move(selected));
    auto statement = std::move(selected).unwrap();
    if (auto result = bindInt(db, statement.raw, 1, producerId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "producer is not registered: " + std::to_string(producerId)));
    }
    return ca::Ok();
}

}   // namespace

ca::Result<void, AnalysisDbError> AnalysisDb::setClassRename(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& shadow,
    const ca::str::Utf8StringRef& token, ca::i64 producerId)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateClassFields(owner, shadow, token); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    if (auto result = validateRegisteredProducer(impl_->handle, producerId); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto classId = ensureClass(impl_->handle, owner);
    if (classId.is_err()) return errorOf(std::move(classId));

    const std::string updatedAt = nowIso8601();
    if (auto result =
            upsertRename(impl_->handle, classId.unwrap(), RenameTargetKind::Class, shadow, token,
                         updatedAt, producerId);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    return txn.commit();
}

ca::Result<void, AnalysisDbError> AnalysisDb::setMemberRename(
    DbMemberKind kind, const ca::str::Utf8StringRef& owner,
    const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc,
    const ca::str::Utf8StringRef& shadow, const ca::str::Utf8StringRef& token,
    ca::i64 producerId)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateMemberFields(kind, owner, name, desc, shadow, token);
        result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    if (auto result = validateRegisteredProducer(impl_->handle, producerId); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto classId = ensureClass(impl_->handle, owner);
    if (classId.is_err()) return errorOf(std::move(classId));
    auto memberId = ensureMember(impl_->handle, classId.unwrap(), kind, name, desc);
    if (memberId.is_err()) return errorOf(std::move(memberId));

    const std::string updatedAt = nowIso8601();
    if (auto result = upsertRename(impl_->handle, memberId.unwrap(), RenameTargetKind::Member,
                                   shadow, token, updatedAt, producerId);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    return txn.commit();
}

ca::Result<void, AnalysisDbError> AnalysisDb::setRenames(std::vector<DbRenameRecord> records,
                                                         ca::i64                     producerId)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateRenameSet(records); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    if (auto result = validateRegisteredProducer(impl_->handle, producerId); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = applyRenameBatch(impl_->handle, records, producerId); result.is_err()) {
        return errorOf(std::move(result));
    }
    return txn.commit();
}

namespace {

ca::Result<std::optional<ca::i64>, AnalysisDbError> lookupClassId(
    sqlite3* db, const ca::str::Utf8StringRef& owner)
{
    auto prepared = prepare(db, "SELECT class_id FROM classes WHERE internal_name=?1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(db, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(std::optional<ca::i64>());
    return ca::Ok(std::optional<ca::i64>(sqlite3_column_int64(statement.raw, 0)));
}

ca::Result<std::optional<ca::i64>, AnalysisDbError> lookupMemberId(
    sqlite3* db, DbMemberKind kind, const ca::str::Utf8StringRef& owner,
    const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc)
{
    auto prepared = prepare(
        db,
        "SELECT m.member_id FROM members m JOIN classes c ON c.class_id=m.class_id "
        "WHERE c.internal_name=?1 AND m.kind=?2 AND m.name=?3 AND m.descriptor=?4");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(db, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement.raw, 2, static_cast<ca::i64>(kind)); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, statement.raw, 3, name); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, statement.raw, 4, desc); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(std::optional<ca::i64>());
    return ca::Ok(std::optional<ca::i64>(sqlite3_column_int64(statement.raw, 0)));
}

}   // namespace

ca::Result<bool, AnalysisDbError> AnalysisDb::clearClassRename(
    const ca::str::Utf8StringRef& owner)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateRequiredText(owner, "owner"); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto classId = lookupClassId(impl_->handle, owner);
    if (classId.is_err()) return errorOf(std::move(classId));
    if (!classId.unwrap().has_value()) return ca::Ok(false);

    if (auto result = deleteRename(impl_->handle, *classId.unwrap(), RenameTargetKind::Class);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(sqlite3_changes(impl_->handle) > 0);
}

ca::Result<bool, AnalysisDbError> AnalysisDb::clearMemberRename(
    DbMemberKind kind, const ca::str::Utf8StringRef& owner,
    const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateMemberKind(kind); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(owner, "owner"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(name, "member name"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(desc, "member descriptor"); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto memberId = lookupMemberId(impl_->handle, kind, owner, name, desc);
    if (memberId.is_err()) return errorOf(std::move(memberId));
    if (!memberId.unwrap().has_value()) return ca::Ok(false);

    if (auto result = deleteRename(impl_->handle, *memberId.unwrap(), RenameTargetKind::Member);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(sqlite3_changes(impl_->handle) > 0);
}

ca::Result<std::optional<DbRenameRecord>, AnalysisDbError> AnalysisDb::findClassRename(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validateRequiredText(owner, "owner"); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto prepared = prepare(
        impl_->handle,
        "SELECT r.new_name, r.token, r.updated_at, c.internal_name, r.producer_id "
        "FROM renames r JOIN classes c ON r.target_kind=0 AND c.class_id=r.target_id "
        "WHERE c.internal_name=?1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto hasRow = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(std::optional<DbRenameRecord>());

    DbRenameRecord record;
    record.target     = RenameTargetKind::Class;
    record.shadow     = utf8Column(statement.raw, 0);
    record.token      = utf8Column(statement.raw, 1);
    record.updatedAt  = utf8Column(statement.raw, 2);
    record.owner      = utf8Column(statement.raw, 3);
    record.producerId = sqlite3_column_int64(statement.raw, 4);
    return ca::Ok(std::optional<DbRenameRecord>(std::move(record)));
}

ca::Result<std::optional<DbRenameRecord>, AnalysisDbError> AnalysisDb::findMemberRename(
    DbMemberKind kind, const ca::str::Utf8StringRef& owner,
    const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validateMemberKind(kind); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(owner, "owner"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(name, "member name"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(desc, "member descriptor"); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto prepared = prepare(
        impl_->handle,
        "SELECT r.new_name, r.token, r.updated_at, c.internal_name, m.kind, m.name, "
        "m.descriptor, r.producer_id "
        "FROM renames r "
        "JOIN members m ON r.target_kind=1 AND m.member_id=r.target_id "
        "JOIN classes c ON c.class_id=m.class_id "
        "WHERE c.internal_name=?1 AND m.kind=?2 AND m.name=?3 AND m.descriptor=?4");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(impl_->handle, statement.raw, 2, static_cast<ca::i64>(kind));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(impl_->handle, statement.raw, 3, name); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(impl_->handle, statement.raw, 4, desc); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto hasRow = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(std::optional<DbRenameRecord>());

    DbRenameRecord record;
    record.target     = RenameTargetKind::Member;
    record.memberKind = static_cast<DbMemberKind>(sqlite3_column_int(statement.raw, 4));
    record.shadow     = utf8Column(statement.raw, 0);
    record.token      = utf8Column(statement.raw, 1);
    record.updatedAt  = utf8Column(statement.raw, 2);
    record.owner      = utf8Column(statement.raw, 3);
    record.name       = utf8Column(statement.raw, 5);
    record.desc       = utf8Column(statement.raw, 6);
    record.producerId = sqlite3_column_int64(statement.raw, 7);
    return ca::Ok(std::optional<DbRenameRecord>(std::move(record)));
}

ca::Result<std::vector<DbRenameRecord>, AnalysisDbError> AnalysisDb::listRenames() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    const std::string sql = std::string(kRenameSelectSql) + kRenameOrderSql;
    auto prepared = prepare(impl_->handle, sql.c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    std::vector<DbRenameRecord> records;
    while (true) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;

        auto row = readRenameRow(statement.raw);
        if (row.is_err()) return errorOf(std::move(row));
        records.push_back(std::move(row).unwrap());
    }
    return ca::Ok(std::move(records));
}

ca::Result<std::vector<DbRenameRecord>, AnalysisDbError> AnalysisDb::findRenamesByShadows(
    const std::vector<ca::str::Utf8StringRef>& shadows) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (shadows.empty()) return ca::Ok(std::vector<DbRenameRecord>());

    const std::string sql =
        std::string(kRenameSelectSql) + "WHERE r.new_name=?1 " + kRenameOrderSql;
    auto prepared = prepare(impl_->handle, sql.c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    std::vector<DbRenameRecord> records;
    for (const auto& shadow : shadows) {
        if (auto result = bindText(impl_->handle, statement.raw, 1, shadow); result.is_err()) {
            return errorOf(std::move(result));
        }
        while (true) {
            auto hasRow = step(impl_->handle, statement.raw);
            if (hasRow.is_err()) return errorOf(std::move(hasRow));
            if (!hasRow.unwrap()) break;

            auto row = readRenameRow(statement.raw);
            if (row.is_err()) return errorOf(std::move(row));
            records.push_back(std::move(row).unwrap());
        }
        resetStatement(statement);
    }
    return ca::Ok(std::move(records));
}

ca::Result<std::vector<DbRenameRecord>, AnalysisDbError> AnalysisDb::findRenamesByShadow(
    const ca::str::Utf8StringRef& shadow) const
{
    return findRenamesByShadows(std::vector<ca::str::Utf8StringRef>{shadow});
}

ca::Result<void, AnalysisDbError> AnalysisDb::replaceRenames(
    std::vector<DbRenameRecord> records, ca::i64 producerId)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }

    if (auto result = validateRenameSet(records); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    if (auto result = validateRegisteredProducer(impl_->handle, producerId); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = exec(impl_->handle, "DELETE FROM renames"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = applyRenameBatch(impl_->handle, records, producerId); result.is_err()) {
        return errorOf(std::move(result));
    }
    return txn.commit();
}

namespace {

///@brief 身份迁移规格的校验 + 批内重复目标门（同批两规格迁同一身份 → 结果
///依处理序而变，fail-loud 拒绝；空批放行 = 零操作）。
ca::Result<void, AnalysisDbError> validateIdentitySet(const std::vector<DbIdentityRename>& renames)
{
    std::set<std::string> keys;
    for (const auto& spec : renames) {
        if (spec.target == RenameTargetKind::Class) {
            if (auto result = validateRequiredText(spec.name.ref(), "class internal name");
                result.is_err()) {
                return result;
            }
            if (auto result = validateRequiredText(spec.newName.ref(), "new class internal name");
                result.is_err()) {
                return result;
            }
        }
        else {
            if (auto result = validateMemberKind(spec.memberKind); result.is_err()) return result;
            if (auto result = validateRequiredText(spec.owner.ref(), "owner"); result.is_err()) {
                return result;
            }
            if (auto result = validateRequiredText(spec.name.ref(), "member name"); result.is_err()) {
                return result;
            }
            if (auto result = validateRequiredText(spec.desc.ref(), "member descriptor");
                result.is_err()) {
                return result;
            }
            if (auto result = validateRequiredText(spec.newName.ref(), "new member name");
                result.is_err()) {
                return result;
            }
        }
        std::string key = spec.target == RenameTargetKind::Class
                              ? "c:" + spec.name.to_std_string()
                              : "m:" + std::to_string(static_cast<ca::i64>(spec.memberKind)) +
                                    ":" + spec.owner.to_std_string() + ":" +
                                    spec.name.to_std_string() + ":" + spec.desc.to_std_string();
        if (!keys.insert(std::move(key)).second) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "duplicate identity migration target in batch"));
        }
    }
    return ca::Ok();
}

///@brief 单条 UPDATE 执行体（prepare/bind/step 样板收拢；迁移路径专用）。
///形参一：SET 位文本；形参二：WHERE 位——id 版按 class_id 定位，text 版按
///旧文本定位（super_name/interfaces 的非规范化副本改写）。
ca::Result<void, AnalysisDbError> execUpdateTextId(sqlite3* db, const char* sql,
                                                   const ca::str::Utf8StringRef& textValue,
                                                   ca::i64 intValue)
{
    auto prepared = prepare(db, sql);
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(db, statement.raw, 1, textValue); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement.raw, 2, intValue); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto done = step(db, statement.raw);
    if (done.is_err()) return errorOf(std::move(done));
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> execUpdateTextText(sqlite3* db, const char* sql,
                                                     const ca::str::Utf8StringRef& newText,
                                                     const ca::str::Utf8StringRef& oldText)
{
    auto prepared = prepare(db, sql);
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(db, statement.raw, 1, newText); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(db, statement.raw, 2, oldText); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto done = step(db, statement.raw);
    if (done.is_err()) return errorOf(std::move(done));
    return ca::Ok();
}

}   // namespace

ca::Result<DbMigrationStats, AnalysisDbError> AnalysisDb::migrateIdentities(
    std::vector<DbIdentityRename> renames)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateIdentitySet(renames); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    DbMigrationStats stats;
    for (const auto& spec : renames) {
        if (spec.target == RenameTargetKind::Class) {
            auto oldId = lookupClassId(impl_->handle, spec.name.ref());
            if (oldId.is_err()) return errorOf(std::move(oldId));
            if (!oldId.unwrap().has_value()) {
                ++stats.skipped;
                continue;
            }
            //撞名预检在事务内（BEGIN IMMEDIATE 持写锁），与执行原子；目标
            //名已登记且非本行 → Invalid 整批回滚。
            auto occupied = lookupClassId(impl_->handle, spec.newName.ref());
            if (occupied.is_err()) return errorOf(std::move(occupied));
            if (occupied.unwrap().has_value() && *occupied.unwrap() != *oldId.unwrap()) {
                return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                         "target class name already registered: " +
                                             spec.newName.to_std_string()));
            }
            if (auto result = execUpdateTextId(
                    impl_->handle,
                    "UPDATE classes SET internal_name=?1 WHERE class_id=?2",
                    spec.newName.ref(), *oldId.unwrap());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            //名字属性的非规范化文本副本连带（super_name / interfaces）。
            if (auto result = execUpdateTextText(
                    impl_->handle,
                    "UPDATE classes SET super_name=?1 WHERE super_name=?2",
                    spec.newName.ref(), spec.name.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = execUpdateTextText(
                    impl_->handle,
                    "UPDATE interfaces SET interface_name=?1 WHERE interface_name=?2",
                    spec.newName.ref(), spec.name.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            ++stats.migratedClasses;
        }
        else {
            auto classId = lookupClassId(impl_->handle, spec.owner.ref());
            if (classId.is_err()) return errorOf(std::move(classId));
            if (!classId.unwrap().has_value()) {
                ++stats.skipped;
                continue;
            }
            auto memberId = lookupMemberId(impl_->handle, spec.memberKind, spec.owner.ref(),
                                           spec.name.ref(), spec.desc.ref());
            if (memberId.is_err()) return errorOf(std::move(memberId));
            if (!memberId.unwrap().has_value()) {
                ++stats.skipped;
                continue;
            }
            const auto newDesc =
                spec.newDesc.is_empty() ? spec.desc.ref() : spec.newDesc.ref();
            auto occupied = lookupMemberId(impl_->handle, spec.memberKind, spec.owner.ref(),
                                           spec.newName.ref(), newDesc);
            if (occupied.is_err()) return errorOf(std::move(occupied));
            if (occupied.unwrap().has_value() && *occupied.unwrap() != *memberId.unwrap()) {
                return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                         "target member identity already registered: " +
                                             spec.owner.to_std_string() + "." +
                                             spec.newName.to_std_string()));
            }
            {
                auto prepared = prepare(
                    impl_->handle,
                    "UPDATE members SET name=?1, descriptor=?2 WHERE member_id=?3");
                if (prepared.is_err()) return errorOf(std::move(prepared));
                auto statement = std::move(prepared).unwrap();
                if (auto result = bindText(impl_->handle, statement.raw, 1, spec.newName.ref());
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                if (auto result = bindText(impl_->handle, statement.raw, 2, newDesc);
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                if (auto result =
                        bindInt(impl_->handle, statement.raw, 3, *memberId.unwrap());
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                auto done = step(impl_->handle, statement.raw);
                if (done.is_err()) return errorOf(std::move(done));
            }
            ++stats.migratedMembers;
        }
    }
    if (auto result = txn.commit(); result.is_err()) return errorOf(std::move(result));
    return ca::Ok(stats);
}

namespace {

/// @brief 写入口的批次挂靠门（#143 批 3 fail-loud 拒绝面）：batchId 必须
///        已注册且未收尾；调用方保证 batchId != 0（0 = 无批写入，不调本门）。
///        在调用方事务内执行：BEGIN IMMEDIATE 持写锁后校验与写入原子。
ca::Result<void, AnalysisDbError> validateOpenBatchForWrite(sqlite3* db, ca::i64 batchId)
{
    auto selected = prepare(db, "SELECT finished_at FROM batches WHERE batch_id=?1");
    if (selected.is_err()) return errorOf(std::move(selected));
    auto statement = std::move(selected).unwrap();
    if (auto result = bindInt(db, statement.raw, 1, batchId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "batch is not registered: " + std::to_string(batchId)));
    }
    if (!textColumn(statement.raw, 0).empty()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "batch is already closed: " + std::to_string(batchId)));
    }
    return ca::Ok();
}

/// @brief 单条作用域锚写入（INSERT OR IGNORE：作用域是集合语义，同批重复
///        覆盖同一区间幂等，不是日志）。调用方负责事务边界。
ca::Result<void, AnalysisDbError> writeScopeRow(sqlite3*        db,
                                                sqlite3_stmt*   statement,
                                                ca::i64         batchId,
                                                DbBatchScopeKind scopeKind,
                                                ca::i64         scopeId)
{
    // 语句批次内复用：上一轮 step 至 DONE 后必须 reset 才能重新绑定。
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    if (auto result = bindInt(db, statement, 1, batchId); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement, 2, static_cast<ca::i64>(scopeKind));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement, 3, scopeId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto done = step(db, statement);
    if (done.is_err()) return errorOf(std::move(done));
    return ca::Ok();
}

constexpr const char* kScopeRowInsertSql =
    "INSERT OR IGNORE INTO batch_scopes(batch_id, scope_kind, scope_id) VALUES(?1, ?2, ?3)";

/// @brief 归属解析的单前缀探取（#143 3.2）：(scope_kind, scope_id) 前缀下
///        batch_id 最大的批次 + 生产者身份。走 batch_scopes_owner 覆盖索引
///        前缀下探，ORDER BY batch_id DESC LIMIT 1 无排序；单条 OR 形态实测
///        慢 2.8 倍，勿合并。
constexpr const char* kScopeOwnershipProbeSql =
    "SELECT s.batch_id, b.producer_id, p.key, b.kind "
    "FROM batch_scopes s "
    "JOIN batches b ON b.batch_id = s.batch_id "
    "JOIN producers p ON p.producer_id = b.producer_id "
    "WHERE s.scope_kind = ?1 AND s.scope_id = ?2 "
    "ORDER BY s.batch_id DESC LIMIT 1";

ca::Result<std::optional<DbOwnershipInfo>, AnalysisDbError> probeScopeOwnership(
    sqlite3* db, DbBatchScopeKind scopeKind, ca::i64 scopeId)
{
    auto prepared = prepare(db, kScopeOwnershipProbeSql);
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(db, statement.raw, 1, static_cast<ca::i64>(scopeKind));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement.raw, 2, scopeId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(std::optional<DbOwnershipInfo>());

    auto kind = batchKindFromSql(textColumn(statement.raw, 3));
    if (kind.is_err()) return errorOf(std::move(kind));
    DbOwnershipInfo info;
    info.batchId    = sqlite3_column_int64(statement.raw, 0);
    info.producerId = sqlite3_column_int64(statement.raw, 1);
    info.producerKey = utf8Column(statement.raw, 2);
    info.kind       = kind.unwrap();
    return ca::Ok(std::optional<DbOwnershipInfo>(std::move(info)));
}

}   // namespace
//-- 条目身份层（inputs / entries，schema v3） ------------------------------

ca::Result<ca::i64, AnalysisDbError> AnalysisDb::ensureInput(
    const ca::str::Utf8StringRef& path, const ca::str::Utf8StringRef& sha256, ca::i64 size)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateRequiredText(path, "input path"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateSha256Hex(sha256, "input sha256"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (size < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "input size must not be negative"));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    {
        // path 是身份键：重登记刷新内容锚，input_id 保持不变。
        auto upserted = prepare(
            impl_->handle,
            "INSERT INTO inputs(path, size, sha256) VALUES(?1, ?2, ?3) "
            "ON CONFLICT(path) DO UPDATE SET size=excluded.size, sha256=excluded.sha256");
        if (upserted.is_err()) return errorOf(std::move(upserted));
        auto statement = std::move(upserted).unwrap();
        if (auto result = bindText(impl_->handle, statement.raw, 1, path); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 2, size); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(impl_->handle, statement.raw, 3, sha256); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto done = step(impl_->handle, statement.raw);
        if (done.is_err()) return errorOf(std::move(done));
    }

    ca::i64 inputId = 0;
    {
        auto selected = prepare(impl_->handle, "SELECT input_id FROM inputs WHERE path=?1");
        if (selected.is_err()) return errorOf(std::move(selected));
        auto statement = std::move(selected).unwrap();
        if (auto result = bindText(impl_->handle, statement.raw, 1, path); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Sqlite,
                                     "input identity vanished after registration"));
        }
        inputId = sqlite3_column_int64(statement.raw, 0);
    }

    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(inputId);
}

ca::Result<DbEntryIngestStats, AnalysisDbError> AnalysisDb::ingestEntries(
    ca::i64 batchId, ca::i64 inputId, std::vector<DbEntryInput> entries)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (batchId < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "batch id must not be negative"));
    }
    if (inputId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "input id must be positive"));
    }

    std::set<std::string> paths;
    for (const DbEntryInput& entry : entries) {
        if (auto result = validateRequiredText(entry.path.ref(), "entry path"); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = validateRequiredText(entry.kind.ref(), "entry kind"); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = validateSha256Hex(entry.sha256.ref(), "entry sha256");
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (entry.size < 0) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "entry size must not be negative"));
        }
        if (!paths.insert(entry.path.to_std_string()).second) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "duplicate entry path in batch: " +
                                         entry.path.to_std_string()));
        }
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    {
        // 输入必须已登记（ensureInput 先行）；不存在整批拒绝。
        auto selected = prepare(impl_->handle, "SELECT input_id FROM inputs WHERE input_id=?1");
        if (selected.is_err()) return errorOf(std::move(selected));
        auto statement = std::move(selected).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, inputId); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "input is not registered: " + std::to_string(inputId)));
        }
    }

    if (batchId != 0) {
        // 批次挂靠（#143 批 3）：门校验 + input 作用域行，与事实同事务。
        if (auto result = validateOpenBatchForWrite(impl_->handle, batchId); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto scopeInsert = prepare(impl_->handle, kScopeRowInsertSql);
        if (scopeInsert.is_err()) return errorOf(std::move(scopeInsert));
        auto scopeStatement = std::move(scopeInsert).unwrap();
        if (auto result = writeScopeRow(impl_->handle, scopeStatement.raw, batchId,
                                        DbBatchScopeKind::Input, inputId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
    }

    // 载入该输入全部既有行（含墓碑）；匹配/复活判定在内存做，SQL 只承担
    // 单行写。行数 = 该输入历史条目总数，量级与单 jar 条目数相当。
    struct ExistingRow
    {
        ca::i64     entryId = 0;
        std::string path;
        std::string sha256;
        bool        live = false;
    };
    std::vector<ExistingRow> existing;
    {
        auto selected = prepare(
            impl_->handle,
            "SELECT entry_id, path, sha256, tombstoned FROM entries "
            "WHERE input_id=?1 ORDER BY entry_id");
        if (selected.is_err()) return errorOf(std::move(selected));
        auto statement = std::move(selected).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, inputId); result.is_err()) {
            return errorOf(std::move(result));
        }
        for (;;) {
            auto hasRow = step(impl_->handle, statement.raw);
            if (hasRow.is_err()) return errorOf(std::move(hasRow));
            if (!hasRow.unwrap()) break;
            ExistingRow row;
            row.entryId = sqlite3_column_int64(statement.raw, 0);
            row.path    = textColumn(statement.raw, 1);
            row.sha256  = textColumn(statement.raw, 2);
            row.live    = sqlite3_column_int(statement.raw, 3) == 0;
            existing.push_back(std::move(row));
        }
    }

    using EntryAnchor = std::pair<std::string, std::string>;   // (path, sha256)

    // 本批锚集合：墓碑判定与匹配共用。
    std::set<EntryAnchor> batchAnchors;
    for (const DbEntryInput& entry : entries) {
        batchAnchors.insert(EntryAnchor(entry.path.to_std_string(), entry.sha256.to_std_string()));
    }

    DbEntryIngestStats stats;
    stats.total = entries.size();

    // 阶段一：墓碑——本批未出现的既有活跃条目标记消失（只改旗标，不删行）。
    {
        auto markDead = prepare(impl_->handle,
                                "UPDATE entries SET tombstoned=1 WHERE entry_id=?1");
        if (markDead.is_err()) return errorOf(std::move(markDead));
        auto statement = std::move(markDead).unwrap();
        for (const ExistingRow& row : existing) {
            if (!row.live || batchAnchors.count(EntryAnchor(row.path, row.sha256)) > 0) continue;
            if (auto result = bindInt(impl_->handle, statement.raw, 1, row.entryId);
                result.is_err()) {
                return errorOf(std::move(result));
            }
            auto done = step(impl_->handle, statement.raw);
            resetStatement(statement);
            if (done.is_err()) return errorOf(std::move(done));
            ++stats.tombstoned;
        }
    }

    // 阶段二：锚匹配——命中复用 id（活跃行刷新元数据，墓碑行复活），未命中
    // 新建。同 (input, path) 至多一行活跃由 entries_live 部分唯一索引兜底。
    std::map<EntryAnchor, std::pair<const ExistingRow*, bool>> byAnchor;
    for (const ExistingRow& row : existing) {
        if (row.live || batchAnchors.count(EntryAnchor(row.path, row.sha256)) > 0) {
            byAnchor.emplace(EntryAnchor(row.path, row.sha256),
                             std::make_pair(&row, row.live));
        }
    }

    {
        auto refresh = prepare(
            impl_->handle,
            "UPDATE entries SET kind=?2, size=?3, regular=?4, tombstoned=0 WHERE entry_id=?1");
        if (refresh.is_err()) return errorOf(std::move(refresh));
        auto refreshStatement = std::move(refresh).unwrap();

        auto insert = prepare(
            impl_->handle,
            "INSERT INTO entries(input_id, path, kind, size, sha256, regular) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
        if (insert.is_err()) return errorOf(std::move(insert));
        auto insertStatement = std::move(insert).unwrap();

        for (const DbEntryInput& entry : entries) {
            const EntryAnchor   anchor(entry.path.to_std_string(), entry.sha256.to_std_string());
            const auto          found = byAnchor.find(anchor);
            const ca::str::Utf8StringRef shaRef(entry.sha256);
            if (found != byAnchor.end()) {
                const ca::i64 entryId = found->second.first->entryId;
                if (auto result = bindInt(impl_->handle, refreshStatement.raw, 1, entryId);
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                if (auto result =
                        bindText(impl_->handle, refreshStatement.raw, 2,
                                 ca::str::Utf8StringRef(entry.kind));
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                if (auto result = bindInt(impl_->handle, refreshStatement.raw, 3, entry.size);
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                if (auto result = bindInt(impl_->handle, refreshStatement.raw, 4,
                                          entry.regular ? 1 : 0);
                    result.is_err()) {
                    return errorOf(std::move(result));
                }
                auto done = step(impl_->handle, refreshStatement.raw);
                resetStatement(refreshStatement);
                if (done.is_err()) return errorOf(std::move(done));

                if (found->second.second) {
                    ++stats.reused;
                }
                else {
                    ++stats.resurrected;
                }
                continue;
            }

            if (auto result = bindInt(impl_->handle, insertStatement.raw, 1, inputId);
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(impl_->handle, insertStatement.raw, 2, entry.path.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(impl_->handle, insertStatement.raw, 3, entry.kind.ref());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindInt(impl_->handle, insertStatement.raw, 4, entry.size);
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(impl_->handle, insertStatement.raw, 5, shaRef);
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindInt(impl_->handle, insertStatement.raw, 6,
                                      entry.regular ? 1 : 0);
                result.is_err()) {
                return errorOf(std::move(result));
            }
            auto done = step(impl_->handle, insertStatement.raw);
            resetStatement(insertStatement);
            if (done.is_err()) return errorOf(std::move(done));
            ++stats.created;
        }
    }

    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(stats);
}

ca::Result<std::vector<DbInputInfo>, AnalysisDbError> AnalysisDb::listInputs() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(impl_->handle,
                            "SELECT input_id, path, size, sha256 FROM inputs ORDER BY path");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    std::vector<DbInputInfo> inputs;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        DbInputInfo info;
        info.inputId = sqlite3_column_int64(statement.raw, 0);
        info.path    = utf8Column(statement.raw, 1);
        info.size    = sqlite3_column_int64(statement.raw, 2);
        info.sha256  = utf8Column(statement.raw, 3);
        inputs.push_back(std::move(info));
    }
    return ca::Ok(std::move(inputs));
}

ca::Result<std::vector<DbEntryInfo>, AnalysisDbError> AnalysisDb::listEntries(
    ca::i64 inputId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (inputId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "input id must be positive"));
    }

    {
        // 输入不存在与「已登记但无条目」区分开：前者是调用方错误。
        auto selected = prepare(impl_->handle, "SELECT input_id FROM inputs WHERE input_id=?1");
        if (selected.is_err()) return errorOf(std::move(selected));
        auto statement = std::move(selected).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, inputId); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "input is not registered: " + std::to_string(inputId)));
        }
    }

    auto prepared = prepare(
        impl_->handle,
        "SELECT entry_id, input_id, path, kind, size, sha256, regular, tombstoned "
        "FROM entries WHERE input_id=?1 ORDER BY path, entry_id");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(impl_->handle, statement.raw, 1, inputId); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<DbEntryInfo> entries;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        DbEntryInfo info;
        info.entryId    = sqlite3_column_int64(statement.raw, 0);
        info.inputId    = sqlite3_column_int64(statement.raw, 1);
        info.path       = utf8Column(statement.raw, 2);
        info.kind       = utf8Column(statement.raw, 3);
        info.size       = sqlite3_column_int64(statement.raw, 4);
        info.sha256     = utf8Column(statement.raw, 5);
        info.regular    = sqlite3_column_int(statement.raw, 6) != 0;
        info.tombstoned = sqlite3_column_int(statement.raw, 7) != 0;
        entries.push_back(std::move(info));
    }
    return ca::Ok(std::move(entries));
}

//-- 普查投影层（census，#121 §5 B4） ------------------------------------------

namespace {

// census 口径的类集合：关联指定输入活跃条目的类（条目墓碑 = 类已不在当前
// 容器内容；entry_id 为空的类不属于任何输入的普查面）。?1 = input_id。
constexpr const char* kCensusLiveClassIds =
    "(SELECT c.class_id FROM classes c JOIN entries e ON e.entry_id = c.entry_id"
    " WHERE e.input_id = ?1 AND e.tombstoned = 0)";

// ACC_NATIVE（JVMS 4.6 方法访问旗标）；members.access 是整数列，按位与即可。
constexpr ca::i64 kMethodAccNative = 0x100;

/// @brief census 查询共用的前置门：连接已打开 + 输入已登记（不存在与
///        「已登记但无条目」区分开，前者是调用方错误）。
ca::Result<void, AnalysisDbError> censusGate(sqlite3* handle, ca::i64 inputId)
{
    if (inputId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "input id must be positive"));
    }
    auto selected = prepare(handle, "SELECT input_id FROM inputs WHERE input_id=?1");
    if (selected.is_err()) return errorOf(std::move(selected));
    auto statement = std::move(selected).unwrap();
    if (auto result = bindInt(handle, statement.raw, 1, inputId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "input is not registered: " + std::to_string(inputId)));
    }
    return ca::Ok();
}

}   // namespace

ca::Result<std::vector<DbClassCensusRow>, AnalysisDbError> AnalysisDb::censusClasses(
    ca::i64 inputId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto gate = censusGate(impl_->handle, inputId); gate.is_err()) {
        return ca::Err(std::move(gate).unwrap_err());
    }

    // 计数走 members/interfaces 的 (class_id, ...) 索引前缀相关子查询；行序
    // internal_name BINARY 升序即确定性序（导出面按库序渲染，不重排）。
    const std::string classesSql =
        std::string("SELECT c.internal_name, c.access, c.super_name,")
        + " (SELECT COUNT(*) FROM interfaces i WHERE i.class_id = c.class_id),"
          " (SELECT COUNT(*) FROM members m WHERE m.class_id = c.class_id AND m.kind = 1),"
          " (SELECT COUNT(*) FROM members m WHERE m.class_id = c.class_id AND m.kind = 0),"
          " (SELECT COUNT(*) FROM members m WHERE m.class_id = c.class_id AND m.kind = 1"
          "   AND (m.access & " + std::to_string(kMethodAccNative) + ") <> 0),"
          " c.entry_id, e.size, e.regular"
          " FROM classes c JOIN entries e ON e.entry_id = c.entry_id"
          " WHERE e.input_id = ?1 AND e.tombstoned = 0"
          " ORDER BY c.internal_name";
    auto prepared = prepare(impl_->handle, classesSql.c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(impl_->handle, statement.raw, 1, inputId); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<DbClassCensusRow> rows;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        DbClassCensusRow row;
        row.internalName = utf8Column(statement.raw, 0);
        row.access       = static_cast<ca::i32>(sqlite3_column_int64(statement.raw, 1));
        row.superName    = utf8Column(statement.raw, 2);
        row.kind         = classifyClassKind(row.access, row.superName.ref());
        row.origin       = classifyLibraryOrigin(row.internalName.ref());
        row.ifaceCount        = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 3));
        row.methodCount       = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 4));
        row.fieldCount        = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 5));
        row.nativeMethodCount = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 6));
        row.entryId           = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 7));
        row.byteSize          = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 8));
        row.entryRegular      = sqlite3_column_int(statement.raw, 9) != 0;
        rows.push_back(std::move(row));
    }
    return ca::Ok(std::move(rows));
}

ca::Result<std::vector<DbMemberCensusRow>, AnalysisDbError> AnalysisDb::censusMembers(
    ca::i64 inputId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto gate = censusGate(impl_->handle, inputId); gate.is_err()) {
        return ca::Err(std::move(gate).unwrap_err());
    }

    // 一条 SQL 成型：成员 JOIN 所属类 JOIN 所属条目，可见域与 censusClasses
    // 同谓词（本输入活跃条目）。行序复合键 BINARY 升序（owner 内部名 → kind
    // 编码序 → 名字 → 描述符）即确定性序（导出面按库序渲染，不重排）。
    const std::string membersSql =
        std::string("SELECT c.internal_name, m.kind, m.name, m.descriptor, m.access, c.entry_id")
        + " FROM members m"
          " JOIN classes c ON c.class_id = m.class_id"
          " JOIN entries e ON e.entry_id = c.entry_id"
          " WHERE e.input_id = ?1 AND e.tombstoned = 0"
          " ORDER BY c.internal_name, m.kind, m.name, m.descriptor";
    auto prepared = prepare(impl_->handle, membersSql.c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(impl_->handle, statement.raw, 1, inputId); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<DbMemberCensusRow> rows;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        DbMemberCensusRow row;
        row.ownerInternalName = utf8Column(statement.raw, 0);
        row.kind              = static_cast<DbMemberKind>(sqlite3_column_int(statement.raw, 1));
        row.name              = utf8Column(statement.raw, 2);
        row.descriptor        = utf8Column(statement.raw, 3);
        row.access            = sqlite3_column_int(statement.raw, 4);
        // native 是投影层派生：方法按 ACC_NATIVE 按位测试，字段恒 0。
        row.native =
            row.kind == DbMemberKind::Method
            && (row.access & static_cast<ca::i32>(kMethodAccNative)) != 0;
        row.entryId           = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 5));
        row.origin            = classifyLibraryOrigin(row.ownerInternalName.ref());
        rows.push_back(std::move(row));
    }
    return ca::Ok(std::move(rows));
}

ca::Result<DbContainerCensus, AnalysisDbError> AnalysisDb::censusContainer(ca::i64 inputId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto gate = censusGate(impl_->handle, inputId); gate.is_err()) {
        return ca::Err(std::move(gate).unwrap_err());
    }

    DbContainerCensus census;
    census.inputId = inputId;

    {
        // 输入容器身份（登记 path / 规模 / 内容锚）。
        auto prepared = prepare(
            impl_->handle, "SELECT path, size, sha256 FROM inputs WHERE input_id=?1");
        if (prepared.is_err()) return errorOf(std::move(prepared));
        auto statement = std::move(prepared).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, inputId); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                     "census input query returned no row"));
        }
        census.path   = utf8Column(statement.raw, 0);
        census.size   = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 1));
        census.sha256 = utf8Column(statement.raw, 2);
    }

    {
        // 条目形态面：活跃/墓碑、regular 二分、kind 存储值分布、路径形态
        // facet 分布（entry_shape 投影层细化）一次扫描全部产出。
        auto prepared = prepare(
            impl_->handle,
            "SELECT path, kind, tombstoned, regular FROM entries WHERE input_id=?1"
            " ORDER BY entry_id");
        if (prepared.is_err()) return errorOf(std::move(prepared));
        auto statement = std::move(prepared).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, inputId); result.is_err()) {
            return errorOf(std::move(result));
        }

        std::map<std::string, ca::i64>         kindCounts;
        std::map<EntryPathShape, ca::i64>      shapeCounts;
        for (;;) {
            auto hasRow = step(impl_->handle, statement.raw);
            if (hasRow.is_err()) return errorOf(std::move(hasRow));
            if (!hasRow.unwrap()) break;
            const bool tombstoned = sqlite3_column_int(statement.raw, 2) != 0;
            if (tombstoned) {
                census.entriesTombstoned += 1;
                continue;
            }
            census.entriesActive += 1;
            const bool regular = sqlite3_column_int(statement.raw, 3) != 0;
            if (regular) {
                census.entriesRegular += 1;
            }
            else {
                census.entriesIrregular += 1;
            }
            const auto kind = utf8Column(statement.raw, 1);
            kindCounts[kind.to_std_string()] += 1;
            const auto path = utf8Column(statement.raw, 0);
            shapeCounts[classifyEntryPathShape(std::string_view(
                reinterpret_cast<const char*>(path.data()), path.byte_length()))] += 1;
        }
        for (const auto& [kind, count] : kindCounts) {
            census.entryKinds.push_back(
                {ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(
                                                              kind.data()),
                                                          kind.size()),
                 count});
        }
        for (const auto& [shape, count] : shapeCounts) {
            const auto name = entryPathShapeName(shape);
            census.entryShapes.push_back(
                {ca::str::Utf8String::from_data_unchecked(
                     reinterpret_cast<const ca::u8*>(name.data()), name.size()),
                 count});
        }
    }

    {
        // 分析事实面：类/成员/引用计数（成员计数经 kCensusLiveClassIds 索引
        // 下探；引用按 source_class_id 索引计数）。
        const std::string nativeBit = std::to_string(kMethodAccNative);
        const std::string factsSql =
            std::string("SELECT")
            + " (SELECT COUNT(*) FROM classes c JOIN entries e ON e.entry_id = c.entry_id"
              "   WHERE e.input_id = ?1 AND e.tombstoned = 0),"
              " (SELECT COUNT(*) FROM members m WHERE m.kind = 1 AND m.class_id IN "
            + std::string(kCensusLiveClassIds) + "),"
            + " (SELECT COUNT(*) FROM members m WHERE m.kind = 0 AND m.class_id IN "
            + std::string(kCensusLiveClassIds) + "),"
            + " (SELECT COUNT(*) FROM members m WHERE m.kind = 1 AND (m.access & " + nativeBit
            + ") <> 0 AND m.class_id IN " + std::string(kCensusLiveClassIds) + "),"
            + " (SELECT COUNT(*) FROM refs r WHERE r.source_class_id IN "
            + std::string(kCensusLiveClassIds) + ")";
        auto prepared = prepare(impl_->handle, factsSql.c_str());
        if (prepared.is_err()) return errorOf(std::move(prepared));
        auto statement = std::move(prepared).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, inputId); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                     "census fact query returned no row"));
        }
        census.classes       = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 0));
        census.methods       = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 1));
        census.fields        = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 2));
        census.nativeMethods = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 3));
        census.references    = static_cast<ca::i64>(sqlite3_column_int64(statement.raw, 4));
    }
    return ca::Ok(std::move(census));
}

//-- 出口遮蔽层（mask_rules / 信任级别，schema v4；行级归属 v7） --------------

ca::Result<DbMaskRuleReplaceStats, AnalysisDbError> AnalysisDb::replaceMaskRules(
    std::vector<DbMaskRuleInput> rules, ca::i64 producerId)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateRegisteredProducer(impl_->handle, producerId); result.is_err()) {
        return errorOf(std::move(result));
    }
    // 整批校验前置（含编译性，与求值器 MaskEvaluator::build 同源）：任何
    // 一条非法整批不写库。
    if (rules.size() > kMaximumMaskRules) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "mask rule count " + std::to_string(rules.size()) +
                                     " exceeds maximum " + std::to_string(kMaximumMaskRules)));
    }
    for (ca::usize index = 0; index < rules.size(); ++index) {
        const DbMaskRuleInput& rule = rules[index];
        const std::string where = "mask rule[" + std::to_string(index) + "]";
        if (rule.pattern.is_empty()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid, where + ": pattern is empty"));
        }
        if (hasNul(rule.pattern)) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     where + ": pattern must not contain NUL"));
        }
        if (rule.pattern.byte_length() > kMaximumMaskPatternBytes) {
            return ca::Err(makeError(
                AnalysisDbErrorKind::Invalid,
                where + ": pattern length " + std::to_string(rule.pattern.byte_length()) +
                    " exceeds maximum " + std::to_string(kMaximumMaskPatternBytes)));
        }
        auto compiled = MaskRegex::compile(std::string_view(
            reinterpret_cast<const char*>(rule.pattern.data()), rule.pattern.byte_length()));
        if (compiled.is_err()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     where + ": pattern failed to compile: " +
                                         std::move(compiled).unwrap_err().message.to_std_string()));
        }
        if (hasNul(rule.note)) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     where + ": note must not contain NUL"));
        }
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    DbMaskRuleReplaceStats stats;
    {
        auto prepared = prepare(impl_->handle, "SELECT COUNT(*) FROM mask_rules");
        if (prepared.is_err()) return errorOf(std::move(prepared));
        auto statement = std::move(prepared).unwrap();
        auto hasRow    = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        stats.retired = hasRow.unwrap() ? sqlite3_column_int64(statement.raw, 0) : 0;
    }
    if (auto result = exec(impl_->handle, "DELETE FROM mask_rules"); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto insert = prepare(impl_->handle,
                          "INSERT INTO mask_rules(priority, pattern, action, note, producer_id) "
                          "VALUES(?1, ?2, ?3, ?4, ?5)");
    if (insert.is_err()) return errorOf(std::move(insert));
    auto insertStatement = std::move(insert).unwrap();
    for (const DbMaskRuleInput& rule : rules) {
        // 语句批次内复用：上一轮 step 至 DONE 后必须 reset 才能重新绑定。
        resetStatement(insertStatement);
        if (auto result = bindInt(impl_->handle, insertStatement.raw, 1, rule.priority);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(impl_->handle, insertStatement.raw, 2, rule.pattern);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, insertStatement.raw, 3,
                                  static_cast<ca::i64>(rule.action));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(impl_->handle, insertStatement.raw, 4, rule.note);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        // 行级归属：0 = 匿名写入（producer_id 落 NULL），非 0 整批统一盖章。
        if (auto result = producerId > 0 ? bindInt(impl_->handle, insertStatement.raw, 5, producerId)
                                         : bindNull(impl_->handle, insertStatement.raw, 5);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, insertStatement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        ++stats.total;
        ++stats.created;
    }
    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(stats);
}

ca::Result<std::vector<DbMaskRule>, AnalysisDbError> AnalysisDb::listMaskRules() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(impl_->handle,
                            "SELECT rule_id, priority, pattern, action, note, hit_count, "
                            "producer_id FROM mask_rules ORDER BY priority, rule_id");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    std::vector<DbMaskRule> rules;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        DbMaskRule rule;
        rule.ruleId     = sqlite3_column_int64(statement.raw, 0);
        rule.priority   = sqlite3_column_int(statement.raw, 1);
        rule.pattern    = utf8Column(statement.raw, 2);
        rule.action     = sqlite3_column_int(statement.raw, 3) != 0 ? MaskAction::Mask
                                                                    : MaskAction::Allow;
        rule.note       = utf8Column(statement.raw, 4);
        rule.hitCount   = sqlite3_column_int64(statement.raw, 5);
        // NULL = 匿名写入，读回 0（与 renames 行级归属同口径）。
        rule.producerId = sqlite3_column_int64(statement.raw, 6);
        rules.push_back(std::move(rule));
    }
    return ca::Ok(std::move(rules));
}

ca::Result<void, AnalysisDbError> AnalysisDb::setMaskTrustLevel(MaskTrustLevel level,
                                                                ca::i64        producerId)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateRegisteredProducer(impl_->handle, producerId); result.is_err()) {
        return errorOf(std::move(result));
    }

    // 信任级别值与归属伴随键（v7 #143）同事务落库：归属随每次设置整体刷新
    // （last-write-wins 口径同 renames），匿名写入清除伴随键。
    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    auto prepared = prepare(impl_->handle,
                            "INSERT INTO meta(key, value) VALUES('mask_trust_level', ?1) "
                            "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    const char*    value = level == MaskTrustLevel::Untrusted ? "untrusted" : "trusted";
    if (auto result = bindText(impl_->handle, statement.raw, 1,
                               ca::str::Utf8StringRef::from_cstr(value));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));

    if (producerId > 0) {
        auto stampPrepared = prepare(
            impl_->handle,
            "INSERT INTO meta(key, value) VALUES('mask_trust_level_producer', ?1) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        if (stampPrepared.is_err()) return errorOf(std::move(stampPrepared));
        auto stampStatement = std::move(stampPrepared).unwrap();
        if (auto result = bindText(impl_->handle, stampStatement.raw, 1,
                                   ca::str::Utf8StringRef::from_string_view(
                                       std::to_string(producerId)));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto stamped = step(impl_->handle, stampStatement.raw);
        if (stamped.is_err()) return errorOf(std::move(stamped));
    }
    else {
        // 匿名写入 = 回到无归属态：伴随键整行清除（缺键即匿名，读路径无歧义）。
        if (auto result = exec(impl_->handle,
                               "DELETE FROM meta WHERE key='mask_trust_level_producer'");
            result.is_err()) {
            return errorOf(std::move(result));
        }
    }
    return txn.commit();
}

ca::Result<MaskTrustLevel, AnalysisDbError> AnalysisDb::maskTrustLevel() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared =
        prepare(impl_->handle, "SELECT value FROM meta WHERE key='mask_trust_level'");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    auto hasRow    = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(MaskTrustLevel::Trusted);

    const auto value = textColumn(statement.raw, 0);
    if (value == "untrusted") return ca::Ok(MaskTrustLevel::Untrusted);
    if (value == "trusted") return ca::Ok(MaskTrustLevel::Trusted);
    return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                             "mask trust level marker is invalid: " + value));
}

ca::Result<void, AnalysisDbError> AnalysisDb::recordMaskRuleHits(
    const std::vector<ca::i64>& ruleIds)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (ruleIds.empty()) return ca::Ok();

    // 按出现次数逐 id 累加：出口按行记账，同批重复 id 各计一次。
    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    auto prepared = prepare(impl_->handle,
                            "UPDATE mask_rules SET hit_count = hit_count + 1 "
                            "WHERE rule_id = ?1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    for (const ca::i64 ruleId : ruleIds) {
        // 语句批次内复用：上一轮 step 至 DONE 后必须 reset 才能重新绑定。
        resetStatement(statement);
        if (auto result = bindInt(impl_->handle, statement.raw, 1, ruleId); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        // 未知 rule_id 的 0 行更新静默跳过：命中记账不因规则替换竞态失败。
    }
    return txn.commit();
}

//-- producer 注册表（producers / batches / batch_scopes，schema v6） --------


ca::Result<ca::i64, AnalysisDbError> AnalysisDb::registerProducer(const DbProducerInput& input)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateRequiredText(input.key.ref(), "producer key"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = validateRequiredText(input.name.ref(), "producer name"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (hasNul(input.toolFingerprint.ref())) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "producer tool fingerprint must not contain NUL"));
    }
    if (input.trustLevel != DbProducerTrustLevel::Untrusted &&
        input.trustLevel != DbProducerTrustLevel::Trusted) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "producer trust level is invalid"));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    {
        // key 是身份键：重注册刷新元数据并复用 id（created_at 只在首注写入）。
        auto upserted = prepare(
            impl_->handle,
            "INSERT INTO producers(key, name, tool_fingerprint, trust_level, created_at, "
            "updated_at) VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
            "ON CONFLICT(key) DO UPDATE SET name=excluded.name, "
            "tool_fingerprint=excluded.tool_fingerprint, trust_level=excluded.trust_level, "
            "updated_at=excluded.updated_at");
        if (upserted.is_err()) return errorOf(std::move(upserted));
        auto statement = std::move(upserted).unwrap();
        if (auto result = bindText(impl_->handle, statement.raw, 1, input.key.ref());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(impl_->handle, statement.raw, 2, input.name.ref());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result =
                bindText(impl_->handle, statement.raw, 3, input.toolFingerprint.ref());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 4,
                                  static_cast<ca::i64>(input.trustLevel));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        // nowIso8601() 的临时串在本语句末销毁：视图必须挂在具名串上，
        // 挂临时串是悬空读（Windows 上凑巧读到未覆盖栈字节，Linux 上读到
        // 乱码，CI 实证）。
        const std::string nowStamp = nowIso8601();
        const auto       now       = ca::str::Utf8StringRef::from_string_view(nowStamp);
        if (auto result = bindText(impl_->handle, statement.raw, 5, now); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(impl_->handle, statement.raw, 6, now); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto done = step(impl_->handle, statement.raw);
        if (done.is_err()) return errorOf(std::move(done));
    }

    ca::i64 producerId = 0;
    {
        auto selected = prepare(impl_->handle,
                                "SELECT producer_id FROM producers WHERE key=?1");
        if (selected.is_err()) return errorOf(std::move(selected));
        auto statement = std::move(selected).unwrap();
        if (auto result = bindText(impl_->handle, statement.raw, 1, input.key.ref());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Sqlite,
                                     "producer identity vanished after registration"));
        }
        producerId = sqlite3_column_int64(statement.raw, 0);
    }

    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(producerId);
}

ca::Result<ca::i64, AnalysisDbError> AnalysisDb::openBatch(ca::i64                       producerId,
                                                           DbBatchKind                   kind,
                                                           const ca::str::Utf8StringRef& note)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (producerId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "producer id must be positive"));
    }
    if (hasNul(note)) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "batch note must not contain NUL"));
    }
    if (batchKindToSql(kind) == nullptr) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "batch kind is invalid"));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    {
        // 生产者必须已注册；FK 也拦，但先查给出可诊断错误。
        auto selected =
            prepare(impl_->handle, "SELECT 1 FROM producers WHERE producer_id=?1");
        if (selected.is_err()) return errorOf(std::move(selected));
        auto statement = std::move(selected).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, producerId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "producer is not registered: " +
                                         std::to_string(producerId)));
        }
    }

    ca::i64 batchId = 0;
    {
        auto inserted = prepare(impl_->handle,
                                "INSERT INTO batches(producer_id, kind, started_at, note) "
                                "VALUES(?1, ?2, ?3, ?4)");
        if (inserted.is_err()) return errorOf(std::move(inserted));
        auto statement = std::move(inserted).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, producerId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(impl_->handle, statement.raw, 2,
                                   ca::str::Utf8StringRef::from_cstr(batchKindToSql(kind)));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result =
                bindText(impl_->handle, statement.raw, 3,
                         ca::str::Utf8StringRef::from_string_view(nowIso8601()));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        // note 允许为空串：缺省构造的视图 data() 为空指针，bind_text(NULL, 0)
        // 会落 SQL NULL 而违反 NOT NULL；空串显式绑 ''（与 bindEmptyText 同口径）。
        if (auto result = note.is_empty()
                              ? bindEmptyText(impl_->handle, statement.raw, 4)
                              : bindText(impl_->handle, statement.raw, 4, note);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto done = step(impl_->handle, statement.raw);
        if (done.is_err()) return errorOf(std::move(done));
        batchId = sqlite3_last_insert_rowid(impl_->handle);
    }

    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(batchId);
}

ca::Result<void, AnalysisDbError> AnalysisDb::closeBatch(ca::i64              batchId,
                                                         const DbBatchCounts& counts)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (batchId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "batch id must be positive"));
    }
    if (counts.classCount < 0 || counts.entryCount < 0 || counts.memberCount < 0 ||
        counts.refCount < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "batch counts must not be negative"));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    {
        // 收尾门：批次必须存在且未收尾（重复收尾与残批不存在都是契约错误）。
        auto selected =
            prepare(impl_->handle, "SELECT finished_at FROM batches WHERE batch_id=?1");
        if (selected.is_err()) return errorOf(std::move(selected));
        auto statement = std::move(selected).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, batchId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "batch is not registered: " + std::to_string(batchId)));
        }
        if (!textColumn(statement.raw, 0).empty()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "batch is already closed: " + std::to_string(batchId)));
        }
    }

    {
        auto updated = prepare(impl_->handle,
                               "UPDATE batches SET finished_at=?2, class_count=?3, "
                               "entry_count=?4, member_count=?5, ref_count=?6 "
                               "WHERE batch_id=?1");
        if (updated.is_err()) return errorOf(std::move(updated));
        auto statement = std::move(updated).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, batchId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result =
                bindText(impl_->handle, statement.raw, 2,
                         ca::str::Utf8StringRef::from_string_view(nowIso8601()));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 3, counts.classCount);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 4, counts.entryCount);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 5, counts.memberCount);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 6, counts.refCount);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto done = step(impl_->handle, statement.raw);
        if (done.is_err()) return errorOf(std::move(done));
    }

    return txn.commit();
}

ca::Result<std::vector<DbProducerInfo>, AnalysisDbError> AnalysisDb::listProducers() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(impl_->handle,
                            "SELECT producer_id, key, name, tool_fingerprint, trust_level, "
                            "created_at, updated_at FROM producers ORDER BY producer_id");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    std::vector<DbProducerInfo> producers;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        DbProducerInfo info;
        info.producerId      = sqlite3_column_int64(statement.raw, 0);
        info.key             = utf8Column(statement.raw, 1);
        info.name            = utf8Column(statement.raw, 2);
        info.toolFingerprint = utf8Column(statement.raw, 3);
        info.trustLevel = sqlite3_column_int(statement.raw, 4) != 0
                              ? DbProducerTrustLevel::Trusted
                              : DbProducerTrustLevel::Untrusted;
        info.createdAt  = utf8Column(statement.raw, 5);
        info.updatedAt  = utf8Column(statement.raw, 6);
        producers.push_back(std::move(info));
    }
    return ca::Ok(std::move(producers));
}

ca::Result<std::vector<DbBatchInfo>, AnalysisDbError> AnalysisDb::listBatches(
    std::optional<ca::i64> producerId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (producerId.has_value() && producerId.value() <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "producer id must be positive"));
    }

    if (producerId.has_value()) {
        // 过滤键未注册与「已注册但无批」区分开：前者是调用方错误。
        auto selected =
            prepare(impl_->handle, "SELECT 1 FROM producers WHERE producer_id=?1");
        if (selected.is_err()) return errorOf(std::move(selected));
        auto existsStatement = std::move(selected).unwrap();
        if (auto result = bindInt(impl_->handle, existsStatement.raw, 1, producerId.value());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasProducer = step(impl_->handle, existsStatement.raw);
        if (hasProducer.is_err()) return errorOf(std::move(hasProducer));
        if (!hasProducer.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "producer is not registered: " +
                                         std::to_string(producerId.value())));
        }
    }

    // 作用域数走 batch_scopes 聚合子查询（批数量级远小于作用域行量级）。
    std::string sql =
        "SELECT b.batch_id, b.producer_id, p.key, b.kind, b.started_at, b.finished_at, "
        "       b.class_count, b.entry_count, b.member_count, b.ref_count, "
        "       COALESCE(sc.scope_count, 0), b.note "
        "FROM batches b "
        "JOIN producers p ON p.producer_id = b.producer_id "
        "LEFT JOIN (SELECT batch_id, COUNT(*) AS scope_count FROM batch_scopes "
        "           GROUP BY batch_id) sc ON sc.batch_id = b.batch_id ";
    if (producerId.has_value()) sql += "WHERE b.producer_id=?1 ";
    sql += "ORDER BY b.batch_id";

    auto prepared = prepare(impl_->handle, sql.c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (producerId.has_value()) {
        if (auto result = bindInt(impl_->handle, statement.raw, 1, producerId.value());
            result.is_err()) {
            return errorOf(std::move(result));
        }
    }

    std::vector<DbBatchInfo> batches;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        auto kind = batchKindFromSql(textColumn(statement.raw, 3));
        if (kind.is_err()) return errorOf(std::move(kind));
        DbBatchInfo info;
        info.batchId     = sqlite3_column_int64(statement.raw, 0);
        info.producerId  = sqlite3_column_int64(statement.raw, 1);
        info.producerKey = utf8Column(statement.raw, 2);
        info.kind        = kind.unwrap();
        info.startedAt   = utf8Column(statement.raw, 4);
        info.finishedAt  = utf8Column(statement.raw, 5);
        info.classCount  = sqlite3_column_int64(statement.raw, 6);
        info.entryCount  = sqlite3_column_int64(statement.raw, 7);
        info.memberCount = sqlite3_column_int64(statement.raw, 8);
        info.refCount    = sqlite3_column_int64(statement.raw, 9);
        info.scopeCount  = sqlite3_column_int64(statement.raw, 10);
        info.note        = utf8Column(statement.raw, 11);
        batches.push_back(std::move(info));
    }
    return ca::Ok(std::move(batches));
}

ca::Result<std::vector<DbBatchScope>, AnalysisDbError> AnalysisDb::batchScopes(
    ca::i64 batchId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (batchId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "batch id must be positive"));
    }

    {
        // 批次不存在与「已登记但无作用域行」区分开：前者是调用方错误。
        auto selected = prepare(impl_->handle, "SELECT 1 FROM batches WHERE batch_id=?1");
        if (selected.is_err()) return errorOf(std::move(selected));
        auto statement = std::move(selected).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, batchId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "batch is not registered: " + std::to_string(batchId)));
        }
    }

    auto prepared = prepare(impl_->handle,
                            "SELECT scope_kind, scope_id FROM batch_scopes "
                            "WHERE batch_id=?1 ORDER BY scope_kind, scope_id");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(impl_->handle, statement.raw, 1, batchId); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<DbBatchScope> scopes;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        DbBatchScope scope;
        scope.batchId = batchId;
        scope.kind    = static_cast<DbBatchScopeKind>(sqlite3_column_int(statement.raw, 0));
        scope.scopeId = sqlite3_column_int64(statement.raw, 1);
        scopes.push_back(std::move(scope));
    }
    return ca::Ok(std::move(scopes));
}

ca::Result<std::optional<DbOwnershipInfo>, AnalysisDbError> AnalysisDb::refSliceOwnership(
    DbMemberKind kind, ca::i64 memberId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validateMemberKind(kind); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (memberId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "member id must be positive"));
    }

    // 取大规则：成员级锚与 refs_all 锚两个前缀各探一次，batch_id 大者胜。
    // 探测结果先 move 出 Result（optional 携带 Utf8String，只移不可拷，
    // 左值 unwrap 的按值返回路径不可用）。
    auto member = probeScopeOwnership(impl_->handle, DbBatchScopeKind::Member, memberId);
    if (member.is_err()) return errorOf(std::move(member));
    auto refsAll = probeScopeOwnership(impl_->handle, DbBatchScopeKind::RefsAll, 0);
    if (refsAll.is_err()) return errorOf(std::move(refsAll));

    std::optional<DbOwnershipInfo> memberInfo  = std::move(member).unwrap();
    std::optional<DbOwnershipInfo> refsAllInfo = std::move(refsAll).unwrap();

    if (!memberInfo.has_value() && !refsAllInfo.has_value()) {
        return ca::Ok(std::optional<DbOwnershipInfo>());
    }
    if (!memberInfo.has_value()) return ca::Ok(std::move(refsAllInfo));
    if (!refsAllInfo.has_value()) return ca::Ok(std::move(memberInfo));
    if (memberInfo->batchId >= refsAllInfo->batchId) {
        return ca::Ok(std::move(memberInfo));
    }
    return ca::Ok(std::move(refsAllInfo));
}

ca::Result<std::optional<DbOwnershipInfo>, AnalysisDbError> AnalysisDb::classOwnership(
    ca::i64 classId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (classId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "class id must be positive"));
    }

    // 类事实归属只探 (2, class_id)：refs_all 不回退参与类事实归属（整库
    // 引用替换不触碰类行）；缺逐类作用域行 = 类归属为空（契约，非回退）。
    return probeScopeOwnership(impl_->handle, DbBatchScopeKind::Class, classId);
}

ca::Result<std::vector<DbProducerStats>, AnalysisDbError> AnalysisDb::producerStats() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(
        impl_->handle,
        "SELECT p.producer_id, p.key, p.name, p.trust_level, COUNT(b.batch_id), "
        "       COALESCE(SUM(b.class_count), 0), COALESCE(SUM(b.entry_count), 0), "
        "       COALESCE(SUM(b.member_count), 0), COALESCE(SUM(b.ref_count), 0), "
        "       COALESCE(MIN(b.started_at), ''), COALESCE(MAX(NULLIF(b.finished_at, '')), '') "
        "FROM producers p LEFT JOIN batches b ON b.producer_id = p.producer_id "
        "GROUP BY p.producer_id ORDER BY p.producer_id");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    std::vector<DbProducerStats> stats;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        DbProducerStats row;
        row.producerId  = sqlite3_column_int64(statement.raw, 0);
        row.key         = utf8Column(statement.raw, 1);
        row.name        = utf8Column(statement.raw, 2);
        row.trustLevel  = sqlite3_column_int(statement.raw, 3) != 0
                              ? DbProducerTrustLevel::Trusted
                              : DbProducerTrustLevel::Untrusted;
        row.batchCount  = sqlite3_column_int64(statement.raw, 4);
        row.classCount  = sqlite3_column_int64(statement.raw, 5);
        row.entryCount  = sqlite3_column_int64(statement.raw, 6);
        row.memberCount = sqlite3_column_int64(statement.raw, 7);
        row.refCount    = sqlite3_column_int64(statement.raw, 8);
        row.firstBatchAt = utf8Column(statement.raw, 9);
        row.lastBatchAt  = utf8Column(statement.raw, 10);
        stats.push_back(std::move(row));
    }
    return ca::Ok(std::move(stats));
}

//-- 分析事实层（类/成员/继承/直接引用） ------------------------------------

ca::Result<ca::usize, AnalysisDbError> AnalysisDb::ingestClasses(
    ca::i64 batchId, std::vector<DbClassInput> classes)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (batchId < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "batch id must not be negative"));
    }
    if (auto result = validateClassInputs(classes); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    if (batchId != 0) {
        if (auto result = validateOpenBatchForWrite(impl_->handle, batchId); result.is_err()) {
            return errorOf(std::move(result));
        }
    }

    std::vector<ca::i64> classIds;
    if (auto result = applyClassBatch(impl_->handle, classes, classIds); result.is_err()) {
        return errorOf(std::move(result));
    }

    if (batchId != 0) {
        // 逐类作用域行（#143 3.6）：full_import 的 classOwnership 可答必需项。
        // 成员登记的归属并入其所在类（成员不落独立作用域行）。
        auto scopeInsert = prepare(impl_->handle, kScopeRowInsertSql);
        if (scopeInsert.is_err()) return errorOf(std::move(scopeInsert));
        auto scopeStatement = std::move(scopeInsert).unwrap();
        for (const ca::i64 classId : classIds) {
            if (auto result = writeScopeRow(impl_->handle, scopeStatement.raw, batchId,
                                            DbBatchScopeKind::Class, classId);
                result.is_err()) {
                return errorOf(std::move(result));
            }
        }
    }

    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(classes.size());
}

ca::Result<ca::usize, AnalysisDbError> AnalysisDb::replaceReferences(
    ca::i64 batchId, std::vector<DbReferenceRecord> references)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (batchId < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "batch id must not be negative"));
    }
    if (auto result = validateReferences(references); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    if (batchId != 0) {
        if (auto result = validateOpenBatchForWrite(impl_->handle, batchId); result.is_err()) {
            return errorOf(std::move(result));
        }
    }

    if (auto result = exec(impl_->handle, "DELETE FROM refs"); result.is_err()) {
        return errorOf(std::move(result));
    }

    if (batchId != 0) {
        // refs_all 锚（scope_id 恒 0）：整库替换覆盖全部引用切片，此前的
        // 成员级归属由解析规则取大后被本批盖过。
        auto scopeInsert = prepare(impl_->handle, kScopeRowInsertSql);
        if (scopeInsert.is_err()) return errorOf(std::move(scopeInsert));
        auto scopeStatement = std::move(scopeInsert).unwrap();
        if (auto result = writeScopeRow(impl_->handle, scopeStatement.raw, batchId,
                                        DbBatchScopeKind::RefsAll, 0);
            result.is_err()) {
            return errorOf(std::move(result));
        }
    }

    if (auto result = applyReferenceBatch(impl_->handle, references); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(references.size());
}

ca::Result<ca::usize, AnalysisDbError> AnalysisDb::replaceReferencesFromMember(
    ca::i64 batchId, ca::i64 sourceMemberId, std::vector<DbReferenceRecord> references)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (batchId < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "batch id must not be negative"));
    }
    if (auto result = validateReferences(references); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    // 切片身份先落定：取成员行（连同宿主类内部名），行内 source 字段必须与
    // 之一致，DELETE 的 member_id 才对准要替换的切片（refs 的来源成员恒为
    // 方法，kind=1 一并锁进查询）。
    auto member = prepare(
        impl_->handle,
        "SELECT c.internal_name, m.name, m.descriptor FROM members m "
        "JOIN classes c ON c.class_id=m.class_id WHERE m.member_id=?1 AND m.kind=1");
    if (member.is_err()) return errorOf(std::move(member));
    auto memberStatement = std::move(member).unwrap();
    if (auto result = bindInt(impl_->handle, memberStatement.raw, 1, sourceMemberId);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasMember = step(impl_->handle, memberStatement.raw);
    if (hasMember.is_err()) return errorOf(std::move(hasMember));
    if (!hasMember.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "reference source member is not a registered method: " +
                                     std::to_string(sourceMemberId)));
    }
    const std::string ownerClass = textColumn(memberStatement.raw, 0);
    const std::string memberName = textColumn(memberStatement.raw, 1);
    const std::string memberDesc = textColumn(memberStatement.raw, 2);
    resetStatement(memberStatement);

    for (const DbReferenceRecord& reference : references) {
        const bool sameMember = reference.sourceClass.ref() == ownerClass &&
                                reference.sourceMethod.ref() == memberName &&
                                reference.sourceDescriptor.ref() == memberDesc;
        if (!sameMember) {
            return ca::Err(makeError(
                AnalysisDbErrorKind::Invalid,
                "reference source member does not match the replaced member: " +
                    reference.sourceClass.to_std_string() + "." +
                    reference.sourceMethod.to_std_string() + "(" +
                    reference.sourceDescriptor.to_std_string() + ")"));
        }
    }

    if (batchId != 0) {
        // 成员切片锚（#143 批 3）：类事实未触碰，不落 class 作用域行——
        // classOwnership 只认类事实 upsert 与 class 头引用切片。
        if (auto result = validateOpenBatchForWrite(impl_->handle, batchId); result.is_err()) {
            return errorOf(std::move(result));
        }
        auto scopeInsert = prepare(impl_->handle, kScopeRowInsertSql);
        if (scopeInsert.is_err()) return errorOf(std::move(scopeInsert));
        auto scopeStatement = std::move(scopeInsert).unwrap();
        if (auto result = writeScopeRow(impl_->handle, scopeStatement.raw, batchId,
                                        DbBatchScopeKind::Member, sourceMemberId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
    }

    auto erase = prepare(impl_->handle, "DELETE FROM refs WHERE source_member_id=?1");
    if (erase.is_err()) return errorOf(std::move(erase));
    auto eraseStatement = std::move(erase).unwrap();
    if (auto result = bindInt(impl_->handle, eraseStatement.raw, 1, sourceMemberId);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    auto erased = step(impl_->handle, eraseStatement.raw);
    if (erased.is_err()) return errorOf(std::move(erased));

    if (auto result = applyReferenceBatch(impl_->handle, references); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(references.size());
}

ca::Result<std::optional<DbClassInfo>, AnalysisDbError> AnalysisDb::findClass(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(
        impl_->handle,
        "SELECT class_id, internal_name, super_name, access, version, entry_id "
        "FROM classes WHERE internal_name=?1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(std::optional<DbClassInfo>());

    DbClassInfo info = readClassRow(statement.raw);
    resetStatement(statement);

    auto interfaces = prepare(
        impl_->handle,
        "SELECT interface_name FROM interfaces WHERE class_id=?1 ORDER BY position");
    if (interfaces.is_err()) return errorOf(std::move(interfaces));
    auto interfaceStatement = std::move(interfaces).unwrap();
    if (auto result = bindInt(impl_->handle, interfaceStatement.raw, 1, info.classId);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    for (;;) {
        auto row = step(impl_->handle, interfaceStatement.raw);
        if (row.is_err()) return errorOf(std::move(row));
        if (!row.unwrap()) break;
        info.interfaces.push_back(utf8Column(interfaceStatement.raw, 0));
    }
    return ca::Ok(std::optional<DbClassInfo>(std::move(info)));
}

ca::Result<std::vector<DbClassInfo>, AnalysisDbError> AnalysisDb::listClasses() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(
        impl_->handle,
        "SELECT c.class_id, c.internal_name, c.super_name, c.access, c.version, c.entry_id, "
        "       i.interface_name "
        "FROM classes c LEFT JOIN interfaces i ON i.class_id=c.class_id "
        "ORDER BY c.internal_name, i.position");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    std::vector<DbClassInfo> classes;
    ca::i64                  lastId = 0;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;

        const ca::i64 classId = sqlite3_column_int64(statement.raw, 0);
        if (classes.empty() || classId != lastId) {
            classes.push_back(readClassRow(statement.raw));
            lastId = classId;
        }
        if (sqlite3_column_type(statement.raw, 6) != SQLITE_NULL) {
            classes.back().interfaces.push_back(utf8Column(statement.raw, 6));
        }
    }
    return ca::Ok(std::move(classes));
}

ca::Result<std::vector<DbMemberInfo>, AnalysisDbError> AnalysisDb::listMembers(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(
        impl_->handle,
        "SELECT m.member_id, m.kind, m.name, m.descriptor, m.access "
        "FROM members m JOIN classes c ON c.class_id=m.class_id "
        "WHERE c.internal_name=?1 "
        "ORDER BY m.kind, m.name, m.descriptor");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<DbMemberInfo> members;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        members.push_back(readMemberRow(statement.raw));
    }
    return ca::Ok(std::move(members));
}

ca::Result<std::optional<DbMemberInfo>, AnalysisDbError> AnalysisDb::findMember(
    DbMemberKind kind, const ca::str::Utf8StringRef& owner,
    const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(
        impl_->handle,
        "SELECT m.member_id, m.kind, m.name, m.descriptor, m.access "
        "FROM members m JOIN classes c ON c.class_id=m.class_id "
        "WHERE c.internal_name=?1 AND m.kind=?2 AND m.name=?3 AND m.descriptor=?4");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(impl_->handle, statement.raw, 2, static_cast<ca::i64>(kind));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(impl_->handle, statement.raw, 3, name); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindText(impl_->handle, statement.raw, 4, desc); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto hasRow = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(std::optional<DbMemberInfo>());
    return ca::Ok(std::optional<DbMemberInfo>(readMemberRow(statement.raw)));
}

ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> AnalysisDb::superChain(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared =
        prepare(impl_->handle, "SELECT super_name FROM classes WHERE internal_name=?1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    std::vector<ca::str::Utf8String> chain;
    std::set<std::string>            seen;
    std::string                      current = owner.to_std_string();
    bool                             first   = true;
    while (!current.empty() && seen.insert(current).second) {
        if (auto result =
                bindText(impl_->handle, statement.raw, 1,
                         ca::str::Utf8StringRef::from_string_view(current));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) {
            resetStatement(statement);
            if (first) return ca::Ok(std::vector<ca::str::Utf8String>());
            // 未登记的父类名来自 classfile 声明，计入链尾后停止（同 ClassHierarchy）。
            chain.push_back(ca::str::Utf8String::from_data_unchecked(
                reinterpret_cast<const ca::u8*>(current.data()), current.size()));
            break;
        }
        chain.push_back(ca::str::Utf8String::from_data_unchecked(
            reinterpret_cast<const ca::u8*>(current.data()), current.size()));
        current = textColumn(statement.raw, 0);
        resetStatement(statement);
        first = false;
    }
    return ca::Ok(std::move(chain));
}

ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> AnalysisDb::directSubclasses(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(
        impl_->handle,
        "SELECT internal_name FROM classes WHERE super_name=?1 ORDER BY internal_name");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<ca::str::Utf8String> names;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        names.push_back(utf8Column(statement.raw, 0));
    }
    return ca::Ok(std::move(names));
}

ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> AnalysisDb::allSubclasses(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(impl_->handle, kAllSubclassesSql);
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<ca::str::Utf8String> names;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        names.push_back(utf8Column(statement.raw, 0));
    }
    return ca::Ok(std::move(names));
}

ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> AnalysisDb::allSubclassesPaged(
    const ca::str::Utf8StringRef& owner, ca::i64 limit, ca::i64 offset) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validatePageWindow(limit, offset); result.is_err()) {
        return errorOf(std::move(result));
    }

    // 窗口在递归 CTE 全量展开、ORDER BY 之后截取（SQLite LIMIT/OFFSET 语义）：
    // 分页只省行对象构造与返回内存，遍历与排序成本不随窗口缩小。
    const std::string sql = std::string(kAllSubclassesSql) + " LIMIT ?2 OFFSET ?3";
    auto prepared = prepare(impl_->handle, sql.c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, owner); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(impl_->handle, statement.raw, 2, limit); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(impl_->handle, statement.raw, 3, offset); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<ca::str::Utf8String> names;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        names.push_back(utf8Column(statement.raw, 0));
    }
    return ca::Ok(std::move(names));
}

ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> AnalysisDb::implementors(
    const ca::str::Utf8StringRef& interfaceName) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(impl_->handle, kImplementorsSql);
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, interfaceName); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<ca::str::Utf8String> names;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        names.push_back(utf8Column(statement.raw, 0));
    }
    return ca::Ok(std::move(names));
}

ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> AnalysisDb::implementorsPaged(
    const ca::str::Utf8StringRef& interfaceName, ca::i64 limit, ca::i64 offset) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validatePageWindow(limit, offset); result.is_err()) {
        return errorOf(std::move(result));
    }

    // 窗口在闭包展开、ORDER BY 之后截取（同 allSubclassesPaged）。
    const std::string sql = std::string(kImplementorsSql) + " LIMIT ?2 OFFSET ?3";
    auto prepared = prepare(impl_->handle, sql.c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindText(impl_->handle, statement.raw, 1, interfaceName); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(impl_->handle, statement.raw, 2, limit); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(impl_->handle, statement.raw, 3, offset); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<ca::str::Utf8String> names;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        names.push_back(utf8Column(statement.raw, 0));
    }
    return ca::Ok(std::move(names));
}

/// @brief 目标成员引用查询（referencesToMethod / referencesToField 共用）：
///        先解析 id 再索引下探（同 referencesFrom* 的两步走）。id 分支
///        refs_target_id 双列精确探（owner 与成员共登记的引用行），文本分支
///        refs_target_text 四元组精确探（写入时未登记的存量行与始终未登记
///        的外部符号）；id 解析未命中（0）时对应分支自然为空，文本分支兜底。
template <DbReferenceKind kRefKind>
ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> referencesToMemberTarget(
    sqlite3* db, const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor, ca::i64 pageLimit, ca::i64 pageOffset)
{
    constexpr bool kIsMethod = kRefKind == DbReferenceKind::Method;
    auto classId = classIdByName(db, owner);
    if (classId.is_err()) return errorOf(std::move(classId));
    auto memberId = memberIdByName(db, kIsMethod ? DbMemberKind::Method : DbMemberKind::Field,
                                   owner, name, descriptor);
    if (memberId.is_err()) return errorOf(std::move(memberId));

    const std::string sql = makeTargetReferenceSql(
        kIsMethod ? "WHERE r.ref_kind=2 AND r.target_class_id=?1 AND r.target_member_id=?2 "
                  : "WHERE r.ref_kind=1 AND r.target_class_id=?1 AND r.target_member_id=?2 ",
        kIsMethod ? "WHERE r.ref_kind=2 AND r.target_class_id IS NULL AND r.target_class=?3 "
                    "AND r.target_name=?4 AND r.target_desc=?5 "
                  : "WHERE r.ref_kind=1 AND r.target_class_id IS NULL AND r.target_class=?3 "
                    "AND r.target_name=?4 AND r.target_desc=?5 ");
    return runReferenceQuery(
        db, sql, 5, pageLimit, pageOffset,
        [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
            if (auto result = bindInt(db, statement, 1, classId.unwrap()); result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindInt(db, statement, 2, memberId.unwrap()); result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(db, statement, 3, owner); result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindText(db, statement, 4, name); result.is_err()) {
                return errorOf(std::move(result));
            }
            return bindText(db, statement, 5, descriptor);
        });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesToMethod(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    return referencesToMemberTarget<DbReferenceKind::Method>(impl_->handle, owner, name,
                                                             descriptor, -1, -1);
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesToMethodPaged(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor, ca::i64 limit, ca::i64 offset) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validatePageWindow(limit, offset); result.is_err()) {
        return errorOf(std::move(result));
    }
    return referencesToMemberTarget<DbReferenceKind::Method>(impl_->handle, owner, name,
                                                             descriptor, limit, offset);
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesToField(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    return referencesToMemberTarget<DbReferenceKind::Field>(impl_->handle, owner, name,
                                                            descriptor, -1, -1);
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesToFieldPaged(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor, ca::i64 limit, ca::i64 offset) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validatePageWindow(limit, offset); result.is_err()) {
        return errorOf(std::move(result));
    }
    return referencesToMemberTarget<DbReferenceKind::Field>(impl_->handle, owner, name,
                                                            descriptor, limit, offset);
}

/// @brief 目标类引用查询：id 分支 refs_target_id 前缀探（owner 登记的全部
///        type/field/method 引用行），文本分支 refs_target_text——文本分支
///        承接始终未登记 owner（包括 owner 其后才登记的存量行，此时行仍是
///        文本形状，身份只增不删不改既有编码）。
ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesToClass(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    auto classId = classIdByName(impl_->handle, owner);
    if (classId.is_err()) return errorOf(std::move(classId));
    const std::string sql =
        makeTargetReferenceSql("WHERE r.target_class_id=?1 ",
                               "WHERE r.target_class_id IS NULL AND r.target_class=?2 ");
    return runReferenceQuery(
        impl_->handle, sql, 2, -1, -1,
        [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
            if (auto result = bindInt(impl_->handle, statement, 1, classId.unwrap());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            return bindText(impl_->handle, statement, 2, owner);
        });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesToClassPaged(
    const ca::str::Utf8StringRef& owner, ca::i64 limit, ca::i64 offset) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validatePageWindow(limit, offset); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto classId = classIdByName(impl_->handle, owner);
    if (classId.is_err()) return errorOf(std::move(classId));
    const std::string sql =
        makeTargetReferenceSql("WHERE r.target_class_id=?1 ",
                               "WHERE r.target_class_id IS NULL AND r.target_class=?2 ");
    return runReferenceQuery(
        impl_->handle, sql, 2, limit, offset,
        [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
            if (auto result = bindInt(impl_->handle, statement, 1, classId.unwrap());
                result.is_err()) {
                return errorOf(std::move(result));
            }
            return bindText(impl_->handle, statement, 2, owner);
        });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesFromMember(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    // 两步走：先解析 member_id 再下探 refs_source_member 索引（JOIN 形状
    // 会让规划器全索引扫 refs，见 classIdByName 注）。
    auto memberId = memberIdByName(impl_->handle, DbMemberKind::Method, owner, name, descriptor);
    if (memberId.is_err()) return errorOf(std::move(memberId));
    if (memberId.unwrap() == 0) return ca::Ok(std::vector<DbReferenceRecord>());
    const std::string sql = std::string(kReferenceSelectSql) +
                            "WHERE r.source_member_id=?1 " + kReferenceOrderSql;
    return runReferenceQuery(impl_->handle, sql, 1, -1, -1,
                             [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
                                 return bindInt(impl_->handle, statement, 1,
                                                memberId.unwrap());
                             });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesFromMemberPaged(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor, ca::i64 limit, ca::i64 offset) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validatePageWindow(limit, offset); result.is_err()) {
        return errorOf(std::move(result));
    }
    // 两步走（同 referencesFromMember）：先解析 member_id 再下探索引。
    auto memberId = memberIdByName(impl_->handle, DbMemberKind::Method, owner, name, descriptor);
    if (memberId.is_err()) return errorOf(std::move(memberId));
    if (memberId.unwrap() == 0) return ca::Ok(std::vector<DbReferenceRecord>());
    const std::string sql = std::string(kReferenceSelectSql) +
                            "WHERE r.source_member_id=?1 " + kReferenceOrderSql;
    return runReferenceQuery(impl_->handle, sql, 1, limit, offset,
                             [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
                                 return bindInt(impl_->handle, statement, 1,
                                                memberId.unwrap());
                             });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesFromClass(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    auto classId = classIdByName(impl_->handle, owner);
    if (classId.is_err()) return errorOf(std::move(classId));
    if (classId.unwrap() == 0) return ca::Ok(std::vector<DbReferenceRecord>());
    const std::string sql = std::string(kReferenceSelectSql) +
                            "WHERE r.source_class_id=?1 " + kReferenceOrderSql;
    return runReferenceQuery(impl_->handle, sql, 1, -1, -1,
                             [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
                                 return bindInt(impl_->handle, statement, 1,
                                                classId.unwrap());
                             });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesFromClassPaged(
    const ca::str::Utf8StringRef& owner, ca::i64 limit, ca::i64 offset) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validatePageWindow(limit, offset); result.is_err()) {
        return errorOf(std::move(result));
    }
    // 两步走（同 referencesFromClass）：先解析 class_id 再下探索引。
    auto classId = classIdByName(impl_->handle, owner);
    if (classId.is_err()) return errorOf(std::move(classId));
    if (classId.unwrap() == 0) return ca::Ok(std::vector<DbReferenceRecord>());
    const std::string sql = std::string(kReferenceSelectSql) +
                            "WHERE r.source_class_id=?1 " + kReferenceOrderSql;
    return runReferenceQuery(impl_->handle, sql, 1, limit, offset,
                             [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
                                 return bindInt(impl_->handle, statement, 1,
                                                classId.unwrap());
                             });
}

ca::Result<ca::usize, AnalysisDbError> AnalysisDb::referenceCount() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    auto count = queryScalarInt(impl_->handle, "SELECT COUNT(*) FROM refs");
    if (count.is_err()) return errorOf(std::move(count));
    return ca::Ok(static_cast<ca::usize>(count.unwrap()));
}

//-- 方法版本指纹层（method_versions，schema v8，#132）--------------------------

ca::Result<ca::i64, AnalysisDbError> AnalysisDb::recordMethodVersion(
    ca::i64 classId, ca::i64 memberId, const analysis::SemanticFp& fingerprint)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (classId <= 0 || memberId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "class id and member id must be positive"));
    }
    if (auto result = validateMethodFingerprint(fingerprint); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    auto recorded = recordOneMethodVersion(impl_->handle, classId, memberId, fingerprint);
    if (recorded.is_err()) return errorOf(std::move(recorded));

    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(recorded.unwrap().currentId);
}

ca::Result<std::optional<analysis::SemanticFp>, AnalysisDbError>
AnalysisDb::currentMethodFingerprint(ca::i64 memberId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (memberId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "member id must be positive"));
    }
    // 成员存在性先行：未登记成员与字段是调用方契约错误（与 listEntries /
    // listBatches 对未知 id 同口径，不静默返回空）；从未落版本才是合法空态。
    auto exists = methodMemberExists(impl_->handle, memberId);
    if (exists.is_err()) return errorOf(std::move(exists));
    if (!exists.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "method fingerprint target is not a registered method: " +
                                     std::to_string(memberId)));
    }

    auto prepared = prepare(impl_->handle,
                            "SELECT fingerprint FROM method_versions "
                            "WHERE member_id=?1 AND is_current=1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(impl_->handle, statement.raw, 1, memberId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(std::optional<analysis::SemanticFp>());
    return ca::Ok(std::optional<analysis::SemanticFp>(
        fingerprintFromColumn(statement.raw, 0)));
}

ca::Result<std::vector<DbMethodVersionInfo>, AnalysisDbError> AnalysisDb::listMethodVersions(
    ca::i64 memberId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (memberId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "member id must be positive"));
    }
    auto exists = methodMemberExists(impl_->handle, memberId);
    if (exists.is_err()) return errorOf(std::move(exists));
    if (!exists.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "method version target is not a registered method: " +
                                     std::to_string(memberId)));
    }

    auto prepared = prepare(impl_->handle,
                            "SELECT method_version_id, class_id, member_id, fingerprint, "
                            "fp_version, is_current, recorded_at "
                            "FROM method_versions WHERE member_id=?1 "
                            "ORDER BY method_version_id");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(impl_->handle, statement.raw, 1, memberId); result.is_err()) {
        return errorOf(std::move(result));
    }

    std::vector<DbMethodVersionInfo> versions;
    for (;;) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        versions.push_back(readMethodVersionRow(statement.raw));
    }
    return ca::Ok(std::move(versions));
}

ca::Result<DbMethodVersionRecordStats, AnalysisDbError> AnalysisDb::recordMethodVersions(
    std::vector<DbMethodVersionInput> inputs)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (inputs.empty()) return ca::Ok(DbMethodVersionRecordStats());

    // 输入先整体校验（纯输入面）：id 为正、指纹非全零、批内成员不重复
    // ——一次编辑只落一个终态版本，重复成员是调用方契约错误。
    std::set<ca::i64> members;
    for (const auto& input : inputs) {
        if (input.classId <= 0 || input.memberId <= 0) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "class id and member id must be positive"));
        }
        if (auto result = validateMethodFingerprint(input.fingerprint); result.is_err()) {
            return errorOf(std::move(result));
        }
        if (!members.insert(input.memberId).second) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "duplicate member in method version batch: " +
                                         std::to_string(input.memberId)));
        }
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    DbMethodVersionRecordStats stats;
    for (const auto& input : inputs) {
        auto recorded = recordOneMethodVersion(impl_->handle, input.classId,
                                               input.memberId, input.fingerprint);
        if (recorded.is_err()) return errorOf(std::move(recorded));
        if (recorded.unwrap().inserted) {
            ++stats.recorded;
        }
        else {
            ++stats.unchanged;
        }
    }

    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(stats);
}

//-- 全程序可达闭包（closure_batches / closure_members，schema v9，#159）----------

constexpr const char* kClosureBatchSelectSql =
    "SELECT cb.batch_id, cb.graph_fingerprint, cb.fp_version, cb.seed_mask, cb.seed_count, "
    "cb.coverage_mask, cb.indy_edge_count, cb.member_count, cb.computed_at "
    "FROM closure_batches cb";

/// @brief 读一行闭包批次事实（三处查询面共用；列序 = kClosureBatchSelectSql）。
DbClosureBatchInfo readClosureBatchRow(sqlite3_stmt* statement)
{
    DbClosureBatchInfo row;
    row.batchId = sqlite3_column_int64(statement, 0);
    const auto* fingerprint =
        static_cast<const ca::u8*>(sqlite3_column_blob(statement, 1));
    const int fingerprintBytes = sqlite3_column_bytes(statement, 1);
    if (fingerprint != nullptr && fingerprintBytes > 0) {
        row.fingerprint.assign(fingerprint, fingerprint + fingerprintBytes);
    }
    row.fpVersion     = static_cast<ca::u32>(sqlite3_column_int64(statement, 2));
    row.seedMask      = static_cast<ca::u32>(sqlite3_column_int64(statement, 3));
    row.seedCount     = sqlite3_column_int64(statement, 4);
    row.coverageMask  = static_cast<ca::u32>(sqlite3_column_int64(statement, 5));
    row.indyEdgeCount = sqlite3_column_int64(statement, 6);
    row.memberCount   = sqlite3_column_int64(statement, 7);
    row.computedAt    = utf8Column(statement, 8);
    return row;
}

/// @brief 闭包批凭证探取：closure_batches 主键行即「是闭包批」的凭证
///        （storeClosureBatch 是唯一写入口）。不存在 → `Invalid`，查询面
///        与 listEntries / listBatches 对未知 id 同口径。
ca::Result<void, AnalysisDbError> requireClosureBatch(sqlite3* db, ca::i64 batchId)
{
    auto prepared = prepare(db, "SELECT 1 FROM closure_batches WHERE batch_id=?1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(db, statement.raw, 1, batchId); result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "closure batch does not exist: " + std::to_string(batchId)));
    }
    return ca::Ok();
}

/// @brief 闭包批存在性校验 + 分页：member_id 升序 OFFSET 窗口（闭包行键唯一、
///        排序确定，分页契约与引用分页一致）。
ca::Result<std::vector<ca::i64>, AnalysisDbError> readClosureMembersPaged(
    sqlite3* db, ca::i64 batchId, ca::i64 limit, ca::i64 offset)
{
    if (auto checked = requireClosureBatch(db, batchId); checked.is_err()) {
        return errorOf(std::move(checked));
    }
    auto prepared = prepare(db, "SELECT member_id FROM closure_members WHERE batch_id=?1 "
                                "ORDER BY member_id LIMIT ?2 OFFSET ?3");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(db, statement.raw, 1, batchId); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement.raw, 2, limit); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(db, statement.raw, 3, offset); result.is_err()) {
        return errorOf(std::move(result));
    }
    std::vector<ca::i64> members;
    while (true) {
        auto hasRow = step(db, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        members.push_back(sqlite3_column_int64(statement.raw, 0));
    }
    return ca::Ok(std::move(members));
}

/// @brief 载荷定宽编码：u32/i64 小端逐字节追加（canonical 序列化只需进程内
///        确定性——载荷不持久化，库里只存哈希）。
void appendU32Le(std::vector<ca::u8>& out, ca::u32 value)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<ca::u8>((value >> (8 * i)) & 0xFFu));
    }
}

void appendI64Le(std::vector<ca::u8>& out, ca::i64 value)
{
    const auto bits = static_cast<ca::u64>(value);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<ca::u8>((bits >> (8 * i)) & 0xFFu));
    }
}

/// @brief 追加一张表的变更敏感聚合三元组（COUNT/MAX/SUM，空表 COALESCE 0）。
ca::Result<void, AnalysisDbError> appendAggregateTriple(std::vector<ca::u8>& out,
                                                        sqlite3*             db,
                                                        const char*          sql)
{
    auto prepared = prepare(db, sql);
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    auto hasRow    = step(db, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                 std::string("aggregate query returned no row: ") + sql));
    }
    for (int column = 0; column < 3; ++column) {
        appendI64Le(out, sqlite3_column_int64(statement.raw, column));
    }
    return ca::Ok();
}

ca::Result<DbClosureBatchInfo, AnalysisDbError> AnalysisDb::storeClosureBatch(
    const DbClosureBatchRecord& record)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (record.producerId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "producer id must be positive"));
    }
    if (record.fingerprint.size() != 32) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "closure graph fingerprint must be 32 bytes"));
    }
    if (record.fpVersion < 1) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "closure fp version must be positive"));
    }
    if (record.seedCount < 0 || record.indyEdgeCount < 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "closure counts must not be negative"));
    }
    for (ca::usize i = 0; i < record.memberIds.size(); ++i) {
        if (record.memberIds[i] <= 0) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "closure member ids must be positive"));
        }
        if (i > 0 && record.memberIds[i] <= record.memberIds[i - 1]) {
            return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                     "closure member ids must be sorted and unique"));
        }
    }

    // 开批（closure_write）：openBatch 承担注册校验与批次行落库。事实段
    // 中途失败不回滚批次行——残批是合法审计状态（设计稿 §6）。
    auto opened = openBatch(record.producerId, DbBatchKind::ClosureWrite);
    if (opened.is_err()) return errorOf(std::move(opened));
    const ca::i64 batchId = opened.unwrap();

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    {
        auto inserted = prepare(impl_->handle,
                                "INSERT INTO closure_batches(batch_id, graph_fingerprint, "
                                "fp_version, seed_mask, seed_count, coverage_mask, "
                                "indy_edge_count, member_count, computed_at) "
                                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
        if (inserted.is_err()) return errorOf(std::move(inserted));
        auto statement = std::move(inserted).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, batchId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindBlob(impl_->handle, statement.raw, 2,
                                   record.fingerprint.data(),
                                   static_cast<int>(record.fingerprint.size()));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 3,
                                  static_cast<ca::i64>(record.fpVersion));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 4,
                                  static_cast<ca::i64>(record.seedMask));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 5, record.seedCount);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 6,
                                  static_cast<ca::i64>(record.coverageMask));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 7, record.indyEdgeCount);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindInt(impl_->handle, statement.raw, 8,
                                  static_cast<ca::i64>(record.memberIds.size()));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result =
                bindText(impl_->handle, statement.raw, 9,
                         ca::str::Utf8StringRef::from_string_view(nowIso8601()));
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto done = step(impl_->handle, statement.raw);
        if (done.is_err()) return errorOf(std::move(done));
    }
    {
        // 成员集批量插入（预编译复用；未登记成员由 FK 在此拒绝，事务回滚留残批）。
        auto inserted =
            prepare(impl_->handle, "INSERT INTO closure_members(batch_id, member_id) VALUES(?1, ?2)");
        if (inserted.is_err()) return errorOf(std::move(inserted));
        auto statement = std::move(inserted).unwrap();
        for (const ca::i64 memberId : record.memberIds) {
            if (auto result = bindInt(impl_->handle, statement.raw, 1, batchId);
                result.is_err()) {
                return errorOf(std::move(result));
            }
            if (auto result = bindInt(impl_->handle, statement.raw, 2, memberId);
                result.is_err()) {
                return errorOf(std::move(result));
            }
            auto done = step(impl_->handle, statement.raw);
            if (done.is_err()) return errorOf(std::move(done));
            resetStatement(statement);
        }
    }
    {
        // 类域作用域行：从成员集经 members.class_id 派生（批内去重由
        // batch_scopes 主键承担），不经输入携带、不可能与成员集漂移。
        auto inserted = prepare(impl_->handle,
                                "INSERT INTO batch_scopes(batch_id, scope_kind, scope_id) "
                                "SELECT ?1, 2, m.class_id FROM closure_members cm "
                                "JOIN members m ON m.member_id = cm.member_id "
                                "WHERE cm.batch_id = ?1 GROUP BY m.class_id");
        if (inserted.is_err()) return errorOf(std::move(inserted));
        auto statement = std::move(inserted).unwrap();
        if (auto result = bindInt(impl_->handle, statement.raw, 1, batchId);
            result.is_err()) {
            return errorOf(std::move(result));
        }
        auto done = step(impl_->handle, statement.raw);
        if (done.is_err()) return errorOf(std::move(done));
    }

    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }

    DbBatchCounts counts;
    counts.memberCount = static_cast<ca::i64>(record.memberIds.size());
    if (auto result = closeBatch(batchId, counts); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto prepared = prepare(
        impl_->handle,
        (std::string(kClosureBatchSelectSql) + " WHERE cb.batch_id=?1").c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(impl_->handle, statement.raw, 1, batchId);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Schema,
                                 "closure batch row vanished after insert: " +
                                     std::to_string(batchId)));
    }
    return ca::Ok(readClosureBatchRow(statement.raw));
}

ca::Result<std::vector<DbClosureBatchInfo>, AnalysisDbError> AnalysisDb::listClosureBatches() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    auto prepared = prepare(impl_->handle,
                            (std::string(kClosureBatchSelectSql) + " ORDER BY cb.batch_id")
                                .c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    std::vector<DbClosureBatchInfo> batches;
    while (true) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;
        batches.push_back(readClosureBatchRow(statement.raw));
    }
    return ca::Ok(std::move(batches));
}

ca::Result<std::optional<DbClosureBatchInfo>, AnalysisDbError> AnalysisDb::freshClosureBatch(
    const std::vector<ca::u8>& fingerprint, ca::u32 seedMask) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (fingerprint.size() != 32) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "closure graph fingerprint must be 32 bytes"));
    }
    auto prepared = prepare(impl_->handle,
                            (std::string(kClosureBatchSelectSql) +
                             " JOIN batches b ON b.batch_id = cb.batch_id "
                             "WHERE cb.graph_fingerprint=?1 AND cb.seed_mask=?2 "
                             "AND b.finished_at != '' "
                             "ORDER BY cb.batch_id DESC LIMIT 1")
                                .c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindBlob(impl_->handle, statement.raw, 1, fingerprint.data(),
                               static_cast<int>(fingerprint.size()));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(impl_->handle, statement.raw, 2,
                              static_cast<ca::i64>(seedMask));
        result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    if (!hasRow.unwrap()) return ca::Ok(std::optional<DbClosureBatchInfo>());
    return ca::Ok(std::optional<DbClosureBatchInfo>(readClosureBatchRow(statement.raw)));
}

ca::Result<std::vector<ca::i64>, AnalysisDbError> AnalysisDb::closureMembersPaged(
    ca::i64 batchId, ca::i64 limit, ca::i64 offset) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (auto result = validatePageWindow(limit, offset); result.is_err()) {
        return errorOf(std::move(result));
    }
    return readClosureMembersPaged(impl_->handle, batchId, limit, offset);
}

ca::Result<bool, AnalysisDbError> AnalysisDb::isClosureMember(ca::i64 batchId,
                                                              ca::i64 memberId) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (memberId <= 0) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid,
                                 "member id must be positive"));
    }
    if (auto checked = requireClosureBatch(impl_->handle, batchId); checked.is_err()) {
        return errorOf(std::move(checked));
    }
    auto prepared = prepare(impl_->handle,
                            "SELECT 1 FROM closure_members WHERE batch_id=?1 AND member_id=?2 "
                            "LIMIT 1");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (auto result = bindInt(impl_->handle, statement.raw, 1, batchId);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = bindInt(impl_->handle, statement.raw, 2, memberId);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    auto hasRow = step(impl_->handle, statement.raw);
    if (hasRow.is_err()) return errorOf(std::move(hasRow));
    return ca::Ok(hasRow.unwrap());
}

ca::Result<DbClosureGraphEdges, AnalysisDbError> AnalysisDb::closureGraphEdges() const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    // 全量快照单查询（PoC M2 裁决：批量装载胜过 per-member 反查一个数量级）。
    // 只收方法体行（source_member_id 非空）——class 级引用（类头声明）不触发
    // JVM 初始化；文本分支行经 classes.internal_name 探取已登记 owner 类，
    // 未登记目标不入闭包域（设计稿 §8）。行序 ref_id 升序（确定性）。
    auto prepared = prepare(
        impl_->handle,
        "SELECT r.source_member_id, r.ref_kind, "
        "COALESCE(r.target_member_id, 0), COALESCE(r.target_class_id, 0), "
        "COALESCE((SELECT c.class_id FROM classes c "
        "          WHERE c.internal_name = r.target_class), 0) "
        "FROM refs r "
        "WHERE r.source_member_id IS NOT NULL "
        "AND (r.target_member_id IS NOT NULL OR r.target_class_id IS NOT NULL "
        "     OR EXISTS (SELECT 1 FROM classes c WHERE c.internal_name = r.target_class)) "
        "ORDER BY r.ref_id");
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();

    DbClosureGraphEdges edges;
    while (true) {
        auto hasRow = step(impl_->handle, statement.raw);
        if (hasRow.is_err()) return errorOf(std::move(hasRow));
        if (!hasRow.unwrap()) break;

        const ca::i64 source  = sqlite3_column_int64(statement.raw, 0);
        const ca::i64 refKind = sqlite3_column_int64(statement.raw, 1);
        const ca::i64 target  = sqlite3_column_int64(statement.raw, 2);
        const ca::i64 idTrigger   = sqlite3_column_int64(statement.raw, 3);
        const ca::i64 textTrigger = sqlite3_column_int64(statement.raw, 4);

        if (refKind == static_cast<ca::i64>(DbReferenceKind::Method) && target > 0) {
            edges.methodEdges.emplace_back(source, target);
        }
        if (idTrigger > 0) edges.classTriggers.emplace_back(source, idTrigger);
        if (textTrigger > 0) edges.textBranchClassTriggers.emplace_back(source, textTrigger);
    }
    return ca::Ok(std::move(edges));
}

ca::Result<std::vector<ca::u8>, AnalysisDbError> AnalysisDb::closureFingerprintPayload(
    ca::u32 fpVersion) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    std::vector<ca::u8> payload;
    appendU32Le(payload, fpVersion);

    {
        // inputs 投影（input_id 升序）：(input_id, size, path 长度前缀 + 字节，
        // sha256 ASCII)。path 长度前缀消除分隔符歧义（canonical 序列化）。
        auto inputCount = queryScalarInt(impl_->handle, "SELECT COUNT(*) FROM inputs");
        if (inputCount.is_err()) return errorOf(std::move(inputCount));
        appendI64Le(payload, inputCount.unwrap());

        auto prepared = prepare(impl_->handle,
                                "SELECT input_id, size, path, sha256 FROM inputs "
                                "ORDER BY input_id");
        if (prepared.is_err()) return errorOf(std::move(prepared));
        auto statement = std::move(prepared).unwrap();
        while (true) {
            auto hasRow = step(impl_->handle, statement.raw);
            if (hasRow.is_err()) return errorOf(std::move(hasRow));
            if (!hasRow.unwrap()) break;

            appendI64Le(payload, sqlite3_column_int64(statement.raw, 0));
            appendI64Le(payload, sqlite3_column_int64(statement.raw, 1));
            const std::string path = textColumn(statement.raw, 2);
            appendU32Le(payload, static_cast<ca::u32>(path.size()));
            payload.insert(payload.end(), path.begin(), path.end());
            const std::string sha256 = textColumn(statement.raw, 3);
            payload.insert(payload.end(), sha256.begin(), sha256.end());
        }
    }

    // 四表变更敏感聚合：refs 聚合含 SUM(ref_id)——ref_id 单调不复用，任何
    // 整库/切片替换必改聚合（设计稿 §5 的 fail-safe 失效方向）。
    if (auto result = appendAggregateTriple(
            payload, impl_->handle,
            "SELECT COUNT(*), COALESCE(MAX(class_id),0), COALESCE(SUM(class_id),0) "
            "FROM classes");
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = appendAggregateTriple(
            payload, impl_->handle,
            "SELECT COUNT(*), COALESCE(MAX(class_id),0), COALESCE(SUM(class_id),0) "
            "FROM interfaces");
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = appendAggregateTriple(
            payload, impl_->handle,
            "SELECT COUNT(*), COALESCE(MAX(member_id),0), COALESCE(SUM(member_id),0) "
            "FROM members");
        result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = appendAggregateTriple(
            payload, impl_->handle,
            "SELECT COUNT(*), COALESCE(MAX(ref_id),0), COALESCE(SUM(ref_id),0) FROM refs");
        result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(std::move(payload));
}

}   // namespace mjbase::project
