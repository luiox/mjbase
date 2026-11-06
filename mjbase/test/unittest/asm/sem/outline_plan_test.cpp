/// @file        outline_plan_test.cpp
/// @brief       OutlinePlan 测试：判据拒绝逐条钉死 + apply 结构断言 +
///              JVM 真跑门（javac → outline → java 行为对拍，JDK 缺失 SKIP）。
///
/// 判据口径见 outline_plan.hpp 头注。
#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "jbc/analysis/frame.hpp"
#include "jbc/analysis/smt_build.hpp"
#include "jbc/jbc.hpp"

#include "mjbase/asm/sem/outline_plan.hpp"
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
using mjbase::asm_sem::OutlineError;
using mjbase::asm_sem::OutlinePlan;
using mjbase::asm_sem::OutlineRequest;
using mjbase::asm_sem::RegionCandidate;
using mjbase::asm_sem::RegionView;

/// 三元形态样本：static int pick(III Z)——then 臂 B（iload a; iconst 2;
/// imul; goto E）为可 outline 直线区域，else 臂 C（iconst_5）fallthrough
/// 出口（LastInsnNotGoto 形态），汇合块 E（iconst_0; iadd; ireturn）。
MethodSpec pickSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IIIZ)I";
    m.maxStack  = 4;
    m.maxLocals = 4;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, elseArm);    /// A（taken → C，fallthrough → B）
        e.varInsn(Op::ILOAD, 0);        /// B
        e.op(Op::ICONST_2);
        e.op(Op::IMUL);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C（fallthrough → E）
        e.bindLabel(join);
        e.op(Op::ICONST_0);             /// E
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 含写局部臂的样本：B 臂写 t（槽 3），汇合块读 t → 出口后仍活跃
/// （判据 3 口径：写回集 = 被写 ∩ 出口后活跃 非空 → 拒绝）。
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
        e.op(Op::ICONST_0);             /// t = 0（两路汇合前初始化，帧可合并）
        e.varInsn(Op::ISTORE, 3);
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, elseArm);
        e.varInsn(Op::ILOAD, 0);        /// B：t = a（出口后 join 仍读 t → 拒）
        e.varInsn(Op::ISTORE, 3);
        e.varInsn(Op::ILOAD, 3);
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C：纯净臂（goto 出口，可 outline 对照）
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.varInsn(Op::ILOAD, 3);        /// 读 t → B 臂的写出口后仍活跃
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 死写臂样本：B 臂写 t 但汇合块不再读 → 写回集为空，判据 3 按活跃性
/// 口径接受（「区域不写局部槽」不是判据本身）。
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
        e.varInsn(Op::ILOAD, 0);        /// B：t = a；a+1 为出口栈顶（返回值）
        e.varInsn(Op::ISTORE, 3);
        e.varInsn(Op::ILOAD, 3);
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C：纯净臂
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::ICONST_0);             /// 不读 t → B 的写出口后不活跃
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 恒被重写样本：B 臂先写后读 t（读的到达定义全在区域内）→ t 不进参数集，
/// 参数集为空（copy-set 最小化的 def-use 剔除档）。
MethodSpec readTrimSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IZ)I";
    m.maxStack  = 3;
    m.maxLocals = 4;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 1);
        e.branch(Op::IFEQ, elseArm);
        e.op(Op::BIPUSH);
        e.u8(7);
        e.varInsn(Op::ISTORE, 3);       /// t = 7（区域内定义）
        e.varInsn(Op::ILOAD, 3);        /// 读 t：到达定义 = 区域内 istore → 剔除
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::ICONST_0);
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// dup_x1 + pop ≡ SWAP 跨切点样本：出口 [b, a] 与入口
/// [a, b] 前缀失配 → ExitShapeImpure（结构性拒绝，判据 2 已拒）。
MethodSpec swapCrossSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(III)I";
    m.maxStack  = 4;
    m.maxLocals = 3;
    m.emit      = [](CodeEmit& e) {
        const std::size_t join = e.newLabel();
        e.varInsn(Op::ILOAD, 0);        /// [a]
        e.varInsn(Op::ILOAD, 1);        /// [a, b]
        e.varInsn(Op::ILOAD, 2);        /// [a, b, f]
        e.branch(Op::IFEQ, join);       /// 弹 f：两臂均携 [a, b]
        e.op(Op::DUP_X1);               /// B：[a, b] → [b, a, b]
        e.op(Op::POP);                  /// [b, a]
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::ISUB);                 /// B 臂 b-a；C 臂 a-b
        e.op(Op::IRETURN);
    };
    return m;
}

/// goto 入边 + 局部参数样本：区域
/// 头标签 = B 臂 goto 目标（分支入边），区域内读 B 臂写入的槽 2；参数
/// 装载块标签锚定首条装载、随入边走——JVM 真跑对拍钉住该形态。
MethodSpec gotoEntryLocalParamSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(III)I";
    m.maxStack  = 3;
    m.maxLocals = 3;
    m.emit      = [](CodeEmit& e) {
        const std::size_t join  = e.newLabel();   /// 0：汇合（ifeq 直达）
        const std::size_t regHead = e.newLabel(); /// 1：区域头（B 臂 goto 直达）
        e.varInsn(Op::ILOAD, 0);        /// [a]
        e.varInsn(Op::ILOAD, 1);        /// [a, b]
        e.varInsn(Op::ILOAD, 2);        /// [a, b, f]
        e.branch(Op::IFEQ, join);       /// C 路：直达 join（携 [a, b]）
        e.varInsn(Op::ISTORE, 2);       /// B 臂：t2 = b（槽 2 复用为临时）
        e.branch(Op::GOTO, regHead);    /// → 区域头
        e.bindLabel(regHead);
        e.varInsn(Op::ILOAD, 2);        /// 区域成员：[a] → [a, t2]（槽 2 入参）
        e.branch(Op::GOTO, join);       /// 出口 goto
        e.bindLabel(join);
        e.op(Op::IADD);                 /// 两路汇合：a + b
        e.op(Op::IRETURN);
    };
    return m;
}

/// 边界 DUP 形态样本：B 臂 = [dup, goto E]——出口栈顶为入口值的复制
/// （返回值非区域内产出 → RetNotProduced）。A 先 dup 再分支（IFEQ 弹条件，
/// 两条后继均携 [a]）；E 仅 B 一个前驱（[a,a] 出口路径独占）。
MethodSpec boundaryDupSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(I)I";
    m.maxStack  = 3;
    m.maxLocals = 1;
    m.emit      = [](CodeEmit& e) {
        const std::size_t ret0 = e.newLabel();   /// 0
        const std::size_t join = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 0);        /// A：[a]
        e.op(Op::DUP);                  /// A：[a, a]
        e.branch(Op::IFEQ, ret0);       /// 弹 1：taken → ret0（[a]），直落 → B（[a]）
        e.op(Op::DUP);                  /// B：[a] → [a, a]，goto join
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::POP);                  /// E：[a, a] → [a] → ireturn
        e.op(Op::IRETURN);
        e.bindLabel(ret0);
        e.op(Op::IRETURN);              /// [a] → ireturn
    };
    return m;
}

struct PlanContext
{
    SemFixture                          fixture;
    CodeUnit*                           unit = nullptr;
    RegionView                          view{};
    std::vector<RegionCandidate>        candidates;   ///< 物化一次（指针稳定）
    jbc::analysis::ShapeResolver        resolver;

    explicit PlanContext(const Bytes& bytes)
        : fixture(bytes)
    {
        unit = fixture.code("pick");
        auto built = RegionView::build(*unit);
        if (built.is_ok()) {
            view = std::move(built).unwrap();
            candidates = view.candidates();
        }
    }

    jbc::analysis::MethodSig sig() const
    {
        return sigOf(true, unitDesc(), "pick", "T");
    }

    std::string_view unitDesc() const
    {
        const jbc::ir::ClassUnit&  cu  = fixture.session->unit();
        const jbc::ir::MethodUnit* mu  = cu.method(cu.findMethodId("pick"));
        return mu != nullptr ? cu.symbols().str(mu->desc) : std::string_view();
    }
};

/// 可 outline 的 then 臂候选：单成员、含 IMUL。
const RegionCandidate* thenArmOf(const PlanContext& ctx)
{
    for (const RegionCandidate& c : ctx.candidates) {
        if (c.members.size() != 1u) {
            continue;
        }
        const mjbase::asm_cfg::BasicBlock* b = ctx.view.cfg().block(c.members[0]);
        for (const jbc::ir::InsnId id : b->insns) {
            const jbc::ir::Insn* in = ctx.unit->dom.peek(id);
            if (in != nullptr && in->op == Op::IMUL) {
                return &c;
            }
        }
    }
    return nullptr;
}

OutlineRequest requestOf(PlanContext& ctx, const RegionCandidate& region, ca::u32 seq = 1)
{
    return OutlineRequest{*ctx.fixture.session,
                           ctx.fixture.session->unit().findMethodId("pick"),
                           ctx.sig(),
                           ctx.resolver,
                           ctx.view,
                           region,
                           seq};
}

/// 行/属性结构断言助手定义在 sem_fixture.hpp，供 sem 面多组用例共用。

/// 候选区域首条指令的布局 pc（行锚定坐标；区域成员块首指令 srcPc）。
ca::u32 regionFirstPc(PlanContext& ctx, const RegionCandidate& r)
{
    const mjbase::asm_cfg::BasicBlock* blk = ctx.view.cfg().block(r.members[0]);
    const jbc::ir::Insn* first = ctx.unit->dom.peek(blk->insns.front());
    EXPECT_NE(first, nullptr);
    return first != nullptr ? first->srcPc : 0u;
}

}   // namespace

/// 判据逐条：直线 + 空 handler + 出口 goto 的 then 臂可识别，处方数据自洽。
TEST(MjAsmSemOutlinePlanTest, PlanThenArmHappyPath)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({pickSpec()}));
    const RegionCandidate* arm = thenArmOf(ctx);
    ASSERT_NE(arm, nullptr);
    EXPECT_TRUE(arm->straightLine);
    EXPECT_TRUE(arm->exceptionHomogeneous);
    EXPECT_TRUE(arm->handlers.empty());

    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, *arm));
    ASSERT_TRUE(planned.is_ok());
    OutlinePlan plan = std::move(planned).unwrap();

    EXPECT_EQ(std::string_view(plan.newName), "outline$pick$1");
    EXPECT_EQ(std::string_view(plan.newDesc), "(I)I");   /// 局部入参 a + int 返回
    ASSERT_EQ(plan.params.size(), 1u);
    EXPECT_EQ(plan.params[0].kind, mjbase::asm_sem::SlotCopy::Kind::Local);
    EXPECT_EQ(plan.params[0].oldLocal, 0u);
    EXPECT_EQ(plan.params[0].newSlot, 0u);
    EXPECT_EQ(std::string_view(plan.params[0].desc), "I");
    EXPECT_TRUE(plan.hasRet);
    EXPECT_EQ(std::string_view(plan.ret.desc), "I");
    ASSERT_EQ(plan.locals.size(), 1u);
    EXPECT_EQ(plan.locals[0].oldSlot, 0u);
    EXPECT_EQ(plan.locals[0].newSlot, 0u);
    /// 体 = iload(显式形态) iconst_2 imul；出口 goto 与链 glue 剥离。
    ASSERT_EQ(plan.body.size(), 3u);
    EXPECT_EQ(plan.body[0].op, Op::ILOAD);
    EXPECT_EQ(plan.body[0].a, 0);
    EXPECT_EQ(plan.body[1].op, Op::ICONST_2);
    EXPECT_EQ(plan.body[2].op, Op::IMUL);
    EXPECT_TRUE(plan.validate().is_ok());
}

/// apply 结构断言：N 生成（private static synthetic）、调用点 invokestatic、
/// 区域删除、SMT 再生、LNT 行按归属拆分。
TEST(MjAsmSemOutlinePlanTest, ApplyStructureAndLntSplit)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({pickSpec()}));
    const RegionCandidate* arm = thenArmOf(ctx);
    ASSERT_NE(arm, nullptr);

    /// 臂内 pc：区域首条指令（B 头）的布局 pc；joinPc 取区域外锚。
    ca::u32 imulPc = 0;
    for (const jbc::ir::InsnId id : ctx.unit->dom.order()) {
        const jbc::ir::Insn* in = ctx.unit->dom.peek(id);
        if (in != nullptr && in->op == Op::IMUL) {
            imulPc = in->srcPc;
        }
    }
    ASSERT_NE(imulPc, 0u);
    const ca::u32 armPc    = regionFirstPc(ctx, *arm);
    const ca::u32 joinPc   = imulPc + 4u;   /// goto(3) + iconst_5(1) 之后
    attachRows(*ctx.fixture.session, ctx.fixture.session->unit().findMethodId("pick"),
               {{0u, 10u}, {armPc, 20u}, {joinPc, 30u}}, {});

    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, *arm));
    if (planned.is_err()) {
        ADD_FAILURE() << "DBG lnt planOutline err=" << static_cast<int>(planned.unwrap_err());
    }
    ASSERT_TRUE(planned.is_ok());
    OutlinePlan plan = std::move(planned).unwrap();
    ASSERT_EQ(plan.lntMoves.size(), 1u);   /// 臂内 LNT 行迁 N
    EXPECT_EQ(plan.lntMoves[0].line, 20u);

    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok());
    const mjbase::asm_sem::OutlineOutcome out = std::move(applied).unwrap();

    jbc::ir::ClassUnit& cu = ctx.fixture.session->unit();
    const jbc::ir::MethodUnit* nMu = cu.method(out.outlined);
    ASSERT_NE(nMu, nullptr);
    EXPECT_EQ(cu.symbols().str(nMu->name), "outline$pick$1");
    EXPECT_EQ(nMu->access, 0x0002u | 0x0008u | 0x1000u);
    EXPECT_EQ(cu.symbols().str(nMu->desc), "(I)I");

    /// N 体：iload_0(显式) iconst_2 imul ireturn；无 SMT（无控制流目标）。
    ASSERT_NE(nMu->code, nullptr);
    const std::vector<jbc::ir::InsnId>& nOrder = nMu->code->dom.order();
    ASSERT_EQ(nOrder.size(), 4u);
    const Op expectOps[] = {Op::ILOAD, Op::ICONST_2, Op::IMUL, Op::IRETURN};
    for (ca::usize i = 0; i < nOrder.size(); ++i) {
        const jbc::ir::Insn* in = nMu->code->dom.peek(nOrder[i]);
        ASSERT_NE(in, nullptr);
        EXPECT_EQ(in->op, expectOps[i]);
    }

    /// 调用点：invokestatic 解析回 N。
    const jbc::ir::Insn* call = ctx.unit->dom.peek(out.callInsn);
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->op, Op::INVOKESTATIC);
    jbc::ir::ClassSession::MemberSem sem;
    ASSERT_TRUE(ctx.fixture.session->memberOf(*call, sem));
    EXPECT_EQ(sem.owner, "T");
    EXPECT_EQ(sem.name, "outline$pick$1");
    EXPECT_EQ(sem.desc, "(I)I");

    /// 父方法体：区域成员消失，局部入参装载 + 调用点 + 转发 goto 就位。
    const ca::usize parentSize = ctx.unit->dom.size();
    EXPECT_EQ(parentSize, 9u);   /// 10 - 4（臂）+ 3（a 装载 + call + 转发 goto）
    EXPECT_TRUE(hasSmt(*ctx.fixture.session, ctx.fixture.session->unit().findMethodId("pick")));

    /// LNT 行拆分：父表剩区域外两行（pc 已重定向），N 挂迁入行。
    const auto parentRows = lntRowsOf(*ctx.fixture.session, ctx.fixture.session->unit().findMethodId("pick"));
    ASSERT_EQ(parentRows.size(), 2u);
    const auto nRows = lntRowsOf(*ctx.fixture.session, out.outlined);
    ASSERT_EQ(nRows.size(), 1u);
    EXPECT_EQ(nRows[0].second, 20u);
    EXPECT_EQ(nRows[0].first, 0u);   /// N 体首条指令
}

/// LVT 行：完全落在区域内的行迁 N 并按重编号表换槽。
TEST(MjAsmSemOutlinePlanTest, LvtRowInsideMovesWithReslot)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({pickSpec()}));
    const RegionCandidate* arm = thenArmOf(ctx);
    ASSERT_NE(arm, nullptr);

    const ca::u32 armPc = regionFirstPc(ctx, *arm);

    jbc::core::ClassFile& cf = ctx.fixture.session->backing();
    attachRows(*ctx.fixture.session, ctx.fixture.session->unit().findMethodId("pick"), {},
               {{armPc, 5u, cf.cp.addUtf8("a"), cf.cp.addUtf8("I"), 0u}});

    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, *arm));
    if (planned.is_err()) {
        ADD_FAILURE() << "DBG lvt planOutline err=" << static_cast<int>(planned.unwrap_err());
    }
    ASSERT_TRUE(planned.is_ok());
    OutlinePlan plan = std::move(planned).unwrap();
    ASSERT_EQ(plan.lvtMoves.size(), 1u);
    EXPECT_FALSE(plan.lvtMoves[0].typeTable);
    EXPECT_EQ(plan.lvtMoves[0].oldSlot, 0u);
    EXPECT_EQ(plan.lvtMoves[0].newSlot, 0u);   /// 读局部 0 → 参数槽 0
}

/// LVT 行越切点：区间起点在区域前、终点入区域 → fail-closed 拒绝。
TEST(MjAsmSemOutlinePlanTest, LvtRowCrossingCutRejected)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({pickSpec()}));
    const RegionCandidate* arm = thenArmOf(ctx);
    ASSERT_NE(arm, nullptr);

    const ca::u32 armPc = regionFirstPc(ctx, *arm);

    jbc::core::ClassFile& cf = ctx.fixture.session->backing();
    attachRows(*ctx.fixture.session, ctx.fixture.session->unit().findMethodId("pick"), {},
               {{0u, armPc + 1u, cf.cp.addUtf8("x"), cf.cp.addUtf8("I"), 1u}});

    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, *arm));
    ASSERT_TRUE(planned.is_err());
    EXPECT_EQ(planned.unwrap_err(), OutlineError::LvtRowCrossesCut);
}

/// LVT 越切点宽松档：applier 显式开关下裁剪（父表
/// 删除、记 lvtDrops），不再 fail-closed。
TEST(MjAsmSemOutlinePlanTest, LvtCrossCutLooseModeDropsRow)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({pickSpec()}));
    const RegionCandidate* arm = thenArmOf(ctx);
    ASSERT_NE(arm, nullptr);

    const ca::u32 armPc = regionFirstPc(ctx, *arm);

    jbc::core::ClassFile& cf = ctx.fixture.session->backing();
    attachRows(*ctx.fixture.session, ctx.fixture.session->unit().findMethodId("pick"), {},
               {{0u, armPc + 1u, cf.cp.addUtf8("x"), cf.cp.addUtf8("I"), 1u}});

    OutlineRequest req = requestOf(ctx, *arm);
    req.allowLvtCrossCut = true;
    auto planned = mjbase::asm_sem::planOutline(req);
    ASSERT_TRUE(planned.is_ok());
    OutlinePlan plan = std::move(planned).unwrap();
    ASSERT_EQ(plan.lvtDrops.size(), 1u);
    EXPECT_EQ(plan.lvtDrops[0].slot, 1u);
    EXPECT_TRUE(plan.lvtMoves.empty());
    ASSERT_TRUE(plan.apply(*ctx.fixture.session, ctx.resolver).is_ok());

    /// 父表：越切点行已删除（唯一 LVT 行消失）。
    const auto parentRows = lvtRowsOf(*ctx.fixture.session,
                                      ctx.fixture.session->unit().findMethodId("pick"),
                                      false);
    EXPECT_TRUE(parentRows.empty());
}

/// 判据 3：被写槽出口后仍活跃 → RegionWritesLocal；
/// 同方法的纯净臂仍可识别（对照）。
TEST(MjAsmSemOutlinePlanTest, RegionWritesLocalRejected)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({writeLocalSpec()}));
    ASSERT_EQ(ctx.candidates.size(), 2u);

    bool rejected = false;
    bool accepted = false;
    for (const RegionCandidate& c : ctx.candidates) {
        auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, c));
        if (planned.is_err()) {
            EXPECT_EQ(planned.unwrap_err(), OutlineError::RegionWritesLocal);
            rejected = true;
        } else {
            accepted = true;
        }
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(accepted);
}

/// 判据 3 的活跃性口径（LivenessView 供出口活跃集）：被写槽出口后不再
/// 被读（写回集为空）→ 接受；写槽不进参数集但入重编号表，N 体显式重槽。
TEST(MjAsmSemOutlinePlanTest, RegionWritesDeadLocalAccepted)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({deadWriteSpec()}));

    const RegionCandidate* writeArm = nullptr;
    for (const RegionCandidate& c : ctx.candidates) {
        const mjbase::asm_cfg::BasicBlock* blk = ctx.view.cfg().block(c.members[0]);
        for (const jbc::ir::InsnId id : blk->insns) {
            const jbc::ir::Insn* in = ctx.unit->dom.peek(id);
            if (in != nullptr && in->op == Op::ISTORE && in->a == 3) {
                writeArm = &c;
            }
        }
    }
    ASSERT_NE(writeArm, nullptr);

    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, *writeArm));
    ASSERT_TRUE(planned.is_ok()) << mjbase::asm_sem::to_string(planned.unwrap_err());
    OutlinePlan plan = std::move(planned).unwrap();

    /// 参数 = 读集 {a=槽 0}（t 槽写后不传）；t 为区域内自足槽排参数后。
    EXPECT_EQ(std::string_view(plan.newDesc), "(I)I");
    ASSERT_EQ(plan.params.size(), 1u);
    EXPECT_EQ(plan.params[0].kind, mjbase::asm_sem::SlotCopy::Kind::Local);
    EXPECT_EQ(plan.params[0].oldLocal, 0u);
    ASSERT_EQ(plan.locals.size(), 2u);
    EXPECT_EQ(plan.locals[0].oldSlot, 0u);
    EXPECT_EQ(plan.locals[0].newSlot, 0u);
    EXPECT_EQ(plan.locals[1].oldSlot, 3u);
    EXPECT_EQ(plan.locals[1].newSlot, 1u);
    /// 体：iload 0; istore 1; iload 1; iconst_1; iadd（t 重映射为显式槽 1）。
    ASSERT_EQ(plan.body.size(), 5u);
    EXPECT_EQ(plan.body[1].op, Op::ISTORE);
    EXPECT_EQ(plan.body[1].a, 1);
    EXPECT_EQ(plan.body[2].op, Op::ILOAD);
    EXPECT_EQ(plan.body[2].a, 1);
    EXPECT_TRUE(plan.validate().is_ok());
}

/// copy-set 最小化（def-use 恒等搬运剔除）：先写后读槽的到达定义全在区域
/// 内 → 不传参；参数集为空（描述符 ()I）。
TEST(MjAsmSemOutlinePlanTest, ReadAfterWriteTrimsParam)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({readTrimSpec()}));

    const RegionCandidate* writeArm = nullptr;
    for (const RegionCandidate& c : ctx.candidates) {
        const mjbase::asm_cfg::BasicBlock* blk = ctx.view.cfg().block(c.members[0]);
        for (const jbc::ir::InsnId id : blk->insns) {
            const jbc::ir::Insn* in = ctx.unit->dom.peek(id);
            if (in != nullptr && in->op == Op::ISTORE && in->a == 3) {
                writeArm = &c;
            }
        }
    }
    ASSERT_NE(writeArm, nullptr);

    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, *writeArm));
    ASSERT_TRUE(planned.is_ok()) << mjbase::asm_sem::to_string(planned.unwrap_err());
    OutlinePlan plan = std::move(planned).unwrap();

    EXPECT_EQ(std::string_view(plan.newDesc), "()I");   /// 无任何入参
    EXPECT_TRUE(plan.params.empty());
    ASSERT_EQ(plan.locals.size(), 1u);
    EXPECT_EQ(plan.locals[0].oldSlot, 3u);
    EXPECT_EQ(plan.locals[0].newSlot, 0u);
    /// 体：bipush 7; istore 0; iload 0; iconst_1; iadd。
    ASSERT_EQ(plan.body.size(), 5u);
    EXPECT_EQ(plan.body[1].op, Op::ISTORE);
    EXPECT_EQ(plan.body[1].a, 0);
    EXPECT_TRUE(plan.validate().is_ok());
}

/// 边界 DUP（outline_plan.hpp 头注的放开一档）：出口栈顶 = 区域末条 DUP
/// 对入口栈顶原值的复制 → 原值升格 Stack 入参 + retIsEntryCopy（调用点 DUP 复位）。
TEST(MjAsmSemOutlinePlanTest, BoundaryDupEntryCopyAccepted)
{
    ClassBytesBuilder b("T");
    MethodSpec        m = boundaryDupSpec();
    m.name = "pick";
    PlanContext ctx(b.assemble({m}));
    ASSERT_EQ(ctx.candidates.size(), 1u);

    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, ctx.candidates[0]));
    ASSERT_TRUE(planned.is_ok());
    OutlinePlan plan = std::move(planned).unwrap();

    EXPECT_TRUE(plan.retIsEntryCopy);
    EXPECT_EQ(std::string_view(plan.newDesc), "(I)I");   /// 原值升格入参 + int 返回
    ASSERT_EQ(plan.params.size(), 1u);
    EXPECT_EQ(plan.params[0].kind, mjbase::asm_sem::SlotCopy::Kind::Stack);
    EXPECT_EQ(plan.params[0].fromTop, 0u);
    EXPECT_EQ(std::string_view(plan.params[0].desc), "I");
    /// N 体：栈参前导装载（描述符槽位落局部，体从栈消费需先装载）+ DUP 产出。
    ASSERT_EQ(plan.body.size(), 2u);
    EXPECT_EQ(plan.body[0].op, Op::ILOAD);
    EXPECT_EQ(plan.body[0].a, 0u);
    EXPECT_EQ(plan.body[1].op, Op::DUP);
    EXPECT_TRUE(plan.validate().is_ok());

    auto applied = plan.apply(*ctx.fixture.session, ctx.resolver);
    ASSERT_TRUE(applied.is_ok()) << mjbase::asm_sem::to_string(applied.unwrap_err());
    const mjbase::asm_sem::OutlineOutcome out = std::move(applied).unwrap();

    /// 调用点序：DUP（复位）→ invokestatic → 转发 goto。
    const std::vector<jbc::ir::InsnId>& order = ctx.unit->dom.order();
    ca::usize callIdx = order.size();
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (order[i].slot == out.callInsn.slot) {
            callIdx = i;
        }
    }
    ASSERT_LT(callIdx, order.size());
    ASSERT_GE(callIdx, 1u);
    const jbc::ir::Insn* dupAtCall = ctx.unit->dom.peek(order[callIdx - 1]);
    const jbc::ir::Insn* callInsn  = ctx.unit->dom.peek(order[callIdx]);
    ASSERT_NE(dupAtCall, nullptr);
    ASSERT_NE(callInsn, nullptr);
    EXPECT_EQ(dupAtCall->op, Op::DUP);
    EXPECT_EQ(callInsn->op, Op::INVOKESTATIC);
}

/// 结构性拒绝：dup_x1 + pop ≡ SWAP 跨切点，出口 [b, a] 与
/// 入口 [a, b] 前缀失配 → ExitShapeImpure（调用表达不了换位）。
TEST(MjAsmSemOutlinePlanTest, DupX1SwapCrossingRejected)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({swapCrossSpec()}));

    const RegionCandidate* swapArm = nullptr;
    for (const RegionCandidate& c : ctx.candidates) {
        const mjbase::asm_cfg::BasicBlock* blk = ctx.view.cfg().block(c.members[0]);
        for (const jbc::ir::InsnId id : blk->insns) {
            const jbc::ir::Insn* in = ctx.unit->dom.peek(id);
            if (in != nullptr && in->op == Op::DUP_X1) {
                swapArm = &c;
            }
        }
    }
    ASSERT_NE(swapArm, nullptr);

    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, *swapArm));
    ASSERT_TRUE(planned.is_err());
    EXPECT_EQ(planned.unwrap_err(), OutlineError::ExitShapeImpure);
}

/// v1 出口形态：fallthrough 出口（末条非 goto）→ LastInsnNotGoto。
TEST(MjAsmSemOutlinePlanTest, FallthroughExitRejected)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({pickSpec()}));
    ASSERT_EQ(ctx.candidates.size(), 2u);

    bool rejected = false;
    for (const RegionCandidate& c : ctx.candidates) {
        auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, c));
        if (planned.is_err()) {
            EXPECT_EQ(planned.unwrap_err(), OutlineError::LastInsnNotGoto);
            rejected = true;
        }
    }
    EXPECT_TRUE(rejected);   /// else 臂（iconst_5 fallthrough 出口）命中
}

/// 判据 1：非直线候选（循环头 + 体）→ NotStraightLine。
TEST(MjAsmSemOutlinePlanTest, NonStraightLoopRejected)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "pick";
    m.desc = "(I)V";
    m.emit = [](CodeEmit& e) {
        const std::size_t head = e.newLabel();
        const std::size_t exit = e.newLabel();
        e.op(Op::ICONST_0);
        e.varInsn(Op::ISTORE, 0);
        e.bindLabel(head);
        e.varInsn(Op::ILOAD, 0);
        e.branch(Op::IFEQ, exit);
        e.varInsn(Op::ILOAD, 0);
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.varInsn(Op::ISTORE, 0);
        e.branch(Op::GOTO, head);
        e.bindLabel(exit);
        e.op(Op::RETURN);
    };
    PlanContext ctx(b.assemble({m}));
    bool found = false;
    for (const RegionCandidate& c : ctx.candidates) {
        if (c.members.size() == 2u) {
            auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, c));
            ASSERT_TRUE(planned.is_err());
            EXPECT_EQ(planned.unwrap_err(), OutlineError::NotStraightLine);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

/// 判据 4：try 覆盖内同质区域（handler 集非空）→ TryCovered。
TEST(MjAsmSemOutlinePlanTest, TryCoveredRejected)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name = "pick";
    m.desc = "(Z)V";
    m.emit = [](CodeEmit& e) {
        const std::size_t tryStart = e.newLabel();   /// 0
        const std::size_t tryEnd   = e.newLabel();   /// 1
        const std::size_t handler  = e.newLabel();   /// 2
        const std::size_t join     = e.newLabel();   /// 3
        const std::size_t end      = e.newLabel();   /// 4
        e.op(Op::ICONST_0);              /// A
        e.varInsn(Op::ISTORE, 0);
        e.bindLabel(tryStart);
        e.op(Op::ICONST_1);              /// B：try [B .. join) 内
        e.varInsn(Op::ISTORE, 1);
        e.branch(Op::GOTO, end);
        e.bindLabel(tryEnd);
        e.bindLabel(join);
        e.op(Op::ICONST_2);              /// C
        e.varInsn(Op::ISTORE, 2);
        e.bindLabel(end);
        e.op(Op::RETURN);                /// D
        e.bindLabel(handler);
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    m.exceptions = {mjbase::analysis::test_support::ExcSpec{0, 1, 2, "java/lang/Exception"}};
    PlanContext ctx(b.assemble({m}));
    bool found = false;
    for (const RegionCandidate& c : ctx.candidates) {
        if (c.members.size() == 1u && c.members[0].index == 1u) {
            ASSERT_FALSE(c.handlers.empty());
            auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, c));
            ASSERT_TRUE(planned.is_err());
            EXPECT_EQ(planned.unwrap_err(), OutlineError::TryCovered);
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

/// cat2 切面：long 局部入参占 2 槽，描述符 (J)J，N 体显式 lload/lreturn。
TEST(MjAsmSemOutlinePlanTest, Cat2LongRegionPlan)
{
    ClassBytesBuilder b("T");
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(JJ)J";
    m.maxStack  = 4;
    m.maxLocals = 4;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();
        const std::size_t join    = e.newLabel();
        e.varInsn(Op::LLOAD, 0);
        e.op(Op::LCONST_0);
        e.op(Op::LCMP);
        e.branch(Op::IFEQ, elseArm);
        e.varInsn(Op::LLOAD, 0);        /// B：出口 [long]
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.varInsn(Op::LLOAD, 0);        /// C：出口 [long]
        e.bindLabel(join);
        e.op(Op::LRETURN);
    };
    PlanContext ctx(b.assemble({m}));
    ASSERT_EQ(ctx.candidates.size(), 2u);

    const RegionCandidate* gotoArm = nullptr;
    for (const RegionCandidate& c : ctx.candidates) {
        const mjbase::asm_cfg::BasicBlock* blk = ctx.view.cfg().block(c.members[0]);
        const jbc::ir::Insn* last = ctx.unit->dom.peek(blk->insns.back());
        if (last != nullptr && last->op == Op::GOTO) {
            gotoArm = &c;
        }
    }
    ASSERT_NE(gotoArm, nullptr);
    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, *gotoArm));
    if (planned.is_err()) {
        ADD_FAILURE() << "DBG cat2 planOutline err=" << static_cast<int>(planned.unwrap_err());
    }
    ASSERT_TRUE(planned.is_ok());
    OutlinePlan plan = std::move(planned).unwrap();
    EXPECT_EQ(std::string_view(plan.newDesc), "(J)J");
    ASSERT_EQ(plan.params.size(), 1u);
    EXPECT_EQ(std::string_view(plan.params[0].desc), "J");
    EXPECT_EQ(plan.params[0].newSlot, 0u);
    ASSERT_EQ(plan.body.size(), 1u);
    EXPECT_EQ(plan.body[0].op, Op::LLOAD);
    EXPECT_EQ(plan.body[0].a, 0);
    EXPECT_EQ(std::string_view(plan.ret.desc), "J");
}

/// validate 数据侧：体被清空 / hasRet 撤销 → MalformedPlan。
TEST(MjAsmSemOutlinePlanTest, ValidateMalformedPlan)
{
    ClassBytesBuilder b("T");
    PlanContext       ctx(b.assemble({pickSpec()}));
    const RegionCandidate* arm = thenArmOf(ctx);
    ASSERT_NE(arm, nullptr);
    auto planned = mjbase::asm_sem::planOutline(requestOf(ctx, *arm));
    ASSERT_TRUE(planned.is_ok());
    OutlinePlan plan = std::move(planned).unwrap();
    EXPECT_TRUE(plan.validate().is_ok());

    OutlinePlan brokenNoBody;
    brokenNoBody.newName = plan.arena.intern("n");
    brokenNoBody.newDesc = plan.arena.intern("(I)I");
    brokenNoBody.hasRet  = true;
    brokenNoBody.ret.desc = plan.arena.intern("I");
    EXPECT_EQ(brokenNoBody.validate().unwrap_err(), OutlineError::MalformedPlan);

    OutlinePlan brokenNoRet;
    brokenNoRet.newName = plan.arena.intern("n");
    brokenNoRet.newDesc = plan.arena.intern("(I)V");
    brokenNoRet.hasRet  = false;
    brokenNoRet.ret.desc = plan.arena.intern("V");
    EXPECT_EQ(brokenNoRet.validate().unwrap_err(), OutlineError::MalformedPlan);
}

/// JVM 真跑门：javac 编译样本 → outline → 写回 class → java 运行断言行为
/// 与 outline 前逐字一致（-Xverify:all 全程验证；JDK 缺失自动 SKIP）。
TEST(MjAsmSemOutlinePlanTest, JvmGateBehaviorPreserved)
{
    namespace fs = std::filesystem;
    static const auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path scratch =
        fs::temp_directory_path()
        / ("mjbase_outline_gate_" + std::to_string(
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

    /// 原始字节 → 行为基线。
    Bytes original;
    {
        std::ifstream in(scratch / "OutlineSample.class", std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    ASSERT_GT(original.size(), 0u);

    /// outline：then 臂（首候选）。
    auto made = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView{original.data(), original.size()});
    ASSERT_TRUE(made.is_ok());
    auto session = std::move(made).unwrap();
    auto visits  = jbc::ir::collectCodeUnits(*session);
    jbc::ir::CodeUnit* pick = nullptr;
    jbc::ir::MethodId  pickId{};
    for (const jbc::ir::CodeVisit& v : visits) {
        if (v.kind != jbc::ir::CodeVisit::Kind::Ok) {
            continue;
        }
        const jbc::ir::MethodUnit& mu = session->unit().methods[v.methodIdx];
        if (session->unit().symbols().str(mu.name) == "pick") {
            pick   = v.unit.get();
            pickId = mu.id;
        }
    }
    ASSERT_NE(pick, nullptr);

    auto builtView = mjbase::asm_sem::RegionView::build(*pick);
    ASSERT_TRUE(builtView.is_ok());
    mjbase::asm_sem::RegionView view = std::move(builtView).unwrap();
    const std::vector<mjbase::asm_sem::RegionCandidate> candidates = view.candidates();
    ASSERT_FALSE(candidates.empty());

    jbc::analysis::ShapeResolver resolver;
    /// 枚举取首个可识别候选（javac 的 else 臂为 fallthrough 出口，v1 拒绝）。
    OutlinePlan plan;
    bool plannedAny = false;
    mjbase::asm_sem::OutlineError lastErr = mjbase::asm_sem::OutlineError::Ok;
    const jbc::analysis::MethodSig gateSig = sigOf(true, "(IIIZ)I", "pick", "OutlineSample");
    for (const mjbase::asm_sem::RegionCandidate& c : candidates) {
        auto planned = mjbase::asm_sem::planOutline(OutlineRequest{
            *session, pickId, gateSig, resolver, view, c, 1});
        if (planned.is_ok()) {
            plan = std::move(planned).unwrap();
            plannedAny = true;
            break;
        }
        lastErr = planned.unwrap_err();
    }
    ASSERT_TRUE(plannedAny) << mjbase::asm_sem::to_string(lastErr);
    auto applied = plan.apply(*session, resolver);
    ASSERT_TRUE(applied.is_ok()) << mjbase::asm_sem::to_string(applied.unwrap_err());

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    const Bytes outlined = std::move(written).unwrap();

    const std::string runnerBody =
        "System.out.println(OutlineSample.pick(3, 4, true));"
        "System.out.println(OutlineSample.pick(3, 4, false));";

    test::jdk::RunnerSpec before;
    before.classes     = {{"OutlineSample", original}};
    before.runnerBody  = runnerBody;
    std::string beforeOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, before, &beforeOut));

    test::jdk::RunnerSpec after;
    after.classes     = {{"OutlineSample", outlined}};
    after.runnerBody  = runnerBody;
    std::string afterOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, after, &afterOut));

    EXPECT_EQ(afterOut, "106\n107\n");
    EXPECT_EQ(afterOut, beforeOut);
    fs::remove_all(scratch, ec);
}

/// builder 语料补 SMT 的对照组助手定义在 sem_fixture.hpp，供多个
/// JVM 真跑门用例共用。

/// JVM 真跑门公共体：plan（谓词过滤后首个可识别候选）→ apply → 写回 →
/// java 前后对拍。期望输出由用例断言；planOutline 全拒时 ADD_FAILURE。
/// 谓词入参 (候选, RegionView)——同一样本多候选时可按块成员数区分。
template <typename Pred>
void runJvmGateWhere(const MethodSpec& spec, const jbc::analysis::MethodSig& gateSig,
                     const std::string& runnerBody, const std::string& expectOut,
                     Pred pred)
{
    static const auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    ClassBytesBuilder b("T");
    Bytes             raw      = b.assemble({spec});
    Bytes             original = withStackMaps(raw);   /// 对照组补 SMT（builder 语料不带）

    auto made = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView{raw.data(), raw.size()});
    ASSERT_TRUE(made.is_ok());
    auto session = std::move(made).unwrap();
    auto visits  = jbc::ir::collectCodeUnits(*session);
    jbc::ir::CodeUnit* pick = nullptr;
    jbc::ir::MethodId  pickId{};
    for (const jbc::ir::CodeVisit& v : visits) {
        if (v.kind != jbc::ir::CodeVisit::Kind::Ok) {
            continue;
        }
        const jbc::ir::MethodUnit& mu = session->unit().methods[v.methodIdx];
        if (session->unit().symbols().str(mu.name) == "pick") {
            pick   = v.unit.get();
            pickId = mu.id;
        }
    }
    ASSERT_NE(pick, nullptr);

    auto builtView = mjbase::asm_sem::RegionView::build(*pick);
    ASSERT_TRUE(builtView.is_ok());
    mjbase::asm_sem::RegionView view = std::move(builtView).unwrap();
    const std::vector<mjbase::asm_sem::RegionCandidate> candidates = view.candidates();

    jbc::analysis::ShapeResolver resolver;
    OutlinePlan plan;
    bool        plannedAny = false;
    mjbase::asm_sem::OutlineError lastErr = mjbase::asm_sem::OutlineError::Ok;
    for (ca::usize ci = 0; ci < candidates.size(); ++ci) {
        if (!pred(candidates[ci], view)) {
            continue;
        }
        auto planned = mjbase::asm_sem::planOutline(
            OutlineRequest{*session, pickId, gateSig, resolver, view, candidates[ci], 1});
        if (planned.is_ok()) {
            plan = std::move(planned).unwrap();
            plannedAny = true;
            break;
        }
        lastErr = planned.unwrap_err();
        ADD_FAILURE() << "cand[" << ci << "] " << mjbase::asm_sem::to_string(lastErr);
    }
    ASSERT_TRUE(plannedAny) << mjbase::asm_sem::to_string(lastErr);
    auto applied = plan.apply(*session, resolver);
    ASSERT_TRUE(applied.is_ok()) << mjbase::asm_sem::to_string(applied.unwrap_err());

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    const Bytes outlined = std::move(written).unwrap();

    test::jdk::RunnerSpec before;
    before.classes    = {{"T", original}};
    before.runnerBody = runnerBody;
    std::string beforeOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, before, &beforeOut));

    test::jdk::RunnerSpec after;
    after.classes    = {{"T", outlined}};
    after.runnerBody = runnerBody;
    std::string afterOut;
    ASSERT_TRUE(test::jdk::runRunnerChecked(tc, after, &afterOut));

    EXPECT_EQ(afterOut, expectOut);
    EXPECT_EQ(afterOut, beforeOut);   /// -Xverify:all 全程验证 + 行为逐字一致
}

/// JVM 真跑门公共体（首个可识别候选版）。
void runJvmGate(const MethodSpec& spec, const jbc::analysis::MethodSig& gateSig,
                const std::string& runnerBody, const std::string& expectOut)
{
    runJvmGateWhere(spec, gateSig, runnerBody, expectOut,
                    [](const mjbase::asm_sem::RegionCandidate&,
                       const mjbase::asm_sem::RegionView&) { return true; });
}

/// JVM 真跑门：goto 入边 + 局部参数——参数装载块标签锚定首条装载、随入边
/// 走，分支不绕过装载，真跑行为与拆分前逐字一致。
TEST(MjAsmSemOutlinePlanTest, JvmGateGotoEntryLocalParams)
{
    const MethodSpec spec = gotoEntryLocalParamSpec();
    runJvmGate(spec, sigOf(true, "(III)I", "pick", "T"),
               "System.out.println(T.pick(3, 4, 0));"
               "System.out.println(T.pick(3, 4, 1));",
               "7\n7\n");
}

/// JVM 真跑门（调用点侧栈形重排那一档）：边界 DUP 出口切点——原值升格入参 + 调用点
/// DUP 复位，真跑行为与拆分前逐字一致。
TEST(MjAsmSemOutlinePlanTest, JvmGateBoundaryDupEntryCopy)
{
    MethodSpec spec = boundaryDupSpec();
    spec.name = "pick";
    runJvmGate(spec, sigOf(true, "(I)I", "pick", "T"),
               "System.out.println(T.pick(0));"    /// ifeq taken 臂（不经区域）
               "System.out.println(T.pick(3));"    /// 直落臂（经区域 DUP 出口）
               "System.out.println(T.pick(9));",
               "0\n3\n9\n");
}

/// JVM 真跑门：自足槽 N 携 Code 嵌套盖章属性（kOutlineStampAttr
/// 未建模嵌套属性）真跑——JVMS 4.7.1 忽略面 + HotSpot 实证，-Xverify:all
/// 全程验证，行为与拆分前逐字一致。谓词钉住写臂（6 成员，含 ISTORE 3）。
TEST(MjAsmSemOutlinePlanTest, JvmGateSelfSufficientStamp)
{
    MethodSpec spec;
    spec.name      = "pick";
    spec.desc      = "(IZ)I";
    spec.maxStack  = 3;
    spec.maxLocals = 4;
    spec.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 1);
        e.branch(Op::IFEQ, elseArm);
        e.op(Op::BIPUSH);
        e.u8(7);
        e.varInsn(Op::ISTORE, 3);       /// t = 7（区域内定义，自足槽）
        e.varInsn(Op::ILOAD, 3);
        e.op(Op::ICONST_1);
        e.op(Op::IADD);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);
        e.branch(Op::GOTO, join);
        e.bindLabel(join);
        e.op(Op::ICONST_0);
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    runJvmGateWhere(spec, sigOf(true, "(IZ)I", "pick", "T"),
                    "System.out.println(T.pick(5, true));"
                    "System.out.println(T.pick(5, false));",
                    "8\n5\n",
                    [](const mjbase::asm_sem::RegionCandidate& c,
                       const mjbase::asm_sem::RegionView& v) {
                        if (c.members.size() != 1u) {
                            return false;
                        }
                        const mjbase::asm_cfg::BasicBlock* blk = v.cfg().block(c.members[0]);
                        return blk != nullptr && blk->insns.size() == 6u;   /// 写臂（含自足槽写读）
                    });
}
