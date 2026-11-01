/// @file        frame_view_test.cpp
/// @brief       FrameView 测试：签名播种帧、引用合并注入面、死代码、
///              构造器 UninitThis 存活与 cat2 槽布局（合成语料，只读 golden）。
///
/// 判据对照 luiox/mjbase #115 §1.1/§2.2：不可算显式 Err、不伪造帧、
/// UninitThis 探针（判据 6）。
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/sem/frame_view.hpp"

#include "sem_fixture.hpp"

namespace {

using jbc::analysis::FrameSummary;
using jbc::analysis::MethodSig;
using jbc::analysis::VKind;
using jbc::analysis::VType;
using jbc::core::Op;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::MethodSpec;
using mjbase::asm_sem::FrameView;
using mjbase::asm_sem::FunctionTypeResolver;

/// 直线方法 (I)V：iload_0; iconst_2; iadd; pop; return。
SemFixture straightLineFixture()
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "(I)V";
    m.emit = [](CodeEmit& e) {
        e.varInsn(Op::ILOAD, 0);
        e.op(Op::ICONST_2);
        e.op(Op::IADD);
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    return SemFixture(b.assemble({m}));
}

}   // namespace

/// 签名播种：入参槽 Int、栈值计数与 fromTop 按值索引（cat2 底槽不计）。
TEST(MjAsmSemFrameViewTest, SeededStraightLineShape)
{
    SemFixture f  = straightLineFixture();
    CodeUnit*  u  = f.code("m");
    ASSERT_NE(u, nullptr);
    jbc::analysis::ShapeResolver resolver;

    auto built = FrameView::build(f.cp(), *u, sigOf(true, "(I)V"), resolver);
    ASSERT_TRUE(built.is_ok());
    FrameView view = std::move(built).unwrap();

    const std::vector<InsnId>& order = u->dom.order();
    ASSERT_EQ(view.frames().size(), order.size());
    const InsnId iload  = order[0];
    const InsnId iadd   = order[2];
    const InsnId ret    = order[4];

    EXPECT_FALSE(view.hasSubroutine());
    EXPECT_TRUE(view.reachable(iload));

    const VType* param = view.localAt(iload, 0);
    ASSERT_NE(param, nullptr);
    EXPECT_EQ(param->k, VKind::Int);
    EXPECT_EQ(view.localAt(iload, 1), nullptr);   /// 越界（maxLocals 只触达 1 槽）

    ASSERT_EQ(view.stackValueCount(iadd), 2u);
    const VType* top = view.stackFromTop(iadd, 0);
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->k, VKind::Int);
    const VType* below = view.stackFromTop(iadd, 1);
    ASSERT_NE(below, nullptr);
    EXPECT_EQ(below->k, VKind::Int);
    EXPECT_EQ(view.stackFromTop(iadd, 2), nullptr);   /// 越过栈深
    EXPECT_EQ(view.stackValueCount(ret), 0u);
}

/// 不可算显式 Err：栈下溢不伪造帧（判据：build 返回 Err 携带诊断）。
TEST(MjAsmSemFrameViewTest, UncomputableFailsExplicitly)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "()V";
    m.emit = [](CodeEmit& e) {
        e.op(Op::POP);       /// 空栈弹出 → Underflow
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);
    jbc::analysis::ShapeResolver resolver;

    auto built = FrameView::build(f.cp(), *u, sigOf(true, "()V"), resolver);
    ASSERT_TRUE(built.is_err());
    const FrameSummary summary = built.unwrap_err();
    EXPECT_FALSE(summary.ok);
    EXPECT_EQ(summary.fail, FrameSummary::Fail::Underflow);
}

/// 死代码无到达帧；frames 与 order 对齐且不可达帧值为空。
TEST(MjAsmSemFrameViewTest, DeadCodeUnreachable)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "m";
    m.desc = "()V";
    m.emit = [](CodeEmit& e) {
        e.op(Op::RETURN);
        e.op(Op::ICONST_0);   /// return 之后：死代码
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);
    jbc::analysis::ShapeResolver resolver;

    auto built = FrameView::build(f.cp(), *u, sigOf(true, "()V"), resolver);
    ASSERT_TRUE(built.is_ok());
    FrameView view = std::move(built).unwrap();

    const std::vector<InsnId>& order = u->dom.order();
    ASSERT_EQ(view.frames().size(), order.size());
    EXPECT_TRUE(view.reachable(order[0]));
    EXPECT_FALSE(view.reachable(order[1]));
    /// 不可达帧探测式返回 nullptr（FrameQuery 同纪律：不交付死帧）。
    EXPECT_EQ(view.frameOf(order[1]), nullptr);
    EXPECT_EQ(view.stackValueCount(order[1]), 0u);
    EXPECT_EQ(view.stackFromTop(order[1], 0), nullptr);
    EXPECT_FALSE(view.hasUninitThis(order[1]));
}

/// ITypeResolver 注入面：分支汇合的引用合并经适配器求解（装配点可插）。
TEST(MjAsmSemFrameViewTest, ResolverPlumbsThroughRefMerge)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name     = "m";
    m.desc     = "(Ljava/lang/String;Ljava/lang/StringBuilder;Z)V";
    m.maxStack = 2;
    m.emit     = [](CodeEmit& e) {
        const std::size_t join = e.newLabel();
        const std::size_t end  = e.newLabel();
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, join);   /// flag == 0 → else 分支
        e.op(Op::ALOAD_0);          /// String
        e.branch(Op::GOTO, end);
        e.bindLabel(join);
        e.op(Op::ALOAD_1);          /// StringBuilder
        e.bindLabel(end);
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);

    /// 自由求解器：任何引用对 → java/lang/Object（装配点为任意 callable）。
    FunctionTypeResolver resolver([](VType, VType) {
        return ca::core::Ok(VType::ref("java/lang/Object"));
    });
    auto built = FrameView::build(f.cp(), *u, sigOf(true, m.desc), resolver);
    ASSERT_TRUE(built.is_ok());
    FrameView view = std::move(built).unwrap();

    /// 汇合点 = POP：栈顶即合并结果。
    const std::vector<InsnId>& order = u->dom.order();
    const InsnId joinPop = order[order.size() - 2];
    const VType* merged  = view.stackFromTop(joinPop, 0);
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->k, VKind::Ref);
    EXPECT_EQ(merged->name, "java/lang/Object");

    /// 内置宽松求解器对照：引用族互并仍交付 Ref（形状档不细分具体类）。
    jbc::analysis::ShapeResolver shape;
    auto lenient = FrameView::build(f.cp(), *u, sigOf(true, m.desc), shape);
    ASSERT_TRUE(lenient.is_ok());
    const VType* shapeMerged = lenient.unwrap().stackFromTop(joinPop, 0);
    ASSERT_NE(shapeMerged, nullptr);
    EXPECT_EQ(shapeMerged->k, VKind::Ref);
}

/// 判据 6 探针：构造器超调用前 UninitThis 存活，超调用后翻转。
TEST(MjAsmSemFrameViewTest, UninitThisAliveBeforeSuperCall)
{
    ClassBytesBuilder b("T");
    MethodSpec ctor;
    ctor.access = 0x0001;   /// public（非 static）
    ctor.name   = "<init>";
    ctor.desc   = "()V";
    ctor.emit   = [](CodeEmit& e) {
        e.op(Op::ALOAD_0);
        e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({ctor}));
    CodeUnit*  u = f.code("<init>");
    ASSERT_NE(u, nullptr);
    jbc::analysis::ShapeResolver resolver;

    const MethodSig sig = sigOf(false, "()V", "<init>", "T");
    auto built = FrameView::build(f.cp(), *u, sig, resolver);
    ASSERT_TRUE(built.is_ok());
    FrameView view = std::move(built).unwrap();

    const std::vector<InsnId>& order = u->dom.order();
    const InsnId aload   = order[0];
    const InsnId super   = order[1];
    const InsnId ret     = order[2];

    const VType* thisBefore = view.localAt(aload, 0);
    ASSERT_NE(thisBefore, nullptr);
    EXPECT_EQ(thisBefore->k, VKind::UninitThis);
    EXPECT_TRUE(view.hasUninitThis(aload));

    const VType* thisAfter = view.localAt(super, 0);
    ASSERT_NE(thisAfter, nullptr);
    EXPECT_EQ(thisAfter->k, VKind::UninitThis);   /// 超调用执行前的到达帧仍为未初始化
    const VType* thisAtRet = view.localAt(ret, 0);
    ASSERT_NE(thisAtRet, nullptr);
    EXPECT_EQ(thisAtRet->k, VKind::Ref);
    EXPECT_EQ(thisAtRet->name, "T");
    EXPECT_FALSE(view.hasUninitThis(ret));
}

/// cat2 布局：值行 + 垫底行，fromTop 按值索引跳过垫底行。
TEST(MjAsmSemFrameViewTest, Cat2SlotLayout)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name     = "m";
    m.desc     = "(J)V";
    m.maxStack = 4;
    m.emit     = [](CodeEmit& e) {
        e.varInsn(Op::LLOAD, 0);
        e.op(Op::LCONST_1);
        e.op(Op::LADD);
        e.op(Op::POP2);
        e.op(Op::RETURN);
    };
    SemFixture f(b.assemble({m}));
    CodeUnit*  u = f.code("m");
    ASSERT_NE(u, nullptr);
    jbc::analysis::ShapeResolver resolver;

    auto built = FrameView::build(f.cp(), *u, sigOf(true, "(J)V"), resolver);
    ASSERT_TRUE(built.is_ok());
    FrameView view = std::move(built).unwrap();

    const std::vector<InsnId>& order = u->dom.order();
    const InsnId lload = order[0];
    const InsnId ladd  = order[2];

    const VType* slot0 = view.localAt(lload, 0);
    ASSERT_NE(slot0, nullptr);
    EXPECT_EQ(slot0->k, VKind::Long);
    EXPECT_EQ(view.localAt(lload, 1), nullptr);   /// cat2 垫底行不承载值

    ASSERT_EQ(view.stackValueCount(ladd), 2u);
    const VType* top = view.stackFromTop(ladd, 0);
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->k, VKind::Long);
    const VType* below = view.stackFromTop(ladd, 1);
    ASSERT_NE(below, nullptr);
    EXPECT_EQ(below->k, VKind::Long);
    EXPECT_EQ(view.stackFromTop(ladd, 2), nullptr);
}
