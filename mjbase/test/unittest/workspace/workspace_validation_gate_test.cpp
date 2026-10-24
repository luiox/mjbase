// 编辑校验门三档模式 + 作用域收窄验收面：
// - 协变捷径 fixture（putstatic HashMap 入 Map 字段 / String 以 CharSequence
//   接口调用 / String[] 作 Object[] 返回）在 relaxed（默认）门下全部通过，
//   在 strict（原精确匹配）门下如约报错——JVM 验证器接受的字节码不再卡死
//   编辑账本；
// - 门禁作用域 = 操作受影响类：容器基线未编辑类不参与门禁（strict 模式下
//   编辑 A 类也不再被基线 B 类的既存形态拒绝）；
// - off 档跳过门禁；失败诊断按类聚合（per-class 单条目 + 截断明细）。
// 底层语义（mjbase jasm relaxed verifier）另见 jasm_service_test 的 relaxed
// 直检用例；本文件走 WorkspaceBase 公开编辑事务面。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/workspace/workspace.hpp"

namespace mjbase::workspace {
namespace model = mjbase::model;

namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// 协变捷径类：三条 ZKM 风格"协变捷径"字节码（javac 不生成、JVM 赋值协变
/// 验证合法、JDK21 实测可运行），外加一条普通可编辑类共用的工作台方法。
std::string covariantSource()
{
    return ".super java/lang/Object\n"
           ".class public super example/Covariant {\n"
           "  .field public static cache Ljava/util/Map;\n"
           "  .method public static setup ()V {\n"
           "    code: {\n"
           "      new java/util/HashMap\n"
           "      dup\n"
           "      invokespecial java/util/HashMap.<init> ()V\n"
           "      putstatic example/Covariant.cache Ljava/util/Map;\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "  .method public static measure (Ljava/lang/String;)I {\n"
           "    parameters: { text },\n"
           "    code: {\n"
           "      aload text\n"
           "      invokeinterface java/lang/CharSequence.length ()I\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "  .method public static view ()[Ljava/lang/Object; {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      anewarray java/lang/String\n"
           "      dup\n"
           "      iconst_0\n"
           "      ldc \"a\"\n"
           "      aastore\n"
           "      astore names\n"
           "      aload names\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string subjectSource(const std::string& value)
{
    return ".super java/lang/Object\n"
           ".class public super workspace/Subject {\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      bipush " +
           value +
           "\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& source)
{
    jasm::JvmJasmService service;
    auto                 result = service.assemble(utf16(name + ".jasm").ref(),
                                                   utf16(source).ref());
    if (!result.isOk()) throw std::runtime_error("gate fixture did not assemble");
    return result.value->bytes;
}

/// 双类 JAR 夹具：example/Covariant（基线协变捷径，编辑中从不触碰）+
/// workspace/Subject（受编辑类）。
std::filesystem::path writeGateFixture(const std::string& fileName)
{
    model::ResourceContainer container;
    container.addResource("example/Covariant.class",
                          std::make_unique<model::ClassResource>(
                              "example/Covariant.class", assemble("example/Covariant",
                                                                  covariantSource())));
    container.addResource("workspace/Subject.class",
                          std::make_unique<model::ClassResource>(
                              "workspace/Subject.class",
                              assemble("workspace/Subject", subjectSource("1"))));
    const auto path = test::getTempOutPath(fileName);
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

MutationRequest request(ca::u64 revision, jasm::ValidationMode mode)
{
    MutationRequest options;
    options.expectedRevision = revision;
    options.validationMode   = mode;
    return options;
}

std::vector<ca::u8> subjectBytes(const WorkspaceBase& workspace)
{
    auto content = workspace.readClassBytes("workspace/Subject");
    EXPECT_TRUE(content.is_ok());
    return content.is_ok() ? std::move(content).unwrap().bytes : std::vector<ca::u8>{};
}

std::vector<ca::u8> covariantBytes(const WorkspaceBase& workspace)
{
    auto content = workspace.readClassBytes("example/Covariant");
    EXPECT_TRUE(content.is_ok());
    return content.is_ok() ? std::move(content).unwrap().bytes : std::vector<ca::u8>{};
}

}   // namespace

TEST(MjWorkspaceValidationGateTest, relaxedGatePassesCovariantShortcutsAndEditsFlow)
{
    WorkspaceBase workspace;
    const auto    input = writeGateFixture("mjbase_workspace_gate_covariant.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    const auto  originalBytes = subjectBytes(workspace);

    // 编辑 → undo → revert → replace bytes 全链路不再被基线协变类卡死
    // （阻断场景验收；默认 relaxed，无需传参）。
    ASSERT_TRUE(workspace.applyClassBytes(
        "workspace/Subject.class", assemble("workspace/Subject", subjectSource("42")),
        request(0, jasm::ValidationMode::Relaxed)).status.is_ok());
    const auto  editedBytes = subjectBytes(workspace);
    EXPECT_NE(originalBytes, editedBytes);

    auto undone = workspace.undo(request(1, jasm::ValidationMode::Relaxed));
    ASSERT_TRUE(undone.status.is_ok());
    EXPECT_EQ(originalBytes, subjectBytes(workspace));

    auto redone = workspace.redo(request(2, jasm::ValidationMode::Relaxed));
    ASSERT_TRUE(redone.status.is_ok());
    EXPECT_EQ(editedBytes, subjectBytes(workspace));

    auto reverted = workspace.revertClass("workspace/Subject.class",
                                          request(3, jasm::ValidationMode::Relaxed));
    ASSERT_TRUE(reverted.status.is_ok());
    EXPECT_EQ(originalBytes, subjectBytes(workspace));
}

TEST(MjWorkspaceValidationGateTest, strictGateRejectsCovariantShortcuts)
{
    jasm::JvmJasmService service;
    jasm::JvmJasmContext strict;
    strict.validationMode = jasm::ValidationMode::Strict;
    auto covariant        = assemble("example/Covariant", covariantSource());

    // strict（原行为）：三条协变捷径各自报错——作为 relaxed 修复的对照面。
    auto validated = service.validate(covariant, strict);
    EXPECT_FALSE(validated.isOk());
    ca::usize errorCount = 0;
    for (const auto& diagnostic : validated.diagnostics) {
        if (diagnostic.severity == jasm::DiagnosticSeverity::Error) ++errorCount;
    }
    EXPECT_GE(errorCount, 3U);

    // relaxed（默认）：同一字节全部通过。
    jasm::JvmJasmContext relaxed;   // 默认 Relaxed
    auto                 accepted = service.validate(covariant, relaxed);
    EXPECT_TRUE(accepted.isOk());
    ca::usize relaxedErrors = 0;
    for (const auto& diagnostic : accepted.diagnostics) {
        if (diagnostic.severity == jasm::DiagnosticSeverity::Error) ++relaxedErrors;
    }
    EXPECT_EQ(0U, relaxedErrors);
}

TEST(MjWorkspaceValidationGateTest, gateScopeCoversOnlyAffectedClasses)
{
    WorkspaceBase workspace;
    const auto    input = writeGateFixture("mjbase_workspace_gate_scope.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    const auto covariant = covariantBytes(workspace);

    // strict 门 + 作用域收窄：编辑 Subject 时基线 Covariant 类不参与
    // 门禁——其既存形态不再拒绝无关编辑。
    auto edited = workspace.applyClassBytes(
        "workspace/Subject.class", assemble("workspace/Subject", subjectSource("7")),
        request(0, jasm::ValidationMode::Strict));
    ASSERT_TRUE(edited.status.is_ok());

    // 受影响类本身带协变捷径时，strict 门如约拒绝（作用域内的真实严格语义；
    // 拒绝不消耗 revision，后续请求沿用当前位）。
    auto strictSelfEdit = workspace.replaceClassBytes("example/Covariant", covariant,
                                                      request(1, jasm::ValidationMode::Strict));
    EXPECT_FALSE(strictSelfEdit.status.is_ok());
    // 失败诊断按类聚合：per-class 单条目（类名 + 计数 + 明细），不再
    // 逐方法刷屏。
    ASSERT_EQ(1U, strictSelfEdit.diagnostics.size());
    const auto summary = strictSelfEdit.diagnostics[0].message.to_utf8_string().to_std_string();
    EXPECT_NE(std::string::npos, summary.find("example/Covariant"));
    EXPECT_NE(std::string::npos, summary.find("error(s)"));
    EXPECT_NE(std::string::npos, summary.find("\n  Method "));
    // 编辑门诊断归因 EditGate 阶段（与重建管线的 Validate 阶段可区分）。
    EXPECT_EQ(jasm::DiagnosticStage::EditGate, strictSelfEdit.diagnostics[0].stage);

    // 同一替换在 relaxed（默认）门下通过——JVM 验证器会接受。
    auto relaxedSelfEdit = workspace.replaceClassBytes(
        "example/Covariant", covariant, request(kAnyRevision, jasm::ValidationMode::Relaxed));
    ASSERT_TRUE(relaxedSelfEdit.status.is_ok());
}

TEST(MjWorkspaceValidationGateTest, offModeSkipsGateEntirely)
{
    WorkspaceBase workspace;
    const auto    input = writeGateFixture("mjbase_workspace_gate_off.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());

    // off 档：受影响类带 strict 违规也直通。
    auto skipped = workspace.replaceClassBytes(
        "example/Covariant", covariantBytes(workspace), request(0, jasm::ValidationMode::Off));
    ASSERT_TRUE(skipped.status.is_ok());
    EXPECT_TRUE(skipped.diagnostics.empty());
}

TEST(MjWorkspaceValidationGateTest, rebuildGateFollowsValidationMode)
{
    WorkspaceBase workspace;
    const auto    input = writeGateFixture("mjbase_workspace_gate_rebuild.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());

    // jasm 重建管线自带校验门随 validation 档位（此前档位不进重建门，
    // 三档诊断恒同）：strict 下重汇编协变捷径文本如约拒绝，诊断归因
    // 重建门的 Validate 阶段而非编辑门。
    auto rejected = workspace.validateJasm("example/Covariant.class",
                                           utf16("example/Covariant.jasm").ref(),
                                           utf16(covariantSource()).ref(),
                                           request(kAnyRevision, jasm::ValidationMode::Strict));
    EXPECT_FALSE(rejected.status.is_ok());
    ASSERT_FALSE(rejected.diagnostics.empty());
    bool rebuildStage = false;
    for (const auto& diagnostic : rejected.diagnostics) {
        if (diagnostic.stage == jasm::DiagnosticStage::Validate) rebuildStage = true;
    }
    EXPECT_TRUE(rebuildStage);

    // off 连重建校验一起跳过（汇编本身仍必须成功），同一文本直通。
    auto skipped = workspace.validateJasm("example/Covariant.class",
                                          utf16("example/Covariant.jasm").ref(),
                                          utf16(covariantSource()).ref(),
                                          request(kAnyRevision, jasm::ValidationMode::Off));
    EXPECT_TRUE(skipped.status.is_ok());
    EXPECT_TRUE(skipped.diagnostics.empty());
}

TEST(MjWorkspaceValidationGateTest, rebuildGateResolvesBareInternalName)
{
    WorkspaceBase workspace;
    const auto    input = writeGateFixture("mjbase_workspace_gate_rebuild_bare.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());

    // jasm 补丁选择器与结构化补丁同口径：裸内部名按字节内容解析到规范
    // URI（此前只有 replaceClassBytes/applyClassMutation 解析，jasm 路径
    // 传裸名报资源不存在）。
    auto roundTrip = workspace.validateJasm("workspace/Subject",
                                            utf16("workspace/Subject.jasm").ref(),
                                            utf16(subjectSource("1")).ref(),
                                            request(kAnyRevision, jasm::ValidationMode::Off));
    ASSERT_TRUE(roundTrip.status.is_ok()) << roundTrip.status.message;
    EXPECT_TRUE(roundTrip.diagnostics.empty());
}

TEST(MjWorkspaceValidationGateTest, structuredWriteBackGateFollowsValidationMode)
{
    WorkspaceBase workspace;
    const auto    input = writeGateFixture("mjbase_workspace_gate_writeback.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());

    // 结构化编辑写回后的整类校验门随档位：strict 下基线协变捷径如约拒绝
    // （该门此前恒用默认档，三档无差别、off 也拦）。
    ClassMutation strictEdit;
    strictEdit.classUri = "example/Covariant.class";
    strictEdit.kind     = ClassMutationKind::ChangeClassAccess;
    strictEdit.setFlags = {"final"};
    auto rejected =
        workspace.applyClassMutation(strictEdit, request(0, jasm::ValidationMode::Strict));
    EXPECT_FALSE(rejected.status.is_ok());

    // off 直通：同编辑照常入账（写回门与受影响类的编辑门同步跳过）。
    auto accepted = workspace.applyClassMutation(
        strictEdit, request(kAnyRevision, jasm::ValidationMode::Off));
    ASSERT_TRUE(accepted.status.is_ok()) << accepted.status.message;
}

}   // namespace mjbase::workspace
