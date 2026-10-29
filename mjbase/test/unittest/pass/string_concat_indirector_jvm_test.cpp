/// StringConcatIndirector 的真 JVM 硬验收：javac --release 8 编译 SB 拼接链
/// 样本（major 52），会话抬 major 过版本门后跑 pass（SB 链 → indy），变换
/// 前后字节分别落盘真机执行，stdout 逐字节一致钉死语义等价（indy 路径真
/// 走 StringConcatFactory bootstrap）。JDK 不可用整测跳过。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

using Bytes = std::vector<std::uint8_t>;
#include "jdk_fixture.hpp"

#include "jbc/core/writer.hpp"

#include "mjbase/pass/string_concat_indirector.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

namespace {

constexpr const char* kConcatSrc = R"JAVA(
public class T {
    public static String m(int x) {
        return "a" + x + "b";
    }
}
)JAVA";

/// 编译样本 → 会话（可选抬 major）→ 跑 pass → 类字节。抬版前 changed=false
/// 属门缺陷；返回空 = 编译失败（显红）。
std::optional<Bytes> buildTransformed(const test::jdk::JdkToolchain& tc, bool bumpMajor)
{
    const test::jdk::fs::path bin = test::jdk::fs::temp_directory_path()
                               / ("mjbase_sci_src_"
                                  + std::to_string(
                                      std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    std::error_code ec;
    test::jdk::fs::create_directories(bin, ec);
    const auto src = bin / "T.java";
    {
        std::ofstream f(src, std::ios::binary);
        f << kConcatSrc;
    }
    const std::string cmd = test::jdk::q(tc.javac) + " -encoding UTF-8 --release 8 -d "
                            + test::jdk::q(bin) + " " + test::jdk::q(src);
    const auto compiled = test::jdk::runBat({cmd});
    if (compiled.exitCode != 0) {
        ADD_FAILURE() << "javac --release 8 failed: " << compiled.out;
        return std::nullopt;
    }
    std::ifstream cf(bin / "T.class", std::ios::binary);
    const Bytes    bytes((std::istreambuf_iterator<char>(cf)),
                    std::istreambuf_iterator<char>());
    test::jdk::fs::remove_all(bin, ec);

    auto made = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView{bytes.data(), bytes.size()});
    if (made.is_err()) {
        ADD_FAILURE() << "makeSessionFromBytes failed";
        return std::nullopt;
    }
    auto session = std::move(made).unwrap();
    if (bumpMajor) {
        session->backing().major = 53;
    }
    ca::str::Utf8StringArena           arena;
    mjbase::spi::MethodAnalysisManager mam(arena);
    mjbase::KeyPool                    identityPool;
    mjbase::spi::MethodPassManager     mpm;
    mpm.addPass(std::make_unique<mjbase::pass::StringConcatIndirector>());
    const mjbase::spi::PreservedAnalyses pa = mjbase::spi::runMethodPipelineOverSession(
        *session, mpm, mam, identityPool, nullptr, {});
    if (bumpMajor && !pa.changed()) {
        ADD_FAILURE() << "pass 未折叠（版本门后应命中窗口）";
        return std::nullopt;
    }
    if (!bumpMajor && !pa.areAllPreserved()) {
        ADD_FAILURE() << "v52 未过版本门却被改写";
        return std::nullopt;
    }
    auto written = jbc::core::writeClass(session->backing());
    if (written.is_err()) {
        ADD_FAILURE() << "writeClass failed";
        return std::nullopt;
    }
    return std::move(written).unwrap();
}

}   // namespace

TEST(StringConcatIndirectorJvm, transformedOutputMatchesOriginal)
{
    const test::jdk::JdkToolchain tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }
    const std::optional<Bytes> original = buildTransformed(tc, /*bumpMajor*/ false);
    ASSERT_TRUE(original.has_value());
    const std::optional<Bytes> transformed = buildTransformed(tc, /*bumpMajor*/ true);
    ASSERT_TRUE(transformed.has_value());

    test::jdk::RunnerSpec spec;
    spec.classes      = {{"T", *transformed}};
    spec.runnerBody   = "System.out.println(T.m(42));";
    spec.verify       = true;
    const auto indyRun = test::jdk::runWithRunnerOutcome(tc, spec);
    if (indyRun.status == test::jdk::JvmRunStatus::JdkUnavailable) {
        GTEST_SKIP() << indyRun.output;
    }
    ASSERT_EQ(indyRun.status, test::jdk::JvmRunStatus::Ok) << indyRun.output;

    spec.classes    = {{"T", *original}};
    const auto sbRun = test::jdk::runWithRunnerOutcome(tc, spec);
    ASSERT_EQ(sbRun.status, test::jdk::JvmRunStatus::Ok) << sbRun.output;

    EXPECT_EQ(indyRun.output, sbRun.output);
    EXPECT_NE(indyRun.output.find("a42b"), std::string::npos) << indyRun.output;
}
