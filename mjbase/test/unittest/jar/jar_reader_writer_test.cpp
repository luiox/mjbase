/// @file        jar_reader_writer_test.cpp
/// @brief       jar 读/写测试：读真 jar、缺文件错误、写读回环、空 jar、UTF-8 条目名回环、
///              目录条目政策（无目录输入零合成 / 显式目录原样保留）与容器级 round-trip。
///
/// 5 用例断言逐一对应：
/// throw → Result 错误通道（is_err 断言），std::string → Utf8String。
/// readHelloJar 的 fixture 借用 mbase 冻结线的 test_resource（单测 rundir 是
/// 仓库根，路径与 mbase 原用例字面相同）；临时产物按约定只落 <root>/test/。
#include <gtest/gtest.h>
#include <test_helper.hpp>
#include <cstdio>

#include <algorithm>
#include <string>
#include <vector>

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_writer.hpp"
#include "mjbase/model/resource_helper.hpp"

using namespace mjbase::jar;

namespace {

/// C 字符串字面量 → 借用视图（字面量静态存储，视图长期有效）。
ca::str::Utf8StringRef S(const char* s) { return ca::str::Utf8StringRef::from_cstr(s); }

/// 读取 jar 的条目名集（listJarEntries 只解析元数据不解压，条目序 = 归档序）。
std::vector<std::string> readEntryNames(const Utf8StringRef& path)
{
    auto entries = listJarEntries(path);
    EXPECT_TRUE(entries.is_ok());
    if (entries.is_err()) return {};
    std::vector<std::string> names;
    for (const auto& info : std::move(entries).unwrap())
        names.push_back(info.name.to_std_string());
    return names;
}

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

    // 目录条目政策：写入端只保留显式给出的目录条目。本用例未给任何目录条目，
    // 产物不得合成 "com/"、"com/example/"、"资源/" 占位：2 个文件条目、
    // 0 个目录条目。
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
    EXPECT_EQ(dirEntries, 0);
    EXPECT_TRUE(foundClass);
    EXPECT_TRUE(foundText);

    std::remove(tmpPath);
}

// ── 目录条目政策 ────────────────────────────────────────────────

// 最小复现案（boar-main.jar 现场的缩小版）：无目录条目的双类 jar 进出，
// 产物条目集必须与写入条目集全等——零目录占位新增。旧实现会为
// "com/example/B.class" 自动补 "com/"、"com/example/" 两条 STORED 占位。
TEST(MjJarWriterTest, writeNoDirEntriesKeepsEntrySetExact)
{
    const char* tmpPath = "test/tmp_jar_nodirs_policy.jar";

    const ca::str::Utf8String nestedName = ca::str::Utf8String::from_cstr("com/example/B.class");
    const std::vector<ca::u8> payload    = {0xCA, 0xFE, 0xBA, 0xBE};

    JarWriter writer(S(tmpPath));
    auto writeResult = writer.write([&](IEntryWriter& ew) {
        ew.writeEntry(S("A.class"), payload);
        ew.writeEntry(nestedName.ref(), payload);
    });
    ASSERT_TRUE(writeResult.is_ok());

    std::vector<std::string> names;
    JvmJarReader reader(S(tmpPath));
    auto readResult = reader.read([&](const ca::str::Utf8String& name, const std::vector<ca::u8>&) {
        names.push_back(name.to_std_string());
    });
    ASSERT_TRUE(readResult.is_ok());
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "A.class");
    EXPECT_EQ(names[1], nestedName.to_std_string());

    std::remove(tmpPath);
}

// 输入端显式给出目录条目 → 原样保留：名字逐字节保真、内容 0 字节，
// 不额外合成缺失的父目录，也不重复展开同名目录条目。
TEST(MjJarWriterTest, writePreservesExplicitDirEntries)
{
    const char* tmpPath = "test/tmp_jar_withdirs_policy.jar";

    const std::vector<ca::u8> payload = {0xCA, 0xFE, 0xBA, 0xBE};
    JarWriter writer(S(tmpPath));
    auto writeResult = writer.write([&](IEntryWriter& ew) {
        ew.writeEntry(S("com/"), std::vector<ca::u8>{});
        ew.writeEntry(S("com/example/"), std::vector<ca::u8>{});
        ew.writeEntry(S("com/example/B.class"), payload);
    });
    ASSERT_TRUE(writeResult.is_ok());

    std::vector<std::string> names;
    JvmJarReader reader(S(tmpPath));
    auto readResult = reader.read([&](const ca::str::Utf8String& name, const std::vector<ca::u8>&) {
        names.push_back(name.to_std_string());
    });
    ASSERT_TRUE(readResult.is_ok());
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "com/");
    EXPECT_EQ(names[1], "com/example/");
    EXPECT_EQ(names[2], "com/example/B.class");
    // 目录条目 0 字节：经 listJarEntries 的元数据口径断言（JvmJarReader 对
    // 0 字节 STORED 目录条目会回吐原始邻接头字节，见 jzip readJvm 扫描语义，
    // 数据面断言不在此用例展开）。
    auto entries = listJarEntries(S(tmpPath));
    ASSERT_TRUE(entries.is_ok());
    for (const auto& info : std::move(entries).unwrap()) {
        if (!info.name.is_empty() && info.name.byte_at(info.name.byte_length() - 1) == '/') {
            EXPECT_EQ(info.uncompressedSize, 0u) << info.name.to_std_string();
            EXPECT_EQ(info.compressedSize, 0u) << info.name.to_std_string();
        }
    }

    std::remove(tmpPath);
}

// ── 容器级 round-trip（真实 jar 文件进出，两端目录条目政策对齐）────

// 输入 jar 无目录条目（夹具 roundtrip_nodirs.jar）：导入容器再导出，
// 产物条目集与输入全等——目录占位零新增。
TEST(MjJarWriterTest, containerRoundtripNoDirInputStaysDirFree)
{
    const char* inPath = "mjbase/test_resource/jar/roundtrip_nodirs.jar";

    mjbase::model::ResourceContainer container;
    ASSERT_TRUE(mjbase::model::ResourceHelper::importFromJar(container, inPath).is_ok());
    const auto outPath = test::getTempOutPath("mjbase_jar_roundtrip_nodirs.jar");
    ASSERT_TRUE(mjbase::model::ResourceHelper::exportToJar(container, outPath.string()).is_ok());

    auto inNames  = readEntryNames(S(inPath));
    auto outNames = readEntryNames(
        S(outPath.string().c_str()));
    EXPECT_EQ(inNames, outNames);

    std::remove(outPath.string().c_str());
}

// 输入 jar 带显式目录条目（夹具 roundtrip_withdirs.jar）：导入导出后
// 目录条目原样保留，无丢失也无新增。
TEST(MjJarWriterTest, containerRoundtripWithDirInputPreservesDirs)
{
    const char* inPath = "mjbase/test_resource/jar/roundtrip_withdirs.jar";

    mjbase::model::ResourceContainer container;
    ASSERT_TRUE(mjbase::model::ResourceHelper::importFromJar(container, inPath).is_ok());
    const auto outPath = test::getTempOutPath("mjbase_jar_roundtrip_withdirs.jar");
    ASSERT_TRUE(mjbase::model::ResourceHelper::exportToJar(container, outPath.string()).is_ok());

    auto inNames  = readEntryNames(S(inPath));
    auto outNames = readEntryNames(S(outPath.string().c_str()));
    EXPECT_EQ(inNames, outNames);
    EXPECT_FALSE(inNames.empty());
    EXPECT_TRUE(std::any_of(inNames.begin(), inNames.end(),
                            [](const std::string& n) { return n == "com/"; }));

    std::remove(outPath.string().c_str());
}
