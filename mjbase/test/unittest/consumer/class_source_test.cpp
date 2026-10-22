/// @file        class_source_test.cpp
/// @brief       E3 类源与骨架扫描单测：IClassSource 双实现 + SummaryScanner。
///
/// 测试类字节经 reference 测试共享的手拼汇编器构造（每字节受控，不依赖
/// 本地语料）；对拍口径与 golden 对拍测试（summary_parity_test）互补：
/// 本文件钉 API 行为（fail-closed/枚举序/first-wins/hasCode 分流），对拍
/// 钉字段级一致。
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/consumer/class_source.hpp"
#include "mjbase/consumer/summary_scanner.hpp"
#include "shared/class_bytes_builder.hpp"

namespace {

using Bytes = std::vector<ca::u8>;

using mjbase::consumer::ClassSummary;
using mjbase::consumer::ContainerClassSource;
using mjbase::consumer::ErrorCode;
using mjbase::consumer::MemoryClassSource;
using mjbase::consumer::SummaryScanner;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::FieldSpec;
using mjbase::reference::test_support::MethodSpec;

/// 组一个最小类：2 方法（一个带体、一个 abstract）+ 1 字段 + 1 接口。
Bytes makeSampleClass(std::string_view name)
{
    ClassBytesBuilder b(name);
    b.setInterfaces({"com/i/Shape"});
    b.addField(FieldSpec{0x0002, "value", "I"});
    std::vector<MethodSpec> methods;
    methods.push_back(MethodSpec{0x0009, "run", "()V", "", 1, 1,
                                 [](mjbase::reference::test_support::CodeEmit& e) {
                                     e.op(jbc::core::Op::RETURN);
                                 }});
    methods.push_back(MethodSpec{0x0411, "shape", "()I"});   // public abstract：无 Code
    return b.assemble(methods);
}

/// E3-1：MemoryClassSource upsert / fail-closed NotFound / 字典序枚举。
TEST(MjConsumerClassSourceTest, MemorySourceUpsertNotFoundAndOrder)
{
    MemoryClassSource source;
    source.add("com/b/Beta", makeSampleClass("com/b/Beta"));
    source.add("com/a/Alpha", makeSampleClass("com/a/Alpha"));

    ASSERT_EQ(source.classCount(), 2u);

    std::vector<std::string_view> names;
    source.listClasses(names);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "com/a/Alpha");
    EXPECT_EQ(names[1], "com/b/Beta");

    auto bytes = source.bytes("com/b/Beta");
    ASSERT_TRUE(bytes.is_ok());
    EXPECT_GE(bytes.unwrap().size(), 8u);   // CAFEBABE + 版本号

    auto missing = source.bytes("com/c/Gamma");
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(missing.unwrap_err().code, ErrorCode::NotFound);
    EXPECT_NE(missing.unwrap_err().message.find("com/c/Gamma"), std::string::npos);

    // upsert：同名覆盖不新增条目。
    source.add("com/a/Alpha", makeSampleClass("com/a/Alpha"));
    EXPECT_EQ(source.classCount(), 2u);
}

/// E3-2：ContainerClassSource 构建——非 Class 资源忽略、URI≠内部名、
/// 重复内部名 first-wins 并记录、字典序枚举。
TEST(MjConsumerClassSourceTest, ContainerSourceProjectsClassResources)
{
    mjbase::model::ResourceContainer container;
    container.addResource("lib/Beta.class", std::make_unique<mjbase::model::ClassResource>(
                                                "lib/Beta.class", makeSampleClass("com/b/Beta")));
    container.addResource(
        "META-INF/MANIFEST.MF",
        std::make_unique<mjbase::model::ManifestResource>(mjbase::jar::Manifest::fromBytes({})));
    container.addResource("assets/logo.png",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "assets/logo.png", Bytes{1, 2}));
    container.addResource("lib/Alpha.class", std::make_unique<mjbase::model::ClassResource>(
                                                 "lib/Alpha.class", makeSampleClass("com/a/Alpha")));
    // 内部名重复（URI 不同）：first-wins 保留首条。
    container.addResource("dup/Beta2.class", std::make_unique<mjbase::model::ClassResource>(
                                                 "dup/Beta2.class", makeSampleClass("com/b/Beta")));

    auto made = ContainerClassSource::make(container);
    ASSERT_TRUE(made.is_ok());

    ContainerClassSource source = std::move(made).unwrap();
    EXPECT_EQ(source.classCount(), 2u);   // manifest/unknown/重复条目都不计

    std::vector<std::string_view> names;
    source.listClasses(names);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "com/a/Alpha");
    EXPECT_EQ(names[1], "com/b/Beta");

    ASSERT_EQ(source.duplicateNames().size(), 1u);
    EXPECT_EQ(source.duplicateNames()[0], "com/b/Beta");

    // 字节来自首个 URI 的资源（内部名→资源映射正确）。
    const Bytes firstBytes = makeSampleClass("com/b/Beta");
    auto        bytes      = source.bytes("com/b/Beta");
    ASSERT_TRUE(bytes.is_ok());
    const jbc::core::ByteView view = bytes.unwrap();
    ASSERT_EQ(view.size(), firstBytes.size());
    EXPECT_EQ(0, std::memcmp(view.data(), firstBytes.data(), firstBytes.size()));

    auto missing = source.bytes("no/such/Name");
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(missing.unwrap_err().code, ErrorCode::NotFound);
}

/// E3-3：ContainerClassSource fail-closed——畸形 class 资源使 make 失败。
TEST(MjConsumerClassSourceTest, ContainerSourceRejectsMalformedResource)
{
    mjbase::model::ResourceContainer container;
    container.addResource("good/A.class", std::make_unique<mjbase::model::ClassResource>(
                                              "good/A.class", makeSampleClass("good/A")));
    container.addResource("bad/B.class", std::make_unique<mjbase::model::ClassResource>(
                                             "bad/B.class", Bytes{0xCA, 0xFE, 0xBA, 0xBE, 0x00}));

    auto made = ContainerClassSource::make(container);
    ASSERT_TRUE(made.is_err());
    EXPECT_EQ(made.unwrap_err().code, ErrorCode::BadClassFile);
    EXPECT_NE(made.unwrap_err().message.find("bad/B.class"), std::string::npos);
}

/// E3-4：SummaryScanner——骨架字段、hasCode 分流、无父类映射、成员序 = 声明序。
TEST(MjConsumerSummaryScannerTest, ScanSkeletonFieldsAndHasCode)
{
    const Bytes               bytes   = makeSampleClass("com/x/Sample");
    mjbase::KeyPool           pool;
    const SummaryScanner      scanner;
    auto                      scanned = scanner.scan(jbc::core::ByteView{bytes}, pool);
    ASSERT_TRUE(scanned.is_ok());

    const ClassSummary& s = scanned.unwrap();
    EXPECT_EQ(pool.str(s.self.id), "com/x/Sample");
    EXPECT_EQ(pool.str(s.super.id), "java/lang/Object");
    ASSERT_EQ(s.interfaces.size(), 1u);
    EXPECT_EQ(pool.str(s.interfaces[0].id), "com/i/Shape");

    ASSERT_EQ(s.methods.size(), 2u);
    EXPECT_EQ(pool.str(s.methods[0].name), "run");
    EXPECT_EQ(pool.str(s.methods[0].desc), "()V");
    EXPECT_TRUE(s.methods[0].hasCode);
    EXPECT_EQ(pool.str(s.methods[1].name), "shape");
    EXPECT_FALSE(s.methods[1].hasCode);   // abstract：无 Code 属性

    ASSERT_EQ(s.fields.size(), 1u);
    EXPECT_EQ(pool.str(s.fields[0].name), "value");
    EXPECT_EQ(pool.str(s.fields[0].desc), "I");
}

/// E3-5：Object 自身无 super 条目 → invalid 键；version 编码 (minor<<16)|major。
TEST(MjConsumerSummaryScannerTest, ObjectClassHasNoSuperAndVersionEncoding)
{
    ClassBytesBuilder       builder("java/lang/Object");
    builder.setNoSuper();   // 无 super 条目（super_index = 0）
    std::vector<MethodSpec> methods;
    methods.push_back(MethodSpec{0x0009, "registerNatives", "()V"});   // static native：无体
    const Bytes bytes = builder.assemble(methods);

    mjbase::KeyPool      pool;
    const SummaryScanner scanner;
    auto                 scanned = scanner.scan(jbc::core::ByteView{bytes}, pool);
    ASSERT_TRUE(scanned.is_ok());

    const ClassSummary& s = scanned.unwrap();
    EXPECT_FALSE(s.super.valid());
    EXPECT_TRUE(s.interfaces.empty());
    ASSERT_EQ(s.methods.size(), 1u);
    EXPECT_FALSE(s.methods[0].hasCode);
    // 手拼汇编器固定 V1_8（major=52, minor=0）；编码与 mbase 口径一致。
    EXPECT_EQ(s.version, 52u);
    EXPECT_EQ(s.version, (0u << 16) | 52u);
}

/// E3-6：畸形字节 fail-closed——Err(BadClassFile)，不产出半截摘要。
TEST(MjConsumerSummaryScannerTest, MalformedBytesFailClosed)
{
    mjbase::KeyPool           pool;
    const SummaryScanner      scanner;
    const Bytes               garbage{0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01};
    auto                      scanned = scanner.scan(jbc::core::ByteView{garbage}, pool);
    ASSERT_TRUE(scanned.is_err());
    EXPECT_EQ(scanned.unwrap_err().code, ErrorCode::BadClassFile);
}

/// E3-7：scanSource——多类批量成功；单类损坏整批 Err（fail-closed，不返回部分）。
TEST(MjConsumerSummaryScannerTest, ScanSourceFailClosedOnFirstBadClass)
{
    MemoryClassSource source;
    source.add("com/a/Alpha", makeSampleClass("com/a/Alpha"));
    source.add("com/b/Beta", makeSampleClass("com/b/Beta"));

    mjbase::KeyPool      pool;
    const SummaryScanner scanner;
    auto                 all = scanner.scanSource(source, pool);
    ASSERT_TRUE(all.is_ok());
    ASSERT_EQ(all.unwrap().size(), 2u);

    source.add("com/c/Cursor", Bytes{0xCA, 0xFE, 0xBA, 0xBE, 0x00});
    auto failed = scanner.scanSource(source, pool);
    ASSERT_TRUE(failed.is_err());
    EXPECT_EQ(failed.unwrap_err().code, ErrorCode::BadClassFile);
    EXPECT_NE(failed.unwrap_err().message.find("com/c/Cursor"), std::string::npos);
}

/// E3-8：scanSource 消费 ContainerClassSource——容器投影与扫描面打通。
TEST(MjConsumerSummaryScannerTest, ScanSourceOverContainerSource)
{
    mjbase::model::ResourceContainer container;
    container.addResource("lib/A.class", std::make_unique<mjbase::model::ClassResource>(
                                             "lib/A.class", makeSampleClass("com/a/Alpha")));

    auto made = ContainerClassSource::make(container);
    ASSERT_TRUE(made.is_ok());

    mjbase::KeyPool       pool;
    const SummaryScanner  scanner;
    auto                  summaries = scanner.scanSource(std::move(made).unwrap(), pool);
    ASSERT_TRUE(summaries.is_ok());
    ASSERT_EQ(summaries.unwrap().size(), 1u);
    EXPECT_EQ(pool.str(summaries.unwrap()[0].self.id), "com/a/Alpha");
}

}   // namespace
