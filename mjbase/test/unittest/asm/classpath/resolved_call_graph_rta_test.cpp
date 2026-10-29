/// @file        resolved_call_graph_rta_test.cpp
/// @brief       RTA 精度档（buildRta）用例：实例化集 fixpoint（new 链传播 +
///              跨方法晚发现重估）、虚分派收窄（覆写停链/继承落祖先/零实例化
///              零边）、调用点粒度接收者静态类型门控（菱形真边保留 + 共享
///              feasible 站点收窄）、外部/缺失目标降级、空描述符反射按名落解、
///              反射/native/classpath 外实例化的欠近似口径、opt-in 关闭（CHA
///              档）零行为变化、显式根与 main 发现开关、GAM 两档并存。
///
/// 语义真源为旧栈 RTA 档当前实现；夹具手拼 classfile 字节（复用 reference
/// 测试目录的 class_bytes_builder.hpp，同一汇编器跨目录共用）。
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "reference/class_bytes_builder.hpp"
#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/reference/resolved_call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/analysis_manager.hpp"

using jbc::core::Op;
using mjbase::asm_classpath::CallGraph;
using mjbase::asm_classpath::CallGraphBuilder;
using mjbase::reference::HierarchyBundle;
using mjbase::reference::ResolvedCallEdge;
using mjbase::reference::ResolvedCallGraph;
using mjbase::reference::RtaOptions;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::reference::MethodRefKey;
using mjbase::reference::ReferenceIndex;
using mjbase::reference::test_support::Bytes;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::CodeEmit;
using mjbase::reference::test_support::MethodSpec;

namespace {

constexpr ca::u16 kAccPublic    = 0x0001;
constexpr ca::u16 kAccSuper     = 0x0020;
constexpr ca::u16 kAccStatic    = 0x0008;
constexpr ca::u16 kAccNative    = 0x0100;
constexpr ca::u16 kAccInterface = 0x0200;
constexpr ca::u16 kAccAbstract  = 0x0400;

MethodSpec m(std::string_view name, std::string_view desc, ca::u16 maxStack, ca::u16 maxLocals,
             std::function<void(CodeEmit&)> emit, ca::u16 access = kAccPublic)
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

/// 普通实例类：默认 <init>（调 Object.<init>，extras 已含 <init> 时不重复）
/// + 附加方法。super 空串 = 直承 java/lang/Object（builder 默认，Object 本身
/// 不随容器登记——真实 jar 形态）。
Bytes instanceClass(std::string_view name, std::string_view super,
                    std::vector<std::string_view> interfaces, std::vector<MethodSpec> extras)
{
    ClassBytesBuilder b(name);
    b.setAccess(kAccPublic | kAccSuper);
    if (!super.empty()) b.setSuper(super);
    if (!interfaces.empty()) b.setInterfaces(interfaces);
    std::vector<MethodSpec> ms;
    bool hasCtor = false;
    for (const MethodSpec& extra : extras) {
        if (extra.name == "<init>") hasCtor = true;
    }
    if (!hasCtor) {
        ms.push_back(m("<init>", "()V", 1, 1, [](CodeEmit& e) {
            e.varInsn(Op::ALOAD, 0);
            e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
            e.op(Op::RETURN);
        }));
    }
    for (auto& extra : extras) ms.push_back(std::move(extra));
    return b.assemble(ms);
}

/// 接口：abstract 方法无 Code 属性（emit 为空即无 Code）。
Bytes interfaceClass(std::string_view name, std::vector<MethodSpec> abstracts)
{
    ClassBytesBuilder b(name);
    b.setAccess(kAccPublic | kAccInterface | kAccAbstract);
    return b.assemble(abstracts);
}

MethodSpec abstractRun(ca::u16 access = kAccPublic | kAccAbstract)
{
    return m("run", "()V", 0, 0, nullptr, access);
}

MethodSpec runMethod()
{
    return m("run", "()V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); });
}

/// main：NEW T → DUP → INVOKESPECIAL T.<init> → INVOKEVIRTUAL owner.run()V →
/// POP → RETURN（「实例化后经基类 owner 虚调」的常态形态）。
MethodSpec mainNewThenRun(std::string_view newType, std::string_view receiverOwner)
{
    return m("main", "([Ljava/lang/String;)V", 2, 2,
             [newType, receiverOwner](CodeEmit& e) {
                 e.typeInsn(Op::NEW, newType);
                 e.op(Op::DUP);
                 e.invoke(Op::INVOKESPECIAL, newType, "<init>", "()V");
                 e.invoke(Op::INVOKEVIRTUAL, receiverOwner, "run", "()V");
                 e.op(Op::POP);
                 e.op(Op::RETURN);
             },
             kAccPublic | kAccStatic);
}

/// RTA 图夹具：容器 + 同源类字节（ReferenceIndex 与 CallGraphBuilder 同批喂入）。
class RtaGraphFixture
{
public:
    void addClass(std::string_view name, const Bytes& bytes)
    {
        classFiles_.push_back(bytes);
        container_.addResource(std::string(name) + ".class",
                               std::make_unique<ClassResource>(std::string(name) + ".class",
                                                               classFiles_.back()));
    }

    /// 建索引 / 调用图 / 层次束并构建 RTA 图（默认根策略 = main 发现）。
    /// 前置：addClass 全部完成后调用一次。
    void buildRta(const RtaOptions& options = {})
    {
        index_.build(container_);
        CallGraphBuilder builder;
        for (const Bytes& bytes : classFiles_) {
            EXPECT_TRUE(builder.addClassBytes(jbc::core::ByteView{bytes.data(), bytes.size()})
                            .is_ok());
        }
        auto built = builder.build();
        EXPECT_TRUE(built.is_ok());
        if (built.is_err()) return;
        callGraph_ = std::move(built).unwrap();
        hierarchy_ = HierarchyBundle::build(container_);
        graph_.buildRta(index_, callGraph_, *hierarchy_, options);
    }

    /// 同一夹具重建 CHA 档（opt-in 关闭侧的对照）。
    void buildCha()
    {
        index_.build(container_);
        CallGraphBuilder builder;
        for (const Bytes& bytes : classFiles_) {
            EXPECT_TRUE(builder.addClassBytes(jbc::core::ByteView{bytes.data(), bytes.size()})
                            .is_ok());
        }
        auto built = builder.build();
        EXPECT_TRUE(built.is_ok());
        if (built.is_err()) return;
        callGraph_ = std::move(built).unwrap();
        hierarchy_ = HierarchyBundle::build(container_);
        graph_.build(index_, callGraph_, *hierarchy_);
    }

    ResourceContainer&            container() { return container_; }
    ReferenceIndex&               index() { return index_; }
    CallGraph&                    callGraph() { return callGraph_; }
    const HierarchyBundle&        hierarchy() { return *hierarchy_; }
    ResolvedCallGraph&            graph() { return graph_; }
    const std::vector<Bytes>&     classFiles() const { return classFiles_; }

private:
    ResourceContainer                             container_;
    std::vector<Bytes>                            classFiles_;
    ReferenceIndex                                index_;
    CallGraph                                     callGraph_;
    std::shared_ptr<const HierarchyBundle>        hierarchy_;
    ResolvedCallGraph                             graph_;
};

bool hasEdge(const ResolvedCallGraph& graph, std::string_view srcOwner, std::string_view srcName,
             std::string_view srcDesc, std::string_view dstOwner, std::string_view dstName,
             std::string_view dstDesc)
{
    return graph.hasResolvedEdge(srcOwner, srcName, srcDesc, dstOwner, dstName, dstDesc);
}

bool containsClass(const std::vector<std::string_view>& classes, std::string_view name)
{
    for (const std::string_view cls : classes) {
        if (cls == name) return true;
    }
    return false;
}

bool sameEdges(const std::vector<ResolvedCallEdge>& a, const std::vector<ResolvedCallEdge>& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const ResolvedCallEdge& ea = a[i];
        const ResolvedCallEdge& eb = b[i];
        if (ea.site.sourceClass != eb.site.sourceClass ||
            ea.site.sourceMethod != eb.site.sourceMethod ||
            ea.site.sourceDesc != eb.site.sourceDesc || ea.site.opcode != eb.site.opcode ||
            ea.site.insnIndex != eb.site.insnIndex ||
            ea.encodedTarget.owner != eb.encodedTarget.owner ||
            ea.encodedTarget.name != eb.encodedTarget.name ||
            ea.encodedTarget.desc != eb.encodedTarget.desc ||
            ea.resolvedTarget.owner != eb.resolvedTarget.owner ||
            ea.resolvedTarget.name != eb.resolvedTarget.name ||
            ea.resolvedTarget.desc != eb.resolvedTarget.desc || ea.kind != eb.kind ||
            ea.expanded != eb.expanded) {
            return false;
        }
    }
    return true;
}

// ── 实例化集 fixpoint ─────────────────────────────────────────

// new 链传播：main 实例化 B，B 的 <init>（因实例化而入闭包）实例化 C——
// C 经 B 的方法体间接进入已实例化集，两条 special 精确边俱在。
TEST(MjResolvedCallGraphRtaTest, newChainFixpointPropagatesThroughInstantiatedBodies)
{
    RtaGraphFixture fx;
    fx.addClass("fx/App", instanceClass("fx/App", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 2, 1,
        [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "fx/B");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "fx/B", "<init>", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.addClass("fx/B", instanceClass("fx/B", "", {}, {m(
        "<init>", "()V", 2, 1,
        [](CodeEmit& e) {
            e.varInsn(Op::ALOAD, 0);
            e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
            e.typeInsn(Op::NEW, "fx/C");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "fx/C", "<init>", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        })}));
    fx.addClass("fx/C", instanceClass("fx/C", "", {}, {}));
    fx.buildRta();

    EXPECT_EQ(fx.graph().rtaInstantiatedCount(), 2u);
    EXPECT_TRUE(containsClass(fx.graph().instantiatedClasses(), "fx/B"));
    EXPECT_TRUE(containsClass(fx.graph().instantiatedClasses(), "fx/C"));
    // 入口可达方法：main + B.<init> + B.<init> 调的 Object.<init>（外部） +
    // C.<init>。
    EXPECT_EQ(fx.graph().rtaReachableMethodCount(), 4u);
    EXPECT_TRUE(hasEdge(fx.graph(), "fx/App", "main", "([Ljava/lang/String;)V",
                        "fx/B", "<init>", "()V"));
    EXPECT_TRUE(hasEdge(fx.graph(), "fx/B", "<init>", "()V", "fx/C", "<init>", "()V"));
    // 间接实例化不经边：main 与 C.<init> 之间无边。
    EXPECT_FALSE(hasEdge(fx.graph(), "fx/App", "main", "([Ljava/lang/String;)V",
                         "fx/C", "<init>", "()V"));
}

// 跨方法晚发现重估：main 先出现 P.run 调用点（当时实例化集为空 → 挂起零可行），
// 再经 Helper.make 间接 new P——worklist 后段才发生的实例化必须回头激活早先
// 挂起的调用点（晚发现的实例化不丢边）。
TEST(MjResolvedCallGraphRtaTest, lateInstantiationReevaluatesEarlierPendingSite)
{
    RtaGraphFixture fx;
    fx.addClass("lp/P", instanceClass("lp/P", "", {}, {runMethod()}));
    fx.addClass("lp/Helper", instanceClass("lp/Helper", "", {}, {m(
        "make", "()V", 2, 0,
        [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "lp/P");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "lp/P", "<init>", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.addClass("lp/LMain", instanceClass("lp/LMain", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 1, 0,
        [](CodeEmit& e) {
            e.op(Op::ACONST_NULL);
            e.invoke(Op::INVOKEVIRTUAL, "lp/P", "run", "()V");
            e.op(Op::POP);
            e.invoke(Op::INVOKESTATIC, "lp/Helper", "make", "()V");
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.buildRta();

    EXPECT_TRUE(containsClass(fx.graph().instantiatedClasses(), "lp/P"));
    // main + P.run + Helper.make + P.<init> + Object.<init>（外部）。
    EXPECT_EQ(fx.graph().rtaReachableMethodCount(), 5u);
    EXPECT_TRUE(hasEdge(fx.graph(), "lp/LMain", "main", "([Ljava/lang/String;)V",
                        "lp/P", "run", "()V"));
}

// ── 虚分派收窄 ────────────────────────────────────────────────

// 子类覆写：已实例化 Child 覆写 run——canonical 解析停在 Child，父类条目
// Base.run 不出边（CHA 档两条都出，此为 RTA 的收窄面）。
TEST(MjResolvedCallGraphRtaTest, overridingSubclassStopsDispatchAtSubclass)
{
    RtaGraphFixture fx;
    fx.addClass("vn/Base", instanceClass("vn/Base", "", {}, {runMethod()}));
    fx.addClass("vn/Child", instanceClass("vn/Child", "vn/Base", {}, {runMethod()}));
    fx.addClass("vn/NMain", instanceClass("vn/NMain", "", {}, {mainNewThenRun("vn/Child", "vn/Base")}));
    fx.buildRta();

    EXPECT_EQ(fx.graph().rtaInstantiatedCount(), 1u);
    EXPECT_TRUE(containsClass(fx.graph().instantiatedClasses(), "vn/Child"));
    EXPECT_TRUE(hasEdge(fx.graph(), "vn/NMain", "main", "([Ljava/lang/String;)V",
                        "vn/Child", "run", "()V"));
    EXPECT_FALSE(hasEdge(fx.graph(), "vn/NMain", "main", "([Ljava/lang/String;)V",
                         "vn/Base", "run", "()V"));
    // <init> special 边 + 唯一虚边。
    EXPECT_EQ(fx.graph().edgesFrom("vn/NMain", "main", "([Ljava/lang/String;)V").size(), 2u);
}

// 继承未覆写：已实例化 Child 不声明 run——解析落在祖先 Base 名下条目（继承
// 未覆写不是欠近似，祖先条目可行）。
TEST(MjResolvedCallGraphRtaTest, inheritedUnoverriddenEntryStaysFeasibleOnAncestor)
{
    RtaGraphFixture fx;
    fx.addClass("iu/Base", instanceClass("iu/Base", "", {}, {runMethod()}));
    fx.addClass("iu/Child", instanceClass("iu/Child", "iu/Base", {}, {}));
    fx.addClass("iu/IMain", instanceClass("iu/IMain", "", {}, {mainNewThenRun("iu/Child", "iu/Base")}));
    fx.buildRta();

    EXPECT_TRUE(containsClass(fx.graph().instantiatedClasses(), "iu/Child"));
    EXPECT_TRUE(hasEdge(fx.graph(), "iu/IMain", "main", "([Ljava/lang/String;)V",
                        "iu/Base", "run", "()V"));
    EXPECT_FALSE(hasEdge(fx.graph(), "iu/IMain", "main", "([Ljava/lang/String;)V",
                         "iu/Child", "run", "()V"));
}

// 组件在册但闭包零实例化：调用点零边（不回退编码目标——相对 CHA 的精度收益）。
TEST(MjResolvedCallGraphRtaTest, zeroInstantiationSiteEmitsZeroEdges)
{
    RtaGraphFixture fx;
    fx.addClass("z/ZBase", instanceClass("z/ZBase", "", {}, {runMethod()}));
    fx.addClass("z/ZChild", instanceClass("z/ZChild", "z/ZBase", {}, {runMethod()}));
    fx.addClass("z/ZMain", instanceClass("z/ZMain", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 1, 0,
        [](CodeEmit& e) {
            e.op(Op::ACONST_NULL);
            e.invoke(Op::INVOKEVIRTUAL, "z/ZBase", "run", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.buildRta();

    EXPECT_EQ(fx.graph().rtaInstantiatedCount(), 0u);
    EXPECT_EQ(fx.graph().rtaReachableMethodCount(), 1u);
    EXPECT_TRUE(fx.graph().instantiatedClasses().empty());
    EXPECT_EQ(fx.graph().edgeCount(), 0u);
}

// ── 接收者静态类型门控 ────────────────────────────────────────

// 「具体父类 × 无关接口」菱形真边不丢：interface R{run}（无 default）、具体
// 类 A{run}（与 R 无关）、桥接类 B extends A implements R{run}（B 的覆写把
// A.run 与 R.run 并入同一覆盖族）、T extends A implements R 不声明 run；调用
// 点 invokeinterface R.run，main 仅 new T。JVMS 5.4.5 具体父类方法优先——
// 运行时确定执行 A.run，而 A 与 R 互不为子型：站点级健全判据（∃ 已实例化
// S=T：S<:R 且 A∈ancestors(T)）保留该真边，旧双向判据在此杀真边。
TEST(MjResolvedCallGraphRtaTest, siteGateKeepsConcreteSuperDiamondTrueEdge)
{
    RtaGraphFixture fx;
    fx.addClass("g/A", instanceClass("g/A", "", {}, {runMethod()}));
    fx.addClass("g/R", interfaceClass("g/R", {abstractRun()}));
    fx.addClass("g/B", instanceClass("g/B", "g/A", {"g/R"}, {runMethod()}));
    fx.addClass("g/T", instanceClass("g/T", "g/A", {"g/R"}, {}));
    fx.addClass("g/GMain", instanceClass("g/GMain", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 2, 2,
        [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "g/T");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "g/T", "<init>", "()V");
            e.invokeInterface("g/R", "run", "()V", 1);
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.buildRta();

    EXPECT_TRUE(containsClass(fx.graph().instantiatedClasses(), "g/T"));
    const std::string mainDesc = "([Ljava/lang/String;)V";
    // 菱形真边：R.run 站点 → 具体父类条目 A.run。
    EXPECT_TRUE(hasEdge(fx.graph(), "g/GMain", "main", mainDesc, "g/A", "run", "()V"));
    // canonical 解析停在 T 的链上首个命中（A）：接口 abstract 条目与不在 T 链
    // 上的桥接条目 B.run 都不出边（feasible 收窄，非门控行为）。
    EXPECT_FALSE(hasEdge(fx.graph(), "g/GMain", "main", mainDesc, "g/R", "run", "()V"));
    EXPECT_FALSE(hasEdge(fx.graph(), "g/GMain", "main", mainDesc, "g/B", "run", "()V"));
}

// per-root feasible 按组件共享、站点逐边收窄：接收者静态类型 X 的调用点只出
// X.run 边（Y.run 被门控筛掉），接收者为接口 I 的调用点 X.run/Y.run 全出。
TEST(MjResolvedCallGraphRtaTest, siteGateNarrowsSharedFeasiblePerReceiverType)
{
    RtaGraphFixture fx;
    fx.addClass("sg/I", interfaceClass("sg/I", {abstractRun()}));
    fx.addClass("sg/X", instanceClass("sg/X", "", {"sg/I"}, {runMethod()}));
    fx.addClass("sg/Y", instanceClass("sg/Y", "", {"sg/I"}, {runMethod()}));
    fx.addClass("sg/SGMain", instanceClass("sg/SGMain", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 2, 1,
        [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "sg/X");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "sg/X", "<init>", "()V");
            e.typeInsn(Op::NEW, "sg/Y");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "sg/Y", "<init>", "()V");
            e.invoke(Op::INVOKEVIRTUAL, "sg/X", "run", "()V");
            e.op(Op::POP);
            e.invoke(Op::INVOKEVIRTUAL, "sg/I", "run", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.buildRta();

    const std::string mainDesc = "([Ljava/lang/String;)V";
    // 站点 1（encoded owner = X）：Y.run 与接收者无关且无已实例化子型桥接 →
    // 筛掉；X.run 保留。
    bool xSiteHasXRun   = false;
    ca::usize xSiteYRun = 0;
    for (const ResolvedCallEdge& edge : fx.graph().edgesFrom("sg/SGMain", "main", mainDesc)) {
        if (edge.encodedTarget.owner != "sg/X") continue;   // 只看站点 1
        if (edge.resolvedTarget.owner == "sg/X" && edge.resolvedTarget.name == "run") {
            xSiteHasXRun = true;
        }
        if (edge.resolvedTarget.owner == "sg/Y") ++xSiteYRun;
    }
    EXPECT_TRUE(xSiteHasXRun);
    EXPECT_EQ(xSiteYRun, 0u);
    // 站点 2（接收者 I）：两个已实例化实现类的条目都可行。
    EXPECT_TRUE(hasEdge(fx.graph(), "sg/SGMain", "main", mainDesc, "sg/X", "run", "()V"));
    EXPECT_TRUE(hasEdge(fx.graph(), "sg/SGMain", "main", mainDesc, "sg/Y", "run", "()V"));
}

// ── 降级与反射口径 ────────────────────────────────────────────

// 外部/缺失虚目标降级为编码目标（与 CHA 同口径）：owner 不在容器的
// invokevirtual 维持精确边，不参与收窄。
TEST(MjResolvedCallGraphRtaTest, externalAndMissingVirtualTargetsDegradeToEncoded)
{
    RtaGraphFixture fx;
    fx.addClass("ed/EMain", instanceClass("ed/EMain", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 2, 0,
        [](CodeEmit& e) {
            e.op(Op::ACONST_NULL);
            e.invoke(Op::INVOKEVIRTUAL, "java/lang/Object", "toString",
                     "()Ljava/lang/String;");
            e.op(Op::POP);
            e.invoke(Op::INVOKEVIRTUAL, "ed/Missing", "run", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.buildRta();

    const std::string mainDesc = "([Ljava/lang/String;)V";
    EXPECT_TRUE(hasEdge(fx.graph(), "ed/EMain", "main", mainDesc, "java/lang/Object",
                        "toString", "()Ljava/lang/String;"));
    EXPECT_TRUE(hasEdge(fx.graph(), "ed/EMain", "main", mainDesc, "ed/Missing", "run", "()V"));
    // main + 两个外部目标。
    EXPECT_EQ(fx.graph().rtaReachableMethodCount(), 3u);
}

// 空描述符反射查找按名落解（GlobalReachability 同口径）：实参数组携带
// Long.TYPE（无同名候选可落解）+ 目标类有两个 run 重载 → 描述符不可唯一
// 落解保持为空 → RTA 按名把全部同名声明入闭包。
TEST(MjResolvedCallGraphRtaTest, emptyDescReflectionLookupResolvesByName)
{
    RtaGraphFixture fx;
    fx.addClass("rb/RBTarget", instanceClass("rb/RBTarget", "", {}, {
        runMethod(),
        m("run", "(I)V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); }),
    }));
    fx.addClass("rb/RBMain", instanceClass("rb/RBMain", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 3, 0,
        [](CodeEmit& e) {
            e.ldcClass("rb/RBTarget");
            e.ldcString("run");
            // Class[]{Long.TYPE}：ANEWARRAY; ICONST_1; DUP; ICONST_0;
            // GETSTATIC Long.TYPE; AASTORE——实参描述符 "(J)" 无同名候选。
            e.typeInsn(Op::ANEWARRAY, "java/lang/Class");
            e.i32const(1);
            e.op(Op::DUP);
            e.i32const(0);
            e.fieldInsn(Op::GETSTATIC, "java/lang/Long", "TYPE", "Ljava/lang/Class;");
            e.op(Op::AASTORE);
            e.invoke(Op::INVOKEVIRTUAL, "java/lang/Class", "getDeclaredMethod",
                     "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.buildRta();

    // 反射事实在索引里保持空描述符。
    bool sawEmptyDescReflection = false;
    for (const auto& ref : fx.index().methodReferences()) {
        if (ref.target.owner == "rb/RBTarget" && ref.target.name == "run" &&
            ref.target.desc.empty() &&
            ref.kind == mjbase::reference::MethodReferenceKind::Reflection) {
            sawEmptyDescReflection = true;
        }
    }
    ASSERT_TRUE(sawEmptyDescReflection) << "夹具未产出空描述符反射事实";
    // 按名落解：两个重载全部入口可达。可达集 = main + 按名落解的 run()/run(I)
    // + 降级的外部目标 Class.getDeclaredMethod。
    EXPECT_EQ(fx.graph().rtaReachableMethodCount(), 4u);
}

// ── 欠近似口径（铁律面：RTA 结果不得直接作删除依据）──────────

// 反射实例化不可见：目标类只经 Class.forName 字符串出现、全程无 new——
// 已实例化集为空。该欠近似是「RTA 结果只作保守根来源」的根因。
TEST(MjResolvedCallGraphRtaTest, reflectiveInstantiationStaysOutOfInstantiatedSet)
{
    RtaGraphFixture fx;
    fx.addClass("ru/RUTarget", instanceClass("ru/RUTarget", "", {}, {m(
        "hello", "()V", 0, 1, [](CodeEmit& e) { e.op(Op::RETURN); })}));
    fx.addClass("ru/RUMain", instanceClass("ru/RUMain", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 1, 0,
        [](CodeEmit& e) {
            e.ldcString("ru/RUTarget");
            e.invoke(Op::INVOKESTATIC, "java/lang/Class", "forName",
                     "(Ljava/lang/String;)Ljava/lang/Class;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.buildRta();

    EXPECT_EQ(fx.graph().rtaInstantiatedCount(), 0u);
    EXPECT_TRUE(fx.graph().instantiatedClasses().empty());
    // 反射目标方法不经静态边可达（欠近似侧：不凭空造边）。
    EXPECT_FALSE(hasEdge(fx.graph(), "ru/RUMain", "main", "([Ljava/lang/String;)V",
                         "ru/RUTarget", "hello", "()V"));
}

// native 声明无方法体：不产生任何出边/入边，不影响 fixpoint（native 体内的
// 实例化按欠近似处理，RTA 看不见）。
TEST(MjResolvedCallGraphRtaTest, nativeDeclarationCarriesNoEdges)
{
    RtaGraphFixture fx;
    fx.addClass("nt/NLib", instanceClass("nt/NLib", "", {}, {m(
        "touch", "()V", 0, 0, nullptr, kAccPublic | kAccNative)}));
    fx.addClass("nt/NMain", instanceClass("nt/NMain", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 2, 1,
        [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "nt/NLib");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "nt/NLib", "<init>", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.buildRta();

    EXPECT_TRUE(containsClass(fx.graph().instantiatedClasses(), "nt/NLib"));
    // touch 无任何边（无体方法既不被调用也不产边）。
    EXPECT_FALSE(hasEdge(fx.graph(), "nt/NMain", "main", "([Ljava/lang/String;)V",
                         "nt/NLib", "touch", "()V"));
    EXPECT_TRUE(hasEdge(fx.graph(), "nt/NMain", "main", "([Ljava/lang/String;)V",
                        "nt/NLib", "<init>", "()V"));
    // main + NLib.<init> + Object.<init>（外部，NLib.<init> 体调用）。
    EXPECT_EQ(fx.graph().rtaReachableMethodCount(), 3u);
}

// classpath 外实例化：new JDK/库类型照实记录进已实例化集（含外部名），但不
// 激活任何容器组件（外部实例的分派链全在外部）；构造走精确 special 边。
TEST(MjResolvedCallGraphRtaTest, externalInstantiationRecordedButInert)
{
    RtaGraphFixture fx;
    fx.addClass("ex/EXMain", instanceClass("ex/EXMain", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 2, 1,
        [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "java/util/ArrayList");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "java/util/ArrayList", "<init>", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.buildRta();

    ASSERT_EQ(fx.graph().instantiatedClasses().size(), 1u);
    EXPECT_EQ(fx.graph().instantiatedClasses().front(), "java/util/ArrayList");
    EXPECT_EQ(fx.graph().rtaInstantiatedCount(), 1u);
    EXPECT_TRUE(hasEdge(fx.graph(), "ex/EXMain", "main", "([Ljava/lang/String;)V",
                        "java/util/ArrayList", "<init>", "()V"));
    EXPECT_EQ(fx.graph().rtaReachableMethodCount(), 2u);
}

// ── opt-in 关闭（CHA 档）零行为变化 ──────────────────────────

// 同一图对象上 build → buildRta → build：CHA 边表逐字段零变化、RTA 计数归零
// ——分析档 opt-in，不调用 buildRta 时行为与现状逐字节一致。
TEST(MjResolvedCallGraphRtaTest, chaProfileRemainsByteIdenticalWhenRtaNotUsed)
{
    RtaGraphFixture fx;
    fx.addClass("vn/Base", instanceClass("vn/Base", "", {}, {runMethod()}));
    fx.addClass("vn/Child", instanceClass("vn/Child", "vn/Base", {}, {runMethod()}));
    fx.addClass("vn/NMain", instanceClass("vn/NMain", "", {}, {mainNewThenRun("vn/Child", "vn/Base")}));
    fx.buildCha();
    // CHA 物化全部索引引用（含不可达方法体）：main 三条（<init> + 展开的
    // Base.run/Child.run）+ NMain/Child/Base 三个默认 <init> → Object.<init>
    // 外部各一条。
    const std::vector<ResolvedCallEdge> chaSnapshot = fx.graph().edges();
    ASSERT_EQ(chaSnapshot.size(), 6u);
    // CHA 档 main 槽位三条边：虚调用点全组件展开。
    EXPECT_EQ(fx.graph().edgesFrom("vn/NMain", "main", "([Ljava/lang/String;)V").size(), 3u);

    fx.buildRta();
    // RTA main 槽位两条边：<init> special + 收窄后的 Child.run。
    EXPECT_EQ(fx.graph().edgesFrom("vn/NMain", "main", "([Ljava/lang/String;)V").size(), 2u);

    fx.buildCha();
    EXPECT_TRUE(sameEdges(fx.graph().edges(), chaSnapshot));
    EXPECT_EQ(fx.graph().rtaInstantiatedCount(), 0u);
    EXPECT_EQ(fx.graph().rtaReachableMethodCount(), 0u);
    EXPECT_TRUE(fx.graph().instantiatedClasses().empty());
}

// ── 根策略 ────────────────────────────────────────────────────

// 入口可达性驱动：无根（关 main 发现、无显式根）→ 零边零闭包；显式根方法
// 驱动同一闭包（roots 变化即边集变化）。
TEST(MjResolvedCallGraphRtaTest, entryRootsDriveTheClosure)
{
    RtaGraphFixture fx;
    fx.addClass("fx/App", instanceClass("fx/App", "", {}, {m(
        "main", "([Ljava/lang/String;)V", 2, 1,
        [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "fx/B");
            e.op(Op::DUP);
            e.invoke(Op::INVOKESPECIAL, "fx/B", "<init>", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        },
        kAccPublic | kAccStatic)}));
    fx.addClass("fx/B", instanceClass("fx/B", "", {}, {runMethod()}));

    RtaOptions noRoots;
    noRoots.includeMainMethods = false;
    fx.buildRta(noRoots);
    EXPECT_EQ(fx.graph().rtaReachableMethodCount(), 0u);
    EXPECT_EQ(fx.graph().rtaInstantiatedCount(), 0u);
    EXPECT_EQ(fx.graph().edgeCount(), 0u);

    RtaOptions explicitRoots;
    explicitRoots.includeMainMethods = false;
    explicitRoots.rootMethods.push_back(MethodRefKey{"fx/App", "main", "([Ljava/lang/String;)V"});
    fx.buildRta(explicitRoots);
    EXPECT_EQ(fx.graph().rtaInstantiatedCount(), 1u);
    EXPECT_TRUE(hasEdge(fx.graph(), "fx/App", "main", "([Ljava/lang/String;)V",
                        "fx/B", "<init>", "()V"));
}

// ── GAM 两档并存 ─────────────────────────────────────────────

// ResolvedCallGraphAnalysis（CHA）与 RtaCallGraphAnalysis（RTA）在同一
// GlobalAnalysisManager 上并存：CHA 图计数恒 0，RTA 图携带实例化集。
TEST(MjResolvedCallGraphRtaTest, gamWrappersKeepChaAndRtaProfilesCoexist)
{
    RtaGraphFixture fx;
    fx.addClass("vn/Base", instanceClass("vn/Base", "", {}, {runMethod()}));
    fx.addClass("vn/Child", instanceClass("vn/Child", "vn/Base", {}, {runMethod()}));
    fx.addClass("vn/NMain", instanceClass("vn/NMain", "", {}, {mainNewThenRun("vn/Child", "vn/Base")}));
    fx.buildRta();

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    gam.registerAnalysis<mjbase::reference::ReferenceIndexAnalysis>();
    gam.registerAnalysis<mjbase::reference::CallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::HierarchyBundleAnalysis>();
    gam.registerAnalysis<mjbase::reference::ResolvedCallGraphAnalysis>();
    gam.registerAnalysis<mjbase::reference::RtaCallGraphAnalysis>();

    auto& cha = gam.getResult<mjbase::reference::ResolvedCallGraphAnalysis>(fx.container());
    auto& rta = gam.getResult<mjbase::reference::RtaCallGraphAnalysis>(fx.container());

    EXPECT_EQ(cha.rtaInstantiatedCount(), 0u);
    // CHA 物化全部索引引用：main 三条（<init> + Base.run/Child.run）+ 三个
    // 默认 <init> → Object.<init> 外部各一条。
    EXPECT_EQ(cha.edgeCount(), 6u);
    EXPECT_EQ(rta.rtaInstantiatedCount(), 1u);
    EXPECT_TRUE(containsClass(rta.instantiatedClasses(), "vn/Child"));
    // RTA 全图：main 两条（<init> + 收窄后的 Child.run）+ Object.<init> 一条。
    EXPECT_EQ(rta.edgeCount(), 3u);
}

}   // namespace
