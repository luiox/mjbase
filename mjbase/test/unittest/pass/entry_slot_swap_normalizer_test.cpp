// EntrySlotSwapNormalizer：入口槽交换序言删除 + 方法体槽位逆映射 +
// 普通拷贝/恒等序列/序言内分支目标的保守放弃。
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/entry_slot_swap_normalizer.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

Bytes assembled(const Bytes& code, std::string_view desc = "(Ljava/lang/String;)V",
                ca::u16 access = 0x0001, ca::u16 maxStack = 2, ca::u16 maxLocals = 4,
                std::vector<ExcSpec> exc = {})
{
    FixtureBuilder b("t/S");
    FixtureMethod  m;
    m.name      = "m";
    m.desc      = desc;
    m.code      = code;
    m.access    = access;
    m.exc       = std::move(exc);
    m.maxStack  = maxStack;
    m.maxLocals = maxLocals;
    b.addMethod(m);
    return b.assemble();
}

/// 写回产物中第 idx 条指令的局部槽号（非局部指令返回 -1）。
ca::i32 localSlotAt(const Inspection& ins, ca::usize idx)
{
    const auto order = ins.cu->dom.order();
    if (idx >= order.size()) return -1;
    const jbc::ir::Insn* in = ins.cu->dom.peek(order[idx]);
    if (in == nullptr) return -1;
    switch (in->op) {
    case Op::ILOAD:
    case Op::LLOAD:
    case Op::FLOAD:
    case Op::DLOAD:
    case Op::ALOAD:
    case Op::ISTORE:
    case Op::LSTORE:
    case Op::FSTORE:
    case Op::DSTORE:
    case Op::ASTORE:
    case Op::IINC: return static_cast<ca::i32>(in->a);
    default: return -1;
    }
}

}   // namespace

// ── this ↔ 首参交换：序言删除，方法体 aload_0 改为 aload_1 ──
TEST(EntrySlotSwapNormalizerTest, SwapsThisAndFirstArgument)
{
    // aload_0; aload_1; astore_0; astore_1; aload_0; areturn
    const Bytes input = assembled(
        {0x2a, 0x2b, 0x4b, 0x4c, 0x2a, 0xb0},
        "(Ljava/lang/String;)Ljava/lang/String;", 0x0001, 2, 2);
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const auto       ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ALOAD);
    EXPECT_EQ(ops[1], Op::ARETURN);
    EXPECT_EQ(localSlotAt(ins, 0), 1);
}

// ── int/ref 混合交换：iinc 跟随映射到交换后的槽 ──
TEST(EntrySlotSwapNormalizerTest, RemapsIincInMixedSwap)
{
    // aload_0; iload_1; istore_0; astore_1; iinc 0,1; return
    const Bytes input = assembled({0x2a, 0x1b, 0x3b, 0x4c, 0x84, 0x00, 0x01, 0xb1},
                                  "(I)V", 0x0001, 2, 2);
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const auto       ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::IINC);
    EXPECT_EQ(ops[1], Op::RETURN);
    EXPECT_EQ(localSlotAt(ins, 0), 1);
}

// ── 非相邻槽交换：0 ↔ 3 ──
TEST(EntrySlotSwapNormalizerTest, RemapsNonAdjacentSlot)
{
    // iload_0; iload_3; istore_0; istore_3; iload_0; pop; return
    const Bytes input = assembled({0x1a, 0x1d, 0x3b, 0x3e, 0x1a, 0x57, 0xb1},
                                  "(IIII)V", 0x0009, 2, 4);
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const auto       ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(localSlotAt(ins, 0), 3);
}

// ── this 别名：`aload_0; astore_k` 后槽 k 承担 this，槽 0 复用为局部 ──
TEST(EntrySlotSwapNormalizerTest, NormalizesThisAliasWithSlotReuse)
{
    // aload_0; astore_1; aconst_null; astore_0; aload_0; aload_1; pop; pop; return
    const Bytes input = assembled({0x2a, 0x4c, 0x01, 0x4b, 0x2a, 0x2b, 0x57, 0x57, 0xb1},
                                  "()V", 0x0001, 2, 2);
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const auto       ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[0], Op::ACONST_NULL);
    EXPECT_EQ(ops[1], Op::ASTORE);
    EXPECT_EQ(localSlotAt(ins, 1), 1);   // 原 slot 0 的局部变量 → slot 1
    EXPECT_EQ(ops[2], Op::ALOAD);
    EXPECT_EQ(localSlotAt(ins, 2), 1);
    EXPECT_EQ(ops[3], Op::ALOAD);
    EXPECT_EQ(localSlotAt(ins, 3), 0);   // 原 slot 1 的 this 别名 → slot 0
}

// ── this 别名：方法体先读槽 0 再写，删除前缀会丢 this 副本，放弃 ──
TEST(EntrySlotSwapNormalizerTest, SkipsThisAliasWhenSlotZeroReadBeforeWrite)
{
    // aload_0; astore_1; aload_0; pop; aconst_null; astore_0; return
    const Bytes input = assembled({0x2a, 0x4c, 0x2a, 0x57, 0x01, 0x4b, 0xb1},
                                  "()V", 0x0001, 1, 2);
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, input);
}

// ── this 别名：宽值占用槽 k，互换会截断 long/double，放弃 ──
TEST(EntrySlotSwapNormalizerTest, SkipsThisAliasWhenWideValueOverlaps)
{
    // aload_0; astore_2; lload_1; lconst_0; ladd; pop2; return
    const Bytes input = assembled({0x2a, 0x4d, 0x1f, 0x09, 0x61, 0x58, 0xb1},
                                  "()V", 0x0001, 4, 4);
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, input);
}

// ── 死拷贝（copy 之后无人使用）同样按别名还原：删除后只剩方法体 ──
TEST(EntrySlotSwapNormalizerTest, RemovesDeadThisAlias)
{
    // aload_0; astore_1; return
    const Bytes input = assembled({0x2a, 0x4c, 0xb1}, "()V", 0x0001, 1, 2);
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    ASSERT_EQ(opsOf(ins).size(), 1u);
    EXPECT_EQ(opsOf(ins)[0], Op::RETURN);
}

// ── 恒等置换（各值写回原槽）不算混淆：原样保留 ──
TEST(EntrySlotSwapNormalizerTest, LeavesIdentityPermutationAlone)
{
    // aload_0; aload_1; astore_1; astore_0; return
    const Bytes input = assembled({0x2a, 0x2b, 0x4c, 0x4b, 0xb1},
                                  "(Ljava/lang/String;)V", 0x0001, 2, 2);
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, input);
}

// ── 分支目标落在序言内：删除会改变控制流，保守放弃 ──
TEST(EntrySlotSwapNormalizerTest, SkipsBranchIntoPrologue)
{
    // 序言 0-3；pc4: goto pc2（偏移 -2）
    const Bytes input = assembled({0x2a, 0x2b, 0x4b, 0x4c, 0xa7, 0xff, 0xfe},
                                  "(Ljava/lang/String;)V", 0x0001, 2, 2);
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, input);
}

// ── 注册面 ──
TEST(EntrySlotSwapNormalizerTest, RegisteredAsMethodPass)
{
    const auto found = mjbase::spi::defaultPassRegistry().find("EntrySlotSwapNormalizer");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, mjbase::spi::PassScope::Method);
}
