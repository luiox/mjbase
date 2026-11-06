/// @file        slot_provenance_test.cpp
/// @brief       SlotProvenanceView 测试：局部链/入参播种、分支合并多定义、
///              DUP 族补充使用边与同 def 占多槽、cat2 布局（合成语料，只读）。
///
/// 判据口径：DUP 族「复制槽不产定义、产使用边」、切面槽干净 =
/// provenance 唯一。
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/asm/sem/slot_provenance.hpp"

#include "sem_fixture.hpp"

namespace {

using jbc::core::Op;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::MethodSpec;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::DefSet;
using mjbase::asm_analyzer::DefUseEdge;
using mjbase::asm_analyzer::DefUseSlot;
using mjbase::asm_sem::SlotProvenanceView;

bool hasEdge(const SlotProvenanceView& v, DefId def, InsnId use, DefUseSlot slot, ca::u32 index)
{
    for (const DefUseEdge& e : v.edges()) {
        if (e.def == def && e.use == use && e.slot == slot && e.index == index) {
            return true;
        }
    }
    return false;
}

bool sameSet(const mjbase::asm_analyzer::DefSet& s,
             std::initializer_list<DefId> expected)
{
    if (s.size() != expected.size()) {
        return false;
    }
    for (const DefId& d : expected) {
        if (!s.contains(d)) {
            return false;
        }
    }
    return true;
}

}   // namespace

/// 局部链：store 产定义、load 搬运定义；入参按签名播种为 Parameter 定义。
TEST(MjAsmSemSlotProvenanceTest, LocalChainAndParamSeed)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name     = "m";
    m.desc     = "(I)I";
    m.emit     = [](CodeEmit& e) {
        e.varInsn(Op::ILOAD, 0);
        e.varInsn(Op::ISTORE, 1);
        e.varInsn(Op::ILOAD, 1);
        e.op(Op::IRETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto view = SlotProvenanceView::build(f.cp(), *u, sigOf(true, "(I)I"));
    ASSERT_TRUE(view.ok());
    EXPECT_FALSE(view.truncated());

    const std::vector<InsnId>& order = u->dom.order();
    const InsnId iload0  = order[0];
    const InsnId istore1 = order[1];
    const InsnId iload1  = order[2];
    const InsnId iret    = order[3];

    /// 入参：Parameter(0) 播种；store 前 local 1 无定义。
    EXPECT_TRUE(sameSet(view.localDefsAt(iload0, 0), {DefId::parameter(0)}));
    EXPECT_TRUE(view.localDefsAt(iload0, 1).empty());
    /// store 定义 = 本指令；load 恒等搬运。
    EXPECT_TRUE(sameSet(view.localDefsAt(iload1, 1), {DefId::instruction(istore1)}));
    EXPECT_TRUE(view.localSlotClean(iload1, 1));
    /// 返回指令的栈顶值 = istore1 经 load 带上来的定义。
    EXPECT_TRUE(sameSet(view.stackDefsAt(iret, 0), {DefId::instruction(istore1)}));
    EXPECT_TRUE(view.stackSlotClean(iret, 0));
    EXPECT_EQ(view.stackValueCount(iret), 1u);
}

/// 分支汇合：同一局部两个到达定义 → provenance 不唯一（切面不干净）。
TEST(MjAsmSemSlotProvenanceTest, MergeTwoDefsNotClean)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name     = "m";
    m.desc     = "(Z)V";
    m.emit     = [](CodeEmit& e) {
        const std::size_t join = e.newLabel();
        const std::size_t end  = e.newLabel();
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, join);
        e.op(Op::ICONST_1);
        e.varInsn(Op::ISTORE, 1);
        e.branch(Op::GOTO, end);
        e.bindLabel(join);
        e.op(Op::ICONST_2);
        e.varInsn(Op::ISTORE, 1);
        e.bindLabel(end);
        e.varInsn(Op::ILOAD, 1);
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto view = SlotProvenanceView::build(f.cp(), *u, sigOf(true, "(Z)V"));
    ASSERT_TRUE(view.ok());

    const std::vector<InsnId>& order = u->dom.order();
    /// 汇合点 = 末段 ILOAD（倒数第 3 条）；两条 store 在 order[3]/order[6]。
    const InsnId joinLoad = order[order.size() - 3];
    const DefSet defs     = view.localDefsAt(joinLoad, 1);
    EXPECT_EQ(defs.size(), 2u);
    EXPECT_TRUE(defs.contains(DefId::instruction(order[3])));   /// then 分支 store
    EXPECT_TRUE(defs.contains(DefId::instruction(order[6])));   /// else 分支 store
    EXPECT_FALSE(view.localSlotClean(joinLoad, 1));
    EXPECT_FALSE(view.stackSlotClean(order[order.size() - 2], 0));
}

/// DUP 族：复制槽不产定义、产使用边；同一 def 占双槽在切面可见。
TEST(MjAsmSemSlotProvenanceTest, DupUseEdgesAndSharedDef)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name     = "m";
    m.desc     = "(Ljava/lang/Object;)V";
    m.emit     = [](CodeEmit& e) {
        e.op(Op::ALOAD_0);
        e.op(Op::DUP);
        e.op(Op::POP);
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto view = SlotProvenanceView::build(f.cp(), *u, sigOf(true, "(Ljava/lang/Object;)V"));
    ASSERT_TRUE(view.ok());

    const std::vector<InsnId>& order = u->dom.order();
    const InsnId dup  = order[1];
    const InsnId pop1 = order[2];
    const InsnId pop2 = order[3];
    const DefId  param = DefId::parameter(0);

    /// dup 的补充使用边（def_use 基础语义里 dup 不产边，本视图补齐）。
    EXPECT_TRUE(hasEdge(view, param, dup, DefUseSlot::Stack, 0));
    /// 两个 pop 的消耗边（基础语义）。
    EXPECT_TRUE(hasEdge(view, param, pop1, DefUseSlot::Stack, 0));
    EXPECT_TRUE(hasEdge(view, param, pop2, DefUseSlot::Stack, 0));

    /// dup 执行前的到达帧：同一 def 占双槽（切面共享值形态可见）。
    EXPECT_TRUE(sameSet(view.stackDefsAt(dup, 0), {param}));
    EXPECT_TRUE(sameSet(view.stackDefsAt(pop1, 0), {param}));
    EXPECT_TRUE(sameSet(view.stackDefsAt(pop1, 1), {param}));
    EXPECT_TRUE(view.stackSlotClean(pop1, 0));
    EXPECT_TRUE(view.stackSlotClean(pop1, 1));

    ASSERT_EQ(view.usesAt(dup).size(), 1u);
    EXPECT_GE(view.usesOf(param).size(), 3u);
    const InsnId* resolved = view.instructionOf(DefId::instruction(dup));
    ASSERT_NE(resolved, nullptr);
    EXPECT_TRUE(*resolved == dup);
}

/// DUP2 双形态：cat2 单值复制与双 cat1 复制都按值深度产补充边。
TEST(MjAsmSemSlotProvenanceTest, Dup2Cat2AndCat1)
{
    /// cat2：lload_0; dup2; pop2; pop2; return。
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name     = "m";
    m.desc     = "(J)V";
    m.maxStack = 4;
    m.emit     = [](CodeEmit& e) {
        e.varInsn(Op::LLOAD, 0);
        e.op(Op::DUP2);
        e.op(Op::POP2);
        e.op(Op::POP2);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto view = SlotProvenanceView::build(f.cp(), *u, sigOf(true, "(J)V"));
    ASSERT_TRUE(view.ok());

    const std::vector<InsnId>& order = u->dom.order();
    const InsnId dup2 = order[1];
    const DefId  param = DefId::parameter(0);

    /// cat2 单值：只有一个值行（垫底行不计），深度 0。
    EXPECT_TRUE(hasEdge(view, param, dup2, DefUseSlot::Stack, 0));
    EXPECT_EQ(view.stackValueCount(dup2), 1u);
    EXPECT_TRUE(sameSet(view.stackDefsAt(dup2, 0), {param}));
    /// pop2 × 2 的消耗边。
    EXPECT_TRUE(hasEdge(view, param, order[2], DefUseSlot::Stack, 0));
    EXPECT_TRUE(hasEdge(view, param, order[3], DefUseSlot::Stack, 0));

    /// 双 cat1：aload_0; iconst_0?——改用独立语料：iload_0; iload_0; dup2。
    ClassBytesBuilder b2("T");
    MethodSpec m2;
    m2.name     = "n";
    m2.desc     = "(II)V";
    m2.maxStack = 4;
    m2.emit     = [](CodeEmit& e) {
        e.varInsn(Op::ILOAD, 0);
        e.varInsn(Op::ILOAD, 1);
        e.op(Op::DUP2);
        e.op(Op::IADD);
        e.op(Op::POP2);
        e.op(Op::RETURN);
    };
    SemFixture f2(b2.assemble({m2}));
    CodeUnit*  u2 = f2.code("n");
    ASSERT_NE(u2, nullptr);

    auto view2 = SlotProvenanceView::build(f2.cp(), *u2, sigOf(true, "(II)V"));
    ASSERT_TRUE(view2.ok());
    const std::vector<InsnId>& order2 = u2->dom.order();
    const InsnId dup2b = order2[2];
    EXPECT_TRUE(hasEdge(view2, DefId::parameter(0), dup2b, DefUseSlot::Stack, 1));
    EXPECT_TRUE(hasEdge(view2, DefId::parameter(1), dup2b, DefUseSlot::Stack, 0));
    EXPECT_EQ(view2.stackValueCount(dup2b), 2u);
}

/// SWAP 只重排不复制：不产补充边；定义经重排后在切面仍可查。
TEST(MjAsmSemSlotProvenanceTest, SwapReordersWithoutDupEdge)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name     = "m";
    m.desc     = "(II)V";
    m.emit     = [](CodeEmit& e) {
        e.varInsn(Op::ILOAD, 0);
        e.varInsn(Op::ILOAD, 1);
        e.op(Op::SWAP);
        e.op(Op::POP);
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto view = SlotProvenanceView::build(f.cp(), *u, sigOf(true, "(II)V"));
    ASSERT_TRUE(view.ok());

    const std::vector<InsnId>& order = u->dom.order();
    const InsnId swap = order[2];
    const DefId  p0   = DefId::parameter(0);
    const DefId  p1   = DefId::parameter(1);

    /// SWAP 不入 DUP 族：无补充边（usesAt(swap) 仅来自后续 pop 的基础边——
    /// swap 自身不产边）。
    EXPECT_TRUE(view.usesAt(swap).empty());
    /// 重排后定义位置互换：param0 到栈顶、param1 到次顶。
    EXPECT_TRUE(sameSet(view.stackDefsAt(order[3], 0), {p0}));
    EXPECT_TRUE(sameSet(view.stackDefsAt(order[3], 1), {p1}));
}

/// 数据流失败：ok()=false，边与查询全部退化（不产部分事实）。
TEST(MjAsmSemSlotProvenanceTest, FailureDegradesToEmpty)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "()V";
    m.emit = [](CodeEmit& e) {
        e.op(Op::POP);   /// 空栈弹出 → 引擎失败
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto view = SlotProvenanceView::build(f.cp(), *u, sigOf(true, "()V"));
    EXPECT_FALSE(view.ok());
    EXPECT_TRUE(view.edges().empty());
    EXPECT_EQ(view.stackValueCount(InsnId{}), 0u);
    EXPECT_TRUE(view.stackDefsAt(InsnId{}, 0).empty());
    EXPECT_FALSE(view.stackSlotClean(InsnId{}, 0));
}
