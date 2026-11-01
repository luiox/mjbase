/// @file        liveness_view_test.cpp
/// @brief       LivenessView 测试：后向活跃事实逐点钉死（写后杀 / 汇合活 /
///              回边活 / cat2 双槽 / 死写不活）。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/sem/liveness_view.hpp"

#include "sem_fixture.hpp"

namespace {

using jbc::core::Op;
using mjbase::asm_sem::LivenessView;

/// 按 opcode + 局部操作数定位指令（返回全部命中，程序序）。
std::vector<jbc::ir::InsnId> findLocalInsns(jbc::ir::CodeUnit& unit, Op op, ca::u32 slot)
{
    std::vector<jbc::ir::InsnId> hits;
    for (const jbc::ir::InsnId id : unit.dom.order()) {
        const jbc::ir::Insn* in = unit.dom.peek(id);
        if (in == nullptr || in->op != op) {
            continue;
        }
        if (static_cast<ca::u32>(in->a) == slot) {
            hits.push_back(id);
        }
    }
    return hits;
}

/// 唯一命中版（断言恰一条）。
jbc::ir::InsnId findLocalInsn(jbc::ir::CodeUnit& unit, Op op, ca::u32 slot)
{
    const std::vector<jbc::ir::InsnId> hits = findLocalInsns(unit, op, slot);
    EXPECT_EQ(hits.size(), 1u) << "opcode " << static_cast<int>(op) << " slot " << slot;
    return hits.empty() ? jbc::ir::InsnId{} : hits.front();
}

}   // namespace

/// 汇合前写、汇合后读：槽全程活跃；入参最后读点之后死亡。
TEST(MjAsmSemLivenessTest, MergeLiveAndParamDeath)
{
    mjbase::analysis::test_support::ClassBytesBuilder b("T");
    mjbase::analysis::test_support::MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IIZ)I";
    m.maxStack  = 2;
    m.maxLocals = 4;
    m.emit      = [](mjbase::analysis::test_support::CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();
        const std::size_t join    = e.newLabel();
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, elseArm);
        e.varInsn(Op::ILOAD, 0);        /// a（最后读点）
        e.varInsn(Op::ISTORE, 3);       /// t = a
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);
        e.varInsn(Op::ISTORE, 3);       /// t = 5
        e.bindLabel(join);
        e.varInsn(Op::ILOAD, 3);        /// 读 t（两路汇合）
        e.varInsn(Op::ILOAD, 1);        /// b（最后读点）
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    SemFixture fx(b.assemble({m}));
    jbc::ir::CodeUnit* unit = fx.code("pick");
    ASSERT_NE(unit, nullptr);

    auto built = LivenessView::build(*unit);
    ASSERT_TRUE(built.is_ok());
    LivenessView view = std::move(built).unwrap();
    EXPECT_EQ(view.slotCount(), 4u);

    /// 汇合读 t → 两臂的 istore 之后 t、b 均活跃。
    const std::vector<jbc::ir::InsnId> istores = findLocalInsns(*unit, Op::ISTORE, 3);
    ASSERT_EQ(istores.size(), 2u);
    for (const jbc::ir::InsnId id : istores) {
        EXPECT_TRUE(view.localLiveAfter(id, 3));
        EXPECT_TRUE(view.localLiveAfter(id, 1));   /// b 在 join 后读
    }
    /// 入参 a：iload_0（最后读点）之后死亡——写 t 不保活 a。
    const jbc::ir::InsnId iloadA = findLocalInsn(*unit, Op::ILOAD, 0);
    EXPECT_FALSE(view.localLiveAfter(iloadA, 0));
    EXPECT_TRUE(view.localLiveBefore(iloadA, 0));
    /// join 首条 iload 3：t 已被读走（死后无读），b 随后仍读。
    const jbc::ir::InsnId iloadT = findLocalInsn(*unit, Op::ILOAD, 3);
    EXPECT_FALSE(view.localLiveAfter(iloadT, 3));
    EXPECT_TRUE(view.localLiveAfter(iloadT, 1));
    /// 入参 b：其最后读点（join 后 iload_1）之后全死。
    const jbc::ir::InsnId iloadB = findLocalInsn(*unit, Op::ILOAD, 1);
    EXPECT_FALSE(view.localLiveAfter(iloadB, 1));
    EXPECT_TRUE(view.liveAfterSlots(iloadB).empty());
}

/// 死写（写后无读）：out 状态不活跃——outline 判据 3 放开的活性依据。
TEST(MjAsmSemLivenessTest, DeadWriteNotLive)
{
    mjbase::analysis::test_support::ClassBytesBuilder b("T");
    mjbase::analysis::test_support::MethodSpec m;
    m.name      = "pick";
    m.desc      = "(I)I";
    m.maxStack  = 2;
    m.maxLocals = 2;
    m.emit      = [](mjbase::analysis::test_support::CodeEmit& e) {
        e.op(Op::BIPUSH);
        e.u8(7);
        e.varInsn(Op::ISTORE, 1);       /// 死写：t = 7（后续无读）
        e.varInsn(Op::ILOAD, 0);
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    SemFixture fx(b.assemble({m}));
    jbc::ir::CodeUnit* unit = fx.code("pick");
    ASSERT_NE(unit, nullptr);

    auto built = LivenessView::build(*unit);
    ASSERT_TRUE(built.is_ok());
    LivenessView view = std::move(built).unwrap();
    const jbc::ir::InsnId istore = findLocalInsn(*unit, Op::ISTORE, 1);
    EXPECT_FALSE(view.localLiveAfter(istore, 1));
}

/// 回边活跃：循环体内 iinc 之后槽经回边仍被头读 → 活跃（环上收敛）。
TEST(MjAsmSemLivenessTest, BackEdgeKeepsLive)
{
    mjbase::analysis::test_support::ClassBytesBuilder b("T");
    mjbase::analysis::test_support::MethodSpec m;
    m.name      = "pick";
    m.desc      = "(I)I";
    m.maxStack  = 2;
    m.maxLocals = 2;
    m.emit      = [](mjbase::analysis::test_support::CodeEmit& e) {
        const std::size_t head = e.newLabel();
        const std::size_t exit = e.newLabel();
        e.op(Op::ICONST_0);
        e.varInsn(Op::ISTORE, 1);
        e.bindLabel(head);
        e.varInsn(Op::ILOAD, 1);        /// 读 i
        e.varInsn(Op::ILOAD, 0);        /// 读 n
        e.branch(Op::IF_ICMPGE, exit);
        e.op(Op::IINC);                 /// i++（回边到 head；index=1 delta=1）
        e.u8(1);
        e.u8(1);
        e.branch(Op::GOTO, head);
        e.bindLabel(exit);
        e.varInsn(Op::ILOAD, 1);        /// 出口读 i
        e.op(Op::IRETURN);
    };
    SemFixture fx(b.assemble({m}));
    jbc::ir::CodeUnit* unit = fx.code("pick");
    ASSERT_NE(unit, nullptr);

    auto built = LivenessView::build(*unit);
    ASSERT_TRUE(built.is_ok());
    LivenessView view = std::move(built).unwrap();
    const jbc::ir::InsnId iinc = findLocalInsn(*unit, Op::IINC, 1);
    EXPECT_TRUE(view.localLiveAfter(iinc, 1));   /// 回边 → head 读 i
    EXPECT_TRUE(view.localLiveAfter(iinc, 0));   /// 回边 → head 读 n
}

/// cat2：lstore 连带杀高半槽；store 前 load 使用。
TEST(MjAsmSemLivenessTest, Cat2StoreKillsHighHalf)
{
    mjbase::analysis::test_support::ClassBytesBuilder b("T");
    mjbase::analysis::test_support::MethodSpec m;
    m.name      = "pick";
    m.desc      = "(J)J";
    m.maxStack  = 4;
    m.maxLocals = 4;
    m.emit      = [](mjbase::analysis::test_support::CodeEmit& e) {
        e.varInsn(Op::LLOAD, 0);
        e.varInsn(Op::LSTORE, 2);       /// t = v（槽 2/3）
        e.varInsn(Op::LLOAD, 2);
        e.op(Op::LRETURN);
    };
    SemFixture fx(b.assemble({m}));
    jbc::ir::CodeUnit* unit = fx.code("pick");
    ASSERT_NE(unit, nullptr);

    auto built = LivenessView::build(*unit);
    ASSERT_TRUE(built.is_ok());
    LivenessView view = std::move(built).unwrap();
    const jbc::ir::InsnId lstore = findLocalInsn(*unit, Op::LSTORE, 2);
    EXPECT_TRUE(view.localLiveAfter(lstore, 2));    /// lload 2 再读
    const jbc::ir::InsnId lloadV = findLocalInsn(*unit, Op::LLOAD, 0);
    EXPECT_FALSE(view.localLiveAfter(lloadV, 0));   /// v 无后续读
}
