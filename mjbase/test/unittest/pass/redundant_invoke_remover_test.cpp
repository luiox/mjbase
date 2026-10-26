// RedundantInvokeRemover（全局件）：容器内定义 × 弹弃消费 × 纯度提名的
// 调用点整组摘除（委派 MethodCallRemover 点名面）——纯方法弹弃删、
// 只读/非纯/返回被消费/容器外目标保留、跨类全应用、被调方不改写、
// 注册面。夹具经 shared/structural_pass_util.hpp 手拼 classfile；方法体
// 断言经会话 promote 后的指令序完成。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/pass/redundant_invoke_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublicStatic = 0x0009;

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

/// u16 大端两字节。
Bytes u16b(ca::u16 v) { return {static_cast<ca::u8>(v >> 8), static_cast<ca::u8>(v)}; }

/// 拼接指令片段。
Bytes cat(Bytes a, Bytes b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

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
        EXPECT_TRUE(promoted.is_ok()) << "promote failed: " << methodName;
        if (promoted.is_err()) return {};
        auto                       cuRef = std::move(promoted).unwrap();
        std::vector<jbc::core::Op> ops;
        for (const jbc::ir::InsnId id : cuRef->dom.order()) {
            const jbc::ir::Insn* in = cuRef->dom.peek(id);
            EXPECT_NE(in, nullptr);
            if (in == nullptr) return {};
            ops.push_back(in->op);
        }
        return ops;
    }
    ADD_FAILURE() << "method not found: " << methodName;
    return {};
}

const mjbase::model::ClassResource* classResource(mjbase::model::ResourceContainer& container,
                                                  const std::string&                uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

/// 被调方类：pick ()I 纯（常量返回）+ rw ()I 只读（getstatic 读）+
/// write ()V 非纯（putstatic 写）。
Bytes buildCalleeClass()
{
    FixtureBuilder builder("Callee");
    builder.addMethod(method("pick", "()I", {0x04, 0xac}));   // ICONST_1; IRETURN
    {
        const ca::u16 fld = builder.fieldRef("Callee", "state", "I");
        // GETSTATIC fld; IRETURN —— 只读面。
        builder.addMethod(method("rw", "()I", cat(cat({0xb2}, u16b(fld)), {0xac})));
    }
    {
        const ca::u16 fld = builder.fieldRef("Callee", "state", "I");
        // ICONST_1; PUTSTATIC fld; RETURN —— 写堆面。
        builder.addMethod(
            method("write", "()V", cat(cat({0x04, 0xb3}, u16b(fld)), {0xb1})));
    }
    return builder.assemble();
}

mjbase::spi::PreservedAnalyses runPass(mjbase::model::ResourceContainer& container)
{
    ca::str::Utf8StringArena            arena;
    mjbase::spi::GlobalAnalysisManager  gam(arena);
    mjbase::pass::RedundantInvokeRemover pass;
    return pass.run(container, gam);
}

}   // namespace

// ── 纯方法弹弃调用点整组摘除 ──
TEST(RedundantInvokeRemoverTest, pureDiscardedCallRemoved)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Callee.class", buildCalleeClass());
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Callee", "pick", "()I");
        // INVOKESTATIC pick; POP; RETURN
        builder.addMethod(
            method("go", "()V", cat(invokeStatic(mref), {0x57, 0xb1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }

    EXPECT_TRUE(runPass(container).changed());

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    const std::vector<jbc::core::Op> ops = methodOps(caller->get(), "go");
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], jbc::core::Op::RETURN);
}

// ── 只读（非 Pure）被调方不提名 ──
TEST(RedundantInvokeRemoverTest, readOnlyCalleeKept)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Callee.class", buildCalleeClass());
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Callee", "rw", "()I");
        builder.addMethod(
            method("go", "()V", cat(invokeStatic(mref), {0x57, 0xb1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    runPass(container);
    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    const std::vector<jbc::core::Op> ops = methodOps(caller->get(), "go");
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], jbc::core::Op::INVOKESTATIC);
}

// ── 非纯（写堆）被调方不提名 ──
TEST(RedundantInvokeRemoverTest, impureCalleeKept)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Callee.class", buildCalleeClass());
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Callee", "write", "()V");
        builder.addMethod(method("go", "()V", cat(invokeStatic(mref), {0xb1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    runPass(container);
    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    const std::vector<jbc::core::Op> ops = methodOps(caller->get(), "go");
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], jbc::core::Op::INVOKESTATIC);
}

// ── 返回值被消费（IRETURN）的调用点保留 ──
TEST(RedundantInvokeRemoverTest, consumedResultKept)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Callee.class", buildCalleeClass());
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Callee", "pick", "()I");
        builder.addMethod(method("go", "()I", cat(invokeStatic(mref), {0xac})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    runPass(container);
    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    const std::vector<jbc::core::Op> ops = methodOps(caller->get(), "go");
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], jbc::core::Op::INVOKESTATIC);
    EXPECT_EQ(ops[1], jbc::core::Op::IRETURN);
}

// ── 提名后跨类全应用：两个 Caller 的弹弃点都摘除 ──
TEST(RedundantInvokeRemoverTest, nominationAppliesAcrossClasses)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Callee.class", buildCalleeClass());
    {
        FixtureBuilder builder("CallerA");
        const ca::u16  mref = builder.methodRef("Callee", "pick", "()I");
        builder.addMethod(
            method("go", "()V", cat(invokeStatic(mref), {0x57, 0xb1})));
        addClassResource(container, "CallerA.class", builder.assemble());
    }
    {
        FixtureBuilder builder("CallerB");
        const ca::u16  mref = builder.methodRef("Callee", "pick", "()I");
        builder.addMethod(
            method("go", "()V", cat(invokeStatic(mref), {0x57, 0xb1})));
        addClassResource(container, "CallerB.class", builder.assemble());
    }

    EXPECT_TRUE(runPass(container).changed());
    for (const char* uri : {"CallerA.class", "CallerB.class"}) {
        const auto* caller = classResource(container, uri);
        ASSERT_NE(caller, nullptr);
        const std::vector<jbc::core::Op> ops = methodOps(caller->get(), "go");
        ASSERT_EQ(ops.size(), 1u);
        EXPECT_EQ(ops[0], jbc::core::Op::RETURN);
    }
}

// ── 容器外目标（JDK 方法）不提名 ──
TEST(RedundantInvokeRemoverTest, externalTargetKept)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Callee.class", buildCalleeClass());
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("java/lang/System", "nanoTime", "()J");
        // INVOKESTATIC nanoTime; POP2; RETURN（宽度匹配的弹弃但目标在容器外）
        builder.addMethod(
            method("go", "()V", cat(invokeStatic(mref), {0x58, 0xb1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    runPass(container);
    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    const std::vector<jbc::core::Op> ops = methodOps(caller->get(), "go");
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], jbc::core::Op::INVOKESTATIC);
}

// ── 被调方定义不改写 ──
TEST(RedundantInvokeRemoverTest, calleeBodyUntouched)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Callee.class", buildCalleeClass());
    {
        FixtureBuilder builder("Caller");
        const ca::u16  mref = builder.methodRef("Callee", "pick", "()I");
        builder.addMethod(
            method("go", "()V", cat(invokeStatic(mref), {0x57, 0xb1})));
        addClassResource(container, "Caller.class", builder.assemble());
    }
    const Bytes before = classResource(container, "Callee.class")->get();
    runPass(container);
    EXPECT_EQ(classResource(container, "Callee.class")->get(), before);
}

// ── 注册面 ──
TEST(RedundantInvokeRemoverTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("RedundantInvokeRemover"));
    auto created =
        mjbase::spi::defaultPassRegistry().createGlobalPass("RedundantInvokeRemover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
