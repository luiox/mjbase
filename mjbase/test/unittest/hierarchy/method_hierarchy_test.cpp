/// @file        method_hierarchy_test.cpp
/// @brief       方法层次测试：override 族、源声明、单继承链虚分派。
#include <gtest/gtest.h>

#include <vector>

#include "hierarchy_test_util.hpp"
#include "mjbase/hierarchy/method_hierarchy.hpp"

using namespace mjbase::hierarchy;
using mjbase::ClassKey;
using mjbase::MemberKey;

namespace {

/// 一张图的建造助手：链式登记（名字, super, 方法列表, 类 access）。
class GraphBuilder
{
public:
    struct M
    {
        std::string_view name;
        std::string_view desc;
        ca::u32 access;
    };

    explicit GraphBuilder(mjbase::KeyPool& pool, ClassHierarchy& h)
        : pool_(pool)
        , h_(h)
    {}

    GraphBuilder& cls(std::string_view self, std::string_view super,
                      std::initializer_list<M> methods, ca::u32 access = kAccPublic)
    {
        return clsWithIfaces(self, super, {}, methods, access);
    }

    GraphBuilder& clsWithIfaces(std::string_view self, std::string_view super,
                                std::initializer_list<std::string_view> ifaces,
                                std::initializer_list<M> methods, ca::u32 access = kAccPublic)
    {
        ClassSummary s = test::makeSummary(pool_, self, super, access);
        for (const std::string_view i : ifaces) {
            s.interfaces.push_back(mjbase::classKeyOf(pool_, i));
        }
        for (const M& m : methods) {
            s.methods.push_back(test::makeMember(pool_, m.name, m.desc, m.access));
        }
        const auto r = h_.addClass(s);
        if (r.is_err()) {
            ADD_FAILURE() << "addClass failed: " << to_string(r.unwrap_err());
        }
        return *this;
    }

private:
    mjbase::KeyPool& pool_;
    ClassHierarchy& h_;
};

bool memberListEquals(const std::vector<MemberKey>& got, const std::vector<MemberKey>& want)
{
    if (got.size() != want.size()) return false;
    for (ca::usize i = 0; i < got.size(); ++i) {
        if (!(got[i] == want[i])) return false;
    }
    return true;
}

}   // namespace

// ── 简单 override 链：族连通（含后代方向）、源声明取最浅 ──
TEST(MjHierarchyTest, OverrideChainConnectsAncestorsAndDescendants)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("A", "java/lang/Object", {{"foo", "()V", kAccPublic}})
        .cls("B", "A", {{"foo", "()V", kAccPublic}});

    MethodHierarchy mh;
    mh.build(h);
    EXPECT_EQ(mh.methodCount(), 2u);

    const ClassKey a = h.findClass("A");
    const ClassKey b = h.findClass("B");
    const MemberKey aFoo = mjbase::memberKeyOf(pool, a, "foo", "()V");
    const MemberKey bFoo = mjbase::memberKeyOf(pool, b, "foo", "()V");

    // 等价族：从任一端看都是两个成员（旧 componentMethods 语义）
    EXPECT_TRUE(memberListEquals(mh.overrideFamily(bFoo), {aFoo, bFoo}));
    EXPECT_TRUE(memberListEquals(mh.overrideFamily(aFoo), {aFoo, bFoo}));

    // 源声明（旧 sourceMethod）：最浅者
    EXPECT_EQ(mh.sourceDeclaration(aFoo), aFoo);
    EXPECT_EQ(mh.sourceDeclaration(bFoo), aFoo);

    EXPECT_TRUE(mh.isVirtual(aFoo));
}

// ── private 方法各自独立成族 ──
TEST(MjHierarchyTest, PrivateMethodsFormSingletonFamilies)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("A", "java/lang/Object", {{"foo", "()V", kAccPrivate}})
        .cls("B", "A", {{"foo", "()V", kAccPrivate}});

    MethodHierarchy mh;
    mh.build(h);
    EXPECT_EQ(mh.methodCount(), 2u);

    const MemberKey aFoo = mjbase::memberKeyOf(pool, h.findClass("A"), "foo", "()V");
    const MemberKey bFoo = mjbase::memberKeyOf(pool, h.findClass("B"), "foo", "()V");

    EXPECT_TRUE(memberListEquals(mh.overrideFamily(aFoo), {aFoo}));
    EXPECT_TRUE(memberListEquals(mh.overrideFamily(bFoo), {bFoo}));
    EXPECT_NE(mh.sourceDeclaration(aFoo), mh.sourceDeclaration(bFoo));
    EXPECT_FALSE(mh.isVirtual(aFoo));
}

// ── static 与 '<init>'/'<clinit>' 不参与多态 ──
TEST(MjHierarchyTest, StaticAndCtorStaySingletons)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("A", "java/lang/Object",
             {{"<init>", "()V", 0},
              {"<clinit>", "()V", kAccStatic},
              {"stat", "()V", kAccPublic | kAccStatic}})
        .cls("B", "A",
             {{"<init>", "()V", 0},
              {"<clinit>", "()V", kAccStatic},
              {"stat", "()V", kAccPublic | kAccStatic}});

    MethodHierarchy mh;
    mh.build(h);
    EXPECT_EQ(mh.methodCount(), 6u);

    const ClassKey a = h.findClass("A");
    const ClassKey b = h.findClass("B");
    for (const std::string_view name : {"<init>", "<clinit>", "stat"}) {
        const MemberKey am = mjbase::memberKeyOf(pool, a, name, "()V");
        const MemberKey bm = mjbase::memberKeyOf(pool, b, name, "()V");
        EXPECT_TRUE(memberListEquals(mh.overrideFamily(am), {am})) << name;
        EXPECT_TRUE(memberListEquals(mh.overrideFamily(bm), {bm})) << name;
        EXPECT_FALSE(mh.isVirtual(am)) << name;
    }
}

// ── 接口方法与实现连通；源声明接口所有者优先 ──
TEST(MjHierarchyTest, InterfaceImplementationJoinsFamily)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("I", "java/lang/Object", {{"foo", "()V", kAccPublic | kAccAbstract}},
             kAccInterface | kAccAbstract)
        .clsWithIfaces("Impl", "java/lang/Object", {"I"}, {{"foo", "()V", kAccPublic}});

    MethodHierarchy mh;
    mh.build(h);
    EXPECT_EQ(mh.methodCount(), 2u);

    const ClassKey i     = h.findClass("I");
    const ClassKey impl  = h.findClass("Impl");
    const MemberKey iFoo    = mjbase::memberKeyOf(pool, i, "foo", "()V");
    const MemberKey implFoo = mjbase::memberKeyOf(pool, impl, "foo", "()V");

    EXPECT_TRUE(memberListEquals(mh.overrideFamily(implFoo), {iFoo, implFoo}));
    EXPECT_TRUE(memberListEquals(mh.overrideFamily(iFoo), {iFoo, implFoo}));
    // 同深度（都直接继承 Object）→ 接口所有者优先（旧 sourceMethod 语义）
    EXPECT_EQ(mh.sourceDeclaration(implFoo), iFoo);
    EXPECT_EQ(mh.sourceDeclaration(iFoo), iFoo);
}

// ── 族含多级链 + 接口：A 定义、B/C 覆盖、I 声明，全部连通 ──
TEST(MjHierarchyTest, DeepFamilyIncludesChainAndInterface)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("I", "java/lang/Object", {{"m", "()I", kAccPublic | kAccAbstract}},
             kAccInterface | kAccAbstract)
        .clsWithIfaces("A", "java/lang/Object", {"I"}, {{"m", "()I", kAccPublic}})
        .cls("B", "A", {{"m", "()I", kAccPublic}})
        .cls("C", "B", {{"m", "()I", kAccPublic}});

    MethodHierarchy mh;
    mh.build(h);

    const ClassKey i = h.findClass("I");
    const ClassKey a = h.findClass("A");
    const ClassKey b = h.findClass("B");
    const ClassKey c = h.findClass("C");
    const MemberKey iM = mjbase::memberKeyOf(pool, i, "m", "()I");
    const MemberKey aM = mjbase::memberKeyOf(pool, a, "m", "()I");
    const MemberKey bM = mjbase::memberKeyOf(pool, b, "m", "()I");
    const MemberKey cM = mjbase::memberKeyOf(pool, c, "m", "()I");

    EXPECT_TRUE(memberListEquals(mh.overrideFamily(cM), {iM, aM, bM, cM}));
    EXPECT_EQ(mh.sourceDeclaration(cM), iM);   // 深度并列时接口优先于类
}

// ── 未声明的方法：空族、全零源声明、非虚判定 ──
TEST(MjHierarchyTest, UnknownMethodYieldsEmptyFamily)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {}).cls("A", "java/lang/Object", {{"foo", "()V", kAccPublic}});

    MethodHierarchy mh;
    mh.build(h);

    const ClassKey a = h.findClass("A");
    const MemberKey absent = mjbase::memberKeyOf(pool, a, "nope", "()V");
    EXPECT_TRUE(mh.overrideFamily(absent).empty());
    EXPECT_EQ(mh.sourceDeclaration(absent), MemberKey{});
    EXPECT_FALSE(mh.isVirtual(absent));

    // 视点语义：owner 未声明该签名时族为空（分派/解析另行走链）
    const ClassKey obj = h.findClass("java/lang/Object");
    const MemberKey view = mjbase::memberKeyOf(pool, obj, "foo", "()V");
    EXPECT_TRUE(mh.overrideFamily(view).empty());
}

// ── 虚分派：类链最近虚声明胜出 ──
TEST(MjHierarchyTest, VirtualDispatchResolvesNearest)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("A", "java/lang/Object", {{"foo", "()V", kAccPublic}})
        .cls("B", "A", {{"foo", "()V", kAccPublic}})
        .cls("C", "B", {{"bar", "()V", kAccPublic}});

    MethodHierarchy mh;
    mh.build(h);

    const ClassKey a = h.findClass("A");
    const ClassKey b = h.findClass("B");
    const ClassKey c = h.findClass("C");
    const MemberKey aFoo = mjbase::memberKeyOf(pool, a, "foo", "()V");
    const MemberKey bFoo = mjbase::memberKeyOf(pool, b, "foo", "()V");

    // receiver B：B 自身声明最近
    auto r1 = mh.virtualDispatch(b, aFoo);
    EXPECT_EQ(r1.status, DispatchResult::Status::Resolved);
    EXPECT_EQ(r1.target, bFoo);

    // receiver C（未覆盖）：上行到 B
    auto r2 = mh.virtualDispatch(c, aFoo);
    EXPECT_EQ(r2.status, DispatchResult::Status::Resolved);
    EXPECT_EQ(r2.target, bFoo);

    // 链上无此签名
    auto r3 = mh.virtualDispatch(c, mjbase::memberKeyOf(pool, c, "zzz", "()V"));
    EXPECT_EQ(r3.status, DispatchResult::Status::NoMethod);

    // receiver 未登记
    auto r4 = mh.virtualDispatch(mjbase::classKeyOf(pool, "NotRegistered"), aFoo);
    EXPECT_EQ(r4.status, DispatchResult::Status::NoMethod);
}

// ── 虚分派：static/private 声明不拦截，继续上行 ──
TEST(MjHierarchyTest, VirtualDispatchSkipsNonVirtualDeclarations)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("A", "java/lang/Object", {{"foo", "()V", kAccPublic}})
        .cls("B", "A", {{"foo", "()V", kAccStatic}})   // B 的 static 同名不参与
        .cls("C", "B", {});

    MethodHierarchy mh;
    mh.build(h);

    const ClassKey c = h.findClass("C");
    const MemberKey aFoo = mjbase::memberKeyOf(pool, h.findClass("A"), "foo", "()V");
    auto r = mh.virtualDispatch(c, aFoo);
    EXPECT_EQ(r.status, DispatchResult::Status::Resolved);
    EXPECT_EQ(r.target, aFoo);
}

// ── 虚分派：最近声明为 abstract（重抽象遮蔽父类实现）→ Abstract ──
TEST(MjHierarchyTest, VirtualDispatchNearestAbstractWins)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("A", "java/lang/Object", {{"foo", "()V", kAccPublic}})
        .cls("B", "A", {{"foo", "()V", kAccPublic | kAccAbstract}})
        .cls("C", "B", {});

    MethodHierarchy mh;
    mh.build(h);

    const MemberKey bFoo = mjbase::memberKeyOf(pool, h.findClass("B"), "foo", "()V");
    const MemberKey sig  = mjbase::memberKeyOf(pool, h.findClass("A"), "foo", "()V");

    // 从 B 或 B 的子类 C 分派：最近的 B.foo 是 abstract → 不回退到 A 的实现
    for (const std::string_view cls : {"B", "C"}) {
        auto r = mh.virtualDispatch(h.findClass(cls), sig);
        EXPECT_EQ(r.status, DispatchResult::Status::Abstract) << cls;
        EXPECT_EQ(r.target, bFoo) << cls;
    }
}

// ── 虚分派：仅接口 default 方法承载实现 → Unresolved（不裁决）──
TEST(MjHierarchyTest, VirtualDispatchInterfaceDefaultIsUnresolved)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    // 接口 default：非 abstract 声明
    g.cls("java/lang/Object", "", {})
        .cls("I", "java/lang/Object", {{"foo", "()V", kAccPublic}}, kAccInterface)
        .clsWithIfaces("Impl", "java/lang/Object", {"I"}, {});

    MethodHierarchy mh;
    mh.build(h);

    const MemberKey iFoo = mjbase::memberKeyOf(pool, h.findClass("I"), "foo", "()V");
    const auto r = mh.virtualDispatch(h.findClass("Impl"), iFoo);
    EXPECT_EQ(r.status, DispatchResult::Status::Unresolved);
    EXPECT_EQ(r.target, iFoo);
}

// ── 虚分派：类链实现优先于接口 default（classes-win）──
TEST(MjHierarchyTest, VirtualDispatchClassChainBeatsInterfaceDefault)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("I", "java/lang/Object", {{"foo", "()V", kAccPublic}}, kAccInterface)
        .clsWithIfaces("Impl", "java/lang/Object", {"I"}, {{"foo", "()V", kAccPublic}});

    MethodHierarchy mh;
    mh.build(h);

    const MemberKey sig = mjbase::memberKeyOf(pool, h.findClass("I"), "foo", "()V");
    const auto r = mh.virtualDispatch(h.findClass("Impl"), sig);
    EXPECT_EQ(r.status, DispatchResult::Status::Resolved);
    EXPECT_EQ(r.target, mjbase::memberKeyOf(pool, h.findClass("Impl"), "foo", "()V"));
}

// ── 虚分派：仅接口 abstract 声明 → Abstract；完全无声明 → NoMethod ──
TEST(MjHierarchyTest, VirtualDispatchInterfaceAbstractOnly)
{
    mjbase::KeyPool pool = test::makePool();
    ClassHierarchy h(pool);
    GraphBuilder g(pool, h);
    g.cls("java/lang/Object", "", {})
        .cls("I", "java/lang/Object", {{"foo", "()V", kAccPublic | kAccAbstract}},
             kAccInterface | kAccAbstract)
        .clsWithIfaces("Impl", "java/lang/Object", {"I"}, {});

    MethodHierarchy mh;
    mh.build(h);

    const MemberKey iFoo = mjbase::memberKeyOf(pool, h.findClass("I"), "foo", "()V");
    const auto r = mh.virtualDispatch(h.findClass("Impl"), iFoo);
    EXPECT_EQ(r.status, DispatchResult::Status::Abstract);
    EXPECT_EQ(r.target, iFoo);

    const auto none = mh.virtualDispatch(
        h.findClass("Impl"), mjbase::memberKeyOf(pool, h.findClass("I"), "bar", "()V"));
    EXPECT_EQ(none.status, DispatchResult::Status::NoMethod);
}
