/// @file        jar_reader_writer_test.cpp
/// @brief       jar 读/写测试：读真 jar、缺文件错误、写读回环、空 jar、UTF-8 条目名回环。
///
/// 5 用例断言逐一对应：
/// throw → Result 错误通道（is_err 断言），std::string → Utf8String。
/// readHelloJar 的 fixture 借用 mbase 冻结线的 test_resource（单测 rundir 是
/// 仓库根，路径与 mbase 原用例字面相同）；临时产物按约定只落 <root>/test/。
#include <gtest/gtest.h>
#include <cstdio>

#include <vector>

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_writer.hpp"

using namespace mjbase::jar;

namespace {

/// C 字符串字面量 → 借用视图（字面量静态存储，视图长期有效）。
ca::str::Utf8StringRef S(const char* s) { return ca::str::Utf8StringRef::from_cstr(s); }

}   // namespace

// ── JvmJarReader ────────────────────────────────────────────────
TEST(MjJarReaderTest, readHelloJar)
{
    JvmJarReader reader(S("mjbase/test_resource/optimize/hello.jar"));
    int          entryCount = 0;
    bool         foundClass = false;
    auto result = reader.read([&](const ca::str::Utf8String& name, const std::vector<ca::u8>& data) {
        (void)data;
        entryCount++;
        if (name.byte_length() > 6 &&
            name.ref().slice(name.byte_length() - 6, name.byte_length()).equals(".class"))
            foundClass = true;
    });
    ASSERT_TRUE(result.is_ok());
    EXPECT_GT(entryCount, 0);
    EXPECT_TRUE(foundClass);
}

TEST(MjJarReaderTest, readNonExistentJar)
{
    // JvmJarReader 在文件不存在时失败：mbase 抛异常 → mjbase 返回 Err(FileNotFound)。
    JvmJarReader reader(S("nonexistent.jar"));
    auto         result = reader.read([](const ca::str::Utf8String&, const std::vector<ca::u8>&) {});
    ASSERT_TRUE(result.is_err());
    const auto kind = std::move(result).unwrap_err().kind;
    EXPECT_EQ(kind, JarError::Kind::FileNotFound);
}

// ── JarWriter + JvmJarReader roundtrip ──────────────────────────
TEST(MjJarWriterTest, writeAndReadRoundtrip)
{
    const char* tmpPath = "test/tmp_jar_test.jar";

    // 写入：只写 class，不写 MANIFEST.MF（写入器不额外补充）
    JarWriter writer(S(tmpPath));
    auto writeResult = writer.write([&](IEntryWriter& ew) {
        ew.writeEntry(S("Hello.class"), std::vector<ca::u8>{0xCA, 0xFE, 0xBA, 0xBE});
    });
    ASSERT_TRUE(writeResult.is_ok());

    // 读取验证
    JvmJarReader reader(S(tmpPath));
    bool         foundClass = false;
    auto readResult = reader.read([&](const ca::str::Utf8String& name, const std::vector<ca::u8>& data) {
        if (name == "Hello.class") {
            foundClass = true;
            EXPECT_EQ(data.size(), 4u);
        }
    });
    ASSERT_TRUE(readResult.is_ok());
    EXPECT_TRUE(foundClass);

    // 清理
    std::remove(tmpPath);
}

TEST(MjJarWriterTest, writeEmptyJar)
{
    const char* tmpPath = "test/tmp_empty.jar";
    JarWriter   writer(S(tmpPath));
    auto        writeResult = writer.write([](IEntryWriter&) {});
    ASSERT_TRUE(writeResult.is_ok());

    JvmJarReader reader(S(tmpPath));
    int          entryCount = 0;
    auto readResult = reader.read([&](const ca::str::Utf8String&, const std::vector<ca::u8>&) {
        entryCount++;
    });
    ASSERT_TRUE(readResult.is_ok());
    EXPECT_GE(entryCount, 0);
    std::remove(tmpPath);
}

// 条目名边界：非 ASCII（UTF-8）类名条目——真实世界混淆 jar（中文资源名/
// 类名）依赖 zip UTF-8 名称通路。写入→读回 URI 与内容必须逐字节保真。
TEST(MjJarWriterTest, writeAndReadRoundtripWithUtf8EntryName)
{
    const char* tmpPath = "test/tmp_jar_utf8_test.jar";

    const ca::str::Utf8String utf8Name = ca::str::Utf8String::from_cstr(u8"com/example/中文类.class");
    const ca::str::Utf8String textName = ca::str::Utf8String::from_cstr(u8"资源/说明.txt");
    const std::vector<ca::u8> payload  = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34};

    JarWriter writer(S(tmpPath));
    auto writeResult = writer.write([&](IEntryWriter& ew) {
        ew.writeEntry(utf8Name.ref(), payload);
        ew.writeEntry(textName.ref(), std::vector<ca::u8>{0x41});
    });
    ASSERT_TRUE(writeResult.is_ok());

    // JarWriter 会为路径分量自动补目录条目（"com/"、"com/example/"、"资源/"），
    // 读取端会收到它们：2 个文件条目 + 3 个目录条目。目录条目名以 '/' 结尾。
    int  fileEntries = 0;
    int  dirEntries  = 0;
    bool foundClass  = false;
    bool foundText   = false;
    JvmJarReader reader(S(tmpPath));
    auto readResult = reader.read([&](const ca::str::Utf8String& name, const std::vector<ca::u8>& data) {
        if (!name.is_empty() && name.byte_at(name.byte_length() - 1) == '/') {
            ++dirEntries;
            return;
        }
        ++fileEntries;
        if (name == utf8Name) {
            foundClass = true;
            EXPECT_EQ(data, payload);
        }
        else if (name == textName) {
            foundText = true;
            EXPECT_EQ(data.size(), 1u);
        }
    });
    ASSERT_TRUE(readResult.is_ok());
    EXPECT_EQ(fileEntries, 2);
    EXPECT_EQ(dirEntries, 3);
    EXPECT_TRUE(foundClass);
    EXPECT_TRUE(foundText);

    std::remove(tmpPath);
}
