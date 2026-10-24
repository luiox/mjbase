/// MethodCallRemover 在真 JVM 上的硬验收：javac 编译样本（点名面删 void
/// 静态调用 + 白名单面删 String.length / Math.abs / System.currentTimeMillis
/// 丢弃形态）→ 产物目录真 java 执行，stdout 必须等于原样本去掉被删调用
/// 的输出行——改写类可加载可校验、行为面恰为「调用点消失」的直接证据。
/// JDK 不可用的机器整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/remove_method_calls.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// 单顶层类样本：main 带分支（javac 必产 StackMapTable——改写后的 Strict
/// 帧再生链被真 JVM verifier 验收）+ 一个被点名删除的 noisy()（打印
/// "noise"，删除后该行从输出消失）+ 三个白名单丢弃形态（结果全部被丢弃，
/// 删除后行为不变——长度/绝对值/时间戳都未进入可观察输出）。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.rmcall;

public class Main {
    public static void main(String[] args) {
        int guard = args.length == 0 ? 1 : 2;
        noisy();
        "abcdef".length();
        Math.abs(-5);
        System.currentTimeMillis();
        System.out.println("alive-" + guard);
    }

    static void noisy() {
        System.out.println("noise");
    }
}
)JAVA";

void writeSource(const fs::path& path, const char* src)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << "cannot write sample source: " << path.string();
    out << src;
}

Bytes readClassBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot read sample class: " << path.string();
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// 按行归一 stdout（丢空行），便于做「原输出减被删行」比较。
std::string withoutNoiseLine(std::string out)
{
    std::string result;
    ca::usize cursor = 0;
    while (cursor <= out.size()) {
        const ca::usize end  = out.find('\n', cursor);
        const std::string line =
            out.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        if (!line.empty() && line != "noise") {
            result += line;
            result += "\n";
        }
        if (end == std::string::npos) break;
        cursor = end + 1;
    }
    return result;
}

}   // namespace

TEST(MethodCallRemoverJvm, CleanedClassRunsOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin_rmc";
    const fs::path outDir = gate / "obf_rmclean";

    // ── 1. 源码 + javac 编译 ──
    writeSource(srcDir / "hb/jvm/rmcall/Main.java", kMainSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/rmcall/Main.java", binDir, &compileErr))
        << compileErr;

    // ── 2. 原样本基线运行 ──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.rmcall.Main");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut    = test::jdk::normalizeOutput(orig.out);
    const std::string expectedOut = withoutNoiseLine(origOut);
    EXPECT_NE(origOut.find("noise"), std::string::npos);
    EXPECT_NE(origOut.find("alive"), std::string::npos);

    // ── 3. 跑 pass：点名 noisy()V + 启用白名单 ──
    mjbase::model::ResourceContainer container;
    const Bytes corpus = readClassBytes(binDir / "hb/jvm/rmcall/Main.class");
    container.addResource("hb/jvm/rmcall/Main.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "hb/jvm/rmcall/Main.class", corpus));

    mjbase::pass::MethodCallRemovalConfig config;
    mjbase::pass::MethodCallSpec noisy;
    noisy.owner = "hb/jvm/rmcall/Main";
    noisy.name  = "noisy";
    noisy.desc  = "()V";
    config.includeCalls.push_back(noisy);
    config.enableUnusedResultWhitelist = true;
    mjbase::pass::MethodCallRemover    pass(config);

    {
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        EXPECT_FALSE(pass.run(container, gam).areAllPreserved()) << "样本应有删除面";
    }

    // ── 4. 硬验收：产物目录真 java 执行（改写类须过 verify，行为恰为
    //      「noisy 调用点消失」，其余输出逐行一致）──
    const fs::path dst = outDir / "hb/jvm/rmcall";
    fs::create_directories(dst);
    const auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/rmcall/Main.class"));
    ASSERT_NE(res, nullptr);
    std::ofstream fout(dst / "Main.class", std::ios::binary);
    ASSERT_TRUE(fout.good()) << (dst / "Main.class").string();
    fout.write(reinterpret_cast<const char*>(res->get().data()),
               static_cast<std::streamsize>(res->get().size()));
    fout.close();

    const auto cleaned = test::jdk::runJavaMain(tc, outDir, "hb.jvm.rmcall.Main");
    EXPECT_EQ(cleaned.exitCode, 0) << "cleaned Main rejected/failed:\n" << cleaned.out;
    EXPECT_EQ(test::jdk::normalizeOutput(cleaned.out), expectedOut);
    EXPECT_EQ(test::jdk::normalizeOutput(cleaned.out).find("noise"), std::string::npos);
}
