// mjbase jasm 服务单测。
//
// 移植自 mbase/unittest/jasm/jasm_service_test.cpp：15 用例逐一对应、断言强度
// 不变；命名空间随 mjbase（mjbase::jasm）、gtest 套件名加 Mj 前缀。底座差异：
// - 层次图改手工登记 ClassSummary（mjbase::hierarchy 不收容器输入；mbase 用
//   容器装配 4 个辅助类再建图，二者产出的类图等价）；
// - StackMapTable 断言改经 jbc::core::readClass 的 StackMapTableAttr 读取
//   （原 jbytecode tree FrameNode）；
// - ResourceJasmService 无 GlobalAnalysisManager：mbase
//   SuccessfulReplacementClearsGlobalAnalysisCache 的「替换后无陈旧分析态」
//   契约改断言为「同一容器连续替换仍成功 + 反汇编可见新字节」（见
//   resource_jasm_service.hpp 头注）。
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/jasm/resource_jasm_service.hpp"

#include "jbc/core/reader.hpp"
#include "libca/core/tag_cast.hpp"
#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mjbase::jasm {

namespace model = mjbase::model;

namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string classSource(const std::string& name, const std::string& instructions)
{
    return ".super java/lang/Object\n"
           ".class public super " +
           name +
           " {\n"
           "  .method public static value ()I {\n"
           "    code: {\n" +
           instructions +
           "    }\n"
           "  }\n"
           "}\n";
}

std::string emptyClassSource(const std::string& name, const std::string& superName)
{
    return ".super " + superName + "\n.class public super " + name + " {\n}\n";
}

std::string mergeSource()
{
    return ".super java/lang/Object\n"
           ".class public super example/MergeSubject {\n"
           "  .method public static choose (Z)Ljava/lang/Object; {\n"
           "    parameters: { flag },\n"
           "    code: {\n"
           "      iload flag\n"
           "      ifeq right\n"
           "      new example/Left\n"
           "      dup\n"
           "      invokespecial example/Left.<init> ()V\n"
           "      goto join\n"
           "    right:\n"
           "      new example/Right\n"
           "      dup\n"
           "      invokespecial example/Right.<init> ()V\n"
           "    join:\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::vector<ca::u8> assembleBytes(const std::string& name, const std::string& instructions)
{
    JvmJasmService service;
    auto           sourceName = utf16(name + ".jasm");
    auto           source     = utf16(classSource(name, instructions));
    auto           result     = service.assemble(sourceName.ref(), source.ref());
    if (!result.isOk()) throw std::runtime_error("test Jasm source did not assemble");
    return result.value->bytes;
}

model::ResourceContainer containerWithSubject()
{
    model::ResourceContainer container;
    auto                     bytes = assembleBytes("example/Subject", "      bipush 1\n      ireturn\n");
    container.addResource(
        "example/Subject.class",
        std::make_unique<model::ClassResource>("example/Subject.class", std::move(bytes)));
    return container;
}

std::string readFixture(const std::string& relativePath)
{
    std::ifstream input(relativePath, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read Jasm fixture: " + relativePath);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}   // namespace

TEST(MjJvmJasmServiceTest, AssemblesValidatesAndDisassemblesOwningClassData)
{
    JvmJasmService service;
    auto           sourceName = utf16("Subject.jasm");
    auto source = utf16(classSource("example/Subject", "      bipush 42\n      ireturn\n"));

    auto assembled = service.assemble(sourceName.ref(), source.ref());
    ASSERT_TRUE(assembled.isOk());
    ASSERT_TRUE(assembled.value.has_value());
    EXPECT_EQ("example/Subject", assembled.value->internalName.to_std_string());
    EXPECT_FALSE(assembled.value->bytes.empty());

    auto validated = service.validate(assembled.value->bytes);
    ASSERT_TRUE(validated.isOk());
    EXPECT_EQ("example/Subject", validated.value->internalName.to_std_string());
    EXPECT_EQ(assembled.value->bytes, validated.value->bytes);

    auto disassembled = service.disassemble(assembled.value->bytes);
    ASSERT_TRUE(disassembled.isOk());
    EXPECT_EQ("example/Subject", disassembled.value->internalName.to_std_string());
    EXPECT_NE(std::string::npos,
              disassembled.value->source.to_utf8_string().to_std_string().find(
                  ".class public super example/Subject"));
}

TEST(MjJvmJasmServiceTest, DisassemblesMethodByExactNameAndDescriptor)
{
    JvmJasmService service;
    auto           sourceName = utf16("MethodSubject.jasm");
    auto           source = utf16(".super java/lang/Object\n"
                                ".class public super example/MethodSubject {\n"
                                "  .method public static value ()I { code: { bipush 1\n ireturn } }\n"
                                "  .method public static value (I)I { code: { iload 0\n ireturn } }\n"
                                "}\n");
    auto assembled = service.assemble(sourceName.ref(), source.ref());
    ASSERT_TRUE(assembled.isOk());

    const auto name = ca::str::Utf8StringRef::from_cstr("value");
    const auto desc = ca::str::Utf8StringRef::from_cstr("(I)I");
    auto method = service.disassembleMethod(assembled.value->bytes, name, desc);

    ASSERT_TRUE(method.isOk());
    EXPECT_EQ("example/MethodSubject", method.value->internalName.to_std_string());
    EXPECT_EQ("value", method.value->name.to_std_string());
    EXPECT_EQ("(I)I", method.value->descriptor.to_std_string());
    const auto text = method.value->source.to_utf8_string().to_std_string();
    EXPECT_NE(std::string::npos, text.find(".method public static value (I)I"));
    EXPECT_EQ(std::string::npos, text.find("bipush 1"));
}

TEST(MjJvmJasmServiceTest, InvalidBytesReturnStableValidationDiagnostic)
{
    JvmJasmService service;
    auto           result = service.validate({0xCA, 0xFE});

    EXPECT_FALSE(result.isOk());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(DiagnosticSeverity::Error, result.diagnostics.front().severity);
    EXPECT_EQ(DiagnosticStage::Validate, result.diagnostics.front().stage);
    EXPECT_FALSE(result.diagnostics.front().message.is_empty());
}

TEST(MjJvmJasmServiceTest, PreservesNonAsciiInternalNameAcrossRoundTrip)
{
    JvmJasmService    service;
    const std::string internalName = "example/\xE7\xB1\xBB";
    auto              sourceName   = utf16("unicode-name.jasm");
    auto              source       = utf16(emptyClassSource(internalName, "java/lang/Object"));

    auto assembled = service.assemble(sourceName.ref(), source.ref());

    ASSERT_TRUE(assembled.isOk());
    EXPECT_EQ(internalName, assembled.value->internalName.to_std_string());
    auto disassembled = service.disassemble(assembled.value->bytes);
    ASSERT_TRUE(disassembled.isOk());
    EXPECT_EQ(internalName, disassembled.value->internalName.to_std_string());
}

TEST(MjJvmJasmServiceTest, SourceDiagnosticsOwnLocationAfterInputLifetimeEnds)
{
    JvmJasmService       service;
    Result<JvmJasmClass> result;
    {
        auto sourceName = utf16("broken.jasm");
        auto source     = utf16("\"");
        result          = service.assemble(sourceName.ref(), source.ref());
    }

    EXPECT_FALSE(result.isOk());
    ASSERT_FALSE(result.diagnostics.empty());
    ASSERT_TRUE(result.diagnostics.front().location.has_value());
    ASSERT_TRUE(result.diagnostics.front().location->source.has_value());
    EXPECT_EQ("broken.jasm",
              result.diagnostics.front().location->source->to_utf8_string().to_std_string());
}

// ── 层次图驱动 StackMapTable 的 common-super：任一 frame 的 stack 上出现
// Object 型 example/基类（jbc StackMapTableAttr 读取）──
TEST(MjJvmJasmServiceTest, ClassHierarchyDrivesEmittedCommonSuperFrame)
{
    JvmJasmService service;

    // 类图与 mbase 版容器建图等价：Object ← 基类 ← {Left, Right}。
    mjbase::KeyPool                   pool;
    mjbase::hierarchy::ClassHierarchy hierarchy(pool);
    const auto addClass = [&](const char* name, const char* super) {
        mjbase::hierarchy::ClassSummary s;
        s.self = mjbase::classKeyOf(pool, name);
        if (super != nullptr) s.super = mjbase::classKeyOf(pool, super);
        EXPECT_TRUE(hierarchy.addClass(s).is_ok()) << name;
    };
    addClass("java/lang/Object", nullptr);
    addClass("example/\xE5\x9F\xBA\xE7\xB1\xBB", "java/lang/Object");
    addClass("example/Left", "example/\xE5\x9F\xBA\xE7\xB1\xBB");
    addClass("example/Right", "example/\xE5\x9F\xBA\xE7\xB1\xBB");

    JvmJasmContext context;
    context.hierarchy = &hierarchy;
    auto sourceName   = utf16("MergeSubject.jasm");
    auto source       = utf16(mergeSource());
    auto assembled    = service.assemble(sourceName.ref(), source.ref(), context);
    ASSERT_TRUE(assembled.isOk());

    auto read = jbc::core::readClass(jbc::core::ByteView(assembled.value->bytes));
    ASSERT_TRUE(read.is_ok());
    auto  parsed          = std::move(read).unwrap();   // ReadResult 持 unique_ptr，只能移出
    auto& file            = *parsed.file;
    bool  foundBaseFrame  = false;
    const std::string baseName   = "example/\xE5\x9F\xBA\xE7\xB1\xBB";
    for (auto& method : file.methods) {
        auto code = file.decodeCode(method);
        if (code.is_err()) continue;
        for (auto& slot : code.unwrap()->attrs) {
            if (file.utf8(slot.nameCp) != "StackMapTable") continue;
            auto body = file.decodeAttr(slot);
            ASSERT_TRUE(body.is_ok());
            const auto* smt =
                ca::core::tag_cast::dyn_cast<const jbc::core::StackMapTableAttr>(body.unwrap());
            if (!smt) continue;
            for (const auto& frame : smt->frames) {
                for (const auto& value : frame.stack) {
                    if (value.tag != 7) continue;   // Object_variable_info
                    foundBaseFrame |= file.className(value.cp) == baseName;
                }
            }
        }
    }
    EXPECT_TRUE(foundBaseFrame);
}

TEST(MjJvmJasmServiceTest, RoundTripsAnnotationsInvokeDynamicAndComplexFrames)
{
    JvmJasmService service;
    for (const auto& [fixture, requiresExternalBootstrap] :
         {std::pair{"Example-class-with-method-annotation.jasm", false},
          std::pair{"Example-invokedynamic.jasm", true},
          std::pair{"Example-obfuscated-indy.jasm", true},
          std::pair{"Example-type-infer-list.jasm", true}}) {
        SCOPED_TRACE(fixture);
        const auto path   = std::string("mjbase/test_resource/jasm/") + fixture;
        auto       name   = utf16(fixture);
        auto       source = utf16(readFixture(path));
        auto       first  = service.assemble(name.ref(), source.ref());
        ASSERT_TRUE(first.isOk());

        if (!requiresExternalBootstrap) {
            auto validated = service.validate(first.value->bytes);
            ASSERT_TRUE(validated.isOk());
        }
        auto printed = service.disassemble(first.value->bytes);
        ASSERT_TRUE(printed.isOk());
        auto second = service.assemble(name.ref(), printed.value->source.ref());
        ASSERT_TRUE(second.isOk());
        EXPECT_EQ(first.value->internalName, second.value->internalName);
    }
}

TEST(MjJvmJasmServiceTest, RejectsIllegalFixtureDuringValidation)
{
    JvmJasmService service;
    const auto path = "mjbase/test_resource/jasm-illegal/Example-arraylen-int.jasm";
    auto       name = utf16("Example-arraylen-int.jasm");
    auto       source = utf16(readFixture(path));
    auto       assembled = service.assemble(name.ref(), source.ref());
    ASSERT_TRUE(assembled.isOk());

    auto validated = service.validate(assembled.value->bytes);
    EXPECT_FALSE(validated.isOk());
    ASSERT_FALSE(validated.diagnostics.empty());
    EXPECT_EQ(DiagnosticStage::Validate, validated.diagnostics.front().stage);
}

// ── 成功替换后无陈旧层次状态：同一容器连续替换仍成功、反汇编可见新字节。
// （mbase 版名 SuccessfulReplacementClearsGlobalAnalysisCache——其 GlobalAnalysisManager
// 缓存在 mjbase 无对应物，见文件头注。）──
TEST(MjResourceJasmServiceTest, SuccessfulReplacementKeepsHierarchyStateFresh)
{
    auto  container = containerWithSubject();
    auto* resource  = ca::core::tag_cast::cast<model::ClassResource>(
        container.getResource("example/Subject.class"));
    const auto        before = resource->get();
    ResourceJasmService service(container);
    auto                sourceName = utf16("Subject.jasm");
    auto source = utf16(classSource("example/Subject", "      bipush 2\n      ireturn\n"));

    auto result = service.assemble("example/Subject.class", sourceName.ref(), source.ref());

    ASSERT_TRUE(result.isOk());
    EXPECT_NE(before, resource->get());
    EXPECT_EQ(result.value->bytes, resource->get());

    auto second = utf16(classSource("example/Subject", "      bipush 42\n      ireturn\n"));
    auto replaced = service.assemble("example/Subject.class", sourceName.ref(), second.ref());
    ASSERT_TRUE(replaced.isOk()) << replaced.diagnostics.front().message.to_utf8_string().to_std_string();
    EXPECT_EQ(replaced.value->bytes, resource->get());
    // 替换后的资源可完整反汇编（服务基于当前容器状态工作，无陈旧分析态）。
    auto fresh = service.disassemble("example/Subject.class");
    ASSERT_TRUE(fresh.isOk());
    EXPECT_EQ("example/Subject", fresh.value->internalName.to_std_string());
}

TEST(MjResourceJasmServiceTest, DisassemblesClassResourceByUri)
{
    auto                container = containerWithSubject();
    ResourceJasmService service(container);

    auto result = service.disassemble("example/Subject.class");

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ("example/Subject", result.value->internalName.to_std_string());
    EXPECT_NE(std::string::npos,
              result.value->source.to_utf8_string().to_std_string().find(
                  ".class public super example/Subject"));
}

TEST(MjResourceJasmServiceTest, ReplacesClassWithNonAsciiInternalName)
{
    const std::string internalName = "example/\xE7\xB1\xBB";
    JvmJasmService    jasm;
    auto              initialSourceName = utf16("unicode-initial.jasm");
    auto initialSource = utf16(classSource(internalName, "      bipush 1\n      ireturn\n"));
    auto initial       = jasm.assemble(initialSourceName.ref(), initialSource.ref());
    ASSERT_TRUE(initial.isOk());

    model::ResourceContainer container;
    container.addResource(
        "example/unicode.class",
        std::make_unique<model::ClassResource>("example/unicode.class",
                                               std::move(initial.value->bytes)));
    ResourceJasmService      service(container);
    auto                     replacementSourceName = utf16("unicode-replacement.jasm");
    auto replacementSource = utf16(classSource(internalName, "      bipush 2\n      ireturn\n"));

    auto result = service.assemble(
        "example/unicode.class", replacementSourceName.ref(), replacementSource.ref());

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(internalName, result.value->internalName.to_std_string());
}

TEST(MjResourceJasmServiceTest, NameMismatchLeavesResourceUnchanged)
{
    auto  container = containerWithSubject();
    auto* resource  = ca::core::tag_cast::cast<model::ClassResource>(
        container.getResource("example/Subject.class"));
    const auto          before = resource->get();
    ResourceJasmService service(container);
    auto                sourceName = utf16("Other.jasm");
    auto source = utf16(classSource("example/Other", "      bipush 2\n      ireturn\n"));

    auto result = service.assemble("example/Subject.class", sourceName.ref(), source.ref());

    EXPECT_FALSE(result.isOk());
    EXPECT_EQ(before, resource->get());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(DiagnosticStage::Resource, result.diagnostics.front().stage);
}

TEST(MjResourceJasmServiceTest, ValidationFailureLeavesResourceUnchanged)
{
    auto  container = containerWithSubject();
    auto* resource  = ca::core::tag_cast::cast<model::ClassResource>(
        container.getResource("example/Subject.class"));
    const auto          before = resource->get();
    ResourceJasmService service(container);
    auto                sourceName = utf16("Subject.jasm");
    auto source = utf16(classSource("example/Subject", "      iconst_1\n      return\n"));

    auto result = service.assemble("example/Subject.class", sourceName.ref(), source.ref());

    EXPECT_FALSE(result.isOk());
    EXPECT_EQ(before, resource->get());
    ASSERT_FALSE(result.diagnostics.empty());
    bool hasValidationError = false;
    for (const auto& diagnostic : result.diagnostics) {
        hasValidationError |= diagnostic.severity == DiagnosticSeverity::Error &&
                              diagnostic.stage == DiagnosticStage::Validate;
    }
    EXPECT_TRUE(hasValidationError);
}

TEST(MjResourceJasmServiceTest, MissingResourceFailsWithoutCreatingEntry)
{
    model::ResourceContainer container;
    ResourceJasmService      service(container);
    auto                     sourceName = utf16("Missing.jasm");
    auto source = utf16(classSource("example/Missing", "      iconst_0\n      ireturn\n"));

    auto result = service.assemble("example/Missing.class", sourceName.ref(), source.ref());

    EXPECT_FALSE(result.isOk());
    EXPECT_FALSE(container.hasResource("example/Missing.class"));
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(DiagnosticStage::Resource, result.diagnostics.front().stage);
}

TEST(MjResourceJasmServiceTest, EditsOnlySelectedMultiReleaseClassEntry)
{
    model::ResourceContainer source;
    source.addResource("example/Multi.class",
                       std::make_unique<model::ClassResource>(
                           "example/Multi.class",
                           assembleBytes("example/Multi", "      bipush 1\n      ireturn\n")));
    source.addResource("META-INF/versions/9/example/Multi.class",
                       std::make_unique<model::ClassResource>(
                           "META-INF/versions/9/example/Multi.class",
                           assembleBytes("example/Multi", "      bipush 9\n      ireturn\n")));
    source.addResource("META-INF/MANIFEST.MF",
                       std::make_unique<model::ManifestResource>(model::ManifestResource::fromBytes(
                           {'M', 'a', 'n', 'i', 'f', 'e', 's', 't', '-', 'V', 'e', 'r', 's', 'i', 'o', 'n',
                            ':', ' ', '1', '.', '0', '\r', '\n', 'M', 'u', 'l', 't', 'i', '-', 'R', 'e', 'l',
                            'e', 'a', 's', 'e', ':', ' ', 't', 'r', 'u', 'e', '\r', '\n', '\r', '\n'})));

    const auto input = test::getTempOutPath("mjbase_jasm_multi_release_input.jar");
    ASSERT_TRUE(model::ResourceHelper::exportToJar(source, input.string()).is_ok());
    model::ResourceContainer container;
    ASSERT_TRUE(model::ResourceHelper::importFromJar(container, input.string()).is_ok());
    const auto baseBefore = ca::core::tag_cast::cast<model::ClassResource>(
                                container.getResource("example/Multi.class"))
                                ->get();

    ResourceJasmService service(container);
    auto                sourceName  = utf16("Multi.jasm");
    auto                replacement = utf16(classSource("example/Multi", "      bipush 42\n      ireturn\n"));
    auto result = service.assemble("META-INF/versions/9/example/Multi.class",
                                   sourceName.ref(), replacement.ref());

    ASSERT_TRUE(result.isOk());
    EXPECT_EQ(baseBefore,
              ca::core::tag_cast::cast<model::ClassResource>(
                  container.getResource("example/Multi.class"))
                  ->get());
    EXPECT_NE(baseBefore,
              ca::core::tag_cast::cast<model::ClassResource>(
                  container.getResource("META-INF/versions/9/example/Multi.class"))
                  ->get());
}

}   // namespace mjbase::jasm
