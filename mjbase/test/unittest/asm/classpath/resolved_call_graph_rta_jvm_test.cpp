/// @file        resolved_call_graph_rta_jvm_test.cpp
/// @brief       RTA 精度档在真 JVM 样本上的硬验收：javac 编译多态样本（基类
///              覆写 + 接口分派）→ 原样本真 java 执行确认基线 → buildRta 在
///              真实 classfile 上验证实例化集、收窄边与 keep_roots 保守根喂入。
///              JDK 不可用的机器整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/asm/classpath/resolved_call_graph.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/keep_roots.hpp"
#include "mjbase/reference/reference_index.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// 基类覆写 + 接口分派的最小多态样本：Child/Impl 被实例化，Base/Iface 从不
/// 实例化——RTA 的收窄面与「已实例化 ≠ 声明类」在真实字节码上同框。
constexpr const char* kMainSrc = R"JAVA(
package hb.jvm.rta;

public class Main {
    public static void main(String[] args) {
        Base b = new Child();
        b.run();
        serve(new Impl());
    }

    static void serve(Iface i) {
        i.run();
    }
}

class Base {
    public void run() {}
}

class Child extends Base {
    public void run() {}
}

interface Iface {
    void run();
}

class Impl implements Iface {
    public void run() {}
}
)JAVA";

Bytes readClassBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot read sample class: " << path.string();
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}   // namespace

TEST(MjResolvedCallGraphRtaJvm, rtaProfileKeepsNarrowedEdgesOnRealClasses)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin_rta";

    // ── 1. 源码 + javac 编译（Main/Base/Child/Iface/Impl 五个 classfile）──
    fs::create_directories(srcDir / "hb/jvm/rta");
    {
        std::ofstream out(srcDir / "hb/jvm/rta/Main.java", std::ios::binary);
        ASSERT_TRUE(out.good()) << "cannot write sample source";
        out << kMainSrc;
    }
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/rta/Main.java", binDir, &compileErr))
        << compileErr;

    // ── 2. 原样本基线真跑（样本语义健全的直接证据）──
    const auto baseline = test::jdk::runJavaMain(tc, binDir, "hb.jvm.rta.Main");
    ASSERT_EQ(baseline.exitCode, 0) << baseline.out;

    // ── 3. 容器装载 + 分析三件套 + RTA 构建 ──
    mjbase::model::ResourceContainer container;
    std::vector<Bytes>               classFiles;
    for (const char* name : {"Main", "Base", "Child", "Iface", "Impl"}) {
        const Bytes corpus =
            readClassBytes(binDir / "hb/jvm/rta" / (std::string(name) + ".class"));
        classFiles.push_back(corpus);
        container.addResource(std::string("hb/jvm/rta/") + name + ".class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  std::string("hb/jvm/rta/") + name + ".class", corpus));
    }

    mjbase::reference::ReferenceIndex index;
    index.build(container);

    mjbase::asm_classpath::CallGraphBuilder builder;
    for (const Bytes& bytes : classFiles) {
        ASSERT_TRUE(builder.addClassBytes(jbc::core::ByteView{bytes.data(), bytes.size()}).is_ok());
    }
    auto built = builder.build();
    ASSERT_TRUE(built.is_ok());
    mjbase::asm_classpath::CallGraph callGraph = std::move(built).unwrap();

    auto hierarchy = mjbase::asm_classpath::HierarchyBundle::build(container);

    mjbase::asm_classpath::ResolvedCallGraph rtaGraph;
    mjbase::asm_classpath::RtaOptions        options;
    rtaGraph.buildRta(index, callGraph, *hierarchy, options);

    const std::string mainDesc = "([Ljava/lang/String;)V";
    // ── 4. 实例化集：只有真被 new 的 Child/Impl，声明类 Base/Iface 不在集 ──
    EXPECT_EQ(rtaGraph.rtaInstantiatedCount(), 2u);
    EXPECT_TRUE(rtaGraph.hasResolvedEdge("hb/jvm/rta/Main", "main", mainDesc,
                                         "hb/jvm/rta/Child", "<init>", "()V"));
    EXPECT_TRUE(rtaGraph.hasResolvedEdge("hb/jvm/rta/Main", "main", mainDesc,
                                         "hb/jvm/rta/Impl", "<init>", "()V"));

    // ── 5. 收窄边：覆写停链（Child.run），父类/接口 abstract 条目不出边 ──
    EXPECT_TRUE(rtaGraph.hasResolvedEdge("hb/jvm/rta/Main", "main", mainDesc,
                                         "hb/jvm/rta/Child", "run", "()V"));
    EXPECT_FALSE(rtaGraph.hasResolvedEdge("hb/jvm/rta/Main", "main", mainDesc,
                                          "hb/jvm/rta/Base", "run", "()V"));
    EXPECT_TRUE(rtaGraph.hasResolvedEdge("hb/jvm/rta/Main", "serve", "(Lhb/jvm/rta/Iface;)V",
                                         "hb/jvm/rta/Impl", "run", "()V"));
    EXPECT_FALSE(rtaGraph.hasResolvedEdge("hb/jvm/rta/Main", "serve", "(Lhb/jvm/rta/Iface;)V",
                                          "hb/jvm/rta/Iface", "run", "()V"));

    // ── 6. keep_roots 喂入：已实例化声明类成为保守根，Base/Iface 不入根 ──
    mjbase::reference::GlobalReachabilityOptions reachOptions;
    mjbase::reference::applyRtaInstantiatedRoots(rtaGraph, index, reachOptions);
    ASSERT_EQ(reachOptions.rootClasses.size(), 2u);
    EXPECT_EQ(reachOptions.rootClasses[0], "hb/jvm/rta/Child");
    EXPECT_EQ(reachOptions.rootClasses[1], "hb/jvm/rta/Impl");

    // ── 7. CHA 对照：同一站点 CHA 展开出 Base.run（RTA 收窄的真实差异）──
    mjbase::asm_classpath::ResolvedCallGraph chaGraph;
    chaGraph.build(index, callGraph, *hierarchy);
    EXPECT_TRUE(chaGraph.hasResolvedEdge("hb/jvm/rta/Main", "main", mainDesc,
                                         "hb/jvm/rta/Base", "run", "()V"));
    EXPECT_EQ(chaGraph.rtaInstantiatedCount(), 0u);
}
