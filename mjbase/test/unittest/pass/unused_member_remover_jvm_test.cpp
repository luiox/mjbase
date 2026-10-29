/// 未使用成员清理家族（批 A 容器级四件）在真 JVM 上的硬验收：javac 编译三件
/// 样本（Main→Util 跨类调用 + Util 携带不可达 private 字段/方法 + 全容器外
/// 类 Dead）→ 家族四件全跑（类删除 + 成员删除改写）→ 产物目录真 java 执行，
/// stdout 必须与原样本一致——删除集正确、改写类可加载可校验的直接证据。
/// JDK 不可用的机器整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/reference/resolved_call_graph.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/unused_member_remover.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// 双顶层类单文件：Main.main 调 Util.label() 并读 Util.live；Util 带不可达
/// private static 字段 deadField（无初始化器——clinit 不得携带其 PUTSTATIC，
/// 否则字段经 <clinit> 根可达）与 private static 方法 deadMethod（仅被
/// deadMethod 引用的 deadField 随方法一起不可达）。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.umclean;

public class Main {
    public static void main(String[] args) {
        System.out.println(Util.label());
        System.out.println(Util.live);
    }
}

class Util {
    public static int live = 7;
    private static int deadField;
    private static String deadMethod() {
        return "never-called-" + deadField;
    }
    public static String label() {
        return "util-label";
    }
}
)JAVA";

/// 全容器外类：无任何引用，UnusedClassRemover 删除集。
constexpr const char* kDeadSrc = R"JAVA(
package hb.jvm.umclean;

class Dead {
    static int touch() {
        return 1;
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

void registerReachability(mjbase::spi::GlobalAnalysisManager& gam)
{
    gam.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    gam.registerAnalysis<mjbase::reference::CallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::HierarchyBundleAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedCallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::GlobalReachabilityAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedGlobalReachabilityAnalysis>();
}

/// 类内是否存在指定名字的成员（方法/字段面）。
bool hasMember(const jbc::core::ClassFile& cf, bool method, const char* name)
{
    const std::vector<jbc::core::MemberInfo>& members = method ? cf.methods : cf.fields;
    for (const jbc::core::MemberInfo& m : members) {
        if (cf.nameOf(m) == name) return true;
    }
    return false;
}

/// 解析 class 字节（失败返回空指针，调用方致命断言）。
std::unique_ptr<jbc::core::ClassFile> parseDom(const Bytes& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(read.is_ok()) << "libjbc readClass failed";
    if (read.is_err()) return nullptr;
    return std::move(read).unwrap().file;
}

}   // namespace

TEST(UnusedMemberRemoverJvm, CleanedClassesRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin_um";
    const fs::path outDir = gate / "obf_umclean";

    // ── 1. 源码 + javac 编译（Main.class + Util.class + Dead.class）──
    writeSource(srcDir / "hb/jvm/umclean/Main.java", kMainSrc);
    writeSource(srcDir / "hb/jvm/umclean/Dead.java", kDeadSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/umclean/Main.java", binDir, &compileErr))
        << compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/umclean/Dead.java", binDir, &compileErr))
        << compileErr;

    // ── 2. 原样本基线运行 ──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.umclean.Main");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut = test::jdk::normalizeOutput(orig.out);
    EXPECT_NE(origOut.find("util-label"), std::string::npos);
    EXPECT_NE(origOut.find("7"), std::string::npos);

    // ── 3. 家族四件全跑：类删除（Dead）+ 成员删除改写（Util）──
    mjbase::model::ResourceContainer container;
    for (const char* name : {"Main", "Util", "Dead"}) {
        const Bytes corpus =
            readClassBytes(binDir / "hb/jvm/umclean" / (std::string(name) + ".class"));
        container.addResource(std::string("hb/jvm/umclean/") + name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  std::string("hb/jvm/umclean/") + name + ".class", corpus));
    }
    const Bytes mainBefore = readClassBytes(binDir / "hb/jvm/umclean/Main.class");
    const Bytes utilBefore = readClassBytes(binDir / "hb/jvm/umclean/Util.class");

    {
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachability(gam);

        mjbase::pass::UnusedClassRemover        classPass;
        mjbase::pass::UnusedFieldRemover        fieldPass;
        mjbase::pass::GlobalUnusedMethodRemover methodPass;
        ASSERT_FALSE(classPass.run(container, gam).areAllPreserved()) << "Dead 未被删除";
        ASSERT_FALSE(fieldPass.run(container, gam).areAllPreserved()) << "deadField 未被删除";
        ASSERT_FALSE(methodPass.run(container, gam).areAllPreserved()) << "deadMethod 未被删除";
    }

    EXPECT_FALSE(container.hasResource("hb/jvm/umclean/Dead.class")) << "Dead 应被删除";
    const auto* mainRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/umclean/Main.class"));
    const auto* utilRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/umclean/Util.class"));
    ASSERT_NE(mainRes, nullptr);
    ASSERT_NE(utilRes, nullptr);
    EXPECT_EQ(mainRes->get(), mainBefore) << "Main 不应有改写面";
    EXPECT_NE(utilRes->get(), utilBefore) << "Util 应被成员删除改写";

    // ── 4. 硬验收：清理产物目录真 java 执行（改写类须过 verify，删除类不得
    //      缺席引用）──
    const fs::path dst = outDir / "hb/jvm/umclean";
    fs::create_directories(dst);
    for (const char* name : {"Main.class", "Util.class"}) {
        const auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource(std::string("hb/jvm/umclean/") + name));
        ASSERT_NE(res, nullptr) << name;
        std::ofstream fout(dst / name, std::ios::binary);
        ASSERT_TRUE(fout.good()) << (dst / name).string();
        fout.write(reinterpret_cast<const char*>(res->get().data()),
                   static_cast<std::streamsize>(res->get().size()));
    }
    const auto cleaned = test::jdk::runJavaMain(tc, outDir, "hb.jvm.umclean.Main");
    EXPECT_EQ(cleaned.exitCode, 0) << "cleaned Main rejected/failed:\n" << cleaned.out;
    EXPECT_EQ(test::jdk::normalizeOutput(cleaned.out), origOut);
}

/// 非私有档位样本：Main 只用 Util.label()（public 面）；Util 携带 package-private
/// 与 protected 死成员（字段全部无初始化器——<clinit>/<init> 不得携带写入面，
/// 否则成员经保留面根可达；方法体只成簇互相引用，两面各删各面后无悬垂）。
constexpr const char* kNonPrivateSrc = R"JAVA(
package hb.jvm.umnpriv;

public class Main {
    public static void main(String[] args) {
        System.out.println(Util.label());
    }
}

class Util {
    public static String label() {
        return "npriv-label";
    }
    static int deadPackageField;
    static String deadPackageMethod() {
        return "never-" + deadPackageField;
    }
    protected int deadProtectedField;
    protected void deadProtectedMethod() {
    }
}
)JAVA";

// 非私有档（默认 NonPublic）：package-private/protected 死成员删除，public 面
// 保留；清理产物真 JVM 执行 stdout 与原样本一致。PrivateOnly 对照档：同一容器
// 上非私有死成员保留（不产生任何改写）。
TEST(UnusedMemberRemoverJvm, NonPrivateDeadMembersRemovedAndCleanedOutputRunsOnJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin_umnpriv";
    const fs::path outDir = gate / "obf_umnpriv";

    writeSource(srcDir / "hb/jvm/umnpriv/Main.java", kNonPrivateSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/umnpriv/Main.java", binDir, &compileErr))
        << compileErr;

    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.umnpriv.Main");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut = test::jdk::normalizeOutput(orig.out);
    EXPECT_NE(origOut.find("npriv-label"), std::string::npos);

    const auto buildContainer = [&]() {
        mjbase::model::ResourceContainer container;
        for (const char* name : {"Main", "Util"}) {
            const Bytes corpus =
                readClassBytes(binDir / "hb/jvm/umnpriv" / (std::string(name) + ".class"));
            container.addResource(std::string("hb/jvm/umnpriv/") + name + ".class",
                                  std::make_unique<mjbase::model::ClassResource>(
                                      std::string("hb/jvm/umnpriv/") + name + ".class", corpus));
        }
        return container;
    };
    const Bytes utilBefore = readClassBytes(binDir / "hb/jvm/umnpriv/Util.class");

    {
        // 默认档：两面各删 package-private/protected 死成员。
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachability(gam);
        mjbase::model::ResourceContainer container = buildContainer();

        mjbase::pass::UnusedFieldRemover        fieldPass;
        mjbase::pass::GlobalUnusedMethodRemover methodPass;
        ASSERT_FALSE(fieldPass.run(container, gam).areAllPreserved()) << "死字段未被删除";
        ASSERT_FALSE(methodPass.run(container, gam).areAllPreserved()) << "死方法未被删除";

        const auto* utilRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource("hb/jvm/umnpriv/Util.class"));
        const auto* mainRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource("hb/jvm/umnpriv/Main.class"));
        ASSERT_NE(utilRes, nullptr);
        ASSERT_NE(mainRes, nullptr);
        EXPECT_EQ(mainRes->get(), readClassBytes(binDir / "hb/jvm/umnpriv/Main.class"))
            << "Main 不应有改写面";
        {
            const std::unique_ptr<jbc::core::ClassFile> utilOut = parseDom(utilRes->get());
            ASSERT_NE(utilOut, nullptr);
            EXPECT_FALSE(hasMember(*utilOut, true, "deadPackageMethod"));
            EXPECT_FALSE(hasMember(*utilOut, true, "deadProtectedMethod"));
            EXPECT_FALSE(hasMember(*utilOut, false, "deadPackageField"));
            EXPECT_FALSE(hasMember(*utilOut, false, "deadProtectedField"));
            EXPECT_TRUE(hasMember(*utilOut, true, "label"));
        }

        // 清理产物真 java 执行。
        const fs::path dst = outDir / "hb/jvm/umnpriv";
        fs::create_directories(dst);
        for (const char* name : {"Main.class", "Util.class"}) {
            const auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
                container.getResource(std::string("hb/jvm/umnpriv/") + name));
            ASSERT_NE(res, nullptr) << name;
            std::ofstream fout(dst / name, std::ios::binary);
            ASSERT_TRUE(fout.good()) << (dst / name).string();
            fout.write(reinterpret_cast<const char*>(res->get().data()),
                       static_cast<std::streamsize>(res->get().size()));
        }
        const auto cleaned = test::jdk::runJavaMain(tc, outDir, "hb.jvm.umnpriv.Main");
        EXPECT_EQ(cleaned.exitCode, 0) << "cleaned Main rejected/failed:\n" << cleaned.out;
        EXPECT_EQ(test::jdk::normalizeOutput(cleaned.out), origOut);
    }
    {
        // PrivateOnly 对照档：非私有死成员保留，Util 无改写面。
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachability(gam);
        mjbase::model::ResourceContainer container = buildContainer();

        mjbase::pass::UnusedFieldRemover fieldPass{
            nullptr, mjbase::pass::UnusedMemberAccessTier::PrivateOnly};
        mjbase::pass::GlobalUnusedMethodRemover methodPass{
            nullptr, mjbase::pass::UnusedMemberAccessTier::PrivateOnly};
        EXPECT_TRUE(fieldPass.run(container, gam).areAllPreserved());
        EXPECT_TRUE(methodPass.run(container, gam).areAllPreserved());

        const auto* utilRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource("hb/jvm/umnpriv/Util.class"));
        ASSERT_NE(utilRes, nullptr);
        EXPECT_EQ(utilRes->get(), utilBefore) << "PrivateOnly 档不应改写 Util";
    }
}
