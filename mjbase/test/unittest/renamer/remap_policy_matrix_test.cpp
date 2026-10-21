/// @file        remap_policy_matrix_test.cpp
/// @brief       renamer 应用面矩阵。
///
/// 范围：options 两态（plan-only 门）、零建议/全拒后的 zero-applied 成功口径、
/// 非空计划的应用面真语义（经 asm/remapper 域就地应用），以及「建议流经
/// NameProvider 进入、plan/record 由引擎构造」的合同。RemapPolicy 五开关面矩阵（bytecode/reflectionStrings/annotations/
/// signatures/resources/authorizedTextResourceUris）在 remapper 自身测试锁死。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/renamer/name_proposal.hpp"
#include "mjbase/renamer/name_provider.hpp"
#include "mjbase/renamer/rename_engine.hpp"

#include "renamer_fixture.hpp"

namespace mjbase::renamer {

namespace {

namespace fixture = renamer::test_support;

NameProposal P(ProposalKind kind, const char* owner, const char* oldName, const char* desc,
               const char* newName, double confidence = 1.0, const char* source = "")
{
    return NameProposal(kind, owner, oldName, desc, newName, confidence, source);
}

/// 固定建议流的测试 provider（与 renamer_engine_test 同款形态，本文件自持）。
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

/// 主类 Old：字段 + 方法——应用面消费 plan 的最小成员面。
std::vector<ca::u8> buildOld()
{
    std::string members;
    members += fixture::fieldDecl("public", "oldField", "I");
    members += fixture::voidMethod("public", "<init>", "()V");
    members += fixture::voidMethod("public", "alpha", "()V");
    members += fixture::voidMethod("public", "use", "()V");
    return fixture::assembleClass("Old", "java/lang/Object", members);
}

/// 引擎夹具：Old 成员面 + 反射/签名引用的辅助类（引用面改写属 remapper 批）。
struct Fixture
{
    model::ResourceContainer container;

    Fixture()
    {
        container.addResource("java/lang/Object.class",
                              std::make_unique<model::ClassResource>("java/lang/Object.class",
                                                                     fixture::buildBareObject()));
        container.addResource(
            "Old.class", std::make_unique<model::ClassResource>("Old.class", buildOld()));
        container.addResource("pkg/OldReflect.class",
                              std::make_unique<model::ClassResource>(
                                  "pkg/OldReflect.class",
                                  fixture::assembleClass("pkg/OldReflect", "java/lang/Object",
                                                         "")));
        container.addResource("OldGeneric.class",
                              std::make_unique<model::ClassResource>(
                                  "OldGeneric.class",
                                  fixture::assembleClass("OldGeneric", "java/lang/Object", "")));
        container.addResource("OldAnno.class",
                              std::make_unique<model::ClassResource>(
                                  "OldAnno.class",
                                  fixture::assembleClass("OldAnno", "java/lang/Object", "")));
    }
};

/// 引擎默认建议批：4 类 + 1 字段 + 1 方法，覆盖全部建议 kind。
std::vector<NameProposal> matrixProposals()
{
    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Class, "", "Old", "", "New"));
    batch.push_back(P(ProposalKind::Class, "", "pkg/OldReflect", "", "pkg/NewReflect"));
    batch.push_back(P(ProposalKind::Class, "", "OldGeneric", "", "NewGeneric"));
    batch.push_back(P(ProposalKind::Class, "", "OldAnno", "", "NewAnno"));
    batch.push_back(P(ProposalKind::Field, "Old", "oldField", "I", "newField"));
    batch.push_back(P(ProposalKind::Method, "Old", "alpha", "()V", "beta"));
    return batch;
}

/// 容器全部 class 资源字节快照，供容器零写入断言。
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
    auto       snapshotOf = [&after](const std::string& uri) -> const std::vector<ca::u8>* {
        for (const auto& [name, bytes] : after) {
            if (name == uri) return &bytes;
        }
        return nullptr;
    };
    for (const auto& [uri, bytes] : snapshotBytes(container)) {
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

/// 容器内某 class 资源经 libjbc 会话读 this_class（读回断言用）；缺失返回空。
std::string thisClassOf(model::ResourceContainer& container, const std::string& uri)
{
    const auto* res = containerClassResource(container, uri);
    if (res == nullptr) return {};
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(res->get()));
    if (sessionResult.is_err()) return {};
    return std::string(std::move(sessionResult).unwrap()->backing().thisName());
}

/// 容器内某 class 资源的字节 → 只读 ClassFile（成员槽级读回断言用）。
std::unique_ptr<jbc::core::ClassFile> parseClassOf(model::ResourceContainer& container,
                                                   const std::string&        uri)
{
    const auto* res = containerClassResource(container, uri);
    if (res == nullptr) return nullptr;
    auto r = jbc::core::readClass(jbc::core::ByteView(res->get()));
    if (r.is_err()) return nullptr;
    return std::move(r).unwrap().file;
}

/// 声明成员名集合（字段+方法；槽级读回——字节文本扫描对 remapper 面失真：
/// 只重指向不改写 Utf8 内容，旧名会以无引用条目残留池中，语义无害）。
std::vector<std::string> declaredMemberNamesOf(const jbc::core::ClassFile& cf)
{
    std::vector<std::string> names;
    for (const auto& field : cf.fields) {
        names.emplace_back(cf.cp.utf8Of(field.nameCp));
    }
    for (const auto& method : cf.methods) {
        names.emplace_back(cf.cp.utf8Of(method.nameCp));
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

// ── 默认档位：plan-only ──

TEST(MjRenamerPolicyMatrixTest, defaultOptionsArePlanOnlyAndLeaveContainerUntouched)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;   // RenamerOptions{}：planOnly 默认 true——零写入口径。
    FixedProvider    provider("matrix", matrixProposals());
    std::vector<NameProvider*> providers{&provider};

    const auto result = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.proposed, 6U);
    EXPECT_EQ(result.stats.accepted, 6U);
    EXPECT_FALSE(result.applied);
    EXPECT_FALSE(result.plan.empty());
    expectContainerUntouched(fixture.container);
}

TEST(MjRenamerPolicyMatrixTest, matrixProposalsProducePlanAndRecordForEveryKind)
{
    // 全 kind 建议批 → plan（类映射 + 字段/方法规则）与 record（映射底座 + 溯源）
    // 各就各位；消费面（含 RemapPolicy 门控）属 remapper 批。
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;
    FixedProvider    provider("matrix", matrixProposals());
    std::vector<NameProvider*> providers{&provider};

    const auto result = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.accepted, 6U);
    EXPECT_EQ(result.plan.classMappings().size(), 4U);
    EXPECT_EQ(result.plan.fieldRules().size(), 1U);
    EXPECT_EQ(result.plan.methodRules().size(), 1U);
    ASSERT_NE(result.plan.findField("Old", "oldField", "I"), nullptr);
    ASSERT_NE(result.plan.findMethod("Old", "alpha", "()V"), nullptr);
    EXPECT_EQ(result.record.mapping()
                  .getMappedClassName(ca::str::Utf8StringRef::from_cstr("Old"))
                  .to_std_string(),
              "New");
    ASSERT_EQ(result.record.accepted().size(), 6U);
    EXPECT_TRUE(result.record.rejected().empty());
    expectContainerUntouched(fixture.container);
}

// ── plan-only 与零应用：空 plan 不误判 ──

TEST(MjRenamerPolicyMatrixTest, applyModeWithEmptyPlanIsZeroAppliedSuccess)
{
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;
    options.planOnly = false;

    std::vector<NameProvider*> providers;   // 无 provider：plan 为空。
    const auto result = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok);   // 空 plan 非失败：zero-applied 成功口径。
    EXPECT_TRUE(result.applied);
    EXPECT_TRUE(result.plan.empty());
    EXPECT_TRUE(result.record.rejected().empty());   // 不误判 ApplyFailed。
    expectContainerUntouched(fixture.container);
}

TEST(MjRenamerPolicyMatrixTest, applyModeWithAllRejectedProposalsIsZeroAppliedSuccess)
{
    // 全部建议被拒（计划为空）≠ 失败：仍按 zero-applied 成功口径交付，
    // 被拒流水照记（与「非空计划 + 应用零变化 ApplyFailed」互为对照）。
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;
    options.planOnly = false;

    std::vector<NameProposal> batch;
    batch.push_back(P(ProposalKind::Method, "Old", "alpha", "()V", "class"));   // 关键字 → 拒
    FixedProvider provider("matrix", std::move(batch));
    std::vector<NameProvider*> providers{&provider};

    const auto result = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.applied);
    EXPECT_TRUE(result.plan.empty());
    EXPECT_EQ(result.stats.proposed, 1U);
    EXPECT_EQ(result.stats.rejected, 1U);
    ASSERT_NE(findRejected(result, RejectionReason::Keyword), nullptr);
    expectContainerUntouched(fixture.container);
}

TEST(MjRenamerPolicyMatrixTest, applyModeWithNonEmptyPlanCommitsBytecodeFace)
{
    // 应用面真语义：planOnly=false 且计划非空 → 经 asm/remapper 域就地应用
    // （两态翻转的对照用例）。
    // 默认 RemapPolicy{} 只改字节码面：类字节 this_class/成员名换新，类资源
    // URI 不随动——remapper 批双门口径（URI 随动需开 resources）。
    Fixture        fixture;
    RenamerEngine  engine;
    RenamerOptions options;
    options.planOnly = false;
    FixedProvider    provider("matrix", matrixProposals());
    std::vector<NameProvider*> providers{&provider};

    auto result = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.applied);
    EXPECT_EQ(result.stats.accepted, 6U);
    ASSERT_EQ(findRejected(result, RejectionReason::ApplyFailed), nullptr);

    // 字节码面换新：this_class 读新名，声明成员槽读新名（槽级读回——remapper
    // 面只重指向不改写 Utf8 条目内容，字节文本扫描会误报残留）。
    EXPECT_EQ(thisClassOf(fixture.container, "Old.class"), "New");
    EXPECT_EQ(thisClassOf(fixture.container, "pkg/OldReflect.class"), "pkg/NewReflect");
    EXPECT_EQ(thisClassOf(fixture.container, "OldGeneric.class"), "NewGeneric");
    EXPECT_EQ(thisClassOf(fixture.container, "OldAnno.class"), "NewAnno");
    {
        auto cf = parseClassOf(fixture.container, "Old.class");
        ASSERT_NE(cf, nullptr);
        const auto members = declaredMemberNamesOf(*cf);
        EXPECT_TRUE(contains(members, "newField")) << "字段改名未落地";
        EXPECT_TRUE(contains(members, "beta")) << "方法改名未落地";
        EXPECT_FALSE(contains(members, "oldField")) << "旧字段名槽残留";
        EXPECT_FALSE(contains(members, "alpha")) << "旧方法名槽残留";
    }
    // URI 面不随动（默认 policy）：旧 URI 全部原位挂着新名字节。
    EXPECT_NE(containerClassResource(fixture.container, "Old.class"), nullptr);
    EXPECT_NE(containerClassResource(fixture.container, "pkg/OldReflect.class"), nullptr);
    EXPECT_EQ(containerClassResource(fixture.container, "New.class"), nullptr);
    EXPECT_EQ(containerClassResource(fixture.container, "pkg/NewReflect.class"), nullptr);
    // plan/record 交付面不受应用影响。
    EXPECT_EQ(result.plan.classMappings().size(), 4U);
    ASSERT_EQ(result.record.accepted().size(), 6U);
}

}   // namespace mjbase::renamer
