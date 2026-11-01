/// @file        shadow_alias_store_test.cpp
/// @brief       shadow 名持久化：ShadowAliasTable ↔ AnalysisDb 整表替换、
///              跨会话还原、机械占位名生成后 token 随行。
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "mjbase/renamer/shadow_alias_store.hpp"

#include "renamer_fixture.hpp"
#include "test_helper.hpp"

using namespace mjbase::renamer;
using ca::str::Utf8StringRef;

namespace {

namespace project = mjbase::project;
namespace model   = mjbase::model;
namespace fixture = mjbase::renamer::test_support;

Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

std::filesystem::path freshDbPath(const std::string& name)
{
    const auto path = test::getTempOutPath("shadow_store_" + name + ".mjdb");
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::remove_all(path.string() + "-wal", ec);
    std::filesystem::remove_all(path.string() + "-shm", ec);
    std::filesystem::create_directories(path.parent_path(), ec);
    return path;
}

project::AnalysisDb openOrDie(const std::filesystem::path& path,
                              project::AnalysisDbOpenMode mode =
                                  project::AnalysisDbOpenMode::ReadWrite)
{
    auto result = project::AnalysisDb::open(refOf(path.string()), mode);
    EXPECT_TRUE(result.is_ok()) << path.string();
    return std::move(result).unwrap();
}

std::string toStd(std::string_view value)
{
    return std::string(value);
}

}   // namespace

TEST(MjShadowAliasStoreTest, TableRoundtripPersistsAcrossReopen)
{
    const auto path = freshDbPath("roundtrip");
    {
        auto db = openOrDie(path);

        ShadowAliasTable table;
        ASSERT_TRUE(table.addClass("a/A", "Class_1", "deadbeef"));
        ASSERT_TRUE(table.addMethod("a/A", "()V", "run", "Method_1", "00000001"));
        ASSERT_TRUE(table.addField("a/A", "I", "f", "Field_1", "00000002"));

        ASSERT_TRUE(importShadowAliases(db, table).is_ok());

        auto loaded = loadShadowAliases(db);
        ASSERT_TRUE(loaded.is_ok());
        auto restored = std::move(loaded).unwrap();

        EXPECT_EQ(3U, restored.size());
        EXPECT_EQ("Class_1", toStd(restored.classShadow("a/A")));
        EXPECT_EQ("Method_1", toStd(restored.methodShadow("a/A", "run", "()V")));
        EXPECT_EQ("Field_1", toStd(restored.fieldShadow("a/A", "f", "I")));

        const auto* classEntry = restored.classEntry("a/A");
        ASSERT_NE(nullptr, classEntry);
        EXPECT_EQ("deadbeef", toStd(classEntry->token8));

        ASSERT_TRUE(db.save().is_ok());
    }

    auto reopened = openOrDie(path, project::AnalysisDbOpenMode::ReadOnly);
    auto loaded   = loadShadowAliases(reopened);
    ASSERT_TRUE(loaded.is_ok());
    auto restored = std::move(loaded).unwrap();

    EXPECT_EQ(3U, restored.size());
    EXPECT_EQ("Class_1", toStd(restored.classShadow("a/A")));
    EXPECT_EQ("Method_1", toStd(restored.methodShadow("a/A", "run", "()V")));
    EXPECT_EQ("Field_1", toStd(restored.fieldShadow("a/A", "f", "I")));
}

TEST(MjShadowAliasStoreTest, ImportReplacesPreviousTable)
{
    const auto path = freshDbPath("replace");
    auto       db   = openOrDie(path);

    ShadowAliasTable first;
    ASSERT_TRUE(first.addClass("a/A", "Class_1", "deadbeef"));
    ASSERT_TRUE(importShadowAliases(db, first).is_ok());

    ShadowAliasTable second;
    ASSERT_TRUE(second.addClass("b/B", "Class_2", "00000001"));
    ASSERT_TRUE(importShadowAliases(db, second).is_ok());

    auto loaded = loadShadowAliases(db);
    ASSERT_TRUE(loaded.is_ok());
    auto restored = std::move(loaded).unwrap();

    EXPECT_EQ(1U, restored.size());
    EXPECT_TRUE(restored.classShadow("a/A").empty());
    EXPECT_EQ("Class_2", toStd(restored.classShadow("b/B")));
}

TEST(MjShadowAliasStoreTest, MechanicalNamesAndTokensPersist)
{
    const auto path = freshDbPath("mechanical");

    model::ResourceContainer container;
    container.addResource("java/lang/Object.class",
                          std::make_unique<model::ClassResource>("java/lang/Object.class",
                                                                 fixture::buildBareObject()));
    container.addResource("A.class", std::make_unique<model::ClassResource>("A.class",
                                                                            fixture::buildA()));

    ShadowAliasTable table;
    auto generated = generateMechanicalShadowNames(container, table);
    ASSERT_TRUE(generated.is_ok());
    ASSERT_FALSE(table.empty());

    const std::string classShadow = toStd(table.classShadow("A"));
    const std::string methodShadow =
        toStd(table.methodShadow("A", "oldMethod", "()V"));
    ASSERT_FALSE(classShadow.empty());
    ASSERT_FALSE(methodShadow.empty());

    {
        auto db = openOrDie(path);
        ASSERT_TRUE(importShadowAliases(db, table).is_ok());
        ASSERT_TRUE(db.save().is_ok());
    }

    auto reopened = openOrDie(path, project::AnalysisDbOpenMode::ReadOnly);
    auto loaded   = loadShadowAliases(reopened);
    ASSERT_TRUE(loaded.is_ok());
    auto restored = std::move(loaded).unwrap();

    EXPECT_EQ(table.size(), restored.size());
    EXPECT_EQ(classShadow, toStd(restored.classShadow("A")));
    EXPECT_EQ(methodShadow, toStd(restored.methodShadow("A", "oldMethod", "()V")));

    const auto* classEntry = restored.classEntry("A");
    ASSERT_NE(nullptr, classEntry);
    EXPECT_FALSE(classEntry->token8.empty());
}
