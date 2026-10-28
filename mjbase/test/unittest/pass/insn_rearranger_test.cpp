// InsnRearranger（#20 重排家族，方法级两件）：ArithmeticInsnRearranger 把
// 连续 xLOAD;CONST;可交换算术;xSTORE 四元块按槽首现序分组重排；
// CommutativeOperatorReorder 把交换律链内常量操作数后移使常量相邻。
// 覆盖：跨槽归组（含 long/快捷形）、单块/同槽恒等不动、窗口/链起点与
// 内部锚标签守卫、非交换律/异宽操作数/形状不齐保留、幂等、注册面。
#include <gtest/gtest.h>

#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/insn_rearranger.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"
#include "pass/method_pass_test_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublicStatic = 0x0009;

// i 泛形 load/store（2 字节）与快捷形。
constexpr ca::u8 kILoad  = 0x15;
constexpr ca::u8 kILoad1 = 0x1b;
constexpr ca::u8 kILoad2 = 0x1c;
constexpr ca::u8 kIStore  = 0x36;
constexpr ca::u8 kIStore1 = 0x3c;
constexpr ca::u8 kIStore2 = 0x3d;
// l 族。
constexpr ca::u8 kLLoad   = 0x16;
constexpr ca::u8 kLLoad0  = 0x1e;
constexpr ca::u8 kLStore  = 0x37;
constexpr ca::u8 kLStore0 = 0x3f;
constexpr ca::u8 kLConst0 = 0x09;
constexpr ca::u8 kLConst1 = 0x0a;
// 常量与算术。
constexpr ca::u8 kIConst1 = 0x04;
constexpr ca::u8 kIConst2 = 0x05;
constexpr ca::u8 kIAdd = 0x60;
constexpr ca::u8 kLAdd = 0x61;
constexpr ca::u8 kISub = 0x64;
constexpr ca::u8 kAReturn  = 0xac;
constexpr ca::u8 kLReturn  = 0xad;
constexpr ca::u8 kReturn   = 0xb1;
constexpr ca::u8 kGoto     = 0xa7;
constexpr ca::u8 kNop      = 0x00;
constexpr ca::u8 kAconstNull = 0x01;

Bytes cat(Bytes a, Bytes b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/// 泛形局部指令（操作码 + 槽位）。
Bytes var(ca::u8 op, ca::u8 slot) { return {op, slot}; }

FixtureMethod method(std::string_view desc, Bytes code, ca::u16 maxStack,
                     ca::u16 maxLocals, std::vector<ExcSpec> exc = {})
{
    FixtureMethod m;
    m.name      = "m";
    m.desc      = desc;
    m.access    = kAccPublicStatic;
    m.maxStack  = maxStack;
    m.maxLocals = maxLocals;
    m.code      = std::move(code);
    m.exc       = std::move(exc);
    return m;
}

}   // namespace

// ── 三块窗口按槽首现序归组：[s2][s1][s2] → [s1][s2][s2] 同槽聚拢 ──
TEST(ArithmeticInsnRearrangerTest, chunksRegroupedBySlot)
{
    FixtureBuilder b("t/A");
    // A(slot1): iload 1; iconst_1; iadd; istore 1
    // B(slot2): iload 2; iconst_2; iadd; istore 2
    // C(slot1): iload 1; iconst_1; iadd; istore 1
    Bytes code = cat(cat(var(kILoad, 1), {kIConst1, kIAdd}), var(kIStore, 1));
    code = cat(code, cat(cat(var(kILoad, 2), {kIConst2, kIAdd}), var(kIStore, 2)));
    code = cat(code, cat(cat(var(kILoad, 1), {kIConst1, kIAdd}), var(kIStore, 1)));
    b.addMethod(method("()I", std::move(code), 2, 3));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    // 归组后 [A][C][B]：第 2 块是 slot1（ICONST_1），第 3 块是 slot2（ICONST_2）。
    ASSERT_EQ(ops.size(), 12u);
    EXPECT_EQ(ops[0], jbc::core::Op::ILOAD);
    EXPECT_EQ(ops[4], jbc::core::Op::ILOAD);
    EXPECT_EQ(ops[5], jbc::core::Op::ICONST_1);
    EXPECT_EQ(ops[8], jbc::core::Op::ILOAD);
    EXPECT_EQ(ops[9], jbc::core::Op::ICONST_2);
}

// ── 单块窗口不动（重排需 ≥2 块） ──
TEST(ArithmeticInsnRearrangerTest, singleChunkKept)
{
    FixtureBuilder b("t/A");
    Bytes code = cat(cat(var(kILoad, 1), {kIConst1, kIAdd}), var(kIStore, 1));
    code = cat(code, Bytes{kNop, kNop, kNop, kNop, kAReturn});
    b.addMethod(method("()I", std::move(code), 2, 2));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOf(inspect(r.out)).size(), 9u);
}

// ── 同槽窗口恒等（分组序 = 原序）不改写 ──
TEST(ArithmeticInsnRearrangerTest, sameSlotIdentityKept)
{
    FixtureBuilder b("t/A");
    Bytes chunk = cat(cat(var(kILoad, 1), {kIConst1, kIAdd}), var(kIStore, 1));
    b.addMethod(method("()I", cat(chunk, chunk), 2, 2));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 快捷形与泛形同判（旧栈 tree 规范化等价面） ──
TEST(ArithmeticInsnRearrangerTest, shorthandChunksRegroup)
{
    FixtureBuilder b("t/A");
    // A(slot1 快捷): iload_1; iconst_1; iadd; istore_1
    // B(slot2 快捷): iload_2; iconst_2; iadd; istore_2
    // C(slot1 快捷): iload_1; iconst_1; iadd; istore_1
    Bytes code = cat({kILoad1, kIConst1, kIAdd, kIStore1},
                     cat({kILoad2, kIConst2, kIAdd, kIStore2},
                         {kILoad1, kIConst1, kIAdd, kIStore1}));
    b.addMethod(method("()I", std::move(code), 2, 3));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 12u);
    EXPECT_EQ(ops[4], jbc::core::Op::ILOAD_1);
    EXPECT_EQ(ops[5], jbc::core::Op::ICONST_1);
    EXPECT_EQ(ops[8], jbc::core::Op::ILOAD_2);
    EXPECT_EQ(ops[9], jbc::core::Op::ICONST_2);
}

// ── 窗口起点锚标签（分支目标）→ 整窗拒收 ──
TEST(ArithmeticInsnRearrangerTest, windowStartAnchorKept)
{
    FixtureBuilder b("t/A");
    // goto pc5; nop; nop; [A(slot2)][B(slot1)]; ireturn —— GOTO 目标锚在
    // 窗口起点，重排会拖走锚点，拒收。
    Bytes code = {kGoto, 0x00, 0x05, kNop, kNop};
    code = cat(code, cat(cat(var(kILoad, 2), {kIConst2, kIAdd}), var(kIStore, 2)));
    code = cat(code, cat(cat(var(kILoad, 1), {kIConst1, kIAdd}), var(kIStore, 1)));
    code.push_back(kAReturn);
    b.addMethod(method("()I", std::move(code), 2, 3));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 窗口内部边界锚标签（异常表起点落在块间）→ 断窗保留 ──
TEST(ArithmeticInsnRearrangerTest, interiorAnchorBreaksWindow)
{
    FixtureBuilder b("t/A");
    // [A(slot2)][B(slot1)] + exc(4,8,8)：异常起点锚在 B 首条（窗口内部
    // 边界），窗只剩单块 → 不动。
    Bytes code = cat(cat(var(kILoad, 2), {kIConst2, kIAdd}), var(kIStore, 2));
    code = cat(code, cat(cat(var(kILoad, 1), {kIConst1, kIAdd}), var(kIStore, 1)));
    code.push_back(kAReturn);
    b.addMethod(method("()I", std::move(code), 2, 3, {ExcSpec{4, 8, 8, 0}}));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── long 族窗口归组：[s0][s2][s0] → 槽首现序 [s0][s0][s2] ──
TEST(ArithmeticInsnRearrangerTest, longFamilyRegroup)
{
    FixtureBuilder b("t/A");
    // A(slot0 快捷): lload_0; lconst_1; ladd; lstore_0
    // B(slot2 泛形): lload 2; lconst_0; ladd; lstore 2
    // C(slot0 快捷): lload_0; lconst_1; ladd; lstore_0
    Bytes code = Bytes{kLLoad0, kLConst1, kLAdd, kLStore0};
    code = cat(code, cat(var(kLLoad, 2), Bytes{kLConst0, kLAdd}));
    code = cat(code, var(kLStore, 2));
    code = cat(code, Bytes{kLLoad0, kLConst1, kLAdd, kLStore0});
    b.addMethod(method("()J", std::move(code), 4, 4));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::ArithmeticInsnRearranger{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 12u);
    // 归组后 [A][C][B]：第 3 块是 slot2 泛形 lload。
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
    FixtureBuilder b("t/C");
    // iconst_1; iload 1; iadd; ireturn → iload 1; iconst_1; iadd; ireturn
    Bytes code = {kIConst1};
    code = cat(code, cat(var(kILoad, 1), Bytes{kIAdd, kAReturn}));
    b.addMethod(method("()I", std::move(code), 2, 2));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::CommutativeOperatorReorder{});
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
    FixtureBuilder b("t/C");
    Bytes code = cat(var(kILoad, 1), Bytes{kIConst1, kIAdd, kAReturn});
    b.addMethod(method("()I", std::move(code), 2, 2));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 混合链：常量聚到一起（iconst_1, iload, iconst_2 → iload, c1, c2） ──
TEST(CommutativeOperatorReorderTest, mixedChainConstantsGrouped)
{
    FixtureBuilder b("t/C");
    // iconst_1; iload 1; iconst_2; iadd; iadd; ireturn
    Bytes code = {kIConst1};
    code = cat(code, var(kILoad, 1));
    code = cat(code, Bytes{kIConst2, kIAdd, kIAdd, kAReturn});
    b.addMethod(method("()I", std::move(code), 2, 2));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::CommutativeOperatorReorder{});
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
    FixtureBuilder b("t/C");
    Bytes code = cat(var(kILoad, 1), Bytes{kIConst1, kISub, kAReturn});
    b.addMethod(method("()I", std::move(code), 2, 2));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 异宽操作数（aconst_null 混入 int 链）不成链 ──
TEST(CommutativeOperatorReorderTest, foreignOperandKept)
{
    FixtureBuilder b("t/C");
    Bytes code = cat(var(kILoad, 1), Bytes{kAconstNull, kIAdd, kAReturn});
    b.addMethod(method("()I", std::move(code), 2, 2));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 链起点锚标签（异常表起点）→ 保守拒收 ──
TEST(CommutativeOperatorReorderTest, chainStartAnchorKept)
{
    FixtureBuilder b("t/C");
    // exc(0,4,4)：异常起点锚在链首指令 → 拒收（声明偏离：旧栈列表语义
    // 隐式重锚，新栈 fail-closed）。
    Bytes code = {kIConst1};
    code = cat(code, cat(var(kILoad, 1), Bytes{kIAdd, kAReturn}));
    b.addMethod(method("()I", std::move(code), 2, 2, {ExcSpec{0, 4, 4, 0}}));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── long 链常量后移 ──
TEST(CommutativeOperatorReorderTest, longChainConstantsMoved)
{
    FixtureBuilder b("t/C");
    // lconst_0; lload 1; ladd; lreturn → lload 1; lconst_0; ladd; lreturn
    Bytes code = Bytes{kLConst0};
    code = cat(code, cat(var(kLLoad, 1), Bytes{kLAdd, kLReturn}));
    b.addMethod(method("()J", std::move(code), 4, 3));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<jbc::core::Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 4u);
    EXPECT_EQ(ops[0], jbc::core::Op::LLOAD);
    EXPECT_EQ(ops[1], jbc::core::Op::LCONST_0);
}

// ── 形状不齐（操作数数 ≠ 算术数 + 1）不成链 ──
TEST(CommutativeOperatorReorderTest, malformedShapeKept)
{
    FixtureBuilder b("t/C");
    // iconst_1; iload 1; iadd; iadd; ireturn —— 2 操作数 2 算术。
    Bytes code = Bytes{kIConst1};
    code = cat(code, cat(var(kILoad, 1), Bytes{kIAdd, kIAdd, kAReturn}));
    b.addMethod(method("()I", std::move(code), 2, 2));

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::CommutativeOperatorReorder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
}

// ── 幂等：重排产物二轮不改写 ──
TEST(ArithmeticInsnRearrangerTest, idempotent)
{
    FixtureBuilder b("t/A");
    Bytes code = cat(cat(var(kILoad, 1), {kIConst1, kIAdd}), var(kIStore, 1));
    code = cat(code, cat(cat(var(kILoad, 2), {kIConst2, kIAdd}), var(kIStore, 2)));
    code = cat(code, cat(cat(var(kILoad, 1), {kIConst1, kIAdd}), var(kIStore, 1)));
    b.addMethod(method("()I", std::move(code), 2, 3));

    const MethodPassRun first =
        runMethodPass(b.assemble(), mjbase::pass::ArithmeticInsnRearranger{});
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
