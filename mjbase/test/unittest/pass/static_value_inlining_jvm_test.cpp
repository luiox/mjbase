// StaticValueInlining 真 JVM 硬验收：javac 编译双类样本（伴生常量类
// Const：clinit 写 int/long/double/float/String + volatile 拒桶 + 多次写
// 拒桶；Main 跨类读全部字段）→ 收集件 + 物化件链式改写 → 产物目录以
// -Xverify:all 真跑，stdout 逐行等于基线——改写类可加载可校验、行为面
// 恒等的直接证据。附加断言：Const.class 字节不动、Main.class 字节变动、
// 已收集字段的 GETSTATIC 消失而 volatile/多次写字段的 GETSTATIC 保留。
// JDK 不可用的机器整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/static_value_collection.hpp"
#include "mjbase/pass/static_value_inlining.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

// 字段全部非 final：javac 不做编译期常量折叠，Main 侧必产真 GETSTATIC。
constexpr const char* kConstSrc = R"JAVA(
package hb.jvm.svi;

public class Const {
    public static int I1;
    public static int I2;
    public static long L1;
    public static double D1;
    public static float F1;
    public static String S1;
    public static volatile int V;
    public static int W;

    static {
        I1 = 42;
        I2 = 100 * 3 + 7;
        L1 = 123456789012345L;
        D1 = 0.5;
        F1 = 2.5f;
        S1 = "const-string-sviline";
        V = 7;
        W = 1;
        W = 2;
    }
}
)JAVA";

constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.svi;

public class Main {
    public static void main(String[] args) {
        int guard = args.length == 0 ? 1 : 2;
        System.out.println("i1=" + Const.I1);
        System.out.println("i2=" + Const.I2);
        System.out.println("l1=" + Const.L1);
        System.out.println("d1=" + Const.D1);
        System.out.println("f1=" + Const.F1);
        System.out.println("s1=" + Const.S1);
        System.out.println("v=" + Const.V);
        System.out.println("w=" + Const.W);
        System.out.println("alive-" + guard);
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

void writeClassBytes(const fs::path& path, const Bytes& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << "cannot write class: " << path.string();
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

/// 改写后 Main 中命中 (owner, name, desc) 的 GETSTATIC 站点数。
ca::usize getStaticSites(const Bytes& bytes, const char* owner, const char* name,
                         const char* desc)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return 0;
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    ca::usize sites = 0;
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (!jbc::ir::hasCode(*session, idx)) continue;
        auto promoted = jbc::ir::promoteMethod(*session, idx);
        EXPECT_TRUE(promoted.is_ok());
        if (promoted.is_err()) continue;
        auto cuRef = std::move(promoted).unwrap();
        for (const jbc::ir::InsnId id : cuRef->dom.order()) {
            const jbc::ir::Insn* in = cuRef->dom.peek(id);
            if (in == nullptr || in->op != jbc::core::Op::GETSTATIC) continue;
            jbc::ir::ClassSession::MemberSem ref;
            if (!session->memberOf(*in, ref)) continue;
            if (ref.owner == owner && ref.name == name && ref.desc == desc) ++sites;
        }
    }
    return sites;
}

}   // namespace

TEST(StaticValueInliningJvm, InlinedReadsRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin_svi";
    const fs::path outDir = gate / "out_svi";

    // ── 1. 源码 + javac 编译（双类一次编译）──
    writeSource(srcDir / "hb/jvm/svi/Const.java", kConstSrc);
    writeSource(srcDir / "hb/jvm/svi/Main.java", kMainSrc);
    const auto compiled = test::jdk::runBat(
        {test::jdk::q(tc.javac) + " -J-Xmx512m -encoding UTF-8 -d " + test::jdk::q(binDir) +
         " " + test::jdk::q(srcDir / "hb/jvm/svi/Const.java") + " " +
         test::jdk::q(srcDir / "hb/jvm/svi/Main.java")});
    ASSERT_EQ(compiled.exitCode, 0) << compiled.out;

    // ── 2. 原样本基线运行 ──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.svi.Main");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string baseline = test::jdk::normalizeOutput(orig.out);
    EXPECT_NE(baseline.find("i1=42"), std::string::npos);
    EXPECT_NE(baseline.find("s1=const-string-sviline"), std::string::npos);

    // ── 3. 链式跑：收集件 + 物化件 ──
    const Bytes constBefore = readClassBytes(binDir / "hb/jvm/svi/Const.class");
    const Bytes mainBefore  = readClassBytes(binDir / "hb/jvm/svi/Main.class");

    mjbase::model::ResourceContainer container;
    container.addResource("hb/jvm/svi/Const.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "hb/jvm/svi/Const.class", constBefore));
    container.addResource("hb/jvm/svi/Main.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "hb/jvm/svi/Main.class", mainBefore));

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::StaticValueCollection collector;
    EXPECT_TRUE(collector.run(container, gam).areAllPreserved());
    mjbase::pass::StaticValueInlining inliner;
    const auto pa = inliner.run(container, gam);
    ASSERT_TRUE(pa.changed()) << "物化件必须发生读点改写";

    // Const（写侧）字节不动；Main（读侧）字节变动。
    const auto* constRes =
        ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
            container.getResource("hb/jvm/svi/Const.class"));
    ASSERT_NE(constRes, nullptr);
    EXPECT_EQ(constRes->get(), constBefore);
    const auto* mainRes =
        ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
            container.getResource("hb/jvm/svi/Main.class"));
    ASSERT_NE(mainRes, nullptr);
    const Bytes mainAfter = mainRes->get();
    EXPECT_NE(mainAfter, mainBefore);

    // 读点面：已收集字段 GETSTATIC 消失；volatile（V）与多次写（W）保留。
    EXPECT_EQ(getStaticSites(mainAfter, "hb/jvm/svi/Const", "I1", "I"), 0u);
    EXPECT_EQ(getStaticSites(mainAfter, "hb/jvm/svi/Const", "L1", "J"), 0u);
    EXPECT_EQ(getStaticSites(mainAfter, "hb/jvm/svi/Const", "S1",
                             "Ljava/lang/String;"),
              0u);
    EXPECT_GE(getStaticSites(mainAfter, "hb/jvm/svi/Const", "V", "I"), 1u);
    EXPECT_GE(getStaticSites(mainAfter, "hb/jvm/svi/Const", "W", "I"), 1u);

    // ── 4. 改写产物真跑（-Xverify:all）：stdout 逐行等于基线 ──
    writeClassBytes(outDir / "hb/jvm/svi/Const.class", constRes->get());
    writeClassBytes(outDir / "hb/jvm/svi/Main.class", mainAfter);
    const auto rerun = test::jdk::runJavaMain(tc, outDir, "hb.jvm.svi.Main", {},
                                              {"-Xverify:all"});
    ASSERT_EQ(rerun.exitCode, 0) << rerun.out;
    EXPECT_EQ(test::jdk::normalizeOutput(rerun.out), baseline) << "行为面必须逐行恒等";
}
