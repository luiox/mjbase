// EntrySlotSwapNormalizer：入口槽交换序言删除 + 方法体槽位逆映射 +
// 普通拷贝/恒等序列/序言内分支目标的保守放弃。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService；装配器自带
// frame 分析），指令体片段 R"JASM 定界。序言内分支形态回边落点栈面冲突
// （fallthrough 与回边栈形状不一致）装配器 frame 门拒收——纪律例外①：
// 从装配合法类出发单点最小偏差（回边 offset 两字节改写：自指 → 落序言），
// 改动前后各做一次 relaxed validate 自检。
#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/entry_slot_swap_normalizer.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 单方法夹具（t/S.m；isStatic 决定 public static / public 实例形态；
/// maxStack/maxLocals 由装配器按代码面计算，与原夹具声明值一致；major
/// 默认 49 帧less 口径，例外①基类要帧面 validate 时显式给 52）。
Bytes assembled(std::string_view desc, bool isStatic, std::string_view codeBody,
                ca::i32 major = 49)
{
    const std::string modifier = isStatic ? "public static" : "public";
    const std::string source =
        R"JASM(.super java/lang/Object
.class public super t/S {
  .method )JASM" + modifier + " m " + std::string(desc) + R"JASM( {
    code: {
)JASM" + std::string(codeBody) + R"JASM(    }
  }
}
)JASM";
    return assembleJasm(source, major);
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
    const Bytes input = assembled("(Ljava/lang/String;)Ljava/lang/String;", false,
                                  R"JASM(      aload_0
      aload_1
      astore_0
      astore_1
      aload_0
      areturn
)JASM");
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
    const Bytes input = assembled("(I)V", false,
                                  R"JASM(      aload_0
      iload_1
      istore_0
      astore_1
      iinc 0 1
      return
)JASM");
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
    const Bytes input = assembled("(IIII)V", true,
                                  R"JASM(      iload_0
      iload_3
      istore_0
      istore_3
      iload_0
      pop
      return
)JASM");
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
    const Bytes input = assembled("()V", false,
                                  R"JASM(      aload_0
      astore_1
      aconst_null
      astore_0
      aload_0
      aload_1
      pop
      pop
      return
)JASM");
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
    const Bytes input = assembled("()V", false,
                                  R"JASM(      aload_0
      astore_1
      aload_0
      pop
      aconst_null
      astore_0
      return
)JASM");
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, input);
}

// ── this 别名：宽值占用槽 k，互换会截断 long/double，放弃 ──
TEST(EntrySlotSwapNormalizerTest, SkipsThisAliasWhenWideValueOverlaps)
{
    // aload_0; astore_2; lload_1; lconst_0; ladd; pop2; return。槽 2 落
    // this 别名（ref）、lload_1 读槽 1-2 宽对——别名槽落进宽读覆盖范围
    // 且截断 long 实参，装配器 frame 门按槽型覆写拒收——纪律例外①：从
    // 装配合法类出发单点最小偏差（astore_3 → astore_2 一字节回改：别名
    // 槽移入宽读覆盖半槽），改动前后各做一次 relaxed validate 自检。
    // 宽读的槽 1-2 借 (J)V 实参长槽播种（jasm 不产未播种槽的宽装载）；
    // 52 帧面装配（49 档装配器对未播种宽装载同样拒收）。
    const Bytes legal = assembled("(J)V", false,
                                  R"JASM(      aload_0
      astore_3
      lload_1
      lconst_0
      ladd
      pop2
      return
)JASM",
                                  52);
    expectValidateOk(legal);
    const std::vector<ca::u8> pat{0x2a, 0x4e, 0x1f, 0x09, 0x61, 0x58, 0xb1};
    const auto                at = std::search(legal.begin(), legal.end(), pat.begin(),
                                               pat.end());
    ASSERT_NE(at, legal.end()) << "指令体字节模式未命中";
    Bytes input = legal;
    input[static_cast<ca::usize>(at - legal.begin()) + 1] = 0x4d;   // astore_3 → astore_2
    // 偏差后 validate relaxed 档放行（半槽 ref 覆写不在其拒收面）——该
    // 形态的门在装配器 frame 槽型覆写检查侧，如实申报。
    expectValidateOk(input);

    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, input);
}

// ── 死拷贝（copy 之后无人使用）同样按别名还原：删除后只剩方法体 ──
TEST(EntrySlotSwapNormalizerTest, RemovesDeadThisAlias)
{
    // aload_0; astore_1; return
    const Bytes input = assembled("()V", false,
                                  R"JASM(      aload_0
      astore_1
      return
)JASM");
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
    const Bytes input = assembled("(Ljava/lang/String;)V", false,
                                  R"JASM(      aload_0
      aload_1
      astore_1
      astore_0
      return
)JASM");
    const MethodPassRun r =
        runMethodPass(input, mjbase::pass::EntrySlotSwapNormalizer{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, input);
}

// ── 分支目标落在序言内：删除会改变控制流，保守放弃 ──
TEST(EntrySlotSwapNormalizerTest, SkipsBranchIntoPrologue)
{
    // 序言 aload_0; aload_1; astore_0; astore_1（pc0..3）；回边落序言
    // aload_1（pc2）。回边落点栈面冲突（fallthrough [ref,ref] vs 回边
    // []）——该形态由装配器 frame 门拒收（validate 的 relaxed 档不做
    // 汇合点栈形裁决）——纪律例外①：从装配合法类出发单点最小偏差（回
    // 边 offset 两字节改写：落方法末边界 → 落序言），52 帧面装配，改动
    // 前后各做一次 relaxed validate 自检（前后均放行——门在装配器侧，
    // 如实申报）。
    const Bytes legal = assembled("(Ljava/lang/String;)V", false,
                                  R"JASM(      aload_0
      aload_1
      astore_0
      astore_1
      goto Lend
      Lend:
      return
)JASM",
                                  52);
    expectValidateOk(legal);
    const std::vector<ca::u8> pat{0x2a, 0x2b, 0x4b, 0x4c, 0xa7, 0x00, 0x03};
    const auto                at = std::search(legal.begin(), legal.end(), pat.begin(),
                                               pat.end());
    ASSERT_NE(at, legal.end()) << "序言 + goto 字节模式未命中";
    Bytes input = legal;
    input[static_cast<ca::usize>(at - legal.begin()) + 5] = 0xff;
    input[static_cast<ca::usize>(at - legal.begin()) + 6] = 0xfe;   // offset -2 → pc2
    expectValidateOk(input);   // 偏差后 validate 仍放行（见上申报）

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
