#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mjbase/cli/arg_parser.hpp"

#include "libca/i18n/i18n.hpp"
#include "mjbase/cli/i18n_tables.hpp"

using namespace mjbase::cli;

namespace {

// 辅助：构造 char** argv 并解析。返回解析结果（可能为空 = 错误或 -h）。
std::optional<ArgResult> parseArgs(const ArgParser& p, std::vector<std::string> args)
{
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("test"));
    std::vector<std::string>  owned = std::move(args);
    std::vector<std::string*> keep;
    for (auto& s : owned) keep.push_back(&s);
    for (auto* s : keep) argv.push_back(s->data());
    return p.parse(static_cast<int>(argv.size()), argv.data());
}

ArgParseOutcome parseDetailedArgs(const ArgParser& p, std::vector<std::string> args)
{
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("test"));
    std::vector<std::string> owned = std::move(args);
    std::vector<std::string*> keep;
    for (auto& s : owned) keep.push_back(&s);
    for (auto* s : keep) argv.push_back(s->data());
    return p.parseDetailed(static_cast<int>(argv.size()), argv.data());
}

ArgParser makeSampleParser()
{
    ArgParser p("test -i <in> -o <out> [pattern]", "sample parser");
    p.add({"--input", "-i"}, OptKind::String, "<in>", "input file", true)
        .add({"--output", "-o"}, OptKind::String, "<out>", "output file", true)
        .add({"--verbose", "-v"}, OptKind::Flag, "", "verbose")
        .add({"--pass", "-p"}, OptKind::CommaList, "<p>", "passes")
        .add({"--length"}, OptKind::Int, "<n>", "length")
        .add({"pattern"}, OptKind::Positional, "<pat>", "search pattern");
    return p;
}

}   // namespace

// ── 基本 String/Flag/Positional ─────────────────────────────────
TEST(MjArgParserTest, BasicStringAndFlag)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i", "in.jar", "-o", "out.jar", "-v", "mypat"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->getString("--input"), "in.jar");
    EXPECT_EQ(r->getString("--output"), "out.jar");
    EXPECT_TRUE(r->has("--verbose"));
    ASSERT_EQ(r->positionals().size(), 1u);
    EXPECT_EQ(r->positionals()[0], "mypat");
}

// ── --name=value 等号形式（issue 原文 -p=A,B,C）─────────────────
TEST(MjArgParserTest, EqualsForm)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i=in.jar", "-o=out.jar", "--pass=A,B,C"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->getString("--input"), "in.jar");
    EXPECT_EQ(r->getString("--output"), "out.jar");
    auto list = r->getList("--pass");
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0], "A");
    EXPECT_EQ(list[1], "B");
    EXPECT_EQ(list[2], "C");
}

// ── 长短名别名都能用 ────────────────────────────────────────────
TEST(MjArgParserTest, Aliases)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"--input", "a", "--output", "b"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->getString("--input"), "a");

    auto p2 = makeSampleParser();
    auto r2 = parseArgs(p2, {"-i", "a", "-o", "b"});
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->getString("--input"), "a");
}

// ── CommaList 多次出现累加 ──────────────────────────────────────
TEST(MjArgParserTest, CommaListAccumulates)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i", "a", "-o", "b", "-p", "A,B", "-p", "C"});
    ASSERT_TRUE(r.has_value());
    auto list = r->getList("--pass");
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0], "A");
    EXPECT_EQ(list[2], "C");
}

// ── Int 类型 ────────────────────────────────────────────────────
TEST(MjArgParserTest, IntValue)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i", "a", "-o", "b", "--length", "42"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->getInt("--length"), 42);
}

TEST(MjArgParserTest, IntDefault)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i", "a", "-o", "b"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->getInt("--length", 99), 99);
}

// ── 缺必填 → nullopt ────────────────────────────────────────────
TEST(MjArgParserTest, MissingRequired)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i", "a"});   // 缺 -o
    EXPECT_FALSE(r.has_value());
}

// ── 未知 option → nullopt ───────────────────────────────────────
TEST(MjArgParserTest, UnknownOption)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i", "a", "-o", "b", "--bogus"});
    EXPECT_FALSE(r.has_value());
}

// ── -h → nullopt（已打印 help）──────────────────────────────────
TEST(MjArgParserTest, HelpReturnsNullopt)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-h"});
    EXPECT_FALSE(r.has_value());
}

// ── Flag 不吃 =value ────────────────────────────────────────────
TEST(MjArgParserTest, FlagRejectsValue)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i", "a", "-o", "b", "-v=true"});
    EXPECT_FALSE(r.has_value());
}

// ── 多个位置参数 ────────────────────────────────────────────────
TEST(MjArgParserTest, MultiplePositionals)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i", "a", "-o", "b", "pat1", "pat2"});
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->positionals().size(), 2u);
    EXPECT_EQ(r->positionals()[0], "pat1");
    EXPECT_EQ(r->positionals()[1], "pat2");
}

// ── 位置参数但无 Positional spec → 报错 ─────────────────────────
TEST(MjArgParserTest, UnexpectedPositional)
{
    ArgParser p("test -i <in>", "no positional");
    p.add({"--input", "-i"}, OptKind::String, "<in>", "input", true);
    auto r = parseArgs(p, {"-i", "a", "stray"});
    EXPECT_FALSE(r.has_value());
}

// ── 非 Int 值报错 ───────────────────────────────────────────────
TEST(MjArgParserTest, InvalidInt)
{
    auto p = makeSampleParser();
    auto r = parseArgs(p, {"-i", "a", "-o", "b", "--length", "xyz"});
    EXPECT_FALSE(r.has_value());
}

TEST(MjArgParserTest, DetailedOutcomeClassifiesUsageFailures)
{
    auto parser = makeSampleParser();
    auto missing = parseDetailedArgs(parser, {"-i", "a", "-o"});
    EXPECT_FALSE(missing.ok());
    EXPECT_EQ(missing.error(), ArgParseError::MissingValue);

    auto empty = parseDetailedArgs(parser, {"-i", "", "-o", "b"});
    EXPECT_FALSE(empty.ok());
    EXPECT_EQ(empty.error(), ArgParseError::EmptyValue);

    auto unknown = parseDetailedArgs(parser, {"-i", "a", "-o", "b", "--bogus"});
    EXPECT_FALSE(unknown.ok());
    EXPECT_EQ(unknown.error(), ArgParseError::UnknownOption);

    auto invalidInteger = parseDetailedArgs(parser, {"-i", "a", "-o", "b", "--length", "x"});
    EXPECT_FALSE(invalidInteger.ok());
    EXPECT_EQ(invalidInteger.error(), ArgParseError::InvalidInteger);

    auto help = parseDetailedArgs(parser, {"--help"});
    EXPECT_FALSE(help.ok());
    EXPECT_TRUE(help.helpRequested());
    EXPECT_EQ(help.error(), ArgParseError::HelpRequested);
}

// ── 互斥组：组内最多一个 ───────────────────────────────────────
namespace {
ArgParser makeMutexParser()
{
    ArgParser p("test -i <in> (-s <c> | --interface <i>)", "mutex");
    p.add({"--input", "-i"}, OptKind::String, "<in>", "input", true)
        .add({"--super", "-s"}, OptKind::String, "<c>", "super class")
        .add({"--interface"}, OptKind::String, "<i>", "interface");
    p.addMutexGroup({"--super", "--interface"}, true);
    return p;
}
}   // namespace

TEST(MjArgParserTest, MutexGroupSingleChoice)
{
    auto p = makeMutexParser();
    auto r = parseArgs(p, {"-i", "a", "-s", "Base"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->getString("--super"), "Base");
}

TEST(MjArgParserTest, MutexGroupViolation)
{
    auto p = makeMutexParser();
    // 组内给了两个 → 报错
    auto r = parseArgs(p, {"-i", "a", "-s", "Base", "--interface", "I"});
    EXPECT_FALSE(r.has_value());
}

TEST(MjArgParserTest, DetailedOutcomeClassifiesMutexFailure)
{
    auto parser = makeMutexParser();
    auto outcome = parseDetailedArgs(parser, {"-i", "a", "-s", "Base", "--interface", "I"});
    EXPECT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error(), ArgParseError::MutexConflict);
}

TEST(MjArgParserTest, MutexGroupRequiredMissing)
{
    auto p = makeMutexParser();
    // required 组一个都没给 → 报错
    auto r = parseArgs(p, {"-i", "a"});
    EXPECT_FALSE(r.has_value());
}

TEST(MjArgParserTest, MutexGroupNotRequired)
{
    // 非 required 组：全不给也行
    ArgParser p("test (-a | -b)", "opt mutex");
    p.add({"--alpha", "-a"}, OptKind::Flag, "", "a").add({"--beta", "-b"}, OptKind::Flag, "", "b");
    p.addMutexGroup({"--alpha", "--beta"}, false);
    char* av[] = {const_cast<char*>("test")};
    auto  res  = p.parse(1, av);
    ASSERT_TRUE(res.has_value());   // 都不给，非 required，OK
}

// ── 框架文案 i18n(mbase.cli.* 单源表,显式 init 接入)──────────
TEST(MjArgParserTest, FrameworkTextsLocalize)
{
    // 显式注册 mjbase 框架表(幂等;生产路径由各线 main init)。
    ca::i18n::init({mjbase::cli::kCliI18nTableSet}, {});
    ca::i18n::set_lang("zh_CN");

    // 默认 zh_CN:框架文案查表命中(而非回退 key 原样)。
    EXPECT_STREQ(ca::i18n::tr("mbase.cli.usage"), "\xE7\x94\xA8\xE6\xB3\x95");
    EXPECT_STRNE(ca::i18n::tr("mbase.cli.err.unknown_option"), "mbase.cli.err.unknown_option");

    // 切换 en_US:框架文案与占位符消息随之切换。
    ASSERT_TRUE(ca::i18n::set_lang("en_US"));
    EXPECT_STREQ(ca::i18n::tr("mbase.cli.usage"), "Usage");
    EXPECT_STREQ(ca::i18n::trf("mbase.cli.err.invalid_integer", {"--n", "xyz"}).c_str(),
                 "--n expects an integer, got 'xyz'.");

    // zh 占位符消息:{0} 需要整数,得到 '{1}'。
    ASSERT_TRUE(ca::i18n::set_lang("zh_CN"));
    EXPECT_STREQ(ca::i18n::trf("mbase.cli.err.invalid_integer", {"--n", "xyz"}).c_str(),
                 "--n \xE9\x9C\x80\xE8\xA6\x81\xE6\x95\xB4\xE6\x95\xB0\xEF\xBC\x8C\xE5\xBE\x97"
                 "\xE5\x88\xB0 'xyz'\xE3\x80\x82");
}
