// CseChainEliminater：GVN/CSE 第一切片——同直线段内同值纯链复写摘除。
// 装载链/计算链/除法链正面、操作数槽覆写/目标槽覆写/异链/区间标签与
// 控制流逐面拒收、跨调用区间摘除、三链保守摘除、幂等写回、注册面。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/cse_chain_eliminater.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "fix_pattern_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单条链形态：iload_1; istore_3。
void buildLoadChain(Body& b)
{
    b.op(Op::ILOAD_1);
    b.op(Op::ISTORE_3);
}

/// 三指令计算链：iload_1; iload_2; iadd; istore_3。
void buildAddChain(Body& b)
{
    b.op(Op::ILOAD_1);
    b.op(Op::ILOAD_2);
    b.iadd();
    b.op(Op::ISTORE_3);
}

}   // namespace

// ── 正面：重复装载链第二条整组摘除 ──
TEST(CseChainEliminaterTest, duplicateLoadRemoved)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        buildLoadChain(b);
        b.iconst_1();
        b.pop();
        buildLoadChain(b);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::CseChainEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 5u);   // iload_1; istore_3; iconst_1; pop; vreturn
}

// ── 正面：计算链（iload; iload; iadd）复写摘除 ──
TEST(CseChainEliminaterTest, computedChainRemoved)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        buildAddChain(b);
        buildAddChain(b);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::CseChainEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 5u);   // 首链 4 条 + vreturn
}

// ── 正面：IDIV 链复写摘除（首链同值已完成求值，重算异常面不可达）──
TEST(CseChainEliminaterTest, divChainRemoved)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        b.op(Op::ILOAD_1);
        b.iconst_2();
        b.idiv();
        b.op(Op::ISTORE_3);
        b.op(Op::ILOAD_1);
        b.iconst_2();
        b.idiv();
        b.op(Op::ISTORE_3);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::CseChainEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 5u);
}

// ── 操作数槽被区间写入 → 链2 值失证，拒收 ──
TEST(CseChainEliminaterTest, operandClobberKept)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        buildLoadChain(b);
        b.iconst_5();
        b.op(Op::ISTORE_1);
        buildLoadChain(b);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::CseChainEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 7u);
}

// ── 目标槽被区间写入 → 拒收 ──
TEST(CseChainEliminaterTest, targetClobberKept)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        buildLoadChain(b);
        b.iconst_2();
        b.op(Op::ISTORE_3);
        buildLoadChain(b);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::CseChainEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 7u);
}

// ── 链结构不同（异槽装载）→ 拒收 ──
TEST(CseChainEliminaterTest, differentChainKept)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        b.op(Op::ILOAD_1);
        b.op(Op::ISTORE_3);
        b.op(Op::ILOAD_2);
        b.op(Op::ISTORE_3);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::CseChainEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 5u);
}

// ── 区间控制流 + 链2 成员标签锚定 → 拒收 ──
TEST(CseChainEliminaterTest, branchAndLabelKept)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        jbc::ir::LabelId tgt = b.label();
        buildLoadChain(b);
        b.iconst_1();
        b.branch(Op::IFEQ, tgt);
        b.bindHere(tgt);
        buildLoadChain(b);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::CseChainEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 7u);
}

// ── 跨调用区间（调用不触局部）正面摘除 ──
TEST(CseChainEliminaterTest, acrossCallRemoved)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        buildLoadChain(b);
        b.invoke(Op::INVOKESTATIC, b.session().internMethodRef("t/S", "f", "()V"));
        buildLoadChain(b);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::CseChainEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);   // 首链 2 条 + invokestatic + vreturn
}

// ── 三链保守：首轮只摘第一对（后续对被链1 存储点外的 t 写拦下）──
TEST(CseChainEliminaterTest, threeChainsFirstPairOnly)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        buildLoadChain(b);
        buildLoadChain(b);
        buildLoadChain(b);
        b.vreturn();
    });
    EXPECT_TRUE(fx.run(mjbase::pass::CseChainEliminater{}).changed());
    EXPECT_TRUE(fx.run(mjbase::pass::CseChainEliminater{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 3u);   // iload_1; istore_3; vreturn（收敛）
}

// ── 幂等：已收敛方法再跑不动；写回一致 ──
TEST(CseChainEliminaterTest, idempotentAndWriteBack)
{
    DomFixture fx("t/M", "m", "()V", 4, 4, [&](Body& b) {
        buildAddChain(b);
        buildAddChain(b);
        b.vreturn();
    });
    ASSERT_TRUE(fx.run(mjbase::pass::CseChainEliminater{}).changed());
    EXPECT_TRUE(fx.run(mjbase::pass::CseChainEliminater{}).areAllPreserved());
    const Bytes out = fx.writeBack();
    EXPECT_FALSE(out.empty());
}

// ── 注册面 ──
TEST(CseChainEliminaterTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("CseChainEliminater"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("CseChainEliminater");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
