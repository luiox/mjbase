// RefCopyPropagator：单来源不可变引用的 aload 原位改写为来源指令。
// 该改（ldc/null/aload 链）/不该改（参数来源/汇合未知/白名单外/自赋值）/
// 幂等与注册面。
#include <gtest/gtest.h>

#include <string>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/ref_copy_propagator.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

void put16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v & 0xff));
}

void putInvoke(Bytes& b, ca::u8 op, ca::u16 cp)
{
    b.push_back(op);
    put16(b, cp);
}

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

ExcSpec exc(ca::u16 start, ca::u16 end, ca::u16 handler, ca::u16 catchCp = 0)
{
    return ExcSpec{start, end, handler, catchCp};
}

}   // namespace

// ── ldc 字符串经暂存槽读回 → aload 改写为 ldc ──
TEST(RefCopyPropagatorTest, ldcViaStoreReplaced)
{
    FixtureBuilder b("t/C");
    const ca::u16 scp = b.strConst("x");
    Bytes        code = {0x12};            // LDC "x"
    code.push_back(static_cast<ca::u8>(scp));
    code.insert(code.end(), {0x3a, 0x01}); // ASTORE 1
    code.insert(code.end(), {0x19, 0x01}); // ALOAD 1
    code.push_back(0xb0);                  // ARETURN
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 1, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops,
              (std::vector<Op>{Op::LDC, Op::ASTORE, Op::LDC, Op::ARETURN}));
}

// ── aconst_null 经暂存槽读回 → aload 改写为 aconst_null ──
TEST(RefCopyPropagatorTest, aconstNullReplaced)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x01,                   // ACONST_NULL
                  0x3a, 0x01,             // ASTORE 1
                  0x19, 0x01,             // ALOAD 1
                  0xb0};                  // ARETURN
    b.addMethod({"m", "()Ljava/lang/Object;", std::move(code), 0x0009, {}, 1, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops,
              (std::vector<Op>{Op::ACONST_NULL, Op::ASTORE, Op::ACONST_NULL,
                               Op::ARETURN}));
}

// ── 来源穿透多级暂存槽：两个 aload 都改写为最初 LDC ──
TEST(RefCopyPropagatorTest, foldsThroughTempSlots)
{
    FixtureBuilder b("t/C");
    const ca::u16 scp = b.strConst("x");
    Bytes        code = {0x12};            // LDC "x"（pc0）
    code.push_back(static_cast<ca::u8>(scp));
    code.insert(code.end(), {0x3a, 0x00}); // ASTORE 0（pc2）
    code.insert(code.end(), {0x19, 0x00}); // ALOAD 0（pc4）
    code.insert(code.end(), {0x3a, 0x01}); // ASTORE 1（pc6）
    code.insert(code.end(), {0x19, 0x01}); // ALOAD 1（pc8）
    code.push_back(0xb0);                  // ARETURN（pc10）
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 1, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::RefCopyPropagator{});
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
    FixtureBuilder b("t/C");
    Bytes code = {0x2a,                   // ALOAD 0
                  0x3a, 0x00,             // ASTORE 0
                  0x19, 0x00,             // ALOAD 0
                  0xb0};                  // ARETURN
    b.addMethod({"m", "(Ljava/lang/Object;)Ljava/lang/Object;", std::move(code),
                 0x0009, {}, 1, 1});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 4u);
}

// ── 参数来源（Param）不是可克隆指令 → 保留 ──
TEST(RefCopyPropagatorTest, paramOriginKept)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x19, 0x00,             // ALOAD 0（入参来源）
                  0xb0};                  // ARETURN
    b.addMethod({"m", "(Ljava/lang/Object;)Ljava/lang/Object;", std::move(code)});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 分支汇合多来源 → Unknown → 保留 ──
TEST(RefCopyPropagatorTest, multiSourceMergeKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 scp = b.strConst("x");
    Bytes        code;
    code.push_back(0x03);                          // ICONST_0（pc0）
    code.insert(code.end(), {0x99, 0x00, 0x07});   // IFEQ → pc8（pc1）
    code.push_back(0x01);                          // ACONST_NULL（pc4）
    code.insert(code.end(), {0xa7, 0x00, 0x05});   // GOTO → pc10（pc5）
    code.push_back(0x12);                          // L1: LDC "x"（pc8）
    code.push_back(static_cast<ca::u8>(scp));
    code.insert(code.end(), {0x3a, 0x01});         // M: ASTORE 1（pc10）
    code.insert(code.end(), {0x19, 0x01});         // ALOAD 1（pc12）
    code.push_back(0xb0);                          // ARETURN（pc14）
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 1, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 8u);
}

// ── 生产者白名单外（方法调用，字段/调用可变面）→ 保留 ──
TEST(RefCopyPropagatorTest, callProducerKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 g = b.methodRef("t/C", "g", "()Ljava/lang/Object;");
    Bytes code;
    putInvoke(code, 0xb8, g);              // INVOKESTATIC g()（pc0）
    code.insert(code.end(), {0x3a, 0x01}); // ASTORE 1（pc3）
    code.insert(code.end(), {0x19, 0x01}); // ALOAD 1（pc5）
    code.push_back(0xb0);                  // ARETURN（pc7）
    b.addMethod({"m", "()Ljava/lang/Object;", std::move(code), 0x0009, {}, 1, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::RefCopyPropagator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 4u);
}

// ── 来源槽被再写：来源域穿透中途 aload，折叠得比旧栈更远（直达最初 LDC） ──
TEST(RefCopyPropagatorTest, sourceSlotRewrittenFoldsThrough)
{
    FixtureBuilder b("t/C");
    const ca::u16 scp = b.strConst("x");
    Bytes        code = {0x12};            // LDC "x"（pc0）
    code.push_back(static_cast<ca::u8>(scp));
    code.insert(code.end(), {0x3a, 0x01}); // ASTORE 1（pc2）
    code.insert(code.end(), {0x19, 0x01}); // ALOAD 1（pc4）
    code.insert(code.end(), {0x3a, 0x01}); // ASTORE 1（pc6，再写）
    code.insert(code.end(), {0x19, 0x01}); // ALOAD 1（pc8）
    code.push_back(0xb0);                  // ARETURN（pc10）
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 1, 2});
    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::RefCopyPropagator{});
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
    FixtureBuilder b("t/C");
    const ca::u16 scp = b.strConst("x");
    Bytes        code = {0x12};            // LDC "x"
    code.push_back(static_cast<ca::u8>(scp));
    code.insert(code.end(), {0x3a, 0x01}); // ASTORE 1
    code.insert(code.end(), {0x19, 0x01}); // ALOAD 1
    code.push_back(0xb0);                  // ARETURN
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 1, 2});
    const Bytes          bytes = b.assemble();
    const MethodPassRun  r     = runMethodPass(bytes, mjbase::pass::RefCopyPropagator{});
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
