// FlatteningReverser：控制流反扁平化——常量状态传播 + switch 分发静态解析。
// 正面（lookup/table/xor 偏移、default 回落、直存/中转弹跳前驱成环）+
// 拒绝面（第二读点/交叠定值/区间内锚定/直落块头/双 switch/iinc）+ 幂等 +
// 形态表注入扩展面 + 注册面。常量均为合成值。
//
// 断言用 op 序列结构性定位（run 管线会从 backing 重新 promote，构建期捕获
// 的 InsnId/LabelId 全部过期，不可跨 run 比较）。
#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/flattening_reverser.hpp"
#include "mjbase/pass/method_rewrite_util.hpp"
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

bool isIntLoadOpT(Op op)
{
    return op == Op::ILOAD || (op >= Op::ILOAD_0 && op <= Op::ILOAD_3);
}

ca::u32 slotOfIntLoadT(const jbc::ir::Insn& in)
{
    if (in.op == Op::ILOAD) return static_cast<ca::u32>(in.a);
    return static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::ILOAD_0);
}

bool isIntStoreOpT(Op op)
{
    return op == Op::ISTORE || (op >= Op::ISTORE_0 && op <= Op::ISTORE_3);
}

ca::u32 slotOfIntStoreT(const jbc::ir::Insn& in)
{
    if (in.op == Op::ISTORE) return static_cast<ca::u32>(in.a);
    return static_cast<ca::u32>(in.op) - static_cast<ca::u32>(Op::ISTORE_0);
}

}   // namespace

// ── 正面：identity 折叠 lookup 分发，直存 + 中转弹跳双前驱成环 ──
TEST(FlatteningReverserTest, lookupDispatcherResolvesStateCycle)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId xL = b.label();
        // 入口前驱：ldc 111111111; istore s; goto D（直存形态）。
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        // 分发块 D：iload s; lookupswitch { -222222222: B, 111111111: A, def: X }
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {-222222222, 111111111}, {bL, aL});
        // A 块：中转弹跳写下一状态后 goto D。
        b.ldcInt(-222222222);
        b.bindHere(aL);
        b.istore(2).iload(2).istore(1).branch(Op::GOTO, dL);
        // B 块 / X 块收尾。
        b.iconst_1();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.changed());

    // 指令集原样（只动 goto 目标字段），两个前驱 goto 分别直达 A 头（第 2
    // 个 LDC）/ B 头（首个 ICONST_1）。
    EXPECT_EQ(opsOf(fx), before);
    EXPECT_EQ(gotoTargetPositions(fx),
              (std::set<ca::i64>{nthPos(fx, Op::LDC, 1), nthPos(fx, Op::ICONST_1, 0)}));
    // 写回可产合法类字节。
    EXPECT_FALSE(fx.writeBack().empty());
}

// ── 正面：table 分发 + isub 常量偏移；越界前驱走 default（合法状态） ──
TEST(FlatteningReverserTest, tableDispatcherResolvesOffsetAndDefault)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId xL = b.label();
        // 入口前驱 K=3：v = 3 - 7 = -4，越界 → default X。
        b.ldcInt(3).istore(1).branch(Op::GOTO, dL);
        // D：iload s; ldc 7; isub; tableswitch low=0 high=2 [A,B,X] def=X。
        b.iload(1);
        b.bindHere(dL);
        b.ldcInt(7).op(Op::ISUB);
        b.tableSwitch(xL, 0, 2, {aL, bL, xL});
        // A 块：K=8 → v = 1 → B。
        b.ldcInt(8);
        b.bindHere(aL);
        b.istore(1).branch(Op::GOTO, dL);
        b.iconst_1();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto pa = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.changed());

    // 入口 goto → default X（首个 ICONST_0）；A 尾 goto → B（首个 ICONST_1）。
    EXPECT_EQ(gotoTargetPositions(fx),
              (std::set<ca::i64>{nthPos(fx, Op::ICONST_0, 0), nthPos(fx, Op::ICONST_1, 0)}));
}

// ── 正面：ixor 常量偏移（环绕位运算正确性） ──
TEST(FlatteningReverserTest, xorOffsetResolvesThroughKey)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId xL = b.label();
        // K=111111111 → 111111111 ^ 5 = 111111106。
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.ldcInt(5).op(Op::IXOR);
        b.lookupSwitch(xL, {-222222217, 111111106}, {bL, aL});
        // K=-222222222 → -222222222 ^ 5 = -222222217。
        b.ldcInt(-222222222);
        b.bindHere(aL);
        b.istore(1).branch(Op::GOTO, dL);
        b.iconst_1();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto pa = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.changed());

    EXPECT_EQ(gotoTargetPositions(fx),
              (std::set<ca::i64>{nthPos(fx, Op::LDC, 2), nthPos(fx, Op::ICONST_1, 0)}));
}

// ── 拒绝面：状态槽第二读点 ──
TEST(FlatteningReverserTest, rejectsSecondReadOfStateSlot)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId xL = b.label();
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {111111111}, {aL});
        b.ldcInt(111111111);
        b.bindHere(aL);
        // 前导多读一次 s（pop 配平）：唯一触发面是第二读点门，写链本身
        // 仍是可解析的直存形态。
        b.iload(1).op(Op::POP).ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 拒绝面：交叠定值（写点后继不是 goto 分发块） ──
TEST(FlatteningReverserTest, rejectsOverlappingWrite)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId xL = b.label();
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {111111111}, {aL});
        b.ldcInt(111111111);
        b.bindHere(aL);
        // 第一处写点后继是 iload（非 goto D）：交叠定值整族拒收。
        b.istore(1).iload(2).istore(2).istore(1).branch(Op::GOTO, dL);
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 拒绝面：分支锚入分发块区间内部 ──
TEST(FlatteningReverserTest, rejectsBranchIntoDispatcherInterior)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL   = b.label();
        LabelId aL   = b.label();
        LabelId xL   = b.label();
        LabelId midL = b.label();
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.ldcInt(7);
        b.bindHere(midL);   // 锚在 D 区间内部的 ldc 上
        b.iadd();
        b.lookupSwitch(xL, {111111118}, {aL});
        b.ldcInt(111111111);
        b.bindHere(aL);
        // A 尾跟随一条死 goto 直指 D 内部锚点：唯一触发面是区间内锚定门。
        b.istore(1).branch(Op::GOTO, dL).branch(Op::GOTO, midL);
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 拒绝面：物理直落进分发块头 ──
TEST(FlatteningReverserTest, rejectsFallthroughIntoDispatcherHead)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId xL = b.label();
        // 入口写状态后直落进 D（无 goto）：入口边不可静态解析。
        b.ldcInt(111111111).istore(1);
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {111111111}, {aL});
        // aL 块：空体回环 D 头（case 目标）。
        b.branch(Op::GOTO, dL);
        b.bindHere(aL);
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 拒绝面：第二个 switch（多汇合点） ──
TEST(FlatteningReverserTest, rejectsSecondSwitch)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId xL = b.label();
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {111111111}, {aL});
        b.ldcInt(111111111);
        b.bindHere(aL);
        b.istore(1).branch(Op::GOTO, dL);
        // 尾随另一个局部上的无关 switch（A 尾 goto 后不可达）。
        b.iload(3).lookupSwitch(xL, {1}, {xL});
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 拒绝面：状态槽被 iinc 触碰（读改写不在引擎建模面） ──
TEST(FlatteningReverserTest, rejectsIincOnStateSlot)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId xL = b.label();
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {111111114}, {aL});
        b.iinc(1, 3);
        b.bindHere(aL);
        b.branch(Op::GOTO, dL);
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 幂等：改写后二轮整族拒收，逐字节一致 ──
TEST(FlatteningReverserTest, idempotentOnSecondRun)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
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
    const auto pa1 = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa1.changed());
    const Bytes out1 = fx.writeBack();

    const auto pa2 = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa2.areAllPreserved());
    EXPECT_FALSE(pa2.changed());
    EXPECT_EQ(fx.writeBack(), out1);
}

// ── 形态表注入：自定义前驱行（双级中转弹跳）被引擎消费，设施不写死 ──
TEST(FlatteningReverserTest, customPredFormRowExtendsFacility)
{
    DomFixture fx("t/F", "m", "(I)I", 4, 5, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId xL = b.label();
        // 双级弹跳：ldc K; istore u; iload u; istore v; iload v; istore s; goto D。
        b.ldcInt(111111111);
        b.istore(2).iload(2).istore(3).iload(3).istore(1).branch(Op::GOTO, dL);
        b.iload(1);
        b.bindHere(dL);
        b.lookupSwitch(xL, {111111111}, {aL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    // 注入行替换默认表：默认直存/单级弹跳均不命中双级弹跳形态，若引擎
    // 写死形态则本用例必然失败。从写点向前：iload v → istore v → iload u
    // → istore u → intConst K。
    mjbase::pass::FlatteningReverser::PredFormFn doubleBounce =
        [](const jbc::ir::ClassSession& session, const jbc::ir::CodeDom& dom,
           const std::vector<jbc::ir::InsnId>& order, ca::usize w, ca::i32& k) {
        if (w < 5) return false;
        const jbc::ir::Insn* l1  = dom.peek(order[w - 1]);
        const jbc::ir::Insn* s1  = dom.peek(order[w - 2]);
        const jbc::ir::Insn* l2  = dom.peek(order[w - 3]);
        const jbc::ir::Insn* s2  = dom.peek(order[w - 4]);
        const jbc::ir::Insn* kIn = dom.peek(order[w - 5]);
        return l1 != nullptr && s1 != nullptr && l2 != nullptr && s2 != nullptr &&
               kIn != nullptr && isIntLoadOpT(l1->op) && isIntStoreOpT(s1->op) &&
               isIntLoadOpT(l2->op) && isIntStoreOpT(s2->op) &&
               slotOfIntLoadT(*l1) == slotOfIntStoreT(*s1) &&
               slotOfIntLoadT(*l2) == slotOfIntStoreT(*s2) &&
               mjbase::pass::method_rewrite_util::intValueOf(session, *kIn, k);
    };
    const auto pa = fx.run(mjbase::pass::FlatteningReverser({doubleBounce}));
    EXPECT_TRUE(pa.changed());
    EXPECT_EQ(gotoTargetPositions(fx), (std::set<ca::i64>{nthPos(fx, Op::ICONST_1, 0)}));
}

// ── 镜像形态 B（常量在左）：[ldc C; iload s; ISUB; switch] → v = C - K ──
TEST(FlatteningReverserTest, mirroredSingleSubResolves)
{
    // v = C - K：K = 111111111、C = 111111113 → v = 2 命中 key 2 → A 块。
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId xL = b.label();
        // 入口前驱：ldc K; istore s; goto D。
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        // 分发块 D（镜像单步，块头 = 常量）：ldc C; iload s; ISUB; switch。
        b.ldcInt(111111113);
        b.bindHere(dL);
        b.iload(1).op(Op::ISUB);
        b.lookupSwitch(xL, {2, 7}, {aL, bL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.changed());
    EXPECT_EQ(opsOf(fx), before);
    // 唯一前驱 goto 直达 key=2 的 A 块头（首个 ICONST_1）。
    EXPECT_EQ(gotoTargetPositions(fx), (std::set<ca::i64>{nthPos(fx, Op::ICONST_1, 0)}));
}

// ── 镜像两步：[C2; C1; iload s; ISUB; IADD; switch] → v = C2 + (C1 - K) ──
TEST(FlatteningReverserTest, mirroredTwoStepChainResolves)
{
    // K = 111111111、C1 = 111111110（近载端，配远端 ISUB）、C2 = 3（配近端
    // IADD）→ v = 3 + (-1) = 2 命中 key 2。块头 = 最深常量 C2。
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId bL = b.label();
        LabelId xL = b.label();
        b.ldcInt(111111111).istore(1).branch(Op::GOTO, dL);
        b.ldcInt(3);
        b.bindHere(dL);
        b.ldcInt(111111110);
        b.iload(1).op(Op::ISUB).op(Op::IADD);
        b.lookupSwitch(xL, {2, 7}, {aL, bL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_2();
        b.bindHere(bL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    // 默认规格 maxSteps=1，两步链注入 maxSteps=2（形态表扩展面）。
    const auto pa = fx.run(mjbase::pass::FlatteningReverser(
        mjbase::pass::FlatteningReverser::defaultPredForms(),
        mjbase::pass::FlatteningReverser::DispatchSpec{2, {Op::IADD, Op::ISUB, Op::IXOR}}));
    EXPECT_TRUE(pa.changed());
    EXPECT_EQ(gotoTargetPositions(fx), (std::set<ca::i64>{nthPos(fx, Op::ICONST_1, 0)}));
}

// ── 镜像 XOR（交换律下与形态 A 等价）：[ldc C; iload s; IXOR; switch] ──
TEST(FlatteningReverserTest, mirroredXorResolves)
{
    // K = 6、C = 5 → v = 5 ^ 6 = 3 命中 key 3。
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId xL = b.label();
        b.ldcInt(6).istore(1).branch(Op::GOTO, dL);
        b.ldcInt(5);
        b.bindHere(dL);
        b.iload(1).op(Op::IXOR);
        b.lookupSwitch(xL, {3}, {aL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto pa = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.changed());
    EXPECT_EQ(gotoTargetPositions(fx), (std::set<ca::i64>{nthPos(fx, Op::ICONST_1, 0)}));
}

// ── 混合操作数链拒收：内层镜像（C1 在左）+ 外层正常（C2 在右）→ 整族不动 ──
TEST(FlatteningReverserTest, mixedOperandChainRejected)
{
    // [ldc C1; iload s; ISUB; ldc C2; IADD; switch]：形态 A 解析在 iload 处
    // 被 ldc C2 隔断失配，形态 B 要求 binop 链紧贴 iload 也失配 → 拒收
    // （混合链挂账）。
    DomFixture fx("t/F", "m", "(I)I", 4, 4, [&](Body& b) {
        LabelId dL = b.label();
        LabelId aL = b.label();
        LabelId xL = b.label();
        b.ldcInt(1).istore(1).branch(Op::GOTO, dL);
        b.ldcInt(2);
        b.bindHere(dL);
        b.iload(1).op(Op::ISUB).ldcInt(3).op(Op::IADD);
        b.lookupSwitch(xL, {4}, {aL});
        b.iconst_1();
        b.bindHere(aL);
        b.ireturn();
        b.iconst_0();
        b.bindHere(xL);
        b.ireturn();
    });
    const auto before = opsOf(fx);
    const auto pa     = fx.run(mjbase::pass::FlatteningReverser{});
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_EQ(opsOf(fx), before);
}

// ── 注册面 ──
TEST(FlatteningReverserTest, registeredAsFlatteningReverser)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("FlatteningReverser"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("FlatteningReverser");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
