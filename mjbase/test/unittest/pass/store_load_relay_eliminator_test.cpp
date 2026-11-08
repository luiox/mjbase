// StoreLoadRelayEliminator：store 定义使用边恰为紧邻 reload 的中转对整删。
// 该删（五族/快捷形/不动点）/不该删（多读点/handler 读点）/边界保守保留
// （对内任一侧锚标签整对拒收，fail-closed）三面。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）；异常表以
// labels 段表达。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/store_load_relay_eliminator.hpp"
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

/// 单方法夹具（默认 ()V，无异常表）。
Bytes oneMethod(std::string_view desc, std::string_view codeBody)
{
    return assembleMethodClass("m", desc, codeBody);
}

/// 单方法夹具 + 异常表（entries 为 labels 口径条目片段）。
Bytes oneMethodEx(std::string_view desc, std::string_view codeBody,
                  std::string_view excEntries)
{
    return assembleJasm(R"JASM(
.super java/lang/Object
.class public super t/C {
  .method public static m )JASM" + std::string(desc) + R"JASM( {
    exceptions: {
)JASM" + std::string(excEntries) + R"JASM(    },
    code: {
)JASM" + std::string(codeBody) + R"JASM(    }
  }
}
)JASM");
}

}   // namespace

// ── int 中转对 → 整删，值留栈 ──
TEST(StoreLoadRelayEliminatorTest, relayPairRemoved)
{
    const MethodPassRun r = runMethodPass(
        oneMethod("()I",
                  R"JASM(
iconst_1
istore 0
iload 0
ireturn
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_1, Op::IRETURN}));
}

// ── cat2（long）中转对 → 整删 ──
TEST(StoreLoadRelayEliminatorTest, longRelayRemoved)
{
    const MethodPassRun r = runMethodPass(
        oneMethod("()J",
                  R"JASM(
lconst_1
lstore 0
lload 0
lreturn
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops, (std::vector<Op>{Op::LCONST_1, Op::LRETURN}));
}

// ── 引用族中转对 → 整删 ──
TEST(StoreLoadRelayEliminatorTest, aloadRelayRemoved)
{
    const MethodPassRun r = runMethodPass(
        oneMethod("()Ljava/lang/Object;",
                  R"JASM(
aconst_null
astore 0
aload 0
areturn
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops, (std::vector<Op>{Op::ACONST_NULL, Op::ARETURN}));
}

// ── 快捷形（istore_n/iload_n）同权 ──
TEST(StoreLoadRelayEliminatorTest, shorthandFormsRemoved)
{
    const MethodPassRun r = runMethodPass(
        oneMethod("()I",
                  R"JASM(
iconst_2
istore_1
iload_1
ireturn
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops, (std::vector<Op>{Op::ICONST_2, Op::IRETURN}));
}

// ── 槽有其他读点 → 单使用不成立 → 保留 ──
TEST(StoreLoadRelayEliminatorTest, foreignReadKept)
{
    const MethodPassRun r = runMethodPass(
        oneMethod("()I",
                  R"JASM(
iconst_1
istore 0
iload 0
iload 0
iadd
ireturn
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 6u);
}

// ── handler 读点（异常边合并）产生额外使用边 → 保留 ──
TEST(StoreLoadRelayEliminatorTest, handlerReadKept)
{
    const MethodPassRun r = runMethodPass(
        oneMethodEx("()I",
                    R"JASM(
iconst_1
Ls:
istore 0
iload 0
pop
invokestatic t/C.g ()V
Le:
iconst_0
ireturn
Lh:
astore 1
iload 0
pop
iconst_0
ireturn
)JASM",
                    R"JASM(      { Ls, Le, Lh, * }
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
    EXPECT_EQ(opsOf(ins).size(), 12u);
}

// ── reload 位外来锚标签（分支跳入）→ 拒收（多定义汇合同样不放宽） ──
TEST(StoreLoadRelayEliminatorTest, anchoredReloadKept)
{
    const MethodPassRun r = runMethodPass(
        oneMethod("()I",
                  R"JASM(
iconst_0
istore 0
iconst_1
ifeq L1
iconst_2
istore 0
L1:
iload 0
ireturn
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 8u);
}

// ── store 位锚标签（异常表边界）→ 双侧锚守卫整对保留（fail-closed） ──
TEST(StoreLoadRelayEliminatorTest, anchoredStoreKept)
{
    const MethodPassRun r = runMethodPass(
        oneMethodEx("()I",
                    R"JASM(
iconst_1
Ls:
istore 0
iload 0
ireturn
Lh:
pop
iconst_0
ireturn
)JASM",
                    R"JASM(      { Ls, Lh, Lh, * }
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection m = inspect(r.out, 0);
    EXPECT_EQ(opsOf(m).size(), 7u);
    EXPECT_EQ(m.cu->exc.size(), 1u);
}

// ── 不动点：删 b 对后 a 对成形 → 下一轮继续消 ──
TEST(StoreLoadRelayEliminatorTest, fixedPointConvergence)
{
    const MethodPassRun r = runMethodPass(
        oneMethod("()V",
                  R"JASM(
iconst_1
iconst_2
istore 0
istore 1
iload 1
iload 0
pop
pop
return
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    EXPECT_EQ(ops,
              (std::vector<Op>{Op::ICONST_1, Op::ICONST_2, Op::POP, Op::POP,
                               Op::RETURN}));
}

// ── 槽位不一致 → 不匹配 → 保留 ──
TEST(StoreLoadRelayEliminatorTest, mismatchedSlotKept)
{
    const MethodPassRun r = runMethodPass(
        oneMethod("()V",
                  R"JASM(
iconst_1
istore 0
iconst_2
istore 1
iload 0
pop
return
)JASM"),
        mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(opsOfOut(r).size(), 7u);
}

// ── 幂等：二轮不再变化 ──
TEST(StoreLoadRelayEliminatorTest, idempotent)
{
    const Bytes bytes = oneMethod("()I",
                                  R"JASM(
iconst_1
istore 0
iload 0
ireturn
)JASM");
    const MethodPassRun  r = runMethodPass(bytes, mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r.pa.changed());
    const MethodPassRun r2 =
        runMethodPass(r.out, mjbase::pass::StoreLoadRelayEliminator{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── 注册面 ──
TEST(StoreLoadRelayEliminatorTest, registeredInRegistry)
{
    const auto found =
        mjbase::spi::defaultPassRegistry().find("StoreLoadRelayEliminator");
    EXPECT_TRUE(found.is_ok());
}
