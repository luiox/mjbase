// NopRemover：NOP 删除/分支目标重定向/边界锚定保留 + PA 纪律 + 注册面。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService；装配器自带
// frame 分析），指令体片段 R"JASM 定界。
#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/nop_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

// ── 顺序 NOP 与末条 NOP 全删 ──
TEST(NopRemoverTest, removesLeadingMiddleAndTrailingNops)
{
    // NOP; ICONST_1; POP; NOP; RETURN; NOP
    const Bytes in = assembleMethodClass("m", "()V",
                                         R"JASM(      nop
      iconst_1
      pop
      nop
      return
      nop
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::NopRemover{});

    EXPECT_TRUE(r.pa.changed());
    EXPECT_FALSE(r.pa.areAllPreserved());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::POP);
    EXPECT_EQ(ops[2], Op::RETURN);
}

// ── 无 NOP：PA=all，写回字节与输入逐字节一致（copy-through 保真）──
TEST(NopRemoverTest, noNopMeansAllPreservedAndByteIdentical)
{
    // ICONST_1; POP; RETURN
    const Bytes in = assembleMethodClass("m", "()V",
                                         R"JASM(      iconst_1
      pop
      return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::NopRemover{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_FALSE(r.pa.changed());
    EXPECT_EQ(r.out, in);
}

// ── 分支目标 NOP：删除后 GOTO 重定向到顺序后继（fallthrough 策略）──
TEST(NopRemoverTest, branchTargetNopRetargetsToFallthrough)
{
    // GOTO→L; L: NOP; RETURN——分支原目标 = NOP。原夹具字节用 JSR 0xa8
    // 充当无条件分支，但无 ret 的 jsr 使子程序入口栈遗 [retAddr]，帧面
    // 非法（装配器拒收）且与其注释声明的 GOTO 语义不符——按原注释语义
    // 改 GOTO，门面（分支目标锚 NOP）不变。
    const Bytes in = assembleMethodClass("m", "()V",
                                         R"JASM(      goto L
      L:
      nop
      return
)JASM");
    const MethodPassRun r = runMethodPass(in, mjbase::pass::NopRemover{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection                   ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 2u);   // GOTO; RETURN
    const jbc::ir::Insn* branch = ins.cu->dom.peek(ids[0]);
    const jbc::ir::Insn* ret    = ins.cu->dom.peek(ids[1]);
    ASSERT_NE(branch, nullptr);
    ASSERT_NE(ret, nullptr);
    // GOTO 的新目标锚定在 RETURN 上（fallthrough 重定向）。
    const auto target = ins.cu->dom.resolveLabel(branch->target);
    ASSERT_TRUE(target.is_ok());
    EXPECT_EQ(target.unwrap().kind, jbc::ir::LabelTargetKind::Instruction);
    EXPECT_EQ(target.unwrap().insn, ids[1]);
}

// ── 异常表边界锚定的 NOP：保留（声明的显式偏离，文档化在头文件）──
TEST(NopRemoverTest, handlerBoundaryNopIsKept)
{
    // NOP; RETURN；异常表 handler 指向 pc0（NOP 上）。该形态下 try 覆盖面
    // 含可达指令、handler 即 NOP 自身：入口 [exc] 与方法入口空栈在 NOP
    // 汇合，后继 RETURN 栈下溢——装配器 frame 门拒收，且 JVM 校验器同样
    // 拒收（handler 帧残值进 RETURN）。纪律例外①：从装配合法类（handler
    // 落尾 athrow，异常值被合法消费）出发单点最小偏差——异常表 handler_pc
    // 一字节回改 2→0（偏差载体的死 ATHROW 无法消去，ops 计数 +1 见断言
    // 处申报），改动前后各做一次 relaxed validate 自检。
    const Bytes legal = assembleJasm(R"JASM(.super java/lang/Object
.class public super t/N {
  .method public static m ()V {
    exceptions: {
      { S, E, H, * }
    },
    code: {
      S:
      nop
      E:
      return
      H:
      athrow
    }
  }
}
)JASM");
    expectValidateOk(legal);
    const std::vector<ca::u8> row{0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00};
    const auto                row0 = std::search(legal.begin(), legal.end(), row.begin(),
                                                 row.end());
    ASSERT_NE(row0, legal.end()) << "异常表行字节模式未命中";
    Bytes in = legal;
    in[static_cast<ca::usize>(row0 - legal.begin()) + 5] = 0x00;   // handler_pc 2 → 0
    // 偏差后 validate relaxed 档放行（handler 入口残值汇合不在其拒收面）
    // ——该形态的门在装配器 frame 汇合检查侧，如实申报。
    expectValidateOk(in);

    const MethodPassRun r = runMethodPass(in, mjbase::pass::NopRemover{});

    // 删除被拒 → 视为未改动（PA=all，字节保真）。ops 计数含偏差载体的死
    // ATHROW 脚手架（原夹具无此件，最小偏差无法消去既有字节，计数 +1
    // 如实申报），NOP 仍居首位。
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection      ins = inspect(r.out);
    const std::vector<Op> ops = opsOf(ins);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::NOP);
    EXPECT_EQ(ops[2], Op::ATHROW);
}

// ── 注册面：种子名可查可构造 ──
TEST(NopRemoverTest, registeredAsNopRemover)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("NopRemover"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("NopRemover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
