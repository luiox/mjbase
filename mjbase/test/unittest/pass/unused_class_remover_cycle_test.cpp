// UnusedClassRemover opt-in 循环继承腐坏判据接线单测（判据件
// mjbase/analysis/cycle_class_scan，Recaf CycleClassRemovingTransformer
// 组合式吸收）。覆盖：opt-in 启用后环类入选删除集（含入口可达环成员）/
// 不启用时行为零变化（可达环全保、字节原样）/ 无根守卫对环类删除豁免 /
// keep 面只加不减（命中的环类照常保留）/ 幂等二跑 / 畸形容器 fail-closed
// 不受 opt-in 影响。夹具一律 jasm 文本装配（JvmJasmService），环引用直接
// 写 .super。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/core/tag_cast.hpp"
#include "libca/str/utf16_string.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/unused_member_remover.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/reference/resolved_call_graph.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

/// UTF-8 → UTF-16（jasm 服务入参口径）。
ca::str::Utf16String utf16Of(const std::string& s)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(s.data()), s.size()));
}

/// jasm 文本汇编为 class 字节（一下一个类；汇编失败即测试失败）。
Bytes assembleClass(const std::string& internalName, const std::string& superName,
                    const std::string& body)
{
    mjbase::jasm::JvmJasmService service;
    const std::string source = ".super " + superName +
                               "\n.class public super " + internalName + " {\n" +
                               body + "}\n";
    const auto sourceName = utf16Of(internalName + ".jasm");
    const auto sourceText = utf16Of(source);
    const auto result     = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败: " << internalName;
        return {};
    }
    return result.value->bytes;
}

/// 入口类 App：main → invokestatic target.m（环类可达性的根）。
Bytes entryFixture(const std::string& target)
{
    return assembleClass("App", "java/lang/Object",
                         "  .method public static main ([Ljava/lang/String;)V {\n"
                         "    code: {\n"
                         "      invokestatic " + target + ".m ()V\n"
                         "      return\n"
                         "    }\n"
                         "  }\n");
}

/// 环类成员：.super peer + public static m()V。
Bytes cycleMemberFixture(const std::string& name, const std::string& peer)
{
    return assembleClass(name, peer,
                         "  .method public static m ()V {\n"
                         "    code: {\n"
                         "      return\n"
                         "    }\n"
                         "  }\n");
}

/// 标准夹具：App(main→X.m) + X↔Y 环。全部类经 main→X 与 super 声明面可达。
void addReachableCycleFixture(mjbase::model::ResourceContainer& container)
{
    addClassResource(container, "App.class", entryFixture("X"));
    addClassResource(container, "X.class", cycleMemberFixture("X", "Y"));
    addClassResource(container, "Y.class", cycleMemberFixture("Y", "X"));
}

/// registerReachabilityAnalyses 对齐（六分析全注册）。
void registerReachabilityAnalyses(mjbase::spi::GlobalAnalysisManager& gam)
{
    gam.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    gam.registerAnalysis<mjbase::reference::CallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::HierarchyBundleAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedCallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::GlobalReachabilityAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedGlobalReachabilityAnalysis>();
}

/// 容器内 class 资源字节快照（uri → bytes）。
std::vector<std::pair<std::string, Bytes>> snapshotClassBytes(
    mjbase::model::ResourceContainer& container)
{
    std::vector<std::pair<std::string, Bytes>> out;
    container.visitResources([&out](std::string_view uri,
                                    const mjbase::model::IResource& res) {
        const auto* cls = ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(&res);
        if (cls == nullptr) return;
        out.emplace_back(std::string(uri), cls->get());
    });
    return out;
}

/// 断言快照中的资源仍在容器且字节恒等（幸存类零改写）。
void expectSurvivedUnchanged(mjbase::model::ResourceContainer&            container,
                             const std::vector<std::pair<std::string, Bytes>>& snapshot,
                             const std::string&                                uri)
{
    ASSERT_TRUE(container.hasResource(uri)) << uri;
    for (const auto& [snapUri, bytes] : snapshot) {
        if (snapUri != uri) continue;
        auto* resource = container.getResource(uri);
        ASSERT_NE(resource, nullptr) << uri;
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(resource);
        ASSERT_NE(cls, nullptr) << uri;
        EXPECT_EQ(cls->get(), bytes) << uri;
        return;
    }
    ADD_FAILURE() << "快照缺 " << uri;
}

}   // namespace

// 用例：opt-in 启用后环类入选删除集——入口可达的环成员（X 被 main 调、Y 是
// X 的 super 声明面可达）也删除，判据独立于可达性；入口 App 原样。
TEST(MjPassUnusedClassRemoverCycleTest, OptInRemovesReachableCycleMembers)
{
    mjbase::model::ResourceContainer container;
    addReachableCycleFixture(container);

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);
    mjbase::pass::UnusedClassRemover pass(nullptr, true);
    const auto                       pa = pass.run(container, gam);

    EXPECT_FALSE(pa.areAllPreserved());
    EXPECT_TRUE(pa.changed());
    EXPECT_FALSE(container.hasResource("X.class"));
    EXPECT_FALSE(container.hasResource("Y.class"));
    EXPECT_TRUE(container.hasResource("App.class"));
}

// 用例：不启用（默认构造）时行为零变化——同一夹具全部类经可达性闭包保留，
// 容器无任何删除、幸存字节原样（wiring off = 既有语义逐字节一致）。
TEST(MjPassUnusedClassRemoverCycleTest, DefaultRunKeepsReachableCycleByteIdentical)
{
    mjbase::model::ResourceContainer container;
    addReachableCycleFixture(container);
    const auto before = snapshotClassBytes(container);

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);
    mjbase::pass::UnusedClassRemover pass;
    const auto                       pa = pass.run(container, gam);

    EXPECT_FALSE(pa.changed());
    EXPECT_EQ(container.size(), before.size());
    for (const auto& [uri, bytes] : before) {
        expectSurvivedUnchanged(container, before, uri);
        (void)bytes;
    }
}

// 用例：无根 library 罐——默认档「无根整 pass 保留」照旧；opt-in 后环类删除
// 不受该守卫门控（环类装载必败，判据独立于可达性）。
TEST(MjPassUnusedClassRemoverCycleTest, OptInRemovesCycleInRootlessLibraryJar)
{
    // 默认档：无根，整 pass 保留。
    {
        mjbase::model::ResourceContainer container;
        addClassResource(container, "X.class", cycleMemberFixture("X", "Y"));
        addClassResource(container, "Y.class", cycleMemberFixture("Y", "X"));

        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);
        mjbase::pass::UnusedClassRemover pass;
        const auto                       pa = pass.run(container, gam);

        EXPECT_FALSE(pa.changed());
        EXPECT_EQ(container.size(), 2);
    }
    // opt-in：环类删除豁免无根守卫。
    {
        mjbase::model::ResourceContainer container;
        addClassResource(container, "X.class", cycleMemberFixture("X", "Y"));
        addClassResource(container, "Y.class", cycleMemberFixture("Y", "X"));

        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        registerReachabilityAnalyses(gam);
        mjbase::pass::UnusedClassRemover pass(nullptr, true);
        const auto                       pa = pass.run(container, gam);

        EXPECT_TRUE(pa.changed());
        EXPECT_EQ(container.size(), 0);
    }
}

// 用例：keep 面只加不减——服务文件 keep 命中的环类在 opt-in 下照常保留
// （keep 守卫优先于腐坏判据），环伙伴（非 keep）删除。
TEST(MjPassUnusedClassRemoverCycleTest, OptInKeepGuardPreservesKeptCycleClass)
{
    mjbase::model::ResourceContainer container;
    addReachableCycleFixture(container);
    const auto before = snapshotClassBytes(container);
    // META-INF/services：服务名与提供者内容行均进资源 keep 名单，内容行点名 X。
    const std::string service = "com.example.X\n";
    container.addResource(
        "META-INF/services/com.example.X",
        std::make_unique<mjbase::model::UnknownResource>(
            "META-INF/services/com.example.X",
            std::vector<ca::u8>(service.begin(), service.end())));

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);
    mjbase::pass::UnusedClassRemover pass(nullptr, true);
    const auto                       pa = pass.run(container, gam);

    EXPECT_TRUE(pa.changed());
    // keep 命中的环类 X 照常保留（字节原样）；非 keep 的环伙伴 Y 删除。
    expectSurvivedUnchanged(container, before, "X.class");
    EXPECT_TRUE(container.hasResource("App.class"));
    EXPECT_TRUE(container.hasResource("META-INF/services/com.example.X"));
    EXPECT_FALSE(container.hasResource("Y.class"));
}

// 用例：opt-in 二跑幂等——首轮删除环类后，二跑无环可报、无根也无删（all()）。
TEST(MjPassUnusedClassRemoverCycleTest, OptInSecondRunIsIdempotent)
{
    mjbase::model::ResourceContainer container;
    addReachableCycleFixture(container);

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);
    mjbase::pass::UnusedClassRemover pass(nullptr, true);
    const auto                       first  = pass.run(container, gam);
    EXPECT_TRUE(first.changed());

    const auto before = snapshotClassBytes(container);
    const auto second = pass.run(container, gam);
    EXPECT_FALSE(second.changed());
    EXPECT_EQ(container.size(), before.size());
    for (const auto& [uri, bytes] : before) {
        expectSurvivedUnchanged(container, before, uri);
        (void)bytes;
    }
}

// 用例：opt-in 不改容器级 fail-closed 原子性——畸形类在场时整 pass 放弃
// （返回 all()，无部分删除，环类也不删）。
TEST(MjPassUnusedClassRemoverCycleTest, OptInStillFailsClosedOnMalformedContainer)
{
    mjbase::model::ResourceContainer container;
    addReachableCycleFixture(container);
    addClassResource(container, "Bad.class", Bytes{0xCA, 0xFE, 0xBA, 0x00});

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    registerReachabilityAnalyses(gam);
    mjbase::pass::UnusedClassRemover pass(nullptr, true);
    const auto                       pa = pass.run(container, gam);

    EXPECT_FALSE(pa.changed());
    EXPECT_EQ(container.size(), 4);
}
