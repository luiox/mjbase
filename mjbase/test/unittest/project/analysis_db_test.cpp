/// @file        analysis_db_test.cpp
/// @brief       AnalysisDb 基础层：建库/重开、稳定身份上的重命名覆盖、
///              token 约束、只读拒写、原子替换与非法输入零写入。
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
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
    auto ingested = db.ingestClasses(std::move(inputs));
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
    ASSERT_TRUE(db.ingestClasses(std::move(first)).is_ok());

    DbClassInput duplicate = classInput("demo/Base", "java/lang/Object");
    duplicate.access       = 0x0010;
    std::vector<DbClassInput> second;
    second.push_back(classInput("demo/Other", "java/lang/Object"));
    second.push_back(std::move(duplicate));
    second.push_back(classInput("demo/Other", "java/lang/Object"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.ingestClasses(std::move(second))));

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
    ASSERT_TRUE(db.ingestClasses(std::move(inputs)).is_ok());

    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 3,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 4,
                                         DbReferenceKind::Field, "demo/Base", "counter", "I"));
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 5,
                                         DbReferenceKind::Type, "demo/Base", "", ""));
    references.push_back(referenceRecord("demo/Child", "", "", -1, DbReferenceKind::Type,
                                         "demo/Base", "", ""));

    auto replaced = db.replaceReferences(std::move(references));
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
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.replaceReferences(std::move(bad))));

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
    ASSERT_TRUE(db.ingestClasses(std::move(inputs)).is_ok());

    std::vector<DbReferenceRecord> classLevelMethod;
    classLevelMethod.push_back(referenceRecord("demo/Base", "", "", -1,
                                               DbReferenceKind::Method, "demo/Base", "run", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferences(std::move(classLevelMethod))));

    std::vector<DbReferenceRecord> methodWithoutSite;
    methodWithoutSite.push_back(referenceRecord("demo/Base", "run", "()V", -1,
                                                DbReferenceKind::Method, "demo/Base", "x", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferences(std::move(methodWithoutSite))));

    std::vector<DbReferenceRecord> typeWithName;
    typeWithName.push_back(referenceRecord("demo/Base", "", "", -1, DbReferenceKind::Type,
                                           "demo/Base", "x", ""));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferences(std::move(typeWithName))));
}
