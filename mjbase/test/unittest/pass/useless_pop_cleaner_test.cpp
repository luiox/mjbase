// UselessPopCleaner：相位一「局部装载 + POP/POP2 相邻对」不动点消除 +
// 弹宽/矩阵边界 + 边界守卫；相位二帧溯源面（def-use 到达定义）——纯常量
// 生产者 + 唯一消费者 pop 整删、POP2 半删降级、多定义汇合，以及栈中继
// 逐中继细化（DUP/DUP2 吸收认证 + 组栈平衡 + fail-closed 形态清单）。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）。两处装配
// 器 frame 门拒收的栈下溢形态（空栈 IADD、cat1 装载配 POP2）按纪律例外①
// 申报：从装配合法类出发单点最小偏差（一个指令字节回改）+ 改动前后
// relaxed validate 自检。
#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/useless_pop_cleaner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器（自定义描述符——帧面测试需要带参方法）。
Bytes oneMethodD(std::string_view name, std::string_view desc, std::string_view codeBody)
{
    return assembleMethodClass(name, desc, codeBody);
}

/// 单方法容器（默认 ()V）。
Bytes oneMethod(std::string_view name, std::string_view codeBody)
{
    return oneMethodD(name, "()V", codeBody);
}

/// 纪律例外①通用面：legal 里检索指令体字节模式 want，把 at[offset] 单字节
/// 回改为 to（装配器 frame 门拒收的最小偏差造法），返回偏差产物。
Bytes singleByteDeviation(const Bytes& legal, const std::vector<ca::u8>& want, ca::usize offset,
                          ca::u8 to)
{
    const auto at = std::search(legal.begin(), legal.end(), want.begin(), want.end());
    EXPECT_NE(at, legal.end()) << "指令体字节模式未命中";
    if (at == legal.end()) return {};
    Bytes illegal = legal;
    illegal[static_cast<ca::usize>(at - legal.begin()) + offset] = to;
    return illegal;
}

}   // namespace

// ── POP 吸收 category-1 局部装载（紧凑形 ILOAD_1）──
TEST(UselessPopCleanerTest, localIntLoadPairFolded)
{
    // ILOAD_1; POP; RETURN → RETURN。配对删除恒栈中性，不读槽内容。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
iload 1
pop
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── POP 吸收引用装载（ALOAD_0）──
TEST(UselessPopCleanerTest, localRefLoadPairFolded)
{
    // ALOAD_0; POP; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
aload 0
pop
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 帧面：纯常量生产者 + POP 相邻对（窗口面不管常量，帧面接管）──
TEST(UselessPopCleanerTest, frameConstProducerAbsorbed)
{
    // ICONST_1; POP; RETURN → RETURN。生产者无副作用、唯一消费者是 pop。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
iconst_1
pop
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 帧面：ACONST_NULL 生产者 ──
TEST(UselessPopCleanerTest, frameAconstNullProducerAbsorbed)
{
    // ACONST_NULL; POP; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
aconst_null
pop
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 帧面：非相邻形态（窗口面够不着，帧面按到达定义删）──
TEST(UselessPopCleanerTest, frameNonAdjacentConstPopsFolded)
{
    // ICONST_1; ICONST_2; POP; POP; RETURN → RETURN。两个 pop 各自的弹值
    // 到达定义互异、各唯一消费。
    const MethodPassRun r =
        runMethodPass(oneMethod("m",
                                R"JASM(
iconst_1
iconst_2
pop
pop
return
)JASM"),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 运算/分析失败形态不动：IADD; POP 的帧数据流不成立（du.ok() 假，
//    空栈弹双值），帧面跳过；即便分析成立，运算生产者也不在纯常量族 ──
TEST(UselessPopCleanerTest, nonFoldableOperandUntouched)
{
    // 形态本身非 Verifier 合法（IADD 弹两推一），仅钉防御面：不匹配不动。
    // 纪律例外①：装配合法基座（ICONST_1→POP 单字节回改成空栈 IADD），前后
    // relaxed validate 自检。
    const Bytes legal = oneMethod("m",
                                  R"JASM(
iconst_1
iconst_2
iadd
pop
return
)JASM");
    expectValidateOk(legal);
    const Bytes illegal =
        singleByteDeviation(legal, {0x04, 0x05, 0x60, 0x57, 0xb1}, 0, 0x57);   // ICONST_1→POP
    ASSERT_NE(illegal.size(), 0u);
    expectValidateFail(illegal);

    const MethodPassRun r = runMethodPass(illegal, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, illegal);
}

// ── POP2 吸收 category-2 局部装载（LLOAD_2 / DLOAD_0）──
TEST(UselessPopCleanerTest, wideLocalLoadPairFoldedByPop2)
{
    // LLOAD_2; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
lload 2
pop2
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(UselessPopCleanerTest, wideLocalDoubleLoadPairFoldedByPop2)
{
    // DLOAD_0; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
dload 0
pop2
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 弹宽失配：cat1 装载配 POP2 不动（POP2 还要吃更深一槽，删两指令
//    非栈中性；双 cat1 半删归帧面批次）──
TEST(UselessPopCleanerTest, widthMismatchLocalLoadKept)
{
    // ILOAD_1; POP2; RETURN——保持不动。纪律例外①：装配合法基座
    //（ICONST_1→NOP 单字节回改成 POP2 弹深不足），前后 relaxed validate 自检。
    const Bytes legal = oneMethod("m",
                                  R"JASM(
iconst_1
iload 1
pop2
return
)JASM");
    expectValidateOk(legal);
    const Bytes illegal =
        singleByteDeviation(legal, {0x04, 0x1b, 0x58, 0xb1}, 0, 0x00);   // ICONST_1→NOP
    ASSERT_NE(illegal.size(), 0u);
    expectValidateFail(illegal);

    const MethodPassRun r = runMethodPass(illegal, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, illegal);
}

// ── 不动点：连续局部装载 + 连续 POP 两轮清空（交错形态）──
TEST(UselessPopCleanerTest, nestedLocalLoadPairsFixpoint)
{
    // ILOAD_1; ILOAD_2; POP; POP; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
iload 1
iload 2
pop
pop
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 首成员（装载）是分支目标：照常消除，GOTO 引用重定向到删除后继 ──
TEST(UselessPopCleanerTest, branchTargetLoadRedirectsThroughPair)
{
    // GOTO→LOAD 锚（装载是分支目标，POP 无锚可消）；ILOAD_1; POP; RETURN →
    // GOTO; RETURN（GOTO 随删除重定向到 RETURN）。goto 边携空栈入 ILOAD，
    // iload 1 读未播种槽为装配器放行面（探针实证），全形合法可装配。
    const MethodPassRun r =
        runMethodPass(oneMethod("m",
                                R"JASM(
goto Lf
Lf:
iload 1
pop
return
)JASM"),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::GOTO);
    EXPECT_EQ(ops[1], Op::RETURN);
}

// ── 边界守卫：POP 是分支目标（有标签锚定）时整对跳过 ──
TEST(UselessPopCleanerTest, branchTargetPopBlocksPair)
{
    // ILOAD_1; Lb: POP; RETURN; GOTO Lb（死段反向锚定 POP）。
    const Bytes in = oneMethod("m",
                               R"JASM(
iload 1
Lb:
pop
return
goto Lb
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 帧面：POP2 单 cat2 常量（long / double）整删 ──
TEST(UselessPopCleanerTest, framePop2LongConstRemoved)
{
    // LCONST_1; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
lconst_1
pop2
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

TEST(UselessPopCleanerTest, framePop2DoubleConstRemoved)
{
    // DCONST_1; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
dconst_1
pop2
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 帧面：POP2 双 cat1 两侧都可删 → 两个生产者 + POP2 整删 ──
TEST(UselessPopCleanerTest, framePop2TwoCat1BothRemoved)
{
    // ICONST_1; ICONST_2; POP2; RETURN → RETURN。index0=ICONST_2、
    // index1=ICONST_1，各自唯一消费。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
iconst_1
iconst_2
pop2
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 帧面：POP2 半删（顶侧可删、底侧是未播种参数 → 降级 POP）──
TEST(UselessPopCleanerTest, framePop2HalfSplitTopRemoved)
{
    // (I)V：ICONST_1; ILOAD_0; POP2; RETURN → ILOAD_0; POP; RETURN。
    // 底侧弹值是参数装载（无到达定义边），不可删；删顶侧生产者后 POP2
    // 原位降级 POP 弹剩余底值。
    const MethodPassRun r =
        runMethodPass(oneMethodD("m",
                                 "(I)V",
                                 R"JASM(
iconst_1
iload 0
pop2
return
)JASM"),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ILOAD_0);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── 帧面：POP2 半删（底侧可删、顶侧是 store 转发 → 降级 POP）──
TEST(UselessPopCleanerTest, framePop2HalfSplitBottomRemoved)
{
    // ICONST_1; ICONST_2; ISTORE_2; ILOAD_2; POP2; RETURN
    //   → ICONST_2; ISTORE_2; ILOAD_2; POP; RETURN。
    // 顶侧弹值定义经装载转发到 ISTORE_2（非纯常量生产者）不可删；底侧
    // ICONST_1 的值垫在栈底、唯一消费者是本 POP2 → 删 ICONST_1、降级。
    const MethodPassRun r =
        runMethodPass(oneMethod("m",
                                R"JASM(
iconst_1
iconst_2
istore 2
iload 2
pop2
return
)JASM"),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::ICONST_2);
    EXPECT_EQ(ops[1], Op::ISTORE_2);
    EXPECT_EQ(ops[2], Op::ILOAD_2);
    EXPECT_EQ(ops[3], Op::POP);
    EXPECT_EQ(ops[4], Op::RETURN);
}

// ── 逐中继细化：DUP 双拷贝全落计划 pop（[常量; DUP; POP; POP]）→
//    溯源认证 + 组栈平衡（+1+1-1-1=0）通过，整链删除 ──
TEST(UselessPopCleanerTest, frameDupAbsorbedByDoublePop)
{
    // ICONST_1; DUP; POP; POP; RETURN → RETURN。中继拷贝透明（拷贝行沿用
    // 原定义），两份拷贝的使用边都落在本批计划 pop 上；第二拷贝不离场。
    const MethodPassRun r =
        runMethodPass(oneMethod("m",
                                R"JASM(
iconst_1
dup
pop
pop
return
)JASM"),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 逐中继细化：DUP + POP2（双拷贝同一 pop 消费）→ 整链删除 ──
TEST(UselessPopCleanerTest, frameDupAbsorbedByPop2)
{
    // ICONST_1; DUP; POP2; RETURN → RETURN。
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
iconst_1
dup
pop2
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 逐中继细化：中继下有垫底值（删除组上方还有在场值）→ 只删链、不动
//    垫底 ──
TEST(UselessPopCleanerTest, frameDupAboveValueAbsorbed)
{
    // (I)V：ILOAD_0; ICONST_1; DUP; POP2; RETURN → ILOAD_0; RETURN。
    // POP2 双侧都是 DUP 拷贝（透明沿用 ICONST_1 定义），删除组
    // {ICONST_1, DUP, POP2} 净栈 +1+1-2=0，参数装载原样保留。
    const MethodPassRun r =
        runMethodPass(oneMethodD("m",
                                 "(I)V",
                                 R"JASM(
iload 0
iconst_1
dup
pop2
return
)JASM"),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ILOAD_0);
    EXPECT_EQ(ops[1], Op::RETURN);
}

// ── 逐中继细化：DUP2 双 cat1 对、四份拷贝全落计划 pop（两个 POP2）→
//    整链删除 ──
TEST(UselessPopCleanerTest, frameDup2Cat1PairAbsorbed)
{
    // ICONST_1; ICONST_2; DUP2; POP2; POP2; RETURN → RETURN。
    const MethodPassRun r =
        runMethodPass(oneMethod("m",
                                R"JASM(
iconst_1
iconst_2
dup2
pop2
pop2
return
)JASM"),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 逐中继细化：非相邻中继链（POP 与 POP2 各吃一份拷贝、中间隔常量）──
TEST(UselessPopCleanerTest, frameDupNonAdjacentAbsorbed)
{
    // ICONST_1; DUP; ICONST_2; POP; POP2; RETURN → RETURN。POP 吃 ICONST_2、
    // POP2 吃两份 DUP 拷贝；两个计划各自溯源认证、组平衡各自归零。
    const MethodPassRun r =
        runMethodPass(oneMethod("m",
                                R"JASM(
iconst_1
dup
iconst_2
pop
pop2
return
)JASM"),
                      mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 逐中继细化 fail-closed：DUP 第二拷贝逃逸（[常量; DUP; POP]）→
//    组栈平衡 +1 非零，整组弃，保持不动 ──
TEST(UselessPopCleanerTest, frameDupEscapedCopyKept)
{
    // ICONST_1; DUP; POP; RETURN——留在栈底的第二拷贝无人消费，删链会
    // 悬空它：整体不动。
    const Bytes in = oneMethod("m",
                               R"JASM(
iconst_1
dup
pop
return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 逐中继细化 fail-closed：DUP2 对 long/double 双槽（cat2 直接输入）──
TEST(UselessPopCleanerTest, frameDup2Cat2InputKept)
{
    // LCONST_0; DUP2; POP2; RETURN——cat2 拷贝的第二份天然离场（双槽形态
    // 挂账）：整体不动。
    const Bytes in = oneMethod("m",
                               R"JASM(
lconst_0
dup2
pop2
return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 逐中继细化 fail-closed：SWAP 重排中继不在吸收族 ──
TEST(UselessPopCleanerTest, frameSwapRelayKept)
{
    // ICONST_1; ICONST_2; SWAP; POP; POP; RETURN——SWAP 的重排形态挂账：
    // 整体不动。
    const Bytes in = oneMethod("m",
                               R"JASM(
iconst_1
iconst_2
swap
pop
pop
return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 逐中继细化 fail-closed：POP2 一侧是未播种参数装载 → 无计划不动 ──
TEST(UselessPopCleanerTest, frameDupMixedForeignSideKept)
{
    // (I)V：ICONST_1; DUP; ILOAD_0; POP2; RETURN——顶侧参数装载无到达
    // 定义边，POP2 侧不可删 → 不动。
    const Bytes in = oneMethodD("m",
                                "(I)V",
                                R"JASM(
iconst_1
dup
iload 0
pop2
return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 帧面：多到达定义汇合——各路径各推一个常量、汇合 pop 统一弹弃 ──
TEST(UselessPopCleanerTest, frameMultiDefMergePopsFolded)
{
    // ICONST_1; ICONST_2; IF_ICMPNE→Lc; ICONST_2; GOTO→Lp; Lc: ICONST_1;
    // Lp: POP; RETURN。POP 的弹值到达定义 = {两条路径的常量生产者}，各自
    // 唯一消费 → 三个生产者/消费者组整删，跳转引用经 fallthrough 重定向
    // 链式落到 RETURN。
    const MethodPassRun r = runMethodPass(
        oneMethod("m",
                  R"JASM(
iconst_1
iconst_2
if_icmpne Lc
iconst_2
goto Lp
Lc:
iconst_1
Lp:
pop
return
)JASM"),
        mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::ICONST_2);
    EXPECT_EQ(ops[2], Op::IF_ICMPNE);
    EXPECT_EQ(ops[3], Op::GOTO);
    EXPECT_EQ(ops[4], Op::RETURN);
}

// ── 帧面：ldc Class 元数据常量为纯生产者（元数据弹弃场景）──
TEST(UselessPopCleanerTest, frameLdcClassPopFolded)
{
    const MethodPassRun r = runMethodPass(oneMethod("m",
                                                    R"JASM(
ldc Ljava/lang/Object;
pop
return
)JASM"),
                                          mjbase::pass::UselessPopCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 注册面 ──
TEST(UselessPopCleanerTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("UselessPopCleaner"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("UselessPopCleaner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
