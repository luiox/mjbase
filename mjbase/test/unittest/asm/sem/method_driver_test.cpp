/// @file        method_driver_test.cpp
/// @brief       MethodDriver 测试：方法级编排逐面钉死——多方法确定性名序
///              聚合 / 零候选快跳（直线 + jsr 枚举零候选）/ 单方法失败隔离
///              （无体方法 ParentUnusable 不阻断）/ 全拒隔离（构造器判据 6
///              全滤不阻断）/ 空池全量枚举 / drive 档位透传（接力档 +
///              排除接力档）/ 挑选策略重载破平 / JVM 真跑端到端门（javac
///              语料 ≥2 方法编排驱动后 -Xverify:all 行为逐字对拍；JDK 缺
///              失 SKIP）。
///
/// 语料纪律同 outline_driver_test：干净臂单区域方法（排除档下每方法恰切
/// 一层收敛）；驱动器语义注（接力候选形态 / 排除档跳过接力）见
/// outline_driver_test 文件头注，本文件不重复。会话纪律：方法升格前置
/// （collectCodeUnits），空池缺省枚举已升格方法——同 SemFixture/既有门口径。
#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"

#include "mjbase/asm/sem/method_driver.hpp"
#include "mjbase/asm/sem/region_view.hpp"

#include "sem_fixture.hpp"

#include "jdk_fixture_gtest.hpp"

namespace {

using jbc::core::Op;
using jbc::ir::MethodId;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::MethodSpec;
using mjbase::asm_sem::driveMethods;
using mjbase::asm_sem::MethodDriveReport;
using mjbase::asm_sem::MethodDriveRequest;
using mjbase::asm_sem::MethodSelectPolicy;
using mjbase::asm_sem::OutlineDriveNote;
using mjbase::asm_sem::RegionSelectNote;

/// 单区域分支样本（同 outline_driver_test singleRegionSpec 形，名字可换）：
/// then 臂 B 单块唯一可预检候选；else 臂 C fallthrough 出口（F4 形态）。
MethodSpec branchPickSpec(std::string_view name)
{
    MethodSpec m;
    m.name      = name;
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
MethodSpec straightSpec(std::string_view name)
{
    MethodSpec m;
    m.name      = name;
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

/// jsr 样本（判据 5 方法级事实）：RegionView 枚举零候选（子程序结构不
/// 交付 SESE 区域）→ 编排便宜预检快跳（保守结局与全拒等价：零落地）。
MethodSpec jsrSpec(std::string_view name)
{
    MethodSpec m;
    m.name      = name;
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

/// 构造器样本（判据 6 F5 形态）：候选非空但策略全滤（UninitThis 存活）
/// → 驱动全拒 SelectRejected（同 outline_driver_test ctorSpec）。
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

/// 无体样本（abstract）：emit 为空 = 无 Code 属性——ParentUnusable 隔离面。
MethodSpec abstractSpec(std::string_view name)
{
    MethodSpec m;
    m.access    = 0x0001 | 0x0400;   // public abstract
    m.name      = name;
    m.desc      = "(I)I";
    m.maxStack  = 1;
    m.maxLocals = 1;
    return m;
}

/// 按方法名取稳定身份（测试断言用）。
MethodId methodIdOf(const SemFixture& fixture, std::string_view name)
{
    return fixture.session->unit().findMethodId(name);
}

}   // namespace

/// 多方法编排：候选池乱序给出 → 确定性内部名序驱动（alpha 先于 zeta），
/// 逐方法独立收敛（排除档各切 1 层），总计聚合（应用层 2 / 跳过 0 /
/// 全拒 0 / 失败 0）。
TEST(MjAsmSemMethodDriveTest, TwoMethodsNameOrderAggregated)
{
    ClassBytesBuilder b("T");
    SemFixture fixture{b.assemble({branchPickSpec("zeta"), branchPickSpec("alpha")})};
    const MethodId zetaId = methodIdOf(fixture, "zeta");
    const MethodId alphaId = methodIdOf(fixture, "alpha");
    ASSERT_NE(zetaId, jbc::ir::kNoMethodId);
    ASSERT_NE(alphaId, jbc::ir::kNoMethodId);

    jbc::analysis::ShapeResolver resolver;
    MethodDriveRequest req{*fixture.session, resolver};
    req.methods                   = {zetaId, alphaId};   /// 乱序池——策略归一
    req.drivePolicy.excludeOutlineRelay = true;          /// 排除接力候选（每方法 1 层收敛）
    const MethodDriveReport report = driveMethods(req);

    ASSERT_EQ(report.methods.size(), 2u);
    EXPECT_EQ(report.methods[0].method, alphaId);   /// 内部名序：alpha 先
    EXPECT_EQ(report.methods[1].method, zetaId);
    for (ca::usize i = 0; i < report.methods.size(); ++i) {
        EXPECT_EQ(report.methods[i].drive.note, OutlineDriveNote::CandidatesExhausted)
            << "method " << i;
        ASSERT_EQ(report.methods[i].drive.appliedCount(), 1u) << "method " << i;
        EXPECT_EQ(report.methods[i].drive.stopSelectNote, RegionSelectNote::AllExcluded)
            << "method " << i;
        EXPECT_EQ(report.methods[i].drive.rounds[0].sequence, 1u)   /// 逐方法序号分域
            << "method " << i;
    }
    EXPECT_EQ(report.totalApplied(), 2u);
    EXPECT_EQ(report.skippedCount, 0u);
    EXPECT_EQ(report.rejectedCount, 0u);
    EXPECT_EQ(report.failedCount, 0u);
}

/// 零候选快跳：直线方法（候选数 0）在便宜预检被跳过（不跑驱动、不落
/// methods，计 skippedCount），同池可开档方法照常驱动。
TEST(MjAsmSemMethodDriveTest, ZeroCandidateFastSkip)
{
    ClassBytesBuilder b("T");
    SemFixture fixture{b.assemble({branchPickSpec("pick"), straightSpec("straight")})};
    const MethodId pickId     = methodIdOf(fixture, "pick");
    const MethodId straightId = methodIdOf(fixture, "straight");
    ASSERT_NE(pickId, jbc::ir::kNoMethodId);
    ASSERT_NE(straightId, jbc::ir::kNoMethodId);

    jbc::analysis::ShapeResolver resolver;
    MethodDriveRequest req{*fixture.session, resolver};
    req.methods                   = {straightId, pickId};
    req.drivePolicy.excludeOutlineRelay = true;
    const MethodDriveReport report = driveMethods(req);

    EXPECT_EQ(report.skippedCount, 1u);
    ASSERT_EQ(report.methods.size(), 1u);   /// 跳过方法不落 methods
    EXPECT_EQ(report.methods[0].method, pickId);
    EXPECT_EQ(report.methods[0].drive.appliedCount(), 1u);
    EXPECT_EQ(report.totalApplied(), 1u);
    EXPECT_EQ(report.rejectedCount, 0u);
    EXPECT_EQ(report.failedCount, 0u);
}

/// 失败隔离：无体方法（abstract，请求面显式点名）驱动定性 ParentUnusable
/// 计入 failedCount，不阻断同池其他方法（pick 照常切 1 层）。
TEST(MjAsmSemMethodDriveTest, FailureIsolatedParentUnusable)
{
    ClassBytesBuilder b("T");
    SemFixture fixture{b.assemble({abstractSpec("stub"), branchPickSpec("pick")})};
    const MethodId stubId = methodIdOf(fixture, "stub");
    const MethodId pickId = methodIdOf(fixture, "pick");
    ASSERT_NE(stubId, jbc::ir::kNoMethodId);
    ASSERT_NE(pickId, jbc::ir::kNoMethodId);

    jbc::analysis::ShapeResolver resolver;
    MethodDriveRequest req{*fixture.session, resolver};
    req.methods                   = {stubId, pickId};
    req.drivePolicy.excludeOutlineRelay = true;
    const MethodDriveReport report = driveMethods(req);

    ASSERT_EQ(report.methods.size(), 2u);   /// 两方法都有驱动记录（不静默跳）
    EXPECT_EQ(report.methods[0].method, pickId);   /// 名序：pick < stub
    EXPECT_EQ(report.methods[0].drive.appliedCount(), 1u);   /// pick 未被阻断
    EXPECT_EQ(report.methods[1].method, stubId);
    EXPECT_EQ(report.methods[1].drive.note, OutlineDriveNote::ParentUnusable);
    EXPECT_EQ(report.methods[1].drive.appliedCount(), 0u);
    EXPECT_EQ(report.totalApplied(), 1u);
    EXPECT_EQ(report.skippedCount, 0u);
    EXPECT_EQ(report.rejectedCount, 0u);
    EXPECT_EQ(report.failedCount, 1u);
}

/// 快跳面（jsr 方法）：RegionView 枚举零候选（子程序结构不交付 SESE
/// 区域）→ 便宜预检快跳（skippedCount），不走驱动——保守结局与全拒等价
/// （零落地），策略级便签（MethodSubroutine）不产出（预检只计数）。
TEST(MjAsmSemMethodDriveTest, JsrMethodFastSkipped)
{
    ClassBytesBuilder b("T");
    SemFixture fixture{b.assemble({branchPickSpec("pick"), jsrSpec("jsr")})};
    const MethodId pickId = methodIdOf(fixture, "pick");
    const MethodId jsrId  = methodIdOf(fixture, "jsr");
    ASSERT_NE(pickId, jbc::ir::kNoMethodId);
    ASSERT_NE(jsrId, jbc::ir::kNoMethodId);

    jbc::analysis::ShapeResolver resolver;
    MethodDriveRequest req{*fixture.session, resolver};
    req.methods                   = {jsrId, pickId};
    req.drivePolicy.excludeOutlineRelay = true;
    const MethodDriveReport report = driveMethods(req);

    EXPECT_EQ(report.skippedCount, 1u);
    ASSERT_EQ(report.methods.size(), 1u);
    EXPECT_EQ(report.methods[0].method, pickId);
    EXPECT_EQ(report.methods[0].drive.appliedCount(), 1u);
    EXPECT_EQ(report.totalApplied(), 1u);
    EXPECT_EQ(report.rejectedCount, 0u);
    EXPECT_EQ(report.failedCount, 0u);
}

/// 全拒隔离：构造器（判据 6 F5 形态，候选非空但策略全滤）驱动全拒
/// SelectRejected 计入 rejectedCount，不阻断同池其他方法。
TEST(MjAsmSemMethodDriveTest, RejectedMethodIsolated)
{
    ClassBytesBuilder b("T");
    SemFixture fixture{b.assemble({branchPickSpec("pick"), ctorSpec()})};
    const MethodId pickId = methodIdOf(fixture, "pick");
    const MethodId ctorId = methodIdOf(fixture, "<init>");
    ASSERT_NE(pickId, jbc::ir::kNoMethodId);
    ASSERT_NE(ctorId, jbc::ir::kNoMethodId);

    jbc::analysis::ShapeResolver resolver;
    MethodDriveRequest req{*fixture.session, resolver};
    req.methods                   = {ctorId, pickId};
    req.drivePolicy.excludeOutlineRelay = true;
    const MethodDriveReport report = driveMethods(req);

    ASSERT_EQ(report.methods.size(), 2u);
    EXPECT_EQ(report.methods[0].method, ctorId);   /// 名序：'<' < 'p'，ctor 先
    EXPECT_EQ(report.methods[0].drive.note, OutlineDriveNote::SelectRejected);
    EXPECT_EQ(report.methods[0].drive.stopSelectNote, RegionSelectNote::AllFiltered);
    EXPECT_EQ(report.methods[1].method, pickId);
    EXPECT_EQ(report.methods[1].drive.appliedCount(), 1u);   /// pick 未被阻断
    EXPECT_EQ(report.totalApplied(), 1u);
    EXPECT_EQ(report.rejectedCount, 1u);
    EXPECT_EQ(report.failedCount, 0u);
    EXPECT_EQ(report.skippedCount, 0u);
}

/// 空池缺省：全量有体方法入池（无体方法不进池），顺序仍由策略归一——
/// 驱动新产出的合成方法不进池（快照语义）。
TEST(MjAsmSemMethodDriveTest, EmptyPoolEnumeratesAllWithCode)
{
    ClassBytesBuilder b("T");
    SemFixture fixture{b.assemble({branchPickSpec("zeta"), abstractSpec("stub"),
                                   branchPickSpec("alpha")})};
    const MethodId alphaId = methodIdOf(fixture, "alpha");
    const MethodId zetaId  = methodIdOf(fixture, "zeta");
    ASSERT_NE(alphaId, jbc::ir::kNoMethodId);
    ASSERT_NE(zetaId, jbc::ir::kNoMethodId);

    jbc::analysis::ShapeResolver resolver;
    MethodDriveRequest req{*fixture.session, resolver};   /// methods 空 = 全量
    req.drivePolicy.excludeOutlineRelay = true;
    const MethodDriveReport report = driveMethods(req);

    ASSERT_EQ(report.methods.size(), 2u);   /// stub 无体不进池
    EXPECT_EQ(report.methods[0].method, alphaId);
    EXPECT_EQ(report.methods[1].method, zetaId);
    EXPECT_EQ(report.totalApplied(), 2u);
    EXPECT_EQ(report.skippedCount, 0u);
    EXPECT_EQ(report.failedCount, 0u);
}

/// drive 档位透传（v1 接力档）：排除档关闭 + 每方法上限 2——单区域
/// 方法接力切层至命中上限（IterationCap），证明 drivePolicy / maxRounds
/// 逐字段透传给 OutlineDriveRequest（编排层不改 outline 行为，只透传档位）。
TEST(MjAsmSemMethodDriveTest, DriveTierPassThroughRelayKept)
{
    ClassBytesBuilder b("T");
    SemFixture fixture{b.assemble({branchPickSpec("pick")})};
    const MethodId pickId = methodIdOf(fixture, "pick");
    ASSERT_NE(pickId, jbc::ir::kNoMethodId);

    jbc::analysis::ShapeResolver resolver;
    MethodDriveRequest req{*fixture.session, resolver};
    req.methods       = {pickId};
    req.maxRounds     = 2;   /// 缺省排除档关闭 = v1 接力档
    const MethodDriveReport report = driveMethods(req);

    ASSERT_EQ(report.methods.size(), 1u);
    EXPECT_EQ(report.methods[0].drive.note, OutlineDriveNote::IterationCap);
    ASSERT_EQ(report.methods[0].drive.appliedCount(), 2u);
    EXPECT_EQ(report.totalApplied(), 2u);
    EXPECT_EQ(report.failedCount, 0u);
}

/// 挑选策略 v1（selectMethods 直测）：内部名序 + 重载描述符破平；档位
/// 关闭回落池序（插拔点换档口径）。
TEST(MjAsmSemMethodDriveTest, SelectorNameOrderAndOverloadTieBreak)
{
    /// 重载对：同名 "f" 两描述符——builder 直拼两个同名方法。
    ClassBytesBuilder b("T");
    MethodSpec wide = straightSpec("f");
    wide.desc       = "(II)I";
    SemFixture ovl{b.assemble({straightSpec("f"), wide, straightSpec("e")})};
    const MethodId f1 = ovl.session->unit().findMethodId("f", "(I)I");
    const MethodId f2 = ovl.session->unit().findMethodId("f", "(II)I");
    const MethodId e1 = ovl.session->unit().findMethodId("e", "(I)I");
    ASSERT_NE(f1, jbc::ir::kNoMethodId);
    ASSERT_NE(f2, jbc::ir::kNoMethodId);
    ASSERT_NE(e1, jbc::ir::kNoMethodId);

    MethodSelectPolicy policy;   /// v1 缺省：内部名序
    const std::vector<MethodId> order = mjbase::asm_sem::selectMethods(
        policy, mjbase::asm_sem::MethodSelectRequest{*ovl.session, {f2, e1, f1}});
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], e1);   /// e < f
    EXPECT_EQ(order[1], f1);   /// 同名重载：描述符 "(I)I" < "(II)I"
    EXPECT_EQ(order[2], f2);

    MethodSelectPolicy keepPool;              /// 档位关闭 = 保池序
    keepPool.internalNameOrder = false;
    const std::vector<MethodId> poolOrder = mjbase::asm_sem::selectMethods(
        keepPool, mjbase::asm_sem::MethodSelectRequest{*ovl.session, {f2, e1, f1}});
    ASSERT_EQ(poolOrder.size(), 3u);
    EXPECT_EQ(poolOrder[0], f2);
    EXPECT_EQ(poolOrder[1], e1);
    EXPECT_EQ(poolOrder[2], f1);
}

/// JVM 真跑端到端门：javac 语料 ≥2 方法 → 方法级编排驱动（排除档，每方法
/// 恰切 1 层收敛；javac 默认构造器零候选快跳）→ 写回 class → java 运行
/// 行为与拆分前逐字一致（-Xverify:all 全程验证合成方法链；JDK 缺失 SKIP）。
TEST(MjAsmSemMethodDriveTest, JvmGateTwoMethodOrchestrationBehaviorPreserved)
{
    namespace fs = std::filesystem;
    static const auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path scratch =
        fs::temp_directory_path()
        / ("mjbase_method_driver_gate_" + std::to_string(
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
               "    static int mix(int a, int b, boolean c) {\n"
               "        return (c ? a - b : b + a) + 50;\n"
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
    const auto visits = jbc::ir::collectCodeUnits(*session);   /// 方法升格前置
    ASSERT_FALSE(visits.empty());

    jbc::analysis::ShapeResolver resolver;
    MethodDriveRequest req{*session, resolver};   /// 空池 = 全量已升格方法
    req.drivePolicy.excludeOutlineRelay = true;   /// 排除接力候选（每方法 1 层收敛）
    const MethodDriveReport report = driveMethods(req);

    /// javac 默认构造器 <init> 直线无候选 → 快跳；pick/mix 名序各切 1 层。
    const jbc::ir::ClassUnit& cu = session->unit();
    ASSERT_EQ(report.methods.size(), 2u) << mjbase::asm_sem::to_string(
        report.methods.empty() ? OutlineDriveNote::CandidatesExhausted
                               : report.methods[0].drive.note);
    EXPECT_EQ(report.methods[0].method,
              cu.findMethodId("mix"));
    EXPECT_EQ(report.methods[1].method,
              cu.findMethodId("pick"));
    EXPECT_EQ(report.skippedCount, 1u);   /// <init> 零候选快跳
    for (ca::usize i = 0; i < report.methods.size(); ++i) {
        ASSERT_EQ(report.methods[i].drive.appliedCount(), 1u) << "method " << i;
        EXPECT_EQ(report.methods[i].drive.note, OutlineDriveNote::CandidatesExhausted)
            << "method " << i;
        EXPECT_NE(report.methods[i].drive.rounds[0].outcome.outlined,
                  jbc::ir::kNoMethodId)
            << "method " << i;
    }
    EXPECT_EQ(report.totalApplied(), 2u);
    EXPECT_EQ(report.failedCount, 0u);
    EXPECT_EQ(report.rejectedCount, 0u);

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    const Bytes outlined = std::move(written).unwrap();

    const std::string runnerBody =
        "System.out.println(OutlineSample.pick(3, 4, true));"
        "System.out.println(OutlineSample.pick(3, 4, false));"
        "System.out.println(OutlineSample.mix(5, 2, true));"
        "System.out.println(OutlineSample.mix(5, 2, false));";

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

    EXPECT_EQ(afterOut, "106\n107\n53\n57\n");
    EXPECT_EQ(afterOut, beforeOut);   /// -Xverify:all 全程验证 + 行为逐字一致
    fs::remove_all(scratch, ec);
}
