/// @file        renamer_engine_test.cpp
/// @brief       统一 renamer 引擎管线单测（mjbase 移植版，luiox/morpher#1025/
///              #1032/#1037 对应物）：合法性矩阵、冲突消解矩阵、记录与导出、
///              run 两态、继承别名。
///
/// mjbase 底座差异（语义不变，见迁移报告偏离清单）：
/// - 夹具类经 jasm 组装（旧栈 jbytecode ClassWriter）；
/// - run 结果 ca::Status → ok+error 值对；
/// - 应用面已接线（asm/remapper 域）：旧栈 planOnly=false 的落盘断言以读回
///   断言落位（fail-loud 占位期的容器零写入断言随接线废止）。
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/mapping/mapping_format_registry.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/renamer/name_proposal.hpp"
#include "mjbase/renamer/name_provider.hpp"
#include "mjbase/renamer/rename_engine.hpp"

#include "renamer_fixture.hpp"

namespace mjbase::renamer {

namespace {

namespace fixture = renamer::test_support;

/// 单条建议批量构造。
inline std::vector<NameProposal> batchOf(NameProposal sole)
{
    std::vector<NameProposal> batch;
    batch.push_back(std::move(sole));
    return batch;
}

NameProposal P(ProposalKind kind, const char* owner, const char* oldName, const char* desc,
               const char* newName, double confidence = 1.0, const char* source = "")
{
    // source 默认留空：由引擎回填 provider 名（审计溯源口径）。
    return NameProposal(kind, owner, oldName, desc, newName, confidence, source);
}

/// 固定建议流的测试 provider（一次性：propose 移交建议列表）。
class FixedProvider final : public NameProvider
{
public:
    FixedProvider(const char* name, std::vector<NameProposal> proposals)
        : name_(name)
        , proposals_(std::move(proposals))
    {}

    std::string_view name() const override { return name_; }

    ca::core::Result<std::vector<NameProposal>, std::string> propose(
        const RenameContext&) override
    {
        return ca::core::Ok(std::move(proposals_));
    }

private:
    std::string               name_;
    std::vector<NameProposal> proposals_;
};

/// propose 整体硬失败的测试 provider。
class FailingProvider final : public NameProvider
{
public:
    explicit FailingProvider(const char* name)
        : name_(name)
    {}

    std::string_view name() const override { return name_; }

    ca::core::Result<std::vector<NameProposal>, std::string> propose(
        const RenameContext&) override
    {
        return ca::core::Err(std::string("boom"));
    }

private:
    std::string name_;
};

/// 基础夹具：Object ← A（field/method/X/alpha）← B（override oldMethod），另有独立 C。
struct Fixture
{
    model::ResourceContainer container;

    Fixture()
    {
        container.addResource("java/lang/Object.class",
                              std::make_unique<model::ClassResource>("java/lang/Object.class",
                                                                     fixture::buildBareObject()));
        container.addResource("A.class",
                              std::make_unique<model::ClassResource>("A.class",
                                                                     fixture::buildA()));
        container.addResource("B.class",
                              std::make_unique<model::ClassResource>("B.class",
                                                                     fixture::buildB()));
        container.addResource("C.class",
                              std::make_unique<model::ClassResource>("C.class",
                                                                     fixture::buildC()));
    }
};

/// 容器全部 class 资源字节快照（URI → 字节），供容器零写入断言。
std::vector<std::pair<std::string, std::vector<ca::u8>>> snapshotBytes(
    model::ResourceContainer& container)
{
    std::vector<std::pair<std::string, std::vector<ca::u8>>> before;
    container.visitResources([&before](std::string_view uri, const model::IResource& resource) {
        const auto* cls = ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        ASSERT_NE(nullptr, cls);
        const auto& bytes = cls->get();
        before.emplace_back(std::string(uri),
                            std::vector<ca::u8>(bytes.begin(), bytes.end()));
    });
    return before;
}

void expectContainerUntouched(model::ResourceContainer& container)
{
    const auto after = snapshotBytes(container);
    // 快照序不保证与首拍一致：按 URI 配对比较。
    auto snapshotOf = [&after](const std::string& uri) -> const std::vector<ca::u8>* {
        for (const auto& [name, bytes] : after) {
            if (name == uri) return &bytes;
        }
        return nullptr;
    };
    const auto before = snapshotBytes(container);
    for (const auto& [uri, bytes] : before) {
        const auto* current = snapshotOf(uri);
        ASSERT_NE(current, nullptr) << uri;
        ASSERT_EQ(bytes, *current) << uri;
    }
}

const RejectedProposal* findRejected(const RenamerRunResult& result, RejectionReason reason)
{
    for (const auto& entry : result.record.rejected()) {
        if (entry.reason == reason) return &entry;
    }
    return nullptr;
}

/// 容器内某 URI 的 class 资源（缺失/异型返回空）。
model::ClassResource* containerClassResource(model::ResourceContainer& container,
                                             const std::string&        uri)
{
    return ca::core::tag_cast::cast<model::ClassResource*>(container.getResource(uri));
}

/// 类内全部指令的成员引用（owner/name/desc；调用点读回断言用）。
struct MemberRef
{
    std::string owner;
    std::string name;
    std::string desc;
};

std::vector<MemberRef> memberRefsOf(model::ResourceContainer& container,
                                    const std::string&        uri)
{
    std::vector<MemberRef> refs;
    const auto*            res = containerClassResource(container, uri);
    if (res == nullptr) return refs;
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(res->get()));
    if (sessionResult.is_err()) return refs;
    auto session = std::move(sessionResult).unwrap();
    for (auto& visit : jbc::ir::collectCodeUnits(*session)) {
        if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
        const jbc::ir::CodeUnit& cu = *visit.unit.get();
        for (const jbc::ir::InsnId iid : cu.dom.order()) {
            const jbc::ir::Insn* in = cu.dom.peek(iid);
            if (in == nullptr) continue;
            jbc::ir::ClassSession::MemberSem sem;
            if (session->memberOf(*in, sem)) {
                refs.push_back({std::string(sem.owner), std::string(sem.name),
                                std::string(sem.desc)});
            }
        }
    }
    return refs;
}

bool hasMemberRef(const std::vector<MemberRef>& refs, const std::string& owner,
                  const std::string& name)
{
    for (const auto& ref : refs) {
        if (ref.owner == owner && ref.name == name) return true;
    }
    return false;
}

bool hasMemberRef(const std::vector<MemberRef>& refs, const std::string& owner,
                  const std::string& name, const std::string& desc)
{
    for (const auto& ref : refs) {
        if (ref.owner == owner && ref.name == name && ref.desc == desc) return true;
    }
    return false;
}

/// 容器内某 class 资源的声明方法名集合（成员槽级读回——remapper 面只重指向
/// 不改写 Utf8 条目内容，字节文本扫描会误报残留）。
std::vector<std::string> declaredMethodNamesOf(model::ResourceContainer& container,
                                               const std::string&        uri)
{
    std::vector<std::string> names;
    const auto*              res = containerClassResource(container, uri);
    if (res == nullptr) return names;
    auto r = jbc::core::readClass(jbc::core::ByteView(res->get()));
    if (r.is_err()) return names;
    const auto file = std::move(r).unwrap().file;
    for (const auto& method : file->methods) {
        names.emplace_back(file->cp.utf8Of(method.nameCp));
    }
    return names;
}

bool contains(const std::vector<std::string>& values, const std::string& value)
{
    for (const auto& item : values) {
        if (item == value) return true;
    }
    return false;
}

}   // namespace

// ── 合法性矩阵 ──

TEST(MjRenamerLegalityTest, rejectsKeywordIllegalCharNoOpSchemaAndMissingTarget)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;   // 默认 plan-only

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "alpha", "()V", "class"));         // 关键字
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "bad-name"));      // 非法字符
    batch.push_back(P(ProposalKind::Class, "", "A", "", "a b/C"));                  // 分段非法
    batch.push_back(P(ProposalKind::Class, "", "no/such/Class", "", "whatever"));   // 类不存在
    batch.push_back(P(ProposalKind::Package, "", "no/such/pkg", "", "new/pkg"));    // 包前缀无类
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "oldField"));      // no-op
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "", "fine"));           // 缺 desc
    batch.push_back(P(ProposalKind::Method, "A", "alpha", "()V", "ok", 1.5));   // confidence 越界
    FixedProvider provider("batch", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.proposed, 8U);
    EXPECT_EQ(result.stats.accepted, 0U);
    EXPECT_EQ(result.stats.rejected, 8U);
    EXPECT_TRUE(result.plan.empty());

    const auto& rejected = result.record.rejected();
    ASSERT_EQ(rejected.size(), 8U);
    EXPECT_EQ(rejected[0].reason, RejectionReason::Keyword);
    EXPECT_EQ(rejected[1].reason, RejectionReason::IllegalIdentifier);
    EXPECT_EQ(rejected[2].reason, RejectionReason::IllegalIdentifier);
    EXPECT_EQ(rejected[3].reason, RejectionReason::NotFound);
    EXPECT_EQ(rejected[4].reason, RejectionReason::NotFound);
    EXPECT_EQ(rejected[5].reason, RejectionReason::Schema);
    EXPECT_EQ(rejected[6].reason, RejectionReason::Schema);
    EXPECT_EQ(rejected[7].reason, RejectionReason::Schema);
    // 被拒流水完整：保留建议原文 + 原因 + 细节
    EXPECT_EQ(std::string(rejected[0].proposal.newName), "class");
    EXPECT_EQ(rejected[0].proposal.source, "batch");
    EXPECT_FALSE(rejected[0].detail.empty());
}

TEST(MjRenamerLegalityTest, exemptsInitClinitAndRejectsThemAsNewNames)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "<init>", "()V", "ctor"));      // 豁免不改
    batch.push_back(P(ProposalKind::Method, "A", "alpha", "()V", "<clinit>"));   // 不可作新名
    batch.push_back(P(ProposalKind::Method, "A", "alpha", "()V", "renamed"));    // 通过
    FixedProvider provider("batch", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.accepted, 1U);
    EXPECT_EQ(result.stats.rejected, 2U);
    const auto& rejected = result.record.rejected();
    ASSERT_EQ(rejected.size(), 2U);
    EXPECT_EQ(rejected[0].reason, RejectionReason::ReservedName);
    EXPECT_EQ(rejected[1].reason, RejectionReason::ReservedName);
    EXPECT_NE(result.plan.findMethod("A", "alpha", "()V"), nullptr);
}

TEST(MjRenamerLegalityTest, nonAsciiIdentifierFollowsIdentifierUtilPolicy)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    // IdentifierUtil::isValidJavaIdentifier 接受 >0x80 码点（JLS 合法口径）——
    // 引擎复用该谓词，此处锁定该既有口径，不另立第三套合法性规则。
    std::vector<NameProposal> batch;
    batch.push_back(
        P(ProposalKind::Field, "A", "oldField", "I", "\xE5\x8F\x98\xE9\x87\x8F"));   // 变量
    FixedProvider provider("batch", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.accepted, 1U);
    const auto* rule = result.plan.findField("A", "oldField", "I");
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(std::string(rule->newName), "\xE5\x8F\x98\xE9\x87\x8F");
}

TEST(MjRenamerLegalityTest, nonAsciiSegmentStillSplitBySlash)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    // 内部名分段：'/' 是路径分隔符，任何一段非法即整体拒。
    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Class, "", "C", "", "p/ok name"));
    batch.push_back(P(ProposalKind::Class, "", "C", "", "p/ok/Mx"));
    FixedProvider provider("batch", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.accepted, 1U);
    EXPECT_EQ(result.stats.rejected, 1U);
    EXPECT_EQ(result.record.rejected().front().reason, RejectionReason::IllegalIdentifier);
    EXPECT_FALSE(result.plan.mapInternalName("C") == "C");
}

// ── 冲突消解矩阵 ──

TEST(MjRenamerConflictTest, sameTargetHigherConfidenceWins)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    std::vector<NameProposal> lowBatch;
    lowBatch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "renamed", 0.4, "low"));
    FixedProvider             first("low", std::move(lowBatch));
    std::vector<NameProposal> highBatch;
    highBatch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "renamed", 0.9, "high"));
    FixedProvider second("high", std::move(highBatch));

    auto       providers = std::vector<NameProvider*>{&first, &second};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.proposed, 2U);
    EXPECT_EQ(result.stats.accepted, 1U);
    EXPECT_EQ(result.stats.rejected, 1U);
    const auto* rejected = findRejected(result, RejectionReason::Conflict);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->proposal.source, "low");   // 高 confidence 者胜
    const auto* rule = result.plan.findField("A", "oldField", "I");
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->newName, "renamed");
    const auto& accepted = result.record.accepted();
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_EQ(accepted.front().source, "high");
}

TEST(MjRenamerConflictTest, sameTargetEqualConfidenceFirstArrivalWins)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    FixedProvider first("p1",
                        batchOf(P(ProposalKind::Method, "A", "alpha", "()V", "target", 0.5, "p1")));
    FixedProvider second(
        "p2", batchOf(P(ProposalKind::Method, "A", "alpha", "()V", "target", 0.5, "p2")));

    auto       providers = std::vector<NameProvider*>{&first, &second};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.accepted, 1U);
    const auto& accepted = result.record.accepted();
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_EQ(accepted.front().source, "p1");   // 同 confidence 先到先得
    const auto* rejected = findRejected(result, RejectionReason::Conflict);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->proposal.source, "p2");
}

TEST(MjRenamerConflictTest, sameOldSlotDifferentNewNamesAdjudicated)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    // 同一成员被改到两个不同新名：旧位槽裁决，高 confidence 胜。
    FixedProvider first("p1",
                        batchOf(P(ProposalKind::Method, "A", "alpha", "()V", "one", 0.5, "p1")));
    FixedProvider second("p2",
                         batchOf(P(ProposalKind::Method, "A", "alpha", "()V", "two", 0.8, "p2")));

    auto       providers = std::vector<NameProvider*>{&first, &second};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.accepted, 1U);
    const auto* rule = result.plan.findMethod("A", "alpha", "()V");
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->newName, "two");
    EXPECT_FALSE(result.plan.findMethod("A", "alpha", "()V")->newName == "one");
}

TEST(MjRenamerConflictTest, overrideClosureRenamesWholeComponent)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    FixedProvider provider(
        "p", batchOf(P(ProposalKind::Method, "A", "oldMethod", "()V", "renamedMethod")));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    // 闭包组件 {A.oldMethod, B.oldMethod} 整组同改
    EXPECT_EQ(result.stats.accepted, 1U);
    EXPECT_EQ(result.stats.renamedMembers, 2U);
    ASSERT_NE(result.plan.findMethod("A", "oldMethod", "()V"), nullptr);
    ASSERT_NE(result.plan.findMethod("B", "oldMethod", "()V"), nullptr);
    EXPECT_EQ(result.plan.findMethod("A", "oldMethod", "()V")->newName, "renamedMethod");
    EXPECT_EQ(result.plan.findMethod("B", "oldMethod", "()V")->newName, "renamedMethod");
    const auto& accepted = result.record.accepted();
    ASSERT_EQ(accepted.size(), 2U);
    EXPECT_EQ(accepted[0].fromClosure, false);
    EXPECT_EQ(accepted[1].fromClosure, true);
}

TEST(MjRenamerConflictTest, residualClassCollisionGetsSuffix)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    // A → C 与未改名的存量类 C 撞名 → 残余撞名 `_N` 后缀
    FixedProvider provider("p", batchOf(P(ProposalKind::Class, "", "A", "", "C")));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.suffixResolved, 1U);
    EXPECT_EQ(result.plan.mapInternalName("A"), "C_1");
    const auto& accepted = result.record.accepted();
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_EQ(accepted.front().newName, "C_1");
}

TEST(MjRenamerConflictTest, residualFieldCollisionGetsSuffix)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    // oldField → other 与 A 既有字段 other（未被改名）撞名 → other_1
    FixedProvider provider("p", batchOf(P(ProposalKind::Field, "A", "oldField", "I", "other")));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.suffixResolved, 1U);
    const auto* rule = result.plan.findField("A", "oldField", "I");
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->newName, "other_1");
}

TEST(MjRenamerConflictTest, closureGroupGetsUniformSuffix)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    // A/B 都已有 X()V；oldMethod → X 需整组统一后缀 X_1（闭包一致性不可破坏）。
    FixedProvider provider("p", batchOf(P(ProposalKind::Method, "A", "oldMethod", "()V", "X")));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.suffixResolved, 1U);
    EXPECT_EQ(result.plan.findMethod("A", "oldMethod", "()V")->newName, "X_1");
    EXPECT_EQ(result.plan.findMethod("B", "oldMethod", "()V")->newName, "X_1");
}

TEST(MjRenamerConflictTest, suffixWallRejectsWholeClosureGroupAndOldSlotsStayOccupied)
{
    // 64 连撞：A/B 各备 65 个占位（X 与 X_1..X_64），oldMethod→X 无一致后缀 → 整组拒
    // （fail-closed）；随后 alpha→oldMethod 必须感知被拒组回插的旧槽占用，落 oldMethod_1。
    // （旧栈在应用后重解析字节码验证旧槽占用；mjbase 锁 plan 面同款不变式：
    //   整组拒 = 无 A/B.oldMethod 规则，且后组落 oldMethod_1，不产出任何 X_65
    //   规则——应用面只消费规则，无规则即无改写，字节码零重名由此保证。）
    model::ResourceContainer container;
    container.addResource("java/lang/Object.class",
                          std::make_unique<model::ClassResource>("java/lang/Object.class",
                                                                 fixture::buildBareObject()));
    container.addResource("A.class",
                          std::make_unique<model::ClassResource>(
                              "A.class",
                              fixture::buildSuffixWallClass("A", "java/lang/Object", true)));
    container.addResource("B.class",
                          std::make_unique<model::ClassResource>(
                              "B.class", fixture::buildSuffixWallClass("B", "A", false)));

    RenamerEngine  engine;
    RenamerOptions options;   // plan-only

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "oldMethod", "()V", "X"));
    batch.push_back(P(ProposalKind::Method, "A", "alpha", "()V", "oldMethod"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.applied);
    EXPECT_EQ(result.stats.proposed, 2U);
    EXPECT_EQ(result.stats.accepted, 1U);
    EXPECT_EQ(result.stats.rejected, 1U);
    EXPECT_EQ(result.stats.renamedMembers, 1U);
    EXPECT_EQ(result.stats.suffixResolved, 1U);
    const auto* rejected = findRejected(result, RejectionReason::Conflict);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->proposal.oldName, "oldMethod");
    EXPECT_EQ(rejected->proposal.newName, "X");

    // 整组拒：被拒组不落任何规则（旧名实际没改走，应用面才不产生重名）。
    EXPECT_EQ(result.plan.findMethod("A", "oldMethod", "()V"), nullptr);
    EXPECT_EQ(result.plan.findMethod("B", "oldMethod", "()V"), nullptr);
    // 不越 64 后缀上限：全计划无 X_65。
    for (const auto& rule : result.plan.methodRules()) {
        EXPECT_FALSE(rule.newName == "X_65") << std::string(rule.owner) << "."
                                             << std::string(rule.oldName);
    }
    // 后组感知旧槽占用：alpha 只能落到 oldMethod_1。
    const auto* alpha = result.plan.findMethod("A", "alpha", "()V");
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(alpha->newName, "oldMethod_1");
}

TEST(MjRenamerConflictTest, methodSlotCollisionCheckedInMappedDescSpace)
{
    // luiox/morpher#1037 复现：类 A 声明 m:(LA;)V 与 n:(LB;)V（B 为容器外类型，
    // A→B 改名不被 6a 后缀拦截），m 改名到 n。修复前 6c 在旧 desc 空间查
    // ("n","(LA;)V")——与既有 ("n","(LB;)V") 不撞放行；应用后类映射 A→B 把 m 的
    // desc 预映像为 (LB;)V，最终类文件出现两个 n:(LB;)V（非法 class file）。
    // 修复后 6c 查撞在 mapped-desc 空间进行（与成员规则 newDesc 预映像同机制），
    // 命中既有 n 槽 → m 走 `_N` 后缀消解落 n_1。
    std::string members;
    members += fixture::voidMethod("public", "<init>", "()V");
    members += fixture::voidMethod("public", "m", "(LA;)V");
    members += fixture::voidMethod("public", "n", "(LB;)V");
    model::ResourceContainer container;
    container.addResource(
        "A.class", std::make_unique<model::ClassResource>(
                       "A.class", fixture::assembleClass("A", "java/lang/Object", members)));

    RenamerEngine  engine;
    RenamerOptions options;   // plan-only（应用面同型断言见本文件尾 applySyncs 用例）

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Class, "", "A", "", "B"));
    batch.push_back(P(ProposalKind::Method, "A", "m", "(LA;)V", "n"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.proposed, 2U);
    EXPECT_EQ(result.stats.accepted, 2U);
    EXPECT_EQ(result.stats.rejected, 0U);
    EXPECT_EQ(result.stats.suffixResolved, 1U);

    // plan：m 的规则 newDesc 预映像 (LB;)V，终名走后缀 n_1，newOwner 同步类改名。
    const auto* rule = result.plan.findMethod("A", "m", "(LA;)V");
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->newName, "n_1");
    EXPECT_EQ(rule->newOwner, "B");
    EXPECT_EQ(rule->newDesc, "(LB;)V");

    // record：accepted 记终名（含 _N 后缀）与建议原文旧 desc，映射底座记旧位。
    const auto& accepted = result.record.accepted();
    ASSERT_EQ(accepted.size(), 2U);
    EXPECT_EQ(accepted[1].newName, "n_1");
    EXPECT_EQ(accepted[1].desc, "(LA;)V");
    EXPECT_EQ(result.record.mapping()
                  .getMappedMethodName(ca::str::Utf8StringRef::from_cstr("A"),
                                       ca::str::Utf8StringRef::from_cstr("m"),
                                       ca::str::Utf8StringRef::from_cstr("(LA;)V"))
                  .to_std_string(),
              "n_1");

    // mapped-desc 查撞：m 在 mapped-desc 空间 (B, ·, (LB;)V) 命中既有 n 槽后落
    // n_1（终名带后缀），既有 n 槽本身无规则；终态空间恰好一条规则落该槽。
    ca::usize rulesOnExistingNSlot = 0;
    for (const auto& methodRule : result.plan.methodRules()) {
        if (methodRule.newOwner == "B" && methodRule.newName == "n_1" &&
            methodRule.newDesc == "(LB;)V") {
            ++rulesOnExistingNSlot;
        }
    }
    EXPECT_EQ(rulesOnExistingNSlot, 1U);
}

TEST(MjRenamerConflictTest, sameComponentDualProposalsAdjudicatedAsOne)
{
    // interface I.oldMethod→foo(0.5) 与 Impl.oldMethod→bar(0.9)：同 override 组件、
    // 不同 owner/终名，3a/3b 双键裁决均拦不住 → step 4 前按组件根去重再裁决，
    // 高 confidence 者整组件生效，败者记 Conflict 留 ledger，record 只有一条建议胜出。
    model::ResourceContainer container;
    container.addResource("java/lang/Object.class",
                          std::make_unique<model::ClassResource>("java/lang/Object.class",
                                                                 fixture::buildBareObject()));
    container.addResource(
        "I.class", std::make_unique<model::ClassResource>("I.class", fixture::buildInterfaceI()));
    container.addResource("Impl.class",
                          std::make_unique<model::ClassResource>("Impl.class",
                                                                 fixture::buildImpl()));

    RenamerEngine  engine;
    RenamerOptions options;

    FixedProvider first(
        "ifc", batchOf(P(ProposalKind::Method, "I", "oldMethod", "()V", "foo", 0.5, "ifc")));
    FixedProvider second(
        "impl", batchOf(P(ProposalKind::Method, "Impl", "oldMethod", "()V", "bar", 0.9, "impl")));

    auto       providers = std::vector<NameProvider*>{&first, &second};
    const auto result    = engine.run(container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.proposed, 2U);
    EXPECT_EQ(result.stats.accepted, 1U);
    EXPECT_EQ(result.stats.rejected, 1U);
    EXPECT_EQ(result.stats.renamedMembers, 2U);   // bar 落全组件 I+Impl
    const auto* rejected = findRejected(result, RejectionReason::Conflict);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->proposal.source, "ifc");
    EXPECT_EQ(rejected->proposal.newName, "foo");
    ASSERT_NE(result.plan.findMethod("I", "oldMethod", "()V"), nullptr);
    ASSERT_NE(result.plan.findMethod("Impl", "oldMethod", "()V"), nullptr);
    EXPECT_EQ(result.plan.findMethod("I", "oldMethod", "()V")->newName, "bar");
    EXPECT_EQ(result.plan.findMethod("Impl", "oldMethod", "()V")->newName, "bar");
    const auto& accepted = result.record.accepted();
    ASSERT_EQ(accepted.size(), 2U);
    for (const auto& entry : accepted) {
        EXPECT_EQ(entry.source, "impl");   // 两条 accepted 均溯源到赢家
        EXPECT_EQ(entry.newName, "bar");   // 终名一致，无双规则
    }
    EXPECT_EQ(accepted[1].fromClosure, true);
}

TEST(MjRenamerConflictTest, freedOldClassNameNeedsNoSuffix)
{
    // A 改走 RenamedA 后旧名 A 让位；C→A 直接占用，不加后缀（占用表让位口径）。
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Class, "", "A", "", "RenamedA"));
    batch.push_back(P(ProposalKind::Class, "", "C", "", "A"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.accepted, 2U);
    EXPECT_EQ(result.stats.suffixResolved, 0U);   // 旧名让位，无需后缀
    EXPECT_EQ(result.plan.mapInternalName("A"), "RenamedA");
    EXPECT_EQ(result.plan.mapInternalName("C"), "A");
}

TEST(MjRenamerConflictTest, packageRenameExpandsToClassesAndRecordsPackageMapping)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    // 单类包前缀展开（C 无子类），Object/A/B 不受影响。
    FixedProvider provider("p", batchOf(P(ProposalKind::Package, "", "C", "", "moved")));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.plan.mapInternalName("C"), "moved");
    EXPECT_EQ(result.plan.mapInternalName("A"), "A");
    ASSERT_NE(result.record.mapping().getPackageMapping(ca::str::Utf8StringRef::from_cstr("C")),
              nullptr);
}

TEST(MjRenamerConflictTest, packageRenameCollidingWithClaimedClassRenameRejectedAtomically)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    // 先到类建议 A → moved；后到包建议 C → moved 展开 C→moved 撞已占目标 → 整包拒。
    std::vector<NameProposal> classBatch;
    classBatch.push_back(P(ProposalKind::Class, "", "A", "", "moved"));
    FixedProvider classProvider("cls", std::move(classBatch));
    FixedProvider packageProvider("pkg", batchOf(P(ProposalKind::Package, "", "C", "", "moved")));

    auto       providers = std::vector<NameProvider*>{&classProvider, &packageProvider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.plan.mapInternalName("A"), "moved");
    EXPECT_EQ(result.plan.mapInternalName("C"), "C");   // 整包未动
    const auto* rejected = findRejected(result, RejectionReason::Conflict);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->proposal.kind, ProposalKind::Package);
}

// ── 记录与导出 ──

TEST(MjRenamerRecordTest, recordExportsTinyV2RoundtripIdentity)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Class, "", "C", "", "moved/C"));
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "newField"));
    batch.push_back(P(ProposalKind::Method, "A", "alpha", "()V", "beta"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok);

    mapping::MappingFormatRegistry registry = mapping::MappingFormatRegistry::withStandardFormats();
    auto                           exported = result.record.exportMapping("Tiny-V2", registry);
    ASSERT_TRUE(exported.is_ok());

    const auto   text     = std::move(exported).unwrap();
    auto         parsedRe = registry.parse(ca::str::Utf8StringRef::from_cstr("Tiny-V2"),
                                       text.ref());
    ASSERT_TRUE(parsedRe.is_ok());
    auto reparsed = std::move(parsedRe).unwrap();

    // 往返恒等：类/字段/方法映射语义一致
    ASSERT_NE(reparsed.getClassMapping(ca::str::Utf8StringRef::from_cstr("C")), nullptr);
    EXPECT_EQ(reparsed.getClassMapping(ca::str::Utf8StringRef::from_cstr("C"))->newName
                  .to_std_string(),
              "moved/C");
    EXPECT_EQ(reparsed
                  .getMappedFieldName(ca::str::Utf8StringRef::from_cstr("A"),
                                      ca::str::Utf8StringRef::from_cstr("oldField"),
                                      ca::str::Utf8StringRef::from_cstr("I"))
                  .to_std_string(),
              "newField");
    EXPECT_EQ(reparsed
                  .getMappedMethodName(ca::str::Utf8StringRef::from_cstr("A"),
                                       ca::str::Utf8StringRef::from_cstr("alpha"),
                                       ca::str::Utf8StringRef::from_cstr("()V"))
                  .to_std_string(),
              "beta");
    // 原记录同查询
    EXPECT_EQ(result.record.mapping()
                  .getMappedFieldName(ca::str::Utf8StringRef::from_cstr("A"),
                                      ca::str::Utf8StringRef::from_cstr("oldField"),
                                      ca::str::Utf8StringRef::from_cstr("I"))
                  .to_std_string(),
              "newField");
}

TEST(MjRenamerRecordTest, tinyV2RoundtripIsByteIdentical)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Class, "", "C", "", "moved/C"));
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "newField"));
    batch.push_back(P(ProposalKind::Method, "A", "alpha", "()V", "beta"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok);

    mapping::MappingFormatRegistry registry = mapping::MappingFormatRegistry::withStandardFormats();
    auto                           exported = result.record.exportMapping("Tiny-V2", registry);
    ASSERT_TRUE(exported.is_ok());
    const auto firstText = std::move(exported).unwrap();
    EXPECT_FALSE(firstText.is_empty());

    // 读回 → 再导出 → 字节恒等（#1032 Q5 字面口径：导出即稳定规范形）。
    auto reparsed = registry.parse(ca::str::Utf8StringRef::from_cstr("Tiny-V2"), firstText.ref());
    ASSERT_TRUE(reparsed.is_ok());
    auto formatted = registry.format(ca::str::Utf8StringRef::from_cstr("Tiny-V2"),
                                     std::move(reparsed).unwrap());
    ASSERT_TRUE(formatted.is_ok());
    EXPECT_TRUE(std::move(formatted).unwrap().equals(firstText.ref()));
}

TEST(MjRenamerRecordTest, staticExportMappingReusesSameChannelForExternalBases)
{
    // P1 消费方通道：任意外部 IntermediateMapping 底座（如 deobf 侧
    // NameRestoreRecord）经静态 exportMapping 导出，与实例方法逐字节一致
    // （luiox/morpher#1032 P1）。
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Class, "", "C", "", "moved/C"));
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "newField"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok);

    mapping::MappingFormatRegistry registry = mapping::MappingFormatRegistry::withStandardFormats();

    auto staticExported =
        NameChangeRecord::exportMapping(result.record.mapping(), "Tiny-V2", registry);
    ASSERT_TRUE(staticExported.is_ok());
    auto instanceExported = result.record.exportMapping("Tiny-V2", registry);
    ASSERT_TRUE(instanceExported.is_ok());
    EXPECT_TRUE(
        std::move(instanceExported).unwrap().equals(std::move(staticExported).unwrap().ref()));

    // 未注册格式同口径报错。
    auto unknown = NameChangeRecord::exportMapping(result.record.mapping(), "Nope-V1", registry);
    EXPECT_TRUE(unknown.is_err());
}

TEST(MjRenamerRecordTest, unknownExportFormatFails)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;
    FixedProvider provider("p", batchOf(P(ProposalKind::Field, "A", "oldField", "I", "renamed")));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok);

    mapping::MappingFormatRegistry registry = mapping::MappingFormatRegistry::withStandardFormats();
    auto                           exported = result.record.exportMapping("Nope-V1", registry);
    EXPECT_TRUE(exported.is_err());
}

TEST(MjRenamerRecordTest, rejectedLedgerKeepsProposalProvenance)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "alpha", "()V", "class", 0.7, "ai:demo"));
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "newField", 0.9, "ai:demo"));
    FixedProvider provider("ai:demo", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.proposed, 2U);
    EXPECT_EQ(result.stats.accepted, 1U);
    const auto& rejected = result.record.rejected();
    ASSERT_EQ(rejected.size(), 1U);
    EXPECT_EQ(rejected.front().reason, RejectionReason::Keyword);
    EXPECT_EQ(rejected.front().proposal.confidence, 0.7);
    EXPECT_EQ(rejected.front().proposal.source, "ai:demo");
    EXPECT_EQ(rejectionReasonName(RejectionReason::Keyword), "keyword");
}

TEST(MjRenamerRecordTest, providerErrorSkippedAndOthersContinue)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;

    FailingProvider broken("broken");
    FixedProvider   healthy("healthy",
                            batchOf(P(ProposalKind::Field, "A", "oldField", "I", "renamed")));

    auto       providers = std::vector<NameProvider*>{&broken, &healthy};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.proposed, 1U);   // 失败 provider 的建议流不计入 proposed
    EXPECT_EQ(result.stats.accepted, 1U);
    const auto* failure = findRejected(result, RejectionReason::ProviderError);
    ASSERT_NE(failure, nullptr);
    EXPECT_NE(failure->detail.find("broken"), std::string::npos);
    EXPECT_NE(result.plan.findField("A", "oldField", "I"), nullptr);
}

// ── run 两态（plan-only 面；应用面落盘断言见本文件尾三件 apply 用例）──

TEST(MjRenamerRunTest, planOnlyLeavesContainerUntouched)
{
    Fixture        fixture;
    RenamerEngine  engine;

    RenamerOptions            options;   // planOnly 默认 true
    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "oldMethod", "()V", "renamedMethod"));
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "newField"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.applied);
    EXPECT_FALSE(result.plan.empty());
    expectContainerUntouched(fixture.container);
}

TEST(MjRenamerRunTest, planModeProducesClosureRulesAndLeavesContainerUntouched)
{
    // 旧栈 applyModeRewritesBytecodeAndClosure 的 plan 面移植：A/B 闭包整组 +
    // 字段规则齐备；容器字节零写入（应用面落盘断言见
    // applyModeWritesClosureIntoBytecode）。
    Fixture       fixture;
    RenamerEngine engine;

    RenamerOptions options;
    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "oldMethod", "()V", "renamedMethod"));
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "newField"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.applied);

    const auto* aMethod = result.plan.findMethod("A", "oldMethod", "()V");
    const auto* bMethod = result.plan.findMethod("B", "oldMethod", "()V");
    ASSERT_NE(aMethod, nullptr);
    ASSERT_NE(bMethod, nullptr);
    EXPECT_EQ(aMethod->newName, "renamedMethod");   // 闭包整组
    EXPECT_EQ(bMethod->newName, "renamedMethod");
    const auto* field = result.plan.findField("A", "oldField", "I");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->newName, "newField");
    expectContainerUntouched(fixture.container);
}

TEST(MjRenamerRunTest, planRecordsFieldRenameForCallSiteSync)
{
    // 旧栈 applyRewritesFieldReadInsnToNewName 的 plan 面移植：record/plan 携带
    // 调用点同步所需的精确三段（GETFIELD A.oldField I → newField 的改写由
    // remapper 批按此消费；应用面字节码断言见
    // applySyncsMemberRuleOwnerAndDescWhenOwnerClassRenamed）。
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;
    FixedProvider provider("p", batchOf(P(ProposalKind::Field, "A", "oldField", "I", "newField")));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    const auto* rule = result.plan.findField("A", "oldField", "I");
    ASSERT_NE(rule, nullptr);
    EXPECT_EQ(rule->newOwner, "A");
    EXPECT_EQ(rule->newName, "newField");
    EXPECT_EQ(rule->newDesc, "I");
    EXPECT_EQ(result.record.mapping()
                  .getMappedFieldName(ca::str::Utf8StringRef::from_cstr("A"),
                                      ca::str::Utf8StringRef::from_cstr("oldField"),
                                      ca::str::Utf8StringRef::from_cstr("I"))
                  .to_std_string(),
              "newField");
}

TEST(MjRenamerRunTest, planPreImagesMemberRuleOwnerAndDescWhenOwnerClassRenamed)
{
    // P1 实证回归（luiox/morpher#1032）plan 面移植：成员改名与 owner 类改名同
    // plan 时，成员规则的 newOwner/newDesc 必须按类映射预映像——否则 remapper 的
    // 调用点同步路径会把 owner/描述符写回旧值。desc 分支用含被改名类的类型
    // （`LA;` → `Lmoved/A;`）锁定 mapFieldDescriptor/mapMethodDescriptor 预映像。
    std::string members;
    members += fixture::fieldDecl("public", "oldField", "LA;");
    members += fixture::voidMethod("public", "<init>", "()V");
    members += fixture::voidMethod("public static", "make", "()LA;");
    model::ResourceContainer container;
    container.addResource(
        "A.class", std::make_unique<model::ClassResource>(
                       "A.class", fixture::assembleClass("A", "java/lang/Object", members)));

    RenamerEngine  engine;
    RenamerOptions options;
    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Class, "", "A", "", "moved/A"));
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "LA;", "newField"));
    batch.push_back(P(ProposalKind::Method, "A", "make", "()LA;", "create"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(container, providers, options);
    ASSERT_TRUE(result.ok);

    // 成员规则预映像：newOwner/newDesc 均按类映射改写，不残留旧名。
    const auto* field = result.plan.findField("A", "oldField", "LA;");
    ASSERT_NE(field, nullptr);
    EXPECT_EQ(field->newOwner, "moved/A");
    EXPECT_EQ(field->newName, "newField");
    EXPECT_EQ(field->newDesc, "Lmoved/A;");
    const auto* method = result.plan.findMethod("A", "make", "()LA;");
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->newOwner, "moved/A");
    EXPECT_EQ(method->newName, "create");
    EXPECT_EQ(method->newDesc, "()Lmoved/A;");

    // 描述符预映像查询：数组与未命中段恒等口径由实现锁死，此处锁类类型段。
    EXPECT_EQ(result.plan.mapFieldDescriptor("LA;"), "Lmoved/A;");
    EXPECT_EQ(result.plan.mapMethodDescriptor("()LA;"), "()Lmoved/A;");
    EXPECT_EQ(result.plan.mapInternalName("A"), "moved/A");
    expectContainerUntouched(container);
}

TEST(MjRenamerRunTest, contextExposesReadOnlyHierarchyQueries)
{
    Fixture fixture;
    auto    context = RenameContext::build(fixture.container);
    ASSERT_TRUE(context.is_ok());
    auto view = std::move(context).unwrap();

    EXPECT_TRUE(view->classExists("A"));
    EXPECT_TRUE(view->classExists("B"));
    EXPECT_FALSE(view->classExists("ghost/Phantom"));
    EXPECT_TRUE(view->isSubType("B", "A"));
    EXPECT_FALSE(view->isSubType("A", "B"));
    EXPECT_EQ(view->superClassOf("B"), "A");
    EXPECT_TRUE(view->superClassOf("java/lang/Object").empty());

    auto component = view->overrideComponentOf("A", "oldMethod", "()V");
    ASSERT_TRUE(component.is_ok());
    const auto members = std::move(component).unwrap();
    ASSERT_EQ(members.size(), 2U);   // A + B 闭包组件
    EXPECT_EQ(members[0].owner, "A");
    EXPECT_EQ(members[0].name, "oldMethod");

    const auto missing = view->overrideComponentOf("A", "nope", "()V");
    EXPECT_TRUE(missing.is_err());
}

// ── 继承别名（luiox/morpher#1032 P2：obf rename_core 需要的应用面闭包）──

TEST(MjRenamerAliasTest, fieldAliasRulesCoverInheritingSubclassesPlanOnly)
{
    model::ResourceContainer container;
    container.addResource(
        "A.class", std::make_unique<model::ClassResource>("A.class", fixture::buildA()));
    // B：继承 A.oldField（不声明）→ 应得别名规则。
    container.addResource(
        "B.class", std::make_unique<model::ClassResource>("B.class", fixture::buildB()));
    // D：同 B → 别名规则。
    container.addResource(
        "D.class",
        std::make_unique<model::ClassResource>("D.class", fixture::buildInheritor("D", false)));
    container.addResource("java/lang/Object.class",
                          std::make_unique<model::ClassResource>("java/lang/Object.class",
                                                                 fixture::buildBareObject()));

    FixedProvider  provider("t", batchOf(P(ProposalKind::Field, "A", "oldField", "I", "newField")));
    RenamerOptions options;
    options.planOnly = true;
    RenamerEngine engine;
    auto          result = engine.run(container, {&provider}, options);
    ASSERT_TRUE(result.ok);

    // 声明处规则照常。
    const auto* declared = result.plan.findField("A", "oldField", "I");
    ASSERT_NE(declared, nullptr);
    EXPECT_EQ(declared->newName, "newField");
    // 继承子类得到 (B, oldField) / (D, oldField) 别名规则（plan-only）。
    const auto* aliasB = result.plan.findField("B", "oldField", "I");
    ASSERT_NE(aliasB, nullptr);
    EXPECT_EQ(aliasB->newName, "newField");
    EXPECT_EQ(aliasB->newOwner, "B");
    const auto* aliasD = result.plan.findField("D", "oldField", "I");
    ASSERT_NE(aliasD, nullptr);
    EXPECT_EQ(aliasD->newName, "newField");
    // 记录/SRG 底座只含声明处一条（别名不入导出，与旧核心口径一致）。
    EXPECT_TRUE(result.record.mapping()
                    .getMappedFieldName(ca::str::Utf8StringRef::from_cstr("B"),
                                        ca::str::Utf8StringRef::from_cstr("oldField"),
                                        ca::str::Utf8StringRef::from_cstr("I"))
                    .is_empty());
    EXPECT_TRUE(result.record.mapping()
                    .getMappedFieldName(ca::str::Utf8StringRef::from_cstr("D"),
                                        ca::str::Utf8StringRef::from_cstr("oldField"),
                                        ca::str::Utf8StringRef::from_cstr("I"))
                    .is_empty());
    EXPECT_FALSE(result.record.mapping()
                     .getMappedFieldName(ca::str::Utf8StringRef::from_cstr("A"),
                                         ca::str::Utf8StringRef::from_cstr("oldField"),
                                         ca::str::Utf8StringRef::from_cstr("I"))
                     .is_empty());
}

TEST(MjRenamerAliasTest, fieldAliasSkipsShadowingSubclass)
{
    model::ResourceContainer container;
    container.addResource(
        "A.class", std::make_unique<model::ClassResource>("A.class", fixture::buildA()));
    // S：自带 (oldField, I) 声明遮蔽 A 的同名字段——S 自己的声明不随 A 改名，
    //    对 S 也不得补别名（否则 S 的遮蔽字段引用会被错改）。
    std::string members;
    members += fixture::fieldDecl("public", "oldField", "I");
    members += fixture::voidMethod("public", "<init>", "()V");
    container.addResource(
        "S.class", std::make_unique<model::ClassResource>(
                       "S.class", fixture::assembleClass("S", "A", members)));
    container.addResource("java/lang/Object.class",
                          std::make_unique<model::ClassResource>("java/lang/Object.class",
                                                                 fixture::buildBareObject()));

    FixedProvider  provider("t", batchOf(P(ProposalKind::Field, "A", "oldField", "I", "newField")));
    RenamerOptions options;
    options.planOnly = true;
    RenamerEngine engine;
    auto          result = engine.run(container, {&provider}, options);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.plan.findField("S", "oldField", "I"), nullptr);
}

TEST(MjRenamerAliasTest, methodAliasRulesCoverClosedWorldInheritingSubclasses)
{
    model::ResourceContainer container;
    container.addResource(
        "A.class", std::make_unique<model::ClassResource>("A.class", fixture::buildA()));
    container.addResource(
        "B.class", std::make_unique<model::ClassResource>("B.class", fixture::buildB()));
    // D：继承 oldMethod 不声明、闭包完整 → 别名。
    container.addResource(
        "D.class",
        std::make_unique<model::ClassResource>("D.class", fixture::buildInheritor("D", false)));
    // E：同样继承但 implements 容器外接口 → 闭包破洞，不补别名（保守）。
    container.addResource(
        "E.class",
        std::make_unique<model::ClassResource>("E.class", fixture::buildInheritor("E", true)));
    container.addResource("java/lang/Object.class",
                          std::make_unique<model::ClassResource>("java/lang/Object.class",
                                                                 fixture::buildBareObject()));

    FixedProvider  provider(
        "t", batchOf(P(ProposalKind::Method, "A", "oldMethod", "()V", "brandNew")));
    RenamerOptions options;
    options.planOnly = true;
    RenamerEngine engine;
    auto          result = engine.run(container, {&provider}, options);
    ASSERT_TRUE(result.ok);

    // 闭包规则：声明处 A + 声明了的 B。
    ASSERT_NE(result.plan.findMethod("A", "oldMethod", "()V"), nullptr);
    ASSERT_NE(result.plan.findMethod("B", "oldMethod", "()V"), nullptr);
    // 继承别名：D 命中，E（闭包破洞）不命中。
    const auto* aliasD = result.plan.findMethod("D", "oldMethod", "()V");
    ASSERT_NE(aliasD, nullptr);
    EXPECT_EQ(aliasD->newName, "brandNew");
    EXPECT_EQ(aliasD->newOwner, "D");
    EXPECT_EQ(aliasD->newDesc, "()V");
    EXPECT_EQ(result.plan.findMethod("E", "oldMethod", "()V"), nullptr);
    // 记录只含声明处（A/B owner 行 + 方法两条），别名不入导出。
    EXPECT_TRUE(result.record.mapping()
                    .getMappedMethodName(ca::str::Utf8StringRef::from_cstr("D"),
                                         ca::str::Utf8StringRef::from_cstr("oldMethod"),
                                         ca::str::Utf8StringRef::from_cstr("()V"))
                    .is_empty());
    EXPECT_TRUE(result.record.mapping()
                    .getMappedMethodName(ca::str::Utf8StringRef::from_cstr("E"),
                                         ca::str::Utf8StringRef::from_cstr("oldMethod"),
                                         ca::str::Utf8StringRef::from_cstr("()V"))
                    .is_empty());
    EXPECT_FALSE(result.record.mapping()
                     .getMappedMethodName(ca::str::Utf8StringRef::from_cstr("A"),
                                          ca::str::Utf8StringRef::from_cstr("oldMethod"),
                                          ca::str::Utf8StringRef::from_cstr("()V"))
                     .is_empty());
    EXPECT_FALSE(result.record.mapping()
                     .getMappedMethodName(ca::str::Utf8StringRef::from_cstr("B"),
                                          ca::str::Utf8StringRef::from_cstr("oldMethod"),
                                          ca::str::Utf8StringRef::from_cstr("()V"))
                     .is_empty());
}

namespace {

/// A + I + C(+Caller) 双组件继承世界：C 同时继承 A.oldMethod 与 I.oldMethod。
void addDualComponentWorld(model::ResourceContainer& container, bool withCaller)
{
    container.addResource(
        "A.class", std::make_unique<model::ClassResource>("A.class", fixture::buildA()));
    container.addResource(
        "I.class", std::make_unique<model::ClassResource>("I.class", fixture::buildInterfaceI()));
    container.addResource("C.class",
                          std::make_unique<model::ClassResource>(
                              "C.class", fixture::buildDualInheritorC()));
    if (withCaller) {
        // Caller.go()V：NEW C 后 INVOKEVIRTUAL C.oldMethod ()V（歧义继承点上的调用）。
        const std::string callerSource =
            ".super java/lang/Object\n"
            ".class public super Caller {\n"
            "  .method public go ()V {\n"
            "    code: {\n"
            "      new C\n"
            "      dup\n"
            "      invokespecial C.<init> ()V\n"
            "      invokevirtual C.oldMethod ()V\n"
            "      return\n"
            "    }\n"
            "  }\n"
            "}\n";
        container.addResource("Caller.class",
                              std::make_unique<model::ClassResource>(
                                  "Caller.class", fixture::assembleJasm(callerSource)));
    }
    container.addResource("java/lang/Object.class",
                          std::make_unique<model::ClassResource>("java/lang/Object.class",
                                                                 fixture::buildBareObject()));
}

}   // namespace

TEST(MjRenamerAliasTest, methodAliasAmbiguityDropsWholeRuleWhenInheritorsDisagree)
{
    // N5（luiox/morpher#1032 评审收口）：同 (owner=C, oldName, desc) 键被两个
    // override 组件组分配到不同新名（A 组 → brandA，I 组 → brandI）时，C 的
    // 继承别名无法二选一——整条放弃（保守口径，与旧核心一致），不得产出
    // 半边别名把另一组件的调用点错改。
    model::ResourceContainer container;
    addDualComponentWorld(container, /*withCaller=*/false);

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "oldMethod", "()V", "brandA"));
    batch.push_back(P(ProposalKind::Method, "I", "oldMethod", "()V", "brandI"));
    FixedProvider  provider("t", std::move(batch));
    RenamerOptions options;
    options.planOnly = true;
    RenamerEngine engine;
    auto          result = engine.run(container, {&provider}, options);
    ASSERT_TRUE(result.ok);

    // 声明处两组件规则照常（A、I 各自整组改名，互不影响）。
    const auto* ruleA = result.plan.findMethod("A", "oldMethod", "()V");
    ASSERT_NE(ruleA, nullptr);
    EXPECT_EQ(ruleA->newName, "brandA");
    const auto* ruleI = result.plan.findMethod("I", "oldMethod", "()V");
    ASSERT_NE(ruleI, nullptr);
    EXPECT_EQ(ruleI->newName, "brandI");
    // 歧义继承点：别名整条不产生。
    EXPECT_EQ(result.plan.findMethod("C", "oldMethod", "()V"), nullptr);
    // 记录（SRG 导出底座）不含 C 条目——别名不入导出，放弃也不留半边。
    EXPECT_TRUE(result.record.mapping()
                    .getMappedMethodName(ca::str::Utf8StringRef::from_cstr("C"),
                                         ca::str::Utf8StringRef::from_cstr("oldMethod"),
                                         ca::str::Utf8StringRef::from_cstr("()V"))
                    .is_empty());
}

TEST(MjRenamerAliasTest, methodAliasAmbiguityAppliesWithoutAliasRuleAndKeepsStaleCallsite)
{
    // N5 对照（应用面接线后真语义）：歧义放弃后计划仍非空——planOnly=false 走
    // 真应用（旧占位 fail-loud 语义随接线废止）。声明处两组件照常整组改名；
    // 歧义键无规则 → C 无别名规则，Caller 继承点上的 INVOKEVIRTUAL C.oldMethod
    // 保守不随动（歧义键无从改错，旧核心同口径）。
    model::ResourceContainer container;
    addDualComponentWorld(container, /*withCaller=*/true);

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "oldMethod", "()V", "brandA"));
    batch.push_back(P(ProposalKind::Method, "I", "oldMethod", "()V", "brandI"));
    FixedProvider  provider("t", std::move(batch));
    RenamerOptions options;
    options.planOnly = false;
    RenamerEngine engine;
    auto          result = engine.run(container, {&provider}, options);
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.applied);
    ASSERT_EQ(findRejected(result, RejectionReason::ApplyFailed), nullptr);
    // 歧义键照旧整条放弃：plan 无 C 规则。
    EXPECT_EQ(result.plan.findMethod("C", "oldMethod", "()V"), nullptr);

    // 字节码面：A/I 声明处换新名（A 声明 brandA、I 声明 brandI）。
    EXPECT_TRUE(contains(declaredMethodNamesOf(container, "A.class"), "brandA"));
    EXPECT_FALSE(contains(declaredMethodNamesOf(container, "A.class"), "oldMethod"));
    EXPECT_TRUE(contains(declaredMethodNamesOf(container, "I.class"), "brandI"));
    EXPECT_FALSE(contains(declaredMethodNamesOf(container, "I.class"), "oldMethod"));
    // 调用点面：Caller 的 C 继承引用无规则可依，原样保守保留。
    const auto callerRefs = memberRefsOf(container, "Caller.class");
    EXPECT_TRUE(hasMemberRef(callerRefs, "C", "oldMethod"));
    EXPECT_FALSE(hasMemberRef(callerRefs, "C", "brandA"));
    EXPECT_FALSE(hasMemberRef(callerRefs, "C", "brandI"));
}

TEST(MjRenamerRunTest, applyModeWritesClosureIntoBytecode)
{
    // 旧栈 applyModeRewritesBytecodeAndClosure 的应用面移植：闭包整组落盘——
    // A 与 B 声明槽都读到新名，旧名槽不复存在（容器零写入的 plan 面对照见
    // planModeProducesClosureRulesAndLeavesContainerUntouched）。
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;
    options.planOnly = false;
    options.policy   = mjbase::asm_remapper::RemapPolicy::bytecodeOnly();
    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "oldMethod", "()V", "renamedMethod"));
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "I", "newField"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(fixture.container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.applied);
    const auto aMethods = declaredMethodNamesOf(fixture.container, "A.class");
    EXPECT_TRUE(contains(aMethods, "renamedMethod"));
    EXPECT_FALSE(contains(aMethods, "oldMethod"));
    const auto bMethods = declaredMethodNamesOf(fixture.container, "B.class");
    EXPECT_TRUE(contains(bMethods, "renamedMethod"));   // 闭包整组落盘
    EXPECT_FALSE(contains(bMethods, "oldMethod"));
}

TEST(MjRenamerRunTest, applyRewritesFieldReadInsnToNewName)
{
    // 旧栈 applyRewritesFieldReadInsnToNewName 的应用面移植：A.alpha 体里的
    // GETFIELD A.oldField I 必须同步指向新名（规则精确三段命中的调用点改写）。
    const std::string src =
        ".super java/lang/Object\n"
        ".class public super A {\n"
        "  .field public oldField I\n"
        "  .method public <init> ()V {\n"
        "    code: {\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public alpha ()V {\n"
        "    code: {\n"
        "      aload 0\n"
        "      getfield A.oldField I\n"
        "      pop\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "}\n";
    model::ResourceContainer container;
    container.addResource(
        "A.class", std::make_unique<model::ClassResource>(
                       "A.class", fixture::assembleJasm(src)));

    RenamerEngine  engine;
    RenamerOptions options;
    options.planOnly = false;
    options.policy   = mjbase::asm_remapper::RemapPolicy::bytecodeOnly();
    FixedProvider provider("p",
                           batchOf(P(ProposalKind::Field, "A", "oldField", "I", "newField")));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(container, providers, options);

    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.applied);
    const auto refs = memberRefsOf(container, "A.class");
    EXPECT_TRUE(hasMemberRef(refs, "A", "newField", "I"));
    EXPECT_FALSE(hasMemberRef(refs, "A", "oldField", "I"));
}

TEST(MjRenamerRunTest, applySyncsMemberRuleOwnerAndDescWhenOwnerClassRenamed)
{
    // P1 实证回归（luiox/morpher#1032）应用面移植：成员改名与 owner 类改名同
    // plan 时，调用点同步必须落到预映像后的 owner/描述符（`LA;` →
    // `Lmoved/A;`），不残留旧名；类资源 URI 随 resources 门同步（deobf 消费
    // 口径；mjbase 双门下 resources 显式开启）。
    const std::string srcA =
        ".super java/lang/Object\n"
        ".class public super A {\n"
        "  .field public oldField LA;\n"
        "  .method public <init> ()V {\n"
        "    code: {\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static make ()LA; {\n"
        "    code: {\n"
        "      aconst_null\n"
        "      areturn\n"
        "    }\n"
        "  }\n"
        "}\n";
    const std::string srcCaller =
        ".super java/lang/Object\n"
        ".class public super Caller {\n"
        "  .method public <init> ()V {\n"
        "    code: {\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public go ()V {\n"
        "    code: {\n"
        "      getstatic A.oldField LA;\n"
        "      invokestatic A.make ()LA;\n"
        "      pop\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "}\n";
    model::ResourceContainer container;
    container.addResource(
        "A.class", std::make_unique<model::ClassResource>(
                       "A.class", fixture::assembleJasm(srcA)));
    container.addResource(
        "Caller.class", std::make_unique<model::ClassResource>(
                            "Caller.class", fixture::assembleJasm(srcCaller)));

    RenamerEngine  engine;
    RenamerOptions options;
    options.planOnly          = false;
    options.policy            = mjbase::asm_remapper::RemapPolicy::bytecodeOnly();
    options.policy.resources  = true;   // 类改名同步 .class URI（deobf 消费口径）。
    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Class, "", "A", "", "moved/A"));
    batch.push_back(P(ProposalKind::Field, "A", "oldField", "LA;", "newField"));
    batch.push_back(P(ProposalKind::Method, "A", "make", "()LA;", "create"));
    FixedProvider provider("p", std::move(batch));

    auto       providers = std::vector<NameProvider*>{&provider};
    const auto result    = engine.run(container, providers, options);
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.applied);
    EXPECT_NE(container.getResource("moved/A.class"), nullptr);
    EXPECT_EQ(container.getResource("A.class"), nullptr);

    // Caller 的 GETSTATIC/INVOKESTATIC 必须同步到 moved/A：owner、成员名与
    // 完整描述符（desc 含被改名类的类型引用，预映像后不得残留旧名）。
    const auto refs = memberRefsOf(container, "Caller.class");
    EXPECT_TRUE(hasMemberRef(refs, "moved/A", "newField", "Lmoved/A;"));
    EXPECT_TRUE(hasMemberRef(refs, "moved/A", "create", "()Lmoved/A;"));
    EXPECT_FALSE(hasMemberRef(refs, "A", "oldField"));
    EXPECT_FALSE(hasMemberRef(refs, "A", "make"));
}

TEST(MjRenamerAliasTest, methodAliasKeepsRuleWhenBothComponentsAgreeOnNewName)
{
    // N5 对照（非歧义）：两组件分配同一新名时键不冲突，C 的继承别名照常产生。
    model::ResourceContainer container;
    addDualComponentWorld(container, /*withCaller=*/false);

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "A", "oldMethod", "()V", "brandNew"));
    batch.push_back(P(ProposalKind::Method, "I", "oldMethod", "()V", "brandNew"));
    FixedProvider  provider("t", std::move(batch));
    RenamerOptions options;
    options.planOnly = true;
    RenamerEngine engine;
    auto          result = engine.run(container, {&provider}, options);
    ASSERT_TRUE(result.ok);
    ASSERT_NE(result.plan.findMethod("A", "oldMethod", "()V"), nullptr);
    ASSERT_NE(result.plan.findMethod("I", "oldMethod", "()V"), nullptr);
    const auto* aliasC = result.plan.findMethod("C", "oldMethod", "()V");
    ASSERT_NE(aliasC, nullptr);
    EXPECT_EQ(aliasC->newName, "brandNew");
    EXPECT_EQ(aliasC->newOwner, "C");
    EXPECT_TRUE(result.record.mapping()
                    .getMappedMethodName(ca::str::Utf8StringRef::from_cstr("C"),
                                         ca::str::Utf8StringRef::from_cstr("oldMethod"),
                                         ca::str::Utf8StringRef::from_cstr("()V"))
                    .is_empty());
}

}   // namespace mjbase::renamer
