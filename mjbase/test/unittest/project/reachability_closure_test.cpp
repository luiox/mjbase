/// @file        reachability_closure_test.cpp
/// @brief       全程序可达闭包（CHA，schema v9，#159）：闭包 ⊇ 直接边闭包与
///              虚分派展开差集、clinit 触发链（id/文本分支）、default 冲突
///              保守并集、种子三档、指纹 + 档位复用与失效往返、producer 批次
///              归属与类域作用域对账、确定性双算、分页与成员判定契约。
///
/// 语料为运行时生成的手算小语料（AnalysisDb 直写类事实与引用事实，不经字节
/// 码扫描）——闭包计算面只吃库内事实，这正是被测契约本身。
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "mjbase/project/analysis_db.hpp"
#include "mjbase/project/reachability_closure.hpp"
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
    const auto path = test::getTempOutPath("reach_closure_" + name + ".mjdb");
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

DbClassInput methodClass(const char* name, const char* superName,
                         std::vector<DbMemberInput> members,
                         std::vector<const char*>   interfaces = {},
                         ca::i32                    access     = 0)
{
    DbClassInput input;
    input.internalName = Utf8String::from_cstr(name);
    input.superName    = Utf8String::from_cstr(superName);
    input.access       = access;
    for (const char* iface : interfaces) input.interfaces.push_back(Utf8String::from_cstr(iface));
    input.members = std::move(members);
    return input;
}

DbMemberInput method(const char* name, const char* desc, ca::i32 access)
{
    DbMemberInput input;
    input.kind       = DbMemberKind::Method;
    input.name       = Utf8String::from_cstr(name);
    input.descriptor = Utf8String::from_cstr(desc);
    input.access     = access;
    return input;
}

DbMemberInput fieldInput(const char* name, const char* desc, ca::i32 access)
{
    DbMemberInput input;
    input.kind       = DbMemberKind::Field;
    input.name       = Utf8String::from_cstr(name);
    input.descriptor = Utf8String::from_cstr(desc);
    input.access     = access;
    return input;
}

DbReferenceRecord methodRef(const char* sourceClass, const char* sourceMethod,
                            const char* sourceDesc, ca::i64 insn, const char* targetClass,
                            const char* targetName, const char* targetDesc)
{
    DbReferenceRecord record;
    record.sourceClass      = Utf8String::from_cstr(sourceClass);
    record.sourceMethod     = Utf8String::from_cstr(sourceMethod);
    record.sourceDescriptor = Utf8String::from_cstr(sourceDesc);
    record.insnIndex        = insn;
    record.kind             = DbReferenceKind::Method;
    record.targetClass      = Utf8String::from_cstr(targetClass);
    record.targetName       = Utf8String::from_cstr(targetName);
    record.targetDescriptor = Utf8String::from_cstr(targetDesc);
    return record;
}

DbReferenceRecord fieldRef(const char* sourceClass, const char* sourceMethod,
                           const char* sourceDesc, ca::i64 insn, const char* targetClass,
                           const char* targetName, const char* targetDesc)
{
    DbReferenceRecord record;
    record.sourceClass      = Utf8String::from_cstr(sourceClass);
    record.sourceMethod     = Utf8String::from_cstr(sourceMethod);
    record.sourceDescriptor = Utf8String::from_cstr(sourceDesc);
    record.insnIndex        = insn;
    record.kind             = DbReferenceKind::Field;
    record.targetClass      = Utf8String::from_cstr(targetClass);
    record.targetName       = Utf8String::from_cstr(targetName);
    record.targetDescriptor = Utf8String::from_cstr(targetDesc);
    return record;
}

/// @brief 已登记方法身份（测试语料全部登记，未命中即测试语料错误）。
ca::i64 methodId(AnalysisDb& db, const char* owner, const char* name, const char* desc)
{
    auto found = db.findMember(DbMemberKind::Method, S(owner), S(name), S(desc));
    EXPECT_TRUE(found.is_ok());
    if (!found.is_ok()) return 0;
    auto info = std::move(found).unwrap();
    EXPECT_TRUE(info.has_value()) << owner << "." << name << desc;
    return info.has_value() ? info->memberId : 0;
}

ca::i64 registerReachProducer(AnalysisDb& db)
{
    DbProducerInput input;
    input.key        = Utf8String::from_cstr("mjbase.reachability.cha");
    input.name       = Utf8String::from_cstr("reachability closure (CHA)");
    input.trustLevel = DbProducerTrustLevel::Trusted;
    auto registered  = db.registerProducer(input);
    EXPECT_TRUE(registered.is_ok());
    return registered.is_ok() ? registered.unwrap() : 0;
}

ca::usize closureBatchCount(AnalysisDb& db)
{
    auto rows = db.listClosureBatches();
    EXPECT_TRUE(rows.is_ok());
    return rows.is_ok() ? std::move(rows).unwrap().size() : 0;
}

std::vector<ca::i64> closureMembers(AnalysisDb& db, ca::i64 batchId)
{
    std::vector<ca::i64> members;
    ca::i64              offset = 0;
    const ca::i64        page   = 64;
    while (true) {
        auto rows = db.closureMembersPaged(batchId, page, offset);
        EXPECT_TRUE(rows.is_ok());
        if (rows.is_err()) break;
        auto chunk = std::move(rows).unwrap();
        if (chunk.empty()) break;
        members.insert(members.end(), chunk.begin(), chunk.end());
        offset += static_cast<ca::i64>(chunk.size());
    }
    return members;
}

/// @brief 虚分派展开差集语料：Caller.call 虚调 Base.virtualTarget（Sub 覆写）
///        + 直调 static Base.directOnly；CHA 闭包须 ⊇ 直接边闭包且差集恰为
///        Sub.virtualTarget。
AnalysisDb buildVirtualCorpus(const std::filesystem::path& path)
{
    auto db = openOrDie(path);
    std::vector<DbClassInput> classes;
    {
        std::vector<DbMemberInput> baseMembers;
        baseMembers.push_back(fieldInput("tag", "I", 0x0008));
        baseMembers.push_back(method("virtualTarget", "()V", 0x0001));
        baseMembers.push_back(method("directOnly", "()V", 0x0009));   // public static
        classes.push_back(methodClass("a/Base", "", std::move(baseMembers)));
    }
    {
        std::vector<DbMemberInput> subMembers;
        subMembers.push_back(method("virtualTarget", "()V", 0x0001));
        classes.push_back(methodClass("a/Sub", "a/Base", std::move(subMembers), {"a/Itf"}));
    }
    {
        std::vector<DbMemberInput> itfMembers;
        itfMembers.push_back(method("virtualTarget", "()V", 0x0401)); // public abstract
        classes.push_back(methodClass("a/Itf", "", std::move(itfMembers), {}, 0x0200));
    }
    {
        std::vector<DbMemberInput> callerMembers;
        callerMembers.push_back(method("call", "()V", 0x0001));
        classes.push_back(methodClass("a/Caller", "", std::move(callerMembers)));
    }
    EXPECT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    std::vector<DbReferenceRecord> refs;
    refs.push_back(methodRef("a/Caller", "call", "()V", 0, "a/Base", "virtualTarget", "()V"));
    refs.push_back(methodRef("a/Caller", "call", "()V", 1, "a/Base", "directOnly", "()V"));
    auto written = db.replaceReferences(0, std::move(refs));
    EXPECT_TRUE(written.is_ok());
    if (written.is_ok()) EXPECT_EQ(2u, std::move(written).unwrap());
    return db;
}

}   // namespace

TEST(MjReachabilityClosureTest, RejectsEmptySeedsAndUnknownExplicitSeed)
{
    const auto path = freshDbPath("seeds_invalid");
    auto       db   = buildVirtualCorpus(path);
    const auto producer = registerReachProducer(db);

    ReachabilityClosureOptions empty;
    auto rejected = computeAndStoreReachabilityClosure(db, producer, empty);
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, std::move(rejected).unwrap_err().kind);

    ReachabilityClosureOptions unknownSeed;
    unknownSeed.explicitSeeds.push_back(424242);
    rejected = computeAndStoreReachabilityClosure(db, producer, unknownSeed);
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, std::move(rejected).unwrap_err().kind);

    // 字段不是闭包域成员：显式种子给字段 id 同样拒绝。
    auto fields = db.findMember(DbMemberKind::Field, S("a/Base"), S("tag"), S("I"));
    ASSERT_TRUE(fields.is_ok());
    auto fieldRow = std::move(fields).unwrap();
    ASSERT_TRUE(fieldRow.has_value());
    ReachabilityClosureOptions fieldSeed;
    fieldSeed.explicitSeeds.push_back(fieldRow->memberId);
    rejected = computeAndStoreReachabilityClosure(db, producer, fieldSeed);
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, std::move(rejected).unwrap_err().kind);

    // 校验失败不得产生批次行（全空快败与种子校验先于开批）。
    EXPECT_EQ(0u, closureBatchCount(db));
}

TEST(MjReachabilityClosureTest, RejectsReadOnlyDatabase)
{
    const auto buildPath = freshDbPath("readonly_build");
    {
        auto writer = buildVirtualCorpus(buildPath);
        ASSERT_GT(registerReachProducer(writer), 0);
        ASSERT_TRUE(writer.save().is_ok());
        ASSERT_TRUE(writer.close().is_ok());
    }
    auto db = openOrDie(buildPath, AnalysisDbOpenMode::ReadOnly);

    ReachabilityClosureOptions options;
    options.explicitSeeds.push_back(methodId(db, "a/Caller", "call", "()V"));
    auto rejected = computeAndStoreReachabilityClosure(db, 1, options);
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(AnalysisDbErrorKind::ReadOnly, std::move(rejected).unwrap_err().kind);
}

TEST(MjReachabilityClosureTest, VirtualFamilyExpansionIsSupersetOfDirectClosure)
{
    const auto path = freshDbPath("virtual_superset");
    auto       db   = buildVirtualCorpus(path);
    const auto producer = registerReachProducer(db);

    const ca::i64 call     = methodId(db, "a/Caller", "call", "()V");
    const ca::i64 baseVirt = methodId(db, "a/Base", "virtualTarget", "()V");
    const ca::i64 subVirt  = methodId(db, "a/Sub", "virtualTarget", "()V");
    const ca::i64 direct   = methodId(db, "a/Base", "directOnly", "()V");

    ReachabilityClosureOptions options;
    options.explicitSeeds.push_back(call);
    auto result = computeAndStoreReachabilityClosure(db, producer, options);
    ASSERT_TRUE(result.is_ok());
    auto stats = std::move(result).unwrap();
    EXPECT_FALSE(stats.reused);

    const auto members = closureMembers(db, stats.batchId);
    // CHA 闭包 ⊇ 直接边传递闭包（call → Base.virtualTarget / Base.directOnly）。
    EXPECT_NE(std::find(members.begin(), members.end(), call), members.end());
    EXPECT_NE(std::find(members.begin(), members.end(), baseVirt), members.end());
    EXPECT_NE(std::find(members.begin(), members.end(), direct), members.end());
    // 差集非空且恰为覆写声明（Sub.virtualTarget；接口 abstract 声明不入族）。
    EXPECT_NE(std::find(members.begin(), members.end(), subVirt), members.end());
    EXPECT_EQ(4u, members.size());

    // 种子档位与覆盖度元数据：显式清单位 + refs direct-only（恒置位）。
    auto batches = db.listClosureBatches();
    ASSERT_TRUE(batches.is_ok());
    auto rows = std::move(batches).unwrap();
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(static_cast<ca::u32>(ClosureSeedMask::Explicit), rows[0].seedMask);
    EXPECT_EQ(static_cast<ca::u32>(ClosureCoverageMask::RefsDirectOnly), rows[0].coverageMask);
    EXPECT_EQ(0, rows[0].indyEdgeCount);
    EXPECT_EQ(4, rows[0].memberCount);
    EXPECT_EQ(1u, rows[0].fpVersion);
    EXPECT_EQ(static_cast<ca::u32>(ClosureSeedMask::Explicit), stats.seedMask);
}

TEST(MjReachabilityClosureTest, ClinitTriggersWalkSuperChainAndTextBranch)
{
    const auto path = freshDbPath("clinit_chain");
    auto       db   = openOrDie(path);
    std::vector<DbClassInput> classes;
    {
        std::vector<DbMemberInput> dMembers;
        dMembers.push_back(method("<clinit>", "()V", 0x0008));
        dMembers.push_back(fieldInput("f", "I", 0x0008));
        classes.push_back(methodClass("c/D", "", std::move(dMembers)));
    }
    {
        std::vector<DbMemberInput> eMembers;
        eMembers.push_back(method("<clinit>", "()V", 0x0008));
        classes.push_back(methodClass("c/E", "c/D", std::move(eMembers)));
    }
    {
        std::vector<DbMemberInput> fMembers;
        fMembers.push_back(method("<init>", "()V", 0x0001));
        classes.push_back(methodClass("c/F", "c/E", std::move(fMembers)));
    }
    {
        std::vector<DbMemberInput> touchMembers;
        touchMembers.push_back(method("touch", "()V", 0x0001));
        classes.push_back(methodClass("c/Touch", "", std::move(touchMembers)));
    }
    {
        std::vector<DbMemberInput> regMembers;
        regMembers.push_back(method("<clinit>", "()V", 0x0008));
        classes.push_back(methodClass("x/Reg", "", std::move(regMembers)));
    }
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    // x/Reg 成员未登记：Field 引用落文本分支（stale 状态），闭包面须取其
    // 已登记 owner 类的 clinit 触发（x/Reg 在类事实侧已登记）。
    std::vector<DbReferenceRecord> refs;
    refs.push_back(fieldRef("c/Touch", "touch", "()V", 0, "c/E", "f", "I"));
    refs.push_back(methodRef("c/Touch", "touch", "()V", 1, "c/F", "<init>", "()V"));
    refs.push_back(fieldRef("c/Touch", "touch", "()V", 2, "x/Reg", "ghost", "I"));
    ASSERT_TRUE(db.replaceReferences(0, std::move(refs)).is_ok());

    const auto producer = registerReachProducer(db);
    ReachabilityClosureOptions options;
    options.explicitSeeds.push_back(methodId(db, "c/Touch", "touch", "()V"));
    auto result = computeAndStoreReachabilityClosure(db, producer, options);
    ASSERT_TRUE(result.is_ok());
    auto stats = std::move(result).unwrap();

    const auto members = closureMembers(db, stats.batchId);
    const ca::i64 dClinit = methodId(db, "c/D", "<clinit>", "()V");
    const ca::i64 eClinit = methodId(db, "c/E", "<clinit>", "()V");
    const ca::i64 rClinit = methodId(db, "x/Reg", "<clinit>", "()V");
    const ca::i64 fInit   = methodId(db, "c/F", "<init>", "()V");

    // Field 首触 c/E：E.<clinit> + super 链 D.<clinit>；直接编码的 <init>
    // 目标可达（构造器真实执行）；文本分支取已登记 owner 类 x/Reg 的
    // <clinit>（ghost 成员未登记不产节点）；F 无 <clinit>，super 链已在集内。
    EXPECT_NE(std::find(members.begin(), members.end(), dClinit), members.end());
    EXPECT_NE(std::find(members.begin(), members.end(), eClinit), members.end());
    EXPECT_NE(std::find(members.begin(), members.end(), rClinit), members.end());
    EXPECT_NE(std::find(members.begin(), members.end(), fInit), members.end());
    EXPECT_EQ(5u, members.size());
    EXPECT_LE(2u, stats.clinitMarks);
    EXPECT_LE(1u, stats.textBranchClasses);
}

TEST(MjReachabilityClosureTest, DefaultMethodConflictConservativeUnion)
{
    const auto path = freshDbPath("default_conflict");
    auto       db   = openOrDie(path);
    std::vector<DbClassInput> classes;
    {
        std::vector<DbMemberInput> i1;
        i1.push_back(method("f", "()V", 0x0001));
        classes.push_back(methodClass("d/I1", "", std::move(i1), {}, 0x0200));
    }
    {
        std::vector<DbMemberInput> i2;
        i2.push_back(method("f", "()V", 0x0001));
        classes.push_back(methodClass("d/I2", "", std::move(i2), {}, 0x0200));
    }
    classes.push_back(methodClass("d/K", "", {}, {"d/I1", "d/I2"}));
    {
        std::vector<DbMemberInput> caller;
        caller.push_back(method("go", "()V", 0x0001));
        classes.push_back(methodClass("d/Caller", "", std::move(caller)));
    }
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());

    std::vector<DbReferenceRecord> refs;
    refs.push_back(methodRef("d/Caller", "go", "()V", 0, "d/I1", "f", "()V"));
    ASSERT_TRUE(db.replaceReferences(0, std::move(refs)).is_ok());

    const auto producer = registerReachProducer(db);
    ReachabilityClosureOptions options;
    options.explicitSeeds.push_back(methodId(db, "d/Caller", "go", "()V"));
    auto result = computeAndStoreReachabilityClosure(db, producer, options);
    ASSERT_TRUE(result.is_ok());
    auto stats = std::move(result).unwrap();

    // 冲突族全部入集（不做 most-specific 裁决）：编码目标 I1.f，I2.f 经
    // sibling 接口组并集同入；种子 go 自身在集。
    const auto members = closureMembers(db, stats.batchId);
    const ca::i64 i1f = methodId(db, "d/I1", "f", "()V");
    const ca::i64 i2f = methodId(db, "d/I2", "f", "()V");
    EXPECT_NE(std::find(members.begin(), members.end(), i1f), members.end());
    EXPECT_NE(std::find(members.begin(), members.end(), i2f), members.end());
    EXPECT_EQ(3u, members.size());
}

TEST(MjReachabilityClosureTest, EntryPointsAndKeepRootsSeedProjection)
{
    const auto path = freshDbPath("seed_masks");
    auto       db   = openOrDie(path);
    std::vector<DbClassInput> classes;
    {
        std::vector<DbMemberInput> appMembers;
        appMembers.push_back(method("main", "([Ljava/lang/String;)V", 0x0009));
        appMembers.push_back(method("run", "()V", 0x0001));
        classes.push_back(methodClass("e/App", "", std::move(appMembers), {}, 0x0021));
    }
    {
        std::vector<DbMemberInput> pubMembers;
        pubMembers.push_back(method("pub", "()V", 0x0001));
        pubMembers.push_back(method("prot", "()V", 0x0004));
        pubMembers.push_back(method("priv", "()V", 0x0002));
        pubMembers.push_back(method("pack", "()V", 0x0000));
        classes.push_back(methodClass("k/Pub", "", std::move(pubMembers), {}, 0x0021));
    }
    {
        std::vector<DbMemberInput> hiddenMembers;
        hiddenMembers.push_back(method("hidden", "()V", 0x0001));
        classes.push_back(methodClass("k/Hidden", "", std::move(hiddenMembers)));
    }
    ASSERT_TRUE(db.ingestClasses(0, std::move(classes)).is_ok());
    ASSERT_TRUE(db.replaceReferences(0, {}).is_ok());

    const auto producer = registerReachProducer(db);

    // entry_points：main 发现（public static void main(String[])），闭包只含
    // 发现的入口（语料无引用边）。
    ReachabilityClosureOptions entry;
    entry.entryPoints = true;
    auto result       = computeAndStoreReachabilityClosure(db, producer, entry);
    ASSERT_TRUE(result.is_ok());
    auto entryStats = std::move(result).unwrap();
    const auto entryMembers = closureMembers(db, entryStats.batchId);
    EXPECT_EQ(std::vector<ca::i64>({methodId(db, "e/App", "main", "([Ljava/lang/String;)V")}),
              entryMembers);
    EXPECT_EQ(static_cast<ca::u32>(ClosureSeedMask::EntryPoints), entryStats.seedMask);
    EXPECT_EQ(1u, entryStats.seedCount);

    // keep_roots：public 类 × public/protected 具体方法投影。private / 包私有
    // / 非 public 类的 public 方法都不入种子。e/App 与 k/Pub 都是 public 类，
    // keep 种子 = {main, run, pub, prot}。
    ReachabilityClosureOptions keep;
    keep.keepRoots = true;
    result         = computeAndStoreReachabilityClosure(db, producer, keep);
    ASSERT_TRUE(result.is_ok());
    auto keepStats = std::move(result).unwrap();
    EXPECT_NE(entryStats.batchId, keepStats.batchId);   // 档位不同 → 不复用
    const auto keepMembers = closureMembers(db, keepStats.batchId);
    std::vector<ca::i64> expectedKeep = {
        methodId(db, "e/App", "main", "([Ljava/lang/String;)V"),
        methodId(db, "e/App", "run", "()V"),
        methodId(db, "k/Pub", "pub", "()V"),
        methodId(db, "k/Pub", "prot", "()V"),
    };
    std::sort(expectedKeep.begin(), expectedKeep.end());
    EXPECT_EQ(expectedKeep, keepMembers);
    EXPECT_EQ(static_cast<ca::u32>(ClosureSeedMask::KeepRoots), keepStats.seedMask);
    EXPECT_EQ(4u, keepStats.seedCount);
    EXPECT_EQ(2u, closureBatchCount(db));   // 两个档位两批
}

TEST(MjReachabilityClosureTest, FingerprintReusesAndInvalidatesOnRefsRewrite)
{
    const auto path = freshDbPath("fingerprint_invalidate");
    auto       db   = buildVirtualCorpus(path);
    const auto producer = registerReachProducer(db);

    ReachabilityClosureOptions options;
    options.explicitSeeds.push_back(methodId(db, "a/Caller", "call", "()V"));

    auto first = computeAndStoreReachabilityClosure(db, producer, options);
    ASSERT_TRUE(first.is_ok());
    auto firstStats = std::move(first).unwrap();
    EXPECT_FALSE(firstStats.reused);

    // 指纹 + 档位命中：同库同状态二次计算复用不重算。
    auto second = computeAndStoreReachabilityClosure(db, producer, options);
    ASSERT_TRUE(second.is_ok());
    auto secondStats = std::move(second).unwrap();
    EXPECT_TRUE(secondStats.reused);
    EXPECT_EQ(firstStats.batchId, secondStats.batchId);
    EXPECT_EQ(firstStats.memberCount, secondStats.memberCount);
    EXPECT_EQ(1u, closureBatchCount(db));

    // refs 整库替换（同内容重插）：ref_id 单调不复用 → 聚合必变 → 指纹失配
    // → 拒复用整体重算落新批。
    std::vector<DbReferenceRecord> refs;
    refs.push_back(methodRef("a/Caller", "call", "()V", 0, "a/Base", "virtualTarget", "()V"));
    refs.push_back(methodRef("a/Caller", "call", "()V", 1, "a/Base", "directOnly", "()V"));
    ASSERT_TRUE(db.replaceReferences(0, std::move(refs)).is_ok());

    auto third = computeAndStoreReachabilityClosure(db, producer, options);
    ASSERT_TRUE(third.is_ok());
    auto thirdStats = std::move(third).unwrap();
    EXPECT_FALSE(thirdStats.reused);
    EXPECT_NE(firstStats.batchId, thirdStats.batchId);
    EXPECT_EQ(firstStats.memberCount, thirdStats.memberCount);
    EXPECT_EQ(2u, closureBatchCount(db));

    // 两批指纹不同；fresh 判定按指纹 + 档位全量匹配（旧批指纹仍命中原批）。
    auto batches = db.listClosureBatches();
    ASSERT_TRUE(batches.is_ok());
    auto rows = std::move(batches).unwrap();
    ASSERT_EQ(2u, rows.size());
    EXPECT_NE(rows[0].fingerprint, rows[1].fingerprint);
    auto fresh = db.freshClosureBatch(std::vector<ca::u8>(rows[1].fingerprint), rows[1].seedMask);
    ASSERT_TRUE(fresh.is_ok());
    auto freshRow = std::move(fresh).unwrap();
    ASSERT_TRUE(freshRow.has_value());
    EXPECT_EQ(rows[1].batchId, freshRow->batchId);
    auto stale = db.freshClosureBatch(std::vector<ca::u8>(rows[0].fingerprint), rows[0].seedMask);
    ASSERT_TRUE(stale.is_ok());
    auto staleRow = std::move(stale).unwrap();
    ASSERT_TRUE(staleRow.has_value());
    EXPECT_EQ(rows[0].batchId, staleRow->batchId);
}

TEST(MjReachabilityClosureTest, ProducerAttributionAndClassScopesReconcile)
{
    const auto path = freshDbPath("attribution_scopes");
    auto       db   = buildVirtualCorpus(path);
    const auto producer = registerReachProducer(db);

    ReachabilityClosureOptions options;
    options.explicitSeeds.push_back(methodId(db, "a/Caller", "call", "()V"));
    auto result = computeAndStoreReachabilityClosure(db, producer, options);
    ASSERT_TRUE(result.is_ok());
    const auto stats = std::move(result).unwrap();

    // 批次归属：closure_write 批挂 producer，member_count 回填闭包行数。
    auto batches = db.listBatches(std::nullopt);
    ASSERT_TRUE(batches.is_ok());
    auto batchRows = std::move(batches).unwrap();
    ASSERT_EQ(1u, batchRows.size());
    EXPECT_EQ(DbBatchKind::ClosureWrite, batchRows[0].kind);
    EXPECT_EQ(producer, batchRows[0].producerId);
    EXPECT_FALSE(batchRows[0].finishedAt.is_empty());
    EXPECT_EQ(4, batchRows[0].memberCount);

    // 类域作用域对账：scope 行 = 闭包成员宿主类集合（去重，升序）。
    auto scopes = db.batchScopes(stats.batchId);
    ASSERT_TRUE(scopes.is_ok());
    auto scopeRows = std::move(scopes).unwrap();
    std::vector<ca::i64> scopeIds;
    for (const auto& scope : scopeRows) {
        EXPECT_EQ(DbBatchScopeKind::Class, scope.kind);
        scopeIds.push_back(scope.scopeId);
    }
    auto baseClass   = db.findClass(S("a/Base"));
    auto callerClass = db.findClass(S("a/Caller"));
    auto subClass    = db.findClass(S("a/Sub"));
    ASSERT_TRUE(baseClass.is_ok() && callerClass.is_ok() && subClass.is_ok());
    std::vector<ca::i64> expectedScopes = {
        std::move(baseClass).unwrap()->classId,
        std::move(callerClass).unwrap()->classId,
        std::move(subClass).unwrap()->classId,
    };
    std::sort(expectedScopes.begin(), expectedScopes.end());
    EXPECT_EQ(expectedScopes, scopeIds);

    // producer 聚合统计对账。
    auto statsRows = db.producerStats();
    ASSERT_TRUE(statsRows.is_ok());
    auto producerRows = std::move(statsRows).unwrap();
    ASSERT_EQ(1u, producerRows.size());
    EXPECT_EQ(1, producerRows[0].batchCount);
    EXPECT_EQ(4, producerRows[0].memberCount);
    EXPECT_FALSE(producerRows[0].lastBatchAt.is_empty());
}

TEST(MjReachabilityClosureTest, DeterministicAcrossDatabases)
{
    const auto pathA = freshDbPath("determinism_a");
    const auto pathB = freshDbPath("determinism_b");

    auto computeOn = [&](AnalysisDb& db) {
        ReachabilityClosureOptions opts;
        opts.entryPoints = true;
        opts.keepRoots   = true;
        const auto producer = registerReachProducer(db);
        auto       result   = computeAndStoreReachabilityClosure(db, producer, opts);
        EXPECT_TRUE(result.is_ok());
        return result.is_ok() ? std::move(result).unwrap() : ReachabilityClosureStats();
    };

    auto dbA = buildVirtualCorpus(pathA);
    auto dbB = buildVirtualCorpus(pathB);
    // 两库语料完全一致（同构造序 → 同 id 分配 → 同指纹）：补一个公共 main 入口
    // 让 entry_points 档有种子。
    for (AnalysisDb* db : {&dbA, &dbB}) {
        std::vector<DbClassInput> extra;
        std::vector<DbMemberInput> mainMembers;
        mainMembers.push_back(method("main", "([Ljava/lang/String;)V", 0x0009));
        extra.push_back(methodClass("z/Entry", "", std::move(mainMembers), {}, 0x0021));
        ASSERT_TRUE(db->ingestClasses(0, std::move(extra)).is_ok());
    }

    auto statsA = computeOn(dbA);
    auto statsB = computeOn(dbB);
    ASSERT_FALSE(statsA.reused);
    ASSERT_FALSE(statsB.reused);

    const auto membersA = closureMembers(dbA, statsA.batchId);
    const auto membersB = closureMembers(dbB, statsB.batchId);
    ASSERT_FALSE(membersA.empty());
    EXPECT_EQ(membersA, membersB);

    auto fpA = reachabilityClosureFingerprint(dbA);
    auto fpB = reachabilityClosureFingerprint(dbB);
    ASSERT_TRUE(fpA.is_ok());
    ASSERT_TRUE(fpB.is_ok());
    EXPECT_EQ(std::move(fpA).unwrap(), std::move(fpB).unwrap());

    // 同库同档位重算走复用：成员集与批次均不变。
    ReachabilityClosureOptions again;
    again.entryPoints = true;
    again.keepRoots   = true;
    auto producers    = dbA.listProducers();
    ASSERT_TRUE(producers.is_ok());
    auto producerRows = std::move(producers).unwrap();
    ASSERT_EQ(1u, producerRows.size());
    auto reused = computeAndStoreReachabilityClosure(dbA, producerRows[0].producerId, again);
    ASSERT_TRUE(reused.is_ok());
    auto reusedStats = std::move(reused).unwrap();
    EXPECT_TRUE(reusedStats.reused);
    EXPECT_EQ(statsA.batchId, reusedStats.batchId);
}

TEST(MjReachabilityClosureTest, PaginationAndMembershipContract)
{
    const auto path = freshDbPath("pagination");
    auto       db   = buildVirtualCorpus(path);
    const auto producer = registerReachProducer(db);

    ReachabilityClosureOptions options;
    options.explicitSeeds.push_back(methodId(db, "a/Caller", "call", "()V"));
    auto result = computeAndStoreReachabilityClosure(db, producer, options);
    ASSERT_TRUE(result.is_ok());
    const auto stats = std::move(result).unwrap();

    // 全量 = 各页拼接，跨页不重不漏（member_id 升序）。
    auto page1 = db.closureMembersPaged(stats.batchId, 2, 0);
    auto page2 = db.closureMembersPaged(stats.batchId, 2, 2);
    auto page3 = db.closureMembersPaged(stats.batchId, 2, 4);
    ASSERT_TRUE(page1.is_ok());
    ASSERT_TRUE(page2.is_ok());
    ASSERT_TRUE(page3.is_ok());
    auto p1 = std::move(page1).unwrap();
    auto p2 = std::move(page2).unwrap();
    auto p3 = std::move(page3).unwrap();
    EXPECT_EQ(2u, p1.size());
    EXPECT_EQ(2u, p2.size());
    EXPECT_TRUE(p3.empty());
    std::vector<ca::i64> all;
    all.insert(all.end(), p1.begin(), p1.end());
    all.insert(all.end(), p2.begin(), p2.end());
    const auto members = closureMembers(db, stats.batchId);
    EXPECT_EQ(members, all);
    EXPECT_TRUE(std::is_sorted(all.begin(), all.end()));

    // limit 0 = 空页；offset 超尾 = 空页；负值 = Invalid。
    auto emptyPage = db.closureMembersPaged(stats.batchId, 0, 0);
    ASSERT_TRUE(emptyPage.is_ok());
    EXPECT_TRUE(std::move(emptyPage).unwrap().empty());
    auto tailPage = db.closureMembersPaged(stats.batchId, 4, 99);
    ASSERT_TRUE(tailPage.is_ok());
    EXPECT_TRUE(std::move(tailPage).unwrap().empty());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.closureMembersPaged(stats.batchId, -1, 0)));
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.closureMembersPaged(stats.batchId, 4, -1)));

    // 成员判定：集合内 true / 集合外 false；未知闭包批 → Invalid。
    const ca::i64 inSet  = members.front();
    const ca::i64 outSet = methodId(db, "a/Itf", "virtualTarget", "()V");
    auto inResult  = db.isClosureMember(stats.batchId, inSet);
    auto outResult = db.isClosureMember(stats.batchId, outSet);
    ASSERT_TRUE(inResult.is_ok());
    ASSERT_TRUE(outResult.is_ok());
    EXPECT_TRUE(std::move(inResult).unwrap());
    EXPECT_FALSE(std::move(outResult).unwrap());
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.isClosureMember(424242, inSet)));

    // 未知批分页与 fresh 判定同口径拒绝；指纹长度不是 32 → Invalid。
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.closureMembersPaged(424242, 10, 0)));
    std::vector<ca::u8> shortFp(31, 0x11);
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.freshClosureBatch(shortFp, 0)));
    auto missing = db.freshClosureBatch(std::vector<ca::u8>(32, 0x22), 0);
    ASSERT_TRUE(missing.is_ok());
    EXPECT_FALSE(std::move(missing).unwrap().has_value());
}

TEST(MjReachabilityClosureTest, StoreClosureBatchInputValidation)
{
    const auto path = freshDbPath("store_validation");
    auto       db   = buildVirtualCorpus(path);
    const auto producer = registerReachProducer(db);

    auto fingerprint = reachabilityClosureFingerprint(db);
    ASSERT_TRUE(fingerprint.is_ok());

    DbClosureBatchRecord record;
    record.producerId  = producer;
    record.fingerprint = std::move(fingerprint).unwrap();
    record.seedMask    = static_cast<ca::u32>(ClosureSeedMask::Explicit);
    record.seedCount   = 1;
    record.memberIds   = {1, 2, 3};
    auto stored        = db.storeClosureBatch(record);
    ASSERT_TRUE(stored.is_ok());
    auto row = std::move(stored).unwrap();
    EXPECT_EQ(3, row.memberCount);
    EXPECT_FALSE(row.computedAt.is_empty());
    EXPECT_EQ(record.fingerprint, row.fingerprint);
    EXPECT_EQ(1u, row.fpVersion);

    // 指纹长度不是 32 / memberIds 非升序去重 / 未知 producer / 负计数 →
    // Invalid。校验先于开批，不产生残批。
    DbClosureBatchRecord badFp = record;
    badFp.fingerprint.pop_back();
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.storeClosureBatch(badFp)));

    DbClosureBatchRecord unsorted = record;
    unsorted.memberIds            = {3, 2};
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.storeClosureBatch(unsorted)));

    DbClosureBatchRecord dup = record;
    dup.memberIds            = {5, 5};
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.storeClosureBatch(dup)));

    DbClosureBatchRecord unknownProducer = record;
    unknownProducer.producerId           = 999;
    EXPECT_EQ(AnalysisDbErrorKind::Invalid,
              takeErrorKind(db.storeClosureBatch(unknownProducer)));

    DbClosureBatchRecord negative = record;
    negative.seedCount            = -1;
    EXPECT_EQ(AnalysisDbErrorKind::Invalid, takeErrorKind(db.storeClosureBatch(negative)));

    // 校验失败输入未留批：批次行仍只有首批事实。
    auto batches = db.listBatches(std::nullopt);
    ASSERT_TRUE(batches.is_ok());
    EXPECT_EQ(1u, std::move(batches).unwrap().size());

    // 未登记成员 FK 拒绝：事实段回滚留残批（合法审计状态），事实行不落。
    DbClosureBatchRecord ghost = record;
    ghost.memberIds            = {424242};
    ASSERT_TRUE(db.storeClosureBatch(ghost).is_err());
    auto ghostBatches = db.listBatches(std::nullopt);
    ASSERT_TRUE(ghostBatches.is_ok());
    EXPECT_EQ(2u, std::move(ghostBatches).unwrap().size());   // 首批 + 残批
    EXPECT_EQ(1u, closureBatchCount(db));                     // 残批无事实行
}
