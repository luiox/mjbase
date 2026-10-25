// ExceptionTrampolineInliner：异常流混淆「aconst_null; athrow → 紧邻 catch-any
// handler → pop」跳板消解 + 各拒绝面 + 栈深门 + 幂等 + 注册面。
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/exception_trampoline_inliner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 解析 branch 首目标锚定的指令句柄（目标必须是指令边界）。
jbc::ir::InsnId resolvedTarget(const Inspection& ins, const jbc::ir::Insn* branch)
{
    auto resolved = ins.cu->dom.resolveLabel(branch->target);
    EXPECT_TRUE(resolved.is_ok());
    return std::move(resolved).unwrap().insn;
}

}   // namespace

// ── 最小跳板：方法入口直落形态，整组消解 ──
TEST(ExceptionTrampolineInlinerTest, dissolvesEntryTrampoline)
{
    FixtureBuilder b("t/E");
    b.addMethod({"m", "()V", {0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1}, 0x0009, {{0, 2, 2, 0}}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.changed());
    EXPECT_FALSE(r.pa.areAllPreserved());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── goto 进入跳板：分支重定向到 handler 体首指令 ──
TEST(ExceptionTrampolineInlinerTest, gotoIntoTrampolineRetargetsToHandlerBody)
{
    FixtureBuilder b("t/E");
    // iconst_1; pop; goto 5; aconst_null; athrow; pop; iconst_1; pop; return
    b.addMethod({"m", "()V",
                 {0x04, 0x57, 0xa7, 0x00, 0x03, 0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1},
                 0x0009, {{5, 7, 7, 0}}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 6u);   // iconst_1, pop, GOTO, iconst_1, pop, return
    EXPECT_EQ(ops[2], Op::GOTO);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    EXPECT_EQ(resolvedTarget(ins, ins.cu->dom.peek(ids[2])), ids[3]);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 状态更新形态：handler 体带状态更新（ldc/istore 中转）照样消解 ──
TEST(ExceptionTrampolineInlinerTest, dissolvesStateUpdatingHandler)
{
    FixtureBuilder b("t/E");
    const ca::u16 k = b.intConst(111111111);
    // goto 3; aconst_null; athrow; pop; ldc K; istore_1; iload_1; istore_2; return
    b.addMethod({"m", "()V",
                 {0xa7, 0x00, 0x03, 0x01, 0xbf, 0x57, 0x13,
                  static_cast<ca::u8>(k >> 8), static_cast<ca::u8>(k & 0xff),
                  0x3c, 0x1b, 0x3d, 0xb1},
                 0x0009, {{3, 5, 5, 0}}, 2, 3});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 6u);   // GOTO, LDC, ISTORE_1, ILOAD_1, ISTORE_2, RETURN
    EXPECT_EQ(ops[0], Op::GOTO);
    EXPECT_TRUE(ops[1] == Op::LDC || ops[1] == Op::LDC_W);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    EXPECT_EQ(resolvedTarget(ins, ins.cu->dom.peek(ids[0])), ids[1]);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 链式跳板（goto 链穿过多个跳板的微缩）：逐项独立消解 ──
TEST(ExceptionTrampolineInlinerTest, dissolvesChainedTrampolines)
{
    FixtureBuilder b("t/E");
    // iconst_1; pop; goto 5; [01 bf 57]; goto 11; [01 bf 57]; iconst_1; pop; return
    b.addMethod({"m", "()V",
                 {0x04, 0x57, 0xa7, 0x00, 0x03, 0x01, 0xbf, 0x57,
                  0xa7, 0x00, 0x03, 0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1},
                 0x0009, {{5, 7, 7, 0}, {11, 13, 13, 0}}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 7u);   // iconst_1, pop, GOTO, GOTO, iconst_1, pop, return
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    // 前跳板的入口 goto 重定向到第二个 goto（原 handler 体首条）；第二个
    // goto 重定向到最终 handler 体首条。
    EXPECT_EQ(resolvedTarget(ins, ins.cu->dom.peek(ids[2])), ids[3]);
    EXPECT_EQ(resolvedTarget(ins, ins.cu->dom.peek(ids[3])), ids[4]);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── catch 类型非 any：不碰（copy-through 保真）──
TEST(ExceptionTrampolineInlinerTest, typedCatchTrampolineUntouched)
{
    FixtureBuilder b("t/E");
    const ca::u16 cls = b.cls("java/lang/Exception");
    b.addMethod({"m", "()V", {0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1}, 0x0009, {{0, 2, 2, cls}}});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_FALSE(r.pa.changed());
    EXPECT_EQ(r.out, in);
}

// ── handler 与 end 不同锚（非紧贴形态）：不碰 ──
TEST(ExceptionTrampolineInlinerTest, nonAdjacentHandlerUntouched)
{
    FixtureBuilder b("t/E");
    b.addMethod({"m", "()V", {0x01, 0xbf, 0x04, 0x57, 0xb1}, 0x0009, {{0, 2, 3, 0}}});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 保护区多一条指令：不碰 ──
TEST(ExceptionTrampolineInlinerTest, oversizedProtectedRangeUntouched)
{
    FixtureBuilder b("t/E");
    b.addMethod({"m", "()V", {0x00, 0x01, 0xbf, 0x57, 0xb1}, 0x0009, {{0, 3, 3, 0}}});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── handler 首指令非 POP（astore 收尾的类型化 handler 形态）：不碰 ──
TEST(ExceptionTrampolineInlinerTest, handlerHeadNotPopUntouched)
{
    FixtureBuilder b("t/E");
    b.addMethod({"m", "()V", {0x01, 0xbf, 0x4c, 0xb1}, 0x0009, {{0, 2, 2, 0}}, 2, 2});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── athrow 为分支目标：绕过 aconst_null 的路径栈形不符，不碰 ──
TEST(ExceptionTrampolineInlinerTest, athrowAsBranchTargetUntouched)
{
    FixtureBuilder b("t/E");
    // 死代码 goto 1（athrow）作为静态引用仍构成 use。
    b.addMethod({"m", "()V",
                 {0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1, 0xa7, 0xff, 0xfb},
                 0x0009, {{0, 2, 2, 0}}});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── handler 头 POP 为分支目标：不碰 ──
TEST(ExceptionTrampolineInlinerTest, popAsBranchTargetUntouched)
{
    FixtureBuilder b("t/E");
    // iconst_1; pop; [01 bf 57]; iconst_1; pop; return; 死代码 goto 4（pop）。
    b.addMethod({"m", "()V",
                 {0x04, 0x57, 0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1, 0xa7, 0xff, 0xfc},
                 0x0009, {{2, 4, 4, 0}}});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 入口栈深非 0：athrow 清栈语义由直落替代后会残留，不碰 ──
TEST(ExceptionTrampolineInlinerTest, nonZeroEntryStackUntouched)
{
    FixtureBuilder b("t/E");
    // iconst_1（残留）; aconst_null; athrow; pop; iconst_1; pop; return
    b.addMethod({"m", "()V",
                 {0x04, 0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1},
                 0x0009, {{1, 3, 3, 0}}});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 外层交叠 try：首匹配语义不可拆，不碰 ──
TEST(ExceptionTrampolineInlinerTest, overlappingEnclosingTryUntouched)
{
    FixtureBuilder b("t/E");
    b.addMethod({"m", "()V",
                 {0x04, 0x57, 0xa7, 0x00, 0x03, 0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1},
                 0x0009, {{5, 7, 7, 0}, {2, 8, 8, 0}}});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 真 try/catch（保护区内实调用 + 类型化 handler）：不碰 ──
TEST(ExceptionTrampolineInlinerTest, realTryCatchPreserved)
{
    FixtureBuilder b("t/E");
    const ca::u16 mref = b.methodRef("java/lang/Class", "forName",
                                     "(Ljava/lang/String;)Ljava/lang/Class;");
    const ca::u16 cls  = b.cls("java/lang/ClassNotFoundException");
    // aconst_null; invokestatic Class.forName; pop; return; astore_1; return
    b.addMethod({"m", "()V",
                 {0x01, 0xb8, static_cast<ca::u8>(mref >> 8), static_cast<ca::u8>(mref & 0xff),
                  0x57, 0xb1, 0x4c, 0xb1},
                 0x0009, {{0, 4, 6, cls}}, 2, 2});
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 同类多方法隔离：只有跳板方法被改写 ──
TEST(ExceptionTrampolineInlinerTest, siblingMethodUntouched)
{
    FixtureBuilder b("t/E");
    b.addMethod({"m", "()V", {0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1}, 0x0009, {{0, 2, 2, 0}}});
    b.addMethod({"plain", "()V", {0x04, 0x57, 0xb1}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins0 = inspect(r.out, 0);
    const Inspection      ins1 = inspect(r.out, 1);
    const std::vector<Op> ops0 = opsOf(ins0);
    const std::vector<Op> ops1 = opsOf(ins1);
    ASSERT_EQ(ops0.size(), 3u);   // 跳板消解
    EXPECT_TRUE(ins0.cu->exc.empty());
    ASSERT_EQ(ops1.size(), 3u);   // 普通方法原样
    EXPECT_EQ(ops1[0], Op::ICONST_1);
    EXPECT_EQ(ops1[2], Op::RETURN);
}

// ── 幂等：二轮无匹配，PA=all 且逐字节一致 ──
TEST(ExceptionTrampolineInlinerTest, idempotentOnSecondRun)
{
    FixtureBuilder b("t/E");
    b.addMethod({"m", "()V",
                 {0x04, 0x57, 0xa7, 0x00, 0x03, 0x01, 0xbf, 0x57,
                  0xa7, 0x00, 0x03, 0x01, 0xbf, 0x57, 0x04, 0x57, 0xb1},
                 0x0009, {{5, 7, 7, 0}, {11, 13, 13, 0}}});
    const MethodPassRun r1 = runMethodPass(b.assemble(), mjbase::pass::ExceptionTrampolineInliner{});
    ASSERT_TRUE(r1.pa.changed());

    const MethodPassRun r2 = runMethodPass(r1.out, mjbase::pass::ExceptionTrampolineInliner{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
    EXPECT_FALSE(r2.pa.changed());
    EXPECT_EQ(r2.out, r1.out);
}

// ── 注册面 ──
TEST(ExceptionTrampolineInlinerTest, registeredAsExceptionTrampolineInliner)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("ExceptionTrampolineInliner"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("ExceptionTrampolineInliner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
