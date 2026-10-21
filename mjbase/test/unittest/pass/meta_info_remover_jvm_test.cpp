/// meta 元数据清理家族（批 B 13 件）在真 JVM 上的硬验收：javac -g -parameters
/// 编译样本（注解两态 / @Deprecated / 泛型继承桥方法 / final 常量 / 参数名）→
/// 家族 13 件经 registry 按名构造顺序全跑 → 产物目录真 java 执行，stdout 与
/// 原样本一致（行为面零变化）；另跑反射检查器钉死「剥离后」语义（注解清零 /
/// final 位消失 / 泛型父类退原始型 / bridge 标志消失——反射断言按剥离后口径
/// 调整，不再与原类一致）。JDK 不可用的机器整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "libca/core/tag_cast.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// 样本（Main.java 公共入口 + Parts.java 携带两注解类型 / 泛型基类子类对）：
/// 覆盖家族全部触发面——SourceFile（javac 恒带）、注解两族（RuntimeMark/
/// ClassOnlyMark/@Deprecated）、Signature（泛型）、final + ConstantValue、
/// 桥方法、LVT/LNT（-g）、MethodParameters（-parameters）。行为输出只打值，
/// 不打反射面（反射面按剥离后口径在 ReflectCheck 单独钉死）。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.metaclean;

import java.lang.annotation.*;

@Retention(RetentionPolicy.RUNTIME)
@interface RuntimeMark {}

@Retention(RetentionPolicy.CLASS)
@interface ClassOnlyMark {}

@Deprecated
class Util {
    @RuntimeMark @ClassOnlyMark
    public static final int CONSTANT = 7;
    public static String label() {
        return "meta-label";
    }
    public static <E> E pick(E v) {
        return v;
    }
}

class Base<T> {
    public T echo(T v) {
        return v;
    }
}

class Child extends Base<String> {
    @Override
    public String echo(String v) {
        return "child:" + v;
    }
}

public class Main {
    public static void main(String[] args) {
        System.out.println(Util.label());
        System.out.println(Util.CONSTANT);
        System.out.println(Util.pick("picked"));
        System.out.println(new Child().echo("hi"));
        System.out.println("done");
    }
}

class ReflectCheck {
    public static void main(String[] args) throws Exception {
        System.out.println("utilAnnos=" + Util.class.getDeclaredAnnotations().length);
        System.out.println(
            "constAnnos=" + Util.class.getDeclaredField("CONSTANT").getDeclaredAnnotations().length);
        System.out.println("constFinal=" + java.lang.reflect.Modifier.isFinal(
            Util.class.getDeclaredField("CONSTANT").getModifiers()));
        System.out.println("childSuperGeneric=" + (Child.class.getGenericSuperclass()
            instanceof java.lang.reflect.ParameterizedType));
        boolean bridge = false;
        for (java.lang.reflect.Method m : Child.class.getDeclaredMethods()) {
            if (m.isBridge() || m.isSynthetic()) bridge = true;
        }
        System.out.println("childBridgeOrSynthetic=" + bridge);
    }
}
)JAVA";

/// 清理后样本的期望反射面。
constexpr const char* kReflectExpected =
    "utilAnnos=0\n"
    "constAnnos=0\n"
    "constFinal=false\n"
    "childSuperGeneric=false\n"
    "childBridgeOrSynthetic=false\n";

/// 家族 13 件（注册名，批 B 全集）。
constexpr const char* kFamilyPasses[] = {
    "MetaInfoRemover",           "FinalRemover",
    "SyntheticBridgeRemover",    "KotlinMetadataRemover",
    "SourceInfoRemover",         "SignatureRemover",
    "IllegalAnnotationRemover",  "IllegalSignatureRemover",
    "IllegalTypeAnnotationRemover", "IllegalSourceInfoRemover",
    "UnknownAttributeCleaner",   "IllegalAccessRemover",
    "SpecificAnnotationRemover",
};

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

}   // namespace

TEST(MetaInfoRemoverJvm, CleanedClassesRunOnRealJvmWithStrippedReflection)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin_meta";
    const fs::path outDir = gate / "obf_metaclean";

    // ── 1. 源码 + javac -g -parameters 编译 ──
    writeSource(srcDir / "hb/jvm/metaclean/Main.java", kMainSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/metaclean/Main.java", binDir,
                                       &compileErr))
        << compileErr;

    // ── 2. 原样本基线运行 ──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.metaclean.Main");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut = test::jdk::normalizeOutput(orig.out);
    EXPECT_NE(origOut.find("meta-label"), std::string::npos);
    EXPECT_NE(origOut.find("child:hi"), std::string::npos);

    // ── 3. 家族 13 件全跑（registry 按名构造，逐类会话顺序过链）──
    const std::vector<std::string> sampleClasses = {"Main", "Util", "Base", "Child",
                                                    "RuntimeMark", "ClassOnlyMark"};
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    std::vector<std::pair<std::string, Bytes>> cleaned;
    bool                                       anyChanged = false;
    for (const std::string& name : sampleClasses) {
        const fs::path  classPath = binDir / "hb/jvm/metaclean" / (name + ".class");
        const Bytes     corpus    = readClassBytes(classPath);
        ASSERT_FALSE(corpus.empty()) << name;

        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
        ASSERT_TRUE(sessionResult.is_ok()) << name;
        auto session = std::move(sessionResult).unwrap();

        for (const char* passName : kFamilyPasses) {
            auto created = mjbase::spi::defaultPassRegistry().createClassPass(passName);
            ASSERT_TRUE(created.is_ok()) << passName;
            std::unique_ptr<mjbase::spi::ClassPass> pass = std::move(created).unwrap();
            const auto pa = pass->run(*session, cam);
            if (!pa.areAllPreserved()) anyChanged = true;
        }

        auto written = jbc::core::writeClass(session->backing());
        ASSERT_TRUE(written.is_ok()) << name;
        cleaned.emplace_back(name, std::move(written).unwrap());
    }
    EXPECT_TRUE(anyChanged) << "家族 13 件对 javac 样本应有改动面";

    // ── 4. 产物目录：全量原类 + 清理后样本类覆写 ──
    fs::remove_all(outDir);
    fs::copy(binDir, outDir, fs::copy_options::recursive);
    for (const auto& [name, bytes] : cleaned) {
        writeClassBytes(outDir / "hb/jvm/metaclean" / (name + ".class"), bytes);
    }

    // ── 5. 硬验收 A：清理产物真 java 执行，行为与原样本一致 ──
    const auto cleanedRun = test::jdk::runJavaMain(tc, outDir, "hb.jvm.metaclean.Main");
    EXPECT_EQ(cleanedRun.exitCode, 0) << "cleaned Main rejected/failed:\n" << cleanedRun.out;
    EXPECT_EQ(test::jdk::normalizeOutput(cleanedRun.out), origOut);

    // ── 6. 硬验收 B：反射面按剥离后口径钉死（不再与原类一致）──
    const auto reflect = test::jdk::runJavaMain(tc, outDir, "hb.jvm.metaclean.ReflectCheck");
    ASSERT_EQ(reflect.exitCode, 0) << reflect.out;
    EXPECT_EQ(test::jdk::normalizeOutput(reflect.out), kReflectExpected);
}
