// 编排入口按新契约重定型（runAsmPipeline→runClassPipeline，runJarPipeline→
// runContainerPipeline；writerFlags/typeResolver 参数随 libjbc 写回面移除）。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/job_context.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "spi_test_util.hpp"

using namespace mjbase::spi;
using namespace mjbase::spi::test;

namespace {

int g_globalClassCountAnalysisRuns = 0;

struct GlobalClassCountAnalysis
{
    using Result = ca::usize;

    Result run(mjbase::model::ResourceContainer& container, GlobalAnalysisManager&)
    {
        ++g_globalClassCountAnalysisRuns;
        ca::usize count = 0;
        for (const auto& [uri, resource] : container.getResources()) {
            (void)uri;
            if (ca::core::tag_cast::isa<mjbase::model::ClassResource>(resource.get())) ++count;
        }
        return count;
    }
};

class ObserveGlobalClassCountPass : public GlobalPass
{
public:
    explicit ObserveGlobalClassCountPass(std::vector<ca::usize>& observed)
        : observed_(observed)
    {}

    PreservedAnalyses run(mjbase::model::ResourceContainer& container,
                          GlobalAnalysisManager&            gam) override
    {
        gam.registerAnalysis<GlobalClassCountAnalysis>();
        observed_.push_back(gam.getResult<GlobalClassCountAnalysis>(container));
        return PreservedAnalyses::all();
    }

private:
    std::vector<ca::usize>& observed_;
};

struct ThisClassNameAnalysis
{
    using Result = std::string;

    Result run(jbc::ir::ClassSession& session, ClassAnalysisManager&)
    {
        return std::string(session.unit().symbols().str(session.unit().thisClass));
    }
};

class ObserveThisClassNamePass : public ClassPass
{
public:
    explicit ObserveThisClassNamePass(std::vector<std::string>& names)
        : names_(names)
    {}

    PreservedAnalyses run(jbc::ir::ClassSession& session, ClassAnalysisManager& cam) override
    {
        cam.registerAnalysis<ThisClassNameAnalysis>();
        names_.push_back(cam.getResult<ThisClassNameAnalysis>(session));
        return PreservedAnalyses::all();
    }

private:
    std::vector<std::string>& names_;
};

/// 容器级装配：gpm = [Adaptor(Recording 方法管线)]。
GlobalPassManager makeRecordingContainerPipeline(std::vector<std::string>& log,
                                                 mjbase::KeyPool&          pool)
{
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<RecordingMethodPass>(log));
    ClassPassManager cpm;
    cpm.addPass(std::make_unique<ClassToMethodPassAdaptor>(std::move(mpm), pool));
    GlobalPassManager gpm;
    gpm.addPass(std::make_unique<GlobalToClassPassAdaptor>(std::move(cpm)));
    return gpm;
}

}   // namespace

// ── PassJobContext：arena intern 去重（同内容两次 intern 返回同一视图）──
TEST(PassJobContextTest, arenaInternDedup)
{
    PassJobContext job;
    auto&          arena = job.arena();
    auto           r1    = arena.intern("java/lang/String");
    auto           r2    = arena.intern("java/lang/String");
    EXPECT_EQ(r1.data(), r2.data());
    EXPECT_EQ(job.arena().size(), 1u);
}

// ── runClassPipeline：空管线原字节透传（copy-through 契约的入口形态）──
TEST(RunClassPipelineTest, roundtripNoOpPasses)
{
    PassJobContext job;
    auto           input = buildClassBytes("T", {{"t", "()I", iconstReturn()}});
    // 两个 manager 都为空，pipeline 不做任何重序列化
    auto result = runClassPipeline(input, nullptr, nullptr, job);
    ASSERT_TRUE(result.is_ok());
    auto output = std::move(result).unwrap();
    EXPECT_EQ(output, input);   // 逐字节一致
}

// ── runClassPipeline：裸方法级管线驱动每个方法 ──
TEST(RunClassPipelineTest, runsMethodPass)
{
    PassJobContext           job;
    std::vector<std::string> log;
    MethodPassManager        mm;
    mm.addPass(std::make_unique<RecordingMethodPass>(log));

    auto input = buildClassBytes("T", {{"t", "()I", iconstReturn()}});
    auto result = runClassPipeline(input, nullptr, &mm, job);
    ASSERT_TRUE(result.is_ok());
    auto output = std::move(result).unwrap();
    EXPECT_FALSE(output.empty());
    // 一个方法 → 记录一次
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], "t");
}

// ── runClassPipeline：类级管线含 ClassToMethodPassAdaptor 照常工作 ──
TEST(RunClassPipelineTest, acceptsClassToMethodAdaptorWithoutThrowing)
{
    PassJobContext job;
    auto           bytes = buildClassBytes("com/example/Foo", {{"run", "()V", iconstReturn()}});

    std::vector<std::string> captured;
    MethodPassManager        mpm;
    mpm.addPass(std::make_unique<OwnerNameCapturingPass>(captured));

    ClassPassManager cpm;
    mjbase::KeyPool  pool;
    cpm.addPass(std::make_unique<ClassToMethodPassAdaptor>(std::move(mpm), pool));

    auto result = runClassPipeline(bytes, &cpm, /*methodManager=*/nullptr, job);
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(std::move(result).unwrap().empty());
    // adaptor 跑了方法 pass，应捕获到类名（proxy 已被入口前置 pin）
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0], "com/example/Foo");
}

// ── runClassPipeline：裸方法路径的方法 pass 也能取到所属类身份 ──
TEST(RunClassPipelineTest, directMethodPassSeesOwnerIdentity)
{
    PassJobContext job;
    auto           bytes = buildClassBytes("com/example/Bar", {{"run", "()V", iconstReturn()}});

    std::vector<std::string> captured;
    MethodPassManager        mpm;
    mpm.addPass(std::make_unique<OwnerNameCapturingPass>(captured));

    auto result = runClassPipeline(bytes, /*classManager=*/nullptr, &mpm, job);
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(std::move(result).unwrap().empty());
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0], "com/example/Bar");
}

// ── runClassPipeline：读失败 → Err(Read)，detail 带错误码 ──
TEST(RunClassPipelineTest, readErrorReturnsErrNotThrow)
{
    PassJobContext      job;
    std::vector<ca::u8> malformed = {0xCA, 0xFE, 0x00};
    auto                result    = runClassPipeline(malformed, nullptr, nullptr, job);
    ASSERT_TRUE(result.is_err());
    auto err = result.unwrap_err();
    EXPECT_EQ(err.stage, PipelineError::Stage::Read);
}

// ── runClassPipeline：方法 pass 改 IR → 重序列化产物合法且已变 ──
TEST(RunClassPipelineTest, changedPipelineReserializes)
{
    PassJobContext job;
    auto           input = buildNopClassBytes("T");

    MethodPassManager mm;
    mm.addPass(std::make_unique<RemoveNopPass>());
    auto result = runClassPipeline(input, nullptr, &mm, job);
    ASSERT_TRUE(result.is_ok());
    auto output = std::move(result).unwrap();
    EXPECT_NE(output, input);   // NOP 已被移除

    auto session = makeSession(output);
    ASSERT_TRUE(session != nullptr);
    ASSERT_EQ(session->unit().methods.size(), 1u);
    auto promo = jbc::ir::promoteMethod(*session, 0);
    ASSERT_TRUE(promo.is_ok());
    EXPECT_EQ(std::move(promo).unwrap()->dom.order().size(), 1u);   // 只剩 RETURN
}

// ── runContainerPipeline：容器 → 嵌套 adaptor → 逐类逐方法处理 ──
TEST(RunContainerPipelineTest, containerPipelineProcessesClasses)
{
    PassJobContext           job;
    std::vector<std::string> log;
    auto                     gpm = makeRecordingContainerPipeline(log, job.identityPool());

    mjbase::model::ResourceContainer container;
    container.addResource("A.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "A.class", buildClassBytes("A", {{"t", "()I", iconstReturn()}})));
    container.addResource("B.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "B.class", buildClassBytes("B", {{"t", "()I", iconstReturn()}})));

    auto result = runContainerPipeline(container, gpm, job);
    ASSERT_TRUE(result.is_ok());

    // 2 个类 × 1 个方法 = 2 次记录
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], "t");
    EXPECT_EQ(log[1], "t");
}

// ── runContainerPipeline：类级分析缓存不跨类复用（每类一造 AM）──
TEST(RunContainerPipelineTest, doesNotReuseClassAnalysisAcrossClasses)
{
    PassJobContext job;
    GlobalPassManager gpm;
    mjbase::KeyPool   pool;

    std::vector<std::string> observedNames;
    ClassPassManager         cpm;
    cpm.addPass(std::make_unique<ObserveThisClassNamePass>(observedNames));
    gpm.addPass(std::make_unique<GlobalToClassPassAdaptor>(std::move(cpm)));

    mjbase::model::ResourceContainer container;
    container.addResource("A.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "A.class", buildClassBytes("A", {{"t", "()I", iconstReturn()}})));
    container.addResource("B.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "B.class", buildClassBytes("B", {{"t", "()I", iconstReturn()}})));

    auto result = runContainerPipeline(container, gpm, job);
    ASSERT_TRUE(result.is_ok());

    std::sort(observedNames.begin(), observedNames.end());
    EXPECT_EQ(observedNames, (std::vector<std::string>{"A", "B"}));
}

// ── 嵌套 adaptor 失效全局分析缓存：变更 pass 之后必须重算 ──
TEST(RunContainerPipelineTest, nestedGlobalAdaptorInvalidatesGlobalAnalysisCache)
{
    PassJobContext job;
    mjbase::model::ResourceContainer container;
    container.addResource("T.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "T.class", buildNopClassBytes("T")));

    std::vector<ca::usize> observed;
    GlobalPassManager      gpm;
    gpm.addPass(std::make_unique<ObserveGlobalClassCountPass>(observed));

    ClassPassManager cpm;
    cpm.addPass(std::make_unique<TouchingClassPass>());
    mjbase::KeyPool pool;
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<RemoveNopPass>());
    cpm.addPass(std::make_unique<ClassToMethodPassAdaptor>(std::move(mpm), pool));
    gpm.addPass(std::make_unique<GlobalToClassPassAdaptor>(std::move(cpm)));
    gpm.addPass(std::make_unique<ObserveGlobalClassCountPass>(observed));

    g_globalClassCountAnalysisRuns = 0;
    auto result                    = runContainerPipeline(container, gpm, job);
    ASSERT_TRUE(result.is_ok());

    ASSERT_EQ(observed, (std::vector<ca::usize>{1, 1}));
    EXPECT_EQ(g_globalClassCountAnalysisRuns, 2);
}

// ── runContainerPipeline：容器级失败收敛为 Err（detail 含资源 URI）──
TEST(RunContainerPipelineTest, containerFailureConvergesToErr)
{
    PassJobContext job;
    GlobalPassManager gpm;
    mjbase::KeyPool   pool;
    gpm.addPass(std::make_unique<GlobalToClassPassAdaptor>(ClassPassManager{}));

    mjbase::model::ResourceContainer container;
    container.addResource("Broken.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "Broken.class", std::vector<ca::u8>{0xCA, 0xFE, 0x00}));

    auto result = runContainerPipeline(container, gpm, job);
    ASSERT_TRUE(result.is_err());
    auto err = result.unwrap_err();
    EXPECT_EQ(err.stage, PipelineError::Stage::Pass);   // 容器级失败经 adaptor 异常上抛
    EXPECT_NE(err.detail.find("Broken.class"), std::string::npos);
}
