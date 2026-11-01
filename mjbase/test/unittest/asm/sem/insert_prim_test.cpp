/// @file        insert_prim_test.cpp
/// @brief       插入原语直接单测（A5）：标签重锚/fallthrough 批量插入、
///              copy-set 前导装载、DUP 复位发射——小语料逐轴钉死，不借
///              outline 间接测。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/sem/insert_prim.hpp"

#include "sem_fixture.hpp"

namespace {

using jbc::core::Op;
using jbc::ir::CodeUnit;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::MethodSpec;
using mjbase::asm_sem::SlotCopy;
using mjbase::asm_sem::ValueFamily;

/// 分支目标语料：ifeq 的目标块头（iconst_5）携标签，批量插入以此为锚。
MethodSpec branchTargetSpec()
{
    MethodSpec m;
    m.name      = "t";
    m.desc      = "(IZ)I";
    m.maxStack  = 2;
    m.maxLocals = 2;
    m.emit      = [](CodeEmit& e) {
        const std::size_t other = e.newLabel();   /// 0
        const std::size_t join  = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, other);
        e.op(Op::ICONST_1);
        e.branch(Op::GOTO, join);
        e.bindLabel(other);
        e.op(Op::ICONST_5);             /// 标签锚定块头（插入原语锚点）
        e.bindLabel(join);
        e.op(Op::IRETURN);
    };
    return m;
}

/// dom 程序序的 opcode 快照。
std::vector<Op> opSequence(CodeUnit& unit)
{
    std::vector<Op> out;
    for (const jbc::ir::InsnId id : unit.dom.order()) {
        const jbc::ir::Insn* in = unit.dom.peek(id);
        if (in != nullptr) {
            out.push_back(in->op);
        }
    }
    return out;
}

bool hasLabelOn(CodeUnit& unit, jbc::ir::LabelId label, jbc::ir::InsnId id)
{
    for (const jbc::ir::LabelId l : unit.dom.labelsAt(id)) {
        if (l == label) {
            return true;
        }
    }
    return false;
}

/// 处方参数速记（desc 驻留调用方 arena——Utf8StringRef 非拥有，arena 须
/// 覆盖用例生命周期，同 planOutline 的 intern 口径）。
SlotCopy stackParam(const ca::str::Utf8StringRef& desc, ca::u32 fromTop, ca::u32 newSlot)
{
    SlotCopy p;
    p.kind    = SlotCopy::Kind::Stack;
    p.fromTop = fromTop;
    p.newSlot = newSlot;
    p.desc    = desc;
    return p;
}

SlotCopy localParam(const ca::str::Utf8StringRef& desc, ca::u32 oldLocal, ca::u32 newSlot)
{
    SlotCopy p;
    p.kind     = SlotCopy::Kind::Local;
    p.oldLocal = oldLocal;
    p.newSlot  = newSlot;
    p.desc     = desc;
    return p;
}

}   // namespace

/// 值形态原语：槽宽/族/装载与返回 opcode 选型轴。
TEST(MjAsmSemInsertPrimTest, ValueFamilySelection)
{
    EXPECT_EQ(mjbase::asm_sem::descSlots('J'), 2u);
    EXPECT_EQ(mjbase::asm_sem::descSlots('D'), 2u);
    EXPECT_EQ(mjbase::asm_sem::descSlots('I'), 1u);
    EXPECT_EQ(mjbase::asm_sem::descSlots('L'), 1u);

    EXPECT_EQ(mjbase::asm_sem::familyOfDesc('I'), ValueFamily::Int);
    EXPECT_EQ(mjbase::asm_sem::familyOfDesc('Z'), ValueFamily::Int);
    EXPECT_EQ(mjbase::asm_sem::familyOfDesc('J'), ValueFamily::Long);
    EXPECT_EQ(mjbase::asm_sem::familyOfDesc('F'), ValueFamily::Float);
    EXPECT_EQ(mjbase::asm_sem::familyOfDesc('D'), ValueFamily::Double);
    EXPECT_EQ(mjbase::asm_sem::familyOfDesc('L'), ValueFamily::Ref);
    EXPECT_EQ(mjbase::asm_sem::familyOfDesc('['), ValueFamily::Ref);

    EXPECT_EQ(mjbase::asm_sem::loadOpOf(ValueFamily::Int), Op::ILOAD);
    EXPECT_EQ(mjbase::asm_sem::loadOpOf(ValueFamily::Long), Op::LLOAD);
    EXPECT_EQ(mjbase::asm_sem::loadOpOf(ValueFamily::Float), Op::FLOAD);
    EXPECT_EQ(mjbase::asm_sem::loadOpOf(ValueFamily::Double), Op::DLOAD);
    EXPECT_EQ(mjbase::asm_sem::loadOpOf(ValueFamily::Ref), Op::ALOAD);
    EXPECT_EQ(mjbase::asm_sem::returnOpOf(ValueFamily::Long), Op::LRETURN);
    EXPECT_EQ(mjbase::asm_sem::returnOpOf(ValueFamily::Ref), Op::ARETURN);
}

/// 紧凑局部形态原语：显式化 + 槽号解出 + 槽宽（cat2 占 2）。
TEST(MjAsmSemInsertPrimTest, CompactLocalFormPrimitives)
{
    EXPECT_EQ(mjbase::asm_sem::explicitLocalOp(Op::ALOAD_1), Op::ALOAD);
    EXPECT_EQ(mjbase::asm_sem::explicitLocalOp(Op::LSTORE_2), Op::LSTORE);
    EXPECT_EQ(mjbase::asm_sem::explicitLocalOp(Op::ILOAD), Op::ILOAD);

    jbc::ir::Insn compact;
    compact.op = Op::ALOAD_2;
    EXPECT_EQ(mjbase::asm_sem::localOperandSlot(compact), 2u);
    jbc::ir::Insn wide;
    wide.op = Op::LLOAD;
    wide.a  = 5;
    EXPECT_EQ(mjbase::asm_sem::localOperandSlot(wide), 5u);

    EXPECT_EQ(mjbase::asm_sem::localOpSlotWidth(Op::LLOAD), 2u);
    EXPECT_EQ(mjbase::asm_sem::localOpSlotWidth(Op::DSTORE), 2u);
    EXPECT_EQ(mjbase::asm_sem::localOpSlotWidth(Op::ISTORE), 1u);
}

/// 前导装载原语：描述符 → 显式装载指令（槽号落 a）。
TEST(MjAsmSemInsertPrimTest, LocalLoadInsnSelection)
{
    const jbc::ir::Insn l = mjbase::asm_sem::localLoadInsn('J', 3);
    EXPECT_EQ(l.op, Op::LLOAD);
    EXPECT_EQ(l.a, 3);
    const jbc::ir::Insn a = mjbase::asm_sem::localLoadInsn('[', 1);
    EXPECT_EQ(a.op, Op::ALOAD);
    EXPECT_EQ(a.a, 1);
}

/// copy-set 前导装载：kind 过滤 + 槽选择（oldLocal / newSlot）+ 参数序。
TEST(MjAsmSemInsertPrimTest, CopySetLoadPreambleFilterAndSlot)
{
    ca::str::Utf8StringArena arena;
    const auto dI = arena.intern("I");
    const auto dL = arena.intern("L");
    const auto dJ = arena.intern("J");

    std::vector<SlotCopy> params;
    params.push_back(stackParam(dI, 1, 0));    /// 栈切片：调用点免装载
    params.push_back(localParam(dL, 4, 2));    /// 局部入参
    params.push_back(localParam(dJ, 1, 3));    /// cat2 局部入参

    const auto callSite = mjbase::asm_sem::copySetLoadPreamble(
        params, SlotCopy::Kind::Local, false);
    ASSERT_EQ(callSite.size(), 2u);
    EXPECT_EQ(callSite[0].op, Op::ALOAD);
    EXPECT_EQ(callSite[0].a, 4);
    EXPECT_EQ(callSite[1].op, Op::LLOAD);
    EXPECT_EQ(callSite[1].a, 1);

    const auto nPrologue = mjbase::asm_sem::copySetLoadPreamble(
        params, SlotCopy::Kind::Stack, true);
    ASSERT_EQ(nPrologue.size(), 1u);
    EXPECT_EQ(nPrologue[0].op, Op::ILOAD);
    EXPECT_EQ(nPrologue[0].a, 0);
}

/// DUP 复位发射原语：DUP/DUP2 透传（§3.4 tier-2 调用点侧栈形重排）。
TEST(MjAsmSemInsertPrimTest, EntryDupResetEmission)
{
    EXPECT_EQ(mjbase::asm_sem::entryDupResetInsn(Op::DUP).op, Op::DUP);
    EXPECT_EQ(mjbase::asm_sem::entryDupResetInsn(Op::DUP2).op, Op::DUP2);
}

/// 标签重锚批量插入：程序序序列落标签边界、标签重锚到首条、句柄按程序序
/// 返回（逆序插入 trick 的核心契约）。
TEST(MjAsmSemInsertPrimTest, InsertAnchoredAtLabelProgramOrder)
{
    ClassBytesBuilder b("T");
    SemFixture        fx(b.assemble({branchTargetSpec()}));
    CodeUnit*         unit = fx.code("t");
    ASSERT_NE(unit, nullptr);

    /// 定位 ifeq 目标签：目标块头（iconst_5）携标签。
    jbc::ir::LabelId  anchor{};
    jbc::ir::InsnId   targetId{};
    for (const jbc::ir::InsnId id : unit->dom.order()) {
        const jbc::ir::Insn* in = unit->dom.peek(id);
        if (in != nullptr && in->op == Op::IFEQ) {
            auto res = unit->dom.resolveLabel(in->target);
            ASSERT_TRUE(res.is_ok());
            targetId = res.unwrap().insn;
            const std::vector<jbc::ir::LabelId> labels = unit->dom.labelsAt(targetId);
            ASSERT_FALSE(labels.empty());
            anchor = labels.front();
            break;
        }
    }
    ASSERT_TRUE(unit->dom.validLabel(anchor));

    /// 序列 [iconst_2, iconst_3] 落标签边界。
    jbc::ir::Insn a;
    a.op = Op::ICONST_2;
    jbc::ir::Insn c;
    c.op = Op::ICONST_3;
    jbc::ir::CodeEditor ed{*unit};
    auto placed =
        mjbase::asm_sem::insertAnchoredAtLabel(ed, anchor, {a, c});
    ASSERT_TRUE(placed.is_ok());
    const std::vector<jbc::ir::InsnId> ids = std::move(placed).unwrap();
    ASSERT_EQ(ids.size(), 2u);

    /// 终序 = 程序序：iload0, ifeq, iconst_1, goto, iconst_2, iconst_3,
    /// iconst_5, ireturn（插入段落标签边界之后、原块头之前）。
    const std::vector<Op> seq = opSequence(*unit);
    ASSERT_EQ(seq.size(), 8u);
    EXPECT_EQ(seq[4], Op::ICONST_2);
    EXPECT_EQ(seq[5], Op::ICONST_3);
    EXPECT_EQ(seq[6], Op::ICONST_5);

    /// 标签重锚到首条；次条无标签。
    EXPECT_TRUE(hasLabelOn(*unit, anchor, ids[0]));
    EXPECT_FALSE(hasLabelOn(*unit, anchor, ids[1]));
}

/// 标签重锚批量插入（多批次堆叠）：后批落标签边界 → 执行序 = 后批在前
/// （outline 调用点「DUP 复位先于装载」的堆叠语义承载）。
TEST(MjAsmSemInsertPrimTest, InsertAnchoredAtLabelBatchesStack)
{
    ClassBytesBuilder b("T");
    SemFixture        fx(b.assemble({branchTargetSpec()}));
    CodeUnit*         unit = fx.code("t");
    ASSERT_NE(unit, nullptr);

    jbc::ir::LabelId anchor{};
    for (const jbc::ir::InsnId id : unit->dom.order()) {
        const jbc::ir::Insn* in = unit->dom.peek(id);
        if (in != nullptr && in->op == Op::IFEQ) {
            auto res = unit->dom.resolveLabel(in->target);
            ASSERT_TRUE(res.is_ok());
            anchor = unit->dom.labelsAt(res.unwrap().insn).front();
            break;
        }
    }
    ASSERT_TRUE(unit->dom.validLabel(anchor));

    jbc::ir::Insn load;
    load.op = Op::ILOAD;
    load.a  = 0;
    jbc::ir::Insn dup;
    dup.op = Op::DUP;

    jbc::ir::CodeEditor ed{*unit};
    auto first = mjbase::asm_sem::insertAnchoredAtLabel(ed, anchor, {load});
    ASSERT_TRUE(first.is_ok());
    auto second = mjbase::asm_sem::insertAnchoredAtLabel(ed, anchor, {dup});
    ASSERT_TRUE(second.is_ok());

    /// 执行序：dup（后批）先于 load（前批）——tier-2 DUP 复位先于装载。
    const std::vector<Op> seq = opSequence(*unit);
    ASSERT_EQ(seq.size(), 8u);
    EXPECT_EQ(seq[4], Op::DUP);
    EXPECT_EQ(seq[5], Op::ILOAD);
    EXPECT_EQ(seq[6], Op::ICONST_5);
}

/// fallthrough 批量插入：程序序正序插锚指令之前。
TEST(MjAsmSemInsertPrimTest, InsertBeforeInsnProgramOrder)
{
    ClassBytesBuilder b("T");
    SemFixture        fx(b.assemble({branchTargetSpec()}));
    CodeUnit*         unit = fx.code("t");
    ASSERT_NE(unit, nullptr);

    /// 锚 = 尾返回指令。
    const std::vector<jbc::ir::InsnId>& order = unit->dom.order();
    const jbc::ir::Insn* ret = unit->dom.peek(order.back());
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->op, Op::IRETURN);

    jbc::ir::Insn a;
    a.op = Op::ICONST_2;
    jbc::ir::Insn c;
    c.op = Op::ICONST_3;
    jbc::ir::CodeEditor ed{*unit};
    auto placed = mjbase::asm_sem::insertBeforeInsn(ed, order.back(), {a, c});
    ASSERT_TRUE(placed.is_ok());
    const std::vector<jbc::ir::InsnId> ids = std::move(placed).unwrap();
    ASSERT_EQ(ids.size(), 2u);

    const std::vector<Op> seq = opSequence(*unit);
    ASSERT_EQ(seq.size(), 8u);
    EXPECT_EQ(seq[5], Op::ICONST_2);
    EXPECT_EQ(seq[6], Op::ICONST_3);
    EXPECT_EQ(seq[7], Op::IRETURN);
}

/// fail-closed：无效标签/锚指令 → Err(EditFailed)（与 apply 内嵌口径一致）。
TEST(MjAsmSemInsertPrimTest, InvalidAnchorFailsClosed)
{
    ClassBytesBuilder b("T");
    SemFixture        fx(b.assemble({branchTargetSpec()}));
    CodeUnit*         unit = fx.code("t");
    ASSERT_NE(unit, nullptr);

    jbc::ir::Insn a;
    a.op = Op::ICONST_2;

    jbc::ir::CodeEditor edL{*unit};
    auto badLabel =
        mjbase::asm_sem::insertAnchoredAtLabel(edL, jbc::ir::kNoLabel, {a});
    ASSERT_TRUE(badLabel.is_err());
    EXPECT_EQ(badLabel.unwrap_err(), mjbase::asm_sem::OutlineError::EditFailed);

    jbc::ir::CodeEditor edI{*unit};
    auto badInsn =
        mjbase::asm_sem::insertBeforeInsn(edI, jbc::ir::kNoInsn, {a});
    ASSERT_TRUE(badInsn.is_err());
    EXPECT_EQ(badInsn.unwrap_err(), mjbase::asm_sem::OutlineError::EditFailed);
}
