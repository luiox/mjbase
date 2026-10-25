// DeadLocalStoreRemover：槽位值链不可达有意义汇点的 store 整删/降级——
// meaningful 槽判定（汇点闭包穿透转存/dup 透明）、常量链联合不动点整删、
// 非常量生产者降级 POP/POP2、参数槽保护、截断/失败跳过。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/dead_local_store_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器（自定义描述符）。
Bytes oneMethodD(std::string_view name, std::string_view desc, Bytes code)
{
    FixtureBuilder b("t/U");
    b.addMethod({name, desc, std::move(code)});
    return b.assemble();
}

/// 单方法容器（默认 ()V）。
Bytes oneMethod(std::string_view name, Bytes code)
{
    return oneMethodD(name, "()V", std::move(code));
}

}   // namespace

// ── 死槽直喂常量：常量 + store 一并整删 ──
TEST(DeadLocalStoreRemoverTest, constToDeadStoreFullyRemoved)
{
    // ICONST_1; ISTORE_1; RETURN → RETURN。槽 1 无读点，store 输入是纯
    // 常量且唯一消费 → 联合整删。
    const MethodPassRun r = runMethodPass(oneMethod("m", {0x04, 0x3c, 0xb1}),
                                          mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 读点存在但只喂 pop（非汇点）：槽仍死，store 与常量整删 ──
TEST(DeadLocalStoreRemoverTest, readToPopIsStillDead)
{
    // ICONST_1; ISTORE_1; ILOAD_1; POP; RETURN → ILOAD_1; POP; RETURN。
    // 弹弃不可观测，装载残段弹宽自洽（装载;pop 由窗口面在管线下一轮收）。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", {0x04, 0x3c, 0x1b, 0x57, 0xb1}),
                      mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ILOAD_1);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── 读点到达有意义汇点（带值返回）：槽活，整体不动 ──
TEST(DeadLocalStoreRemoverTest, meaningfulReadKeepsStore)
{
    // ()I：ICONST_1; ISTORE_1; ILOAD_1; IRETURN——IRETURN 是汇点。
    const Bytes in = oneMethodD("m", "()I", {0x04, 0x3c, 0x1b, 0xac, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 参数槽保护：签名播种入参定义，参数值喂返回 → 槽活 store 不动 ──
TEST(DeadLocalStoreRemoverTest, paramSlotStoreKept)
{
    // (I)I：ILOAD_0; ISTORE_0; ILOAD_0; IRETURN——参数定义闭包到达汇点。
    const Bytes in = oneMethodD("m", "(I)I", {0x1a, 0x3b, 0x1a, 0xac, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 运算生产者不整删：store 降级 POP，运算与其常量输入保留 ──
TEST(DeadLocalStoreRemoverTest, opProducerDowngradesStoreToPop)
{
    // ICONST_1; ICONST_2; IADD; ISTORE_1; RETURN
    //   → ICONST_1; ICONST_2; IADD; POP; RETURN。
    // 弹值定义是运算（非纯常量），整删会让两个常量成为孤儿推送。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", {0x04, 0x05, 0x60, 0x3c, 0xb1}),
                      mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::ICONST_2);
    EXPECT_EQ(ops[2], Op::IADD);
    EXPECT_EQ(ops[3], Op::POP);
    EXPECT_EQ(ops[4], Op::RETURN);
}

// ── 常量被活 store 共享（dup 复制透明）：常量逐出，死 store 降级 ──
TEST(DeadLocalStoreRemoverTest, sharedConstEvictedByLiveConsumer)
{
    // ()I：ICONST_1; DUP; ISTORE_1; ISTORE_2; ILOAD_2; IRETURN
    //   → ICONST_1; DUP; POP; ISTORE_2; ILOAD_2; IRETURN。
    // def-use 复制透明：ICONST_1 的使用边直达两个 store；ISTORE_2 的槽
    // 活（读点到达返回）→ 常量有集外消费者 → 逐出，ISTORE_1 降级 POP。
    const MethodPassRun r = runMethodPass(
        oneMethodD("m", "()I", {0x04, 0x59, 0x3c, 0x3d, 0x1c, 0xac, 0xb1}),
        mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::DUP);
    EXPECT_EQ(ops[2], Op::POP);
    EXPECT_EQ(ops[3], Op::ISTORE_2);
    EXPECT_EQ(ops[4], Op::ILOAD_2);
    EXPECT_EQ(ops[5], Op::IRETURN);
    EXPECT_EQ(ops[6], Op::RETURN);
}

// ── 槽位间转存穿透：链尾死 → 全链死；中转 store 降级留下装载残段 ──
TEST(DeadLocalStoreRemoverTest, transitiveSlotChainDies)
{
    // ICONST_1; ISTORE_1; ILOAD_1; ISTORE_2; RETURN
    //   → ILOAD_1; POP; RETURN。
    // 槽 1 的读点闭包经槽 2 的 store（中间定义）续走，槽 2 无读 → 全死；
    // ISTORE_2 弹值定义是 ISTORE_1（装载转发、非纯常量）→ 降级 POP。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", {0x04, 0x3c, 0x1b, 0x3d, 0xb1}),
                      mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ILOAD_1);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── cat2 族：LCONST; LSTORE 整删，宽度按 store 族降级 ──
TEST(DeadLocalStoreRemoverTest, longConstChainRemoved)
{
    // LCONST_1; LSTORE_1; LLOAD_1; POP2; RETURN → LLOAD_1; POP2; RETURN。
    const MethodPassRun r =
        runMethodPass(oneMethod("m", {0x0a, 0x40, 0x1f, 0x58, 0xb1}),
                      mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::LLOAD_1);
    EXPECT_EQ(ops[1], Op::POP2);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── 分支汇合多定义：各路径常量 + store 一并整删，跳转引用重定向 ──
TEST(DeadLocalStoreRemoverTest, branchMergeDefsFullyRemoved)
{
    // ICONST_1; ICONST_2; IF_ICMPNE→pc9; ICONST_2; GOTO→pc10; ICONST_1;
    // ISTORE_1(pc10); RETURN。store 的输入到达定义 = 两条路径的常量，
    // 各自唯一消费 → 全部整删，分支目标经 fallthrough 链落到 RETURN。
    const MethodPassRun r = runMethodPass(
        oneMethod("m", {0x04, 0x05, 0xa0, 0x00, 0x07, 0x05, 0xa7, 0x00, 0x04, 0x04, 0x3c, 0xb1}),
        mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::ICONST_2);
    EXPECT_EQ(ops[2], Op::IF_ICMPNE);
    EXPECT_EQ(ops[3], Op::GOTO);
    EXPECT_EQ(ops[4], Op::RETURN);
}

// ── 帧数据流失败（空栈 IADD）→ 整件跳过 ──
TEST(DeadLocalStoreRemoverTest, failedAnalysisSkips)
{
    // 形态非 Verifier 合法，仅钉防御面：分析失败不动。
    const Bytes in = oneMethod("m", {0x60, 0x3c, 0xb1});
    const MethodPassRun r = runMethodPass(in, mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 注册面 ──
TEST(DeadLocalStoreRemoverTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("DeadLocalStoreRemover"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("DeadLocalStoreRemover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
