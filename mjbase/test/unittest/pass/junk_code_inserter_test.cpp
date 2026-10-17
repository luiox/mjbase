// JunkCodeInserter：确定性垃圾分支插入 + maxStack 抬升 + A1 帧链接线
// （StrictTypeResolverAdapter 注入 → SMT 再生被读侧接受）。
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

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 8×NOP 垫头 + 方法体（首个 i%8==0 落在垫头之后的原首条指令上）。
Bytes paddedMethod(Bytes body)
{
    FixtureBuilder b("t/J");
    Bytes          code(8, 0x00);   // NOP ×8
    appendBytes(code, std::move(body));
    b.addMethod({"m", "(I)I", std::move(code)});
    return b.assemble();
}

/// static int pick(int)：ILOAD_0; IFLE L; ICONST_1; IRETURN; ICONST_M1; IRETURN。
Bytes branchBody()
{
    // IFLE offset +5：自 pc3 落到 ICONST_M1。
    return {0x1a, 0x9e, 0x00, 0x05, 0x04, 0xac, 0x02, 0xac};
}

}   // namespace

// ── 插入点确定性：kStride 位置 + fallthrough/handler 双守卫 ──
TEST(JunkCodeInserterTest, insertsAtDeterministicStridePositions)
{
    // 8 NOP + branchBody = 14 条快照；i=8（原 ILOAD_0 前）命中一个插入点。
    const MethodPassRun r = runMethodPass(paddedMethod(branchBody()),
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
    FixtureBuilder b("t/J");
    b.addMethod({"m", "()V", {0x04, 0x57, 0xb1}});
    const Bytes         plain = b.assemble();
    const MethodPassRun r     = runMethodPass(plain, mjbase::pass::JunkCodeInserter{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, plain);
}

// ── 确定性：同一输入两次驱动，写回字节一致 ──
TEST(JunkCodeInserterTest, deterministicAcrossRuns)
{
    const Bytes in = paddedMethod(branchBody());
    const MethodPassRun a = runMethodPass(in, mjbase::pass::JunkCodeInserter{});
    const MethodPassRun b = runMethodPass(in, mjbase::pass::JunkCodeInserter{});
    EXPECT_EQ(a.out, b.out);
}

// ── maxStack 防御性抬升 ──
TEST(JunkCodeInserterTest, bumpsMaxStackFloor)
{
    FixtureBuilder b("t/J");
    Bytes          code(8, 0x00);
    code.push_back(0xb1);   // RETURN
    FixtureMethod m{"m", "()V", std::move(code), 0x0009, {}, 0};
    b.addMethod(m);
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::JunkCodeInserter{});

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

    const MethodPassRun r =
        runMethodPassOpts(paddedMethod(branchBody()), options, mjbase::pass::JunkCodeInserter{});
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
    const MethodPassRun r = runMethodPass(paddedMethod(branchBody()),
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
