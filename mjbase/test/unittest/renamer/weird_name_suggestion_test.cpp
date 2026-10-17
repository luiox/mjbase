/// @file        weird_name_suggestion_test.cpp
/// @brief       renamer P0 验收矩阵补遗（luiox/morpher#1025 决议 Q3）：怪名用例
///              ——空格/控制字符/非 ASCII 的 UTF-8 原文口径，经建议文件（严格
///              v1 JSON）与 FileSuggestionProvider 到引擎全程不变形（mjbase
///              移植版）。
///
/// Q3 口径：名字一律 UTF-8 原文；JSON 字符串转义（\u0020 等）在载入期解码为
/// 原文字节，JVM 转义形态（字面反斜杠序列）不作为身份字段被解释——引擎按
/// 原文判定合法性与存在性，被拒流水保留原文指纹供审计回放。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/renamer/name_proposal.hpp"
#include "mjbase/renamer/name_provider.hpp"
#include "mjbase/renamer/name_suggestion_file.hpp"
#include "mjbase/renamer/rename_engine.hpp"

#include "renamer_fixture.hpp"

namespace mjbase::renamer {

namespace {

namespace fixture = renamer::test_support;

// 非 ASCII 怪名样本："变量"（UTF-8 原文 6 字节，hex 逐字节书写防源码编码漂移）。
const char kWeirdUtf8Name[] = "\xE5\x8F\x98\xE9\x87\x8F";

std::string errText(ca::core::Result<NameSuggestionFile, SuggestionFileError> result)
{
    return std::move(result).unwrap_err().message;
}

/// 引擎夹具：A{ oldField:I; other:J; flag:Z; <init>()V } + java/lang/Object。
struct Fixture
{
    model::ResourceContainer container;

    Fixture()
    {
        container.addResource("java/lang/Object.class",
                              std::make_unique<model::ClassResource>(
                                  "java/lang/Object.class", fixture::buildBareObject()));
        std::string members;
        members += fixture::fieldDecl("public", "oldField", "I");
        members += fixture::fieldDecl("public", "other", "J");
        members += fixture::fieldDecl("public", "flag", "Z");
        members += fixture::voidMethod("public", "<init>", "()V");
        container.addResource(
            "A.class",
            std::make_unique<model::ClassResource>(
                "A.class", fixture::assembleClass("A", "java/lang/Object", members)));
    }
};

/// 单条 field 建议（v1 proposal 对象）JSON 片段，newNameJson 含引号。
std::string fieldProposalJson(const std::string& newNameJson)
{
    return "{\"kind\": \"field\", \"owner\": \"A\", \"name\": \"oldField\", \"desc\": \"I\", "
           "\"newName\": " +
           newNameJson + ", \"confidence\": 0.9}";
}

/// 单 proposal 文件全文（source 固定，便于溯源断言）。
std::string singleProposalFile(const std::string& newNameJson)
{
    return "{\"version\": 1, \"source\": \"ai:weird\", \"proposals\": [" +
           fieldProposalJson(newNameJson) + "]}";
}

NameSuggestionFile parseOk(const std::string& text)
{
    auto parsed = parseNameSuggestionFile(text);
    EXPECT_TRUE(parsed.is_ok()) << errText(std::move(parsed));
    return std::move(parsed).unwrap();
}

/// 建议文件经 FileSuggestionProvider 跑一轮引擎（file 按值接管，建议批克隆语义）。
RenamerRunResult runFileThroughEngine(Fixture& fixture, NameSuggestionFile file)
{
    RenamerEngine           engine;
    RenamerOptions          options;
    FileSuggestionProvider  provider(std::move(file));
    std::vector<NameProvider*> providers{&provider};
    return engine.run(fixture.container, providers, options);
}

const RejectedProposal* findRejected(const RenamerRunResult& result, RejectionReason reason)
{
    for (const auto& entry : result.record.rejected()) {
        if (entry.reason == reason) return &entry;
    }
    return nullptr;
}

}   // namespace

// ── JSON 转义解码 = UTF-8 原文身份 ──

TEST(MjWeirdNameSuggestionTest, jsonEscapesDecodeToUtf8RawIdentity)
{
    // 空格（原文）/\u0009（转义控制字符）/变量（原文非 ASCII）——三种怪名同一文件。
    const std::string text =
        std::string("{\"version\": 1, \"source\": \"ai:weird\", \"proposals\": [") +
        "{\"kind\": \"field\", \"owner\": \"A\", \"name\": \"oldField\", \"desc\": \"I\", "
        "\"newName\": \"a b\", \"confidence\": 0.9}," +
        "{\"kind\": \"field\", \"owner\": \"A\", \"name\": \"other\", \"desc\": \"J\", "
        "\"newName\": \"a\\u0009b\", \"confidence\": 0.9}," +
        "{\"kind\": \"field\", \"owner\": \"A\", \"name\": \"flag\", \"desc\": \"Z\", "
        "\"newName\": \"" +
        kWeirdUtf8Name + "\", \"confidence\": 0.9}]}";

    const auto file = parseOk(text);
    ASSERT_EQ(file.proposals.size(), static_cast<ca::usize>(3));

    // 原文空格：身份字段就是空格字节本身。
    EXPECT_EQ(std::string(file.proposals[0].newName), std::string("a b"));
    // \u0009 转义：载入期解码为 0x09 原文字节（非字面反斜杠序列）。
    EXPECT_EQ(std::string(file.proposals[1].newName), std::string("a\tb"));
    // 原文非 ASCII：UTF-8 字节透传不变形。
    EXPECT_EQ(std::string(file.proposals[2].newName), std::string(kWeirdUtf8Name));
}

TEST(MjWeirdNameSuggestionTest, escapedSpaceAndRawSpaceAreSameIdentityBytes)
{
    // Q3"归一"口径：\u0020 转义形态与原文空格解析出完全相同的身份字节。
    const auto escaped = parseOk(singleProposalFile("\"a\\u0020b\""));
    const auto raw     = parseOk(singleProposalFile("\"a b\""));
    ASSERT_EQ(escaped.proposals.size(), static_cast<ca::usize>(1));
    ASSERT_EQ(raw.proposals.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(std::string(escaped.proposals[0].newName), std::string("a b"));
    EXPECT_EQ(std::string(raw.proposals[0].newName), std::string("a b"));
    EXPECT_EQ(std::string(escaped.proposals[0].newName),
              std::string(raw.proposals[0].newName));
}

TEST(MjWeirdNameSuggestionTest, jvmEscapeFormIsNotInterpretedAsIdentity)
{
    // Q3 红线：字面反斜杠序列（"a\\u0020b" 7 字节）不是空格——引擎按原文拒，
    // 流水保留 7 字节原文指纹，不做 JVM 反转义。
    auto file = parseOk(singleProposalFile("\"a\\\\u0020b\""));
    ASSERT_EQ(file.proposals.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(std::string(file.proposals[0].newName), std::string("a\\u0020b"));

    Fixture  fixture;
    auto     result = runFileThroughEngine(fixture, std::move(file));
    ASSERT_TRUE(result.ok);
    const auto* rejected = findRejected(result, RejectionReason::IllegalIdentifier);
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(std::string(rejected->proposal.newName), std::string("a\\u0020b"));
    EXPECT_EQ(result.plan.findField("A", "oldField", "I"), nullptr);
}

// ── 引擎对怪名的判定：空格/控制字符拒（IdentifierUtil 单一口径）──

TEST(MjWeirdNameSuggestionTest, engineRejectsSpaceAndControlCharNewNamesViaProvider)
{
    const std::string text =
        std::string("{\"version\": 1, \"source\": \"ai:weird\", \"proposals\": [") +
        fieldProposalJson("\"a b\"") + "," + fieldProposalJson("\"a\\u0001b\"") + "]}";

    Fixture fixture;
    auto    file   = parseOk(text);
    auto    result = runFileThroughEngine(fixture, std::move(file));

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.proposed, 2U);
    EXPECT_EQ(result.stats.accepted, 0U);
    ASSERT_EQ(result.record.rejected().size(), static_cast<ca::usize>(2));
    EXPECT_EQ(result.record.rejected()[0].reason, RejectionReason::IllegalIdentifier);
    EXPECT_EQ(result.record.rejected()[1].reason, RejectionReason::IllegalIdentifier);
    // 被拒流水保留原文（含控制字符 0x01），审计可回放。
    EXPECT_EQ(std::string(result.record.rejected()[0].proposal.newName), std::string("a b"));
    EXPECT_EQ(std::string(result.record.rejected()[1].proposal.newName),
              std::string("a\x01"
                          "b"));
    EXPECT_EQ(result.record.rejected()[0].proposal.source, "ai:weird");
    EXPECT_TRUE(result.plan.empty());
}

TEST(MjWeirdNameSuggestionTest, nonAsciiNewNameAcceptedInPlanWithUtf8RawBytes)
{
    Fixture    fixture;
    auto       file   = parseOk(singleProposalFile(std::string("\"") + kWeirdUtf8Name + "\""));
    const auto result = runFileThroughEngine(fixture, std::move(file));

    // IdentifierUtil JLS 口径：>0x80 码点是合法标识符——引擎复用同一谓词接受。
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.stats.accepted, 1U);
    EXPECT_NE(result.plan.findField("A", "oldField", "I"), nullptr);
    ASSERT_FALSE(result.record.accepted().empty());
    EXPECT_EQ(result.record.accepted().front().source, "ai:weird");
    EXPECT_EQ(std::string(result.record.accepted().front().newName),
              std::string(kWeirdUtf8Name));
}

// ── 写出/回读往返：怪名原文指纹恒等 ──

TEST(MjWeirdNameSuggestionTest, weirdNamesSurviveWriteReadRoundTripByteIdentical)
{
    // rationale 含 JSON \u0001 转义控制符；evidence/新名含原文非 ASCII。
    const std::string text =
        std::string("{\"version\": 1, \"source\": \"ai:weird src\", \"proposals\": [") +
        "{\"kind\": \"field\", \"owner\": \"A\", \"name\": \"oldField\", \"desc\": \"I\", "
        "\"newName\": \"a b\", \"confidence\": 0.9, "
        "\"rationale\": \"\xE5\x90\xAB \xE7\xA9\xBA\xE6\xA0\xBC \xE4\xB8\x8E\\u0001\xE6\x8E\xA7\xE5\x88\xB6\xE7\xAC\xA6\", "
        "\"evidence\": \"" +
        kWeirdUtf8Name + "\"}," +
        "{\"kind\": \"field\", \"owner\": \"A\", \"name\": \"other\", \"desc\": \"J\", "
        "\"newName\": \"" +
        kWeirdUtf8Name + "\", \"confidence\": 0.5}]}";

    const auto first   = parseOk(text);
    auto       written = writeNameSuggestionText(first);
    ASSERT_TRUE(written.is_ok());
    const auto second = parseOk(std::move(written).unwrap());

    EXPECT_EQ(std::string(second.source), "ai:weird src");
    ASSERT_EQ(second.proposals.size(), first.proposals.size());
    for (ca::usize i = 0; i < first.proposals.size(); ++i) {
        EXPECT_EQ(second.proposals[i].kind, first.proposals[i].kind);
        EXPECT_EQ(std::string(second.proposals[i].owner),
                  std::string(first.proposals[i].owner));
        EXPECT_EQ(std::string(second.proposals[i].oldName),
                  std::string(first.proposals[i].oldName));
        EXPECT_EQ(std::string(second.proposals[i].desc), std::string(first.proposals[i].desc));
        EXPECT_EQ(std::string(second.proposals[i].newName),
                  std::string(first.proposals[i].newName));
        EXPECT_DOUBLE_EQ(second.proposals[i].confidence, first.proposals[i].confidence);
        EXPECT_EQ(std::string(second.proposals[i].rationale),
                  std::string(first.proposals[i].rationale));
        EXPECT_EQ(std::string(second.proposals[i].evidence),
                  std::string(first.proposals[i].evidence));
    }
    // 往返前后怪名身份字节逐字节恒等（空格原样、控制符解码一致、非 ASCII 原文）。
    EXPECT_EQ(std::string(second.proposals[0].newName), std::string("a b"));
    // "含 空格 与\x01控制符"（\u0001 经两次解析恒等于 0x01 原文字节）。
    EXPECT_EQ(std::string(second.proposals[0].rationale),
              std::string("\xE5\x90\xAB"
                          " "
                          "\xE7\xA9\xBA\xE6\xA0\xBC"
                          " \xE4\xB8\x8E\x01\xE6\x8E\xA7\xE5\x88\xB6\xE7\xAC\xA6"));
    EXPECT_EQ(std::string(second.proposals[0].evidence), std::string(kWeirdUtf8Name));
    EXPECT_EQ(std::string(second.proposals[1].newName), std::string(kWeirdUtf8Name));
}

}   // namespace mjbase::renamer
