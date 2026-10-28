// TryCatchCleanup：透明重抛三形状删除（共享/可达/重叠守卫）、恒不抛收口、
// NPE 形改写（栈深门槛 + 锚入标签守卫）、自捕抛改写、空区间修复删除、
// 非当前类恒等包装保守不认。
#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/try_catch_cleanup.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

void put16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v & 0xff));
}

void putSipush(Bytes& b, ca::i16 v) { put16(b, static_cast<ca::u16>(v)); }

void putInvoke(Bytes& b, ca::u8 op, ca::u16 cp)
{
    b.push_back(op);
    put16(b, cp);
}

ExcSpec exc(ca::u16 start, ca::u16 end, ca::u16 handler, ca::u16 catchCp = 0)
{
    return ExcSpec{start, end, handler, catchCp};
}

std::vector<Op> opsOfOut(const MethodPassRun& r)
{
    return opsOf(inspect(r.out));
}

}   // namespace

// ── 透明重抛（直接 athrow）→ 条目删除 ──
TEST(TryCatchCleanupTest, transparentDirectAthrowRemoved)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x01, 0xbf,   // ACONST_NULL ATHROW（保护区）
                  0xbf,         // handler@2: ATHROW（透明）
                  0xb1};        // RETURN
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 2, 2)}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 透明重抛（同槽 astore/aload）→ 条目删除 ──
TEST(TryCatchCleanupTest, transparentAstoreAloadRemoved)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x01, 0xbf,   // ACONST_NULL ATHROW
                  0x4b, 0x2a, 0xbf,   // handler@2: ASTORE_0 ALOAD_0 ATHROW
                  0xb1};
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 2, 2)}, 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 透明重抛（当前类恒等静态包装）→ 条目删除 ──
TEST(TryCatchCleanupTest, transparentIdentityWrapperRemoved)
{
    FixtureBuilder b("t/C");
    const ca::u16 wrap = b.methodRef("t/C", "wrap",
                                     "(Ljava/lang/Throwable;)Ljava/lang/Throwable;");
    Bytes code = {0x01, 0xbf};   // ACONST_NULL ATHROW
    putInvoke(code, 0xb8, wrap); // handler@2: INVOKESTATIC t/C.wrap
    code.push_back(0xbf);        // ATHROW
    code.push_back(0xb1);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 2, 2)}});
    b.addMethod({"wrap", "(Ljava/lang/Throwable;)Ljava/lang/Throwable;",
                 Bytes{0x2a, 0xb0}, 0x0009});   // public static; aload_0; areturn
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 非当前类 owner 的包装调用：保守不认 → 保留 ──
TEST(TryCatchCleanupTest, identityWrapperForeignOwnerKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 wrap = b.methodRef("other/C", "wrap",
                                     "(Ljava/lang/Throwable;)Ljava/lang/Throwable;");
    Bytes code = {0x01, 0xbf};
    putInvoke(code, 0xb8, wrap);
    code.push_back(0xbf);
    code.push_back(0xb1);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 2, 2)}});
    b.addMethod({"wrap", "(Ljava/lang/Throwable;)Ljava/lang/Throwable;",
                 Bytes{0x2a, 0xb0}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── handler 被两条目共享 → 透明面拒收（条目1 保留；条目2 保护区为死代码，被恒不抛面删） ──
TEST(TryCatchCleanupTest, transparentSharedHandlerEntry1Kept)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x01, 0xbf,   // ACONST_NULL ATHROW（条目1 保护区）
                  0x01, 0xbf,   // ACONST_NULL ATHROW（条目2 保护区）
                  0xbf,         // handler@4: ATHROW（共享）
                  0xb1};
    b.addMethod({"m", "()V", std::move(code), 0x0009,
                 {exc(0, 2, 4), exc(2, 4, 4)}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);   // 条目1（共享）保留
}

// ── handler 正常流可达 → 透明面拒收 ──
TEST(TryCatchCleanupTest, transparentReachableHandlerKept)
{
    FixtureBuilder b("t/C");
    Bytes code;
    code.push_back(0x03);   // ICONST_0（pc0）
    code.push_back(0x99);   // IFEQ → pc5（pc1，3B）
    putSipush(code, 4);
    code.push_back(0xbf);   // ATHROW（pc4，保护区内）
    code.push_back(0xbf);   // handler@5: ATHROW（IFEQ 跳入 = 正常可达）
    code.push_back(0xb1);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 5, 5)}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── 恒不抛段收口：区间内无可达可抛指令 → 条目删除 ──
TEST(TryCatchCleanupTest, impossibleCatchRemoved)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x03, 0x04,   // ICONST_0 ICONST_1（均不抛）
                  0x57,         // handler@2: POP
                  0xb1};
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 2, 2)}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 区间内含可达可抛指令（ATHROW）→ 保留 ──
TEST(TryCatchCleanupTest, impossibleCatchWithThrowKept)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x03, 0xbf,   // ICONST_0 ATHROW
                  0x57,         // handler@2: POP
                  0xb1};
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 2, 2)}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── NPE 形：aconst_null;athrow + pop;goto handler → aconst_null 改 goto 续点 ──
TEST(TryCatchCleanupTest, npeShapeRewritten)
{
    FixtureBuilder b("t/C");
    const ca::u16 npe = b.cls("java/lang/NullPointerException");
    Bytes code = {0x01, 0xbf,   // ACONST_NULL ATHROW（保护区 pc0-1）
                  0x57,         // handler@2: POP
                  0xa7, 0x00, 0x03,   // GOTO → pc6（handler 跳续点）
                  0xb1,         // RETURN（pc4）
                  0x03,         // ICONST_0（pc5）
                  0xb1};        // RETURN（pc6，续点锚）
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 2, 2, npe)}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    ASSERT_EQ(ops.size(), 6u);   // athrow 已删
    EXPECT_EQ(ops[0], Op::GOTO); // aconst_null → goto 续点
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── NPE 形 + 保护区入口栈积压（depth≠0）→ 拒收 ──
TEST(TryCatchCleanupTest, npeShapeJunkStackKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 npe = b.cls("java/lang/NullPointerException");
    Bytes code = {0x11};
    putSipush(code, 3);          // SIPUSH 3（pc0，积压）
    code.push_back(0x01);        // ACONST_NULL（pc3）
    code.push_back(0xbf);        // ATHROW（pc4）
    code.push_back(0x57);        // handler@5: POP
    code.push_back(0xa7);        // GOTO → pc9（pc6）
    putSipush(code, 3);
    code.push_back(0xb1);        // RETURN（pc9... 见下）
    code.push_back(0x03);
    code.push_back(0xb1);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(3, 5, 5, npe)}, 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── NPE 形 + athrow 被分支锚入 → 拒收 ──
TEST(TryCatchCleanupTest, npeShapeForeignLabelKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 npe = b.cls("java/lang/NullPointerException");
    Bytes code;
    code.push_back(0xa7);        // GOTO → pc4（athrow，锚入）
    putSipush(code, 4);
    code.push_back(0x01);        // ACONST_NULL（pc3）
    code.push_back(0xbf);        // ATHROW（pc4，锚点）
    code.push_back(0x57);        // handler@5: POP
    code.push_back(0xa7);        // GOTO → pc9
    putSipush(code, 4);
    code.push_back(0xb1);
    code.push_back(0x03);
    code.push_back(0xb1);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(3, 5, 5, npe)}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── 自捕抛：new E;dup;invokespecial;athrow 被自身 handler 接住 → athrow 改 goto handler ──
TEST(TryCatchCleanupTest, selfCaughtThrowRewritten)
{
    FixtureBuilder b("t/C");
    const ca::u16 clsE = b.cls("t/E");
    const ca::u16 init = b.methodRef("t/E", "<init>", "()V");
    Bytes code;
    putInvoke(code, 0xbb, clsE);   // NEW t/E（pc0）
    code.push_back(0x59);          // DUP（pc3）
    putInvoke(code, 0xb7, init);   // INVOKESPECIAL <init>（pc4）
    code.push_back(0xbf);          // ATHROW（pc7）
    code.push_back(0x57);          // handler@8: POP
    code.push_back(0xa7);          // GOTO → pc12
    putSipush(code, 3);
    code.push_back(0xb1);          // RETURN
    code.push_back(0x03);
    code.push_back(0xb1);          // RETURN
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 8, 8, clsE)}, 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOfOut(r);
    ASSERT_EQ(ops.size(), 9u);
    EXPECT_EQ(ops[0], Op::NEW);
    EXPECT_EQ(ops[1], Op::DUP);
    EXPECT_EQ(ops[2], Op::INVOKESPECIAL);
    EXPECT_EQ(ops[3], Op::GOTO);   // athrow → goto handler
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 自捕抛 + athrow 处栈深 2 → 拒收 ──
TEST(TryCatchCleanupTest, selfCaughtDeepStackKept)
{
    FixtureBuilder b("t/C");
    const ca::u16 clsE = b.cls("t/E");
    const ca::u16 init = b.methodRef("t/E", "<init>", "()V");
    Bytes code = {0x11};
    putSipush(code, 9);            // SIPUSH 9（积压）
    putInvoke(code, 0xbb, clsE);   // NEW（pc3）
    code.push_back(0x59);          // DUP
    putInvoke(code, 0xb7, init);
    code.push_back(0xbf);          // ATHROW（栈深 2）
    code.push_back(0x57);          // handler: POP
    code.push_back(0xa7);
    putSipush(code, 3);
    code.push_back(0xb1);
    code.push_back(0x03);
    code.push_back(0xb1);
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(3, 11, 11, clsE)}, 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    const Inspection ins = inspect(r.out);
    EXPECT_EQ(ins.cu->exc.size(), 1u);
}

// ── 空区间条目修复性删除 ──
TEST(TryCatchCleanupTest, emptyRangeRemoved)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x03, 0xb1};   // ICONST_0 RETURN
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 0, 0)}});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    EXPECT_TRUE(ins.cu->exc.empty());
}

// ── 幂等 ──
TEST(TryCatchCleanupTest, idempotent)
{
    FixtureBuilder b("t/C");
    Bytes code = {0x01, 0xbf, 0xbf, 0xb1};
    b.addMethod({"m", "()V", std::move(code), 0x0009, {exc(0, 2, 2)}});
    const Bytes          bytes = b.assemble();
    const MethodPassRun  r     = runMethodPass(bytes, mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r.pa.changed());
    const MethodPassRun r2 = runMethodPass(r.out, mjbase::pass::TryCatchCleanup{});
    EXPECT_TRUE(r2.pa.areAllPreserved());
}

// ── 注册面 ──
TEST(TryCatchCleanupTest, registeredInRegistry)
{
    const mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    const auto                           e   = reg.find("TryCatchCleanup");
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.unwrap().scope, mjbase::spi::PassScope::Method);
}
