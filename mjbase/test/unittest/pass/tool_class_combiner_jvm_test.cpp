/// ToolClassCombiner（批 A 收口件）在真 JVM 上的硬验收：javac 编译多 static
/// 工具类样本（Main 调用两个工具类；工具类带分支方法与 static final 常量
/// 字段）→ 合并 → 产物目录真 java 执行搬移后的成员，stdout 与合并前等价；
/// 反射核对字段 owner 落在合并宿主；产物全量扫描零供体引用。JDK 不可用的
/// 机器整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/tool_class_combiner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// 单文件四顶层类：Main（带调用，稳定落选）+ UtilMath / UtilText（工具类，
/// 分支体 + static final 常量字段）+ ResidualProbe（反射核对字段 owner 与
/// 常量值——纯反射形态，不参与合并容器）。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.combine;

public class Main {
    public static void main(String[] args) {
        System.out.println(UtilMath.clamp(5));
        System.out.println(UtilMath.clamp(-3));
        System.out.println(UtilMath.clamp(1000));
        System.out.println(UtilText.pick(true));
        System.out.println(UtilText.pick(false));
    }
}

class UtilMath {
    public static final int LIMIT = 99;
    public static int clamp(int v) {
        if (v > LIMIT) {
            return LIMIT;
        }
        if (v < 0) {
            return 0;
        }
        return v;
    }
}

class UtilText {
    public static final String TAG = "tool-tag";
    public static String pick(boolean which) {
        String r;
        if (which) {
            r = "yes";
        } else {
            r = "no";
        }
        return r;
    }
}
)JAVA";

/// 反射核对：字段 owner 必须是合并宿主，常量值随迁（ConstantValue 重建的
/// 行为级证据）。宿主名确定：两个候选无撞名必入同一宿主 ToolClassMergedClass_0。
constexpr const char* kProbeSrc = R"JAVA(
package hb.jvm.combine;

public class ResidualProbe {
    public static void main(String[] args) throws Exception {
        Class<?> host = Class.forName("me.canrad.morpher.ToolClassMergedClass_0");
        System.out.println("limitOwner=" + host.getField("LIMIT").getDeclaringClass().getName());
        System.out.println("tagOwner=" + host.getField("TAG").getDeclaringClass().getName());
        System.out.println("limit=" + host.getField("LIMIT").get(null));
        System.out.println("tag=" + host.getField("TAG").get(null));
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

void writeClassFile(const fs::path& path, const Bytes& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << path.string();
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

/// 供体引用零残留：宿主/消费类全部方法体的 memberOf/classOf/字符串常量面。
void expectNoDonorRefs(const Bytes& classBytes)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(classBytes));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();
    for (auto& visit : jbc::ir::collectCodeUnits(*session)) {
        ASSERT_NE(visit.kind, jbc::ir::CodeVisit::Kind::Failed);
        if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
        const jbc::ir::CodeUnit& cu = *visit.unit.get();
        for (const jbc::ir::InsnId iid : cu.dom.order()) {
            const jbc::ir::Insn* in = cu.dom.peek(iid);
            ASSERT_NE(in, nullptr);
            jbc::ir::ClassSession::MemberSem sem;
            if (session->memberOf(*in, sem)) {
                EXPECT_NE(sem.owner, "hb/jvm/combine/UtilMath");
                EXPECT_NE(sem.owner, "hb/jvm/combine/UtilText");
            }
            const std::string_view cn = session->classOf(*in);
            if (!cn.empty()) {
                EXPECT_NE(cn, "hb/jvm/combine/UtilMath");
                EXPECT_NE(cn, "hb/jvm/combine/UtilText");
            }
            jbc::ir::ClassSession::ConstSem c;
            if (session->constOf(*in, c)
                && c.kind == jbc::ir::ClassSession::ConstSem::Kind::String) {
                const std::string literal(c.str);
                EXPECT_EQ(literal.find("hb.jvm.combine.Util"), std::string::npos)
                    << "残留供体字符串常量: " << literal;
            }
        }
    }
}

}   // namespace

TEST(ToolClassCombinerJvm, MergedToolClassesRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate    = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir  = gate / "src";
    const fs::path binDir  = gate / "bin_combine";
    const fs::path outDir  = gate / "obf_combine";

    // ── 1. 源码 + javac 编译（Main + UtilMath + UtilText + ResidualProbe）──
    writeSource(srcDir / "hb/jvm/combine/Main.java", kMainSrc);
    writeSource(srcDir / "hb/jvm/combine/ResidualProbe.java", kProbeSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/combine/Main.java", binDir, &compileErr))
        << compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/combine/ResidualProbe.java", binDir,
                                       &compileErr))
        << compileErr;

    // ── 2. 原样本基线运行 ──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.combine.Main");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    const std::string origOut = test::jdk::normalizeOutput(orig.out);
    EXPECT_NE(origOut.find("5"), std::string::npos);
    EXPECT_NE(origOut.find("0"), std::string::npos);
    EXPECT_NE(origOut.find("99"), std::string::npos);
    EXPECT_NE(origOut.find("yes"), std::string::npos);
    EXPECT_NE(origOut.find("no"), std::string::npos);

    // ── 3. 合并：Main（非候选）+ 两个工具类候选 ──
    mjbase::model::ResourceContainer container;
    for (const char* name : {"Main", "UtilMath", "UtilText"}) {
        const Bytes corpus =
            readClassBytes(binDir / "hb/jvm/combine" / (std::string(name) + ".class"));
        container.addResource(std::string("hb/jvm/combine/") + name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  std::string("hb/jvm/combine/") + name + ".class", corpus));
    }
    const Bytes mainBefore = readClassBytes(binDir / "hb/jvm/combine/Main.class");

    {
        ca::str::Utf8StringArena          arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        mjbase::pass::ToolClassCombiner   pass;
        const auto                        pa = pass.run(container, gam);
        ASSERT_TRUE(pa.changed()) << "合并未发生";
        ASSERT_FALSE(pa.areAllPreserved());
    }

    EXPECT_FALSE(container.hasResource("hb/jvm/combine/UtilMath.class"));
    EXPECT_FALSE(container.hasResource("hb/jvm/combine/UtilText.class"));
    const std::string hostUri = "me/canrad/morpher/ToolClassMergedClass_0.class";
    ASSERT_TRUE(container.hasResource(hostUri));
    const auto* mainRes = ca::core::tag_cast::cast<mjbase::model::ClassResource*>(
        container.getResource("hb/jvm/combine/Main.class"));
    const auto* hostRes =
        ca::core::tag_cast::cast<mjbase::model::ClassResource*>(container.getResource(hostUri));
    ASSERT_NE(mainRes, nullptr);
    ASSERT_NE(hostRes, nullptr);
    EXPECT_NE(mainRes->get(), mainBefore) << "Main 应有重映射改写面";

    // ── 4. 静态复核：宿主与消费类零供体引用 ──
    expectNoDonorRefs(mainRes->get());
    expectNoDonorRefs(hostRes->get());

    // ── 5. 硬验收：合并产物隔离落盘真 java 执行（宿主过 verifier、行为等价）
    //      + 反射核对字段 owner 与常量值随迁（probe 类随产物目录运行）──
    writeClassFile(outDir / "hb/jvm/combine/Main.class", mainRes->get());
    writeClassFile(outDir / "me/canrad/morpher/ToolClassMergedClass_0.class", hostRes->get());
    writeClassFile(outDir / "hb/jvm/combine/ResidualProbe.class",
                   readClassBytes(binDir / "hb/jvm/combine/ResidualProbe.class"));
    const auto merged = test::jdk::runJavaMain(tc, outDir, "hb.jvm.combine.Main");
    EXPECT_EQ(merged.exitCode, 0) << "merged Main rejected/failed:\n" << merged.out;
    EXPECT_EQ(test::jdk::normalizeOutput(merged.out), origOut);

    const auto probe = test::jdk::runJavaMain(tc, outDir, "hb.jvm.combine.ResidualProbe");
    ASSERT_EQ(probe.exitCode, 0) << probe.out;
    EXPECT_NE(probe.out.find("limitOwner=me.canrad.morpher.ToolClassMergedClass_0"),
              std::string::npos)
        << probe.out;
    EXPECT_NE(probe.out.find("tagOwner=me.canrad.morpher.ToolClassMergedClass_0"),
              std::string::npos)
        << probe.out;
    EXPECT_NE(probe.out.find("limit=99"), std::string::npos) << probe.out;
    EXPECT_NE(probe.out.find("tag=tool-tag"), std::string::npos) << probe.out;
}
