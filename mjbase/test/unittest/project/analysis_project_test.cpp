// 分析工程文件：空工程最小 schema + 实体全量
// roundtrip（文本/文件）+ schema 违约逐类拒载 + id 单调不复用 + 确定性
// 稳定输出 + 与 sidecar JSON 互斥（kind 标记自描述）。
//
// 对应、语义不变；仅命名空间（mjbase::project）与 gtest 套件名随 mjbase
// 约定调整。
#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "mjbase/project/analysis_project.hpp"
#include "test_helper.hpp"

using namespace mjbase::project;
using ca::str::Utf8String;
using ca::str::Utf8StringRef;

namespace {

Utf8StringRef S(const char* value)
{
    return Utf8StringRef::from_cstr(value);
}

Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

AnnotationTarget classTarget(const char* owner)
{
    AnnotationTarget target;
    target.kind  = AnnotationTargetKind::Class;
    target.owner = Utf8String::from_cstr(owner);
    return target;
}

AnnotationTarget methodTarget(const char* owner, const char* name, const char* desc)
{
    AnnotationTarget target;
    target.kind  = AnnotationTargetKind::Method;
    target.owner = Utf8String::from_cstr(owner);
    target.name  = Utf8String::from_cstr(name);
    target.desc  = Utf8String::from_cstr(desc);
    return target;
}

AnnotationTarget instructionTarget(const char* owner, const char* name, const char* desc,
                                   ca::i64 index)
{
    AnnotationTarget target = methodTarget(owner, name, desc);
    target.kind             = AnnotationTargetKind::Instruction;
    target.index            = index;
    return target;
}

AnalysisInput makeInput(const char* path, const char* fingerprint, AnalysisInputRole role)
{
    AnalysisInput input;
    input.path        = Utf8String::from_cstr(path);
    input.fingerprint = Utf8String::from_cstr(fingerprint);
    input.role        = role;
    return input;
}

/// 刻意乱序登记（输出要求 inputs 按路径字典序、annotations 按 id 稳定排列）。
AnalysisProject buildSample()
{
    auto project = AnalysisProject::createEmpty();
    project.addInput(makeInput("samples/z-dep.jar", "sha256:dd", AnalysisInputRole::Support));
    project.addInput(makeInput("samples/a-app.jar", "sha256:aa", AnalysisInputRole::Primary));

    ShadowSidecarRef sidecar;
    sidecar.path        = Utf8String::from_cstr("samples/a-app.jar.mjt-shadow");
    sidecar.fingerprint = Utf8String::from_cstr("sha256:cc");
    project.setShadowSidecar(std::move(sidecar));

    EXPECT_EQ(1U, project.addAnnotation(methodTarget("a/A", "run", "(I)V"),
                                        S("entry point"), S("b-machine"), S("2026-10-02T00:00:00Z")));
    EXPECT_EQ(2U, project.addAnnotation(classTarget("z/Z"), S("wrapper class"),
                                        S("b-machine"), S("2026-10-02T00:00:01Z")));
    EXPECT_EQ(3U, project.addAnnotation(instructionTarget("a/A", "run", "(I)V", 4),
                                        S("opaque predicate"), S("a-machine"),
                                        S("2026-10-02T00:00:02Z")));
    return project;
}

void expectSampleCore(const AnalysisProject& project)
{
    ASSERT_EQ(2U, project.inputCount());
    const auto* primary = project.findInput(S("samples/a-app.jar"));
    ASSERT_NE(nullptr, primary);
    EXPECT_EQ("sha256:aa", primary->fingerprint.to_std_string());
    EXPECT_EQ(AnalysisInputRole::Primary, primary->role);
    const auto* support = project.findInput(S("samples/z-dep.jar"));
    ASSERT_NE(nullptr, support);
    EXPECT_EQ(AnalysisInputRole::Support, support->role);

    ASSERT_NE(nullptr, project.shadowSidecar());
    EXPECT_EQ("samples/a-app.jar.mjt-shadow", project.shadowSidecar()->path.to_std_string());
    EXPECT_EQ("sha256:cc", project.shadowSidecar()->fingerprint.to_std_string());

    ASSERT_EQ(3U, project.annotationCount());
    const auto* run = project.findAnnotation(1);
    ASSERT_NE(nullptr, run);
    EXPECT_EQ(AnnotationTargetKind::Method, run->target.kind);
    EXPECT_EQ("a/A", run->target.owner.to_std_string());
    EXPECT_EQ("run", run->target.name.to_std_string());
    EXPECT_EQ("(I)V", run->target.desc.to_std_string());
    EXPECT_EQ(-1, run->target.index);
    EXPECT_EQ("entry point", run->text.to_std_string());

    const auto* wrapper = project.findAnnotation(2);
    ASSERT_NE(nullptr, wrapper);
    EXPECT_EQ(AnnotationTargetKind::Class, wrapper->target.kind);
    EXPECT_TRUE(wrapper->target.name.is_empty());
    EXPECT_EQ(-1, wrapper->target.index);

    const auto* insn = project.findAnnotation(3);
    ASSERT_NE(nullptr, insn);
    EXPECT_EQ(AnnotationTargetKind::Instruction, insn->target.kind);
    EXPECT_EQ(4, insn->target.index);
    EXPECT_EQ(4U, project.nextAnnotationId());
}

/// 错误载荷含 move-only Utf8String：取错误一律 std::move(x).unwrap_err()，
/// 材料化成局部 err 后再断言（Result 无 & 重载，左值 unwrap_err 会拷贝）。

}   // namespace

TEST(MjProjectAnalysisTest, emptyProjectWritesMinimalSchemaAndParsesBack)
{
    auto text = writeAnalysisProjectText(AnalysisProject::createEmpty());
    ASSERT_FALSE(text.is_err());
    const auto  body = std::move(text).unwrap().to_std_string();
    EXPECT_NE(std::string::npos, body.find("\"version\": 1"));
    EXPECT_NE(std::string::npos, body.find("\"mjt-analysis-project\""));
    EXPECT_EQ(std::string::npos, body.find("shadow_sidecar"));

    auto parsed = parseAnalysisProjectText(
        Utf8StringRef::from_string_view(std::string_view(body.data(), body.size())));
    ASSERT_FALSE(parsed.is_err());
    EXPECT_TRUE(std::move(parsed).unwrap().empty());
}

TEST(MjProjectAnalysisTest, textRoundtripPreservesEntities)
{
    auto written = writeAnalysisProjectText(buildSample());
    ASSERT_FALSE(written.is_err());
    const auto  body = std::move(written).unwrap().to_std_string();
    auto parsed = parseAnalysisProjectText(
        Utf8StringRef::from_string_view(std::string_view(body.data(), body.size())));
    ASSERT_FALSE(parsed.is_err());
    expectSampleCore(std::move(parsed).unwrap());
}

TEST(MjProjectAnalysisTest, fileRoundtripViaTempPath)
{
    const auto path = test::getTempOutPath("analysis_project_file_roundtrip.mjt-project");

    auto written = writeAnalysisProjectFile(refOf(path.string()), buildSample());
    ASSERT_FALSE(written.is_err());

    auto read = readAnalysisProjectFile(refOf(path.string()));
    ASSERT_FALSE(read.is_err());
    expectSampleCore(std::move(read).unwrap());
}

TEST(MjProjectAnalysisTest, strictParseRejectsSchemaViolations)
{
    const auto write = [](const AnalysisProject& project) {
        auto text = writeAnalysisProjectText(project);
        return text.is_err() ? std::string() : std::move(text).unwrap().to_std_string();
    };

    // 基准文件（合法），逐字段破坏。
    const std::string base = write(buildSample());
    ASSERT_FALSE(base.empty());

    struct Violation
    {
        const char* name;
        std::string before;
        std::string after;
    };
    const std::vector<Violation> violations = {
        // 锚点均为单 token 级、与 pretty 排版无关。
        {"unsupported version", "\"version\": 1", "\"version\": 2"},
        {"wrong kind", "\"mjt-analysis-project\"", "\"something-else\""},
        {"unknown top field", "\"inputs\"", "\"suspicious_extra\": 1, \"inputs\""},
        {"unknown role", "\"role\": \"primary\"", "\"role\": \"hub\""},
        {"class target with name", "\"owner\": \"z/Z\"",
         "\"owner\": \"z/Z\", \"name\": \"X\", \"desc\": \"I\""},
        {"method target missing desc", "\"desc\": \"(I)V\"", "\"desc\": \"\""},
        {"instruction missing index", "\"index\": 4", "\"index\": -1"},
        {"missing text", "\"text\": \"entry point\"", "\"text\": \"\""},
    };
    for (const auto& violation : violations) {
        const auto  at = base.find(violation.before);
        ASSERT_NE(std::string::npos, at) << violation.name << ": anchor missing";
        const auto  broken = base.substr(0, at) + violation.after + base.substr(at + violation.before.size());
        auto parsed = parseAnalysisProjectText(refOf(broken));
        ASSERT_TRUE(parsed.is_err()) << violation.name << ": accepted";
        const auto  err = std::move(parsed).unwrap_err();
        EXPECT_EQ(AnalysisProjectErrorKind::Schema, err.kind) << violation.name;
    }
}

TEST(MjProjectAnalysisTest, strictParseRejectsIdReuseAndDuplicates)
{
    // next_annotation_id 回退到会复用已占 id → 拒载。
    auto project = buildSample();
    auto text    = writeAnalysisProjectText(project);
    ASSERT_FALSE(text.is_err());
    auto  body = std::move(text).unwrap().to_std_string();
    const auto  at = body.find("\"next_annotation_id\": 4");
    ASSERT_NE(std::string::npos, at);
    body.replace(at, std::string("\"next_annotation_id\": 4").size(),
                 "\"next_annotation_id\": 3");
    auto parsed = parseAnalysisProjectText(refOf(body));
    ASSERT_TRUE(parsed.is_err());
    const auto  err = std::move(parsed).unwrap_err();
    EXPECT_EQ(AnalysisProjectErrorKind::Schema, err.kind);
    EXPECT_NE(std::string::npos, err.message.to_std_string().find("reuse"));

    // 注释 id 重复 → 拒载。
    const auto  idAt = body.find("\"id\": 2");
    ASSERT_NE(std::string::npos, idAt);
    auto duped = body;
    duped.replace(idAt, std::string("\"id\": 2").size(), "\"id\": 1");
    auto dupParsed = parseAnalysisProjectText(refOf(duped));
    ASSERT_TRUE(dupParsed.is_err());
    EXPECT_NE(std::string::npos,
              std::move(dupParsed).unwrap_err().message.to_std_string().find("duplicate annotation id"));
}

TEST(MjProjectAnalysisTest, jsonSyntaxErrorIsReportedAsJsonRead)
{
    auto parsed = parseAnalysisProjectText(S("{\n  \"version\": 1, oops}"));
    ASSERT_TRUE(parsed.is_err());
    EXPECT_EQ(AnalysisProjectErrorKind::JsonRead, std::move(parsed).unwrap_err().kind);
}

TEST(MjProjectAnalysisTest, missingFileFailsToOpen)
{
    auto read = readAnalysisProjectFile(S("no/such/analysis.mjt-project"));
    ASSERT_TRUE(read.is_err());
    EXPECT_EQ(AnalysisProjectErrorKind::JsonRead, std::move(read).unwrap_err().kind);
}

TEST(MjProjectAnalysisTest, addAnnotationIdsAreMonotonicAcrossDeleteAndReload)
{
    auto project = buildSample();
    ASSERT_EQ(3U, project.annotationCount());
    ASSERT_TRUE(project.removeAnnotation(3));
    ASSERT_FALSE(project.removeAnnotation(3));

    // id 3 已删也不复用：下一个是 4。
    ASSERT_EQ(4U, project.addAnnotation(methodTarget("a/A", "tick", "()V"), S("later"),
                                        S("b-machine"), S("2026-10-02T00:00:03Z")));

    auto written = writeAnalysisProjectText(project);
    ASSERT_FALSE(written.is_err());
    const auto  body = std::move(written).unwrap().to_std_string();
    auto parsed = parseAnalysisProjectText(
        Utf8StringRef::from_string_view(std::string_view(body.data(), body.size())));
    ASSERT_FALSE(parsed.is_err());
    auto reloaded = std::move(parsed).unwrap();
    ASSERT_EQ(3U, reloaded.annotationCount());
    EXPECT_EQ(5U, reloaded.nextAnnotationId());
    EXPECT_EQ(nullptr, reloaded.findAnnotation(3));
    ASSERT_NE(nullptr, reloaded.findAnnotation(4));
    EXPECT_EQ(5U, reloaded.addAnnotation(classTarget("w/W"), S("after reload"), S("b-machine"),
                                         S("2026-10-02T00:00:04Z")));
}

TEST(MjProjectAnalysisTest, addInputRejectsDuplicatePathAndRemoveRebuildsIndex)
{
    auto project = AnalysisProject::createEmpty();
    ASSERT_TRUE(project.addInput(makeInput("a.jar", "sha256:1", AnalysisInputRole::Primary)));
    ASSERT_FALSE(project.addInput(makeInput("a.jar", "sha256:2", AnalysisInputRole::Support)));
    ASSERT_EQ(1U, project.inputCount());

    ASSERT_EQ(1U, project.removeInput(S("a.jar")));
    ASSERT_EQ(0U, project.removeInput(S("a.jar")));
    EXPECT_EQ(nullptr, project.findInput(S("a.jar")));

    ASSERT_TRUE(project.addInput(makeInput("a.jar", "sha256:2", AnalysisInputRole::Support)));
    const auto* readded = project.findInput(S("a.jar"));
    ASSERT_NE(nullptr, readded);
    EXPECT_EQ("sha256:2", readded->fingerprint.to_std_string());
    EXPECT_EQ(AnalysisInputRole::Support, readded->role);
}

TEST(MjProjectAnalysisTest, annotationsOfFiltersByOwnerSortedById)
{
    auto project = buildSample();
    ASSERT_EQ(4U, project.addAnnotation(classTarget("b/B"), S("b note"), S("b-machine"),
                                        S("2026-10-02T00:00:05Z")));
    const auto  ofA = project.annotationsOf(S("a/A"));
    ASSERT_EQ(2U, ofA.size());
    EXPECT_EQ(1U, ofA[0]->id);
    EXPECT_EQ(3U, ofA[1]->id);
    EXPECT_TRUE(project.annotationsOf(S("no/such")).empty());
}

TEST(MjProjectAnalysisTest, invalidTargetsAndBodiesAreRejectedWithoutIdAllocation)
{
    auto project = AnalysisProject::createEmpty();

    AnnotationTarget noOwner              = classTarget("");
    AnnotationTarget classWithName        = classTarget("a/A");
    classWithName.name                    = Utf8String::from_cstr("X");
    AnnotationTarget methodWithoutDesc    = methodTarget("a/A", "run", "");
    AnnotationTarget methodWithIndex      = methodTarget("a/A", "run", "(I)V");
    methodWithIndex.index                 = 2;
    AnnotationTarget instructionNoIndex   = methodTarget("a/A", "run", "(I)V");
    instructionNoIndex.kind               = AnnotationTargetKind::Instruction;

    EXPECT_EQ(0U, project.addAnnotation(std::move(noOwner), S("t"), S("a"), S("2026")));
    EXPECT_EQ(0U, project.addAnnotation(std::move(classWithName), S("t"), S("a"), S("2026")));
    EXPECT_EQ(0U, project.addAnnotation(std::move(methodWithoutDesc), S("t"), S("a"), S("2026")));
    EXPECT_EQ(0U, project.addAnnotation(std::move(methodWithIndex), S("t"), S("a"), S("2026")));
    EXPECT_EQ(0U, project.addAnnotation(std::move(instructionNoIndex), S("t"), S("a"), S("2026")));
    // 正文/作者/时间任一为空都拒绝。
    EXPECT_EQ(0U, project.addAnnotation(classTarget("a/A"), S(""), S("a"), S("2026")));
    EXPECT_EQ(0U, project.addAnnotation(classTarget("a/A"), S("t"), S(""), S("2026")));
    EXPECT_EQ(0U, project.addAnnotation(classTarget("a/A"), S("t"), S("a"), S("")));
    // 拒绝不烧 id。
    EXPECT_EQ(1U, project.addAnnotation(classTarget("a/A"), S("ok"), S("a"), S("2026")));
    EXPECT_EQ(1U, project.annotationCount());
}

TEST(MjProjectAnalysisTest, outputIsDeterministicAcrossInsertionOrder)
{
    // 同一构建函数两次 → 逐字节一致（unordered_map 迭代序不得泄漏进输出）。
    auto first  = writeAnalysisProjectText(buildSample());
    auto second = writeAnalysisProjectText(buildSample());
    ASSERT_FALSE(first.is_err());
    ASSERT_FALSE(second.is_err());
    EXPECT_EQ(std::move(first).unwrap().to_std_string(),
              std::move(second).unwrap().to_std_string());

    // inputs 插入序不同 → 输出仍一致（annotations/sidecar 为空口径）。
    auto forward = AnalysisProject::createEmpty();
    forward.addInput(makeInput("a.jar", "sha256:1", AnalysisInputRole::Primary));
    forward.addInput(makeInput("b.jar", "sha256:2", AnalysisInputRole::Support));
    auto backward = AnalysisProject::createEmpty();
    backward.addInput(makeInput("b.jar", "sha256:2", AnalysisInputRole::Support));
    backward.addInput(makeInput("a.jar", "sha256:1", AnalysisInputRole::Primary));
    auto forwardText  = writeAnalysisProjectText(forward);
    auto backwardText = writeAnalysisProjectText(backward);
    ASSERT_FALSE(forwardText.is_err());
    ASSERT_FALSE(backwardText.is_err());
    EXPECT_EQ(std::move(forwardText).unwrap().to_std_string(),
              std::move(backwardText).unwrap().to_std_string());
}

TEST(MjProjectAnalysisTest, sidecarFingerprintIsOptionalAndRoundtripsAsAbsent)
{
    auto project = AnalysisProject::createEmpty();
    ShadowSidecarRef sidecar;
    sidecar.path = Utf8String::from_cstr("x.jar.mjt-shadow");
    project.setShadowSidecar(std::move(sidecar));

    auto written = writeAnalysisProjectText(project);
    ASSERT_FALSE(written.is_err());
    const auto  body = std::move(written).unwrap().to_std_string();
    EXPECT_NE(std::string::npos, body.find("shadow_sidecar"));
    EXPECT_EQ(std::string::npos, body.find("\"fingerprint\""));

    auto parsed = parseAnalysisProjectText(
        Utf8StringRef::from_string_view(std::string_view(body.data(), body.size())));
    ASSERT_FALSE(parsed.is_err());
    // 指针指向 optional 内部：先材料化工程对象再取指针（临时上取即悬垂）。
    auto reloadedProject = std::move(parsed).unwrap();
    const auto* reloaded = reloadedProject.shadowSidecar();
    ASSERT_NE(nullptr, reloaded);
    EXPECT_EQ("x.jar.mjt-shadow", reloaded->path.to_std_string());
    EXPECT_TRUE(reloaded->fingerprint.is_empty());
}

TEST(MjProjectAnalysisTest, shadowSidecarTextIsRejectedAsProjectFile)
{
    // kind 标记自描述：sidecar JSON 喂给工程解析器必须被拒（字段集不相交）。
    const auto* sidecarText =
        "{\n  \"version\": 1,\n  \"aliases\": [{\"kind\": \"class\", \"name\": \"A\", "
        "\"shadow\": \"Class_1\"}]\n}\n";
    auto parsed = parseAnalysisProjectText(S(sidecarText));
    ASSERT_TRUE(parsed.is_err());
    const auto  err = std::move(parsed).unwrap_err();
    EXPECT_EQ(AnalysisProjectErrorKind::Schema, err.kind);
    EXPECT_NE(std::string::npos, err.message.to_std_string().find("unknown analysis project field"));
}
