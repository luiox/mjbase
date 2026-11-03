/// @file        analysis_producer_registry_test.cpp
/// @brief       AnalysisDb producer 注册表（schema v6，#143）：注册幂等、
///              批次生命周期（started → 收尾回填）、残批可见、trust_level
///              CHECK 门、只读拒写；后续批次在此叠加 renames 行级归属与
///              作用域归属解析。
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <filesystem>
#include <string>
#include <vector>

#include "mjbase/project/analysis_db.hpp"
#include "test_helper.hpp"

using namespace mjbase::project;
using ca::str::Utf8String;
using ca::str::Utf8StringRef;

namespace {

Utf8StringRef S(const char* value)
{
    return Utf8StringRef::from_cstr(value);
}

Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

std::filesystem::path freshDbPath(const std::string& name)
{
    const auto path = test::getTempOutPath("analysis_registry_" + name + ".mjdb");
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::remove_all(path.string() + "-wal", ec);
    std::filesystem::remove_all(path.string() + "-shm", ec);
    std::filesystem::create_directories(path.parent_path(), ec);
    return path;
}

AnalysisDb openOrDie(const std::filesystem::path& path,
                     AnalysisDbOpenMode          mode = AnalysisDbOpenMode::ReadWrite)
{
    auto result = AnalysisDb::open(refOf(path.string()), mode);
    EXPECT_TRUE(result.is_ok()) << path.string();
    return std::move(result).unwrap();
}

template <typename T>
AnalysisDbErrorKind takeErrorKind(ca::Result<T, AnalysisDbError>&& result)
{
    EXPECT_TRUE(result.is_err());
    if (result.is_err()) return std::move(result).unwrap_err().kind;
    return AnalysisDbErrorKind::Sqlite;
}

DbProducerInput producerInput(const char* key, const char* name, const char* fingerprint,
                              DbProducerTrustLevel trust = DbProducerTrustLevel::Trusted)
{
    DbProducerInput input;
    input.key             = Utf8String::from_cstr(key);
    input.name            = Utf8String::from_cstr(name);
    input.toolFingerprint = Utf8String::from_cstr(fingerprint);
    input.trustLevel      = trust;
    return input;
}

/// @brief 裸 sqlite3 只读连接（AnalysisDb 已 save + close 后使用）。
sqlite3* openRawReadOnly(const std::filesystem::path& path)
{
    sqlite3* handle = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_open_v2(path.string().c_str(), &handle,
                                         SQLITE_OPEN_READONLY, nullptr));
    return handle;
}

/// @brief 裸连接上的标量查询；SQL 由测试内给定，失败即致命。
ca::i64 rawScalar(sqlite3* db, const char* sql)
{
    sqlite3_stmt* statement = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &statement, nullptr));
    EXPECT_EQ(SQLITE_ROW, sqlite3_step(statement));
    const ca::i64 value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

/// @brief 裸连接上的文本标量；无行返回空串。
std::string rawText(sqlite3* db, const char* sql)
{
    sqlite3_stmt* statement = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql, -1, &statement, nullptr));
    std::string value;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement, 0);
        if (text != nullptr) {
            value.assign(reinterpret_cast<const char*>(text),
                         static_cast<std::size_t>(sqlite3_column_bytes(statement, 0)));
        }
    }
    sqlite3_finalize(statement);
    return value;
}

/// @brief 裸读写连接上执行 SQL；返回 (rc, 错误文本) 供负例断言。
std::pair<int, std::string> rawExec(sqlite3* db, const char* sql)
{
    char*       message = nullptr;
    const int   rc      = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    std::string detail  = message != nullptr ? message : "";
    sqlite3_free(message);
    return {rc, detail};
}

/// @brief 裸读写连接（FK 开启，与 AnalysisDb 连接纪律一致）。
sqlite3* openRawReadWrite(const std::filesystem::path& path)
{
    sqlite3* handle = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_open_v2(path.string().c_str(), &handle,
                                         SQLITE_OPEN_READWRITE, nullptr));
    if (handle != nullptr) {
        rawExec(handle, "PRAGMA foreign_keys=ON");
    }
    return handle;
}

}   // namespace

TEST(MjAnalysisProducerRegistryTest, RegistersProducerIdempotentlyByKey)
{
    const auto path = freshDbPath("register_idempotent");
    {
        auto db = openOrDie(path);
        EXPECT_EQ(kAnalysisDbSchemaVersion, db.schemaVersion());

        auto first = db.registerProducer(producerInput("mjt.workspace.ingest",
                                                       "MJT Workspace Ingest", "mjt 1.0.0"));
        ASSERT_TRUE(first.is_ok());
        const ca::i64 producerId = first.unwrap();

        // key 命中：复用同 producer_id，刷新 name / fingerprint / trust 元数据。
        auto refreshed = producerInput("mjt.workspace.ingest", "MJT Workspace Ingest Bridge",
                                       "mjt 1.2.0+build.7", DbProducerTrustLevel::Untrusted);
        auto second = db.registerProducer(refreshed);
        ASSERT_TRUE(second.is_ok());
        EXPECT_EQ(producerId, second.unwrap());

        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());

        sqlite3* raw = openRawReadOnly(path);
        ASSERT_NE(nullptr, raw);
        // 三表与覆盖索引进了 schema；注册只占一行（key 幂等去重）。
        EXPECT_EQ(3, rawScalar(raw,
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
            "('producers','batches','batch_scopes')"));
        EXPECT_EQ(1, rawScalar(raw,
            "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
            "name='batch_scopes_owner'"));
        EXPECT_EQ(1, rawScalar(raw, "SELECT COUNT(*) FROM producers"));
        EXPECT_EQ("MJT Workspace Ingest Bridge",
                  rawText(raw, "SELECT name FROM producers"));
        EXPECT_EQ("mjt 1.2.0+build.7",
                  rawText(raw, "SELECT tool_fingerprint FROM producers"));
        EXPECT_EQ(0, rawScalar(raw, "SELECT trust_level FROM producers"));
        // created_at 保持首注值：与 updated_at 同刻写入，重注册只动 updated_at。
        EXPECT_EQ(rawText(raw, "SELECT created_at FROM producers"),
                  rawText(raw, "SELECT updated_at FROM producers"));
        EXPECT_FALSE(rawText(raw, "SELECT created_at FROM producers").empty());
        sqlite3_close(raw);
    }
}

TEST(MjAnalysisProducerRegistryTest, TrustLevelCheckRejectsOutOfRangeValues)
{
    const auto path = freshDbPath("trust_check");
    auto       db   = openOrDie(path);
    ASSERT_TRUE(db.registerProducer(
                     producerInput("poc.a", "A", ""))
                    .is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());

    // trust_level 两档 CHECK 是兜底门：C++ 枚举造不出的值从裸连接直插必须拒写。
    sqlite3* rw = openRawReadWrite(path);
    ASSERT_NE(nullptr, rw);
    auto over = rawExec(rw,
        "INSERT INTO producers(key,name,tool_fingerprint,trust_level,created_at,updated_at) "
        "VALUES('poc.bad','x','',2,'t','t')");
    EXPECT_NE(SQLITE_OK, over.first);
    EXPECT_NE(std::string::npos, over.second.find("CHECK"));
    auto negative = rawExec(rw,
        "INSERT INTO producers(key,name,tool_fingerprint,trust_level,created_at,updated_at) "
        "VALUES('poc.bad2','x','',-1,'t','t')");
    EXPECT_NE(SQLITE_OK, negative.first);
    EXPECT_EQ(1, rawScalar(rw, "SELECT COUNT(*) FROM producers"));
    sqlite3_close(rw);
}

TEST(MjAnalysisProducerRegistryTest, BatchLifecycleWritesStartedAndFinishedRows)
{
    const auto path = freshDbPath("batch_lifecycle");
    auto       db   = openOrDie(path);

    auto producer = db.registerProducer(producerInput("mjt.workspace.ingest", "Ingest", ""));
    ASSERT_TRUE(producer.is_ok());
    const ca::i64 producerId = producer.unwrap();

    auto opened = db.openBatch(producerId, DbBatchKind::FullImport, S("first import"));
    ASSERT_TRUE(opened.is_ok());
    const ca::i64 batchId = opened.unwrap();

    // started 批：finished_at 为空串（残批可见），计数缺省 0。
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
    {
        sqlite3* raw = openRawReadOnly(path);
        ASSERT_NE(nullptr, raw);
        EXPECT_EQ(1, rawScalar(raw, "SELECT COUNT(*) FROM batches"));
        EXPECT_EQ(producerId, rawScalar(raw, "SELECT producer_id FROM batches"));
        EXPECT_EQ("full_import", rawText(raw, "SELECT kind FROM batches"));
        EXPECT_EQ("first import", rawText(raw, "SELECT note FROM batches"));
        EXPECT_TRUE(rawText(raw, "SELECT finished_at FROM batches").empty());
        EXPECT_FALSE(rawText(raw, "SELECT started_at FROM batches").empty());
        EXPECT_EQ(0, rawScalar(raw, "SELECT class_count FROM batches"));
        EXPECT_EQ(0, rawScalar(raw, "SELECT member_count FROM batches"));
        EXPECT_EQ(0, rawScalar(raw, "SELECT ref_count FROM batches"));
        sqlite3_close(raw);
    }

    // 收尾回填：finished_at + 四项计数。
    auto reopened = openOrDie(path);
    DbBatchCounts counts;
    counts.classCount  = 263;
    counts.entryCount  = 900;
    counts.memberCount = 1400;
    counts.refCount    = 30000;
    ASSERT_TRUE(reopened.closeBatch(batchId, counts).is_ok());
    ASSERT_TRUE(reopened.save().is_ok());
    ASSERT_TRUE(reopened.close().is_ok());
    {
        sqlite3* raw = openRawReadOnly(path);
        ASSERT_NE(nullptr, raw);
        EXPECT_FALSE(rawText(raw, "SELECT finished_at FROM batches").empty());
        EXPECT_EQ(263, rawScalar(raw, "SELECT class_count FROM batches"));
        EXPECT_EQ(900, rawScalar(raw, "SELECT entry_count FROM batches"));
        EXPECT_EQ(1400, rawScalar(raw, "SELECT member_count FROM batches"));
        EXPECT_EQ(30000, rawScalar(raw, "SELECT ref_count FROM batches"));
        sqlite3_close(raw);
    }

    // 重复收尾 / 未知批次 / 负计数 → Invalid。
    auto again = openOrDie(path);
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(again.closeBatch(batchId, DbBatchCounts{})));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(again.closeBatch(424242, DbBatchCounts{})));
    DbBatchCounts badCounts;
    badCounts.refCount = -1;
    auto badBatch      = again.openBatch(producerId, DbBatchKind::RefsReplace);
    ASSERT_TRUE(badBatch.is_ok());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(again.closeBatch(badBatch.unwrap(), badCounts)));
}

TEST(MjAnalysisProducerRegistryTest, ResidualBatchStaysVisibleAcrossReopen)
{
    // 未 closeBatch 的 finished_at 空串行是合法审计状态：重开（含只读）可读。
    const auto path = freshDbPath("residual_batch");
    ca::i64 batchId = 0;
    {
        auto db = openOrDie(path);
        auto producer = db.registerProducer(
            producerInput("mjt.pysdk.facade", "Facade", "", DbProducerTrustLevel::Untrusted));
        ASSERT_TRUE(producer.is_ok());
        auto opened = db.openBatch(producer.unwrap(), DbBatchKind::RefsSliceReplace);
        ASSERT_TRUE(opened.is_ok());
        batchId = opened.unwrap();
        // 刻意不 closeBatch，直接 save 收敛单文件。
        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
    }

    auto readOnly = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    EXPECT_EQ(kAnalysisDbSchemaVersion, readOnly.schemaVersion());
    ASSERT_TRUE(readOnly.save().is_ok());
    ASSERT_TRUE(readOnly.close().is_ok());

    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);
    EXPECT_EQ(1, rawScalar(raw, "SELECT COUNT(*) FROM batches"));
    EXPECT_EQ(static_cast<ca::i64>(batchId), rawScalar(raw, "SELECT batch_id FROM batches"));
    EXPECT_TRUE(rawText(raw, "SELECT finished_at FROM batches").empty());
    EXPECT_EQ("refs_slice_replace", rawText(raw, "SELECT kind FROM batches"));
    sqlite3_close(raw);
}

TEST(MjAnalysisProducerRegistryTest, OpenBatchRejectsUnknownProducerAndBadKind)
{
    const auto path = freshDbPath("open_batch_gates");
    auto       db   = openOrDie(path);

    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.openBatch(999, DbBatchKind::FullImport)));

    // kind CHECK 是兜底门：词表外的 kind 从裸连接直插必须拒写。
    auto producer = db.registerProducer(producerInput("poc.a", "A", ""));
    ASSERT_TRUE(producer.is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());

    sqlite3* rw = openRawReadWrite(path);
    ASSERT_NE(nullptr, rw);
    auto bogus = rawExec(rw,
        "INSERT INTO batches(producer_id, kind, started_at, note) "
        "VALUES(1, 'bogus_kind', 't', '')");
    EXPECT_NE(SQLITE_OK, bogus.first);
    EXPECT_NE(std::string::npos, bogus.second.find("CHECK"));
    // 作用域行 FK 门：未知 batch_id 拒写（fail-loud 拒绝面）。
    auto scopeFk = rawExec(rw,
        "INSERT INTO batch_scopes(batch_id, scope_kind, scope_id) VALUES(999999, 0, 0)");
    EXPECT_NE(SQLITE_OK, scopeFk.first);
    EXPECT_EQ(1, rawScalar(rw, "SELECT COUNT(*) FROM producers"));
    EXPECT_EQ(0, rawScalar(rw, "SELECT COUNT(*) FROM batches"));
    EXPECT_EQ(0, rawScalar(rw, "SELECT COUNT(*) FROM batch_scopes"));
    sqlite3_close(rw);
}

TEST(MjAnalysisProducerRegistryTest, RegistryWritesValidateInputsAndReadOnly)
{
    const auto path = freshDbPath("registry_gates");
    {
        auto db = openOrDie(path);
        DbProducerInput emptyKey;
        emptyKey.name = Utf8String::from_cstr("A");
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.registerProducer(emptyKey)));
        DbProducerInput emptyName;
        emptyName.key = Utf8String::from_cstr("poc.a");
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.registerProducer(emptyName)));

        auto producer = db.registerProducer(producerInput("poc.a", "A", ""));
        ASSERT_TRUE(producer.is_ok());
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.openBatch(0, DbBatchKind::FullImport)));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.closeBatch(0, DbBatchCounts{})));
        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
    }

    auto readOnly = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly,
              takeErrorKind(readOnly.registerProducer(
                  producerInput("poc.b", "B", ""))));
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly,
              takeErrorKind(readOnly.openBatch(1, DbBatchKind::FullImport)));
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly,
              takeErrorKind(readOnly.closeBatch(1, DbBatchCounts{})));
}
