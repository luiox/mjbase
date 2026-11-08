// TryCatchCleanup：透明重抛三形状删除（共享/可达/重叠守卫）、恒不抛收口、
// NPE 形改写（栈深门槛 + 锚入标签守卫）、自捕抛改写、空区间修复删除、
// 非当前类恒等包装保守不认。
//
// 夹具纪律：方法体一律 jasm 文本装配（JvmJasmService），原 FixtureBuilder
// 手拼载体退役。异常表为 jasm 原生面（exceptions: 段），无例外申报。原字节
// 级夹具的两处「标签锚进操作数中部」技巧形按语义合法重表达：
// - 透明 handler 正常可达：ifeq 直接以分支锚 handler 首（双标签同址
//   Lb/Lh——分支目标与 handler 锚同指令，普通流帧与 handler 入栈帧各自
//   独立成根）；
// - NPE 形 foreign-label 守卫：athrow 上双标签同址（分支锚 + start+1 位），
//   分支载荷与直落路径同形（aconst_null）保帧一致。
#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/try_catch_cleanup.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 方法级夹具便捷形：t/C 单 public static m()V（exceptions 段 + code 体文本；
/// 异常条目类型写描述符形 L...;，全捕获 *）。extra 为附加方法声明文本
///（identity wrap 载体用）。
Bytes asmM(std::string_view excText, std::string_view codeBody,
           std::string_view extra = "")
{
    const std::string src = R"JASM(.super java/lang/Object
.class public super t/C {
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

std::vector<Op> opsOfOut(const MethodPassRun& r)
{
    return opsOf(inspect(r.out));
}

}   // namespace

// ── 透明重抛（直接 athrow）→ 条目删除 ──
TEST(TryCatchCleanupTest, transparentDirectAthrowRemoved)
{
    const MethodPassRun r = runMethodPass(asmM("      { L0, L1, L1, * }\n",
                                              "      L0:\n      aconst_null\n"
                                              "      athrow\n      L1:\n      athrow\n"
                                              "      return\n"),
                                          mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 透明重抛（同槽 astore/aload）→ 条目删除 ──
TEST(TryCatchCleanupTest, transparentAstoreAloadRemoved)
{
    const MethodPassRun r = runMethodPass(asmM("      { L0, L1, L1, * }\n",
                                              "      L0:\n      aconst_null\n"
                                              "      athrow\n      L1:\n      astore 0\n"
                                              "      aload 0\n      athrow\n"
                                              "      return\n"),
                                          mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 透明重抛（当前类恒等静态包装）→ 条目删除 ──
TEST(TryCatchCleanupTest, transparentIdentityWrapperRemoved)
{
    const MethodPassRun r = runMethodPass(
        asmM("      { L0, L1, L1, * }\n",
             "      L0:\n      aconst_null\n      athrow\n      L1:\n"
             "      invokestatic t/C.wrap (Ljava/lang/Throwable;)Ljava/lang/Throwable;\n"
             "      athrow\n      return\n",
             R"JASM(
  .method public static wrap (Ljava/lang/Throwable;)Ljava/lang/Throwable; {
    code: {
      aload 0
      areturn
    }
  }
)JASM"),
        mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 非当前类 owner 的包装调用：保守不认 → 保留 ──
TEST(TryCatchCleanupTest, identityWrapperForeignOwnerKept)
{
    const MethodPassRun r = runMethodPass(
        asmM("      { L0, L1, L1, * }\n",
             "      L0:\n      aconst_null\n      athrow\n      L1:\n"
             "      invokestatic other/C.wrap (Ljava/lang/Throwable;)Ljava/lang/Throwable;\n"
             "      athrow\n      return\n",
             R"JASM(
  .method wrap (Ljava/lang/Throwable;)Ljava/lang/Throwable; {
    code: {
      aload 0
      areturn
    }
  }
)JASM"),
        mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── handler 被两条目共享 → 透明面拒收（条目1 保留；条目2 保护区为死代码，被恒不抛面删） ──
TEST(TryCatchCleanupTest, transparentSharedHandlerEntry1Kept)
{
    const MethodPassRun r =
        runMethodPass(asmM("      { L0, L1, L2, * }\n      { L1, L2, L2, * }\n",
                           "      L0:\n      aconst_null\n      athrow\n"
                           "      L1:\n      aconst_null\n      athrow\n"
                           "      L2:\n      athrow\n      return\n"),
                      mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);   // 条目1（共享）保留
}

// ── handler 正常流可达 → 透明面拒收 ──
// 合法重表达：ifeq 分支锚 handler 首（Lb 与 handler 锚 Lh 双标签同址，
// 分支与 handler 入帧各自独立成根），normalReach 面与原字节技巧形同实。
TEST(TryCatchCleanupTest, transparentReachableHandlerKept)
{
    const MethodPassRun r = runMethodPass(asmM("      { L0, Lh, Lh, * }\n",
                                              "      L0:\n      iconst_0\n"
                                              "      ifeq Lb\n      athrow\n"
                                              "      Lb:\n      Lh:\n      athrow\n"
                                              "      return\n"),
                                          mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── 恒不抛段收口：区间内无可达可抛指令 → 条目删除 ──
TEST(TryCatchCleanupTest, impossibleCatchRemoved)
{
    const MethodPassRun r = runMethodPass(asmM("      { L0, L1, L1, * }\n",
                                              "      L0:\n      iconst_0\n"
                                              "      iconst_1\n      L1:\n      pop\n"
                                              "      return\n"),
                                          mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 区间内含可达可抛指令（ATHROW）→ 保留 ──
TEST(TryCatchCleanupTest, impossibleCatchWithThrowKept)
{
    const MethodPassRun r = runMethodPass(asmM("      { L0, L1, L1, * }\n",
                                              "      L0:\n      iconst_0\n"
                                              "      athrow\n      L1:\n      pop\n"
                                              "      return\n"),
                                          mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── NPE 形：aconst_null;athrow + pop;goto handler → aconst_null 改 goto 续点 ──
TEST(TryCatchCleanupTest, npeShapeRewritten)
{
    const MethodPassRun r = runMethodPass(asmM("      { L0, L1, L1, Ljava/lang/NullPointerException; }\n",
                                              "      L0:\n      aconst_null\n"
                                              "      athrow\n      L1:\n      pop\n"
                                              "      goto Lcont\n      Lcont:\n"
                                              "      iconst_0\n      return\n"),
                                          mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    ASSERT_EQ(ops.size(), 5u);   // aconst_null+athrow → 单 GOTO
    EXPECT_EQ(ops[0], Op::GOTO); // aconst_null → goto 续点
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── NPE 形 + 保护区入口栈积压（depth≠0）→ 拒收 ──
TEST(TryCatchCleanupTest, npeShapeJunkStackKept)
{
    const MethodPassRun r =
        runMethodPass(asmM("      { L1, L2, L2, Ljava/lang/NullPointerException; }\n",
                           "      L0:\n      sipush 3\n      L1:\n      aconst_null\n"
                           "      athrow\n      L2:\n      pop\n      goto Lcont\n"
                           "      Lcont:\n      return\n"),
                      mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── NPE 形 + athrow 被分支锚入 → 拒收 ──
// 合法重表达：athrow 上双标签同址（Lx 分支锚 + Lthrow 锚），分支载荷与直落
// 路径同形（aconst_null）保帧一致——foreign-label 守卫面同实。
TEST(TryCatchCleanupTest, npeShapeForeignLabelKept)
{
    const MethodPassRun r =
        runMethodPass(asmM("      { Lp, L2, L2, Ljava/lang/NullPointerException; }\n",
                           "      L0:\n      aconst_null\n      goto Lx\n"
                           "      Lp:\n      aconst_null\n      Lx:\n      Lthrow:\n"
                           "      athrow\n      L2:\n      pop\n      goto Lcont\n"
                           "      Lcont:\n      return\n"),
                      mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── 自捕抛：new E;dup;invokespecial;athrow 被自身 handler 接住 → athrow 改 goto handler ──
TEST(TryCatchCleanupTest, selfCaughtThrowRewritten)
{
    const MethodPassRun r = runMethodPass(asmM("      { L0, L1, L1, Lt/E; }\n",
                                              "      L0:\n      new t/E\n      dup\n"
                                              "      invokespecial t/E.<init> ()V\n"
                                              "      athrow\n      L1:\n      pop\n"
                                              "      return\n"),
                                          mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[0], Op::NEW);
    EXPECT_EQ(ops[1], Op::DUP);
    EXPECT_EQ(ops[2], Op::INVOKESPECIAL);
    EXPECT_EQ(ops[3], Op::GOTO);   // athrow → goto handler
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 自捕抛 + athrow 处栈深 2 → 拒收 ──
TEST(TryCatchCleanupTest, selfCaughtDeepStackKept)
{
    const MethodPassRun r =
        runMethodPass(asmM("      { L1, L2, L2, Lt/E; }\n",
                           "      L0:\n      sipush 9\n      L1:\n      new t/E\n"
                           "      dup\n      invokespecial t/E.<init> ()V\n"
                           "      athrow\n      L2:\n      pop\n      return\n"),
                      mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── 空区间条目修复性删除 ──
TEST(TryCatchCleanupTest, emptyRangeRemoved)
{
    const MethodPassRun r = runMethodPass(asmM("      { L0, L0, L0, * }\n",
                                              "      L0:\n      iconst_0\n"
                                              "      return\n"),
                                          mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 幂等 ──
TEST(TryCatchCleanupTest, idempotent)
{
    const Bytes bytes = asmM("      { L0, L1, L1, * }\n",
                             "      L0:\n      aconst_null\n      athrow\n"
                             "      L1:\n      athrow\n      return\n");
    const MethodPassRun r = runMethodPass(bytes, mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const MethodPassRun r2 = runMethodPass(r.out, mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── 注册面 ──
TEST(TryCatchCleanupTest, registeredInRegistry)
{
    const mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    const auto                           e   = reg.find("TryCatchCleanup");
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.unwrap().scope, mjbase::spi::PassScope::Method);
}
