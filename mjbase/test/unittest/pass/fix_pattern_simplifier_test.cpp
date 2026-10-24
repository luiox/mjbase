// FixPatternSimplifier：固定模式简化（位运算族 + 死代码族 + 整数比较常量偏移
// 折叠 + 同 slot store/load 中转消除）。用例意图移植自旧栈
// mbase/unittest/pass/opt/replace_pass_test.cpp 的 FixPatternSimplifierTest 段，
// 并按新栈底座补标签边界、来源 pc 继承与终止性守卫的面。
//
// 构造路径：CodeDom 直构（种子会话 + append + LabelId 标签），不碰 classfile
// 裸字节；跑生产管线（promote → pass → 统一 lower）后断言变换产物。
#include <gtest/gtest.h>

#include <string>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/fix_pattern_simplifier.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "fix_pattern_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

// ── 位运算固定模式（规则 1-7） ──

TEST(FixPatternSimplifierTest, inegIconstM1IaddFoldsToIxor)
{
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::INEG);
        b.op(Op::ICONST_M1);
        b.op(Op::IADD);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::ICONST_M1);
    EXPECT_EQ(ops[2], Op::IXOR);
    EXPECT_EQ(ops[3], Op::IRETURN);
}

TEST(FixPatternSimplifierTest, swapDupX1IandIsubFoldsToM1XorIand)
{
    DomFixture fx("t/F", "m", "()I", 3, 0, [](Body& b) {
        b.op(Op::ICONST_1);
        b.op(Op::ICONST_2);
        b.op(Op::SWAP);
        b.op(Op::DUP_X1);
        b.op(Op::IAND);
        b.op(Op::ISUB);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[2], Op::ICONST_M1);
    EXPECT_EQ(ops[3], Op::IXOR);
    EXPECT_EQ(ops[4], Op::IAND);
    EXPECT_EQ(ops[5], Op::IRETURN);
}

TEST(FixPatternSimplifierTest, dupX1IorSwapIsubFoldsToM1XorIand)
{
    DomFixture fx("t/F", "m", "()I", 3, 0, [](Body& b) {
        b.op(Op::ICONST_1);
        b.op(Op::ICONST_2);
        b.op(Op::DUP_X1);
        b.op(Op::IOR);
        b.op(Op::SWAP);
        b.op(Op::ISUB);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[2], Op::ICONST_M1);
    EXPECT_EQ(ops[3], Op::IXOR);
    EXPECT_EQ(ops[4], Op::IAND);
}

TEST(FixPatternSimplifierTest, dupX1IorIsubInegFoldsToM1XorIand)
{
    DomFixture fx("t/F", "m", "()I", 3, 0, [](Body& b) {
        b.op(Op::ICONST_1);
        b.op(Op::ICONST_2);
        b.op(Op::DUP_X1);
        b.op(Op::IOR);
        b.op(Op::ISUB);
        b.op(Op::INEG);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[2], Op::ICONST_M1);
    EXPECT_EQ(ops[3], Op::IXOR);
    EXPECT_EQ(ops[4], Op::IAND);
}

TEST(FixPatternSimplifierTest, swapIsubIconst1IsubFoldsToIsubM1Xor)
{
    DomFixture fx("t/F", "m", "()I", 2, 0, [](Body& b) {
        b.op(Op::ICONST_5);
        b.op(Op::ICONST_2);
        b.op(Op::SWAP);
        b.op(Op::ISUB);
        b.op(Op::ICONST_1);
        b.op(Op::ISUB);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[0], Op::ICONST_5);
    EXPECT_EQ(ops[1], Op::ICONST_2);
    EXPECT_EQ(ops[2], Op::ISUB);
    EXPECT_EQ(ops[3], Op::ICONST_M1);
    EXPECT_EQ(ops[4], Op::IXOR);
}

TEST(FixPatternSimplifierTest, swapIconstM1IxorIaddFoldsToIsubM1Xor)
{
    DomFixture fx("t/F", "m", "()I", 3, 0, [](Body& b) {
        b.op(Op::ICONST_5);
        b.op(Op::ICONST_2);
        b.op(Op::SWAP);
        b.op(Op::ICONST_M1);
        b.op(Op::IXOR);
        b.op(Op::IADD);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[2], Op::ISUB);
    EXPECT_EQ(ops[3], Op::ICONST_M1);
    EXPECT_EQ(ops[4], Op::IXOR);
}

TEST(FixPatternSimplifierTest, dupX1IconstM1IxorIandIaddToIor)
{
    // 旧栈同款：ICONST_0; ICONST_1; DUP_X1; ICONST_M1; IXOR; IAND; IADD;
    // IRETURN → 4 条，第 3 条为 IOR。
    DomFixture fx("t/F", "m", "()I", 4, 0, [](Body& b) {
        b.op(Op::ICONST_0);
        b.op(Op::ICONST_1);
        b.op(Op::DUP_X1);
        b.op(Op::ICONST_M1);
        b.op(Op::IXOR);
        b.op(Op::IAND);
        b.op(Op::IADD);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ICONST_0);
    EXPECT_EQ(ops[1], Op::ICONST_1);
    EXPECT_EQ(ops[2], Op::IOR);
    EXPECT_EQ(ops[3], Op::IRETURN);
}

// ── x ^ x == 0（规则 8） ──

TEST(FixPatternSimplifierTest, loadDupIxorFoldsToZero)
{
    // ILOAD_0; DUP; IXOR; IRETURN → ICONST_0; IRETURN（装载也在窗口内：
    // x ^ x == 0 与 x 无关）。
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::DUP);
        b.op(Op::IXOR);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_0);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(FixPatternSimplifierTest, ldcDupIxorFoldsToZero)
{
    // LDC 7; DUP; IXOR; IRETURN → ICONST_0; IRETURN（常量装载面同样参与）。
    DomFixture fx("t/F", "m", "()I", 2, 0, [](Body& b) {
        b.ldcInt(7);
        b.op(Op::DUP);
        b.op(Op::IXOR);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_0);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

// ── 死代码清理族（规则 9） ──

TEST(FixPatternSimplifierTest, deadConstPopPairRemoved)
{
    // ICONST_1; POP; RETURN → RETURN
    DomFixture fx("t/F", "m", "()V", 1, 0, [](Body& b) {
        b.op(Op::ICONST_1);
        b.op(Op::POP);
        b.op(Op::RETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(FixPatternSimplifierTest, deadLongPop2PairRemoved)
{
    for (const Op lconst : {Op::LCONST_0, Op::LCONST_1}) {
        DomFixture fx("t/F", "m", "()V", 2, 0, [lconst](Body& b) {
            b.op(lconst);
            b.op(Op::POP2);
            b.op(Op::RETURN);
        });
        EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
        const std::vector<Op> ops = opsOf(fx);
        ASSERT_EQ(ops.size(), 1u);
        EXPECT_EQ(ops[0], Op::RETURN);
    }
}

TEST(FixPatternSimplifierTest, deadWideSequenceRemoved)
{
    // ICONST_1; DUP; IADD; I2L; L2I; POP; RETURN → RETURN
    DomFixture fx("t/F", "m", "()V", 2, 0, [](Body& b) {
        b.op(Op::ICONST_1);
        b.op(Op::DUP);
        b.op(Op::IADD);
        b.op(Op::I2L);
        b.op(Op::L2I);
        b.op(Op::POP);
        b.op(Op::RETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(FixPatternSimplifierTest, deadMulSequenceRemoved)
{
    // ICONST_M1; DUP; IMUL; POP; RETURN → RETURN
    DomFixture fx("t/F", "m", "()V", 2, 0, [](Body& b) {
        b.op(Op::ICONST_M1);
        b.op(Op::DUP);
        b.op(Op::IMUL);
        b.op(Op::POP);
        b.op(Op::RETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(FixPatternSimplifierTest, keepsDiscardedArrayAllocationExceptionSemantics)
{
    // ICONST_1; NEWARRAY T_INT; POP; RETURN——分配仍可能抛 OOM，无规则匹配。
    DomFixture fx("t/F", "m", "()V", 1, 0, [](Body& b) {
        b.op(Op::ICONST_1);
        b.newarray(4);   // T_INT
        b.op(Op::POP);
        b.op(Op::RETURN);
    });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
}

TEST(FixPatternSimplifierTest, keepsStringHashCollisionComparisonUnchanged)
{
    // "Aa"/"BB" hashCode 碰撞比较：不同字符串同 hash，不得改写。
    DomFixture fx("t/F", "m", "()Z", 2, 0, [](Body& b) {
        auto& s    = b.session();
        b.ldcString("Aa");
        b.ldcString("BB");
        b.invoke(Op::INVOKEVIRTUAL, s.internMethodRef("java/lang/String", "hashCode", "()I"));
        b.invoke(Op::INVOKESTATIC,
                 s.internMethodRef("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;"));
        b.op(Op::SWAP);
        b.invoke(Op::INVOKEVIRTUAL, s.internMethodRef("java/lang/String", "hashCode", "()I"));
        b.invoke(Op::INVOKESTATIC,
                 s.internMethodRef("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;"));
        b.invoke(Op::INVOKEVIRTUAL,
                 s.internMethodRef("java/lang/Integer", "equals", "(Ljava/lang/Object;)Z"));
        b.op(Op::IRETURN);
    });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
}

// ── 整数比较常量偏移折叠（规则 10） ──

TEST(FixPatternSimplifierTest, compareFoldEqualityAndZeroOffset)
{
    const std::vector<std::pair<Op, Op>> variants = {
        {Op::IF_ICMPEQ, Op::ICONST_1},   // 等值 c1=1 → 差 1
        {Op::IF_ICMPNE, Op::ICONST_1},
        {Op::IF_ICMPGT, Op::ICONST_2},   // 零偏移大小比较 c1=0 → 差 2
        {Op::IF_ICMPGE, Op::ICONST_2},
        {Op::IF_ICMPLT, Op::ICONST_2},
        {Op::IF_ICMPLE, Op::ICONST_2},
    };
    for (const auto& [branchOp, expectedDiff] : variants) {
        const bool     equality = branchOp == Op::IF_ICMPEQ || branchOp == Op::IF_ICMPNE;
        DomFixture fx("t/F", "m", "(I)I", 2, 1, [&](Body& b) {
            b.iload(0);
            b.op(equality ? Op::ICONST_1 : Op::ICONST_0);
            b.op(Op::IADD);
            b.op(Op::ICONST_2);
            const jbc::ir::LabelId hit = b.label();
            b.branch(branchOp, hit);
            b.op(Op::ICONST_0);
            b.op(Op::IRETURN);
            b.op(Op::ICONST_1);
            b.bindHere(hit);
            b.op(Op::IRETURN);
        });
        EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed())
            << "opcode=" << static_cast<int>(branchOp);
        const std::vector<Op> ops = opsOf(fx);
        // ILOAD_0; <diff>; if_icmp*; ICONST_0; IRETURN; ICONST_1; IRETURN
        ASSERT_EQ(ops.size(), 7u) << "opcode=" << static_cast<int>(branchOp);
        EXPECT_EQ(ops[0], Op::ILOAD) << "opcode=" << static_cast<int>(branchOp);
        EXPECT_EQ(ops[1], expectedDiff) << "opcode=" << static_cast<int>(branchOp);
        EXPECT_EQ(ops[2], branchOp) << "opcode=" << static_cast<int>(branchOp);
        for (const Op o : ops) {
            EXPECT_NE(o, Op::IADD) << "opcode=" << static_cast<int>(branchOp);
        }
    }
}

TEST(FixPatternSimplifierTest, keepsNonZeroOffsetRelationalComparisonOnOverflowRisk)
{
    // ILOAD_0; ICONST_1; IADD; ICONST_2; IF_ICMPGT——非零偏移有符号大小比较，
    // INT_MAX+1 环绕会破坏大小关系，保留原形。
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::ICONST_1);
        b.op(Op::IADD);
        b.op(Op::ICONST_2);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPGT, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
}

TEST(FixPatternSimplifierTest, foldsLdcConstCompareWithBipushDifference)
{
    // ILOAD_0; LDC 100; IADD; LDC 200; IF_ICMPEQ → ILOAD_0; BIPUSH 100; IF_ICMPEQ
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.ldcInt(100);
        b.op(Op::IADD);
        b.ldcInt(200);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::BIPUSH);
    EXPECT_EQ(ops[2], Op::IF_ICMPEQ);
    const jbc::ir::Insn* push = uniqueInsn(fx, Op::BIPUSH);
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->a, 100);
}

TEST(FixPatternSimplifierTest, foldsLdcWConstCompare)
{
    // LDC_W 形态同样参与常量读取（旧栈 tree 把 LDC_W 折叠为 LDC 节点，
    // getIntValue 的 LDC 臂在 classfile 面即覆盖 LDC_W）。
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.ldcWInt(100);
        b.op(Op::IADD);
        b.ldcWInt(200);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[1], Op::BIPUSH);
}

TEST(FixPatternSimplifierTest, foldsCompareWithWraparoundExtremes)
{
    // BIPUSH 127; IADD; BIPUSH -128; IF_ICMPEQ → 差 -255（SIPUSH 域）。
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.bipush(127);
        b.op(Op::IADD);
        b.bipush(-128);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[1], Op::SIPUSH);
    const jbc::ir::Insn* push = uniqueInsn(fx, Op::SIPUSH);
    ASSERT_NE(push, nullptr);
    EXPECT_EQ(push->a, -255);
}

TEST(FixPatternSimplifierTest, keepsNonLiteralComparisonPattern)
{
    // ILOAD_0; ILOAD_1; IADD; ICONST_1; IF_ICMPGT——左非常量，不匹配。
    DomFixture fx("t/F", "m", "(II)I", 2, 2, [](Body& b) {
        b.iload(0);
        b.iload(1);
        b.op(Op::IADD);
        b.op(Op::ICONST_1);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPGT, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
}

TEST(FixPatternSimplifierTest, compareFoldKeepsBranchTarget)
{
    // 折叠后 if_icmp* 的分支边不变：目标仍是原 L 处的 ICONST_1。
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::ICONST_1);
        b.op(Op::IADD);
        b.op(Op::ICONST_2);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 7u);
    const jbc::ir::Insn* jump = uniqueInsn(fx, Op::IF_ICMPEQ);
    ASSERT_NE(jump, nullptr);
    auto target = fx.unit().dom.resolveLabel(jump->target);
    ASSERT_TRUE(target.is_ok());
    // 新序：ILOAD_0(0); ICONST_1(1); IF_ICMPEQ(2); ICONST_0(3); IRETURN(4);
    //      ICONST_1(5); IRETURN(6)——目标为下标 5。
    EXPECT_EQ(indexOfId(fx, target.unwrap().insn), 5);
}

// ── 同 slot store/load 中转消除（规则 11） ──

TEST(FixPatternSimplifierTest, replacesSameSlotStoreLoadCompareWithClonedProducer)
{
    // 旧栈同款：LDC 432559991; ISTORE 5; LDC 153092102; ILOAD 5; IF_ICMPGT
    // → 3 条 LDC、无 ILOAD、IF_ICMPGT 保留、ISTORE 保留。
    DomFixture fx("t/F", "m", "()I", 2, 6, [](Body& b) {
        b.ldcInt(432559991);
        b.istore(5);
        b.ldcInt(153092102);
        b.iload(5);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPGT, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 9u);
    EXPECT_EQ(ops[0], Op::LDC);
    EXPECT_EQ(ops[1], Op::ISTORE);
    EXPECT_EQ(ops[2], Op::LDC);
    EXPECT_EQ(ops[3], Op::LDC);   // reload 位 clone producer
    EXPECT_EQ(ops[4], Op::IF_ICMPGT);
    for (const Op o : ops) {
        EXPECT_NE(o, Op::ILOAD);
    }
}

TEST(FixPatternSimplifierTest, keepsMismatchedStoreLoadCompare)
{
    // ISTORE 5 / ILOAD 6——slot 不匹配，保留原形。
    DomFixture fx("t/F", "m", "()I", 2, 7, [](Body& b) {
        b.ldcInt(432559991);
        b.istore(5);
        b.ldcInt(153092102);
        b.iload(6);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPGT, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
}

TEST(FixPatternSimplifierTest, keepsShortFormStoreLoadCompare)
{
    // iload_3/istore_3 短形态：旧栈 step 谓词只认通用 ISTORE/ILOAD，不匹配。
    DomFixture fx("t/F", "m", "()I", 2, 4, [](Body& b) {
        b.op(Op::ILOAD_3);
        b.op(Op::ISTORE_3);
        b.op(Op::ICONST_1);
        b.op(Op::ILOAD_3);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPGT, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
}

TEST(FixPatternSimplifierTest, sameSlotIloadProducerTerminatesUnchanged)
{
    // ILOAD 1; ISTORE 1; ICONST_2; ILOAD 1; IF_ICMPGT——producer 为同 slot 的
    // 通用 ILOAD 时替换式与窗口同形；旧栈在此形态上无限自匹配（挂死），本实现
    // 显式放弃（终止性守卫，替换为恒等无语义损失）。
    DomFixture fx("t/F", "m", "()I", 2, 2, [](Body& b) {
        b.iload(1);
        b.istore(1);
        b.op(Op::ICONST_2);
        b.iload(1);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPGT, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
}

// ── 标签边界守卫与迁移 ──

TEST(FixPatternSimplifierTest, interiorExceptionBoundaryBlocksFold)
{
    // 异常表 start 锚在窗口内部成员（ICONST_2）上：旧栈 LabelNode 阻断窗口，
    // CodeDom 等价即内部成员标签守卫——整窗跳过。
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::ICONST_1);
        b.op(Op::IADD);
        b.op(Op::ICONST_2);
        const jbc::ir::InsnId atConst = b.last();
        const jbc::ir::LabelId start = b.label();
        const jbc::ir::LabelId hit   = b.label();
        b.branch(Op::IF_ICMPEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        const jbc::ir::InsnId atEnd = b.last();
        const jbc::ir::LabelId end = b.label();
        b.op(Op::ICONST_1);
        const jbc::ir::InsnId atHit = b.last();
        b.op(Op::IRETURN);
        b.bindAt(start, atConst);
        b.bindAt(end, atEnd);
        b.bindAt(hit, atHit);
        b.tryCatch(start, end, hit);
    });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
}

TEST(FixPatternSimplifierTest, branchTargetAtWindowHeadMigratesToReplacement)
{
    // IFEQ 跳窗口头（ICONST_1）：折叠后标签迁移到替换头（新常量）。
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::ICONST_0);
        const jbc::ir::LabelId zero = b.label();
        b.branch(Op::IFEQ, zero);
        b.op(Op::ICONST_5);
        b.op(Op::POP);
        b.op(Op::ICONST_1);
        const jbc::ir::InsnId atHead = b.last();
        b.op(Op::IADD);
        b.op(Op::ICONST_2);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        const jbc::ir::InsnId atHit = b.last();
        b.op(Op::IRETURN);
        b.bindAt(zero, atHead);
        b.bindAt(hit, atHit);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    // ILOAD; ICONST_0; IFEQ; ICONST_5; POP; ICONST_1; IF_ICMPEQ; ICONST_0;
    // IRETURN; ICONST_1; IRETURN
    ASSERT_EQ(ops.size(), 11u);
    EXPECT_EQ(ops[5], Op::ICONST_1);   // 替换头
    EXPECT_EQ(ops[6], Op::IF_ICMPEQ);

    const jbc::ir::Insn* ifeq = uniqueInsn(fx, Op::IFEQ);
    ASSERT_NE(ifeq, nullptr);
    auto headTarget = fx.unit().dom.resolveLabel(ifeq->target);
    ASSERT_TRUE(headTarget.is_ok());
    EXPECT_EQ(indexOfId(fx, headTarget.unwrap().insn), 5);   // 迁移到替换头

    const jbc::ir::Insn* cmp = uniqueInsn(fx, Op::IF_ICMPEQ);
    ASSERT_NE(cmp, nullptr);
    auto cmpTarget = fx.unit().dom.resolveLabel(cmp->target);
    ASSERT_TRUE(cmpTarget.is_ok());
    EXPECT_EQ(indexOfId(fx, cmpTarget.unwrap().insn), 9);    // 分支边保留
}

TEST(FixPatternSimplifierTest, branchTargetAtDeadPairHeadMigratesToSuccessor)
{
    // 空替换（死代码对）+ 首成员分支目标：标签迁移到窗口后继。
    DomFixture fx("t/F", "m", "()V", 1, 0, [](Body& b) {
        const jbc::ir::LabelId zero = b.label();
        b.branch(Op::GOTO, zero);
        b.op(Op::ICONST_1);
        const jbc::ir::InsnId atHead = b.last();
        b.op(Op::POP);
        b.op(Op::RETURN);
        b.bindAt(zero, atHead);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::GOTO);
    EXPECT_EQ(ops[1], Op::RETURN);
    const jbc::ir::Insn* jump = uniqueInsn(fx, Op::GOTO);
    ASSERT_NE(jump, nullptr);
    auto target = fx.unit().dom.resolveLabel(jump->target);
    ASSERT_TRUE(target.is_ok());
    EXPECT_EQ(indexOfId(fx, target.unwrap().insn), 1);   // 后继 RETURN
}

TEST(FixPatternSimplifierTest, lineNumberAtWindowHeadRelocatesToReplacement)
{
    // LNT 条目锚在窗口首成员（ICONST_1）：折叠后经 pcmap 重定向到替换头
    // （替换头继承首成员来源 pc；缺失即 lower LNT miss/漂移）。
    DomFixture fx("t/F", "m", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::ICONST_1);
        b.op(Op::IADD);
        b.op(Op::ICONST_2);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    fx.addLineNumberRow(Op::ICONST_1, 42);
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const std::vector<jbc::core::LineNumberTableAttr::Row> rows = lntRowsOf(fx.writeBack());
    ASSERT_EQ(rows.size(), 1u);
    // 行号必须落在替换头上：lower 后替换头（新序第一条 ICONST_1）的 srcPc
    // 即其新 pc。若无 srcPc 继承，该行会悬垂并被吸附到跳转 clone 的 pc。
    ca::u32 headPc = jbc::ir::kNoPc;
    for (const jbc::ir::InsnId id : fx.unit().dom.order()) {
        const jbc::ir::Insn* in = fx.unit().dom.peek(id);
        if (in != nullptr && in->op == Op::ICONST_1) {
            headPc = in->srcPc;
            break;
        }
    }
    ASSERT_NE(headPc, jbc::ir::kNoPc);
    EXPECT_EQ(rows[0].startPc, headPc);
    EXPECT_EQ(rows[0].line, 42u);
}

// ── 不动点与 PA 纪律 ──

TEST(FixPatternSimplifierTest, secondRunIsNoOp)
{
    DomFixture fx("t/F", "m", "()I", 4, 0, [](Body& b) {
        b.op(Op::ICONST_0);
        b.op(Op::ICONST_1);
        b.op(Op::DUP_X1);
        b.op(Op::ICONST_M1);
        b.op(Op::IXOR);
        b.op(Op::IAND);
        b.op(Op::IADD);
        b.op(Op::IRETURN);
    });
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).changed());
    const Bytes first = fx.writeBack();

    auto session = sessionFromBytes(first);
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(runPassesOn(session, mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(std::move(written).unwrap(), first);
}

TEST(FixPatternSimplifierTest, emptyMethodStaysAllPreserved)
{
    DomFixture fx("t/F", "m", "()V", 0, 0, [](Body& b) { b.op(Op::RETURN); });
    const Bytes before = fx.writeBack();
    EXPECT_TRUE(fx.run(mjbase::pass::FixPatternSimplifier{}).areAllPreserved());
    EXPECT_EQ(fx.writeBack(), before);
}

// ── 注册面 ──

TEST(FixPatternSimplifierTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("FixPatternSimplifier"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("FixPatternSimplifier");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
