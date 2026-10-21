// （FakeUnit 替身跑机制，不绑定具体 IR）；typeResolver 注入口已随重定型移除，
// 无对应用例。
#include <gtest/gtest.h>

#include <memory>

#include "mjbase/spi/analysis_manager.hpp"

using namespace mjbase::spi;

namespace {

// 用一个简单 IR 单元类型代替真实 ClassSession/MethodScope，专注测 AM 机制。
struct FakeUnit
{
    int value = 0;
};

// 全局计数器：记录各分析被实际计算（run）的次数，用来验证惰性 + 缓存。
int g_analysisARuns = 0;
int g_analysisBRuns = 0;

// 分析 A：Result = int，等于 unit.value * 2。
struct DoubleAnalysis
{
    using Result = int;
    Result run(FakeUnit& unit, AnalysisManager<FakeUnit>&)
    {
        ++g_analysisARuns;
        return unit.value * 2;
    }
};

// 分析 B：依赖分析 A（验证 analysis 可经 am 取其他 analysis）。
struct PlusOneOfDoubleAnalysis
{
    using Result = int;
    Result run(FakeUnit& unit, AnalysisManager<FakeUnit>& am)
    {
        ++g_analysisBRuns;
        return am.getResult<DoubleAnalysis>(unit) + 1;
    }
};

struct PinnedAnalysis
{
    using Result = int;
    Result run(FakeUnit&, AnalysisManager<FakeUnit>&) { return 0; }
};

struct SharedResultAnalysis
{
    using Result = std::shared_ptr<int>;
    Result run(FakeUnit& unit, AnalysisManager<FakeUnit>&)
    {
        return std::make_shared<int>(unit.value);
    }
};

struct ThrowingAnalysis
{
    using Result = int;
    Result run(FakeUnit&, AnalysisManager<FakeUnit>&)
    {
        ++g_analysisARuns;
        throw std::runtime_error("analysis failed");
    }
};

class AnalysisManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        g_analysisARuns = 0;
        g_analysisBRuns = 0;
    }
    ca::str::Utf8StringArena arena;
};

}   // namespace

// ── 注册查询 ──
TEST_F(AnalysisManagerTest, registerAndQuery)
{
    AnalysisManager<FakeUnit> am(arena);
    EXPECT_FALSE(am.isRegistered<DoubleAnalysis>());
    am.registerAnalysis<DoubleAnalysis>();
    EXPECT_TRUE(am.isRegistered<DoubleAnalysis>());
}

TEST_F(AnalysisManagerTest, duplicateRegistrationIsIdempotent)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    am.registerAnalysis<DoubleAnalysis>();
    FakeUnit u{3};
    EXPECT_EQ(am.getResult<DoubleAnalysis>(u), 6);
    EXPECT_EQ(g_analysisARuns, 1);
}

TEST_F(AnalysisManagerTest, unregisteredAnalysisThrowsStableError)
{
    AnalysisManager<FakeUnit> am(arena);
    FakeUnit                  u{3};
    EXPECT_THROW(am.getResult<DoubleAnalysis>(u), std::logic_error);
    EXPECT_EQ(am.getCachedResult<DoubleAnalysis>(u), nullptr);
}

// ── 惰性求值 + 结果正确 ──
TEST_F(AnalysisManagerTest, lazyComputeCorrectResult)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    FakeUnit u{21};
    EXPECT_EQ(g_analysisARuns, 0);   // 注册不触发计算
    EXPECT_EQ(am.getResult<DoubleAnalysis>(u), 42);
    EXPECT_EQ(g_analysisARuns, 1);   // 第一次 get 才算
}

// ── 缓存命中：第二次 get 不重算 ──
TEST_F(AnalysisManagerTest, cacheHitNoRecompute)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    FakeUnit u{10};
    am.getResult<DoubleAnalysis>(u);
    am.getResult<DoubleAnalysis>(u);
    EXPECT_EQ(g_analysisARuns, 1);   // 只算了一次
}

// ── 不同 unit 各自缓存 ──
TEST_F(AnalysisManagerTest, perUnitCache)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    FakeUnit u1{1}, u2{2};
    EXPECT_EQ(am.getResult<DoubleAnalysis>(u1), 2);
    EXPECT_EQ(am.getResult<DoubleAnalysis>(u2), 4);
    EXPECT_EQ(g_analysisARuns, 2);   // 两个 unit 各算一次
    am.getResult<DoubleAnalysis>(u1);
    EXPECT_EQ(g_analysisARuns, 2);   // u1 命中缓存
}

// ── getCachedResult：未算返回 nullptr，算后返回指针 ──
TEST_F(AnalysisManagerTest, getCachedResult)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    FakeUnit u{7};
    EXPECT_EQ(am.getCachedResult<DoubleAnalysis>(u), nullptr);   // 未算
    EXPECT_EQ(g_analysisARuns, 0);                               // getCached 不触发计算
    am.getResult<DoubleAnalysis>(u);
    ASSERT_NE(am.getCachedResult<DoubleAnalysis>(u), nullptr);
    EXPECT_EQ(*am.getCachedResult<DoubleAnalysis>(u), 14);
}

// ── analysis 依赖 analysis ──
TEST_F(AnalysisManagerTest, analysisDependsOnAnalysis)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    am.registerAnalysis<PlusOneOfDoubleAnalysis>();
    FakeUnit u{20};
    EXPECT_EQ(am.getResult<PlusOneOfDoubleAnalysis>(u), 41);   // 20*2 + 1
    EXPECT_EQ(g_analysisARuns, 1);                             // B 内部触发了 A
    EXPECT_EQ(g_analysisBRuns, 1);
}

// ── invalidate：未保留的失效、保留的留存 ──
TEST_F(AnalysisManagerTest, invalidateRespectsPreserved)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    am.registerAnalysis<PlusOneOfDoubleAnalysis>();
    FakeUnit u{5};
    am.getResult<DoubleAnalysis>(u);
    am.getResult<PlusOneOfDoubleAnalysis>(u);
    EXPECT_EQ(g_analysisARuns, 1);
    EXPECT_EQ(g_analysisBRuns, 1);

    // 只保留 DoubleAnalysis，失效其余
    auto pa = PreservedAnalyses::none();
    pa.preserve<DoubleAnalysis>();
    am.invalidate(u, pa);

    // DoubleAnalysis 仍命中缓存（不重算）
    am.getResult<DoubleAnalysis>(u);
    EXPECT_EQ(g_analysisARuns, 1);
    // PlusOne 被失效，重算（且会再次触发 A，但 A 仍有缓存所以 A 不增）
    am.getResult<PlusOneOfDoubleAnalysis>(u);
    EXPECT_EQ(g_analysisBRuns, 2);
    EXPECT_EQ(g_analysisARuns, 1);   // A 仍命中缓存
}

// ── invalidate(all)：什么都不失效 ──
TEST_F(AnalysisManagerTest, invalidateAllPreservedIsNoop)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    FakeUnit u{5};
    am.getResult<DoubleAnalysis>(u);
    am.invalidate(u, PreservedAnalyses::all());
    am.getResult<DoubleAnalysis>(u);
    EXPECT_EQ(g_analysisARuns, 1);   // 没失效，命中缓存
}

// ── invalidate(none)：全失效 ──
TEST_F(AnalysisManagerTest, invalidateNonePreservedClearsAll)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    FakeUnit u{5};
    am.getResult<DoubleAnalysis>(u);
    am.invalidate(u, PreservedAnalyses::none());
    am.getResult<DoubleAnalysis>(u);
    EXPECT_EQ(g_analysisARuns, 2);   // 失效后重算
}

// ── setResult：绕过惰性计算，直接注入结果 ──
TEST_F(AnalysisManagerTest, setResultBypassesCompute)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    FakeUnit u{100};

    am.setResult<DoubleAnalysis>(u, 999);
    EXPECT_EQ(am.getResult<DoubleAnalysis>(u), 999);   // 注入值命中
    EXPECT_EQ(g_analysisARuns, 0);                     // 未触发计算
    EXPECT_EQ(*am.getCachedResult<DoubleAnalysis>(u), 999);

    // 第二次 setResult 覆盖旧值
    am.setResult<DoubleAnalysis>(u, 42);
    EXPECT_EQ(am.getResult<DoubleAnalysis>(u), 42);
    EXPECT_EQ(g_analysisARuns, 0);
}

// ── clear：清某 unit 缓存 ──
TEST_F(AnalysisManagerTest, clearUnit)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<DoubleAnalysis>();
    FakeUnit u{5};
    am.getResult<DoubleAnalysis>(u);
    am.clear(u);
    EXPECT_EQ(am.getCachedResult<DoubleAnalysis>(u), nullptr);
    am.getResult<DoubleAnalysis>(u);
    EXPECT_EQ(g_analysisARuns, 2);   // 清后重算
}

// ── arena 可达 ──
TEST_F(AnalysisManagerTest, arenaAccessible)
{
    AnalysisManager<FakeUnit> am(arena);
    EXPECT_EQ(&am.arena(), &arena);
}

TEST_F(AnalysisManagerTest, pinnedResultSurvivesInvalidateAndClear)
{
    AnalysisManager<FakeUnit> am(arena);
    FakeUnit                  u{1};
    EXPECT_FALSE(am.hasPinned<PinnedAnalysis>());
    am.setPinned<PinnedAnalysis>(42);
    ASSERT_TRUE(am.hasPinned<PinnedAnalysis>());
    ASSERT_NE(am.getPinned<PinnedAnalysis>(), nullptr);
    EXPECT_EQ(*am.getPinned<PinnedAnalysis>(), 42);

    am.invalidate(u, PreservedAnalyses::none());
    am.clear(u);
    am.clearAll();
    ASSERT_NE(am.getPinned<PinnedAnalysis>(), nullptr);
    EXPECT_EQ(*am.getPinned<PinnedAnalysis>(), 42);
}

TEST_F(AnalysisManagerTest, invalidationReleasesOrdinaryResultOwnership)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<SharedResultAnalysis>();
    FakeUnit u{7};

    std::weak_ptr<int> result = am.getResult<SharedResultAnalysis>(u);
    ASSERT_FALSE(result.expired());
    am.invalidate(u, PreservedAnalyses::none());
    EXPECT_TRUE(result.expired());
}

TEST_F(AnalysisManagerTest, pinnedOwnershipEndsWithManagerLifetime)
{
    std::weak_ptr<int> result;
    {
        AnalysisManager<FakeUnit> am(arena);
        auto                      value = std::make_shared<int>(42);
        result                          = value;
        am.setPinned<SharedResultAnalysis>(std::move(value));
        am.clearAll();
        ASSERT_FALSE(result.expired());
    }
    EXPECT_TRUE(result.expired());
}

TEST_F(AnalysisManagerTest, failedAnalysisIsNotCached)
{
    AnalysisManager<FakeUnit> am(arena);
    am.registerAnalysis<ThrowingAnalysis>();
    FakeUnit u{1};

    EXPECT_THROW(am.getResult<ThrowingAnalysis>(u), std::runtime_error);
    EXPECT_EQ(am.getCachedResult<ThrowingAnalysis>(u), nullptr);
    EXPECT_THROW(am.getResult<ThrowingAnalysis>(u), std::runtime_error);
    EXPECT_EQ(g_analysisARuns, 2);
}
