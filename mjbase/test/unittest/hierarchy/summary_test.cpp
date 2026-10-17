/// @file        summary_test.cpp
/// @brief       ClassSummary 值结构与 fillSummary 测试（真实字节骨架填充）。
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "hierarchy_test_util.hpp"
#include "jbc/jbc.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"

using namespace mjbase::hierarchy;
using mjbase::hierarchy::test::ClassBytesBuilder;
using mjbase::ClassKey;
using mjbase::KeyPool;
using MemberSpec = ClassBytesBuilder::MemberSpec;

namespace {

/// 真实字节 → 会话 → fillSummary 的一步管线；断言失败返回 nullptr。
std::shared_ptr<jbc::ir::ClassSession> sessionOf(const std::vector<ca::u8>& bytes)
{
    auto r = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{bytes});
    if (r.is_err()) return nullptr;
    return r.unwrap();
}

}   // namespace

// ── 真实字节骨架填充：this/super/interfaces/成员名与访问位 ──
TEST(MjHierarchyTest, FillSummaryFromRealBytes)
{
    KeyPool pool = test::makePool();

    ClassBytesBuilder b;
    b.setThis("p/Demo")
        .setSuper("java/lang/Object")
        .setAccess(kAccPublic | kAccSuper)
        .addInterface("p/Iface")
        .addField({"sCounter", "I", kAccPublic | kAccStatic})
        .addField({"x", "J", kAccPublic})
        .addMethod({"<init>", "()V", 0})
        .addMethod({"work", "(I)V", kAccPublic})
        .addMethod({"stat", "()V", kAccPublic | kAccStatic});
    const auto bytes = b.build();

    auto session = sessionOf(bytes);
    ASSERT_NE(session, nullptr);

    const ClassSummary s = fillSummary(*session, pool);

    EXPECT_EQ(pool.str(s.self.id), "p/Demo");
    ASSERT_TRUE(s.super.valid());
    EXPECT_EQ(pool.str(s.super.id), "java/lang/Object");
    EXPECT_EQ(s.access, kAccPublic | kAccSuper);

    ASSERT_EQ(s.interfaces.size(), 1u);
    EXPECT_EQ(pool.str(s.interfaces[0].id), "p/Iface");

    ASSERT_EQ(s.fields.size(), 2u);
    EXPECT_EQ(pool.str(s.fields[0].name), "sCounter");
    EXPECT_EQ(pool.str(s.fields[0].desc), "I");
    EXPECT_TRUE(s.fields[0].isStatic);
    EXPECT_EQ(s.fields[0].access, kAccPublic | kAccStatic);
    EXPECT_EQ(pool.str(s.fields[1].name), "x");
    EXPECT_EQ(pool.str(s.fields[1].desc), "J");
    EXPECT_FALSE(s.fields[1].isStatic);

    ASSERT_EQ(s.methods.size(), 3u);
    EXPECT_EQ(pool.str(s.methods[0].name), "<init>");
    EXPECT_EQ(pool.str(s.methods[0].desc), "()V");
    EXPECT_FALSE(s.methods[0].isStatic);
    EXPECT_EQ(pool.str(s.methods[1].name), "work");
    EXPECT_EQ(pool.str(s.methods[1].desc), "(I)V");
    EXPECT_EQ(s.methods[1].access, kAccPublic);
    EXPECT_EQ(pool.str(s.methods[2].name), "stat");
    EXPECT_TRUE(s.methods[2].isStatic);
}

// ── Object 形态：无父类 → invalid super 键 ──
TEST(MjHierarchyTest, FillSummaryObjectHasNoSuper)
{
    KeyPool pool = test::makePool();

    ClassBytesBuilder b;
    b.setThis("java/lang/Object").setSuper("").setAccess(kAccPublic | kAccSuper);
    auto session = sessionOf(b.build());
    ASSERT_NE(session, nullptr);

    const ClassSummary s = fillSummary(*session, pool);
    EXPECT_EQ(pool.str(s.self.id), "java/lang/Object");
    EXPECT_FALSE(s.super.valid());
    EXPECT_TRUE(s.interfaces.empty());
    EXPECT_TRUE(s.methods.empty());
    EXPECT_TRUE(s.fields.empty());
}

// ── 填充 → 建图衔接：fillSummary 的产物直接作为 ClassHierarchy 输入 ──
TEST(MjHierarchyTest, FillSummaryFeedsClassHierarchy)
{
    KeyPool pool = test::makePool();

    ClassBytesBuilder iface;
    iface.setThis("p/Iface")
        .setSuper("java/lang/Object")
        .setAccess(kAccPublic | kAccInterface);
    ClassBytesBuilder impl;
    impl.setThis("p/Impl")
        .setSuper("java/lang/Object")
        .setAccess(kAccPublic | kAccSuper)
        .addInterface("p/Iface")
        .addMethod({"work", "()V", kAccPublic});

    auto ifaceSession = sessionOf(iface.build());
    auto implSession  = sessionOf(impl.build());
    ASSERT_NE(ifaceSession, nullptr);
    ASSERT_NE(implSession, nullptr);

    ClassHierarchy h(pool);
    ASSERT_TRUE(h.addClass(fillSummary(*ifaceSession, pool)).is_ok());
    ASSERT_TRUE(h.addClass(fillSummary(*implSession, pool)).is_ok());

    const ClassKey implKey = h.findClass("p/Impl");
    ASSERT_TRUE(implKey.valid());
    EXPECT_EQ(pool.str(h.superClass(implKey).id), "java/lang/Object");
    EXPECT_TRUE(h.isInterface(h.findClass("p/Iface")));
    EXPECT_TRUE(h.isSubtype(implKey, h.findClass("p/Iface")));
}

// ── 截断字节 → 读段失败（SessionError::Stage::Read），不建会话 ──
TEST(MjHierarchyTest, TruncatedBytesFailAtReadStage)
{
    ClassBytesBuilder b;
    b.setThis("p/Bad").setSuper("java/lang/Object");
    std::vector<ca::u8> bytes = b.build();
    bytes.resize(10);   // 掐掉主体

    auto r = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{bytes});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().stage, jbc::ir::SessionError::Stage::Read);
}

// ── 手工构造的摘要与填充产物同为纯值：拷贝后内容不变 ──
TEST(MjHierarchyTest, ManualSummaryIsSelfContainedValue)
{
    KeyPool pool = test::makePool();
    ClassSummary s = test::makeSummary(pool, "p/A", "java/lang/Object", kAccPublic);
    s.methods.push_back(test::makeMember(pool, "run", "()V", kAccPublic | kAccFinal));

    const ClassSummary copy = s;   // 自持值：深拷贝（vector 快照）
    s.methods.clear();
    s.interfaces.push_back(mjbase::classKeyOf(pool, "p/X"));

    EXPECT_EQ(copy.methods.size(), 1u);
    EXPECT_TRUE(copy.interfaces.empty());
    EXPECT_EQ(pool.str(copy.methods[0].name), "run");
    EXPECT_EQ(copy.methods[0].access, kAccPublic | kAccFinal);
}
