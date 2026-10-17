/// @file        field_hierarchy_test.cpp
/// @brief       字段层次测试：沿链最近声明解析（找到定义类）与实例字段遮蔽。
#include <gtest/gtest.h>

#include "hierarchy_test_util.hpp"
#include "mjbase/hierarchy/field_hierarchy.hpp"

using namespace mjbase::hierarchy;
using mjbase::ClassKey;
using mjbase::MemberKey;

namespace {

/// Object ← A ← B 的两字段链建造。
void buildShadowGraph(mjbase::KeyPool& pool, ClassHierarchy& h, ca::u32 bFieldAccess,
                      ca::u32 aFieldAccess)
{
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "java/lang/Object", "")).is_ok());

    ClassSummary a = test::makeSummary(pool, "A", "java/lang/Object");
    a.fields.push_back(test::makeMember(pool, "x", "I", aFieldAccess));
    ASSERT_TRUE(h.addClass(a).is_ok());

    ClassSummary b = test::makeSummary(pool, "B", "A");
    b.fields.push_back(test::makeMember(pool, "x", "I", bFieldAccess));
    ASSERT_TRUE(h.addClass(b).is_ok());
}

}   // namespace

// ── 同名实例字段：B.x shadow A.x；解析取最近声明（定义类）──
TEST(MjHierarchyTest, FieldShadowResolvesToNearestDeclaration)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    buildShadowGraph(pool, h, kAccPublic, kAccPublic);

    FieldHierarchy fh;
    fh.build(h);
    EXPECT_EQ(fh.fieldCount(), 2u);

    const ClassKey a = h.findClass("A");
    const ClassKey b = h.findClass("B");
    const MemberKey aX = mjbase::memberKeyOf(pool, a, "x", "I");
    const MemberKey bX = mjbase::memberKeyOf(pool, b, "x", "I");

    // B 视角解析到 B 自己的声明（最近者胜出）
    const auto r = fh.resolveField(b, aX.name, aX.desc);
    EXPECT_EQ(r.status, FieldHierarchy::FieldResolution::Status::Found);
    EXPECT_EQ(r.target, bX);

    // shadow 判定（旧 isSourceField 的反面）：B.x 遮蔽 A.x，A.x 无遮蔽
    EXPECT_TRUE(fh.shadowsAncestor(bX));
    EXPECT_FALSE(fh.shadowsAncestor(aX));
}

// ── 继承字段：子类未声明 → 解析找到父类定义 ──
TEST(MjHierarchyTest, InheritedFieldResolvesToDefiningClass)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "java/lang/Object", "")).is_ok());

    ClassSummary a = test::makeSummary(pool, "A", "java/lang/Object");
    a.fields.push_back(test::makeMember(pool, "x", "I", kAccPublic));
    ASSERT_TRUE(h.addClass(a).is_ok());

    ClassSummary b = test::makeSummary(pool, "B", "A");   // B 不声明 x
    b.fields.push_back(test::makeMember(pool, "y", "J", kAccPublic));
    ASSERT_TRUE(h.addClass(b).is_ok());

    FieldHierarchy fh;
    fh.build(h);

    const ClassKey aKey = h.findClass("A");
    const ClassKey bKey = h.findClass("B");
    const MemberKey aX = mjbase::memberKeyOf(pool, aKey, "x", "I");

    const auto r = fh.resolveField(bKey, aX.name, aX.desc);
    EXPECT_EQ(r.status, FieldHierarchy::FieldResolution::Status::Found);
    EXPECT_EQ(r.target, aX);

    // B 自己的字段解析到自身；y 在链上唯一
    const MemberKey bY = mjbase::memberKeyOf(pool, bKey, "y", "J");
    const auto ry = fh.resolveField(bKey, bY.name, bY.desc);
    EXPECT_EQ(ry.status, FieldHierarchy::FieldResolution::Status::Found);
    EXPECT_EQ(ry.target, bY);
    EXPECT_FALSE(fh.shadowsAncestor(bY));
}

// ── 父类 static 同名字段不遮蔽子类实例字段（旧 isSourceField 语义）──
TEST(MjHierarchyTest, StaticAncestorFieldDoesNotShadow)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    buildShadowGraph(pool, h, kAccPublic, kAccPublic | kAccStatic);

    FieldHierarchy fh;
    fh.build(h);

    const MemberKey bX = mjbase::memberKeyOf(pool, h.findClass("B"), "x", "I");
    EXPECT_FALSE(fh.shadowsAncestor(bX));

    // 解析不受访问位影响：仍然取最近声明
    const auto r = fh.resolveField(h.findClass("B"), bX.name, bX.desc);
    EXPECT_EQ(r.target, bX);
}

// ── static 自身字段恒不 shadow（但解析可命中）──
TEST(MjHierarchyTest, StaticOwnFieldNeverShadows)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    buildShadowGraph(pool, h, kAccPublic | kAccStatic, kAccPublic);

    FieldHierarchy fh;
    fh.build(h);

    const MemberKey bX = mjbase::memberKeyOf(pool, h.findClass("B"), "x", "I");
    EXPECT_FALSE(fh.shadowsAncestor(bX));
    EXPECT_EQ(fh.resolveField(h.findClass("B"), bX.name, bX.desc).target, bX);
}

// ── 未声明/未知 receiver：NotFound，不挂死 ──
TEST(MjHierarchyTest, FieldResolutionNotFoundCases)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    buildShadowGraph(pool, h, kAccPublic, kAccPublic);

    FieldHierarchy fh;
    fh.build(h);

    const ClassKey b = h.findClass("B");
    const auto miss = fh.resolveField(b, pool.intern("zz"), pool.intern("I"));
    EXPECT_EQ(miss.status, FieldHierarchy::FieldResolution::Status::NotFound);

    const auto unknown = fh.resolveField(mjbase::classKeyOf(pool, "NotRegistered"),
                                         pool.intern("x"), pool.intern("I"));
    EXPECT_EQ(unknown.status, FieldHierarchy::FieldResolution::Status::NotFound);

    // 悬挂父类截断：D extends Missing（未登记），链上无声明
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "D", "Missing")).is_ok());
    const auto dangling = fh.resolveField(h.findClass("D"), pool.intern("x"),
                                          pool.intern("I"));
    EXPECT_EQ(dangling.status, FieldHierarchy::FieldResolution::Status::NotFound);

    // 未声明的键：shadowsAncestor false
    EXPECT_FALSE(fh.shadowsAncestor(mjbase::memberKeyOf(pool, h.findClass("A"),
                                                        "nope", "I")));
}

// ── 长链上的多级遮蔽：最深的声明胜出，逐级 shadow ──
TEST(MjHierarchyTest, DeepChainFieldResolution)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "java/lang/Object", "")).is_ok());

    ClassSummary a = test::makeSummary(pool, "A", "java/lang/Object");
    a.fields.push_back(test::makeMember(pool, "v", "I", kAccPublic));
    ASSERT_TRUE(h.addClass(a).is_ok());

    ClassSummary b = test::makeSummary(pool, "B", "A");
    b.fields.push_back(test::makeMember(pool, "v", "I", kAccPublic));
    ASSERT_TRUE(h.addClass(b).is_ok());

    ClassSummary c = test::makeSummary(pool, "C", "B");   // C 不声明 v
    ASSERT_TRUE(h.addClass(c).is_ok());

    FieldHierarchy fh;
    fh.build(h);

    const ClassKey aK = h.findClass("A");
    const ClassKey bK = h.findClass("B");
    const ClassKey cK = h.findClass("C");
    const MemberKey aV = mjbase::memberKeyOf(pool, aK, "v", "I");
    const MemberKey bV = mjbase::memberKeyOf(pool, bK, "v", "I");

    EXPECT_EQ(fh.resolveField(cK, aV.name, aV.desc).target, bV);   // 越过 C 到 B
    EXPECT_TRUE(fh.shadowsAncestor(bV));
    EXPECT_FALSE(fh.shadowsAncestor(aV));
}
