// 断言强度原样保留，
// IR 单元按新契约重定型：MethodNode/ClassNode → MethodScope/ClassSession；
// CurrentClassContext 断言改写为 MethodScope 自带 owner 身份的等价验收）。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/progress_sink.hpp"
#include "spi_test_util.hpp"

using namespace mjbase::spi;
using namespace mjbase::spi::test;

namespace {

ca::str::Utf8StringArena tArena;

int g_insnCountAnalysisRuns = 0;

struct InsnCountAnalysis
{
    using Result = ca::usize;
    Result run(MethodScope& scope, MethodAnalysisManager&)
    {
        ++g_insnCountAnalysisRuns;
        return scope.code.dom.order().size();
    }
};

struct MethodCacheProbeAnalysis
{
    using Result = int;
    Result run(MethodScope&, MethodAnalysisManager&) { return 17; }
};

struct MethodNameAnalysis
{
    using Result = std::string;
    Result run(MethodScope& scope, MethodAnalysisManager&) { return std::string(scope.name); }
};

int g_classInsnCountAnalysisRuns = 0;

/// 类级分析：全会话已升格指令总数（promote 前调用也能算——经 collectCodeUnits）。
struct SessionInsnCountAnalysis
{
    using Result = ca::usize;
    Result run(jbc::ir::ClassSession& session, ClassAnalysisManager&)
    {
        ++g_classInsnCountAnalysisRuns;
        auto        visits = jbc::ir::collectCodeUnits(session);
        ca::usize   n      = 0;
        for (const auto& v : visits) {
            if (v.kind == jbc::ir::CodeVisit::Kind::Ok) n += v.unit->dom.order().size();
        }
        return n;
    }
};

class ObserveSessionInsnCountPass : public ClassPass
{
public:
    explicit ObserveSessionInsnCountPass(std::vector<ca::usize>& observed)
        : observed_(observed)
    {}

    PreservedAnalyses run(jbc::ir::ClassSession& session, ClassAnalysisManager& cam) override
    {
        cam.registerAnalysis<SessionInsnCountAnalysis>();
        observed_.push_back(cam.getResult<SessionInsnCountAnalysis>(session));
        return PreservedAnalyses::all();
    }

private:
    std::vector<ca::usize>& observed_;
};

/// 方法级 pass：追加 NOP 后抛出（验证异常路径的保守失效）。
class AddNopThenThrowPass : public MethodPass
{
public:
    PreservedAnalyses run(MethodScope& scope, MethodAnalysisManager&) override
    {
        jbc::ir::CodeEditor editor(scope.code);
        (void)editor.append(jbc::ir::Insn{});
        throw std::runtime_error("mutating pass failed");
    }
};

/// 方法级 pass：经 AM 按当前 scope 读分析并记录（scope 地址键缓存验收）。
class ProbeByScopePass : public MethodPass
{
public:
    explicit ProbeByScopePass(std::vector<std::string>& log)
        : log_(log)
    {}
    PreservedAnalyses run(MethodScope& scope, MethodAnalysisManager& mam) override
    {
        log_.push_back(mam.getResult<MethodNameAnalysis>(scope));
        return PreservedAnalyses::all();
    }

private:
    std::vector<std::string>& log_;
};

/// 方法级 pass：抛 std::runtime_error。
class ThrowingMethodPass : public MethodPass
{
public:
    PreservedAnalyses run(MethodScope&, MethodAnalysisManager&) override
    {
        throw std::runtime_error("pass failed");
    }
};

/// 类级 pass：抛 std::runtime_error。
class ThrowingClassPass : public ClassPass
{
public:
    PreservedAnalyses run(jbc::ir::ClassSession&, ClassAnalysisManager&) override
    {
        throw std::runtime_error("class pass failed");
    }
};

/// 进度快照记录 sink（update/finishPhase 为虚函数，测试可拦截）。
class RecordingSink : public ProgressSink
{
public:
    std::vector<std::pair<int, int>> seen;   ///< (index, total)

    void update(State s) override { seen.emplace_back(s.index, s.total); }
    void finishPhase(State, std::string_view) override {}
};

/// 装配：cpm = [敌意类抛出点(可选)] + [ClassToMethodPassAdaptor(RemoveNop)]。
GlobalToClassPassAdaptor makeHostileAwareAdaptor(const std::string&  throwForClassName,
                                                 mjbase::KeyPool&    pool,
                                                 PassPipelineOptions options = {},
                                                 ProgressSink*       progress = nullptr)
{
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<RemoveNopPass>());
    ClassPassManager cpm;
    if (!throwForClassName.empty()) {
        cpm.addPass(std::make_unique<ThrowingClassPassByName>(throwForClassName));
    }
    cpm.addPass(std::make_unique<ClassToMethodPassAdaptor>(std::move(mpm), pool));
    return GlobalToClassPassAdaptor(std::move(cpm), progress, options);
}

}   // namespace

// ── PassManager 顺序执行 ──
TEST(PassPipelineTest, methodManagerRunsInOrder)
{
    std::vector<std::string> log;
    MethodPassManager        mgr;
    mgr.addPass(std::make_unique<RecordingMethodPass>(log))
        .addPass(std::make_unique<RecordingMethodPass>(log))
        .addPass(std::make_unique<RecordingMethodPass>(log));
    EXPECT_EQ(mgr.size(), 3u);

    MethodAnalysisManager mam(tArena);
    ScopeFixture          fx(buildClassBytes("T", {{"t", "()V", iconstReturn()}}));
    mgr.run(*fx.scope, mam);
    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], "t");
    EXPECT_EQ(log[1], "t");
    EXPECT_EQ(log[2], "t");
}

// ── 空 manager 返回 all（无 pass 改动一切保留）──
TEST(PassPipelineTest, emptyManagerPreservesAll)
{
    MethodPassManager mgr;
    EXPECT_TRUE(mgr.empty());
    MethodAnalysisManager mam(tArena);
    ScopeFixture          fx(buildClassBytes("T", {{"t", "()V", iconstReturn()}}));
    EXPECT_TRUE(mgr.run(*fx.scope, mam).areAllPreserved());
}

// ── 全只读 pass：整体保留 all ──
TEST(PassPipelineTest, allReadOnlyPreservesAll)
{
    MethodPassManager mgr;
    mgr.addPass(std::make_unique<ReadOnlyMethodPass>())
        .addPass(std::make_unique<ReadOnlyMethodPass>());
    MethodAnalysisManager mam(tArena);
    ScopeFixture          fx(buildClassBytes("T", {{"t", "()V", iconstReturn()}}));
    EXPECT_TRUE(mgr.run(*fx.scope, mam).areAllPreserved());
}

// ── 含一个改动 pass：整体不再 all（聚合取交）──
TEST(PassPipelineTest, oneModifyingPassClearsAll)
{
    std::vector<std::string> log;
    MethodPassManager        mgr;
    mgr.addPass(std::make_unique<ReadOnlyMethodPass>())
        .addPass(std::make_unique<RecordingMethodPass>(log));   // none()
    MethodAnalysisManager mam(tArena);
    ScopeFixture          fx(buildClassBytes("T", {{"t", "()V", iconstReturn()}}));
    EXPECT_FALSE(mgr.run(*fx.scope, mam).areAllPreserved());
}

TEST(PassPipelineTest, rejectsNullPass)
{
    MethodPassManager mgr;
    EXPECT_THROW(mgr.addPass(nullptr), std::invalid_argument);
    EXPECT_TRUE(mgr.empty());
}

TEST(PassPipelineTest, passExceptionStopsLaterPasses)
{
    std::vector<std::string> log;
    MethodPassManager        mgr;
    mgr.addPass(std::make_unique<RecordingMethodPass>(log))
        .addPass(std::make_unique<ThrowingMethodPass>())
        .addPass(std::make_unique<RecordingMethodPass>(log));

    MethodAnalysisManager mam(tArena);
    ScopeFixture          fx(buildClassBytes("T", {{"t", "()V", iconstReturn()}}));
    EXPECT_THROW(mgr.run(*fx.scope, mam), std::runtime_error);
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], "t");
}

TEST(PassPipelineTest, passExceptionInvalidatesChangedMethodAnalyses)
{
    g_insnCountAnalysisRuns = 0;
    MethodPassManager mgr;
    mgr.addPass(std::make_unique<AddNopThenThrowPass>());

    MethodAnalysisManager mam(tArena);
    ScopeFixture          fx(buildClassBytes("T", {{"t", "()V", bareReturn()}}));
    mam.registerAnalysis<InsnCountAnalysis>();

    ASSERT_EQ(mam.getResult<InsnCountAnalysis>(*fx.scope), 1u);
    ASSERT_EQ(g_insnCountAnalysisRuns, 1);
    EXPECT_THROW(mgr.run(*fx.scope, mam), std::runtime_error);

    // pass 在抛出异常前已经修改方法；后续消费者必须重新计算，
    // 不能继续读取异常前的指令数。
    EXPECT_EQ(mam.getResult<InsnCountAnalysis>(*fx.scope), 2u);
    EXPECT_EQ(g_insnCountAnalysisRuns, 2);
}

// ── ClassToMethodPassAdaptor：method pipeline 对会话内每个方法跑一遍 ──
TEST(PassPipelineTest, classToMethodAdaptorRunsPerMethod)
{
    std::vector<std::string> log;
    MethodPassManager        mpm;
    mpm.addPass(std::make_unique<RecordingMethodPass>(log));

    mjbase::KeyPool pool;
    ClassToMethodPassAdaptor adaptor(std::move(mpm), pool);

    auto     session = makeSession(buildClassBytes("T", {{"a", "()V", iconstReturn()},
                                              {"b", "()V", iconstReturn()},
                                              {"c", "()V", iconstReturn()}}));
    ClassAnalysisManager cam(tArena);
    MethodAnalysisManager mam(tArena);
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});

    adaptor.run(*session, cam);
    EXPECT_EQ(log.size(), 3u);   // pass 在 3 个方法上各跑一次
}

TEST(PassPipelineTest, classToMethodAdaptorRejectsMissingProxy)
{
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<ReadOnlyMethodPass>());
    mjbase::KeyPool      pool;
    ClassToMethodPassAdaptor adaptor(std::move(mpm), pool);
    auto                 session =
        makeSession(buildClassBytes("T", {{"t", "()V", iconstReturn()}}));
    ClassAnalysisManager cam(tArena);   // 未 pin proxy

    EXPECT_THROW(adaptor.run(*session, cam), std::logic_error);
}

// ── 类级 pass 声明改动 → pinned 方法级 AM 缓存被清（S3 跨层失效联动）──
TEST(PassPipelineTest, classInvalidationClearsPinnedMethodAnalysisCache)
{
    ScopeFixture          fx(buildNopClassBytes("T"));
    ClassAnalysisManager  cam(tArena);
    MethodAnalysisManager mam(tArena);
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});
    mam.registerAnalysis<MethodCacheProbeAnalysis>();

    EXPECT_EQ(mam.getResult<MethodCacheProbeAnalysis>(*fx.scope), 17);
    ASSERT_NE(mam.getCachedResult<MethodCacheProbeAnalysis>(*fx.scope), nullptr);

    ClassPassManager cpm;
    cpm.addPass(std::make_unique<TouchingClassPass>());
    EXPECT_FALSE(cpm.run(*fx.session, cam).areAllPreserved());
    EXPECT_EQ(mam.getCachedResult<MethodCacheProbeAnalysis>(*fx.scope), nullptr);
}

// ── 方法级管线改动 IR → 类级缓存失效（嵌套 adaptor 方向联动）──
TEST(PassPipelineTest, nestedMethodAdaptorInvalidatesClassAnalysisCache)
{
    g_classInsnCountAnalysisRuns = 0;

    ClassAnalysisManager  cam(tArena);
    MethodAnalysisManager mam(tArena);
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});

    auto     session = makeSession(buildNopClassBytes("T"));
    std::vector<ca::usize> observed;
    ClassPassManager       cpm;
    cpm.addPass(std::make_unique<ObserveSessionInsnCountPass>(observed));
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<AddNopPass>());
    mjbase::KeyPool pool;
    cpm.addPass(std::make_unique<ClassToMethodPassAdaptor>(std::move(mpm), pool));
    cpm.addPass(std::make_unique<ObserveSessionInsnCountPass>(observed));

    cpm.run(*session, cam);
    EXPECT_EQ(observed.size(), 2u);
    EXPECT_EQ(observed[0] + 1, observed[1]);   // AddNop 之后指令总数 +1（缓存已失效重算）
    EXPECT_EQ(g_classInsnCountAnalysisRuns, 2);
}

TEST(PassPipelineTest, classPassExceptionClearsPinnedMethodAnalysisCache)
{
    ScopeFixture          fx(buildNopClassBytes("T"));
    ClassAnalysisManager  cam(tArena);
    MethodAnalysisManager mam(tArena);
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});
    mam.registerAnalysis<MethodCacheProbeAnalysis>();

    EXPECT_EQ(mam.getResult<MethodCacheProbeAnalysis>(*fx.scope), 17);
    ClassPassManager cpm;
    cpm.addPass(std::make_unique<ThrowingClassPass>());

    EXPECT_THROW(cpm.run(*fx.session, cam), std::runtime_error);
    EXPECT_EQ(mam.getCachedResult<MethodCacheProbeAnalysis>(*fx.scope), nullptr);
}

// ── 方法级单元自带 owner 身份（CurrentClassContext 折叠进 MethodScope）──
TEST(PassPipelineTest, methodPassSeesOwnerIdentity)
{
    std::vector<std::string> captured;
    MethodPassManager        mpm;
    mpm.addPass(std::make_unique<OwnerNameCapturingPass>(captured));

    mjbase::KeyPool      pool;
    ClassToMethodPassAdaptor adaptor(std::move(mpm), pool);

    auto session = makeSession(buildClassBytes("com/example/Foo",
                                               {{"a", "()V", iconstReturn()},
                                                {"b", "()V", iconstReturn()}}));
    ClassAnalysisManager  cam(tArena);
    MethodAnalysisManager mam(tArena);
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});

    adaptor.run(*session, cam);
    ASSERT_EQ(captured.size(), 2u);
    // 两个方法都取到所属类名（MethodScope.session 直达会话骨架）
    EXPECT_EQ(captured[0], "com/example/Foo");
    EXPECT_EQ(captured[1], "com/example/Foo");
    // adaptor 未注入任何 pinned 上下文（旧 CurrentClassContext 协议已折叠），
    // cam 的 pinned 只剩管线 bootstrap 的 proxy 本身。
    ASSERT_TRUE(cam.hasPinned<MethodAnalysisManagerProxy>());
}

// ── 方法缓存的键是 MethodScope 地址：驱动方保证 scope 互异，不跨方法伪造命中 ──
TEST(PassPipelineTest, perMethodScopeCacheDoesNotAliasAcrossMethods)
{
    std::vector<std::string> log;
    MethodPassManager        mpm;
    mpm.addPass(std::make_unique<ProbeByScopePass>(log));

    mjbase::KeyPool      pool;
    ClassToMethodPassAdaptor adaptor(std::move(mpm), pool);

    auto     session = makeSession(buildClassBytes("T", {{"a", "()V", iconstReturn()},
                                              {"b", "()V", iconstReturn()},
                                              {"c", "()V", iconstReturn()}}));
    ClassAnalysisManager cam(tArena);
    MethodAnalysisManager mam(tArena);
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});
    mam.registerAnalysis<MethodNameAnalysis>();

    adaptor.run(*session, cam);
    // 若地址复用伪造缓存命中，后续方法会读到首个方法的名字。
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b", "c"}));
}

// ── 方法过滤谓词：被过滤方法不跑管线（promote 前按身份面求值）──
TEST(PassPipelineTest, methodPredicateFiltersBeforePromote)
{
    std::vector<std::string> log;
    MethodPassManager        mpm;
    mpm.addPass(std::make_unique<RecordingMethodPass>(log));

    mjbase::KeyPool pool;
    ClassToMethodPassAdaptor adaptor(
        std::move(mpm), pool, [](const MethodTarget& t) { return t.name == "b"; });

    auto     session = makeSession(buildClassBytes("T", {{"a", "()V", iconstReturn()},
                                              {"b", "()V", iconstReturn()},
                                              {"c", "()V", iconstReturn()}}));
    ClassAnalysisManager cam(tArena);
    MethodAnalysisManager mam(tArena);
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});

    adaptor.run(*session, cam);
    EXPECT_EQ(log, (std::vector<std::string>{"b"}));
}

// ── adaptor 装进 ClassPassManager：可嵌套（PassManager 本身是 Pass）──
TEST(PassPipelineTest, adaptorNestsInClassManager)
{
    std::vector<std::string> log;
    MethodPassManager        mpm;
    mpm.addPass(std::make_unique<RecordingMethodPass>(log));

    ClassPassManager cpm;
    mjbase::KeyPool  pool;
    cpm.addPass(std::make_unique<ClassToMethodPassAdaptor>(std::move(mpm), pool));

    auto session = makeSession(buildClassBytes("T", {{"x", "()V", iconstReturn()},
                                                     {"y", "()V", iconstReturn()}}));
    ClassAnalysisManager  cam(tArena);
    MethodAnalysisManager mam(tArena);
    cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});

    cpm.run(*session, cam);
    EXPECT_EQ(log.size(), 2u);
}

// ── 管线统一驱动 lower：方法 pass 改 IR → backing 变 → writeClass 写回 ──
TEST(PassPipelineTest, globalAdaptorWritesBackChangedClasses)
{
    auto                container = mjbase::model::ResourceContainer();
    auto                aBytes    = buildNopClassBytes("com/example/A");
    auto                bBytes    = buildNopClassBytes("com/example/B");
    auto                aRes      = std::make_unique<mjbase::model::ClassResource>("A.class", aBytes);
    auto                bRes      = std::make_unique<mjbase::model::ClassResource>("B.class", bBytes);
    auto*               aRaw      = aRes.get();
    auto*               bRaw      = bRes.get();
    container.addResource("A.class", std::move(aRes));
    container.addResource("B.class", std::move(bRes));

    GlobalAnalysisManager gam(tArena);
    mjbase::KeyPool       pool;
    auto                  adaptor = makeHostileAwareAdaptor("", pool);
    adaptor.run(container, gam);

    EXPECT_NE(aRaw->get(), aBytes);   // NOP 已被移除（写回发生）
    EXPECT_NE(bRaw->get(), bBytes);
    // 写回产物仍是合法 class（重新建会话成功且方法仍在）
    auto session = makeSession(aRaw->get());
    ASSERT_TRUE(session != nullptr);
    EXPECT_EQ(session->unit().methods.size(), 1u);
}

TEST(PassPipelineTest, globalAdaptorReportsMalformedClassAndPreservesBytes)
{
    mjbase::KeyPool  pool;
    ClassPassManager cpm;
    GlobalToClassPassAdaptor adaptor(std::move(cpm));
    mjbase::model::ResourceContainer container;
    std::vector<ca::u8> malformed = {0xCA, 0xFE, 0x00};
    auto resource = std::make_unique<mjbase::model::ClassResource>("Broken.class", malformed);
    auto* raw     = resource.get();
    container.addResource("Broken.class", std::move(resource));
    GlobalAnalysisManager gam(tArena);

    try {
        adaptor.run(container, gam);
        FAIL() << "malformed class should fail";
    }
    catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("Broken.class"), std::string::npos);
    }
    EXPECT_EQ(raw->get(), malformed);
}

// ── 空 class 管线：无改动不重序列化，原字节透传 ──
TEST(PassPipelineTest, globalAdaptorLeavesUntouchedClassesByteIdentical)
{
    mjbase::KeyPool  pool;
    ClassPassManager cpm;   // 空
    GlobalToClassPassAdaptor adaptor(std::move(cpm));
    mjbase::model::ResourceContainer container;
    auto aBytes = buildNopClassBytes("com/example/A");
    auto aRes   = std::make_unique<mjbase::model::ClassResource>("A.class", aBytes);
    auto* aRaw  = aRes.get();
    container.addResource("A.class", std::move(aRes));
    GlobalAnalysisManager gam(tArena);

    auto pa = adaptor.run(container, gam);
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(aRaw->get(), aBytes);   // 逐字节一致（copy-through 语义）
}

// ── #作业级原子性（两阶段提交）──

// 成功路径：两个 class 都被改写并统一提交。
TEST(PassPipelineTest, globalAdaptorCommitsAllClassesOnSuccess)
{
    auto container = mjbase::model::ResourceContainer();
    auto aBytes    = buildNopClassBytes("com/example/A");
    auto bBytes    = buildNopClassBytes("com/example/B");
    auto aRes      = std::make_unique<mjbase::model::ClassResource>("A.class", aBytes);
    auto bRes      = std::make_unique<mjbase::model::ClassResource>("B.class", bBytes);
    auto* aRaw     = aRes.get();
    auto* bRaw     = bRes.get();
    container.addResource("A.class", std::move(aRes));
    container.addResource("B.class", std::move(bRes));
    GlobalAnalysisManager gam(tArena);

    mjbase::KeyPool pool;
    auto            adaptor = makeHostileAwareAdaptor("", pool);
    adaptor.run(container, gam);

    EXPECT_NE(aRaw->get(), aBytes);   // NOP 已被移除（写回发生）
    EXPECT_NE(bRaw->get(), bBytes);
}

// 失败路径：前序 class 已序列化、后序 class 读失败 → 整个作业不产生部分写回。
TEST(PassPipelineTest, globalAdaptorRollsBackWholeJobOnLateFailure)
{
    auto                container = mjbase::model::ResourceContainer();
    auto                aBytes    = buildNopClassBytes("com/example/A");
    std::vector<ca::u8> malformed = {0xCA, 0xFE, 0x00};
    auto                aRes      = std::make_unique<mjbase::model::ClassResource>("A.class", aBytes);
    auto                bRes = std::make_unique<mjbase::model::ClassResource>("Broken.class", malformed);
    auto*               aRaw = aRes.get();
    auto*               bRaw = bRes.get();
    container.addResource("A.class", std::move(aRes));
    container.addResource("Broken.class", std::move(bRes));
    GlobalAnalysisManager gam(tArena);

    mjbase::KeyPool pool;
    auto            adaptor = makeHostileAwareAdaptor("", pool);
    EXPECT_THROW(adaptor.run(container, gam), std::runtime_error);

    // 原子性：前序成功的 class 也没有被写回（NOP 保留），容器保持原样。
    EXPECT_EQ(aRaw->get(), aBytes);
    EXPECT_EQ(bRaw->get(), malformed);
}

// ── 进度回调序（issue #332 语义）：Pipeline 阶段逐类 1..N/total 刷新 ──
TEST(PassPipelineTest, globalAdaptorReportsProgressInClassOrder)
{
    mjbase::KeyPool pool;
    auto            adaptor = makeHostileAwareAdaptor("", pool);

    mjbase::model::ResourceContainer container;
    container.addResource("A.class",
                          std::make_unique<mjbase::model::ClassResource>("A.class",
                                                                         buildNopClassBytes("A")));
    container.addResource("B.class",
                          std::make_unique<mjbase::model::ClassResource>("B.class",
                                                                         buildNopClassBytes("B")));
    container.addResource("C.class",
                          std::make_unique<mjbase::model::ClassResource>("C.class",
                                                                         buildNopClassBytes("C")));

    RecordingSink  sink;
    GlobalAnalysisManager gam(tArena);
    adaptor = makeHostileAwareAdaptor("", pool, PassPipelineOptions{}, &sink);
    adaptor.run(container, gam);

    ASSERT_EQ(sink.seen.size(), 3u);
    std::sort(sink.seen.begin(), sink.seen.end());
    EXPECT_EQ(sink.seen, (std::vector<std::pair<int, int>>{{1, 3}, {2, 3}, {3, 3}}));
}
