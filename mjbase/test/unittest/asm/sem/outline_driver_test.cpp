/// @file        outline_driver_test.cpp
/// @brief       OutlineDriver 测试：驱动闭环逐面钉死——无候选收敛 / 单候选
///              切层 / 嵌套梯内层优先多轮 / 全拒（方法级 jsr、判据 6 面）/
///              迭代上限命中 / JVM 真跑端到端门（javac 语料多方法连续
///              outline 后 -Xverify:all 行为对拍；JDK 缺失 SKIP）。
///
/// 驱动器语义注（v1 策略面的接力候选形态，见 outline_driver.hpp 文件头注）：
/// apply 后调用点块（装载 + invokestatic + 转发 goto）自身仍满足预检与出口
/// 形，干净臂语料上每轮都能再切一层（接力）——上限是唯一护栏。故可开档
/// 语料的驱动要么命中上限、要么候选从始耗尽；多轮用例以「每轮产物独立合
/// 法 + 行为保持」验收，收敛用例以零候选语料钉死。
#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/sem/outline_driver.hpp"
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
using mjbase::asm_sem::driveOutlines;
using mjbase::asm_sem::OutlineDriveNote;
using mjbase::asm_sem::OutlineDriveReport;
using mjbase::asm_sem::OutlineDriveRequest;
using mjbase::asm_sem::OutlineError;
using mjbase::asm_sem::RegionSelectNote;

/// 嵌套候选梯样本（同 region_select_test ladderSpec）：A（iload c; ifeq
/// else）→ B1（iload a; goto B2）→ B2（iconst 2; goto B3）→ B3（imul;
/// goto join）为嵌套候选梯 {B1,B2,B3} ⊃ {B2,B3} ⊃ {B3}；else 臂 C
/// fallthrough 出口（末条非 goto，F4 形态）。
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
        e.branch(Op::IFEQ, elseArm);    /// A
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

/// 单候选样本：then 臂 B 单块（iload a; iconst 2; imul; goto join）唯一
/// 可预检候选；else 臂 C fallthrough（F4 形态）。
MethodSpec singleRegionSpec()
{
    MethodSpec m;
    m.name      = "pick";
    m.desc      = "(IIIZ)I";
    m.maxStack  = 3;
    m.maxLocals = 4;
    m.emit      = [](CodeEmit& e) {
        const std::size_t elseArm = e.newLabel();   /// 0
        const std::size_t join    = e.newLabel();   /// 1
        e.varInsn(Op::ILOAD, 2);
        e.branch(Op::IFEQ, elseArm);    /// A
        e.varInsn(Op::ILOAD, 0);        /// B（单块 then 臂）
        e.op(Op::ICONST_2);
        e.op(Op::IMUL);
        e.branch(Op::GOTO, join);
        e.bindLabel(elseArm);
        e.op(Op::ICONST_5);             /// C（fallthrough → join）
        e.bindLabel(join);
        e.op(Op::ICONST_0);
        e.op(Op::IADD);
        e.op(Op::IRETURN);
    };
    return m;
}

/// 零候选样本：直线方法（单块以 return 收尾——无出口候选不交付）。
MethodSpec straightSpec()
{
    MethodSpec m;
    m.name      = "pick";
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

/// 构造器样本（判据 6 F5 形态）：super() 前的 then 臂 B 为候选（UninitThis
/// 存活 → 全滤）。
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
        e.branch(Op::IFEQ, skip);       /// A
        e.varInsn(Op::ALOAD, 0);        /// B：super() 前段，UninitThis 存活
        e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
        e.branch(Op::GOTO, join);
        e.bindLabel(skip);
        e.varInsn(Op::ALOAD, 0);        /// C（fallthrough → join）
        e.invoke(Op::INVOKESPECIAL, "java/lang/Object", "<init>", "()V");
        e.bindLabel(join);
        e.op(Op::RETURN);
    };
    return m;
}

/// jsr 样本（判据 5 方法级事实）：策略方法级全拒 → SelectRejected。
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
        e.varInsn(Op::ASTORE, 0);
        e.varInsn(Op::RET, 0);
    };
    return m;
}

}   // namespace

/// 零候选语料：首轮重建即候选耗尽（CandidatesExhausted），零轮 apply。
TEST(MjAsmSemOutlineDriverTest, StraightMethodCandidatesExhausted)
{
    ClassBytesBuilder b("T");
    SemFixture        fixture{b.assemble({straightSpec()})};
    CodeUnit*         unit = fixture.code("pick");
    ASSERT_NE(unit, nullptr);
    const jbc::ir::MethodId pickId =
        fixture.session->unit().methods[fixture.visits[0].methodIdx].id;

    jbc::analysis::ShapeResolver resolver;
    OutlineDriveRequest req{*fixture.session, pickId,
                            sigOf(true, "(I)I", "pick", "T"), resolver};
    const OutlineDriveReport report = driveOutlines(req);
    EXPECT_EQ(report.note, OutlineDriveNote::CandidatesExhausted);
    EXPECT_EQ(report.appliedCount(), 0u);
    EXPECT_EQ(report.stopSelectNote, RegionSelectNote::NoCandidates);
    EXPECT_EQ(report.stopOutlineError, OutlineError::Ok);
}

/// 单候选语料 + 上限 1：恰切一轮（内层唯一候选），第二轮被上限拦截
/// （IterationCap）；轮记录携带策略便签与命名序号。
TEST(MjAsmSemOutlineDriverTest, SingleRegionCutThenCap)
{
    ClassBytesBuilder b("T");
    SemFixture        fixture{b.assemble({singleRegionSpec()})};
    CodeUnit*         unit = fixture.code("pick");
    ASSERT_NE(unit, nullptr);
    const jbc::ir::MethodId pickId =
        fixture.session->unit().methods[fixture.visits[0].methodIdx].id;

    jbc::analysis::ShapeResolver resolver;
    OutlineDriveRequest req{*fixture.session, pickId,
                            sigOf(true, "(IIIZ)I", "pick", "T"), resolver};
    req.maxRounds = 1;
    const OutlineDriveReport report = driveOutlines(req);
    ASSERT_EQ(report.appliedCount(), 1u);
    EXPECT_EQ(report.note, OutlineDriveNote::IterationCap);
    EXPECT_EQ(report.rounds[0].sequence, 1u);
    EXPECT_EQ(report.rounds[0].candidateCount, 2u);   /// then 臂 + else 臂（F4 形态）
    EXPECT_EQ(report.rounds[0].chosenMemberCount, 1u);
    EXPECT_EQ(report.rounds[0].selectNote, RegionSelectNote::Ok);
    EXPECT_EQ(report.stopOutlineError, OutlineError::Ok);
}

/// 嵌套候选梯 + 上限 3：首轮内层优先（chosenMemberCount = 1，梯的最内层
/// {B3}），逐轮接力切层至上限；轮序号递增、轮轮独立合法。
TEST(MjAsmSemOutlineDriverTest, NestedLadderInnermostFirstMultiRound)
{
    ClassBytesBuilder b("T");
    SemFixture        fixture{b.assemble({ladderSpec()})};
    CodeUnit*         unit = fixture.code("pick");
    ASSERT_NE(unit, nullptr);
    const jbc::ir::MethodId pickId =
        fixture.session->unit().methods[fixture.visits[0].methodIdx].id;

    jbc::analysis::ShapeResolver resolver;
    OutlineDriveRequest req{*fixture.session, pickId,
                            sigOf(true, "(IIIZ)I", "pick", "T"), resolver};
    req.maxRounds = 3;
    const OutlineDriveReport report = driveOutlines(req);
    ASSERT_EQ(report.appliedCount(), 3u);
    EXPECT_EQ(report.note, OutlineDriveNote::IterationCap);
    EXPECT_EQ(report.rounds[0].candidateCount, 4u);   /// 梯三档 + else 臂
    EXPECT_EQ(report.rounds[0].chosenMemberCount, 1u);   /// 内层优先（{B3}）
    for (ca::usize i = 0; i < report.rounds.size(); ++i) {
        EXPECT_EQ(report.rounds[i].sequence, i + 1u);
        EXPECT_EQ(report.rounds[i].selectNote, RegionSelectNote::Ok);
        EXPECT_NE(report.rounds[i].outcome.outlined, jbc::ir::kNoMethodId)
            << "round " << i;   /// 产物面（合成方法身份）
        EXPECT_NE(report.rounds[i].outcome.callInsn, jbc::ir::kNoInsn)
            << "round " << i;   /// 产物面（调用点句柄）
    }
}

/// 上限 0：零轮执行，直接命中上限（显式参数语义——什么都不切）。
TEST(MjAsmSemOutlineDriverTest, ZeroCapAppliesNothing)
{
    ClassBytesBuilder b("T");
    SemFixture        fixture{b.assemble({ladderSpec()})};
    CodeUnit*         unit = fixture.code("pick");
    ASSERT_NE(unit, nullptr);
    const jbc::ir::MethodId pickId =
        fixture.session->unit().methods[fixture.visits[0].methodIdx].id;

    jbc::analysis::ShapeResolver resolver;
    OutlineDriveRequest req{*fixture.session, pickId,
                            sigOf(true, "(IIIZ)I", "pick", "T"), resolver};
    req.maxRounds = 0;
    const OutlineDriveReport report = driveOutlines(req);
    EXPECT_EQ(report.appliedCount(), 0u);
    EXPECT_EQ(report.note, OutlineDriveNote::IterationCap);
}

/// 全拒面一：判据 6（构造器 UninitThis 存活，候选全滤）→ SelectRejected +
/// stopSelectNote = AllFiltered，零轮 apply（fail-closed：策略全拒不落地）。
TEST(MjAsmSemOutlineDriverTest, CtorUninitThisAllRejected)
{
    ClassBytesBuilder b("T");
    SemFixture        fixture{b.assemble({ctorSpec()})};
    CodeUnit*         unit = nullptr;
    jbc::ir::MethodId ctorId{};
    for (const jbc::ir::CodeVisit& v : fixture.visits) {
        if (v.kind != jbc::ir::CodeVisit::Kind::Ok) {
            continue;
        }
        const jbc::ir::MethodUnit& mu = fixture.session->unit().methods[v.methodIdx];
        if (fixture.session->unit().symbols().str(mu.name) == "<init>") {
            unit   = v.unit.get();
            ctorId = mu.id;
        }
    }
    ASSERT_NE(unit, nullptr);

    jbc::analysis::ShapeResolver resolver;
    OutlineDriveRequest req{*fixture.session, ctorId,
                            sigOf(false, "(I)V", "<init>", "T"), resolver};
    const OutlineDriveReport report = driveOutlines(req);
    EXPECT_EQ(report.appliedCount(), 0u);
    EXPECT_EQ(report.note, OutlineDriveNote::SelectRejected);
    EXPECT_EQ(report.stopSelectNote, RegionSelectNote::AllFiltered);
}

/// 全拒面二：方法级 jsr 事实（判据 5）→ SelectRejected + MethodSubroutine。
TEST(MjAsmSemOutlineDriverTest, JsrMethodAllRejected)
{
    ClassBytesBuilder b("T");
    SemFixture        fixture{b.assemble({jsrSpec()})};
    CodeUnit*         unit = fixture.code("pick");
    ASSERT_NE(unit, nullptr);
    const jbc::ir::MethodId pickId =
        fixture.session->unit().methods[fixture.visits[0].methodIdx].id;

    jbc::analysis::ShapeResolver resolver;
    OutlineDriveRequest req{*fixture.session, pickId,
                            sigOf(true, "()I", "pick", "T"), resolver};
    const OutlineDriveReport report = driveOutlines(req);
    EXPECT_EQ(report.appliedCount(), 0u);
    EXPECT_EQ(report.note, OutlineDriveNote::SelectRejected);
    EXPECT_EQ(report.stopSelectNote, RegionSelectNote::MethodSubroutine);
}

/// JVM 真跑端到端门：javac 语料 → 驱动多方法连续 outline（接力切层至上限，
/// 每轮产物独立合法）→ 写回 class → java 运行行为与拆分前逐字一致
/// （-Xverify:all 全程验证合成方法链；JDK 缺失自动 SKIP）。
TEST(MjAsmSemOutlineDriverTest, JvmGateDrivenMultiOutlineBehaviorPreserved)
{
    namespace fs = std::filesystem;
    static const auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path scratch =
        fs::temp_directory_path()
        / ("mjbase_outline_driver_gate_" + std::to_string(
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

    jbc::analysis::ShapeResolver resolver;
    OutlineDriveRequest req{*session, pickId,
                            sigOf(true, "(IIIZ)I", "pick", "OutlineSample"),
                            resolver};
    req.maxRounds = 4;   /// 保守上限：接力切层 4 轮（每轮产物独立合法）
    const OutlineDriveReport report = driveOutlines(req);
    ASSERT_EQ(report.appliedCount(), 4u)
        << mjbase::asm_sem::to_string(report.note);
    EXPECT_EQ(report.note, OutlineDriveNote::IterationCap);
    for (ca::usize i = 0; i < report.rounds.size(); ++i) {
        EXPECT_EQ(report.rounds[i].sequence, i + 1u);
    }

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
    EXPECT_EQ(afterOut, beforeOut);   /// -Xverify:all 全程验证 + 行为逐字一致
    fs::remove_all(scratch, ec);
}
