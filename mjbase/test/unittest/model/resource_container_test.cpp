/// @file        resource_container_test.cpp
/// @brief       资源容器测试（10 用例）。
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

// 重复 zip 条目：last-wins 保留末条目（对齐 JVM 类加载实测口径：mini jar 对照
// 实验钉死 CEN 末现生效，推翻  first-wins 误判），重复项记录在
// duplicateResourceUris() 供导入方告警；有意静默替换走 replaceResource。
TEST(MjModelContainerTest, DuplicateEntriesAreRecordedAndLastWins)
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

    // last-wins：保留的是末条目 0xCC；非 Dup 名不受影响。
    ASSERT_EQ(c.size(), 2u);
    auto* got = c.getResource("Dup.class");
    ASSERT_NE(got, nullptr);
    const auto& bytes = static_cast<const ClassResource*>(got)->get();
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], 0xCC);
    auto* other = c.getResource("Other.class");
    ASSERT_NE(other, nullptr);
    EXPECT_EQ(static_cast<const ClassResource*>(other)->get()[0], 0xBB);
}

// SSTO 实证形态：同名条目「垃圾 → 垃圾 → 真实类」三连（CRC 全伪的恶意罐形）。
// 末现口径下真实类整条存活；每次冲突各记一条（计数 = 冲突次数，非去重名数）。
TEST(MjModelContainerTest, SSTOShapeGarbageFrontRealClassLastSurvives)
{
    ResourceContainer c;
    const std::vector<ca::u8> garbage2B{0xDE, 0xAD};
    const std::vector<ca::u8> garbage31B(31, 0x7F);
    const std::vector<ca::u8> realClass{0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34};
    c.addResource("app/Dup.class", std::make_unique<UnknownResource>("app/Dup.class", garbage2B));
    c.addResource("app/Dup.class",
                  std::make_unique<UnknownResource>("app/Dup.class", garbage31B));
    c.addResource("app/Dup.class",
                  std::make_unique<ClassResource>("app/Dup.class", realClass));
    ASSERT_EQ(c.size(), 1u);
    ASSERT_EQ(c.duplicateResourceUris().size(), 2u);
    EXPECT_EQ(c.duplicateResourceUris()[0], "app/Dup.class");
    EXPECT_EQ(c.duplicateResourceUris()[1], "app/Dup.class");
    auto* got = c.getResource("app/Dup.class");
    ASSERT_NE(got, nullptr);
    // 末现 = 真实类（ClassResource），前两条垃圾（UnknownResource）被覆盖。
    ASSERT_EQ(got->getType(), ResourceType::Class);
    EXPECT_EQ(static_cast<const ClassResource*>(got)->get(), realClass);
}

// replaceResource：候选重建、事务预览等对已知 URI 的有意替换，直接覆盖
// 且不记入重复导入告警（区别于 addResource 的重复条目告警路径）。
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
