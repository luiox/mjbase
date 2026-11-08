// RedundantInvokeRemover（全局件）：容器内定义 × 弹弃消费 × 纯度提名的
// 调用点整组摘除（委派 MethodCallRemover 点名面）——纯方法弹弃删、
// 只读/非纯/返回被消费/容器外目标保留、跨类全应用、被调方不改写、
// 注册面。夹具经 jasm 文本装配（JvmJasmService）；方法体断言经会话
// promote 后的指令序完成。
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

#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;

namespace {

/// 单静态方法类夹具：internalName 定类名，methodDecl 为 .method 声明文本
///（含 body，不含收尾 `}`——由本 helper 闭合类体）。
Bytes oneClass(const std::string& internalName, const std::string& methodDecl)
{
    return assembleJasm(R"JASM(
.super java/lang/Object
.class public super )JASM" + internalName + R"JASM( {
)JASM" + methodDecl + R"JASM(}
)JASM");
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
    return oneClass("Callee",
                    R"JASM(  .method public static pick ()I {
    code: {
      iconst_1
      ireturn
    }
  }
  .method public static rw ()I {
    code: {
      getstatic Callee.state I
      ireturn
    }
  }
  .method public static write ()V {
    code: {
      iconst_1
      putstatic Callee.state I
      return
    }
  }
)JASM");
}

/// Caller 单方法夹具：body 为 go 方法体指令片段。
Bytes callerClass(const std::string& internalName, const std::string& desc,
                  const std::string& body)
{
    return oneClass(internalName,
                    "  .method public static go " + desc + R"JASM( {
    code: {
)JASM" + body + R"JASM(    }
  }
)JASM");
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
        addClassResource(container, "Caller.class",
                         callerClass("Caller", "()V",
                                     R"JASM(      invokestatic Callee.pick ()I
      pop
      return
)JASM"));
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
        addClassResource(container, "Caller.class",
                         callerClass("Caller", "()V",
                                     R"JASM(      invokestatic Callee.rw ()I
      pop
      return
)JASM"));
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
        addClassResource(container, "Caller.class",
                         callerClass("Caller", "()V",
                                     R"JASM(      invokestatic Callee.write ()V
      return
)JASM"));
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
        addClassResource(container, "Caller.class",
                         callerClass("Caller", "()I",
                                     R"JASM(      invokestatic Callee.pick ()I
      ireturn
)JASM"));
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
        addClassResource(container, "CallerA.class",
                         callerClass("CallerA", "()V",
                                     R"JASM(      invokestatic Callee.pick ()I
      pop
      return
)JASM"));
    }
    {
        addClassResource(container, "CallerB.class",
                         callerClass("CallerB", "()V",
                                     R"JASM(      invokestatic Callee.pick ()I
      pop
      return
)JASM"));
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
        // INVOKESTATIC nanoTime; POP2; RETURN（宽度匹配的弹弃但目标在容器外）
        addClassResource(container, "Caller.class",
                         callerClass("Caller", "()V",
                                     R"JASM(      invokestatic java/lang/System.nanoTime ()J
      pop2
      return
)JASM"));
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
        addClassResource(container, "Caller.class",
                         callerClass("Caller", "()V",
                                     R"JASM(      invokestatic Callee.pick ()I
      pop
      return
)JASM"));
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
