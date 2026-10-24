/// ClassRenamer Multi-Release 族契约的真 JVM 硬验收：javac --release 11 编译
/// 三源根样本（base + versions/11 + versions/17），ClassRenamer 全容器改名后
/// 经 ResourceHelper::exportToJar 落盘（主属性守护自动接线），多版本运行时
/// 矩阵真跑——各 release 路径各取变体（运行时取 ≤ 当前 JVM 的最大 N）、改名
/// 后引用完整、变体独立生命周期（删 base/删变体三种形态逐一体检）。JDK 不可
/// 用或 javac < 11 的机器整测跳过。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "jdk_fixture.hpp"

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/class_renamer.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

namespace {

// ── 样本源码（三源根：base / v11 / v17）────────────────────────────────────

constexpr const char* kMainSrc = R"JAVA(
package app;

public class Main {
    public static void main(String[] args) {
        System.out.println(Api.marker());
        System.out.println(Util.label());
        try {
            Extra.touch();
            System.out.println("extra-present");
        } catch (Throwable t) {
            System.out.println("extra-absent");
        }
    }
}
)JAVA";

constexpr const char* kApiBaseSrc = R"JAVA(
package app;

public class Api {
    public static String marker() { return "base"; }
}
)JAVA";

constexpr const char* kApiV11Src = R"JAVA(
package app;

public class Api {
    public static String marker() { return "v11"; }
}
)JAVA";

constexpr const char* kApiV17Src = R"JAVA(
package app;

public class Api {
    public static String marker() { return "v17"; }
}
)JAVA";

constexpr const char* kUtilSrc = R"JAVA(
package app;

public class Util {
    public static String label() { return "util-label"; }
}
)JAVA";

constexpr const char* kExtraSrc = R"JAVA(
package app;

public class Extra {
    public static void touch() {}
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

/// javac 主版本（"javac 17.0.8" → 17）；解析失败返回 0。
int javacMajor(const test::jdk::JdkToolchain& tc)
{
    const auto r = test::jdk::runBat({test::jdk::q(tc.javac) + " -version"});
    if (r.exitCode != 0) return 0;
    const std::string out = r.out;
    const auto        pos = out.find("javac ");
    if (pos == std::string::npos) return 0;
    const auto        quoteHead = out.find('"', pos);
    const std::string version =
        quoteHead == std::string::npos ? out.substr(pos + 6) : out.substr(quoteHead + 1);
    const auto dot = version.find('.');
    try {
        return std::stoi(dot == std::string::npos ? version : version.substr(0, dot));
    }
    catch (const std::exception&) {
        return 0;
    }
}

/// 额外运行时：主 JDK 家目录的兄弟 jdk<N> 目录（并排安装布局），逐个以
/// `java -version` 验证。纯 best-effort——找不到就降级为单运行时矩阵。
struct ExtraRuntime
{
    int       major = 0;
    fs::path  java;
};

std::vector<ExtraRuntime> probeExtraRuntimes(const test::jdk::JdkToolchain& primary)
{
    std::vector<ExtraRuntime> out;
#ifdef _WIN32
    constexpr const char* kJavaName = "java.exe";
#else
    constexpr const char* kJavaName = "java";
#endif
    for (int major = 8; major <= 40; ++major) {
        const fs::path home = primary.javaHome.parent_path() / ("jdk" + std::to_string(major));
        if (home == primary.javaHome) continue;
        const fs::path java = home / "bin" / kJavaName;
        std::error_code ec;
        if (!fs::is_regular_file(java, ec)) continue;
        const auto r = test::jdk::runBat({test::jdk::q(java) + " -version"});
        if (r.exitCode != 0) continue;
        const auto pos = r.out.find("version \"");
        if (pos == std::string::npos) continue;
        const std::string version = r.out.substr(pos + strlen("version \""));
        const auto        dot     = version.find('.');
        try {
            const int found = std::stoi(version.substr(0, dot));
            if (found == major) out.push_back({found, java});
        }
        catch (const std::exception&) {
            continue;
        }
    }
    return out;
}

/// 组装改名后容器（形态由 keepBaseApi / keepExtra 控制）并导出 jar；返回
/// (jarPath, Main 改名后的入口内部名)。编号随形态位移：Extra 在场占 Class1，
/// 缺席时 Main/Util 各前移一号。
std::pair<fs::path, std::string> buildObfuscatedJar(const fs::path& gateDir,
                                                    const fs::path& binDir,
                                                    bool keepBaseApi, bool keepExtra,
                                                    const char* jarName)
{
    // 装载顺序有意打乱（变体先于 base 入容器）：编号契约必须与插入序无关。
    mjbase::model::ResourceContainer container;
    const auto add = [&container](const char* uri, const fs::path& path) {
        container.addResource(uri, std::make_unique<mjbase::model::ClassResource>(uri,
                                                                                  readClassBytes(path)));
    };
    add("META-INF/versions/17/app/Extra.class", binDir / "v17/app/Extra.class");
    add("app/Api.class", binDir / "base/app/Api.class");
    add("META-INF/versions/17/app/Api.class", binDir / "v17/app/Api.class");
    add("app/Util.class", binDir / "base/app/Util.class");
    add("META-INF/versions/11/app/Api.class", binDir / "v11/app/Api.class");
    add("app/Main.class", binDir / "base/app/Main.class");
    if (!keepBaseApi) container.getResources().erase("app/Api.class");
    if (!keepExtra) container.getResources().erase("META-INF/versions/17/app/Extra.class");

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::ClassRenamer         pass;
    const auto                         pa = pass.run(container, gam);
    EXPECT_FALSE(pa.areAllPreserved()) << "重命名未发生";

    // 族契约断言：无论形态如何，改名后 URI 集形状固定（编号与插入序无关）。
    std::set<std::string> uris;
    container.visitResources([&](std::string_view uri, const mjbase::model::IResource&) {
        uris.emplace(uri);
    });
    std::set<std::string> expected = {
        "META-INF/versions/11/app/Class0.class", // Api 族（编号只计一次）
        "META-INF/versions/17/app/Class0.class",
    };
    if (keepBaseApi) expected.insert("app/Class0.class");
    if (keepExtra) {
        expected.insert("META-INF/versions/17/app/Class1.class"); // Extra v17 变体
        expected.insert("app/Class2.class");                      // Main（运行入口）
        expected.insert("app/Class3.class");                      // Util（改名目标）
    }
    else {
        expected.insert("app/Class1.class");                      // Main（运行入口）
        expected.insert("app/Class2.class");                      // Util（改名目标）
    }
    EXPECT_EQ(uris, expected) << jarName;

    const fs::path jarPath = gateDir / jarName;
    const auto     written = mjbase::model::ResourceHelper::exportToJar(container, jarPath.string());
    EXPECT_TRUE(written.is_ok()) << jarName;
    return {jarPath, keepExtra ? "app/Class2" : "app/Class1"};
}

}   // namespace

TEST(ClassRenamerMultiReleaseJvm, RenamedFamilyRunsOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }
    const int compilerMajor = javacMajor(tc);
    if (compilerMajor < 11) {
        GTEST_SKIP() << "javac < 11（无 --release 11 交叉编译面）: major=" << compilerMajor;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate_mr";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin";

    // ── 1. 三源根交叉编译（--release 11：classfile 55，JDK 11+ 全可加载）──
    // 编译序：v11/v17 先行，base 根以 -cp 引 v17 产物——Main 对 Extra 的引用
    // 是编译期符号（运行期探针走 NCDFE 捕获），而 Extra 只存在于 v17 源根。
    writeSource(srcDir / "base/app/Main.java", kMainSrc);
    writeSource(srcDir / "base/app/Api.java", kApiBaseSrc);
    writeSource(srcDir / "base/app/Util.java", kUtilSrc);
    writeSource(srcDir / "v11/app/Api.java", kApiV11Src);
    writeSource(srcDir / "v17/app/Api.java", kApiV17Src);
    writeSource(srcDir / "v17/app/Extra.java", kExtraSrc);
    std::string compileErr;
    const auto  compileRoot = [&](const char* root, const std::string& extraCp) {
        const fs::path srcRoot = srcDir / root;
        const fs::path outRoot = binDir / root;
        fs::create_directories(outRoot);
        std::string cmd = test::jdk::q(tc.javac) + " -encoding UTF-8 --release 11 -d " +
                          test::jdk::q(outRoot);
        if (!extraCp.empty()) cmd += " -cp " + test::jdk::q(extraCp);
        for (const auto& entry : fs::recursive_directory_iterator(srcRoot)) {
            if (entry.path().extension() == ".java") {
                cmd += " " + test::jdk::q(entry.path());
            }
        }
        const auto r = test::jdk::runBat({cmd});
        if (r.exitCode != 0) compileErr = r.out;
        return r.exitCode == 0;
    };
    ASSERT_TRUE(compileRoot("v11", "")) << "compile failed [v11]";
    ASSERT_TRUE(compileRoot("v17", "")) << "compile failed [v17]";
    ASSERT_TRUE(compileRoot("base", (binDir / "v17").string())) << "compile failed [base]:\n"
                                                               << compileErr;

    // ── 2. 全形态 jar：改名 + 导出（主属性守护接线）+ 多运行时矩阵真跑 ──
    const auto [fullJar, fullMainName] =
        buildObfuscatedJar(gate, binDir, true, true, "full.jar");

    // 导出产物主属性由守护补齐（JEP 238：缺属性 JVM 9+ 忽略整棵 versions 树）。
    {
        mjbase::model::ResourceContainer reloaded;
        ASSERT_TRUE(mjbase::model::ResourceHelper::importFromJar(reloaded, fullJar.string()).is_ok());
        const auto* manifest = ca::core::tag_cast::dyn_cast<const mjbase::model::ManifestResource>(
            reloaded.getResource(ca::str::Utf8StringRef::from_cstr("META-INF/MANIFEST.MF")));
        ASSERT_NE(manifest, nullptr);
        EXPECT_EQ(manifest->get()
                      .getMainAttribute(ca::str::Utf8StringRef::from_cstr("Multi-Release"))
                      .to_std_string(),
                  "true");
    }

    // 旧名清零：全 jar 无 app/Main|Api|Util|Extra 条目（改名完整性，非 JVM 面）。
    {
        auto entries = mjbase::jar::listJarEntries(
            ca::str::Utf8StringRef::from_string_view(fullJar.string()));
        ASSERT_TRUE(entries.is_ok());
        auto entryList = std::move(entries).unwrap();
        for (const char* old : {"app/Main.class", "app/Api.class", "app/Util.class",
                                "app/Extra.class"}) {
            bool found = false;
            for (const auto& info : entryList) {
                if (info.name.ref().to_std_string() == old) found = true;
            }
            EXPECT_FALSE(found) << "旧名条目残留: " << old;
        }
    }

    const auto expectFullRun = [](const test::jdk::ExecResult& run, int runtimeMajor) {
        EXPECT_EQ(run.exitCode, 0) << "runtime " << runtimeMajor << ":\n" << run.out;
        const std::string out = test::jdk::normalizeOutput(run.out);
        // 运行时取 ≤ 当前 major 的最大 N：≥17 取 v17，11 取 v11；base 兜底只在
        // 变体缺席时生效（本形态三份齐全，marker 恒为变体）。
        EXPECT_NE(out.find(runtimeMajor >= 17 ? "v17" : "v11"), std::string::npos)
            << "runtime " << runtimeMajor << ":\n"
            << out;
        EXPECT_NE(out.find("util-label"), std::string::npos) << out;
        // Extra 只存在于 versions/17：运行时 ≥17 可见，<17 不可见（NCDFE 探针）。
        if (runtimeMajor >= 17) {
            EXPECT_NE(out.find("extra-present"), std::string::npos) << out;
            EXPECT_EQ(out.find("extra-absent"), std::string::npos) << out;
        }
        else {
            EXPECT_NE(out.find("extra-absent"), std::string::npos) << out;
        }
    };

    // 主运行时 + 兄弟 JDK（best-effort，找不到即单运行时矩阵）。
    std::vector<std::pair<int, test::jdk::JdkToolchain>> runtimes;
    runtimes.emplace_back(compilerMajor, tc);
    for (const ExtraRuntime& extra : probeExtraRuntimes(tc)) {
        if (extra.major < 11) continue;   // classfile 55 以下无法加载
        test::jdk::JdkToolchain sibling;
        sibling.javaHome   = extra.java.parent_path().parent_path();
        sibling.java       = extra.java;
        sibling.available  = true;
        runtimes.emplace_back(extra.major, sibling);
    }
    for (const auto& [major, runtime] : runtimes) {
        const auto run = test::jdk::runJavaMain(runtime, fullJar, fullMainName);
        expectFullRun(run, major);
    }

    // ── 3. 删 base 的 Api：release 路径各取 v11/v17（变体兜底，无 base）──
    {
        const auto [jar, mainName] = buildObfuscatedJar(gate, binDir, false, true, "no_base_api.jar");
        for (const auto& [major, runtime] : runtimes) {
            const auto run = test::jdk::runJavaMain(runtime, jar, mainName);
            EXPECT_EQ(run.exitCode, 0) << "runtime " << major << ":\n" << run.out;
            const std::string out = test::jdk::normalizeOutput(run.out);
            EXPECT_NE(out.find(major >= 17 ? "v17" : "v11"), std::string::npos) << out;
            EXPECT_NE(out.find("util-label"), std::string::npos) << out;
            if (major >= 17) {
                EXPECT_NE(out.find("extra-present"), std::string::npos) << out;
            }
        }
    }

    // ── 4. 删 Extra 变体：probe 报 absent，其余 release 路径不受影响 ──
    {
        const auto [jar, mainName]   = buildObfuscatedJar(gate, binDir, true, false, "no_extra.jar");
        const auto& [major, runtime] = runtimes.front();
        const auto run               = test::jdk::runJavaMain(runtime, jar, mainName);
        EXPECT_EQ(run.exitCode, 0) << run.out;
        const std::string out = test::jdk::normalizeOutput(run.out);
        EXPECT_NE(out.find(major >= 17 ? "v17" : "v11"), std::string::npos) << out;
        EXPECT_NE(out.find("util-label"), std::string::npos) << out;
        EXPECT_NE(out.find("extra-absent"), std::string::npos) << out;
        EXPECT_EQ(out.find("extra-present"), std::string::npos) << out;
    }
}
