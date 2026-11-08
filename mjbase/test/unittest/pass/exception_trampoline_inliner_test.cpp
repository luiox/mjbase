// ExceptionTrampolineInliner：异常流混淆「aconst_null; athrow → 紧邻 catch-any
// handler → pop」跳板消解 + 各拒绝面 + 栈深门 + 幂等 + 注册面。
//
// 夹具纪律：方法体一律 jasm 文本装配（JvmJasmService），原 FixtureBuilder
// 手拼载体退役；异常表走 jasm 原生 exceptions: 段（catch-any 写 *），无例外
// 申报。原字节级「死代码 goto 锚中腹」技巧形（athrow / pop 为分支目标的
// 拒绝面）以 return 后死段 goto 合法重表达——死段不进帧分析、分支引用照计。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/exception_trampoline_inliner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 方法级夹具便捷形：t/E 单 public static m()V（exceptions 段 + code 体文本；
/// extra 为附加方法声明文本，sibling 用例挂 plain 方法）。
Bytes asmM(std::string_view excText, std::string_view codeBody,
           std::string_view extra = "")
{
    const std::string src = R"JASM(.super java/lang/Object
.class public super t/E {
  .method public static m ()V {
    exceptions: {
)JASM" + std::string(excText) + R"JASM(    },
    code: {
)JASM" + std::string(codeBody) + R"JASM(    }
  }
)JASM" + std::string(extra) + R"JASM(}
)JASM";
    return assembleJasm(src);
}

/// 最小跳板体：aconst_null; athrow → pop; iconst_1; pop; return。
std::string_view trampolineBody()
{
    return "      L0:\n      aconst_null\n      athrow\n      L1:\n      pop\n"
           "      iconst_1\n      pop\n      return\n";
}

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
    const MethodPassRun r =
        runMethodPass(asmM("      { L0, L1, L1, * }\n", trampolineBody()),
                      mjbase::pass::ExceptionTrampolineInliner{});

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
    const MethodPassRun r = runMethodPass(
        asmM("      { L1, L2, L2, * }\n",
             "      L0:\n      iconst_1\n      pop\n      goto L1\n"
             "      L1:\n      aconst_null\n      athrow\n      L2:\n"
             "      pop\n      iconst_1\n      pop\n      return\n"),
        mjbase::pass::ExceptionTrampolineInliner{});

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
    const MethodPassRun r = runMethodPass(
        asmM("      { L1, L2, L2, * }\n",
             "      L0:\n      goto L1\n      L1:\n      aconst_null\n      athrow\n"
             "      L2:\n      pop\n      ldc 111111111\n      istore 1\n"
             "      iload 1\n      istore 2\n      return\n"),
        mjbase::pass::ExceptionTrampolineInliner{});

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
    const MethodPassRun r = runMethodPass(
        asmM("      { L1, L2, L2, * }\n      { L3, L4, L4, * }\n",
             "      L0:\n      iconst_1\n      pop\n      goto L1\n"
             "      L1:\n      aconst_null\n      athrow\n      L2:\n      pop\n"
             "      goto L3\n      L3:\n      aconst_null\n      athrow\n"
             "      L4:\n      pop\n      iconst_1\n      pop\n      return\n"),
        mjbase::pass::ExceptionTrampolineInliner{});

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
    const Bytes in = asmM("      { L0, L1, L1, Ljava/lang/Exception; }\n", trampolineBody());
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_FALSE(r.pa.changed());
    EXPECT_EQ(r.out, in);
}

// ── handler 与 end 不同锚（非紧贴形态）：不碰 ──
TEST(ExceptionTrampolineInlinerTest, nonAdjacentHandlerUntouched)
{
    const Bytes in = asmM("      { L0, L1, L2, * }\n",
                          "      L0:\n      aconst_null\n      athrow\n"
                          "      L1:\n      iconst_1\n      L2:\n      pop\n"
                          "      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 保护区多一条指令：不碰 ──
TEST(ExceptionTrampolineInlinerTest, oversizedProtectedRangeUntouched)
{
    const Bytes in = asmM("      { L0, L2, L2, * }\n",
                          "      L0:\n      nop\n      aconst_null\n      athrow\n"
                          "      L2:\n      pop\n      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── handler 首指令非 POP（astore 收尾的类型化 handler 形态）：不碰 ──
TEST(ExceptionTrampolineInlinerTest, handlerHeadNotPopUntouched)
{
    const Bytes in = asmM("      { L0, L1, L1, * }\n",
                          "      L0:\n      aconst_null\n      athrow\n"
                          "      L1:\n      astore 1\n      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── athrow 为分支目标：绕过 aconst_null 的路径栈形不符，不碰 ──
TEST(ExceptionTrampolineInlinerTest, athrowAsBranchTargetUntouched)
{
    // 死代码 goto L1（athrow）作为静态引用仍构成 use（return 后死段不进帧分析）。
    const Bytes in = asmM("      { L0, L1, L1, * }\n",
                          "      L0:\n      aconst_null\n      L1:\n      athrow\n"
                          "      L2:\n      pop\n      iconst_1\n      pop\n"
                          "      return\n      Ldead:\n      goto L1\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── handler 头 POP 为分支目标：不碰 ──
TEST(ExceptionTrampolineInlinerTest, popAsBranchTargetUntouched)
{
    // iconst_1; pop; [01 bf 57]; iconst_1; pop; return; 死代码 goto L2（pop）。
    const Bytes in = asmM("      { L1, L2, L2, * }\n",
                          "      L0:\n      iconst_1\n      pop\n      L1:\n"
                          "      aconst_null\n      athrow\n      L2:\n      pop\n"
                          "      iconst_1\n      pop\n      return\n"
                          "      Ldead:\n      goto L2\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 入口栈深非 0：athrow 清栈语义由直落替代后会残留，不碰 ──
TEST(ExceptionTrampolineInlinerTest, nonZeroEntryStackUntouched)
{
    // iconst_1（残留）; aconst_null; athrow; pop; iconst_1; pop; return
    const Bytes in = asmM("      { L1, L2, L2, * }\n",
                          "      L0:\n      iconst_1\n      L1:\n      aconst_null\n"
                          "      athrow\n      L2:\n      pop\n      iconst_1\n"
                          "      pop\n      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 外层交叠 try：首匹配语义不可拆，不碰 ──
TEST(ExceptionTrampolineInlinerTest, overlappingEnclosingTryUntouched)
{
    const Bytes in = asmM("      { L1, L2, L2, * }\n      { Lg, Lend, Lend, * }\n",
                          "      L0:\n      iconst_1\n      pop\n      Lg:\n"
                          "      goto L1\n      L1:\n      aconst_null\n      athrow\n"
                          "      L2:\n      pop\n      Lend:\n      iconst_1\n"
                          "      pop\n      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 真 try/catch（保护区内实调用 + 类型化 handler）：不碰 ──
TEST(ExceptionTrampolineInlinerTest, realTryCatchPreserved)
{
    const Bytes in = asmM(
        "      { L0, L1, L2, Ljava/lang/ClassNotFoundException; }\n",
        "      L0:\n      aconst_null\n"
        "      invokestatic java/lang/Class.forName (Ljava/lang/String;)Ljava/lang/Class;\n"
        "      L1:\n      pop\n      return\n      L2:\n      astore 1\n      return\n");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 同类多方法隔离：只有跳板方法被改写 ──
TEST(ExceptionTrampolineInlinerTest, siblingMethodUntouched)
{
    const MethodPassRun r = runMethodPass(
        asmM("      { L0, L1, L1, * }\n", trampolineBody(),
             R"JASM(
  .method plain ()V {
    code: {
      iconst_1
      pop
      return
    }
  }
)JASM"),
        mjbase::pass::ExceptionTrampolineInliner{});

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
    const Bytes in = asmM("      { L1, L2, L2, * }\n      { L3, L4, L4, * }\n",
                          "      L0:\n      iconst_1\n      pop\n      goto L1\n"
                          "      L1:\n      aconst_null\n      athrow\n      L2:\n"
                          "      pop\n      goto L3\n      L3:\n      aconst_null\n"
                          "      athrow\n      L4:\n      pop\n      iconst_1\n"
                          "      pop\n      return\n");
    const MethodPassRun r1 = runMethodPass(in, mjbase::pass::ExceptionTrampolineInliner{});
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
