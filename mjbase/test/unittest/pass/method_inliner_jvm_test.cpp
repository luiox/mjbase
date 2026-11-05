/// MethodInliner 在真 JVM 上的硬验收：javac 编译真实样本（跨类 static 纯函数
/// 内联、前向分支体内联、超预算站点放弃）→ pass 改写 → 产物目录真 java 执行
/// （默认 -Xverify:all），stdout 与退出码必须与原样本一致——改写类过
/// verifier、行为等价的直接证据；另验壳保留/放弃类字节逐位保持与二次运行
/// 幂等。JDK 不可用的机器整测跳过。
///
/// 同类 private 实例（invokespecial）形态不进本门：Java 8 javac 编译为
/// invokespecial、Java 11+ nestmate 编译为 invokevirtual，断言随 JDK 版本
/// 漂移；该形态的拼接正确性由 method_inliner_test.cpp 的指令序事实断言
/// 钉死（v1 边界：invokevirtual 调用点本来就不收）。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/method_inliner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// 样本设计（内联点全为纯计算体；打印只出现在 main——非候选）：
/// - Main.main：Main.Helper.add(3,4)=7（跨类 static 双 public 内联）+
///   Helper.pick(2)=3（三分支前向形态内联）+ Helper.k()=42（零参内联）。
///   Helper 是 Main 的 public static 嵌套类——javac 逐文件只收一个 public
///   顶层类，嵌套类是同文件多 public 形态的唯一出口，编译产物独立类
///   Main$Helper，跨类门（双 public / 无 <clinit> / 体白名单）照常生效。
/// - big：30 组 iinc 超预算（34 条 > 32）→ 候选拒收；Gate.main 的同类 big
///   站点原样保留（Gate 字节逐位保持）。
/// - Helper 壳保留（删除延后删除族管线，本 pass 不动）。
/// - 编译口径：Main.java（Main + Main$Helper）与 Gate.java（自含 big 副本）
///   各自 compileJava 单文件自足。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.inl;

public class Main {
    public static void main(String[] args) {
        System.out.println(Helper.add(3, 4));
        System.out.println(Helper.pick(2));
        System.out.println(Helper.k());
    }

    public static class Helper {
        public static int add(int a, int b) {
            return a + b;
        }
        public static int pick(int x) {
            return x > 0 ? 3 : 4;
        }
        public static int k() {
            return 42;
        }
        public static int big(int x) {
            int r = x;
            r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1;
            r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1;
            r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1;
            return r;
        }
    }
}
)JAVA";

constexpr const char* kGateSrc = R"JAVA(
package hb.jvm.inl;

public class Gate {
    public static void main(String[] args) {
        System.out.println(big(1));
    }

    public static int big(int x) {
        int r = x;
        r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1;
        r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1;
        r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1; r += 1;
        return r;
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

constexpr const char* kClassNames[] = {"Main", "Main$Helper", "Gate"};

}   // namespace

TEST(MjPassMethodInlinerJvm, RewrittenClassesRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin_inliner";
    const fs::path outDir = gate / "obf_inliner";

    // ── 1. 源码 + javac 编译（两单元各自自足）──
    writeSource(srcDir / "hb/jvm/inl/Main.java", kMainSrc);
    writeSource(srcDir / "hb/jvm/inl/Gate.java", kGateSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/inl/Main.java", binDir, &compileErr))
        << compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/inl/Gate.java", binDir, &compileErr))
        << compileErr;

    // ── 2. 原样本基线运行（-Xverify:all 默认口径）──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.inl.Main");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut = test::jdk::normalizeOutput(orig.out);
    EXPECT_NE(origOut.find("7"), std::string::npos);
    EXPECT_NE(origOut.find("3"), std::string::npos);
    EXPECT_NE(origOut.find("42"), std::string::npos);

    const auto origGate = test::jdk::runJavaMain(tc, binDir, "hb.jvm.inl.Gate");
    ASSERT_EQ(origGate.exitCode, 0) << origGate.out;
    const std::string origGateOut = test::jdk::normalizeOutput(origGate.out);
    EXPECT_NE(origGateOut.find("31"), std::string::npos);

    // ── 3. pass 改写：Main 有改写面，Main$Helper/Gate 原样 ──
    mjbase::model::ResourceContainer container;
    std::unordered_map<std::string, Bytes> before;
    for (const char* name : kClassNames) {
        const Bytes corpus =
            readClassBytes(binDir / "hb/jvm/inl" / (std::string(name) + ".class"));
        before.emplace(name, corpus);
        container.addResource(std::string("hb/jvm/inl/") + name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  std::string("hb/jvm/inl/") + name + ".class", corpus));
    }

    {
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        mjbase::pass::MethodInliner        pass;
        const auto                         pa = pass.run(container, gam);
        ASSERT_FALSE(pa.areAllPreserved()) << "Main 应有改写面";
    }

    const auto* mainRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/inl/Main.class"));
    const auto* helperRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/inl/Main$Helper.class"));
    const auto* gateRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/inl/Gate.class"));
    ASSERT_NE(mainRes, nullptr);
    ASSERT_NE(helperRes, nullptr);
    ASSERT_NE(gateRes, nullptr);
    EXPECT_NE(mainRes->get(), before.at("Main")) << "Main 的 add/pick/k 站点应被内联";
    EXPECT_EQ(helperRes->get(), before.at("Main$Helper")) << "Helper 壳应逐位保持";
    EXPECT_EQ(gateRes->get(), before.at("Gate")) << "Gate 超预算站点应逐位保持";

    // ── 4. 幂等：二次运行严格 no-op ──
    {
        const Bytes mainSettled = mainRes->get();
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        mjbase::pass::MethodInliner        pass;
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        const auto* mainAgain = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource("hb/jvm/inl/Main.class"));
        ASSERT_NE(mainAgain, nullptr);
        EXPECT_EQ(mainAgain->get(), mainSettled);
    }

    // ── 5. 硬验收：改写产物真 java 执行（verifier 接受 + 行为等价）──
    const fs::path dst = outDir / "hb/jvm/inl";
    fs::create_directories(dst);
    for (const char* name : kClassNames) {
        const auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource(std::string("hb/jvm/inl/") + name + ".class"));
        ASSERT_NE(res, nullptr) << name;
        std::ofstream fout(dst / (std::string(name) + ".class"), std::ios::binary);
        ASSERT_TRUE(fout.good()) << (dst / name).string();
        fout.write(reinterpret_cast<const char*>(res->get().data()),
                   static_cast<std::streamsize>(res->get().size()));
    }
    const auto cleaned = test::jdk::runJavaMain(tc, outDir, "hb.jvm.inl.Main");
    EXPECT_EQ(cleaned.exitCode, 0) << "inlined Main rejected/failed:\n" << cleaned.out;
    EXPECT_EQ(test::jdk::normalizeOutput(cleaned.out), origOut);

    const auto cleanedGate = test::jdk::runJavaMain(tc, outDir, "hb.jvm.inl.Gate");
    EXPECT_EQ(cleanedGate.exitCode, 0) << "Gate rejected/failed:\n" << cleanedGate.out;
    EXPECT_EQ(test::jdk::normalizeOutput(cleanedGate.out), origGateOut);
}
