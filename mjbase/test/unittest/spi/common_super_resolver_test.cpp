// CommonSuperClassResolver：公共父类求解适配件的行为契约（挂 ClassHierarchy，
// 不可判定回退语义逐条验收；super-chain-only 与旧 provider 同构）。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/spi/common_super_resolver.hpp"

using namespace mjbase::spi;
using mjbase::ClassKey;
using mjbase::KeyPool;

namespace {

/// 手工类摘要（无成员；super 留空 = Object 形态）。
mjbase::hierarchy::ClassSummary makeSummary(KeyPool& pool, std::string_view self,
                                            std::string_view super)
{
    mjbase::hierarchy::ClassSummary s;
    s.self  = mjbase::classKeyOf(pool, self);
    s.super = super.empty() ? ClassKey{} : mjbase::classKeyOf(pool, super);
    return s;
}

/// Object ← A ← B；Base ← Z；Iface（接口）；X implements Iface。
struct HierarchyFixture
{
    KeyPool pool;
    mjbase::hierarchy::ClassHierarchy h{pool};
    CommonSuperClassResolver resolver{h};

    HierarchyFixture()
    {
        EXPECT_TRUE(h.addClass(makeSummary(pool, "java/lang/Object", "")).is_ok());
        EXPECT_TRUE(h.addClass(makeSummary(pool, "A", "java/lang/Object")).is_ok());
        EXPECT_TRUE(h.addClass(makeSummary(pool, "B", "A")).is_ok());
        EXPECT_TRUE(h.addClass(makeSummary(pool, "Base", "java/lang/Object")).is_ok());
        EXPECT_TRUE(h.addClass(makeSummary(pool, "Z", "Base")).is_ok());
        EXPECT_TRUE(h.addClass(makeSummary(pool, "Iface", "java/lang/Object")).is_ok());
        // X implements Iface（接口边走 isSubtype，不进 super 链）
        auto x = makeSummary(pool, "X", "java/lang/Object");
        x.interfaces.push_back(mjbase::classKeyOf(pool, "Iface"));
        EXPECT_TRUE(h.addClass(x).is_ok());
    }
};

}   // namespace

// ── super 链上的正常求解（与旧 provider 的 root-first 首段比较同构）──
TEST(CommonSuperResolverTest, commonAlongSuperChain)
{
    HierarchyFixture fx;
    EXPECT_EQ(fx.resolver.getCommonSuperClass("B", "A"), "A");
    EXPECT_EQ(fx.resolver.getCommonSuperClass("A", "B"), "A");   // 对称
    EXPECT_EQ(fx.resolver.getCommonSuperClass("B", "java/lang/Object"), "java/lang/Object");
    EXPECT_EQ(fx.resolver.getCommonSuperClass("B", "B"), "B");   // 自身
    EXPECT_EQ(fx.resolver.getCommonSuperClass("Z", "A"), "java/lang/Object");
}

// ── 不可判定回退 1：任一类型未登记 → fallback ──
TEST(CommonSuperResolverTest, unknownTypeFallsBack)
{
    HierarchyFixture fx;
    EXPECT_EQ(fx.resolver.getCommonSuperClass("B", "no/Such"), "java/lang/Object");
    EXPECT_EQ(fx.resolver.getCommonSuperClass("no/Such", "also/Missing"), "java/lang/Object");
}

// ── 不可判定回退 2：悬挂键截断 → 两链无交 → fallback ──
TEST(CommonSuperResolverTest, hangingSuperChainFallsBack)
{
    HierarchyFixture fx;
    // Hang 的 super "Ghost" 未登记（悬挂键）：链 [Hang, Ghost] 与 [A, Object] 无交。
    EXPECT_TRUE(fx.h.addClass(makeSummary(fx.pool, "Hang", "Ghost")).is_ok());
    EXPECT_EQ(fx.resolver.getCommonSuperClass("Hang", "A"), "java/lang/Object");
}

// ── 接口维度从不参与（super-chain-only，与旧 provider 一致）──
TEST(CommonSuperResolverTest, interfaceEdgesAreNotConsulted)
{
    HierarchyFixture fx;
    // X implements Iface（Iface extends Object），Z extends Base：
    // X 与 Z 的 super 链只交于 Object——Iface/Base 的接口语义不改变结果。
    EXPECT_EQ(fx.resolver.getCommonSuperClass("X", "Z"), "java/lang/Object");
    EXPECT_EQ(fx.resolver.getCommonSuperClass("X", "B"), "java/lang/Object");
}

// ── 查询缓存：归一化 (t1,t2)/(t2,t1) 等价对命中 ──
TEST(CommonSuperResolverTest, symmetricPairHitsCache)
{
    HierarchyFixture fx;
    EXPECT_EQ(fx.resolver.getCommonSuperClass("B", "A"), "A");
    EXPECT_EQ(fx.resolver.cachedQueryCount(), 1u);
    EXPECT_EQ(fx.resolver.getCommonSuperClass("A", "B"), "A");   // 反序命中同一缓存项
    EXPECT_EQ(fx.resolver.cachedQueryCount(), 1u);
    EXPECT_EQ(fx.resolver.getCommonSuperClass("B", "B"), "B");
    EXPECT_EQ(fx.resolver.cachedQueryCount(), 2u);
}

// ── fallback 可改设；改设作废缓存 ──
TEST(CommonSuperResolverTest, setFallbackReplacesAndClearsCache)
{
    HierarchyFixture fx;
    EXPECT_EQ(fx.resolver.getCommonSuperClass("no/Such1", "no/Such2"), "java/lang/Object");
    fx.resolver.setFallback("java/lang/Object");
    EXPECT_EQ(fx.resolver.cachedQueryCount(), 0u);
    EXPECT_EQ(fx.resolver.getCommonSuperClass("no/Such1", "no/Such2"), "java/lang/Object");

    // 换一个已登记的 fallback：不可判定路径返回新 fallback。
    fx.resolver.setFallback("Base");
    EXPECT_EQ(fx.resolver.getCommonSuperClass("no/Such1", "no/Such2"), "Base");
}

// ── 结果视图稳定：指向驻留池，重复查询视图一致 ──
TEST(CommonSuperResolverTest, resultViewIsStablePoolBacked)
{
    HierarchyFixture fx;
    auto first  = fx.resolver.getCommonSuperClass("B", "A");
    auto second = fx.resolver.getCommonSuperClass("B", "A");
    EXPECT_EQ(first.data(), second.data());   // 缓存命中路径同视图
    EXPECT_EQ(first, "A");
}
