/// @file        resource_io_test.cpp
/// @brief       资源导入导出测试（移植自旧栈 unittest/model/resource_io_test.cpp，
///              12 用例；末尾 3 个为本批新增的 Result 错误面用例）。
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_io.hpp"
#include "mjbase/model/resource_model.hpp"

using namespace mjbase::model;

namespace {

/// 内存版 IJarReader：按预设条目列表喂给 consumer，避免依赖磁盘 jar。
/// 条目名以 std::string 预设（字面量友好），消费时按原始字节转 Utf8String
/// （与 mjbase::jar 的零解码口径一致）。
class FakeJarReader : public mjbase::jar::IJarReader
{
public:
    using Entry = std::pair<std::string, std::vector<ca::u8>>;

    explicit FakeJarReader(std::vector<Entry> entries)
        : entries_(std::move(entries))
    {}

    ca::core::Result<void, mjbase::jar::JarError> read(
        const std::function<void(const ca::str::Utf8String&, const std::vector<ca::u8>&)>& consumer)
        override
    {
        for (const auto& [name, data] : entries_) {
            consumer(ca::str::Utf8String::from_data_unchecked(
                         reinterpret_cast<const ca::u8*>(name.data()), name.size()),
                     data);
        }
        return ca::core::Ok();
    }

private:
    std::vector<Entry> entries_;
};

/// CAFEBABE 魔数前缀，满足 JarUtil::isClassFile(bytes) 判定。
std::vector<ca::u8> classMagicBytes()
{
    return {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34};
}

}   // namespace

// ── classifyAndAdd 经 JarFileImporter 的分拣行为 ───────────────

// 普通 .class entry：识别为 ClassResource，URI/location 保留原样。
TEST(MjModelIoTest, PlainClassEntryBecomesClassResource)
{
    FakeJarReader     reader({{"Bar.class", classMagicBytes()}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 1u);
    EXPECT_TRUE(container.hasResource("Bar.class"));
    auto* res = container.getResource("Bar.class");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Class);
    EXPECT_EQ(res->getLocation(), "Bar.class");
}

// OS2.9.jar 混淆产物：类 entry 名为 *.class/（尾斜杠）。
// 必须识别为 ClassResource，且 URI/location 归一化为 *.class（剥掉尾斜杠）。
TEST(MjModelIoTest, TrailingSlashClassEntryNormalizedToClassResource)
{
    FakeJarReader     reader({{"com/example/Foo.class/", classMagicBytes()}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    // 归一化后容器以无尾斜杠的 URI 索引，原始 *.class/ 不应存在。
    EXPECT_FALSE(container.hasResource("com/example/Foo.class/"));
    EXPECT_TRUE(container.hasResource("com/example/Foo.class"));
    auto* res = container.getResource("com/example/Foo.class");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Class);
    EXPECT_EQ(res->getLocation(), "com/example/Foo.class");
}

// *.class/ 但内容魔数不对：名字通过但内容不通过，落入 UnknownResource（保持原始 entry 名）。
TEST(MjModelIoTest, TrailingSlashWithBadMagicBecomesUnknown)
{
    FakeJarReader     reader({{"Bad.class/", std::vector<ca::u8>{0x50, 0x4B, 0x03, 0x04}}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 1u);
    auto* res = container.getResource("Bad.class/");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Unknown);
}

// 非 class 条目：仍作为 UnknownResource，entry 名不变。
TEST(MjModelIoTest, NonClassEntryBecomesUnknownResource)
{
    FakeJarReader     reader({{"Baz.txt", std::vector<ca::u8>{0x41, 0x42, 0x43}}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 1u);
    auto* res = container.getResource("Baz.txt");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Unknown);
    EXPECT_EQ(res->getLocation(), "Baz.txt");
}

// 混合条目：*.class/ 归一化、*.class 正常、未知资源各就各位，互不干扰。
TEST(MjModelIoTest, MixedEntriesClassifiedAndNormalized)
{
    FakeJarReader reader({
        {"a/A.class/", classMagicBytes()},
        {"b/B.class", classMagicBytes()},
        {"c/c.txt", std::vector<ca::u8>{0x01}},
    });
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 3u);
    EXPECT_TRUE(container.hasResource("a/A.class"));
    EXPECT_FALSE(container.hasResource("a/A.class/"));
    EXPECT_TRUE(container.hasResource("b/B.class"));
    EXPECT_TRUE(container.hasResource("c/c.txt"));
}

// ── Recaf 98dd1f528 InfoImporterTest 两用例移植 ─────────────────
//
// 上游语义：头部形似 class 但 CP 首个 tag 越界（121）的伪 class 内容按普通
// 文件处理；stub 前缀 JAR 整体按 ZIP 容器打开、条目全部可加载。旧栈 的
// 对应行为：条目级 isClassFile 双检（名字+内容）失败 → UnknownResource 保留
// （不丢弃、不产出损坏 ClassResource），同容器的真实 class 条目照常加载。

namespace {

/// 构造"伪 class + 尾随 ZIP 数据"的 stub 前缀 blob：
/// CAFEBABE 头 + cpCount=2 + 越界 tag 121 + "PK\x03\x04" 容器字节。
std::vector<ca::u8> stubPrefixedJarBlob()
{
    return {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34,   // magic + version
            0x00, 0x02,                                       // cpCount = 2
            121,                                              // 越界 CP tag
            0x00, 0x01,                                       // 载荷 u2
            0x50, 0x4B, 0x03, 0x04};                          // "PK\x03\x04"
}

}   // namespace

// 伪 class 条目按普通文件保留为 UnknownResource：容器打开不因坏 class 丢条目
// （对应上游 "corrupted-PP-tag content read as a file"）。
TEST(MjModelIoTest, PseudoClassEntryWithCorruptedTagBecomesUnknown)
{
    FakeJarReader     reader({{"Stub.class", stubPrefixedJarBlob()}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 1u);
    auto* res = container.getResource("Stub.class");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Unknown);
}

// stub 前缀 JAR 作为容器打开：伪 class 条目与真实 class 条目共存，全部可加载
// （对应上游 "stub-prefixed JAR read as a ZIP container"——修复前条目全都不
// 能加载，此处断言两类条目各就各位）。
TEST(MjModelIoTest, StubPrefixedJarEntriesAllLoad)
{
    FakeJarReader reader({{"Stub.class", stubPrefixedJarBlob()},
                          {"Real.class", classMagicBytes()}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    ASSERT_EQ(container.size(), 2u);
    auto* stub = container.getResource("Stub.class");
    ASSERT_NE(stub, nullptr);
    EXPECT_EQ(stub->getType(), ResourceType::Unknown);
    auto* real = container.getResource("Real.class");
    ASSERT_NE(real, nullptr);
    EXPECT_EQ(real->getType(), ResourceType::Class);
}

// ── os29/Jbytec 样本实测形态的 fail-closed 回归 ────────────────
// （判据与数字溯源见私有侧 survey 笔记）

// 目录形态 entry 藏 class 内容（OS2.9 中央目录视角：59 个 class 内容挂在
// 'yaakov/jni/' 目录名下）：名字关拒绝 → UnknownResource 保留原始名，
// 不得因内容是 CAFEBABE 就收编为 class（fail-closed，与 Recaf 内容派行为划界）。
TEST(MjModelIoTest, DirNamedEntryWithClassContentStaysResource)
{
    FakeJarReader     reader({{"yaakov/jni/", classMagicBytes()}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 1u);
    auto* res = container.getResource("yaakov/jni/");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Unknown);
    EXPECT_EQ(res->getLocation(), "yaakov/jni/");
}

// 空 entry 名 + class 内容（OS2.9 中央目录里 NUL 前缀 polyglot 条目的截断形态）：
// 同样按资源通道保留。
TEST(MjModelIoTest, EmptyNameEntryWithClassContentStaysResource)
{
    FakeJarReader     reader({{"", classMagicBytes()}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 1u);
    auto* res = container.getResource("");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Unknown);
}

// 双后缀（X.class.class）：文本尾缀命中 + 内容魔数命中 → 按 class 走，
// 容器 key/location 保留完整 entry 名（不做额外剥离）。
TEST(MjModelIoTest, DoubleSuffixClassEntryAdmitted)
{
    FakeJarReader     reader({{"X.class.class", classMagicBytes()}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 1u);
    auto* res = container.getResource("X.class.class");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Class);
    EXPECT_EQ(res->getLocation(), "X.class.class");
}

// class 内容挂在非 class 名下（OS2.9 里 'config.xml' 等资源名形态的名字反用）：
// 名字关拒绝，按资源通道走（内容关不是收编通道）。
TEST(MjModelIoTest, ClassContentUnderNonClassNameStaysResource)
{
    FakeJarReader     reader({{"config.xml", classMagicBytes()}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 1u);
    auto* res = container.getResource("config.xml");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Unknown);
}

// MANIFEST 名独占资源通道：即使内容是 CAFEBABE（伪造），也走 Manifest 通道，
// 不进入 class 通道（Manifest::fromBytes 宽松解析，不抛异常）。
TEST(MjModelIoTest, ManifestNameRoutesThroughManifestChannel)
{
    FakeJarReader     reader({{"META-INF/MANIFEST.MF", classMagicBytes()}});
    JarFileImporter   importer(reader);
    ResourceContainer container;

    ASSERT_TRUE(importer.importResource(container).is_ok());

    EXPECT_EQ(container.size(), 1u);
    auto* res = container.getResource("META-INF/MANIFEST.MF");
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res->getType(), ResourceType::Manifest);
}

// ── Result 错误面（本批新增；）──
// 全部不触磁盘：错误分支在打开文件前返回。

// 空 URI 导出 class：容器非「恰好一个 ClassResource」时拒绝。
TEST(MjModelIoTest, ExportClassFileWithoutUriRequiresSingleClassResource)
{
    ResourceContainer container;
    SimpleClassFileExporter exporter("", "unused.bin");

    // 空容器：size != 1。
    auto r1 = exporter.exportResource(container);
    ASSERT_TRUE(r1.is_err());
    EXPECT_EQ(r1.unwrap_err(), ModelError::BadArgument);

    // 唯一资源不是 ClassResource。
    container.addResource("x.txt",
                          std::make_unique<UnknownResource>("x.txt", std::vector<ca::u8>{1}));
    auto r2 = exporter.exportResource(container);
    ASSERT_TRUE(r2.is_err());
    EXPECT_EQ(r2.unwrap_err(), ModelError::TypeMismatch);
}

// 指定 URI 导出 class：URI 不存在 / 非 class 资源分别报 NotFound / TypeMismatch。
TEST(MjModelIoTest, ExportClassFileByUriRejectsMissingAndNonClass)
{
    ResourceContainer container;
    container.addResource("a.txt",
                          std::make_unique<UnknownResource>("a.txt", std::vector<ca::u8>{1}));

    SimpleClassFileExporter missing("nope.class", "unused.bin");
    auto r1 = missing.exportResource(container);
    ASSERT_TRUE(r1.is_err());
    EXPECT_EQ(r1.unwrap_err(), ModelError::NotFound);   // 旧栈: "uri not found in container"

    SimpleClassFileExporter wrongType("a.txt", "unused.bin");
    auto r2 = wrongType.exportResource(container);
    ASSERT_TRUE(r2.is_err());
    EXPECT_EQ(r2.unwrap_err(), ModelError::TypeMismatch);   // 旧栈: "resource is not ClassResource"
}

// 从磁盘导入不存在的 class 文件：Err(Io)，容器保持为空。
TEST(MjModelIoTest, ImportClassFileFromMissingPathIsIoError)
{
    ResourceContainer container;
    SimpleClassFileImporter importer("definitely/missing/input.class");
    auto r = importer.importResource(container);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), ModelError::Io);
    EXPECT_EQ(container.size(), 0);
}
