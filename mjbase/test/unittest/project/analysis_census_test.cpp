/// @file        analysis_census_test.cpp
/// @brief       普查投影层（#121 §5 B4）：censusClasses 事实面（计数、条目
///              关联、形态旗标、墓碑排除、排序确定性）、censusContainer 汇总
///              面（形态分布、分析事实计数）、空容器边界与非法输入。
#include <gtest/gtest.h>

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
    const auto path = test::getTempOutPath("analysis_census_" + name + ".mjdb");
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

std::string shaOf(char seed)
{
    return std::string(64, seed);
}

template <typename T>
AnalysisDbErrorKind takeErrorKind(ca::Result<T, AnalysisDbError>&& result)
{
    EXPECT_TRUE(result.is_err());
    if (result.is_err()) return std::move(result).unwrap_err().kind;
    return AnalysisDbErrorKind::Sqlite;
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

DbClassInput classInput(const char* name, const char* superName)
{
    DbClassInput input;
    input.internalName = Utf8String::from_cstr(name);
    input.superName    = Utf8String::from_cstr(superName);
    input.access       = 0x0021;   // public | super
    input.version      = 52;
    return input;
}

DbMemberInput memberInput(DbMemberKind kind, const char* name, const char* desc, ca::i32 access)
{
    DbMemberInput member;
    member.kind       = kind;
    member.name       = Utf8String::from_cstr(name);
    member.descriptor = Utf8String::from_cstr(desc);
    member.access     = access;
    return member;
}

/// 活跃条目 path → entry_id 索引（类关联用）。
std::map<std::string, ca::i64> liveEntryIds(AnalysisDb& db, ca::i64 inputId)
{
    std::map<std::string, ca::i64> ids;
    auto listed = db.listEntries(inputId);
    EXPECT_TRUE(listed.is_ok());
    for (const DbEntryInfo& row : std::move(listed).unwrap()) {
        if (!row.tombstoned) ids[row.path.to_std_string()] = row.entryId;
    }
    return ids;
}

const DbClassCensusRow* findRow(const std::vector<DbClassCensusRow>& rows,
                                const std::string&                   name)
{
    for (const DbClassCensusRow& row : rows) {
        if (row.internalName.to_std_string() == name) return &row;
    }
    return nullptr;
}

const DbCensusFacetCount* findFacet(const std::vector<DbCensusFacetCount>& facets,
                                    const std::string&                     name)
{
    for (const DbCensusFacetCount& facet : facets) {
        if (facet.facet.to_std_string() == name) return &facet;
    }
    return nullptr;
}

}   // namespace

TEST(MjAnalysisCensusTest, EmptyInputProjectsZeroRowsAndZeroFacets)
{
    const auto path = freshDbPath("empty");
    auto       db   = openOrDie(path);
    auto inputId = db.ensureInput(S("lib/empty.jar"), refOf(shaOf('0')), 22);
    ASSERT_TRUE(inputId.is_ok());

    auto classes = db.censusClasses(inputId.unwrap());
    ASSERT_TRUE(classes.is_ok());
    EXPECT_TRUE(std::move(classes).unwrap().empty());

    auto container = db.censusContainer(inputId.unwrap());
    ASSERT_TRUE(container.is_ok());
    const auto summary = std::move(container).unwrap();
    EXPECT_EQ(inputId.unwrap(), summary.inputId);
    EXPECT_EQ("lib/empty.jar", summary.path.to_std_string());
    EXPECT_EQ(22, summary.size);
    EXPECT_EQ(shaOf('0'), summary.sha256.to_std_string());
    EXPECT_EQ(0, summary.entriesActive);
    EXPECT_EQ(0, summary.entriesTombstoned);
    EXPECT_EQ(0, summary.entriesRegular);
    EXPECT_EQ(0, summary.entriesIrregular);
    EXPECT_TRUE(summary.entryKinds.empty());
    EXPECT_TRUE(summary.entryShapes.empty());
    EXPECT_EQ(0, summary.classes);
    EXPECT_EQ(0, summary.methods);
    EXPECT_EQ(0, summary.fields);
    EXPECT_EQ(0, summary.nativeMethods);
    EXPECT_EQ(0, summary.references);
}

TEST(MjAnalysisCensusTest, ClassRowsProjectCountsEntryFactsAndDerivedKind)
{
    const auto path = freshDbPath("classes");
    auto       db   = openOrDie(path);
    auto inputId = db.ensureInput(S("lib/app.jar"), refOf(shaOf('0')), 4096);
    ASSERT_TRUE(inputId.is_ok());
    const ca::i64 jar = inputId.unwrap();

    std::vector<DbEntryInput> entries;
    entries.push_back(entryInput("demo/Impl.class", "class", shaOf('a'), 1024));
    entries.push_back(entryInput("demo/Point.class", "class", shaOf('b'), 512));
    entries.push_back(entryInput("demo/Flag.class", "class", shaOf('c'), 256));
    entries.push_back(entryInput("demo/Api.class", "class", shaOf('d'), 128));
    entries.push_back(entryInput("demo/ops/Sum.class", "class", shaOf('e'), 3210987654,
                                 false));   // > INT32_MAX：LP64 i64 直取
    entries.push_back(entryInput("demo/bad name.class", "class", shaOf('f'), 64));
    entries.push_back(entryInput("dir/", "resource", shaOf('7'), 0, false));
    entries.push_back(entryInput("META-INF/MANIFEST.MF", "manifest", shaOf('8'), 8, false));
    auto ingested = db.ingestEntries(jar, std::move(entries));
    if (ingested.is_err()) {
        ADD_FAILURE() << "ingestEntries: "
                      << std::move(ingested).unwrap_err().message.to_std_string();
        return;
    }

    const auto ids = liveEntryIds(db, jar);
    ASSERT_EQ(8U, ids.size());

    std::vector<DbClassInput> classes;
    // 常规类：2 方法（1 native）+ 1 字段 + 2 接口。
    DbClassInput impl = classInput("demo/Impl", "demo/Base");
    impl.entryId      = ids.at("demo/Impl.class");
    impl.access       = 0x0021;
    impl.interfaces.push_back(Utf8String::from_cstr("demo/Api"));
    impl.interfaces.push_back(Utf8String::from_cstr("java/io/Serializable"));
    impl.members.push_back(memberInput(DbMemberKind::Method, "run", "()V", 0x0001));
    impl.members.push_back(memberInput(DbMemberKind::Method, "probe", "()J", 0x0100));
    impl.members.push_back(memberInput(DbMemberKind::Field, "state", "I", 0x0002));
    classes.push_back(std::move(impl));
    // record 判据走父类（super = java/lang/Record，无伪位）。
    DbClassInput point = classInput("demo/Point", "java/lang/Record");
    point.entryId      = ids.at("demo/Point.class");
    classes.push_back(std::move(point));
    // enum 形态位。
    DbClassInput flag = classInput("demo/Flag", "java/lang/Enum");
    flag.entryId      = ids.at("demo/Flag.class");
    flag.access       = 0x4011;   // enum | final | public
    classes.push_back(std::move(flag));
    // 接口形态位。
    DbClassInput api = classInput("demo/Api", "");
    api.entryId      = ids.at("demo/Api.class");
    api.access       = 0x0601;   // interface | abstract | public
    classes.push_back(std::move(api));
    // 非常规条目上的类（伪装路径形态）+ annotation 形态。
    DbClassInput sum = classInput("demo/ops/Sum", "");
    sum.entryId      = ids.at("demo/ops/Sum.class");
    sum.access       = 0x2601;   // annotation | interface | abstract | public
    classes.push_back(std::move(sum));
    DbClassInput bad = classInput("demo/bad name", "");
    bad.entryId      = ids.at("demo/bad name.class");
    classes.push_back(std::move(bad));
    ASSERT_TRUE(db.ingestClasses(std::move(classes)).is_ok());

    auto projected = db.censusClasses(jar);
    ASSERT_TRUE(projected.is_ok());
    auto rows = std::move(projected).unwrap();
    ASSERT_EQ(6U, rows.size());

    // 行序 = internal_name BINARY 升序（'A' 0x41 < 'b' 0x62 < 'o' 0x6F：
    // 大写在前，demo/bad name 先于 demo/ops/Sum）。
    ASSERT_EQ("demo/Api", rows[0].internalName.to_std_string());
    ASSERT_EQ("demo/Flag", rows[1].internalName.to_std_string());
    ASSERT_EQ("demo/Impl", rows[2].internalName.to_std_string());
    ASSERT_EQ("demo/Point", rows[3].internalName.to_std_string());
    ASSERT_EQ("demo/bad name", rows[4].internalName.to_std_string());
    ASSERT_EQ("demo/ops/Sum", rows[5].internalName.to_std_string());

    const DbClassCensusRow* implRow = findRow(rows, "demo/Impl");
    ASSERT_NE(implRow, nullptr);
    EXPECT_EQ(DbClassKind::Class, implRow->kind);
    EXPECT_EQ(0x0021, implRow->access);
    EXPECT_EQ("demo/Base", implRow->superName.to_std_string());
    EXPECT_EQ(2, implRow->ifaceCount);
    EXPECT_EQ(2, implRow->methodCount);
    EXPECT_EQ(1, implRow->fieldCount);
    EXPECT_EQ(1, implRow->nativeMethodCount);
    EXPECT_EQ(ids.at("demo/Impl.class"), implRow->entryId);
    EXPECT_EQ(1024, implRow->byteSize);
    EXPECT_TRUE(implRow->entryRegular);

    const DbClassCensusRow* pointRow = findRow(rows, "demo/Point");
    ASSERT_NE(pointRow, nullptr);
    EXPECT_EQ(DbClassKind::Record, pointRow->kind);
    const DbClassCensusRow* flagRow = findRow(rows, "demo/Flag");
    ASSERT_NE(flagRow, nullptr);
    EXPECT_EQ(DbClassKind::Enum, flagRow->kind);
    const DbClassCensusRow* apiRow = findRow(rows, "demo/Api");
    ASSERT_NE(apiRow, nullptr);
    EXPECT_EQ(DbClassKind::Interface, apiRow->kind);
    const DbClassCensusRow* sumRow = findRow(rows, "demo/ops/Sum");
    ASSERT_NE(sumRow, nullptr);
    EXPECT_EQ(DbClassKind::Annotation, sumRow->kind);
    EXPECT_EQ(3210987654LL, sumRow->byteSize);   // LP64：i64 直取不截断
    EXPECT_FALSE(sumRow->entryRegular);
    const DbClassCensusRow* badRow = findRow(rows, "demo/bad name");
    ASSERT_NE(badRow, nullptr);
    EXPECT_TRUE(badRow->entryRegular);   // 路径形态判定与 kind 无关
    EXPECT_EQ(0, badRow->methodCount);   // 无成员登记 → 计数为 0
}

TEST(MjAnalysisCensusTest, ClassRowsProjectLibraryOriginByPrefix)
{
    // origin 是投影层派生（前缀分类，#132 L1）：Jdk/Known 按内部名前缀命中，
    // Unknown 兜底；与条目形态、类形态派生正交。普查面只含关联到本输入活跃
    // 条目的类，先登记条目并挂 entryId 再断言。
    const auto path = freshDbPath("origin");
    auto       db   = openOrDie(path);
    auto inputId = db.ensureInput(S("lib/mix.jar"), refOf(shaOf('0')), 4096);
    ASSERT_TRUE(inputId.is_ok());
    const ca::i64 jar = inputId.unwrap();

    std::vector<DbEntryInput> entries;
    entries.push_back(entryInput("java/test/Sample.class", "class", shaOf('1'), 128));
    entries.push_back(entryInput("com/google/gson/Stub.class", "class", shaOf('2'), 256));
    entries.push_back(entryInput("app/Main.class", "class", shaOf('3'), 512));
    auto ingested = db.ingestEntries(jar, std::move(entries));
    ASSERT_TRUE(ingested.is_ok());
    const auto ids = liveEntryIds(db, jar);
    ASSERT_EQ(3U, ids.size());

    std::vector<DbClassInput> classes;
    DbClassInput jdkClass = classInput("java/test/Sample", "java/lang/Object");
    jdkClass.entryId      = ids.at("java/test/Sample.class");
    classes.push_back(std::move(jdkClass));
    DbClassInput knownClass = classInput("com/google/gson/Stub", "");
    knownClass.entryId      = ids.at("com/google/gson/Stub.class");
    classes.push_back(std::move(knownClass));
    DbClassInput unknownClass = classInput("app/Main", "");
    unknownClass.entryId      = ids.at("app/Main.class");
    classes.push_back(std::move(unknownClass));
    ASSERT_TRUE(db.ingestClasses(std::move(classes)).is_ok());

    auto projected = db.censusClasses(jar);
    ASSERT_TRUE(projected.is_ok());
    auto rows = std::move(projected).unwrap();
    ASSERT_EQ(3U, rows.size());

    const DbClassCensusRow* jdk = findRow(rows, "java/test/Sample");
    ASSERT_NE(jdk, nullptr);
    EXPECT_EQ(LibraryOrigin::Jdk, jdk->origin);
    const DbClassCensusRow* known = findRow(rows, "com/google/gson/Stub");
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(LibraryOrigin::Known, known->origin);
    const DbClassCensusRow* unknown = findRow(rows, "app/Main");
    ASSERT_NE(unknown, nullptr);
    EXPECT_EQ(LibraryOrigin::Unknown, unknown->origin);
}

TEST(MjAnalysisCensusTest, TombstonedEntriesDropTheirClassesFromCensus)
{
    const auto path = freshDbPath("tombstone");
    auto       db   = openOrDie(path);
    auto inputId = db.ensureInput(S("lib/app.jar"), refOf(shaOf('0')), 1024);
    ASSERT_TRUE(inputId.is_ok());
    const ca::i64 jar = inputId.unwrap();

    std::vector<DbEntryInput> first;
    first.push_back(entryInput("a/Keep.class", "class", shaOf('a')));
    first.push_back(entryInput("b/Gone.class", "class", shaOf('b')));
    ASSERT_TRUE(db.ingestEntries(jar, std::move(first)).is_ok());
    const auto ids = liveEntryIds(db, jar);

    std::vector<DbClassInput> classes;
    DbClassInput keep = classInput("a/Keep", "java/lang/Object");
    keep.entryId      = ids.at("a/Keep.class");
    classes.push_back(std::move(keep));
    DbClassInput gone = classInput("b/Gone", "java/lang/Object");
    gone.entryId      = ids.at("b/Gone.class");
    classes.push_back(std::move(gone));
    ASSERT_TRUE(db.ingestClasses(std::move(classes)).is_ok());

    // 重导入只剩 a/Keep：b/Gone 条目进墓碑，其类退出普查面（类行不删）。
    std::vector<DbEntryInput> shrunk;
    shrunk.push_back(entryInput("a/Keep.class", "class", shaOf('a')));
    ASSERT_TRUE(db.ingestEntries(jar, std::move(shrunk)).is_ok());

    auto projected = db.censusClasses(jar);
    ASSERT_TRUE(projected.is_ok());
    auto rows = std::move(projected).unwrap();
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ("a/Keep", rows[0].internalName.to_std_string());
    // 类身份仍在库中（listClasses 不受普查口径影响）。
    auto all = db.listClasses();
    ASSERT_TRUE(all.is_ok());
    EXPECT_EQ(2U, std::move(all).unwrap().size());

    auto container = db.censusContainer(jar);
    ASSERT_TRUE(container.is_ok());
    EXPECT_EQ(1, std::move(container).unwrap().entriesTombstoned);
}

TEST(MjAnalysisCensusTest, ContainerSummaryAggregatesFacetsAndFacts)
{
    const auto path = freshDbPath("summary");
    auto       db   = openOrDie(path);
    auto inputId = db.ensureInput(S("lib/mixed.jar"), refOf(shaOf('0')), 8192);
    ASSERT_TRUE(inputId.is_ok());
    const ca::i64 jar = inputId.unwrap();

    // 登记旗标与路径形态分类器一致（导入桥的取法）：旗标面事实与 facet
    // 投影在此口径下同向，summary 两轴可对账。
    std::vector<DbEntryInput> entries;
    entries.push_back(entryInput("a/One.class", "class", shaOf('a'), 100));
    entries.push_back(entryInput("b/Two.class", "class", shaOf('b'), 200));
    entries.push_back(entryInput("c/Three.class", "class", shaOf('c'), 300));
    entries.push_back(entryInput("pkg/", "resource", shaOf('d'), 0, false));
    entries.push_back(entryInput("res/data.bin", "resource", shaOf('e'), 24));
    entries.push_back(entryInput("META-INF/MANIFEST.MF", "manifest", shaOf('f'), 8));
    entries.push_back(entryInput("weird//name.txt", "resource", shaOf('7'), 4, false));
    ASSERT_TRUE(db.ingestEntries(jar, std::move(entries)).is_ok());
    const auto ids = liveEntryIds(db, jar);

    std::vector<DbClassInput> classes;
    for (const char* name : {"a/One", "b/Two", "c/Three"}) {
        DbClassInput input = classInput(name, "java/lang/Object");
        const std::string pathName = std::string(name) + ".class";
        input.entryId              = ids.at(pathName);
        input.members.push_back(memberInput(DbMemberKind::Method, "act", "()V", 0x0001));
        if (std::string(name) != "c/Three") {
            input.members.push_back(
                memberInput(DbMemberKind::Method, "probe", "()J", 0x0100));
        }
        classes.push_back(std::move(input));
    }
    ASSERT_TRUE(db.ingestClasses(std::move(classes)).is_ok());

    std::vector<DbReferenceRecord> references;
    DbReferenceRecord typeRef;
    typeRef.sourceClass = Utf8String::from_cstr("a/One");
    typeRef.kind        = DbReferenceKind::Type;
    typeRef.targetClass = Utf8String::from_cstr("java/lang/Object");
    references.push_back(std::move(typeRef));
    DbReferenceRecord methodRef;
    methodRef.sourceClass      = Utf8String::from_cstr("a/One");
    methodRef.sourceMethod     = Utf8String::from_cstr("act");
    methodRef.sourceDescriptor = Utf8String::from_cstr("()V");
    methodRef.insnIndex        = 0;
    methodRef.kind             = DbReferenceKind::Method;
    methodRef.targetClass      = Utf8String::from_cstr("b/Two");
    methodRef.targetName       = Utf8String::from_cstr("act");
    methodRef.targetDescriptor = Utf8String::from_cstr("()V");
    references.push_back(std::move(methodRef));
    auto replaced = db.replaceReferences(std::move(references));
    ASSERT_TRUE(replaced.is_ok());
    EXPECT_EQ(2U, replaced.unwrap());

    auto summaryResult = db.censusContainer(jar);
    ASSERT_TRUE(summaryResult.is_ok());
    const auto summary = std::move(summaryResult).unwrap();

    EXPECT_EQ(7, summary.entriesActive);
    EXPECT_EQ(0, summary.entriesTombstoned);
    EXPECT_EQ(5, summary.entriesRegular);
    EXPECT_EQ(2, summary.entriesIrregular);

    // kind 分布按存储值名称升序；只含非零项。
    ASSERT_EQ(3U, summary.entryKinds.size());
    EXPECT_EQ("class", summary.entryKinds[0].facet.to_std_string());
    EXPECT_EQ(3, summary.entryKinds[0].count);
    EXPECT_EQ("manifest", summary.entryKinds[1].facet.to_std_string());
    EXPECT_EQ(1, summary.entryKinds[1].count);
    EXPECT_EQ("resource", summary.entryKinds[2].facet.to_std_string());
    EXPECT_EQ(3, summary.entryKinds[2].count);

    // 形态 facet 分布按枚举声明序；只含非零项。旗标与分类器同向时
    // regular facet 数 == entriesRegular。
    ASSERT_EQ(3U, summary.entryShapes.size());
    EXPECT_EQ("regular", summary.entryShapes[0].facet.to_std_string());
    EXPECT_EQ(5, summary.entryShapes[0].count);
    EXPECT_EQ("empty_segment", summary.entryShapes[1].facet.to_std_string());
    EXPECT_EQ(1, summary.entryShapes[1].count);
    EXPECT_EQ("trailing_slash", summary.entryShapes[2].facet.to_std_string());
    EXPECT_EQ(1, summary.entryShapes[2].count);

    EXPECT_EQ(3, summary.classes);
    EXPECT_EQ(5, summary.methods);
    EXPECT_EQ(0, summary.fields);
    EXPECT_EQ(2, summary.nativeMethods);
    EXPECT_EQ(2, summary.references);
}

TEST(MjAnalysisCensusTest, RejectedOnUnknownInputClosedDbAndReadOnlySurvives)
{
    const auto path = freshDbPath("gates");
    ca::i64    registered = 0;
    {
        auto db = openOrDie(path);
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.censusClasses(1234)));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid,
                  takeErrorKind(db.censusContainer(1234)));
        EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.censusClasses(0)));
        auto inputId = db.ensureInput(S("ro/app.jar"), refOf(shaOf('0')), 8);
        ASSERT_TRUE(inputId.is_ok());
        registered = inputId.unwrap();
        ASSERT_TRUE(db.close().is_ok());
        // 关闭后拒绝（错误类别与 listEntries 同门）。
        EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.censusClasses(registered)));
    }
    // 只读连接可用（census 是读路径）。
    auto readonly = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    auto classes  = readonly.censusClasses(registered);
    ASSERT_TRUE(classes.is_ok());
    EXPECT_TRUE(std::move(classes).unwrap().empty());
    auto summary = readonly.censusContainer(registered);
    ASSERT_TRUE(summary.is_ok());
    EXPECT_EQ("ro/app.jar", std::move(summary).unwrap().path.to_std_string());
}
