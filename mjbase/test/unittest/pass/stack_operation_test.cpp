// StackOperationSimplifier：窗口折叠规则面 + 不动点收敛 + 边界保守停 + 注册面。
//
// 窗口语义记账基准：窗口 [S1..Sn] → replacement R，产出 = 窗口前指令 + R +
// 窗口后指令（整窗替换，窗口内指令不保留）；消除规则的 R 为空。
// 夹具一律为可验证字节码：jasm 文本汇编（JvmJasmService，汇编器 frame
// 分析本身拒收不可验证栈形——pop2/swap 落点深度不足、返回时栈剩余都会
// 被打回）；异常表面走 ClassBytesBuilder 标签装配（start/end/handler 为
// label id，jasm 文本面不载异常表）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "analysis/class_bytes_builder.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/pass/stack_operation.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// jasm 文本汇编为 class 字节（汇编失败即测试失败）；desc 缺省 ()V，
/// 留值夹具用 ()I / ()J / ()Ljava/lang/Object; 收口返回类型。
Bytes assembleFixture(std::string_view instructions, std::string_view desc = "()V")
{
    mjbase::jasm::JvmJasmService service;
    const std::string source =
        std::string(".super java/lang/Object\n.class public super t/S {\n"
                    "  .method public m ")
        + std::string(desc) + " {\n    code: {\n" + std::string(instructions)
        + "    }\n  }\n}\n";
    const auto sourceName = ca::str::Utf16String::from_utf8_string(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>("t/S.jasm"), 8));
    const auto sourceText = ca::str::Utf16String::from_utf8_string(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(source.data()),
                                          source.size()));
    const auto result = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败";
        return {};
    }
    return result.value->bytes;
}

/// 写回产物里按序取操作码，断言快捷。
std::vector<Op> expectOps(const MethodPassRun& r)
{
    return opsOf(inspect(r.out));
}

/// 发射装载的 int 立即值（ICONST_0..5 / BIPUSH / SIPUSH；LDC 载荷是
/// CP 索引不可读，既有用例不用 CP 形态装载）。
bool pushedIntValueOf(const jbc::ir::Insn& in, ca::i32& out)
{
    if (in.op >= Op::ICONST_0 && in.op <= Op::ICONST_5) {
        out = static_cast<ca::i32>(in.op) - static_cast<ca::i32>(Op::ICONST_0);
        return true;
    }
    if (in.op == Op::BIPUSH || in.op == Op::SIPUSH) {
        out = in.a;
        return true;
    }
    return false;
}

}   // namespace


// ── 消除族 ──

TEST(StackOperationSimplifierTest, dupPopEliminated)
{
    // aconst_null; dup; pop; areturn —— [dup,pop] 整窗删除（垫底值使
    // dup 落点深度可验证，areturn 收口类型）。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      aconst_null\n      dup\n      pop\n      areturn\n",
                        "()Ljava/lang/Object;"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::ARETURN);
}

TEST(StackOperationSimplifierTest, swapSwapEliminated)
{
    // aload_0; aconst_null; swap; swap; areturn —— [swap,swap] 恒等消除。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      aload_0\n      aconst_null\n      swap\n      swap\n"
                        "      areturn\n",
                        "()Ljava/lang/Object;"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ALOAD_0);
    EXPECT_EQ(ops[1], Op::ACONST_NULL);
    EXPECT_EQ(ops[2], Op::ARETURN);
}

TEST(StackOperationSimplifierTest, cat1LoadPopEliminated)
{
    // iconst_2; pop; return —— 装载进窗，随窗消除。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      iconst_2\n      pop\n      return\n"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(StackOperationSimplifierTest, cat2LoadPop2Eliminated)
{
    // lconst_0; pop2; return
    const MethodPassRun r = runMethodPass(
        assembleFixture("      lconst_0\n      pop2\n      return\n"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 合并族 ──

TEST(StackOperationSimplifierTest, popPopMergedToPop2)
{
    // aconst_null; aconst_null; pop; pop; return —— [pop,pop] => [pop2]。
    const MethodPassRun r = runMethodPass(
        assembleFixture(
            "      aconst_null\n      aconst_null\n      pop\n      pop\n      return\n"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::ACONST_NULL);
    EXPECT_EQ(ops[2], Op::POP2);
    EXPECT_EQ(ops[3], Op::RETURN);
}

TEST(StackOperationSimplifierTest, dupPop2FoldedToPop)
{
    // aconst_null; dup; pop2; return —— [dup,pop2] => [pop]。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      aconst_null\n      dup\n      pop2\n      return\n"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

TEST(StackOperationSimplifierTest, cat1LoadPop2FoldedToPop)
{
    // aconst_null; iconst_2; pop2; return —— [loadInt,pop2] => [pop]。
    // 可验证面上 [装载,pop2] 短窗只在双值深度出现（装载底下必垫一个
    // cat1 值凑满 pop2 的两槽），垫底 aconst_null 不入装载族、不级联。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      aconst_null\n      iconst_2\n      pop2\n      return\n"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

TEST(StackOperationSimplifierTest, cat1LoadPairPop2EliminatedWholeGroup)
{
    // iconst_3; iconst_4; pop2; return —— [loadInt,loadInt,pop2] => ∅。
    // 胜出纪律钉死：Longest 使双装载整组随 pop2 消费，单装载短窗不得
    // 抢吃（旧规则序纪律在此形状吃掉 [装载2,pop2] 留下死装载，栈面 +1）。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      iconst_3\n      iconst_4\n      pop2\n      return\n"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 展开族（常量装载复制）──

TEST(StackOperationSimplifierTest, constLoadDupDuplicated)
{
    // iconst_5; dup; ireturn —— [loadInt,dup] => [loadInt,loadInt]。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      iconst_5\n      dup\n      ireturn\n", "()I"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                   ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 3u);
    ca::i32 v0 = 0, v1 = 0;
    ASSERT_TRUE(pushedIntValueOf(*ins.cu->dom.peek(ids[0]), v0));
    ASSERT_TRUE(pushedIntValueOf(*ins.cu->dom.peek(ids[1]), v1));
    EXPECT_EQ(v0, 5);
    EXPECT_EQ(v1, 5);
    EXPECT_EQ(ins.cu->dom.peek(ids[2])->op, Op::IRETURN);
}

TEST(StackOperationSimplifierTest, cat2LoadDup2Duplicated)
{
    // lconst_0; dup2; lreturn —— [loadLong,dup2] => [loadLong,loadLong]。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      lconst_0\n      dup2\n      lreturn\n", "()J"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::LCONST_0);
    EXPECT_EQ(ops[1], Op::LCONST_0);
    EXPECT_EQ(ops[2], Op::LRETURN);
}

TEST(StackOperationSimplifierTest, loadSwapPopReordered)
{
    // iconst_1; iconst_2; swap; pop; ireturn —— [loadInt,swap,pop] =>
    // [pop,loadInt]（弹走换上来的旧栈顶，装载存活）。收敛断言：
    // [装载1;装载2;swap] 与 [装载2;swap;pop] 两候选窗在 Longest 平局下的
    // 任一胜出路径，第二轮都收敛到 [装载2;ireturn]。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      iconst_1\n      iconst_2\n      swap\n      pop\n"
                        "      ireturn\n",
                        "()I"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_2);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(StackOperationSimplifierTest, twoLoadSwapSwapped)
{
    // iconst_1; iconst_2; swap; ireturn —— [loadInt,loadInt,swap] =>
    // [装载2,装载1]。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      iconst_1\n      iconst_2\n      swap\n      ireturn\n",
                        "()I"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                   ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 3u);
    ca::i32 v0 = 0, v1 = 0;
    ASSERT_TRUE(pushedIntValueOf(*ins.cu->dom.peek(ids[0]), v0));
    ASSERT_TRUE(pushedIntValueOf(*ins.cu->dom.peek(ids[1]), v1));
    EXPECT_EQ(v0, 2);
    EXPECT_EQ(v1, 1);
    EXPECT_EQ(ins.cu->dom.peek(ids[2])->op, Op::IRETURN);
}

TEST(StackOperationSimplifierTest, dupX1Pop2KeepsSecond)
{
    // iconst_1; iconst_2; dup_x1; pop2; ireturn —— 4 条窗 => [装载2]。
    const MethodPassRun r = runMethodPass(
        assembleFixture(
            "      iconst_1\n      iconst_2\n      dup_x1\n      pop2\n      ireturn\n",
            "()I"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                   ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 2u);
    ca::i32 v0 = 0;
    ASSERT_TRUE(pushedIntValueOf(*ins.cu->dom.peek(ids[0]), v0));
    EXPECT_EQ(v0, 2);
    EXPECT_EQ(ins.cu->dom.peek(ids[1])->op, Op::IRETURN);
}

TEST(StackOperationSimplifierTest, dupX1PopReorders)
{
    // iconst_1; iconst_2; dup_x1; pop; ireturn —— 4 条窗 => [装载2,装载1]。
    const MethodPassRun r = runMethodPass(
        assembleFixture(
            "      iconst_1\n      iconst_2\n      dup_x1\n      pop\n      ireturn\n",
            "()I"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                   ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 3u);
    ca::i32 v0 = 0, v1 = 0;
    ASSERT_TRUE(pushedIntValueOf(*ins.cu->dom.peek(ids[0]), v0));
    ASSERT_TRUE(pushedIntValueOf(*ins.cu->dom.peek(ids[1]), v1));
    EXPECT_EQ(v0, 2);
    EXPECT_EQ(v1, 1);
    EXPECT_EQ(ins.cu->dom.peek(ids[2])->op, Op::IRETURN);
}

TEST(StackOperationSimplifierTest, cat2Dup2X2ChainKeepsSecond)
{
    // lconst_1; lconst_0; lconst_1; dup2_x2; pop2; pop2; lreturn ——
    // 5 条窗 => [装载2]（cat2 窗底下垫 lconst_1 凑 dup2_x2 的三值形式）。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      lconst_1\n      lconst_0\n      lconst_1\n      dup2_x2\n"
                        "      pop2\n      pop2\n      lreturn\n",
                        "()J"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    // 垫底 lconst_1 保留在位，5 条窗整体折叠为 [装载2] 的值拷贝。
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::LCONST_1);
    EXPECT_EQ(ops[1], Op::LCONST_1);
    EXPECT_EQ(ops[2], Op::LRETURN);
}

// ── 不动点收敛 ──

TEST(StackOperationSimplifierTest, nestedShapesConvergeToFixpoint)
{
    // iconst_3; dup; pop; pop; return —— 首轮三个重叠候选窗（[装载,dup]/
    // [dup,pop]/[pop,pop]，Longest 平局）任一胜出路径都收敛到只剩 return。
    const MethodPassRun r = runMethodPass(
        assembleFixture("      iconst_3\n      dup\n      pop\n      pop\n      return\n"),
        mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const auto ops = expectOps(r);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 保守面 ──

TEST(StackOperationSimplifierTest, noMatchMeansAllPreservedAndByteIdentical)
{
    // aconst_null; pop; return —— 无规则命中（loadInt 不含 aconst_null）。
    const Bytes in = assembleFixture("      aconst_null\n      pop\n      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_FALSE(r.pa.changed());
    EXPECT_EQ(r.out, in);
}

TEST(StackOperationSimplifierTest, boundaryAnchoredWindowStopsConservatively)
{
    // aconst_null; dup; pop; areturn，异常表 handler 锚定在窗口内点 pop 上：
    // 窗口 [dup,pop] 命中且内点被边界锚定 → 纪律门按窗口粒度拒收，
    // 无其余候选 → 无进展终止，声明语义 = 整方法未改动、字节保真。
    // 异常表面走 ClassBytesBuilder 标签装配：try [lTry,lEnd) 覆盖
    // dup..pop，handler = lHandler（pop 落点）；窗口首条 dup 的 lTry 锚定
    // 走首条豁免，内点 pop 的 handler 锚定触发拒收。
    namespace support = mjbase::analysis::test_support;
    support::ClassBytesBuilder builder("t/S");
    support::MethodSpec        spec;
    spec.name      = "m";
    spec.desc      = "()Ljava/lang/Object;";
    spec.maxStack  = 2;
    spec.maxLocals = 1;
    spec.emit      = [](support::CodeEmit& e) {
        const std::size_t lNull     = e.newLabel();   // 0
        const std::size_t lTry      = e.newLabel();   // 1
        const std::size_t lHandler  = e.newLabel();   // 2
        const std::size_t lEnd      = e.newLabel();   // 3
        e.bindLabel(lNull);
        e.op(Op::ACONST_NULL);
        e.bindLabel(lTry);
        e.op(Op::DUP);
        e.bindLabel(lHandler);
        e.op(Op::POP);
        e.bindLabel(lEnd);
        e.op(Op::ARETURN);
    };
    spec.exceptions = {{1, 3, 2, ""}};
    const Bytes in        = builder.assemble({spec});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::StackOperationSimplifier{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 注册面 ──

TEST(StackOperationSimplifierTest, registeredAsStackOperationSimplifier)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("StackOperationSimplifier"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("StackOperationSimplifier");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
