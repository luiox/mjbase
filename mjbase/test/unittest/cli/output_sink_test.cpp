#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "mjbase/cli/output_sink.hpp"

using namespace mjbase::cli;

namespace {

// 用 tmpfile 捕获输出，读回字符串断言。tmpfile 在关闭时自动删除。
std::string captureOutput(void (*fn)(OutputSink&))
{
    FILE* f = std::tmpfile();
    if (!f) return "<tmpfile failed>";
    OutputSink sink(f);
    fn(sink);
    std::fflush(f);
    std::rewind(f);
    std::string out;
    char        buf[256];
    while (size_t n = std::fread(buf, 1, sizeof(buf), f)) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

}   // namespace

TEST(MjOutputSinkTest, PrintfBasic)
{
    auto result = captureOutput([](OutputSink& s) { s.printf("Version: %d.%d\n", 52, 0); });
    EXPECT_EQ(result, "Version: 52.0\n");
}

TEST(MjOutputSinkTest, PrintfStringArg)
{
    auto result =
        captureOutput([](OutputSink& s) { s.printf("Error: %s\n", std::string("boom").c_str()); });
    EXPECT_EQ(result, "Error: boom\n");
}

TEST(MjOutputSinkTest, PutsAddsNewline)
{
    auto result = captureOutput([](OutputSink& s) { s.puts("hello"); });
    EXPECT_EQ(result, "hello\n");
}

TEST(MjOutputSinkTest, WriteNoNewline)
{
    auto result = captureOutput([](OutputSink& s) {
        s.write("a");
        s.write("b");
    });
    EXPECT_EQ(result, "ab");
}

TEST(MjOutputSinkTest, FileAccessor)
{
    FILE*      f = std::tmpfile();
    OutputSink sink(f);
    EXPECT_EQ(sink.file(), f);
    std::fclose(f);
}

// ── 全局 out()/err() 默认指向 stdout/stderr ────────────────────
TEST(MjOutputSinkTest, GlobalOutPointsToStdout)
{
    EXPECT_EQ(out().file(), stdout);
    EXPECT_EQ(err().file(), stderr);
}

// ── parseFormat ────────────────────────────────────────────────
TEST(MjParseFormatTest, KnownFormats)
{
    EXPECT_EQ(parseFormat("json"), Format::Json);
    EXPECT_EQ(parseFormat("csv"), Format::Csv);
    EXPECT_EQ(parseFormat("plain"), Format::Plain);
}

TEST(MjParseFormatTest, UnknownDefaultsToPlain)
{
    EXPECT_EQ(parseFormat("xml"), Format::Plain);
    EXPECT_EQ(parseFormat(""), Format::Plain);
}
