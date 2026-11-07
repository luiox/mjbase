// DeadLocalStoreRemover：槽位值链不可达有意义汇点的 store 整删/降级——
// meaningful 槽判定（汇点闭包穿透转存/dup 透明）、常量链联合不动点整删、
// 非常量生产者降级 POP/POP2、参数槽保护、截断/失败跳过。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService；装配器自带
// frame 分析），指令体片段 R"JASM 定界。空栈 IADD 形态装配器 frame 门拒收，
// 按纪律例外①申报：从装配合法类出发单点最小偏差（一个指令字节回改）+ 改动
// 前后 relaxed validate 自检。
#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/dead_local_store_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法夹具（自定义描述符，指令体为 jasm 文本片段）。
Bytes oneMethodD(std::string_view name, std::string_view desc, std::string_view codeBody)
{
    return assembleMethodClass(name, desc, codeBody);
}

/// 单方法夹具（默认 ()V）。
Bytes oneMethod(std::string_view name, std::string_view codeBody)
{
    return oneMethodD(name, "()V", codeBody);
}

}   // namespace

// ── 死槽直喂常量：常量 + store 一并整删 ──
TEST(DeadLocalStoreRemoverTest, constToDeadStoreFullyRemoved)
{
    // ICONST_1; ISTORE_1; RETURN → RETURN。槽 1 无读点，store 输入是纯
    // 常量且唯一消费 → 联合整删。
    const MethodPassRun r =
        runMethodPass(oneMethod("m",
                                R"JASM(      iconst_1
      istore_1
      return
)JASM"),
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
        runMethodPass(oneMethod("m",
                                R"JASM(      iconst_1
      istore_1
      iload_1
      pop
      return
)JASM"),
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
    // ()I：ICONST_1; ISTORE_1; ILOAD_1; IRETURN; RETURN——IRETURN 是汇点。
    const Bytes in = oneMethodD("m", "()I",
                                R"JASM(      iconst_1
      istore_1
      iload_1
      ireturn
      return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 参数槽保护：签名播种入参定义，参数值喂返回 → 槽活 store 不动 ──
TEST(DeadLocalStoreRemoverTest, paramSlotStoreKept)
{
    // (I)I：ILOAD_0; ISTORE_0; ILOAD_0; IRETURN——参数定义闭包到达汇点。
    const Bytes in = oneMethodD("m", "(I)I",
                                R"JASM(      iload_0
      istore_0
      iload_0
      ireturn
      return
)JASM");
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
        runMethodPass(oneMethod("m",
                                R"JASM(      iconst_1
      iconst_2
      iadd
      istore_1
      return
)JASM"),
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
    const MethodPassRun r =
        runMethodPass(oneMethodD("m", "()I",
                                 R"JASM(      iconst_1
      dup
      istore_1
      istore_2
      iload_2
      ireturn
      return
)JASM"),
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
        runMethodPass(oneMethod("m",
                                R"JASM(      iconst_1
      istore_1
      iload_1
      istore_2
      return
)JASM"),
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
        runMethodPass(oneMethod("m",
                                R"JASM(      lconst_1
      lstore_1
      lload_1
      pop2
      return
)JASM"),
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
    // ICONST_1; ICONST_2; IF_ICMPNE→Lm; ICONST_2; GOTO→Ls; Lm: ICONST_1;
    // Ls: ISTORE_0; RETURN。if_icmpne 落汇点常量、goto 落 store（原字节
    // 偏移 +7/+4 的双目标形态）。store 的输入到达定义 = 两条路径的常量，
    // 各自唯一消费 → 全部整删，分支目标经 fallthrough 链落到 RETURN。
    const MethodPassRun r =
        runMethodPass(oneMethod("m",
                                R"JASM(      iconst_1
      iconst_2
      if_icmpne Lm
      iconst_2
      goto Ls
      Lm:
      iconst_1
      Ls:
      istore_0
      return
)JASM"),
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
    // 形态非 Verifier 合法（空栈 POP 下溢），jasm 装配器 frame 门拒收——
    // 纪律例外①：从装配合法类出发单点最小偏差（ICONST_1 → POP 一字节回
    // 改，栈下溢），改动前后各做一次 relaxed validate 自检，仅钉防御面：
    // 分析失败不动。
    const Bytes legal = oneMethod("m",
                                  R"JASM(      iconst_1
      iconst_2
      iadd
      istore_1
      return
)JASM");
    expectValidateOk(legal);
    const std::vector<ca::u8> want{0x04, 0x05, 0x60, 0x3c, 0xb1};   // 指令体字节模式
    const auto                at    = std::search(legal.begin(), legal.end(), want.begin(),
                                                  want.end());
    ASSERT_NE(at, legal.end()) << "指令体字节模式未命中";
    Bytes illegal = legal;
    illegal[static_cast<ca::usize>(at - legal.begin())] = 0x57;   // ICONST_1 → POP
    expectValidateFail(illegal);

    const MethodPassRun r = runMethodPass(illegal, mjbase::pass::DeadLocalStoreRemover{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, illegal);
}

// ── 注册面 ──
TEST(DeadLocalStoreRemoverTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("DeadLocalStoreRemover"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("DeadLocalStoreRemover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
