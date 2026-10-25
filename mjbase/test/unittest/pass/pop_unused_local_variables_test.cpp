// PopUnUsedLocalVariables：无读取槽位的 store 降级 pop/pop2 + 读点审计面
// （iinc/ret/两槽高半槽/不可达读点保守）+ 幂等 + 三件链端到端（死状态链
// store 降级）+ 注册面。常量均为合成值。
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/flattening_reverser.hpp"
#include "mjbase/pass/pop_unused_local_variables.hpp"
#include "mjbase/pass/redundant_load_val_eliminator.hpp"
#include "mjbase/pass/unreachable_insn_remover.hpp"
#include "mjbase/pass/useless_pop_cleaner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "fix_pattern_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;
using jbc::ir::LabelId;

// ── 正面：无读取 int 槽的 store → pop，有读取槽不动 ──
TEST(PopUnUsedLocalVariablesTest, downgradesUnreadIntStoreToPop)
{
    DomFixture fx("t/P", "m", "(I)I", 4, 4, [&](Body& b) {
        b.ldcInt(111111111).istore(1);   // 槽 1 无读取 → pop
        b.ldcInt(-222222222).istore(2);  // 槽 2 有读取 → 保持
        b.iload(2).ireturn();
    });
    const auto pa = fx.run(mjbase::pass::PopUnUsedLocalVariables{});
    EXPECT_TRUE(pa.changed());
    EXPECT_EQ(opsOf(fx), (std::vector<Op>{Op::LDC, Op::POP, Op::LDC, Op::ISTORE,
                                          Op::ILOAD, Op::IRETURN}));
    EXPECT_FALSE(fx.writeBack().empty());
}

// ── 正面：两槽值（long）无读取 → pop2 ──
TEST(PopUnUsedLocalVariablesTest, downgradesUnreadLongStoreToPop2)
{
    DomFixture fx("t/P", "m", "()V", 4, 6, [&](Body& b) {
        b.ldcLong(111111111);
        b.op(Op::LSTORE_2);   // 槽 2,3 无任何读取 → pop2
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::PopUnUsedLocalVariables{});
    EXPECT_TRUE(pa.changed());
    EXPECT_EQ(opsOf(fx), (std::vector<Op>{Op::LDC2_W, Op::POP2, Op::RETURN}));
}

// ── 正面：短形态 istore_n 同权 ──
TEST(PopUnUsedLocalVariablesTest, downgradesCompactStoreForm)
{
    DomFixture fx("t/P", "m", "()V", 4, 4, [&](Body& b) {
        b.iconst_1();
        b.op(Op::ISTORE_3);   // 槽 3 无读取 → pop
        b.iconst_2().pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::PopUnUsedLocalVariables{});
    EXPECT_TRUE(pa.changed());
    EXPECT_EQ(opsOf(fx), (std::vector<Op>{Op::ICONST_1, Op::POP, Op::ICONST_2, Op::POP,
                                          Op::RETURN}));
}

// ── 拒绝面：iinc 触碰的槽位按读点计 ──
TEST(PopUnUsedLocalVariablesTest, iincCountsAsRead)
{
    DomFixture fx("t/P", "m", "(I)I", 4, 4, [&](Body& b) {
        b.iload(0).istore(1);
        b.iinc(1, 3);   // 槽 1 被 iinc 读改写
        b.iload(1).ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::PopUnUsedLocalVariables{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 拒绝面：两槽装载保护基槽与高半槽（lstore_2 + lload_2） ──
TEST(PopUnUsedLocalVariablesTest, longPairReadBlocksStore)
{
    DomFixture fx("t/P", "m", "()V", 4, 6, [&](Body& b) {
        b.ldcLong(111111111);
        b.op(Op::LSTORE_2);   // lstore 2（写槽 2,3）
        b.op(Op::LLOAD_2);    // lload 2（读槽 2,3）→ 保护
        b.pop2();
        b.vreturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::PopUnUsedLocalVariables{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 无可降级 store：全保留 ──
TEST(PopUnUsedLocalVariablesTest, allSlotsReadLeavesAllPreserved)
{
    DomFixture fx("t/P", "m", "(I)I", 4, 4, [&](Body& b) {
        b.iload(0).istore(1);
        b.iload(1).ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::PopUnUsedLocalVariables{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 幂等：二轮产物是 pop 族非 store，逐字节一致 ──
TEST(PopUnUsedLocalVariablesTest, idempotentOnSecondRun)
{
    DomFixture fx("t/P", "m", "()V", 4, 4, [&](Body& b) {
        b.ldcInt(111111111).istore(1);
        b.vreturn();
    });
    const auto pa1 = fx.run(mjbase::pass::PopUnUsedLocalVariables{});
    EXPECT_TRUE(pa1.changed());
    const Bytes out1 = fx.writeBack();

    const auto pa2 = fx.run(mjbase::pass::PopUnUsedLocalVariables{});
    EXPECT_TRUE(pa2.areAllPreserved());
    EXPECT_EQ(fx.writeBack(), out1);
}

// ── 端到端（三件链）：分发解析 + 死分发块清理后，状态槽 store 全部降级 ──
TEST(PopUnUsedLocalVariablesTest, downgradesStateStoresAfterDeflattening)
{
    DomFixture fx("t/P", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId xL = b.label();
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {-222222222, 111111111}, {bL, aL});
        b.ldcInt(-222222222);
        b.bindHere(aL);
        b.istore(2).iload(2).istore(1).branch(Op::GOTO, dL);
        b.iconst_1();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    // 读点审计按指令全集做：分发块的 iload(1)（死但仍在）会保护状态槽，
    // 故链中带上结构级死代码清理先消掉死分发块。此后槽 1 无读取（两处
    // istore(1) → pop），槽 2 仍被 A 块存活的 iload(2) 读取（istore(2) 保持）。
    const auto pa = fx.run(mjbase::pass::FlatteningReverser{},
                           mjbase::pass::UnreachableInsnRemover{},
                           mjbase::pass::PopUnUsedLocalVariables{});
    EXPECT_TRUE(pa.changed());

    const std::vector<Op> ops = opsOf(fx);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ISTORE), 1);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::POP), 2);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::LOOKUPSWITCH), 0);
    EXPECT_FALSE(fx.writeBack().empty());
}

// ── 端到端（家族收敛）：装载;pop 残段消除后状态槽彻底失去读取，
//    store 降级与装载值消除交替两轮把状态链残段清空 ──
TEST(PopUnUsedLocalVariablesTest, stateChainResidueFullyConvergesWithLoadValElimination)
{
    DomFixture fx("t/P", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId xL = b.label();
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {-222222222, 111111111}, {bL, aL});
        b.ldcInt(-222222222);
        b.bindHere(aL);
        b.istore(2).iload(2).istore(1).branch(Op::GOTO, dL);
        b.iconst_1();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    // 收敛链：装载值消除吃掉 iload(2);pop 后，槽 2 失去唯一读取点 → 第二轮
    // store 降级把它变 pop → 常量;pop 相邻对再被常量装载面清理吃掉。终态
    // 无状态槽访问、无 pop、无常量残段。
    const auto pa = fx.run(mjbase::pass::FlatteningReverser{},
                           mjbase::pass::UnreachableInsnRemover{},
                           mjbase::pass::PopUnUsedLocalVariables{},
                           mjbase::pass::RedundantLoadValEliminator{},
                           mjbase::pass::UselessPopCleaner{},
                           mjbase::pass::PopUnUsedLocalVariables{},
                           mjbase::pass::RedundantLoadValEliminator{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::LOOKUPSWITCH), 0);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ISTORE), 0);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ILOAD), 0);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::POP), 0);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::LDC), 0);
    // 分发解析后默认块（xL）不可达，已随死分发块一并清理——终态仅存
    // B 块返回路径。
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ICONST_1), 1);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ICONST_0), 0);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::IRETURN), 1);
    EXPECT_FALSE(fx.writeBack().empty());
}

// ── 注册面 ──
TEST(PopUnUsedLocalVariablesTest, registeredAsPopUnUsedLocalVariables)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("PopUnUsedLocalVariables"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("PopUnUsedLocalVariables");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
