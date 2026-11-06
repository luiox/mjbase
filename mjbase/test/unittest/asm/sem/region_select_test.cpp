/// @file        region_select_test.cpp
/// @brief       RegionSelect 测试：策略全拒面逐条钉死 + 多候选排序 +
///              冲突保守拒绝 + 准入探针 +
///              JVM 真跑端到端小门（策略选区域 → planOutline → apply →
///              java 行为对拍；JDK 缺失 SKIP）。
///
/// 纯数据面用例（手构候选）走 view 缺省档——策略接口的事实面全部可缺省，
/// 缺哪面跳过哪层预检（region_select.hpp 注）；语料面用例走 builder/javac
/// 全事实面档。
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "jbc/analysis/frame.hpp"
#include "jbc/jbc.hpp"

#include "mjbase/asm/sem/outline_plan.hpp"
#include "mjbase/asm/sem/region_select.hpp"
#include "mjbase/asm/sem/region_view.hpp"

#include "sem_fixture.hpp"

#include "jdk_fixture_gtest.hpp"

namespace {

using jbc::core::Op;
using jbc::ir::CodeUnit;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::MethodSpec;
using mjbase::asm_sem::FrameView;
using mjbase::asm_sem::LivenessView;
using mjbase::asm_sem::OutlinePlan;
using mjbase::asm_sem::OutlineRequest;
using mjbase::asm_sem::RegionCandidate;
using mjbase::asm_sem::RegionDecision;
using mjbase::asm_sem::RegionSelectNote;
using mjbase::asm_sem::RegionSelectPolicy;
using mjbase::asm_sem::RegionSelectRequest;
using mjbase::asm_sem::RegionView;
using mjbase::asm_cfg::BlockId;

/// 语义等价助手：v1 保守策略的直认旗标档（手构候选同款，可读性优先）。
RegionCandidate straightCand(std::initializer_list<ca::u32> members)
{
    RegionCandidate c;
    for (ca::u32 m : members) {
        c.members.push_back(BlockId{m});
    }
    c.straightLine         = true;
    c.exceptionHomogeneous = true;
    return c;
}

/// 三元形态样本（then 臂 goto 链三块）：A（iload c; ifeq else）→
/// B1（iload a; goto B2）→ B2（iconst 2; goto B3）→ B3（imul; goto join）
/// 为嵌套候选梯 {B1,B2,B3} ⊃ {B2,B3} ⊃ {B3}；else 臂 C（iconst_5）
/// fallthrough 出口（末条非 goto，F4 形态）；join 读栈顶（iadd; ireturn）。
MethodSpec ladderSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IIIZ)I";
    m.maxStack  = 4;
    m.maxLocals = 4;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        const std::size_t b2      = e.newLabel();   /// 2
        const std::size_t b3      = e.newLabel();   /// 3
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, elseArm);    /// A（taken → C，fallthrough → B1）
        e.varInsn(Op::ILOAD, 0);        /// B1
        e.branch(Op::GOTO, b2);
        e.bindLabel(b2);
        e.op(Op::ICONST_2);             /// B2
        e.branch(Op::GOTO, b3);
        e.bindLabel(b3);
        e.op(Op::IMUL);                 /// B3
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C（fallthrough → join）
        e.bindLabel(join);
        e.op(Op::ICONST_0);             /// join
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 写局部臂样本（判据 3 F6 形态）：
/// B 臂写 t（槽 3）且 join 仍读 → 写回集非空；C 臂纯净（goto 出口）。
MethodSpec writeLocalSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IIIZ)I";
    m.maxStack  = 3;
    m.maxLocals = 4;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.op(Op::ICONST_0);             /// t = 0（汇合前初始化，帧可合并）
        e.varInsn(Op::ISTORE, 3);
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, elseArm);
        e.varInsn(Op::ILOAD, 0);        /// B：t = a（join 仍读 t → F6 拒）
        e.varInsn(Op::ISTORE, 3);
        e.varInsn(Op::ILOAD, 3);
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C：纯净臂
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.varInsn(Op::ILOAD, 3);        /// 读 t
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 死写臂样本（F6 正控）：B 臂写 t 但
/// join 不再读 → 写回集为空，F6 放行（planOutline 判据 3 同判）。
MethodSpec deadWriteSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IIIZ)I";
    m.maxStack  = 3;
    m.maxLocals = 4;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.op(Op::ICONST_0);
        e.varInsn(Op::ISTORE, 3);       /// t = 0
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, elseArm);
        e.varInsn(Op::ILOAD, 0);        /// B：t = a；a+1 为出口栈顶
        e.varInsn(Op::ISTORE, 3);
        e.varInsn(Op::ILOAD, 3);
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::ICONST_0);             /// 不读 t → B 的写出口后不活跃
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 构造器样本（判据 6 F5 形态）：super() 前的 then 臂 B（aload 0 帧含
/// UninitThis）为候选；else 臂 C 同段但 fallthrough 出口（F4 形态）。
MethodSpec ctorSpec()
{
    MethodSpec m;
    m.access    = 0x0001;   // public（实例方法）
    m.name      = "<init>";
    m.desc      = "(I)V";
    m.maxStack  = 2;
    m.maxLocals = 2;
    m.emit      = [](CodeEmit& e) {
        const std::size_t join = e.newLabel();   /// 0
        const std::size_t skip = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 1);
        e.branch(Op::IFEQ, skip);       /// A（taken → C，fallthrough → B）
        e.varInsn(Op::ALOAD, 0);        /// B：super() 前段，UninitThis 存活
        e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
        e.branch(Op::GOTO, join);
        e.bindLabel(skip);
        e.varInsn(Op::ALOAD, 0);        /// C（fallthrough → join，末条非 goto）
        e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
        e.bindLabel(join);
        e.op(Op::RETURN);               /// join
    };
    return m;
}

/// jsr 样本（判据 5 方法级事实）：jsr/ret 语形扫描 → 方法级全拒。
MethodSpec jsrSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "()I";
    m.maxStack  = 2;
    m.maxLocals = 1;
    m.emit      = [](CodeEmit& e) {
        const std::size_t sub = e.newLabel();   /// 0
        e.branch(Op::JSR, sub);         /// A
        e.op(Op::ICONST_5);             /// B
        e.op(Op::IRETURN);
        e.bindLabel(sub);
        e.varInsn(Op::ASTORE, 0);       /// 子程序：RA 落槽 0
        e.varInsn(Op::RET, 0);
    };
    return m;
}

/// 接力块样本（排除档形态，apply 调用点块同构）：A（iload a; iload c;
/// ifeq else，出口栈 [a]）→ R 单块 = [dup; invokestatic outline$pick$1;
/// goto join]（复位 DUP + lone invokestatic + 转发 goto）；else 臂 C
/// （iconst_5）fallthrough join（F4 形态）；join iadd 收两臂双值栈。
/// callOwner 可调（异类 owner 负控用——「非本设施产物不排除」）。
MethodSpec relaySpec(std::string_view callOwner = "T")
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IZ)I";
    m.maxStack  = 3;
    m.maxLocals = 2;
    m.emit      = [callOwner](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 0);        /// A：[a]
        e.varInsn(Op::ILOAD, 1);        /// [a, c]
        e.branch(Op::IFEQ, elseArm);    /// 弹 c → 出口栈 [a]
        e.op(Op::DUP);                  /// R：复位 DUP 形态 [a] → [a, a]
        e.invoke(Op::INVOKESTATIC, callOwner, "outline$pick$1", "(I)I");
        e.branch(Op::GOTO, join);       /// [a, ret]
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C：[a, 5]（fallthrough → join）
        e.bindLabel(join);
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 接力块的合成callee样本（apply newAccess 恒等 private|static|synthetic）。
MethodSpec outlineSyntheticSpec(ca::u16 access = 0x0002u | 0x0008u | 0x1000u)
{
    MethodSpec m;
    m.access    = access;
    m.name      = "outline$pick$1";
    m.desc      = "(I)I";
    m.maxStack  = 2;
    m.maxLocals = 1;
    m.emit      = [](CodeEmit& e) {
        e.varInsn(Op::ILOAD, 0);
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 装载前缀接力样本（局部入参装载形态，javac 语料首层切完同构）：
/// A（iload c; ifeq else）→ R 单块 = [iload a; invokestatic outline$pick$1;
/// goto join]；C = [iconst_5; goto join]（goto 出口的真实臂——非接力形态，
/// 排除档下保留）；join ireturn 收单值栈。
MethodSpec relayLoadSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IIZ)I";
    m.maxStack  = 2;
    m.maxLocals = 3;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, elseArm);    /// A
        e.varInsn(Op::ILOAD, 0);        /// R：局部入参装载（a）
        e.invoke(Op::INVOKESTATIC, "T", "outline$pick$1", "(I)I");
        e.branch(Op::GOTO, join);       /// [ret]
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C：真实臂（goto 出口，非接力）
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 语料策略上下文：会话保活 + pick 的视图/事实面装配（builder 语料档）。
struct SelectContext
{
    SemFixture                   fixture;
    CodeUnit*                    unit = nullptr;
    RegionView                   view{};
    std::vector<RegionCandidate> candidates;
    LivenessView                 live{};
    FrameView                    frames{};
    bool                         hasFrames = false;

    SelectContext(const Bytes& raw, const jbc::analysis::MethodSig& sig,
                  std::string_view methodName = "pick")
        : fixture(raw)
    {
        unit = fixture.code(methodName);
        EXPECT_NE(unit, nullptr);
        if (unit == nullptr) {
            return;
        }
        auto builtView = RegionView::build(*unit);
        EXPECT_TRUE(builtView.is_ok());
        if (builtView.is_err()) {
            return;   /// 失败面由用例体的候选数断言兜底（EXPECT 非致命）。
        }
        view       = std::move(builtView).unwrap();
        candidates = view.candidates();
        auto builtLive = LivenessView::build(*unit);
        EXPECT_TRUE(builtLive.is_ok());
        if (builtLive.is_err()) {
            return;
        }
        live = std::move(builtLive).unwrap();
        /// 帧面宽容装配（jsr 语料 Strict 引擎可能不交付帧；需要的用例
        /// 自行断言 hasFrames——帧正确性由 frame_view_test 钉死）。
        auto builtFrames = FrameView::build(fixture.cp(), *unit, sig, resolver);
        if (builtFrames.is_ok()) {
            frames    = std::move(builtFrames).unwrap();
            hasFrames = true;
        }
    }

    jbc::analysis::ShapeResolver resolver;
};

/// 按成员集找候选下标（断言可读性助手；找不到返回 SIZE_MAX）。
ca::usize indexOfMembers(const std::vector<RegionCandidate>& cands,
                         std::initializer_list<ca::u32> members)
{
    for (ca::usize i = 0; i < cands.size(); ++i) {
        if (cands[i].members.size() != members.size()) {
            continue;
        }
        bool      eq = true;
        ca::usize k  = 0;
        for (const BlockId m : cands[i].members) {
            if (m.index != *(members.begin() + static_cast<std::ptrdiff_t>(k++))) {
                eq = false;
                break;
            }
        }
        if (eq) {
            return i;
        }
    }
    return static_cast<ca::usize>(-1);
}

}   // namespace

/// 空候选集：全拒（NoCandidates），决策不携带优先序。
TEST(MjAsmSemRegionSelectTest, NoCandidatesNote)
{
    const std::vector<RegionCandidate> none;
    const RegionDecision d = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{}, RegionSelectRequest{none});
    EXPECT_FALSE(d.accepted);
    EXPECT_EQ(d.note, RegionSelectNote::NoCandidates);
    EXPECT_TRUE(d.order.empty());
}

/// 方法级 jsr 事实（判据 5）先于候选级过滤：全拒 MethodSubroutine。
TEST(MjAsmSemRegionSelectTest, MethodSubroutineRejectsAll)
{
    ClassBytesBuilder b("T");
    SelectContext     ctx{b.assemble({jsrSpec()}), sigOf(true, "()I", "pick", "T")};

    const RegionDecision d = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{},
        RegionSelectRequest{ctx.candidates, &ctx.view, ctx.unit});
    EXPECT_FALSE(d.accepted);
    EXPECT_EQ(d.note, RegionSelectNote::MethodSubroutine);
    EXPECT_TRUE(d.order.empty());
}

/// 嵌套候选梯排序（规模次之）：{B1,B2,B3} ⊃ {B2,B3} ⊃ {B3} 全过预检，
/// 优先序按成员数升序，纯事实档选中最内层 {B3}；else 臂（末条非 goto）
/// 被 F4 过滤不进序。
TEST(MjAsmSemRegionSelectTest, NestingLadderRanksSmallestFirst)
{
    ClassBytesBuilder b("T");
    SelectContext     ctx{b.assemble({ladderSpec()}),
                          sigOf(true, "(IIIZ)I", "pick", "T")};
    ASSERT_EQ(ctx.candidates.size(), 4u);   /// 梯三档 + else 臂（F4 形态）

    const RegionDecision d = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{},
        RegionSelectRequest{ctx.candidates, &ctx.view, ctx.unit, nullptr,
                            &ctx.live, nullptr});
    ASSERT_TRUE(d.accepted);
    EXPECT_EQ(d.note, RegionSelectNote::Ok);
    ASSERT_EQ(d.order.size(), 3u);

    const ca::usize iFull  = indexOfMembers(ctx.candidates, {1u, 2u, 3u});
    const ca::usize iMid   = indexOfMembers(ctx.candidates, {2u, 3u});
    const ca::usize iInner = indexOfMembers(ctx.candidates, {3u});
    const ca::usize iElse  = indexOfMembers(ctx.candidates, {4u});
    ASSERT_NE(iFull, static_cast<ca::usize>(-1));
    ASSERT_NE(iMid, static_cast<ca::usize>(-1));
    ASSERT_NE(iInner, static_cast<ca::usize>(-1));
    ASSERT_NE(iElse, static_cast<ca::usize>(-1));
    EXPECT_EQ(d.order[0], iInner);
    EXPECT_EQ(d.order[1], iMid);
    EXPECT_EQ(d.order[2], iFull);
    EXPECT_EQ(d.chosen, iInner);

    /// F4 面：else 臂（iconst_5 fallthrough 出口）不进优先序。
    EXPECT_EQ(std::find(d.order.begin(), d.order.end(), iElse), d.order.end());
}

/// 排序取向开关：preferSmallest = false 时保枚举序（下标升序交付）。
TEST(MjAsmSemRegionSelectTest, EnumOrderWhenNotPreferSmallest)
{
    ClassBytesBuilder b("T");
    SelectContext     ctx{b.assemble({ladderSpec()}),
                          sigOf(true, "(IIIZ)I", "pick", "T")};

    RegionSelectPolicy policy;
    policy.preferSmallest = false;
    const RegionDecision d = mjbase::asm_sem::selectRegion(
        policy, RegionSelectRequest{ctx.candidates, &ctx.view, ctx.unit});
    ASSERT_TRUE(d.accepted);
    ASSERT_EQ(d.order.size(), 3u);
    EXPECT_TRUE(std::is_sorted(d.order.begin(), d.order.end()));
    EXPECT_EQ(d.chosen, d.order.front());
}

/// 判据 3 写回面（F6）：写局部臂被 LivenessView 预检过滤，纯臂入选；
/// 无活跃面时同一样本选中写臂（事实面价值对照）。
TEST(MjAsmSemRegionSelectTest, WritebackLivenessPrefilter)
{
    ClassBytesBuilder b("T");
    SelectContext     ctx{b.assemble({writeLocalSpec()}),
                          sigOf(true, "(IIIZ)I", "pick", "T")};
    ASSERT_EQ(ctx.candidates.size(), 2u);

    const ca::usize iWrite = indexOfMembers(ctx.candidates, {1u});
    const ca::usize iClean = indexOfMembers(ctx.candidates, {2u});
    ASSERT_NE(iWrite, static_cast<ca::usize>(-1));
    ASSERT_NE(iClean, static_cast<ca::usize>(-1));

    const RegionDecision gated = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{},
        RegionSelectRequest{ctx.candidates, &ctx.view, ctx.unit, nullptr,
                            &ctx.live, nullptr});
    ASSERT_TRUE(gated.accepted);
    ASSERT_EQ(gated.order.size(), 1u);
    EXPECT_EQ(gated.order[0], iClean);
    EXPECT_EQ(gated.chosen, iClean);

    const RegionDecision ungated = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{},
        RegionSelectRequest{ctx.candidates, &ctx.view, ctx.unit});
    ASSERT_TRUE(ungated.accepted);
    EXPECT_EQ(ungated.order.size(), 2u);
    EXPECT_EQ(ungated.chosen, iWrite);   /// 等尺寸平局按首成员块号破平
}

/// 判据 3 死写正控：写槽出口后不活跃 → F6 放行。
TEST(MjAsmSemRegionSelectTest, DeadWritePassesWritebackPrefilter)
{
    ClassBytesBuilder b("T");
    SelectContext     ctx{b.assemble({deadWriteSpec()}),
                          sigOf(true, "(IIIZ)I", "pick", "T")};

    const RegionDecision d = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{},
        RegionSelectRequest{ctx.candidates, &ctx.view, ctx.unit, nullptr,
                            &ctx.live, nullptr});
    ASSERT_TRUE(d.accepted);
    EXPECT_EQ(d.order.size(), 2u);
    EXPECT_EQ(d.chosen, indexOfMembers(ctx.candidates, {1u}));
}

/// 判据 6 UninitThis 面（F5）：构造器 super() 前段的候选全被过滤
/// （AllFiltered）；缺帧面时同一段入选（事实面缺省语义对照）。
TEST(MjAsmSemRegionSelectTest, UninitThisPrefilter)
{
    ClassBytesBuilder b("T");
    SelectContext     ctx{b.assemble({ctorSpec()}),
                          sigOf(false, "(I)V", "<init>", "T"), "<init>"};
    ASSERT_TRUE(ctx.hasFrames);
    ASSERT_EQ(ctx.candidates.size(), 2u);

    const RegionDecision gated = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{},
        RegionSelectRequest{ctx.candidates, &ctx.view, ctx.unit, &ctx.frames,
                            &ctx.live, nullptr});
    EXPECT_FALSE(gated.accepted);
    EXPECT_EQ(gated.note, RegionSelectNote::AllFiltered);
    EXPECT_TRUE(gated.order.empty());

    const RegionDecision ungated = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{},
        RegionSelectRequest{ctx.candidates, &ctx.view, ctx.unit});
    ASSERT_TRUE(ungated.accepted);
    EXPECT_EQ(ungated.order.size(), 1u);   /// else 臂为 F4 形态，不进序
}

/// 旗标面全滤（纯数据档）：非直线候选 + requireStraightLine 开 → 全拒。
TEST(MjAsmSemRegionSelectTest, FlagFilterAllFiltered)
{
    std::vector<RegionCandidate> cands{
        straightCand({1u, 2u, 3u}),
    };
    cands[0].straightLine = false;

    const RegionDecision d = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{}, RegionSelectRequest{cands});
    EXPECT_FALSE(d.accepted);
    EXPECT_EQ(d.note, RegionSelectNote::AllFiltered);
    EXPECT_TRUE(d.order.empty());

    RegionSelectPolicy loose;
    loose.requireStraightLine = false;
    const RegionDecision ok = mjbase::asm_sem::selectRegion(
        loose, RegionSelectRequest{cands});
    EXPECT_TRUE(ok.accepted);
    EXPECT_EQ(ok.chosen, 0u);
}

/// 冲突保守拒绝（纯数据档）：等尺寸部分重叠对（相交且互异）→ 全拒；
/// 关闭冲突档则按首成员块号破平挑选。
TEST(MjAsmSemRegionSelectTest, OverlapTieConflictRejectsAll)
{
    std::vector<RegionCandidate> cands{
        straightCand({2u, 3u}),
        straightCand({3u, 4u}),
    };

    const RegionDecision d = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{}, RegionSelectRequest{cands});
    EXPECT_FALSE(d.accepted);
    EXPECT_EQ(d.note, RegionSelectNote::ConflictAmbiguous);
    EXPECT_EQ(d.order.size(), 2u);   /// 序仍携带通过预检的候选（诊断面）

    RegionSelectPolicy loose;
    loose.rejectOverlapTie = false;
    const RegionDecision ok = mjbase::asm_sem::selectRegion(
        loose, RegionSelectRequest{cands});
    ASSERT_TRUE(ok.accepted);
    EXPECT_EQ(ok.chosen, 0u);   /// 等尺寸平局：首成员块号小者优先
}

/// 嵌套与重复不构成冲突（纯数据档）：嵌套取最内层；重复按枚举序取首。
TEST(MjAsmSemRegionSelectTest, NestedAndDuplicateNotConflict)
{
    std::vector<RegionCandidate> nested{
        straightCand({1u, 2u, 3u}),
        straightCand({2u, 3u}),
    };
    const RegionDecision dNest = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{}, RegionSelectRequest{nested});
    ASSERT_TRUE(dNest.accepted);
    EXPECT_EQ(dNest.chosen, 1u);

    std::vector<RegionCandidate> dup{
        straightCand({2u, 3u}),
        straightCand({2u, 3u}),
    };
    const RegionDecision dDup = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{}, RegionSelectRequest{dup});
    ASSERT_TRUE(dDup.accepted);
    EXPECT_EQ(dDup.chosen, 0u);
}

/// 准入探针：按优先序逐个探测，首个通过者
/// 入选；全不通过 → NoneAdmitted（全拒面第三档）。
TEST(MjAsmSemRegionSelectTest, AdmitProbeWalkAndNoneAdmitted)
{
    ClassBytesBuilder b("T");
    SelectContext     ctx{b.assemble({ladderSpec()}),
                          sigOf(true, "(IIIZ)I", "pick", "T")};

    RegionSelectRequest req{ctx.candidates, &ctx.view, ctx.unit};
    req.admit = [](const RegionCandidate& c) { return c.members.size() >= 2u; };
    const RegionDecision d = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{}, req);
    ASSERT_TRUE(d.accepted);
    EXPECT_EQ(d.chosen, indexOfMembers(ctx.candidates, {2u, 3u}));

    RegionSelectRequest rejectAll{ctx.candidates, &ctx.view, ctx.unit};
    rejectAll.admit = [](const RegionCandidate&) { return false; };
    const RegionDecision none = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{}, rejectAll);
    EXPECT_FALSE(none.accepted);
    EXPECT_EQ(none.note, RegionSelectNote::NoneAdmitted);
    EXPECT_EQ(none.order.size(), 3u);
}

/// JVM 真跑端到端小门：javac 语料 → 策略（全事实面 + planOutline 准入
/// 探针）选区域 → apply → java 运行行为与拆分前逐字一致（-Xverify:all
/// 全程验证；JDK 缺失自动 SKIP）。
TEST(MjAsmSemRegionSelectTest, JvmGateStrategyPickedRegionBehaviorPreserved)
{
    namespace fs = std::filesystem;
    static const auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path scratch =
        fs::temp_directory_path()
        / ("mjbase_region_select_gate_" + std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(scratch, ec);
    ASSERT_FALSE(ec);
    const fs::path src = scratch / "OutlineSample.java";
    {
        std::ofstream out(src, std::ios::binary);
        out << "public class OutlineSample {\n"
               "    static int pick(int a, int b, boolean c) {\n"
               "        return (c ? a * 2 : b + 3) + 100;\n"
               "    }\n"
               "}\n";
    }
    ASSERT_TRUE(test::jdk::compileJava(tc, src, scratch));

    Bytes original;
    {
        std::ifstream in(scratch / "OutlineSample.class", std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>());
    }
    ASSERT_GT(original.size(), 0u);

    auto made = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView{original.data(), original.size()});
    ASSERT_TRUE(made.is_ok());
    auto session = std::move(made).unwrap();
    auto visits  = jbc::ir::collectCodeUnits(*session);
    CodeUnit*             pick   = nullptr;
    jbc::ir::MethodId     pickId{};
    jbc::analysis::MethodSig gateSig;
    for (const jbc::ir::CodeVisit& v : visits) {
        if (v.kind != jbc::ir::CodeVisit::Kind::Ok) {
            continue;
        }
        const jbc::ir::MethodUnit& mu = session->unit().methods[v.methodIdx];
        if (session->unit().symbols().str(mu.name) == "pick") {
            pick    = v.unit.get();
            pickId  = mu.id;
            gateSig = sigOf(true, session->unit().symbols().str(mu.desc), "pick",
                            "OutlineSample");
        }
    }
    ASSERT_NE(pick, nullptr);

    auto builtView = RegionView::build(*pick);
    ASSERT_TRUE(builtView.is_ok());
    RegionView view = std::move(builtView).unwrap();
    const std::vector<RegionCandidate> candidates = view.candidates();
    ASSERT_FALSE(candidates.empty());
    auto builtLive = LivenessView::build(*pick);
    ASSERT_TRUE(builtLive.is_ok());
    LivenessView live = std::move(builtLive).unwrap();
    jbc::analysis::ShapeResolver resolver;
    auto builtFrames = FrameView::build(session->backing().cp, *pick, gateSig,
                                        resolver);
    ASSERT_TRUE(builtFrames.is_ok());
    FrameView frames = std::move(builtFrames).unwrap();

    /// 策略 + 准入探针（planOutline 识别包成探针——判据级精确准入）。
    RegionSelectRequest req{candidates, &view, pick, &frames, &live, nullptr};
    req.admit = [&](const RegionCandidate& c) {
        return mjbase::asm_sem::planOutline(
                   OutlineRequest{*session, pickId, gateSig, resolver, view, c, 1})
            .is_ok();
    };
    const RegionDecision d = mjbase::asm_sem::selectRegion(RegionSelectPolicy{}, req);
    ASSERT_TRUE(d.accepted) << mjbase::asm_sem::to_string(d.note);

    auto planned = mjbase::asm_sem::planOutline(
        OutlineRequest{*session, pickId, gateSig, resolver, view,
                       candidates[d.chosen], 1});
    ASSERT_TRUE(planned.is_ok());
    OutlinePlan plan = std::move(planned).unwrap();
    auto applied = plan.apply(*session, resolver);
    ASSERT_TRUE(applied.is_ok()) << mjbase::asm_sem::to_string(applied.unwrap_err());

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    const Bytes outlined = std::move(written).unwrap();

    const std::string runnerBody =
        "System.out.println(OutlineSample.pick(3, 4, true));"
        "System.out.println(OutlineSample.pick(3, 4, false));";

    test::jdk::RunnerSpec before;
    before.classes    = {{"OutlineSample", original}};
    before.runnerBody = runnerBody;
    std::string beforeOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, before, &beforeOut));

    test::jdk::RunnerSpec after;
    after.classes    = {{"OutlineSample", outlined}};
    after.runnerBody = runnerBody;
    std::string afterOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, after, &afterOut));

    EXPECT_EQ(afterOut, "106\n107\n");
    EXPECT_EQ(afterOut, beforeOut);
    fs::remove_all(scratch, ec);
}


// ---------------------------------------------------------------------------
// v2 排除档：合成接力块跳过
// ---------------------------------------------------------------------------

/// v1 缺省档不排除：接力形态候选照常进优先序，等尺寸平局
/// 按首成员程序序选中接力块 R；真实臂 C 同在序中。
TEST(MjAsmSemRegionSelectTest, RelayBlockSelectableByDefault)
{
    ClassBytesBuilder b("T");
    SelectContext     ctx{b.assemble({relayLoadSpec(), outlineSyntheticSpec()}),
                          sigOf(true, "(IIZ)I", "pick", "T")};
    ASSERT_EQ(ctx.candidates.size(), 2u);   /// 接力块 R + 真实臂 C

    const RegionDecision d = mjbase::asm_sem::selectRegion(
        RegionSelectPolicy{},
        RegionSelectRequest{ctx.candidates, &ctx.view, ctx.unit});
    ASSERT_TRUE(d.accepted);
    EXPECT_EQ(d.order.size(), 2u);
    EXPECT_EQ(d.chosen, indexOfMembers(ctx.candidates, {1u}));
}

/// 排除档开启：装载前缀接力块被跳过（不进优先序、不进探针），真实臂 C
/// 保留入选——部分排除不记 AllExcluded（仍有通过预检的候选）。
TEST(MjAsmSemRegionSelectTest, RelayLoadPrefixExcludedKeepsGenuineRegion)
{
    ClassBytesBuilder b("T");
    SelectContext     ctx{b.assemble({relayLoadSpec(), outlineSyntheticSpec()}),
                          sigOf(true, "(IIZ)I", "pick", "T")};

    RegionSelectPolicy policy;
    policy.excludeOutlineRelay = true;
    RegionSelectRequest req{ctx.candidates, &ctx.view, ctx.unit};
    req.session = ctx.fixture.session.get();
    const RegionDecision d = mjbase::asm_sem::selectRegion(policy, req);
    ASSERT_TRUE(d.accepted);
    ASSERT_EQ(d.order.size(), 1u);
    EXPECT_EQ(d.chosen, indexOfMembers(ctx.candidates, {2u}));

    /// 复位 DUP 形态（apply retIsEntryCopy 同构：[dup; invokestatic; goto]）
    /// 同判接力块：仅剩接力候选时全拒 AllExcluded（批量驱动耗尽映射源）。
    ClassBytesBuilder b2("T");
    SelectContext     dup{b2.assemble({relaySpec(), outlineSyntheticSpec()}),
                          sigOf(true, "(IZ)I", "pick", "T")};
    ASSERT_EQ(dup.candidates.size(), 2u);
    RegionSelectRequest dupReq{dup.candidates, &dup.view, dup.unit};
    dupReq.session = dup.fixture.session.get();
    const RegionDecision dDup = mjbase::asm_sem::selectRegion(policy, dupReq);
    EXPECT_FALSE(dDup.accepted);
    EXPECT_EQ(dDup.note, RegionSelectNote::AllExcluded);
    EXPECT_TRUE(dDup.order.empty());
}

/// 身份事实门（planInline NotOutlineSynthetic 同源三元）：access 恒等不符
/// （public|static）或异类 owner（invoke 指向他类 outline$ 名）= 非本设施
/// 产物，不排除、照常入选。
TEST(MjAsmSemRegionSelectTest, RelayIdentityFactsGateExclusion)
{
    RegionSelectPolicy policy;
    policy.excludeOutlineRelay = true;

    /// access 不符：outline$ 前缀 + 同类，但非 private|static|synthetic。
    {
        ClassBytesBuilder b("T");
        SelectContext ctx{b.assemble({relaySpec(), outlineSyntheticSpec(0x0009u)}),
                          sigOf(true, "(IZ)I", "pick", "T")};
        RegionSelectRequest req{ctx.candidates, &ctx.view, ctx.unit};
        req.session = ctx.fixture.session.get();
        const RegionDecision d = mjbase::asm_sem::selectRegion(policy, req);
        ASSERT_TRUE(d.accepted);
        EXPECT_EQ(d.chosen, indexOfMembers(ctx.candidates, {1u}));
    }
    /// 异类 owner：调用点引用他类（U）同名方法，本类方法身份不构成事实。
    {
        ClassBytesBuilder b("T");
        SelectContext ctx{b.assemble({relaySpec("U"), outlineSyntheticSpec()}),
                          sigOf(true, "(IZ)I", "pick", "T")};
        RegionSelectRequest req{ctx.candidates, &ctx.view, ctx.unit};
        req.session = ctx.fixture.session.get();
        const RegionDecision d = mjbase::asm_sem::selectRegion(policy, req);
        ASSERT_TRUE(d.accepted);
        EXPECT_EQ(d.chosen, indexOfMembers(ctx.candidates, {1u}));
    }
}

/// 缺面保守回落（纯数据档）：无 view/unit/session 时排除档无从判定身份
/// 事实，一律不排除（v1 口径）——排除档绝不比事实面更激进。
TEST(MjAsmSemRegionSelectTest, HandBuiltWithoutFacesNeverExcluded)
{
    std::vector<RegionCandidate> cands{straightCand({1u})};
    RegionSelectPolicy policy;
    policy.excludeOutlineRelay = true;
    const RegionDecision d = mjbase::asm_sem::selectRegion(
        policy, RegionSelectRequest{cands});
    ASSERT_TRUE(d.accepted);
    EXPECT_EQ(d.chosen, 0u);
    EXPECT_EQ(d.note, RegionSelectNote::Ok);
}
