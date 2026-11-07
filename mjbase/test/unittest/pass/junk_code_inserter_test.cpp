// JunkCodeInserter：确定性垃圾分支插入 + maxStack 抬升 + A1 帧链接线
// （StrictTypeResolverAdapter 注入 → SMT 再生被读侧接受）。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService），指令体
// 片段 R"JASM 定界。
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "jbc/analysis/smt_read.hpp"
#include "jbc/core/opcode.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/pass/junk_code_inserter.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 8×NOP 垫头 + 分支体（首个 i%8==0 落在垫头之后的原首条指令上）。
/// static int pick(int)：ILOAD_0; IFLE→L; ICONST_1; IRETURN; ICONST_M1; IRETURN
/// （IFLE 原偏移 +5 落 ICONST_M1，jasm 标签 L 同位）。
Bytes paddedMethod(ca::i32 major = 49)
{
    return assembleMethodClass("m", "(I)I",
                               R"JASM(      nop
      nop
      nop
      nop
      nop
      nop
      nop
      nop
      iload_0
      ifle L
      iconst_1
      ireturn
      L:
      iconst_m1
      ireturn
)JASM",
                               "t/J", major);
}

}   // namespace

// ── 插入点确定性：kStride 位置 + fallthrough/handler 双守卫 ──
TEST(JunkCodeInserterTest, insertsAtDeterministicStridePositions)
{
    // 8 NOP + branchBody = 14 条快照；i=8（原 ILOAD_0 前）命中一个插入点。
    const MethodPassRun r = runMethodPass(paddedMethod(),
                                          mjbase::pass::JunkCodeInserter{});
    EXPECT_TRUE(r.pa.changed());

    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    // 8 NOP + 4 垃圾块 + 6 原体 = 18。
    ASSERT_EQ(ops.size(), 18u);
    EXPECT_EQ(ops[8], Op::ICONST_0);
    EXPECT_EQ(ops[9], Op::IFEQ);
    EXPECT_EQ(ops[10], Op::ICONST_1);
    EXPECT_EQ(ops[11], Op::POP);
    EXPECT_EQ(ops[12], Op::ILOAD_0);
}

// ── 短方法（快照内无 kStride 命中点）不动 ──
TEST(JunkCodeInserterTest, shortMethodUntouched)
{
    const Bytes plain = assembleMethodClass("m", "()V",
                                            R"JASM(      iconst_1
      pop
      return
)JASM",
                                            "t/J");
    const MethodPassRun r     = runMethodPass(plain, mjbase::pass::JunkCodeInserter{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, plain);
}

// ── 确定性：同一输入两次驱动，写回字节一致 ──
TEST(JunkCodeInserterTest, deterministicAcrossRuns)
{
    const Bytes in = paddedMethod();
    const MethodPassRun a = runMethodPass(in, mjbase::pass::JunkCodeInserter{});
    const MethodPassRun b = runMethodPass(in, mjbase::pass::JunkCodeInserter{});
    EXPECT_EQ(a.out, b.out);
}

// ── maxStack 防御性抬升 ──
TEST(JunkCodeInserterTest, bumpsMaxStackFloor)
{
    // 8×NOP + RETURN 的帧面零栈需求：装配器计算 maxStack = 0，与原夹具
    // 显式 0 声明同实——垃圾块插入后 pass 抬升下限的断言面不变。
    const Bytes in = assembleMethodClass("m", "()V",
                                         R"JASM(      nop
      nop
      nop
      nop
      nop
      nop
      nop
      nop
      return
)JASM",
                                         "t/J");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::JunkCodeInserter{});

    EXPECT_TRUE(r.pa.changed());
    EXPECT_GE(maxStackOf(inspect(r.out)), 1);
}

// ── A1 帧链接线（B 批验收核心）：resolver 注入 → SMT 再生且被读侧接受 ──
TEST(JunkCodeInserterTest, resolverWiredPipelineRegeneratesSmt)
{
    mjbase::KeyPool                            pool;
    mjbase::hierarchy::ClassHierarchy          hierarchy{pool};
    mjbase::spi::CommonSuperClassResolver      resolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter     typeResolver{resolver};

    mjbase::spi::PassPipelineOptions options;
    options.typeResolver = &typeResolver;

    // A1 验收走 52 帧面（原 FixtureBuilder 为 V1_8 无 SMT；jasm ≥50 档自带
    // SMT，resolver 注入后的再生面同向覆盖）。
    const MethodPassRun r =
        runMethodPassOpts(paddedMethod(52), options, mjbase::pass::JunkCodeInserter{});
    EXPECT_TRUE(r.pa.changed());

    // 写回产物再升格：SMT 存在、读侧（readFramesFromSmt）防御全绿且非空。
    const Inspection ins = inspect(r.out);
    auto frames = jbc::analysis::readFramesFromSmt(*ins.session, *ins.cu);
    ASSERT_TRUE(frames.is_ok());
    const std::vector<jbc::analysis::SmtFrameView> views = std::move(frames).unwrap();
    EXPECT_FALSE(views.empty());
    // maxes 被重算覆盖后仍 ≥ 垃圾块的栈需求。
    EXPECT_GE(ins.cu->maxStack, 1);
}

// ── 未接 resolver：无既有 SMT 的类照常通过（新目标无帧 = 文档化的 opt-in 缺席）──
TEST(JunkCodeInserterTest, unwiredPipelineLeavesNoSmt)
{
    const MethodPassRun r = runMethodPass(paddedMethod(),
                                          mjbase::pass::JunkCodeInserter{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    auto frames = jbc::analysis::readFramesFromSmt(*ins.session, *ins.cu);
    ASSERT_TRUE(frames.is_ok());
    EXPECT_TRUE(std::move(frames).unwrap().empty());
}

// ── 注册面 ──
TEST(JunkCodeInserterTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("JunkCodeInserter"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("JunkCodeInserter");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
