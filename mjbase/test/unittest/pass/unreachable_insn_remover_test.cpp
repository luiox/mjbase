// UnreachableInsnRemover：帧可达性证明删除不可达普通指令 + 异常边保守门 +
// 标签政策 + 幂等 + 与 FlatteningReverser 的端到端集成（死分发块清理）。
// 常量均为合成值。断言用 op 序列结构性定位（run 管线会从 backing 重新
// promote，构建期捕获的句柄跨 run 过期）。
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/flattening_reverser.hpp"
#include "mjbase/pass/unreachable_insn_remover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "fix_pattern_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;
using jbc::ir::InsnId;
using jbc::ir::LabelId;

namespace {

/// 解析标签锚定的指令句柄（必须是指令边界）。
InsnId anchorOf(DomFixture& fx, LabelId l)
{
    auto resolved = fx.unit().dom.resolveLabel(l);
    EXPECT_TRUE(resolved.is_ok());
    return std::move(resolved).unwrap().insn;
}

/// 收集方法体内全部 GOTO 的目标锚定位置。
std::set<ca::i64> gotoTargetPositions(DomFixture& fx)
{
    std::set<ca::i64> out;
    for (const InsnId id : fx.unit().dom.order()) {
        const jbc::ir::Insn* in = fx.unit().dom.peek(id);
        if (in == nullptr) {
            ADD_FAILURE();
            continue;
        }
        if (in->op != Op::GOTO && in->op != Op::GOTO_W) continue;
        out.insert(indexOfId(fx, anchorOf(fx, in->target)));
    }
    return out;
}

/// ops 序列里第 n（0 起）个 op 的位置（找不到报失败返回 -1）。
ca::i64 nthPos(const DomFixture& fx, Op op, ca::usize n)
{
    const std::vector<Op> ops = opsOf(fx);
    ca::usize              seen = 0;
    for (ca::usize i = 0; i < ops.size(); ++i) {
        if (ops[i] != op) continue;
        if (seen == n) return static_cast<ca::i64>(i);
        ++seen;
    }
    ADD_FAILURE() << "op not found: " << static_cast<int>(op);
    return -1;
}

}   // namespace

// ── 正面：无条件跳转后的死岛整段删除 ──
TEST(UnreachableInsnRemoverTest, removesDeadIslandAfterGoto)
{
    DomFixture fx("t/U", "m", "()V", 4, 4, [&](Body& b) {
        LabelId skip = b.label();
        b.branch(Op::GOTO, skip);
        // 死岛：无正常入边。
        b.iconst_1().iconst_2().iadd().pop();
        b.vreturn();
        b.iconst_0();
        b.bindHere(skip);
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UnreachableInsnRemover{});
    EXPECT_TRUE(pa.changed());
    // 死岛四条全删，剩 goto + iconst_0/pop/vreturn。
    EXPECT_EQ(opsOf(fx), (std::vector<Op>{Op::GOTO, Op::ICONST_0, Op::POP, Op::RETURN}));
    EXPECT_FALSE(fx.writeBack().empty());
}

// ── 正面：死块内部的死分支互引一并删除（批内互引） ──
TEST(UnreachableInsnRemoverTest, removesDeadBlocksWithInternalJumps)
{
    DomFixture fx("t/U", "m", "()V", 4, 4, [&](Body& b) {
        LabelId end   = b.label();
        LabelId dead2 = b.label();
        b.branch(Op::GOTO, end);
        b.iconst_1().pop();          // 死块 1
        b.branch(Op::GOTO, dead2);   // 死块 1 → 死块 2（批内互引）
        b.iconst_2();                // 死块 2 头（唯一入边来自死块 1）
        b.bindHere(dead2);
        b.pop();
        b.vreturn();
        b.iconst_0();
        b.bindHere(end);
        b.pop();
        b.vreturn();
    });
    const auto pa = fx.run(mjbase::pass::UnreachableInsnRemover{});
    EXPECT_TRUE(pa.changed());
    EXPECT_EQ(opsOf(fx), (std::vector<Op>{Op::GOTO, Op::ICONST_0, Op::POP, Op::RETURN}));
}

// ── 保守门：锚异常表标签的不可达指令保留 ──
TEST(UnreachableInsnRemoverTest, keepsUnreachableInsnsAnchoringExcLabels)
{
    DomFixture fx("t/U", "m", "()V", 4, 4, [&](Body& b) {
        LabelId skip    = b.label();
        LabelId excS    = b.label();
        LabelId excE    = b.label();
        LabelId handler = b.label();
        b.branch(Op::GOTO, skip);
        b.ldcInt(111111111);   // 死保护区（start 锚点）
        b.bindHere(excS);
        b.istore(1);
        b.bindHere(excE);      // end 锚点
        b.bindHere(skip);
        b.vreturn();
        // handler 体（帧引擎按可达种子，永不入待删集）。
        b.pop();
        b.bindHere(handler);
        b.vreturn();
        b.tryCatch(excS, excE, handler, 0);
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::UnreachableInsnRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 正面：可达代码锚定的异常 handler 种子可达，handler 体保留 ──
TEST(UnreachableInsnRemoverTest, keepsSeededHandlerBody)
{
    DomFixture fx("t/U", "m", "()V", 4, 4, [&](Body& b) {
        LabelId excS    = b.label();
        LabelId excE    = b.label();
        LabelId handler = b.label();
        b.aconstNull();
        b.bindHere(excS);
        b.pop();                    // 可达保护区
        b.bindHere(excE);
        b.vreturn();
        // handler 仅经异常边进入（帧引擎种子可达）。
        b.pop();
        b.bindHere(handler);
        b.vreturn();
        b.tryCatch(excS, excE, handler, 0);
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::UnreachableInsnRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 无死代码：全保留 ──
TEST(UnreachableInsnRemoverTest, noDeadCodeLeavesAllPreserved)
{
    DomFixture fx("t/U", "m", "(I)I", 4, 4, [&](Body& b) {
        b.iload(0).iconst_1().iadd().ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::UnreachableInsnRemover{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 幂等：二轮无死代码，逐字节一致 ──
TEST(UnreachableInsnRemoverTest, idempotentOnSecondRun)
{
    DomFixture fx("t/U", "m", "()V", 4, 4, [&](Body& b) {
        LabelId skip = b.label();
        b.branch(Op::GOTO, skip);
        b.iconst_1().pop();
        b.vreturn();
        b.iconst_0();
        b.bindHere(skip);
        b.pop();
        b.vreturn();
    });
    const auto pa1 = fx.run(mjbase::pass::UnreachableInsnRemover{});
    EXPECT_TRUE(pa1.changed());
    const Bytes out1 = fx.writeBack();

    const auto pa2 = fx.run(mjbase::pass::UnreachableInsnRemover{});
    EXPECT_TRUE(pa2.areAllPreserved());
    EXPECT_EQ(fx.writeBack(), out1);
}

// ── 端到端：FlatteningReverser 解析分发后，死分发块被本件清走 ──
TEST(UnreachableInsnRemoverTest, removesDeadDispatcherAfterDeflattening)
{
    DomFixture fx("t/U", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId xL = b.label();
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {-222222222, 111111111}, {bL, aL});
        b.ldcInt(-222222222);
        b.bindHere(aL);
        b.istore(2).iload(2).istore(1).branch(Op::GOTO, dL);
        b.iconst_1();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto pa =
        fx.run(mjbase::pass::FlatteningReverser{}, mjbase::pass::UnreachableInsnRemover{});
    EXPECT_TRUE(pa.changed());

    const std::vector<Op> ops = opsOf(fx);
    // 分发块（iload s + lookupswitch）不可达被删；X 块（default 目标，解析
    // 后无前驱）同样不可达被删；A 块弹跳链的 iload(2) 可达保留；解析后的
    // goto 直达功能块。
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::LOOKUPSWITCH), 0);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ILOAD), 1);
    EXPECT_EQ(std::count(ops.begin(), ops.end(), Op::ICONST_0), 0);
    EXPECT_EQ(gotoTargetPositions(fx),
              (std::set<ca::i64>{nthPos(fx, Op::LDC, 1), nthPos(fx, Op::ICONST_1, 0)}));
    EXPECT_FALSE(fx.writeBack().empty());
}

// ── 注册面 ──
TEST(UnreachableInsnRemoverTest, registeredAsUnreachableInsnRemover)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("UnreachableInsnRemover"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("UnreachableInsnRemover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
