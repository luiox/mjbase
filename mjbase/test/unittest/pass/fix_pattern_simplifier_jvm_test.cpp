// FixPatternSimplifier 真 JVM 硬验收（C 方法级指令变换组首件）：
// CodeDom 直构分支语料（无 StackMapTable）→ fixture 面经 A1 链补帧（V50+
// 真验证器门）→ mjbase 方法级管线变换（changed 方法的帧由管线同一条链再生，
// 未 changed 方法的帧原样保留）→ 变换产物交真 java 执行，stdout 必须与原样本
// 一致。全程无 classfile 裸字节输入。
//
// 覆盖面：等值比较常量偏移折叠（含 int 极值）、非零偏移大小比较保留、
// 同 slot store/load 中转消除（clone producer）、窗口头分支目标标签迁移、
// 位运算固定模式与 x^x 死代码。JDK 不可用的机器整测跳过（硬验收门在有 JDK
// 的环境强制成立）。
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>

#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/pass/fix_pattern_simplifier.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"

#include "fix_pattern_fixture.hpp"
#include "jdk_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

using Bytes = std::vector<ca::u8>;

/// 语料类 hb/jvm/FixProbe：七方法覆盖触发面与不变面（全部 public static）。
DomFixture buildFixProbe()
{
    DomFixture fx("hb/jvm/FixProbe", "cmpEq", "(I)I", 2, 1, [](Body& b) {
        // ILOAD_0; ICONST_1; IADD; ICONST_2; IF_ICMPEQ L——折叠为 x == 1。
        b.iload(0);
        b.op(Op::ICONST_1);
        b.op(Op::IADD);
        b.op(Op::ICONST_2);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    return fx;
}

/// 多方法语料：在 buildFixProbe 的单方法骨架上逐方法追加（种子类只建一次，
/// 其余方法经 addBodyMethod + CodeDom 构建挂入同一会话）。
std::shared_ptr<jbc::ir::ClassSession> buildProbeSession()
{
    auto    probe = buildFixProbe();
    auto    s     = probe.session;

    // cmpGtKeep(I)I：同形但 IF_ICMPGT + 非零偏移——不变面。
    addBodyMethod(*s, "cmpGtKeep", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::ICONST_1);
        b.op(Op::IADD);
        b.op(Op::ICONST_2);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPGT, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    // storeLoad()I：LDC c1; ISTORE 5; LDC c2; ILOAD 5; IF_ICMPGT L——中转消除。
    addBodyMethod(*s, "storeLoad", "()I", 2, 6, [](Body& b) {
        b.ldcInt(432559991);
        b.istore(5);
        b.ldcInt(153092102);
        b.iload(5);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPGT, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        b.bindHere(hit);
        b.op(Op::IRETURN);
    });
    // headLabel(I)I：IFEQ 跳窗口头（L0: ICONST_1）——折叠后标签迁移到替换头。
    addBodyMethod(*s, "headLabel", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::ICONST_0);
        const jbc::ir::LabelId zero = b.label();
        b.branch(Op::IFEQ, zero);
        b.op(Op::ICONST_5);
        b.op(Op::POP);
        b.op(Op::ICONST_1);
        const jbc::ir::InsnId atHead = b.last();
        b.op(Op::IADD);
        b.op(Op::ICONST_3);
        const jbc::ir::LabelId hit = b.label();
        b.branch(Op::IF_ICMPEQ, hit);
        b.op(Op::ICONST_0);
        b.op(Op::IRETURN);
        b.op(Op::ICONST_1);
        const jbc::ir::InsnId atHit = b.last();
        b.op(Op::IRETURN);
        b.bindAt(zero, atHead);
        b.bindAt(hit, atHit);
    });
    // bitwise()I：规则 7（dup_x1 + iconst_m1 + ixor + iand + iadd → ior）。
    addBodyMethod(*s, "bitwise", "()I", 4, 0, [](Body& b) {
        b.op(Op::ICONST_0);
        b.op(Op::ICONST_1);
        b.op(Op::DUP_X1);
        b.op(Op::ICONST_M1);
        b.op(Op::IXOR);
        b.op(Op::IAND);
        b.op(Op::IADD);
        b.op(Op::IRETURN);
    });
    // xorZero(I)I：规则 8（load + dup + ixor → iconst_0）。
    addBodyMethod(*s, "xorZero", "(I)I", 2, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::DUP);
        b.op(Op::IXOR);
        b.op(Op::IRETURN);
    });
    // deadPair()V：规则 9b（iconst_1 + pop → 空）。
    addBodyMethod(*s, "deadPair", "()V", 1, 0, [](Body& b) {
        b.op(Op::ICONST_1);
        b.op(Op::POP);
        b.op(Op::RETURN);
    });
    // longHalfAdder(J)J：规则 15（(x^K)+((x&K)<<1) == x+K 的 long 形态，
    // 先算进位项再算主项）。
    addBodyMethod(*s, "longHalfAdder", "(J)J", 8, 2, [](Body& b) {
        b.lload(0);
        b.ldcLong(11111111111111LL);
        b.op(Op::LAND);
        b.op(Op::ICONST_1);
        b.op(Op::LSHL);
        b.lload(0);
        b.ldcLong(11111111111111LL);
        b.op(Op::LXOR);
        b.op(Op::LADD);
        b.lreturn();
    });
    // longBorrow(J)J：规则 16（(x^K)-((K&~x)<<1) == x-K 的 long 形态，
    // 主项先行、借位项压顶——lsub 深减顶）。
    addBodyMethod(*s, "longBorrow", "(J)J", 8, 2, [](Body& b) {
        b.lload(0);
        b.ldcLong(-22222222222222LL);
        b.op(Op::LXOR);
        b.lload(0);
        b.op(Op::ICONST_M1);
        b.op(Op::I2L);
        b.op(Op::LXOR);
        b.ldcLong(-22222222222222LL);
        b.op(Op::LAND);
        b.op(Op::ICONST_1);
        b.op(Op::LSHL);
        b.op(Op::LSUB);
        b.lreturn();
    });
    // longCarryCancel(J)J：规则 17（(x+K)-((x&K)<<1) == x^K 的 long 形态，
    // 主项先行、进位项压顶——lsub 深减顶）。
    addBodyMethod(*s, "longCarryCancel", "(J)J", 8, 2, [](Body& b) {
        b.lload(0);
        b.ldcLong(33333333333333LL);
        b.op(Op::LADD);
        b.lload(0);
        b.ldcLong(33333333333333LL);
        b.op(Op::LAND);
        b.op(Op::ICONST_1);
        b.op(Op::LSHL);
        b.op(Op::LSUB);
        b.lreturn();
    });
    // longXorZero(J)J：规则 18（x ^ x == 0）。
    addBodyMethod(*s, "longXorZero", "(J)J", 4, 2, [](Body& b) {
        b.lload(0);
        b.op(Op::DUP2);
        b.op(Op::LXOR);
        b.lreturn();
    });
    // longCmpSelf(J)I：规则 19（lcmp 自比恒 0）。
    addBodyMethod(*s, "longCmpSelf", "(J)I", 4, 2, [](Body& b) {
        b.lload(0);
        b.op(Op::DUP2);
        b.op(Op::LCMP);
        b.ireturn();
    });
    // wideRoundTrip(I)I：规则 20（i2l; l2i 往返恒等）。
    addBodyMethod(*s, "wideRoundTrip", "(I)I", 3, 1, [](Body& b) {
        b.iload(0);
        b.op(Op::I2L);
        b.op(Op::L2I);
        b.ireturn();
    });
    return s;
}

/// 变换：FixPatternSimplifier 经方法级管线（注入 A1 帧链接线——changed 方法
/// 的 SMT 悬垂后由同一条链再生）。
Bytes transformClass(std::shared_ptr<jbc::ir::ClassSession> session,
                     mjbase::spi::StrictTypeResolverAdapter& typeResolver)
{
    mjbase::spi::PassPipelineOptions options;
    options.typeResolver = &typeResolver;

    ca::str::Utf8StringArena           arena;
    mjbase::spi::MethodAnalysisManager mam(arena);
    mjbase::KeyPool                    identityPool;
    mjbase::spi::MethodPassManager     mpm;
    mpm.addPass(std::make_unique<mjbase::pass::FixPatternSimplifier>());

    const auto pa = mjbase::spi::runMethodPipelineOverSession(*session, mpm, mam,
                                                              identityPool, nullptr, options);
    EXPECT_TRUE(pa.changed()) << "pass 在语料上未发生任何改动（触发面失效）";

    auto written = jbc::core::writeClass(session->backing());
    EXPECT_TRUE(written.is_ok());
    return std::move(written).unwrap();
}

constexpr const char* kRunnerBody = R"JAVA(
System.out.println(hb.jvm.FixProbe.cmpEq(1));
System.out.println(hb.jvm.FixProbe.cmpEq(0));
System.out.println(hb.jvm.FixProbe.cmpEq(Integer.MIN_VALUE));
System.out.println(hb.jvm.FixProbe.cmpEq(Integer.MAX_VALUE));
System.out.println(hb.jvm.FixProbe.cmpGtKeep(0));
System.out.println(hb.jvm.FixProbe.cmpGtKeep(2));
System.out.println(hb.jvm.FixProbe.cmpGtKeep(Integer.MAX_VALUE));
System.out.println(hb.jvm.FixProbe.storeLoad());
System.out.println(hb.jvm.FixProbe.headLabel(2));
System.out.println(hb.jvm.FixProbe.headLabel(1));
System.out.println(hb.jvm.FixProbe.bitwise());
System.out.println(hb.jvm.FixProbe.xorZero(42));
System.out.println(hb.jvm.FixProbe.xorZero(Integer.MIN_VALUE));
hb.jvm.FixProbe.deadPair();
System.out.println(hb.jvm.FixProbe.longHalfAdder(0L));
System.out.println(hb.jvm.FixProbe.longHalfAdder(-1L));
System.out.println(hb.jvm.FixProbe.longHalfAdder(Long.MIN_VALUE));
System.out.println(hb.jvm.FixProbe.longHalfAdder(Long.MAX_VALUE));
System.out.println(hb.jvm.FixProbe.longBorrow(123456789012345L));
System.out.println(hb.jvm.FixProbe.longBorrow(-1L));
System.out.println(hb.jvm.FixProbe.longBorrow(Long.MIN_VALUE));
System.out.println(hb.jvm.FixProbe.longBorrow(Long.MAX_VALUE));
System.out.println(hb.jvm.FixProbe.longCarryCancel(987654321098L));
System.out.println(hb.jvm.FixProbe.longCarryCancel(-1L));
System.out.println(hb.jvm.FixProbe.longCarryCancel(Long.MIN_VALUE));
System.out.println(hb.jvm.FixProbe.longCarryCancel(Long.MAX_VALUE));
System.out.println(hb.jvm.FixProbe.longXorZero(42L));
System.out.println(hb.jvm.FixProbe.longXorZero(Long.MIN_VALUE));
System.out.println(hb.jvm.FixProbe.longCmpSelf(0L));
System.out.println(hb.jvm.FixProbe.longCmpSelf(-987654321098L));
System.out.println(hb.jvm.FixProbe.wideRoundTrip(0));
System.out.println(hb.jvm.FixProbe.wideRoundTrip(-5));
System.out.println(hb.jvm.FixProbe.wideRoundTrip(Integer.MIN_VALUE));
System.out.println("done");
)JAVA";

}   // namespace

TEST(FixPatternSimplifierJvm, TransformedClassesRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    mjbase::KeyPool                        pool;
    mjbase::hierarchy::ClassHierarchy      hierarchy{pool};
    mjbase::spi::CommonSuperClassResolver  resolver{hierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{resolver};

    // ── 1. 语料 + fixture 面补帧（V50+ 真验证器门）→ 原样本基线运行 ──
    auto baselineSession = buildProbeSession();
    attachFrames(*baselineSession, typeResolver);
    const Bytes baseline = [&] {
        auto written = jbc::core::writeClass(baselineSession->backing());
        EXPECT_TRUE(written.is_ok());
        return std::move(written).unwrap();
    }();

    test::jdk::RunnerSpec before;
    before.classes    = {{"hb/jvm/FixProbe", baseline}};
    before.runnerBody = kRunnerBody;
    const auto beforeOut = test::jdk::runWithRunnerOutcome(tc, before);
    ASSERT_TRUE(beforeOut.ok()) << "原样本运行失败:\n" << beforeOut.output;

    // ── 2. 变换（fresh 会话 + A1 帧链接线：changed 方法帧再生，未 changed
    //       方法的基线帧原样保留）→ 落盘 ──
    auto transformSession = sessionFromBytes(baseline);
    ASSERT_NE(transformSession, nullptr);
    const Bytes out = transformClass(transformSession, typeResolver);

    // ── 3. 硬验收：真 java 执行变换产物，行为与原样本一致 ──
    test::jdk::RunnerSpec after;
    after.classes    = {{"hb/jvm/FixProbe", out}};
    after.runnerBody = kRunnerBody;
    const auto afterOut = test::jdk::runWithRunnerOutcome(tc, after);
    EXPECT_TRUE(afterOut.ok()) << "变换产物被拒/失败:\n" << afterOut.output;
    EXPECT_EQ(test::jdk::normalizeOutput(afterOut.output), beforeOut.output);
}
