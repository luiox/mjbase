/// @file        analysis_producer_registry_test.cpp
/// @brief       AnalysisDb producer 注册表（schema v6，#143）：注册幂等、
///              批次生命周期（started → 收尾回填）、残批可见、trust_level
///              CHECK 门、只读拒写；renames 行级归属与作用域归属解析，及
///              mask 配置面行级归属（schema v7 遗留回填）。
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

//-- mask 配置面归属化（schema v7，#143 遗留回填）--------------------------------

namespace {

DbMaskRuleInput maskRuleInput(ca::i32 priority, const char* pattern, const char* note = "")
{
    DbMaskRuleInput input;
    input.priority = priority;
    input.pattern  = Utf8String::from_cstr(pattern);
    input.action   = MaskAction::Mask;
    input.note     = Utf8String::from_cstr(note);
    return input;
}

}   // namespace

TEST(MjAnalysisProducerRegistryTest, MaskRulesSchemaKeepsColumnOrderAndIndex)
{
    // 可空外键列追加在列序末位（等价 ALTER ADD COLUMN 落位），既有列序与
    // 既有索引保持不动——与 renames 行级归属同一条重建纪律。
    const auto path = freshDbPath("mask_rules_schema");
    auto       db   = openOrDie(path);
    std::vector<DbMaskRuleInput> rules;
    rules.push_back(maskRuleInput(1, "^poc_never_matches_[0-9]+$"));
    ASSERT_TRUE(db.replaceMaskRules(std::move(rules)).is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());

    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);
    // pragma_table_info 的 cid 序：producer_id 必须是最后一列。
    EXPECT_EQ("rule_id,priority,pattern,action,note,hit_count,producer_id",
              rawText(raw,
                      "SELECT group_concat(name, ',') FROM "
                      "(SELECT name FROM pragma_table_info('mask_rules') ORDER BY cid)"));
    EXPECT_EQ(1, rawScalar(raw,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='mask_rules_order'"));
    sqlite3_close(raw);
}

TEST(MjAnalysisProducerRegistryTest, MaskRuleRowsCarryProducerAttribution)
{
    const auto path = freshDbPath("mask_rules_attribution");
    auto       db   = openOrDie(path);

    auto first  = db.registerProducer(producerInput("mjt.mcp.mask_exit", "MaskExit", ""));
    ASSERT_TRUE(first.is_ok());
    const ca::i64 exitId = first.unwrap();
    auto second = db.registerProducer(producerInput("mjt.pysdk.facade", "Facade", ""));
    ASSERT_TRUE(second.is_ok());
    const ca::i64 facadeId = second.unwrap();

    // 盖章写入：listMaskRules 逐行带出写入者，且与 listProducers 登记行对上。
    std::vector<DbMaskRuleInput> firstBatch;
    firstBatch.push_back(maskRuleInput(1, "^secret_[a-z]+$", "a"));
    firstBatch.push_back(maskRuleInput(2, "^token_[a-z]+$", "b"));
    ASSERT_TRUE(db.replaceMaskRules(std::move(firstBatch), exitId).is_ok());
    {
        auto listed = db.listMaskRules();
        ASSERT_TRUE(listed.is_ok());
        auto rows = std::move(listed).unwrap();
        ASSERT_EQ(2U, rows.size());
        for (const auto& row : rows) {
            EXPECT_EQ(exitId, row.producerId);
        }
        auto producers = db.listProducers();
        ASSERT_TRUE(producers.is_ok());
        bool matched = false;
        for (const auto& producer : std::move(producers).unwrap()) {
            matched = matched || producer.producerId == exitId;
        }
        EXPECT_TRUE(matched);
    }

    // 缺省（producerId 0）= 匿名写入：整体替换后新行 producer_id 落 NULL，
    // 记录输出 0，不残留上一轮归属——缺省参数对既有调用语义零破坏。
    std::vector<DbMaskRuleInput> replay;
    replay.push_back(maskRuleInput(1, "^secret_[a-z]+$", "a"));
    ASSERT_TRUE(db.replaceMaskRules(std::move(replay)).is_ok());
    {
        auto listed = db.listMaskRules();
        ASSERT_TRUE(listed.is_ok());
        auto rows = std::move(listed).unwrap();
        ASSERT_EQ(1U, rows.size());
        EXPECT_EQ(0, rows[0].producerId);
    }
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
    {
        sqlite3* raw = openRawReadOnly(path);
        ASSERT_NE(nullptr, raw);
        EXPECT_EQ(1, rawScalar(raw, "SELECT COUNT(*) FROM mask_rules"));
        EXPECT_EQ(0,
                  rawScalar(raw, "SELECT COUNT(*) FROM mask_rules WHERE producer_id IS NOT NULL"));
        sqlite3_close(raw);
    }

    // 后写者整体覆盖先写者：归属随替换翻到新写者。
    auto reopened = openOrDie(path);
    {
        std::vector<DbMaskRuleInput> stamped;
        stamped.push_back(maskRuleInput(1, "^secret_[a-z]+$", "a"));
        stamped.push_back(maskRuleInput(2, "^token_[a-z]+$", "b"));
        ASSERT_TRUE(reopened.replaceMaskRules(std::move(stamped), facadeId).is_ok());
        auto listed = reopened.listMaskRules();
        ASSERT_TRUE(listed.is_ok());
        for (const auto& row : std::move(listed).unwrap()) {
            EXPECT_EQ(facadeId, row.producerId);
        }
    }

    // 匿名重放 → 外部归批动线：规则面挂批属消费方职责，对 NULL 行一次
    // 盖章 UPDATE，读路径随之逐行可见。
    std::vector<DbMaskRuleInput> anonymous;
    anonymous.push_back(maskRuleInput(1, "^secret_[a-z]+$", "a"));
    ASSERT_TRUE(reopened.replaceMaskRules(std::move(anonymous)).is_ok());
    ASSERT_TRUE(reopened.save().is_ok());
    ASSERT_TRUE(reopened.close().is_ok());
    {
        sqlite3* rw = openRawReadWrite(path);
        ASSERT_NE(nullptr, rw);
        const std::string stamp =
            "UPDATE mask_rules SET producer_id=" + std::to_string(exitId) +
            " WHERE producer_id IS NULL";
        EXPECT_EQ(SQLITE_OK, sqlite3_exec(rw, stamp.c_str(), nullptr, nullptr, nullptr));
        EXPECT_EQ(exitId, rawScalar(rw, "SELECT MIN(producer_id) FROM mask_rules"));
        EXPECT_EQ(SQLITE_OK, sqlite3_exec(rw, stamp.c_str(), nullptr, nullptr, nullptr));
        EXPECT_EQ(1, rawScalar(rw, "SELECT COUNT(*) FROM mask_rules"));
        sqlite3_close(rw);
    }
    {
        auto after = openOrDie(path);
        auto listed = after.listMaskRules();
        ASSERT_TRUE(listed.is_ok());
        auto rows = std::move(listed).unwrap();
        ASSERT_EQ(1U, rows.size());
        EXPECT_EQ(exitId, rows[0].producerId);
    }
}

TEST(MjAnalysisProducerRegistryTest, MaskRuleProducerGateRejectsUnregisteredIds)
{
    const auto path = freshDbPath("mask_rules_producer_gate");
    auto       db   = openOrDie(path);

    // 未注册生产者 → Invalid（可诊断拒绝面，非裸 FK 报错）；负值同拒。
    std::vector<DbMaskRuleInput> rules;
    rules.push_back(maskRuleInput(1, "^poc_[a-z]+$"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceMaskRules(std::move(rules), 42)));
    std::vector<DbMaskRuleInput> negative;
    negative.push_back(maskRuleInput(1, "^poc_[a-z]+$"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceMaskRules(std::move(negative), -1)));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setMaskTrustLevel(MaskTrustLevel::Untrusted, 42)));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setMaskTrustLevel(MaskTrustLevel::Untrusted, -1)));
    // 拒绝零写入：规则面与信任级别键都保持原状。
    auto listed = db.listMaskRules();
    ASSERT_TRUE(listed.is_ok());
    EXPECT_TRUE(std::move(listed).unwrap().empty());
    auto level = db.maskTrustLevel();
    ASSERT_TRUE(level.is_ok());
    EXPECT_EQ(MaskTrustLevel::Trusted, std::move(level).unwrap());

    // FK 兜底门：绕过 API 的直插对未知生产者必须拒写。
    std::vector<DbMaskRuleInput> ok;
    ok.push_back(maskRuleInput(1, "^poc_[a-z]+$"));
    ASSERT_TRUE(db.replaceMaskRules(std::move(ok)).is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
    sqlite3* rw = openRawReadWrite(path);
    ASSERT_NE(nullptr, rw);
    auto fk = rawExec(rw,
        "INSERT INTO mask_rules(priority, pattern, action, note, producer_id) "
        "VALUES(2, '^x$', 0, '', 999)");
    EXPECT_NE(SQLITE_OK, fk.first);
    EXPECT_EQ(1, rawScalar(rw, "SELECT COUNT(*) FROM mask_rules"));
    sqlite3_close(rw);
}

TEST(MjAnalysisProducerRegistryTest, MaskTrustLevelAttributionFollowsLastWriter)
{
    const auto path = freshDbPath("mask_trust_attribution");
    auto       db   = openOrDie(path);

    auto first  = db.registerProducer(producerInput("mjt.mcp.mask_exit", "MaskExit", ""));
    ASSERT_TRUE(first.is_ok());
    const ca::i64 exitId = first.unwrap();
    auto second = db.registerProducer(producerInput("poc.other", "Other", ""));
    ASSERT_TRUE(second.is_ok());
    const ca::i64 otherId = second.unwrap();

    // 匿名缺省写入：值落库，伴随归属键不存在（缺键即匿名）。
    ASSERT_TRUE(db.setMaskTrustLevel(MaskTrustLevel::Untrusted).is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
    {
        sqlite3* raw = openRawReadOnly(path);
        ASSERT_NE(nullptr, raw);
        EXPECT_EQ("untrusted", rawText(raw, "SELECT value FROM meta WHERE key='mask_trust_level'"));
        EXPECT_EQ(0,
                  rawScalar(raw, "SELECT COUNT(*) FROM meta WHERE key='mask_trust_level_producer'"));
        sqlite3_close(raw);
    }

    // 盖章写入：伴随键落写入者 id；重复设置同值幂等（归属随写刷新）。
    auto reopened = openOrDie(path);
    ASSERT_TRUE(reopened.setMaskTrustLevel(MaskTrustLevel::Untrusted, exitId).is_ok());
    ASSERT_TRUE(reopened.setMaskTrustLevel(MaskTrustLevel::Untrusted, exitId).is_ok());
    ASSERT_TRUE(reopened.save().is_ok());
    ASSERT_TRUE(reopened.close().is_ok());
    {
        sqlite3* raw = openRawReadOnly(path);
        ASSERT_NE(nullptr, raw);
        EXPECT_EQ("untrusted", rawText(raw, "SELECT value FROM meta WHERE key='mask_trust_level'"));
        EXPECT_EQ(exitId,
                  rawScalar(raw,
                            "SELECT CAST(value AS INTEGER) FROM meta "
                            "WHERE key='mask_trust_level_producer'"));
        sqlite3_close(raw);
    }

    // 后写者整体覆盖：值翻转 + 归属刷新；匿名重设清除伴随键、值保持。
    auto again = openOrDie(path);
    ASSERT_TRUE(again.setMaskTrustLevel(MaskTrustLevel::Trusted, otherId).is_ok());
    ASSERT_TRUE(again.setMaskTrustLevel(MaskTrustLevel::Trusted).is_ok());
    ASSERT_TRUE(again.save().is_ok());
    ASSERT_TRUE(again.close().is_ok());
    {
        sqlite3* raw = openRawReadOnly(path);
        ASSERT_NE(nullptr, raw);
        EXPECT_EQ("trusted", rawText(raw, "SELECT value FROM meta WHERE key='mask_trust_level'"));
        EXPECT_EQ(0,
                  rawScalar(raw, "SELECT COUNT(*) FROM meta WHERE key='mask_trust_level_producer'"));
        sqlite3_close(raw);
    }

    // 读路径（maskTrustLevel）全程不感知归属键，语义与 v7 之前一致。
    auto readOnly = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    auto level    = readOnly.maskTrustLevel();
    ASSERT_TRUE(level.is_ok());
    EXPECT_EQ(MaskTrustLevel::Trusted, std::move(level).unwrap());
}

//-- 批 3：写入口批次参数化、作用域归属解析与查询面 ------------------------------
//
// PoC（mjt/scratch/scratch_prod.cpp，#143 成本模型验证）29 项断言的场景
// 逐项复刻为 gtest；PoC 驱动里手写 SQL 的部分（作用域行、盖章 UPDATE）
// 在此经库面写入口或裸连接等价表达：
//   1  trust CHECK 拒绝         → TrustLevelCheckRejectsOutOfRangeValues
//   2  batch_scopes FK 拒绝     → OpenBatchRejectsUnknownProducerAndBadKind
//   3  注册幂等同 id            → RegistersProducerIdempotentlyByKey
//   4-5  batch1 后成员/类归属   → BatchScopedImportResolvesOwnershipAcrossBatches
//   6  选中带引用方法           → 语料构造保证（run()V 携带三条引用）
//   7-8  batch2 取大/他人不动   → 同上
//   9  切片批携带 class 锚翻转  → 同上（PoC 驱动手写 (2, class_id) 行，
//                                  以裸连接等价复刻：解析规则与批种类无关）
//   10 未触类仍属 batch1       → 同上
//   11 命中记账不入批次        → MaskHitAccountingStaysOutsideBatches
//   12 重导 created=0          → BatchScopedImportResolvesOwnershipAcrossBatches
//   13-15 batch3 重导翻转      → 同上
//   16-19 batch4 refs_replace 非对称 → 同上
//   20-21 renames 列落位       → RenamesSchemaKeepsColumnOrderAndIndexes
//   22-24 rename 写入读回      → RenameRowsCarryProducerAttribution
//   25 匿名写入 NULL           → 同上
//   26 盖章 UPDATE 生效        → ReplaceRenamesReplayStartsNullAndStampWorks
//   27-28 重放后新行 NULL      → 同上
//   29 加列后读路径不变        → ReplaceRenamesReplayStartsNullAndStampWorks

namespace {

DbMemberInput methodInput(const char* name, const char* desc)
{
    DbMemberInput member;
    member.kind       = DbMemberKind::Method;
    member.name       = Utf8String::from_cstr(name);
    member.descriptor = Utf8String::from_cstr(desc);
    return member;
}

DbMemberInput fieldInput(const char* name, const char* desc)
{
    DbMemberInput member;
    member.kind       = DbMemberKind::Field;
    member.name       = Utf8String::from_cstr(name);
    member.descriptor = Utf8String::from_cstr(desc);
    return member;
}

DbClassInput classInput(const char* name, const char* superName)
{
    DbClassInput input;
    input.internalName = Utf8String::from_cstr(name);
    input.superName    = Utf8String::from_cstr(superName);
    return input;
}

DbEntryInput entryInput(const char* path, ca::i64 size)
{
    DbEntryInput entry;
    entry.path   = Utf8String::from_cstr(path);
    entry.kind   = Utf8String::from_cstr("class");
    entry.size   = size;
    entry.sha256 = Utf8String::from_cstr(
        "0000000000000000000000000000000000000000000000000000000000000000");
    return entry;
}

DbReferenceRecord referenceRecord(const char* sourceClass, const char* sourceMethod,
                                  const char* sourceDesc, ca::i64 insn, DbReferenceKind kind,
                                  const char* targetClass, const char* targetName,
                                  const char* targetDesc)
{
    DbReferenceRecord record;
    record.sourceClass      = Utf8String::from_cstr(sourceClass);
    record.sourceMethod     = Utf8String::from_cstr(sourceMethod);
    record.sourceDescriptor = Utf8String::from_cstr(sourceDesc);
    record.insnIndex        = insn;
    record.kind             = kind;
    record.targetClass      = Utf8String::from_cstr(targetClass);
    record.targetName       = Utf8String::from_cstr(targetName);
    record.targetDescriptor = Utf8String::from_cstr(targetDesc);
    return record;
}

/// @brief PoC 语料（等比缩小）：三类四成员一条输入；Alpha.run 携带三条引用
///        作切片替换对象。类/条目/引用三段以 batchId 0 匿名写入（无批口径），
///        供非批次路径单独验证。
void buildCorpus(AnalysisDb& db, ca::i64& inputId)
{
    std::vector<DbClassInput> classes;
    DbClassInput alpha = classInput("demo/Alpha", "java/lang/Object");
    alpha.members.push_back(methodInput("run", "()V"));
    alpha.members.push_back(methodInput("go", "()V"));
    alpha.members.push_back(fieldInput("counter", "I"));
    classes.push_back(std::move(alpha));
    DbClassInput beta = classInput("demo/Beta", "java/lang/Object");
    beta.members.push_back(methodInput("go", "()V"));
    classes.push_back(std::move(beta));
    classes.push_back(classInput("demo/Gamma", "java/lang/Object"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    auto ensured = db.ensureInput(
        S("poc/poc.jar"),
        S("1111111111111111111111111111111111111111111111111111111111111111"), 4096);
    ASSERT_TRUE(ensured.is_ok());
    inputId = ensured.unwrap();
    ASSERT_TRUE(inputId > 0);
    std::vector<DbEntryInput> entries;
    entries.push_back(entryInput("demo/Alpha.class", 512));
    entries.push_back(entryInput("demo/Beta.class", 256));
    entries.push_back(entryInput("demo/Gamma.class", 128));
    ASSERT_TRUE(db.ingestEntries(0, inputId, std::move(entries)).is_ok());

    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 3,
                                         DbReferenceKind::Method, "demo/Beta", "go", "()V"));
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 4,
                                         DbReferenceKind::Field, "demo/Alpha", "counter", "I"));
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 5,
                                         DbReferenceKind::Type, "demo/Gamma", "", ""));
    references.push_back(referenceRecord("demo/Beta", "go", "()V", 0,
                                         DbReferenceKind::Type, "demo/Gamma", "", ""));
    ASSERT_TRUE(db.replaceReferences(0, std::move(references)).is_ok());
}

DbReferenceRecord alphaRunSlice()
{
    return referenceRecord("demo/Alpha", "run", "()V", 3,
                           DbReferenceKind::Method, "demo/Beta", "go", "()V");
}

/// @brief 断言成员切片解析到期望批次（nullopt = 期望无归属）。
void expectSliceOwnership(AnalysisDb& db, DbMemberKind kind, const char* owner,
                          const char* name, const char* desc, const char* what,
                          const std::optional<ca::i64>& expectedBatch)
{
    auto memberId = db.findMember(kind, S(owner), S(name), S(desc));
    ASSERT_TRUE(memberId.is_ok()) << what;
    auto memberValue = std::move(memberId).unwrap();
    ASSERT_TRUE(memberValue.has_value()) << what;
    auto resolved = db.refSliceOwnership(kind, memberValue->memberId);
    ASSERT_TRUE(resolved.is_ok()) << what;
    auto ownership = std::move(resolved).unwrap();
    if (!expectedBatch.has_value()) {
        EXPECT_FALSE(ownership.has_value()) << what;
        return;
    }
    ASSERT_TRUE(ownership.has_value()) << what;
    EXPECT_EQ(expectedBatch.value(), ownership->batchId) << what;
}

/// @brief 断言类事实解析到期望批次（nullopt = 期望无归属）。
void expectClassOwnership(AnalysisDb& db, const char* owner, const char* what,
                          const std::optional<ca::i64>& expectedBatch)
{
    auto classId = db.findClass(S(owner));
    ASSERT_TRUE(classId.is_ok()) << what;
    auto classValue = std::move(classId).unwrap();
    ASSERT_TRUE(classValue.has_value()) << what;
    auto resolved = db.classOwnership(classValue->classId);
    ASSERT_TRUE(resolved.is_ok()) << what;
    auto ownership = std::move(resolved).unwrap();
    if (!expectedBatch.has_value()) {
        EXPECT_FALSE(ownership.has_value()) << what;
        return;
    }
    ASSERT_TRUE(ownership.has_value()) << what;
    EXPECT_EQ(expectedBatch.value(), ownership->batchId) << what;
}

/// @brief 归属解析 probe 形状镜像 analysis_db.cpp 的 kScopeOwnershipProbeSql
///        （计划检查用；改实现必须同步改这里）。
const char* kTestScopeProbeSql =
    "SELECT s.batch_id, b.producer_id, p.key, b.kind "
    "FROM batch_scopes s "
    "JOIN batches b ON b.batch_id = s.batch_id "
    "JOIN producers p ON p.producer_id = b.producer_id "
    "WHERE s.scope_kind = ?1 AND s.scope_id = ?2 "
    "ORDER BY s.batch_id DESC LIMIT 1";

/// @brief EXPLAIN QUERY PLAN 的 detail 列（列 3）逐行收集。
std::vector<std::string> rawQueryPlanDetails(sqlite3* db, const std::string& sql)
{
    sqlite3_stmt* statement = nullptr;
    EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, ("EXPLAIN QUERY PLAN " + sql).c_str(), -1,
                                            &statement, nullptr));
    std::vector<std::string> details;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const auto* text = sqlite3_column_text(statement, 3);
        details.emplace_back(text != nullptr ? reinterpret_cast<const char*>(text) : "");
    }
    sqlite3_finalize(statement);
    return details;
}

bool hasSearchOnIndex(const std::vector<std::string>& details, const char* indexName)
{
    for (const auto& detail : details) {
        if (detail.find("SEARCH") != std::string::npos &&
            detail.find(indexName) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// @brief batch_scopes（别名 s）被裸 SCAN（无 USING 的全表扫）即回归。
bool hasBareScopeScan(const std::vector<std::string>& details)
{
    for (const auto& detail : details) {
        const bool scopeScan = detail.rfind("SCAN", 0) == 0 &&
                               (detail.find("batch_scopes") != std::string::npos ||
                                detail.rfind("SCAN s", 0) == 0);
        if (scopeScan && detail.find("USING") == std::string::npos) {
            return true;
        }
    }
    return false;
}

}   // namespace

TEST(MjAnalysisProducerRegistryTest, BatchScopedImportResolvesOwnershipAcrossBatches)
{
    const auto path = freshDbPath("ownership_batches");
    auto       db   = openOrDie(path);

    auto producer = db.registerProducer(producerInput("mjt.workspace.ingest", "Ingest", ""));
    ASSERT_TRUE(producer.is_ok());
    const ca::i64 producerId = producer.unwrap();

    ca::i64 inputId = 0;
    buildCorpus(db, inputId);
    const ca::i64 alphaRunId = [&] {
        auto member = db.findMember(DbMemberKind::Method, S("demo/Alpha"), S("run"), S("()V"));
        EXPECT_TRUE(member.is_ok());
        return std::move(member).unwrap()->memberId;
    }();

    // batch1 = full_import：三段写入口都挂靠本批（input + 逐类 + refs_all）。
    const auto batch1Opened = db.openBatch(producerId, DbBatchKind::FullImport, S("first"));
    ASSERT_TRUE(batch1Opened.is_ok());
    const ca::i64 batch1 = batch1Opened.unwrap();

    std::vector<DbClassInput> classes;
    {
        DbClassInput alpha = classInput("demo/Alpha", "java/lang/Object");
        alpha.members.push_back(methodInput("run", "()V"));
        alpha.members.push_back(methodInput("go", "()V"));
        alpha.members.push_back(fieldInput("counter", "I"));
        classes.push_back(std::move(alpha));
        DbClassInput beta = classInput("demo/Beta", "java/lang/Object");
        beta.members.push_back(methodInput("go", "()V"));
        classes.push_back(std::move(beta));
        classes.push_back(classInput("demo/Gamma", "java/lang/Object"));
    }
    ASSERT_TRUE(db.ingestClasses(batch1, std::move(classes)).is_ok());

    std::vector<DbEntryInput> entries;
    entries.push_back(entryInput("demo/Alpha.class", 512));
    entries.push_back(entryInput("demo/Beta.class", 256));
    entries.push_back(entryInput("demo/Gamma.class", 128));
    auto entryStats = db.ingestEntries(batch1, inputId, std::move(entries));
    ASSERT_TRUE(entryStats.is_ok());
    // 语料经 buildCorpus 匿名预登记：锚命中复用 id（created=0，reused=3）。
    EXPECT_EQ(0U, entryStats.unwrap().created);
    EXPECT_EQ(3U, entryStats.unwrap().reused);

    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 3,
                                         DbReferenceKind::Method, "demo/Beta", "go", "()V"));
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 4,
                                         DbReferenceKind::Field, "demo/Alpha", "counter", "I"));
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 5,
                                         DbReferenceKind::Type, "demo/Gamma", "", ""));
    references.push_back(referenceRecord("demo/Beta", "go", "()V", 0,
                                         DbReferenceKind::Type, "demo/Gamma", "", ""));
    ASSERT_TRUE(db.replaceReferences(batch1, std::move(references)).is_ok());

    DbBatchCounts counts1;
    counts1.classCount  = 3;
    counts1.entryCount  = 3;
    counts1.memberCount = 4;
    counts1.refCount    = 4;
    ASSERT_TRUE(db.closeBatch(batch1, counts1).is_ok());

    // PoC 断言 4/5：batch1-only 态，成员切片与类事实都解析到整库批。
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "run", "()V",
                         "alpha.run after batch1", std::optional<ca::i64>(batch1));
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "go", "()V",
                         "alpha.go after batch1", std::optional<ca::i64>(batch1));
    expectSliceOwnership(db, DbMemberKind::Field, "demo/Alpha", "counter", "I",
                         "alpha.counter after batch1", std::optional<ca::i64>(batch1));
    expectClassOwnership(db, "demo/Alpha", "alpha after batch1", std::optional<ca::i64>(batch1));
    expectClassOwnership(db, "demo/Gamma", "gamma after batch1", std::optional<ca::i64>(batch1));

    // batch2 = refs_slice_replace：单成员切片批（只落 (3, member_id) 锚）。
    const auto batch2Opened = db.openBatch(producerId, DbBatchKind::RefsSliceReplace, S("slice"));
    ASSERT_TRUE(batch2Opened.is_ok());
    const ca::i64 batch2 = batch2Opened.unwrap();
    {
        std::vector<DbReferenceRecord> slice;
        slice.push_back(alphaRunSlice());
        ASSERT_TRUE(db.replaceReferencesFromMember(batch2, alphaRunId, std::move(slice)).is_ok());
    }
    DbBatchCounts counts2;
    counts2.refCount = 1;
    ASSERT_TRUE(db.closeBatch(batch2, counts2).is_ok());

    // PoC 断言 7：被切片成员取大翻到成员批；8：未切片成员仍属 batch1；
    // 10：类事实未被触碰仍属 batch1（切片批不落 class 锚）。
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "run", "()V",
                         "alpha.run after batch2", std::optional<ca::i64>(batch2));
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "go", "()V",
                         "alpha.go after batch2", std::optional<ca::i64>(batch1));
    expectClassOwnership(db, "demo/Alpha", "alpha after batch2", std::optional<ca::i64>(batch1));

    // PoC 断言 9 的场景：切片批携带 class 锚（PoC 驱动手写 (2, class_id) 行）
    // 时类事实归属翻到该批——解析规则与批种类无关，按锚前缀取大。
    {
        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
        sqlite3* rw = openRawReadWrite(path);
        ASSERT_NE(nullptr, rw);
        const std::string anchor =
            "INSERT INTO batch_scopes(batch_id, scope_kind, scope_id) "
            "VALUES(" + std::to_string(batch2) + ", 2, "
            "(SELECT class_id FROM classes WHERE internal_name='demo/Alpha'))";
        EXPECT_EQ(SQLITE_OK, sqlite3_exec(rw, anchor.c_str(), nullptr, nullptr, nullptr));
        sqlite3_close(rw);
        auto reopened = openOrDie(path);
        expectClassOwnership(reopened, "demo/Alpha", "alpha with batch2 class anchor",
                             std::optional<ca::i64>(batch2));
        db = std::move(reopened);
    }

    // batch3 = full_import 重导入：锚复用 created=0（PoC 断言 12），重落逐类
    // 锚与 refs_all 锚。
    const auto batch3Opened = db.openBatch(producerId, DbBatchKind::FullImport, S("reimport"));
    ASSERT_TRUE(batch3Opened.is_ok());
    const ca::i64 batch3 = batch3Opened.unwrap();
    {
        std::vector<DbEntryInput> sameEntries;
        sameEntries.push_back(entryInput("demo/Alpha.class", 512));
        sameEntries.push_back(entryInput("demo/Beta.class", 256));
        sameEntries.push_back(entryInput("demo/Gamma.class", 128));
        auto sameStats = db.ingestEntries(batch3, inputId, std::move(sameEntries));
        ASSERT_TRUE(sameStats.is_ok());
        EXPECT_EQ(0U, sameStats.unwrap().created);

        std::vector<DbClassInput> sameClasses;
        DbClassInput alpha = classInput("demo/Alpha", "java/lang/Object");
        alpha.members.push_back(methodInput("run", "()V"));
        alpha.members.push_back(methodInput("go", "()V"));
        alpha.members.push_back(fieldInput("counter", "I"));
        sameClasses.push_back(std::move(alpha));
        DbClassInput beta = classInput("demo/Beta", "java/lang/Object");
        beta.members.push_back(methodInput("go", "()V"));
        sameClasses.push_back(std::move(beta));
        sameClasses.push_back(classInput("demo/Gamma", "java/lang/Object"));
        ASSERT_TRUE(db.ingestClasses(batch3, std::move(sameClasses)).is_ok());

        std::vector<DbReferenceRecord> sameRefs;
        sameRefs.push_back(referenceRecord("demo/Alpha", "run", "()V", 3,
                                           DbReferenceKind::Method, "demo/Beta", "go", "()V"));
        sameRefs.push_back(referenceRecord("demo/Alpha", "run", "()V", 4,
                                           DbReferenceKind::Field, "demo/Alpha", "counter", "I"));
        sameRefs.push_back(referenceRecord("demo/Alpha", "run", "()V", 5,
                                           DbReferenceKind::Type, "demo/Gamma", "", ""));
        sameRefs.push_back(referenceRecord("demo/Beta", "go", "()V", 0,
                                           DbReferenceKind::Type, "demo/Gamma", "", ""));
        ASSERT_TRUE(db.replaceReferences(batch3, std::move(sameRefs)).is_ok());
    }
    ASSERT_TRUE(db.closeBatch(batch3, DbBatchCounts{}).is_ok());

    // PoC 断言 13：成员切片被更新的 refs_all 批盖过（取大）；14/15：其余
    // 成员与类事实都到 batch3（重导入逐类锚比 batch2 手写锚新）。
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "run", "()V",
                         "alpha.run after batch3", std::optional<ca::i64>(batch3));
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "go", "()V",
                         "alpha.go after batch3", std::optional<ca::i64>(batch3));
    expectClassOwnership(db, "demo/Alpha", "alpha after batch3", std::optional<ca::i64>(batch3));

    // batch4 = refs_replace：纯整库引用替换，只落 refs_all 锚。
    const auto batch4Opened = db.openBatch(producerId, DbBatchKind::RefsReplace, S("refs only"));
    ASSERT_TRUE(batch4Opened.is_ok());
    const ca::i64 batch4 = batch4Opened.unwrap();
    {
        std::vector<DbReferenceRecord> allRefs;
        allRefs.push_back(referenceRecord("demo/Alpha", "run", "()V", 3,
                                          DbReferenceKind::Method, "demo/Beta", "go", "()V"));
        allRefs.push_back(referenceRecord("demo/Alpha", "run", "()V", 4,
                                          DbReferenceKind::Field, "demo/Alpha", "counter", "I"));
        allRefs.push_back(referenceRecord("demo/Alpha", "run", "()V", 5,
                                          DbReferenceKind::Type, "demo/Gamma", "", ""));
        allRefs.push_back(referenceRecord("demo/Beta", "go", "()V", 0,
                                          DbReferenceKind::Type, "demo/Gamma", "", ""));
        ASSERT_TRUE(db.replaceReferences(batch4, std::move(allRefs)).is_ok());
    }
    ASSERT_TRUE(db.closeBatch(batch4, DbBatchCounts{}).is_ok());

    // PoC 断言 16/17：切片归属翻转；18/19：类事实非对称——refs_replace
    // 只动成员切片，类事实停在 batch3。
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "run", "()V",
                         "alpha.run after batch4", std::optional<ca::i64>(batch4));
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "go", "()V",
                         "alpha.go after batch4", std::optional<ca::i64>(batch4));
    expectSliceOwnership(db, DbMemberKind::Field, "demo/Alpha", "counter", "I",
                         "alpha.counter after batch4", std::optional<ca::i64>(batch4));
    expectClassOwnership(db, "demo/Alpha", "alpha stays at batch3 after batch4",
                         std::optional<ca::i64>(batch3));
    expectClassOwnership(db, "demo/Beta", "beta stays at batch3 after batch4",
                         std::optional<ca::i64>(batch3));
    expectClassOwnership(db, "demo/Gamma", "gamma stays at batch3 after batch4",
                         std::optional<ca::i64>(batch3));

    // batch5 = 类头切片替换（replaceReferencesFromClass）：Alpha 的类头引用行
    // 换新并落 (2, class_id) 锚——与 ingestClasses 同锚，类事实归属合流翻到
    // batch5；方法体行未触碰，成员切片归属停在 batch4。
    const auto batch5Opened =
        db.openBatch(producerId, DbBatchKind::RefsSliceReplace, S("header slice"));
    ASSERT_TRUE(batch5Opened.is_ok());
    const ca::i64 batch5 = batch5Opened.unwrap();
    {
        auto alphaClass = db.findClass(S("demo/Alpha"));
        ASSERT_TRUE(alphaClass.is_ok());
        auto alphaRow = std::move(alphaClass).unwrap();
        ASSERT_TRUE(alphaRow.has_value());
        std::vector<DbReferenceRecord> header;
        header.push_back(referenceRecord("demo/Alpha", "", "", -1,
                                         DbReferenceKind::Type, "demo/Gamma", "", ""));
        ASSERT_TRUE(
            db.replaceReferencesFromClass(batch5, alphaRow->classId, std::move(header)).is_ok());
    }
    ASSERT_TRUE(db.closeBatch(batch5, DbBatchCounts{}).is_ok());

    expectClassOwnership(db, "demo/Alpha", "alpha after batch5 header slice",
                         std::optional<ca::i64>(batch5));
    expectClassOwnership(db, "demo/Beta", "beta stays at batch3 after batch5",
                         std::optional<ca::i64>(batch3));
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "run", "()V",
                         "alpha.run stays at batch4 after batch5",
                         std::optional<ca::i64>(batch4));
}

TEST(MjAnalysisProducerRegistryTest, MaskHitAccountingStaysOutsideBatches)
{
    // PoC 断言 11：命中记账与注册表共存且不入任何批次（recordMaskRuleHits
    // 不参数化——聚合计数列，无行级事实可归属）。
    const auto path = freshDbPath("mask_hits_outside_batches");
    auto       db   = openOrDie(path);

    auto producer = db.registerProducer(producerInput("mjt.mcp.mask_exit", "MaskExit", ""));
    ASSERT_TRUE(producer.is_ok());
    auto batch = db.openBatch(producer.unwrap(), DbBatchKind::RulesWrite);
    ASSERT_TRUE(batch.is_ok());

    std::vector<DbMaskRuleInput> rules;
    DbMaskRuleInput rule;
    rule.priority = 1;
    rule.pattern  = Utf8String::from_cstr("^poc_never_matches_[0-9]+$");
    rule.action   = MaskAction::Mask;
    rule.note     = Utf8String::from_cstr("poc hit accounting");
    rules.push_back(std::move(rule));
    ASSERT_TRUE(db.replaceMaskRules(std::move(rules)).is_ok());

    std::vector<ca::i64> hits(3, 1);
    ASSERT_TRUE(db.recordMaskRuleHits(hits).is_ok());

    auto listed = db.listMaskRules();
    ASSERT_TRUE(listed.is_ok());
    auto ruleRows = std::move(listed).unwrap();
    ASSERT_EQ(1U, ruleRows.size());
    EXPECT_EQ(3, static_cast<int>(ruleRows[0].hitCount));

    // 命中记账没有留下任何作用域行；规则批仍是空残批。
    auto scopes = db.batchScopes(batch.unwrap());
    ASSERT_TRUE(scopes.is_ok());
    EXPECT_TRUE(std::move(scopes).unwrap().empty());
}

TEST(MjAnalysisProducerRegistryTest, RolledBackWriteLeavesNoScopeRows)
{
    // 验收线（#143 批 3）：批内回滚不留作用域行——写入口的事务把事实与
    // 作用域行绑在一起，任何一步失败整批回滚。
    const auto path = freshDbPath("rollback_no_scope");
    auto       db   = openOrDie(path);

    auto producer = db.registerProducer(producerInput("poc.a", "A", ""));
    ASSERT_TRUE(producer.is_ok());
    const ca::i64 producerId = producer.unwrap();
    ca::i64       inputId    = 0;
    buildCorpus(db, inputId);

    const auto batchOpened = db.openBatch(producerId, DbBatchKind::RefsReplace, S("doomed"));
    ASSERT_TRUE(batchOpened.is_ok());
    const ca::i64 batch = batchOpened.unwrap();

    // 先匿名落一份旧 refs 作为回滚基线。
    std::vector<DbReferenceRecord> baseline;
    baseline.push_back(referenceRecord("demo/Alpha", "go", "()V", 1,
                                       DbReferenceKind::Type, "demo/Gamma", "", ""));
    ASSERT_TRUE(db.replaceReferences(0, std::move(baseline)).is_ok());

    // 挂靠批次的整库替换中途失败（source 类未登记，applyReferenceBatch 内
    // 报错）→ Invalid；作用域行与 DELETE 随事务一起回滚，旧 refs 原样。
    std::vector<DbReferenceRecord> doomed;
    doomed.push_back(referenceRecord("demo/Nowhere", "run", "()V", 0,
                                     DbReferenceKind::Type, "demo/Gamma", "", ""));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferences(batch, std::move(doomed))));

    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);
    EXPECT_EQ(0, rawScalar(raw,
        ("SELECT COUNT(*) FROM batch_scopes WHERE batch_id=" + std::to_string(batch)).c_str()));
    EXPECT_EQ(1, rawScalar(raw, "SELECT COUNT(*) FROM refs"));
    sqlite3_close(raw);

    // 批次仍开着（残批），可正常收尾。
    auto reopened = openOrDie(path);
    ASSERT_TRUE(reopened.closeBatch(batch, DbBatchCounts{}).is_ok());
}

TEST(MjAnalysisProducerRegistryTest, MissingPerClassScopeLeavesClassOwnershipEmpty)
{
    // 负例（#143 3.6 批 3 验收）：full_import 缺逐类作用域行时类归属为空，
    // 不回退到 refs_all / input 等别的锚。
    const auto path = freshDbPath("negative_no_class_rows");
    auto       db   = openOrDie(path);

    auto producer = db.registerProducer(producerInput("poc.bad-import", "BadImport", ""));
    ASSERT_TRUE(producer.is_ok());
    const ca::i64 producerId = producer.unwrap();
    ca::i64       inputId    = 0;
    buildCorpus(db, inputId);

    const auto batchOpened =
        db.openBatch(producerId, DbBatchKind::FullImport, S("skips classes"));
    ASSERT_TRUE(batchOpened.is_ok());
    const ca::i64 batch = batchOpened.unwrap();

    std::vector<DbEntryInput> entries;
    entries.push_back(entryInput("demo/Alpha.class", 512));
    entries.push_back(entryInput("demo/Beta.class", 256));
    entries.push_back(entryInput("demo/Gamma.class", 128));
    ASSERT_TRUE(db.ingestEntries(batch, inputId, std::move(entries)).is_ok());

    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 3,
                                         DbReferenceKind::Method, "demo/Beta", "go", "()V"));
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 5,
                                         DbReferenceKind::Type, "demo/Gamma", "", ""));
    ASSERT_TRUE(db.replaceReferences(batch, std::move(references)).is_ok());
    ASSERT_TRUE(db.closeBatch(batch, DbBatchCounts{}).is_ok());

    // 类事实匿名写入（ingestClasses 未挂批）→ 没有 (2, class_id) 行 →
    // 类归属为空；引用切片照常经 refs_all 解析到本批。
    expectClassOwnership(db, "demo/Alpha", "alpha without class anchor",
                         std::optional<ca::i64>());
    expectClassOwnership(db, "demo/Gamma", "gamma without class anchor",
                         std::optional<ca::i64>());
    expectSliceOwnership(db, DbMemberKind::Method, "demo/Alpha", "run", "()V",
                         "alpha.run via refs_all", std::optional<ca::i64>(batch));
}

TEST(MjAnalysisProducerRegistryTest, OwnershipQueryPlansProbeIndexWithoutFullScans)
{
    // 验收线（#143 批 3）：归属解析 EXPLAIN 无全表扫——两个前缀探都走
    // batch_scopes_owner 覆盖索引，绝不对 batch_scopes 裸扫。
    const auto path = freshDbPath("ownership_plans");
    auto       db   = openOrDie(path);
    auto       producer = db.registerProducer(producerInput("poc.a", "A", ""));
    ASSERT_TRUE(producer.is_ok());
    ca::i64 inputId = 0;
    buildCorpus(db, inputId);
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());

    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);

    EXPECT_EQ(1, rawScalar(raw,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
        "name='batch_scopes_owner'"));

    for (int scopeKind = 0; scopeKind <= 3; ++scopeKind) {
        auto details = rawQueryPlanDetails(raw, kTestScopeProbeSql);
        EXPECT_TRUE(hasSearchOnIndex(details, "batch_scopes_owner")) << scopeKind;
        EXPECT_FALSE(hasBareScopeScan(details)) << scopeKind;
    }
    sqlite3_close(raw);

    // API 面自证可用：挂一批 refs_replace 后，成员切片经 refs_all 探（0,0）
    // 解析到该批。
    auto reopened = openOrDie(path);
    const auto batch = reopened.openBatch(producer.unwrap(), DbBatchKind::RefsReplace);
    ASSERT_TRUE(batch.is_ok());
    std::vector<DbReferenceRecord> refs;
    refs.push_back(alphaRunSlice());
    ASSERT_TRUE(reopened.replaceReferences(batch.unwrap(), std::move(refs)).is_ok());
    auto memberId = reopened.findMember(DbMemberKind::Method, S("demo/Alpha"), S("run"),
                                        S("()V"));
    ASSERT_TRUE(memberId.is_ok());
    auto slice = reopened.refSliceOwnership(DbMemberKind::Method,
                                            std::move(memberId).unwrap()->memberId);
    ASSERT_TRUE(slice.is_ok());
    auto ownership = std::move(slice).unwrap();
    ASSERT_TRUE(ownership.has_value());
    EXPECT_EQ(batch.unwrap(), ownership->batchId);
}

TEST(MjAnalysisProducerRegistryTest, WriteEntriesAttachScopesAndRejectUnknownBatch)
{
    // fail-loud 拒绝面：写入口收到未注册 / 已收尾 / 负数 batchId → Invalid，
    // 不静默匿名写入；0 = 无批写入（事实照常、无作用域行）；同批重复锚幂等。
    const auto path = freshDbPath("write_entry_gates");
    auto       db   = openOrDie(path);

    auto producer = db.registerProducer(producerInput("poc.a", "A", ""));
    ASSERT_TRUE(producer.is_ok());
    const ca::i64 producerId = producer.unwrap();
    ca::i64       inputId    = 0;
    buildCorpus(db, inputId);

    std::vector<DbClassInput> oneClass;
    oneClass.push_back(classInput("demo/Delta", "java/lang/Object"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestClasses(999, std::move(oneClass))));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestClasses(-1, {})));
    std::vector<DbEntryInput> oneEntry;
    oneEntry.push_back(entryInput("demo/Delta.class", 64));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(999, inputId, std::move(oneEntry))));
    std::vector<DbReferenceRecord> oneRef;
    oneRef.push_back(referenceRecord("demo/Alpha", "run", "()V", 9,
                                     DbReferenceKind::Type, "demo/Gamma", "", ""));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferences(999, std::move(oneRef))));
    auto runId = db.findMember(DbMemberKind::Method, S("demo/Alpha"), S("run"), S("()V"));
    ASSERT_TRUE(runId.is_ok());
    std::vector<DbReferenceRecord> oneSlice;
    oneSlice.push_back(alphaRunSlice());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferencesFromMember(
                  999, std::move(runId).unwrap()->memberId, std::move(oneSlice))));

    // 拒绝零写入：无作用域行、无新事实。
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
    {
        sqlite3* raw = openRawReadOnly(path);
        ASSERT_NE(nullptr, raw);
        EXPECT_EQ(0, rawScalar(raw, "SELECT COUNT(*) FROM batch_scopes"));
        EXPECT_EQ(0, rawScalar(raw,
            "SELECT COUNT(*) FROM classes WHERE internal_name='demo/Delta'"));
        EXPECT_EQ(4, rawScalar(raw, "SELECT COUNT(*) FROM refs"));
        sqlite3_close(raw);
    }

    auto reopened = openOrDie(path);
    // 已收尾批拒写。
    const auto closedBatch = reopened.openBatch(producerId, DbBatchKind::FullImport, S("closing"));
    ASSERT_TRUE(closedBatch.is_ok());
    ASSERT_TRUE(reopened.closeBatch(closedBatch.unwrap(), DbBatchCounts{}).is_ok());
    std::vector<DbClassInput> another;
    another.push_back(classInput("demo/Delta", "java/lang/Object"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(reopened.ingestClasses(closedBatch.unwrap(), std::move(another))));

    // 0 = 无批写入：事实照常落库，作用域行一处不留，归属为空。
    std::vector<DbClassInput> anonymous;
    anonymous.push_back(classInput("demo/Delta", "java/lang/Object"));
    ASSERT_TRUE(reopened.ingestClasses(0, std::move(anonymous)).is_ok());
    expectClassOwnership(reopened, "demo/Delta", "anonymous class facts",
                         std::optional<ca::i64>());

    // 同批重复锚幂等：两次 ingestEntries 同输入 + 重叠 ingestClasses，锚集合不膨胀。
    const auto batch = reopened.openBatch(producerId, DbBatchKind::FullImport, S("dup anchors"));
    ASSERT_TRUE(batch.is_ok());
    {
        std::vector<DbEntryInput> entries;
        entries.push_back(entryInput("demo/Alpha.class", 512));
        entries.push_back(entryInput("demo/Beta.class", 256));
        entries.push_back(entryInput("demo/Gamma.class", 128));
        ASSERT_TRUE(reopened.ingestEntries(batch.unwrap(), inputId, std::move(entries)).is_ok());
        std::vector<DbEntryInput> again;
        again.push_back(entryInput("demo/Alpha.class", 512));
        ASSERT_TRUE(reopened.ingestEntries(batch.unwrap(), inputId, std::move(again)).is_ok());

        std::vector<DbClassInput> overlap;
        overlap.push_back(classInput("demo/Alpha", "java/lang/Object"));
        overlap.push_back(classInput("demo/Beta", "java/lang/Object"));
        ASSERT_TRUE(reopened.ingestClasses(batch.unwrap(), std::move(overlap)).is_ok());
        std::vector<DbClassInput> overlap2;
        overlap2.push_back(classInput("demo/Alpha", "java/lang/Object"));
        overlap2.push_back(classInput("demo/Gamma", "java/lang/Object"));
        ASSERT_TRUE(reopened.ingestClasses(batch.unwrap(), std::move(overlap2)).is_ok());
    }
    auto scopes = reopened.batchScopes(batch.unwrap());
    ASSERT_TRUE(scopes.is_ok());
    auto scopeRows = std::move(scopes).unwrap();
    // (0,0) 无、(1,inputId)×1、(2,Alpha)(2,Beta)(2,Gamma) 各 1 → 共 4 行。
    ASSERT_EQ(4U, scopeRows.size());
    EXPECT_EQ(DbBatchScopeKind::Input, scopeRows[0].kind);
    EXPECT_EQ(inputId, scopeRows[0].scopeId);
    EXPECT_EQ(DbBatchScopeKind::Class, scopeRows[1].kind);
    EXPECT_EQ(DbBatchScopeKind::Class, scopeRows[3].kind);
}

TEST(MjAnalysisProducerRegistryTest, RegistryQueryFacesRoundTrip)
{
    const auto path = freshDbPath("query_faces");
    {
        auto db = openOrDie(path);

        auto facade = db.registerProducer(
            producerInput("mjt.pysdk.facade", "Facade", "facade 2.0",
                          DbProducerTrustLevel::Untrusted));
        ASSERT_TRUE(facade.is_ok());
        auto ingest = db.registerProducer(
            producerInput("mjt.workspace.ingest", "Ingest", "ingest 1.0"));
        ASSERT_TRUE(ingest.is_ok());
        const ca::i64 facadeId = facade.unwrap();
        const ca::i64 ingestId = ingest.unwrap();

        ca::i64 inputId = 0;
        buildCorpus(db, inputId);

        // ingest：full_import 批（三锚 + 计数收尾）。
        const auto batch1 = db.openBatch(ingestId, DbBatchKind::FullImport, S("import"));
        ASSERT_TRUE(batch1.is_ok());
        std::vector<DbClassInput> classes;
        {
            DbClassInput alpha = classInput("demo/Alpha", "java/lang/Object");
            alpha.members.push_back(methodInput("run", "()V"));
            alpha.members.push_back(methodInput("go", "()V"));
            alpha.members.push_back(fieldInput("counter", "I"));
            classes.push_back(std::move(alpha));
            DbClassInput beta = classInput("demo/Beta", "java/lang/Object");
            beta.members.push_back(methodInput("go", "()V"));
            classes.push_back(std::move(beta));
            classes.push_back(classInput("demo/Gamma", "java/lang/Object"));
        }
        ASSERT_TRUE(db.ingestClasses(batch1.unwrap(), std::move(classes)).is_ok());
        std::vector<DbEntryInput> entries;
        entries.push_back(entryInput("demo/Alpha.class", 512));
        entries.push_back(entryInput("demo/Beta.class", 256));
        entries.push_back(entryInput("demo/Gamma.class", 128));
        ASSERT_TRUE(db.ingestEntries(batch1.unwrap(), inputId, std::move(entries)).is_ok());
        std::vector<DbReferenceRecord> refs;
        refs.push_back(referenceRecord("demo/Alpha", "run", "()V", 3,
                                       DbReferenceKind::Type, "demo/Gamma", "", ""));
        ASSERT_TRUE(db.replaceReferences(batch1.unwrap(), std::move(refs)).is_ok());
        DbBatchCounts counts;
        counts.classCount  = 3;
        counts.entryCount  = 3;
        counts.memberCount = 4;
        counts.refCount    = 1;
        ASSERT_TRUE(db.closeBatch(batch1.unwrap(), counts).is_ok());

        // facade：一条残批（不收尾）。
        auto residual = db.openBatch(facadeId, DbBatchKind::RenamesWrite, S("residual"));
        ASSERT_TRUE(residual.is_ok());

        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
    }

    auto db = openOrDie(path, AnalysisDbOpenMode::ReadOnly);

    auto producers = db.listProducers();
    ASSERT_TRUE(producers.is_ok());
    {
        auto rows = std::move(producers).unwrap();
        ASSERT_EQ(2U, rows.size());
        EXPECT_EQ("mjt.pysdk.facade", rows[0].key.to_std_string());
        EXPECT_EQ(DbProducerTrustLevel::Untrusted, rows[0].trustLevel);
        EXPECT_EQ("facade 2.0", rows[0].toolFingerprint.to_std_string());
        EXPECT_EQ("mjt.workspace.ingest", rows[1].key.to_std_string());
        EXPECT_EQ(DbProducerTrustLevel::Trusted, rows[1].trustLevel);
        EXPECT_FALSE(rows[0].createdAt.to_std_string().empty());
    }

    // listBatches 全量（含计数、作用域数、生产者 key、残批空 finished_at）。
    auto allBatches = db.listBatches(std::nullopt);
    ASSERT_TRUE(allBatches.is_ok());
    std::vector<DbBatchInfo> batches;
    {
        auto rows = std::move(allBatches).unwrap();
        ASSERT_EQ(2U, rows.size());
        batches = std::move(rows);
    }
    EXPECT_EQ("mjt.workspace.ingest", batches[0].producerKey.to_std_string());
    EXPECT_EQ(DbBatchKind::FullImport, batches[0].kind);
    EXPECT_FALSE(batches[0].finishedAt.to_std_string().empty());
    EXPECT_EQ(3, static_cast<int>(batches[0].classCount));
    EXPECT_EQ(4, static_cast<int>(batches[0].memberCount));
    EXPECT_EQ(1, static_cast<int>(batches[0].refCount));
    EXPECT_EQ(5, static_cast<int>(batches[0].scopeCount));   // refs_all + input + 3 类
    EXPECT_EQ("mjt.pysdk.facade", batches[1].producerKey.to_std_string());
    EXPECT_TRUE(batches[1].finishedAt.to_std_string().empty());
    EXPECT_EQ(0, static_cast<int>(batches[1].scopeCount));

    // 过滤面 + 未注册生产者拒绝。
    const ca::i64 facadeId = batches[1].producerId;
    auto filtered = db.listBatches(std::optional<ca::i64>(facadeId));
    ASSERT_TRUE(filtered.is_ok());
    EXPECT_EQ(1U, std::move(filtered).unwrap().size());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.listBatches(std::optional<ca::i64>(424242))));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.listBatches(std::optional<ca::i64>(0))));

    // batchScopes：(scope_kind, scope_id) 升序；未知批次 Invalid。
    {
        auto scopes = db.batchScopes(batches[0].batchId);
        ASSERT_TRUE(scopes.is_ok());
        auto rows = std::move(scopes).unwrap();
        ASSERT_EQ(5U, rows.size());
        EXPECT_EQ(DbBatchScopeKind::RefsAll, rows[0].kind);
        EXPECT_EQ(0, static_cast<int>(rows[0].scopeId));
        EXPECT_EQ(DbBatchScopeKind::Input, rows[1].kind);
        EXPECT_EQ(DbBatchScopeKind::Class, rows[2].kind);
        EXPECT_EQ(DbBatchScopeKind::Class, rows[4].kind);
        EXPECT_TRUE(rows[2].scopeId < rows[3].scopeId);
    }
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.batchScopes(424242)));

    // producerStats：批次数、累计写入计数、首末批时间（残批不计 last）。
    auto stats = db.producerStats();
    ASSERT_TRUE(stats.is_ok());
    auto statRows = std::move(stats).unwrap();
    ASSERT_EQ(2U, statRows.size());
    EXPECT_EQ("mjt.pysdk.facade", statRows[0].key.to_std_string());
    EXPECT_EQ(1, static_cast<int>(statRows[0].batchCount));
    EXPECT_EQ(0, static_cast<int>(statRows[0].refCount));
    // 残批有 started_at（first 计入），无 finished_at（last 不计）。
    EXPECT_FALSE(statRows[0].firstBatchAt.to_std_string().empty());
    EXPECT_TRUE(statRows[0].lastBatchAt.to_std_string().empty());
    EXPECT_EQ("mjt.workspace.ingest", statRows[1].key.to_std_string());
    EXPECT_EQ(1, static_cast<int>(statRows[1].batchCount));
    EXPECT_EQ(3, static_cast<int>(statRows[1].classCount));
    EXPECT_EQ(3, static_cast<int>(statRows[1].entryCount));
    EXPECT_EQ(4, static_cast<int>(statRows[1].memberCount));
    EXPECT_EQ(1, static_cast<int>(statRows[1].refCount));
    EXPECT_FALSE(statRows[1].firstBatchAt.to_std_string().empty());
    EXPECT_FALSE(statRows[1].lastBatchAt.to_std_string().empty());
    EXPECT_EQ(statRows[1].firstBatchAt.to_std_string(),
              statRows[1].lastBatchAt.to_std_string());
}
