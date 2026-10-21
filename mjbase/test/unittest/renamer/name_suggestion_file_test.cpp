/// @file        name_suggestion_file_test.cpp
/// @brief       AI 建议文件 schema + FileSuggestionProvider 单测（mjbase 移植
///              版）。
///
/// schema 错误整文件拒载（零应用）、版本不识别拒载、往返恒等、provider 原样
/// 产出，以及 provider→引擎端到端的 rationale/evidence 流水留痕。
///
/// mjbase 面：schema/错误分类/行号定位语义照旧；字符串断言改驻留视图口径。
#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "test_helper.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/renamer/name_suggestion_file.hpp"
#include "mjbase/renamer/rename_engine.hpp"

#include "renamer_fixture.hpp"

namespace mjbase::renamer {

namespace {

namespace fixture = renamer::test_support;

std::string errText(ca::core::Result<NameSuggestionFile, SuggestionFileError> result)
{
    return std::move(result).unwrap_err().message;
}

/// 最小引擎夹具：A{ oldField:I; alpha()V }。
struct EngineFixture
{
    model::ResourceContainer container;

    EngineFixture()
    {
        std::string members;
        members += fixture::fieldDecl("public", "oldField", "I");
        members += fixture::voidMethod("public", "alpha", "()V");
        container.addResource("A.class",
                              std::make_unique<model::ClassResource>(
                                  "A.class", fixture::assembleClass("A", "java/lang/Object",
                                                                    members)));
    }
};

}   // namespace

// ── schema 解析：合法文件 ──

TEST(MjNameSuggestionFileTest, parsesValidFileAndMapsToProposalContract)
{
    const char* json = R"({
        "version": 1,
        "source": "ai:claude",
        "proposals": [
            {"kind": "class", "name": "A", "newName": "ui/Widget", "confidence": 0.9,
             "rationale": "语义化 UI 组件", "evidence": "被 Main 引用 3 次"},
            {"kind": "package", "name": "old/pkg", "newName": "new/pkg", "confidence": 0.5},
            {"kind": "field", "owner": "A", "name": "oldField", "desc": "I",
             "newName": "count", "confidence": 1},
            {"kind": "method", "owner": "A", "name": "alpha", "desc": "()V",
             "newName": "run", "confidence": 0.75, "rationale": "入口方法"}
        ]
    })";

    auto parsed = parseNameSuggestionFile(json);
    ASSERT_TRUE(parsed.is_ok()) << errText(std::move(parsed));
    auto file = std::move(parsed).unwrap();
    EXPECT_EQ(file.version, 1U);
    EXPECT_EQ(std::string(file.source), "ai:claude");
    ASSERT_EQ(file.proposals.size(), static_cast<ca::usize>(4));

    const auto& classProposal = file.proposals[0];
    EXPECT_EQ(classProposal.kind, ProposalKind::Class);
    EXPECT_TRUE(classProposal.owner.empty());
    EXPECT_EQ(classProposal.oldName, "A");
    EXPECT_EQ(classProposal.newName, "ui/Widget");
    EXPECT_DOUBLE_EQ(classProposal.confidence, 0.9);
    EXPECT_EQ(std::string(classProposal.rationale), "语义化 UI 组件");
    EXPECT_EQ(std::string(classProposal.evidence), "被 Main 引用 3 次");

    EXPECT_EQ(file.proposals[1].kind, ProposalKind::Package);
    EXPECT_TRUE(file.proposals[1].desc.empty());

    const auto& fieldProposal = file.proposals[2];
    EXPECT_EQ(fieldProposal.kind, ProposalKind::Field);
    EXPECT_EQ(fieldProposal.owner, "A");
    EXPECT_EQ(fieldProposal.desc, "I");
    EXPECT_DOUBLE_EQ(fieldProposal.confidence, 1.0);   // 整数字面量按 number 收
    EXPECT_TRUE(fieldProposal.rationale.empty());

    const auto& methodProposal = file.proposals[3];
    EXPECT_EQ(methodProposal.kind, ProposalKind::Method);
    EXPECT_EQ(std::string(methodProposal.rationale), "入口方法");
    EXPECT_TRUE(methodProposal.evidence.empty());
}

TEST(MjNameSuggestionFileTest, acceptsEmptyProposalsArray)
{
    auto parsed = parseNameSuggestionFile(R"({"version": 1, "source": "ai:x", "proposals": []})");
    ASSERT_TRUE(parsed.is_ok()) << errText(std::move(parsed));
    EXPECT_TRUE(std::move(parsed).unwrap().proposals.empty());
}

// ── schema 解析：语法错误与版本 ──

TEST(MjNameSuggestionFileTest, syntaxErrorReportsLineAndJsonReadKind)
{
    auto parsed = parseNameSuggestionFile(
        "{\n  \"version\": 1,\n  \"source\": \"ai:x\",\n  \"proposals\": [},\n]}\n");
    ASSERT_TRUE(parsed.is_err());
    const auto error = std::move(parsed).unwrap_err();
    EXPECT_EQ(error.kind, SuggestionFileErrorKind::JsonRead);
    EXPECT_NE(error.message.find("line"), std::string::npos) << error.message;
}

TEST(MjNameSuggestionFileTest, rejectsUnsupportedSchemaVersion)
{
    auto parsed =
        parseNameSuggestionFile(R"({"version": 2, "source": "ai:x", "proposals": []})");
    ASSERT_TRUE(parsed.is_err());
    const auto error = std::move(parsed).unwrap_err();
    EXPECT_EQ(error.kind, SuggestionFileErrorKind::Schema);
    EXPECT_NE(error.message.find("unsupported schema version"), std::string::npos)
        << error.message;
    EXPECT_NE(error.message.find("refusing to load"), std::string::npos) << error.message;
}

TEST(MjNameSuggestionFileTest, rejectsMissingOrNonIntegerVersion)
{
    auto missing =
        parseNameSuggestionFile(R"({"source": "ai:x", "proposals": []})");
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(std::move(missing).unwrap_err().kind, SuggestionFileErrorKind::Schema);

    auto nonInt = parseNameSuggestionFile(
        R"({"version": "1", "source": "ai:x", "proposals": []})");
    ASSERT_TRUE(nonInt.is_err());
    EXPECT_NE(errText(std::move(nonInt)).find("version must be an integer"), std::string::npos);
}

TEST(MjNameSuggestionFileTest, rejectsMissingOrEmptySource)
{
    auto missing = parseNameSuggestionFile(R"({"version": 1, "proposals": []})");
    ASSERT_TRUE(missing.is_err());
    EXPECT_NE(errText(std::move(missing)).find("missing source"), std::string::npos);

    auto empty =
        parseNameSuggestionFile(R"({"version": 1, "source": "", "proposals": []})");
    ASSERT_TRUE(empty.is_err());
    EXPECT_NE(errText(std::move(empty)).find("source must be a non-empty string"),
              std::string::npos);
}

// ── schema 解析：容器与 proposal 形状 ──

TEST(MjNameSuggestionFileTest, rejectsBadProposalsContainer)
{
    auto missing = parseNameSuggestionFile(R"({"version": 1, "source": "ai:x"})");
    ASSERT_TRUE(missing.is_err());
    EXPECT_NE(errText(std::move(missing)).find("missing proposals"), std::string::npos);

    auto notArray = parseNameSuggestionFile(
        R"({"version": 1, "source": "ai:x", "proposals": {}})");
    ASSERT_TRUE(notArray.is_err());
    EXPECT_NE(errText(std::move(notArray)).find("proposals must be an array"),
              std::string::npos);

    auto elementNotObject = parseNameSuggestionFile(
        R"({"version": 1, "source": "ai:x", "proposals": ["method"]})");
    ASSERT_TRUE(elementNotObject.is_err());
    const auto text = errText(std::move(elementNotObject));
    EXPECT_NE(text.find("proposals[0]"), std::string::npos) << text;
    EXPECT_NE(text.find("proposal must be an object"), std::string::npos) << text;
}

TEST(MjNameSuggestionFileTest, rejectsUnknownAndDuplicateFields)
{
    auto unknownTop = parseNameSuggestionFile(
        R"({"version": 1, "source": "ai:x", "proposals": [], "extra": 1})");
    ASSERT_TRUE(unknownTop.is_err());
    EXPECT_NE(errText(std::move(unknownTop)).find("unknown suggestion file field: extra"),
              std::string::npos);

    auto unknownProposal = parseNameSuggestionFile(
        R"({"version": 1, "source": "ai:x", "proposals": [{"kind": "class", "name": "A", "newName": "B", "confidence": 0.5, "why": "?"}]})");
    ASSERT_TRUE(unknownProposal.is_err());
    const auto text = errText(std::move(unknownProposal));
    EXPECT_NE(text.find("proposals[0]"), std::string::npos) << text;
    EXPECT_NE(text.find("unknown proposal field: why"), std::string::npos) << text;

    auto duplicateTop = parseNameSuggestionFile(
        R"({"version": 1, "source": "ai:x", "source": "ai:y", "proposals": []})");
    ASSERT_TRUE(duplicateTop.is_err());
    // 重复字段报出字段名 + 行号（评审 N4；单行 JSON 中首次出现在 line 1）。
    EXPECT_NE(
        errText(std::move(duplicateTop)).find("duplicate suggestion file field: source (line 1)"),
        std::string::npos);

    auto duplicateProposal = parseNameSuggestionFile(
        R"({"version": 1, "source": "ai:x", "proposals": [{"kind": "class", "kind": "field", "name": "A", "newName": "B", "confidence": 0.5}]})");
    ASSERT_TRUE(duplicateProposal.is_err());
    EXPECT_NE(errText(std::move(duplicateProposal)).find("duplicate proposal field: kind"),
              std::string::npos);
}

TEST(MjNameSuggestionFileTest, rejectsProposalShapeViolations)
{
    const char* cases[] = {
        R"({"version": 1, "source": "s", "proposals": [{"name": "A", "newName": "B", "confidence": 0.5}]})",   // 缺 kind
        R"({"version": 1, "source": "s", "proposals": [{"kind": "function", "name": "A", "newName": "B", "confidence": 0.5}]})",   // 未知 kind
        R"({"version": 1, "source": "s", "proposals": [{"kind": "class", "newName": "B", "confidence": 0.5}]})",   // 缺 name
        R"({"version": 1, "source": "s", "proposals": [{"kind": "class", "name": "", "newName": "B", "confidence": 0.5}]})",   // 空 name
        R"({"version": 1, "source": "s", "proposals": [{"kind": "class", "name": "A", "confidence": 0.5}]})",   // 缺 newName
        R"({"version": 1, "source": "s", "proposals": [{"kind": "class", "name": "A", "newName": "B"}]})",   // 缺 confidence
        R"({"version": 1, "source": "s", "proposals": [{"kind": "class", "name": "A", "newName": "B", "confidence": "high"}]})",   // confidence 非数值
        R"({"version": 1, "source": "s", "proposals": [{"kind": "class", "name": "A", "newName": "B", "confidence": 1.5}]})",   // 越界
        R"({"version": 1, "source": "s", "proposals": [{"kind": "field", "owner": "A", "name": "f", "newName": "g", "confidence": 0.5}]})",   // field 缺 desc
        R"({"version": 1, "source": "s", "proposals": [{"kind": "method", "name": "m", "desc": "()V", "newName": "n", "confidence": 0.5}]})",   // method 缺 owner
        R"({"version": 1, "source": "s", "proposals": [{"kind": "class", "owner": "X", "name": "A", "newName": "B", "confidence": 0.5}]})",   // class 带 owner
        R"({"version": 1, "source": "s", "proposals": [{"kind": "package", "desc": "()V", "name": "p", "newName": "q", "confidence": 0.5}]})",   // package 带 desc
        R"({"version": 1, "source": "s", "proposals": [{"kind": "class", "name": "A", "newName": "B", "confidence": 0.5, "rationale": 7}]})",   // rationale 非字符串
    };
    for (const auto* json : cases) {
        auto parsed = parseNameSuggestionFile(json);
        ASSERT_TRUE(parsed.is_err()) << json;
        EXPECT_EQ(std::move(parsed).unwrap_err().kind, SuggestionFileErrorKind::Schema) << json;
    }
}

TEST(MjNameSuggestionFileTest, schemaErrorMessagesCarryLineNumbers)
{
    // proposals[1] 的 confidence 越界在文件第 6 行。
    const char* json =
        "{\n"
        "  \"version\": 1,\n"
        "  \"source\": \"ai:x\",\n"
        "  \"proposals\": [\n"
        "    {\"kind\": \"class\", \"name\": \"A\", \"newName\": \"B\", \"confidence\": 0.5},\n"
        "    {\"kind\": \"class\", \"name\": \"A\", \"newName\": \"C\", \"confidence\": 9}\n"
        "  ]\n"
        "}\n";
    auto parsed = parseNameSuggestionFile(json);
    ASSERT_TRUE(parsed.is_err());
    const auto text = errText(std::move(parsed));
    EXPECT_NE(text.find("proposals[1] (line 6)"), std::string::npos) << text;
}

// 非对象元素同样占据 proposals 数组槽位（评审 N1）：错误行号指向元素自身真实
// 行，不因元素形态前移错位（拒载判定本身与行号无关，仅伤诊断）。
TEST(MjNameSuggestionFileTest, nonObjectElementOccupiesLineSlot)
{
    // proposals[0] = null（第 5 行）。
    const char* json =
        "{\n"
        "  \"version\": 1,\n"
        "  \"source\": \"ai:x\",\n"
        "  \"proposals\": [\n"
        "    null,\n"
        "    {\"kind\": \"class\", \"name\": \"A\", \"newName\": \"B\", \"confidence\": 9}\n"
        "  ]\n"
        "}\n";
    auto parsed = parseNameSuggestionFile(json);
    ASSERT_TRUE(parsed.is_err());
    const auto text = errText(std::move(parsed));
    EXPECT_NE(text.find("proposals[0] (line 5)"), std::string::npos) << text;
    EXPECT_NE(text.find("proposal must be an object"), std::string::npos) << text;
}

TEST(MjNameSuggestionFileTest, arrayElementOccupiesLineSlot)
{
    // proposals[0] = 内嵌数组（第 5 行 `[`）：数组元素同样占槽位。
    const char* json =
        "{\n"
        "  \"version\": 1,\n"
        "  \"source\": \"ai:x\",\n"
        "  \"proposals\": [\n"
        "    [],\n"
        "    {\"kind\": \"class\", \"name\": \"A\", \"newName\": \"B\", \"confidence\": 9}\n"
        "  ]\n"
        "}\n";
    auto parsed = parseNameSuggestionFile(json);
    ASSERT_TRUE(parsed.is_err());
    const auto text = errText(std::move(parsed));
    EXPECT_NE(text.find("proposals[0] (line 5)"), std::string::npos) << text;
    EXPECT_NE(text.find("proposal must be an object"), std::string::npos) << text;
}

// ── 序列化：审阅稿往返 ──

TEST(MjNameSuggestionFileTest, writeRoundTripsContractFields)
{
    const char* json  = R"({
        "version": 1,
        "source": "ai:claude",
        "proposals": [
            {"kind": "method", "owner": "A", "name": "alpha", "desc": "()V",
             "newName": "run", "confidence": 0.75, "rationale": "入口", "evidence": "x"},
            {"kind": "field", "owner": "A", "name": "oldField", "desc": "I",
             "newName": "count", "confidence": 1}
        ]
    })";
    auto        first = parseNameSuggestionFile(json);
    ASSERT_TRUE(first.is_ok()) << errText(std::move(first));

    auto text = writeNameSuggestionText(std::move(first).unwrap());
    ASSERT_TRUE(text.is_ok());
    auto second = parseNameSuggestionFile(std::move(text).unwrap());
    ASSERT_TRUE(second.is_ok()) << errText(std::move(second));

    auto file = std::move(second).unwrap();
    ASSERT_EQ(file.proposals.size(), static_cast<ca::usize>(2));
    EXPECT_EQ(file.proposals[0].kind, ProposalKind::Method);
    EXPECT_EQ(file.proposals[0].owner, "A");
    EXPECT_EQ(file.proposals[0].oldName, "alpha");
    EXPECT_EQ(file.proposals[0].desc, "()V");
    EXPECT_EQ(file.proposals[0].newName, "run");
    EXPECT_DOUBLE_EQ(file.proposals[0].confidence, 0.75);
    EXPECT_EQ(std::string(file.proposals[0].rationale), "入口");
    EXPECT_EQ(file.proposals[0].evidence, "x");
    EXPECT_EQ(file.proposals[1].kind, ProposalKind::Field);
    EXPECT_DOUBLE_EQ(file.proposals[1].confidence, 1.0);
    EXPECT_TRUE(file.proposals[1].rationale.empty());
    EXPECT_TRUE(file.proposals[1].evidence.empty());
}

TEST(MjNameSuggestionFileTest, fileReadWriteRoundTrip)
{
    const char* json = R"({"version": 1, "source": "ai:x",
        "proposals": [{"kind": "class", "name": "A", "newName": "B", "confidence": 0.5}]})";

    const auto jsonPath = test::getTempOutPath("renamer_p4_suggestions_roundtrip.json");
    auto       first    = parseNameSuggestionFile(json);
    ASSERT_TRUE(first.is_ok());
    auto written = writeNameSuggestionFile(jsonPath.string(), std::move(first).unwrap());
    ASSERT_TRUE(written.is_ok()) << std::move(written).unwrap_err().message;

    auto reread = readNameSuggestionFile(jsonPath.string());
    ASSERT_TRUE(reread.is_ok()) << errText(std::move(reread));
    EXPECT_EQ(std::move(reread).unwrap().proposals.size(), static_cast<ca::usize>(1));
}

TEST(MjNameSuggestionFileTest, readFileErrorsCarryPath)
{
    auto missing = readNameSuggestionFile(
        test::getTempOutPath("renamer_p4_no_such_suggestions.json").string());
    ASSERT_TRUE(missing.is_err());
    const auto text = errText(std::move(missing));
    EXPECT_NE(text.find("failed to open suggestion file"), std::string::npos) << text;
    EXPECT_NE(text.find("renamer_p4_no_such_suggestions.json"), std::string::npos) << text;

    const auto brokenPath = test::getTempOutPath("renamer_p4_broken_suggestions.json");
    {
        std::ofstream out(brokenPath, std::ios::binary | std::ios::trunc);
        out << "{\"version\": 99, \"source\": \"ai:x\", \"proposals\": []}";
    }
    auto badVersion = readNameSuggestionFile(brokenPath.string());
    ASSERT_TRUE(badVersion.is_err());
    const auto versionText = errText(std::move(badVersion));
    EXPECT_NE(versionText.find("unsupported schema version"), std::string::npos) << versionText;
    // 路径前缀 + 行号定位并存。
    EXPECT_NE(versionText.find("renamer_p4_broken_suggestions.json"), std::string::npos)
        << versionText;
    EXPECT_NE(versionText.find("line 1"), std::string::npos) << versionText;
}

// ── FileSuggestionProvider ──

TEST(MjFileSuggestionProviderTest, replaysBatchAcrossRunsAndFillsSourceFromFile)
{
    auto parsed = parseNameSuggestionFile(
        R"({"version": 1, "source": "ai:claude",
            "proposals": [
                {"kind": "field", "owner": "A", "name": "oldField", "desc": "I",
                 "newName": "count", "confidence": 0.8, "rationale": "计数"},
                {"kind": "method", "owner": "A", "name": "alpha", "desc": "()V",
                 "newName": "run", "confidence": 0.9}
            ]})");
    ASSERT_TRUE(parsed.is_ok()) << errText(std::move(parsed));

    FileSuggestionProvider provider(std::move(parsed).unwrap());
    EXPECT_EQ(provider.name(), "suggestion-file:ai:claude");

    EngineFixture fixture;
    auto          contextResult = RenameContext::build(fixture.container);
    ASSERT_TRUE(contextResult.is_ok());
    auto  context    = std::move(contextResult).unwrap();
    auto& contextRef = *context;

    auto firstRun = provider.propose(contextRef);
    ASSERT_TRUE(firstRun.is_ok());
    auto batch = std::move(firstRun).unwrap();
    ASSERT_EQ(batch.size(), static_cast<ca::usize>(2));
    EXPECT_EQ(batch[0].source, "ai:claude");
    EXPECT_EQ(std::string(batch[0].rationale), "计数");
    EXPECT_DOUBLE_EQ(batch[0].confidence, 0.8);

    // 第二次 run 产出同一批（克隆语义，provider 可复用）。
    auto secondRun = provider.propose(contextRef);
    ASSERT_TRUE(secondRun.is_ok());
    auto replay = std::move(secondRun).unwrap();
    ASSERT_EQ(replay.size(), static_cast<ca::usize>(2));
    EXPECT_EQ(replay[0].oldName, "oldField");
    EXPECT_EQ(replay[1].oldName, "alpha");
}

// ── provider → 引擎端到端：rationale/evidence 全程流水留痕 ──

TEST(MjFileSuggestionProviderTest, engineRunCarriesRationaleThroughAcceptedAndRejectedLedger)
{
    // count 合法（接受）；bad-name 非法（拒）；两条建议都带 rationale/evidence。
    auto parsed = parseNameSuggestionFile(
        R"({"version": 1, "source": "ai:claude",
            "proposals": [
                {"kind": "field", "owner": "A", "name": "oldField", "desc": "I",
                 "newName": "count", "confidence": 0.8, "rationale": "语义化字段",
                 "evidence": "alpha()V 读它"},
                {"kind": "method", "owner": "A", "name": "alpha", "desc": "()V",
                 "newName": "bad-name", "confidence": 0.9, "rationale": "坏建议",
                 "evidence": "无"}
            ]})");
    ASSERT_TRUE(parsed.is_ok()) << errText(std::move(parsed));

    EngineFixture          fixture;
    RenamerEngine          engine;
    RenamerOptions         options;   // 默认 plan-only
    FileSuggestionProvider provider(std::move(parsed).unwrap());
    std::vector<NameProvider*> providers{&provider};

    const auto result = engine.run(fixture.container, providers, options);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.stats.proposed, static_cast<ca::usize>(2));
    EXPECT_EQ(result.stats.accepted, static_cast<ca::usize>(1));
    EXPECT_EQ(result.stats.rejected, static_cast<ca::usize>(1));

    // accepted 流水带 rationale/evidence + source/confidence 溯源。
    ASSERT_EQ(result.record.accepted().size(), static_cast<ca::usize>(1));
    const auto& accepted = result.record.accepted().front();
    EXPECT_EQ(accepted.kind, ProposalKind::Field);
    EXPECT_EQ(accepted.oldName, "oldField");
    EXPECT_EQ(accepted.newName, "count");
    EXPECT_EQ(accepted.source, "ai:claude");
    EXPECT_EQ(std::string(accepted.rationale), "语义化字段");
    EXPECT_EQ(std::string(accepted.evidence), "alpha()V 读它");

    // rejected 流水原样保留建议全文（含 rationale/evidence）+ 原因。
    ASSERT_EQ(result.record.rejected().size(), static_cast<ca::usize>(1));
    const auto& rejected = result.record.rejected().front();
    EXPECT_EQ(rejected.reason, RejectionReason::IllegalIdentifier);
    EXPECT_EQ(rejected.proposal.newName, "bad-name");
    EXPECT_EQ(std::string(rejected.proposal.rationale), "坏建议");
    EXPECT_EQ(std::string(rejected.proposal.evidence), "无");
}

}   // namespace mjbase::renamer
