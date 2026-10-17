/// @file        keep_cli_test.cpp
/// @brief       keep 命令行暴露面用例（审计 P0#1）。
///
/// 覆盖：长名/短名/等号三种写法、缺失即留空、档位文本 → 枚举、未知档位
/// fail-loud。本面只做搬运与档位解析，规则文件的读入在命令侧——故用例只
/// 断言路径被搬进 CliOptions，不做任何文件系统访问。
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "mjbase/cli/arg_parser.hpp"
#include "mjbase/cli/keep_cli.hpp"

using mjbase::cli::ArgParser;
using mjbase::cli::ArgResult;
using mjbase::cli::CliOptions;
using mjbase::cli::OptKind;
using mjbase::reference::KeepRuleErrorCode;
using mjbase::reference::KeepTier;

namespace {

/// 按 ArgParser 的 argv 契约装配参数串（首参为命令名）。
std::optional<ArgResult> parseArgs(const ArgParser&                 parser,
                                   const std::vector<std::string>& args)
{
    std::vector<std::string> storage(args);
    std::vector<char*>       argv;
    argv.push_back(const_cast<char*>("cmd"));
    for (std::string& s : storage) argv.push_back(const_cast<char*>(s.c_str()));
    return parser.parse(static_cast<int>(argv.size()), argv.data());
}

ArgParser makeParser()
{
    ArgParser parser("cmd -i <jar> -o <out>", "keep 暴露面测试");
    mjbase::cli::addKeepOptions(parser);
    return parser;
}

// ── 解析 → CliOptions ──────────────────────────────────────────

TEST(MjKeepCliTest, parsesKeepFileAndTierByLongName)
{
    const ArgParser          parser = makeParser();
    const auto               result = parseArgs(parser, {"--keep", "rules.txt",
                                                         "--keep-tier", "public"});
    ASSERT_TRUE(result.has_value());
    CliOptions options;
    mjbase::cli::applyKeepOptions(*result, options);
    EXPECT_EQ(options.keepFile, "rules.txt");
    EXPECT_EQ(options.keepTier, "public");
}

TEST(MjKeepCliTest, acceptsShortAliasAndEqualsForm)
{
    {
        const ArgParser parser = makeParser();
        const auto      result = parseArgs(parser, {"-k", "rules.txt"});
        ASSERT_TRUE(result.has_value());
        CliOptions options;
        mjbase::cli::applyKeepOptions(*result, options);
        EXPECT_EQ(options.keepFile, "rules.txt");
        EXPECT_TRUE(options.keepTier.empty());
    }
    {
        const ArgParser parser = makeParser();
        const auto      result = parseArgs(parser, {"--keep=rules.txt", "--keep-tier=public"});
        ASSERT_TRUE(result.has_value());
        CliOptions options;
        mjbase::cli::applyKeepOptions(*result, options);
        EXPECT_EQ(options.keepFile, "rules.txt");
        EXPECT_EQ(options.keepTier, "public");
    }
}

TEST(MjKeepCliTest, absentOptionsLeaveFieldsEmpty)
{
    const ArgParser parser = makeParser();
    const auto      result = parseArgs(parser, {});
    ASSERT_TRUE(result.has_value());
    CliOptions options;
    mjbase::cli::applyKeepOptions(*result, options);
    EXPECT_TRUE(options.keepFile.empty());
    EXPECT_TRUE(options.keepTier.empty());
}

// 未注册 keep option 的解析器：未知 option 解析失败（配置错误稳定失败）。
TEST(MjKeepCliTest, unknownKeepOptionFailsParse)
{
    ArgParser  parser("cmd", "无 keep 注册");
    const auto result = parseArgs(parser, {"--keep", "rules.txt"});
    EXPECT_FALSE(result.has_value());
}

// ── 档位文本 → 枚举 ────────────────────────────────────────────

TEST(MjKeepCliTest, tierTextResolvesToEnum)
{
    EXPECT_EQ(mjbase::cli::keepTierOf("").unwrap(), KeepTier::None);
    EXPECT_EQ(mjbase::cli::keepTierOf("none").unwrap(), KeepTier::None);
    EXPECT_EQ(mjbase::cli::keepTierOf("public").unwrap(), KeepTier::Public);
    EXPECT_EQ(mjbase::cli::keepTierOf("public-protected").unwrap(), KeepTier::PublicProtected);
    EXPECT_EQ(mjbase::cli::keepTierOf("public_protected").unwrap(), KeepTier::PublicProtected);
}

// 未知档位 fail-loud：不静默降级为 none（否则库型 JAR 的 protected 面会被删）。
TEST(MjKeepCliTest, unknownTierFailsLoud)
{
    const auto r = mjbase::cli::keepTierOf("public-er");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), KeepRuleErrorCode::UnknownTier);

    CliOptions options;
    options.keepTier = "PUBLIC";   // 大小写敏感：拼错即失败，不猜测
    const auto fromOptions = mjbase::cli::keepTierOf(options);
    ASSERT_TRUE(fromOptions.is_err());
    EXPECT_EQ(fromOptions.unwrap_err(), KeepRuleErrorCode::UnknownTier);
}

}   // namespace
