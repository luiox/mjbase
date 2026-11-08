// InsnRearranger（#20 重排家族，方法级两件）：ArithmeticInsnRearranger 把
// 连续 xLOAD;CONST;可交换算术;xSTORE 四元块按槽首现序分组重排；
// CommutativeOperatorReorder 把交换律链内常量操作数后移使常量相邻。
// 覆盖：跨槽归组（含 long/快捷形）、单块/同槽恒等不动、窗口/链起点与
// 内部锚标签守卫、非交换律/异宽操作数/形状不齐保留、幂等、注册面。
//
// 夹具纪律：方法体一律 jasm 文本装配（JvmJasmService），原 FixtureBuilder
// 手拼载体退役。装配器对 0-3 槽自动发快捷形——泛形（2 字节 xLOAD/xSTORE）
// 载体改用 ≥4 槽表达，快捷形载体用 1/2 槽（等价面不变）。被载槽一律以实参
// 形参覆盖（desc 给足 I/J 形参使槽有初始化定义——装配期帧分析拒收未定义
// 槽的 int 型消费），字节语义同实。两处「可达但不合栈」的畸形链
// （aconst_null 混入 int 链 / 形状不齐欠栈）以 goto 越段死代码表达（死段
// 不进帧分析，pass 的结构扫描照判），字节语义同实。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/insn_rearranger.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;

namespace {

/// 方法级夹具便捷形：cls 单 public static m(desc)（excText 为 exceptions 段
/// 条目文本，可空）。
Bytes asmM(const std::string& cls, std::string_view desc, std::string_view codeBody,
           std::string_view excText = "")
{
    const std::string excSeg = excText.empty()
        ? std::string()
        : "    exceptions: {\n" + std::string(excText) + "    },\n";
    const std::string src = R"JASM(.super java/lang/Object
.class public super )JASM" + cls + R"JASM( {
  .method public static m )JASM" + std::string(desc) + R"JASM( {
)JASM" + excSeg + "    code: {\n" + std::string(codeBody) + "    }\n  }\n}\n";
    return assembleJasm(src);
}

/// 四元块文本（loadText 形如 "iload 4"，constText 形如 "iconst_1"，op 为
/// iadd/ladd 家族，storeText 形如 "istore 4"）。
std::string chunk(std::string_view loadText, std::string_view constText,
                  std::string_view op, std::string_view storeText)
{
    std::string s;
    s.reserve(128);
    s.append("      ").append(loadText).push_back('\n');
    s.append("      ").append(constText).push_back('\n');
    s.append("      ").append(op).push_back('\n');
    s.append("      ").append(storeText).push_back('\n');
    return s;
}

}   // namespace

// ── 三块窗口按槽首现序归组：[s4][s5][s4] → [s4][s4][s5] 同槽聚拢 ──
TEST(ArithmeticInsnRearrangerTest, chunksRegroupedBySlot)
{
    // A(slot4 泛形): iload 4; iconst_1; iadd; istore 4
    // B(slot5 泛形): iload 5; iconst_2; iadd; istore 5
    // C(slot4 泛形): iload 4; iconst_1; iadd; istore 4
    const Bytes in = asmM("t/A", "(IIIIII)V",
                          chunk("iload 4", "iconst_1", "iadd", "istore 4")
                              + chunk("iload 5", "iconst_2", "iadd", "istore 5")
                              + chunk("iload 4", "iconst_1", "iadd", "istore 4")
                              + "      return\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    // 归组后 [A][C][B]：第 2 块是 slot4（ICONST_1），第 3 块是 slot5（ICONST_2）。
    // 块各净零栈（xLOAD..xSTORE 相抵），尾 return 空栈收口（()V）。
    ASSERT_EQ(ops.size(), 13u);
    EXPECT_EQ(ops[0], jbc::core::Op::ILOAD);
    EXPECT_EQ(ops[4], jbc::core::Op::ILOAD);
    EXPECT_EQ(ops[5], jbc::core::Op::ICONST_1);
    EXPECT_EQ(ops[8], jbc::core::Op::ILOAD);
    EXPECT_EQ(ops[9], jbc::core::Op::ICONST_2);
}

// ── 单块窗口不动（重排需 ≥2 块） ──
TEST(ArithmeticInsnRearrangerTest, singleChunkKept)
{
    const Bytes in = asmM("t/A", "(II)V",
                          chunk("iload 1", "iconst_1", "iadd", "istore 1")
                              + "      nop\n      nop\n      nop\n      nop\n"
                              "      return\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 9u);
}

// ── 同槽窗口恒等（分组序 = 原序）不改写 ──
TEST(ArithmeticInsnRearrangerTest, sameSlotIdentityKept)
{
    const std::string body = chunk("iload 1", "iconst_1", "iadd", "istore 1");
    const Bytes in = asmM("t/A", "(II)V", body + body + "      return\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 快捷形与泛形同判（旧栈 tree 规范化等价面；1/2 槽装配器自动发快捷形） ──
TEST(ArithmeticInsnRearrangerTest, shorthandChunksRegroup)
{
    // A(slot1 快捷): iload_1; iconst_1; iadd; istore_1
    // B(slot2 快捷): iload_2; iconst_2; iadd; istore_2
    // C(slot1 快捷): iload_1; iconst_1; iadd; istore_1
    const Bytes in = asmM("t/A", "(III)V",
                          chunk("iload 1", "iconst_1", "iadd", "istore 1")
                              + chunk("iload 2", "iconst_2", "iadd", "istore 2")
                              + chunk("iload 1", "iconst_1", "iadd", "istore 1")
                              + "      return\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 13u);
    EXPECT_EQ(ops[4], jbc::core::Op::ILOAD_1);
    EXPECT_EQ(ops[5], jbc::core::Op::ICONST_1);
    EXPECT_EQ(ops[8], jbc::core::Op::ILOAD_2);
    EXPECT_EQ(ops[9], jbc::core::Op::ICONST_2);
}

// ── 窗口起点锚标签（分支目标）→ 整窗拒收 ──
TEST(ArithmeticInsnRearrangerTest, windowStartAnchorKept)
{
    // goto 越过垫步落在窗口起点——GOTO 目标锚在窗口首条，重排会拖走锚点，
    // 拒收。
    const Bytes in = asmM("t/A", "(IIIIII)V",
                          "      L0:\n      goto Lw\n      nop\n      nop\n"
                          "      Lw:\n"
                          + chunk("iload 4", "iconst_2", "iadd", "istore 4")
                          + chunk("iload 5", "iconst_1", "iadd", "istore 5")
                          + "      return\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 窗口内部边界锚标签（异常表起点落在块间）→ 断窗保留 ──
TEST(ArithmeticInsnRearrangerTest, interiorAnchorBreaksWindow)
{
    // [A(slot4)][B(slot5)] + 异常起点锚在 B 首条（窗口内部边界），窗只剩
    // 单块 → 不动。
    const Bytes in = asmM("t/A", "(IIIIII)V",
                          chunk("iload 4", "iconst_2", "iadd", "istore 4")
                              + "      Lb:\n"
                              + chunk("iload 5", "iconst_1", "iadd", "istore 5")
                              + "      Lr:\n      pop\n      return\n",
                          "      { Lb, Lr, Lr, * }\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── long 族窗口归组：[s0][s4][s0] → 槽首现序 [s0][s0][s4] ──
TEST(ArithmeticInsnRearrangerTest, longFamilyRegroup)
{
    // A(slot0 快捷): lload_0; lconst_1; ladd; lstore_0
    // B(slot4 泛形): lload 4; lconst_0; ladd; lstore 4
    // C(slot0 快捷): lload_0; lconst_1; ladd; lstore_0
    const Bytes in = asmM("t/A", "(JJJ)V",
                          chunk("lload_0", "lconst_1", "ladd", "lstore_0")
                              + chunk("lload 4", "lconst_0", "ladd", "lstore 4")
                              + chunk("lload_0", "lconst_1", "ladd", "lstore_0")
                              + "      return\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 13u);
    // 归组后 [A][C][B]：第 3 块是 slot4 泛形 lload。
    EXPECT_EQ(ops[0], jbc::core::Op::LLOAD_0);
    EXPECT_EQ(ops[1], jbc::core::Op::LCONST_1);
    EXPECT_EQ(ops[4], jbc::core::Op::LLOAD_0);
    EXPECT_EQ(ops[5], jbc::core::Op::LCONST_1);
    EXPECT_EQ(ops[8], jbc::core::Op::LLOAD);
    EXPECT_EQ(ops[9], jbc::core::Op::LCONST_0);
}

// ── 交换律链：常量操作数后移 ──
TEST(CommutativeOperatorReorderTest, constantsMovedToTail)
{
    // iconst_1; iload 4; iadd; ireturn → iload 4; iconst_1; iadd; ireturn
    const Bytes in = asmM("t/C", "(IIIII)I",
                          "      iconst_1\n      iload 4\n      iadd\n      ireturn\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::ILOAD);
    EXPECT_EQ(ops[1], jbc::core::Op::ICONST_1);
    EXPECT_EQ(ops[2], jbc::core::Op::IADD);
}

// ── 常量已在链尾 → 恒等不改写 ──
TEST(CommutativeOperatorReorderTest, constantsAlreadyTailKept)
{
    const Bytes in = asmM("t/C", "(IIIII)I",
                          "      iload 4\n      iconst_1\n      iadd\n      ireturn\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 混合链：常量聚到一起（iconst_1, iload, iconst_2 → iload, c1, c2） ──
TEST(CommutativeOperatorReorderTest, mixedChainConstantsGrouped)
{
    // iconst_1; iload 4; iconst_2; iadd; iadd; ireturn
    const Bytes in = asmM("t/C", "(IIIII)I",
                          "      iconst_1\n      iload 4\n      iconst_2\n"
                          "      iadd\n      iadd\n      ireturn\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 6u);
    EXPECT_EQ(ops[0], jbc::core::Op::ILOAD);
    EXPECT_EQ(ops[1], jbc::core::Op::ICONST_1);
    EXPECT_EQ(ops[2], jbc::core::Op::ICONST_2);
    EXPECT_EQ(ops[3], jbc::core::Op::IADD);
    EXPECT_EQ(ops[4], jbc::core::Op::IADD);
}

// ── 非交换律算术不成链 ──
TEST(CommutativeOperatorReorderTest, nonCommutativeKept)
{
    const Bytes in = asmM("t/C", "(IIIII)I",
                          "      iload 4\n      iconst_1\n      isub\n      ireturn\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 异宽操作数（aconst_null 混入 int 链）不成链 ──
// 可达即帧分析拒收——以 goto 越段死代码表达（pass 结构扫描照判）。
TEST(CommutativeOperatorReorderTest, foreignOperandKept)
{
    const Bytes in = asmM("t/C", "(IIIII)I",
                          "      L0:\n      goto Lend\n      Ld:\n"
                          "      iload 4\n      aconst_null\n      iadd\n"
                          "      ireturn\n      Lend:\n      iconst_0\n"
                          "      ireturn\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 链起点锚标签（异常表起点）→ 保守拒收 ──
TEST(CommutativeOperatorReorderTest, chainStartAnchorKept)
{
    // 异常起点锚在链首指令 → 拒收（声明偏离：旧栈列表语义隐式重锚，新栈
    // fail-closed）。
    const Bytes in = asmM("t/C", "(IIIII)I",
                          "      L0:\n      iconst_1\n      iload 4\n      iadd\n"
                          "      Lend:\n      ireturn\n",
                          "      { L0, Lend, Lend, * }\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── long 链常量后移 ──
TEST(CommutativeOperatorReorderTest, longChainConstantsMoved)
{
    // lconst_0; lload 4; ladd; lreturn → lload 4; lconst_0; ladd; lreturn
    const Bytes in = asmM("t/C", "(JJJ)J",
                          "      lconst_0\n      lload 4\n      ladd\n      lreturn\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::LLOAD);
    EXPECT_EQ(ops[1], jbc::core::Op::LCONST_0);
}

// ── 形状不齐（操作数数 ≠ 算术数 + 1）不成链 ──
// 可达即帧分析拒收（第二个 iadd 欠栈）——以 goto 越段死代码表达。
TEST(CommutativeOperatorReorderTest, malformedShapeKept)
{
    const Bytes in = asmM("t/C", "(IIIII)I",
                          "      L0:\n      goto Lend\n      Ld:\n"
                          "      iconst_1\n      iload 4\n      iadd\n      iadd\n"
                          "      ireturn\n      Lend:\n      iconst_0\n"
                          "      ireturn\n");
    const MethodPassRun r =
        runMethodPass(in, mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 幂等：重排产物二轮不改写 ──
TEST(ArithmeticInsnRearrangerTest, idempotent)
{
    const Bytes in = asmM("t/A", "(IIIIII)V",
                          chunk("iload 4", "iconst_1", "iadd", "istore 4")
                              + chunk("iload 5", "iconst_2", "iadd", "istore 5")
                              + chunk("iload 4", "iconst_1", "iadd", "istore 4")
                              + "      return\n");
    const MethodPassRun first =
        runMethodPass(in, mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(first.pa.changed());
    const MethodPassRun second =
        runMethodPass(first.out, mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(second.pa.areAllPreserved());
}

// ── 注册面 ──
TEST(InsnRearrangerRegistryTest, registeredByName)
{
    for (const char* name : {"ArithmeticInsnRearranger", "CommutativeOperatorReorder"}) {
        EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has(name)) << name;
        auto created = mjbase::spi::defaultPassRegistry().createMethodPass(name);
        ASSERT_TRUE(created.is_ok()) << name;
        EXPECT_NE(std::move(created).unwrap(), nullptr) << name;
    }
}
