// ConstantSwitchFolder：常量 selector 的 switch 相邻窗折叠（lookup 精确
// 命中 / table 区间命中 / 未命中走 default）+ 边界守卫（switch 前锚标签
// 拒收、头标签放行）+ 非常量 selector 不动 + 幂等 + PA 纪律。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/constant_switch_folder.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "fix_pattern_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;
using jbc::ir::LabelId;

namespace {

/// 唯一 GOTO 的目标锚定位置（order 下标；找不到报失败返回 -1）。
ca::i64 gotoTargetPos(DomFixture& fx)
{
    const jbc::ir::CodeDom&        dom   = fx.unit().dom;
    const std::vector<jbc::ir::InsnId>& order = dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) {
        const jbc::ir::Insn* in = dom.peek(order[i]);
        if (in == nullptr || in->op != Op::GOTO) continue;
        auto resolved = dom.resolveLabel(in->target);
        EXPECT_TRUE(resolved.is_ok());
        const jbc::ir::InsnId anchor = std::move(resolved).unwrap().insn;
        for (ca::usize j = 0; j < order.size(); ++j) {
            if (order[j] == anchor) return static_cast<ca::i64>(j);
        }
    }
    ADD_FAILURE() << "no GOTO found";
    return -1;
}

}   // namespace

// ── lookup 精确命中：selector 落在 keys → GOTO 到对应 case ──
TEST(ConstantSwitchFolderTest, lookupHitFoldsToCase)
{
    DomFixture fx("t/F", "m", "()I", 4, 2, [&](Body& b) {
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId dL = b.label();
        b.iconst_1().lookupSwitch(dL, {1, 2}, {aL, bL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_5();
        b.bindHere(dL);
        b.ireturn();
    });
    const auto pa = fx.run(mjbase::pass::ConstantSwitchFolder{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::LOOKUPSWITCH), 0);
    EXPECT_EQ(ops[0], Op::GOTO);
    // 命中 key 1 → A 块（GOTO 后第一条）。
    EXPECT_EQ(gotoTargetPos(fx), 1);
}

// ── lookup 未命中：selector 不在 keys → GOTO 到 default ──
TEST(ConstantSwitchFolderTest, lookupMissFoldsToDefault)
{
    DomFixture fx("t/F", "m", "()I", 4, 2, [&](Body& b) {
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId dL = b.label();
        b.iconst_3().lookupSwitch(dL, {1, 2}, {aL, bL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_5();
        b.bindHere(dL);
        b.ireturn();
    });
    const auto pa = fx.run(mjbase::pass::ConstantSwitchFolder{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::LOOKUPSWITCH), 0);
    // default 块在第 5 位（GOTO + A 两指令 + B 两指令之后）。
    EXPECT_EQ(gotoTargetPos(fx), 5);
}

// ── table 区间命中：selector 在 [low, high] → GOTO 到 targets[i] ──
TEST(ConstantSwitchFolderTest, tableHitFoldsToCase)
{
    DomFixture fx("t/F", "m", "()I", 4, 2, [&](Body& b) {
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId cL = b.label();
        LabelId dL = b.label();
        b.iconst_2().tableSwitch(dL, 1, 3, {aL, bL, cL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_3();
        b.bindHere(cL);
        b.ireturn();
        b.iconst_5();
        b.bindHere(dL);
        b.ireturn();
    });
    const auto pa = fx.run(mjbase::pass::ConstantSwitchFolder{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::TABLESWITCH), 0);
    // key 2 → targets[1] = B 块（第 3 位）。
    EXPECT_EQ(gotoTargetPos(fx), 3);
}

// ── table 出界：selector 超出 [low, high] → GOTO 到 default ──
TEST(ConstantSwitchFolderTest, tableMissFoldsToDefault)
{
    DomFixture fx("t/F", "m", "()I", 4, 2, [&](Body& b) {
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId cL = b.label();
        LabelId dL = b.label();
        b.bipush(9).tableSwitch(dL, 1, 3, {aL, bL, cL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_3();
        b.bindHere(cL);
        b.ireturn();
        b.iconst_5();
        b.bindHere(dL);
        b.ireturn();
    });
    const auto pa = fx.run(mjbase::pass::ConstantSwitchFolder{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::TABLESWITCH), 0);
    // default 块在第 7 位（GOTO + A/B/C 各两指令之后）。
    EXPECT_EQ(gotoTargetPos(fx), 7);
}

// ── 边界守卫：switch 前置边界锚标签（外部路径直接落到 switch 上，
//    其栈顶 selector 正等待这次弹消费）→ 整窗拒收 ──
TEST(ConstantSwitchFolderTest, interiorLabelBlocksFold)
{
    DomFixture fx("t/F", "m", "()I", 4, 2, [&](Body& b) {
        LabelId aL  = b.label();
        LabelId bL  = b.label();
        LabelId dL  = b.label();
        LabelId swL = b.label();
        b.iconst_1().lookupSwitch(dL, {1, 2}, {aL, bL});
        b.bindHere(swL);   // 锚在 switch 前置边界
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_5();
        b.bindHere(dL);
        b.ireturn();
        b.branch(Op::GOTO, swL);
    });
    const auto pa = fx.run(mjbase::pass::ConstantSwitchFolder{});
    EXPECT_TRUE(pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(fx);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::LOOKUPSWITCH), 1);
}

// ── 窗口头（常量装载）前置边界锚标签：放行折叠——头部改写后仍在
//    （GOTO），标签原地有效 ──
TEST(ConstantSwitchFolderTest, headLabelFoldsThrough)
{
    DomFixture fx("t/F", "m", "()I", 4, 2, [&](Body& b) {
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId dL = b.label();
        LabelId hL = b.label();
        b.iconst_1();
        b.bindHere(hL);   // 锚在常量装载前置边界
        b.lookupSwitch(dL, {1, 2}, {aL, bL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_5();
        b.bindHere(dL);
        b.ireturn();
        b.branch(Op::GOTO, hL);
    });
    const auto pa = fx.run(mjbase::pass::ConstantSwitchFolder{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::LOOKUPSWITCH), 0);
    // 命中 key 1 → A 块；指向头部的额外 GOTO 不影响折叠产物。
    EXPECT_EQ(gotoTargetPos(fx), 1);
}

// ── 非常量 selector：装载结果不动 ──
TEST(ConstantSwitchFolderTest, nonConstSelectorUntouched)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 2, [&](Body& b) {
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId dL = b.label();
        b.iload(1).lookupSwitch(dL, {1, 2}, {aL, bL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_5();
        b.bindHere(dL);
        b.ireturn();
    });
    const auto pa = fx.run(mjbase::pass::ConstantSwitchFolder{});
    EXPECT_TRUE(pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(fx);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::LOOKUPSWITCH), 1);
}

// ── 幂等：产物再跑无变化（写回字节等价）──
TEST(ConstantSwitchFolderTest, idempotentOnFoldedOutput)
{
    DomFixture fx("t/F", "m", "()I", 4, 2, [&](Body& b) {
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId dL = b.label();
        b.iconst_1().lookupSwitch(dL, {1, 2}, {aL, bL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_5();
        b.bindHere(dL);
        b.ireturn();
    });
    EXPECT_TRUE(fx.run(mjbase::pass::ConstantSwitchFolder{}).changed());
    const Bytes out1 = fx.writeBack();
    const auto  pa2  = fx.run(mjbase::pass::ConstantSwitchFolder{});
    EXPECT_TRUE(pa2.areAllPreserved());
    EXPECT_EQ(fx.writeBack(), out1);
}

// ── 注册面 ──
TEST(ConstantSwitchFolderTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("ConstantSwitchFolder"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("ConstantSwitchFolder");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
