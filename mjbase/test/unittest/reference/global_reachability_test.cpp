/// @file        global_reachability_test.cpp
/// @brief       GlobalReachability 闭包用例（6 用例逐字等强）。
///
/// 夹具为手拼 classfile 字节（class_bytes_builder.hpp）。
/// CallGraph 自建会话，此处用 CallGraphBuilder 对同一批类字节建图——直接
/// 编码调用边集合，闭包语义等强。
/// GAM 注册的 ClassHierarchyAnalysis/MethodHierarchyAnalysis 由本批的
/// HierarchyBundleAnalysis 承载（停泊件，见 resolved_call_graph.hpp）。
#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

using jbc::core::Op;
using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::ClassDeclaration;
using mjbase::reference::FieldDeclaration;
using mjbase::reference::GlobalReachability;
using mjbase::reference::GlobalReachabilityOptions;
using mjbase::reference::MethodDeclaration;
using mjbase::reference::MethodRefKey;
using mjbase::reference::ReferenceIndex;
using mjbase::spi::GlobalAnalysisManager;
using mjbase::reference::test_support::AnnotationSpec;
using mjbase::reference::test_support::Bytes;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::CodeEmit;
using mjbase::reference::test_support::FieldSpec;
using mjbase::reference::test_support::MethodSpec;

namespace {

constexpr ca::u16 kAccPublic    = 0x0001;
constexpr ca::u16 kAccStatic    = 0x0008;
constexpr ca::u16 kAccInterface = 0x0200;
constexpr ca::u16 kAccAbstract  = 0x0400;

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

/// 容器 + 同源类字节集（容器登记与 CallGraphBuilder 喂入共用）。
struct ScanSet
{
    ResourceContainer  container;
    std::vector<Bytes> bytes;

    void add(std::string_view name, Bytes b)
    {
        bytes.push_back(std::move(b));
        container.addResource(std::string(name) + ".class",
                              std::make_unique<ClassResource>(std::string(name) + ".class",
                                                              bytes.back()));
    }
};

CallGraph buildGraph(const std::vector<Bytes>& classFiles)
{
    CallGraphBuilder builder;
    for (const Bytes& bytes : classFiles) {
        EXPECT_TRUE(builder.addClassBytes(jbc::core::ByteView{bytes.data(), bytes.size()})
                        .is_ok());
    }
    auto built = builder.build();
    EXPECT_TRUE(built.is_ok());
    return std::move(built).unwrap();
}

Bytes buildEmptyClass(std::string_view name, ca::u16 access = kAccPublic)
{
    ClassBytesBuilder b(name);
    b.setAccess(access);
    return b.assemble({});
}

Bytes buildDepClass()
{
    ClassBytesBuilder b("Dep");
    std::vector<MethodSpec> ms;
    ms.push_back(m("work", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    ms.push_back(m("unused", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }));
    return b.assemble(ms);
}

Bytes buildAppClass()
{
    ClassBytesBuilder b("App");
    b.setSuper("Base");
    b.setInterfaces({"Iface"});
    b.addField(FieldSpec{kAccPublic | kAccStatic, "dep", "LDep;"});
    std::vector<MethodSpec> ms;
    ms.push_back(m("<clinit>", "()V", 0, 0, [](CodeEmit& e) { e.op(Op::RETURN); }, kAccStatic));
    ms.push_back(m("entry", "()V", 1, 0, [](CodeEmit& e) {
        e.fieldInsn(Op::GETSTATIC, "App", "dep", "LDep;");
        e.op(Op::POP);
        e.invoke(Op::INVOKESTATIC, "Dep", "work", "()V");
        e.op(Op::RETURN);
    }));
    ms.push_back(m("main", "([Ljava/lang/String;)V", 0, 1, [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "App", "entry", "()V");
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

ScanSet buildReachabilityFixture()
{
    ScanSet fx;
    fx.add("Iface",
           buildEmptyClass("Iface", kAccPublic | kAccInterface | kAccAbstract));
    fx.add("Base", buildEmptyClass("Base"));
    fx.add("Dep", buildDepClass());
    fx.add("App", buildAppClass());
    fx.add("Unused", buildEmptyClass("Unused"));
    return fx;
}

Bytes buildStructuralRootClass()
{
    ClassBytesBuilder b("StructuralRoot");
    b.addNestMember("StructuralDep");
    b.addClassAnnotation({"LAnnotationDep;", {}});
    b.addField(FieldSpec{kAccPublic, "generic", "Ljava/lang/Object;", "LSignatureDep;"});
    std::vector<MethodSpec> ms;
    ms.push_back(m("main", "([Ljava/lang/String;)V", 0, 1,
                   [](CodeEmit& e) { e.op(Op::RETURN); }));
    return b.assemble(ms);
}

ScanSet buildStructuralReachabilityFixture()
{
    ScanSet fx;
    fx.add("StructuralRoot", buildStructuralRootClass());
    fx.add("StructuralDep", buildEmptyClass("StructuralDep"));
    fx.add("AnnotationDep", buildEmptyClass("AnnotationDep"));
    fx.add("SignatureDep", buildEmptyClass("SignatureDep"));
    fx.add("Unused", buildEmptyClass("Unused"));
    return fx;
}

Bytes buildVirtualMethodClass(std::string_view owner, std::string_view superName)
{
    ClassBytesBuilder b(owner);
    b.setSuper(superName);
    std::vector<MethodSpec> ms;
    ms.push_back(m("run", "()V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); }, kAccPublic));
    return b.assemble(ms);
}

Bytes buildVirtualMainClass()
{
    ClassBytesBuilder b("VirtualApp");
    std::vector<MethodSpec> ms;
    ms.push_back(m("main", "([Ljava/lang/String;)V", 1, 1, [](CodeEmit& e) {
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "Base", "run", "()V");
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

ScanSet buildVirtualReachabilityFixture()
{
    ScanSet fx;
    fx.add("Base", buildVirtualMethodClass("Base", "java/lang/Object"));
    fx.add("Child", buildVirtualMethodClass("Child", "Base"));
    fx.add("VirtualApp", buildVirtualMainClass());
    return fx;
}

bool containsClass(const std::vector<ClassDeclaration>& declarations, std::string_view name)
{
    return std::any_of(declarations.begin(), declarations.end(),
                       [&](const ClassDeclaration& decl) { return decl.name == name; });
}

bool containsMethod(const std::vector<MethodDeclaration>& declarations, std::string_view owner,
                    std::string_view name, std::string_view desc)
{
    return std::any_of(declarations.begin(), declarations.end(),
                       [&](const MethodDeclaration& decl) {
                           return decl.method.owner == owner && decl.method.name == name &&
                                  decl.method.desc == desc;
                       });
}

}   // namespace

// ── 1. 显式根经声明事实与直接调用扩展 ───────────────────────────

TEST(MjReachabilityTest, expandsExplicitRootsThroughDeclarationsAndDirectCalls)
{
    ScanSet  fx = buildReachabilityFixture();
    CallGraph graph = buildGraph(fx.bytes);

    ReferenceIndex index;
    index.build(fx.container);

    GlobalReachabilityOptions options;
    options.includeMainMethods = false;
    options.rootMethods.push_back({"App", "entry", "()V"});

    GlobalReachability reachability;
    reachability.build(index, graph, options);

    EXPECT_TRUE(reachability.isClassReachable("App"));
    EXPECT_TRUE(reachability.isClassReachable("Base"));
    EXPECT_TRUE(reachability.isClassReachable("Iface"));
    EXPECT_TRUE(reachability.isClassReachable("Dep"));
    EXPECT_FALSE(reachability.isClassReachable("Unused"));

    EXPECT_TRUE(reachability.isMethodReachable("App", "entry", "()V"));
    EXPECT_TRUE(reachability.isMethodReachable("App", "<clinit>", "()V"));
    EXPECT_TRUE(reachability.isMethodReachable("Dep", "work", "()V"));
    EXPECT_FALSE(reachability.isMethodReachable("App", "main", "([Ljava/lang/String;)V"));
    EXPECT_FALSE(reachability.isMethodReachable("Dep", "unused", "()V"));
    EXPECT_TRUE(reachability.isFieldReachable("App", "dep", "LDep;"));

    const std::vector<ClassDeclaration> unreachableClasses =
        reachability.unreachableDeclaredClasses(index);
    EXPECT_TRUE(containsClass(unreachableClasses, "Unused"));
    EXPECT_FALSE(containsClass(unreachableClasses, "App"));

    const std::vector<MethodDeclaration> unreachableMethods =
        reachability.unreachableDeclaredMethods(index);
    EXPECT_TRUE(containsMethod(unreachableMethods, "Dep", "unused", "()V"));
    EXPECT_TRUE(containsMethod(unreachableMethods, "App", "main", "([Ljava/lang/String;)V"));
    EXPECT_FALSE(containsMethod(unreachableMethods, "Dep", "work", "()V"));
}

// ── 2. 默认策略发现 Java main ───────────────────────────────────

TEST(MjReachabilityTest, defaultPolicyRootsJavaMainMethods)
{
    ScanSet  fx = buildReachabilityFixture();
    CallGraph graph = buildGraph(fx.bytes);

    ReferenceIndex index;
    index.build(fx.container);

    GlobalReachability reachability;
    reachability.build(index, graph);

    EXPECT_TRUE(reachability.isMethodReachable("App", "main", "([Ljava/lang/String;)V"));
    EXPECT_TRUE(reachability.isMethodReachable("App", "entry", "()V"));
    EXPECT_TRUE(reachability.isMethodReachable("Dep", "work", "()V"));
    EXPECT_FALSE(reachability.isMethodReachable("Dep", "unused", "()V"));
}

// ── 3. 结构元数据保持被引用类可达 ───────────────────────────────

TEST(MjReachabilityTest, structuralMetadataKeepsReferencedClassesReachable)
{
    ScanSet  fx = buildStructuralReachabilityFixture();
    CallGraph graph = buildGraph(fx.bytes);

    ReferenceIndex index;
    index.build(fx.container);

    GlobalReachability reachability;
    reachability.build(index, graph);

    EXPECT_TRUE(reachability.isClassReachable("StructuralRoot"));
    EXPECT_TRUE(reachability.isClassReachable("StructuralDep"));
    EXPECT_TRUE(reachability.isClassReachable("AnnotationDep"));
    EXPECT_TRUE(reachability.isClassReachable("SignatureDep"));
    EXPECT_FALSE(reachability.isClassReachable("Unused"));
}

// ── 4. GlobalAnalysisManager 集成 ───────────────────────────────

TEST(MjReachabilityTest, globalAnalysisManagerIntegration)
{
    ScanSet fx = buildReachabilityFixture();

    ca::str::Utf8StringArena arena;
    GlobalAnalysisManager    gam(arena);
    gam.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    gam.registerAnalysis<mjbase::reference::CallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::GlobalReachabilityAnalysis>();

    auto& reachability = gam.getResult<mjbase::reference::GlobalReachabilityAnalysis>(
        fx.container);

    EXPECT_TRUE(reachability.isClassReachable("App"));
    EXPECT_TRUE(reachability.isMethodReachable("Dep", "work", "()V"));
    EXPECT_FALSE(reachability.isClassReachable("Unused"));
}

// ── 5. resolved 分析扩展虚覆盖目标 ──────────────────────────────

TEST(MjReachabilityTest, resolvedAnalysisExpandsVirtualOverrideTargets)
{
    ScanSet fx = buildVirtualReachabilityFixture();

    ca::str::Utf8StringArena arena;
    GlobalAnalysisManager    gam(arena);
    gam.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    gam.registerAnalysis<mjbase::reference::CallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::HierarchyBundleAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedCallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::GlobalReachabilityAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedGlobalReachabilityAnalysis>();

    auto& directReachability =
        gam.getResult<mjbase::reference::GlobalReachabilityAnalysis>(fx.container);
    auto& resolvedReachability =
        gam.getResult<mjbase::reference::ResolvedGlobalReachabilityAnalysis>(fx.container);

    EXPECT_TRUE(directReachability.isMethodReachable("Base", "run", "()V"));
    EXPECT_FALSE(directReachability.isMethodReachable("Child", "run", "()V"));

    EXPECT_TRUE(resolvedReachability.isMethodReachable("Base", "run", "()V"));
    EXPECT_TRUE(resolvedReachability.isMethodReachable("Child", "run", "()V"));
    EXPECT_TRUE(resolvedReachability.isClassReachable("Child"));
}

// ── 6. 入口清单记录显式根与发现的 main ─────────────────────────

TEST(MjReachabilityTest, entryMethodsRecordExplicitRootsAndDiscoveredMains)
{
    ScanSet  fx = buildVirtualReachabilityFixture();
    CallGraph graph = buildGraph(fx.bytes);

    ReferenceIndex index;
    index.build(fx.container);

    GlobalReachabilityOptions options;
    options.rootMethods.push_back({"Child", "run", "()V"});

    GlobalReachability reachability;
    reachability.build(index, graph, options);

    ASSERT_EQ(reachability.entryMethods().size(), 2u);
    EXPECT_EQ(reachability.entryMethods()[0].owner, "Child");
    EXPECT_EQ(reachability.entryMethods()[0].name, "run");
    EXPECT_EQ(reachability.entryMethods()[1].owner, "VirtualApp");
    EXPECT_EQ(reachability.entryMethods()[1].name, "main");
    EXPECT_EQ(reachability.entryMethods()[1].desc, "([Ljava/lang/String;)V");

    // 禁用 main 发现且无显式根：入口清单为空（闭包也无方法根）。
    GlobalReachabilityOptions noRoots;
    noRoots.includeMainMethods = false;
    GlobalReachability emptyReachability;
    emptyReachability.build(index, graph, noRoots);
    EXPECT_TRUE(emptyReachability.entryMethods().empty());
}
