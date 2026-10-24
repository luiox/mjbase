/// @file        keep_roots_rta_test.cpp
/// @brief       RTA 已实例化类 → 保守根（applyRtaInstantiatedRoots）用例：
///              已实例化声明类入根、外部类型不入根、纯增量与视图指向 RTA 图
///              自有池（生命周期契约）、CHA 图无产物（恒等）、可达性闭环
///              （根真进闭包，不只是填 vector）。
///
/// 闭环口径：RTA 产出是保守根来源，不是删除依据——根进三槽后由既有可达性
/// 闭包消费，RTA 收窄只体现在边表面。
#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/asm/classpath/resolved_call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/keep_roots.hpp"
#include "mjbase/reference/reference_index.hpp"

using jbc::core::Op;
using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::asm_classpath::HierarchyBundle;
using mjbase::asm_classpath::ResolvedCallGraph;
using mjbase::asm_classpath::RtaOptions;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::GlobalReachability;
using mjbase::reference::GlobalReachabilityCallExpansion;
using mjbase::reference::GlobalReachabilityOptions;
using mjbase::reference::MethodRefKey;
using mjbase::reference::ReferenceIndex;
using mjbase::reference::applyRtaInstantiatedRoots;
using mjbase::reference::test_support::Bytes;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::CodeEmit;
using mjbase::reference::test_support::MethodSpec;

namespace {

constexpr ca::u16 kAccPublic = 0x0001;
constexpr ca::u16 kAccSuper  = 0x0020;
constexpr ca::u16 kAccStatic = 0x0008;

MethodSpec m(std::string_view name, std::string_view desc, ca::u16 maxStack, ca::u16 maxLocals,
             std::function<void(CodeEmit&)> emit, ca::u16 access = kAccPublic | kAccStatic)
{
    MethodSpec out;
    out.access    = access;
    out.name      = name;
    out.desc      = desc;
    out.maxStack  = maxStack;
    out.maxLocals = maxLocals;
    out.emit      = std::move(emit);
    return out;
}

Bytes instanceClass(std::string_view name, std::vector<MethodSpec> extras)
{
    ClassBytesBuilder b(name);
    b.setAccess(kAccPublic | kAccSuper);
    std::vector<MethodSpec> ms;
    bool hasCtor = false;
    for (const MethodSpec& extra : extras) {
        if (extra.name == "<init>") hasCtor = true;
    }
    if (!hasCtor) {
        ms.push_back(m("<init>", "()V", 1, 1,
                       [](CodeEmit& e) {
                           e.varInsn(Op::ALOAD, 0);
                           e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
                           e.op(Op::RETURN);
                       },
                       kAccPublic));
    }
    for (auto& extra : extras) ms.push_back(std::move(extra));
    return b.assemble(ms);
}

/// kr 夹具：App.main 实例化 kr/Used（并调用它）+ classpath 外实例化
/// java/util/ArrayList；kr/Unused 无任何引用。
struct KeepRtaFixture
{
    ResourceContainer         container;
    std::vector<Bytes>        classFiles;
    ReferenceIndex            index;
    CallGraph                 callGraph;
    ResolvedCallGraph         rtaGraph;

    KeepRtaFixture()
    {
        addClass("kr/Used", instanceClass("kr/Used", {m(
            "hello", "()V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); })}));
        addClass("kr/Unused", instanceClass("kr/Unused", {m(
            "ghost", "()V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); })}));
        addClass("kr/App", instanceClass("kr/App", {m(
            "main", "([Ljava/lang/String;)V", 2, 1,
            [](CodeEmit& e) {
                e.typeInsn(Op::NEW, "kr/Used");
                e.op(Op::DUP);
                e.invoke(Op::INVOKESPECIAL, "kr/Used", "<init>", "()V");
                e.invoke(Op::INVOKESTATIC, "kr/Used", "hello", "()V");
                e.typeInsn(Op::NEW, "java/util/ArrayList");
                e.op(Op::DUP);
                e.invoke(Op::INVOKESPECIAL, "java/util/ArrayList", "<init>", "()V");
                e.op(Op::POP);
                e.op(Op::RETURN);
            })}));
        index.build(container);

        CallGraphBuilder builder;
        for (const Bytes& bytes : classFiles) {
            EXPECT_TRUE(
                builder.addClassBytes(jbc::core::ByteView{bytes.data(), bytes.size()}).is_ok());
        }
        auto built = builder.build();
        EXPECT_TRUE(built.is_ok());
        if (built.is_ok()) callGraph = std::move(built).unwrap();

        RtaOptions options;
        rtaGraph.buildRta(index, callGraph, *HierarchyBundle::build(container), options);
    }

    void addClass(std::string_view name, const Bytes& bytes)
    {
        classFiles.push_back(bytes);
        container.addResource(std::string(name) + ".class",
                              std::make_unique<ClassResource>(std::string(name) + ".class",
                                                              classFiles.back()));
    }
};

bool hasClassRoot(const GlobalReachabilityOptions& options, std::string_view name)
{
    for (std::string_view cls : options.rootClasses) {
        if (cls == name) return true;
    }
    return false;
}

// ── 根产出 ────────────────────────────────────────────────────

// 已实例化的容器内声明类入根；未实例化类与 classpath 外实例化类型不入根。
TEST(MjKeepRtaRootsTest, instantiatedDeclaredClassesRootedExternalSkipped)
{
    KeepRtaFixture fx;
    GlobalReachabilityOptions options;
    applyRtaInstantiatedRoots(fx.rtaGraph, fx.index, options);

    ASSERT_EQ(options.rootClasses.size(), 1u);
    EXPECT_EQ(options.rootClasses.front(), "kr/Used");
    EXPECT_FALSE(hasClassRoot(options, "kr/Unused"));
    EXPECT_FALSE(hasClassRoot(options, "kr/App"));
    EXPECT_FALSE(hasClassRoot(options, "java/util/ArrayList"));
    // 方法/字段槽不受影响。
    EXPECT_TRUE(options.rootMethods.empty());
    EXPECT_TRUE(options.rootFields.empty());
}

// 纯增量：既有根保留在前、本面产物追加在后；新根视图指向 RTA 图自有池
// （与 instantiatedClasses() 同指针），不是局部临时串。
TEST(MjKeepRtaRootsTest, purelyAdditiveAndViewsPointAtGraphPool)
{
    KeepRtaFixture fx;
    GlobalReachabilityOptions options;
    // 借索引池声明文本模拟"调用方既有根"（资源 keep 同款来源）。
    ASSERT_FALSE(fx.index.classDeclarations().empty());
    options.rootClasses.emplace_back(fx.index.classDeclarations().front().name);
    const std::string_view existing = options.rootClasses.front();

    applyRtaInstantiatedRoots(fx.rtaGraph, fx.index, options);

    ASSERT_EQ(options.rootClasses.size(), 2u);
    EXPECT_EQ(options.rootClasses.front(), existing);
    EXPECT_EQ(options.rootClasses.front().data(), existing.data());
    // kr/Used 根与 RTA 图已实例化集合视图同源（图池驻留，随图存活）。
    ASSERT_FALSE(fx.rtaGraph.instantiatedClasses().empty());
    EXPECT_EQ(options.rootClasses.back(), "kr/Used");
    EXPECT_EQ(options.rootClasses.back().data(),
              fx.rtaGraph.instantiatedClasses().front().data());
}

// CHA 档图（build 路径）已实例化集为空 → 本面恒等变换，消费方无需分支判断。
TEST(MjKeepRtaRootsTest, chaGraphYieldsNoRoots)
{
    KeepRtaFixture fx;
    ResolvedCallGraph chaGraph;
    chaGraph.build(fx.index, fx.callGraph, *HierarchyBundle::build(fx.container));

    GlobalReachabilityOptions options;
    applyRtaInstantiatedRoots(chaGraph, fx.index, options);
    EXPECT_TRUE(options.rootClasses.empty());
    EXPECT_TRUE(options.rootMethods.empty());
    EXPECT_TRUE(options.rootFields.empty());
}

// ── 闭环：根真进可达性闭包 ────────────────────────────────────

// 关掉 main 发现、只喂 RTA 保守根：kr/Used 经根入闭包；对照组（不喂根）同一
// 闭包里 kr/Used 不可达——喂入路径真实驱动闭包，不只是填 vector。
TEST(MjKeepRtaRootsTest, rtaRootsDriveTheClosure)
{
    KeepRtaFixture fx;

    GlobalReachabilityOptions withRoots;
    withRoots.includeMainMethods   = false;
    withRoots.callExpansion        = GlobalReachabilityCallExpansion::Resolved;
    withRoots.resolvedEdgesExclusive = true;
    applyRtaInstantiatedRoots(fx.rtaGraph, fx.index, withRoots);
    ASSERT_EQ(withRoots.rootClasses.size(), 1u);

    GlobalReachability reachability;
    reachability.build(fx.index, fx.callGraph, fx.rtaGraph, withRoots);
    EXPECT_TRUE(reachability.isClassReachable("kr/Used"));
    EXPECT_FALSE(reachability.isClassReachable("kr/Unused"));
    EXPECT_FALSE(reachability.isClassReachable("kr/App"));

    // 对照组：同旗标不喂根 → 集合为空（无根不删库型容器的前提在消费侧）。
    GlobalReachabilityOptions withoutRoots;
    withoutRoots.includeMainMethods     = false;
    withoutRoots.callExpansion          = GlobalReachabilityCallExpansion::Resolved;
    withoutRoots.resolvedEdgesExclusive = true;
    GlobalReachability control;
    control.build(fx.index, fx.callGraph, fx.rtaGraph, withoutRoots);
    EXPECT_FALSE(control.isClassReachable("kr/Used"));
}

}   // namespace
