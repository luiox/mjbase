/// @file        region_view_test.cpp
/// @brief       RegionView 测试：直线/菱形/循环回边的 SESE 候选枚举、
///              try 边界的异常同质旗标与 jsr 语形旗标（合成语料，只读 golden）。
///
/// 判据对照 luiox/mjbase #115 §1.1/§3.1：单入口单出口（判据 1）过滤生效；
/// 异常同质（判据 4）与 jsr/ret（判据 5）以旗标/事实交付。
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/sem/region_view.hpp"

#include "sem_fixture.hpp"

namespace {

using jbc::core::Op;
using jbc::ir::CodeUnit;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::ExcSpec;
using mjbase::analysis::test_support::MethodSpec;
using mjbase::asm_cfg::BlockId;
using mjbase::asm_sem::RegionCandidate;
using mjbase::asm_sem::RegionView;

bool hasMember(const RegionCandidate& c, ca::u32 blockIndex)
{
    return std::any_of(c.members.begin(), c.members.end(),
                       [blockIndex](BlockId b) { return b.index == blockIndex; });
}

}   // namespace

/// 直线 if-then：then 块 = 唯一 SESE 候选（直线、无异常、空 handler 集）。
TEST(MjAsmSemRegionViewTest, StraightLineIfThenCandidate)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "(Z)V";
    m.emit = [](CodeEmit& e) {
        const std::size_t join = e.newLabel();
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, join);   /// taken → join；not-taken 落入 then 块
        e.op(Op::ICONST_1);
        e.varInsn(Op::ISTORE, 1);
        e.bindLabel(join);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto built = RegionView::build(*u);
    ASSERT_TRUE(built.is_ok());
    RegionView view = std::move(built).unwrap();

    EXPECT_FALSE(view.hasSubroutine());
    const std::vector<RegionCandidate> cs = view.candidates();
    ASSERT_EQ(cs.size(), 1u);
    EXPECT_EQ(cs[0].entryFrom.index, 0u);    /// A（分支块）
    EXPECT_EQ(cs[0].entryBlock.index, 1u);   /// B（then 块 = 区域头）
    EXPECT_EQ(cs[0].exitTarget.index, 2u);   /// C（join = 越出目标）
    ASSERT_EQ(cs[0].members.size(), 1u);
    EXPECT_EQ(cs[0].members[0].index, 1u);
    EXPECT_TRUE(cs[0].straightLine);
    EXPECT_TRUE(cs[0].exceptionHomogeneous);
    EXPECT_TRUE(cs[0].handlers.empty());
}

/// 菱形：两个 then/else 单块候选，join 侧与多入口侧不成候选。
TEST(MjAsmSemRegionViewTest, DiamondYieldsTwoArmCandidates)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "(Z)V";
    m.emit = [](CodeEmit& e) {
        const std::size_t join = e.newLabel();
        const std::size_t end  = e.newLabel();
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, join);
        e.op(Op::ICONST_1);
        e.varInsn(Op::ISTORE, 1);
        e.branch(Op::GOTO, end);
        e.bindLabel(join);
        e.op(Op::ICONST_2);
        e.varInsn(Op::ISTORE, 1);
        e.bindLabel(end);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto built = RegionView::build(*u);
    ASSERT_TRUE(built.is_ok());
    const std::vector<RegionCandidate> cs = built.unwrap().candidates();

    ASSERT_EQ(cs.size(), 2u);
    for (const RegionCandidate& c : cs) {
        EXPECT_EQ(c.members.size(), 1u);
        EXPECT_TRUE(c.straightLine);
        EXPECT_EQ(c.exitTarget.index, 3u);   /// join 后的 return 块
    }
    EXPECT_TRUE(hasMember(cs[0], 1u) || hasMember(cs[0], 2u));
    EXPECT_TRUE(hasMember(cs[1], 1u) || hasMember(cs[1], 2u));
    EXPECT_FALSE(cs[0].members[0] == cs[1].members[0]);
}

/// 循环回边：头 + 循环体成候选（非直线）；回边目标块与体单独不成候选
/// （体 → 头无续延边）。块序：A(init) B(head) C(body) D(exit)。
TEST(MjAsmSemRegionViewTest, LoopHeadAndBodyCandidate)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "(I)V";
    m.emit = [](CodeEmit& e) {
        const std::size_t head = e.newLabel();
        const std::size_t exit = e.newLabel();
        e.op(Op::ICONST_0);
        e.varInsn(Op::ISTORE, 0);
        e.bindLabel(head);
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, exit);
        e.op(Op::IINC);
        e.u8(0);
        e.u8(1);
        e.branch(Op::GOTO, head);
        e.bindLabel(exit);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto built = RegionView::build(*u);
    ASSERT_TRUE(built.is_ok());
    const std::vector<RegionCandidate> cs = built.unwrap().candidates();

    /// 两个候选：头 + 体（入口 (A,B)，越出 D）；体单独的环形候选
    /// （入口 (B,C)，经回边 C→B 越出回 B——结构上单入口单出口，是否
    /// 可 outline 由栈形判据（A2）裁决）。
    ASSERT_EQ(cs.size(), 2u);
    bool foundLoop = false;
    bool foundBody = false;
    for (const RegionCandidate& c : cs) {
        if (c.members.size() == 2u) {
            foundLoop = true;
            EXPECT_EQ(c.entryFrom.index, 0u);    /// A
            EXPECT_EQ(c.entryBlock.index, 1u);   /// B（循环头）
            EXPECT_EQ(c.exitTarget.index, 3u);   /// D（越出目标）
            EXPECT_EQ(c.members[0].index, 1u);   /// B
            EXPECT_EQ(c.members[1].index, 2u);   /// C（循环体）
            EXPECT_FALSE(c.straightLine);        /// 头带条件分支
            EXPECT_TRUE(c.exceptionHomogeneous);
        } else {
            foundBody = true;
            ASSERT_EQ(c.members.size(), 1u);
            EXPECT_EQ(c.entryFrom.index, 1u);    /// B
            EXPECT_EQ(c.entryBlock.index, 2u);   /// C（体）
            EXPECT_EQ(c.exitTarget.index, 1u);   /// 回边落回 B
            EXPECT_TRUE(c.straightLine);
        }
    }
    EXPECT_TRUE(foundLoop);
    EXPECT_TRUE(foundBody);
}

/// 判据 4（同质命中）：候选块落在单一 try 内 → 同质成立且交付 handler 事实。
TEST(MjAsmSemRegionViewTest, RegionInsideSingleTryIsHomogeneous)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "(Z)V";
    m.emit = [](CodeEmit& e) {
        const std::size_t tryStart = e.newLabel();   /// 0
        const std::size_t tryEnd   = e.newLabel();   /// 1
        const std::size_t handler  = e.newLabel();   /// 2
        const std::size_t join     = e.newLabel();   /// 3
        const std::size_t end      = e.newLabel();   /// 4
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, join);    /// A：taken → join；not-taken 落入 B
        e.bindLabel(tryStart);
        e.op(Op::ICONST_1);          /// B：try [B .. join) 内
        e.varInsn(Op::ISTORE, 1);
        e.branch(Op::GOTO, end);
        e.bindLabel(tryEnd);
        e.bindLabel(join);
        e.op(Op::ICONST_2);          /// C：try 外
        e.varInsn(Op::ISTORE, 2);
        e.bindLabel(end);
        e.op(Op::RETURN);            /// D
        e.bindLabel(handler);
        e.op(Op::POP);               /// H：throwable
        e.op(Op::RETURN);
    };
    m.exceptions = {ExcSpec{0, 1, 2, "java/lang/Exception"}};
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);
    ASSERT_EQ(u->exc.size(), 1u);

    auto built = RegionView::build(*u);
    ASSERT_TRUE(built.is_ok());
    RegionView view = std::move(built).unwrap();

    const std::vector<RegionCandidate> cs = view.candidates();
    /// {B}（try 内单块）与 {C}（try 外单块）各自成候选。
    ASSERT_EQ(cs.size(), 2u);
    bool foundInTry = false;
    for (const RegionCandidate& c : cs) {
        EXPECT_TRUE(c.straightLine);
        EXPECT_TRUE(c.exceptionHomogeneous);
        if (c.members.size() == 1u && c.members[0].index == 1u) {
            foundInTry = true;
            ASSERT_EQ(c.handlers.size(), 1u);
            EXPECT_EQ(c.handlers[0].handler.index, 4u);   /// H 块
            EXPECT_NE(c.handlers[0].catchCp, 0u);         /// catch Exception 已物化
        } else {
            EXPECT_TRUE(c.handlers.empty());              /// try 外无 handler 事实
        }
    }
    EXPECT_TRUE(foundInTry);
}

/// 判据 4（越界拒绝形态）：候选成员跨越 try 边界 → 同质旗标为假。
TEST(MjAsmSemRegionViewTest, RegionCrossingTryBoundaryFlagged)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "(Z)V";
    m.emit = [](CodeEmit& e) {
        const std::size_t tryStart = e.newLabel();   /// 0
        const std::size_t tryEnd   = e.newLabel();   /// 1
        const std::size_t handler  = e.newLabel();   /// 2
        const std::size_t end      = e.newLabel();   /// 3
        e.op(Op::ICONST_0);          /// A：落入 B
        e.varInsn(Op::ISTORE, 0);
        e.bindLabel(tryStart);
        e.op(Op::ICONST_1);          /// B：try [B .. C) 内
        e.varInsn(Op::ISTORE, 1);
        e.bindLabel(tryEnd);
        e.op(Op::ICONST_2);          /// C：try 外
        e.varInsn(Op::ISTORE, 2);
        e.branch(Op::GOTO, end);     /// D 独立成块（end 是真分支目标）
        e.bindLabel(end);
        e.op(Op::RETURN);            /// D
        e.bindLabel(handler);
        e.op(Op::POP);               /// H：throwable
        e.op(Op::RETURN);
    };
    m.exceptions = {ExcSpec{0, 1, 2, "java/lang/Exception"}};
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto built = RegionView::build(*u);
    ASSERT_TRUE(built.is_ok());
    RegionView view = std::move(built).unwrap();

    const std::vector<RegionCandidate> cs = view.candidates();
    /// 入口 (A,B) 的最大候选 = {B, C}（B 在 try 内、C 在外）——判据 4 旗标为假。
    bool foundCrossing = false;
    for (const RegionCandidate& c : cs) {
        if (c.members.size() == 2u) {
            foundCrossing = true;
            EXPECT_EQ(c.entryFrom.index, 0u);
            EXPECT_EQ(c.entryBlock.index, 1u);
            EXPECT_EQ(c.exitTarget.index, 3u);
            EXPECT_FALSE(c.exceptionHomogeneous);
            EXPECT_TRUE(c.handlers.empty());
            EXPECT_TRUE(c.straightLine);
        }
    }
    EXPECT_TRUE(foundCrossing);
}

/// 判据 5：jsr/ret 语形在场 → hasSubroutine；直线语料为否。
TEST(MjAsmSemRegionViewTest, SubroutineFlaggedSyntactically)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name     = "m";
    m.desc     = "()V";
    m.maxLocals = 2;
    m.emit     = [](CodeEmit& e) {
        const std::size_t sub = e.newLabel();
        const std::size_t end = e.newLabel();
        e.branch(Op::JSR, sub);
        e.bindLabel(end);
        e.op(Op::RETURN);
        e.bindLabel(sub);
        e.varInsn(Op::ASTORE, 1);
        e.varInsn(Op::RET, 1);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto built = RegionView::build(*u);
    ASSERT_TRUE(built.is_ok());
    EXPECT_TRUE(built.unwrap().hasSubroutine());
}

/// 单块方法：无入口边 → 无候选（整方法形态不交付）。
TEST(MjAsmSemRegionViewTest, SingleBlockNoCandidates)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "()V";
    m.emit = [](CodeEmit& e) {
        e.op(Op::NOP);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    auto built = RegionView::build(*u);
    ASSERT_TRUE(built.is_ok());
    EXPECT_TRUE(built.unwrap().candidates().empty());
}
