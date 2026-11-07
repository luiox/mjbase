/// IndySiteInliner 真 JVM 硬验收：javac 真样本三态（lambda / 字符串拼接 /
/// 普通自定义 indy——BSM 自带）+ 尾参洗牌与虚调用形态的 jasm 真跑件。前两类
/// 生成面字节不动；自定义 indy 站点经测试 resolver 内联后 -Xverify:all 真跑，
/// stdout 与原样本逐字节一致；幂等二跑 no-op。JDK 不可用整测跳过。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/str/utf16_string.hpp"

#include "jdk_fixture.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/indy_site_inliner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/indy_test_resolver.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

using mjbase::spi::PreservedAnalyses;
using mjbase::spi::ResolvedIndyTarget;
using TestResolver = TableIndyResolver;

/// jasm 源面 UTF-16 载体（jasm_service_test 同款局部助手）。
ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// javac 真样本：lambda + 字符串拼接（生成面两态）+ 三个 indy 站点的 BSM 与
/// 直接目标（jasm 夹具经 invokedynamic 引到这里）。
constexpr const char* kSampleSrc = R"JAVA(
import java.lang.invoke.ConstantCallSite;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.function.IntUnaryOperator;

public class Sample {
    public static int secret(int x) { return x * 3; }
    public static int secret2(int x) { return x + 100; }
    public static String vHello() { return "v-ok"; }
    public static Sample make() { return new Sample(); }
    public static IntUnaryOperator triple() { return x -> x * 7; }
    public static String concat(int x) { return "c" + x + "d"; }

    public static CallSite bsm(MethodHandles.Lookup lookup, String name, MethodType type)
            throws Throwable {
        return new ConstantCallSite(lookup.findStatic(
            Sample.class, "secret", MethodType.methodType(int.class, int.class)));
    }

    public static CallSite bsmDropping(MethodHandles.Lookup lookup, String name,
            MethodType type, String meta) throws Throwable {
        MethodHandle h = lookup.findStatic(
            Sample.class, "secret2", MethodType.methodType(int.class, int.class));
        return new ConstantCallSite(MethodHandles.dropArguments(h, 1, String.class));
    }

    public static CallSite bsmVirtual(MethodHandles.Lookup lookup, String name,
            MethodType type) throws Throwable {
        return new ConstantCallSite(
            lookup.findVirtual(Sample.class, "vHello", MethodType.methodType(String.class)));
    }
}
)JAVA";

constexpr std::string_view kBsm3Desc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;";
constexpr std::string_view kBsm4Desc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;Ljava/lang/String;)Ljava/lang/invoke/CallSite;";

/// 自定义 indy（exact 静态形）：站点 (I)I → Sample.secret(I)I。
constexpr std::string_view kCallerJasm =
    ".super java/lang/Object\n"
    ".class public super Caller {\n"
    "    .method public static scramble (I)I {\n"
    "        code: {\n"
    "            iload_0\n"
    "            invokedynamic scramble (I)I { invokestatic, Sample.bsm, "
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
    "Ljava/lang/invoke/CallSite; } { }\n"
    "            ireturn\n"
    "        }\n"
    "    }\n"
    "}\n";

/// 尾参洗牌态：站点 (I Ljava/lang/String;)I（尾参为 BSM 元数据）→
/// Sample.secret2(I)I，resolver 应答 trailing = 1。
constexpr std::string_view kMetaCallerJasm =
    ".super java/lang/Object\n"
    ".class public super MetaCaller {\n"
    "    .method public static decode (ILjava/lang/String;)I {\n"
    "        code: {\n"
    "            iload_0\n"
    "            ldc \"m\"\n"
    "            invokedynamic mdec (ILjava/lang/String;)I { invokestatic, Sample.bsmDropping, "
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;"
    "Ljava/lang/String;)Ljava/lang/invoke/CallSite; } { \"m\" }\n"
    "            ireturn\n"
    "        }\n"
    "    }\n"
    "}\n";

/// 虚调用形态：站点 (LSample;)Ljava/lang/String; → invokevirtual
/// Sample.vHello ()Ljava/lang/String;（接收者由 Sample.make 供给）。
constexpr std::string_view kVCallerJasm =
    ".super java/lang/Object\n"
    ".class public super VCaller {\n"
    "    .method public static greet ()Ljava/lang/String; {\n"
    "        code: {\n"
    "            invokestatic Sample.make ()LSample;\n"
    "            invokedynamic greet (LSample;)Ljava/lang/String; { invokestatic,"
    " Sample.bsmVirtual, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { }\n"
    "            areturn\n"
    "        }\n"
    "    }\n"
    "}\n";

/// Runner 调用面：三个 indy 站点 + lambda + 拼接的输出全部上 stdout。
constexpr const char* kRunnerBody =
    "System.out.println(Caller.scramble(5));"
    "System.out.println(MetaCaller.decode(5, \"m\"));"
    "System.out.println(VCaller.greet());"
    "System.out.println(Sample.triple().applyAsInt(4));"
    "System.out.println(Sample.concat(9));";

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

/// jasm 源装配成类字节（失败显红返回空）。
Bytes assemble(const std::string_view jasm)
{
    mjbase::jasm::JvmJasmService service;
    auto                         name   = utf16(std::string(jasm).substr(0, 12) + ".jasm");
    auto                         source = utf16(std::string(jasm));
    auto                         built  = service.assemble(name.ref(), source.ref());
    if (!built.isOk()) ADD_FAILURE() << "jasm assemble failed";
    if (!built.isOk()) return {};
    return built.value->bytes;
}

/// resolver 应答表（三站点 = 三态改写矩阵的 JVM 面）。
std::vector<TestResolver::Spec> specs()
{
    return {
        {std::string_view("scramble"), ResolvedIndyTarget::Form::Static,
         std::string_view("Sample"), std::string_view("secret"), std::string_view("(I)I"), 0,
         false},
        {std::string_view("mdec"), ResolvedIndyTarget::Form::Static,
         std::string_view("Sample"), std::string_view("secret2"), std::string_view("(I)I"), 1,
         false},
        {std::string_view("greet"), ResolvedIndyTarget::Form::Virtual,
         std::string_view("Sample"), std::string_view("vHello"),
         std::string_view("()Ljava/lang/String;"), 0, false},
    };
}

std::vector<std::pair<std::string, Bytes>> classSet(const Bytes& sample, const Bytes& caller,
                                                    const Bytes& metaCaller,
                                                    const Bytes& vCaller)
{
    return {
        {"Sample", sample}, {"Caller", caller}, {"MetaCaller", metaCaller}, {"VCaller", vCaller},
    };
}

}   // namespace

TEST(IndySiteInlinerJvm, InlinedSitesRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin_indy";

    // ── 1. javac 真样本 + jasm 真跑件 ──
    writeSource(srcDir / "Sample.java", kSampleSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "Sample.java", binDir, &compileErr))
        << compileErr;
    const Bytes sample     = readClassBytes(binDir / "Sample.class");
    const Bytes caller     = assemble(kCallerJasm);
    const Bytes metaCaller = assemble(kMetaCallerJasm);
    const Bytes vCaller    = assemble(kVCallerJasm);
    ASSERT_FALSE(sample.empty());
    ASSERT_FALSE(caller.empty());
    ASSERT_FALSE(metaCaller.empty());
    ASSERT_FALSE(vCaller.empty());

    // ── 2. 原样本基线运行（indy 原生走 BSM 链路）──
    test::jdk::RunnerSpec origSpec;
    origSpec.classes       = classSet(sample, caller, metaCaller, vCaller);
    origSpec.runnerBody    = kRunnerBody;
    origSpec.scratchPrefix = "mjbase_indy_orig_";
    const auto orig = test::jdk::runWithRunnerOutcome(tc, origSpec);
    ASSERT_EQ(orig.status, test::jdk::JvmRunStatus::Ok) << orig.output;
    const std::string origOut = orig.output;
    EXPECT_NE(origOut.find("15"), std::string::npos) << origOut;    // scramble(5) = 5*3
    EXPECT_NE(origOut.find("105"), std::string::npos) << origOut;   // decode(5) = 5+100
    EXPECT_NE(origOut.find("v-ok"), std::string::npos) << origOut;
    EXPECT_NE(origOut.find("28"), std::string::npos) << origOut;    // lambda 4*7
    EXPECT_NE(origOut.find("c9d"), std::string::npos) << origOut;   // concat

    // ── 3. pass 改写：三个自定义站点内联，Sample（lambda+concat）字节不动 ──
    mjbase::model::ResourceContainer container;
    for (const auto& [name, bytes] :
         classSet(sample, caller, metaCaller, vCaller)) {
        container.addResource(name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  name + ".class", bytes));
    }
    {
        std::vector<std::unique_ptr<mjbase::spi::IndyResolver>> resolvers;
        resolvers.push_back(std::make_unique<TestResolver>(specs()));
        mjbase::pass::IndySiteInliner     pass(std::move(resolvers));
        ca::str::Utf8StringArena          arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        const auto                        pa = pass.run(container, gam);
        ASSERT_FALSE(pa.areAllPreserved()) << "三个自定义站点应有改写面";
    }

    const auto* sampleRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("Sample.class"));
    const auto* callerRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("Caller.class"));
    const auto* metaRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("MetaCaller.class"));
    const auto* vRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("VCaller.class"));
    ASSERT_NE(sampleRes, nullptr);
    ASSERT_NE(callerRes, nullptr);
    ASSERT_NE(metaRes, nullptr);
    ASSERT_NE(vRes, nullptr);
    EXPECT_EQ(sampleRes->get(), sample) << "lambda/concat 生成面字节必须不动";
    EXPECT_NE(callerRes->get(), caller) << "scramble 站点应被内联";
    EXPECT_NE(metaRes->get(), metaCaller) << "mdec 站点应被内联（尾参洗牌）";
    EXPECT_NE(vRes->get(), vCaller) << "greet 站点应被内联";

    // ── 4. 幂等：二次运行严格 no-op ──
    {
        const Bytes settled = callerRes->get();
        std::vector<std::unique_ptr<mjbase::spi::IndyResolver>> resolvers;
        resolvers.push_back(std::make_unique<TestResolver>(specs()));
        mjbase::pass::IndySiteInliner     pass(std::move(resolvers));
        ca::str::Utf8StringArena          arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        const auto* again = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
            container.getResource("Caller.class"));
        ASSERT_NE(again, nullptr);
        EXPECT_EQ(again->get(), settled);
    }

    // ── 5. 硬验收：改写产物 -Xverify:all 真跑，stdout 与基线逐字节一致 ──
    test::jdk::RunnerSpec cleanedSpec;
    cleanedSpec.classes = classSet(sampleRes->get(), callerRes->get(), metaRes->get(),
                                   vRes->get());
    cleanedSpec.runnerBody    = kRunnerBody;
    cleanedSpec.scratchPrefix = "mjbase_indy_inlined_";
    const auto cleaned = test::jdk::runWithRunnerOutcome(tc, cleanedSpec);
    ASSERT_EQ(cleaned.status, test::jdk::JvmRunStatus::Ok) << cleaned.output;
    EXPECT_EQ(cleaned.output, origOut) << "内联后行为不等价";
}
