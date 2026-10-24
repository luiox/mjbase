/// MethodCallRemover（调用点收尾件，opt pass 波次）单测：点名删调用点的栈
/// 等价（四族 invoke / cat2 吞参 / 死局部 store / void 吞参）、放弃路径
/// （dup 前缀 / 方法句柄钉死 / 引用锚定 / 返回值被消费 / 容器不可读）、
/// 调用图边同步（none() 失效驱动重建）、默认 no-op 与无用结果白名单
/// 可配置面。夹具经 shared/structural_pass_util.hpp 手拼 classfile；方法体
/// 断言经 libjbc 会话 promote 后的 CodeDom 指令序完成（不依赖字节偏移）。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/asm/classpath/call_graph.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/remove_method_calls.hpp"
#include "mjbase/reference/global_reachability.hpp"
#include "mjbase/reference/reference_index_analysis.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

using mjbase::pass::MethodCallRemovalConfig;
using mjbase::pass::MethodCallRemover;
using mjbase::pass::MethodCallSpec;

constexpr ca::u16 kAccPublicStatic = 0x0009;

// ── 夹具构造 ─────────────────────────────────────────────────────────────────

/// FixtureMethod 便捷装配（public static，栈深给足余量）。
FixtureMethod method(const char* name, const char* desc, Bytes code)
{
    FixtureMethod m;
    m.name      = name;
    m.desc      = desc;
    m.access    = kAccPublicStatic;
    m.maxStack  = 6;
    m.maxLocals = 2;
    m.code      = std::move(code);
    return m;
}

/// 被调用方类：pick()I（返回 1）+ keep()V。调用点删除不得改写它。
Bytes buildCalleeClass(const char* name)
{
    FixtureBuilder builder(name);
    builder.addMethod(method("pick", "()I", {0x04, 0xAC}));       // ICONST_1; IRETURN
    builder.addMethod(method("keep", "()V", bareReturn()));
    return builder.assemble();
}

/// 指令片段顺序拼接（右值友好版 appendBytes；重载到 5 段足够单测用）。
inline Bytes chain(Bytes a, Bytes b)
{
    appendBytes(a, std::move(b));
    return a;
}

inline Bytes chain(Bytes a, Bytes b, Bytes c)
{
    appendBytes(a, std::move(b));
    appendBytes(a, std::move(c));
    return a;
}

inline Bytes chain(Bytes a, Bytes b, Bytes c, Bytes d)
{
    appendBytes(a, std::move(b));
    appendBytes(a, std::move(c));
    appendBytes(a, std::move(d));
    return a;
}

inline Bytes chain(Bytes a, Bytes b, Bytes c, Bytes d, Bytes e)
{
    appendBytes(a, std::move(b));
    appendBytes(a, std::move(c));
    appendBytes(a, std::move(d));
    appendBytes(a, std::move(e));
    return a;
}

/// 常用指令片段。
Bytes invokeVirtual(ca::u16 cp)
{
    return {0xB6, static_cast<ca::u8>(cp >> 8), static_cast<ca::u8>(cp)};
}

Bytes invokeSpecial(ca::u16 cp)
{
    return {0xB7, static_cast<ca::u8>(cp >> 8), static_cast<ca::u8>(cp)};
}

Bytes invokeInterface(ca::u16 cp)
{
    return {0xB9, static_cast<ca::u8>(cp >> 8), static_cast<ca::u8>(cp), 0x01, 0x00};
}

Bytes ldc(ca::u16 cp)
{
    // LDC 操作数是 u1：夹具池很小，索引恒在 0xFF 内。
    return {0x12, static_cast<ca::u8>(cp)};
}

/// 点名 include 规格。
MethodCallSpec spec(const char* owner, const char* name, const char* desc)
{
    MethodCallSpec s;
    s.owner = owner;
    s.name  = name;
    s.desc  = desc;
    return s;
}

/// Used.pick()I 的 include 规格（本文件最常用点名）。
MethodCallSpec pickSpec() { return spec("Used", "pick", "()I"); }

/// 读回 class 字节中某方法 promote 后的指令序（按方法名取第一个命中）。
std::vector<jbc::core::Op> methodOps(const Bytes& bytes, const char* methodName)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok()) << "session open failed: " << methodName;
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (unit.symbols().str(unit.methods[idx].name) != methodName) continue;
        if (!jbc::ir::hasCode(*session, idx)) return {};
        auto promoted = jbc::ir::promoteMethod(*session, idx);
        EXPECT_TRUE(promoted.is_ok())
            << "promote failed: " << methodName << ": "
            << (promoted.is_err() ? jbc::ir::to_string(promoted.unwrap_err()) : "");
        if (promoted.is_err()) return {};
        auto                       cuRef = std::move(promoted).unwrap();
        std::vector<jbc::core::Op> ops;
        for (const jbc::ir::InsnId id : cuRef->dom.order()) {
            const jbc::ir::Insn* in = cuRef->dom.peek(id);
            EXPECT_NE(in, nullptr) << "dead insn slot: " << methodName;
            if (in == nullptr) return {};
            ops.push_back(in->op);
        }
        return ops;
    }
    ADD_FAILURE() << "method not found: " << methodName;
    return {};
}

/// 方法体是否仍含 invoke 家族指令。
bool hasInvokeOp(const Bytes& bytes, const char* methodName)
{
    for (const jbc::core::Op op : methodOps(bytes, methodName)) {
        if (op == jbc::core::Op::INVOKEVIRTUAL || op == jbc::core::Op::INVOKESPECIAL ||
            op == jbc::core::Op::INVOKESTATIC || op == jbc::core::Op::INVOKEINTERFACE) {
            return true;
        }
    }
    return false;
}

const mjbase::model::ClassResource* classResource(mjbase::model::ResourceContainer& container,
                                                  const std::string&                uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

/// 跑一次 pass 的便利壳（AM 仅作管线上下文，本 pass 不消费分析）。
mjbase::spi::PreservedAnalyses runPass(MethodCallRemover&                 pass,
                                       mjbase::model::ResourceContainer&  container)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    return pass.run(container, gam);
}

}   // namespace

// ── 默认行为与点名删调用点 ───────────────────────────────────────────────────

TEST(MethodCallRemover, NoConfigIsCompleteNoOp)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        builder.addMethod(method("go", "()V", chain(invokeStatic(mref), {0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    const auto* before = classResource(container, "Caller.class");
    ASSERT_NE(before, nullptr);
    const Bytes original = before->get();

    MethodCallRemover pass;   // 默认配置：两面全关
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    const auto* after = classResource(container, "Caller.class");
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->get(), original);
}

TEST(MethodCallRemover, RemovesStaticCallWithSwallowedCat1Result)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        builder.addMethod(method("go", "()V", chain(invokeStatic(mref), {0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes callerBefore = classResource(container, "Caller.class")->get();
    const Bytes usedBefore   = classResource(container, "Used.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    const auto* used   = classResource(container, "Used.class");
    ASSERT_NE(caller, nullptr);
    ASSERT_NE(used, nullptr);
    EXPECT_EQ(used->get(), usedBefore) << "被点名成员所在类不得改写";
    const auto ops = methodOps(caller->get(), "go");
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], jbc::core::Op::RETURN) << "invokestatic + pop 应整窗消失";
    EXPECT_NE(caller->get(), callerBefore);
}

TEST(MethodCallRemover, RemovesAllFourInvokeFamilies)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  st  = builder.methodRef("Used", "keep", "()V");
        const ca::u16  vt  = builder.methodRef("Used", "virt", "()V");
        const ca::u16  sp  = builder.methodRef("Used", "priv", "()V");
        const ca::u16  itf = builder.interfaceMethodRef("Itf", "f", "()V");
        builder.addMethod(method("mStatic", "()V", chain(invokeStatic(st), {0xB1})));
        // aconst_null; invokevirtual; return —— void 接收者吞 pop。
        builder.addMethod(method("mVirtual", "()V",
                                 chain({0x01}, invokeVirtual(vt), {0xB1})));
        builder.addMethod(method("mSpecial", "()V",
                                 chain({0x01}, invokeSpecial(sp), {0xB1})));
        builder.addMethod(method("mIface", "()V",
                                 chain({0x01}, invokeInterface(itf), {0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "keep", "()V"));
    config.includeCalls.push_back(spec("Used", "virt", "()V"));
    config.includeCalls.push_back(spec("Used", "priv", "()V"));
    config.includeCalls.push_back(spec("Itf", "f", "()V"));
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    // static：无参无返回 → 纯 [RETURN]。
    EXPECT_EQ(methodOps(caller->get(), "mStatic"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
    // virtual/special/interface：aconst_null 生产者保留，接收者由补入的
    // POP 吞掉（原位插入在调用点之前）。
    for (const char* m : {"mVirtual", "mSpecial", "mIface"}) {
        EXPECT_EQ(methodOps(caller->get(), m),
                  (std::vector<jbc::core::Op>{jbc::core::Op::ACONST_NULL, jbc::core::Op::POP,
                                              jbc::core::Op::RETURN}))
            << m;
    }
}

TEST(MethodCallRemover, SwallowsCat2ResultAndCat2Args)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  jret = builder.methodRef("Used", "jl", "()J");
        const ca::u16  jarg = builder.methodRef("Used", "pl", "(J)I");
        // invokestatic ()J; pop2; return → [RETURN]。
        builder.addMethod(method("mLongRet", "()V",
                                 chain(invokeStatic(jret), {0x58, 0xB1})));
        // lconst_1; invokestatic (J)I; pop; return → [LCONST_1, POP2, POP, RETURN]。
        builder.addMethod(method("mLongArg", "()V",
                                 chain({0x0A}, invokeStatic(jarg), {0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "jl", "()J"));
    config.includeCalls.push_back(spec("Used", "pl", "(J)I"));
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    EXPECT_EQ(methodOps(caller->get(), "mLongRet"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
    // cat2 实参吞 POP2；返回值消费面（POP）随调用一并消失，生产者保留。
    EXPECT_EQ(methodOps(caller->get(), "mLongArg"),
              (std::vector<jbc::core::Op>{jbc::core::Op::LCONST_1, jbc::core::Op::POP2,
                                          jbc::core::Op::RETURN}));
}

TEST(MethodCallRemover, VoidCallArgsSwallowedByPops)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  str = builder.strConst("x");
        const ca::u16  one = builder.methodRef("Used", "takes", "(IJLjava/lang/String;)V");
        builder.addMethod(
            method("go", "()V", chain({0x04, 0x0A}, ldc(str), invokeStatic(one), {0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "takes", "(IJLjava/lang/String;)V"));
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    // iconst_1 + lconst_0 + ldc 生产者保留；吞参序 POP(I) POP2(J) POP(ref)。
    const auto ops = methodOps(caller->get(), "go");
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[3], jbc::core::Op::POP);
    EXPECT_EQ(ops[4], jbc::core::Op::POP2);
    EXPECT_EQ(ops[5], jbc::core::Op::POP);
    EXPECT_EQ(ops[6], jbc::core::Op::RETURN);
}

TEST(MethodCallRemover, DeadLocalStoreResultRemovedLiveStoreKept)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        // 结果存入死局部：istore_1 后全方法无 iload 1 → 随调用整删。
        builder.addMethod(method("deadStore", "()V",
                                 chain(invokeStatic(mref), {0x3C, 0xB1})));
        // 存入活局部：后文 iload_1 读 → 调用保留。
        builder.addMethod(method(
            "liveStore", "()V",
            chain(invokeStatic(mref), {0x3C, 0x1B, 0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    EXPECT_EQ(methodOps(caller->get(), "deadStore"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
    EXPECT_TRUE(hasInvokeOp(caller->get(), "liveStore")) << "活局部 store 的调用必须保留";
}

// ── 放弃路径（宁可留调用，不可产生非法栈）────────────────────────────────────

TEST(MethodCallRemover, DupPrefixConstructorSiteAbandoned)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  ctor = builder.methodRef("Used", "<init>", "()V");
        const ca::u16  cls  = builder.cls("Used");
        // new Used; dup; invokespecial <init>; pop; return —— dup 前缀放弃。
        const Bytes code = {0xBB, static_cast<ca::u8>(cls >> 8), static_cast<ca::u8>(cls),
                            0x59,
                            0xB7, static_cast<ca::u8>(ctor >> 8), static_cast<ca::u8>(ctor),
                            0x57, 0xB1};
        builder.addMethod(method("make", "()V", code));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "<init>", "()V"));
    MethodCallRemover pass(config);
    EXPECT_TRUE(runPass(pass, container).areAllPreserved()) << "dup 前缀调用点保守放弃";
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, Dup2PrefixStaticSiteAbandoned)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pl", "(J)V");
        // lconst_1; dup2; invokestatic (J)V; return —— dup2 前缀放弃。
        const Bytes code = {0x0A, 0x5C,
                            0xB8, static_cast<ca::u8>(mref >> 8), static_cast<ca::u8>(mref),
                            0xB1};
        builder.addMethod(method("go", "()V", code));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(spec("Used", "pl", "(J)V"));
    MethodCallRemover pass(config);
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, MethodHandlePinnedMemberAbandoned)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref    = builder.methodRef("Used", "pick", "()I");
        const ca::u16  handle  = builder.methodHandle(6, mref);   // REF_invokeStatic
        // ldc (MethodHandle pick); pop; invokestatic pick()I; pop; return。
        Bytes code = ldc(handle);
        appendBytes(code, {0x57});
        appendBytes(code, invokeStatic(mref));
        appendBytes(code, {0x57, 0xB1});
        builder.addMethod(method("go", "()V", code));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(config);
    EXPECT_TRUE(runPass(pass, container).areAllPreserved())
        << "句柄钉死的成员整体放弃：删调用与删除意图矛盾";
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, ExcludeCancelsInclude)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        builder.addMethod(method("go", "()V", chain(invokeStatic(mref), {0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    config.excludeCalls.push_back(pickSpec());
    MethodCallRemover pass(config);
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, UnreadableClassAbortsWholePass)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        builder.addMethod(method("go", "()V", chain(invokeStatic(mref), {0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    // 畸形 class 资源：句柄钉死证明需要全容器可读，任一不可读整 pass 放弃。
    container.addResource("broken.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "broken.class", Bytes{0xCA, 0xFE, 0xBA, 0xBE, 0x00}));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(config);
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, BranchAnchoredSiteKept)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        // goto +3 直落到 invoke 指令上：调用点被分支锚定 → 放弃。
        Bytes code = {0xA7, 0x00, 0x03};
        appendBytes(code, invokeStatic(mref));
        code.push_back(0xB1);
        builder.addMethod(method("go", "()V", code));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(config);
    EXPECT_TRUE(runPass(pass, container).areAllPreserved()) << "分支锚定的调用点保守放弃";
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, ConsumedResultSiteAbandoned)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        // invokestatic; iconst_1; iadd; pop; return —— 返回值被消费 → 放弃。
        Bytes code = invokeStatic(mref);
        appendBytes(code, {0x04, 0x60, 0x57, 0xB1});
        builder.addMethod(method("go", "()V", code));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(config);
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

// ── 调用图边同步 ─────────────────────────────────────────────────────────────

TEST(MethodCallRemover, CallGraphEdgesResyncAfterRemoval)
{
    namespace ref = mjbase::reference;
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("App");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        builder.addMethod(method("main", "()V", chain(invokeStatic(mref), {0x57, 0xB1})));
        addClassResource(container, "App.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    gam.registerAnalysis<ref::ReferenceIndexAnalysis>();
    gam.registerAnalysis<ref::CallGraphAnalysis>();

    auto keepKey = [](mjbase::asm_classpath::CallGraph& graph) {
        return mjbase::memberKeyOf(graph.pool(), mjbase::classKeyOf(graph.pool(), "Used"),
                                   "pick", "()I");
    };

    auto& graphBefore = gam.getResult<ref::CallGraphAnalysis>(container);
    ASSERT_EQ(graphBefore.incomingSitesOf(keepKey(graphBefore)).size(), 1u);

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    // 经 PassManager 跑：失效由编排器按 none() 驱动——后续消费者重建出的
    // 调用图不再有被删调用边。
    mjbase::spi::GlobalPassManager mgr;
    mgr.addPass(std::make_unique<MethodCallRemover>(config));
    EXPECT_FALSE(mgr.run(container, gam).areAllPreserved());

    auto& graphAfter = gam.getResult<ref::CallGraphAnalysis>(container);
    EXPECT_TRUE(graphAfter.incomingSitesOf(keepKey(graphAfter)).empty());
    EXPECT_EQ(graphAfter.callSiteCountOf(keepKey(graphAfter)), 0u);
}

TEST(MethodCallRemover, BranchyMethodRewrittenWithFrameRewire)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        // 调用点在分支方法头部（分支目标都不落在删除区间内）：
        //   0  invokestatic pick()I   (3 字节)
        //   3  pop
        //   4  iload_0
        //   5  ifeq +8 → 13
        //   8  iconst_0; pop
        //  10  goto +5 → 15
        //  13  iconst_1; pop
        //  15  return
        // 方法带分支 → 写回走 Strict 帧再生链（V52 夹具，再生后 SMT 在场）。
        Bytes code = invokeStatic(mref);
        appendBytes(code, {0x57,              // pop
                           0x1A,              // iload_0
                           0x99, 0x00, 0x08,  // ifeq +8 → 13（目标 = 本指令 pc 5 + 8）
                           0x03, 0x57,        // iconst_0; pop
                           0xA7, 0x00, 0x05,  // goto +5 → 15（目标 = 本指令 pc 10 + 5）
                           0x04, 0x57,        // iconst_1; pop
                           0xB1});            // return
        builder.addMethod(method("go", "(I)V", code));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    // 改写类必须仍可解析（会话重开 + promote 即解析门），调用点消失：
    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    EXPECT_EQ(methodOps(caller->get(), "go"),
              (std::vector<jbc::core::Op>{jbc::core::Op::ILOAD_0, jbc::core::Op::IFEQ,
                                          jbc::core::Op::ICONST_0, jbc::core::Op::POP,
                                          jbc::core::Op::GOTO, jbc::core::Op::ICONST_1,
                                          jbc::core::Op::POP, jbc::core::Op::RETURN}));
    // 帧再生落地：Code 属性带 StackMapTable（Strict 重算产物）。
    auto read = jbc::core::readClass(jbc::core::ByteView(caller->get()));
    ASSERT_TRUE(read.is_ok());
    auto cf = std::move(read).unwrap().file;
    const jbc::core::CodeAttr* codeAttr = nullptr;
    for (auto& member : cf->methods) {
        if (cf->nameOf(member) != "go") continue;
        for (auto& slot : member.attrs) {
            if (!cf->cp.utf8Equals(slot.nameCp, "Code")) continue;
            auto decoded = cf->decodeAttr(slot);
            ASSERT_TRUE(decoded.is_ok()) << "Code attr decode failed";
            codeAttr = ca::core::tag_cast::dyn_cast<jbc::core::CodeAttr>(decoded.unwrap());
        }
    }
    ASSERT_NE(codeAttr, nullptr);
    bool hasSmt = false;
    for (const auto& slot : codeAttr->attrs) {
        if (cf->cp.utf8Equals(slot.nameCp, "StackMapTable")) hasSmt = true;
    }
    EXPECT_TRUE(hasSmt) << "分支体改写后必须再生 StackMapTable";
}

TEST(MethodCallRemover, SecondRunIsStrictNoOp)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Used", "pick", "()I");
        builder.addMethod(method("go", "()V", chain(invokeStatic(mref), {0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.includeCalls.push_back(pickSpec());
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());
    const Bytes afterFirst = classResource(container, "Caller.class")->get();

    EXPECT_TRUE(runPass(pass, container).areAllPreserved()) << "固定点：第二跑严格 no-op";
    EXPECT_EQ(classResource(container, "Caller.class")->get(), afterFirst);
}

// ── 无用结果白名单面 ─────────────────────────────────────────────────────────

TEST(MethodCallRemover, WhitelistStringWindowRemoved)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  str  = builder.strConst("hello");
        const ca::u16  mref = builder.methodRef("java/lang/String", "length", "()I");
        builder.addMethod(
            method("go", "()V", chain(ldc(str), invokeVirtual(mref), {0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;   // 内建默认白名单
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(methodOps(classResource(container, "Caller.class")->get(), "go"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
}

TEST(MethodCallRemover, WhitelistStringNonConstReceiverKept)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("java/lang/String", "length", "()I");
        // aload_0（非常量接收者，可能 NPE）→ 保守保留。
        builder.addMethod(
            method("go", "()V", chain({0x2A}, invokeVirtual(mref), {0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;
    MethodCallRemover pass(config);
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}

TEST(MethodCallRemover, WhitelistMathWindowRemovedNonConstArgKept)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  abs = builder.methodRef("java/lang/Math", "abs", "(I)I");
        // iconst_5; invokestatic Math.abs; pop; return → 整窗删。
        builder.addMethod(
            method("constArg", "()V",
                   chain({0x08}, invokeStatic(abs), {0x57, 0xB1})));
        // iload_0（非常量实参）→ 保留。
        builder.addMethod(
            method("varArg", "()V",
                   chain({0x1A}, invokeStatic(abs), {0x57, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    EXPECT_EQ(methodOps(caller->get(), "constArg"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
    EXPECT_NE(caller->get(), before) << "varArg 保留，类应有差异（constArg 被删）";
    EXPECT_TRUE(hasInvokeOp(caller->get(), "varArg"));
}

TEST(MethodCallRemover, WhitelistCat2JdkCallRemoved)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  nano = builder.methodRef("java/lang/System", "nanoTime", "()J");
        // invokestatic System.nanoTime()J; pop2; return → [RETURN]。
        builder.addMethod(
            method("go", "()V", chain(invokeStatic(nano), {0x58, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(methodOps(classResource(container, "Caller.class")->get(), "go"),
              (std::vector<jbc::core::Op>{jbc::core::Op::RETURN}));
}

TEST(MethodCallRemover, WhitelistUserDiscardableEntrySwallowsArgs)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  tag = builder.strConst("TAG");
        const ca::u16  msg = builder.strConst("msg");
        const ca::u16  log = builder.methodRef(
            "com/example/Log", "d", "(Ljava/lang/String;Ljava/lang/String;)I");
        Bytes code = ldc(tag);
        appendBytes(code, ldc(msg));
        appendBytes(code, invokeStatic(log));
        appendBytes(code, {0x57, 0xB1});
        builder.addMethod(method("go", "()V", code));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));

    MethodCallRemovalConfig config;
    config.enableUnusedResultWhitelist = true;
    config.useDefaultWhitelist         = false;   // 只用用户追加名单
    config.discardableCalls.push_back(
        spec("com/example/Log", "d", "(Ljava/lang/String;Ljava/lang/String;)I"));
    MethodCallRemover pass(config);
    EXPECT_FALSE(runPass(pass, container).areAllPreserved());

    // 调用与结果 pop 消失；实参生产者保留、由吞参 pop 收走。
    const auto ops = methodOps(classResource(container, "Caller.class")->get(), "go");
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], jbc::core::Op::LDC);
    EXPECT_EQ(ops[1], jbc::core::Op::LDC);
    EXPECT_EQ(ops[2], jbc::core::Op::POP);
    EXPECT_EQ(ops[3], jbc::core::Op::POP);
    EXPECT_EQ(ops[4], jbc::core::Op::RETURN);
}

TEST(MethodCallRemover, WhitelistFacetOffByDefault)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder builder("Caller");
        const ca::u16  str  = builder.strConst("hello");
        const ca::u16  len  = builder.methodRef("java/lang/String", "length", "()I");
        const ca::u16  nano = builder.methodRef("java/lang/System", "nanoTime", "()J");
        builder.addMethod(
            method("s", "()V",
                   chain(ldc(str), invokeVirtual(len), {0x57, 0xB1})));
        builder.addMethod(
            method("t", "()V", chain(invokeStatic(nano), {0x58, 0xB1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    addClassResource(container, "Used.class", buildCalleeClass("Used"));
    const Bytes before = classResource(container, "Caller.class")->get();

    MethodCallRemovalConfig config;   // 白名单面默认关
    MethodCallRemover pass(config);
    EXPECT_TRUE(runPass(pass, container).areAllPreserved());
    EXPECT_EQ(classResource(container, "Caller.class")->get(), before);
}
