// UncontendedMonitorEliminater：方法内受限监视器对整组摘除——junk
// synchronized 无害化正面、无副作用实参游程随组摘除、参数接收者/逃逸读/
// 体内标签/副作用实参 init/不成对逐面拒收、多组独立摘除、幂等写回、
// 注册面。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/uncontended_monitor_eliminator.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "fix_pattern_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 头段：NEW X; DUP; <init>()V; ASTORE_1（紧凑形态，槽 1）。
void buildMonitorHead(Body& b, const char* cls)
{
    b.invoke(Op::NEW, b.session().internClass(cls));
    b.dup();
    b.invoke(Op::INVOKESPECIAL, b.session().internMethodRef(cls, "<init>", "()V"));
    b.op(Op::ASTORE_1);
}

}   // namespace

// ── 正面：confined 监控对整组摘除（体内指令保留）──
TEST(UncontendedMonitorEliminaterTest, junkMonitorRemoved)
{
    DomFixture fx("t/M", "m", "()V", 4, 2, [&](Body& b) {
        buildMonitorHead(b, "java/lang/Object");
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.iconst_1();
        b.pop();
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UncontendedMonitorEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 3u);   // iconst_1; pop; vreturn
}

// ── 局部装载实参（ALOAD 前值引用）随整组摘除 ──
TEST(UncontendedMonitorEliminaterTest, loadArgInitRemoved)
{
    DomFixture fx("t/M", "m", "()V", 4, 3, [&](Body& b) {
        b.aconstNull();
        b.op(Op::ASTORE_2);   // 槽 2 前值，作 <init> 实参
        b.invoke(Op::NEW, b.session().internClass("t/C"));
        b.dup();
        b.op(Op::ALOAD_2);
        b.invoke(Op::INVOKESPECIAL,
                 b.session().internMethodRef("t/C", "<init>",
                                             "(Ljava/lang/Object;)V"));
        b.op(Op::ASTORE_1);
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.iconst_1();
        b.pop();
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UncontendedMonitorEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 5u);   // aconst_null; astore_2; iconst_1; pop; vreturn
}

// ── 参数接收者（def = Parameter，非 confined）不动 ──
TEST(UncontendedMonitorEliminaterTest, paramMonitorKept)
{
    DomFixture fx("t/M", "m", "(Ljava/lang/Object;)V", 4, 1, [&](Body& b) {
        b.op(Op::ALOAD_0);
        b.monitorenter();
        b.op(Op::ALOAD_0);
        b.monitorexit();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UncontendedMonitorEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 5u);
}

// ── 对象逃逸（槽位第三个读点喂 areturn）不动 ──
TEST(UncontendedMonitorEliminaterTest, escapedObjectKept)
{
    DomFixture fx("t/M", "m", "()Ljava/lang/Object;", 4, 2, [&](Body& b) {
        buildMonitorHead(b, "java/lang/Object");
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.op(Op::ALOAD_1);
        b.areturn();
    });
    const auto pa = fx.run(mjbase::pass::UncontendedMonitorEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 10u);
}

// ── enter..exit 区间内标签锚定（跳入体内绕过 enter）不动 ──
TEST(UncontendedMonitorEliminaterTest, labelInsideBodyKept)
{
    DomFixture fx("t/M", "m", "()V", 4, 2, [&](Body& b) {
        jbc::ir::LabelId tgt = b.label();
        buildMonitorHead(b, "java/lang/Object");
        b.iconst_1();
        b.branch(Op::IFEQ, tgt);
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.bindHere(tgt);
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UncontendedMonitorEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
}

// ── 副作用实参 <init>（调用实参不可擦，擦除即语义变化）不动 ──
TEST(UncontendedMonitorEliminaterTest, callArgInitKept)
{
    DomFixture fx("t/M", "m", "()V", 4, 2, [&](Body& b) {
        b.invoke(Op::NEW, b.session().internClass("java/lang/Object"));
        b.dup();
        b.invoke(Op::INVOKESTATIC,
                 b.session().internMethodRef("t/S", "f", "()Ljava/lang/Object;"));
        b.invoke(Op::INVOKESPECIAL,
                 b.session().internMethodRef("java/lang/Object", "<init>",
                                             "(Ljava/lang/Object;)V"));
        b.op(Op::ASTORE_1);
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UncontendedMonitorEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 10u);
}

// ── 无副作用实参游程（null + 常量双实参）随整组摘除 ──
TEST(UncontendedMonitorEliminaterTest, constArgInitRemoved)
{
    DomFixture fx("t/M", "m", "()V", 4, 2, [&](Body& b) {
        b.invoke(Op::NEW, b.session().internClass("t/C"));
        b.dup();
        b.aconstNull();
        b.iconst_5();
        b.invoke(Op::INVOKESPECIAL,
                 b.session().internMethodRef("t/C", "<init>",
                                             "(Ljava/lang/Object;I)V"));
        b.op(Op::ASTORE_1);
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.iconst_1();
        b.pop();
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UncontendedMonitorEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 3u);   // iconst_1; pop; vreturn
}

// ── 不成对（两 enter 一 exit）不动 ──
TEST(UncontendedMonitorEliminaterTest, unpairedKept)
{
    DomFixture fx("t/M", "m", "()V", 4, 2, [&](Body& b) {
        buildMonitorHead(b, "java/lang/Object");
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UncontendedMonitorEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 11u);
}

// ── 多组独立 junk 监控块逐一摘除 ──
TEST(UncontendedMonitorEliminaterTest, twoBlocksBothRemoved)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        buildMonitorHead(b, "java/lang/Object");
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.iconst_1();
        b.pop();
        buildMonitorHead(b, "java/lang/String");
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UncontendedMonitorEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 3u);   // iconst_1; pop; vreturn
}

// ── 幂等：已收敛方法再跑不动；写回一致 ──
TEST(UncontendedMonitorEliminaterTest, idempotentAndWriteBack)
{
    DomFixture fx("t/M", "m", "()V", 4, 2, [&](Body& b) {
        buildMonitorHead(b, "java/lang/Object");
        b.op(Op::ALOAD_1);
        b.monitorenter();
        b.op(Op::ALOAD_1);
        b.monitorexit();
        b.vreturn();
    });
    ASSERT_TRUE(fx.run(mjbase::pass::UncontendedMonitorEliminater{}).changed());
    EXPECT_TRUE(fx.run(mjbase::pass::UncontendedMonitorEliminater{}).areAllPreserved());
    const Bytes out = fx.writeBack();
    EXPECT_FALSE(out.empty());
}

// ── 注册面 ──
TEST(UncontendedMonitorEliminaterTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("UncontendedMonitorEliminater"));
    auto created =
        mjbase::spi::defaultPassRegistry().createMethodPass("UncontendedMonitorEliminater");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
