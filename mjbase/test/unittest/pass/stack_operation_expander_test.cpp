// StackOperationExpander：常量 producer DUP 族帧面展开——正例（dup/dup2
// 原位消解、X 族整窗重排、iinc 隔距穿针、ldc2_w/字符串/null 元素）+
// 形态外拒绝面（局部/入参来源、分支汇合未知来源、X 族窗内非常量与
// 隔距、异常边界锚定窗内）+ 组合面（展开后简化器再折叠回等价形）+
// 幂等 + 注册面。
//
// 夹具一律 ClassBytesBuilder 手拼可验证栈形（分支/异常表/iinc/ldc2_w
// 面不在 jasm 文本可达范围）；写回产物经生产管线 lower + Strict 帧重算
// 全链验证。窗口语义记账基准与 stack_operation_test 一致：整窗替换，
// 产出 = 窗口前指令 + replacement + 窗口后指令。
#include <gtest/gtest.h>

#include <functional>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "analysis/class_bytes_builder.hpp"
#include "mjbase/pass/stack_operation.hpp"
#include "mjbase/pass/stack_operation_expander.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法夹具装配（CodeEmit 回调式）。
Bytes fixture(std::string_view desc, ca::u16 maxStack, ca::u16 maxLocals, ca::u16 access,
              const std::function<void(mjbase::analysis::test_support::CodeEmit&)>& emit,
              std::vector<mjbase::analysis::test_support::ExcSpec> exc = {})
{
    namespace support = mjbase::analysis::test_support;
    support::ClassBytesBuilder builder("t/S");
    support::MethodSpec spec;
    spec.name      = "m";
    spec.desc      = desc;
    spec.maxStack  = maxStack;
    spec.maxLocals = maxLocals;
    spec.access    = access;
    spec.emit      = emit;
    spec.exceptions = std::move(exc);
    return builder.assemble({spec});
}

/// 无异常表便捷版（public static）。
Bytes fixture(std::string_view desc, ca::u16 maxStack, ca::u16 maxLocals,
              const std::function<void(mjbase::analysis::test_support::CodeEmit&)>& emit)
{
    return fixture(desc, maxStack, maxLocals, 0x0009, emit);
}

std::vector<Op> expectOps(const MethodPassRun& r)
{
    return opsOf(inspect(r.out));
}

/// ICONST 族装载的 int 立即值（夹具只用 iconst 形）。
bool iconstValueOf(const jbc::ir::Insn& in, ca::i32& out)
{
    if (in.op >= Op::ICONST_0 && in.op <= Op::ICONST_5) {
        out = static_cast<ca::i32>(in.op) - static_cast<ca::i32>(Op::ICONST_0);
        return true;
    }
    return false;
}

}   // namespace

// ── 正例：dup 原位消解 ──

TEST(StackOperationExpanderTest, dupAdjacentConstLoadExpanded)
{
    // iconst_5; dup; pop; ireturn —— [iconst_5;iconst_5;pop;ireturn]
    // （邻接基线；pop 消费第二份克隆，夹具栈平衡）。
    const MethodPassRun r = runMethodPass(
        fixture("()I", 2, 1, [](mjbase::analysis::test_support::CodeEmit& e) {
            e.i32const(5);
            e.op(Op::DUP);
            e.op(Op::POP);
            e.op(Op::IRETURN);
        }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                   ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 4u);
    ca::i32 v0 = 0, v1 = 0;
    ASSERT_TRUE(iconstValueOf(*ins.cu->dom.peek(ids[0]), v0));
    ASSERT_TRUE(iconstValueOf(*ins.cu->dom.peek(ids[1]), v1));
    EXPECT_EQ(v0, 5);
    EXPECT_EQ(v1, 5);
    EXPECT_EQ(ins.cu->dom.peek(ids[2])->op, Op::POP);
    EXPECT_EQ(ins.cu->dom.peek(ids[3])->op, Op::IRETURN);
}

TEST(StackOperationExpanderTest, dupExpandedThroughIincSandwich)
{
    // iconst_5; iinc 0,1; dup; pop; ireturn —— dup 顶值来源穿 iinc（纯栈
    // 窗口不可见，帧溯源给到 iconst_5）：dup → iconst_5 克隆；pop 消费
    // 第二份克隆，夹具栈平衡。
    const MethodPassRun r = runMethodPass(
        fixture("()I", 2, 1, [](mjbase::analysis::test_support::CodeEmit& e) {
            e.i32const(5);
            e.op(Op::IINC);
            e.u8(0);
            e.u8(1);
            e.op(Op::DUP);
            e.op(Op::POP);
            e.op(Op::IRETURN);
        }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::ICONST_5);
    EXPECT_EQ(ops[1], Op::IINC);
    EXPECT_EQ(ops[2], Op::ICONST_5);
    EXPECT_EQ(ops[3], Op::POP);
    EXPECT_EQ(ops[4], Op::IRETURN);
}

TEST(StackOperationExpanderTest, nullDupExpanded)
{
    // aconst_null; dup; pop; areturn。
    const MethodPassRun r = runMethodPass(
        fixture("()Ljava/lang/Object;", 2, 1,
                [](mjbase::analysis::test_support::CodeEmit& e) {
                    e.op(Op::ACONST_NULL);
                    e.op(Op::DUP);
                    e.op(Op::POP);
                    e.op(Op::ARETURN);
                }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::ACONST_NULL);
    EXPECT_EQ(ops[2], Op::POP);
    EXPECT_EQ(ops[3], Op::ARETURN);
}

TEST(StackOperationExpanderTest, strDupExpanded)
{
    // ldc "x"; dup; pop; areturn —— 字符串常量克隆（cp 索引随值携带）。
    const MethodPassRun r = runMethodPass(
        fixture("()Ljava/lang/Object;", 2, 1,
                [](mjbase::analysis::test_support::CodeEmit& e) {
                    e.ldcString("x");
                    e.op(Op::DUP);
                    e.op(Op::POP);
                    e.op(Op::ARETURN);
                }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const auto       ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 4u);
    EXPECT_EQ(ldcStringOf(ins, ins.cu->dom.peek(ids[0])), "x");
    EXPECT_EQ(ldcStringOf(ins, ins.cu->dom.peek(ids[1])), "x");
    EXPECT_EQ(ins.cu->dom.peek(ids[2])->op, Op::POP);
    EXPECT_EQ(ins.cu->dom.peek(ids[3])->op, Op::ARETURN);
}

// ── 正例：dup2 两形 ──

TEST(StackOperationExpanderTest, dup2AdjacentLdc2WExpanded)
{
    // ldc2_w K; dup2; pop2; lreturn —— cat2 形邻接基线（简化器装载族
    // 不含 ldc2_w，本件是这一形状的唯一展开面）；pop2 消费第二份克隆。
    const MethodPassRun r = runMethodPass(
        fixture("()J", 4, 1, [](mjbase::analysis::test_support::CodeEmit& e) {
            e.ldcLong(1000000L);
            e.op(Op::DUP2);
            e.op(Op::POP2);
            e.op(Op::LRETURN);
        }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::LDC2_W);
    EXPECT_EQ(ops[1], Op::LDC2_W);
    EXPECT_EQ(ops[2], Op::POP2);
    EXPECT_EQ(ops[3], Op::LRETURN);
}

TEST(StackOperationExpanderTest, dup2Cat2ExpandedThroughIinc)
{
    // ldc2_w K; iinc 0,1; dup2; pop2; lreturn —— cat2 形隔距穿针。
    const MethodPassRun r = runMethodPass(
        fixture("()J", 4, 1, [](mjbase::analysis::test_support::CodeEmit& e) {
            e.ldcLong(1000000L);
            e.op(Op::IINC);
            e.u8(0);
            e.u8(1);
            e.op(Op::DUP2);
            e.op(Op::POP2);
            e.op(Op::LRETURN);
        }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::LDC2_W);
    EXPECT_EQ(ops[1], Op::IINC);
    EXPECT_EQ(ops[2], Op::LDC2_W);
    EXPECT_EQ(ops[3], Op::POP2);
    EXPECT_EQ(ops[4], Op::LRETURN);
}

TEST(StackOperationExpanderTest, dup2TwoCat1ConstsExpanded)
{
    // iconst_1; iinc 0,1; iconst_2; dup2; pop2; pop2; return —— 双 cat1
    // 形：两槽来源各克隆一条，iconst_1 隔 iinc 不可见，帧面给到。
    const MethodPassRun r = runMethodPass(
        fixture("()V", 4, 1, [](mjbase::analysis::test_support::CodeEmit& e) {
            e.i32const(1);
            e.op(Op::IINC);
            e.u8(0);
            e.u8(1);
            e.i32const(2);
            e.op(Op::DUP2);
            e.op(Op::POP2);
            e.op(Op::POP2);
            e.op(Op::RETURN);
        }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 8u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::IINC);
    EXPECT_EQ(ops[2], Op::ICONST_2);
    EXPECT_EQ(ops[3], Op::ICONST_1);
    EXPECT_EQ(ops[4], Op::ICONST_2);
    EXPECT_EQ(ops[5], Op::POP2);
    EXPECT_EQ(ops[6], Op::POP2);
    EXPECT_EQ(ops[7], Op::RETURN);
}

// ── 正例：X 族整窗重排 ──

TEST(StackOperationExpanderTest, dupX1WindowReordered)
{
    // iconst_1; iconst_2; dup_x1; pop2; ireturn —— 输出形 [2,1,2] 整窗
    // 重发，窗内两生产者随之删除。
    const MethodPassRun r = runMethodPass(
        fixture("()I", 3, 1, [](mjbase::analysis::test_support::CodeEmit& e) {
            e.i32const(1);
            e.i32const(2);
            e.op(Op::DUP_X1);
            e.op(Op::POP2);
            e.op(Op::IRETURN);
        }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                   ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 5u);
    ca::i32 v0 = 0, v1 = 0, v2 = 0;
    ASSERT_TRUE(iconstValueOf(*ins.cu->dom.peek(ids[0]), v0));
    ASSERT_TRUE(iconstValueOf(*ins.cu->dom.peek(ids[1]), v1));
    ASSERT_TRUE(iconstValueOf(*ins.cu->dom.peek(ids[2]), v2));
    EXPECT_EQ(v0, 2);
    EXPECT_EQ(v1, 1);
    EXPECT_EQ(v2, 2);
    EXPECT_EQ(ins.cu->dom.peek(ids[3])->op, Op::POP2);
    EXPECT_EQ(ins.cu->dom.peek(ids[4])->op, Op::IRETURN);
}

TEST(StackOperationExpanderTest, dupX2Cat2InsertedReordered)
{
    // ldc2_w K; iconst_3; dup_x2; pop; lstore 1; pop; return —— dup_x2
    // cat2 插入形：输出 [3,K,3] 整窗重发。
    const MethodPassRun r = runMethodPass(
        fixture("()V", 4, 3, [](mjbase::analysis::test_support::CodeEmit& e) {
            e.ldcLong(7L);
            e.i32const(3);
            e.op(Op::DUP_X2);
            e.op(Op::POP);
            e.varInsn(Op::LSTORE, 1);
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[0], Op::ICONST_3);
    EXPECT_EQ(ops[1], Op::LDC2_W);
    EXPECT_EQ(ops[2], Op::ICONST_3);
    EXPECT_EQ(ops[3], Op::POP);
    EXPECT_EQ(ops[4], Op::LSTORE);
    EXPECT_EQ(ops[5], Op::POP);
    EXPECT_EQ(ops[6], Op::RETURN);
}

TEST(StackOperationExpanderTest, dup2X1Cat2DupReordered)
{
    // iconst_1; ldc2_w K; dup2_x1; pop2; pop; lreturn —— dup2_x1 cat2
    // 复制形：输出 [K,1,K] 整窗重发。
    const MethodPassRun r = runMethodPass(
        fixture("()J", 4, 1, [](mjbase::analysis::test_support::CodeEmit& e) {
            e.i32const(1);
            e.ldcLong(7L);
            e.op(Op::DUP2_X1);
            e.op(Op::POP2);
            e.op(Op::POP);
            e.op(Op::LRETURN);
        }),
        mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[0], Op::LDC2_W);
    EXPECT_EQ(ops[1], Op::ICONST_1);
    EXPECT_EQ(ops[2], Op::LDC2_W);
    EXPECT_EQ(ops[3], Op::POP2);
    EXPECT_EQ(ops[4], Op::POP);
    EXPECT_EQ(ops[5], Op::LRETURN);
}

// ── 拒绝面 ──

TEST(StackOperationExpanderTest, dupOnInstanceRefNotExpanded)
{
    // aload_0(this); dup; pop; areturn —— 来源是局部装载非常量，保守
    // 不动、字节保真。
    const Bytes in = fixture("()Ljava/lang/Object;", 2, 1, 0x0001,
                             [](mjbase::analysis::test_support::CodeEmit& e) {
                                 e.op(Op::ALOAD_0);
                                 e.op(Op::DUP);
                                 e.op(Op::POP);
                                 e.op(Op::ARETURN);
                             });
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

TEST(StackOperationExpanderTest, dupOnParameterNotExpanded)
{
    // iload_0; dup; iadd; ireturn —— 来源是入参（Parameter 身份），拒收。
    const Bytes in = fixture("(I)I", 2, 1,
                             [](mjbase::analysis::test_support::CodeEmit& e) {
                                 e.op(Op::ILOAD_0);
                                 e.op(Op::DUP);
                                 e.op(Op::IADD);
                                 e.op(Op::IRETURN);
                             });
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

TEST(StackOperationExpanderTest, dupOnConfluentOriginsNotExpanded)
{
    // 入参条件分流，双路径分别 iconst_1 / iconst_2 汇合后 dup —— 汇合帧
    // 来源退化为未知，拒收（不同生产者同一值的形态也不在面内）。
    const Bytes in = fixture("(I)I", 2, 1,
                             [](mjbase::analysis::test_support::CodeEmit& e) {
                                 const std::size_t l1 = e.newLabel();
                                 const std::size_t l2 = e.newLabel();
                                 e.op(Op::ILOAD_0);
                                 e.branch(Op::IFEQ, l1);
                                 e.i32const(1);
                                 e.branch(Op::GOTO, l2);
                                 e.bindLabel(l1);
                                 e.i32const(2);
                                 e.bindLabel(l2);
                                 e.op(Op::DUP);
                                 e.op(Op::POP);
                                 e.op(Op::IRETURN);
                             });
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

TEST(StackOperationExpanderTest, dupX1NonConstSlotNotExpanded)
{
    // iconst_1; aload_0(this); dup_x1; pop; pop; areturn —— X 族整窗要求
    // 全输入槽常量来源，obj 槽非常量即拒收。
    const Bytes in = fixture("()Ljava/lang/Object;", 3, 1, 0x0001,
                             [](mjbase::analysis::test_support::CodeEmit& e) {
                                 e.i32const(1);
                                 e.op(Op::ALOAD_0);
                                 e.op(Op::DUP_X1);
                                 e.op(Op::POP);
                                 e.op(Op::POP);
                                 e.op(Op::ARETURN);
                             });
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

TEST(StackOperationExpanderTest, dupX1ProducerWindowNotContiguousNotExpanded)
{
    // iconst_1; iinc 0,1; iconst_2; dup_x1; pop2; ireturn —— 两来源都
    // 是常量但 iinc 隔在词法窗内（iinc 不可删、输出重排不可原位表达），
    // 连续性门拒收。dup/dup2 形 A 不受此门（对照 dupExpandedThroughIinc）。
    const Bytes in = fixture("()I", 3, 1,
                             [](mjbase::analysis::test_support::CodeEmit& e) {
                                 e.i32const(1);
                                 e.op(Op::IINC);
                                 e.u8(0);
                                 e.u8(1);
                                 e.i32const(2);
                                 e.op(Op::DUP_X1);
                                 e.op(Op::POP2);
                                 e.op(Op::IRETURN);
                             });
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

TEST(StackOperationExpanderTest, dupX1AnchorInsideWindowNotExpanded)
{
    // aconst_null; aconst_null; dup_x1; pop2; areturn，异常表 handler 锚定
    // 在窗内生产者上：纪律门（首条豁免）整窗拒收，字节保真。异常表面走
    // ClassBytesBuilder 标签装配。
    namespace support = mjbase::analysis::test_support;
    const Bytes in = fixture(
        "()Ljava/lang/Object;", 3, 1, 0x0009,
        [](support::CodeEmit& e) {
            const std::size_t lNull    = e.newLabel();   // 0
            const std::size_t lTry     = e.newLabel();   // 1（窗内生产者）
            const std::size_t lHandler = e.newLabel();   // 2（dup_x1 落点）
            const std::size_t lEnd     = e.newLabel();   // 3
            e.bindLabel(lNull);
            e.op(Op::ACONST_NULL);
            e.bindLabel(lTry);
            e.op(Op::ACONST_NULL);
            e.bindLabel(lHandler);
            e.op(Op::DUP_X1);
            e.bindLabel(lEnd);
            e.op(Op::POP2);
            e.op(Op::ARETURN);
        },
        {{1, 3, 2, std::string_view{}}});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StackOperationExpander{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 组合面 / 幂等 / 注册面 ──

TEST(StackOperationExpanderTest, expandedShapeRefoldsToEquivalentViaSimplifier)
{
    // dup2 双 cat1 隔 iinc 形态：展开器 → 简化器再折叠回等价形
    // （净栈效应同为零，残留 [iconst_1; iinc; pop] 面为简化器窗口语义）。
    const Bytes in = fixture("()V", 4, 1, [](mjbase::analysis::test_support::CodeEmit& e) {
        e.i32const(1);
        e.op(Op::IINC);
        e.u8(0);
        e.u8(1);
        e.i32const(2);
        e.op(Op::DUP2);
        e.op(Op::POP2);
        e.op(Op::POP2);
        e.op(Op::RETURN);
    });
    const MethodPassRun expanded =
        runMethodPass(in, mjbase::pass::StackOperationExpander{});
    EXPECT_TRUE(expanded.pa.changed());

    const MethodPassRun refolded = runMethodPass(expanded.out,
                                                 mjbase::pass::StackOperationSimplifier{});
    EXPECT_TRUE(refolded.pa.changed());
    const auto ops = expectOps(refolded);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::IINC);
    EXPECT_EQ(ops[2], Op::POP);
    EXPECT_EQ(ops[3], Op::RETURN);
}

TEST(StackOperationExpanderTest, secondRunIsIdempotentByteIdentical)
{
    // 展开产物上再跑一轮：无可展开 DUP，字节保真。
    const Bytes in = fixture("()I", 2, 1, [](mjbase::analysis::test_support::CodeEmit& e) {
        e.i32const(5);
        e.op(Op::IINC);
        e.u8(0);
        e.u8(1);
        e.op(Op::DUP);
        e.op(Op::POP);
        e.op(Op::IRETURN);
    });
    const MethodPassRun first  = runMethodPass(in, mjbase::pass::StackOperationExpander{});
    EXPECT_TRUE(first.pa.changed());
    const MethodPassRun second = runMethodPass(first.out, mjbase::pass::StackOperationExpander{});
    EXPECT_TRUE(second.pa.areAllPreserved());
    EXPECT_EQ(second.out, first.out);
}

TEST(StackOperationExpanderTest, registeredAsStackOperationExpander)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("StackOperationExpander"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("StackOperationExpander");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
