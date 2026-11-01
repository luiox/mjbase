#include "mjbase/project/analysis_db.hpp"

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

ca::Result<void, AnalysisDbError> bindInt(sqlite3* db, sqlite3_stmt* statement, int index,
                                          ca::i64 value)
{
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
        return ca::Err(sqliteFailure(db, "bind integer failed"));
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
    return ca::Ok(sqlite3_column_int64(statement.raw, 0));
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

ca::Result<void, AnalysisDbError> validateMemberKind(DbMemberKind kind)
{
    if (kind != DbMemberKind::Field && kind != DbMemberKind::Method) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "member kind is invalid"));
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
    return ca::Ok(sqlite3_column_int64(statement.raw, 0));
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
    return ca::Ok(sqlite3_column_int64(statement.raw, 0));
}

ca::Result<void, AnalysisDbError> upsertRename(sqlite3* db, ca::i64 targetId,
                                                RenameTargetKind targetKind,
                                                const ca::str::Utf8StringRef& shadow,
                                                const ca::str::Utf8StringRef& token,
                                                const std::string&            updatedAt)
{
    auto prepared = prepare(
        db,
        "INSERT INTO renames(target_kind, target_id, new_name, token, updated_at) "
        "VALUES(?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(target_kind, target_id) DO UPDATE SET "
        "new_name=excluded.new_name, token=excluded.token, updated_at=excluded.updated_at");
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
    "       c.internal_name, m.kind, m.name, m.descriptor, mc.internal_name "
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
        "INSERT INTO renames(target_kind, target_id, new_name, token, updated_at) "
        "VALUES(?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(target_kind, target_id) DO UPDATE SET "
        "new_name=excluded.new_name, token=excluded.token, updated_at=excluded.updated_at");
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
/// 热路径，不要退回逐条 prepare。
ca::Result<void, AnalysisDbError> applyRenameBatch(
    sqlite3* db, const std::vector<DbRenameRecord>& records)
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
        auto upserted = step(db, batch.upsertRename.raw);
        resetStatement(batch.upsertRename);
        if (upserted.is_err()) return errorOf(std::move(upserted));
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> bindNull(sqlite3* db, sqlite3_stmt* statement, int index)
{
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
        return ca::Err(sqliteFailure(db, "bind null failed"));
    }
    return ca::Ok();
}

ca::Result<void, AnalysisDbError> applyClassBatch(sqlite3* db,
                                                  const std::vector<DbClassInput>& classes)
{
    auto upsertClass = prepare(
        db,
        "INSERT INTO classes(internal_name, super_name, access, version) "
        "VALUES(?1, ?2, ?3, ?4) "
        "ON CONFLICT(internal_name) DO UPDATE SET "
        "super_name=excluded.super_name, access=excluded.access, version=excluded.version");
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

ca::Result<void, AnalysisDbError> applyReferenceBatch(
    sqlite3* db, const std::vector<DbReferenceRecord>& references)
{
    auto insert = prepare(
        db,
        "INSERT INTO refs(source_class_id, source_member_id, insn_index, ref_kind, "
        "                 target_class, target_name, target_desc) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    if (insert.is_err()) return errorOf(std::move(insert));
    auto insertStatement = std::move(insert).unwrap();

    auto selectClass = prepare(db, "SELECT class_id FROM classes WHERE internal_name=?1");
    if (selectClass.is_err()) return errorOf(std::move(selectClass));
    auto selectClassStatement = std::move(selectClass).unwrap();

    auto selectMember = prepare(
        db, "SELECT member_id FROM members WHERE class_id=?1 AND kind=1 AND name=?2 AND descriptor=?3");
    if (selectMember.is_err()) return errorOf(std::move(selectMember));
    auto selectMemberStatement = std::move(selectMember).unwrap();

    std::unordered_map<std::string_view, ca::i64> classIds;
    std::map<std::tuple<ca::i64, std::string_view, std::string_view>, ca::i64> memberIds;

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
        if (auto result = bindText(db, insertStatement.raw, 5, reference.targetClass.ref());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(db, insertStatement.raw, 6, reference.targetName.ref());
            result.is_err()) {
            return errorOf(std::move(result));
        }
        if (auto result = bindText(db, insertStatement.raw, 7, reference.targetDescriptor.ref());
            result.is_err()) {
            return errorOf(std::move(result));
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

constexpr const char* kReferenceSelectSql =
    "SELECT c.internal_name, COALESCE(m.name, ''), COALESCE(m.descriptor, ''), "
    "       r.insn_index, r.ref_kind, r.target_class, r.target_name, r.target_desc "
    "FROM refs r "
    "JOIN classes c ON c.class_id=r.source_class_id "
    "LEFT JOIN members m ON m.member_id=r.source_member_id ";

constexpr const char* kReferenceOrderSql =
    "ORDER BY r.source_class_id, r.source_member_id, r.insn_index, r.ref_id";

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> runReferenceQuery(
    sqlite3* db, const std::string& sql, int bindCount,
    const std::function<ca::Result<void, AnalysisDbError>(sqlite3_stmt*)>& bind)
{
    auto prepared = prepare(db, sql.c_str());
    if (prepared.is_err()) return errorOf(std::move(prepared));
    auto statement = std::move(prepared).unwrap();
    if (bindCount > 0) {
        if (auto result = bind(statement.raw); result.is_err()) {
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

constexpr const char* kSchemaDdl[] = {
    "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID",
    "CREATE TABLE classes("
    "  class_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  internal_name TEXT NOT NULL UNIQUE,"
    "  super_name TEXT NOT NULL DEFAULT '',"
    "  access INTEGER NOT NULL DEFAULT 0,"
    "  version INTEGER NOT NULL DEFAULT 0"
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
    "CREATE TABLE refs("
    "  ref_id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  source_class_id INTEGER NOT NULL REFERENCES classes(class_id) ON DELETE CASCADE,"
    "  source_member_id INTEGER REFERENCES members(member_id) ON DELETE CASCADE,"
    "  insn_index INTEGER NOT NULL DEFAULT -1,"
    "  ref_kind INTEGER NOT NULL CHECK(ref_kind IN (0,1,2)),"
    "  target_class TEXT NOT NULL,"
    "  target_name TEXT NOT NULL DEFAULT '',"
    "  target_desc TEXT NOT NULL DEFAULT ''"
    ")",
    "CREATE INDEX refs_target ON refs(ref_kind, target_class, target_name, target_desc)",
    "CREATE INDEX refs_source_member ON refs(source_member_id)",
    "CREATE TABLE renames("
    "  target_kind INTEGER NOT NULL CHECK(target_kind IN (0,1)),"
    "  target_id INTEGER NOT NULL,"
    "  new_name TEXT NOT NULL,"
    "  token TEXT,"
    "  updated_at TEXT NOT NULL,"
    "  PRIMARY KEY(target_kind, target_id)"
    ") WITHOUT ROWID",
    "CREATE UNIQUE INDEX renames_token ON renames(token) WHERE token IS NOT NULL",
    "CREATE INDEX renames_new_name ON renames(new_name)",
};

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
        if (auto result = exec(handle, "PRAGMA synchronous=FULL"); result.is_err()) {
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

ca::Result<void, AnalysisDbError> AnalysisDb::setClassRename(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& shadow,
    const ca::str::Utf8StringRef& token)
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

    auto classId = ensureClass(impl_->handle, owner);
    if (classId.is_err()) return errorOf(std::move(classId));

    const std::string updatedAt = nowIso8601();
    if (auto result =
            upsertRename(impl_->handle, classId.unwrap(), RenameTargetKind::Class, shadow, token,
                         updatedAt);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    return txn.commit();
}

ca::Result<void, AnalysisDbError> AnalysisDb::setMemberRename(
    DbMemberKind kind, const ca::str::Utf8StringRef& owner,
    const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc,
    const ca::str::Utf8StringRef& shadow, const ca::str::Utf8StringRef& token)
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

    auto classId = ensureClass(impl_->handle, owner);
    if (classId.is_err()) return errorOf(std::move(classId));
    auto memberId = ensureMember(impl_->handle, classId.unwrap(), kind, name, desc);
    if (memberId.is_err()) return errorOf(std::move(memberId));

    const std::string updatedAt = nowIso8601();
    if (auto result = upsertRename(impl_->handle, memberId.unwrap(), RenameTargetKind::Member,
                                   shadow, token, updatedAt);
        result.is_err()) {
        return errorOf(std::move(result));
    }
    return txn.commit();
}

ca::Result<void, AnalysisDbError> AnalysisDb::setRenames(std::vector<DbRenameRecord> records)
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

    if (auto result = applyRenameBatch(impl_->handle, records); result.is_err()) {
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
        "SELECT r.new_name, r.token, r.updated_at, c.internal_name "
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
    record.target    = RenameTargetKind::Class;
    record.shadow    = utf8Column(statement.raw, 0);
    record.token     = utf8Column(statement.raw, 1);
    record.updatedAt = utf8Column(statement.raw, 2);
    record.owner     = utf8Column(statement.raw, 3);
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
        "m.descriptor "
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
    std::vector<DbRenameRecord> records)
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

    if (auto result = exec(impl_->handle, "DELETE FROM renames"); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = applyRenameBatch(impl_->handle, records); result.is_err()) {
        return errorOf(std::move(result));
    }
    return txn.commit();
}

//-- 分析事实层 ------------------------------------------------------------

ca::Result<ca::usize, AnalysisDbError> AnalysisDb::ingestClasses(
    std::vector<DbClassInput> classes)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateClassInputs(classes); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    if (auto result = applyClassBatch(impl_->handle, classes); result.is_err()) {
        return errorOf(std::move(result));
    }
    if (auto result = txn.commit(); result.is_err()) {
        return errorOf(std::move(result));
    }
    return ca::Ok(classes.size());
}

ca::Result<ca::usize, AnalysisDbError> AnalysisDb::replaceReferences(
    std::vector<DbReferenceRecord> references)
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    if (isReadOnly()) {
        return ca::Err(makeError(AnalysisDbErrorKind::ReadOnly,
                                 "cannot write a read-only analysis database"));
    }
    if (auto result = validateReferences(references); result.is_err()) {
        return errorOf(std::move(result));
    }

    auto transaction = Transaction::begin(impl_->handle);
    if (transaction.is_err()) return errorOf(std::move(transaction));
    auto txn = std::move(transaction).unwrap();

    if (auto result = exec(impl_->handle, "DELETE FROM refs"); result.is_err()) {
        return errorOf(std::move(result));
    }
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
        "SELECT class_id, internal_name, super_name, access, version "
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
        "SELECT c.class_id, c.internal_name, c.super_name, c.access, c.version, "
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
        if (sqlite3_column_type(statement.raw, 5) != SQLITE_NULL) {
            classes.back().interfaces.push_back(utf8Column(statement.raw, 5));
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

    auto prepared = prepare(
        impl_->handle,
        "WITH RECURSIVE subs(name) AS ("
        "  SELECT internal_name FROM classes WHERE super_name=?1"
        "  UNION"
        "  SELECT c.internal_name FROM classes c JOIN subs s ON c.super_name=s.name"
        ") SELECT name FROM subs ORDER BY name");
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

ca::Result<std::vector<ca::str::Utf8String>, AnalysisDbError> AnalysisDb::implementors(
    const ca::str::Utf8StringRef& interfaceName) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }

    auto prepared = prepare(
        impl_->handle,
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
        ") SELECT name FROM impls ORDER BY name");
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

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesToMethod(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    const std::string sql = std::string(kReferenceSelectSql) +
                            "WHERE r.ref_kind=2 AND r.target_class=?1 AND r.target_name=?2 "
                            "AND r.target_desc=?3 " +
                            kReferenceOrderSql;
    return runReferenceQuery(impl_->handle, sql, 3,
                             [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
                                 if (auto result = bindText(impl_->handle, statement, 1, owner);
                                     result.is_err()) {
                                     return errorOf(std::move(result));
                                 }
                                 if (auto result = bindText(impl_->handle, statement, 2, name);
                                     result.is_err()) {
                                     return errorOf(std::move(result));
                                 }
                                 return bindText(impl_->handle, statement, 3, descriptor);
                             });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesToField(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    const std::string sql = std::string(kReferenceSelectSql) +
                            "WHERE r.ref_kind=1 AND r.target_class=?1 AND r.target_name=?2 "
                            "AND r.target_desc=?3 " +
                            kReferenceOrderSql;
    return runReferenceQuery(impl_->handle, sql, 3,
                             [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
                                 if (auto result = bindText(impl_->handle, statement, 1, owner);
                                     result.is_err()) {
                                     return errorOf(std::move(result));
                                 }
                                 if (auto result = bindText(impl_->handle, statement, 2, name);
                                     result.is_err()) {
                                     return errorOf(std::move(result));
                                 }
                                 return bindText(impl_->handle, statement, 3, descriptor);
                             });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesToClass(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    const std::string sql = std::string(kReferenceSelectSql) +
                            "WHERE r.target_class=?1 " + kReferenceOrderSql;
    return runReferenceQuery(impl_->handle, sql, 1,
                             [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
                                 return bindText(impl_->handle, statement, 1, owner);
                             });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesFromMember(
    const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
    const ca::str::Utf8StringRef& descriptor) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    const std::string sql = std::string(kReferenceSelectSql) +
                            "WHERE c.internal_name=?1 AND m.name=?2 AND m.descriptor=?3 " +
                            kReferenceOrderSql;
    return runReferenceQuery(impl_->handle, sql, 3,
                             [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
                                 if (auto result = bindText(impl_->handle, statement, 1, owner);
                                     result.is_err()) {
                                     return errorOf(std::move(result));
                                 }
                                 if (auto result = bindText(impl_->handle, statement, 2, name);
                                     result.is_err()) {
                                     return errorOf(std::move(result));
                                 }
                                 return bindText(impl_->handle, statement, 3, descriptor);
                             });
}

ca::Result<std::vector<DbReferenceRecord>, AnalysisDbError> AnalysisDb::referencesFromClass(
    const ca::str::Utf8StringRef& owner) const
{
    if (!isOpen()) {
        return ca::Err(makeError(AnalysisDbErrorKind::Invalid, "database is not open"));
    }
    const std::string sql = std::string(kReferenceSelectSql) +
                            "WHERE c.internal_name=?1 " + kReferenceOrderSql;
    return runReferenceQuery(impl_->handle, sql, 1,
                             [&](sqlite3_stmt* statement) -> ca::Result<void, AnalysisDbError> {
                                 return bindText(impl_->handle, statement, 1, owner);
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

}   // namespace mjbase::project
