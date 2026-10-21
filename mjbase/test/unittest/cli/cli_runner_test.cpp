#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mjbase/cli/cli_runner.hpp"
#include "jbc/core/reader.hpp"
#include "libca/fs/file_util.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

using namespace mjbase::cli;
using ca::fs::FileUtil;

namespace model = mjbase::model;

namespace {

// ── 最小可解析 classfile 手工构造 ────────────────────────────────
// 形态：public class <name> extends Object，含一个 public static int
// t() { return 1; }，无 StackMapTable。字节按骨架直拼（单槽 CP 条目，
// 无 long/double）。
void putU2(std::vector<ca::u8>& out, ca::u16 v)
{
    out.push_back(static_cast<ca::u8>(v >> 8));
    out.push_back(static_cast<ca::u8>(v & 0xFF));
}

void putU4(std::vector<ca::u8>& out, ca::u32 v)
{
    putU2(out, static_cast<ca::u16>(v >> 16));
    putU2(out, static_cast<ca::u16>(v & 0xFFFF));
}

std::vector<ca::u8> buildMinimalClass(std::string_view className = "T")
{
    std::vector<ca::u8> cp;
    auto utf8 = [&cp](std::string_view text) {
        cp.push_back(0x01);   // CP tag：Utf8（单字节）
        putU2(cp, static_cast<ca::u16>(text.size()));
        cp.insert(cp.end(), text.begin(), text.end());
    };
    auto classRef = [&cp](ca::u16 nameIndex) {
        cp.push_back(0x07);   // CP tag：Class（单字节）
        putU2(cp, nameIndex);
    };
    // CP #1 Utf8 <className>，#2 Class #1，#3 Utf8 Object，#4 Class #3，
    // #5 Utf8 "t"，#6 Utf8 "()I"，#7 Utf8 "Code"（全部单槽条目，索引连续）。
    utf8(className);
    classRef(1);
    utf8("java/lang/Object");
    classRef(3);
    utf8("t");
    utf8("()I");
    utf8("Code");

    std::vector<ca::u8> out;
    putU4(out, 0xCAFEBABEu);   // magic
    putU2(out, 0);             // minor
    putU2(out, 52);            // major: V1_8
    putU2(out, 8);             // cp_count（#0 不计条目）
    out.insert(out.end(), cp.begin(), cp.end());
    putU2(out, 0x0021);        // ACC_PUBLIC | ACC_SUPER
    putU2(out, 2);             // this #2
    putU2(out, 4);             // super #4
    putU2(out, 0);             // interfaces_count
    putU2(out, 0);             // fields_count
    putU2(out, 1);             // methods_count
    putU2(out, 0x0009);        // ACC_PUBLIC | ACC_STATIC
    putU2(out, 5);             // name #5 "t"
    putU2(out, 6);             // descriptor #6 "()I"
    putU2(out, 1);             // method attributes_count
    putU2(out, 7);             // attribute name #7 "Code"
    putU4(out, 14);            // attribute length
    putU2(out, 1);             // max_stack
    putU2(out, 0);             // max_locals
    putU4(out, 2);             // code_length
    out.push_back(0x04);       // iconst_1
    out.push_back(0xAC);       // ireturn
    putU2(out, 0);             // exception_table_length
    putU2(out, 0);             // code attributes_count
    putU2(out, 0);             // class attributes_count
    return out;
}

// RAII 临时文件
class TempFile
{
public:
    explicit TempFile(std::string suffix)
        : path_(std::string(std::tmpnam(nullptr)) + suffix)
    {}
    ~TempFile() { std::remove(path_.c_str()); }
    const std::string& path() const { return path_; }
    bool               exists() const { return FileUtil::exists(path_); }

private:
    std::string path_;
};

}   // namespace

// ── 空参数：返回 ok=false 且 processed=0 ──
TEST(MjCliRunnerTest, emptyOptionsFailsCleanly)
{
    CliOptions   opts;
    CliRunReport report =
        runCli(opts, [](std::string_view, std::vector<ca::u8>&) { return ClassProcessResult{}; });
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.processed, 0);
    EXPECT_EQ(report.failure, CliFailureKind::InvalidArguments);
}

// ── 不支持的输入类型 ──
TEST(MjCliRunnerTest, unsupportedInputType)
{
    CliOptions opts;
    opts.input  = "foo.txt";
    opts.output = "bar.txt";
    CliRunReport report =
        runCli(opts, [](std::string_view, std::vector<ca::u8>&) { return ClassProcessResult{}; });
    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.processed, 0);
    EXPECT_EQ(report.failure, CliFailureKind::UnsupportedInput);
}

// ── class 文件往返：processor 被调用 + 输出生成 ──
TEST(MjCliRunnerTest, classFileProcessorInvoked)
{
    TempFile in(".class");
    TempFile out(".class");

    auto bytes = buildMinimalClass();
    ASSERT_TRUE(
        FileUtil::write_bytes(in.path(), ca::core::ByteSlice(bytes.data(), bytes.size())).is_ok());

    int             callCount = 0;
    std::string     seenUri;
    ClassProcessor proc = [&](std::string_view uri, std::vector<ca::u8>& b) {
        callCount++;
        seenUri = std::string(uri);
        EXPECT_FALSE(b.empty());
        return ClassProcessResult{};
    };

    CliOptions opts;
    opts.input          = in.path();
    opts.output         = out.path();
    CliRunReport report = runCli(opts, proc);

    EXPECT_EQ(callCount, 1);
    EXPECT_EQ(report.processed, 1);
    EXPECT_EQ(report.failed, 0);
    EXPECT_EQ(report.failure, CliFailureKind::None);
    EXPECT_TRUE(report.ok());
    EXPECT_TRUE(out.exists());
    // 输出是合法 class（能被 reader 解析，方法数不变）
    auto outBytes = FileUtil::read_all_bytes(out.path());
    ASSERT_TRUE(outBytes.is_ok());
    auto ob    = outBytes.unwrap();
    auto read  = jbc::core::readClass(jbc::core::ByteView(ob.as_ptr(), ob.len()));
    ASSERT_TRUE(read.is_ok());
    auto parsed = std::move(read).unwrap();   // ReadResult 持 unique_ptr，只能移出
    EXPECT_EQ(parsed.file->methods.size(), 1u);
}

// ── -c 过滤：只处理匹配 class，导出 jar 保留未匹配 class ──
TEST(MjCliRunnerTest, classFilterPreservesUnmatchedJarResources)
{
    TempFile in(".jar");
    TempFile out(".jar");

    model::ResourceContainer input;
    input.addResource(
        "Target.class",
        std::make_unique<model::ClassResource>("Target.class", buildMinimalClass("Target")));
    input.addResource(
        "Other.class",
        std::make_unique<model::ClassResource>("Other.class", buildMinimalClass("Other")));
    ASSERT_TRUE(model::ResourceHelper::exportToJar(input, in.path()).is_ok());

    CliOptions opts;
    opts.input       = in.path();
    opts.output      = out.path();
    opts.targetClass = "Target";
    CliRunReport report =
        runCli(opts, [](std::string_view, std::vector<ca::u8>&) { return ClassProcessResult{}; });

    EXPECT_EQ(report.processed, 1);
    EXPECT_EQ(report.failed, 0);
    EXPECT_EQ(report.failure, CliFailureKind::None);
    EXPECT_TRUE(report.ok());

    model::ResourceContainer reloaded;
    ASSERT_TRUE(model::ResourceHelper::importFromJar(reloaded, out.path()).is_ok());
    EXPECT_TRUE(reloaded.hasResource("Target.class"));
    EXPECT_TRUE(reloaded.hasResource("Other.class"));
}

// ── processor 失败时计入 failed ──
TEST(MjCliRunnerTest, processorFailureCounted)
{
    TempFile in(".class");
    TempFile out(".class");

    auto bytes = buildMinimalClass();
    ASSERT_TRUE(
        FileUtil::write_bytes(in.path(), ca::core::ByteSlice(bytes.data(), bytes.size())).is_ok());

    ClassProcessor proc = [](std::string_view, std::vector<ca::u8>&) {
        ClassProcessResult r;
        r.ok      = false;
        r.message = "boom";
        return r;
    };

    CliOptions opts;
    opts.input  = in.path();
    opts.output = out.path();
    testing::internal::CaptureStderr();
    CliRunReport report = runCli(opts, proc);
    std::string  err    = testing::internal::GetCapturedStderr();

    EXPECT_EQ(report.processed, 0);
    EXPECT_EQ(report.failed, 1);
    EXPECT_EQ(report.failure, CliFailureKind::Processing);
    EXPECT_EQ(report.failureMessage, "boom");
    EXPECT_FALSE(report.ok());
    EXPECT_NE(err.find("FAIL:"), std::string::npos);
    EXPECT_EQ(err.find("no class files found"), std::string::npos);
    EXPECT_FALSE(out.exists());
}

TEST(MjCliRunnerTest, verificationRejectsMalformedProcessorOutput)
{
    TempFile in(".class");
    TempFile out(".class");
    const auto input = buildMinimalClass();
    ASSERT_TRUE(FileUtil::write_bytes(
                    in.path(), ca::core::ByteSlice(input.data(), input.size()))
                    .is_ok());

    ClassProcessor proc = [](std::string_view, std::vector<ca::u8>& bytes) {
        bytes = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00};
        return ClassProcessResult{};
    };

    CliOptions opts;
    opts.input  = in.path();
    opts.output = out.path();
    const auto report = runCli(opts, proc);

    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.failure, CliFailureKind::Verification);
    EXPECT_EQ(report.processed, 0);
    EXPECT_EQ(report.failed, 1);
    EXPECT_FALSE(out.exists());
}

TEST(MjCliRunnerTest, toleratesJarClassEntryWithoutClassMagic)
{
    TempFile in(".jar");
    TempFile out(".jar");
    model::ResourceContainer input;
    const auto               goodClass    = buildMinimalClass();
    const std::vector<ca::u8> invalidClass = {0x00, 0x01, 0x02, 0x03};
    input.addResource("Good.class", std::make_unique<model::ClassResource>("Good.class", goodClass));
    input.addResource(
        "Invalid.class", std::make_unique<model::UnknownResource>("Invalid.class", invalidClass));
    ASSERT_TRUE(model::ResourceHelper::exportToJar(input, in.path()).is_ok());

    CliOptions opts;
    opts.input  = in.path();
    opts.output = out.path();
    const auto report =
        runCli(opts, [](std::string_view, std::vector<ca::u8>&) { return ClassProcessResult{}; });

    // 病态 jar 中“以 .class 命名但内容无 class magic”的 entry 按内容分拣为
    // UnknownResource（如被混淆器改坏的 module-info.class），容错跳过；
    // 有效 class 存在时输出仍视为可验证通过，且输出文件产生。
    EXPECT_TRUE(report.ok()) << report.failureMessage;
    EXPECT_EQ(report.processed, 1);
    EXPECT_EQ(report.failed, 0);
    EXPECT_TRUE(out.exists());
}

TEST(MjCliRunnerTest, refusesToOverwriteExistingOutput)
{
    TempFile in(".class");
    TempFile out(".class");
    const std::vector<ca::u8> existingOutput = {0x01, 0x02, 0x03};
    const auto input = buildMinimalClass();
    ASSERT_TRUE(FileUtil::write_bytes(
                    in.path(), ca::core::ByteSlice(input.data(), input.size()))
                    .is_ok());
    ASSERT_TRUE(FileUtil::write_bytes(
                    out.path(),
                    ca::core::ByteSlice(existingOutput.data(), existingOutput.size()))
                    .is_ok());

    CliOptions opts;
    opts.input  = in.path();
    opts.output = out.path();
    testing::internal::CaptureStderr();
    const auto report =
        runCli(opts, [](std::string_view, std::vector<ca::u8>&) { return ClassProcessResult{}; });
    const auto error = testing::internal::GetCapturedStderr();

    EXPECT_FALSE(report.ok());
    EXPECT_EQ(report.failure, CliFailureKind::InvalidArguments);
    EXPECT_NE(error.find("refusing to overwrite"), std::string::npos);
    const auto actual = FileUtil::read_all_bytes(out.path());
    ASSERT_TRUE(actual.is_ok());
    const auto bytes = actual.unwrap();
    ASSERT_EQ(bytes.len(), existingOutput.size());
    EXPECT_TRUE(std::equal(bytes.as_ptr(), bytes.as_ptr() + bytes.len(), existingOutput.begin()));
}

// ── u8path 清扫：UTF-8 输出路径 staged 发布钉 ─────────────────
// createStagedOutput 曾按 ACP 隐式窄构造 destination 且 generic_string 回写
// 窄串（FileUtil 以 UTF-8 契约再解码 = 二次转码）；非 UTF-8 ACP 进程里 UTF-8
// 输出路径的 staged 文件落 mojibake 位、发布失败或错位。修复后 u8path 显式
// 构造 + generic_u8string 全程 UTF-8 字节域——发布必须精确落在真实 UTF-8 路径。
TEST(MjCliRunnerTest, stagedPublishesToExactUtf8OutputPath)
{
    namespace fs = std::filesystem;

    const auto outDir =
        fs::temp_directory_path() / fs::u8path("mjbase_cli_u8_\xE7\xBC\x96\xE7\xA0\x81");
    fs::remove_all(outDir);
    fs::create_directories(outDir);
    // "输出.class"（UTF-8 手工字节转义）。
    const auto outPath = outDir / fs::u8path("\xE8\xBE\x93\xE5\x87\xBA.class");

    TempFile in(".class");
    auto     bytes = buildMinimalClass();
    ASSERT_TRUE(
        FileUtil::write_bytes(in.path(), ca::core::ByteSlice(bytes.data(), bytes.size()))
            .is_ok());

    CliOptions opts;
    opts.input        = in.path();
    opts.output       = outPath.u8string();
    const auto report = runCli(opts, [](std::string_view, std::vector<ca::u8>&) {
        return ClassProcessResult{};
    });

    EXPECT_TRUE(report.ok()) << static_cast<int>(report.failure);
    EXPECT_EQ(report.processed, 1);
    ASSERT_TRUE(fs::exists(outPath)) << "staged 发布必须精确落在 UTF-8 输出路径";
    const auto reparsed = FileUtil::read_all_bytes(outPath.u8string());
    ASSERT_TRUE(reparsed.is_ok());
    EXPECT_GT(reparsed.unwrap().len(), 0U);

    fs::remove_all(outDir);
}

// ── targetMethod 解析：name / name()I 过滤语义 ──
TEST(MjCliRunnerTest, methodPredicateMatchesNameAndDescriptor)
{
    auto byName = MethodFilter::parse("keep");
    ASSERT_TRUE(byName.has_value());
    EXPECT_TRUE(byName->matches("keep", "()I"));
    EXPECT_TRUE(byName->matches("keep", "()V"));
    EXPECT_FALSE(byName->matches("other", "()I"));

    auto byDescriptor = MethodFilter::parse("keep()I");
    ASSERT_TRUE(byDescriptor.has_value());
    EXPECT_TRUE(byDescriptor->matches("keep", "()I"));
    EXPECT_FALSE(byDescriptor->matches("keep", "()V"));
    EXPECT_FALSE(byDescriptor->matches("other", "()I"));

    // 空串 → nullopt（不过滤）。
    EXPECT_FALSE(MethodFilter::parse("").has_value());
}
