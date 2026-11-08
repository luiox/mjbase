/// @file        analysis_db_test.cpp
/// @brief       AnalysisDb 基础层：建库/重开、稳定身份上的重命名覆盖、
///              token 约束、只读拒写、原子替换与非法输入零写入；
///              v3 条目身份层的输入/条目登记、重导入匹配与墓碑语义；
///              v5 引用目标侧 id 化的行形状、CHECK 门、登记时序等价、
///              字节语义与查询计划回归；v8 方法版本指纹层的记录/查询
///              闭环、版本历史与当前指针唯一、批量原子性、指纹字节往返；
///              v9 版本门对 v8 库的 fail-loud 拒绝（闭包面单测见
///              reachability_closure_test.cpp）。
#include <gtest/gtest.h>

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
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
    const auto path = test::getTempOutPath("analysis_db_" + name + ".mjdb");
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

}   // namespace

TEST(MjAnalysisDbTest, CreatesSchemaAndReopensReadOnly)
{
    const auto path = freshDbPath("create");
    {
        auto db = openOrDie(path);
        EXPECT_TRUE(db.isOpen());
        EXPECT_FALSE(db.isReadOnly());
        EXPECT_EQ(kAnalysisDbSchemaVersion, db.schemaVersion());
        EXPECT_TRUE(db.save().is_ok());
        EXPECT_TRUE(db.close().is_ok());
        EXPECT_FALSE(db.isOpen());
    }

    ASSERT_TRUE(std::filesystem::is_regular_file(path));
    EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
    EXPECT_FALSE(std::filesystem::exists(path.string() + "-shm"));

    auto readOnly = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    EXPECT_TRUE(readOnly.isReadOnly());
    EXPECT_EQ(kAnalysisDbSchemaVersion, readOnly.schemaVersion());
}

TEST(MjAnalysisDbTest, RenamesPersistAcrossReopen)
{
    const auto path = freshDbPath("roundtrip");
    {
        auto db = openOrDie(path);
        ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1"), S("deadbeef")).is_ok());
        ASSERT_TRUE(db.setMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"),
                                       S("Method_1"), S("00000001"))
                        .is_ok());
        ASSERT_TRUE(db.setMemberRename(DbMemberKind::Field, S("a/A"), S("run"), S("()V"),
                                       S("Field_1"), S("00000002"))
                        .is_ok());
        EXPECT_TRUE(db.save().is_ok());
    }

    auto reopened = openOrDie(path, AnalysisDbOpenMode::ReadOnly);

    auto classFound = reopened.findClassRename(S("a/A"));
    ASSERT_TRUE(classFound.is_ok());
    auto classRecordValue = std::move(classFound).unwrap();
    ASSERT_TRUE(classRecordValue.has_value());
    EXPECT_EQ("Class_1", classRecordValue->shadow.to_std_string());
    EXPECT_EQ("deadbeef", classRecordValue->token.to_std_string());
    EXPECT_EQ("a/A", classRecordValue->owner.to_std_string());
    EXPECT_FALSE(classRecordValue->updatedAt.is_empty());

    auto methodFound = reopened.findMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"));
    ASSERT_TRUE(methodFound.is_ok());
    auto methodRecordValue = std::move(methodFound).unwrap();
    ASSERT_TRUE(methodRecordValue.has_value());
    EXPECT_EQ(DbMemberKind::Method, methodRecordValue->memberKind);
    EXPECT_EQ("Method_1", methodRecordValue->shadow.to_std_string());

    auto fieldFound = reopened.findMemberRename(DbMemberKind::Field, S("a/A"), S("run"), S("()V"));
    ASSERT_TRUE(fieldFound.is_ok());
    auto fieldRecordValue = std::move(fieldFound).unwrap();
    ASSERT_TRUE(fieldRecordValue.has_value());
    EXPECT_EQ(DbMemberKind::Field, fieldRecordValue->memberKind);
    EXPECT_EQ("Field_1", fieldRecordValue->shadow.to_std_string());

    auto all = reopened.listRenames();
    ASSERT_TRUE(all.is_ok());
    EXPECT_EQ(3U, std::move(all).unwrap().size());
}

TEST(MjAnalysisDbTest, OverwriteKeepsOneRow)
{
    const auto path = freshDbPath("overwrite");
    auto       db   = openOrDie(path);

    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1"), S("deadbeef")).is_ok());
    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_2"), S("deadbeef")).is_ok());

    auto found = db.findClassRename(S("a/A"));
    ASSERT_TRUE(found.is_ok());
    auto value = std::move(found).unwrap();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ("Class_2", value->shadow.to_std_string());

    auto all = db.listRenames();
    ASSERT_TRUE(all.is_ok());
    EXPECT_EQ(1U, std::move(all).unwrap().size());
}

TEST(MjAnalysisDbTest, ClearDropsRenameButKeepsIdentityUsable)
{
    const auto path = freshDbPath("clear");
    auto       db   = openOrDie(path);

    ASSERT_TRUE(db.setMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"),
                                   S("Method_1"), S("00000001"))
                    .is_ok());

    auto cleared = db.clearMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"));
    ASSERT_TRUE(cleared.is_ok());
    EXPECT_TRUE(cleared.unwrap());

    auto missing = db.findMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"));
    ASSERT_TRUE(missing.is_ok());
    EXPECT_FALSE(std::move(missing).unwrap().has_value());

    auto again = db.clearMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"));
    ASSERT_TRUE(again.is_ok());
    EXPECT_FALSE(again.unwrap());

    EXPECT_TRUE(db.setClassRename(S("a/A"), S("Class_1")).is_ok());
    auto classCleared = db.clearClassRename(S("a/A"));
    ASSERT_TRUE(classCleared.is_ok());
    EXPECT_TRUE(classCleared.unwrap());
}

TEST(MjAnalysisDbTest, InvalidArgumentsRejected)
{
    const auto path = freshDbPath("invalid");
    auto       db   = openOrDie(path);

    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setClassRename(S(""), S("Class_1"))));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setClassRename(S("a/A"), S(""))));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setClassRename(S("a/A"), S("Class_1"), S("DEADBEEF"))));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setClassRename(S("a/A"), S("Class_1"), S("deadbee"))));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setMemberRename(static_cast<DbMemberKind>(7), S("a/A"), S("run"),
                                               S("()V"), S("Method_1"))));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S(""),
                                               S("Method_1"))));
    EXPECT_TRUE(db.listRenames().is_ok());
    EXPECT_EQ(0U, db.listRenames().unwrap().size());
}

TEST(MjAnalysisDbTest, DuplicateTokenRejectedWithoutClobbering)
{
    const auto path = freshDbPath("token");
    auto       db   = openOrDie(path);

    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1"), S("deadbeef")).is_ok());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setClassRename(S("b/B"), S("Class_2"), S("deadbeef"))));

    auto found = db.findClassRename(S("a/A"));
    ASSERT_TRUE(found.is_ok());
    auto value = std::move(found).unwrap();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ("Class_1", value->shadow.to_std_string());

    auto all = db.listRenames();
    ASSERT_TRUE(all.is_ok());
    EXPECT_EQ(1U, std::move(all).unwrap().size());
}

TEST(MjAnalysisDbTest, ReplaceRenamesIsAtomicOnValidationFailure)
{
    const auto path = freshDbPath("replace_invalid");
    auto       db   = openOrDie(path);
    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1")).is_ok());

    std::vector<DbRenameRecord> records;
    records.push_back(classRecord("b/B", "Class_2", "00000001"));
    records.push_back(classRecord("c/C", "Class_3", "00000001"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceRenames(std::move(records))));

    auto all = db.listRenames();
    ASSERT_TRUE(all.is_ok());
    auto rows = std::move(all).unwrap();
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ("a/A", rows.front().owner.to_std_string());
}

TEST(MjAnalysisDbTest, ReplaceRenamesSwapsAllRows)
{
    const auto path = freshDbPath("replace");
    auto       db   = openOrDie(path);
    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1")).is_ok());

    std::vector<DbRenameRecord> records;
    records.push_back(classRecord("b/B", "Class_2", "00000001"));
    records.push_back(memberRecord(DbMemberKind::Method, "b/B", "run", "()V", "Method_1",
                                   "00000002"));
    ASSERT_TRUE(db.replaceRenames(std::move(records)).is_ok());

    auto all = db.listRenames();
    ASSERT_TRUE(all.is_ok());
    auto rows = std::move(all).unwrap();
    ASSERT_EQ(2U, rows.size());
    EXPECT_EQ("b/B", rows[0].owner.to_std_string());
    EXPECT_EQ("b/B", rows[1].owner.to_std_string());

    auto old = db.findClassRename(S("a/A"));
    ASSERT_TRUE(old.is_ok());
    EXPECT_FALSE(std::move(old).unwrap().has_value());
}

TEST(MjAnalysisDbTest, BatchSetRenamesUpsertsWithoutDeletingOthers)
{
    const auto path = freshDbPath("batch_upsert");
    auto       db   = openOrDie(path);

    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1"), S("deadbeef")).is_ok());

    std::vector<DbRenameRecord> records;
    records.push_back(classRecord("a/A", "Class_1b", "deadbeef"));
    records.push_back(classRecord("b/B", "Class_2", "00000001"));
    records.push_back(
        memberRecord(DbMemberKind::Method, "b/B", "run", "()V", "Method_1", "00000002"));
    ASSERT_TRUE(db.setRenames(std::move(records)).is_ok());

    auto all = db.listRenames();
    ASSERT_TRUE(all.is_ok());
    EXPECT_EQ(3U, std::move(all).unwrap().size());

    auto found = db.findClassRename(S("a/A"));
    ASSERT_TRUE(found.is_ok());
    auto value = std::move(found).unwrap();
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ("Class_1b", value->shadow.to_std_string());
}

TEST(MjAnalysisDbTest, BatchSetRenamesIsAtomicOnDuplicateTargets)
{
    const auto path = freshDbPath("batch_duplicate");
    auto       db   = openOrDie(path);
    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1")).is_ok());

    std::vector<DbRenameRecord> records;
    records.push_back(classRecord("b/B", "Class_2", "00000001"));
    records.push_back(classRecord("b/B", "Class_3", "00000002"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setRenames(std::move(records))));

    auto all = db.listRenames();
    ASSERT_TRUE(all.is_ok());
    EXPECT_EQ(1U, std::move(all).unwrap().size());

    auto missing = db.findClassRename(S("b/B"));
    ASSERT_TRUE(missing.is_ok());
    EXPECT_FALSE(std::move(missing).unwrap().has_value());
}

TEST(MjAnalysisDbTest, BatchSetRenamesRollsBackOnTokenConflict)
{
    const auto path = freshDbPath("batch_token_conflict");
    auto       db   = openOrDie(path);
    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1"), S("deadbeef")).is_ok());

    std::vector<DbRenameRecord> records;
    records.push_back(classRecord("b/B", "Class_2", "00000001"));
    records.push_back(classRecord("c/C", "Class_3", "deadbeef"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.setRenames(std::move(records))));

    auto all = db.listRenames();
    ASSERT_TRUE(all.is_ok());
    auto rows = std::move(all).unwrap();
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ("a/A", rows.front().owner.to_std_string());

    auto missing = db.findClassRename(S("b/B"));
    ASSERT_TRUE(missing.is_ok());
    EXPECT_FALSE(std::move(missing).unwrap().has_value());
}

TEST(MjAnalysisDbTest, MigrateClassIdentityKeepsIdAndOverlayIntact)
{
    const auto path = freshDbPath("migrate_class");
    auto       db   = openOrDie(path);

    DbClassInput input;
    input.internalName = Utf8String::from_cstr("a/A");
    input.superName    = Utf8String::from_cstr("java/lang/Object");
    input.interfaces.push_back(Utf8String::from_cstr("a/I"));
    DbMemberInput run;
    run.kind       = DbMemberKind::Method;
    run.name       = Utf8String::from_cstr("run");
    run.descriptor = Utf8String::from_cstr("(La/A;)V");
    input.members.push_back(std::move(run));
    DbClassInput implementor;
    implementor.internalName = Utf8String::from_cstr("a/Impl");
    implementor.superName    = Utf8String::from_cstr("a/A");
    implementor.interfaces.push_back(Utf8String::from_cstr("a/A"));
    std::vector<DbClassInput> classes;
    classes.push_back(std::move(input));
    classes.push_back(std::move(implementor));
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1"), S("deadbeef")).is_ok());
    ASSERT_TRUE(db.setMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("(La/A;)V"),
                                   S("Method_1"), S("00000001"))
                    .is_ok());

    std::vector<DbIdentityRename> migrations;
    DbIdentityRename             spec;
    spec.target  = RenameTargetKind::Class;
    spec.name    = Utf8String::from_cstr("a/A");
    spec.newName = Utf8String::from_cstr("b/B");
    migrations.push_back(std::move(spec));
    auto migrated = db.migrateIdentities(std::move(migrations));
    ASSERT_TRUE(migrated.is_ok());
    auto stats = std::move(migrated).unwrap();
    EXPECT_EQ(1U, stats.migratedClasses);
    EXPECT_EQ(0U, stats.migratedMembers);
    EXPECT_EQ(0U, stats.skipped);

    //覆盖层按稳定 ID 键控：shadow 与 token 原样，查找键换到新内部名。
    auto overlay = db.findClassRename(S("b/B"));
    ASSERT_TRUE(overlay.is_ok());
    auto record = std::move(overlay).unwrap();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ("Class_1", record->shadow.to_std_string());
    EXPECT_EQ("deadbeef", record->token.to_std_string());

    auto gone = db.findClassRename(S("a/A"));
    ASSERT_TRUE(gone.is_ok());
    EXPECT_FALSE(std::move(gone).unwrap().has_value());

    //成员行 class_id 不动：owner 键经 join 走新类名，overlay 原样。
    auto memberOverlay =
        db.findMemberRename(DbMemberKind::Method, S("b/B"), S("run"), S("(La/A;)V"));
    ASSERT_TRUE(memberOverlay.is_ok());
    auto memberRecord = std::move(memberOverlay).unwrap();
    ASSERT_TRUE(memberRecord.has_value());
    EXPECT_EQ("Method_1", memberRecord->shadow.to_std_string());
    EXPECT_EQ("00000001", memberRecord->token.to_std_string());

    //名字属性的非规范化副本连带：super_name 与 interfaces 跟随新名。
    auto impl = db.findClass(S("a/Impl"));
    ASSERT_TRUE(impl.is_ok());
    auto implInfo = std::move(impl).unwrap();
    ASSERT_TRUE(implInfo.has_value());
    EXPECT_EQ("b/B", implInfo->superName.to_std_string());
    ASSERT_EQ(1U, implInfo->interfaces.size());
    EXPECT_EQ("b/B", implInfo->interfaces.front().to_std_string());

    //undo 对称：反向规格迁移回去，overlay 仍原样。
    std::vector<DbIdentityRename> revert;
    DbIdentityRename             back;
    back.target  = RenameTargetKind::Class;
    back.name    = Utf8String::from_cstr("b/B");
    back.newName = Utf8String::from_cstr("a/A");
    revert.push_back(std::move(back));
    auto reverted = db.migrateIdentities(std::move(revert));
    ASSERT_TRUE(reverted.is_ok());
    EXPECT_EQ(1U, std::move(reverted).unwrap().migratedClasses);
    auto roundTrip = db.findClassRename(S("a/A"));
    ASSERT_TRUE(roundTrip.is_ok());
    auto roundTripRecord = std::move(roundTrip).unwrap();
    ASSERT_TRUE(roundTripRecord.has_value());
    EXPECT_EQ("Class_1", roundTripRecord->shadow.to_std_string());
    EXPECT_EQ("deadbeef", roundTripRecord->token.to_std_string());
}

TEST(MjAnalysisDbTest, MigrateMemberIdentityKeepsIdAndRewritesDescriptor)
{
    const auto path = freshDbPath("migrate_member");
    auto       db   = openOrDie(path);

    DbClassInput input;
    input.internalName = Utf8String::from_cstr("a/A");
    DbMemberInput run;
    run.kind       = DbMemberKind::Method;
    run.name       = Utf8String::from_cstr("run");
    run.descriptor = Utf8String::from_cstr("(La/Gift;)V");
    input.members.push_back(std::move(run));
    DbMemberInput value;
    value.kind       = DbMemberKind::Field;
    value.name       = Utf8String::from_cstr("value");
    value.descriptor = Utf8String::from_cstr("La/Gift;");
    input.members.push_back(std::move(value));
    std::vector<DbClassInput> classes;
    classes.push_back(std::move(input));
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    ASSERT_TRUE(db.setMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("(La/Gift;)V"),
                                   S("Method_1"), S("00000001"))
                    .is_ok());
    ASSERT_TRUE(db.setMemberRename(DbMemberKind::Field, S("a/A"), S("value"), S("La/Gift;"),
                                   S("Field_1"), S("00000002"))
                    .is_ok());

    std::vector<DbIdentityRename> migrations;
    DbIdentityRename             method;
    method.target     = RenameTargetKind::Member;
    method.memberKind = DbMemberKind::Method;
    method.owner      = Utf8String::from_cstr("a/A");
    method.name       = Utf8String::from_cstr("run");
    method.desc       = Utf8String::from_cstr("(La/Gift;)V");
    method.newName    = Utf8String::from_cstr("walk");
    method.newDesc    = Utf8String::from_cstr("(La/Present;)V");
    migrations.push_back(std::move(method));
    DbIdentityRename field;
    field.target     = RenameTargetKind::Member;
    field.memberKind = DbMemberKind::Field;
    field.owner      = Utf8String::from_cstr("a/A");
    field.name       = Utf8String::from_cstr("value");
    field.desc       = Utf8String::from_cstr("La/Gift;");
    field.newName    = Utf8String::from_cstr("payload");
    //newDesc 空 = 保持旧描述符。
    migrations.push_back(std::move(field));
    auto migrated = db.migrateIdentities(std::move(migrations));
    ASSERT_TRUE(migrated.is_ok());
    auto stats = std::move(migrated).unwrap();
    EXPECT_EQ(0U, stats.migratedClasses);
    EXPECT_EQ(2U, stats.migratedMembers);

    auto methodOverlay =
        db.findMemberRename(DbMemberKind::Method, S("a/A"), S("walk"), S("(La/Present;)V"));
    ASSERT_TRUE(methodOverlay.is_ok());
    auto methodRecord = std::move(methodOverlay).unwrap();
    ASSERT_TRUE(methodRecord.has_value());
    EXPECT_EQ("Method_1", methodRecord->shadow.to_std_string());
    EXPECT_EQ("00000001", methodRecord->token.to_std_string());

    auto fieldOverlay =
        db.findMemberRename(DbMemberKind::Field, S("a/A"), S("payload"), S("La/Gift;"));
    ASSERT_TRUE(fieldOverlay.is_ok());
    auto fieldRecord = std::move(fieldOverlay).unwrap();
    ASSERT_TRUE(fieldRecord.has_value());
    EXPECT_EQ("Field_1", fieldRecord->shadow.to_std_string());
}

TEST(MjAnalysisDbTest, MigrateIdentitiesRejectsCollisionAtomically)
{
    const auto path = freshDbPath("migrate_collision");
    auto       db   = openOrDie(path);

    DbClassInput first;
    first.internalName = Utf8String::from_cstr("a/A");
    DbClassInput second;
    second.internalName = Utf8String::from_cstr("b/B");
    std::vector<DbClassInput> classes;
    classes.push_back(std::move(first));
    classes.push_back(std::move(second));
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());
    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1"), S("deadbeef")).is_ok());

    std::vector<DbIdentityRename> migrations;
    DbIdentityRename             spec;
    spec.target  = RenameTargetKind::Class;
    spec.name    = Utf8String::from_cstr("a/A");
    spec.newName = Utf8String::from_cstr("b/B");
    migrations.push_back(std::move(spec));
    auto failed = db.migrateIdentities(std::move(migrations));
    ASSERT_TRUE(failed.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(std::move(failed)));

    //整批回滚：旧身份与 overlay 全部原样。
    auto intact = db.findClassRename(S("a/A"));
    ASSERT_TRUE(intact.is_ok());
    auto record = std::move(intact).unwrap();
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ("Class_1", record->shadow.to_std_string());
}

TEST(MjAnalysisDbTest, MigrateIdentitiesSkipsUnregisteredAndRejectsDuplicates)
{
    const auto path = freshDbPath("migrate_skip");
    auto       db   = openOrDie(path);

    DbClassInput input;
    input.internalName = Utf8String::from_cstr("a/A");
    std::vector<DbClassInput> classes;
    classes.push_back(std::move(input));
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    std::vector<DbIdentityRename> migrations;
    DbIdentityRename             absent;
    absent.target  = RenameTargetKind::Class;
    absent.name    = Utf8String::from_cstr("x/X");
    absent.newName = Utf8String::from_cstr("y/Y");
    migrations.push_back(std::move(absent));
    DbIdentityRename present;
    present.target  = RenameTargetKind::Class;
    present.name    = Utf8String::from_cstr("a/A");
    present.newName = Utf8String::from_cstr("a/A2");
    migrations.push_back(std::move(present));
    auto migrated = db.migrateIdentities(std::move(migrations));
    ASSERT_TRUE(migrated.is_ok());
    auto stats = std::move(migrated).unwrap();
    EXPECT_EQ(1U, stats.migratedClasses);
    EXPECT_EQ(1U, stats.skipped);

    auto found = db.findClass(S("a/A2"));
    ASSERT_TRUE(found.is_ok());
    EXPECT_TRUE(std::move(found).unwrap().has_value());

    std::vector<DbIdentityRename> duplicates;
    DbIdentityRename             forward;
    forward.target  = RenameTargetKind::Class;
    forward.name    = Utf8String::from_cstr("a/A2");
    forward.newName = Utf8String::from_cstr("a/A3");
    duplicates.push_back(std::move(forward));
    DbIdentityRename forwardAgain;
    forwardAgain.target  = RenameTargetKind::Class;
    forwardAgain.name    = Utf8String::from_cstr("a/A2");
    forwardAgain.newName = Utf8String::from_cstr("a/A3");
    duplicates.push_back(std::move(forwardAgain));
    auto rejected = db.migrateIdentities(std::move(duplicates));
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(std::move(rejected)));
}

TEST(MjAnalysisDbTest, BatchSetRenamesHandlesManyRecords)
{
    const auto path = freshDbPath("batch_many");
    auto       db   = openOrDie(path);

    constexpr int               kCount = 2000;
    std::vector<DbRenameRecord> records;
    records.reserve(kCount);
    char owner[32]  = {};
    char shadow[32] = {};
    char token[16]  = {};
    for (int i = 0; i < kCount; ++i) {
        std::snprintf(owner, sizeof(owner), "pkg/C%04d", i);
        std::snprintf(shadow, sizeof(shadow), "Class_%04d", i);
        std::snprintf(token, sizeof(token), "%08x", i);
        records.push_back(classRecord(owner, shadow, token));
    }
    ASSERT_TRUE(db.setRenames(std::move(records)).is_ok());

    auto all = db.listRenames();
    ASSERT_TRUE(all.is_ok());
    EXPECT_EQ(static_cast<ca::usize>(kCount), std::move(all).unwrap().size());
}

TEST(MjAnalysisDbTest, ReverseLookupByShadow)
{
    const auto path = freshDbPath("reverse");
    auto       db   = openOrDie(path);

    ASSERT_TRUE(db.setClassRename(S("a/A"), S("Shared"), S("deadbeef")).is_ok());
    ASSERT_TRUE(db.setClassRename(S("b/B"), S("Shared"), S("00000001")).is_ok());
    ASSERT_TRUE(db.setMemberRename(DbMemberKind::Method, S("a/A"), S("run"), S("()V"),
                                   S("Method_1"), S("00000002"))
                    .is_ok());
    ASSERT_TRUE(db.setMemberRename(DbMemberKind::Field, S("b/B"), S("run"), S("()V"),
                                   S("Field_1"), S("00000003"))
                    .is_ok());

    auto classes = db.findRenamesByShadow(S("Shared"));
    ASSERT_TRUE(classes.is_ok());
    auto classRows = std::move(classes).unwrap();
    ASSERT_EQ(2U, classRows.size());
    EXPECT_EQ(RenameTargetKind::Class, classRows[0].target);
    EXPECT_EQ("a/A", classRows[0].owner.to_std_string());
    EXPECT_EQ("b/B", classRows[1].owner.to_std_string());

    auto methods = db.findRenamesByShadow(S("Method_1"));
    ASSERT_TRUE(methods.is_ok());
    auto methodRows = std::move(methods).unwrap();
    ASSERT_EQ(1U, methodRows.size());
    EXPECT_EQ(RenameTargetKind::Member, methodRows[0].target);
    EXPECT_EQ(DbMemberKind::Method, methodRows[0].memberKind);
    EXPECT_EQ("a/A", methodRows[0].owner.to_std_string());
    EXPECT_EQ("run", methodRows[0].name.to_std_string());
    EXPECT_EQ("()V", methodRows[0].desc.to_std_string());

    auto missing = db.findRenamesByShadow(S("Nope"));
    ASSERT_TRUE(missing.is_ok());
    EXPECT_TRUE(std::move(missing).unwrap().empty());

    std::vector<Utf8StringRef> shadows{S("Shared"), S("Method_1"), S("Field_1")};
    auto                       batch = db.findRenamesByShadows(shadows);
    ASSERT_TRUE(batch.is_ok());
    EXPECT_EQ(4U, std::move(batch).unwrap().size());
}

TEST(MjAnalysisDbTest, ReadOnlyRejectsWrites)
{
    const auto path = freshDbPath("readonly");
    {
        auto db = openOrDie(path);
        ASSERT_TRUE(db.setClassRename(S("a/A"), S("Class_1")).is_ok());
        ASSERT_TRUE(db.save().is_ok());
    }

    auto readOnly = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly,
              takeErrorKind(readOnly.setClassRename(S("b/B"), S("Class_2"))));
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly,
              takeErrorKind(readOnly.clearClassRename(S("a/A"))));

    std::vector<DbRenameRecord> records;
    records.push_back(classRecord("b/B", "Class_2"));
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly,
              takeErrorKind(readOnly.replaceRenames(std::move(records))));

    std::vector<DbRenameRecord> batch;
    batch.push_back(classRecord("b/B", "Class_2"));
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly,
              takeErrorKind(readOnly.setRenames(std::move(batch))));

    auto reverse = readOnly.findRenamesByShadow(S("Class_1"));
    ASSERT_TRUE(reverse.is_ok());
    EXPECT_EQ(1U, std::move(reverse).unwrap().size());
}

TEST(MjAnalysisDbTest, RejectsNonDatabaseFile)
{
    const auto path = freshDbPath("not_a_db");
    {
        std::ofstream out(path, std::ios::binary);
        out << "this is not an sqlite database";
    }

    auto result = AnalysisDb::open(refOf(path.string()));
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::Open, std::move(result).unwrap_err().kind);
}

TEST(MjAnalysisDbTest, ReadOnlyMissingPathFails)
{
    const auto path = freshDbPath("missing");
    auto       result = AnalysisDb::open(refOf(path.string()), AnalysisDbOpenMode::ReadOnly);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::Open, std::move(result).unwrap_err().kind);
}

namespace {

DbMemberInput fieldInput(const char* name, const char* desc, ca::i32 access = 0x0001)
{
    DbMemberInput member;
    member.kind       = DbMemberKind::Field;
    member.name       = Utf8String::from_cstr(name);
    member.descriptor = Utf8String::from_cstr(desc);
    member.access     = access;
    return member;
}

DbMemberInput methodInput(const char* name, const char* desc, ca::i32 access = 0x0001)
{
    DbMemberInput member;
    member.kind       = DbMemberKind::Method;
    member.name       = Utf8String::from_cstr(name);
    member.descriptor = Utf8String::from_cstr(desc);
    member.access     = access;
    return member;
}

DbClassInput classInput(const char* name, const char* superName)
{
    DbClassInput input;
    input.internalName = Utf8String::from_cstr(name);
    input.superName    = Utf8String::from_cstr(superName);
    return input;
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

/// @brief 引用行逐字段比较（DbReferenceRecord 无 operator==）。
void expectSameRecords(const std::vector<DbReferenceRecord>& expected,
                       const std::vector<DbReferenceRecord>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (ca::usize i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i].sourceClass.to_std_string(), actual[i].sourceClass.to_std_string());
        EXPECT_EQ(expected[i].sourceMethod.to_std_string(),
                  actual[i].sourceMethod.to_std_string());
        EXPECT_EQ(expected[i].sourceDescriptor.to_std_string(),
                  actual[i].sourceDescriptor.to_std_string());
        EXPECT_EQ(expected[i].insnIndex, actual[i].insnIndex);
        EXPECT_EQ(expected[i].kind, actual[i].kind);
        EXPECT_EQ(expected[i].targetClass.to_std_string(), actual[i].targetClass.to_std_string());
        EXPECT_EQ(expected[i].targetName.to_std_string(), actual[i].targetName.to_std_string());
        EXPECT_EQ(expected[i].targetDescriptor.to_std_string(),
                  actual[i].targetDescriptor.to_std_string());
    }
}

/// @brief 期望基线行集：记录只移不拷（Utf8String 无拷贝），逐个 move 进
///        向量——braced initializer_list 对只移类型走拷贝构造，不可用。
template <typename... Rest>
std::vector<DbReferenceRecord> recordSet(DbReferenceRecord first, Rest... rest)
{
    std::vector<DbReferenceRecord> rows;
    rows.push_back(std::move(first));
    (rows.push_back(std::move(rest)), ...);
    return rows;
}

/// @brief 类名列表逐项比较（分页窗口与全量基线对拍用）。
void expectSameNames(const std::vector<Utf8String>& expected,
                     const std::vector<Utf8String>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (ca::usize i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i].to_std_string(), actual[i].to_std_string());
    }
}

}   // namespace

TEST(MjAnalysisDbTest, IngestsHierarchyMembersAndQueries)
{
    const auto path = freshDbPath("ingest");
    auto       db   = openOrDie(path);

    DbClassInput marker = classInput("demo/Marker", "");
    marker.access       = 0x0200;

    DbClassInput base = classInput("demo/Base", "java/lang/Object");
    base.access       = 0x0020;
    base.interfaces.push_back(Utf8String::from_cstr("demo/Marker"));
    base.members.push_back(fieldInput("counter", "I"));
    base.members.push_back(methodInput("run", "()V"));
    base.members.push_back(methodInput("id", "()I"));

    DbClassInput child = classInput("demo/Child", "demo/Base");
    child.members.push_back(methodInput("run", "()V"));

    std::vector<DbClassInput> inputs;
    inputs.push_back(std::move(marker));
    inputs.push_back(std::move(base));
    inputs.push_back(std::move(child));
    auto ingested = db.ingestClasses(0, std::move(inputs));
    ASSERT_TRUE(ingested.is_ok());
    EXPECT_EQ(3U, ingested.unwrap());

    auto found = db.findClass(S("demo/Base"));
    ASSERT_TRUE(found.is_ok());
    auto baseInfo = std::move(found).unwrap();
    ASSERT_TRUE(baseInfo.has_value());
    EXPECT_GT(baseInfo->classId, 0);
    EXPECT_EQ("demo/Base", baseInfo->internalName.to_std_string());
    EXPECT_EQ("java/lang/Object", baseInfo->superName.to_std_string());
    ASSERT_EQ(1U, baseInfo->interfaces.size());
    EXPECT_EQ("demo/Marker", baseInfo->interfaces[0].to_std_string());

    auto members = db.listMembers(S("demo/Base"));
    ASSERT_TRUE(members.is_ok());
    auto memberList = std::move(members).unwrap();
    ASSERT_EQ(3U, memberList.size());
    EXPECT_EQ(DbMemberKind::Field, memberList[0].kind);
    EXPECT_EQ("counter", memberList[0].name.to_std_string());
    EXPECT_EQ("id", memberList[1].name.to_std_string());
    EXPECT_EQ("run", memberList[2].name.to_std_string());

    auto member = db.findMember(DbMemberKind::Method, S("demo/Base"), S("run"), S("()V"));
    ASSERT_TRUE(member.is_ok());
    ASSERT_TRUE(std::move(member).unwrap().has_value());

    auto chain = db.superChain(S("demo/Child"));
    ASSERT_TRUE(chain.is_ok());
    auto chainNames = std::move(chain).unwrap();
    ASSERT_EQ(3U, chainNames.size());
    EXPECT_EQ("demo/Child", chainNames[0].to_std_string());
    EXPECT_EQ("demo/Base", chainNames[1].to_std_string());
    EXPECT_EQ("java/lang/Object", chainNames[2].to_std_string());

    auto direct = db.directSubclasses(S("demo/Base"));
    ASSERT_TRUE(direct.is_ok());
    auto directNames = std::move(direct).unwrap();
    ASSERT_EQ(1U, directNames.size());
    EXPECT_EQ("demo/Child", directNames[0].to_std_string());

    auto all = db.allSubclasses(S("demo/Base"));
    ASSERT_TRUE(all.is_ok());
    auto allNames = std::move(all).unwrap();
    ASSERT_EQ(1U, allNames.size());
    EXPECT_EQ("demo/Child", allNames[0].to_std_string());

    auto implementors = db.implementors(S("demo/Marker"));
    ASSERT_TRUE(implementors.is_ok());
    auto implementorNames = std::move(implementors).unwrap();
    ASSERT_EQ(2U, implementorNames.size());
    EXPECT_EQ("demo/Base", implementorNames[0].to_std_string());
    EXPECT_EQ("demo/Child", implementorNames[1].to_std_string());

    auto unknownChain = db.superChain(S("demo/Unknown"));
    ASSERT_TRUE(unknownChain.is_ok());
    EXPECT_TRUE(std::move(unknownChain).unwrap().empty());
}

TEST(MjAnalysisDbTest, ClassIngestRejectsDuplicateBatchWithoutWriting)
{
    const auto path = freshDbPath("ingest_atomic");
    auto       db   = openOrDie(path);

    DbClassInput base = classInput("demo/Base", "java/lang/Object");
    base.access       = 0x0001;
    std::vector<DbClassInput> first;
    first.push_back(std::move(base));
    ASSERT_TRUE(db.ingestClasses(0, std::move(first)).is_ok());

    DbClassInput duplicate = classInput("demo/Base", "java/lang/Object");
    duplicate.access       = 0x0010;
    std::vector<DbClassInput> second;
    second.push_back(classInput("demo/Other", "java/lang/Object"));
    second.push_back(std::move(duplicate));
    second.push_back(classInput("demo/Other", "java/lang/Object"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.ingestClasses(0, std::move(second))));

    auto found = db.findClass(S("demo/Base"));
    ASSERT_TRUE(found.is_ok());
    ASSERT_TRUE(std::move(found).unwrap().has_value());
    auto other = db.findClass(S("demo/Other"));
    ASSERT_TRUE(other.is_ok());
    EXPECT_FALSE(std::move(other).unwrap().has_value());
}

TEST(MjAnalysisDbTest, ReferencesReplaceAtomicallyAndQueryBySourceAndTarget)
{
    const auto path = freshDbPath("refs");
    auto       db   = openOrDie(path);

    std::vector<DbClassInput> inputs;
    inputs.push_back(classInput("demo/Base", "java/lang/Object"));
    DbClassInput caller = classInput("demo/Caller", "java/lang/Object");
    caller.members.push_back(methodInput("call", "()V"));
    caller.members.push_back(fieldInput("slot", "I"));
    inputs.push_back(std::move(caller));
    inputs.push_back(classInput("demo/Child", "demo/Base"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 3,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 4,
                                         DbReferenceKind::Field, "demo/Base", "counter", "I"));
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 5,
                                         DbReferenceKind::Type, "demo/Base", "", ""));
    references.push_back(referenceRecord("demo/Child", "", "", -1, DbReferenceKind::Type,
                                         "demo/Base", "", ""));

    auto replaced = db.replaceReferences(0, std::move(references));
    ASSERT_TRUE(replaced.is_ok());
    EXPECT_EQ(4U, replaced.unwrap());

    auto count = db.referenceCount();
    ASSERT_TRUE(count.is_ok());
    EXPECT_EQ(4U, count.unwrap());

    auto calls = db.referencesToMethod(S("demo/Base"), S("run"), S("()V"));
    ASSERT_TRUE(calls.is_ok());
    auto callRecords = std::move(calls).unwrap();
    ASSERT_EQ(1U, callRecords.size());
    EXPECT_EQ("demo/Caller", callRecords[0].sourceClass.to_std_string());
    EXPECT_EQ("call", callRecords[0].sourceMethod.to_std_string());
    EXPECT_EQ(3, callRecords[0].insnIndex);

    auto fields = db.referencesToField(S("demo/Base"), S("counter"), S("I"));
    ASSERT_TRUE(fields.is_ok());
    EXPECT_EQ(1U, std::move(fields).unwrap().size());

    auto toClass = db.referencesToClass(S("demo/Base"));
    ASSERT_TRUE(toClass.is_ok());
    EXPECT_EQ(4U, std::move(toClass).unwrap().size());

    auto fromMember = db.referencesFromMember(S("demo/Caller"), S("call"), S("()V"));
    ASSERT_TRUE(fromMember.is_ok());
    EXPECT_EQ(3U, std::move(fromMember).unwrap().size());

    auto fromClass = db.referencesFromClass(S("demo/Child"));
    ASSERT_TRUE(fromClass.is_ok());
    auto childRefs = std::move(fromClass).unwrap();
    ASSERT_EQ(1U, childRefs.size());
    EXPECT_EQ(-1, childRefs[0].insnIndex);
    EXPECT_EQ(DbReferenceKind::Type, childRefs[0].kind);

    std::vector<DbReferenceRecord> bad;
    bad.push_back(referenceRecord("demo/Missing", "call", "()V", 0, DbReferenceKind::Method,
                                  "demo/Base", "run", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.replaceReferences(0, std::move(bad))));

    count = db.referenceCount();
    ASSERT_TRUE(count.is_ok());
    EXPECT_EQ(4U, count.unwrap());
}

TEST(MjAnalysisDbTest, ReferenceValidationRejectsMalformedSites)
{
    const auto path = freshDbPath("refs_validation");
    auto       db   = openOrDie(path);

    std::vector<DbClassInput> inputs;
    inputs.push_back(classInput("demo/Base", "java/lang/Object"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    std::vector<DbReferenceRecord> classLevelMethod;
    classLevelMethod.push_back(referenceRecord("demo/Base", "", "", -1,
                                               DbReferenceKind::Method, "demo/Base", "run", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferences(0, std::move(classLevelMethod))));

    std::vector<DbReferenceRecord> methodWithoutSite;
    methodWithoutSite.push_back(referenceRecord("demo/Base", "run", "()V", -1,
                                                DbReferenceKind::Method, "demo/Base", "x", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferences(0, std::move(methodWithoutSite))));

    std::vector<DbReferenceRecord> typeWithName;
    typeWithName.push_back(referenceRecord("demo/Base", "", "", -1, DbReferenceKind::Type,
                                           "demo/Base", "x", ""));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferences(0, std::move(typeWithName))));
}

TEST(MjAnalysisDbTest, ReplaceReferencesFromMemberSwapsSliceAndLeavesOtherRows)
{
    const auto path = freshDbPath("refs_slice");
    auto       db   = openOrDie(path);

    std::vector<DbClassInput> inputs;
    DbClassInput caller = classInput("demo/Caller", "java/lang/Object");
    caller.members.push_back(methodInput("call", "()V"));
    caller.members.push_back(methodInput("keep", "()J"));
    caller.members.push_back(fieldInput("slot", "I"));
    inputs.push_back(std::move(caller));
    inputs.push_back(classInput("demo/Base", "java/lang/Object"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    // 初版引用事实：call()V 两行、keep()J 两行、class 头一行。
    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 3,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 4,
                                         DbReferenceKind::Field, "demo/Base", "counter", "I"));
    references.push_back(referenceRecord("demo/Caller", "keep", "()J", 7,
                                         DbReferenceKind::Type, "demo/Base", "", ""));
    references.push_back(referenceRecord("demo/Caller", "keep", "()J", 8,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    references.push_back(
        referenceRecord("demo/Caller", "", "", -1, DbReferenceKind::Type, "demo/Base", "", ""));
    ASSERT_TRUE(db.replaceReferences(0, std::move(references)).is_ok());

    auto callMember = db.findMember(DbMemberKind::Method, S("demo/Caller"), S("call"), S("()V"));
    ASSERT_TRUE(callMember.is_ok());
    auto callRow = std::move(callMember).unwrap();
    ASSERT_TRUE(callRow.has_value());
    const ca::i64 callId = callRow->memberId;

    // 切片替换：call()V 的旧引用（insn 3/4）全清，新引用（insn 30/31）全进。
    std::vector<DbReferenceRecord> slice;
    slice.push_back(referenceRecord("demo/Caller", "call", "()V", 30,
                                    DbReferenceKind::Method, "demo/Base", "run", "()V"));
    slice.push_back(referenceRecord("demo/Caller", "call", "()V", 31,
                                    DbReferenceKind::Type, "demo/Base", "", ""));
    auto replaced = db.replaceReferencesFromMember(0, callId, std::move(slice));
    ASSERT_TRUE(replaced.is_ok());
    EXPECT_EQ(2U, replaced.unwrap());

    auto fromCall = db.referencesFromMember(S("demo/Caller"), S("call"), S("()V"));
    ASSERT_TRUE(fromCall.is_ok());
    auto callRefs = std::move(fromCall).unwrap();
    ASSERT_EQ(2U, callRefs.size());
    EXPECT_EQ(30, callRefs[0].insnIndex);
    EXPECT_EQ(DbReferenceKind::Method, callRefs[0].kind);
    EXPECT_EQ("demo/Base", callRefs[0].targetClass.to_std_string());
    EXPECT_EQ("run", callRefs[0].targetName.to_std_string());
    EXPECT_EQ(31, callRefs[1].insnIndex);
    EXPECT_EQ(DbReferenceKind::Type, callRefs[1].kind);

    // 切片语义核心验收：其他方法的引用逐行不动（行内容与行数原样）。
    auto fromKeep = db.referencesFromMember(S("demo/Caller"), S("keep"), S("()J"));
    ASSERT_TRUE(fromKeep.is_ok());
    auto keepRefs = std::move(fromKeep).unwrap();
    ASSERT_EQ(2U, keepRefs.size());
    EXPECT_EQ(7, keepRefs[0].insnIndex);
    EXPECT_EQ(DbReferenceKind::Type, keepRefs[0].kind);
    EXPECT_EQ(8, keepRefs[1].insnIndex);
    EXPECT_EQ(DbReferenceKind::Method, keepRefs[1].kind);

    // class 头引用（source_member 为 NULL）同属切片之外；referencesFromClass
    // 返回该类全部来源行（class 头 + 两个方法体 = 5），其中 insn -1 行仍在。
    auto fromClass = db.referencesFromClass(S("demo/Caller"));
    ASSERT_TRUE(fromClass.is_ok());
    auto classRefs = std::move(fromClass).unwrap();
    ASSERT_EQ(5U, classRefs.size());
    ca::usize classLevelRows = 0;
    for (const auto& record : classRefs) {
        if (record.insnIndex == -1) {
            ++classLevelRows;
            EXPECT_TRUE(record.sourceMethod.is_empty());
            EXPECT_EQ(DbReferenceKind::Type, record.kind);
        }
    }
    EXPECT_EQ(1U, classLevelRows);

    auto total = db.referenceCount();
    ASSERT_TRUE(total.is_ok());
    EXPECT_EQ(5U, total.unwrap());

    // 空输入 = 清空该成员切片，别的行依旧不动。
    std::vector<DbReferenceRecord> empty;
    auto cleared = db.replaceReferencesFromMember(0, callId, std::move(empty));
    ASSERT_TRUE(cleared.is_ok());
    EXPECT_EQ(0U, cleared.unwrap());
    fromCall = db.referencesFromMember(S("demo/Caller"), S("call"), S("()V"));
    ASSERT_TRUE(fromCall.is_ok());
    EXPECT_TRUE(std::move(fromCall).unwrap().empty());
    total = db.referenceCount();
    ASSERT_TRUE(total.is_ok());
    EXPECT_EQ(3U, total.unwrap());
}

TEST(MjAnalysisDbTest, ReplaceReferencesFromMemberRejectsBadSliceWithoutWriting)
{
    const auto path = freshDbPath("refs_slice_invalid");
    auto       db   = openOrDie(path);

    std::vector<DbClassInput> inputs;
    DbClassInput caller = classInput("demo/Caller", "java/lang/Object");
    caller.members.push_back(methodInput("call", "()V"));
    caller.members.push_back(methodInput("keep", "()J"));
    caller.members.push_back(fieldInput("slot", "I"));
    inputs.push_back(std::move(caller));
    inputs.push_back(classInput("demo/Base", "java/lang/Object"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 3,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    ASSERT_TRUE(db.replaceReferences(0, std::move(references)).is_ok());

    auto callMember = db.findMember(DbMemberKind::Method, S("demo/Caller"), S("call"), S("()V"));
    ASSERT_TRUE(callMember.is_ok());
    auto callRow = std::move(callMember).unwrap();
    ASSERT_TRUE(callRow.has_value());
    const ca::i64 callId = callRow->memberId;

    // 不存在的 member_id：与 replaceReferences 对未登记来源一致，Invalid 且不写库。
    std::vector<DbReferenceRecord> ghost;
    ghost.push_back(referenceRecord("demo/Caller", "call", "()V", 9,
                                    DbReferenceKind::Method, "demo/Base", "run", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferencesFromMember(0, 424242, std::move(ghost))));
    auto count = db.referenceCount();
    ASSERT_TRUE(count.is_ok());
    EXPECT_EQ(1U, count.unwrap());

    // 字段不是合法引用来源：同样 Invalid。
    auto fieldMember = db.findMember(DbMemberKind::Field, S("demo/Caller"), S("slot"), S("I"));
    ASSERT_TRUE(fieldMember.is_ok());
    auto fieldRow = std::move(fieldMember).unwrap();
    ASSERT_TRUE(fieldRow.has_value());
    std::vector<DbReferenceRecord> viaField;
    viaField.push_back(referenceRecord("demo/Caller", "slot", "I", 0,
                                       DbReferenceKind::Method, "demo/Base", "run", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferencesFromMember(0, fieldRow->memberId,
                                                           std::move(viaField))));

    // 行内 source 与参数成员不一致：切片身份错位，Invalid 且 call()V 切片原样。
    std::vector<DbReferenceRecord> mismatched;
    mismatched.push_back(referenceRecord("demo/Caller", "keep", "()J", 9,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferencesFromMember(0, callId, std::move(mismatched))));

    auto fromCall = db.referencesFromMember(S("demo/Caller"), S("call"), S("()V"));
    ASSERT_TRUE(fromCall.is_ok());
    auto callRefs = std::move(fromCall).unwrap();
    ASSERT_EQ(1U, callRefs.size());
    EXPECT_EQ(3, callRefs[0].insnIndex);

    // class 级引用混入成员切片：形状合法但 source 与成员身份错位，同样 Invalid。
    std::vector<DbReferenceRecord> malformed;
    malformed.push_back(referenceRecord("demo/Caller", "", "", -1, DbReferenceKind::Type,
                                        "demo/Base", "", ""));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferencesFromMember(0, callId, std::move(malformed))));
    count = db.referenceCount();
    ASSERT_TRUE(count.is_ok());
    EXPECT_EQ(1U, count.unwrap());
}

TEST(MjAnalysisDbTest, ReplaceReferencesFromClassSwapsHeaderSliceAndLeavesOtherRows)
{
    const auto path = freshDbPath("refs_class_slice");
    auto       db   = openOrDie(path);

    std::vector<DbClassInput> inputs;
    DbClassInput caller = classInput("demo/Caller", "java/lang/Object");
    caller.members.push_back(methodInput("call", "()V"));
    inputs.push_back(std::move(caller));
    inputs.push_back(classInput("demo/Base", "java/lang/Object"));
    inputs.push_back(classInput("demo/Other", "java/lang/Object"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    // 初版引用事实：Caller 类头一行（Type demo/Base）+ call()V 方法体一行；
    // Other 类头一行。类头行即 source_member 为 NULL 的行。
    std::vector<DbReferenceRecord> references;
    references.push_back(
        referenceRecord("demo/Caller", "", "", -1, DbReferenceKind::Type, "demo/Base", "", ""));
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 3,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    references.push_back(
        referenceRecord("demo/Other", "", "", -1, DbReferenceKind::Type, "demo/Base", "", ""));
    ASSERT_TRUE(db.replaceReferences(0, std::move(references)).is_ok());

    auto callerClass = db.findClass(S("demo/Caller"));
    ASSERT_TRUE(callerClass.is_ok());
    auto callerRow = std::move(callerClass).unwrap();
    ASSERT_TRUE(callerRow.has_value());
    const ca::i64 callerId = callerRow->classId;

    // 类头切片替换：Caller 旧类头行（demo/Base）清空，新两行落库（demo/New
    // 未登记目标走文本分支）；call()V 方法体行与 Other 的类头行逐行不动。
    std::vector<DbReferenceRecord> slice;
    slice.push_back(
        referenceRecord("demo/Caller", "", "", -1, DbReferenceKind::Type, "demo/New", "", ""));
    slice.push_back(
        referenceRecord("demo/Caller", "", "", -1, DbReferenceKind::Type, "demo/Base", "", ""));
    auto replaced = db.replaceReferencesFromClass(0, callerId, std::move(slice));
    ASSERT_TRUE(replaced.is_ok());
    EXPECT_EQ(2U, replaced.unwrap());

    auto fromCaller = db.referencesFromClass(S("demo/Caller"));
    ASSERT_TRUE(fromCaller.is_ok());
    auto callerRefs = std::move(fromCaller).unwrap();
    ASSERT_EQ(3U, callerRefs.size());
    ca::usize headerRows = 0;
    ca::usize methodRows = 0;
    for (const auto& record : callerRefs) {
        if (record.insnIndex == -1) {
            ++headerRows;
            EXPECT_TRUE(record.sourceMethod.is_empty());
            EXPECT_EQ(DbReferenceKind::Type, record.kind);
        }
        else {
            ++methodRows;
        }
    }
    EXPECT_EQ(2U, headerRows);
    EXPECT_EQ(1U, methodRows);

    // 切片语义核心验收：Other 的类头行原样（行数与目标都不变）。
    auto fromOther = db.referencesFromClass(S("demo/Other"));
    ASSERT_TRUE(fromOther.is_ok());
    auto otherRefs = std::move(fromOther).unwrap();
    ASSERT_EQ(1U, otherRefs.size());
    EXPECT_EQ(-1, otherRefs[0].insnIndex);
    EXPECT_EQ("demo/Base", otherRefs[0].targetClass.to_std_string());

    auto total = db.referenceCount();
    ASSERT_TRUE(total.is_ok());
    EXPECT_EQ(4U, total.unwrap());

    // 空输入 = 清空该类的类头切片，方法体行与 Other 依旧不动。
    std::vector<DbReferenceRecord> empty;
    auto cleared = db.replaceReferencesFromClass(0, callerId, std::move(empty));
    ASSERT_TRUE(cleared.is_ok());
    EXPECT_EQ(0U, cleared.unwrap());
    fromCaller = db.referencesFromClass(S("demo/Caller"));
    ASSERT_TRUE(fromCaller.is_ok());
    callerRefs = std::move(fromCaller).unwrap();
    ASSERT_EQ(1U, callerRefs.size());
    EXPECT_EQ(3, callerRefs[0].insnIndex);
    total = db.referenceCount();
    ASSERT_TRUE(total.is_ok());
    EXPECT_EQ(2U, total.unwrap());
}

TEST(MjAnalysisDbTest, ReplaceReferencesFromClassRejectsBadSliceWithoutWriting)
{
    const auto path = freshDbPath("refs_class_slice_invalid");
    auto       db   = openOrDie(path);

    std::vector<DbClassInput> inputs;
    DbClassInput caller = classInput("demo/Caller", "java/lang/Object");
    caller.members.push_back(methodInput("call", "()V"));
    inputs.push_back(std::move(caller));
    inputs.push_back(classInput("demo/Base", "java/lang/Object"));
    inputs.push_back(classInput("demo/Other", "java/lang/Object"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    std::vector<DbReferenceRecord> references;
    references.push_back(
        referenceRecord("demo/Caller", "", "", -1, DbReferenceKind::Type, "demo/Base", "", ""));
    ASSERT_TRUE(db.replaceReferences(0, std::move(references)).is_ok());

    auto callerClass = db.findClass(S("demo/Caller"));
    ASSERT_TRUE(callerClass.is_ok());
    auto callerRow = std::move(callerClass).unwrap();
    ASSERT_TRUE(callerRow.has_value());
    const ca::i64 callerId = callerRow->classId;

    // 不存在的 class_id：与切片族一致，Invalid 且不写库。
    std::vector<DbReferenceRecord> ghost;
    ghost.push_back(
        referenceRecord("demo/Caller", "", "", -1, DbReferenceKind::Type, "demo/Base", "", ""));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferencesFromClass(0, 424242, std::move(ghost))));
    auto count = db.referenceCount();
    ASSERT_TRUE(count.is_ok());
    EXPECT_EQ(1U, count.unwrap());

    // 带方法源的记录：对 replaceReferencesFromMember 合法，但类头切片不收。
    std::vector<DbReferenceRecord> viaMethod;
    viaMethod.push_back(referenceRecord("demo/Caller", "call", "()V", 3,
                                        DbReferenceKind::Method, "demo/Base", "run", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferencesFromClass(0, callerId, std::move(viaMethod))));

    // 行内 source 类与参数类不一致：切片身份错位，同样 Invalid。
    std::vector<DbReferenceRecord> mismatched;
    mismatched.push_back(
        referenceRecord("demo/Other", "", "", -1, DbReferenceKind::Type, "demo/Base", "", ""));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferencesFromClass(0, callerId, std::move(mismatched))));

    count = db.referenceCount();
    ASSERT_TRUE(count.is_ok());
    EXPECT_EQ(1U, count.unwrap());

    // 原类头行原样。
    auto fromCaller = db.referencesFromClass(S("demo/Caller"));
    ASSERT_TRUE(fromCaller.is_ok());
    auto callerRefs = std::move(fromCaller).unwrap();
    ASSERT_EQ(1U, callerRefs.size());
    EXPECT_EQ(-1, callerRefs[0].insnIndex);
    EXPECT_EQ("demo/Base", callerRefs[0].targetClass.to_std_string());
}

//-- 分页变体（引用与层次闭包查询，行偏移窗口） ------------------------------

TEST(MjAnalysisDbTest, ReferencePagedQueriesMatchFullResults)
{
    const auto path = freshDbPath("refs_paged");
    auto       db   = openOrDie(path);

    std::vector<DbClassInput> inputs;
    DbClassInput callerA = classInput("demo/A", "java/lang/Object");
    callerA.members.push_back(methodInput("hit", "()V"));
    callerA.members.push_back(methodInput("loop", "()V"));
    DbClassInput callerB = classInput("demo/B", "java/lang/Object");
    callerB.members.push_back(methodInput("visit", "()V"));
    DbClassInput callerC = classInput("demo/C", "java/lang/Object");
    callerC.members.push_back(methodInput("go", "()V"));
    DbClassInput base = classInput("demo/Base", "java/lang/Object");
    base.members.push_back(methodInput("run", "()V"));
    base.members.push_back(fieldInput("counter", "I"));
    base.members.push_back(fieldInput("next", "J"));
    inputs.push_back(std::move(callerA));
    inputs.push_back(std::move(callerB));
    inputs.push_back(std::move(callerC));
    inputs.push_back(std::move(base));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    // 8 行命中 demo/Base：三个方法体 + 一个 class 头，三种引用类别混排。
    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/A", "hit", "()V", 1,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    references.push_back(referenceRecord("demo/A", "loop", "()V", 2,
                                         DbReferenceKind::Field, "demo/Base", "counter", "I"));
    references.push_back(referenceRecord("demo/A", "loop", "()V", 3,
                                         DbReferenceKind::Type, "demo/Base", "", ""));
    references.push_back(referenceRecord("demo/B", "visit", "()V", 4,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    references.push_back(referenceRecord("demo/B", "visit", "()V", 5,
                                         DbReferenceKind::Type, "demo/Base", "", ""));
    references.push_back(referenceRecord("demo/C", "go", "()V", 6,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    references.push_back(referenceRecord("demo/C", "go", "()V", 7,
                                         DbReferenceKind::Field, "demo/Base", "next", "J"));
    references.push_back(referenceRecord("demo/C", "", "", -1, DbReferenceKind::Type,
                                         "demo/Base", "", ""));
    auto replaced = db.replaceReferences(0, std::move(references));
    ASSERT_TRUE(replaced.is_ok());
    EXPECT_EQ(8U, replaced.unwrap());

    // 全量基线。
    auto fullToClass = db.referencesToClass(S("demo/Base"));
    ASSERT_TRUE(fullToClass.is_ok());
    std::vector<DbReferenceRecord> toClassRows = std::move(fullToClass).unwrap();
    ASSERT_EQ(8U, toClassRows.size());

    // 整窗一次取全 = 全量（行序一致）。
    auto whole = db.referencesToClassPaged(S("demo/Base"), 100, 0);
    ASSERT_TRUE(whole.is_ok());
    expectSameRecords(toClassRows, std::move(whole).unwrap());

    // 3 行一页跨页拼接 = 全量：不重不漏，尾页只回剩余行。
    std::vector<DbReferenceRecord> stitched;
    for (ca::i64 offset = 0; offset < 8; offset += 3) {
        auto page = db.referencesToClassPaged(S("demo/Base"), 3, offset);
        ASSERT_TRUE(page.is_ok());
        auto rows = std::move(page).unwrap();
        ASSERT_LE(rows.size(), 3U);
        // Utf8String 只移不拷：页行整体 move 进拼接窗口。
        stitched.insert(stitched.end(), std::make_move_iterator(rows.begin()),
                        std::make_move_iterator(rows.end()));
    }
    expectSameRecords(toClassRows, stitched);

    auto tail = db.referencesToClassPaged(S("demo/Base"), 3, 6);
    ASSERT_TRUE(tail.is_ok());
    EXPECT_EQ(2U, std::move(tail).unwrap().size());

    // offset 越界与 limit=0 都是空页（合法，非错误）。
    auto beyond = db.referencesToClassPaged(S("demo/Base"), 3, 8);
    ASSERT_TRUE(beyond.is_ok());
    EXPECT_TRUE(std::move(beyond).unwrap().empty());
    auto far = db.referencesToClassPaged(S("demo/Base"), 3, 1000);
    ASSERT_TRUE(far.is_ok());
    EXPECT_TRUE(std::move(far).unwrap().empty());
    auto zero = db.referencesToClassPaged(S("demo/Base"), 0, 0);
    ASSERT_TRUE(zero.is_ok());
    EXPECT_TRUE(std::move(zero).unwrap().empty());

    // 负窗口参数是契约错误：Invalid。
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.referencesToClassPaged(S("demo/Base"), -1, 0)));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.referencesToClassPaged(S("demo/Base"), 3, -1)));

    // 精确目标分页：与全量版同序，两页拼接等于全量。
    auto fullToMethod = db.referencesToMethod(S("demo/Base"), S("run"), S("()V"));
    ASSERT_TRUE(fullToMethod.is_ok());
    std::vector<DbReferenceRecord> toMethodRows = std::move(fullToMethod).unwrap();
    ASSERT_EQ(3U, toMethodRows.size());
    auto methodHead = db.referencesToMethodPaged(S("demo/Base"), S("run"), S("()V"), 2, 0);
    ASSERT_TRUE(methodHead.is_ok());
    auto methodTail = db.referencesToMethodPaged(S("demo/Base"), S("run"), S("()V"), 2, 2);
    ASSERT_TRUE(methodTail.is_ok());
    std::vector<DbReferenceRecord> methodPages = std::move(methodHead).unwrap();
    auto methodTailRows = std::move(methodTail).unwrap();
    ASSERT_EQ(1U, methodTailRows.size());
    methodPages.insert(methodPages.end(), std::make_move_iterator(methodTailRows.begin()),
                       std::make_move_iterator(methodTailRows.end()));
    expectSameRecords(toMethodRows, methodPages);

    auto fullToField = db.referencesToField(S("demo/Base"), S("counter"), S("I"));
    ASSERT_TRUE(fullToField.is_ok());
    std::vector<DbReferenceRecord> toFieldRows = std::move(fullToField).unwrap();
    ASSERT_EQ(1U, toFieldRows.size());
    auto fieldPage = db.referencesToFieldPaged(S("demo/Base"), S("counter"), S("I"), 5, 0);
    ASSERT_TRUE(fieldPage.is_ok());
    expectSameRecords(toFieldRows, std::move(fieldPage).unwrap());

    // 按源分页：referencesFromClassPaged 单行窗口逐步走完 = 全量。
    auto fullFromA = db.referencesFromClass(S("demo/A"));
    ASSERT_TRUE(fullFromA.is_ok());
    std::vector<DbReferenceRecord> fromARows = std::move(fullFromA).unwrap();
    ASSERT_EQ(3U, fromARows.size());
    std::vector<DbReferenceRecord> fromAStitched;
    for (ca::i64 offset = 0; offset < 3; ++offset) {
        auto page = db.referencesFromClassPaged(S("demo/A"), 1, offset);
        ASSERT_TRUE(page.is_ok());
        auto rows = std::move(page).unwrap();
        ASSERT_EQ(1U, rows.size());
        fromAStitched.push_back(std::move(rows[0]));
    }
    expectSameRecords(fromARows, fromAStitched);

    auto fullFromLoop = db.referencesFromMember(S("demo/A"), S("loop"), S("()V"));
    ASSERT_TRUE(fullFromLoop.is_ok());
    std::vector<DbReferenceRecord> fromLoopRows = std::move(fullFromLoop).unwrap();
    ASSERT_EQ(2U, fromLoopRows.size());
    auto loopPage = db.referencesFromMemberPaged(S("demo/A"), S("loop"), S("()V"), 10, 0);
    ASSERT_TRUE(loopPage.is_ok());
    expectSameRecords(fromLoopRows, std::move(loopPage).unwrap());

    // 未登记目标/来源：全量与分页同为空页（非错误）。
    auto ghostFull = db.referencesFromClass(S("demo/Nope"));
    ASSERT_TRUE(ghostFull.is_ok());
    EXPECT_TRUE(std::move(ghostFull).unwrap().empty());
    auto ghostPage = db.referencesFromClassPaged(S("demo/Nope"), 5, 0);
    ASSERT_TRUE(ghostPage.is_ok());
    EXPECT_TRUE(std::move(ghostPage).unwrap().empty());
    auto ghostMember = db.referencesFromMemberPaged(S("demo/Nope"), S("x"), S("()V"), 5, 0);
    ASSERT_TRUE(ghostMember.is_ok());
    EXPECT_TRUE(std::move(ghostMember).unwrap().empty());
}

TEST(MjAnalysisDbTest, HierarchyPagedQueriesMatchFullResults)
{
    const auto path = freshDbPath("hierarchy_paged");
    auto       db   = openOrDie(path);

    // 层次语料：Root 下两链一支（A→A1→A1x、A→A2、B），另有一支实现 If 接口。
    std::vector<DbClassInput> inputs;
    inputs.push_back(classInput("demo/Root", "java/lang/Object"));
    inputs.push_back(classInput("demo/A", "demo/Root"));
    inputs.push_back(classInput("demo/B", "demo/Root"));
    inputs.push_back(classInput("demo/A1", "demo/A"));
    inputs.push_back(classInput("demo/A2", "demo/A"));
    inputs.push_back(classInput("demo/A1x", "demo/A1"));
    DbClassInput wing = classInput("demo/Wing", "java/lang/Object");
    wing.interfaces.push_back(Utf8String::from_cstr("demo/If"));
    inputs.push_back(std::move(wing));
    DbClassInput wingImpl = classInput("demo/WingImpl", "demo/Wing");
    wingImpl.interfaces.push_back(Utf8String::from_cstr("demo/If"));
    inputs.push_back(std::move(wingImpl));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    auto fullSubs = db.allSubclasses(S("demo/Root"));
    ASSERT_TRUE(fullSubs.is_ok());
    std::vector<Utf8String> subsRows = std::move(fullSubs).unwrap();
    ASSERT_EQ(5U, subsRows.size());
    EXPECT_EQ("demo/A", subsRows[0].to_std_string());
    EXPECT_EQ("demo/B", subsRows[4].to_std_string());

    // 整窗 = 全量；2 行一页跨页拼接 = 全量（不重不漏）。
    auto whole = db.allSubclassesPaged(S("demo/Root"), 100, 0);
    ASSERT_TRUE(whole.is_ok());
    expectSameNames(subsRows, std::move(whole).unwrap());

    std::vector<Utf8String> stitched;
    for (ca::i64 offset = 0; offset < 5; offset += 2) {
        auto page = db.allSubclassesPaged(S("demo/Root"), 2, offset);
        ASSERT_TRUE(page.is_ok());
        auto rows = std::move(page).unwrap();
        ASSERT_LE(rows.size(), 2U);
        stitched.insert(stitched.end(), std::make_move_iterator(rows.begin()),
                        std::make_move_iterator(rows.end()));
    }
    expectSameNames(subsRows, stitched);

    // 尾页只回剩余行；offset 越界与 limit=0 空页；负窗口参数 Invalid。
    auto tail = db.allSubclassesPaged(S("demo/Root"), 2, 4);
    ASSERT_TRUE(tail.is_ok());
    auto tailRows = std::move(tail).unwrap();
    ASSERT_EQ(1U, tailRows.size());
    EXPECT_EQ("demo/B", tailRows[0].to_std_string());
    auto beyond = db.allSubclassesPaged(S("demo/Root"), 2, 5);
    ASSERT_TRUE(beyond.is_ok());
    EXPECT_TRUE(std::move(beyond).unwrap().empty());
    auto zero = db.allSubclassesPaged(S("demo/Root"), 0, 0);
    ASSERT_TRUE(zero.is_ok());
    EXPECT_TRUE(std::move(zero).unwrap().empty());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.allSubclassesPaged(S("demo/Root"), -1, 0)));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.allSubclassesPaged(S("demo/Root"), 2, -1)));

    // 接口实现类分页：与 implementors 全量同序，单行窗口逐步走完。
    auto fullImpls = db.implementors(S("demo/If"));
    ASSERT_TRUE(fullImpls.is_ok());
    std::vector<Utf8String> implRows = std::move(fullImpls).unwrap();
    ASSERT_EQ(2U, implRows.size());
    EXPECT_EQ("demo/Wing", implRows[0].to_std_string());
    EXPECT_EQ("demo/WingImpl", implRows[1].to_std_string());

    std::vector<Utf8String> implStitched;
    for (ca::i64 offset = 0; offset < 2; ++offset) {
        auto page = db.implementorsPaged(S("demo/If"), 1, offset);
        ASSERT_TRUE(page.is_ok());
        auto rows = std::move(page).unwrap();
        ASSERT_EQ(1U, rows.size());
        implStitched.push_back(std::move(rows[0]));
    }
    expectSameNames(implRows, implStitched);
    auto implBeyond = db.implementorsPaged(S("demo/If"), 1, 2);
    ASSERT_TRUE(implBeyond.is_ok());
    EXPECT_TRUE(std::move(implBeyond).unwrap().empty());

    // 未登记类/接口：空页（非错误），与全量语义一致。
    auto ghostSubs = db.allSubclassesPaged(S("demo/Nope"), 10, 0);
    ASSERT_TRUE(ghostSubs.is_ok());
    EXPECT_TRUE(std::move(ghostSubs).unwrap().empty());
    auto ghostImpls = db.implementorsPaged(S("demo/Nope"), 10, 0);
    ASSERT_TRUE(ghostImpls.is_ok());
    EXPECT_TRUE(std::move(ghostImpls).unwrap().empty());
}

//-- 条目身份层（schema v3） ------------------------------------------------

namespace {

/// @brief 64 位小写十六进制测试锚：单字符重复填充。
std::string shaOf(char seed)
{
    return std::string(64, seed);
}

DbEntryInput entryInput(const std::string& path, const char* kind, const std::string& sha,
                        ca::i64 size = 16, bool regular = true)
{
    DbEntryInput input;
    input.path =
        Utf8String::from_data(reinterpret_cast<const ca::u8*>(path.data()), path.size());
    input.kind   = Utf8String::from_cstr(kind);
    input.size   = size;
    input.sha256 =
        Utf8String::from_data(reinterpret_cast<const ca::u8*>(sha.data()), sha.size());
    input.regular = regular;
    return input;
}

}   // namespace

TEST(MjAnalysisDbTest, RegistersInputsWithStablePathIdentity)
{
    const auto path = freshDbPath("entries_inputs");
    auto       db   = openOrDie(path);

    auto first = db.ensureInput(S("lib/app.jar"), refOf(shaOf('1')), 1234);
    ASSERT_TRUE(first.is_ok());
    const ca::i64 firstId = first.unwrap();
    EXPECT_GT(firstId, 0);

    // 同 path 重登记：复用 input_id，内容锚刷新。
    auto rehashed = db.ensureInput(S("lib/app.jar"), refOf(shaOf('2')), 2048);
    ASSERT_TRUE(rehashed.is_ok());
    EXPECT_EQ(firstId, rehashed.unwrap());

    // 不同 path 分配新 id，单调递增。
    auto second = db.ensureInput(S("lib/other.jar"), refOf(shaOf('3')), 64);
    ASSERT_TRUE(second.is_ok());
    EXPECT_GT(second.unwrap(), firstId);

    auto listed = db.listInputs();
    ASSERT_TRUE(listed.is_ok());
    auto inputs = std::move(listed).unwrap();
    ASSERT_EQ(2U, inputs.size());
    EXPECT_EQ("lib/app.jar", inputs[0].path.to_std_string());
    EXPECT_EQ(firstId, inputs[0].inputId);
    EXPECT_EQ(2048, inputs[0].size);
    EXPECT_EQ(shaOf('2'), inputs[0].sha256.to_std_string());
    EXPECT_EQ("lib/other.jar", inputs[1].path.to_std_string());

    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ensureInput(S("lib/bad.jar"), S("nothex"), 1)));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ensureInput(S(""), refOf(shaOf('4')), 1)));
}

TEST(MjAnalysisDbTest, EntryIngestKeepsStableIdsAndTombstonesMissing)
{
    const auto path = freshDbPath("entries_ingest");
    auto       db   = openOrDie(path);

    auto inputId = db.ensureInput(S("lib/app.jar"), refOf(shaOf('0')), 4096);
    ASSERT_TRUE(inputId.is_ok());
    const ca::i64 jar = inputId.unwrap();

    // 首次登记：三个条目全量新建。
    std::vector<DbEntryInput> first;
    first.push_back(entryInput("a/A.class", "class", shaOf('a')));
    first.push_back(entryInput("b/B.class", "class", shaOf('b')));
    first.push_back(entryInput("META-INF/MANIFEST.MF", "manifest", shaOf('c'), 8, false));
    auto firstStats = db.ingestEntries(0, jar, std::move(first));
    ASSERT_TRUE(firstStats.is_ok());
    {
        const auto stats = firstStats.unwrap();
        EXPECT_EQ(3U, stats.total);
        EXPECT_EQ(3U, stats.created);
        EXPECT_EQ(0U, stats.reused);
        EXPECT_EQ(0U, stats.resurrected);
        EXPECT_EQ(0U, stats.tombstoned);
    }

    auto listed = db.listEntries(jar);
    ASSERT_TRUE(listed.is_ok());
    auto rows = std::move(listed).unwrap();
    ASSERT_EQ(3U, rows.size());
    ASSERT_EQ("META-INF/MANIFEST.MF", rows[0].path.to_std_string());
    ASSERT_EQ("a/A.class", rows[1].path.to_std_string());
    ASSERT_EQ("b/B.class", rows[2].path.to_std_string());
    const ca::i64 entryM = rows[0].entryId;
    const ca::i64 entryA = rows[1].entryId;
    const ca::i64 entryB = rows[2].entryId;
    EXPECT_GT(entryA, 0);
    EXPECT_GT(entryB, entryA);
    EXPECT_GT(entryM, entryB);
    EXPECT_FALSE(rows[0].regular);
    EXPECT_FALSE(rows[0].tombstoned);
    EXPECT_EQ("manifest", rows[0].kind.to_std_string());

    // 同内容重导入（乱序）：id 不漂，全量复用。
    std::vector<DbEntryInput> same;
    same.push_back(entryInput("b/B.class", "class", shaOf('b')));
    same.push_back(entryInput("META-INF/MANIFEST.MF", "manifest", shaOf('c'), 8, false));
    same.push_back(entryInput("a/A.class", "class", shaOf('a')));
    auto sameStats = db.ingestEntries(0, jar, std::move(same));
    ASSERT_TRUE(sameStats.is_ok());
    {
        const auto stats = sameStats.unwrap();
        EXPECT_EQ(3U, stats.total);
        EXPECT_EQ(0U, stats.created);
        EXPECT_EQ(3U, stats.reused);
        EXPECT_EQ(0U, stats.resurrected);
        EXPECT_EQ(0U, stats.tombstoned);
    }

    // 二次重导入：只保留 a/A.class，其余进墓碑（不删行）。
    std::vector<DbEntryInput> shrunk;
    shrunk.push_back(entryInput("a/A.class", "class", shaOf('a')));
    auto shrunkStats = db.ingestEntries(0, jar, std::move(shrunk));
    ASSERT_TRUE(shrunkStats.is_ok());
    EXPECT_EQ(2U, shrunkStats.unwrap().tombstoned);

    listed = db.listEntries(jar);
    ASSERT_TRUE(listed.is_ok());
    rows = std::move(listed).unwrap();
    ASSERT_EQ(3U, rows.size());
    EXPECT_FALSE(rows[1].tombstoned);
    EXPECT_EQ(entryA, rows[1].entryId);
    EXPECT_TRUE(rows[0].tombstoned);
    EXPECT_TRUE(rows[2].tombstoned);

    // 消失条目重现：锚命中墓碑行，原地复活，id 不漂。
    std::vector<DbEntryInput> revived;
    revived.push_back(entryInput("a/A.class", "class", shaOf('a')));
    revived.push_back(entryInput("b/B.class", "class", shaOf('b')));
    auto revivedStats = db.ingestEntries(0, jar, std::move(revived));
    ASSERT_TRUE(revivedStats.is_ok());
    {
        const auto stats = revivedStats.unwrap();
        EXPECT_EQ(1U, stats.resurrected);
        EXPECT_EQ(0U, stats.created);
        EXPECT_EQ(1U, stats.reused);
        EXPECT_EQ(0U, stats.tombstoned);
    }

    // 同 path 内容变化：旧行进墓碑，新内容拿新 id。
    std::vector<DbEntryInput> mutated;
    mutated.push_back(entryInput("a/A.class", "class", shaOf('a')));
    mutated.push_back(entryInput("b/B.class", "class", shaOf('d')));
    auto mutatedStats = db.ingestEntries(0, jar, std::move(mutated));
    ASSERT_TRUE(mutatedStats.is_ok());
    {
        const auto stats = mutatedStats.unwrap();
        EXPECT_EQ(1U, stats.created);
        EXPECT_EQ(1U, stats.reused);
        EXPECT_EQ(0U, stats.resurrected);
        EXPECT_EQ(1U, stats.tombstoned);
    }

    // 内容复原：最初那条 b/B.class 再次复活——锚即身份。
    std::vector<DbEntryInput> flippedBack;
    flippedBack.push_back(entryInput("a/A.class", "class", shaOf('a')));
    flippedBack.push_back(entryInput("b/B.class", "class", shaOf('b')));
    auto flipBack = db.ingestEntries(0, jar, std::move(flippedBack));
    ASSERT_TRUE(flipBack.is_ok());
    EXPECT_EQ(1U, flipBack.unwrap().resurrected);

    listed = db.listEntries(jar);
    ASSERT_TRUE(listed.is_ok());
    rows = std::move(listed).unwrap();
    ASSERT_EQ(4U, rows.size());   // 行只增不删：a/A + 两条 b/B 历史 + MANIFEST
    ca::usize liveCount = 0;
    for (const DbEntryInfo& row : rows) {
        if (!row.tombstoned) ++liveCount;
        if (row.path.to_std_string() == "b/B.class" && row.sha256.to_std_string() == shaOf('b')) {
            EXPECT_EQ(entryB, row.entryId);
        }
    }
    EXPECT_EQ(2U, liveCount);
}

TEST(MjAnalysisDbTest, EntryIngestRejectsInvalidBatchWithoutWriting)
{
    const auto path = freshDbPath("entries_invalid");
    auto       db   = openOrDie(path);

    auto inputId = db.ensureInput(S("lib/app.jar"), refOf(shaOf('0')), 4096);
    ASSERT_TRUE(inputId.is_ok());
    const ca::i64 jar = inputId.unwrap();

    std::vector<DbEntryInput> valid;
    valid.push_back(entryInput("a/A.class", "class", shaOf('a')));
    ASSERT_TRUE(db.ingestEntries(0, jar, std::move(valid)).is_ok());

    std::vector<DbEntryInput> duplicate;
    duplicate.push_back(entryInput("x/Dup", "resource", shaOf('b')));
    duplicate.push_back(entryInput("x/Dup", "resource", shaOf('c')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(0, jar, std::move(duplicate))));

    std::vector<DbEntryInput> missingSha;
    missingSha.push_back(entryInput("x/NoSha", "resource", std::string(63, 'a')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(0, jar, std::move(missingSha))));

    std::vector<DbEntryInput> upperSha;
    upperSha.push_back(entryInput("x/Upper", "resource", std::string(64, 'A')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(0, jar, std::move(upperSha))));

    std::vector<DbEntryInput> emptyKind;
    emptyKind.push_back(entryInput("x/NoKind", "", shaOf('d')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(0, jar, std::move(emptyKind))));

    std::vector<DbEntryInput> emptyPath;
    emptyPath.push_back(entryInput("", "resource", shaOf('e')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(0, jar, std::move(emptyPath))));

    std::vector<DbEntryInput> negativeSize;
    negativeSize.push_back(entryInput("x/Neg", "resource", shaOf('f'), -1));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(0, jar, std::move(negativeSize))));

    std::vector<DbEntryInput> unknownInput;
    unknownInput.push_back(entryInput("a/A.class", "class", shaOf('a')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(0, jar + 999, std::move(unknownInput))));

    // 全部失败路径零写入。
    auto listed = db.listEntries(jar);
    ASSERT_TRUE(listed.is_ok());
    EXPECT_EQ(1U, std::move(listed).unwrap().size());

    auto unknown = db.listEntries(jar + 999);
    ASSERT_TRUE(unknown.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, std::move(unknown).unwrap_err().kind);
}

TEST(MjAnalysisDbTest, ClassesLinkToEntries)
{
    const auto path = freshDbPath("entries_classes");
    auto       db   = openOrDie(path);

    auto inputId = db.ensureInput(S("lib/app.jar"), refOf(shaOf('0')), 4096);
    ASSERT_TRUE(inputId.is_ok());
    const ca::i64 jar = inputId.unwrap();

    std::vector<DbEntryInput> entries;
    entries.push_back(entryInput("demo/Base.class", "class", shaOf('a')));
    auto stats = db.ingestEntries(0, jar, std::move(entries));
    ASSERT_TRUE(stats.is_ok());
    ASSERT_EQ(1U, stats.unwrap().created);

    auto listed = db.listEntries(jar);
    ASSERT_TRUE(listed.is_ok());
    auto rows = std::move(listed).unwrap();
    ASSERT_EQ(1U, rows.size());
    const ca::i64 entryId = rows[0].entryId;

    std::vector<DbClassInput> classes;
    classes.push_back(classInput("demo/Base", "java/lang/Object"));
    classes.back().entryId = entryId;
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    auto found = db.findClass(S("demo/Base"));
    ASSERT_TRUE(found.is_ok());
    auto base = std::move(found).unwrap();
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(entryId, base->entryId);

    // 条目内容更新后重登记类：链接指向新条目。
    std::vector<DbEntryInput> mutated;
    mutated.push_back(entryInput("demo/Base.class", "class", shaOf('b')));
    ASSERT_TRUE(db.ingestEntries(0, jar, std::move(mutated)).is_ok());
    listed = db.listEntries(jar);
    ASSERT_TRUE(listed.is_ok());
    rows = std::move(listed).unwrap();
    ASSERT_EQ(2U, rows.size());
    ca::i64 newEntryId = 0;
    for (const DbEntryInfo& row : rows) {
        if (!row.tombstoned) newEntryId = row.entryId;
    }
    EXPECT_NE(newEntryId, entryId);

    std::vector<DbClassInput> relinked;
    relinked.push_back(classInput("demo/Base", "java/lang/Object"));
    relinked.back().entryId = newEntryId;
    ASSERT_TRUE(db.ingestClasses(0, std::move(relinked)).is_ok());
    found = db.findClass(S("demo/Base"));
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(newEntryId, std::move(found).unwrap()->entryId);

    // entryId=0 = 未指定：既有链接保持不动。
    std::vector<DbClassInput> unspecified;
    unspecified.push_back(classInput("demo/Base", "java/lang/Other"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(unspecified)).is_ok());
    found = db.findClass(S("demo/Base"));
    ASSERT_TRUE(found.is_ok());
    auto baseAgain = std::move(found).unwrap();
    EXPECT_EQ(newEntryId, baseAgain->entryId);
    EXPECT_EQ("java/lang/Other", baseAgain->superName.to_std_string());

    // 不存在的条目 id：FK 校验整批拒绝，类事实不变。
    std::vector<DbClassInput> dangling;
    dangling.push_back(classInput("demo/Base", "java/lang/Object"));
    dangling.back().entryId = newEntryId + 42;
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.ingestClasses(0, std::move(dangling))));
    found = db.findClass(S("demo/Base"));
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(newEntryId, std::move(found).unwrap()->entryId);

    // listClasses 同样带出条目链接。
    auto all = db.listClasses();
    ASSERT_TRUE(all.is_ok());
    auto classList = std::move(all).unwrap();
    ASSERT_EQ(1U, classList.size());
    EXPECT_EQ(newEntryId, classList[0].entryId);
}

TEST(MjAnalysisDbTest, RejectsForeignSchemaVersionDatabase)
{
    // 旧库 fail-loud：application_id / user_version 落在 SQLite 文件头固定
    // 偏移（60 = user_version，68 = application_id，大端 u32）。在已保存的
    // 库文件上就地改戳即可构造「旧版本库」，版本门必须在触碰任何表之前以
    // Schema 错误拒绝——不提供自动迁移，旧库由上层显式重建。
    const auto patchHeaderField = [](const std::filesystem::path& file, std::fstream::off_type offset,
                                     ca::u32 value) {
        std::fstream io(file, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(io.good()) << file.string();
        const char bytes[4] = {static_cast<char>((value >> 24) & 0xff),
                               static_cast<char>((value >> 16) & 0xff),
                               static_cast<char>((value >> 8) & 0xff),
                               static_cast<char>(value & 0xff)};
        io.seekp(offset);
        io.write(bytes, 4);
        ASSERT_TRUE(io.good());
    };

    const auto v2Path = freshDbPath("entries_old_version");
    {
        auto db = openOrDie(v2Path);
        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
    }
    patchHeaderField(v2Path, 60, 2);   // 上一代 schema 戳
    {
        auto rejected = AnalysisDb::open(refOf(v2Path.string()));
        ASSERT_TRUE(rejected.is_err());
        EXPECT_EQ(AnalysisDbErrorKind::Schema, std::move(rejected).unwrap_err().kind);
        auto readOnly = AnalysisDb::open(refOf(v2Path.string()), AnalysisDbOpenMode::ReadOnly);
        ASSERT_TRUE(readOnly.is_err());
        EXPECT_EQ(AnalysisDbErrorKind::Schema, std::move(readOnly).unwrap_err().kind);
    }
    patchHeaderField(v2Path, 60, 99);   // 未知未来版本同样拒绝
    {
        auto rejected = AnalysisDb::open(refOf(v2Path.string()));
        ASSERT_TRUE(rejected.is_err());
        EXPECT_EQ(AnalysisDbErrorKind::Schema, std::move(rejected).unwrap_err().kind);
    }
    patchHeaderField(v2Path, 60, 4);   // v4：随包词条 0.0.10 发布过的真实上一代
    {
        auto rejected = AnalysisDb::open(refOf(v2Path.string()));
        ASSERT_TRUE(rejected.is_err());
        // 错误可诊断：拒绝消息带上被拒版本号，重导入动线由上层据此提示。
        auto error = std::move(rejected).unwrap_err();
        EXPECT_EQ(AnalysisDbErrorKind::Schema, error.kind);
        EXPECT_NE(std::string::npos, error.message.to_std_string().find("version 4"));
    }
    patchHeaderField(v2Path, 60, 5);   // v5：refs 目标侧 id 化一代
    {
        auto rejected = AnalysisDb::open(refOf(v2Path.string()));
        ASSERT_TRUE(rejected.is_err());
        auto error = std::move(rejected).unwrap_err();
        EXPECT_EQ(AnalysisDbErrorKind::Schema, error.kind);
        EXPECT_NE(std::string::npos, error.message.to_std_string().find("version 5"));
    }
    patchHeaderField(v2Path, 60, 6);   // v6：producer 注册表一代
    {
        auto rejected = AnalysisDb::open(refOf(v2Path.string()));
        ASSERT_TRUE(rejected.is_err());
        auto error = std::move(rejected).unwrap_err();
        EXPECT_EQ(AnalysisDbErrorKind::Schema, error.kind);
        EXPECT_NE(std::string::npos, error.message.to_std_string().find("version 6"));
    }
    patchHeaderField(v2Path, 60, 7);   // v7：mask 配置面归属化一代（v8 的直接前代）
    {
        auto rejected = AnalysisDb::open(refOf(v2Path.string()));
        ASSERT_TRUE(rejected.is_err());
        auto error = std::move(rejected).unwrap_err();
        EXPECT_EQ(AnalysisDbErrorKind::Schema, error.kind);
        EXPECT_NE(std::string::npos, error.message.to_std_string().find("version 7"));
    }
    patchHeaderField(v2Path, 60, 8);   // v8：方法版本指纹层一代
    {
        auto rejected = AnalysisDb::open(refOf(v2Path.string()));
        ASSERT_TRUE(rejected.is_err());
        auto error = std::move(rejected).unwrap_err();
        EXPECT_EQ(AnalysisDbErrorKind::Schema, error.kind);
        EXPECT_NE(std::string::npos, error.message.to_std_string().find("version 8"));
    }
    patchHeaderField(v2Path, 60, 9);   // v9：CHA 闭包一代（v10 的直接前代）
    {
        auto rejected = AnalysisDb::open(refOf(v2Path.string()));
        ASSERT_TRUE(rejected.is_err());
        auto error = std::move(rejected).unwrap_err();
        EXPECT_EQ(AnalysisDbErrorKind::Schema, error.kind);
        EXPECT_NE(std::string::npos, error.message.to_std_string().find("version 9"));
    }

    const auto foreignPath = freshDbPath("entries_foreign_app");
    {
        auto db = openOrDie(foreignPath);
        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
    }
    patchHeaderField(foreignPath, 68, 0x12345678u);   // application_id 不匹配
    auto foreign = AnalysisDb::open(refOf(foreignPath.string()));
    ASSERT_TRUE(foreign.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::Schema, std::move(foreign).unwrap_err().kind);
}

//-- v5 引用目标侧 id 化：行形状、CHECK 门、登记时序等价、字节语义、查询计划 --

namespace {

/// @brief 混合行形状语料（行形状检查与查询计划检查共用）：已登记目标与
///        未登记目标混排，demo/Later 刻意不登记（成员引用文本行常驻）。
///        五行覆盖四类行形状：Type 登记目标、Type 未登记目标、成员双登记、
///        成员未登记。写完 save + close 收敛单文件。
void writeMixedCorpus(const std::filesystem::path& path)
{
    auto db = openOrDie(path);
    std::vector<DbClassInput> inputs;
    DbClassInput alpha = classInput("demo/Alpha", "java/lang/Object");
    alpha.members.push_back(methodInput("run", "()V"));
    alpha.members.push_back(fieldInput("counter", "I"));
    inputs.push_back(std::move(alpha));
    DbClassInput beta = classInput("demo/Beta", "java/lang/Object");
    beta.members.push_back(methodInput("go", "()V"));
    inputs.push_back(std::move(beta));
    inputs.push_back(classInput("demo/Gamma", "java/lang/Object"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 3,
                                         DbReferenceKind::Method, "demo/Beta", "go", "()V"));
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 4,
                                         DbReferenceKind::Field, "demo/Alpha", "counter", "I"));
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 5,
                                         DbReferenceKind::Type, "demo/Gamma", "", ""));
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 6,
                                         DbReferenceKind::Type, "demo/External", "", ""));
    references.push_back(referenceRecord("demo/Alpha", "run", "()V", 7,
                                         DbReferenceKind::Method, "demo/Later", "later", "()V"));
    ASSERT_TRUE(db.replaceReferences(0, std::move(references)).is_ok());
    ASSERT_TRUE(db.save().is_ok());
    ASSERT_TRUE(db.close().is_ok());
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

/// @brief 计划里存在走指定索引的 SEARCH 行（索引下探证据；老版 SQLite 的
///        "SEARCH TABLE refs AS r USING INDEX x" 与新版 "SEARCH r USING
///        INDEX x" 两种 detail 文案都命中）。
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

/// @brief refs（别名 r）被裸 SCAN（无 USING 的全表扫）即回归。
///
/// 文本分支类目标查询允许 "SCAN r USING PARTIAL INDEX refs_target_text"
/// （部分索引全扫，PoC 实测的诚实代价），裸表扫才是漏建索引的信号。
bool hasBareRefsScan(const std::vector<std::string>& details)
{
    for (const auto& detail : details) {
        const bool refsScan = detail.rfind("SCAN", 0) == 0 &&
                              (detail.find("refs") != std::string::npos ||
                               detail.rfind("SCAN r", 0) == 0);
        if (refsScan && detail.find("USING") == std::string::npos) {
            return true;
        }
    }
    return false;
}

/// @brief 目标侧两分支 UNION ALL 查询的计划检查共用分支形状（镜像
///        analysis_db.cpp 的 kReferenceTargetBranchSql）。
const char* kTestTargetBranchSql =
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

/// @brief 引用查询输出形状（镜像 kReferenceSelectSql，计划里 source 探测
///        用的就是这个 join 形状）。
const char* kTestReferenceSelectSql =
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

}   // namespace

TEST(MjAnalysisDbTest, TargetRowsBinarizeAndCheckRejectsMixedShapes)
{
    const auto path = freshDbPath("refs_v5_shapes");
    writeMixedCorpus(path);

    // 四类行形状各就其位（v5 二元化：id 行文本三列清空、target_class 为
    // NULL；文本行两个 id 列为 NULL；无混合行）。
    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);
    EXPECT_EQ(5, rawScalar(raw, "SELECT COUNT(*) FROM refs"));
    EXPECT_EQ(1, rawScalar(raw,
        "SELECT COUNT(*) FROM refs WHERE ref_kind=0 AND target_class_id IS NOT NULL "
        "AND target_member_id IS NULL AND target_class IS NULL "
        "AND target_name='' AND target_desc=''"));
    EXPECT_EQ(1, rawScalar(raw,
        "SELECT COUNT(*) FROM refs WHERE ref_kind=0 AND target_class_id IS NULL "
        "AND target_member_id IS NULL AND target_class='demo/External' "
        "AND target_name='' AND target_desc=''"));
    EXPECT_EQ(2, rawScalar(raw,
        "SELECT COUNT(*) FROM refs WHERE ref_kind!=0 AND target_class_id IS NOT NULL "
        "AND target_member_id IS NOT NULL AND target_class IS NULL "
        "AND target_name='' AND target_desc=''"));
    EXPECT_EQ(1, rawScalar(raw,
        "SELECT COUNT(*) FROM refs WHERE ref_kind=2 AND target_class_id IS NULL "
        "AND target_member_id IS NULL AND target_class='demo/Later' "
        "AND target_name='later' AND target_desc='()V'"));
    sqlite3_close(raw);

    // 三条 CHECK 与代码校验同源：混合行拒写（裸读写连接直插验证）。
    sqlite3* rw = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open_v2(path.string().c_str(), &rw,
                                         SQLITE_OPEN_READWRITE, nullptr));
    const auto execRaw = [&](const char* sql) {
        char*       message = nullptr;
        const int   rc      = sqlite3_exec(rw, sql, nullptr, nullptr, &message);
        std::string detail  = message != nullptr ? message : "";
        sqlite3_free(message);
        return std::make_pair(rc, detail);
    };

    // owner 走 id、成员走文本的混合行：CHECK((class_id IS NULL) != (class IS NULL)) 拒绝。
    auto mixed = execRaw(
        "INSERT INTO refs(source_class_id, insn_index, ref_kind, target_class_id, "
        "target_class, target_name, target_desc) VALUES("
        "(SELECT class_id FROM classes WHERE internal_name='demo/Alpha'), 900, 1, "
        "(SELECT class_id FROM classes WHERE internal_name='demo/Beta'), "
        "'demo/Beta', 'go', '()V')");
    EXPECT_NE(SQLITE_OK, mixed.first);
    EXPECT_NE(std::string::npos, mixed.second.find("CHECK"));

    // 成员 id 行携带非空文本：CHECK(member_id IS NULL OR (name='' AND desc='')) 拒绝。
    auto texted = execRaw(
        "INSERT INTO refs(source_class_id, insn_index, ref_kind, target_class_id, "
        "target_member_id, target_name, target_desc) VALUES("
        "(SELECT class_id FROM classes WHERE internal_name='demo/Alpha'), 901, 1, "
        "(SELECT class_id FROM classes WHERE internal_name='demo/Beta'), "
        "(SELECT member_id FROM members WHERE name='counter'), 'counter', '')");
    EXPECT_NE(SQLITE_OK, texted.first);
    EXPECT_NE(std::string::npos, texted.second.find("CHECK"));

    // Type 引用挂成员 id：CHECK(member_id IS NULL OR ref_kind != 0) 拒绝。
    auto typed = execRaw(
        "INSERT INTO refs(source_class_id, insn_index, ref_kind, target_class_id, "
        "target_member_id) VALUES("
        "(SELECT class_id FROM classes WHERE internal_name='demo/Alpha'), 902, 0, "
        "(SELECT class_id FROM classes WHERE internal_name='demo/Gamma'), "
        "(SELECT member_id FROM members WHERE name='counter'))");
    EXPECT_NE(SQLITE_OK, typed.first);
    EXPECT_NE(std::string::npos, typed.second.find("CHECK"));

    // 正向对照：合法 id 行（双登记成员）可写，随后清理复原行数。
    auto valid = execRaw(
        "INSERT INTO refs(source_class_id, insn_index, ref_kind, target_class_id, "
        "target_member_id) VALUES("
        "(SELECT class_id FROM classes WHERE internal_name='demo/Alpha'), 903, 1, "
        "(SELECT class_id FROM classes WHERE internal_name='demo/Beta'), "
        "(SELECT member_id FROM members WHERE name='go'))");
    EXPECT_EQ(SQLITE_OK, valid.first) << valid.second;
    EXPECT_EQ(6, rawScalar(rw, "SELECT COUNT(*) FROM refs"));
    EXPECT_EQ(SQLITE_OK, execRaw("DELETE FROM refs WHERE insn_index=903").first);
    EXPECT_EQ(5, rawScalar(rw, "SELECT COUNT(*) FROM refs"));
    sqlite3_close(rw);
}

TEST(MjAnalysisDbTest, TargetQueriesStayEquivalentAcrossRegistrationTiming)
{
    const auto path = freshDbPath("refs_v5_equivalence");
    auto       db   = openOrDie(path);

    std::vector<DbClassInput> inputs;
    DbClassInput alpha = classInput("demo/Alpha", "java/lang/Object");
    alpha.members.push_back(methodInput("run", "()V"));
    alpha.members.push_back(fieldInput("counter", "I"));
    inputs.push_back(std::move(alpha));
    DbClassInput beta = classInput("demo/Beta", "java/lang/Object");
    beta.members.push_back(methodInput("go", "()V"));
    inputs.push_back(std::move(beta));
    inputs.push_back(classInput("demo/Gamma", "java/lang/Object"));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    // 9 行混排语料：登记目标（id 行）与未登记目标（文本行）；demo/Later
    // 刻意在引用写入之后才登记（登记时序交错）。行序 = 写入序。记录只移
    // 不拷（Utf8String），基线行用工厂按需重建。
    const auto r1 = [] { return referenceRecord("demo/Alpha", "run", "()V", 3,
                                                DbReferenceKind::Method, "demo/Beta", "go", "()V"); };
    const auto r2 = [] { return referenceRecord("demo/Alpha", "run", "()V", 4,
                                                DbReferenceKind::Field, "demo/Alpha", "counter", "I"); };
    const auto r3 = [] { return referenceRecord("demo/Alpha", "run", "()V", 5,
                                                DbReferenceKind::Type, "demo/Gamma", "", ""); };
    const auto r4 = [] { return referenceRecord("demo/Alpha", "run", "()V", 6,
                                                DbReferenceKind::Type, "demo/External", "", ""); };
    const auto r5 = [] { return referenceRecord("demo/Alpha", "run", "()V", 7,
                                                DbReferenceKind::Method, "demo/Later", "later", "()V"); };
    const auto r6 = [] { return referenceRecord("demo/Beta", "go", "()V", 0,
                                                DbReferenceKind::Field, "demo/External", "extField", "I"); };
    const auto r7 = [] { return referenceRecord("demo/Beta", "go", "()V", 1,
                                                DbReferenceKind::Method, "demo/Beta", "go", "()V"); };
    const auto r8 = [] { return referenceRecord("demo/Alpha", "", "", -1, DbReferenceKind::Type,
                                                "demo/External", "", ""); };
    const auto r9 = [] { return referenceRecord("demo/Beta", "", "", -1, DbReferenceKind::Type,
                                                "demo/Gamma", "", ""); };
    auto references = recordSet(r1(), r2(), r3(), r4(), r5(), r6(), r7(), r8(), r9());
    ASSERT_TRUE(db.replaceReferences(0, std::move(references)).is_ok());
    EXPECT_EQ(9U, db.referenceCount().unwrap());

    const auto rowsOf = [&](auto result) {
        EXPECT_TRUE(result.is_ok());
        return std::move(result).unwrap();
    };

    // 按源反查（source 侧零改动面）：class 头行（member NULL）排最前，方法体
    // 行按 insn 升序。
    expectSameRecords(recordSet(r1(), r2(), r3(), r4(), r5()),
                      rowsOf(db.referencesFromMember(S("demo/Alpha"), S("run"), S("()V"))));
    expectSameRecords(recordSet(r8(), r1(), r2(), r3(), r4(), r5()),
                      rowsOf(db.referencesFromClass(S("demo/Alpha"))));
    expectSameRecords(recordSet(r6(), r7()),
                      rowsOf(db.referencesFromMember(S("demo/Beta"), S("go"), S("()V"))));
    expectSameRecords(recordSet(r9(), r6(), r7()), rowsOf(db.referencesFromClass(S("demo/Beta"))));

    // 按目标反查：id 分支（登记目标）与文本分支（未登记目标）拼接结果与
    // 名字键控基线逐行一致；demo/Later 尚未登记，全靠文本分支。
    expectSameRecords(recordSet(r1(), r7()),
                      rowsOf(db.referencesToMethod(S("demo/Beta"), S("go"), S("()V"))));
    expectSameRecords(recordSet(r2()),
                      rowsOf(db.referencesToField(S("demo/Alpha"), S("counter"), S("I"))));
    expectSameRecords(recordSet(r6()),
                      rowsOf(db.referencesToField(S("demo/External"), S("extField"), S("I"))));
    expectSameRecords(recordSet(r5()),
                      rowsOf(db.referencesToMethod(S("demo/Later"), S("later"), S("()V"))));
    expectSameRecords(recordSet(r2()), rowsOf(db.referencesToClass(S("demo/Alpha"))));
    expectSameRecords(recordSet(r1(), r7()), rowsOf(db.referencesToClass(S("demo/Beta"))));
    expectSameRecords(recordSet(r3(), r9()), rowsOf(db.referencesToClass(S("demo/Gamma"))));
    expectSameRecords(recordSet(r8(), r4(), r6()), rowsOf(db.referencesToClass(S("demo/External"))));
    expectSameRecords(recordSet(r5()), rowsOf(db.referencesToClass(S("demo/Later"))));

    // 未登记目标/成员：空结果（非错误）。
    EXPECT_TRUE(rowsOf(db.referencesToClass(S("demo/Nowhere"))).empty());
    EXPECT_TRUE(
        rowsOf(db.referencesToMethod(S("demo/External"), S("nope"), S("()V"))).empty());

    // UNION ALL 不重不漏：各目标命中数之和 == 库内引用总数（行按目标恰计
    // 一次——id 形状与文本形状互斥，任何一行只可能落进一个分支）。
    ca::usize summed = rowsOf(db.referencesToClass(S("demo/Alpha"))).size() +
                       rowsOf(db.referencesToClass(S("demo/Beta"))).size() +
                       rowsOf(db.referencesToClass(S("demo/Gamma"))).size() +
                       rowsOf(db.referencesToClass(S("demo/External"))).size() +
                       rowsOf(db.referencesToClass(S("demo/Later"))).size();
    EXPECT_EQ(9U, summed);

    // 分页窗口与全量同序：单行页拼接 = 全量（复合查询 + 外层排序 + 窗口）。
    std::vector<DbReferenceRecord> toClassStitched;
    for (ca::i64 offset = 0;; ++offset) {
        auto page = db.referencesToClassPaged(S("demo/External"), 1, offset);
        ASSERT_TRUE(page.is_ok());
        auto rows = std::move(page).unwrap();
        if (rows.empty()) break;
        ASSERT_LE(rows.size(), 1U);
        toClassStitched.push_back(std::move(rows[0]));
    }
    expectSameRecords(recordSet(r8(), r4(), r6()), toClassStitched);
    std::vector<DbReferenceRecord> toMethodStitched;
    for (ca::i64 offset = 0;; ++offset) {
        auto page = db.referencesToMethodPaged(S("demo/Beta"), S("go"), S("()V"), 1, offset);
        ASSERT_TRUE(page.is_ok());
        auto rows = std::move(page).unwrap();
        if (rows.empty()) break;
        ASSERT_LE(rows.size(), 1U);
        toMethodStitched.push_back(std::move(rows[0]));
    }
    expectSameRecords(recordSet(r1(), r7()), toMethodStitched);

    // 登记时序交错第一段：Later 现在登记——既有文本行原样保留（身份只增
    // 不删不改既有编码），目标反查经文本分支仍然命中。
    std::vector<DbClassInput> laterBatch;
    DbClassInput laterClass = classInput("demo/Later", "java/lang/Object");
    laterClass.members.push_back(methodInput("later", "()V"));
    laterBatch.push_back(std::move(laterClass));
    ASSERT_TRUE(db.ingestClasses(0, std::move(laterBatch)).is_ok());
    expectSameRecords(recordSet(r5()),
                      rowsOf(db.referencesToMethod(S("demo/Later"), S("later"), S("()V"))));
    expectSameRecords(recordSet(r5()), rowsOf(db.referencesToClass(S("demo/Later"))));

    // 登记时序交错第二段：切片重写 A.run——同一逻辑目标改落 id 行，反查
    // 结果逐行一致（insn 更新为 70），其余方向与切片语义对齐。
    auto runMember = db.findMember(DbMemberKind::Method, S("demo/Alpha"), S("run"), S("()V"));
    ASSERT_TRUE(runMember.is_ok());
    const ca::i64 runId = std::move(runMember).unwrap()->memberId;
    const auto r5b = [] { return referenceRecord("demo/Alpha", "run", "()V", 70,
                                                 DbReferenceKind::Method, "demo/Later", "later", "()V"); };
    auto slice = recordSet(r5b());
    ASSERT_TRUE(db.replaceReferencesFromMember(0, runId, std::move(slice)).is_ok());
    expectSameRecords(recordSet(r5b()),
                      rowsOf(db.referencesToMethod(S("demo/Later"), S("later"), S("()V"))));
    expectSameRecords(recordSet(r5b()),
                      rowsOf(db.referencesFromMember(S("demo/Alpha"), S("run"), S("()V"))));
    expectSameRecords(recordSet(r7()),
                      rowsOf(db.referencesToMethod(S("demo/Beta"), S("go"), S("()V"))));
    expectSameRecords(recordSet(r8(), r6()), rowsOf(db.referencesToClass(S("demo/External"))));
    EXPECT_EQ(5U, db.referenceCount().unwrap());
}

TEST(MjAnalysisDbTest, TargetTextRoundTripsByteSemantics)
{
    const auto path = freshDbPath("refs_v5_bytes");
    auto       db   = openOrDie(path);

    // 对抗名（非合法 UTF-8 字节流，无 NUL——NUL 走既有 Invalid 校验）：
    // 登记成员名（id 分支）与未登记 owner/成员名（文本分支）都要按字节往返。
    const std::string binField("\xFF\x80z", 3);
    const std::string rawOwner("\xC3\x28q", 3);
    const std::string rawName("\xFF\xFE", 2);
    const std::string rawDesc("\x80J", 2);

    std::vector<DbClassInput> inputs;
    DbClassInput alpha = classInput("demo/Alpha", "java/lang/Object");
    alpha.members.push_back(methodInput("run", "()V"));
    inputs.push_back(std::move(alpha));
    DbClassInput bin = classInput("demo/Bin", "java/lang/Object");
    DbMemberInput hostileField;
    hostileField.kind       = DbMemberKind::Field;
    hostileField.name       = Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(binField.data()), binField.size());
    hostileField.descriptor = Utf8String::from_cstr("I");
    hostileField.access     = 0x0001;
    bin.members.push_back(std::move(hostileField));
    inputs.push_back(std::move(bin));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());

    // from_cstr 走严格 UTF-8 解码（非法字节静默落空串）——对抗名必须走
    // from_data_unchecked 字节构造；读写两侧按字节绑定与还原，不解码。
    const auto bytesOf = [](const std::string& raw) {
        return Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(raw.data()),
                                               raw.size());
    };
    const auto byteRecord = [&](const char* sourceClass, const char* sourceMethod,
                                const char* sourceDesc, ca::i64 insn, DbReferenceKind kind,
                                const std::string& targetClass, const std::string& targetName,
                                const std::string& targetDesc) {
        DbReferenceRecord record;
        record.sourceClass      = Utf8String::from_cstr(sourceClass);
        record.sourceMethod     = Utf8String::from_cstr(sourceMethod);
        record.sourceDescriptor = Utf8String::from_cstr(sourceDesc);
        record.insnIndex        = insn;
        record.kind             = kind;
        record.targetClass      = bytesOf(targetClass);
        record.targetName       = bytesOf(targetName);
        record.targetDescriptor = bytesOf(targetDesc);
        return record;
    };

    auto r1 = byteRecord("demo/Alpha", "run", "()V", 0, DbReferenceKind::Field,
                         "demo/Bin", binField, "I");
    auto r2 = byteRecord("demo/Alpha", "run", "()V", 1, DbReferenceKind::Type,
                         rawOwner, "", "");
    auto r3 = byteRecord("demo/Alpha", "run", "()V", 2, DbReferenceKind::Field,
                         rawOwner, rawName, rawDesc);
    std::vector<DbReferenceRecord> references;
    references.push_back(std::move(r1));
    references.push_back(std::move(r2));
    references.push_back(std::move(r3));
    ASSERT_TRUE(db.replaceReferences(0, std::move(references)).is_ok());

    const auto expectBytes = [](const Utf8String& actual, const std::string& expected) {
        ASSERT_EQ(expected.size(), actual.byte_length());
        EXPECT_EQ(0, std::memcmp(actual.data(), expected.data(), expected.size()));
    };

    // 按源反查逐行字节还原（读写两侧都走带长度的字节 API）。
    auto fromRun = db.referencesFromMember(S("demo/Alpha"), S("run"), S("()V"));
    ASSERT_TRUE(fromRun.is_ok());
    auto runRows = std::move(fromRun).unwrap();
    ASSERT_EQ(3U, runRows.size());
    expectBytes(runRows[0].targetClass, "demo/Bin");
    expectBytes(runRows[0].targetName, binField);
    expectBytes(runRows[0].targetDescriptor, "I");
    expectBytes(runRows[1].targetClass, rawOwner);
    expectBytes(runRows[2].targetClass, rawOwner);
    expectBytes(runRows[2].targetName, rawName);
    expectBytes(runRows[2].targetDescriptor, rawDesc);

    // id 分支：对抗名成员已登记，按字节键解析 member_id 命中。
    auto toField = db.referencesToField(S("demo/Bin"), refOf(binField), S("I"));
    ASSERT_TRUE(toField.is_ok());
    auto fieldRows = std::move(toField).unwrap();
    ASSERT_EQ(1U, fieldRows.size());
    expectBytes(fieldRows[0].targetClass, "demo/Bin");
    expectBytes(fieldRows[0].targetName, binField);

    // 文本分支：未登记 owner 与成员按字节键命中。
    auto toClass = db.referencesToClass(refOf(rawOwner));
    ASSERT_TRUE(toClass.is_ok());
    auto classRows = std::move(toClass).unwrap();
    ASSERT_EQ(2U, classRows.size());
    expectBytes(classRows[0].targetClass, rawOwner);
    expectBytes(classRows[1].targetClass, rawOwner);
    expectBytes(classRows[1].targetName, rawName);
    expectBytes(classRows[1].targetDescriptor, rawDesc);

    auto toRawField = db.referencesToField(refOf(rawOwner), refOf(rawName), refOf(rawDesc));
    ASSERT_TRUE(toRawField.is_ok());
    auto rawFieldRows = std::move(toRawField).unwrap();
    ASSERT_EQ(1U, rawFieldRows.size());
    expectBytes(rawFieldRows[0].targetClass, rawOwner);
    expectBytes(rawFieldRows[0].targetName, rawName);
    expectBytes(rawFieldRows[0].targetDescriptor, rawDesc);
}

TEST(MjAnalysisDbTest, ReferenceQueryPlansProbeIndexesWithoutFullScans)
{
    const auto path = freshDbPath("refs_v5_plans");
    writeMixedCorpus(path);

    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);

    // 索引沿用清单：source 侧两条（漏建回归 0.575 → 1.672ms）+ v5 两条部分索引。
    EXPECT_EQ(4, rawScalar(raw,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name IN "
        "('refs_source_class','refs_source_member','refs_target_id','refs_target_text')"));

    // 按源反查：refs_source_member / refs_source_class 索引下探，无 refs 裸扫。
    {
        auto details = rawQueryPlanDetails(
            raw, std::string(kTestReferenceSelectSql) +
                     "WHERE r.source_member_id=1 ORDER BY r.source_class_id, "
                     "r.source_member_id, r.insn_index, r.ref_id");
        EXPECT_TRUE(hasSearchOnIndex(details, "refs_source_member"));
        EXPECT_FALSE(hasBareRefsScan(details));
    }
    {
        auto details = rawQueryPlanDetails(
            raw, std::string(kTestReferenceSelectSql) +
                     "WHERE r.source_class_id=1 ORDER BY r.source_class_id, "
                     "r.source_member_id, r.insn_index, r.ref_id");
        EXPECT_TRUE(hasSearchOnIndex(details, "refs_source_class"));
        EXPECT_FALSE(hasBareRefsScan(details));
    }

    // 目标成员反查（referencesToMethod/Field 形状）：id 分支 refs_target_id
    // 双列探 + 文本分支 refs_target_text 四元组探，两分支都走索引。
    {
        const std::string compound =
            std::string("SELECT v.source_class, v.source_member, v.source_descriptor, "
                        "v.insn_index, v.ref_kind, v.target_class, v.target_name, "
                        "v.target_desc FROM (") +
            kTestTargetBranchSql +
            "WHERE r.ref_kind=2 AND r.target_class_id=?1 AND r.target_member_id=?2 "
            "UNION ALL " + kTestTargetBranchSql +
            "WHERE r.ref_kind=2 AND r.target_class_id IS NULL AND r.target_class=?3 "
            "AND r.target_name=?4 AND r.target_desc=?5 "
            ") v ORDER BY v.order_class, v.order_member, v.order_insn, v.order_ref";
        auto details = rawQueryPlanDetails(raw, compound);
        EXPECT_TRUE(hasSearchOnIndex(details, "refs_target_id"));
        EXPECT_TRUE(hasSearchOnIndex(details, "refs_target_text"));
        EXPECT_FALSE(hasBareRefsScan(details));
    }

    // 目标类反查（referencesToClass 形状）：id 分支 refs_target_id 前缀探；
    // 文本分支允许部分索引全扫（PoC 实测的诚实代价），但绝不裸扫表。
    {
        const std::string compound =
            std::string("SELECT v.source_class, v.source_member, v.source_descriptor, "
                        "v.insn_index, v.ref_kind, v.target_class, v.target_name, "
                        "v.target_desc FROM (") +
            kTestTargetBranchSql +
            "WHERE r.target_class_id=?1 "
            "UNION ALL " + kTestTargetBranchSql +
            "WHERE r.target_class_id IS NULL AND r.target_class=?2 "
            ") v ORDER BY v.order_class, v.order_member, v.order_insn, v.order_ref";
        auto details = rawQueryPlanDetails(raw, compound);
        EXPECT_TRUE(hasSearchOnIndex(details, "refs_target_id"));
        bool textBranchOnIndex = false;
        for (const auto& detail : details) {
            if (detail.find("refs_target_text") != std::string::npos) textBranchOnIndex = true;
        }
        EXPECT_TRUE(textBranchOnIndex);
        EXPECT_FALSE(hasBareRefsScan(details));
    }

    sqlite3_close(raw);
}

//-- 方法版本指纹层（method_versions，schema v8，#132）--------------------------

namespace {

using mjbase::analysis::SemanticFp;
using mjbase::analysis::kSemanticFpHashBytes;
using mjbase::analysis::kSemanticFpVersion;

/// @brief 构造确定性指纹：32 字节 = seed + i（不同 seed 得到不同指纹）。
///        全零字节模式不在此生成——那是引擎不可能的输出，拒绝用例显式构造。
SemanticFp fpOf(ca::u8 seed)
{
    SemanticFp fp;
    for (ca::usize i = 0; i < kSemanticFpHashBytes; ++i) {
        fp.bytes[i] = static_cast<ca::u8>(seed + i);
    }
    return fp;
}

/// @brief 覆盖全部 256 个字节值的指纹模式（bytes[i] = i）：边界字节逐位还原。
SemanticFp allBytesPattern()
{
    SemanticFp fp;
    for (ca::usize i = 0; i < kSemanticFpHashBytes; ++i) {
        fp.bytes[i] = static_cast<ca::u8>(i);
    }
    return fp;
}

/// @brief 高位全 1 的对偶指纹模式（防高位截断）。
SemanticFp highOnesPattern()
{
    SemanticFp fp;
    for (auto& byte : fp.bytes) byte = 0xFF;
    return fp;
}

/// @brief 方法版本语料的身份（resolveMethodVersionCorpus 产出）。
struct MethodVersionCorpus
{
    ca::i64 classId     = 0; ///< demo/Alpha
    ca::i64 runId       = 0; ///< demo/Alpha.run()I
    ca::i64 goId        = 0; ///< demo/Alpha.go()V
    ca::i64 fieldId     = 0; ///< demo/Alpha.counter:I（字段，指纹面外）
    ca::i64 betaClassId = 0; ///< demo/Beta
    ca::i64 betaGoId    = 0; ///< demo/Beta.go()V（跨类重名方法）
};

/// @brief 登记方法版本语料：两类各带方法，Alpha 另带一个字段（负例用）。
void writeMethodVersionCorpus(AnalysisDb& db)
{
    std::vector<DbClassInput> inputs;
    DbClassInput alpha = classInput("demo/Alpha", "java/lang/Object");
    alpha.members.push_back(methodInput("run", "()I"));
    alpha.members.push_back(methodInput("go", "()V"));
    alpha.members.push_back(fieldInput("counter", "I"));
    inputs.push_back(std::move(alpha));
    DbClassInput beta = classInput("demo/Beta", "java/lang/Object");
    beta.members.push_back(methodInput("go", "()V"));
    inputs.push_back(std::move(beta));
    ASSERT_TRUE(db.ingestClasses(0, std::move(inputs)).is_ok());
}

/// @brief 解析语料身份（只读连接可用；重开后 id 不漂移）。
MethodVersionCorpus resolveMethodVersionCorpus(const AnalysisDb& db)
{
    MethodVersionCorpus corpus;
    auto alphaClass = db.findClass(S("demo/Alpha"));
    EXPECT_TRUE(alphaClass.is_ok());
    corpus.classId = std::move(alphaClass).unwrap()->classId;
    auto run = db.findMember(DbMemberKind::Method, S("demo/Alpha"), S("run"), S("()I"));
    EXPECT_TRUE(run.is_ok());
    corpus.runId = std::move(run).unwrap()->memberId;
    auto go = db.findMember(DbMemberKind::Method, S("demo/Alpha"), S("go"), S("()V"));
    EXPECT_TRUE(go.is_ok());
    corpus.goId = std::move(go).unwrap()->memberId;
    auto field = db.findMember(DbMemberKind::Field, S("demo/Alpha"), S("counter"), S("I"));
    EXPECT_TRUE(field.is_ok());
    corpus.fieldId = std::move(field).unwrap()->memberId;
    auto betaClass = db.findClass(S("demo/Beta"));
    EXPECT_TRUE(betaClass.is_ok());
    corpus.betaClassId = std::move(betaClass).unwrap()->classId;
    auto betaGo = db.findMember(DbMemberKind::Method, S("demo/Beta"), S("go"), S("()V"));
    EXPECT_TRUE(betaGo.is_ok());
    corpus.betaGoId = std::move(betaGo).unwrap()->memberId;
    return corpus;
}

/// @brief 读出侧 hex 契约：64 位小写 hex 且逐位对应指纹字节（派生重建）。
void expectHexMatchesBytes(const SemanticFp& fp)
{
    ASSERT_EQ(64U, fp.hex.byte_length());
    const std::string text = fp.hex.to_std_string();
    static constexpr char kDigits[] = "0123456789abcdef";
    for (ca::usize i = 0; i < kSemanticFpHashBytes; ++i) {
        EXPECT_EQ(kDigits[fp.bytes[i] >> 4], text[2 * i]) << "byte " << i;
        EXPECT_EQ(kDigits[fp.bytes[i] & 0x0F], text[2 * i + 1]) << "byte " << i;
    }
}

/// @brief 期望指纹逐字节相等（32 字节 BLOB 逐位还原的核心断言）。
void expectSameFingerprint(const SemanticFp& actual, const SemanticFp& expected)
{
    EXPECT_EQ(0, std::memcmp(actual.bytes.data(), expected.bytes.data(),
                             kSemanticFpHashBytes));
}

/// @brief method_versions 被裸 SCAN（无 USING 的全表扫）即回归。
bool hasBareMethodVersionsScan(const std::vector<std::string>& details)
{
    for (const auto& detail : details) {
        const bool scan = detail.rfind("SCAN", 0) == 0 &&
                          detail.find("method_versions") != std::string::npos;
        if (scan && detail.find("USING") == std::string::npos) return true;
    }
    return false;
}

}   // namespace

TEST(MjAnalysisDbTest, MethodVersionRecordThenCurrentQueryLoop)
{
    const auto path = freshDbPath("mv_loop");
    {
        auto db = openOrDie(path);
        writeMethodVersionCorpus(db);
        auto corpus = resolveMethodVersionCorpus(db);

        // 未落版本：合法空态（成员已登记、无版本行）。
        auto none = db.currentMethodFingerprint(corpus.runId);
        ASSERT_TRUE(none.is_ok());
        EXPECT_FALSE(std::move(none).unwrap().has_value());
        auto emptyHistory = db.listMethodVersions(corpus.runId);
        ASSERT_TRUE(emptyHistory.is_ok());
        EXPECT_TRUE(std::move(emptyHistory).unwrap().empty());

        auto recorded = db.recordMethodVersion(corpus.classId, corpus.runId, fpOf(7));
        ASSERT_TRUE(recorded.is_ok());
        const ca::i64 firstVersionId = std::move(recorded).unwrap();
        EXPECT_GT(firstVersionId, 0);

        auto current = db.currentMethodFingerprint(corpus.runId);
        ASSERT_TRUE(current.is_ok());
        auto fp = std::move(current).unwrap();
        ASSERT_TRUE(fp.has_value());
        expectSameFingerprint(*fp, fpOf(7));
        expectHexMatchesBytes(*fp);

        // 同指纹幂等：返回同一当前行 id，不产生伪版本行。
        auto again = db.recordMethodVersion(corpus.classId, corpus.runId, fpOf(7));
        ASSERT_TRUE(again.is_ok());
        EXPECT_EQ(firstVersionId, std::move(again).unwrap());
        auto history = db.listMethodVersions(corpus.runId);
        ASSERT_TRUE(history.is_ok());
        EXPECT_EQ(1U, std::move(history).unwrap().size());

        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
    }

    // 跨重开持久：只读连接逐字节还原。
    auto reopened = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    auto corpus   = resolveMethodVersionCorpus(reopened);
    auto current  = reopened.currentMethodFingerprint(corpus.runId);
    ASSERT_TRUE(current.is_ok());
    auto fp = std::move(current).unwrap();
    ASSERT_TRUE(fp.has_value());
    expectSameFingerprint(*fp, fpOf(7));
}

TEST(MjAnalysisDbTest, MethodVersionHistoryAccumulatesAndCurrentStaysUnique)
{
    const auto path = freshDbPath("mv_history");
    {
        auto db = openOrDie(path);
        writeMethodVersionCorpus(db);
        auto corpus = resolveMethodVersionCorpus(db);

        ASSERT_TRUE(db.recordMethodVersion(corpus.classId, corpus.runId, fpOf(1)).is_ok());
        ASSERT_TRUE(db.recordMethodVersion(corpus.classId, corpus.runId, fpOf(2)).is_ok());
        auto third = db.recordMethodVersion(corpus.classId, corpus.runId, fpOf(3));
        ASSERT_TRUE(third.is_ok());
        const ca::i64 thirdId = std::move(third).unwrap();

        // 版本历史累积：三行、id 升序（= 记录序）、只有最新一行是当前，
        // 每行盖 kSemanticFpVersion 算法版本字节并带记录时间。
        auto history = db.listMethodVersions(corpus.runId);
        ASSERT_TRUE(history.is_ok());
        auto rows = std::move(history).unwrap();
        ASSERT_EQ(3U, rows.size());
        for (ca::usize i = 1; i < rows.size(); ++i) {
            EXPECT_GT(rows[i].methodVersionId, rows[i - 1].methodVersionId);
        }
        EXPECT_FALSE(rows[0].isCurrent);
        EXPECT_FALSE(rows[1].isCurrent);
        EXPECT_TRUE(rows[2].isCurrent);
        EXPECT_EQ(thirdId, rows[2].methodVersionId);
        expectSameFingerprint(rows[0].fingerprint, fpOf(1));
        expectSameFingerprint(rows[1].fingerprint, fpOf(2));
        expectSameFingerprint(rows[2].fingerprint, fpOf(3));
        for (const auto& row : rows) {
            EXPECT_EQ(kSemanticFpVersion, row.fpVersion);
            EXPECT_EQ(corpus.classId, row.classId);
            EXPECT_EQ(corpus.runId, row.memberId);
            EXPECT_FALSE(row.recordedAt.is_empty());
        }

        // 语义回退（fp3 → fp1）也是新版本事件：历史累积第四行，当前指回
        // 同指纹的「新」行（版本行只增不删，不与历史行合并）。
        ASSERT_TRUE(db.recordMethodVersion(corpus.classId, corpus.runId, fpOf(1)).is_ok());
        auto reverted = db.listMethodVersions(corpus.runId);
        ASSERT_TRUE(reverted.is_ok());
        auto revertRows = std::move(reverted).unwrap();
        ASSERT_EQ(4U, revertRows.size());
        EXPECT_TRUE(revertRows[3].isCurrent);
        EXPECT_FALSE(revertRows[2].isCurrent);
        expectSameFingerprint(revertRows[3].fingerprint, fpOf(1));

        // 旧指纹（fp2）查不中当前行 = 标脏语义可查询：当前只有 fp1。
        auto current = db.currentMethodFingerprint(corpus.runId);
        ASSERT_TRUE(current.is_ok());
        expectSameFingerprint(*std::move(current).unwrap(), fpOf(1));

        // go 方法独立记账：跨方法互不干扰。
        ASSERT_TRUE(db.recordMethodVersion(corpus.classId, corpus.goId, fpOf(50)).is_ok());
        auto goHistory = db.listMethodVersions(corpus.goId);
        ASSERT_TRUE(goHistory.is_ok());
        EXPECT_EQ(1U, std::move(goHistory).unwrap().size());

        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
    }

    // 存储层锁死「同成员至多一行当前版本」：部分唯一索引在位（带 WHERE
    // 子句），绕过写入口的裸 INSERT 撞 UNIQUE 约束——插入的其余约束（FK、
    // CHECK、长度门）全部合法，能 firing 的只有当前指针唯一性。
    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);
    EXPECT_EQ(1, rawScalar(raw,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
        "AND name='method_versions_current' AND sql LIKE '%WHERE is_current=1'"));
    EXPECT_EQ(1, rawScalar(raw,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
        "AND name='method_versions_member'"));
    sqlite3_close(raw);

    sqlite3* rw = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_open_v2(path.string().c_str(), &rw,
                                         SQLITE_OPEN_READWRITE, nullptr));
    {
        sqlite3_stmt* statement = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(rw,
            "INSERT INTO method_versions(class_id, member_id, fingerprint, fp_version, "
            "is_current, recorded_at) VALUES("
            "(SELECT class_id FROM classes WHERE internal_name='demo/Alpha'), "
            "(SELECT member_id FROM members WHERE name='run' AND descriptor='()I'), "
            "x'0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20', "
            "1, 1, 'x')",
            -1, &statement, nullptr));
        EXPECT_EQ(SQLITE_CONSTRAINT, sqlite3_step(statement));
        const std::string message = sqlite3_errmsg(rw);
        EXPECT_NE(std::string::npos, message.find("UNIQUE constraint failed")) << message;
        sqlite3_finalize(statement);
    }
    sqlite3_close(rw);
}

TEST(MjAnalysisDbTest, MethodVersionBatchRecordsWholeClassAtomically)
{
    const auto path = freshDbPath("mv_batch");
    {
        auto db = openOrDie(path);
        writeMethodVersionCorpus(db);
        auto corpus = resolveMethodVersionCorpus(db);

        // 预置 go 的当前指纹：批量面里它将以同指纹出现（幂等跳过路径）。
        ASSERT_TRUE(db.recordMethodVersion(corpus.classId, corpus.goId, fpOf(20)).is_ok());

        // 整类重写场景：Alpha 两方法 + Beta 一方法一批（跨类批量合法）——
        // run 语义有变化（落新行）、go 语义未动（幂等跳过）、Beta.go 首次
        // 落版本。
        std::vector<DbMethodVersionInput> batch;
        DbMethodVersionInput runRow;
        runRow.classId     = corpus.classId;
        runRow.memberId    = corpus.runId;
        runRow.fingerprint = fpOf(11);
        batch.push_back(std::move(runRow));
        DbMethodVersionInput goRow;
        goRow.classId     = corpus.classId;
        goRow.memberId    = corpus.goId;
        goRow.fingerprint = fpOf(20);
        batch.push_back(std::move(goRow));
        DbMethodVersionInput betaRow;
        betaRow.classId     = corpus.betaClassId;
        betaRow.memberId    = corpus.betaGoId;
        betaRow.fingerprint = fpOf(21);
        batch.push_back(std::move(betaRow));

        auto applied = db.recordMethodVersions(std::move(batch));
        ASSERT_TRUE(applied.is_ok());
        auto stats = std::move(applied).unwrap();
        EXPECT_EQ(2U, stats.recorded);
        EXPECT_EQ(1U, stats.unchanged);

        auto runCurrent = db.currentMethodFingerprint(corpus.runId);
        ASSERT_TRUE(runCurrent.is_ok());
        expectSameFingerprint(*std::move(runCurrent).unwrap(), fpOf(11));
        auto goCurrent = db.currentMethodFingerprint(corpus.goId);
        ASSERT_TRUE(goCurrent.is_ok());
        expectSameFingerprint(*std::move(goCurrent).unwrap(), fpOf(20));
        auto betaCurrent = db.currentMethodFingerprint(corpus.betaGoId);
        ASSERT_TRUE(betaCurrent.is_ok());
        expectSameFingerprint(*std::move(betaCurrent).unwrap(), fpOf(21));
        auto goHistory = db.listMethodVersions(corpus.goId);
        ASSERT_TRUE(goHistory.is_ok());
        EXPECT_EQ(1U, std::move(goHistory).unwrap().size());

        // 空批为空操作。
        auto empty = db.recordMethodVersions({});
        ASSERT_TRUE(empty.is_ok());
        auto emptyStats = std::move(empty).unwrap();
        EXPECT_EQ(0U, emptyStats.recorded);
        EXPECT_EQ(0U, emptyStats.unchanged);

        // 原子性：批内夹一行未登记成员 → Invalid，整批回滚——成功路径上
        // 已落的版本行不出现半截状态（go 无新行、当前指纹不动）。
        std::vector<DbMethodVersionInput> bad;
        DbMethodVersionInput okRow;
        okRow.classId     = corpus.classId;
        okRow.memberId    = corpus.goId;
        okRow.fingerprint = fpOf(30);
        bad.push_back(std::move(okRow));
        DbMethodVersionInput ghostRow;
        ghostRow.classId     = corpus.classId;
        ghostRow.memberId    = 999999;
        ghostRow.fingerprint = fpOf(31);
        bad.push_back(std::move(ghostRow));
        auto failed = db.recordMethodVersions(std::move(bad));
        ASSERT_TRUE(failed.is_err());
        EXPECT_EQ(AnalysisDbErrorKind::Invalid, std::move(failed).unwrap_err().kind);

        auto goAfter = db.currentMethodFingerprint(corpus.goId);
        ASSERT_TRUE(goAfter.is_ok());
        expectSameFingerprint(*std::move(goAfter).unwrap(), fpOf(20));
        auto goHistoryAfter = db.listMethodVersions(corpus.goId);
        ASSERT_TRUE(goHistoryAfter.is_ok());
        EXPECT_EQ(1U, std::move(goHistoryAfter).unwrap().size());
        auto betaHistory = db.listMethodVersions(corpus.betaGoId);
        ASSERT_TRUE(betaHistory.is_ok());
        EXPECT_EQ(1U, std::move(betaHistory).unwrap().size());
    }

    // 失败批回滚后重开：库状态与失败前一致（无半截版本）。
    auto reopened = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    auto corpus   = resolveMethodVersionCorpus(reopened);
    auto runHistory = reopened.listMethodVersions(corpus.runId);
    ASSERT_TRUE(runHistory.is_ok());
    EXPECT_EQ(1U, std::move(runHistory).unwrap().size());
}

TEST(MjAnalysisDbTest, MethodVersionFingerprintBytesRoundTripByteExact)
{
    const auto path = freshDbPath("mv_bytes");
    {
        auto db = openOrDie(path);
        writeMethodVersionCorpus(db);
        auto corpus = resolveMethodVersionCorpus(db);

        // 两个对偶模式覆盖全部 256 个字节值与高位全 1。
        ASSERT_TRUE(
            db.recordMethodVersion(corpus.classId, corpus.runId, allBytesPattern()).is_ok());
        ASSERT_TRUE(
            db.recordMethodVersion(corpus.classId, corpus.goId, highOnesPattern()).is_ok());

        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());

        // 存储形状：BLOB 恰 32 字节逐位相等，fp_version 盖 kSemanticFpVersion，
        // recorded_at 非空。
        sqlite3* raw = openRawReadOnly(path);
        ASSERT_NE(nullptr, raw);
        const auto rawCurrentRow = [&](ca::i64 memberId) {
            sqlite3_stmt* statement = nullptr;
            EXPECT_EQ(SQLITE_OK, sqlite3_prepare_v2(raw,
                "SELECT fingerprint, fp_version, is_current, length(recorded_at) "
                "FROM method_versions WHERE member_id=?1 AND is_current=1",
                -1, &statement, nullptr));
            EXPECT_EQ(SQLITE_OK, sqlite3_bind_int64(statement, 1, memberId));
            EXPECT_EQ(SQLITE_ROW, sqlite3_step(statement));
            const auto* bytes = static_cast<const ca::u8*>(sqlite3_column_blob(statement, 0));
            const int   size  = sqlite3_column_bytes(statement, 0);
            std::vector<ca::u8> stored(bytes, bytes + size);
            const ca::i64 fpVersion  = sqlite3_column_int64(statement, 1);
            const ca::i64 isCurrent  = sqlite3_column_int64(statement, 2);
            const ca::i64 recordedAt = sqlite3_column_int64(statement, 3);
            sqlite3_finalize(statement);
            return std::make_tuple(stored, fpVersion, isCurrent, recordedAt);
        };
        {
            auto [stored, fpVersion, isCurrent, recordedAt] = rawCurrentRow(corpus.runId);
            EXPECT_EQ(static_cast<ca::usize>(kSemanticFpHashBytes), stored.size());
            EXPECT_EQ(0, std::memcmp(stored.data(), allBytesPattern().bytes.data(),
                                     kSemanticFpHashBytes));
            EXPECT_EQ(static_cast<ca::i64>(kSemanticFpVersion), fpVersion);
            EXPECT_EQ(1, isCurrent);
            EXPECT_GT(recordedAt, 0);
        }
        {
            auto [stored, fpVersion, isCurrent, recordedAt] = rawCurrentRow(corpus.goId);
            EXPECT_EQ(static_cast<ca::usize>(kSemanticFpHashBytes), stored.size());
            EXPECT_EQ(0, std::memcmp(stored.data(), highOnesPattern().bytes.data(),
                                     kSemanticFpHashBytes));
            EXPECT_EQ(static_cast<ca::i64>(kSemanticFpVersion), fpVersion);
            EXPECT_EQ(1, isCurrent);
            EXPECT_GT(recordedAt, 0);
        }
        sqlite3_close(raw);
    }

    // 只读重开：C++ API 逐字节 + hex 派生重建。
    auto reopened = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    auto corpus   = resolveMethodVersionCorpus(reopened);
    auto runFp    = reopened.currentMethodFingerprint(corpus.runId);
    ASSERT_TRUE(runFp.is_ok());
    auto runValue = std::move(runFp).unwrap();
    ASSERT_TRUE(runValue.has_value());
    expectSameFingerprint(*runValue, allBytesPattern());
    expectHexMatchesBytes(*runValue);
    auto goFp = reopened.currentMethodFingerprint(corpus.goId);
    ASSERT_TRUE(goFp.is_ok());
    auto goValue = std::move(goFp).unwrap();
    ASSERT_TRUE(goValue.has_value());
    expectSameFingerprint(*goValue, highOnesPattern());
    expectHexMatchesBytes(*goValue);
}

TEST(MjAnalysisDbTest, MethodVersionRecordValidationRejections)
{
    const auto path = freshDbPath("mv_reject");
    {
        auto db = openOrDie(path);
        writeMethodVersionCorpus(db);
        auto corpus = resolveMethodVersionCorpus(db);

        SemanticFp zeroFp;   // 显式全零：引擎不可能的输出（未初始化形状）

        // 非正 id。
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.recordMethodVersion(0, corpus.runId, fpOf(1))));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.recordMethodVersion(corpus.classId, -1, fpOf(1))));

        // 全零指纹。
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.recordMethodVersion(corpus.classId, corpus.runId, zeroFp)));

        // 未登记成员。
        auto ghost = db.recordMethodVersion(corpus.classId, 999999, fpOf(1));
        ASSERT_TRUE(ghost.is_err());
        EXPECT_EQ(AnalysisDbErrorKind::Invalid, std::move(ghost).unwrap_err().kind);

        // 字段成员：无指纹面。
        auto fieldRow = db.recordMethodVersion(corpus.classId, corpus.fieldId, fpOf(1));
        ASSERT_TRUE(fieldRow.is_err());
        EXPECT_NE(std::string::npos,
                  std::move(fieldRow).unwrap_err().message.to_std_string()
                      .find("not a registered method"));

        // 归属错位：runId 挂在 Beta 类名下。
        auto mismatched = db.recordMethodVersion(corpus.betaClassId, corpus.runId, fpOf(1));
        ASSERT_TRUE(mismatched.is_err());
        EXPECT_NE(std::string::npos,
                  std::move(mismatched).unwrap_err().message.to_std_string()
                      .find("does not belong to class"));

        // 读口同口径：未登记成员与字段都 Invalid，不静默返回空。
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.currentMethodFingerprint(999999)));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.currentMethodFingerprint(corpus.fieldId)));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.currentMethodFingerprint(0)));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.listMethodVersions(999999)));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.listMethodVersions(corpus.fieldId)));

        // 批量面校验失败不写库：重复成员、全零指纹。
        std::vector<DbMethodVersionInput> duplicate;
        DbMethodVersionInput first;
        first.classId     = corpus.classId;
        first.memberId    = corpus.runId;
        first.fingerprint = fpOf(1);
        duplicate.push_back(std::move(first));
        DbMethodVersionInput second;
        second.classId     = corpus.classId;
        second.memberId    = corpus.runId;
        second.fingerprint = fpOf(2);
        duplicate.push_back(std::move(second));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.recordMethodVersions(std::move(duplicate))));

        std::vector<DbMethodVersionInput> zero;
        DbMethodVersionInput zeroRow;
        zeroRow.classId     = corpus.classId;
        zeroRow.memberId    = corpus.runId;
        zeroRow.fingerprint = zeroFp;
        zero.push_back(std::move(zeroRow));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.recordMethodVersions(std::move(zero))));

        // 全部拒绝后零写入：各方法仍是合法空态。
        auto runCurrent = db.currentMethodFingerprint(corpus.runId);
        ASSERT_TRUE(runCurrent.is_ok());
        EXPECT_FALSE(std::move(runCurrent).unwrap().has_value());
        auto betaCurrent = db.currentMethodFingerprint(corpus.betaGoId);
        ASSERT_TRUE(betaCurrent.is_ok());
        EXPECT_FALSE(std::move(betaCurrent).unwrap().has_value());

        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
    }

    // 只读连接拒写（读写口在只读连接上都是 ReadOnly）。
    auto readOnly = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    auto corpus   = resolveMethodVersionCorpus(readOnly);
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly,
              takeErrorKind(readOnly.recordMethodVersion(corpus.classId, corpus.runId,
                                                         fpOf(1))));
    std::vector<DbMethodVersionInput> batch;
    DbMethodVersionInput row;
    row.classId     = corpus.classId;
    row.memberId    = corpus.runId;
    row.fingerprint = fpOf(1);
    batch.push_back(std::move(row));
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly,
              takeErrorKind(readOnly.recordMethodVersions(std::move(batch))));
    // 读口不受限：只读连接上查询可用。
    auto current = readOnly.currentMethodFingerprint(corpus.runId);
    ASSERT_TRUE(current.is_ok());
    EXPECT_FALSE(std::move(current).unwrap().has_value());
}

TEST(MjAnalysisDbTest, MethodVersionQueryPlansProbeIndexesWithoutFullScans)
{
    const auto path = freshDbPath("mv_plans");
    {
        auto db = openOrDie(path);
        writeMethodVersionCorpus(db);
        auto corpus = resolveMethodVersionCorpus(db);
        ASSERT_TRUE(db.recordMethodVersion(corpus.classId, corpus.runId, fpOf(1)).is_ok());
        ASSERT_TRUE(db.recordMethodVersion(corpus.classId, corpus.runId, fpOf(2)).is_ok());
        ASSERT_TRUE(db.recordMethodVersion(corpus.classId, corpus.runId, fpOf(3)).is_ok());
        ASSERT_TRUE(db.save().is_ok());
        ASSERT_TRUE(db.close().is_ok());
    }

    sqlite3* raw = openRawReadOnly(path);
    ASSERT_NE(nullptr, raw);

    // 两条索引在位：当前指针部分唯一索引 + 版本历史索引。
    EXPECT_EQ(2, rawScalar(raw,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name IN "
        "('method_versions_current','method_versions_member')"));

    // 当前指纹查询：部分唯一索引 (member_id) WHERE is_current=1 精确探，
    // 不裸扫。
    {
        auto details = rawQueryPlanDetails(raw,
            "SELECT fingerprint FROM method_versions WHERE member_id=1 AND is_current=1");
        EXPECT_TRUE(hasSearchOnIndex(details, "method_versions_current"));
        EXPECT_FALSE(hasBareMethodVersionsScan(details));
    }
    // 版本历史：method_versions_member (member_id, method_version_id) 索引
    // 扫描，免排序（无 TEMP B-TREE）、不裸扫。
    {
        auto details = rawQueryPlanDetails(raw,
            "SELECT method_version_id FROM method_versions WHERE member_id=1 "
            "ORDER BY method_version_id");
        EXPECT_TRUE(hasSearchOnIndex(details, "method_versions_member"));
        EXPECT_FALSE(hasBareMethodVersionsScan(details));
        for (const auto& detail : details) {
            EXPECT_EQ(std::string::npos, detail.find("TEMP B-TREE")) << detail;
        }
    }

    sqlite3_close(raw);
}

//-- 类文本搜索（searchClassesPaged）------------------------------------------

namespace {

DbEntryInput searchEntry(const std::string& path, char shaSeed)
{
    DbEntryInput entry;
    entry.path =
        Utf8String::from_data(reinterpret_cast<const ca::u8*>(path.data()), path.size());
    entry.kind    = Utf8String::from_cstr("class");
    entry.size    = 64;
    const std::string sha(64, shaSeed);
    entry.sha256  = Utf8String::from_data(reinterpret_cast<const ca::u8*>(sha.data()), sha.size());
    entry.regular = true;
    return entry;
}

DbClassInput searchClass(const char* name, ca::i64 entryId)
{
    DbClassInput input;
    input.internalName = Utf8String::from_cstr(name);
    input.superName    = Utf8String::from_cstr("java/lang/Object");
    input.access       = 0x0021;
    input.version      = 52;
    input.entryId      = entryId;
    return input;
}

///活跃条目 path → entry_id（类关联用）。
std::map<std::string, ca::i64> searchLiveEntryIds(AnalysisDb& db, ca::i64 inputId)
{
    std::map<std::string, ca::i64> ids;
    auto listed = db.listEntries(inputId);
    EXPECT_TRUE(listed.is_ok());
    for (const DbEntryInfo& row : std::move(listed).unwrap()) {
        if (!row.tombstoned) ids[row.path.to_std_string()] = row.entryId;
    }
    return ids;
}

std::vector<std::string> searchRowNames(const DbClassSearchPage& page)
{
    std::vector<std::string> names;
    names.reserve(page.rows.size());
    for (const DbClassSearchRow& row : page.rows) {
        names.push_back(row.internalName.to_std_string());
    }
    return names;
}

DbClassSearchQuery searchQuery(const char* needle, DbTextMatchKind match, bool caseSensitive)
{
    DbClassSearchQuery query;
    query.needle       = Utf8String::from_cstr(needle);
    query.match        = match;
    query.caseSensitive = caseSensitive;
    return query;
}

}   // namespace

///文本面三档 × 折叠/字节敏感、path 双面命中、作用域 AND、LIKE/GLOB 转义。
TEST(MjAnalysisDbTest, SearchClassesMatchesTextScopesAndWildcards)
{
    const auto path = freshDbPath("search_classes_match");
    auto       db   = openOrDie(path);
    auto inputId = db.ensureInput(S("lib/app.jar"), refOf(std::string(64, '0')), 4096);
    ASSERT_TRUE(inputId.is_ok());
    const ca::i64 jar = inputId.unwrap();

    std::vector<DbEntryInput> entries;
    entries.push_back(searchEntry("demo/Widget.class", 'a'));
    entries.push_back(searchEntry("demo/Boxed.class", 'b'));
    entries.push_back(searchEntry("demo/p_ct.class", 'c'));
    entries.push_back(searchEntry("demo/axb.class", 'd'));
    entries.push_back(searchEntry("demo/a%b.class", 'e'));
    ASSERT_TRUE(db.ingestEntries(0, jar, std::move(entries)).is_ok());
    const auto live = searchLiveEntryIds(db, jar);

    std::vector<DbClassInput> classes;
    classes.push_back(searchClass("demo/Widget", live.at("demo/Widget.class")));
    classes.push_back(searchClass("demo/Boxed", live.at("demo/Boxed.class")));
    classes.push_back(searchClass("demo/PctMatch", live.at("demo/p_ct.class")));
    classes.push_back(searchClass("demo/Axb", live.at("demo/axb.class")));
    classes.push_back(searchClass("demo/AB", live.at("demo/a%b.class")));
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    //contains 不敏感：折叠命中（needle 大写/混合同面）。
    {
        auto page = db.searchClassesPaged(searchQuery("WID", DbTextMatchKind::Contains, false),
                                          100, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(1U, result.total);
        ASSERT_EQ(1U, result.rows.size());
        EXPECT_EQ("demo/Widget", result.rows[0].internalName.to_std_string());
        EXPECT_EQ("demo/Widget.class", result.rows[0].entryPath.to_std_string());
    }
    //contains 字节敏感：大小写不折叠，大写 needle 落空。
    {
        auto page = db.searchClassesPaged(searchQuery("WID", DbTextMatchKind::Contains, true),
                                          100, 0);
        ASSERT_TRUE(page.is_ok());
        EXPECT_EQ(0U, std::move(page).unwrap().total);
    }
    //path 双面：needle 只在条目 path 出现（internal_name 不含 ".class"）。
    {
        auto page = db.searchClassesPaged(
            searchQuery("widget.class", DbTextMatchKind::Contains, false), 100, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(1U, result.total);
        ASSERT_EQ(1U, result.rows.size());
        EXPECT_EQ("demo/Widget", result.rows[0].internalName.to_std_string());
    }
    //prefix 不敏感：demo/ 前缀命中全部 demo 类，行序按 (path, name)——path
    //首段 'B'/'W'（大写）字节序先于小写 'a'/'p'，'%'(0x25) 先于 'x'。
    {
        auto page = db.searchClassesPaged(searchQuery("DEMO/", DbTextMatchKind::Prefix, false),
                                          100, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(5U, result.total);
        EXPECT_EQ((std::vector<std::string>{"demo/Boxed", "demo/Widget", "demo/AB",
                                            "demo/Axb", "demo/PctMatch"}),
                  searchRowNames(result));
    }
    //exact 不敏感 vs 字节敏感。
    {
        auto folded = db.searchClassesPaged(
            searchQuery("demo/boxed", DbTextMatchKind::Exact, false), 100, 0);
        ASSERT_TRUE(folded.is_ok());
        EXPECT_EQ(1U, std::move(folded).unwrap().total);
        auto byteWise = db.searchClassesPaged(
            searchQuery("demo/Boxed", DbTextMatchKind::Exact, true), 100, 0);
        ASSERT_TRUE(byteWise.is_ok());
        EXPECT_EQ(1U, std::move(byteWise).unwrap().total);
        auto wrongCase = db.searchClassesPaged(
            searchQuery("demo/boxed", DbTextMatchKind::Exact, true), 100, 0);
        ASSERT_TRUE(wrongCase.is_ok());
        EXPECT_EQ(0U, std::move(wrongCase).unwrap().total);
    }
    //LIKE 转义：'_' 不吞任意单字符（p_ct 不命中 axb 的 path 面）。
    {
        auto page = db.searchClassesPaged(searchQuery("p_ct", DbTextMatchKind::Contains, false),
                                          100, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(1U, result.total);
        ASSERT_EQ(1U, result.rows.size());
        EXPECT_EQ("demo/PctMatch", result.rows[0].internalName.to_std_string());
    }
    //LIKE 转义：'%' 不当通配（a%b 不命中 axb）。
    {
        auto page = db.searchClassesPaged(searchQuery("a%b", DbTextMatchKind::Contains, false),
                                          100, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(1U, result.total);
        ASSERT_EQ(1U, result.rows.size());
        EXPECT_EQ("demo/AB", result.rows[0].internalName.to_std_string());
    }
    //LIKE 转义：'_' 不当单字符通配（a_b 无字面命中；未转义会吸进 axb）。
    {
        auto page = db.searchClassesPaged(searchQuery("a_b", DbTextMatchKind::Contains, false),
                                          100, 0);
        ASSERT_TRUE(page.is_ok());
        EXPECT_EQ(0U, std::move(page).unwrap().total);
    }
    //作用域 owner：字节精确（AND 于文本面）。
    {
        DbClassSearchQuery query = searchQuery("id", DbTextMatchKind::Contains, false);
        query.owner = Utf8String::from_cstr("demo/Widget");
        auto page = db.searchClassesPaged(query, 100, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(1U, result.total);
        ASSERT_EQ(1U, result.rows.size());
        EXPECT_EQ("demo/Widget", result.rows[0].internalName.to_std_string());
    }
    {
        DbClassSearchQuery query = searchQuery("id", DbTextMatchKind::Contains, false);
        query.owner = Utf8String::from_cstr("demo/widget");   // 大小写不折叠（字节语义）
        auto page = db.searchClassesPaged(query, 100, 0);
        ASSERT_TRUE(page.is_ok());
        EXPECT_EQ(0U, std::move(page).unwrap().total);
    }
    //作用域 packagePrefix：字节前缀（无 '/' 边界，与工作副本扫描面同语义）。
    {
        DbClassSearchQuery query = searchQuery("class", DbTextMatchKind::Contains, false);
        query.packagePrefix = Utf8String::from_cstr("demo/B");
        auto page = db.searchClassesPaged(query, 100, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(1U, result.total);
        ASSERT_EQ(1U, result.rows.size());
        EXPECT_EQ("demo/Boxed", result.rows[0].internalName.to_std_string());
    }
    //GLOB 元字符进 needle（'[' 字面化不裂查询）。
    {
        auto page = db.searchClassesPaged(searchQuery("a[", DbTextMatchKind::Contains, true),
                                          100, 0);
        ASSERT_TRUE(page.is_ok());
        EXPECT_EQ(0U, std::move(page).unwrap().total);
    }
}

///可见域（活跃锚跨输入并集、墓碑/无锚出）、空 needle 全量、窗口语义。
TEST(MjAnalysisDbTest, SearchClassesPagesLiveAnchoredClassesDeterministically)
{
    const auto path = freshDbPath("search_classes_page");
    auto       db   = openOrDie(path);
    auto first = db.ensureInput(S("lib/app.jar"), refOf(std::string(64, '0')), 4096);
    ASSERT_TRUE(first.is_ok());
    const ca::i64 jar = first.unwrap();

    std::vector<DbEntryInput> entries;
    entries.push_back(searchEntry("demo/Widget.class", 'a'));
    entries.push_back(searchEntry("demo/Gone.class", 'b'));
    entries.push_back(searchEntry("dup/Same.class", 'c'));
    ASSERT_TRUE(db.ingestEntries(0, jar, std::move(entries)).is_ok());
    const auto firstLive = searchLiveEntryIds(db, jar);
    const ca::i64 goneEntry = firstLive.at("demo/Gone.class");

    //墓碑：同输入重导，缺 demo/Gone.class → 该条目入墓碑（类行不删）。
    std::vector<DbEntryInput> reduced;
    reduced.push_back(searchEntry("demo/Widget.class", 'a'));
    reduced.push_back(searchEntry("dup/Same.class", 'c'));
    ASSERT_TRUE(db.ingestEntries(0, jar, std::move(reduced)).is_ok());
    auto live = searchLiveEntryIds(db, jar);

    //第二输入同 path 条目：跨输入并集，同 path 双行按 internal_name 定序。
    auto second = db.ensureInput(S("lib/dup.jar"), refOf(std::string(64, '1')), 128);
    ASSERT_TRUE(second.is_ok());
    const ca::i64 dupJar = second.unwrap();
    std::vector<DbEntryInput> dupEntries;
    dupEntries.push_back(searchEntry("dup/Same.class", 'f'));
    ASSERT_TRUE(db.ingestEntries(0, dupJar, std::move(dupEntries)).is_ok());
    const auto dupLive = searchLiveEntryIds(db, dupJar);

    std::vector<DbClassInput> classes;
    classes.push_back(searchClass("demo/Widget", live.at("demo/Widget.class")));
    classes.push_back(searchClass("demo/Gone", goneEntry));   // 墓碑锚：不在可见域
    classes.push_back(searchClass("demo/NoAnchor", 0));       // 无锚类：不在可见域
    classes.push_back(searchClass("dup/Alpha", live.at("dup/Same.class")));
    classes.push_back(searchClass("dup/Beta", dupLive.at("dup/Same.class")));
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    //空 needle 全量：(path, internal_name) BINARY 确定序（"demo/…" 的
    //'e' < "dup/…" 的 'u'）；墓碑锚/无锚类不在面内（悬垂锚经 FK 约束
    //不可达）。
    {
        DbClassSearchQuery query;
        auto page = db.searchClassesPaged(query, 100, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(3U, result.total);
        EXPECT_EQ((std::vector<std::string>{"demo/Widget", "dup/Alpha", "dup/Beta"}),
                  searchRowNames(result));
        EXPECT_EQ("demo/Widget.class", result.rows[0].entryPath.to_std_string());
        EXPECT_EQ("dup/Same.class", result.rows[1].entryPath.to_std_string());
        EXPECT_EQ("dup/Same.class", result.rows[2].entryPath.to_std_string());
    }
    //窗口：offset 跳行、limit 截尾；total 恒全量。
    {
        DbClassSearchQuery query;
        auto page = db.searchClassesPaged(query, 1, 1);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(3U, result.total);
        EXPECT_EQ((std::vector<std::string>{"dup/Alpha"}), searchRowNames(result));
    }
    {
        DbClassSearchQuery query;
        auto page = db.searchClassesPaged(query, 0, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(3U, result.total);
        EXPECT_TRUE(result.rows.empty());
    }
    {
        DbClassSearchQuery query;
        auto page = db.searchClassesPaged(query, 100, 9999);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        EXPECT_EQ(3U, result.total);
        EXPECT_TRUE(result.rows.empty());
    }
    //负窗口：Invalid。
    {
        DbClassSearchQuery query;
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.searchClassesPaged(query, -1, 0)));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.searchClassesPaged(query, 10, -1)));
    }
}
