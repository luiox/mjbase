/// MethodInliner 在真 JVM 上的硬验收：javac 编译真实样本（v1 跨类 static 纯
/// 函数内联、前向分支体内联、超预算站点放弃；v2 去虚化（final 方法/final
/// 类/private 不参与分派）、nestmate 跨类 private（invokestatic/invokevirtual
/// 双形态）、private 接口方法、浅循环体、switch 体）→ pass 改写 → 产物目录
/// 真 java 执行（默认 -Xverify:all），stdout 与退出码必须与原样本一致——
/// 改写类过 verifier、行为等价的直接证据；另验壳保留/放弃类字节逐位保持
/// 与二次运行幂等。JDK 不可用的机器整测跳过。
///
/// JDK 17 javac 口径（探针实测）：同类/跨 nest private 实例方法一律
/// invokevirtual、private static 一律 invokestatic、private 接口实例方法
/// invokeinterface——v2 去虚化判据覆盖三种形态，断言不随 JDK 小版本漂移
///（JEP 181 起 javac 形态已稳定）。
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
/// - Main.main：v1 面 = Helper.add/pick/k（跨类 static 双 public + 前向
///   分支 + 零参）+ Gate 超预算对照；v2 面 = Helper.fmul（final 方法
///   去虚化，接收者 = new 表达式）、Helper.loopSum（浅循环体，12 条含
///   1 回边）、Helper.switchPick（tableswitch 体）、new Main().twice
///   （同类 private 经 invokevirtual——JEP 181 形态）、Main.half（同类
///   private static，v1 面）、NestUser.use（跨 nest private static +
///   private 实例双形态）、FHelper.add7（final 类去虚化，方法本身非
///   final）、Priv.use（private 接口方法经 invokeinterface）。
/// - big：30 组 iinc 超预算（34 条 > 32）→ 候选拒收；Gate.main 的同类 big
///   站点原样保留（Gate 字节逐位保持）。
/// - Helper/FHelper/NestUser/Priv/PrivImpl 壳保留（删除延后删除族管线，
///   本 pass 不动；Priv.use 的体内站点会被改写——其调用方在接口自身）。
/// - 编译口径：Main.java（Main + 嵌套类族）与 Gate.java（自含 big 副本）
///   各自 compileJava 单文件自足。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.inl;

public class Main {
    public static void main(String[] args) {
        System.out.println(Helper.add(3, 4));
        System.out.println(Helper.pick(2));
        System.out.println(Helper.k());
        System.out.println(new Helper().fmul(3, 4));
        System.out.println(Helper.loopSum(5));
        System.out.println(Helper.switchPick(2));
        System.out.println(new Main().twice(9));
        System.out.println(half(21));
        System.out.println(NestUser.use());
        System.out.println(new FHelper().add7(1));
        System.out.println(Priv.use(new PrivImpl()));
    }

    private int twice(int x) {
        return x * 2;
    }

    private static int half(int x) {
        return x / 2;
    }

    public static class NestUser {
        public static int use() {
            Main m = new Main();
            return m.twice(5) + Main.half(8);
        }
    }

    public static final class FHelper {
        public int add7(int x) {
            return x + 7;
        }
    }

    public interface Priv {
        private int p(int x) {
            return x + 1;
        }
        static int use(Priv i) {
            return i.p(1);
        }
    }

    public static class PrivImpl implements Priv {
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
        public final int fmul(int a, int b) {
            return a * b;
        }
        public static int loopSum(int n) {
            int s = 0;
            for (int i = 0; i < n; ++i) {
                s += i;
            }
            return s;
        }
        public static int switchPick(int x) {
            switch (x) {
                case 1: return 10;
                case 2: return 20;
                default: return 30;
            }
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

constexpr const char* kClassNames[] = {
    "Main", "Main$Helper", "Main$FHelper", "Main$NestUser", "Main$Priv", "Main$PrivImpl",
    "Gate",
};

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
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.inl.Main", {},
                                             {"-Xverify:all"});
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut = test::jdk::normalizeOutput(orig.out);
    // 逐输出真值：v1 面（7/3/42/10）+ v2 面（12/10/20/18/14/8/2）。
    const char* kExpected[] = {"7", "3", "42", "12", "10", "20", "18", "10", "14", "8", "2"};
    for (const char* token : kExpected) {
        EXPECT_NE(origOut.find(token), std::string::npos) << "基线缺输出 " << token;
    }

    const auto origGate = test::jdk::runJavaMain(tc, binDir, "hb.jvm.inl.Gate", {},
                                                 {"-Xverify:all"});
    ASSERT_EQ(origGate.exitCode, 0) << origGate.out;
    const std::string origGateOut = test::jdk::normalizeOutput(origGate.out);
    EXPECT_NE(origGateOut.find("31"), std::string::npos);

    // ── 3. pass 改写：Main 与其嵌套类有改写面，Gate 原样 ──
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
    const auto* nestUserRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/inl/Main$NestUser.class"));
    const auto* privRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/inl/Main$Priv.class"));
    const auto* helperRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/inl/Main$Helper.class"));
    const auto* gateRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/inl/Gate.class"));
    ASSERT_NE(mainRes, nullptr);
    ASSERT_NE(nestUserRes, nullptr);
    ASSERT_NE(privRes, nullptr);
    ASSERT_NE(helperRes, nullptr);
    ASSERT_NE(gateRes, nullptr);
    EXPECT_NE(mainRes->get(), before.at("Main")) << "Main 的内联站点应被改写";
    EXPECT_NE(nestUserRes->get(), before.at("Main$NestUser"))
        << "NestUser 的跨 nest private 站点应被内联";
    EXPECT_NE(privRes->get(), before.at("Main$Priv"))
        << "Priv.use 的 private 接口方法站点应被内联";
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
    const auto cleaned = test::jdk::runJavaMain(tc, outDir, "hb.jvm.inl.Main", {},
                                                {"-Xverify:all"});
    EXPECT_EQ(cleaned.exitCode, 0) << "inlined Main rejected/failed:\n" << cleaned.out;
    EXPECT_EQ(test::jdk::normalizeOutput(cleaned.out), origOut);

    const auto cleanedGate = test::jdk::runJavaMain(tc, outDir, "hb.jvm.inl.Gate", {},
                                                    {"-Xverify:all"});
    EXPECT_EQ(cleanedGate.exitCode, 0) << "Gate rejected/failed:\n" << cleanedGate.out;
    EXPECT_EQ(test::jdk::normalizeOutput(cleanedGate.out), origGateOut);
}
