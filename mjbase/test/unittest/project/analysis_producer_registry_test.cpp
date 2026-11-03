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

DbRenameRecord classRecord(const char* owner, const char* shadow, const char* token = "")
{
    DbRenameRecord record;
    record.target = RenameTargetKind::Class;
    record.owner  = Utf8String::from_cstr(owner);
    record.shadow = Utf8String::from_cstr(shadow);
    record.token  = Utf8String::from_cstr(token);
    return record;
}

DbRenameRecord memberRecord(DbMemberKind kind, const char* owner, const char* name,
                            const char* desc, const char* shadow, const char* token = "")
{
    DbRenameRecord record;
    record.target     = RenameTargetKind::Member;
    record.memberKind = kind;
    record.owner      = Utf8String::from_cstr(owner);
    record.name       = Utf8String::from_cstr(name);
    record.desc       = Utf8String::from_cstr(desc);
    record.shadow     = Utf8String::from_cstr(shadow);
    record.token      = Utf8String::from_cstr(token);
    return record;
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

//-- 批 2：renames 行级归属 ------------------------------------------------------

TEST(MjAnalysisProducerRegistryTest, RenamesSchemaKeepsColumnOrderAndIndexes)
{
    // 可空外键列追加在列序末位（等价 ALTER ADD COLUMN 落位），既有列序与
    // 两条索引保持不动——重建表路径的纪律就是原样保留这两样。
    const auto path = freshDbPath("renames_schema");
    auto       db   = openOrDie(path);
    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1")).is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());

    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);
    // pragma_table_info 的 cid 序：producer_id 必须是最后一列。
    EXPECT_EQ("target_kind,target_id,new_name,token,updated_at,producer_id",
              rawText(raw,
                      "SELECT group_concat(name, ',') FROM "
                      "(SELECT name FROM pragma_table_info('renames') ORDER BY cid)"));
    EXPECT_EQ(2, rawScalar(raw,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name IN "
        "('renames_token','renames_new_name')"));
    sqlite3_close(raw);
}

TEST(MjAnalysisProducerRegistryTest, RenameRowsCarryProducerAttribution)
{
    const auto path = freshDbPath("renames_attribution");
    auto       db   = openOrDie(path);

    auto first = db.registerProducer(producerInput("mjt.pysdk.facade", "Facade", ""));
    ASSERT_TRUE(first.is_ok());
    const ca::i64 facadeId = first.unwrap();
    auto second = db.registerProducer(producerInput("mjt.workspace.ingest", "Ingest", ""));
    ASSERT_TRUE(second.is_ok());
    const ca::i64 ingestId = second.unwrap();
    ASSERT_NE(facadeId, ingestId);

    // 带生产者盖章写入：逐行可查（API 输出与裸表一致）。
    ASSERT_TRUE(
        db.setClassRename(S("a/A"), S("Class_1"), S("deadbeef"), facadeId).is_ok());
    ASSERT_TRUE(db.setMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"),
                                   S("Method_1"), S("00000001"), facadeId)
                    .is_ok());
    {
        auto found = db.findClassRename(S("a/A"));
        ASSERT_TRUE(found.is_ok());
        auto value = std::move(found).unwrap();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(facadeId, value->producerId);
        auto member = db.findMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"));
        ASSERT_TRUE(member.is_ok());
        EXPECT_EQ(facadeId, std::move(member).unwrap()->producerId);
    }

    // 缺省（producerId 0）= 匿名写入：producer_id 落 NULL，记录输出 0。
    ASSERT_TRUE(db.setClassRename(S("a/B"), S("Class_2")).is_ok());
    {
        auto found = db.findClassRename(S("a/B"));
        ASSERT_TRUE(found.is_ok());
        EXPECT_EQ(0, std::move(found).unwrap()->producerId);
    }

    // 批量入口整批盖章；覆盖（last-write-wins）把方法行归属翻到新写者。
    std::vector<DbRenameRecord> batch;
    batch.push_back(memberRecord(DbMemberKind::Method, "a/A", "run", "()V", "Method_1b",
                                 "00000002"));
    batch.push_back(classRecord("a/C", "Class_3", "00000003"));
    ASSERT_TRUE(db.setRenames(std::move(batch), ingestId).is_ok());
    {
        auto all = db.listRenames();
        ASSERT_TRUE(all.is_ok());
        ca::usize facadeRows = 0;
        ca::usize ingestRows = 0;
        ca::usize nullRows   = 0;
        for (const auto& row : std::move(all).unwrap()) {
            if (row.producerId == facadeId) ++facadeRows;
            if (row.producerId == ingestId) ++ingestRows;
            if (row.producerId == 0) ++nullRows;
        }
        // (0,A)=facade、(1,method)=ingest（覆盖翻转）、(0,B)=NULL、(0,C)=ingest。
        EXPECT_EQ(1, static_cast<int>(facadeRows));
        EXPECT_EQ(2, static_cast<int>(ingestRows));
        EXPECT_EQ(1, static_cast<int>(nullRows));
    }

    // 后写者整体覆盖先写者：方法行归属随最后一次覆盖再翻回来。
    ASSERT_TRUE(db.setMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"),
                                   S("Method_2"), S("00000001"), facadeId)
                    .is_ok());
    {
        auto all = db.listRenames();
        ASSERT_TRUE(all.is_ok());
        ca::usize facadeRows = 0;
        ca::usize ingestRows = 0;
        for (const auto& row : std::move(all).unwrap()) {
            if (row.producerId == facadeId) ++facadeRows;
            if (row.producerId == ingestId) ++ingestRows;
        }
        EXPECT_EQ(2, static_cast<int>(facadeRows));
        EXPECT_EQ(1, static_cast<int>(ingestRows));
    }

    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);
    EXPECT_EQ(4, rawScalar(raw, "SELECT COUNT(*) FROM renames"));
    EXPECT_EQ(3, rawScalar(raw, "SELECT COUNT(*) FROM renames WHERE producer_id IS NOT NULL"));
    sqlite3_close(raw);
}

TEST(MjAnalysisProducerRegistryTest, RenameProducerGateRejectsUnregisteredIds)
{
    const auto path = freshDbPath("renames_producer_gate");
    auto       db   = openOrDie(path);

    // 未注册生产者 → Invalid（可诊断拒绝面，非裸 FK 报错）；负值同拒。
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setClassRename(S("a/A"), S("Class_1"), S(""), 42)));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setClassRename(S("a/A"), S("Class_1"), S(""), -1)));
    std::vector<DbRenameRecord> batchSet;
    batchSet.push_back(classRecord("a/A", "Class_1"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setRenames(std::move(batchSet), 42)));
    std::vector<DbRenameRecord> batchReplace;
    batchReplace.push_back(classRecord("a/A", "Class_1"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceRenames(std::move(batchReplace), 42)));
    // 拒绝零写入：行不存在。
    auto all = db.listRenames();
    ASSERT_TRUE(all.is_ok());
    EXPECT_TRUE(std::move(all).unwrap().empty());

    // FK 兜底门：绕过 API 的直插对未知生产者必须拒写。
    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1")).is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
    sqlite3* rw = openRawReadWrite(path);
    ASSERT_NE(nullptr, rw);
    // target_id 无 FK（身份存在性由写入口保证）；这里只有 producer FK 会响。
    auto fk = rawExec(rw,
        "INSERT INTO renames(target_kind, target_id, new_name, token, updated_at, producer_id) "
        "VALUES(1, 12345, 'x', NULL, 't', 999)");
    EXPECT_NE(SQLITE_OK, fk.first);
    sqlite3_close(rw);
}

TEST(MjAnalysisProducerRegistryTest, ReplaceRenamesReplayStartsNullAndStampWorks)
{
    // 验收线（#143 批 2）：覆盖重放后新行 NULL；盖章 UPDATE 路径可重放生效。
    const auto path = freshDbPath("renames_replay_stamp");
    auto       db   = openOrDie(path);

    auto producer = db.registerProducer(producerInput("mjt.workspace.ingest", "Ingest", ""));
    ASSERT_TRUE(producer.is_ok());
    const ca::i64 producerId = producer.unwrap();

    // 首轮覆盖导入（带生产者）：行级盖章。
    std::vector<DbRenameRecord> first;
    first.push_back(classRecord("a/A", "Class_1", "deadbeef"));
    first.push_back(memberRecord(DbMemberKind::Method, "a/A", "run", "()V", "Method_1"));
    ASSERT_TRUE(db.replaceRenames(std::move(first), producerId).is_ok());
    {
        auto all = db.listRenames();
        ASSERT_TRUE(all.is_ok());
        for (const auto& row : std::move(all).unwrap()) {
            EXPECT_EQ(producerId, row.producerId);
        }
    }

    // 覆盖重放（匿名缺省）：整层替换后新行全部 NULL，不残留上一轮归属。
    std::vector<DbRenameRecord> replay;
    replay.push_back(classRecord("a/A", "Class_1", "deadbeef"));
    replay.push_back(memberRecord(DbMemberKind::Method, "a/A", "run", "()V", "Method_1"));
    ASSERT_TRUE(db.replaceRenames(std::move(replay), 0).is_ok());
    {
        auto all = db.listRenames();
        ASSERT_TRUE(all.is_ok());
        auto rows = std::move(all).unwrap();
        ASSERT_EQ(2U, rows.size());
        for (const auto& row : rows) {
            EXPECT_EQ(0, row.producerId);
        }
    }
    {
        auto byShadow = db.findRenamesByShadow(S("Method_1"));
        ASSERT_TRUE(byShadow.is_ok());
        EXPECT_EQ(1U, std::move(byShadow).unwrap().size());
    }

    // 盖章 UPDATE（外部归批动线）：对 NULL 行一次盖章，读路径随之可见；
    // 重复执行（重放同一条 UPDATE）幂等。
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
    sqlite3* rw = openRawReadWrite(path);
    ASSERT_NE(nullptr, rw);
    const std::string stamp =
        "UPDATE renames SET producer_id=" + std::to_string(producerId) +
        " WHERE producer_id IS NULL";
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(rw, stamp.c_str(), nullptr, nullptr, nullptr));
    EXPECT_EQ(2, rawScalar(rw, "SELECT COUNT(*) FROM renames WHERE producer_id IS NOT NULL"));
    EXPECT_EQ(producerId,
              rawScalar(rw, "SELECT MIN(producer_id) FROM renames"));
    EXPECT_EQ(SQLITE_OK, sqlite3_exec(rw, stamp.c_str(), nullptr, nullptr, nullptr));
    EXPECT_EQ(2, rawScalar(rw, "SELECT COUNT(*) FROM renames WHERE producer_id IS NOT NULL"));
    sqlite3_close(rw);

    // 盖章后 API 读路径逐行可见。
    auto reopened = openOrDie(path);
    {
        auto found = reopened.findClassRename(S("a/A"));
        ASSERT_TRUE(found.is_ok());
        EXPECT_EQ(producerId, std::move(found).unwrap()->producerId);
    }
}
