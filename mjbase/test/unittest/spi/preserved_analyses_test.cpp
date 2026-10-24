// 断言强度原样保留。
#include <gtest/gtest.h>

#include "mjbase/spi/preserved_analyses.hpp"

using namespace mjbase::spi;

namespace {

// 测试用的虚拟分析类型（只需类型本身，不需实现）
struct AnalysisA
{};
struct AnalysisB
{};
struct AnalysisC
{};

}   // namespace

// ── analysisID：每类型唯一、同类型稳定 ──
TEST(PreservedAnalysesTest, analysisIDUniqueAndStable)
{
    EXPECT_EQ(analysisID<AnalysisA>(), analysisID<AnalysisA>());   // 同类型稳定
    EXPECT_NE(analysisID<AnalysisA>(), analysisID<AnalysisB>());   // 不同类型不同
    EXPECT_NE(analysisID<AnalysisB>(), analysisID<AnalysisC>());
}

// ── all()：保留一切 ──
TEST(PreservedAnalysesTest, allPreservesEverything)
{
    auto pa = PreservedAnalyses::all();
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_TRUE(pa.isPreserved<AnalysisA>());
    EXPECT_TRUE(pa.isPreserved<AnalysisB>());
}

// ── none()：什么都不保留 ──
TEST(PreservedAnalysesTest, nonePreservesNothing)
{
    auto pa = PreservedAnalyses::none();
    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_FALSE(pa.isPreserved<AnalysisA>());
}

// ── preserve<A>()：逐个保留 ──
TEST(PreservedAnalysesTest, preserveIndividual)
{
    auto pa = PreservedAnalyses::none();
    pa.preserve<AnalysisA>();
    EXPECT_TRUE(pa.isPreserved<AnalysisA>());
    EXPECT_FALSE(pa.isPreserved<AnalysisB>());   // 没保留的仍失效
}

// ── all 状态下 preserve 是 no-op（all 已蕴含全部）──
TEST(PreservedAnalysesTest, preserveOnAllIsNoop)
{
    auto pa = PreservedAnalyses::all();
    pa.preserve<AnalysisA>();
    EXPECT_TRUE(pa.areAllPreserved());          // 仍是 all
    EXPECT_TRUE(pa.isPreserved<AnalysisB>());   // 未显式 preserve 的也保留
}

// ── markChanged：改动信号与分析保留正交 ──
TEST(PreservedAnalysesTest, markChangedDecouplesFromPreservation)
{
    auto pa = PreservedAnalyses::all();
    EXPECT_FALSE(pa.changed());              // all 默认未改动
    pa.markChanged();
    EXPECT_TRUE(pa.changed());               // 显式标记改动
    EXPECT_TRUE(pa.areAllPreserved());       // 保留集合不受影响
    EXPECT_TRUE(PreservedAnalyses::none().changed());   // none 默认已改动
}

// ── intersect：other=all 不改变自身 ──
TEST(PreservedAnalysesTest, intersectWithAllKeepsSelf)
{
    auto pa = PreservedAnalyses::none();
    pa.preserve<AnalysisA>();
    pa.intersect(PreservedAnalyses::all());
    EXPECT_TRUE(pa.isPreserved<AnalysisA>());
    EXPECT_FALSE(pa.isPreserved<AnalysisB>());
}

// ── intersect：self=all 时取 other ──
TEST(PreservedAnalysesTest, intersectAllWithSpecific)
{
    auto pa    = PreservedAnalyses::all();
    auto other = PreservedAnalyses::none();
    other.preserve<AnalysisA>();
    pa.intersect(other);
    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(pa.isPreserved<AnalysisA>());
    EXPECT_FALSE(pa.isPreserved<AnalysisB>());
}

// ── intersect：两个具体集合取交 ──
TEST(PreservedAnalysesTest, intersectTwoSpecificSets)
{
    auto a = PreservedAnalyses::none();
    a.preserve<AnalysisA>();
    a.preserve<AnalysisB>();

    auto b = PreservedAnalyses::none();
    b.preserve<AnalysisB>();
    b.preserve<AnalysisC>();

    a.intersect(b);
    EXPECT_FALSE(a.isPreserved<AnalysisA>());   // 只在 a → 失效
    EXPECT_TRUE(a.isPreserved<AnalysisB>());    // 两边都有 → 保留
    EXPECT_FALSE(a.isPreserved<AnalysisC>());   // 只在 b → 失效
}

// ── intersect：任一 pass 失效某分析则整体失效（聚合语义）──
TEST(PreservedAnalysesTest, intersectNoneClearsAll)
{
    auto a = PreservedAnalyses::none();
    a.preserve<AnalysisA>();
    a.intersect(PreservedAnalyses::none());
    EXPECT_FALSE(a.isPreserved<AnalysisA>());
}

// ── intersect：改动信号取并集（all 但改了 IR 的 pass 不丢 changed 位）──
TEST(PreservedAnalysesTest, intersectPropagatesChangedBit)
{
    auto touching = PreservedAnalyses::all().markChanged();
    auto readonly = PreservedAnalyses::all();
    readonly.intersect(touching);
    EXPECT_TRUE(readonly.changed());   // 早退分支前必须已传播
}
