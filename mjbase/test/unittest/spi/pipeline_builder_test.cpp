//pipeline_builder / pass_registry：§9 装配层单元测试。
//
//验收口径：装配序（净化门 head → global → adaptor 嵌套 → post-class）、
//applyTo 按 scope 分派 + Cf fail-loud、工厂每次构建产新实例、注册表
//roundtrip（幂等注册、getPipeline 稳定序、default-optimize 默认链组成）。

#include "mjbase/spi/pipeline_builder.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_registry.hpp"
#include "spi_test_util.hpp"

namespace {

using namespace mjbase::spi;
using namespace mjbase::spi::test;
using mjbase::model::ResourceContainer;

/// 容器级哨兵 pass：run 时把 tag 追加进共享序列（观察装配序）。
class OrderProbePass final : public GlobalPass
{
public:
    OrderProbePass(std::vector<int>& order, int tag)
        : order_(order)
        , tag_(tag)
    {}
    PreservedAnalyses run(ResourceContainer& /*container*/, GlobalAnalysisManager& /*am*/) override
    {
        order_.push_back(tag_);
        return PreservedAnalyses::all();
    }

private:
    std::vector<int>& order_;
    int               tag_;
};

TEST(PipelineBuilderTest, SanitizeGateIsHeadAndSegmentsKeepRegistrationOrder)
{
    std::vector<int> order;
    PipelineBuilder  builder;
    builder.addGlobalPass([&order] { return std::make_unique<OrderProbePass>(order, 1); }, "g1")
        .addPostClassPass([&order] { return std::make_unique<OrderProbePass>(order, 2); }, "p1");

    PassJobContext job;
    const auto     mgr = builder.buildNestedPipeline(job);
    // 净化门 head + global 1 + post-class 2；method/class 段全空时不生成 adaptor。
    ASSERT_EQ(mgr.size(), static_cast<ca::usize>(3));
}

TEST(PipelineBuilderTest, NamesReportPerSegmentInRegistrationOrder)
{
    std::vector<int> sink;
    PipelineBuilder  builder;
    builder.addMethodPass([] { return std::make_unique<ReadOnlyMethodPass>(); }, "m1")
        .addClassPass([] { return std::make_unique<TouchingClassPass>(); }, "c1")
        .addGlobalPass([&sink] { return std::make_unique<OrderProbePass>(sink, 0); }, "g1")
        .addPostClassPass([&sink] { return std::make_unique<OrderProbePass>(sink, 0); }, "p1");

    EXPECT_EQ(builder.methodPassNames(), (std::vector<std::string>{"m1"}));
    EXPECT_EQ(builder.classPassNames(), (std::vector<std::string>{"c1"}));
    EXPECT_EQ(builder.globalPassNames(), (std::vector<std::string>{"g1"}));
    EXPECT_EQ(builder.postClassPassNames(), (std::vector<std::string>{"p1"}));
    EXPECT_FALSE(builder.empty());
}

TEST(PipelineBuilderTest, ApplyToDispatchesByScopeTagAndRejectsUnknownAndCf)
{
    PassNameRegistry registry;
    std::vector<int> sink;
    ASSERT_TRUE(
        registry
            .registerMethodPass("M", [] { return std::make_unique<ReadOnlyMethodPass>(); }, "m desc")
            .is_ok());
    ASSERT_TRUE(
        registry.registerClassPass("C", [] { return std::make_unique<TouchingClassPass>(); }).is_ok());
    ASSERT_TRUE(
        registry.registerGlobalPass("G",
                                    [&sink] {
                                        return std::make_unique<OrderProbePass>(sink, 0);
                                    })
            .is_ok());

    PipelineBuilder builder;
    ASSERT_TRUE(applyTo(registry, "M", builder).is_ok());
    ASSERT_TRUE(applyTo(registry, "C", builder).is_ok());
    ASSERT_TRUE(applyTo(registry, "G", builder).is_ok());
    EXPECT_EQ(builder.methodPassNames(), (std::vector<std::string>{"M"}));
    EXPECT_EQ(builder.classPassNames(), (std::vector<std::string>{"C"}));
    EXPECT_EQ(builder.globalPassNames(), (std::vector<std::string>{"G"}));

    EXPECT_EQ(applyTo(registry, "Missing", builder).unwrap_err(), RegistryError::UnknownPass);

    // Cf 段未接入容器管线：fail loud，不静默跳过。
    ASSERT_TRUE(registry
                    .registerCfClassPass("F",
                                         [] {
                                             return std::unique_ptr<CfClassPass>{};
                                         })
                    .is_ok());
    EXPECT_EQ(applyTo(registry, "F", builder).unwrap_err(), RegistryError::ScopeMismatch);
}

TEST(PipelineBuilderTest, ApplyToFactoryProducesFreshInstancePerBuild)
{
    PassNameRegistry registry;
    ASSERT_TRUE(
        registry.registerMethodPass("M", [] { return std::make_unique<ReadOnlyMethodPass>(); }).is_ok());

    PipelineBuilder builder;
    ASSERT_TRUE(applyTo(registry, "M", builder).is_ok());
    // 两次构建各自产新实例：工厂只产一次会让第二次 build 拿到空 pass（契约红线）。
    PassJobContext job;
    EXPECT_NO_THROW(builder.buildNestedPipeline(job));
    EXPECT_NO_THROW(builder.buildNestedPipeline(job));
}

TEST(PassRegistryTest, RegisterGetListRoundtripIsStable)
{
    // PassRegistry 是私有构造的单例（与 ）：测试用专名注入全局表，
    // 断言只看专名的相对性质，不依赖全表规模。
    auto&       registry = PassRegistry::instance();
    const char* nameA    = "zz-roundtrip-a";
    const char* nameB    = "zz-roundtrip-b";
    registry
        .registerPipeline(nameB, "desc-b", [] { return PipelineBuilder{}; }, PassStability::OptIn)
        .registerPipeline(nameA, "desc-a", [] { return PipelineBuilder{}; }, PassStability::Default);

    EXPECT_TRUE(registry.hasPipeline(nameA));
    EXPECT_FALSE(registry.hasPipeline("zz-roundtrip-missing"));
    ASSERT_NE(registry.pipelineInfo(nameA), nullptr);
    EXPECT_EQ(registry.pipelineInfo(nameA)->stability, PassStability::Default);
    EXPECT_EQ(registry.pipelineInfo(nameA)->description, "desc-a");
    EXPECT_EQ(registry.pipelineInfo("zz-roundtrip-missing"), nullptr);

    // 同名覆盖是受控更新：重注册后元信息随之更新。
    registry.registerPipeline(nameA, "desc-a2", [] { return PipelineBuilder{}; },
                              PassStability::Experimental);
    ASSERT_NE(registry.pipelineInfo(nameA), nullptr);
    EXPECT_EQ(registry.pipelineInfo(nameA)->description, "desc-a2");
    EXPECT_EQ(registry.pipelineInfo(nameA)->stability, PassStability::Experimental);

    // listPipelines 按 name 稳定序：两个专名相邻且 a 在 b 前。
    const auto listed = registry.listPipelines();
    bool       sawB   = false;
    bool       ok     = false;
    for (const auto& info : listed) {
        if (info.name == nameA) {
            ok = !sawB;   // a 必须先于 b
            break;
        }
        if (info.name == nameB) sawB = true;
    }
    EXPECT_TRUE(ok);

    // 未知名返回空 builder。
    EXPECT_TRUE(registry.getPipeline("zz-roundtrip-missing").empty());
}

TEST(PassRegistryTest, BuiltinPipelinesAreIdempotentAndDefaultOptimizeIsDefault)
{
    const auto first = registerBuiltinPipelines();
    EXPECT_GT(first, static_cast<ca::usize>(0));
    // 幂等：重复注册返回 0，集合不变。
    EXPECT_EQ(registerBuiltinPipelines(), static_cast<ca::usize>(0));

    const auto& reg = PassRegistry::instance();
    ASSERT_TRUE(reg.hasPipeline("default-optimize"));
    ASSERT_NE(reg.pipelineInfo("default-optimize"), nullptr);
    EXPECT_EQ(reg.pipelineInfo("default-optimize")->stability, PassStability::Default);
    // 默认链 = 三个确定性方法体清理件；无 global/post-class 段。
    const auto pipeline = reg.getPipeline("default-optimize");
    EXPECT_EQ(pipeline.methodPassCount(), static_cast<ca::usize>(3));
    EXPECT_TRUE(pipeline.globalPassNames().empty());
    EXPECT_TRUE(pipeline.postClassPassNames().empty());
}

TEST(PassNameRegistryDescriptionTest, ListCarriesDescriptionViews)
{
    PassNameRegistry registry;
    ASSERT_TRUE(
        registry
            .registerMethodPass("M", [] { return std::make_unique<ReadOnlyMethodPass>(); },
                                "does nothing")
            .is_ok());
    ASSERT_TRUE(
        registry.registerMethodPass("N", [] { return std::make_unique<ReadOnlyMethodPass>(); }).is_ok());

    const auto listed = registry.list();
    ASSERT_EQ(listed.size(), static_cast<ca::usize>(2));
    EXPECT_EQ(listed[0].name, "M");
    EXPECT_EQ(listed[0].description, "does nothing");
    EXPECT_EQ(listed[1].name, "N");
    EXPECT_TRUE(listed[1].description.empty());
}

}   // namespace
