// RedundantStoreEliminater：store 定义零使用边 → 原位降级 pop/pop2——
// 覆写消除、尾部死写（修正扩展）、读取保槽、旧栈控制流孔洞闭合（跳出到
// 区间后读取的两 store 全保）、iinc 边保护、cat2 宽度、分析失败跳过、
// 幂等、注册面。
//
// 夹具纪律：DomFixture 用例是数据流域夹具不涉字节；分析失败形态的类
// 容器一律 jasm 文本装配（JvmJasmService），指令体片段 R"JASM 定界。空栈
// IADD 形态装配器 frame 门拒收，按纪律例外①申报：从装配合法类出发单点
// 最小偏差（一个指令字节回改）+ 改动前后 relaxed validate 自检。
#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/redundant_store_eliminater.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "fix_pattern_fixture.hpp"
#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法容器（分析失败形态用；DomFixture 构不出非法帧形）。
Bytes oneMethod()
{
    return assembleMethodClass("m", "()V",
                               R"JASM(      iconst_1
      iconst_2
      iadd
      istore_1
      return
)JASM");
}

}   // namespace

// ── 覆写消除：两次写槽、只读后一次 → 前一次降级 POP ──
TEST(RedundantStoreEliminaterTest, overwrittenStoreDowngraded)
{
    DomFixture fx("t/F", "m", "()V", 2, 2, [&](Body& b) {
        b.iconst_1();
        b.istore(1);
        b.iconst_2();
        b.istore(1);
        b.iload(1);
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::RedundantStoreEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::ICONST_2);
    EXPECT_EQ(ops[3], Op::ISTORE);
    EXPECT_EQ(ops[4], Op::ILOAD);
    EXPECT_EQ(ops[5], Op::POP);
    EXPECT_EQ(ops[6], Op::RETURN);
}

// ── 尾部死写：方法结束前无人读的 store 降级（到达定义粒度，槽内其他
//    定义不牵连；旧栈线性序只杀「被覆写」的前一次，此为修正扩展）──
TEST(RedundantStoreEliminaterTest, trailingUnreadStoreDowngraded)
{
    DomFixture fx("t/F", "m", "()V", 2, 2, [&](Body& b) {
        b.iconst_1();
        b.istore(1);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::RedundantStoreEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── 读取保槽：中间有读取的前一次 store 不动；尾 store 降级 ──
TEST(RedundantStoreEliminaterTest, readBetweenKeepsEarlierStore)
{
    DomFixture fx("t/F", "m", "()V", 2, 2, [&](Body& b) {
        b.iconst_1();
        b.istore(1);
        b.iload(1);
        b.pop();
        b.iconst_2();
        b.istore(1);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::RedundantStoreEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::ISTORE);
    EXPECT_EQ(ops[2], Op::ILOAD);
    EXPECT_EQ(ops[3], Op::POP);
    EXPECT_EQ(ops[4], Op::ICONST_2);
    EXPECT_EQ(ops[5], Op::POP);
    EXPECT_EQ(ops[6], Op::RETURN);
}

// ── 旧栈控制流孔洞闭合：条件跳出到「位置在两次 store 之后」的读取——
//    两条路径各带一个定义，读取双定义 → 两个 store 全保（旧栈位置区间
//    推理会误删 store1）──
TEST(RedundantStoreEliminaterTest, jumpOutToLaterLoadKeepsBothStores)
{
    DomFixture fx("t/F", "m", "()V", 2, 2, [&](Body& b) {
        jbc::ir::LabelId late = b.label();
        b.iconst_1();
        b.istore(1);
        b.iconst_0();
        b.branch(Op::IFEQ, late);
        b.iconst_2();
        b.istore(1);
        b.iload(1);
        b.bindHere(late);
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::RedundantStoreEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 9u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::ISTORE);
    EXPECT_EQ(ops[4], Op::ICONST_2);
    EXPECT_EQ(ops[5], Op::ISTORE);
}

// ── iinc 读改写产生使用边：store 保留 ──
TEST(RedundantStoreEliminaterTest, iincReadKeepsStore)
{
    DomFixture fx("t/F", "m", "()V", 2, 2, [&](Body& b) {
        b.iconst_1();
        b.istore(1);
        b.iinc(1, 2);
        b.iload(1);
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::RedundantStoreEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 6u);
}

// ── cat2 宽度：无人读的 LSTORE 降级 POP2 ──
TEST(RedundantStoreEliminaterTest, cat2StoreDowngradesToPop2)
{
    DomFixture fx("t/F", "m", "()V", 4, 3, [&](Body& b) {
        b.ldcLong(111111111L);
        b.op(Op::LSTORE_1);
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::RedundantStoreEliminater{});
    EXPECT_TRUE(pa.changed());
    const std::vector<Op> ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::LDC2_W);
    EXPECT_EQ(ops[1], Op::POP2);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── 参数槽 store（签名播种）：读参数定义边的 store 不动 ──
TEST(RedundantStoreEliminaterTest, paramSlotStoreReadKept)
{
    DomFixture fx("t/F", "m", "(I)V", 2, 2, [&](Body& b) {
        b.iload(0);
        b.istore(1);
        b.iload(1);
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::RedundantStoreEliminater{});
    EXPECT_TRUE(pa.areAllPreserved());
    ASSERT_EQ(opsOf(fx).size(), 5u);
}

// ── 分析失败（空栈下溢）→ 整件跳过 ──
TEST(RedundantStoreEliminaterTest, failedAnalysisSkips)
{
    // 形态非 Verifier 合法（空栈 POP 下溢），jasm 装配器 frame 门拒收——
    // 纪律例外①：从装配合法类出发单点最小偏差（ICONST_1 → POP 一字节回
    // 改，栈下溢），改动前后各做一次 relaxed validate 自检，仅钉防御面：
    // 分析失败不动。
    const Bytes legal = oneMethod();
    expectValidateOk(legal);
    const std::vector<ca::u8> want{0x04, 0x05, 0x60, 0x3c, 0xb1};   // 指令体字节模式
    const auto                at = std::search(legal.begin(), legal.end(), want.begin(),
                                               want.end());
    ASSERT_NE(at, legal.end()) << "指令体字节模式未命中";
    Bytes illegal = legal;
    illegal[static_cast<ca::usize>(at - legal.begin())] = 0x57;   // ICONST_1 → POP
    expectValidateFail(illegal);

    const MethodPassRun r = runMethodPass(illegal, mjbase::pass::RedundantStoreEliminater{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, illegal);
}

// ── 幂等 ──
TEST(RedundantStoreEliminaterTest, idempotentAfterConvergence)
{
    DomFixture fx("t/F", "m", "()V", 2, 2, [&](Body& b) {
        b.iconst_1();
        b.istore(1);
        b.iconst_2();
        b.istore(1);
        b.iload(1);
        b.pop();
        b.vreturn();
    });
    ASSERT_TRUE(fx.run(mjbase::pass::RedundantStoreEliminater{}).changed());
    const auto pa2 = fx.run(mjbase::pass::RedundantStoreEliminater{});
    EXPECT_TRUE(pa2.areAllPreserved());
}

// ── 注册面 ──
TEST(RedundantStoreEliminaterTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("RedundantStoreEliminater"));
    auto created =
        mjbase::spi::defaultPassRegistry().createMethodPass("RedundantStoreEliminater");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
