/// DevirtualizeSingleTarget 在真 JVM 上的硬验收：javac 编译真实样本（含唯一
/// 目标改写点、多目标放弃点、非 this 接收者放弃点）→ pass 改写 → 产物目录
/// 真 java 执行，stdout 与退出码必须与原样本一致——改写类过 verifier、分派
/// 行为等价的直接证据；另验改写集字节差与二次运行幂等。JDK 不可用的机器
/// 整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "mjbase/reference/resolved_call_graph.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/devirtualize_single_target.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// 样本设计（改写点全部以 this 为接收者——pass 的接收者守卫口径）：
/// - Runner.run：describe()（唯一目标，改写）+ tag()（Runner2 覆写 → 多目标，
///   放弃）；
/// - Counter.inc：value()（唯一目标，改写）+ setN(int)（同类私有，javac
///   编译为 invokevirtual——Java 8 同类私有 / Java 11+ nestmate 同形；唯一
///   私有目标，C_t == 调用者，改写）；
/// - Main.main：new Runner().run() 接收者走局部变量（放弃）+
///   System.out.println（外部 owner，放弃）。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.devirt;

public class Main {
    public static void main(String[] args) {
        System.out.println(new Runner().run());
        System.out.println(Helper.text());
    }
}

class Runner {
    String run() {
        return describe() + "/" + tag();
    }
    String describe() {
        return "runner";
    }
    String tag() {
        return "base";
    }
}

class Runner2 extends Runner {
    String tag() {
        return "sub";
    }
}

class Counter {
    private int n = 0;
    void inc() {
        setN(value() + 1);
    }
    private void setN(int v) {
        n = v;
    }
    int value() {
        return n;
    }
}

class Helper {
    public static String text() {
        return "helper-text";
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

void registerAnalyses(mjbase::spi::GlobalAnalysisManager& gam)
{
    gam.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    gam.registerAnalysis<mjbase::reference::CallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::HierarchyBundleAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedCallGraphAnalysis>();
}

constexpr const char* kClassNames[] = {"Main", "Runner", "Runner2", "Counter", "Helper"};

}   // namespace

TEST(DevirtualizeSingleTargetJvm, RewrittenClassesRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin_devirt";
    const fs::path outDir = gate / "obf_devirt";

    // ── 1. 源码 + javac 编译 ──
    writeSource(srcDir / "hb/jvm/devirt/Main.java", kMainSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/devirt/Main.java", binDir, &compileErr))
        << compileErr;

    // ── 2. 原样本基线运行 ──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.devirt.Main");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut = test::jdk::normalizeOutput(orig.out);
    EXPECT_NE(origOut.find("runner/base"), std::string::npos);
    EXPECT_NE(origOut.find("helper-text"), std::string::npos);

    // ── 3. pass 改写：Runner 与 Counter 有改写面，Main/Runner2/Helper 原样 ──
    mjbase::model::ResourceContainer container;
    for (const char* name : kClassNames) {
        const Bytes corpus =
            readClassBytes(binDir / "hb/jvm/devirt" / (std::string(name) + ".class"));
        container.addResource(std::string("hb/jvm/devirt/") + name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  std::string("hb/jvm/devirt/") + name + ".class", corpus));
    }
    std::unordered_map<std::string, Bytes> before;
    for (const char* name : kClassNames) {
        before.emplace(name,
                       readClassBytes(binDir / "hb/jvm/devirt" / (std::string(name) + ".class")));
    }

    {
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerAnalyses(gam);
        mjbase::pass::DevirtualizeSingleTarget pass;
        const auto                             pa = pass.run(container, gam);
        ASSERT_FALSE(pa.areAllPreserved()) << "Runner/Counter 应有改写面";
    }

    const auto* runnerRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/devirt/Runner.class"));
    const auto* counterRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/devirt/Counter.class"));
    const auto* mainRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/devirt/Main.class"));
    const auto* subRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/devirt/Runner2.class"));
    const auto* helperRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/devirt/Helper.class"));
    ASSERT_NE(runnerRes, nullptr);
    ASSERT_NE(counterRes, nullptr);
    ASSERT_NE(mainRes, nullptr);
    ASSERT_NE(subRes, nullptr);
    ASSERT_NE(helperRes, nullptr);
    EXPECT_NE(runnerRes->get(), before.at("Runner")) << "Runner.run 的 describe() 应被改写";
    EXPECT_NE(counterRes->get(), before.at("Counter")) << "Counter 的 value()/setN() 应被改写";
    EXPECT_EQ(mainRes->get(), before.at("Main")) << "Main 不应有改写面（接收者守卫/外部 owner）";
    EXPECT_EQ(subRes->get(), before.at("Runner2")) << "Runner2 不应有改写面";
    EXPECT_EQ(helperRes->get(), before.at("Helper")) << "Helper 不应有改写面";

    // ── 4. 幂等：二次运行严格 no-op ──
    {
        const Bytes runnerSettled = runnerRes->get();
        const Bytes counterSettled = counterRes->get();
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerAnalyses(gam);
        mjbase::pass::DevirtualizeSingleTarget pass;
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        const auto* runnerAgain = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource("hb/jvm/devirt/Runner.class"));
        const auto* counterAgain = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource("hb/jvm/devirt/Counter.class"));
        ASSERT_NE(runnerAgain, nullptr);
        ASSERT_NE(counterAgain, nullptr);
        EXPECT_EQ(runnerAgain->get(), runnerSettled);
        EXPECT_EQ(counterAgain->get(), counterSettled);
    }

    // ── 5. 硬验收：改写产物真 java 执行（verifier 接受 + 行为等价）──
    const fs::path dst = outDir / "hb/jvm/devirt";
    fs::create_directories(dst);
    for (const char* name : kClassNames) {
        const auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource(std::string("hb/jvm/devirt/") + name + ".class"));
        ASSERT_NE(res, nullptr) << name;
        std::ofstream fout(dst / (std::string(name) + ".class"), std::ios::binary);
        ASSERT_TRUE(fout.good()) << (dst / name).string();
        fout.write(reinterpret_cast<const char*>(res->get().data()),
                   static_cast<std::streamsize>(res->get().size()));
    }
    const auto cleaned = test::jdk::runJavaMain(tc, outDir, "hb.jvm.devirt.Main");
    EXPECT_EQ(cleaned.exitCode, 0) << "cleaned Main rejected/failed:\n" << cleaned.out;
    EXPECT_EQ(test::jdk::normalizeOutput(cleaned.out), origOut);
}
