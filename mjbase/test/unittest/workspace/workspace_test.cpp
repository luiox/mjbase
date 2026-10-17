/// @file        workspace_test.cpp
/// @brief       WorkspaceBase 事务/查询面单测（移植自
///              mbase/unittest/workspace/workspace_test.cpp，26 用例处置见下）。
///
/// 移植口径（批次 B6-B）：gtest 套件名加 Mj 前缀；命名空间 mjbase::workspace；
/// 错误通道 ca::StatusCode → WorkspaceError（断言码一一对应）；access 常量
/// jbytecode Opcodes → workspace_types kAcc*。夹具全部经 mjbase jasm 现汇编
/// （零拷贝红线：无样本字节入库）。
///
/// 26 用例处置清单：
/// - 直迁 15：ProvidesStructuredQueriesAndSupportingResources /
///   KeepsLargeSignaturesOutOfClassSummaries / OpensEditsUndoesAndExportsSingleClass /
///   RejectsInvalidSingleClassWithoutChangingLoadedState / OpensAndExportsUtf8ArtifactPaths /
///   SearchesClassesMembersConstantsAndInstructions / AppliesValidatedJasmWithRevisionAndHistory /
///   AppliesStructuredClassMutationsThroughOneTransactionPath /
///   ReplacesListsAndRevertsRawClassBytesTransactionally /
///   DiffsAllResourcesAndRepairsSelectedJarDamageTransactionally /
///   ReportsOverrideComponentWithPrimaryAndSupportingDecoration（组件闭包已在新栈
///   mjbase 层次图上等价重建，不依赖 renamer 编排面）/
///   RejectsInvalidJasmAndProtectsInputDuringAtomicExport /
///   UsesIWorkspaceThroughInterfacePolymorphism / summary 三例（单后端化，见
///   MjWorkspaceSummaryTest）。
/// - 等价改写 2：
///   - CommitsPrivateWorkflowCandidateJarAtomically：候选产物旧栈经 applyRemap
///     整库重命名构造；mjbase 重映射器未随迁，改经两次 applyClassBytes 等价构造
///     （Subject→RenamedSubject + Child super 同步改指），候选 JAR 协议断言
///     （dry-run/导出原子性/revision 门/ABORTED 清理）逐条保留；
///   - BuildsExecutableInstructionFamiliesFromSymbolicLabels：指令索引从 ASM
///     树序号（含 label/line 节点）换算为 CodeDom 代码指令序号（ifeq 2→1、
///     "zero" 绑定 5→4）；LdcHandle/LdcDynamic/InvokeDynamic 家族改断言入口拒绝
///     （libjbc 常量池无 MethodHandle/Dynamic/InvokeDynamic 追加 API——主线
///     裁决点，见迁移报告）；LdcType 传 JVM 内部名（mjbase InstructionSpec
///     契约）；夹具本就 V17（mjbase jasm 缺省 major=61），withClassVersion 面
///     不再需要。
///   - RenameLocal 直通（旧 RoutesLocalRenameThroughClassMutationDirectly）：
///     rename 编排面未随迁，Local 能力由 ClassMutationKind::RenameLocal 承载，
///     断言 LVT 记录名 held→kept、index/descriptor 不变与 NOT_FOUND 负例。
/// - 缓迁 7（依赖的共享设施未在 mjbase 落地，待主线裁决后补）：
///   RunsRemapAndPipelineAsOneWorkflowTransaction /
///   RunsPassPipelinesThroughCandidateTransactions（MemberRemapPlan/RemapPolicy
///   + pass-registry 内置 pass）；AnalyzesAndAppliesWorkspaceRenamesThroughRemapPlan /
///   RequiresReviewBeforeRenamingOverrideComponent /
///   PinsRenameErrorCodeMatrixForInvalidRequests /
///   EmitsVerbatimRenameMappingStringsForEveryKind /
///   KeepsCascadeTwoStatePlansOnIdenticalMappings（renamer 引擎 RenameContext/
///   RenameRequest 编排面；Local 形态已由上面改写用例承载）。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/workspace/classfile_support.hpp"
#include "mjbase/workspace/summary_scan.hpp"
#include "mjbase/workspace/workspace.hpp"

namespace mjbase::workspace {
namespace model = mjbase::model;

namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string classSource(const std::string& name, const std::string& value)
{
    return ".super java/lang/Object\n"
           ".class public super " +
           name +
           " {\n"
           "  .field public counter I\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      bipush " +
           value +
           "\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "  .method public virtualValue ()I {\n"
           "    code: {\n"
           "      iconst_0\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string callerSource()
{
    return ".super java/lang/Object\n"
           ".class public super workspace/Caller {\n"
           "  .method public static call ()I {\n"
           "    code: {\n"
           "      invokestatic workspace/Subject.value ()I\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string childSource(const std::string& superName)
{
    return ".super " + superName + "\n"
           ".class public super workspace/Child {\n"
           "  .method public virtualValue ()I {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& value)
{
    jasm::JvmJasmService service;
    auto                 sourceName = utf16(name + ".jasm");
    auto                 source     = utf16(classSource(name, value));
    auto                 result     = service.assemble(sourceName.ref(), source.ref());
    if (!result.isOk()) throw std::runtime_error("workspace fixture did not assemble");
    return result.value->bytes;
}

std::vector<ca::u8> assembleSource(const std::string& name, const std::string& sourceText)
{
    jasm::JvmJasmService service;
    auto                 sourceName = utf16(name + ".jasm");
    auto                 source     = utf16(sourceText);
    auto                 result     = service.assemble(sourceName.ref(), source.ref());
    if (!result.isOk()) throw std::runtime_error("workspace fixture did not assemble");
    return result.value->bytes;
}

/// @brief 给 class 追加 Signature 属性（jbc 直改：typed SignatureAttr）。
std::vector<ca::u8> withClassSignature(const std::vector<ca::u8>& bytes,
                                       const std::string&         signature)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) throw std::runtime_error("signature fixture read failed");
    auto                  parsed  = std::move(read).unwrap();
    jbc::core::ClassFile& cf      = *parsed.file;
    auto sig = std::make_unique<jbc::core::SignatureAttr>();
    sig->signatureCp = support::internUtf8(cf, signature);
    cf.attrs.push_back(jbc::core::AttrSlot::makeNew(support::internUtf8(cf, "Signature"),
                                                    std::move(sig)));
    auto written = jbc::core::writeClass(cf);
    if (written.is_err()) throw std::runtime_error("signature fixture write failed");
    return std::move(written).unwrap();
}

std::filesystem::path writeLongSignatureFixture(const std::string& signature)
{
    model::ResourceContainer container;
    const std::string        uri = "workspace/LongSignature.class";
    container.addResource(
        uri,
        std::make_unique<model::ClassResource>(
            uri, withClassSignature(assemble("workspace/LongSignature", "1"), signature)));
    const auto path = test::getTempOutPath("mjbase_workspace_long_signature.jar");
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path writeWorkspaceFixture()
{
    model::ResourceContainer container;
    container.addResource("workspace/Subject.class",
                          std::make_unique<model::ClassResource>(
                              "workspace/Subject.class", assemble("workspace/Subject", "1")));
    container.addResource("workspace/Caller.class",
                          std::make_unique<model::ClassResource>(
                              "workspace/Caller.class", assembleSource("workspace/Caller", callerSource())));
    container.addResource("workspace/Child.class",
                          std::make_unique<model::ClassResource>(
                              "workspace/Child.class",
                              assembleSource("workspace/Child", childSource("workspace/Subject"))));
    container.addResource("data.txt",
                          std::make_unique<model::UnknownResource>(
                              "data.txt",
                              std::vector<ca::u8>{'w', 'o', 'r', 'k', 's', 'p', 'a', 'c', 'e'}));
    container.addResource("META-INF/MANIFEST.MF",
                          std::make_unique<model::ManifestResource>(mjbase::jar::Manifest::fromBytes(
                              {'M', 'a', 'n', 'i', 'f', 'e', 's', 't', '-', 'V', 'e', 'r', 's', 'i', 'o', 'n',
                               ':', ' ', '1', '.', '0', '\r', '\n', '\r', '\n'})));
    const auto path = test::getTempOutPath("mjbase_workspace_core_input.jar");
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path writeClassFixture(const std::string& fileName,
                                        const std::string& internalName,
                                        const std::string& value)
{
    model::ResourceContainer container;
    const auto               uri = internalName + ".class";
    container.addResource(uri,
                          std::make_unique<model::ClassResource>(uri, assemble(internalName, value)));
    const auto path = test::getTempOutPath(fileName);
    model::ResourceHelper::exportToClassFile(container, path.string(), uri);
    return path;
}

std::filesystem::path writeMalformedClassFixture()
{
    model::ResourceContainer container;
    const std::string        uri = "broken/Invalid.class";
    container.addResource(uri,
                          std::make_unique<model::ClassResource>(
                              uri, std::vector<ca::u8>{0xCA, 0xFE, 0xBA, 0xBE}));
    const auto path = test::getTempOutPath("mjbase_workspace_invalid_input.class");
    model::ResourceHelper::exportToClassFile(container, path.string(), uri);
    return path;
}

std::filesystem::path writeSupportingFixture()
{
    model::ResourceContainer container;
    container.addResource("support/Library.class",
                          std::make_unique<model::ClassResource>(
                              "support/Library.class",
                              assembleSource("support/Library",
                                             ".super workspace/Subject\n"
                                             ".class public super support/Library {\n"
                                             "  .method public static call ()I {\n"
                                             "    code: {\n"
                                             "      invokestatic workspace/Subject.value ()I\n"
                                             "      ireturn\n"
                                             "    }\n"
                                             "  }\n"
                                             "  .method public virtualValue ()I {\n"
                                             "    code: {\n"
                                             "      iconst_2\n"
                                             "      ireturn\n"
                                             "    }\n"
                                             "  }\n"
                                             "}\n")));
    const auto path = test::getTempOutPath("mjbase_workspace_core_support.jar");
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path writeRepairFixture()
{
    model::ResourceContainer container;
    container.addResource("workspace/RepairSubject.class",
                          std::make_unique<model::ClassResource>(
                              "workspace/RepairSubject.class", assemble("workspace/RepairSubject", "1")));
    // 照旧 import 分拣语义：broken/Truncated.class 带 4 字节 CAFEBABE 魔数，
    // 导入器按魔数嗅探分拣为 ClassResource（jasm 验证失败 → invalid_class，
    // 计入 classCount）；broken/Invalid.class 3 字节 → UnknownResource →
    // invalid_class_entry。
    container.addResource("broken/Invalid.class",
                          std::make_unique<model::UnknownResource>(
                              "broken/Invalid.class", std::vector<ca::u8>{0xCA, 0xFE, 0xBA}));
    container.addResource("broken/Truncated.class",
                          std::make_unique<model::UnknownResource>(
                              "broken/Truncated.class",
                              std::vector<ca::u8>{0xCA, 0xFE, 0xBA, 0xBE}));
    container.addResource("logs/corrupt_output_stream.txt",
                          std::make_unique<model::UnknownResource>(
                              "logs/corrupt_output_stream.txt", std::vector<ca::u8>{'b', 'a', 'd'}));
    const auto path = test::getTempOutPath("mjbase_workspace_repair_input.jar");
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// @brief Local 直通夹具：hold()I 真实占用 0 号局部槽；LVT 记录经 jbc 直改补入
/// （mjbase jasm 不产 LVT；等价 javac 常规产物形态，全程程序化构造）。
std::vector<ca::u8> localHolderBytes()
{
    const auto source = ".super java/lang/Object\n"
                        ".class public super workspace/LocalHolder {\n"
                        "  .method public static hold ()I {\n"
                        "    code: {\n"
                        "      bipush 7\n"
                        "      istore 0\n"
                        "      iload 0\n"
                        "      ireturn\n"
                        "    }\n"
                        "  }\n"
                        "}\n";
    auto read = jbc::core::readClass(
        jbc::core::ByteView(assembleSource("workspace/LocalHolder", source)));
    if (read.is_err()) throw std::runtime_error("local holder fixture read failed");
    auto                  parsed = std::move(read).unwrap();
    jbc::core::ClassFile& cf     = *parsed.file;
    jbc::core::MemberInfo* hold = nullptr;
    for (auto& method : cf.methods) {
        if (support::utf8Text(cf, method.nameCp) == "hold" &&
            support::utf8Text(cf, method.descCp) == "()I") {
            hold = &method;
            break;
        }
    }
    if (hold == nullptr) throw std::runtime_error("local holder method not found");
    auto code = support::ensureCode(cf, *hold);
    if (code == nullptr) throw std::runtime_error("local holder code missing");

    // LVT 行覆盖方法体全程：start=0（首指令）、length=code 长度、slot 0。
    jbc::core::LocalVariableTableAttr::Row row;
    row.startPc  = 0;
    row.length   = static_cast<ca::u32>(code->code.size());
    row.nameCp   = support::internUtf8(cf, "held");
    row.descCp   = support::internUtf8(cf, "I");
    row.slot     = 0;
    const ca::u32 lvtName = support::internUtf8(cf, "LocalVariableTable");
    auto lvt = std::make_unique<jbc::core::LocalVariableTableAttr>();
    lvt->rows.push_back(row);
    code->attrs.push_back(jbc::core::AttrSlot::makeNew(lvtName, std::move(lvt)));

    auto written = jbc::core::writeClass(cf);
    if (written.is_err()) throw std::runtime_error("local holder fixture write failed");
    return std::move(written).unwrap();
}

std::filesystem::path writeLocalHolderFixture()
{
    model::ResourceContainer container;
    container.addResource("workspace/LocalHolder.class",
                          std::make_unique<model::ClassResource>("workspace/LocalHolder.class",
                                                                 localHolderBytes()));
    const auto path = test::getTempOutPath("mjbase_workspace_local_holder_input.jar");
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

MutationRequest expected(ca::u64 revision, bool dryRun = false)
{
    MutationRequest request;
    request.expectedRevision = revision;
    request.dryRun           = dryRun;
    request.label            = "workspace test";
    return request;
}

}   // namespace

// ── 结构化查询 + supporting 生命周期（test 1 直迁）───────────────────────

TEST(MjWorkspaceCoreTest, ProvidesStructuredQueriesAndSupportingResources)
{
    WorkspaceBase workspace;
    const auto    input = writeWorkspaceFixture();
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    EXPECT_EQ(ArtifactKind::Jar, workspace.artifactKind());
    EXPECT_TRUE(workspace.primaryClassUri().empty());
    EXPECT_EQ(0U, workspace.revision());
    EXPECT_FALSE(workspace.isDirty());
    EXPECT_TRUE(workspace.canExport());

    auto resources = workspace.listResources();
    ASSERT_TRUE(resources.is_ok());
    auto resourceList = std::move(resources).unwrap();
    ASSERT_GE(resourceList.size(), 3U);
    EXPECT_NE(std::find_if(resourceList.begin(), resourceList.end(),
                           [](const ResourceSummary& resource) {
                               return resource.uri == "META-INF/MANIFEST.MF";
                           }),
              resourceList.end());

    auto content = workspace.readResource("data.txt");
    ASSERT_TRUE(content.is_ok());
    auto contentValue = std::move(content).unwrap();
    EXPECT_EQ("resource", contentValue.summary.type);
    EXPECT_EQ("workspace", std::string(contentValue.bytes.begin(), contentValue.bytes.end()));

    auto classBytes = workspace.readClassBytes("workspace/Subject");
    ASSERT_TRUE(classBytes.is_ok());
    const auto classBytesValue = std::move(classBytes).unwrap();
    EXPECT_EQ("workspace/Subject.class", classBytesValue.resourceUri);
    EXPECT_EQ("workspace/Subject", classBytesValue.internalName);
    EXPECT_EQ(0U, classBytesValue.fingerprint.find("fnv1a64:"));
    ASSERT_GE(classBytesValue.bytes.size(), 4U);
    EXPECT_EQ(0xcaU, static_cast<ca::u32>(classBytesValue.bytes[0]));
    EXPECT_EQ(0xfeU, static_cast<ca::u32>(classBytesValue.bytes[1]));
    EXPECT_EQ(0xbaU, static_cast<ca::u32>(classBytesValue.bytes[2]));
    EXPECT_EQ(0xbeU, static_cast<ca::u32>(classBytesValue.bytes[3]));

    auto summaries = workspace.listClassSummaries();
    ASSERT_TRUE(summaries.is_ok());
    const auto summaryList = std::move(summaries).unwrap();
    ASSERT_EQ(3U, summaryList.size());
    const auto subjectSummary =
        std::find_if(summaryList.begin(), summaryList.end(), [](const ClassSummary& summary) {
            return summary.internalName == "workspace/Subject";
        });
    ASSERT_NE(subjectSummary, summaryList.end());
    EXPECT_EQ("workspace/Subject.class", subjectSummary->resourceUri);
    EXPECT_EQ("java/lang/Object", subjectSummary->superName);
    EXPECT_EQ(0U, subjectSummary->interfaceCount);
    EXPECT_EQ(1U, subjectSummary->fieldCount);
    EXPECT_EQ(2U, subjectSummary->methodCount);

    auto classes = workspace.listClasses();
    ASSERT_TRUE(classes.is_ok());
    auto classList = std::move(classes).unwrap();
    ASSERT_EQ(3U, classList.size());
    const auto subject = std::find_if(classList.begin(), classList.end(), [](const ClassInfo& info) {
        return info.internalName == "workspace/Subject";
    });
    ASSERT_NE(subject, classList.end());
    ASSERT_EQ(2U, subject->methods.size());
    EXPECT_NE(std::find_if(subject->methods.begin(), subject->methods.end(),
                           [](const MethodInfo& method) {
                               return method.name == "value" && method.descriptor == "()I";
                           }),
              subject->methods.end());

    auto method = workspace.methodInfo("workspace/Subject", "value", "()I");
    ASSERT_TRUE(method.is_ok());
    auto methodValue = std::move(method).unwrap();
    EXPECT_EQ(2U, methodValue.instructionCount);
    EXPECT_EQ(0U, methodValue.tryCatchCount);

    auto field = workspace.fieldInfo("workspace/Subject", "counter", "I");
    ASSERT_TRUE(field.is_ok());
    EXPECT_EQ("counter", field.unwrap().name);
    EXPECT_EQ("I", field.unwrap().descriptor);

    auto bytecode = workspace.methodBytecode("workspace/Subject", "value", "()I");
    ASSERT_TRUE(bytecode.is_ok());
    ASSERT_EQ(2U, bytecode.unwrap().instructions.size());
    EXPECT_EQ("ldc", bytecode.unwrap().instructions[0].mnemonic);
    EXPECT_EQ("ireturn", bytecode.unwrap().instructions[1].mnemonic);

    auto methodJasm = workspace.getMethodJasm("workspace/Subject", "value", "()I");
    ASSERT_TRUE(methodJasm.isOk());
    EXPECT_NE(
        std::string::npos,
        methodJasm.value->source.to_utf8_string().to_std_string().find(".method public static value ()I"));

    auto manifest = workspace.manifestAttributes();
    ASSERT_TRUE(manifest.is_ok());
    ASSERT_EQ(1U, manifest.unwrap().size());
    EXPECT_EQ("Manifest-Version", manifest.unwrap()[0].key);
    EXPECT_EQ("1.0", manifest.unwrap()[0].value);

    auto strings = workspace.resourceStrings();
    ASSERT_TRUE(strings.is_ok());
    auto stringValues = std::move(strings).unwrap();
    EXPECT_NE(std::find_if(stringValues.begin(), stringValues.end(),
                           [](const ResourceStringInfo& item) {
                               return item.uri == "data.txt" && item.value == "workspace";
                           }),
              stringValues.end());

    auto methodXrefs = workspace.xrefsToMethod("workspace/Subject", "value", "()I");
    ASSERT_TRUE(methodXrefs.is_ok());
    auto methodXrefValues = std::move(methodXrefs).unwrap();
    ASSERT_EQ(1U, methodXrefValues.size());
    EXPECT_EQ("workspace/Caller", methodXrefValues[0].sourceClass);
    EXPECT_EQ("workspace/Subject.value()I", methodXrefValues[0].target);

    auto directCallees = workspace.calleesOfMethod("workspace/Caller", "call", "()I");
    ASSERT_TRUE(directCallees.is_ok());
    ASSERT_EQ(1U, directCallees.unwrap().size());
    EXPECT_EQ("workspace/Subject.value()I", directCallees.unwrap()[0].target);

    auto relations = workspace.classRelations("workspace/Subject");
    ASSERT_TRUE(relations.is_ok());
    auto relationValue = std::move(relations).unwrap();
    EXPECT_NE(std::find(relationValue.superclasses.begin(), relationValue.superclasses.end(),
                        "java/lang/Object"),
              relationValue.superclasses.end());
    EXPECT_EQ(std::vector<std::string>({"workspace/Child"}), relationValue.directSubclasses);

    auto jasmText = workspace.getJasm("workspace/Subject.class");
    ASSERT_TRUE(jasmText.isOk());
    EXPECT_NE(std::string::npos,
              jasmText.value->source.to_utf8_string().to_std_string().find("workspace/Subject"));

    const auto supportPath = writeSupportingFixture();
    auto       supportAdded = workspace.addSupportingJar(supportPath.string());
    ASSERT_TRUE(supportAdded.is_ok());
    const auto supportAddedValue = std::move(supportAdded).unwrap();
    EXPECT_EQ("support-1", supportAddedValue.id);
    EXPECT_EQ(0U, supportAddedValue.fingerprint.find("fnv1a64:"));
    EXPECT_EQ(1U, supportAddedValue.classCount);
    const auto supporting = workspace.supportingResources();
    ASSERT_EQ(1U, supporting.size());
    EXPECT_EQ("support-1", supporting[0].id);
    EXPECT_EQ(supportAddedValue.fingerprint, supporting[0].fingerprint);
    EXPECT_EQ(1U, supporting[0].classCount);
    auto duplicateSupport = workspace.addSupportingJar(supportPath.string());
    ASSERT_TRUE(duplicateSupport.is_err());
    EXPECT_EQ(WorkspaceError::AlreadyExists, duplicateSupport.unwrap_err().code);
    const auto supportCopy = test::getTempOutPath("mjbase_workspace_core_support_copy.jar");
    std::error_code copyError;
    std::filesystem::remove(supportCopy, copyError);
    copyError.clear();
    std::filesystem::copy_file(supportPath, supportCopy, copyError);
    ASSERT_FALSE(copyError);
    auto duplicateContent = workspace.addSupportingJar(supportCopy.string());
    ASSERT_TRUE(duplicateContent.is_err());
    EXPECT_EQ(WorkspaceError::AlreadyExists, duplicateContent.unwrap_err().code);

    auto relationsWithSupport = workspace.classRelations("workspace/Subject");
    ASSERT_TRUE(relationsWithSupport.is_ok());
    const auto relationsWithSupportValue = std::move(relationsWithSupport).unwrap();
    EXPECT_NE(std::find(relationsWithSupportValue.allSubclasses.begin(),
                        relationsWithSupportValue.allSubclasses.end(), "support/Library"),
              relationsWithSupportValue.allSubclasses.end());
    const auto supportRelation =
        std::find_if(relationsWithSupportValue.sources.begin(),
                     relationsWithSupportValue.sources.end(),
                     [](const ClassRelationSource& source) {
                         return source.internalName == "support/Library";
                     });
    ASSERT_NE(supportRelation, relationsWithSupportValue.sources.end());
    EXPECT_TRUE(supportRelation->supporting);
    EXPECT_EQ("support-1", supportRelation->supportId);
    auto supportCallers = workspace.xrefsToMethod("workspace/Subject", "value", "()I");
    ASSERT_TRUE(supportCallers.is_ok());
    const auto supportCallerValues = std::move(supportCallers).unwrap();
    const auto supportCaller =
        std::find_if(supportCallerValues.begin(), supportCallerValues.end(),
                     [](const ReferenceInfo& reference) {
                         return reference.sourceClass == "support/Library";
                     });
    ASSERT_NE(supportCaller, supportCallerValues.end());
    EXPECT_TRUE(supportCaller->supporting);
    EXPECT_EQ("support-1", supportCaller->supportId);
    auto supportOverrides =
        workspace.overridesOfMethod("workspace/Subject", "virtualValue", "()I");
    ASSERT_TRUE(supportOverrides.is_ok());
    const auto supportOverrideValues = std::move(supportOverrides).unwrap();
    const auto supportOverride =
        std::find_if(supportOverrideValues.begin(), supportOverrideValues.end(),
                     [](const MethodOverrideInfo& overrideInfo) {
                         return overrideInfo.owner == "support/Library";
                     });
    ASSERT_NE(supportOverride, supportOverrideValues.end());
    EXPECT_TRUE(supportOverride->supporting);
    EXPECT_EQ("support-1", supportOverride->supportId);

    auto removedSupport = workspace.removeSupportingJar("support-1");
    ASSERT_TRUE(removedSupport.is_ok());
    EXPECT_EQ(supportAddedValue.fingerprint, removedSupport.unwrap().fingerprint);
    EXPECT_TRUE(workspace.supportingResources().empty());
    auto relationsWithoutSupport = workspace.classRelations("workspace/Subject");
    ASSERT_TRUE(relationsWithoutSupport.is_ok());
    const auto relationsWithoutSupportValue = std::move(relationsWithoutSupport).unwrap();
    EXPECT_EQ(relationsWithoutSupportValue.allSubclasses.end(),
              std::find(relationsWithoutSupportValue.allSubclasses.begin(),
                        relationsWithoutSupportValue.allSubclasses.end(), "support/Library"));
    auto missingSupport = workspace.removeSupportingJar("support-1");
    ASSERT_TRUE(missingSupport.is_err());
    EXPECT_EQ(WorkspaceError::NotFound, missingSupport.unwrap_err().code);

    const auto damagedSupport = test::getTempOutPath("mjbase_workspace_damaged_support.jar");
    std::ofstream(damagedSupport, std::ios::binary | std::ios::trunc) << "not a jar";
    auto damaged = workspace.addSupportingJar(damagedSupport.string());
    ASSERT_TRUE(damaged.is_err());
    EXPECT_EQ(WorkspaceError::DataLoss, damaged.unwrap_err().code);
}

TEST(MjWorkspaceCoreTest, KeepsLargeSignaturesOutOfClassSummaries)
{
    const std::string signature =
        "<T" + std::string(65000, 'A') + ":Ljava/lang/Object;>Ljava/lang/Object;";
    WorkspaceBase workspace;
    ASSERT_TRUE(workspace.openJar(writeLongSignatureFixture(signature).string()).is_ok());

    auto summaries = workspace.listClassSummaries();
    ASSERT_TRUE(summaries.is_ok());
    const auto summaryValues = std::move(summaries).unwrap();
    ASSERT_EQ(1U, summaryValues.size());
    EXPECT_EQ("workspace/LongSignature.class", summaryValues[0].resourceUri);
    EXPECT_EQ("workspace/LongSignature", summaryValues[0].internalName);
    EXPECT_EQ(1U, summaryValues[0].fieldCount);
    EXPECT_EQ(2U, summaryValues[0].methodCount);

    auto detail = workspace.classInfo("workspace/LongSignature");
    ASSERT_TRUE(detail.is_ok());
    EXPECT_EQ(signature, detail.unwrap().signature);
}

// ── 单 class 工作区（test 3/4 直迁）─────────────────────────────────────

TEST(MjWorkspaceCoreTest, OpensEditsUndoesAndExportsSingleClass)
{
    WorkspaceBase workspace;
    const auto    input = writeClassFixture("mjbase_workspace_class_input.class",
                                            "workspace/SingleSubject", "1");
    ASSERT_TRUE(workspace.openClass(input.string()).is_ok());
    EXPECT_TRUE(workspace.isLoaded());
    EXPECT_EQ(ArtifactKind::Class, workspace.artifactKind());
    EXPECT_EQ("workspace/SingleSubject.class", workspace.primaryClassUri());
    EXPECT_EQ(input.string(), workspace.sourcePath());
    EXPECT_EQ(0U, workspace.revision());
    EXPECT_FALSE(workspace.isDirty());

    auto resources = workspace.listResources();
    ASSERT_TRUE(resources.is_ok());
    ASSERT_EQ(1U, resources.unwrap().size());
    EXPECT_EQ(workspace.primaryClassUri(), resources.unwrap()[0].uri);
    EXPECT_TRUE(workspace.classInfo("workspace/SingleSubject").is_ok());
    EXPECT_TRUE(workspace.methodBytecode("workspace/SingleSubject", "value", "()I").is_ok());
    auto originalJasm = workspace.getJasm(workspace.primaryClassUri());
    ASSERT_TRUE(originalJasm.isOk());
    const auto originalSource = originalJasm.value->source.to_utf8_string().to_std_string();

    auto sourceName   = utf16("SingleSubject.jasm");
    auto replacement  = utf16(classSource("workspace/SingleSubject", "42"));
    auto stale = workspace.applyJasm(workspace.primaryClassUri(), sourceName.ref(),
                                     replacement.ref(), expected(1));
    EXPECT_EQ(WorkspaceError::Aborted, stale.status.code);
    EXPECT_EQ(0U, workspace.revision());

    auto applied = workspace.applyJasm(workspace.primaryClassUri(), sourceName.ref(),
                                       replacement.ref(), expected(0));
    ASSERT_TRUE(applied.status.is_ok());
    EXPECT_TRUE(applied.committed);
    EXPECT_EQ(1U, applied.revision);
    EXPECT_TRUE(workspace.isDirty());

    const auto undone = workspace.undo(expected(1));
    ASSERT_TRUE(undone.status.is_ok());
    EXPECT_EQ(2U, undone.revision);
    EXPECT_FALSE(workspace.isDirty());
    const auto redone = workspace.redo(expected(2));
    ASSERT_TRUE(redone.status.is_ok());
    EXPECT_EQ(3U, redone.revision);
    EXPECT_TRUE(workspace.isDirty());

    EXPECT_EQ(WorkspaceError::PermissionDenied,
              workspace.exportClass(input.string()).unwrap_err().code);
    EXPECT_EQ(WorkspaceError::FailedPrecondition,
              workspace.exportJar(test::getTempOutPath("class_as_jar.jar").string())
                  .unwrap_err()
                  .code);
    const auto output = test::getTempOutPath("mjbase_workspace_class_output.class");
    std::error_code filesystemError;
    std::filesystem::remove(output, filesystemError);
    ASSERT_TRUE(workspace.exportClass(output.string()).is_ok());
    EXPECT_TRUE(std::filesystem::exists(output));
    EXPECT_EQ(WorkspaceError::AlreadyExists,
              workspace.exportClass(output.string()).unwrap_err().code);

    WorkspaceBase exported;
    ASSERT_TRUE(exported.openClass(output.string()).is_ok());
    auto exportedJasm = exported.getJasm(exported.primaryClassUri());
    ASSERT_TRUE(exportedJasm.isOk());
    EXPECT_NE(originalSource, exportedJasm.value->source.to_utf8_string().to_std_string());

    WorkspaceBase original;
    ASSERT_TRUE(original.openClass(input.string()).is_ok());
    auto reopenedOriginalJasm = original.getJasm(original.primaryClassUri());
    ASSERT_TRUE(reopenedOriginalJasm.isOk());
    EXPECT_EQ(originalSource,
              reopenedOriginalJasm.value->source.to_utf8_string().to_std_string());
}

TEST(MjWorkspaceCoreTest, RejectsInvalidSingleClassWithoutChangingLoadedState)
{
    WorkspaceBase workspace;
    const auto    input = writeClassFixture("mjbase_workspace_class_state_input.class",
                                            "workspace/StableSubject", "1");
    ASSERT_TRUE(workspace.openClass(input.string()).is_ok());
    auto sourceName  = utf16("StableSubject.jasm");
    auto replacement = utf16(classSource("workspace/StableSubject", "42"));
    ASSERT_TRUE(workspace
                    .applyJasm(workspace.primaryClassUri(), sourceName.ref(), replacement.ref(),
                               expected(0))
                    .status.is_ok());
    auto editedJasm = workspace.getJasm(workspace.primaryClassUri());
    ASSERT_TRUE(editedJasm.isOk());
    const auto editedSource = editedJasm.value->source.to_utf8_string().to_std_string();

    auto invalidJasm = utf16(classSource("workspace/StableSubject", "1\n      return"));
    const auto rejectedEdit = workspace.applyJasm(
        workspace.primaryClassUri(), sourceName.ref(), invalidJasm.ref(), expected(1));
    EXPECT_EQ(WorkspaceError::InvalidArgument, rejectedEdit.status.code);
    EXPECT_EQ(1U, workspace.revision());

    const auto malformed = writeMalformedClassFixture();
    EXPECT_EQ(WorkspaceError::DataLoss,
              workspace.openClass(malformed.string()).unwrap_err().code);
    EXPECT_EQ(ArtifactKind::Class, workspace.artifactKind());
    EXPECT_EQ(input.string(), workspace.sourcePath());
    EXPECT_EQ("workspace/StableSubject.class", workspace.primaryClassUri());
    EXPECT_EQ(1U, workspace.revision());
    EXPECT_TRUE(workspace.isDirty());
    auto currentJasm = workspace.getJasm(workspace.primaryClassUri());
    ASSERT_TRUE(currentJasm.isOk());
    EXPECT_EQ(editedSource, currentJasm.value->source.to_utf8_string().to_std_string());
}

TEST(MjWorkspaceCoreTest, OpensAndExportsUtf8ArtifactPaths)
{
    namespace fs = std::filesystem;
    const auto directory = test::getTempOutPath("mjbase_utf8_artifacts").parent_path() /
                           fs::u8path(u8"含 空格目录");
    std::error_code filesystemError;
    fs::create_directories(directory, filesystemError);
    ASSERT_FALSE(filesystemError);

    const auto sourceClass =
        writeClassFixture("mjbase_utf8_source.class", "workspace/Utf8Subject", "1");
    const auto utf8Class = directory / fs::u8path(u8"输入样本.class");
    fs::copy_file(sourceClass, utf8Class, fs::copy_options::overwrite_existing, filesystemError);
    ASSERT_FALSE(filesystemError);

    WorkspaceBase classWorkspace;
    ASSERT_TRUE(classWorkspace.openClass(utf8Class.u8string()).is_ok());
    EXPECT_EQ(utf8Class.u8string(), classWorkspace.sourcePath());
    const auto exportedClass = directory / fs::u8path(u8"导出样本.class");
    fs::remove(exportedClass, filesystemError);
    ASSERT_TRUE(classWorkspace.exportClass(exportedClass.u8string()).is_ok());
    EXPECT_TRUE(fs::exists(exportedClass));

    const auto sourceJar = writeWorkspaceFixture();
    const auto utf8Jar   = directory / fs::u8path(u8"输入样本.jar");
    fs::copy_file(sourceJar, utf8Jar, fs::copy_options::overwrite_existing, filesystemError);
    ASSERT_FALSE(filesystemError);

    WorkspaceBase jarWorkspace;
    ASSERT_TRUE(jarWorkspace.openJar(utf8Jar.u8string()).is_ok());
    EXPECT_EQ(utf8Jar.u8string(), jarWorkspace.sourcePath());
    const auto exportedJar = directory / fs::u8path(u8"导出样本.jar");
    fs::remove(exportedJar, filesystemError);
    ASSERT_TRUE(jarWorkspace.exportJar(exportedJar.u8string()).is_ok());
    EXPECT_TRUE(fs::exists(exportedJar));
}

// ── 搜索面（test 6 直迁）────────────────────────────────────────────────

TEST(MjWorkspaceCoreTest, SearchesClassesMembersConstantsAndInstructions)
{
    WorkspaceBase workspace;
    ASSERT_TRUE(workspace.openJar(writeWorkspaceFixture().string()).is_ok());

    auto classes = workspace.searchClasses("subject");
    ASSERT_TRUE(classes.is_ok());
    auto classHits = std::move(classes).unwrap();
    EXPECT_NE(std::find_if(classHits.begin(), classHits.end(), [](const SearchHit& hit) {
                  return hit.kind == "class" && hit.owner == "workspace/Subject";
              }),
              classHits.end());

    SearchOptions caseSensitive;
    caseSensitive.caseSensitive = true;
    auto caseSensitiveClasses = workspace.searchClasses("subject", caseSensitive);
    ASSERT_TRUE(caseSensitiveClasses.is_ok());
    EXPECT_TRUE(caseSensitiveClasses.unwrap().empty());

    auto members = workspace.searchMembers("counter");
    ASSERT_TRUE(members.is_ok());
    auto memberHits = std::move(members).unwrap();
    EXPECT_NE(std::find_if(memberHits.begin(), memberHits.end(), [](const SearchHit& hit) {
                  return hit.kind == "field" && hit.owner == "workspace/Subject" &&
                         hit.name == "counter" && hit.descriptor == "I";
              }),
              memberHits.end());

    auto strings = workspace.searchStrings("workspace/Subject");
    ASSERT_TRUE(strings.is_ok());
    auto stringHits = std::move(strings).unwrap();
    EXPECT_NE(std::find_if(stringHits.begin(), stringHits.end(), [](const SearchHit& hit) {
                  return hit.resourceUri == "workspace/Subject.class" &&
                         hit.value == "workspace/Subject";
              }),
              stringHits.end());

    auto numbers = workspace.searchNumbers("1.0");
    ASSERT_TRUE(numbers.is_ok());
    auto numberHits = std::move(numbers).unwrap();
    EXPECT_NE(std::find_if(numberHits.begin(), numberHits.end(), [](const SearchHit& hit) {
                  return hit.kind == "instruction_number" && hit.owner == "workspace/Subject" &&
                         hit.name == "value" && hit.value == "1";
              }),
              numberHits.end());

    auto instructions = workspace.searchInstructions("invokestatic");
    ASSERT_TRUE(instructions.is_ok());
    auto instructionHits = std::move(instructions).unwrap();
    EXPECT_NE(std::find_if(instructionHits.begin(), instructionHits.end(),
                           [](const SearchHit& hit) {
                               return hit.kind == "instruction" && hit.owner == "workspace/Caller" &&
                                      hit.name == "call" &&
                                      hit.value.find("workspace/Subject.value()I") !=
                                          std::string::npos;
                           }),
              instructionHits.end());

    SearchOptions exactClass;
    exactClass.matchMode = SearchMatchMode::Exact;
    auto exactClasses = workspace.searchClasses("workspace/Subject", exactClass);
    ASSERT_TRUE(exactClasses.is_ok());
    ASSERT_EQ(1U, exactClasses.unwrap().size());

    SearchOptions regexClass;
    regexClass.matchMode     = SearchMatchMode::Regex;
    regexClass.caseSensitive = true;
    regexClass.packagePrefix = "workspace/";
    auto regexClasses =
        workspace.searchClasses("^workspace/(Subject|Caller)$", regexClass);
    ASSERT_TRUE(regexClasses.is_ok());
    EXPECT_EQ(2U, regexClasses.unwrap().size());

    SearchOptions fieldOnly;
    fieldOnly.memberKind = "field";
    fieldOnly.owner      = "workspace/Subject";
    auto filteredMembers = workspace.searchMembers("counter", fieldOnly);
    ASSERT_TRUE(filteredMembers.is_ok());
    ASSERT_EQ(1U, filteredMembers.unwrap().size());
    EXPECT_EQ("field", filteredMembers.unwrap()[0].kind);

    SearchOptions invokeStaticOnly;
    invokeStaticOnly.opcode = 184;
    invokeStaticOnly.owner  = "workspace/Caller";
    auto filteredInstructions =
        workspace.searchInstructions("workspace/Subject", invokeStaticOnly);
    ASSERT_TRUE(filteredInstructions.is_ok());
    ASSERT_EQ(1U, filteredInstructions.unwrap().size());

    SearchOptions invalidRegex;
    invalidRegex.matchMode = SearchMatchMode::Regex;
    auto malformedRegex    = workspace.searchClasses("[", invalidRegex);
    ASSERT_TRUE(malformedRegex.is_err());
    EXPECT_EQ(WorkspaceError::InvalidArgument, malformedRegex.unwrap_err().code);

    auto emptyQuery = workspace.searchStrings("");
    ASSERT_TRUE(emptyQuery.is_err());
    EXPECT_EQ(WorkspaceError::InvalidArgument, emptyQuery.unwrap_err().code);

    auto invalidNumber = workspace.searchNumbers("not-a-number");
    ASSERT_TRUE(invalidNumber.is_err());
    EXPECT_EQ(WorkspaceError::InvalidArgument, invalidNumber.unwrap_err().code);
}

// ── 候选 JAR 提交协议（test 8，等价改写：候选经 applyClassBytes 构造）────

TEST(MjWorkspaceCoreTest, CommitsPrivateWorkflowCandidateJarAtomically)
{
    const auto input = writeWorkspaceFixture();
    WorkspaceBase candidateWorkspace;
    ASSERT_TRUE(candidateWorkspace.openJar(input.string()).is_ok());
    // 候选产物：旧栈经 applyRemap 整库重命名 Subject→RenamedSubject；mjbase
    // 重映射器未随迁，改经两次 applyClassBytes 等价构造（类内部名迁移 +
    // Child super 同步改指），候选 JAR 协议断言逐条保留。
    auto renamedSubject = assembleSource("workspace/RenamedSubject",
                                         classSource("workspace/RenamedSubject", "1"));
    auto renamedChild   = assembleSource("workspace/Child", childSource("workspace/RenamedSubject"));
    ASSERT_TRUE(candidateWorkspace
                    .applyClassBytes("workspace/Subject.class", renamedSubject, expected(0))
                    .status.is_ok());
    ASSERT_TRUE(candidateWorkspace
                    .applyClassBytes("workspace/Child.class", renamedChild, expected(1))
                    .status.is_ok());

    const auto candidateJar = test::getTempOutPath("mjbase_workspace_private_candidate.jar");
    const auto exportedJar  = test::getTempOutPath("mjbase_workspace_candidate_export.jar");
    std::error_code filesystemError;
    std::filesystem::remove(candidateJar, filesystemError);
    std::filesystem::remove(exportedJar, filesystemError);
    ASSERT_TRUE(candidateWorkspace.exportJar(candidateJar.string()).is_ok());

    WorkspaceBase workspace;
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    WorkflowCandidateCommitRequest request;
    request.candidateJarPath = candidateJar.string();
    request.exportPath       = exportedJar.string();

    const auto preview = workspace.commitWorkflowCandidateJar(request, expected(0, true));
    ASSERT_TRUE(preview.mutation.status.is_ok());
    EXPECT_FALSE(preview.mutation.committed);
    EXPECT_EQ(0U, preview.mutation.revision);
    EXPECT_FALSE(preview.exported);
    EXPECT_EQ(std::filesystem::absolute(exportedJar).lexically_normal().string(),
              preview.exportPath);
    EXPECT_FALSE(std::filesystem::exists(exportedJar));
    ASSERT_TRUE(workspace.classInfo("workspace/Subject").is_ok());
    EXPECT_TRUE(workspace.classInfo("workspace/RenamedSubject").is_err());

    auto invalidExport = request;
    invalidExport.exportPath = input.string();
    const auto rejected = workspace.commitWorkflowCandidateJar(invalidExport, expected(0));
    EXPECT_TRUE(rejected.mutation.status.is_err());
    EXPECT_FALSE(rejected.mutation.committed);
    EXPECT_FALSE(rejected.exported);
    EXPECT_EQ(0U, workspace.revision());

    const auto committed = workspace.commitWorkflowCandidateJar(request, expected(0));
    ASSERT_TRUE(committed.mutation.status.is_ok());
    EXPECT_TRUE(committed.mutation.committed);
    EXPECT_EQ(1U, committed.mutation.revision);
    EXPECT_TRUE(committed.exported);
    ASSERT_TRUE(std::filesystem::exists(exportedJar));
    EXPECT_TRUE(workspace.classInfo("workspace/Subject").is_err());
    ASSERT_TRUE(workspace.classInfo("workspace/RenamedSubject").is_ok());

    WorkspaceBase exported;
    ASSERT_TRUE(exported.openJar(exportedJar.string()).is_ok());
    ASSERT_TRUE(exported.classInfo("workspace/RenamedSubject").is_ok());

    const auto staleExport =
        test::getTempOutPath("mjbase_workspace_stale_candidate_export.jar");
    std::filesystem::remove(staleExport, filesystemError);
    auto staleRequest = request;
    staleRequest.exportPath = staleExport.string();
    const auto stale = workspace.commitWorkflowCandidateJar(staleRequest, expected(0));
    EXPECT_EQ(WorkspaceError::Aborted, stale.mutation.status.code);
    EXPECT_FALSE(stale.exported);
    EXPECT_FALSE(std::filesystem::exists(staleExport));
}

// ── JASM 变更事务（test 9 直迁）─────────────────────────────────────────

TEST(MjWorkspaceCoreTest, AppliesValidatedJasmWithRevisionAndHistory)
{
    WorkspaceBase workspace;
    const auto    input = writeWorkspaceFixture();
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    auto sourceName  = utf16("Subject.jasm");
    auto replacement = utf16(classSource("workspace/Subject", "42"));

    auto stale = workspace.applyJasm("workspace/Subject.class", sourceName.ref(),
                                     replacement.ref(), expected(1));
    EXPECT_EQ(WorkspaceError::Aborted, stale.status.code);
    EXPECT_EQ(0U, stale.revision);

    auto preview = workspace.validateJasm("workspace/Subject.class", sourceName.ref(),
                                          replacement.ref(), expected(0, true));
    EXPECT_TRUE(preview.status.is_ok());
    EXPECT_FALSE(preview.committed);
    EXPECT_EQ(0U, preview.revision);
    ASSERT_FALSE(preview.byteDiffs.empty());
    EXPECT_TRUE(preview.exportable);

    ASSERT_TRUE(workspace.createSavepoint("original").is_ok());
    auto applied = workspace.applyJasm("workspace/Subject.class", sourceName.ref(),
                                       replacement.ref(), expected(0));
    ASSERT_TRUE(applied.status.is_ok());
    EXPECT_TRUE(applied.committed);
    EXPECT_EQ(1U, applied.revision);
    EXPECT_TRUE(workspace.isDirty());
    ASSERT_EQ(1U, applied.affectedClasses.size());
    EXPECT_EQ("workspace/Subject", applied.affectedClasses[0]);

    auto undone = workspace.undo(expected(1));
    ASSERT_TRUE(undone.status.is_ok());
    EXPECT_EQ(2U, undone.revision);
    EXPECT_FALSE(workspace.isDirty());

    auto redone = workspace.redo(expected(2));
    ASSERT_TRUE(redone.status.is_ok());
    EXPECT_EQ(3U, redone.revision);
    EXPECT_TRUE(workspace.isDirty());

    auto restored = workspace.restoreSavepoint("original", expected(3));
    ASSERT_TRUE(restored.status.is_ok());
    EXPECT_EQ(4U, restored.revision);
    EXPECT_FALSE(workspace.isDirty());
}

// ── 结构化 class 变更（test 10 直迁；RemoveInstruction 负例改序号越界）────

TEST(MjWorkspaceCoreTest, AppliesStructuredClassMutationsThroughOneTransactionPath)
{
    WorkspaceBase workspace;
    const auto    input = writeWorkspaceFixture();
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());

    ClassMutation setFieldAccess;
    setFieldAccess.classUri = "workspace/Subject.class";
    setFieldAccess.kind     = ClassMutationKind::SetFieldAccess;
    setFieldAccess.member   = {"counter", "I"};
    setFieldAccess.access   = kAccPrivate;

    auto preview = workspace.validateClassMutation(setFieldAccess, expected(0, true));
    ASSERT_TRUE(preview.status.is_ok());
    EXPECT_FALSE(preview.committed);
    EXPECT_EQ(0U, workspace.revision());
    ASSERT_FALSE(preview.byteDiffs.empty());

    auto accessChanged = workspace.applyClassMutation(setFieldAccess, expected(0));
    ASSERT_TRUE(accessChanged.status.is_ok());
    EXPECT_TRUE(accessChanged.committed);
    EXPECT_EQ(1U, accessChanged.revision);
    auto field = workspace.fieldInfo("workspace/Subject", "counter", "I");
    ASSERT_TRUE(field.is_ok());
    EXPECT_EQ(static_cast<ca::i32>(kAccPrivate), field.unwrap().access);

    // 代码指令序号口径：value()I 体 = [ldc 1, ireturn]，0 号即 ldc。
    ClassMutation replaceInstruction;
    replaceInstruction.classUri                 = "workspace/Subject.class";
    replaceInstruction.kind                     = ClassMutationKind::ReplaceInstruction;
    replaceInstruction.member                   = {"value", "()I"};
    replaceInstruction.instructionIndex         = 0;
    replaceInstruction.instruction.kind         = InstructionKind::LdcInt;
    replaceInstruction.instruction.operand      = 42;
    auto instructionChanged = workspace.applyClassMutation(replaceInstruction, expected(1));
    ASSERT_TRUE(instructionChanged.status.is_ok());
    EXPECT_EQ(2U, instructionChanged.revision);

    ClassMutation addMethod;
    addMethod.classUri = "workspace/Subject.class";
    addMethod.kind     = ClassMutationKind::AddMethod;
    addMethod.member   = {"added", "()V"};
    addMethod.access   = kAccPublic | kAccStatic;
    addMethod.initialInstructions.push_back({InstructionKind::Simple, 0xb1 /* RETURN */});
    auto methodAdded = workspace.applyClassMutation(addMethod, expected(2));
    ASSERT_TRUE(methodAdded.status.is_ok());
    EXPECT_EQ(3U, methodAdded.revision);
    EXPECT_TRUE(workspace.methodInfo("workspace/Subject", "added", "()V").is_ok());

    ClassMutation removeMethod;
    removeMethod.classUri = "workspace/Subject.class";
    removeMethod.kind     = ClassMutationKind::RemoveMethod;
    removeMethod.member   = {"added", "()V"};
    auto methodRemoved = workspace.applyClassMutation(removeMethod, expected(3));
    ASSERT_TRUE(methodRemoved.status.is_ok());
    EXPECT_EQ(4U, methodRemoved.revision);
    EXPECT_EQ(WorkspaceError::NotFound,
              workspace.methodInfo("workspace/Subject", "added", "()V").unwrap_err().code);

    ClassMutation addMarker;
    addMarker.classUri             = "workspace/Subject.class";
    addMarker.kind                 = ClassMutationKind::AddMarkerAnnotation;
    addMarker.annotationTarget     = AnnotationTarget::Field;
    addMarker.annotationVisible    = true;
    addMarker.annotationDescriptor = "Lworkspace/Marker;";
    addMarker.member               = {"counter", "I"};
    auto markerAdded = workspace.applyClassMutation(addMarker, expected(4));
    ASSERT_TRUE(markerAdded.status.is_ok());
    EXPECT_EQ(5U, markerAdded.revision);
    auto annotatedField = workspace.fieldInfo("workspace/Subject", "counter", "I");
    ASSERT_TRUE(annotatedField.is_ok());
    ASSERT_EQ(1U, annotatedField.unwrap().annotations.size());
    EXPECT_EQ("Lworkspace/Marker;", annotatedField.unwrap().annotations[0].descriptor);

    // 负例改写（类型面）：旧栈树序号 0 命中 label 节点被拒；mjbase 代码指令
    // 序号 0 是未引用 ldc、移除会成功——拒收面改断言序号越界（同一错误通道）。
    ClassMutation invalidDelete;
    invalidDelete.classUri         = "workspace/Subject.class";
    invalidDelete.kind             = ClassMutationKind::RemoveInstruction;
    invalidDelete.member           = {"value", "()I"};
    invalidDelete.instructionIndex = 99;
    auto rejected = workspace.applyClassMutation(invalidDelete, expected(5));
    EXPECT_FALSE(rejected.status.is_ok());
    EXPECT_EQ(WorkspaceError::InvalidArgument, rejected.status.code);
    EXPECT_EQ(5U, workspace.revision());
}

// ── 原始字节替换 / revert（test 11 直迁）────────────────────────────────

TEST(MjWorkspaceCoreTest, ReplacesListsAndRevertsRawClassBytesTransactionally)
{
    WorkspaceBase workspace;
    ASSERT_TRUE(workspace.openJar(writeWorkspaceFixture().string()).is_ok());
    const auto replacement = assemble("workspace/Subject", "42");

    auto preview = workspace.replaceClassBytes("workspace/Subject.class", replacement,
                                               expected(0, true));
    ASSERT_TRUE(preview.status.is_ok());
    EXPECT_FALSE(preview.committed);
    EXPECT_EQ(0U, workspace.revision());
    ASSERT_EQ(1U, preview.byteDiffs.size());
    ASSERT_TRUE(workspace.modifiedClasses().is_ok());
    EXPECT_TRUE(workspace.modifiedClasses().unwrap().empty());

    auto badMagic = replacement;
    badMagic[0]   = 0;
    EXPECT_EQ(WorkspaceError::InvalidArgument,
              workspace.replaceClassBytes("workspace/Subject.class", badMagic, expected(0))
                  .status.code);
    EXPECT_EQ(WorkspaceError::InvalidArgument,
              workspace.replaceClassBytes("workspace/Subject.class",
                                          assemble("workspace/Other", "1"), expected(0))
                  .status.code);

    auto applied = workspace.replaceClassBytes("workspace/Subject", replacement, expected(0));
    ASSERT_TRUE(applied.status.is_ok());
    EXPECT_TRUE(applied.committed);
    EXPECT_EQ(1U, workspace.revision());
    auto modified = workspace.modifiedClasses();
    ASSERT_TRUE(modified.is_ok());
    ASSERT_EQ(1U, modified.unwrap().size());
    EXPECT_EQ("workspace/Subject.class", modified.unwrap()[0].resourceUri);
    EXPECT_NE(modified.unwrap()[0].originalFingerprint, modified.unwrap()[0].currentFingerprint);

    auto revertPreview = workspace.revertClass("workspace/Subject.class", expected(1, true));
    ASSERT_TRUE(revertPreview.status.is_ok());
    EXPECT_FALSE(revertPreview.committed);
    EXPECT_EQ(1U, workspace.revision());
    EXPECT_TRUE(workspace.isDirty());

    auto reverted = workspace.revertClass("workspace/Subject.class", expected(1));
    ASSERT_TRUE(reverted.status.is_ok());
    EXPECT_TRUE(reverted.committed);
    EXPECT_EQ(2U, workspace.revision());
    EXPECT_FALSE(workspace.isDirty());
    ASSERT_TRUE(workspace.modifiedClasses().is_ok());
    EXPECT_TRUE(workspace.modifiedClasses().unwrap().empty());

    ASSERT_TRUE(workspace.undo(expected(2)).status.is_ok());
    EXPECT_TRUE(workspace.isDirty());
    ASSERT_TRUE(workspace.redo(expected(3)).status.is_ok());
    EXPECT_FALSE(workspace.isDirty());
}

// ── 指令族构建（test 12，等价改写：代码序号 + MH/Dynamic 拒收面）─────────

TEST(MjWorkspaceCoreTest, BuildsExecutableInstructionFamiliesFromSymbolicLabels)
{
    WorkspaceBase workspace;
    ASSERT_TRUE(workspace.openJar(writeWorkspaceFixture().string()).is_ok());
    ca::u64 revision = 0;

    ClassMutation field;
    field.classUri            = "workspace/Subject.class";
    field.kind                = ClassMutationKind::AddField;
    field.member              = {"answer", "I"};
    field.access              = kAccPublic | kAccStatic | kAccFinal;
    field.hasFieldValue       = true;
    field.fieldValue.kind     = ConstantKind::Integer;
    field.fieldValue.intValue = 42;
    auto fieldAdded = workspace.applyClassMutation(field, expected(revision));
    ASSERT_TRUE(fieldAdded.status.is_ok()) << fieldAdded.status.message;
    ++revision;

    ClassMutation genericField;
    genericField.classUri  = "workspace/Subject.class";
    genericField.kind      = ClassMutationKind::AddField;
    genericField.member    = {"names", "Ljava/util/List;"};
    genericField.access    = kAccPrivate;
    genericField.signature = "Ljava/util/List<Ljava/lang/String;>;";
    ASSERT_TRUE(workspace.applyClassMutation(genericField, expected(revision++)).status.is_ok());
    auto names = workspace.fieldInfo("workspace/Subject", "names", "Ljava/util/List;");
    ASSERT_TRUE(names.is_ok());
    EXPECT_EQ(genericField.signature, names.unwrap().signature);

    ClassMutation invalidAccess = field;
    invalidAccess.member = {"invalid", "I"};
    invalidAccess.access = kAccFinal | kAccVolatile;
    auto rejected = workspace.applyClassMutation(invalidAccess, expected(revision));
    EXPECT_EQ(WorkspaceError::InvalidArgument, rejected.status.code);
    EXPECT_EQ(revision, workspace.revision());

    const auto simple = [](ca::i32 opcode) {
        InstructionSpec value;
        value.kind   = InstructionKind::Simple;
        value.opcode = opcode;
        return value;
    };
    const auto var = [](ca::i32 opcode, ca::i32 slot) {
        InstructionSpec value;
        value.kind    = InstructionKind::Var;
        value.opcode  = opcode;
        value.operand = slot;
        return value;
    };
    const auto label = [](const std::string& id) {
        InstructionSpec value;
        value.kind  = InstructionKind::Label;
        value.label = id;
        return value;
    };
    const auto ldcInt = [](ca::i32 number) {
        InstructionSpec value;
        value.kind    = InstructionKind::LdcInt;
        value.operand = number;
        return value;
    };
    const auto addMethod = [&](ClassMutation mutation) {
        mutation.classUri = "workspace/Subject.class";
        mutation.kind     = ClassMutationKind::AddMethod;
        mutation.access   = kAccPublic | kAccStatic;
        auto result = workspace.applyClassMutation(mutation, expected(revision));
        if (result.status.is_ok()) ++revision;
        return result;
    };

    ClassMutation branch;
    branch.member     = {"branch", "(I)I"};
    branch.signature  = "(I)I";
    branch.exceptions = {"java/lang/RuntimeException"};
    InstructionSpec jump;
    jump.kind   = InstructionKind::Jump;
    jump.opcode = 0x99 /* IFEQ */;
    jump.label  = "zero";
    branch.initialInstructions = {label("start"), var(0x15 /* ILOAD */, 0), jump, ldcInt(1),
                                  simple(0xac /* IRETURN */), label("zero"), ldcInt(0),
                                  simple(0xac), label("end"), label("handler"),
                                  simple(0x57 /* POP */), ldcInt(-1), simple(0xac)};
    TryCatchSpec catchBlock;
    catchBlock.type          = "java/lang/RuntimeException";
    catchBlock.startLabel    = "start";
    catchBlock.endLabel      = "end";
    catchBlock.handlerLabel  = "handler";
    branch.tryCatchBlocks.push_back(catchBlock);
    branch.localVariables.push_back({"value", "I", {}, "start", "end", 0});
    const auto branchResult = addMethod(branch);
    ASSERT_TRUE(branchResult.status.is_ok()) << branchResult.status.message;

    // 代码指令序号口径：branch 体序号 = [iload 0, ifeq, ldc 1, ireturn, ldc 0,
    // ireturn, pop, ldc -1, ireturn]——ifeq 为 1（旧栈树序号 2），ldc 0 为 4
    //（旧栈树序号 5 的 "zero" 绑定点）。
    ClassMutation replaceJump;
    replaceJump.classUri          = "workspace/Subject.class";
    replaceJump.kind              = ClassMutationKind::ReplaceInstruction;
    replaceJump.member            = branch.member;
    replaceJump.instructionIndex  = 1;
    replaceJump.existingLabels.push_back({"zero", 4});
    replaceJump.instruction.kind  = InstructionKind::Jump;
    replaceJump.instruction.opcode = 0x9a /* IFNE */;
    replaceJump.instruction.label = "zero";
    auto jumpChanged = workspace.applyClassMutation(replaceJump, expected(revision));
    ASSERT_TRUE(jumpChanged.status.is_ok()) << jumpChanged.status.message;
    ++revision;

    ClassMutation table;
    table.member = {"table", "(I)I"};
    InstructionSpec tableSwitch;
    tableSwitch.kind         = InstructionKind::TableSwitch;
    tableSwitch.minimum      = 0;
    tableSwitch.maximum      = 1;
    tableSwitch.defaultLabel = "default";
    tableSwitch.labels       = {"case0", "case1"};
    table.initialInstructions = {var(0x15, 0), tableSwitch, label("case0"), ldcInt(10),
                                 simple(0xac), label("case1"), ldcInt(11), simple(0xac),
                                 label("default"), ldcInt(-1), simple(0xac)};
    ASSERT_TRUE(addMethod(table).status.is_ok());

    ClassMutation lookup;
    lookup.member = {"lookup", "(I)I"};
    InstructionSpec lookupSwitch;
    lookupSwitch.kind         = InstructionKind::LookupSwitch;
    lookupSwitch.defaultLabel = "default";
    lookupSwitch.keys         = {7, 42};
    lookupSwitch.labels       = {"seven", "fortyTwo"};
    lookup.initialInstructions = {var(0x15, 0), lookupSwitch, label("seven"), ldcInt(7),
                                  simple(0xac), label("fortyTwo"), ldcInt(42), simple(0xac),
                                  label("default"), ldcInt(-1), simple(0xac)};
    ASSERT_TRUE(addMethod(lookup).status.is_ok());

    ClassMutation multiArray;
    multiArray.member = {"matrix", "()[[I"};
    InstructionSpec array;
    array.kind       = InstructionKind::MultiANewArray;
    array.descriptor = "[[I";
    array.dimensions = 2;
    multiArray.initialInstructions = {ldcInt(2), ldcInt(3), array, simple(0xb0 /* ARETURN */)};
    ASSERT_TRUE(addMethod(multiArray).status.is_ok());

    // 宽常量族：LdcLong/LdcFloat/LdcDouble/LdcType（mjbase InstructionSpec 契约：
    // LdcType 收 JVM 内部名）。LdcHandle/LdcDynamic 家族不进成功面——libjbc
    // 常量池无 MethodHandle/Dynamic 追加 API（主线裁决点，见文件头注）。
    ClassMutation constants;
    constants.member = {"constants", "()V"};
    InstructionSpec wideLong;
    wideLong.kind       = InstructionKind::LdcLong;
    wideLong.longValue  = 0x102030405060708LL;
    InstructionSpec wideFloat;
    wideFloat.kind       = InstructionKind::LdcFloat;
    wideFloat.floatValue = 1.25F;
    InstructionSpec wideDouble;
    wideDouble.kind       = InstructionKind::LdcDouble;
    wideDouble.doubleValue = 2.5;
    InstructionSpec type;
    type.kind       = InstructionKind::LdcType;
    type.descriptor = "java/lang/String";
    constants.initialInstructions = {wideLong, simple(0x58 /* POP2 */), wideFloat,
                                     simple(0x57 /* POP */), wideDouble, simple(0x58), type,
                                     simple(0x57), simple(0xb1 /* RETURN */)};
    ASSERT_TRUE(addMethod(constants).status.is_ok());

    // indy 拒收钉子：旧栈 AddMethod(InvokeDynamic) 成功；mjbase 入口即拒
    //（InvalidArgument），产物不出半成品。
    ClassMutation indy;
    indy.member = {"concat", "(I)Ljava/lang/String;"};
    InstructionSpec invokeDynamic;
    invokeDynamic.kind       = InstructionKind::InvokeDynamic;
    invokeDynamic.name       = "makeConcatWithConstants";
    invokeDynamic.descriptor = "(I)Ljava/lang/String;";
    invokeDynamic.bootstrapMethod.tag       = 6 /* H_INVOKESTATIC */;
    invokeDynamic.bootstrapMethod.owner     = "java/lang/invoke/StringConcatFactory";
    invokeDynamic.bootstrapMethod.name      = "makeConcatWithConstants";
    invokeDynamic.bootstrapMethod.descriptor =
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
        "MethodType;Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/invoke/CallSite;";
    ConstantSpec recipe;
    recipe.kind        = ConstantKind::String;
    recipe.stringValue = "value=\1";
    invokeDynamic.bootstrapArguments.push_back(recipe);
    indy.initialInstructions = {var(0x15, 0), invokeDynamic, simple(0xb0)};
    auto indyRejected = addMethod(indy);
    EXPECT_FALSE(indyRejected.status.is_ok());
    EXPECT_EQ(WorkspaceError::InvalidArgument, indyRejected.status.code);

    ClassMutation replacement = branch;
    replacement.classUri      = "workspace/Subject.class";
    replacement.kind          = ClassMutationKind::ReplaceMethodBody;
    replacement.member        = {"value", "()I"};
    replacement.signature.clear();
    replacement.exceptions.clear();
    replacement.tryCatchBlocks.clear();
    replacement.localVariables.clear();
    replacement.initialInstructions = {ldcInt(99), simple(0xac)};
    ASSERT_TRUE(workspace.applyClassMutation(replacement, expected(revision++)).status.is_ok());

    auto bytes = workspace.readClassBytes("workspace/Subject");
    ASSERT_TRUE(bytes.is_ok());
    jasm::JvmJasmService validator;
    EXPECT_TRUE(validator.validate(bytes.unwrap().bytes).isOk());
    auto constantsInfo = workspace.methodInfo("workspace/Subject", "constants", "()V");
    ASSERT_TRUE(constantsInfo.is_ok()) << constantsInfo.unwrap_err().message;
    EXPECT_EQ(revision, workspace.revision());
}

// ── diff + 修复事务（test 13 直迁）──────────────────────────────────────

TEST(MjWorkspaceCoreTest, DiffsAllResourcesAndRepairsSelectedJarDamageTransactionally)
{
    WorkspaceBase workspace;
    ASSERT_TRUE(workspace.openJar(writeWorkspaceFixture().string()).is_ok());

    model::ResourceContainer reference;
    reference.addResource("workspace/Subject.class",
                          std::make_unique<model::ClassResource>(
                              "workspace/Subject.class", assemble("workspace/Subject", "2")));
    reference.addResource("data.txt",
                          std::make_unique<model::UnknownResource>(
                              "data.txt",
                              std::vector<ca::u8>{'c', 'h', 'a', 'n', 'g', 'e', 'd', 'x', 'x'}));
    reference.addResource("reference-only.txt",
                          std::make_unique<model::UnknownResource>(
                              "reference-only.txt", std::vector<ca::u8>{'n', 'e', 'w'}));
    const auto referencePath = test::getTempOutPath("mjbase_workspace_diff_reference.jar");
    model::ResourceHelper::exportToJar(reference, referencePath.string());

    const auto diff = workspace.diffJar(referencePath.string());
    ASSERT_TRUE(diff.status.is_ok());
    EXPECT_GE(diff.workspaceOnly, 3U);
    EXPECT_GE(diff.referenceOnly, 1U);
    EXPECT_GE(diff.modified, 2U);
    EXPECT_EQ(diff.workspaceOnly + diff.referenceOnly + diff.modified, diff.byteDiffs.size());
    EXPECT_NE(std::find_if(diff.byteDiffs.begin(), diff.byteDiffs.end(),
                           [](const ByteDiff& item) {
                               return item.uri == "data.txt" && item.beforeSize == item.afterSize;
                           }),
              diff.byteDiffs.end());
    EXPECT_NE(std::find_if(diff.byteDiffs.begin(), diff.byteDiffs.end(),
                            [](const ByteDiff& item) {
                                return item.uri == "reference-only.txt" && item.beforeSize == 0;
                            }),
              diff.byteDiffs.end());

    WorkspaceBase repairWorkspace;
    ASSERT_TRUE(repairWorkspace.openJar(writeRepairFixture().string()).is_ok());
    const auto inspected = repairWorkspace.analyzeJarRepair({}, expected(0, true));
    ASSERT_TRUE(inspected.mutation.status.is_ok());
    EXPECT_GE(inspected.resourceCount, 3U);
    EXPECT_EQ(2U, inspected.classCount);
    ASSERT_EQ(3U, inspected.issues.size());
    EXPECT_FALSE(inspected.issues[0].selectedForRemoval);
    EXPECT_TRUE(inspected.mutation.byteDiffs.empty());

    JarRepairRequest incompleteRepair;
    incompleteRepair.removeCorruptOutputMarkers = true;
    const auto rejectedRepair =
        repairWorkspace.applyJarRepair(incompleteRepair, expected(0));
    EXPECT_EQ(WorkspaceError::InvalidArgument, rejectedRepair.mutation.status.code);
    EXPECT_FALSE(rejectedRepair.mutation.committed);
    EXPECT_EQ(0U, repairWorkspace.revision());
    EXPECT_TRUE(repairWorkspace.readResource("logs/corrupt_output_stream.txt").is_ok());

    JarRepairRequest repair;
    repair.removeInvalidClassEntries  = true;
    repair.removeCorruptOutputMarkers = true;
    const auto preview = repairWorkspace.analyzeJarRepair(repair, expected(0, true));
    ASSERT_TRUE(preview.mutation.status.is_ok());
    EXPECT_FALSE(preview.mutation.committed);
    ASSERT_EQ(3U, preview.mutation.byteDiffs.size());
    EXPECT_TRUE(std::all_of(preview.issues.begin(), preview.issues.end(),
                            [](const JarRepairIssue& issue) {
                                return issue.selectedForRemoval;
                            }));

    const auto applied = repairWorkspace.applyJarRepair(repair, expected(0));
    ASSERT_TRUE(applied.mutation.status.is_ok());
    EXPECT_TRUE(applied.mutation.committed);
    EXPECT_EQ(1U, applied.mutation.revision);
    auto resources = repairWorkspace.listResources();
    ASSERT_TRUE(resources.is_ok());
    auto resourceList = std::move(resources).unwrap();
    EXPECT_EQ(std::find_if(resourceList.begin(), resourceList.end(),
                           [](const ResourceSummary& item) {
                               return item.uri == "broken/Invalid.class";
                           }),
              resourceList.end());
    EXPECT_TRUE(repairWorkspace.readResource("workspace/RepairSubject.class").is_ok());

    const auto restored = repairWorkspace.undo(expected(1));
    ASSERT_TRUE(restored.status.is_ok());
    EXPECT_EQ(2U, restored.revision);
    EXPECT_TRUE(repairWorkspace.readResource("broken/Invalid.class").is_ok());
}

TEST(MjWorkspaceCoreTest, RejectsInvalidJasmAndProtectsInputDuringAtomicExport)
{
    WorkspaceBase workspace;
    const auto    input = writeWorkspaceFixture();
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    auto sourceName = utf16("Subject.jasm");
    auto invalid    = utf16(classSource("workspace/Subject", "1\n      return"));

    auto rejected =
        workspace.applyJasm("workspace/Subject.class", sourceName.ref(), invalid.ref(), expected(0));
    EXPECT_EQ(WorkspaceError::InvalidArgument, rejected.status.code);
    EXPECT_EQ(0U, rejected.revision);
    EXPECT_FALSE(workspace.isDirty());
    ASSERT_FALSE(rejected.diagnostics.empty());

    EXPECT_EQ(WorkspaceError::PermissionDenied,
              workspace.exportJar(input.string()).unwrap_err().code);
    const auto output = test::getTempOutPath("mjbase_workspace_core_output.jar");
    std::error_code filesystemError;
    std::filesystem::remove(output, filesystemError);
    ASSERT_TRUE(workspace.exportJar(output.string()).is_ok());
    EXPECT_TRUE(std::filesystem::exists(output));

    auto reverted = workspace.revertClass("workspace/Subject.class", expected(0));
    ASSERT_TRUE(reverted.status.is_ok());
    EXPECT_FALSE(reverted.committed);
    EXPECT_EQ(0U, reverted.revision);
}

// ── 接口多态（test 14 直迁）─────────────────────────────────────────────

TEST(MjWorkspaceCoreTest, UsesIWorkspaceThroughInterfacePolymorphism)
{
    std::unique_ptr<IWorkspace> workspace = std::make_unique<WorkspaceBase>();
    ASSERT_TRUE(workspace->openJar(writeWorkspaceFixture().string()).is_ok());

    EXPECT_TRUE(workspace->isLoaded());
    EXPECT_EQ(ArtifactKind::Jar, workspace->artifactKind());

    auto summaries = workspace->listClassSummaries();
    ASSERT_TRUE(summaries.is_ok());
    // unwrap() 按值返回：先物化再取迭代器，避免两次 unwrap() 产生不同临时 vector
    // 导致跨容器迭代器比较（MSVC debug 断言，release 下为 UB）。
    const auto summaryList = summaries.unwrap();
    EXPECT_NE(std::find_if(summaryList.begin(), summaryList.end(),
                           [](const ClassSummary& s) {
                               return s.internalName == "workspace/Subject";
                           }),
              summaryList.end());

    auto classes = workspace->searchClasses("subject");
    ASSERT_TRUE(classes.is_ok());
    EXPECT_FALSE(classes.unwrap().empty());

    // 变更路径同样经接口：applyClassMutation 事务语义不变。
    ClassMutation mutation;
    mutation.classUri            = "workspace/Subject.class";
    mutation.kind                = ClassMutationKind::AddField;
    mutation.member.name         = "extra";
    mutation.member.descriptor   = "I";
    mutation.access              = kAccPublic;
    auto applied = workspace->applyClassMutation(mutation);
    ASSERT_TRUE(applied.status.is_ok());
    EXPECT_TRUE(applied.committed);
    EXPECT_EQ(1U, applied.revision);

    auto classes2 = workspace->listClassSummaries();
    ASSERT_TRUE(classes2.is_ok());
    // 同上：物化一次，避免 find_if 返回的迭代器指向已析构的临时 vector。
    const auto classList = classes2.unwrap();
    auto       subject   = std::find_if(classList.begin(), classList.end(),
                                  [](const ClassSummary& s) {
                                      return s.internalName == "workspace/Subject";
                                  });
    ASSERT_NE(subject, classList.end());
    EXPECT_EQ(2U, subject->fieldCount);   // 原 counter + 新增 extra
}

// ── Local 直通（旧 RoutesLocalRenameThroughClassMutationDirectly 等价改写）──

TEST(MjWorkspaceCoreTest, RenamesLocalsThroughClassMutationTransaction)
{
    WorkspaceBase workspace;
    ASSERT_TRUE(workspace.openJar(writeLocalHolderFixture().string()).is_ok());

    // rename 编排面（analyzeRename/applyRename）未随迁；Local 能力由
    // ClassMutationKind::RenameLocal 承载，断言 LVT 记录名 held→kept、
    // index/descriptor 不变（class_mutation_service 直改 LVT/LVTT 行）。
    ClassMutation rename;
    rename.classUri               = "workspace/LocalHolder.class";
    rename.kind                   = ClassMutationKind::RenameLocal;
    rename.member                 = {"hold", "()I"};
    rename.localName              = "held";
    rename.replacementLocalName   = "kept";
    auto applied = workspace.applyClassMutation(rename, expected(0));
    ASSERT_TRUE(applied.status.is_ok()) << applied.status.message;
    EXPECT_TRUE(applied.committed);
    EXPECT_EQ(1U, applied.revision);

    auto bytes = workspace.readClassBytes("workspace/LocalHolder");
    ASSERT_TRUE(bytes.is_ok());
    const auto holder = std::move(bytes).unwrap();   // 物化一次，避免 unwrap() 临时悬挂
    auto read = jbc::core::readClass(jbc::core::ByteView(holder.bytes));
    ASSERT_TRUE(read.is_ok());
    auto                  parsed = std::move(read).unwrap();
    jbc::core::ClassFile& cf     = *parsed.file;
    jbc::core::MemberInfo* hold  = nullptr;
    for (auto& method : cf.methods) {
        if (support::utf8Text(cf, method.nameCp) == "hold" &&
            support::utf8Text(cf, method.descCp) == "()I") {
            hold = &method;
            break;
        }
    }
    ASSERT_TRUE(hold != nullptr);
    jbc::core::CodeAttr* code = support::ensureCode(cf, *hold);
    ASSERT_TRUE(code != nullptr);
    const jbc::core::LocalVariableTableAttr* lvt = nullptr;
    for (jbc::core::AttrSlot& nested : code->attrs) {
        if (!cf.cp.utf8Equals(nested.nameCp, "LocalVariableTable")) continue;
        if (!nested.decoded() && cf.decodeAttr(nested).is_err()) continue;
        lvt = ca::core::tag_cast::cast<const jbc::core::LocalVariableTableAttr>(nested.peek());
        break;
    }
    ASSERT_TRUE(lvt != nullptr);
    ASSERT_EQ(1U, lvt->rows.size());
    EXPECT_EQ("kept", support::utf8Text(cf, lvt->rows[0].nameCp));
    EXPECT_EQ("I", support::utf8Text(cf, lvt->rows[0].descCp));
    EXPECT_EQ(0U, lvt->rows[0].slot);

    // 负例（今日事实行为）：owner 传不存在的方法名 → NOT_FOUND、revision 不动。
    ClassMutation missing;
    missing.classUri             = "workspace/LocalHolder.class";
    missing.kind                 = ClassMutationKind::RenameLocal;
    missing.member               = {"nope", "()I"};
    missing.localName            = "held";
    missing.replacementLocalName = "whatever";
    auto notFound = workspace.applyClassMutation(missing, expected(1));
    EXPECT_EQ(WorkspaceError::NotFound, notFound.status.code);
    EXPECT_EQ(1U, workspace.revision());
}

// ── 覆盖组件报告（旧 ReportsOverrideComponentWithPrimaryAndSupportingDecoration
// 直迁）：组件游走在新栈已等价重建（mjbase 层次图并查集闭包，不经 renamer），
// 断言逐字保留：owner 字典序、primary 只落请求点名的声明、支撑库成员带
// supporting/supportId 装饰、owner 未登记与 owner 在但方法未声明双 NOT_FOUND。
TEST(MjWorkspaceCoreTest, ReportsOverrideComponentWithPrimaryAndSupportingDecoration)
{
    WorkspaceBase workspace;
    ASSERT_TRUE(workspace.openJar(writeWorkspaceFixture().string()).is_ok());
    ASSERT_TRUE(workspace.addSupportingJar(writeSupportingFixture().string()).is_ok());

    auto overrides = workspace.overridesOfMethod("workspace/Subject", "virtualValue", "()I");
    ASSERT_TRUE(overrides.is_ok());
    const auto members = std::move(overrides).unwrap();
    ASSERT_EQ(3U, members.size());
    EXPECT_EQ("support/Library", members[0].owner);
    EXPECT_FALSE(members[0].primary);
    EXPECT_TRUE(members[0].supporting);
    EXPECT_EQ("support-1", members[0].supportId);
    EXPECT_EQ("workspace/Child", members[1].owner);
    EXPECT_FALSE(members[1].primary);
    EXPECT_FALSE(members[1].supporting);
    EXPECT_TRUE(members[1].supportId.empty());
    EXPECT_EQ("workspace/Subject", members[2].owner);
    EXPECT_TRUE(members[2].primary);
    EXPECT_FALSE(members[2].supporting);
    for (const auto& member : members) {
        EXPECT_EQ("virtualValue", member.name);
        EXPECT_EQ("()I", member.descriptor);
    }

    // owner 不在工作区/支撑库 → NOT_FOUND；owner 在但方法未声明 → NOT_FOUND。
    auto missingOwner = workspace.overridesOfMethod("missing/Type", "virtualValue", "()I");
    ASSERT_TRUE(missingOwner.is_err());
    EXPECT_EQ(WorkspaceError::NotFound, missingOwner.unwrap_err().code);
    auto missingMethod = workspace.overridesOfMethod("workspace/Subject", "nope", "()I");
    ASSERT_TRUE(missingMethod.is_err());
    EXPECT_EQ(WorkspaceError::NotFound, missingMethod.unwrap_err().code);
}

// ── summary 扫描单后端字段契约（旧 JbcSummaryScanTest 三例单后端化）──────
//
// 旧栈是 legacy（jbytecode SKIP）× jbc 双后端字段级对拍；mjbase 只有 libjbc
// 一个底座，对拍面收敛为单后端字段契约：对同一 (uri, bytes) 断言 internalName/
// superName/access/version/interfaceCount/fieldCount/methodCount（resourceUri
// 原样回填），强度与双后端逐字段相等断言一致。

namespace {

std::string plainSummarySource(const std::string& name)
{
    return ".super java/lang/Object\n"
           ".class public super " +
           name +
           " {\n"
           "  .field public counter I\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      bipush 7\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string richSummarySource(const std::string& name)
{
    // 注：不声明 <init>——jhasm 方法名不支持尖括号（现有夹具亦无构造器声明），
    // 契约关注接口/字段/方法计数，构造器不在覆盖面内。
    return ".super java/lang/Object\n"
           ".implements java/lang/Runnable\n"
           ".implements java/io/Serializable\n"
           ".class public super " +
           name +
           " {\n"
           "  .field private id J\n"
           "  .field public label Ljava/lang/String;\n"
           "  .method public run ()V {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "  .method public static compute (II)I {\n"
           "    code: {\n"
           "      iload 0\n"
           "      iload 1\n"
           "      iadd\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

}   // namespace

TEST(MjWorkspaceSummaryTest, FieldContractOnPlainClass)
{
    const auto bytes = assembleSource("parity/Plain", plainSummarySource("parity/Plain"));
    model::ClassResource resource("parity/Plain.class", bytes);
    auto decoded = decodeClassSummary("parity/Plain.class", resource);
    ASSERT_TRUE(decoded.is_ok());
    const ClassSummary summary = std::move(decoded).unwrap();
    EXPECT_EQ("parity/Plain.class", summary.resourceUri);
    EXPECT_EQ("parity/Plain", summary.internalName);
    EXPECT_EQ("java/lang/Object", summary.superName);
    EXPECT_EQ(kAccPublic | kAccSuper, static_cast<ca::u32>(summary.access));
    EXPECT_EQ(0U, summary.interfaceCount);
    EXPECT_EQ(1U, summary.fieldCount);
    EXPECT_EQ(1U, summary.methodCount);
}

TEST(MjWorkspaceSummaryTest, FieldContractOnRichClass)
{
    const auto bytes = assembleSource("parity/Rich", richSummarySource("parity/Rich"));
    model::ClassResource resource("parity/Rich.class", bytes);
    auto decoded = decodeClassSummary("parity/Rich.class", resource);
    ASSERT_TRUE(decoded.is_ok());
    const ClassSummary summary = std::move(decoded).unwrap();
    EXPECT_EQ("parity/Rich.class", summary.resourceUri);
    EXPECT_EQ("parity/Rich", summary.internalName);
    EXPECT_EQ("java/lang/Object", summary.superName);
    EXPECT_EQ(static_cast<ca::u32>(kAccPublic | kAccSuper),
              static_cast<ca::u32>(summary.access) & (kAccPublic | kAccSuper | kAccInterface));
    EXPECT_EQ(2U, summary.interfaceCount);
    EXPECT_EQ(2U, summary.fieldCount);
    EXPECT_EQ(2U, summary.methodCount);
}

TEST(MjWorkspaceSummaryTest, RejectsGarbage)
{
    const std::vector<ca::u8> garbage{0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0xFF, 0xFF, 0x00};
    model::ClassResource      resource("parity/Garbage.class", garbage);
    auto decoded = decodeClassSummary("parity/Garbage.class", resource);
    ASSERT_TRUE(decoded.is_err());
    EXPECT_EQ(WorkspaceError::DataLoss, decoded.unwrap_err().code);
}

}   // namespace mjbase::workspace
