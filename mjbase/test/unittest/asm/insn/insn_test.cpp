/// @file        insn_test.cpp
/// @brief       asm/insn 模块测试：分类谓词全枚举覆盖、栈效应与 FrameEngine
///        对拍、typed 工厂校验。
///
/// 全部用例基于纯 jbc::ir::CodeDom/CodeUnit 构造（无需 classfile）；
/// FrameEngine 对拍以 jbc::analysis（domain.hpp 的槽位迁移）为 oracle。
/// 构建模式：先落跳转目标指令与锚定标签，再 insertBefore 把分支插到
/// 程序位（连续 insertBefore(end, x) 的插入序即程序序）。
#include <gtest/gtest.h>
#include <cstdio>

#include <cmath>
#include <vector>

#include "mjbase/asm/insn/insn_factory.hpp"
#include "mjbase/asm/insn/insn_op.hpp"

#include "jbc/analysis/frame.hpp"
#include "jbc/core/bytes.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

namespace {

using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_insn::ArrayType;
using mjbase::asm_insn::BuildError;
using mjbase::asm_insn::BuildResult;
using mjbase::asm_insn::kStackDependent;
using mjbase::asm_insn::OpKind;
using mjbase::asm_insn::StackEffect;

/// Op 枚举连续覆盖 0x00..0xC9（JSR_W）；全枚举遍历按字节值进行。
constexpr ca::u32 kMaxOpByte = 0xc9;

Op opAt(ca::u32 v) { return static_cast<Op>(static_cast<ca::u8>(v)); }

/// 工厂结果解包（失败时先记一条 EXPECT 再给安全值）。
Insn mustBuild(const BuildResult& r)
{
    EXPECT_TRUE(r.is_ok());
    return r.is_ok() ? r.unwrap() : Insn{};
}

InsnId appendOk(CodeDom& d, Insn i)
{
    auto r = d.append(std::move(i));
    EXPECT_TRUE(r.is_ok());
    return r.is_ok() ? r.unwrap() : jbc::ir::kNoInsn;
}

InsnId insertBeforeOk(CodeDom& d, InsnId at, Insn i)
{
    auto r = d.insertBefore(at, std::move(i));
    EXPECT_TRUE(r.is_ok());
    return r.is_ok() ? r.unwrap() : jbc::ir::kNoInsn;
}

LabelId labelAt(CodeDom& d, InsnId anchor)
{
    auto r = d.createLabel(anchor);
    EXPECT_TRUE(r.is_ok());
    return r.is_ok() ? r.unwrap() : jbc::ir::kNoLabel;
}

/// 构造单条指令（对拍 sweep 用，绕开工厂族籍约束）。
Insn rawOf(Op op)
{
    Insn i;
    i.op = op;
    return i;
}

/// order 中句柄的位置（找不到返回 size）。
ca::usize posOf(const CodeDom& d, InsnId id)
{
    const auto& ord = d.order();
    for (ca::usize i = 0; i < ord.size(); ++i) {
        if (ord[i] == id) {
            return i;
        }
    }
    return ord.size();
}

}   // namespace

// ---------------------------------------------------------------------------
// 分类谓词：全枚举覆盖（不漏）+ 互斥（不重）+ 谓词↔主分类一致
// ---------------------------------------------------------------------------

TEST(MjAsmInsnTest, ClassifyCoversEveryOpcodeAndEveryKind)
{
    ca::u32 seen = 0;
    for (ca::u32 v = 0; v <= kMaxOpByte; ++v) {
        const OpKind k = mjbase::asm_insn::classify(opAt(v));
        ASSERT_LT(k, OpKind::Count) << "opcode 0x" << std::hex << v;
        seen |= 1u << static_cast<ca::u32>(k);
    }
    // 每个分类至少有一个成员：谓词族无空集。
    const ca::u32 all = (1u << static_cast<ca::u32>(OpKind::Count)) - 1u;
    EXPECT_EQ(seen, all);
    // 枚举外的字节值（0xCA 起的保留码）显式返回哨兵。
    EXPECT_EQ(mjbase::asm_insn::classify(static_cast<Op>(0xCA)), OpKind::Count);
}

TEST(MjAsmInsnTest, PredicatesAgreeWithClassification)
{
    for (ca::u32 v = 0; v <= kMaxOpByte; ++v) {
        const Op op    = opAt(v);
        const OpKind k = mjbase::asm_insn::classify(op);
        EXPECT_EQ(mjbase::asm_insn::isConst(op), k == OpKind::Const) << v;
        EXPECT_EQ(mjbase::asm_insn::isLoad(op), k == OpKind::LocalLoad) << v;
        EXPECT_EQ(mjbase::asm_insn::isStore(op), k == OpKind::LocalStore) << v;
        EXPECT_EQ(mjbase::asm_insn::isLocalAccess(op),
                  k == OpKind::LocalLoad || k == OpKind::LocalStore ||
                      k == OpKind::LocalInc || k == OpKind::Ret)
            << v;
        EXPECT_EQ(mjbase::asm_insn::isArrayLoad(op), k == OpKind::ArrayLoad) << v;
        EXPECT_EQ(mjbase::asm_insn::isArrayStore(op), k == OpKind::ArrayStore) << v;
        EXPECT_EQ(mjbase::asm_insn::isArray(op),
                  k == OpKind::ArrayLoad || k == OpKind::ArrayStore ||
                      k == OpKind::ArrayLength)
            << v;
        EXPECT_EQ(mjbase::asm_insn::isStackOp(op), k == OpKind::Stack) << v;
        EXPECT_EQ(mjbase::asm_insn::isArithmetic(op), k == OpKind::Arithmetic) << v;
        EXPECT_EQ(mjbase::asm_insn::isConversion(op), k == OpKind::Conversion) << v;
        EXPECT_EQ(mjbase::asm_insn::isCompare(op), k == OpKind::Compare) << v;
        EXPECT_EQ(mjbase::asm_insn::isBranch(op), k == OpKind::Branch) << v;
        EXPECT_EQ(mjbase::asm_insn::isSwitch(op), k == OpKind::Switch) << v;
        EXPECT_EQ(mjbase::asm_insn::isReturn(op), k == OpKind::Return) << v;
        EXPECT_EQ(mjbase::asm_insn::isInvoke(op), k == OpKind::Invoke) << v;
        EXPECT_EQ(mjbase::asm_insn::isFieldAccess(op), k == OpKind::FieldAccess) << v;
        EXPECT_EQ(mjbase::asm_insn::isThrow(op), k == OpKind::Throw) << v;
        EXPECT_EQ(mjbase::asm_insn::isMonitor(op), k == OpKind::Monitor) << v;
        EXPECT_EQ(mjbase::asm_insn::isGoto(op), op == Op::GOTO || op == Op::GOTO_W) << v;
        EXPECT_EQ(mjbase::asm_insn::isIfNull(op), op == Op::IFNULL || op == Op::IFNONNULL)
            << v;
        const bool condExpect = k == OpKind::Branch && op != Op::GOTO &&
                                op != Op::GOTO_W && op != Op::JSR && op != Op::JSR_W;
        EXPECT_EQ(mjbase::asm_insn::isConditionalBranch(op), condExpect) << v;
        // 主分类与形状底座的一致性：分支/开关谓词 ⟺ 操作数形状。
        const jbc::core::Shape sh = jbc::core::shapeOf(op);
        EXPECT_EQ(mjbase::asm_insn::isBranch(op),
                  sh == jbc::core::Shape::Branch2 || sh == jbc::core::Shape::Branch4)
            << v;
        EXPECT_EQ(mjbase::asm_insn::isSwitch(op),
                  sh == jbc::core::Shape::TableSwitch ||
                      sh == jbc::core::Shape::LookupSwitch)
            << v;
    }
}

/// 旧 InsnUtil 分类断言的行为移植（含旧实现缺漏的 LDC_W/LDC2_W/GOTO_W 补齐）。
TEST(MjAsmInsnTest, ClassificationMatchesLegacyInsnUtilSemantics)
{
    // isReturn：IRETURN..RETURN 全真；RET 不算 return（jsr/ret 遗留自成一族）。
    for (ca::u32 v = 0xac; v <= 0xb1; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isReturn(opAt(v))) << v;
    }
    EXPECT_FALSE(mjbase::asm_insn::isReturn(Op::RET));
    // isConditionalBranch（旧 isIf）：IFEQ..IF_ACMPNE + IFNULL/IFNONNULL。
    for (ca::u32 v = 0x99; v <= 0xa6; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isConditionalBranch(opAt(v))) << v;
    }
    EXPECT_TRUE(mjbase::asm_insn::isConditionalBranch(Op::IFNULL));
    EXPECT_TRUE(mjbase::asm_insn::isConditionalBranch(Op::IFNONNULL));
    EXPECT_FALSE(mjbase::asm_insn::isConditionalBranch(Op::GOTO));
    // isIfNull。
    EXPECT_TRUE(mjbase::asm_insn::isIfNull(Op::IFNULL));
    EXPECT_TRUE(mjbase::asm_insn::isIfNull(Op::IFNONNULL));
    EXPECT_FALSE(mjbase::asm_insn::isIfNull(Op::IFEQ));
    // isGoto：含 GOTO_W（新枚举补齐）；JSR 不是 goto。
    EXPECT_TRUE(mjbase::asm_insn::isGoto(Op::GOTO));
    EXPECT_TRUE(mjbase::asm_insn::isGoto(Op::GOTO_W));
    EXPECT_FALSE(mjbase::asm_insn::isGoto(Op::JSR));
    EXPECT_FALSE(mjbase::asm_insn::isGoto(Op::IFNE));
    // isSwitch。
    EXPECT_TRUE(mjbase::asm_insn::isSwitch(Op::TABLESWITCH));
    EXPECT_TRUE(mjbase::asm_insn::isSwitch(Op::LOOKUPSWITCH));
    // isLoad/isStore：显式 + 紧凑形态，两族互斥。
    for (ca::u32 v = 0x15; v <= 0x2d; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isLoad(opAt(v))) << v;
        EXPECT_FALSE(mjbase::asm_insn::isStore(opAt(v))) << v;
    }
    for (ca::u32 v = 0x36; v <= 0x4e; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isStore(opAt(v))) << v;
        EXPECT_FALSE(mjbase::asm_insn::isLoad(opAt(v))) << v;
    }
    // isArrayLoad/isArrayStore：IALOAD..SALOAD / IASTORE..SASTORE。
    for (ca::u32 v = 0x2e; v <= 0x35; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isArrayLoad(opAt(v))) << v;
        EXPECT_TRUE(mjbase::asm_insn::isArray(opAt(v))) << v;
    }
    for (ca::u32 v = 0x4f; v <= 0x56; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isArrayStore(opAt(v))) << v;
        EXPECT_TRUE(mjbase::asm_insn::isArray(opAt(v))) << v;
    }
    EXPECT_TRUE(mjbase::asm_insn::isArray(Op::ARRAYLENGTH));
    // isConst：ACONST_NULL..DCONST_1 + BIPUSH/SIPUSH + ldc 族
    //（旧实现漏 LDC_W/LDC2_W——被注释掉的行，本实现补齐）。
    for (ca::u32 v = 0x01; v <= 0x14; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isConst(opAt(v))) << v;
    }
    EXPECT_FALSE(mjbase::asm_insn::isConst(Op::ILOAD));
    EXPECT_FALSE(mjbase::asm_insn::isConst(Op::NOP));
    // isInvoke / isFieldAccess。
    for (ca::u32 v = 0xb6; v <= 0xba; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isInvoke(opAt(v))) << v;
    }
    for (ca::u32 v = 0xb2; v <= 0xb5; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isFieldAccess(opAt(v))) << v;
    }
    // isThrow / isMonitor / isStackOp / isArithmetic / isConversion / isCompare。
    EXPECT_TRUE(mjbase::asm_insn::isThrow(Op::ATHROW));
    EXPECT_TRUE(mjbase::asm_insn::isMonitor(Op::MONITORENTER));
    EXPECT_TRUE(mjbase::asm_insn::isMonitor(Op::MONITOREXIT));
    EXPECT_TRUE(mjbase::asm_insn::isStackOp(Op::POP));
    EXPECT_TRUE(mjbase::asm_insn::isStackOp(Op::SWAP));
    EXPECT_FALSE(mjbase::asm_insn::isStackOp(Op::NOP));
    for (ca::u32 v = 0x60; v <= 0x83; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isArithmetic(opAt(v))) << v;
    }
    EXPECT_FALSE(mjbase::asm_insn::isArithmetic(Op::IINC));
    EXPECT_FALSE(mjbase::asm_insn::isArithmetic(Op::LCMP));
    for (ca::u32 v = 0x85; v <= 0x93; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isConversion(opAt(v))) << v;
    }
    for (ca::u32 v = 0x94; v <= 0x98; ++v) {
        EXPECT_TRUE(mjbase::asm_insn::isCompare(opAt(v))) << v;
    }
}

/// 旧 InsnUtilTest::invertIfRoundTripsAllConditionalOpcodes 的移植。
TEST(MjAsmInsnTest, InvertIfRoundTripsAllConditionalOpcodes)
{
    const Op cond[] = {Op::IFEQ,      Op::IFNE,       Op::IFLT,      Op::IFGE,
                       Op::IFGT,      Op::IFLE,       Op::IF_ICMPEQ, Op::IF_ICMPNE,
                       Op::IF_ICMPLT, Op::IF_ICMPGE,  Op::IF_ICMPGT, Op::IF_ICMPLE,
                       Op::IF_ACMPEQ, Op::IF_ACMPNE,  Op::IFNULL,    Op::IFNONNULL};
    for (Op op : cond) {
        EXPECT_EQ(mjbase::asm_insn::invertIf(mjbase::asm_insn::invertIf(op)), op);
    }
    EXPECT_EQ(mjbase::asm_insn::invertIf(Op::IFEQ), Op::IFNE);
    EXPECT_EQ(mjbase::asm_insn::invertIf(Op::IF_ICMPLT), Op::IF_ICMPGE);
    EXPECT_EQ(mjbase::asm_insn::invertIf(Op::IFNULL), Op::IFNONNULL);
    // 非条件跳转按契约原样返回。
    EXPECT_EQ(mjbase::asm_insn::invertIf(Op::GOTO), Op::GOTO);
    EXPECT_EQ(mjbase::asm_insn::invertIf(Op::IRETURN), Op::IRETURN);
    EXPECT_EQ(mjbase::asm_insn::invertIf(Op::NOP), Op::NOP);
}

TEST(MjAsmInsnTest, TerminatorAndPureComputation)
{
    // 终结指令：return/goto/goto_w/switch/throw/if*/ret（ret 为新纳入，
    // 与 FrameEngine 的控制流终结名单对齐）。
    EXPECT_TRUE(mjbase::asm_insn::isTerminator(Op::RETURN));
    EXPECT_TRUE(mjbase::asm_insn::isTerminator(Op::IRETURN));
    EXPECT_TRUE(mjbase::asm_insn::isTerminator(Op::GOTO));
    EXPECT_TRUE(mjbase::asm_insn::isTerminator(Op::GOTO_W));
    EXPECT_TRUE(mjbase::asm_insn::isTerminator(Op::LOOKUPSWITCH));
    EXPECT_TRUE(mjbase::asm_insn::isTerminator(Op::ATHROW));
    EXPECT_TRUE(mjbase::asm_insn::isTerminator(Op::IFEQ));
    EXPECT_TRUE(mjbase::asm_insn::isTerminator(Op::IFNONNULL));
    EXPECT_TRUE(mjbase::asm_insn::isTerminator(Op::RET));
    EXPECT_FALSE(mjbase::asm_insn::isTerminator(Op::NOP));
    EXPECT_FALSE(mjbase::asm_insn::isTerminator(Op::ISTORE));
    EXPECT_FALSE(mjbase::asm_insn::isTerminator(Op::IADD));
    EXPECT_FALSE(mjbase::asm_insn::isTerminator(Op::GETSTATIC));
    // 纯计算：常量/算术/转换/栈操作（旧语义；不含 compare）。
    EXPECT_TRUE(mjbase::asm_insn::isPureComputation(Op::ICONST_1));
    EXPECT_TRUE(mjbase::asm_insn::isPureComputation(Op::BIPUSH));
    EXPECT_TRUE(mjbase::asm_insn::isPureComputation(Op::IADD));
    EXPECT_TRUE(mjbase::asm_insn::isPureComputation(Op::I2L));
    EXPECT_TRUE(mjbase::asm_insn::isPureComputation(Op::DUP_X2));
    EXPECT_FALSE(mjbase::asm_insn::isPureComputation(Op::LCMP));   // 旧语义不含 compare
    EXPECT_FALSE(mjbase::asm_insn::isPureComputation(Op::ILOAD));
    EXPECT_FALSE(mjbase::asm_insn::isPureComputation(Op::IFEQ));
    EXPECT_FALSE(mjbase::asm_insn::isPureComputation(Op::GETFIELD));
    EXPECT_FALSE(mjbase::asm_insn::isPureComputation(Op::INVOKESTATIC));
    EXPECT_FALSE(mjbase::asm_insn::isPureComputation(Op::ATHROW));
}

// ---------------------------------------------------------------------------
// 栈效应静态表
// ---------------------------------------------------------------------------

TEST(MjAsmInsnTest, StackEffectKnownOps)
{
    const auto eff = [](Op op) { return mjbase::asm_insn::stackEffectOf(op); };
    // 常量与装载。
    EXPECT_EQ(eff(Op::ACONST_NULL), (StackEffect{0, 1}));
    EXPECT_EQ(eff(Op::LCONST_0), (StackEffect{0, 2}));
    EXPECT_EQ(eff(Op::DCONST_1), (StackEffect{0, 2}));
    EXPECT_EQ(eff(Op::BIPUSH), (StackEffect{0, 1}));
    EXPECT_EQ(eff(Op::LDC), (StackEffect{0, 1}));
    EXPECT_EQ(eff(Op::LDC_W), (StackEffect{0, 1}));
    EXPECT_EQ(eff(Op::LDC2_W), (StackEffect{0, 2}));
    EXPECT_EQ(eff(Op::LLOAD), (StackEffect{0, 2}));
    EXPECT_EQ(eff(Op::DLOAD_3), (StackEffect{0, 2}));
    EXPECT_EQ(eff(Op::ALOAD_0), (StackEffect{0, 1}));
    EXPECT_EQ(eff(Op::LSTORE), (StackEffect{2, 0}));
    EXPECT_EQ(eff(Op::ISTORE_2), (StackEffect{1, 0}));
    // 数组载荷：cat2 值加倍。
    EXPECT_EQ(eff(Op::IALOAD), (StackEffect{2, 1}));
    EXPECT_EQ(eff(Op::LALOAD), (StackEffect{2, 2}));
    EXPECT_EQ(eff(Op::DALOAD), (StackEffect{2, 2}));
    EXPECT_EQ(eff(Op::IASTORE), (StackEffect{3, 0}));
    EXPECT_EQ(eff(Op::LASTORE), (StackEffect{4, 0}));
    EXPECT_EQ(eff(Op::DASTORE), (StackEffect{4, 0}));
    // 栈操作：dup 族净新增。
    EXPECT_EQ(eff(Op::POP), (StackEffect{1, 0}));
    EXPECT_EQ(eff(Op::POP2), (StackEffect{2, 0}));
    EXPECT_EQ(eff(Op::DUP), (StackEffect{0, 1}));
    EXPECT_EQ(eff(Op::DUP2), (StackEffect{0, 2}));
    EXPECT_EQ(eff(Op::DUP_X2), (StackEffect{0, 1}));
    EXPECT_EQ(eff(Op::SWAP), (StackEffect{0, 0}));
    // 算术：cat2 加倍；long 移位量是 int。
    EXPECT_EQ(eff(Op::IADD), (StackEffect{2, 1}));
    EXPECT_EQ(eff(Op::LADD), (StackEffect{4, 2}));
    EXPECT_EQ(eff(Op::DADD), (StackEffect{4, 2}));
    EXPECT_EQ(eff(Op::INEG), (StackEffect{1, 1}));
    EXPECT_EQ(eff(Op::LNEG), (StackEffect{2, 2}));
    EXPECT_EQ(eff(Op::LSHL), (StackEffect{3, 2}));
    EXPECT_EQ(eff(Op::LUSHR), (StackEffect{3, 2}));
    EXPECT_EQ(eff(Op::LAND), (StackEffect{4, 2}));
    EXPECT_EQ(eff(Op::IINC), (StackEffect{0, 0}));
    // 转换与比较。
    EXPECT_EQ(eff(Op::I2L), (StackEffect{1, 2}));
    EXPECT_EQ(eff(Op::L2I), (StackEffect{2, 1}));
    EXPECT_EQ(eff(Op::L2D), (StackEffect{2, 2}));
    EXPECT_EQ(eff(Op::F2D), (StackEffect{1, 2}));
    EXPECT_EQ(eff(Op::I2B), (StackEffect{1, 1}));
    EXPECT_EQ(eff(Op::LCMP), (StackEffect{4, 1}));
    EXPECT_EQ(eff(Op::FCMPG), (StackEffect{2, 1}));
    EXPECT_EQ(eff(Op::DCMPL), (StackEffect{4, 1}));
    // 分支与终结。
    EXPECT_EQ(eff(Op::IFEQ), (StackEffect{1, 0}));
    EXPECT_EQ(eff(Op::IFNULL), (StackEffect{1, 0}));
    EXPECT_EQ(eff(Op::IF_ICMPEQ), (StackEffect{2, 0}));
    EXPECT_EQ(eff(Op::IF_ACMPNE), (StackEffect{2, 0}));
    EXPECT_EQ(eff(Op::GOTO), (StackEffect{0, 0}));
    EXPECT_EQ(eff(Op::JSR), (StackEffect{0, 1}));   // 压 returnAddress
    EXPECT_EQ(eff(Op::GOTO_W), (StackEffect{0, 0}));
    EXPECT_EQ(eff(Op::JSR_W), (StackEffect{0, 1}));
    EXPECT_EQ(eff(Op::RET), (StackEffect{0, 0}));
    EXPECT_EQ(eff(Op::TABLESWITCH), (StackEffect{1, 0}));
    EXPECT_EQ(eff(Op::LOOKUPSWITCH), (StackEffect{1, 0}));
    EXPECT_EQ(eff(Op::IRETURN), (StackEffect{1, 0}));
    EXPECT_EQ(eff(Op::LRETURN), (StackEffect{2, 0}));
    EXPECT_EQ(eff(Op::DRETURN), (StackEffect{2, 0}));
    EXPECT_EQ(eff(Op::RETURN), (StackEffect{0, 0}));
    // 对象族。
    EXPECT_EQ(eff(Op::NEW), (StackEffect{0, 1}));
    EXPECT_EQ(eff(Op::NEWARRAY), (StackEffect{1, 1}));
    EXPECT_EQ(eff(Op::ANEWARRAY), (StackEffect{1, 1}));
    EXPECT_EQ(eff(Op::MULTIANEWARRAY), (StackEffect{kStackDependent, 1}));
    EXPECT_EQ(eff(Op::ARRAYLENGTH), (StackEffect{1, 1}));
    EXPECT_EQ(eff(Op::ATHROW), (StackEffect{1, 0}));
    EXPECT_EQ(eff(Op::CHECKCAST), (StackEffect{1, 1}));
    EXPECT_EQ(eff(Op::INSTANCEOF), (StackEffect{1, 1}));
    EXPECT_EQ(eff(Op::MONITORENTER), (StackEffect{1, 0}));
    EXPECT_EQ(eff(Op::MONITOREXIT), (StackEffect{1, 0}));
    // 净效应与最小深度。
    EXPECT_EQ(mjbase::asm_insn::netStackEffectOf(Op::IADD), -1);
    EXPECT_EQ(mjbase::asm_insn::netStackEffectOf(Op::DUP), 1);
    EXPECT_EQ(mjbase::asm_insn::netStackEffectOf(Op::DUP2), 2);
    EXPECT_EQ(mjbase::asm_insn::netStackEffectOf(Op::GETSTATIC), kStackDependent);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::DUP_X2), 3);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::DUP2_X2), 4);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::SWAP), 2);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::POP2), 2);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::IF_ICMPEQ), 2);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::LASTORE), 4);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::LSHL), 3);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::GETFIELD), 1);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::GETSTATIC), 0);
    EXPECT_EQ(mjbase::asm_insn::minStackDepthOf(Op::INVOKEVIRTUAL), kStackDependent);
}

/// 静态不可知的字段集合必须精确：描述符依赖只出现在成员访问族与 multianewarray。
TEST(MjAsmInsnTest, DependentEffectSetIsExact)
{
    for (ca::u32 v = 0; v <= kMaxOpByte; ++v) {
        const Op op         = opAt(v);
        const StackEffect e = mjbase::asm_insn::stackEffectOf(op);
        const bool popDep   = e.pop == kStackDependent;
        const bool pushDep  = e.push == kStackDependent;
        const OpKind k      = mjbase::asm_insn::classify(op);
        bool wantPopDep     = false;
        bool wantPushDep    = false;
        switch (k) {
        case OpKind::FieldAccess:
            wantPopDep  = op == Op::PUTSTATIC || op == Op::PUTFIELD;
            wantPushDep = op == Op::GETSTATIC || op == Op::GETFIELD;
            break;
        case OpKind::Invoke:
            wantPopDep  = true;
            wantPushDep = true;
            break;
        case OpKind::NewArray:
            wantPopDep  = op == Op::MULTIANEWARRAY;
            wantPushDep = false;
            break;
        default: break;
        }
        EXPECT_EQ(popDep, wantPopDep) << "op 0x" << std::hex << v;
        EXPECT_EQ(pushDep, wantPushDep) << "op 0x" << std::hex << v;
    }
}

TEST(MjAsmInsnTest, FieldAndInvokeStackEffectsFollowDescriptors)
{
    using mjbase::asm_insn::fieldStackEffect;
    using mjbase::asm_insn::invokeStackEffect;
    using mjbase::asm_insn::StackError;

    // 字段族：cat2 字段 2 槽；getfield/putfield 额外弹 1 个对象引用。
    EXPECT_EQ(fieldStackEffect(Op::GETSTATIC, std::string_view{"I"}).unwrap(),
              (StackEffect{0, 1}));
    EXPECT_EQ(fieldStackEffect(Op::GETSTATIC, std::string_view{"J"}).unwrap(),
              (StackEffect{0, 2}));
    EXPECT_EQ(fieldStackEffect(Op::GETSTATIC, std::string_view{"Ljava/lang/String;"}).unwrap(),
              (StackEffect{0, 1}));
    EXPECT_EQ(fieldStackEffect(Op::GETFIELD, std::string_view{"D"}).unwrap(),
              (StackEffect{1, 2}));
    EXPECT_EQ(fieldStackEffect(Op::PUTSTATIC, std::string_view{"J"}).unwrap(),
              (StackEffect{2, 0}));
    EXPECT_EQ(fieldStackEffect(Op::PUTFIELD, std::string_view{"I"}).unwrap(),
              (StackEffect{2, 0}));
    EXPECT_EQ(fieldStackEffect(Op::PUTFIELD, std::string_view{"[Ljava/lang/Object;"}).unwrap(),
              (StackEffect{2, 0}));
    // 错误路径：族外 op / 非法描述符。
    EXPECT_TRUE(fieldStackEffect(Op::INVOKESTATIC, std::string_view{"I"}).is_err());
    EXPECT_TRUE(fieldStackEffect(Op::GETSTATIC, std::string_view{"X"}).is_err());
    EXPECT_TRUE(fieldStackEffect(Op::GETSTATIC, std::string_view{""}).is_err());
    EXPECT_TRUE(fieldStackEffect(Op::GETSTATIC, std::string_view{"V"}).is_err());

    // invoke 族：参数 + 接收者（static/indy 无）；返回值 void=0。
    EXPECT_EQ(invokeStackEffect(Op::INVOKESTATIC, std::string_view{"()V"}).unwrap(),
              (StackEffect{0, 0}));
    EXPECT_EQ(invokeStackEffect(Op::INVOKESTATIC, std::string_view{"()J"}).unwrap(),
              (StackEffect{0, 2}));
    EXPECT_EQ(invokeStackEffect(Op::INVOKEVIRTUAL, std::string_view{"()I"}).unwrap(),
              (StackEffect{1, 1}));
    EXPECT_EQ(invokeStackEffect(Op::INVOKESPECIAL, std::string_view{"(I)V"}).unwrap(),
              (StackEffect{2, 0}));
    EXPECT_EQ(
        invokeStackEffect(Op::INVOKEVIRTUAL, std::string_view{"(JLjava/lang/String;I)D"}).unwrap(),
        (StackEffect{5, 2}));   // 2+1+1 参数槽 + 接收者；double 返回 2 槽
    EXPECT_EQ(invokeStackEffect(Op::INVOKEDYNAMIC, std::string_view{"(I)V"}).unwrap(),
              (StackEffect{1, 0}));   // indy 无接收者
    EXPECT_EQ(invokeStackEffect(Op::INVOKEINTERFACE, std::string_view{"([I[D)V"}).unwrap(),
              (StackEffect{3, 0}));   // 2 个数组引用参数槽 + 接收者
    // 错误路径 + 错误码可判别。
    const auto notInvoke = invokeStackEffect(Op::GETSTATIC, std::string_view{"()V"});
    ASSERT_TRUE(notInvoke.is_err());
    EXPECT_EQ(notInvoke.unwrap_err(), StackError::NotInvokeOp);
    EXPECT_TRUE(invokeStackEffect(Op::INVOKESTATIC, std::string_view{"(I"}).is_err());
    EXPECT_TRUE(invokeStackEffect(Op::INVOKESTATIC, std::string_view{"I"}).is_err());
    const auto badDesc = invokeStackEffect(Op::INVOKESTATIC, std::string_view{"(Q)V"});
    ASSERT_TRUE(badDesc.is_err());
    EXPECT_EQ(badDesc.unwrap_err(), StackError::BadDescriptor);
}

// ---------------------------------------------------------------------------
// 栈效应 × FrameEngine 对拍（oracle = jbc::analysis 的槽位迁移）
// ---------------------------------------------------------------------------

/// 线性样本：进入每条指令的栈深度 == 静态表逐条累加值。
/// 程序：iconst_1; i2d; dconst_1; dcmpl; ifeq L; lconst_1; lconst_0; ladd;
///       l2i; istore_1; L: return
TEST(MjAsmInsnTest, StackEffectsAgreeWithFrameEngineLinear)
{
    CodeUnit u;
    // 构建顺序：先落 return（兼作 IFEQ 目标），再按程序序逐条 insertBefore。
    const InsnId end   = appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::RETURN)));
    const LabelId lend = labelAt(u.dom, end);
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::simpleInsn(Op::ICONST_1)));
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::simpleInsn(Op::I2D)));
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::simpleInsn(Op::DCONST_1)));
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::simpleInsn(Op::DCMPL)));
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::branchInsn(Op::IFEQ, lend)));
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::simpleInsn(Op::LCONST_1)));
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::simpleInsn(Op::LCONST_0)));
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::simpleInsn(Op::LADD)));
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::simpleInsn(Op::L2I)));
    insertBeforeOk(u.dom, end, mustBuild(mjbase::asm_insn::simpleInsn(Op::ISTORE_1)));
    ASSERT_EQ(u.dom.size(), 11u);

    jbc::core::ConstantPool cp;   // 样本不含 CP 引用
    jbc::analysis::FrameEngine fe(cp);
    std::vector<jbc::analysis::FrameAt> frames;
    const auto sum = fe.compute(u, frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);
    ASSERT_EQ(frames.size(), u.dom.size());

    // 期望深度由静态表累加；另给两个硬编码锚点防表错位。
    ca::i32 depth  = 0;
    const auto& ord = u.dom.order();
    for (ca::usize i = 0; i < ord.size(); ++i) {
        const Insn* in = u.dom.peek(ord[i]);
        ASSERT_NE(in, nullptr);
        ASSERT_TRUE(frames[i].reachable);
        EXPECT_EQ(frames[i].stack.size(), static_cast<ca::usize>(depth))
            << "insn " << i << " " << jbc::core::mnemonicOf(in->op);
        const auto se = mjbase::asm_insn::stackEffectOf(in->op);
        ASSERT_NE(se.pop, kStackDependent);
        ASSERT_NE(se.push, kStackDependent);
        depth += se.push - se.pop;
    }
    EXPECT_EQ(frames[2].stack.size(), 2u);   // i2d 后：int→double 占 2 槽
    EXPECT_EQ(frames[8].stack.size(), 2u);   // ladd 后：两 long 折叠为一个
}

/// CP 依赖族样本：getstatic/getfield/ldc/ldc2_w/invokestatic 的深度由
/// fieldStackEffect / invokeStackEffect 的值逐条累加。
TEST(MjAsmInsnTest, StackEffectsAgreeWithFrameEngineMemberSample)
{
    jbc::core::ConstantPool cp;
    const ca::u32 cls  = cp.addClass(cp.addUtf8("p/T"));
    const ca::u32 fldJ = cp.addRef(
        jbc::core::CpTag::Fieldref, cls,
        cp.addNameAndType(cp.addUtf8("f"), cp.addUtf8("J")));
    const ca::u32 mthD = cp.addRef(
        jbc::core::CpTag::Methodref, cls,
        cp.addNameAndType(cp.addUtf8("m"), cp.addUtf8("(D)D")));
    const ca::u32 intC = cp.addInteger(9);
    const ca::u32 lngC = cp.addLong(7);

    CodeUnit u;
    appendOk(u.dom, mustBuild(mjbase::asm_insn::fieldInsn(Op::GETSTATIC, fldJ)));     // 0→2
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::LSTORE_1)));           // 2→0
    appendOk(u.dom, mustBuild(mjbase::asm_insn::cpInsn(Op::LDC, intC)));              // 0→1
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::POP)));                // 1→0
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::ACONST_NULL)));        // 0→1
    appendOk(u.dom, mustBuild(mjbase::asm_insn::fieldInsn(Op::GETFIELD, fldJ)));      // 1→3
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::L2D)));                // 3→3
    appendOk(u.dom, mustBuild(mjbase::asm_insn::invokeInsn(Op::INVOKESTATIC, mthD)));   // 3→3
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::DSTORE_2)));           // 3→0
    appendOk(u.dom, mustBuild(mjbase::asm_insn::cpInsn(Op::LDC2_W, lngC)));           // 0→2
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::POP2)));               // 2→0
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::RETURN)));             // 0

    jbc::analysis::FrameEngine fe(cp);
    std::vector<jbc::analysis::FrameAt> frames;
    const auto sum = fe.compute(u, frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);
    ASSERT_EQ(frames.size(), u.dom.size());

    const auto descOf = [&](ca::u32 idx) -> std::string_view {
        if (idx == fldJ) {
            return "J";
        }
        if (idx == mthD) {
            return "(D)D";
        }
        return "";
    };
    ca::i32 depth   = 0;
    const auto& ord = u.dom.order();
    for (ca::usize i = 0; i < ord.size(); ++i) {
        const Insn* in = u.dom.peek(ord[i]);
        ASSERT_NE(in, nullptr);
        ASSERT_TRUE(frames[i].reachable);
        EXPECT_EQ(frames[i].stack.size(), static_cast<ca::usize>(depth))
            << "insn " << i << " " << jbc::core::mnemonicOf(in->op);
        const StackEffect e = mjbase::asm_insn::stackEffectOf(in->op);
        ca::i32 net         = 0;
        if (e.pop == kStackDependent || e.push == kStackDependent) {
            if (mjbase::asm_insn::isFieldAccess(in->op)) {
                const auto r = mjbase::asm_insn::fieldStackEffect(in->op, descOf(in->cp));
                ASSERT_TRUE(r.is_ok());
                net = r.unwrap().push - r.unwrap().pop;
            } else {
                const auto r = mjbase::asm_insn::invokeStackEffect(in->op, descOf(in->cp));
                ASSERT_TRUE(r.is_ok());
                net = r.unwrap().push - r.unwrap().pop;
            }
        } else {
            net = e.push - e.pop;
        }
        depth += net;
    }
    EXPECT_EQ(frames[6].stack.size(), 2u);   // getfield J：入口 1，弹 ref 压 long → 2 槽
}

/// 全枚举 sweep：minStackDepthOf 精确等于 FrameEngine 不下溢的最少预备槽数。
/// 非控制流的固定效应 op 逐个验证（差一槽必 Underflow，备足必 ok）。
TEST(MjAsmInsnTest, MinDepthUnderflowBoundaryMatchesFrameEngine)
{
    jbc::core::ConstantPool cp;
    jbc::analysis::FrameEngine fe(cp);
    const auto runSummary = [&](const std::vector<Op>& program) {
        CodeUnit u;
        for (Op op : program) {
            appendOk(u.dom, rawOf(op));
        }
        return fe.compute(u);
    };

    for (ca::u32 v = 0; v <= kMaxOpByte; ++v) {
        const Op op    = opAt(v);
        const OpKind k = mjbase::asm_insn::classify(op);
        // 控制流族需要标签结构，另测；WIDE 是前缀占位；
        // ldc 族宽度静态可知但 FrameEngine 需查 CP tag，由 member 样本覆盖。
        if (k == OpKind::Branch || k == OpKind::Switch || k == OpKind::Ret ||
            k == OpKind::Return || k == OpKind::Throw || k == OpKind::Wide ||
            op == Op::LDC || op == Op::LDC_W || op == Op::LDC2_W) {
            continue;
        }
        const StackEffect e = mjbase::asm_insn::stackEffectOf(op);
        if (e.pop == kStackDependent || e.push == kStackDependent) {
            // 依赖集必须精确 = 字段访问族 / invoke 族 / multianewarray。
            const bool expectedDependent = k == OpKind::FieldAccess || k == OpKind::Invoke ||
                                           op == Op::MULTIANEWARRAY;
            EXPECT_TRUE(expectedDependent) << "op 0x" << std::hex << v;
            continue;
        }
        const ca::i32 need = mjbase::asm_insn::minStackDepthOf(op);
        ASSERT_GE(need, 0) << "op 0x" << std::hex << v;

        // 备足槽数 → 数据流 ok。
        std::vector<Op> ok;
        ok.reserve(static_cast<ca::usize>(need) + 2);
        for (ca::i32 i = 0; i < need; ++i) {
            ok.push_back(Op::ACONST_NULL);
        }
        ok.push_back(op);
        ok.push_back(Op::RETURN);
        const auto sum = runSummary(ok);
        EXPECT_TRUE(sum.ok) << "op 0x" << std::hex << v << " ("
                            << jbc::core::mnemonicOf(op) << ")";

        // 差一槽 → 必在 op 处 Underflow。
        if (need > 0) {
            std::vector<Op> bad(static_cast<ca::usize>(need - 1), Op::ACONST_NULL);
            bad.push_back(op);
            bad.push_back(Op::RETURN);
            const auto sumBad = runSummary(bad);
            EXPECT_FALSE(sumBad.ok) << "op 0x" << std::hex << v;
            EXPECT_EQ(sumBad.fail, jbc::analysis::FrameSummary::Fail::Underflow)
                << "op 0x" << std::hex << v << " (" << jbc::core::mnemonicOf(op) << ")";
        }
    }
}

/// 控制流族的效应对拍：if 家族弹 1/2、goto 断落 fallthrough、jsr 向两路
/// 后继压 returnAddress、switch 弹选择子、athrow/return 族的弹出数。
TEST(MjAsmInsnTest, ControlOpEffectsMatchFrameEngine)
{
    jbc::core::ConstantPool cp;
    const auto framesOf = [&](CodeUnit& u, std::vector<jbc::analysis::FrameAt>& frames) {
        jbc::analysis::FrameEngine fe(cp);
        const auto sum = fe.compute(u, frames);
        ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);
        ASSERT_EQ(frames.size(), u.dom.size());
    };

    // ifeq：[iconst_1, ifeq L, nop, L: return]——nop（fallthrough）入口深度 0。
    {
        CodeUnit u;
        appendOk(u.dom, rawOf(Op::ICONST_1));
        const InsnId nop = appendOk(u.dom, rawOf(Op::NOP));
        const InsnId ret = appendOk(u.dom, rawOf(Op::RETURN));
        const LabelId l  = labelAt(u.dom, ret);
        insertBeforeOk(u.dom, nop, mustBuild(mjbase::asm_insn::branchInsn(Op::IFEQ, l)));

        std::vector<jbc::analysis::FrameAt> frames;
        framesOf(u, frames);
        EXPECT_EQ(frames[2].stack.size(), 0u);   // ifeq 弹掉了 int
    }
    // if_icmpeq：弹 2。
    {
        CodeUnit u;
        appendOk(u.dom, rawOf(Op::ICONST_1));
        appendOk(u.dom, rawOf(Op::ICONST_2));
        const InsnId nop = appendOk(u.dom, rawOf(Op::NOP));
        const InsnId ret = appendOk(u.dom, rawOf(Op::RETURN));
        const LabelId l  = labelAt(u.dom, ret);
        insertBeforeOk(u.dom, nop,
                       mustBuild(mjbase::asm_insn::branchInsn(Op::IF_ICMPEQ, l)));

        std::vector<jbc::analysis::FrameAt> frames;
        framesOf(u, frames);
        EXPECT_EQ(frames[3].stack.size(), 0u);
    }
    // goto：顺序后继不可达。
    {
        CodeUnit u;
        const InsnId nop = appendOk(u.dom, rawOf(Op::NOP));
        const InsnId ret = appendOk(u.dom, rawOf(Op::RETURN));
        const LabelId l  = labelAt(u.dom, ret);
        insertBeforeOk(u.dom, nop, mustBuild(mjbase::asm_insn::branchInsn(Op::GOTO, l)));

        std::vector<jbc::analysis::FrameAt> frames;
        framesOf(u, frames);
        EXPECT_FALSE(frames[1].reachable);   // goto 后的 nop 是死代码
    }
    // jsr/ret：jsr 向两个后继（目标与 fallthrough）传播 1 槽 returnAddress。
    {
        CodeUnit u;
        const InsnId nop    = appendOk(u.dom, rawOf(Op::NOP));
        const InsnId astore = appendOk(u.dom, rawOf(Op::ASTORE_1));
        appendOk(u.dom, mustBuild(mjbase::asm_insn::localInsn(Op::RET, 1)));
        const LabelId l = labelAt(u.dom, astore);
        insertBeforeOk(u.dom, nop, mustBuild(mjbase::asm_insn::branchInsn(Op::JSR, l)));

        std::vector<jbc::analysis::FrameAt> frames;
        framesOf(u, frames);
        EXPECT_EQ(frames[1].stack.size(), 1u);   // nop（fallthrough）入口
        EXPECT_EQ(frames[2].stack.size(), 1u);   // astore（jsr 目标）入口
    }
    // tableswitch：弹 1 个选择子；case 与 default 目标入口深度 0（目标用
    // nop/return——ireturn 在 0 深度会再弹 1，样本自身不合法）。
    {
        CodeUnit u;
        appendOk(u.dom, rawOf(Op::ICONST_1));
        const InsnId a   = appendOk(u.dom, rawOf(Op::NOP));
        const InsnId b   = appendOk(u.dom, rawOf(Op::NOP));
        const InsnId end = appendOk(u.dom, rawOf(Op::RETURN));
        const LabelId la = labelAt(u.dom, a);
        const LabelId lb = labelAt(u.dom, b);
        const LabelId ld = labelAt(u.dom, end);
        insertBeforeOk(u.dom, a,
                       mustBuild(mjbase::asm_insn::tableSwitchInsn(ld, 0, 1, {la, lb})));

        std::vector<jbc::analysis::FrameAt> frames;
        framesOf(u, frames);
        EXPECT_EQ(frames[2].stack.size(), 0u);   // case a
        EXPECT_EQ(frames[3].stack.size(), 0u);   // case b
        EXPECT_EQ(frames[4].stack.size(), 0u);   // default
    }
    // lookupswitch：弹 1 个选择子。
    {
        CodeUnit u;
        appendOk(u.dom, rawOf(Op::ICONST_1));
        const InsnId t    = appendOk(u.dom, rawOf(Op::NOP));
        const InsnId end  = appendOk(u.dom, rawOf(Op::RETURN));
        const LabelId lt  = labelAt(u.dom, t);
        const LabelId ld  = labelAt(u.dom, end);
        insertBeforeOk(u.dom, t,
                       mustBuild(mjbase::asm_insn::lookupSwitchInsn(ld, {1, 5}, {lt, lt})));

        std::vector<jbc::analysis::FrameAt> frames;
        framesOf(u, frames);
        EXPECT_EQ(frames[2].stack.size(), 0u);
    }
    // athrow：1 槽 ok / 0 槽 Underflow。
    {
        CodeUnit u;
        appendOk(u.dom, rawOf(Op::ACONST_NULL));
        appendOk(u.dom, rawOf(Op::ATHROW));
        appendOk(u.dom, rawOf(Op::RETURN));
        jbc::analysis::FrameEngine fe(cp);
        EXPECT_TRUE(fe.compute(u).ok);
    }
    {
        CodeUnit u;
        appendOk(u.dom, rawOf(Op::ATHROW));
        appendOk(u.dom, rawOf(Op::RETURN));
        jbc::analysis::FrameEngine fe(cp);
        const auto sum = fe.compute(u);
        EXPECT_FALSE(sum.ok);
        EXPECT_EQ(sum.fail, jbc::analysis::FrameSummary::Fail::Underflow);
    }
    // lreturn 弹 2：1 槽不够 → Underflow；2 槽 ok。return 弹 0。
    {
        CodeUnit u;
        appendOk(u.dom, rawOf(Op::ICONST_1));
        appendOk(u.dom, rawOf(Op::LRETURN));
        appendOk(u.dom, rawOf(Op::RETURN));
        jbc::analysis::FrameEngine fe(cp);
        EXPECT_EQ(fe.compute(u).fail, jbc::analysis::FrameSummary::Fail::Underflow);
    }
    {
        CodeUnit u;
        appendOk(u.dom, rawOf(Op::LCONST_0));
        appendOk(u.dom, rawOf(Op::LRETURN));
        appendOk(u.dom, rawOf(Op::RETURN));
        jbc::analysis::FrameEngine fe(cp);
        EXPECT_TRUE(fe.compute(u).ok);
    }
    {
        CodeUnit u;
        appendOk(u.dom, rawOf(Op::RETURN));
        jbc::analysis::FrameEngine fe(cp);
        EXPECT_TRUE(fe.compute(u).ok);
    }
}

// ---------------------------------------------------------------------------
// typed 工厂：合法构造 + 族籍/值域拒绝
// ---------------------------------------------------------------------------

TEST(MjAsmInsnTest, FactoriesBuildValidInsns)
{
    using namespace mjbase::asm_insn;
    // simple：无操作数族。
    {
        const Insn i = mustBuild(simpleInsn(Op::IADD));
        EXPECT_EQ(i.op, Op::IADD);
        EXPECT_EQ(i.a, 0);
        EXPECT_EQ(i.cp, 0u);
    }
    ASSERT_TRUE(simpleInsn(Op::ILOAD_0).is_ok());   // 紧凑 load 是合法 None 形状
    ASSERT_TRUE(simpleInsn(Op::SWAP).is_ok());
    // local：Local1 族。
    {
        const Insn i = mustBuild(localInsn(Op::ALOAD, 12));
        EXPECT_EQ(i.op, Op::ALOAD);
        EXPECT_EQ(i.a, 12);
    }
    ASSERT_TRUE(localInsn(Op::RET, 3).is_ok());
    // iinc。
    {
        const Insn i = mustBuild(iincInsn(7, -3));
        EXPECT_EQ(i.op, Op::IINC);
        EXPECT_EQ(i.a, 7);
        EXPECT_EQ(i.b, -3);
    }
    // 分支族（含宽形态）；产物可入库（句柄校验由 CodeDom 负责）。
    CodeDom d;
    const InsnId anchor = appendOk(d, rawOf(Op::RETURN));
    const LabelId l     = labelAt(d, anchor);
    {
        const Insn i = mustBuild(branchInsn(Op::IF_ICMPGT, l));
        EXPECT_EQ(i.op, Op::IF_ICMPGT);
        EXPECT_EQ(i.target, l);
    }
    const InsnId gw = appendOk(d, mustBuild(branchInsn(Op::GOTO_W, l)));
    EXPECT_NE(gw, jbc::ir::kNoInsn);
    // CP 族。
    {
        const Insn i = mustBuild(invokeInsn(Op::INVOKEINTERFACE, 0x1234));
        EXPECT_EQ(i.op, Op::INVOKEINTERFACE);
        EXPECT_EQ(i.cp, 0x1234u);
    }
    {
        const Insn i = mustBuild(fieldInsn(Op::PUTFIELD, 20));
        EXPECT_EQ(i.op, Op::PUTFIELD);
        EXPECT_EQ(i.cp, 20u);
    }
    {
        const Insn i = mustBuild(classInsn(Op::NEW, 1));
        EXPECT_EQ(i.op, Op::NEW);
        EXPECT_EQ(i.cp, 1u);
    }
    ASSERT_TRUE(cpInsn(Op::LDC_W, 0x1234).is_ok());
    ASSERT_TRUE(cpInsn(Op::LDC2_W, 2).is_ok());
    // newarray 全部 atype。
    for (ca::u8 t = 4; t <= 11; ++t) {
        const Insn i = mustBuild(newArrayInsn(static_cast<ArrayType>(t)));
        EXPECT_EQ(i.op, Op::NEWARRAY);
        EXPECT_EQ(i.a, static_cast<ca::i32>(t));
    }
    // multianewarray。
    {
        const Insn i = mustBuild(multiANewArrayInsn(9, 3));
        EXPECT_EQ(i.op, Op::MULTIANEWARRAY);
        EXPECT_EQ(i.cp, 9u);
        EXPECT_EQ(i.a, 3);
    }
    // switch。
    {
        const Insn i = mustBuild(tableSwitchInsn(l, -2, 1, {l, l, l, l}));
        EXPECT_EQ(i.op, Op::TABLESWITCH);
        EXPECT_EQ(i.a, -2);
        EXPECT_EQ(i.b, 1);
        EXPECT_EQ(i.target, l);
        EXPECT_EQ(i.targets.size(), 4u);
    }
    {
        const Insn i = mustBuild(lookupSwitchInsn(l, {-5, 0, 7}, {l, l, l}));
        EXPECT_EQ(i.op, Op::LOOKUPSWITCH);
        EXPECT_EQ(i.keys.size(), 3u);
        EXPECT_EQ(i.targets.size(), 3u);
        EXPECT_EQ(i.target, l);
    }
    // 取反分支。
    {
        const Insn src = mustBuild(branchInsn(Op::IFGE, l));
        const Insn inv = mustBuild(invertedBranch(src));
        EXPECT_EQ(inv.op, Op::IFLT);
        EXPECT_EQ(inv.target, l);
    }
}

TEST(MjAsmInsnTest, FactoriesRejectFamilyAndDomainViolations)
{
    using namespace mjbase::asm_insn;
    const auto err = [](const BuildResult& r, BuildError want) {
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), want);
    };
    // simple：带操作数形状 / wide 前缀一律拒绝。
    err(simpleInsn(Op::ILOAD), BuildError::ShapeMismatch);
    err(simpleInsn(Op::IINC), BuildError::ShapeMismatch);
    err(simpleInsn(Op::IFEQ), BuildError::ShapeMismatch);
    err(simpleInsn(Op::GETSTATIC), BuildError::ShapeMismatch);
    err(simpleInsn(Op::WIDE), BuildError::ShapeMismatch);
    // local：非 Local1 族 / 槽号超界。
    err(localInsn(Op::ILOAD_0, 1), BuildError::ShapeMismatch);   // 紧凑形态无操作数
    err(localInsn(Op::ISTORE, 0x10000), BuildError::BadOperand);
    // iinc：增量超 i16 / 槽号超界。
    err(iincInsn(0, 32768), BuildError::BadOperand);
    err(iincInsn(0, -32769), BuildError::BadOperand);
    err(iincInsn(0x10000, 1), BuildError::BadOperand);
    // 分支：非分支形状 / 空标签。
    err(branchInsn(Op::TABLESWITCH, jbc::ir::kNoLabel), BuildError::ShapeMismatch);
    err(branchInsn(Op::RETURN, jbc::ir::kNoLabel), BuildError::ShapeMismatch);
    err(branchInsn(Op::NOP, jbc::ir::kNoLabel), BuildError::ShapeMismatch);
    CodeDom d;
    const InsnId anchor = appendOk(d, rawOf(Op::RETURN));
    const LabelId l     = labelAt(d, anchor);
    err(branchInsn(Op::GOTO, jbc::ir::kNoLabel), BuildError::BadOperand);
    // CP：非 CP 形状 / 索引值域 / LDC 的 u1 域。
    err(cpInsn(Op::NOP, 1), BuildError::ShapeMismatch);
    err(cpInsn(Op::IADD, 1), BuildError::ShapeMismatch);
    err(cpInsn(Op::LDC, 0), BuildError::BadOperand);
    err(cpInsn(Op::NEW, 0x10000), BuildError::BadOperand);
    err(cpInsn(Op::LDC, 0x100), BuildError::BadOperand);   // 0x100 须走 LDC_W
    ASSERT_TRUE(cpInsn(Op::LDC, 0xFF).is_ok());
    // 窄族工厂的族籍校验（族外邻位 op 必拒）。
    err(invokeInsn(Op::GETSTATIC, 1), BuildError::ShapeMismatch);
    err(invokeInsn(Op::NEW, 1), BuildError::ShapeMismatch);   // invokedynamic 之后
    err(fieldInsn(Op::INVOKESTATIC, 1), BuildError::ShapeMismatch);
    err(fieldInsn(Op::INVOKEVIRTUAL, 1), BuildError::ShapeMismatch);   // putfield 之后
    err(classInsn(Op::GOTO, 1), BuildError::ShapeMismatch);
    err(classInsn(Op::MULTIANEWARRAY, 1),
        BuildError::ShapeMismatch);   // 走 multiANewArrayInsn
    // multianewarray 维数域。
    err(multiANewArrayInsn(1, 0), BuildError::BadOperand);
    err(multiANewArrayInsn(1, 256), BuildError::BadOperand);
    // tableswitch：数量 / 顺序 / 空标签。
    err(tableSwitchInsn(jbc::ir::kNoLabel, 0, 1, {l, l}), BuildError::BadOperand);
    err(tableSwitchInsn(l, 2, 1, {l}), BuildError::CountMismatch);   // high < low
    err(tableSwitchInsn(l, 0, 2, {l, l}), BuildError::CountMismatch);
    err(tableSwitchInsn(l, 0, 1, {l, jbc::ir::kNoLabel}), BuildError::BadOperand);
    // lookupswitch：等长 / 严格递增 / 空标签。
    err(lookupSwitchInsn(jbc::ir::kNoLabel, {1}, {l}), BuildError::BadOperand);
    err(lookupSwitchInsn(l, {1, 2}, {l}), BuildError::CountMismatch);
    err(lookupSwitchInsn(l, {2, 1}, {l, l}), BuildError::CountMismatch);
    err(lookupSwitchInsn(l, {1, 1}, {l, l}), BuildError::CountMismatch);   // 重复键
    err(lookupSwitchInsn(l, {1, 2}, {l, jbc::ir::kNoLabel}), BuildError::BadOperand);
    // 取反分支只收条件跳转。
    err(invertedBranch(mustBuild(branchInsn(Op::GOTO, l))), BuildError::ShapeMismatch);
    err(invertedBranch(rawOf(Op::IADD)), BuildError::ShapeMismatch);
}

/// 工厂产物可入库、可编码、可再导入（无 classfile 的 E2E）。
TEST(MjAsmInsnTest, FactoryOutputEncodesAndReimports)
{
    CodeUnit u;
    // 程序：iconst_1; tableswitch(0..1){a,b} def=d; a: iconst_2; ireturn;
    //       b: iconst_3; ireturn; d: return
    // append 按程序序落块；switch 在 a 块头之前插入（case 标签先有锚即可）。
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::ICONST_1)));
    const InsnId a = appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::ICONST_2)));
    const LabelId la = labelAt(u.dom, a);
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::IRETURN)));
    const InsnId b = appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::ICONST_3)));
    const LabelId lb = labelAt(u.dom, b);
    appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::IRETURN)));
    const InsnId dend = appendOk(u.dom, mustBuild(mjbase::asm_insn::simpleInsn(Op::RETURN)));
    const LabelId ld  = labelAt(u.dom, dend);
    insertBeforeOk(u.dom, a,
                   mustBuild(mjbase::asm_insn::tableSwitchInsn(ld, 0, 1, {la, lb})));
    ASSERT_EQ(u.dom.size(), 7u);

    // 编码 → 再导入 → 结构一致。
    auto bytes = u.dom.encode();
    ASSERT_TRUE(bytes.is_ok());
    auto imported = CodeDom::importFrom(jbc::core::ByteView{bytes.unwrap()});
    ASSERT_TRUE(imported.is_ok());
    CodeDom d2 = std::move(imported).unwrap();   // CodeDom move-only，move 出 Result
    ASSERT_TRUE(d2.roundtripEquals(jbc::core::ByteView{bytes.unwrap()}));
    ASSERT_EQ(d2.size(), 7u);

    // 指令序列一致。
    for (ca::usize i = 0; i < d2.size(); ++i) {
        const Insn* x = u.dom.peek(u.dom.order()[i]);
        const Insn* y = d2.peek(d2.order()[i]);
        ASSERT_NE(x, nullptr);
        ASSERT_NE(y, nullptr);
        EXPECT_EQ(x->op, y->op) << "insn " << i;
    }
    // switch 结构与目标边界一致。
    const Insn* s1 = u.dom.peek(u.dom.order()[1]);
    const Insn* s2 = d2.peek(d2.order()[1]);
    ASSERT_EQ(s1->op, Op::TABLESWITCH);
    ASSERT_EQ(s2->op, Op::TABLESWITCH);
    EXPECT_EQ(s2->a, s1->a);
    EXPECT_EQ(s2->b, s1->b);
    ASSERT_EQ(s2->targets.size(), s1->targets.size());
    const auto resolvedPos = [](const CodeDom& d, LabelId label) -> ca::usize {
        auto t = d.resolveLabel(label);
        if (t.is_err() || t.unwrap().isEnd()) {
            return d.size() + 1;
        }
        return posOf(d, t.unwrap().insn);
    };
    EXPECT_EQ(resolvedPos(d2, s2->target), resolvedPos(u.dom, s1->target));
    EXPECT_EQ(resolvedPos(d2, s2->targets[0]), resolvedPos(u.dom, s1->targets[0]));
    EXPECT_EQ(resolvedPos(d2, s2->targets[1]), resolvedPos(u.dom, s1->targets[1]));
    // 期望布局：default→末条 return；case0→a(2)；case1→b(4)。
    EXPECT_EQ(resolvedPos(u.dom, s1->target), 6u);
    EXPECT_EQ(resolvedPos(u.dom, s1->targets[0]), 2u);
    EXPECT_EQ(resolvedPos(u.dom, s1->targets[1]), 4u);
}

/// 常量工厂的短形态选择（旧 InsnUtil::get*Insn 的行为断言移植；
/// 超范围值旧实现回落 ldc，新实现返回 Err——需池索引由调用方 intern）。
TEST(MjAsmInsnTest, ConstFactoriesPickShortestForm)
{
    using namespace mjbase::asm_insn;
    // int：[-1,5] iconst / [-128,127] bipush / [-32768,32767] sipush。
    EXPECT_EQ(mustBuild(intConstInsn(-1)).op, Op::ICONST_M1);
    EXPECT_EQ(mustBuild(intConstInsn(0)).op, Op::ICONST_0);
    EXPECT_EQ(mustBuild(intConstInsn(5)).op, Op::ICONST_5);
    EXPECT_EQ(mustBuild(intConstInsn(6)).op, Op::BIPUSH);
    EXPECT_EQ(mustBuild(intConstInsn(6)).a, 6);
    EXPECT_EQ(mustBuild(intConstInsn(-128)).op, Op::BIPUSH);
    EXPECT_EQ(mustBuild(intConstInsn(-128)).a, -128);
    EXPECT_EQ(mustBuild(intConstInsn(127)).op, Op::BIPUSH);
    EXPECT_EQ(mustBuild(intConstInsn(128)).op, Op::SIPUSH);
    EXPECT_EQ(mustBuild(intConstInsn(128)).a, 128);
    EXPECT_EQ(mustBuild(intConstInsn(-32768)).op, Op::SIPUSH);
    EXPECT_EQ(mustBuild(intConstInsn(32767)).op, Op::SIPUSH);
    EXPECT_EQ(mustBuild(intConstInsn(32767)).a, 32767);
    ASSERT_TRUE(intConstInsn(32768).is_err());
    ASSERT_TRUE(intConstInsn(-32769).is_err());
    // long：0/1。
    EXPECT_EQ(mustBuild(longConstInsn(0)).op, Op::LCONST_0);
    EXPECT_EQ(mustBuild(longConstInsn(1)).op, Op::LCONST_1);
    ASSERT_TRUE(longConstInsn(2).is_err());
    ASSERT_TRUE(longConstInsn(-1).is_err());
    // float：+0.0/1.0/2.0；-0.0f 与 NaN 不取短形态。
    EXPECT_EQ(mustBuild(floatConstInsn(0.0f)).op, Op::FCONST_0);
    EXPECT_EQ(mustBuild(floatConstInsn(1.0f)).op, Op::FCONST_1);
    EXPECT_EQ(mustBuild(floatConstInsn(2.0f)).op, Op::FCONST_2);
    ASSERT_TRUE(floatConstInsn(-0.0f).is_err());
    ASSERT_TRUE(floatConstInsn(3.0f).is_err());
    ASSERT_TRUE(floatConstInsn(std::nanf("")).is_err());
    // double：+0.0/1.0。
    EXPECT_EQ(mustBuild(doubleConstInsn(0.0)).op, Op::DCONST_0);
    EXPECT_EQ(mustBuild(doubleConstInsn(1.0)).op, Op::DCONST_1);
    ASSERT_TRUE(doubleConstInsn(2.0).is_err());
    ASSERT_TRUE(doubleConstInsn(-0.0).is_err());
    ASSERT_TRUE(doubleConstInsn(std::nan("")).is_err());
}
