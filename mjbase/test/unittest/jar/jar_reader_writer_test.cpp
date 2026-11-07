/// @file        jar_reader_writer_test.cpp
/// @brief       jar 读/写测试：读真 jar、缺文件错误、写读回环、空 jar、UTF-8 条目名回环、
///              目录条目政策（无目录输入零合成 / 显式目录原样保留）、零长条目内容回空
///              （jzip readJvm 签名扫描推断的读侧收口）与容器级 round-trip。
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
#include <utility>
#include <vector>

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_writer.hpp"
#include "mjbase/model/resource_helper.hpp"

using namespace mjbase::jar;

namespace {

/// C 字符串字面量 → 借用视图（字面量静态存储，视图长期有效）。
ca::str::Utf8StringRef S(const char* s) { return ca::str::Utf8StringRef::from_cstr(s); }

/// 读取 jar 全部条目的名字与内容字节（JvmJarReader 数据面，条目序 = 归档序）。
std::vector<std::pair<std::string, std::vector<ca::u8>>> readEntries(const Utf8StringRef& path)
{
    JvmJarReader                                             reader(path);
    std::vector<std::pair<std::string, std::vector<ca::u8>>> out;
    auto result = reader.read([&](const ca::str::Utf8String& name, const std::vector<ca::u8>& data) {
        out.emplace_back(name.to_std_string(), data);
    });
    EXPECT_TRUE(result.is_ok());
    if (result.is_err()) out.clear();
    return out;
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

// ── 零长条目读侧 guard（jzip readJvm 签名扫描推断的收口）──────────

// 0 字节 STORED 条目按规范回空内容，与条目名无关（目录占位条目与普通 0 字节
// 文件条目同路径）。未收口的旧行为：jzip 的 JvmZipReader 对 0 字节 STORED
// 条目按"扫到下一签名"推断数据区，把相邻原始头字节当条目内容回吐（字节级
// 取证：目录条目在前回吐 77 字节邻接 LFH+数据、目录条目收尾回吐 99 字节
// CEN）；CEN 收养侧对显式大小 0 不重对齐，垃圾直达 STORED 直通。夹具为
// 手工构造的确定性 jar（test_resource/jar/zero_size_*.jar，python zipfile
// 可正常打开）。
TEST(MjJarReaderTest, readZeroSizeStoredEntriesReturnEmptyContent)
{
    const std::vector<ca::u8> payloadA = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                          0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                                          0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                          0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
                                          0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                                          0x28, 0x29};

    struct Case
    {
        const char*         jar;
        std::vector<size_t> emptyIndices;   // 期待空内容的条目位序
        size_t              dataIndex;      // 期待 42 字节 payload 的条目位序
    };
    const Case cases[] = {
        {"mjbase/test_resource/jar/zero_size_dir_then_file.jar", {0}, 1},
        {"mjbase/test_resource/jar/zero_size_dir_last.jar", {1}, 0},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.jar);
        const auto entries = readEntries(S(c.jar));
        ASSERT_EQ(entries.size(), 2u);
        for (const size_t i : c.emptyIndices) {
            EXPECT_TRUE(entries[i].second.empty())
                << "entry: " << entries[i].first << " got " << entries[i].second.size()
                << " bytes of garbage";
        }
        EXPECT_EQ(entries[c.dataIndex].second, payloadA);
    }

    // 普通 0 字节 STORED 文件条目：与目录条目同一 guard 通路，内容回空。
    const auto fileCase = readEntries(S("mjbase/test_resource/jar/zero_size_file_then_file.jar"));
    ASSERT_EQ(fileCase.size(), 2u);
    EXPECT_EQ(fileCase[0].first, "empty.bin");
    EXPECT_TRUE(fileCase[0].second.empty());
    EXPECT_EQ(fileCase[1].first, "b.txt");
    EXPECT_EQ(fileCase[1].second.size(), 30u);
    EXPECT_EQ(fileCase[1].second[0], ca::u8{0x42});
}

// 0 字节 DEFLATED 条目（空 deflate 流占 2 字节，现实工具链写法）：内容按规范
// 回空。guard 统一按 CEN 权威未压缩大小收口，DEFLATED 空条目同样不进解压通路
// （空流解压结果亦为空，行为一致）。
TEST(MjJarReaderTest, readZeroSizeDeflatedEntryReturnsEmptyContent)
{
    const auto entries = readEntries(S("mjbase/test_resource/jar/zero_size_deflated.jar"));
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "z.txt");
    EXPECT_TRUE(entries[0].second.empty());
    EXPECT_EQ(entries[1].first, "c.txt");
    EXPECT_EQ(entries[1].second.size(), 20u);
    EXPECT_EQ(entries[1].second[0], ca::u8{0x43});
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
    // 目录条目 0 字节：元数据口径（listJarEntries）与数据面口径（JvmJarReader
    // 内容回空，零长条目读侧 guard）双断言。
    auto entries = listJarEntries(S(tmpPath));
    ASSERT_TRUE(entries.is_ok());
    for (const auto& info : std::move(entries).unwrap()) {
        if (!info.name.is_empty() && info.name.byte_at(info.name.byte_length() - 1) == '/') {
            EXPECT_EQ(info.uncompressedSize, 0u) << info.name.to_std_string();
            EXPECT_EQ(info.compressedSize, 0u) << info.name.to_std_string();
        }
    }
    const auto contents = readEntries(S(tmpPath));
    ASSERT_EQ(contents.size(), 3u);
    for (const auto& entry : contents) {
        if (!entry.first.empty() && entry.first.back() == '/') {
            EXPECT_TRUE(entry.second.empty()) << "entry: " << entry.first;
        }
    }

    std::remove(tmpPath);
}

// ── 容器级 round-trip（真实 jar 文件进出，两端目录条目政策对齐）────

// 输入 jar 无目录条目（夹具 roundtrip_nodirs.jar）：导入容器再导出，
// 产物条目集与输入全等——目录占位零新增。对比口径为逐条目名字 + 内容字节
// 全等（含条目序）。
TEST(MjJarWriterTest, containerRoundtripNoDirInputStaysDirFree)
{
    const char* inPath = "mjbase/test_resource/jar/roundtrip_nodirs.jar";

    mjbase::model::ResourceContainer container;
    ASSERT_TRUE(mjbase::model::ResourceHelper::importFromJar(container, inPath).is_ok());
    const auto outPath = test::getTempOutPath("mjbase_jar_roundtrip_nodirs.jar");
    ASSERT_TRUE(mjbase::model::ResourceHelper::exportToJar(container, outPath.string()).is_ok());

    const auto in  = readEntries(S(inPath));
    const auto out = readEntries(S(outPath.string().c_str()));
    ASSERT_EQ(in.size(), out.size());
    for (size_t i = 0; i < in.size(); ++i) {
        EXPECT_EQ(in[i].first, out[i].first) << "entry index: " << i;
        EXPECT_EQ(in[i].second, out[i].second) << "entry: " << in[i].first;
    }

    std::remove(outPath.string().c_str());
}

// 输入 jar 带显式目录条目（夹具 roundtrip_withdirs.jar）：导入导出后
// 目录条目原样保留，无丢失也无新增。对比口径含目录条目——名字与内容字节
// （目录条目内容为空）逐条目全等。
TEST(MjJarWriterTest, containerRoundtripWithDirInputPreservesDirs)
{
    const char* inPath = "mjbase/test_resource/jar/roundtrip_withdirs.jar";

    mjbase::model::ResourceContainer container;
    ASSERT_TRUE(mjbase::model::ResourceHelper::importFromJar(container, inPath).is_ok());
    const auto outPath = test::getTempOutPath("mjbase_jar_roundtrip_withdirs.jar");
    ASSERT_TRUE(mjbase::model::ResourceHelper::exportToJar(container, outPath.string()).is_ok());

    const auto in  = readEntries(S(inPath));
    const auto out = readEntries(S(outPath.string().c_str()));
    ASSERT_EQ(in.size(), out.size());
    for (size_t i = 0; i < in.size(); ++i) {
        EXPECT_EQ(in[i].first, out[i].first) << "entry index: " << i;
        EXPECT_EQ(in[i].second, out[i].second) << "entry: " << in[i].first;
    }
    EXPECT_FALSE(in.empty());
    EXPECT_TRUE(std::any_of(in.begin(), in.end(), [](const auto& e) {
        return e.first == "com/";
    }));

    std::remove(outPath.string().c_str());
}

// ── 条目路径规范化 ──────────────────────────────────────────────

// 规范化折叠形状：'\'→'/'、连续 '/' 与 "." 段剔除、".." 按段折叠（含目录
// 条目形状，尾斜杠保留）。各形状归一为不同名字，读回条目集逐名对账。
TEST(MjJarWriterTest, writeEntryNameNormalizationCollapsesShapes)
{
    const char* tmpPath = "test/tmp_jar_normalize_shapes.jar";

    const std::vector<ca::u8> payload = {0xCA, 0xFE, 0xBA, 0xBE};
    JarWriter writer(S(tmpPath));
    auto writeResult = writer.write([&](IEntryWriter& ew) {
        ew.writeEntry(S("a\\b.class"), payload);        // → a/b.class
        ew.writeEntry(S("x//y.class"), payload);        // → x/y.class
        ew.writeEntry(S("./p.class"), payload);         // → p.class
        ew.writeEntry(S("q/./r.class"), payload);       // → q/r.class
        ew.writeEntry(S("s/../t.class"), payload);      // → t.class
        ew.writeEntry(S("d/./e/"), std::vector<ca::u8>{});   // → d/e/（目录）
        ew.writeEntry(S("u\\"), std::vector<ca::u8>{});      // → u/（目录）
    });
    ASSERT_TRUE(writeResult.is_ok());

    std::vector<std::string> names;
    JvmJarReader reader(S(tmpPath));
    auto readResult = reader.read([&](const ca::str::Utf8String& name, const std::vector<ca::u8>&) {
        names.push_back(name.to_std_string());
    });
    ASSERT_TRUE(readResult.is_ok());
    ASSERT_EQ(names.size(), 7u);
    // 归档序 = 原始名字排序（排序发生在规范化之前，与既有行为一致）：
    // "./p.class" < "a\\b.class" < "d/./e/" < "q/./r.class" < "s/../t.class"
    // < "u\\" < "x//y.class"。
    EXPECT_EQ(names[0], "p.class");
    EXPECT_EQ(names[1], "a/b.class");
    EXPECT_EQ(names[2], "d/e/");
    EXPECT_EQ(names[3], "q/r.class");
    EXPECT_EQ(names[4], "t.class");
    EXPECT_EQ(names[5], "u/");
    EXPECT_EQ(names[6], "x/y.class");

    std::remove(tmpPath);
}

// 非法条目名拒写：规范化后逃出根（"../a.class"、"a/../../b.class"）或为空
// （""、"."、".."、"./"、"/"、"a/.."）→ Err(BadEntryName)，err.path 为非法
// 条目名；首条非法即中止，输出文件不落盘。
TEST(MjJarWriterTest, writeRejectsNamesThatEscapeRootOrAreEmpty)
{
    const char* const badNames[] = {
        "", ".", "..", "./", "/", "../a.class", "a/../../b.class", "a/..",
    };
    for (const char* bad : badNames) {
        const char* tmpPath = "test/tmp_jar_badname_reject.jar";
        std::remove(tmpPath);
        JarWriter writer(S(tmpPath));
        auto writeResult = writer.write(
            [&](IEntryWriter& ew) { ew.writeEntry(S(bad), std::vector<ca::u8>{0xCA}); });
        ASSERT_TRUE(writeResult.is_err()) << "entry name: " << bad;
        const auto err = std::move(writeResult).unwrap_err();
        EXPECT_EQ(err.kind, JarError::Kind::BadEntryName) << "entry name: " << bad;
        EXPECT_EQ(err.path.to_std_string(), bad);
    }

    // 首条非法即中止：输出文件从未写出。
    JvmJarReader reader(S("test/tmp_jar_badname_reject.jar"));
    auto readResult = reader.read([&](const ca::str::Utf8String&, const std::vector<ca::u8>&) {});
    ASSERT_TRUE(readResult.is_err());
    EXPECT_EQ(std::move(readResult).unwrap_err().kind, JarError::Kind::FileNotFound);
}
