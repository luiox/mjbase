/// @file        cfg_test.cpp
/// @brief       控制流图测试：切块、边分类、异常覆盖区间、可达集与支配树。
///
/// 直接在 jbc::ir::CodeUnit 上构造指令序列（append + createLabel/bindLabel），
/// 不需要 classfile。构建纪律：分支/switch 的目标标签在 append 时必须已
/// 锚定到活指令，因此标签先临时锚定、终态再 rebind。
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/cfg/cfg_analysis.hpp"
#include "mjbase/asm/cfg/control_flow_graph.hpp"

namespace {

using jbc::core::Op;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_cfg::BlockId;
using mjbase::asm_cfg::CfgTraversal;
using mjbase::asm_cfg::ControlFlowGraph;
using mjbase::asm_cfg::DominatorTree;
using mjbase::asm_cfg::Edge;
using mjbase::asm_cfg::EdgeKind;

/// 测试脚手架：CodeUnit 上的追加/插桩便利（append 失败直接 fail）。
struct DomFixture
{
    CodeUnit unit;

    InsnId add(Insn in)
    {
        const auto r = unit.dom.append(std::move(in));
        EXPECT_TRUE(r.is_ok());
        return r.is_ok() ? r.unwrap() : jbc::ir::kNoInsn;
    }
    InsnId add(Op op) { return add(jbc::ir::CodeEditor::simple(op)); }
    InsnId add(Op op, LabelId target) { return add(jbc::ir::CodeEditor::branch(op, target)); }

    InsnId insertBefore(InsnId at, Insn in)
    {
        const auto r = unit.dom.insertBefore(at, std::move(in));
        EXPECT_TRUE(r.is_ok());
        return r.is_ok() ? r.unwrap() : jbc::ir::kNoInsn;
    }

    /// 创建并锚定一个标签（append 校验要求目标标签锚定活指令）。
    LabelId label(InsnId anchor)
    {
        const auto r = unit.dom.createLabel(anchor);
        EXPECT_TRUE(r.is_ok());
        return r.is_ok() ? r.unwrap() : jbc::ir::kNoLabel;
    }
    /// 创建绑定到方法末尾（End 边界）的标签；异常表 end 的常见形态。
    LabelId endLabel()
    {
        const auto r = unit.dom.createLabel();
        EXPECT_TRUE(r.is_ok());
        return r.is_ok() ? r.unwrap() : jbc::ir::kNoLabel;
    }
    void bind(LabelId l, InsnId anchor) { EXPECT_TRUE(unit.dom.bindLabel(l, anchor).is_ok()); }

    /// 构建并断言成功；图存为成员，返回其引用（Result 按值取值不悬垂）。
    const ControlFlowGraph& build()
    {
        auto built = ControlFlowGraph::build(unit);
        EXPECT_TRUE(built.is_ok());
        if (built.is_ok()) {
            graph = std::move(built).unwrap();
        }
        return graph;
    }

    ControlFlowGraph graph;
};

bool hasEdge(const ControlFlowGraph& g, BlockId s, BlockId t, EdgeKind kind)
{
    for (const Edge& e : g.succEdgesOf(s)) {
        if (e.source == s && e.target == t && e.kind == kind) {
            return true;
        }
    }
    return false;
}

std::vector<ca::u32> indexes(const std::vector<BlockId>& v)
{
    std::vector<ca::u32> out;
    out.reserve(v.size());
    for (const BlockId b : v) {
        out.push_back(b.index);
    }
    return out;
}

}   // namespace

// ========== 基本切块与终止块 ==========

TEST(MjAsmCfgTest, SingleBlockMethod)
{
    DomFixture f;
    const auto nop = f.add(Op::NOP);
    const auto ret = f.add(Op::RETURN);

    const ControlFlowGraph& g = f.build();
    EXPECT_EQ(g.blockCount(), 1u);
    ASSERT_NE(g.block(BlockId{0}), nullptr);
    EXPECT_EQ(g.block(BlockId{0})->insns.size(), 2u);
    EXPECT_EQ(g.block(BlockId{0})->entry, nop);
    EXPECT_TRUE(g.entry().valid());
    EXPECT_TRUE(g.isTerminated(BlockId{0}));
    EXPECT_TRUE(g.succEdgesOf(BlockId{0}).empty());
    EXPECT_EQ(g.successors(BlockId{0}).size(), 0u);
    EXPECT_EQ(g.blockOfInsn(nop), BlockId{0});
    EXPECT_EQ(g.blockOfInsn(ret), BlockId{0});
}

TEST(MjAsmCfgTest, EmptyMethod)
{
    DomFixture f;
    const ControlFlowGraph& g = f.build();
    EXPECT_EQ(g.blockCount(), 0u);
    EXPECT_FALSE(g.entry().valid());
    EXPECT_EQ(g.block(mjbase::asm_cfg::kNoBlock), nullptr);
    EXPECT_TRUE(g.successors(mjbase::asm_cfg::kNoBlock).empty());
    EXPECT_FALSE(g.isTerminated(mjbase::asm_cfg::kNoBlock));
    EXPECT_EQ(g.blockOfInsn(jbc::ir::kNoInsn), mjbase::asm_cfg::kNoBlock);

    CfgTraversal traversal(g);
    EXPECT_EQ(traversal.reversePostorder().size(), 0u);
    DominatorTree dom;
    dom.compute(g);
    EXPECT_FALSE(dom.isReachable(mjbase::asm_cfg::kNoBlock));
}

TEST(MjAsmCfgTest, TerminatedByReturnAndAthrow)
{
    {
        DomFixture f;
        f.add(Op::ACONST_NULL);
        f.add(Op::ATHROW);
        const ControlFlowGraph& g = f.build();
        ASSERT_EQ(g.blockCount(), 1u);
        EXPECT_TRUE(g.isTerminated(BlockId{0}));
        EXPECT_TRUE(g.successors(BlockId{0}).empty());
    }
    {
        // 条件分支中路的 nop 块以 NOP 结尾：非终止块，顺序落入后继。
        DomFixture f;
        const auto iconst = f.add(Op::ICONST_0);
        const auto lbl = f.label(iconst);
        f.add(Op::IFEQ, lbl);
        const auto nop = f.add(Op::NOP);
        const auto ret = f.add(Op::RETURN);
        f.bind(lbl, ret);
        (void)nop;

        const ControlFlowGraph& g = f.build();
        ASSERT_EQ(g.blockCount(), 3u);
        EXPECT_FALSE(g.isTerminated(BlockId{1}));
        EXPECT_TRUE(hasEdge(g, BlockId{1}, BlockId{2}, EdgeKind::FallThrough));
    }
}

// ========== 普通边 ==========

TEST(MjAsmCfgTest, ConditionalSplitAndEdges)
{
    // [iconst, ifeq→L] [nop] [L: return]
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_0);
    const auto lbl = f.label(iconst);   // 先临时锚定，append 后 rebind
    f.add(Op::IFEQ, lbl);
    f.add(Op::NOP);
    const auto ret = f.add(Op::RETURN);
    f.bind(lbl, ret);

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 3u);

    // b0：taken 边（Condition）+ not-taken 边（FallThrough）。
    const auto& out = g.succEdgesOf(BlockId{0});
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].kind, EdgeKind::Condition);
    EXPECT_EQ(out[0].target, BlockId{2});
    EXPECT_EQ(out[1].kind, EdgeKind::FallThrough);
    EXPECT_EQ(out[1].target, BlockId{1});
    EXPECT_EQ(indexes(g.successors(BlockId{0})), (std::vector<ca::u32>{1, 2}));

    // 汇合块 b2 的前驱来自 b0（taken）与 b1（fallthrough）。
    EXPECT_EQ(indexes(g.predecessors(BlockId{2})), (std::vector<ca::u32>{0, 1}));
    EXPECT_EQ(g.edgeCount(EdgeKind::Condition), 1u);
    EXPECT_EQ(g.edgeCount(EdgeKind::FallThrough), 2u);   // b0→b1（not-taken）+ b1→b2（直落）

    // 标签 → 块定位。
    EXPECT_EQ(g.blockOfLabel(f.unit.dom, lbl), BlockId{2});
    EXPECT_EQ(g.blockOfInsn(iconst), BlockId{0});
}

TEST(MjAsmCfgTest, GotoBranchEdge)
{
    // [goto→L] [L: return]
    DomFixture f;
    const auto ret = f.add(Op::RETURN);
    const auto lbl = f.label(ret);
    const auto jmp = f.insertBefore(ret, jbc::ir::CodeEditor::branch(Op::GOTO, lbl));

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 2u);
    EXPECT_TRUE(hasEdge(g, BlockId{0}, BlockId{1}, EdgeKind::Branch));
    EXPECT_EQ(indexes(g.successors(BlockId{0})), (std::vector<ca::u32>{1}));
    EXPECT_EQ(g.edgeCount(EdgeKind::Branch), 1u);
    EXPECT_EQ(g.edgeCount(EdgeKind::FallThrough), 0u);
    EXPECT_EQ(g.blockOfInsn(jmp), BlockId{0});
}

TEST(MjAsmCfgTest, DeadCodeAfterGotoIsUnreachable)
{
    // [goto→L] [iconst, ireturn（死代码）] [L: return]
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_1);
    f.add(Op::IRETURN);
    const auto ret = f.add(Op::RETURN);
    const auto lbl = f.label(ret);
    f.insertBefore(iconst, jbc::ir::CodeEditor::branch(Op::GOTO, lbl));

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 3u);

    CfgTraversal traversal(g);
    ASSERT_EQ(traversal.reversePostorder().size(), 2u);
    EXPECT_TRUE(traversal.isReachable(BlockId{0}));
    EXPECT_FALSE(traversal.isReachable(BlockId{1}));
    EXPECT_TRUE(traversal.isReachable(BlockId{2}));

    // 死块仍保留在块表里（可寻址），但不参与支配。
    DominatorTree dom;
    dom.compute(g);
    EXPECT_FALSE(dom.isReachable(BlockId{1}));
    EXPECT_EQ(dom.idom(BlockId{2}), BlockId{0});
    EXPECT_TRUE(dom.dominates(BlockId{0}, BlockId{2}));
}

TEST(MjAsmCfgTest, JsrHasBranchAndFallThroughEdges)
{
    // [jsr→Lr] [nop（返回点）] [Lr: return]：taken 走 Branch，返回点走 FallThrough。
    DomFixture f;
    const auto jsrAnchor = f.add(Op::ICONST_0);
    const auto lr = f.label(jsrAnchor);
    f.add(Op::JSR, lr);
    f.add(Op::NOP);
    const auto ret = f.add(Op::RETURN);
    f.bind(lr, ret);

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 3u);
    EXPECT_TRUE(hasEdge(g, BlockId{0}, BlockId{2}, EdgeKind::Branch));
    EXPECT_TRUE(hasEdge(g, BlockId{0}, BlockId{1}, EdgeKind::FallThrough));
    EXPECT_EQ(g.edgeCount(EdgeKind::Branch), 1u);
    EXPECT_EQ(g.edgeCount(EdgeKind::FallThrough), 2u);
}

TEST(MjAsmCfgTest, ConditionalSelfLoop)
{
    // [iconst, ifeq→块首]：单块自环；末块条件分支无顺序后继也不崩溃。
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_0);
    const auto lbl = f.label(iconst);
    f.add(Op::IFEQ, lbl);

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 1u);
    EXPECT_EQ(indexes(g.successors(BlockId{0})), (std::vector<ca::u32>{0}));

    DominatorTree dom;
    dom.compute(g);
    EXPECT_EQ(dom.idom(BlockId{0}), BlockId{0});
    EXPECT_TRUE(dom.dominates(BlockId{0}, BlockId{0}));
}

// ========== switch 边 ==========

TEST(MjAsmCfgTest, TableSwitchEdges)
{
    // [iconst, tableswitch] [A: ireturn] [B: ireturn] [D: return]
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_0);
    const auto la = f.label(iconst);
    const auto lb = f.label(iconst);
    const auto ld = f.label(iconst);
    Insn sw;
    sw.op = Op::TABLESWITCH;
    sw.a = 0;
    sw.b = 1;
    sw.target = ld;
    sw.targets = {la, lb};
    f.add(sw);
    const auto ra = f.add(Op::IRETURN);
    const auto rb = f.add(Op::IRETURN);
    const auto rd = f.add(Op::RETURN);
    f.bind(la, ra);
    f.bind(lb, rb);
    f.bind(ld, rd);

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 4u);
    EXPECT_EQ(g.edgeCount(EdgeKind::Switch), 3u);
    EXPECT_EQ(g.edgeCount(EdgeKind::FallThrough), 0u);   // switch 无顺序落入
    EXPECT_EQ(indexes(g.successors(BlockId{0})), (std::vector<ca::u32>{1, 2, 3}));
    EXPECT_EQ(indexes(g.predecessors(BlockId{1})), (std::vector<ca::u32>{0}));
    EXPECT_EQ(indexes(g.predecessors(BlockId{3})), (std::vector<ca::u32>{0}));
}

TEST(MjAsmCfgTest, LookupSwitchEdges)
{
    // [iconst, lookupswitch(keys 10/20)] [A: ireturn] [B: ireturn] [D: return]
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_0);
    const auto la = f.label(iconst);
    const auto lb = f.label(iconst);
    const auto ld = f.label(iconst);
    Insn sw;
    sw.op = Op::LOOKUPSWITCH;
    sw.keys = {10, 20};
    sw.target = ld;
    sw.targets = {la, lb};
    f.add(sw);
    const auto ra = f.add(Op::IRETURN);
    const auto rb = f.add(Op::IRETURN);
    const auto rd = f.add(Op::RETURN);
    f.bind(la, ra);
    f.bind(lb, rb);
    f.bind(ld, rd);

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 4u);
    EXPECT_EQ(g.edgeCount(EdgeKind::Switch), 3u);
    EXPECT_EQ(indexes(g.successors(BlockId{0})), (std::vector<ca::u32>{1, 2, 3}));
}

TEST(MjAsmCfgTest, SwitchDuplicateTargetsDeduplicated)
{
    // default 与两个 case 指向同一块：折叠为一条 Switch 边。
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_0);
    const auto la = f.label(iconst);
    Insn sw;
    sw.op = Op::TABLESWITCH;
    sw.a = 0;
    sw.b = 1;
    sw.target = la;
    sw.targets = {la, la};
    f.add(sw);
    const auto ra = f.add(Op::IRETURN);
    f.bind(la, ra);

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 2u);
    EXPECT_EQ(g.edgeCount(EdgeKind::Switch), 1u);
    EXPECT_EQ(indexes(g.successors(BlockId{0})), (std::vector<ca::u32>{1}));
}

// ========== 异常边 ==========

TEST(MjAsmCfgTest, TryCatchExceptionEdges)
{
    // [Ls: iconst, pop] [goto→Lh] [Lh: return]
    // exc: {start=Ls, end=Le(→goto，开区间), handler=Lh}
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_1);
    const auto ls = f.label(iconst);
    f.add(Op::POP);
    const auto lh = f.label(iconst);   // 临时锚定，append 后 rebind
    const auto jmp = f.add(Op::GOTO, lh);
    const auto ret = f.add(Op::RETURN);
    const auto le = f.label(jmp);
    f.bind(lh, ret);
    f.unit.exc.push_back({ls, le, lh, 0});

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 3u);
    EXPECT_EQ(g.edgeCount(EdgeKind::Exception), 1u);
    EXPECT_TRUE(hasEdge(g, BlockId{0}, BlockId{2}, EdgeKind::Exception));

    // 覆盖区间是 [Ls, Le)：goto 块（b1）不在覆盖内。
    EXPECT_FALSE(hasEdge(g, BlockId{1}, BlockId{2}, EdgeKind::Exception));

    // 异常 handler 算后继：b0 的后继含 fallthrough(b1) 与 handler(b2)。
    EXPECT_EQ(indexes(g.successors(BlockId{0})), (std::vector<ca::u32>{1, 2}));
    // handler 入边：异常边（来自 b0）+ goto 边（来自 b1）。
    EXPECT_EQ(indexes(g.predecessors(BlockId{2})), (std::vector<ca::u32>{0, 1}));

    // excIndex 回查 CodeUnit::exc 表项。
    for (const Edge& e : g.succEdgesOf(BlockId{0})) {
        if (e.kind == EdgeKind::Exception) {
            EXPECT_EQ(e.excIndex, 0u);
            EXPECT_EQ(f.unit.exc[e.excIndex].catchCp, 0u);
        }
    }
}

TEST(MjAsmCfgTest, TryRegionSpansMultipleBlocks)
{
    // [Ls: iconst, ifeq→Lmid] [Lmid: nop] [Le: return] [Lh: return]
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_0);
    const auto ls = f.label(iconst);
    const auto lmid = f.label(iconst);
    f.add(Op::IFEQ, lmid);
    const auto nop = f.add(Op::NOP);
    const auto leRet = f.add(Op::RETURN);
    const auto hRet = f.add(Op::RETURN);
    const auto le = f.label(leRet);
    const auto lh = f.label(hRet);
    f.bind(lmid, nop);
    f.unit.exc.push_back({ls, le, lh, 5});   // catchCp=5 仅透传，不解释

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 4u);
    // 覆盖 [Ls, Le)：b0、b1 在 try 内，Le 所在块 b2 不在。
    EXPECT_EQ(g.edgeCount(EdgeKind::Exception), 2u);
    EXPECT_TRUE(hasEdge(g, BlockId{0}, BlockId{3}, EdgeKind::Exception));
    EXPECT_TRUE(hasEdge(g, BlockId{1}, BlockId{3}, EdgeKind::Exception));
    EXPECT_FALSE(hasEdge(g, BlockId{2}, BlockId{3}, EdgeKind::Exception));
    EXPECT_EQ(indexes(g.predecessors(BlockId{3})), (std::vector<ca::u32>{0, 1}));
}

TEST(MjAsmCfgTest, TryEndAtMethodEnd)
{
    // [Ls: iconst, pop, goto→Lh] [Lh: return]
    // exc: {start=Ls, end=End（方法末尾）, handler=Lh} → 覆盖到最后一块。
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_1);
    const auto ls = f.label(iconst);
    const auto lh = f.label(iconst);
    f.add(Op::POP);
    f.add(Op::GOTO, lh);
    const auto ret = f.add(Op::RETURN);
    const auto le = f.endLabel();
    f.bind(lh, ret);
    f.unit.exc.push_back({ls, le, lh, 0});

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 2u);
    EXPECT_EQ(g.edgeCount(EdgeKind::Exception), 2u);
    EXPECT_TRUE(hasEdge(g, BlockId{0}, BlockId{1}, EdgeKind::Exception));
    // handler 落在区间内：自覆盖边 b1→b1（区间语义的忠实表达）。
    EXPECT_TRUE(hasEdge(g, BlockId{1}, BlockId{1}, EdgeKind::Exception));
    EXPECT_EQ(indexes(g.successors(BlockId{1})), (std::vector<ca::u32>{1}));
}

// ========== 遍历与支配 ==========

TEST(MjAsmCfgTest, TraversalOrders)
{
    // [iconst, ifeq→L] [nop] [L: return]
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_0);
    const auto lbl = f.label(iconst);
    f.add(Op::IFEQ, lbl);
    f.add(Op::NOP);
    const auto ret = f.add(Op::RETURN);
    f.bind(lbl, ret);

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 3u);

    CfgTraversal traversal(g);
    ASSERT_EQ(traversal.reversePostorder().size(), 3u);
    EXPECT_EQ(traversal.preorder()[0], BlockId{0});
    EXPECT_EQ(traversal.postorder().back(), BlockId{0});
    EXPECT_EQ(traversal.reversePostorder()[0], BlockId{0});
    EXPECT_TRUE(traversal.isReachable(BlockId{2}));
}

TEST(MjAsmCfgTest, DominatorOnBranchShape)
{
    // [iconst, ifeq→L] [nop] [L: return]：菱形的退化（单臂）。
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_0);
    const auto lbl = f.label(iconst);
    f.add(Op::IFEQ, lbl);
    f.add(Op::NOP);
    const auto ret = f.add(Op::RETURN);
    f.bind(lbl, ret);

    const ControlFlowGraph& g = f.build();
    DominatorTree dom;
    dom.compute(g);
    EXPECT_EQ(dom.idom(BlockId{0}), BlockId{0});
    EXPECT_EQ(dom.idom(BlockId{1}), BlockId{0});
    EXPECT_EQ(dom.idom(BlockId{2}), BlockId{0});
    EXPECT_TRUE(dom.dominates(BlockId{0}, BlockId{2}));
    EXPECT_FALSE(dom.dominates(BlockId{1}, BlockId{2}));
    // b1 的支配边界是汇合点 b2。
    ASSERT_EQ(dom.frontier(BlockId{1}).size(), 1u);
    EXPECT_EQ(dom.frontier(BlockId{1})[0], BlockId{2});
}

TEST(MjAsmCfgTest, DominatorDiamondFrontier)
{
    // [iconst, ifeq→Lr] [goto→Lj] [Lr: nop] [Lj: return]
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_0);
    const auto lr = f.label(iconst);
    const auto lj = f.label(iconst);
    f.add(Op::IFEQ, lr);
    f.add(Op::GOTO, lj);
    const auto nop = f.add(Op::NOP);
    const auto ret = f.add(Op::RETURN);
    f.bind(lr, nop);
    f.bind(lj, ret);

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 4u);
    DominatorTree dom;
    dom.compute(g);
    EXPECT_EQ(dom.idom(BlockId{1}), BlockId{0});
    EXPECT_EQ(dom.idom(BlockId{2}), BlockId{0});
    EXPECT_EQ(dom.idom(BlockId{3}), BlockId{0});
    EXPECT_EQ(dom.frontier(BlockId{0}).size(), 0u);
    EXPECT_EQ(indexes(dom.frontier(BlockId{1})), (std::vector<ca::u32>{3}));
    EXPECT_EQ(indexes(dom.frontier(BlockId{2})), (std::vector<ca::u32>{3}));
}

TEST(MjAsmCfgTest, LoopHeaderDominatesBody)
{
    // [goto→Lh] [return（不可达）] [Lh: iconst, ifeq→Le] [goto→Lh] [Le: return]
    DomFixture f;
    const auto deadRet = f.add(Op::RETURN);
    const auto iconst = f.add(Op::ICONST_0);
    const auto lh = f.label(iconst);
    const auto le = f.label(iconst);
    f.add(Op::IFEQ, le);
    f.add(Op::GOTO, lh);
    const auto exit = f.add(Op::RETURN);
    f.bind(le, exit);
    f.insertBefore(deadRet, jbc::ir::CodeEditor::branch(Op::GOTO, lh));

    const ControlFlowGraph& g = f.build();
    ASSERT_EQ(g.blockCount(), 5u);

    CfgTraversal traversal(g);
    ASSERT_EQ(traversal.reversePostorder().size(), 4u);   // b1 不可达
    EXPECT_FALSE(traversal.isReachable(BlockId{1}));

    DominatorTree dom;
    dom.compute(g);
    EXPECT_EQ(dom.idom(BlockId{2}), BlockId{0});
    EXPECT_EQ(dom.idom(BlockId{3}), BlockId{2});
    EXPECT_EQ(dom.idom(BlockId{4}), BlockId{2});
    EXPECT_TRUE(dom.dominates(BlockId{2}, BlockId{3}));
    EXPECT_FALSE(dom.dominates(BlockId{3}, BlockId{2}));
    // 回边 b3→b2：循环头在自己的支配边界里，回边源在头的边界里。
    EXPECT_EQ(indexes(dom.frontier(BlockId{2})), (std::vector<ca::u32>{2}));
    EXPECT_EQ(indexes(dom.frontier(BlockId{3})), (std::vector<ca::u32>{2}));
}

// ========== 错误路径 ==========

TEST(MjAsmCfgTest, BadExcEntryIsRejected)
{
    // handler 标签被销毁：构建显式失败，不静默吞掉异常表条目。
    DomFixture f;
    const auto iconst = f.add(Op::ICONST_1);
    const auto ls = f.label(iconst);
    const auto lh = f.label(iconst);
    f.add(Op::POP);
    const auto ret = f.add(Op::RETURN);
    const auto le = f.label(ret);
    f.bind(lh, ret);
    f.unit.exc.push_back({ls, le, lh, 0});
    ASSERT_TRUE(f.unit.dom.destroyLabel(lh).is_ok());

    const auto built = ControlFlowGraph::build(f.unit);
    ASSERT_TRUE(built.is_err());
    EXPECT_EQ(built.unwrap_err(), mjbase::asm_cfg::CfgError::BadExcEntry);
}
