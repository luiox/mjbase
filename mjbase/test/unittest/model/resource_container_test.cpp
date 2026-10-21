/// @file        resource_container_test.cpp
/// @brief       资源容器测试（移植自）。
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

using namespace mjbase::model;

// ── ResourceContainer ───────────────────────────────────────────
TEST(MjModelContainerTest, EmptyInitially)
{
    ResourceContainer c;
    EXPECT_EQ(c.size(), 0);
}

TEST(MjModelContainerTest, AddAndGet)
{
    ResourceContainer c;
    auto              res = std::make_unique<ClassResource>(
        "Hello.class", std::vector<ca::u8>{0xCA, 0xFE, 0xBA, 0xBE});
    c.addResource("Hello.class", std::move(res));

    EXPECT_EQ(c.size(), 1);
    EXPECT_TRUE(c.hasResource("Hello.class"));

    auto* got = c.getResource("Hello.class");
    ASSERT_NE(got, nullptr);
}

TEST(MjModelContainerTest, GetNonExistentReturnsNull)
{
    ResourceContainer c;
    EXPECT_EQ(c.getResource("nonexistent"), nullptr);
}

TEST(MjModelContainerTest, ClearRemovesAll)
{
    ResourceContainer c;
    c.addResource("a.class",
                  std::make_unique<ClassResource>("a.class", std::vector<ca::u8>{0}));
    c.addResource("b.class",
                  std::make_unique<ClassResource>("b.class", std::vector<ca::u8>{0}));
    EXPECT_EQ(c.size(), 2);
    c.clear();
    EXPECT_EQ(c.size(), 0);
}

TEST(MjModelContainerTest, VisitAllResources)
{
    ResourceContainer c;
    c.addResource("A.class",
                  std::make_unique<ClassResource>("A.class", std::vector<ca::u8>{1}));
    c.addResource("B.class",
                  std::make_unique<ClassResource>("B.class", std::vector<ca::u8>{2}));

    int count = 0;
    c.visitResources([&](std::string_view, const IResource&) { count++; });
    EXPECT_EQ(count, 2);
}

TEST(MjModelContainerTest, GetMutableResources)
{
    ResourceContainer c;
    c.addResource("X.class",
                  std::make_unique<ClassResource>("X.class", std::vector<ca::u8>{0}));
    EXPECT_EQ(c.getResources().size(), 1);
}

// 重复 zip 条目：first-wins 保留首条目（与 JVM 类加载语义一致，issue #962），
// 重复项记录在 duplicateResourceUris() 供导入方告警；有意替换走 replaceResource。
TEST(MjModelContainerTest, DuplicateEntriesAreRecordedAndFirstWins)
{
    ResourceContainer c;
    c.addResource("Dup.class",
                  std::make_unique<ClassResource>("Dup.class", std::vector<ca::u8>{0xAA}));
    c.addResource("Other.class",
                  std::make_unique<ClassResource>("Other.class", std::vector<ca::u8>{0xBB}));
    EXPECT_TRUE(c.duplicateResourceUris().empty());

    c.addResource("Dup.class",
                  std::make_unique<ClassResource>("Dup.class", std::vector<ca::u8>{0xCC}));
    ASSERT_EQ(c.duplicateResourceUris().size(), 1u);
    EXPECT_EQ(c.duplicateResourceUris()[0], "Dup.class");

    // first-wins：保留的是首条目 0xAA
    ASSERT_EQ(c.size(), 2u);
    auto* got = c.getResource("Dup.class");
    ASSERT_NE(got, nullptr);
    const auto& bytes = static_cast<const ClassResource*>(got)->get();
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], 0xAA);
}

// replaceResource：候选重建、事务预览等对已知 URI 的有意替换，直接覆盖
// 且不记入重复导入告警（issue #962 引入，与 addResource 的 first-wins 区分）。
TEST(MjModelContainerTest, ReplaceResourceOverwritesWithoutDuplicateRecord)
{
    ResourceContainer c;
    c.addResource("A.class",
                  std::make_unique<ClassResource>("A.class", std::vector<ca::u8>{0xAA}));
    c.replaceResource("A.class",
                      std::make_unique<ClassResource>("A.class", std::vector<ca::u8>{0xCC}));
    EXPECT_TRUE(c.duplicateResourceUris().empty());
    ASSERT_EQ(c.size(), 1u);
    auto* got = c.getResource("A.class");
    ASSERT_NE(got, nullptr);
    const auto& bytes = static_cast<const ClassResource*>(got)->get();
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], 0xCC);
}

// 进度导入路径（ResourceHelper::importFromJar 带 progress 重载）不经
// ResourceContainer::importResource，由 ResourceHelper 先调 beginImport()；
// 新一轮无重复条目的导入不得残留上一轮的重复记录。
TEST(MjModelContainerTest, BeginImportClearsStaleDuplicateRecords)
{
    ResourceContainer c;
    c.addResource("Dup.class",
                  std::make_unique<ClassResource>("Dup.class", std::vector<ca::u8>{0xAA}));
    c.addResource("Dup.class",
                  std::make_unique<ClassResource>("Dup.class", std::vector<ca::u8>{0xCC}));
    ASSERT_EQ(c.duplicateResourceUris().size(), 1u);

    // 模拟第二轮导入（无重复条目）：beginImport 后不再累积
    c.beginImport();
    c.addResource("Solo.class",
                  std::make_unique<ClassResource>("Solo.class", std::vector<ca::u8>{0x11}));
    EXPECT_TRUE(c.duplicateResourceUris().empty());
}
