/// @file        reflection_call_weave_test.cpp
/// @brief       反射绑定织入调用图（weaveReflectionBindings）用例（4 用例逐字等强）。
///
/// 夹具为手拼 classfile 字节（class_bytes_builder.hpp）；
/// 层次输入由 ClassHierarchy/MethodHierarchy 换成 HierarchyBundle
/// （池 + 两图同生共死的停泊载体）。断言逐字等强：Resolved 方法级目标织入为
/// Reflect 动态边、resolved 模式沿反射边扩展可达闭包、Pending/落不进声明集的
/// 目标计入 dropped、类级绑定只计量不产生边。
#include <gtest/gtest.h>

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
#include "mjbase/reference/reflection_binding_analyzer.hpp"
#include "mjbase/reference/reference_index.hpp"

using jbc::core::Op;
using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::asm_classpath::HierarchyBundle;
using mjbase::asm_classpath::ResolvedCallGraph;
using mjbase::asm_classpath::ResolvedCallKind;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::GlobalReachability;
using mjbase::reference::GlobalReachabilityCallExpansion;
using mjbase::reference::GlobalReachabilityOptions;
using mjbase::reference::ReflectionApiKind;
using mjbase::reference::ReflectionBinding;
using mjbase::reference::ReflectionBindingAnalyzer;
using mjbase::reference::ReflectionBindingState;
using mjbase::reference::ReferenceIndex;
using mjbase::reference::weaveReflectionBindings;
using mjbase::reference::test_support::Bytes;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::CodeEmit;
using mjbase::reference::test_support::MethodSpec;

namespace {

constexpr ca::u16 kAccPublic = 0x0001;
constexpr ca::u16 kAccStatic = 0x0008;

constexpr const char* kForNameDesc       = "(Ljava/lang/String;)Ljava/lang/Class;";
constexpr const char* kGetMethodDesc     =
    "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;";
constexpr const char* kMethodInvokeDesc  =
    "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;";
constexpr const char* kAppendDesc        = "(Ljava/lang/String;)Ljava/lang/StringBuilder;";
constexpr const char* kToStringDesc      = "()Ljava/lang/String;";
constexpr const char* kClassNewInstanceDesc = "()Ljava/lang/Object;";

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

void addClassBytes(ResourceContainer& container, std::string_view name, const Bytes& bytes)
{
    container.addResource(std::string(name) + ".class",
                          std::make_unique<ClassResource>(std::string(name) + ".class", bytes));
}

// rf/Target：<init>()V + hello()V（反射落解目标）。
Bytes buildTargetClass()
{
    ClassBytesBuilder b("rf/Target");
    std::vector<MethodSpec> ms;
    ms.push_back(m("<init>", "()V", 1, 1,
                   [](CodeEmit& e) {
                       e.varInsn(Op::ALOAD, 0);
                       e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
                       e.op(Op::RETURN);
                   },
                   kAccPublic));
    ms.push_back(m("hello", "()V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); }, kAccPublic));
    return b.assemble(ms);
}

// 直线反射形态：类名实参 → forName(String) → getMethod(name, 空Class[]) → 存入
// 局部变量 → Method.invoke。foldClassName=true 时类名经 StringBuilder 链折叠——
// 该形态 ReferenceIndex 的单 ldc 直线口径不识别（无反射引用事实），只有反射绑定
// 分析器能落解，用于构造 --reflect 前后的可达性差；=false 时 ldc 直线形态，
// ReferenceIndex 既口径已能落解（BadCaller 用，成员不存在两边都落不下）。
Bytes buildCallerInvoke(std::string_view className, std::string_view methodName,
                        std::string_view memberName, bool foldClassName)
{
    ClassBytesBuilder b(className);
    std::vector<MethodSpec> ms;
    ms.push_back(m(methodName, "([Ljava/lang/String;)V", 3, 2, [&](CodeEmit& e) {
        if (foldClassName) {
            // new StringBuilder().append("rf.").append("Target").toString()
            e.typeInsn(Op::NEW, "java/lang/StringBuilder");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "java/lang/StringBuilder", "<init>", "()V");
            e.varInsn(Op::ASTORE, 1);
            e.varInsn(Op::ALOAD, 1);
            e.ldcString("rf.");
            e.invoke(Op::INVOKEVIRTUAL, "java/lang/StringBuilder", "append", kAppendDesc);
            e.op(Op::POP);
            e.varInsn(Op::ALOAD, 1);
            e.ldcString("Target");
            e.invoke(Op::INVOKEVIRTUAL, "java/lang/StringBuilder", "append", kAppendDesc);
            e.op(Op::POP);
            e.varInsn(Op::ALOAD, 1);
            e.invoke(Op::INVOKEVIRTUAL, "java/lang/StringBuilder", "toString", kToStringDesc);
        }
        else {
            e.ldcString("rf.Target");
        }
        e.invoke(Op::INVOKESTATIC, "java/lang/Class", "forName", kForNameDesc);
        e.ldcString(memberName);
        e.op(Op::ICONST_0);
        e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getMethod", kGetMethodDesc);
        e.varInsn(Op::ASTORE, 1);
        e.varInsn(Op::ALOAD, 1);
        e.op(Op::ACONST_NULL);
        e.op(Op::ACONST_NULL);
        e.invoke(Op::INVOKEVIRTUAL, "java/lang/reflect/Method", "invoke", kMethodInvokeDesc);
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    return b.assemble(ms);
}

struct WeaveFixture
{
    ResourceContainer        container;
    ReferenceIndex           index;
    CallGraph                graph;
    std::shared_ptr<const HierarchyBundle> hierarchy;
    ResolvedCallGraph        resolvedGraph;
    ReflectionBindingAnalyzer analyzer;

    WeaveFixture()
    {
        // Caller 用 SB 链折叠类名：ReferenceIndex 窄口径不落解，织入前 hello
        // 必须不可达，织入后经反射边可达（前后差即反射边的贡献）。
        addClassBytes(container, "rf/Target", buildTargetClass());
        addClassBytes(container, "rf/Caller",
                      buildCallerInvoke("rf/Caller", "main", "hello", true));
        // BadCaller 用 ldc 直线形态：成员不存在，两类口径都落不下 → dropped。
        addClassBytes(container, "rf/BadCaller",
                      buildCallerInvoke("rf/BadCaller", "main", "nope", false));

        index.build(container);

        // 图与索引同源：直接对同一批类字节建图（CallGraphBuilder 按字节喂入）。
        const std::vector<Bytes> classFiles = collectClassBytes();
        CallGraphBuilder         builder;
        for (const Bytes& bytes : classFiles) {
            builder.addClassBytes(jbc::core::ByteView{bytes.data(), bytes.size()});
        }
        graph = std::move(builder.build().unwrap());

        hierarchy = HierarchyBundle::build(container);
        resolvedGraph.build(index, graph, *hierarchy);
        analyzer.build(container);
    }

    /// 织入端喂图用的类字节（与容器登记同源同序）。
    std::vector<Bytes> collectClassBytes() const
    {
        return {buildTargetClass(),
                buildCallerInvoke("rf/Caller", "main", "hello", true),
                buildCallerInvoke("rf/BadCaller", "main", "nope", false)};
    }

    const ReflectionBinding* findInvokeBinding(std::string_view sourceClass) const
    {
        for (const ReflectionBinding& binding : analyzer.bindings()) {
            if (binding.site.sourceClass == sourceClass &&
                binding.api == ReflectionApiKind::MethodInvoke) {
                return &binding;
            }
        }
        return nullptr;
    }

    GlobalReachability reachabilityOf(const ResolvedCallGraph& resolved) const
    {
        GlobalReachabilityOptions options;
        options.callExpansion = GlobalReachabilityCallExpansion::Resolved;
        GlobalReachability reachability;
        reachability.build(index, graph, resolved, options);
        return reachability;
    }
};

}   // namespace

TEST(MjReflectionWeaveTest, invokeBindingWovenAsReflectEdgeAndFollowedByReachability)
{
    WeaveFixture fx;

    // 前置：invoke 绑定已落解目标 hello。
    const ReflectionBinding* invoke = fx.findInvokeBinding("rf/Caller");
    ASSERT_NE(invoke, nullptr);
    EXPECT_EQ(invoke->state, ReflectionBindingState::Resolved);
    EXPECT_EQ(invoke->target.owner, "rf/Target");
    EXPECT_EQ(invoke->target.name, "hello");
    EXPECT_EQ(invoke->target.desc, "()V");

    // 织入前：hello 不经任何静态边可达。
    EXPECT_FALSE(
        fx.reachabilityOf(fx.resolvedGraph).isMethodReachable("rf/Target", "hello", "()V"));

    const mjbase::reference::ReflectionWeaveReport report =
        weaveReflectionBindings(fx.resolvedGraph, fx.index, fx.analyzer.bindings());

    // 织入后：Reflect 边存在，来源标记与目标正确。
    EXPECT_TRUE(fx.resolvedGraph.hasResolvedEdge(
        "rf/Caller", "main", "([Ljava/lang/String;)V", "rf/Target", "hello", "()V"));
    bool sawReflectEdge = false;
    for (const auto& edge :
         fx.resolvedGraph.edgesFrom("rf/Caller", "main", "([Ljava/lang/String;)V")) {
        if (edge.kind == ResolvedCallKind::Reflect && edge.resolvedTarget.name == "hello") {
            EXPECT_FALSE(edge.expanded);
            EXPECT_EQ(edge.site.sourceClass, "rf/Caller");
            sawReflectEdge = true;
        }
    }
    EXPECT_TRUE(sawReflectEdge);

    // resolved 模式可达闭包沿反射边扩展：hello 与其 owner 类进入闭包。
    const GlobalReachability reach = fx.reachabilityOf(fx.resolvedGraph);
    EXPECT_TRUE(reach.isMethodReachable("rf/Target", "hello", "()V"));
    EXPECT_TRUE(reach.isClassReachable("rf/Target"));
}

TEST(MjReflectionWeaveTest, weaveReportCountsSitesResolvedDroppedAndClasses)
{
    WeaveFixture fx;

    // BadCaller 的 invoke 指向不存在成员：绑定 Pending，但仍计 site 并落 dropped。
    const ReflectionBinding* bad = fx.findInvokeBinding("rf/BadCaller");
    ASSERT_NE(bad, nullptr);
    EXPECT_EQ(bad->state, ReflectionBindingState::Pending);

    // 织入前基线：无 Reflect 边，静态边数就是全量边数。
    const ca::usize staticEdgesBefore = fx.resolvedGraph.edgeCount();

    const mjbase::reference::ReflectionWeaveReport report =
        weaveReflectionBindings(fx.resolvedGraph, fx.index, fx.analyzer.bindings());

    EXPECT_EQ(report.sites, 2U);      // Caller.main + BadCaller.main 各一个 invoke sink
    EXPECT_EQ(report.resolved, 1U);   // 仅 hello 落解
    EXPECT_EQ(report.edges, 1U);      // 织入一条边
    EXPECT_EQ(report.dropped, 1U);    // nope Pending 未落图
    EXPECT_EQ(report.classes, 1U);    // forName Resolved → rf/Target 去重 1
    EXPECT_EQ(report.sites - report.edges, report.dropped);
    EXPECT_EQ(fx.resolvedGraph.edgeCount(), staticEdgesBefore + 1);
}

TEST(MjReflectionWeaveTest, classLevelBindingsCountClassesWithoutEdges)
{
    // 只有类级 sink 的容器：forName Resolved 计 classes，无任何 Reflect 边。
    ResourceContainer container;
    addClassBytes(container, "rf/Target", buildTargetClass());
    {
        ClassBytesBuilder b("rf/OnlyForName");
        std::vector<MethodSpec> ms;
        ms.push_back(m("touch", "()V", 1, 0, [](CodeEmit& e) {
            e.ldcString("rf.Target");
            e.invoke(Op::INVOKESTATIC, "java/lang/Class", "forName", kForNameDesc);
            e.op(Op::POP);
            e.op(Op::RETURN);
        }));
        addClassBytes(container, "rf/OnlyForName", b.assemble(ms));
    }

    ReferenceIndex index;
    index.build(container);
    // 图与索引同源：直接重建同序类字节喂入。
    const Bytes targetBytes = buildTargetClass();
    const Bytes onlyForName = []() {
        ClassBytesBuilder bb("rf/OnlyForName");
        std::vector<MethodSpec> ms;
        ms.push_back(m("touch", "()V", 1, 0, [](CodeEmit& e) {
            e.ldcString("rf.Target");
            e.invoke(Op::INVOKESTATIC, "java/lang/Class", "forName", kForNameDesc);
            e.op(Op::POP);
            e.op(Op::RETURN);
        }));
        return bb.assemble(ms);
    }();
    CallGraph graph = [&]() {
        CallGraphBuilder b2;
        b2.addClassBytes(jbc::core::ByteView{targetBytes.data(), targetBytes.size()});
        b2.addClassBytes(jbc::core::ByteView{onlyForName.data(), onlyForName.size()});
        return std::move(b2.build().unwrap());
    }();

    const std::shared_ptr<const HierarchyBundle> hierarchy = HierarchyBundle::build(container);
    ResolvedCallGraph resolvedGraph;
    resolvedGraph.build(index, graph, *hierarchy);

    ReflectionBindingAnalyzer analyzer;
    analyzer.build(container);

    const ca::usize                             edgesBefore = resolvedGraph.edgeCount();
    const mjbase::reference::ReflectionWeaveReport report =
        weaveReflectionBindings(resolvedGraph, index, analyzer.bindings());

    // 类级 sink 只计量：不产生边，图内容不变。
    EXPECT_EQ(report.sites, 0U);
    EXPECT_EQ(report.edges, 0U);
    EXPECT_EQ(report.dropped, 0U);
    EXPECT_EQ(report.classes, 1U);
    EXPECT_EQ(resolvedGraph.edgeCount(), edgesBefore);
    for (const auto& edge : resolvedGraph.edges()) {
        EXPECT_NE(edge.kind, ResolvedCallKind::Reflect);
    }
}

TEST(MjReflectionWeaveTest, classNewInstanceWithoutDefaultConstructorDropsAtWeave)
{
    // 分析器对 Class.newInstance：目标类在容器内即 Resolved，target 硬编码
    // <init>/()V（reflection_binding_analyzer 的类级执行 sink 口径）。类只有
    // 带参构造时，织入端 declaredMethod 复核兜底丢弃——resolved 与 edges 在
    // 此分叉（Resolved-but-dropped：resolved=1、edges=0、dropped=1）。
    ResourceContainer container;
    {
        // rf/NoDefault：只有 <init>(Ljava/lang/String;)V，无无参构造。
        ClassBytesBuilder b("rf/NoDefault");
        std::vector<MethodSpec> ms;
        ms.push_back(m("<init>", "(Ljava/lang/String;)V", 2, 2,
                       [](CodeEmit& e) {
                           e.varInsn(Op::ALOAD, 0);
                           e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
                           e.op(Op::RETURN);
                       },
                       kAccPublic));
        addClassBytes(container, "rf/NoDefault", b.assemble(ms));
    }
    {
        // rf/NewCaller：touch()V = rf/NoDefault.class 字面量 → Class.newInstance。
        ClassBytesBuilder b("rf/NewCaller");
        std::vector<MethodSpec> ms;
        ms.push_back(m("touch", "()V", 1, 0, [](CodeEmit& e) {
            e.ldcClass("rf/NoDefault");
            e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "newInstance",
                     kClassNewInstanceDesc);
            e.op(Op::POP);
            e.op(Op::RETURN);
        }));
        addClassBytes(container, "rf/NewCaller", b.assemble(ms));
    }

    ReferenceIndex index;
    index.build(container);
    const Bytes noDefaultBytes = []() {
        ClassBytesBuilder b("rf/NoDefault");
        std::vector<MethodSpec> ms;
        ms.push_back(m("<init>", "(Ljava/lang/String;)V", 2, 2,
                       [](CodeEmit& e) {
                           e.varInsn(Op::ALOAD, 0);
                           e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
                           e.op(Op::RETURN);
                       },
                       kAccPublic));
        return b.assemble(ms);
    }();
    const Bytes newCallerBytes = []() {
        ClassBytesBuilder b("rf/NewCaller");
        std::vector<MethodSpec> ms;
        ms.push_back(m("touch", "()V", 1, 0, [](CodeEmit& e) {
            e.ldcClass("rf/NoDefault");
            e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "newInstance",
                     kClassNewInstanceDesc);
            e.op(Op::POP);
            e.op(Op::RETURN);
        }));
        return b.assemble(ms);
    }();
    CallGraph graph = [&]() {
        CallGraphBuilder b2;
        b2.addClassBytes(jbc::core::ByteView{noDefaultBytes.data(), noDefaultBytes.size()});
        b2.addClassBytes(jbc::core::ByteView{newCallerBytes.data(), newCallerBytes.size()});
        return std::move(b2.build().unwrap());
    }();

    const std::shared_ptr<const HierarchyBundle> hierarchy = HierarchyBundle::build(container);
    ResolvedCallGraph resolvedGraph;
    resolvedGraph.build(index, graph, *hierarchy);

    ReflectionBindingAnalyzer analyzer;
    analyzer.build(container);

    // 前置：绑定 Resolved，target 硬编码 <init>/()V（容器内实际只有带参构造）。
    const ReflectionBinding* newInstance = nullptr;
    for (const ReflectionBinding& binding : analyzer.bindings()) {
        if (binding.site.sourceClass == "rf/NewCaller" &&
            binding.api == ReflectionApiKind::ClassNewInstance) {
            newInstance = &binding;
        }
    }
    ASSERT_NE(newInstance, nullptr);
    EXPECT_EQ(newInstance->state, ReflectionBindingState::Resolved);
    EXPECT_EQ(newInstance->target.owner, "rf/NoDefault");
    EXPECT_EQ(newInstance->target.name, "<init>");
    EXPECT_EQ(newInstance->target.desc, "()V");

    const ca::usize                             edgesBefore = resolvedGraph.edgeCount();
    const mjbase::reference::ReflectionWeaveReport report =
        weaveReflectionBindings(resolvedGraph, index, analyzer.bindings());

    // 织入复核兜底：<init>()V 不在声明集 → 不产边，计 dropped。
    EXPECT_EQ(report.sites, 1U);
    EXPECT_EQ(report.resolved, 1U);   // 分析器结论 Resolved（与 edges 分叉）
    EXPECT_EQ(report.edges, 0U);      // 复核未过，未织入
    EXPECT_EQ(report.dropped, 1U);    // Resolved-but-dropped
    EXPECT_EQ(report.classes, 0U);
    EXPECT_EQ(report.sites - report.edges, report.dropped);
    EXPECT_GT(report.resolved, report.edges);
    EXPECT_EQ(resolvedGraph.edgeCount(), edgesBefore);
    for (const auto& edge : resolvedGraph.edges()) {
        EXPECT_NE(edge.kind, ResolvedCallKind::Reflect);
    }
}
