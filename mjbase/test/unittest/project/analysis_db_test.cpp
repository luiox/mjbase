/// @file        analysis_db_test.cpp
/// @brief       AnalysisDb 基础层：建库/重开、稳定身份上的重命名覆盖、
///              token 约束、只读拒写、原子替换与非法输入零写入；
///              v3 条目身份层的输入/条目登记、重导入匹配与墓碑语义。
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
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
    ASSERT_TRUE(db.ingestClasses(std::move(inputs)).is_ok());

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
    ASSERT_TRUE(db.replaceReferences(std::move(references)).is_ok());

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
    auto replaced = db.replaceReferencesFromMember(callId, std::move(slice));
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
    auto cleared = db.replaceReferencesFromMember(callId, std::move(empty));
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
    ASSERT_TRUE(db.ingestClasses(std::move(inputs)).is_ok());

    std::vector<DbReferenceRecord> references;
    references.push_back(referenceRecord("demo/Caller", "call", "()V", 3,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    ASSERT_TRUE(db.replaceReferences(std::move(references)).is_ok());

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
              takeErrorKind(db.replaceReferencesFromMember(424242, std::move(ghost))));
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
              takeErrorKind(db.replaceReferencesFromMember(fieldRow->memberId,
                                                           std::move(viaField))));

    // 行内 source 与参数成员不一致：切片身份错位，Invalid 且 call()V 切片原样。
    std::vector<DbReferenceRecord> mismatched;
    mismatched.push_back(referenceRecord("demo/Caller", "keep", "()J", 9,
                                         DbReferenceKind::Method, "demo/Base", "run", "()V"));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.replaceReferencesFromMember(callId, std::move(mismatched))));

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
              takeErrorKind(db.replaceReferencesFromMember(callId, std::move(malformed))));
    count = db.referenceCount();
    ASSERT_TRUE(count.is_ok());
    EXPECT_EQ(1U, count.unwrap());
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
    auto firstStats = db.ingestEntries(jar, std::move(first));
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
    auto sameStats = db.ingestEntries(jar, std::move(same));
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
    auto shrunkStats = db.ingestEntries(jar, std::move(shrunk));
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
    auto revivedStats = db.ingestEntries(jar, std::move(revived));
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
    auto mutatedStats = db.ingestEntries(jar, std::move(mutated));
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
    auto flipBack = db.ingestEntries(jar, std::move(flippedBack));
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
    ASSERT_TRUE(db.ingestEntries(jar, std::move(valid)).is_ok());

    std::vector<DbEntryInput> duplicate;
    duplicate.push_back(entryInput("x/Dup", "resource", shaOf('b')));
    duplicate.push_back(entryInput("x/Dup", "resource", shaOf('c')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(jar, std::move(duplicate))));

    std::vector<DbEntryInput> missingSha;
    missingSha.push_back(entryInput("x/NoSha", "resource", std::string(63, 'a')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(jar, std::move(missingSha))));

    std::vector<DbEntryInput> upperSha;
    upperSha.push_back(entryInput("x/Upper", "resource", std::string(64, 'A')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(jar, std::move(upperSha))));

    std::vector<DbEntryInput> emptyKind;
    emptyKind.push_back(entryInput("x/NoKind", "", shaOf('d')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(jar, std::move(emptyKind))));

    std::vector<DbEntryInput> emptyPath;
    emptyPath.push_back(entryInput("", "resource", shaOf('e')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(jar, std::move(emptyPath))));

    std::vector<DbEntryInput> negativeSize;
    negativeSize.push_back(entryInput("x/Neg", "resource", shaOf('f'), -1));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(jar, std::move(negativeSize))));

    std::vector<DbEntryInput> unknownInput;
    unknownInput.push_back(entryInput("a/A.class", "class", shaOf('a')));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.ingestEntries(jar + 999, std::move(unknownInput))));

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
    auto stats = db.ingestEntries(jar, std::move(entries));
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
    ASSERT_TRUE(db.ingestClasses(std::move(classes)).is_ok());

    auto found = db.findClass(S("demo/Base"));
    ASSERT_TRUE(found.is_ok());
    auto base = std::move(found).unwrap();
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(entryId, base->entryId);

    // 条目内容更新后重登记类：链接指向新条目。
    std::vector<DbEntryInput> mutated;
    mutated.push_back(entryInput("demo/Base.class", "class", shaOf('b')));
    ASSERT_TRUE(db.ingestEntries(jar, std::move(mutated)).is_ok());
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
    ASSERT_TRUE(db.ingestClasses(std::move(relinked)).is_ok());
    found = db.findClass(S("demo/Base"));
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(newEntryId, std::move(found).unwrap()->entryId);

    // entryId=0 = 未指定：既有链接保持不动。
    std::vector<DbClassInput> unspecified;
    unspecified.push_back(classInput("demo/Base", "java/lang/Other"));
    ASSERT_TRUE(db.ingestClasses(std::move(unspecified)).is_ok());
    found = db.findClass(S("demo/Base"));
    ASSERT_TRUE(found.is_ok());
    auto baseAgain = std::move(found).unwrap();
    EXPECT_EQ(newEntryId, baseAgain->entryId);
    EXPECT_EQ("java/lang/Other", baseAgain->superName.to_std_string());

    // 不存在的条目 id：FK 校验整批拒绝，类事实不变。
    std::vector<DbClassInput> dangling;
    dangling.push_back(classInput("demo/Base", "java/lang/Object"));
    dangling.back().entryId = newEntryId + 42;
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.ingestClasses(std::move(dangling))));
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
