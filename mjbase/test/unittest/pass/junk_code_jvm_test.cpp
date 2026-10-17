// JunkCodeInserter（B 批）+ A 批变换族在真 JVM 上的硬验收：
// javac 编译真实样本（V52，带 LNT/SMT）→ mjbase 管线（A 批四 pass + B 批垃圾
// 分支 + StrictTypeResolverAdapter 注入的 A1 帧链接线）→ 变换产物交真 java
// 执行，stdout 必须与原样本一致——A1 链再生（Strict 帧 → maxes 采用 → SMT
// 挂回）的帧被真验证器接受的直接证据。
//
// 样本覆盖面：循环/分支（垃圾插入点 + 帧重算）、try-catch（handler 帧）、
// Objects.requireNonNull 表达式语句（UnusedCheckCleaner）、char[] 字面构造
// （CharArrToStrSimplifier，含 SIPUSH 装载的 UTF-16 码元）、长/窄常量
// （RedundantLoadValEliminator）。JDK 不可用的机器整测跳过（硬验收门在
// 有 JDK 的环境强制成立）。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/analysis/smt_read.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/pass/char_arr_to_str_simplifier.hpp"
#include "mjbase/pass/junk_code_inserter.hpp"
#include "mjbase/pass/nop_remover.hpp"
#include "mjbase/pass/redundant_load_val_eliminator.hpp"
#include "mjbase/pass/unused_check_cleaner.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// 样本 1：循环 + 分支 + try/catch + requireNonNull 表达式语句。
constexpr const char* kProbe1Src = R"JAVA(
package hb.jvm;

public class Probe1 {
    public static void main(String[] args) {
        int acc = 0;
        for (int i = 1; i <= 10; i++) {
            try {
                if (i % 2 == 0) {
                    acc += i;
                } else {
                    acc -= 1;
                }
            } catch (RuntimeException e) {
                acc = -999;
            }
        }
        System.out.println(acc);
        System.out.println(check("hello"));
    }

    static int check(String s) {
        java.util.Objects.requireNonNull(s);
        return s.length() > 3 ? 1 : 0;
    }
}
)JAVA";

/// 样本 2：char[] 字面构造（含 SIPUSH 装载的非 ASCII 码元）+ 循环分支。
constexpr const char* kProbe2Src = R"JAVA(
package hb.jvm;

public class Probe2 {
    public static void main(String[] args) {
        String a = new String(new char[]{'m', 'o', 'r', 'p', 'h'});
        String b = new String(new char[]{0x4E2D, 0x6587});
        System.out.println(a);
        System.out.println(b);
        System.out.println(a.length() + b.length());
        long v = 1L;
        for (int i = 0; i < 20; i++) {
            v = v * 3 + (i % 2);
        }
        System.out.println(v);
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

/// 变换单类：A 批四 pass + JunkCodeInserter，管线注入 StrictTypeResolverAdapter。
/// 返回写回字节（管线抛错即测试失败——A1 链在真实样本上必须完整走通）。
Bytes transformClass(const Bytes& corpus)
{
    mjbase::KeyPool                       pool;
    mjbase::hierarchy::ClassHierarchy     hierarchy{pool};
    mjbase::spi::CommonSuperClassResolver resolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{resolver};

    mjbase::spi::PassPipelineOptions options;
    options.typeResolver = &typeResolver;

    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{corpus});
    EXPECT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    ca::str::Utf8StringArena           arena;
    mjbase::spi::MethodAnalysisManager mam(arena);
    mjbase::KeyPool                    identityPool;
    mjbase::spi::MethodPassManager     mpm;
    mpm.addPass(std::make_unique<mjbase::pass::NopRemover>());
    mpm.addPass(std::make_unique<mjbase::pass::RedundantLoadValEliminator>());
    mpm.addPass(std::make_unique<mjbase::pass::CharArrToStrSimplifier>());
    mpm.addPass(std::make_unique<mjbase::pass::UnusedCheckCleaner>());
    mpm.addPass(std::make_unique<mjbase::pass::JunkCodeInserter>());

    const mjbase::spi::PreservedAnalyses pa = mjbase::spi::runMethodPipelineOverSession(
        *session, mpm, mam, identityPool, nullptr, options);
    EXPECT_TRUE(pa.changed()) << "变换族在真实样本上未发生任何改动（触发面失效）";

    auto written = jbc::core::writeClass(session->backing());
    EXPECT_TRUE(written.is_ok());
    return std::move(written).unwrap();
}

}   // namespace

TEST(JunkCodeJvm, TransformedClassesRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin";
    const fs::path obfDir = gate / "obf";

    // ── 1. 源码 + javac 编译（V52，带 LNT/SMT）──
    writeSource(srcDir / "hb/jvm/Probe1.java", kProbe1Src);
    writeSource(srcDir / "hb/jvm/Probe2.java", kProbe2Src);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/Probe1.java", binDir, &compileErr))
        << compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/Probe2.java", binDir, &compileErr))
        << compileErr;

    // ── 2. 原样本基线运行 ──
    const auto orig1 = test::jdk::runJavaMain(tc, binDir, "hb.jvm.Probe1");
    ASSERT_EQ(orig1.exitCode, 0) << orig1.out;
    const std::string origOut1 = test::jdk::normalizeOutput(orig1.out);
    EXPECT_NE(origOut1.find("25"), std::string::npos);
    const auto orig2 = test::jdk::runJavaMain(tc, binDir, "hb.jvm.Probe2");
    ASSERT_EQ(orig2.exitCode, 0) << orig2.out;
    const std::string origOut2 = test::jdk::normalizeOutput(orig2.out);
    EXPECT_NE(origOut2.find("morph"), std::string::npos);

    // ── 3. 管线变换 + 落盘（包路径对齐 javac 布局）──
    for (const char* name : {"Probe1", "Probe2"}) {
        fprintf(stderr, "[jvm_gate] transforming %s\n", name);
        const Bytes  corpus = readClassBytes(binDir / "hb/jvm" / (std::string(name) + ".class"));
        const Bytes  out    = transformClass(corpus);
        fprintf(stderr, "[jvm_gate] transformed %s\n", name);
        const fs::path dst  = obfDir / "hb/jvm" / (std::string(name) + ".class");
        fs::create_directories(dst.parent_path());
        std::ofstream fout(dst, std::ios::binary);
        ASSERT_TRUE(fout.good()) << dst.string();
        fout.write(reinterpret_cast<const char*>(out.data()),
                   static_cast<std::streamsize>(out.size()));
    }

    // ── 4. A1 链再生证据：带分支方法的输出类携带可读回的 StackMapTable ──
    for (const char* name : {"Probe1", "Probe2"}) {
        const Bytes out = readClassBytes(obfDir / "hb/jvm" / (std::string(name) + ".class"));
        auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{out});
        ASSERT_TRUE(sessionResult.is_ok());
        auto session = std::move(sessionResult).unwrap();
        bool sawFrames = false;
        for (ca::usize mi = 0; mi < session->backing().methods.size(); ++mi) {
            auto promoted = jbc::ir::promoteMethod(*session, mi);
            if (promoted.is_err()) continue;
            auto cu   = std::move(promoted).unwrap();
            auto read = jbc::analysis::readFramesFromSmt(*session, *cu);
            ASSERT_TRUE(read.is_ok()) << name << " method #" << mi << ": SMT 不可读回";
            if (!std::move(read).unwrap().empty()) sawFrames = true;
        }
        EXPECT_TRUE(sawFrames) << name << ": 输出类无任何 StackMapTable 帧（A1 再生未生效）";
    }

    // ── 5. 硬验收：真 java 执行变换产物，行为与原样本一致 ──
    const auto new1 = test::jdk::runJavaMain(tc, obfDir, "hb.jvm.Probe1");
    EXPECT_EQ(new1.exitCode, 0) << "transformed Probe1 rejected/failed:\n" << new1.out;
    EXPECT_EQ(test::jdk::normalizeOutput(new1.out), origOut1);

    const auto new2 = test::jdk::runJavaMain(tc, obfDir, "hb.jvm.Probe2");
    EXPECT_EQ(new2.exitCode, 0) << "transformed Probe2 rejected/failed:\n" << new2.out;
    EXPECT_EQ(test::jdk::normalizeOutput(new2.out), origOut2);
}
