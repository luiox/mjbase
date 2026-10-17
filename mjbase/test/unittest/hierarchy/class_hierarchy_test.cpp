/// @file        class_hierarchy_test.cpp
/// @brief       类层次图测试：链/接口/悬挂边查询、环拒绝、确定性。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hierarchy_test_util.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"

using namespace mjbase::hierarchy;
using mjbase::ClassKey;

namespace {

/// Object ← A ← B ← C 的标准链（登记序固定，查询断言依赖它）。
struct ChainFixture
{
    mjbase::KeyPool pool;
    ClassHierarchy h;
    ClassKey obj, a, b, c;

    ChainFixture()
        : pool(test::makePool())
        , h(pool)
    {
        EXPECT_TRUE(h.addClass(test::makeSummary(pool, "java/lang/Object", "")).is_ok());
        EXPECT_TRUE(h.addClass(test::makeSummary(pool, "A", "java/lang/Object")).is_ok());
        EXPECT_TRUE(h.addClass(test::makeSummary(pool, "B", "A")).is_ok());
        EXPECT_TRUE(h.addClass(test::makeSummary(pool, "C", "B")).is_ok());
        obj = h.findClass("java/lang/Object");
        a   = h.findClass("A");
        b   = h.findClass("B");
        c   = h.findClass("C");
        EXPECT_TRUE(obj.valid());
        EXPECT_TRUE(a.valid());
        EXPECT_TRUE(b.valid());
        EXPECT_TRUE(c.valid());
    }
};

bool keyListEquals(const std::vector<ClassKey>& got, const std::vector<ClassKey>& want)
{
    if (got.size() != want.size()) return false;
    for (ca::usize i = 0; i < got.size(); ++i) {
        if (!(got[i] == want[i])) return false;
    }
    return true;
}

}   // namespace

// ── 简单链：super/isSubclass/ancestors/commonAncestor/objectClass ──
TEST(MjHierarchyTest, SimpleChainQueries)
{
    ChainFixture f;

    EXPECT_EQ(f.h.classCount(), 4u);
    EXPECT_EQ(f.h.objectClass(), f.obj);

    EXPECT_FALSE(f.h.superClass(f.obj).valid());   // Object 无父类
    EXPECT_EQ(f.h.superClass(f.a), f.obj);
    EXPECT_EQ(f.h.superClass(f.b), f.a);
    EXPECT_EQ(f.h.superClass(f.c), f.b);

    // 自反
    EXPECT_TRUE(f.h.isSubclass(f.a, f.a));
    // 传递
    EXPECT_TRUE(f.h.isSubclass(f.a, f.obj));
    EXPECT_TRUE(f.h.isSubclass(f.b, f.obj));
    EXPECT_TRUE(f.h.isSubclass(f.c, f.obj));
    EXPECT_TRUE(f.h.isSubclass(f.b, f.a));
    EXPECT_TRUE(f.h.isSubclass(f.c, f.a));
    EXPECT_TRUE(f.h.isSubclass(f.c, f.b));
    // 反向不成立
    EXPECT_FALSE(f.h.isSubclass(f.a, f.b));
    EXPECT_FALSE(f.h.isSubclass(f.obj, f.a));

    // 最近公共祖先
    EXPECT_EQ(f.h.commonAncestor(f.b, f.c), f.b);
    EXPECT_EQ(f.h.commonAncestor(f.a, f.c), f.a);
    EXPECT_EQ(f.h.commonAncestor(f.obj, f.c), f.obj);
    EXPECT_EQ(f.h.commonAncestor(f.a, f.a), f.a);

    // 祖先序：自身最先，Object 殿后
    EXPECT_TRUE(keyListEquals(f.h.ancestors(f.obj), {f.obj}));
    EXPECT_TRUE(keyListEquals(f.h.ancestors(f.a), {f.a, f.obj}));
    EXPECT_TRUE(keyListEquals(f.h.ancestors(f.c), {f.c, f.b, f.a, f.obj}));

    // 名称与摘要
    EXPECT_EQ(f.h.nameOf(f.b), "B");
    ASSERT_NE(f.h.summary(f.b), nullptr);
    EXPECT_EQ(f.h.summary(f.b)->super, f.a);
    EXPECT_EQ(f.h.summary(ClassKey{}), nullptr);   // 无效键
}

// ── 后代：沿 super 边、登记序输出 ──
TEST(MjHierarchyTest, DescendantsFollowSuperEdgesInRegistrationOrder)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "java/lang/Object", "")).is_ok());
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "A", "java/lang/Object")).is_ok());
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "B", "A")).is_ok());
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "C", "A")).is_ok());
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "D", "B")).is_ok());

    const ClassKey a = h.findClass("A");
    const ClassKey b = h.findClass("B");
    const ClassKey d = h.findClass("D");

    EXPECT_TRUE(keyListEquals(h.descendants(a), {h.findClass("B"), h.findClass("C"),
                                                 h.findClass("D")}));
    EXPECT_TRUE(keyListEquals(h.descendants(b), {d}));
    EXPECT_TRUE(h.descendants(d).empty());
    EXPECT_TRUE(h.descendants(ClassKey{}).empty());
}

// ── 接口：isSubtype 含接口闭包，isSubclass 仅 extends ──
TEST(MjHierarchyTest, InterfaceClosureAndSubtype)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "java/lang/Object", "")).is_ok());
    ASSERT_TRUE(
        h.addClass(test::makeSummary(pool, "I", "java/lang/Object", kAccInterface | kAccAbstract)).is_ok());
    ASSERT_TRUE(
        h.addClass(test::makeSummary(pool, "J", "java/lang/Object", kAccInterface | kAccAbstract)).is_ok());

    ClassSummary implOfI = test::makeSummary(pool, "ImplI", "java/lang/Object");
    implOfI.interfaces.push_back(mjbase::classKeyOf(pool, "I"));
    ASSERT_TRUE(h.addClass(implOfI).is_ok());

    ClassSummary implOfJ = test::makeSummary(pool, "ImplJ", "java/lang/Object");
    implOfJ.interfaces.push_back(mjbase::classKeyOf(pool, "J"));
    implOfJ.interfaces.push_back(mjbase::classKeyOf(pool, "I"));   // 双接口
    ASSERT_TRUE(h.addClass(implOfJ).is_ok());

    const ClassKey i     = h.findClass("I");
    const ClassKey j     = h.findClass("J");
    const ClassKey implI = h.findClass("ImplI");
    const ClassKey implJ = h.findClass("ImplJ");

    EXPECT_TRUE(h.isInterface(i));
    EXPECT_FALSE(h.isInterface(implI));

    // 直接接口
    const auto& ifs = h.interfaces(implI);
    ASSERT_EQ(ifs.size(), 1u);
    EXPECT_EQ(ifs[0], i);
    EXPECT_TRUE(h.interfaces(ClassKey{}).empty());

    // 子类型语义：接口闭包参与，extends 链不参与
    EXPECT_TRUE(h.isSubtype(implI, i));
    EXPECT_FALSE(h.isSubclass(implI, i));
    EXPECT_FALSE(h.isSubtype(i, implI));

    // 双接口闭包：BFS 序（声明序）
    EXPECT_TRUE(keyListEquals(h.interfaceClosure(implJ), {j, i}));
    EXPECT_TRUE(h.isSubtype(implJ, i));
    EXPECT_TRUE(h.isSubtype(implJ, j));
}

// ── 接口传递：J 的超接口 I 对 K 可见 ──
TEST(MjHierarchyTest, TransitiveSuperinterfaceVisibleInClosure)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "java/lang/Object", "")).is_ok());

    ClassSummary i = test::makeSummary(pool, "I", "java/lang/Object", kAccInterface);
    ASSERT_TRUE(h.addClass(i).is_ok());

    ClassSummary j = test::makeSummary(pool, "J", "java/lang/Object", kAccInterface);
    j.interfaces.push_back(h.findClass("I"));
    ASSERT_TRUE(h.addClass(j).is_ok());

    ClassSummary k = test::makeSummary(pool, "K", "java/lang/Object");
    k.interfaces.push_back(h.findClass("J"));
    ASSERT_TRUE(h.addClass(k).is_ok());

    const ClassKey iKey = h.findClass("I");
    const ClassKey jKey = h.findClass("J");
    const ClassKey kKey = h.findClass("K");

    // J 的闭包含其超接口 I；K 经 J 见到 I
    EXPECT_TRUE(keyListEquals(h.interfaceClosure(jKey), {iKey}));
    EXPECT_TRUE(keyListEquals(h.interfaceClosure(kKey), {jKey, iKey}));
    EXPECT_TRUE(h.isSubtype(kKey, iKey));
}

// ── 悬挂父类：名字驻留但未登记 → 链上计入后截断 ──
TEST(MjHierarchyTest, DanglingSuperKeyTruncatesChain)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    const ClassKey missing = mjbase::classKeyOf(pool, "Missing");

    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "A", "Missing")).is_ok());
    const ClassKey a = h.findClass("A");

    EXPECT_FALSE(h.contains(missing));
    EXPECT_EQ(h.superClass(a), missing);
    EXPECT_TRUE(keyListEquals(h.ancestors(a), {a, missing}));
    EXPECT_TRUE(h.isSubclass(a, missing));

    // 两棵无交集的树（无共同根）→ 无公共祖先
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "R", "")).is_ok());
    const ClassKey r = h.findClass("R");
    EXPECT_FALSE(h.commonAncestor(a, r).valid());
}

// ── 畸形输入：环/重复/坏键被拒绝，图保持原状 ──
TEST(MjHierarchyTest, MalformedEdgesRejected)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);

    // 自指父类
    {
        const auto r = h.addClass(test::makeSummary(pool, "Self", "Self"));
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), HierarchyError::Cycle);
    }
    // 直接环：先 B(super=A 悬挂)，后 A(super=B) 闭合 → 在闭合边处拒绝
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "B", "A")).is_ok());
    {
        const auto r = h.addClass(test::makeSummary(pool, "A", "B"));
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), HierarchyError::Cycle);
        // A 的名字已驻留但未登记，图大小不变
        EXPECT_FALSE(h.contains(mjbase::classKeyOf(pool, "A")));
        EXPECT_EQ(h.classCount(), 1u);
    }
    // 拒绝环闭合后图仍可用：继续加深链条（B←C←D）
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "C", "B")).is_ok());
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "D", "C")).is_ok());
    // 重复登记
    {
        const auto r = h.addClass(test::makeSummary(pool, "B", "A"));
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), HierarchyError::DuplicateClass);
    }
    // 坏键：self 未设置
    {
        ClassSummary bad = test::makeSummary(pool, "X", "B");
        bad.self         = ClassKey{};
        const auto r     = h.addClass(bad);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), HierarchyError::BadKey);
    }
    // 拒绝后查询不挂死、语义完好
    EXPECT_EQ(h.classCount(), 3u);
    const ClassKey b = h.findClass("B");
    const ClassKey c = h.findClass("C");
    const ClassKey d = h.findClass("D");
    EXPECT_TRUE(h.isSubclass(d, b));
    EXPECT_TRUE(h.validate().is_ok());
}

// ── 接口环：J 与 I 互为超接口 → 闭合边处拒绝 ──
TEST(MjHierarchyTest, InterfaceCycleRejected)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "java/lang/Object", "")).is_ok());

    ClassSummary i = test::makeSummary(pool, "I", "java/lang/Object", kAccInterface);
    i.interfaces.push_back(mjbase::classKeyOf(pool, "J"));   // J 未登记：悬挂
    ASSERT_TRUE(h.addClass(i).is_ok());

    ClassSummary j = test::makeSummary(pool, "J", "java/lang/Object", kAccInterface);
    j.interfaces.push_back(h.findClass("I"));   // 闭合 I→J→I
    const auto r = h.addClass(j);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), HierarchyError::Cycle);
    EXPECT_EQ(h.classCount(), 2u);
}

// ── 自指接口 ──
TEST(MjHierarchyTest, SelfInterfaceRejected)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    ClassSummary i = test::makeSummary(pool, "I", "java/lang/Object", kAccInterface);
    i.interfaces.push_back(mjbase::classKeyOf(pool, "I"));
    const auto r = h.addClass(i);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), HierarchyError::Cycle);
}

// ── 深链：1200 层迭代遍历（无递归栈风险），祖先序与子类判定稳定 ──
TEST(MjHierarchyTest, LongChainStaysIterativeAndConsistent)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    ASSERT_TRUE(h.addClass(test::makeSummary(pool, "java/lang/Object", "")).is_ok());

    constexpr int kDepth = 1200;
    std::string previous = "java/lang/Object";
    for (int i = 0; i < kDepth; ++i) {
        const std::string name = "Chain" + std::to_string(i);
        ASSERT_TRUE(h.addClass(test::makeSummary(pool, name, previous)).is_ok());
        previous = name;
    }

    const ClassKey first = h.findClass("Chain0");
    const ClassKey last  = h.findClass("Chain1199");
    ASSERT_TRUE(first.valid());
    ASSERT_TRUE(last.valid());

    EXPECT_TRUE(h.isSubclass(last, first));
    EXPECT_EQ(h.commonAncestor(first, last), first);

    const auto ancestors = h.ancestors(last);
    ASSERT_EQ(ancestors.size(), static_cast<ca::usize>(kDepth + 1));
    EXPECT_EQ(ancestors[0], last);
    EXPECT_EQ(ancestors[1], h.findClass("Chain1198"));
    EXPECT_EQ(ancestors[ancestors.size() - 1], h.objectClass());
}
