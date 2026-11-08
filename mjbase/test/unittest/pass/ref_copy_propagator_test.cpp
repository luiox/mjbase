// RefCopyPropagator：单来源不可变引用的 aload 原位改写为来源指令。
// 该改（ldc/null/aload 链）/不该改（参数来源/汇合未知/白名单外/自赋值）/
// 幂等与注册面。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）。
#include <gtest/gtest.h>

#include <string>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/ref_copy_propagator.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

std::vector<Op> opsOfOut(const MethodPassRun& r)
{
    return opsOf(inspect(r.out));
}

std::vector<const jbc::ir::Insn*> allInsns(const Inspection& ins)
{
    std::vector<const jbc::ir::Insn*> all;
    for (const jbc::ir::InsnId id : ins.cu->dom.order()) {
        if (const jbc::ir::Insn* in = ins.cu->dom.peek(id)) all.push_back(in);
    }
    return all;
}

}   // namespace

// ── ldc 字符串经暂存槽读回 → aload 改写为 ldc ──
TEST(RefCopyPropagatorTest, ldcViaStoreReplaced)
{
    const MethodPassRun r =
        runMethodPass(assembleMethodClass("m",
                                          "()Ljava/lang/String;",
                                          R"JASM(
ldc "x"
astore 1
aload 1
areturn
)JASM"),
                      mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    // astore 1 经装配器窄形规范化落 ASTORE_1。
    EXPECT_EQ(ops,
              (std::vector<Op>{Op::LDC, Op::ASTORE_1, Op::LDC, Op::ARETURN}));
}

// ── aconst_null 经暂存槽读回 → aload 改写为 aconst_null ──
TEST(RefCopyPropagatorTest, aconstNullReplaced)
{
    const MethodPassRun r =
        runMethodPass(assembleMethodClass("m",
                                          "()Ljava/lang/Object;",
                                          R"JASM(
aconst_null
astore 1
aload 1
areturn
)JASM"),
                      mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    // astore 1 经装配器窄形规范化落 ASTORE_1。
    EXPECT_EQ(ops,
              (std::vector<Op>{Op::ACONST_NULL, Op::ASTORE_1, Op::ACONST_NULL,
                               Op::ARETURN}));
}

// ── 来源穿透多级暂存槽：两个 aload 都改写为最初 LDC ──
TEST(RefCopyPropagatorTest, foldsThroughTempSlots)
{
    const Bytes in = assembleMethodClass("m",
                                         "()Ljava/lang/String;",
                                         R"JASM(
ldc "x"
astore 0
aload 0
astore 1
aload 1
areturn
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 6u);
    EXPECT_EQ(all[2]->op, Op::LDC);
    EXPECT_EQ(all[4]->op, Op::LDC);
}

// ── 同槽自赋值（aload 0; astore 0; aload 0）→ 改写无意义，保留 ──
TEST(RefCopyPropagatorTest, selfAssignSkipped)
{
    const MethodPassRun r = runMethodPass(
        assembleMethodClass("m",
                            "(Ljava/lang/Object;)Ljava/lang/Object;",
                            R"JASM(
aload 0
astore 0
aload 0
areturn
)JASM"),
        mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 4u);
}

// ── 参数来源（Param）不是可克隆指令 → 保留 ──
TEST(RefCopyPropagatorTest, paramOriginKept)
{
    const MethodPassRun r = runMethodPass(
        assembleMethodClass("m",
                            "(Ljava/lang/Object;)Ljava/lang/Object;",
                            R"JASM(
aload 0
areturn
)JASM"),
        mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 分支汇合多来源 → Unknown → 保留 ──
TEST(RefCopyPropagatorTest, multiSourceMergeKept)
{
    const MethodPassRun r = runMethodPass(
        assembleMethodClass("m",
                            "()Ljava/lang/String;",
                            R"JASM(
iconst_0
ifeq L1
aconst_null
goto Lm
L1:
ldc "x"
Lm:
astore 1
aload 1
areturn
)JASM"),
        mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 8u);
}

// ── 生产者白名单外（方法调用，字段/调用可变面）→ 保留 ──
TEST(RefCopyPropagatorTest, callProducerKept)
{
    const MethodPassRun r = runMethodPass(
        assembleMethodClass("m",
                            "()Ljava/lang/Object;",
                            R"JASM(
invokestatic t/C.g ()Ljava/lang/Object;
astore 1
aload 1
areturn
)JASM"),
        mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 4u);
}

// ── 来源槽被再写：来源域穿透中途 aload，折叠得比旧栈更远（直达最初 LDC） ──
TEST(RefCopyPropagatorTest, sourceSlotRewrittenFoldsThrough)
{
    const MethodPassRun r = runMethodPass(
        assembleMethodClass("m",
                            "()Ljava/lang/String;",
                            R"JASM(
ldc "x"
astore 1
aload 1
astore 1
aload 1
areturn
)JASM"),
        mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
    const std::vector<const jbc::ir::Insn*> all = allInsns(ins);
    ASSERT_EQ(all.size(), 6u);
    EXPECT_EQ(all[2]->op, Op::LDC);
    EXPECT_EQ(all[4]->op, Op::LDC);   // 穿透中途 aload，直达最初 LDC
}

// ── 幂等：二轮不再变化 ──
TEST(RefCopyPropagatorTest, idempotent)
{
    const Bytes bytes = assembleMethodClass("m",
                                            "()Ljava/lang/String;",
                                            R"JASM(
ldc "x"
astore 1
aload 1
areturn
)JASM");
    const MethodPassRun r = runMethodPass(bytes, mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const MethodPassRun r2 =
        runMethodPass(r.out, mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── 注册面 ──
TEST(RefCopyPropagatorTest, registeredInRegistry)
{
    const auto found = mjbase::spi::defaultPassRegistry().find("RefCopyPropagator");
    EXPECT_TRUE(found.is_ok());
}
