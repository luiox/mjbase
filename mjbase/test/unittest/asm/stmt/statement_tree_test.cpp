/// @file        statement_tree_test.cpp
/// @brief       语句层测试：if/else、while/for/do-while 恢复、未识别块兜底、
///       语句↔指令窗口映射，以及叶子替换的写回与拒绝面。
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/stmt/statement_tree.hpp"

using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_stmt::replaceLeaf;
using mjbase::asm_stmt::StatementTree;
using mjbase::asm_stmt::StmtEditError;
using mjbase::asm_stmt::StmtId;
using mjbase::asm_stmt::StmtKind;

namespace {

InsnId appendOp(CodeDom& dom, Op op)
{
    Insn insn;
    insn.op    = op;
    const auto result = dom.append(insn);
    EXPECT_TRUE(result.is_ok());
    return result.unwrap();
}

InsnId appendImm(CodeDom& dom, Op op, ca::i32 operand)
{
    Insn insn;
    insn.op = op;
    insn.a  = operand;
    const auto result = dom.append(insn);
    EXPECT_TRUE(result.is_ok());
    return result.unwrap();
}

InsnId appendIinc(CodeDom& dom, ca::u32 slot, ca::i32 delta)
{
    Insn insn;
    insn.op = Op::IINC;
    insn.a  = static_cast<ca::i32>(slot);
    insn.b  = delta;
    const auto result = dom.append(insn);
    EXPECT_TRUE(result.is_ok());
    return result.unwrap();
}

InsnId appendBr(CodeDom& dom, Op op, LabelId target)
{
    Insn insn;
    insn.op     = op;
    insn.target = target;
    const auto result = dom.append(insn);
    EXPECT_TRUE(result.is_ok());
    return result.unwrap();
}

LabelId makeLbl(CodeDom& dom, InsnId anchor)
{
    const auto result = dom.createLabel(anchor);
    EXPECT_TRUE(result.is_ok());
    return result.unwrap();
}

void bindLbl(CodeDom& dom, LabelId label, InsnId anchor)
{
    EXPECT_TRUE(dom.bindLabel(label, anchor).is_ok());
}

Insn makeGoto(LabelId target)
{
    Insn insn;
    insn.op     = Op::GOTO;
    insn.target = target;
    return insn;
}

Insn makeImm(Op op, ca::i32 operand)
{
    Insn insn;
    insn.op = op;
    insn.a  = operand;
    return insn;
}

CodeUnit unitOf(CodeDom&& dom)
{
    CodeUnit unit;
    unit.dom = std::move(dom);
    return unit;
}

/// @brief 方法体 + 语句树；测试里一次性拿两者。
struct Built
{
    CodeUnit      unit;
    StatementTree tree;

    explicit Built(CodeDom dom)
        : unit(unitOf(std::move(dom)))
    {
        auto result = StatementTree::build(unit);
        EXPECT_TRUE(result.is_ok());
        if (result.is_ok()) tree = std::move(result).unwrap();
    }

    StmtId top(ca::usize index) const
    {
        const auto& children = tree.childrenOf(tree.root());
        return index < children.size() ? children[index] : mjbase::asm_stmt::kNoStmt;
    }
};

/// if/else：block0 条件、block1 跳汇合、block2 顺序落入汇合、block3 汇合。
CodeDom ifElseDom()
{
    CodeDom dom;
    const InsnId first = appendOp(dom, Op::NOP);
    appendOp(dom, Op::ILOAD_0);

    const LabelId lElse = makeLbl(dom, first);
    appendBr(dom, Op::IFEQ, lElse);

    appendIinc(dom, 0, 1);
    const LabelId lJoin = makeLbl(dom, first);
    appendBr(dom, Op::GOTO, lJoin);

    const InsnId elseFirst = appendIinc(dom, 0, 2);
    const InsnId joinFirst = appendOp(dom, Op::RETURN);

    bindLbl(dom, lElse, elseFirst);
    bindLbl(dom, lJoin, joinFirst);
    return dom;
}

}   // namespace

TEST(MjAsmStmtTest, BuildsLinearBlockAndCoversEveryInstruction)
{
    CodeDom dom;
    appendImm(dom, Op::BIPUSH, 1);
    const InsnId store = appendOp(dom, Op::ISTORE_0);
    appendOp(dom, Op::RETURN);

    Built built(std::move(dom));
    ASSERT_TRUE(built.tree.root().valid());
    EXPECT_EQ(built.tree.kindOf(built.tree.root()), StmtKind::Sequence);

    const auto& children = built.tree.childrenOf(built.tree.root());
    ASSERT_EQ(1u, children.size());
    EXPECT_EQ(built.tree.kindOf(children[0]), StmtKind::Block);
    EXPECT_EQ(built.tree.blockOf(children[0]).index, 0u);
    EXPECT_TRUE(built.tree.insnsOf(built.tree.root()) == built.unit.dom.order());
    EXPECT_EQ(built.tree.stmtOf(store), children[0]);
    EXPECT_EQ(built.tree.stmtOfBlock(mjbase::asm_cfg::BlockId{0}), children[0]);
}

TEST(MjAsmStmtTest, RecoversIfElseInProgramOrder)
{
    Built built(ifElseDom());
    ASSERT_EQ(2u, built.tree.childrenOf(built.tree.root()).size());

    const StmtId ifStmt = built.top(0);
    ASSERT_EQ(built.tree.kindOf(ifStmt), StmtKind::If);
    const auto* info = built.tree.ifInfoOf(ifStmt);
    ASSERT_NE(nullptr, info);
    EXPECT_TRUE(info->hasSecondArm);
    EXPECT_FALSE(info->firstArmOnTaken);

    const auto& arms = built.tree.childrenOf(ifStmt);
    ASSERT_EQ(2u, arms.size());
    EXPECT_EQ(built.tree.blockOf(arms[0]).index, 1u);
    EXPECT_EQ(built.tree.blockOf(arms[1]).index, 2u);
    EXPECT_EQ(built.tree.takenArmOf(ifStmt), arms[1]);
    EXPECT_EQ(built.tree.fallThroughArmOf(ifStmt), arms[0]);

    EXPECT_EQ(built.tree.stmtOfBlock(mjbase::asm_cfg::BlockId{0}), ifStmt);
    EXPECT_EQ(built.tree.stmtOfBlock(mjbase::asm_cfg::BlockId{1}), arms[0]);
    EXPECT_EQ(built.tree.stmtOfBlock(mjbase::asm_cfg::BlockId{2}), arms[1]);
    EXPECT_EQ(built.tree.stmtOfBlock(mjbase::asm_cfg::BlockId{3}), built.top(1));

    EXPECT_TRUE(built.tree.insnsOf(built.tree.root()) == built.unit.dom.order());
    for (const auto& insn : built.unit.dom.order()) {
        EXPECT_TRUE(built.tree.stmtOf(insn).valid());
    }
}

TEST(MjAsmStmtTest, RecoversSingleArmIfUntilJoin)
{
    CodeDom dom;
    const InsnId first = appendOp(dom, Op::ILOAD_0);
    const LabelId lJoin = makeLbl(dom, first);
    appendBr(dom, Op::IFEQ, lJoin);
    appendIinc(dom, 0, 1);
    const InsnId join = appendOp(dom, Op::RETURN);
    bindLbl(dom, lJoin, join);

    Built built(std::move(dom));
    ASSERT_EQ(2u, built.tree.childrenOf(built.tree.root()).size());

    const StmtId ifStmt = built.top(0);
    ASSERT_EQ(built.tree.kindOf(ifStmt), StmtKind::If);
    EXPECT_FALSE(built.tree.hasSecondArm(ifStmt));
    EXPECT_FALSE(built.tree.ifInfoOf(ifStmt)->firstArmOnTaken);
    EXPECT_EQ(built.tree.takenArmOf(ifStmt), mjbase::asm_stmt::kNoStmt);

    const StmtId arm = built.tree.fallThroughArmOf(ifStmt);
    ASSERT_TRUE(arm.valid());
    EXPECT_EQ(built.tree.blockOf(arm).index, 1u);
    EXPECT_EQ(built.tree.kindOf(built.top(1)), StmtKind::Block);
    EXPECT_TRUE(built.tree.insnsOf(built.tree.root()) == built.unit.dom.order());
}

TEST(MjAsmStmtTest, RecoversWhileLoopWithHeaderCondition)
{
    CodeDom dom;
    const InsnId headerFirst = appendOp(dom, Op::ILOAD_0);
    const LabelId lExit = makeLbl(dom, headerFirst);
    const InsnId branch = appendBr(dom, Op::IFEQ, lExit);
    const LabelId lHeader = makeLbl(dom, headerFirst);

    appendIinc(dom, 0, -1);
    appendBr(dom, Op::GOTO, lHeader);

    const InsnId exitFirst = appendOp(dom, Op::RETURN);
    bindLbl(dom, lExit, exitFirst);

    Built built(std::move(dom));
    ASSERT_EQ(2u, built.tree.childrenOf(built.tree.root()).size());

    const StmtId loop = built.top(0);
    ASSERT_EQ(built.tree.kindOf(loop), StmtKind::While);
    const auto* info = built.tree.loopInfoOf(loop);
    ASSERT_NE(nullptr, info);
    EXPECT_EQ(info->branch, branch);
    EXPECT_FALSE(info->bodyOnTaken);   // 条件是 IFEQ 退出：体走 fall-through

    const StmtId body = built.tree.loopBodyOf(loop);
    ASSERT_TRUE(body.valid());
    EXPECT_EQ(built.tree.blockOf(body).index, 1u);
    EXPECT_EQ(built.tree.stmtOf(branch), loop);
    EXPECT_EQ(built.tree.stmtOfBlock(mjbase::asm_cfg::BlockId{0}), loop);
    EXPECT_EQ(built.tree.kindOf(built.top(1)), StmtKind::Block);
    EXPECT_TRUE(built.tree.insnsOf(built.tree.root()) == built.unit.dom.order());
}

TEST(MjAsmStmtTest, RecoversForLoopWithUpdateBlock)
{
    CodeDom dom;
    const InsnId headerFirst = appendOp(dom, Op::ILOAD_0);
    const LabelId lExit = makeLbl(dom, headerFirst);
    const InsnId branch = appendBr(dom, Op::IFEQ, lExit);
    const LabelId lHeader = makeLbl(dom, headerFirst);
    const LabelId lUpdate = makeLbl(dom, headerFirst);

    // 体（block1）
    const InsnId bodyFirst = appendIinc(dom, 0, 1);
    appendBr(dom, Op::GOTO, lUpdate);

    // 更新（block2）
    const InsnId updateFirst = appendIinc(dom, 0, 1);
    bindLbl(dom, lUpdate, updateFirst);
    appendBr(dom, Op::GOTO, lHeader);

    // 退出（block3）
    const InsnId exitFirst = appendOp(dom, Op::RETURN);
    bindLbl(dom, lExit, exitFirst);

    Built built(std::move(dom));
    ASSERT_EQ(2u, built.tree.childrenOf(built.tree.root()).size());

    const StmtId loop = built.top(0);
    ASSERT_EQ(built.tree.kindOf(loop), StmtKind::For);
    const auto* info = built.tree.loopInfoOf(loop);
    ASSERT_NE(nullptr, info);
    EXPECT_EQ(info->branch, branch);
    EXPECT_FALSE(info->bodyOnTaken);

    const auto& children = built.tree.childrenOf(loop);
    ASSERT_EQ(2u, children.size());
    EXPECT_EQ(built.tree.blockOf(children[0]).index, 1u);
    EXPECT_EQ(built.tree.blockOf(children[1]).index, 2u);
    EXPECT_EQ(built.tree.loopBodyOf(loop), children[0]);
    EXPECT_EQ(built.tree.loopUpdateOf(loop), children[1]);
    EXPECT_EQ(built.tree.stmtOf(updateFirst), children[1]);
    EXPECT_EQ(built.tree.stmtOf(bodyFirst), children[0]);
    EXPECT_EQ(built.tree.stmtOf(info->branch), loop);
    EXPECT_TRUE(built.tree.insnsOf(built.tree.root()) == built.unit.dom.order());
}

TEST(MjAsmStmtTest, RecoversDoWhileWithLatchCondition)
{
    CodeDom dom;
    const InsnId first = appendOp(dom, Op::ICONST_5);
    appendOp(dom, Op::ISTORE_0);
    const LabelId lBody = makeLbl(dom, first);
    appendBr(dom, Op::GOTO, lBody);

    const InsnId bodyFirst = appendIinc(dom, 0, -1);
    bindLbl(dom, lBody, bodyFirst);
    const LabelId lLatch = makeLbl(dom, first);
    appendBr(dom, Op::GOTO, lLatch);

    const InsnId latchFirst = appendOp(dom, Op::ILOAD_0);
    bindLbl(dom, lLatch, latchFirst);
    const InsnId branch = appendBr(dom, Op::IFNE, lBody);

    appendOp(dom, Op::RETURN);

    Built built(std::move(dom));
    ASSERT_EQ(3u, built.tree.childrenOf(built.tree.root()).size());
    EXPECT_EQ(built.tree.kindOf(built.top(0)), StmtKind::Block);

    const StmtId loop = built.top(1);
    ASSERT_EQ(built.tree.kindOf(loop), StmtKind::DoWhile);
    const auto* info = built.tree.loopInfoOf(loop);
    ASSERT_NE(nullptr, info);
    EXPECT_EQ(info->branch, branch);
    EXPECT_TRUE(info->bodyOnTaken);

    const StmtId body = built.tree.loopBodyOf(loop);
    EXPECT_EQ(built.tree.blockOf(body).index, 1u);
    EXPECT_EQ(built.tree.stmtOf(latchFirst), loop);
    EXPECT_EQ(built.tree.stmtOf(branch), loop);
    EXPECT_EQ(built.tree.kindOf(built.top(2)), StmtKind::Block);
    EXPECT_TRUE(built.tree.insnsOf(built.tree.root()) == built.unit.dom.order());
}

TEST(MjAsmStmtTest, LeavesUnrecognizedConditionalOpaque)
{
    CodeDom dom;
    const InsnId first = appendOp(dom, Op::ILOAD_0);
    const LabelId lTarget = makeLbl(dom, first);
    appendBr(dom, Op::IFEQ, lTarget);

    const LabelId lOther = makeLbl(dom, first);
    appendBr(dom, Op::IFNE, lOther);

    const InsnId targetFirst = appendOp(dom, Op::RETURN);
    bindLbl(dom, lTarget, targetFirst);
    const InsnId otherFirst = appendOp(dom, Op::RETURN);
    bindLbl(dom, lOther, otherFirst);

    Built built(std::move(dom));
    ASSERT_EQ(2u, built.tree.childrenOf(built.tree.root()).size());

    EXPECT_EQ(built.tree.kindOf(built.top(0)), StmtKind::Opaque);
    EXPECT_EQ(built.tree.blockOf(built.top(0)).index, 0u);
    EXPECT_EQ(built.tree.kindOf(built.top(1)), StmtKind::If);
    EXPECT_TRUE(built.tree.insnsOf(built.tree.root()) == built.unit.dom.order());
    for (const auto& insn : built.unit.dom.order()) {
        EXPECT_TRUE(built.tree.stmtOf(insn).valid());
    }
}

TEST(MjAsmStmtTest, EmptyMethodHasOnlySequenceRoot)
{
    CodeUnit unit;
    auto result = StatementTree::build(unit);
    ASSERT_TRUE(result.is_ok());
    StatementTree tree = std::move(result).unwrap();

    EXPECT_TRUE(tree.root().valid());
    EXPECT_EQ(tree.kindOf(tree.root()), StmtKind::Sequence);
    EXPECT_TRUE(tree.childrenOf(tree.root()).empty());
    EXPECT_TRUE(tree.insnsOf(tree.root()).empty());
    EXPECT_EQ(tree.cfg().blockCount(), 0u);
    EXPECT_EQ(tree.size(), 1u);
}

TEST(MjAsmStmtTest, ReplaceLeafWritesThroughCodeEditorAndTreeRebuilds)
{
    CodeDom dom;
    const InsnId first = appendImm(dom, Op::BIPUSH, 1);
    const LabelId lTarget = makeLbl(dom, first);
    const InsnId oldGoto = appendBr(dom, Op::GOTO, lTarget);
    const InsnId targetFirst = appendOp(dom, Op::ISTORE_0);
    appendOp(dom, Op::RETURN);
    bindLbl(dom, lTarget, targetFirst);

    Built built(std::move(dom));
    ASSERT_EQ(2u, built.tree.childrenOf(built.tree.root()).size());

    const StmtId leaf = built.top(0);
    ASSERT_EQ(built.tree.kindOf(leaf), StmtKind::Block);

    const auto labels = built.unit.dom.labelsAt(targetFirst);
    ASSERT_FALSE(labels.empty());
    const LabelId target = labels.front();

    auto replaced = replaceLeaf(built.unit, built.tree, leaf,
                                {makeImm(Op::BIPUSH, 7), makeGoto(target)});
    ASSERT_TRUE(replaced.is_ok());
    const InsnId newFirst = replaced.unwrap();
    ASSERT_NE(newFirst, jbc::ir::kNoInsn);

    const jbc::ir::Insn* fresh = built.unit.dom.peek(newFirst);
    ASSERT_NE(nullptr, fresh);
    EXPECT_EQ(fresh->op, Op::BIPUSH);
    EXPECT_EQ(fresh->a, 7);
    EXPECT_EQ(built.unit.dom.size(), 4u);
    EXPECT_EQ(nullptr, built.unit.dom.peek(oldGoto));

    auto rebuilt = StatementTree::build(built.unit);
    ASSERT_TRUE(rebuilt.is_ok());
    StatementTree tree = std::move(rebuilt).unwrap();
    ASSERT_EQ(2u, tree.childrenOf(tree.root()).size());
    EXPECT_EQ(tree.kindOf(tree.childrenOf(tree.root())[0]), StmtKind::Block);
    EXPECT_EQ(tree.insnsOf(tree.childrenOf(tree.root())[0]).size(), 2u);
    EXPECT_TRUE(tree.insnsOf(tree.root()) == built.unit.dom.order());
}

TEST(MjAsmStmtTest, ReplaceLeafWithEmptySequenceDeletesBlock)
{
    CodeDom dom;
    appendImm(dom, Op::BIPUSH, 1);
    appendOp(dom, Op::IRETURN);

    Built built(std::move(dom));
    ASSERT_EQ(1u, built.tree.childrenOf(built.tree.root()).size());

    auto replaced = replaceLeaf(built.unit, built.tree, built.top(0), {});
    ASSERT_TRUE(replaced.is_ok());
    EXPECT_EQ(built.unit.dom.size(), 0u);
    EXPECT_EQ(replaced.unwrap(), jbc::ir::kNoInsn);

    auto rebuilt = StatementTree::build(built.unit);
    ASSERT_TRUE(rebuilt.is_ok());
    StatementTree tree = std::move(rebuilt).unwrap();
    EXPECT_TRUE(tree.childrenOf(tree.root()).empty());
    EXPECT_TRUE(tree.insnsOf(tree.root()).empty());
}

TEST(MjAsmStmtTest, ReplaceLeafRejectsExternallyReferencedWindow)
{
    CodeDom dom;
    const InsnId first = appendOp(dom, Op::NOP);
    const LabelId lTarget = makeLbl(dom, first);
    appendBr(dom, Op::GOTO, lTarget);
    const InsnId targetFirst = appendOp(dom, Op::RETURN);
    bindLbl(dom, lTarget, targetFirst);

    Built built(std::move(dom));
    ASSERT_EQ(2u, built.tree.childrenOf(built.tree.root()).size());
    const StmtId leaf = built.top(1);
    ASSERT_EQ(built.tree.blockOf(leaf).index, 1u);

    const ca::usize before = built.unit.dom.size();
    auto            replaced = replaceLeaf(built.unit, built.tree, leaf, {makeImm(Op::BIPUSH, 2)});
    ASSERT_TRUE(replaced.is_err());
    EXPECT_EQ(replaced.unwrap_err(), StmtEditError::ExternallyReferenced);
    EXPECT_EQ(built.unit.dom.size(), before);
    EXPECT_NE(nullptr, built.unit.dom.peek(targetFirst));
}

TEST(MjAsmStmtTest, ReplaceLeafRejectsAttachedBoundaryLabel)
{
    CodeDom dom;
    const InsnId first = appendImm(dom, Op::BIPUSH, 1);
    appendOp(dom, Op::IRETURN);

    Built built(std::move(dom));
    ASSERT_EQ(1u, built.tree.childrenOf(built.tree.root()).size());

    auto label = built.unit.dom.createLabel(first);
    ASSERT_TRUE(label.is_ok());

    auto replaced = replaceLeaf(built.unit, built.tree, built.top(0), {makeImm(Op::BIPUSH, 2)});
    ASSERT_TRUE(replaced.is_err());
    EXPECT_EQ(replaced.unwrap_err(), StmtEditError::LabelAttached);
    EXPECT_EQ(built.unit.dom.size(), 2u);
}

TEST(MjAsmStmtTest, ReplaceLeafRejectsExceptionBoundaryInsideWindow)
{
    CodeDom dom;
    const InsnId first = appendImm(dom, Op::BIPUSH, 1);
    appendOp(dom, Op::IRETURN);

    Built built(std::move(dom));
    ASSERT_EQ(1u, built.tree.childrenOf(built.tree.root()).size());

    auto start   = built.unit.dom.createLabel(first);
    auto end     = built.unit.dom.createLabel(first);
    auto handler = built.unit.dom.createLabel(first);
    ASSERT_TRUE(start.is_ok());
    ASSERT_TRUE(end.is_ok());
    ASSERT_TRUE(handler.is_ok());
    built.unit.exc.push_back(jbc::ir::CodeUnit::Exc{start.unwrap(), end.unwrap(), handler.unwrap(), 0});

    auto replaced = replaceLeaf(built.unit, built.tree, built.top(0), {makeImm(Op::BIPUSH, 2)});
    ASSERT_TRUE(replaced.is_err());
    EXPECT_EQ(replaced.unwrap_err(), StmtEditError::ExceptionBoundary);
}

TEST(MjAsmStmtTest, ReplaceLeafRollsBackWhenReplacementIsUnencodable)
{
    CodeDom dom;
    const InsnId first = appendImm(dom, Op::BIPUSH, 1);
    appendOp(dom, Op::IRETURN);

    Built built(std::move(dom));
    ASSERT_EQ(1u, built.tree.childrenOf(built.tree.root()).size());

    Insn bad;
    bad.op     = Op::GOTO;
    bad.target = jbc::ir::kNoLabel;
    auto replaced = replaceLeaf(built.unit, built.tree, built.top(0), {bad});
    ASSERT_TRUE(replaced.is_err());
    EXPECT_EQ(replaced.unwrap_err(), StmtEditError::EditFailed);
    EXPECT_EQ(built.unit.dom.size(), 2u);
    EXPECT_NE(nullptr, built.unit.dom.peek(first));
}

TEST(MjAsmStmtTest, ReplaceLeafRejectsStructuredAndForeignStatements)
{
    Built built(ifElseDom());
    const StmtId ifStmt = built.top(0);
    ASSERT_EQ(built.tree.kindOf(ifStmt), StmtKind::If);

    auto replaced = replaceLeaf(built.unit, built.tree, ifStmt, {});
    ASSERT_TRUE(replaced.is_err());
    EXPECT_EQ(replaced.unwrap_err(), StmtEditError::NotLeaf);

    StatementTree foreign;
    auto          invalid = replaceLeaf(built.unit, foreign, StmtId{0}, {});
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(invalid.unwrap_err(), StmtEditError::InvalidStmt);
}
