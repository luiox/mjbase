/// ClassRenamer（批 A）在真 JVM 上的硬验收：javac 编译双类样本（调用方引用
/// 被调方：方法调用 + 字段访问 + 类型引用 + 描述符内嵌类名）→ ClassRenamer
/// 全容器重命名 → 新名类真 java 执行，stdout 必须与原样本一致——重命名后
/// 类可加载、跨类引用改写语义完整的直接证据。JDK 不可用的机器整测跳过。
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/core/reader.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/class_renamer.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// 双类样本（单文件双顶层类，compileJava 无 -cp 面）：Main.main 调用 Util 的
/// 静态方法并读静态字段（跨类 method/field/描述符三类引用面）。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.rename;

public class Main {
    public static void main(String[] args) {
        System.out.println(Util.label());
        System.out.println(Util.magic);
        System.out.println(Util.greet("legacy"));
    }
}

class Util {
    public static int magic = 42;
    public static String label() {
        return "util-label";
    }
    public static String greet(String who) {
        return "hello " + who;
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

}   // namespace

TEST(ClassRenamerJvm, RenamedClassesRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin";
    const fs::path obfDir = gate / "obf_rename";

    // ── 1. 源码 + javac 编译（单文件双顶层类 → Main.class + Util.class）──
    writeSource(srcDir / "hb/jvm/rename/Main.java", kMainSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/rename/Main.java", binDir, &compileErr))
        << compileErr;

    // ── 2. 原样本基线运行 ──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.rename.Main");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut = test::jdk::normalizeOutput(orig.out);
    EXPECT_NE(origOut.find("util-label"), std::string::npos);
    EXPECT_NE(origOut.find("42"), std::string::npos);
    EXPECT_NE(origOut.find("hello legacy"), std::string::npos);

    // ── 3. ClassRenamer 全容器重命名 + 落盘到包路径对齐目录 ──
    mjbase::model::ResourceContainer container;
    for (const char* name : {"Main", "Util"}) {
        const Bytes corpus = readClassBytes(binDir / "hb/jvm/rename" / (std::string(name) + ".class"));
        container.addResource(std::string("hb/jvm/rename/") + name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  std::string("hb/jvm/rename/") + name + ".class", corpus));
    }
    {
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        mjbase::pass::ClassRenamer         pass;
        const auto                         pa = pass.run(container, gam);
        ASSERT_FALSE(pa.areAllPreserved()) << "重命名未发生";
    }
    for (const char* uri : {"hb/jvm/rename/Class0.class", "hb/jvm/rename/Class1.class"}) {
        const auto* res = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource(uri));
        ASSERT_NE(res, nullptr) << uri << " 缺失（重命名产物面破坏）";
        const fs::path dst = obfDir / uri;
        fs::create_directories(dst.parent_path());
        std::ofstream fout(dst, std::ios::binary);
        ASSERT_TRUE(fout.good()) << dst.string();
        fout.write(reinterpret_cast<const char*>(res->get().data()),
                   static_cast<std::streamsize>(res->get().size()));
    }

    // ── 4. 硬验收：新名类真 java 执行（main 未改名，跨类引用须全部随行）──
    const auto renamed = test::jdk::runJavaMain(tc, obfDir, "hb.jvm.rename.Class0");
    EXPECT_EQ(renamed.exitCode, 0) << "renamed Main rejected/failed:\n" << renamed.out;
    EXPECT_EQ(test::jdk::normalizeOutput(renamed.out), origOut);
}
