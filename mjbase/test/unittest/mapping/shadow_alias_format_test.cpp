// shadow-alias sidecar（"Shadow-Alias" 注册格式，luiox/morpher#1072）：
// registry 注册与宽松口径 + 严格入口 write/read roundtrip + schema 错误逐类
// 拒载 + 文件级 roundtrip + 确定性稳定输出。
#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <utility>

#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/mapping/mapping_format_registry.hpp"
#include "mjbase/mapping/shadow_alias_format.hpp"
#include "test_helper.hpp"

using namespace mjbase::mapping;
using ca::str::Utf8StringRef;

namespace {

Utf8StringRef S(const char* value)
{
    return Utf8StringRef::from_cstr(value);
}

Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

/// 刻意乱序登记（format() 输出要求按真名字典序稳定排列）。
IntermediateMapping buildSample()
{
    IntermediateMapping mappings;
    mappings.addClass(S("z/Z"), S("Class_9"));
    mappings.addMethod(S("a/A"), S("(I)V"), S("run"), S("Method_2"));
    mappings.addClass(S("a/A"), S("Class_1"));
    mappings.addField(S("a/A"), S("J"), S("wide"), S("Field_2"));
    mappings.addField(S("a/A"), S("I"), S("flag"), S("Field_1"));
    mappings.addMethod(S("a/A"), S("()V"), S("<init>"), S("Method_1"));
    return mappings;
}

void expectSampleCore(const IntermediateMapping& mappings)
{
    EXPECT_EQ("Class_1", mappings.getMappedClassName(S("a/A")).to_std_string());
    EXPECT_EQ("Class_9", mappings.getMappedClassName(S("z/Z")).to_std_string());
    EXPECT_EQ("Field_1", mappings.getMappedFieldName(S("a/A"), S("flag"), S("I")).to_std_string());
    EXPECT_EQ("Field_2",
              mappings.getMappedFieldName(S("a/A"), S("wide"), S("J")).to_std_string());
    EXPECT_EQ("Method_1",
              mappings.getMappedMethodName(S("a/A"), S("<init>"), S("()V")).to_std_string());
    EXPECT_EQ("Method_2",
              mappings.getMappedMethodName(S("a/A"), S("run"), S("(I)V")).to_std_string());
}

}   // namespace

TEST(MjMappingShadowAliasTest, registeredInStandardRegistry)
{
    auto registry = MappingFormatRegistry::withStandardFormats();
    EXPECT_TRUE(registry.hasFormat(S("Shadow-Alias")));

    auto format = registry.createFormat(S("Shadow-Alias"));
    ASSERT_NE(format, nullptr);
    EXPECT_EQ("Shadow-Alias", format->getFormatName().to_std_string());
}

TEST(MjMappingShadowAliasTest, strictWriteReadRoundtrip)
{
    auto written = writeShadowAliasText(buildSample());
    ASSERT_FALSE(written.is_err());
    const auto writtenText = std::move(written).unwrap().to_std_string();

    auto parsed = parseShadowAliasText(refOf(writtenText));
    ASSERT_FALSE(parsed.is_err());
    auto mappings = std::move(parsed).unwrap();
    expectSampleCore(mappings);

    // parse(write(parse(x))) 关键字段恒等：再序列化一次，文本逐字节一致。
    auto rewritten = writeShadowAliasText(mappings);
    ASSERT_FALSE(rewritten.is_err());
    EXPECT_EQ(writtenText, std::move(rewritten).unwrap().to_std_string());
}

TEST(MjMappingShadowAliasTest, fileRoundtripViaTempPath)
{
    const auto path = test::getTempOutPath("shadow_alias_file_roundtrip.mjt-shadow");

    auto written = writeShadowAliasFile(refOf(path.string()), buildSample());
    ASSERT_FALSE(written.is_err());

    auto read = readShadowAliasFile(refOf(path.string()));
    ASSERT_FALSE(read.is_err());
    expectSampleCore(std::move(read).unwrap());
}

TEST(MjMappingShadowAliasTest, strictParseRejectsSchemaViolations)
{
    struct Case
    {
        const char*              json;
        ShadowAliasFileErrorKind kind;
    };
    const Case cases[] = {
        // JSON 语法错误
        {"{\"version\": 1, \"aliases\": [}", ShadowAliasFileErrorKind::JsonRead},
        // 版本缺失 / 不识别
        {"{\"aliases\": []}", ShadowAliasFileErrorKind::Schema},
        {"{\"version\": 99, \"aliases\": []}", ShadowAliasFileErrorKind::Schema},
        // 未知 kind
        {"{\"version\": 1, \"aliases\": [{\"kind\": \"package\", \"name\": \"a\", "
         "\"shadow\": \"b\"}]}",
         ShadowAliasFileErrorKind::Schema},
        // shadow 缺失
        {"{\"version\": 1, \"aliases\": [{\"kind\": \"class\", \"name\": \"a\"}]}",
         ShadowAliasFileErrorKind::Schema},
        // 成员条目缺 owner/desc
        {"{\"version\": 1, \"aliases\": [{\"kind\": \"method\", \"name\": \"m\", "
         "\"shadow\": \"M\"}]}",
         ShadowAliasFileErrorKind::Schema},
        // 同一 class 符号重复登记
        {"{\"version\": 1, \"aliases\": ["
         "{\"kind\": \"class\", \"name\": \"a/A\", \"shadow\": \"X\"},"
         "{\"kind\": \"class\", \"name\": \"a/A\", \"shadow\": \"Y\"}]}",
         ShadowAliasFileErrorKind::Schema},
    };
    for (const auto& item : cases) {
        auto result = parseShadowAliasText(S(item.json));
        ASSERT_TRUE(result.is_err()) << item.json;
        EXPECT_EQ(item.kind, std::move(result).unwrap_err().kind) << item.json;
    }
}

TEST(MjMappingShadowAliasTest, registryParseIsLenientAndLive)
{
    auto registry = MappingFormatRegistry::withStandardFormats();

    // 宽松口径：schema 错误 → Ok(空映射)（注册表"静默跳过"语义，格式实现
    // 自吞错误不产生 Err）。
    auto broken = registry.parse(S("Shadow-Alias"), S("{\"aliases\": []}"));
    ASSERT_TRUE(broken.is_ok());
    EXPECT_TRUE(std::move(broken).unwrap().isEmpty());

    // 活映射：registry.parse/format 与严格入口同一数据面。
    auto parsed = registry.parse(
        S("Shadow-Alias"),
        S("{\"version\": 1, \"aliases\": [{\"kind\": \"class\", \"name\": \"a/A\", "
          "\"shadow\": \"Class_1\"}]}"));
    ASSERT_TRUE(parsed.is_ok());
    const auto mappings = std::move(parsed).unwrap();
    EXPECT_EQ("Class_1", mappings.getMappedClassName(S("a/A")).to_std_string());

    IntermediateMapping sample;
    sample.addClass(S("a/A"), S("Class_1"));
    auto formatted = registry.format(S("Shadow-Alias"), sample);
    ASSERT_TRUE(formatted.is_ok());
    const auto text = std::move(formatted).unwrap();
    EXPECT_NE(std::string::npos, text.to_std_string().find("\"Class_1\""));
}

TEST(MjMappingShadowAliasTest, formatOutputIsDeterministicAndSorted)
{
    auto first  = writeShadowAliasText(buildSample());
    auto second = writeShadowAliasText(buildSample());
    ASSERT_FALSE(first.is_err());
    ASSERT_FALSE(second.is_err());
    const auto text = std::move(first).unwrap().to_std_string();
    EXPECT_EQ(text, std::move(second).unwrap().to_std_string());

    // class 条目按真名字典序：a/A 在 z/Z 之前。
    EXPECT_NE(std::string::npos, text.find("\"a/A\""));
    EXPECT_NE(std::string::npos, text.find("\"z/Z\""));
    EXPECT_LT(text.find("\"a/A\""), text.find("\"z/Z\""));
}

TEST(MjMappingShadowAliasTest, orphanMemberEntriesSurviveRoundtrip)
{
    // R27 P2 回归样本：owner 无 class 条目的成员条目（手编/局部 sidecar 常态）
    // + 一条正常 class 条目。写出不得静默丢孤儿成员（--prune 重写即触发丢数据
    // 的根因就是分组骨架只取 getClasses() 键）；hpp 验收口径
    // parse(write(parse(x))) 关键字段恒等在此含孤儿样本钉死。
    IntermediateMapping mappings;
    mappings.addClass(S("a/A"), S("Class_1"));
    mappings.addField(S("orphan/E"), S("J"), S("wide"), S("Field_9"));
    mappings.addMethod(S("orphan/E"), S("()V"), S("go"), S("Method_9"));

    auto written = writeShadowAliasText(mappings);
    ASSERT_FALSE(written.is_err());
    const auto text = std::move(written).unwrap().to_std_string();

    // class 与孤儿成员条目全部落文本，且 owner 键并入同一字典序骨架（a/A < orphan/E）。
    EXPECT_NE(std::string::npos, text.find("\"a/A\"")) << text;
    EXPECT_NE(std::string::npos, text.find("\"orphan/E\"")) << text;
    EXPECT_LT(text.find("\"a/A\""), text.find("\"orphan/E\"")) << text;

    auto parsed = parseShadowAliasText(refOf(text));
    ASSERT_FALSE(parsed.is_err()) << text;
    auto roundtripped = std::move(parsed).unwrap();
    EXPECT_EQ("Class_1", roundtripped.getMappedClassName(S("a/A")).to_std_string());
    EXPECT_EQ("Field_9",
              roundtripped.getMappedFieldName(S("orphan/E"), S("wide"), S("J")).to_std_string());
    EXPECT_EQ("Method_9",
              roundtripped.getMappedMethodName(S("orphan/E"), S("go"), S("()V")).to_std_string());

    // parse(write(parse(x))) 再序列化逐字节一致。
    auto rewritten = writeShadowAliasText(roundtripped);
    ASSERT_FALSE(rewritten.is_err());
    EXPECT_EQ(text, std::move(rewritten).unwrap().to_std_string());
}

TEST(MjMappingShadowAliasTest, rejectsForeignMappingText)
{
    // 与其它 remap 格式互不混读：拿 Tiny-V2 文本喂 Shadow-Alias 严格入口必须拒。
    auto result = parseShadowAliasText(S("c\tmcp/Subject\tmcp/Clean\n"));
    ASSERT_TRUE(result.is_err());
}

// ═══════════════════════════════════════════════════════════════
// #1156 shadow 引用 token —— v2 加法式 bump（T1 切片）
// ═══════════════════════════════════════════════════════════════

TEST(MjMappingShadowAliasTest, tokenColumnRoundtripsByteFidelity)
{
    // v2 token 列逐字节保真 + parse(write(parse(x))) 文本恒等；混入一条无
    // token 条目（空 token 加法式省略列，不得写空串）。
    IntermediateMapping mappings;
    mappings.addClass(S("a/A"), S("Class_1"), S("0123abcd"));
    mappings.addMethod(S("a/A"), S("(I)V"), S("run"), S("Method_1"), S("deadbeef"));
    mappings.addField(S("a/A"), S("I"), S("flag"), S("Field_1"), S("00000000"));
    mappings.addClass(S("z/Z"), S("Class_9"));   // 无 token（legacy 条目）

    auto written = writeShadowAliasText(mappings);
    ASSERT_FALSE(written.is_err());
    const auto text = std::move(written).unwrap().to_std_string();
    EXPECT_NE(std::string::npos, text.find("\"version\": 2")) << text;
    EXPECT_NE(std::string::npos, text.find("\"0123abcd\"")) << text;
    EXPECT_NE(std::string::npos, text.find("\"deadbeef\"")) << text;
    EXPECT_NE(std::string::npos, text.find("\"00000000\"")) << text;

    auto parsed = parseShadowAliasText(refOf(text));
    ASSERT_FALSE(parsed.is_err()) << text;
    auto back = std::move(parsed).unwrap();

    // token 逐字节保真（class 直取；成员经 owner 组取首条）。
    const auto* cm = back.getClassMapping(S("a/A"));
    ASSERT_NE(nullptr, cm);
    EXPECT_EQ("0123abcd", cm->token.to_std_string());
    const auto* cm9 = back.getClassMapping(S("z/Z"));
    ASSERT_NE(nullptr, cm9);
    EXPECT_TRUE(cm9->token.is_empty());   // 无 token 条目：留空

    const auto* methods = back.getClassMethodMappings(S("a/A"));
    ASSERT_NE(nullptr, methods);
    ASSERT_EQ(1U, methods->size());
    EXPECT_EQ("deadbeef", (*methods)[0].token.to_std_string());

    const auto* fields = back.getClassFieldMappings(S("a/A"));
    ASSERT_NE(nullptr, fields);
    ASSERT_EQ(1U, fields->size());
    EXPECT_EQ("00000000", (*fields)[0].token.to_std_string());

    // 再序列化逐字节一致（稳定序 + token 列还原）。
    auto rewritten = writeShadowAliasText(back);
    ASSERT_FALSE(rewritten.is_err());
    EXPECT_EQ(text, std::move(rewritten).unwrap().to_std_string());
}

TEST(MjMappingShadowAliasTest, legacyV1LoadsTokensEmptyFieldsIntact)
{
    // 加法式 bump 的核心兼容分支：旧格式（无 token 列）照常加载，token 留空、
    // 其余字段无损。
    const char* v1 =
        "{\n  \"version\": 1,\n  \"aliases\": [\n"
        "    {\"kind\": \"class\", \"name\": \"a/A\", \"shadow\": \"Class_1\"},\n"
        "    {\"kind\": \"method\", \"owner\": \"a/A\", \"desc\": \"(I)V\", "
        "\"name\": \"run\", \"shadow\": \"Method_2\"},\n"
        "    {\"kind\": \"field\", \"owner\": \"a/A\", \"desc\": \"I\", "
        "\"name\": \"flag\", \"shadow\": \"Field_3\"}\n  ]\n}\n";

    auto parsed = parseShadowAliasText(S(v1));
    ASSERT_FALSE(parsed.is_err());
    auto mappings = std::move(parsed).unwrap();

    // 其余字段无损（与 v1 时代同一关键读数）。
    EXPECT_EQ("Class_1", mappings.getMappedClassName(S("a/A")).to_std_string());
    EXPECT_EQ("Method_2",
              mappings.getMappedMethodName(S("a/A"), S("run"), S("(I)V")).to_std_string());
    EXPECT_EQ("Field_3", mappings.getMappedFieldName(S("a/A"), S("flag"), S("I")).to_std_string());

    EXPECT_TRUE(mappings.getClassMapping(S("a/A"))->token.is_empty());
    const auto* methods = mappings.getClassMethodMappings(S("a/A"));
    ASSERT_NE(nullptr, methods);
    EXPECT_TRUE((*methods)[0].token.is_empty());
    const auto* fields = mappings.getClassFieldMappings(S("a/A"));
    ASSERT_NE(nullptr, fields);
    EXPECT_TRUE((*fields)[0].token.is_empty());
}

TEST(MjMappingShadowAliasTest, versionBumpAdditiveCompatPinned)
{
    // 写出恒为 version 2（bump 钉子）。
    auto written = writeShadowAliasText(IntermediateMapping());
    ASSERT_FALSE(written.is_err());
    const auto text = std::move(written).unwrap().to_std_string();
    EXPECT_NE(std::string::npos, text.find("\"version\": 2")) << text;
    EXPECT_EQ(std::string::npos, text.find("\"version\": 1")) << text;

    // v1 文件出现 token 列 → Schema 拒载（版本门控新列）。
    auto v1WithToken = parseShadowAliasText(
        S("{\"version\": 1, \"aliases\": [{\"kind\": \"class\", \"name\": \"a/A\", "
          "\"shadow\": \"Class_1\", \"token\": \"0123abcd\"}]}"));
    ASSERT_TRUE(v1WithToken.is_err());
    EXPECT_EQ(ShadowAliasFileErrorKind::Schema, std::move(v1WithToken).unwrap_err().kind);

    // 版本 0 / 3 拒载；消息声明支持域（加法式窗口）。
    for (const char* bogus : {"0", "3"}) {
        const std::string json =
            std::string("{\"version\": ") + bogus +
            ", \"aliases\": [{\"kind\": \"class\", \"name\": \"a/A\", \"shadow\": \"X\"}]}";
        auto result = parseShadowAliasText(refOf(json));
        ASSERT_TRUE(result.is_err()) << json;
        auto error = std::move(result).unwrap_err();
        EXPECT_EQ(ShadowAliasFileErrorKind::Schema, error.kind) << json;
        EXPECT_NE(std::string::npos, error.message.to_std_string().find("supported: 1-2"))
            << json;
    }

    // v2 无 token 列 → 加载成功、token 空（列本就可选）。
    auto v2Bare = parseShadowAliasText(
        S("{\"version\": 2, \"aliases\": [{\"kind\": \"class\", \"name\": \"a/A\", "
          "\"shadow\": \"Class_1\"}]}"));
    ASSERT_FALSE(v2Bare.is_err());
    auto bare = std::move(v2Bare).unwrap();
    EXPECT_EQ("Class_1", bare.getMappedClassName(S("a/A")).to_std_string());
    EXPECT_TRUE(bare.getClassMapping(S("a/A"))->token.is_empty());

    // token 形状违约 → Schema 拒载：大写 / 过短 / 过长 / 非 hex。
    const char* badTokens[] = {"0123ABCD", "0123abc", "0123abcde", "xyzwqrst", "0123abcg"};
    for (const char* bad : badTokens) {
        const std::string json =
            std::string("{\"version\": 2, \"aliases\": [{\"kind\": \"class\", \"name\": "
                        "\"a/A\", \"shadow\": \"X\", \"token\": \"") +
            bad + "\"}]}";
        auto result = parseShadowAliasText(refOf(json));
        ASSERT_TRUE(result.is_err()) << json;
        EXPECT_EQ(ShadowAliasFileErrorKind::Schema, std::move(result).unwrap_err().kind) << json;
    }

    // 形状边界样本：全 0 / 全 f 合法（uint32 %08x 全值域两端）。
    for (const char* good : {"00000000", "ffffffff"}) {
        const std::string json =
            std::string("{\"version\": 2, \"aliases\": [{\"kind\": \"class\", \"name\": "
                        "\"a/A\", \"shadow\": \"X\", \"token\": \"") +
            good + "\"}]}";
        auto result = parseShadowAliasText(refOf(json));
        ASSERT_FALSE(result.is_err()) << json;
        EXPECT_EQ(good, std::move(result).unwrap().getClassMapping(S("a/A"))->token.to_std_string())
            << json;
    }
}

TEST(MjMappingShadowAliasTest, duplicateTokenAcrossEntriesRejectsLoadFailClosed)
{
    // T1 落刀 N3：token 表内唯一不能只靠生成路径保证——手编 v2 文件两符号持
    // 同一非空 token8 必须整文件拒载（fail-closed 硬报错，不静默去重"修复"），
    // 诊断点名两条冲突条目的 owner（消歧：谁和谁撞号）。
    const char* duplicate =
        "{\"version\": 2, \"aliases\": ["
        "{\"kind\": \"class\", \"name\": \"a/A\", \"shadow\": \"Class_1\", \"token\": \"0123abcd\"},"
        "{\"kind\": \"class\", \"name\": \"z/Z\", \"shadow\": \"Class_2\", \"token\": \"0123abcd\"}]}";
    auto parsed = parseShadowAliasText(S(duplicate));
    ASSERT_TRUE(parsed.is_err());
    auto error = std::move(parsed).unwrap_err();
    EXPECT_EQ(ShadowAliasFileErrorKind::Schema, error.kind);
    const auto message = error.message.to_std_string();
    // 条目定位 + 撞号 token + 两条冲突 owner 全部在诊断里。
    EXPECT_NE(std::string::npos, message.find("aliases[1]:")) << message;
    EXPECT_NE(std::string::npos, message.find("duplicate token '0123abcd'")) << message;
    EXPECT_NE(std::string::npos, message.find("a/A")) << message;
    EXPECT_NE(std::string::npos, message.find("z/Z")) << message;
    EXPECT_NE(std::string::npos, message.find("refusing to load")) << message;

    // 跨 kind 同样拒载：class(z/Z) 与 method(owner a/A) 持同号——唯一性是
    // 全表面（生成侧 taken 集合不分 kind），诊断两条 owner 名照常点名。
    const char* crossKind =
        "{\"version\": 2, \"aliases\": ["
        "{\"kind\": \"class\", \"name\": \"z/Z\", \"shadow\": \"Class_1\", \"token\": \"deadbeef\"},"
        "{\"kind\": \"method\", \"owner\": \"a/A\", \"desc\": \"(I)V\", \"name\": \"run\", "
        "\"shadow\": \"Method_1\", \"token\": \"deadbeef\"}]}";
    auto cross = parseShadowAliasText(S(crossKind));
    ASSERT_TRUE(cross.is_err());
    const auto crossMessage = std::move(cross).unwrap_err().message.to_std_string();
    EXPECT_NE(std::string::npos, crossMessage.find("duplicate token 'deadbeef'")) << crossMessage;
    EXPECT_NE(std::string::npos, crossMessage.find("z/Z")) << crossMessage;
    EXPECT_NE(std::string::npos, crossMessage.find("a/A")) << crossMessage;

    // 手编表文件走文件级入口同样拒载（消息带路径 + 双 owner 诊断）。
    const auto path = test::getTempOutPath("shadow_alias_dup_token.mjt-shadow");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << duplicate;
    }
    auto fromFile = readShadowAliasFile(refOf(path.string()));
    ASSERT_TRUE(fromFile.is_err());
    const auto fileMessage = std::move(fromFile).unwrap_err().message.to_std_string();
    EXPECT_NE(std::string::npos, fileMessage.find("a/A")) << fileMessage;
    EXPECT_NE(std::string::npos, fileMessage.find("z/Z")) << fileMessage;

    // 对照组：同文件两符号各持其号 + 一条空 token（v1 遗留）照常加载——
    // 拒载面只对真实撞号，不误伤合法手编文件。
    const char* distinct =
        "{\"version\": 2, \"aliases\": ["
        "{\"kind\": \"class\", \"name\": \"a/A\", \"shadow\": \"Class_1\", \"token\": \"0123abcd\"},"
        "{\"kind\": \"class\", \"name\": \"z/Z\", \"shadow\": \"Class_2\", \"token\": \"deadbeef\"},"
        "{\"kind\": \"method\", \"owner\": \"a/A\", \"desc\": \"(I)V\", \"name\": \"run\", "
        "\"shadow\": \"Method_1\"}]}";
    auto ok = parseShadowAliasText(S(distinct));
    ASSERT_FALSE(ok.is_err());
    auto mappings = std::move(ok).unwrap();
    EXPECT_EQ("0123abcd", mappings.getClassMapping(S("a/A"))->token.to_std_string());
    EXPECT_EQ("deadbeef", mappings.getClassMapping(S("z/Z"))->token.to_std_string());
    EXPECT_TRUE((*mappings.getClassMethodMappings(S("a/A")))[0].token.is_empty());
}
