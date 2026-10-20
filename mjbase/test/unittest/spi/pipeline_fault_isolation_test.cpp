// 移植自旧栈 unittest/pass/pipeline_fault_isolation_test.cpp：容错档位契约
// 断言原样保留（IR 单元按新契约重定型；PipelineBuilder 装配两例不迁——该
// builder 属 pass/pass_registry 旧栈装配面，未随迁，见迁移报告）。
//
// 三条主场景（提案验收项）：
//   a) perClassFaultIsolation：3 类 1 类抛异常 → 其余 2 类正常产出，失败类被记录，
//      作业结束汇总（跳过数/失败类清单）经 PassFaultReport 带出；
//   b) perMethodFaultIsolation：单类 3 方法中 1 方法抛异常 → 其余方法仍被处理，
//      失败方法被记录；
//   c) 默认档位：同样输入直接抛出（作业级原子语义不变）。
// 另覆盖：reader_error 阶段诊断、无报告 sink 时容错仍生效、非 std 异常不吞。
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "spi_test_util.hpp"

using namespace mjbase::spi;
using namespace mjbase::spi::test;

namespace {

ca::str::Utf8StringArena tFaultArena;

/// cpm = [敌意抛出点] + [RemoveNop 方法管线]，产出一个可直接 run 的 Global adaptor。
GlobalToClassPassAdaptor makeHostileAwareAdaptor(const std::string&  throwForClassName,
                                                 mjbase::KeyPool&    pool,
                                                 PassPipelineOptions options)
{
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<RemoveNopPass>());
    ClassPassManager cpm;
    if (!throwForClassName.empty()) {
        cpm.addPass(std::make_unique<ThrowingClassPassByName>(throwForClassName));
    }
    cpm.addPass(std::make_unique<ClassToMethodPassAdaptor>(std::move(mpm), pool));
    return GlobalToClassPassAdaptor(std::move(cpm), nullptr, options);
}

/// 容器内登记 3 个可解析 class，返回三个资源的原始指针。
struct ThreeClassFixture
{
    mjbase::model::ResourceContainer container;
    mjbase::KeyPool                  pool;
    mjbase::model::ClassResource*    a = nullptr;
    mjbase::model::ClassResource*    b = nullptr;
    mjbase::model::ClassResource*    c = nullptr;

    ThreeClassFixture()
    {
        auto aBytes = buildNopClassBytes("com/example/A");
        auto bBytes = buildNopClassBytes("com/example/B");
        auto cBytes = buildNopClassBytes("com/example/C");
        auto aRes   = std::make_unique<mjbase::model::ClassResource>("A.class", aBytes);
        auto bRes   = std::make_unique<mjbase::model::ClassResource>("B.class", bBytes);
        auto cRes   = std::make_unique<mjbase::model::ClassResource>("C.class", cBytes);
        a           = aRes.get();
        b           = bRes.get();
        c           = cRes.get();
        container.addResource("A.class", std::move(aRes));
        container.addResource("B.class", std::move(bRes));
        container.addResource("C.class", std::move(cRes));
    }
};

/// 直接 adaptor 跑类内方法的公共装配：类 hostile/Owner 含 a/b/c 三方法，
/// pin 好方法级 AM proxy。
struct MethodLoopFixture
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    ClassAnalysisManager                   cam{tFaultArena};
    MethodAnalysisManager                  mam{tFaultArena};
    mjbase::KeyPool                        pool;

    MethodLoopFixture()
    {
        session = makeSession(buildClassBytes("hostile/Owner",
                                              {{"a", "()V", iconstReturn()},
                                               {"b", "()V", iconstReturn()},
                                               {"c", "()V", iconstReturn()}}));
        cam.setPinned<MethodAnalysisManagerProxy>(InnerAMProxyResult<MethodAnalysisManager>{&mam});
    }
};

}   // namespace

// ── a) 类级容错：3 类 1 类抛异常 → 其余 2 类正常产出，失败类被记录，汇总带出 ──

TEST(PassFaultIsolationTest, classFaultIsolationSkipsBadClassAndCommitsRest)
{
    ThreeClassFixture     fx;
    GlobalAnalysisManager gam(tFaultArena);
    PassFaultReport       report;
    PassPipelineOptions   options;
    options.perClassFaultIsolation = true;
    options.faultReport            = &report;

    mjbase::KeyPool pool;
    auto            adaptor = makeHostileAwareAdaptor("com/example/C", pool, options);
    EXPECT_NO_THROW(adaptor.run(fx.container, gam));

    // 其余 2 类正常产出：NOP 被移除（写回发生）；失败类原 bytes 保持原样（跳过不写回）。
    EXPECT_NE(fx.a->get(), buildNopClassBytes("com/example/A"));
    EXPECT_NE(fx.b->get(), buildNopClassBytes("com/example/B"));
    EXPECT_EQ(fx.c->get(), buildNopClassBytes("com/example/C"));

    // 作业结束汇总：跳过数 / 失败类清单 / stable-skip 诊断行。
    ASSERT_EQ(report.skippedCount(), 1u);
    const auto& entry = report.entries.front();
    EXPECT_EQ(entry.scope, "class");
    EXPECT_EQ(entry.target, "C.class");
    EXPECT_EQ(entry.reason, "pass_error");
    EXPECT_EQ(entry.detail, "hostile class");
    EXPECT_EQ(entry.toLine(), "SKIP class=C.class reason=pass_error detail=hostile class");
    ASSERT_EQ(report.failedClasses().size(), 1u);
    EXPECT_EQ(report.failedClasses().front(), "C.class");
}

// 敌意 jar 主场景：坏类在读段就炸（无法解析的字节）→ reason=reader_error，
// 其余类照常跑完并统一提交。
TEST(PassFaultIsolationTest, classFaultIsolationSkipsMalformedClassWithReaderError)
{
    mjbase::model::ResourceContainer container;
    auto                aBytes    = buildNopClassBytes("com/example/A");
    std::vector<ca::u8> malformed = {0xCA, 0xFE, 0x00};
    auto                aRes      = std::make_unique<mjbase::model::ClassResource>("A.class", aBytes);
    auto                badRes = std::make_unique<mjbase::model::ClassResource>("Broken.class", malformed);
    auto*               aRaw   = aRes.get();
    auto*               badRaw = badRes.get();
    container.addResource("A.class", std::move(aRes));
    container.addResource("Broken.class", std::move(badRes));

    PassFaultReport     report;
    PassPipelineOptions options;
    options.perClassFaultIsolation = true;
    options.faultReport            = &report;

    GlobalAnalysisManager gam(tFaultArena);
    mjbase::KeyPool       pool;
    auto                  adaptor = makeHostileAwareAdaptor("", pool, options);
    EXPECT_NO_THROW(adaptor.run(container, gam));

    // 好类正常产出；坏类保持原样并被 reader_error 诊断记录。
    EXPECT_NE(aRaw->get(), aBytes);
    EXPECT_EQ(badRaw->get(), malformed);
    ASSERT_EQ(report.skippedCount(), 1u);
    EXPECT_EQ(report.entries.front().scope, "class");
    EXPECT_EQ(report.entries.front().target, "Broken.class");
    EXPECT_EQ(report.entries.front().reason, "reader_error");
}

// 容错档位不强制要求报告 sink：不传 faultReport 时跳过仍然生效（静默容错）。
TEST(PassFaultIsolationTest, classFaultIsolationWorksWithoutReportSink)
{
    ThreeClassFixture     fx;
    GlobalAnalysisManager gam(tFaultArena);
    PassPipelineOptions   options;
    options.perClassFaultIsolation = true;   // faultReport 保持 nullptr

    mjbase::KeyPool pool;
    auto            adaptor = makeHostileAwareAdaptor("com/example/C", pool, options);
    EXPECT_NO_THROW(adaptor.run(fx.container, gam));
    EXPECT_NE(fx.a->get(), buildNopClassBytes("com/example/A"));
    EXPECT_EQ(fx.c->get(), buildNopClassBytes("com/example/C"));
}

// 多个失败类：逐个记录，失败类清单按发生顺序去重带出。
TEST(PassFaultIsolationTest, classFaultIsolationReportsAllFailedClasses)
{
    ThreeClassFixture     fx;
    GlobalAnalysisManager gam(tFaultArena);
    PassFaultReport       report;
    PassPipelineOptions   options;
    options.perClassFaultIsolation = true;
    options.faultReport            = &report;

    // 同时瞄准 B 与 C 两个敌意类：各自命中时抛出。
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<RemoveNopPass>());
    ClassPassManager cpm;
    cpm.addPass(std::make_unique<ThrowingClassPassByName>("com/example/B"));
    cpm.addPass(std::make_unique<ThrowingClassPassByName>("com/example/C"));
    cpm.addPass(std::make_unique<ClassToMethodPassAdaptor>(std::move(mpm), fx.pool));
    GlobalToClassPassAdaptor adaptor(std::move(cpm), nullptr, options);

    EXPECT_NO_THROW(adaptor.run(fx.container, gam));
    EXPECT_NE(fx.a->get(), buildNopClassBytes("com/example/A"));   // A 正常产出
    EXPECT_EQ(report.skippedCount(), 2u);
    // 失败名单的顺序随类容器迭代序（平台间不同），这里校验集合而非顺序。
    auto failed = report.failedClasses();
    std::sort(failed.begin(), failed.end(), [](const ca::str::Utf8String& l,
                                               const ca::str::Utf8String& r) {
        return l.to_std_string() < r.to_std_string();
    });
    ASSERT_EQ(failed.size(), 2u);
    EXPECT_EQ(failed[0], "B.class");
    EXPECT_EQ(failed[1], "C.class");
}

// ── b) 方法级容错：单类 3 方法 1 方法抛异常 → 其余方法仍被处理，失败方法被记录 ──

TEST(PassFaultIsolationTest, methodFaultIsolationContinuesRemainingMethods)
{
    MethodLoopFixture       fx;
    std::vector<std::string> log;
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<RecordingMethodPass>(log));
    mpm.addPass(std::make_unique<ThrowingMethodPassByName>("b"));
    mpm.addPass(std::make_unique<RecordingMethodPass>(log));

    PassFaultReport     report;
    PassPipelineOptions options;
    options.perMethodFaultIsolation = true;
    options.faultReport             = &report;
    ClassToMethodPassAdaptor adaptor(std::move(mpm), fx.pool, nullptr, options);

    // 容错档位不抛出；a、c 两个方法被完整处理（前+后两个 recording pass 各记
    // 一次），b 只被前一个记到。
    auto pa = adaptor.run(*fx.session, fx.cam);
    EXPECT_EQ(log, (std::vector<std::string>{"a", "a", "b", "c", "c"}));

    // 失败方法被记录：scope/target/reason/detail 对齐 stable-skip 契约。
    ASSERT_EQ(report.skippedCount(), 1u);
    const auto& entry = report.entries.front();
    EXPECT_EQ(entry.scope, "method");
    EXPECT_EQ(entry.target, "hostile/Owner.b()V");
    EXPECT_EQ(entry.reason, "pass_error");
    EXPECT_EQ(entry.detail, "pass failed");
    EXPECT_EQ(entry.toLine(), "SKIP method=hostile/Owner.b()V reason=pass_error detail=pass failed");
    EXPECT_TRUE(report.failedClasses().empty());   // 方法级失败不计入失败类清单

    // 异常 pass 可能已改 IR：保守按已改动聚合，驱动外层重序列化。
    EXPECT_TRUE(pa.changed());
}

// ── c) 默认档位：同样输入直接抛出（作业级原子语义不变）──

TEST(PassFaultIsolationTest, defaultModeClassFailureThrowsAndKeepsWholeJobAtomic)
{
    ThreeClassFixture     fx;
    GlobalAnalysisManager gam(tFaultArena);
    auto                  aBytes = buildNopClassBytes("com/example/A");
    auto                  bBytes = buildNopClassBytes("com/example/B");
    auto                  cBytes = buildNopClassBytes("com/example/C");

    mjbase::KeyPool pool;
    auto adaptor = makeHostileAwareAdaptor("com/example/C", pool, PassPipelineOptions{});
    EXPECT_THROW(adaptor.run(fx.container, gam), std::runtime_error);

    // 原子语义不变：写回推迟到提交点，异常逃逸 → 整个作业无部分写回
    // （unordered_map 迭代序不定，但两阶段提交保证所有类都保持原样）。
    EXPECT_EQ(fx.a->get(), aBytes);
    EXPECT_EQ(fx.b->get(), bBytes);
    EXPECT_EQ(fx.c->get(), cBytes);
}

TEST(PassFaultIsolationTest, defaultModeMethodFailureThrowsAndStopsLaterMethods)
{
    MethodLoopFixture        fx;
    std::vector<std::string> log;
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<RecordingMethodPass>(log));
    mpm.addPass(std::make_unique<ThrowingMethodPassByName>("b"));
    mpm.addPass(std::make_unique<RecordingMethodPass>(log));

    ClassToMethodPassAdaptor adaptor(std::move(mpm), fx.pool);   // 默认档位
    EXPECT_THROW(adaptor.run(*fx.session, fx.cam), std::runtime_error);
    // b 处的异常中止整类：c 的两个 recording 都未发生。
    EXPECT_EQ(log, (std::vector<std::string>{"a", "a", "b"}));
}

// ── 非 std 异常不吞：容错档位只捕获 std::exception ──

TEST(PassFaultIsolationTest, methodFaultIsolationDoesNotSwallowNonStdExceptions)
{
    MethodLoopFixture fx;
    MethodPassManager mpm;
    mpm.addPass(std::make_unique<NonStdThrowingMethodPass>());

    PassPipelineOptions options;
    options.perMethodFaultIsolation = true;
    ClassToMethodPassAdaptor adaptor(std::move(mpm), fx.pool, nullptr, options);

    // 非 std 异常照常向上传播。
    EXPECT_THROW(adaptor.run(*fx.session, fx.cam), FakeThrowable);
}

TEST(PassFaultIsolationTest, classFaultIsolationDoesNotSwallowNonStdExceptions)
{
    ThreeClassFixture     fx;
    GlobalAnalysisManager gam(tFaultArena);
    auto                  aBytes = buildNopClassBytes("com/example/A");

    PassPipelineOptions options;
    options.perClassFaultIsolation = true;
    ClassPassManager cpm;
    cpm.addPass(std::make_unique<NonStdThrowingClassPass>());
    GlobalToClassPassAdaptor adaptor(std::move(cpm), nullptr, options);

    // 非 std 异常不被容错档位静默跳过：作业终止（经既有 catch-all 转成
    // runtime_error 保持历史包装语义，前序成功类不写回）。
    EXPECT_THROW(adaptor.run(fx.container, gam), std::runtime_error);
    EXPECT_EQ(fx.a->get(), aBytes);
}
