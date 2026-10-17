// UnusedCheckCleaner：requireNonNull 三元组删除 + 静态方法豁免 + 形态守卫。
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/unused_check_cleaner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

constexpr ca::u16 kAccPublic = 0x0001;   // 非 static（ACC_STATIC 不置）

/// ALOAD_0; INVOKESTATIC Objects.requireNonNull; POP; RETURN。
Bytes checkBody(ca::u16 requireNonNullCp)
{
    return {0x2a,
            0xb8,
            static_cast<ca::u8>(requireNonNullCp >> 8),
            static_cast<ca::u8>(requireNonNullCp),
            0x57,
            0xb1};
}

/// 装配含 requireNonNull 引用的容器。
FixtureBuilder makeFixture()
{
    FixtureBuilder b("t/U");
    return b;
}

}   // namespace

// ── 实例方法：ALOAD_0 + requireNonNull + POP 三元组删除 ──
TEST(UnusedCheckCleanerTest, removesTripleInInstanceMethod)
{
    FixtureBuilder b = makeFixture();
    const ca::u16  cp = b.methodRef("java/util/Objects", "requireNonNull",
                                    "(Ljava/lang/Object;)Ljava/lang/Object;");
    b.addMethod({"m", "()V", checkBody(cp), kAccPublic});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::UnusedCheckCleaner{});

    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 静态方法豁免（ALOAD_0 无 this 语义）──
TEST(UnusedCheckCleanerTest, staticMethodUntouched)
{
    FixtureBuilder b = makeFixture();
    const ca::u16  cp = b.methodRef("java/util/Objects", "requireNonNull",
                                    "(Ljava/lang/Object;)Ljava/lang/Object;");
    b.addMethod({"m", "()V", checkBody(cp)});   // 缺省含 ACC_STATIC
    const Bytes          in = b.assemble();
    const MethodPassRun  r  = runMethodPass(in, mjbase::pass::UnusedCheckCleaner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 非 requireNonNull 的 INVOKESTATIC：不动 ──
TEST(UnusedCheckCleanerTest, otherStaticCallUntouched)
{
    FixtureBuilder b = makeFixture();
    const ca::u16  cp = b.methodRef("java/util/Objects", "toString", "(Ljava/lang/Object;)Ljava/lang/String;");
    b.addMethod({"m", "()V", checkBody(cp), kAccPublic});
    const Bytes          in = b.assemble();
    const MethodPassRun  r  = runMethodPass(in, mjbase::pass::UnusedCheckCleaner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 边界守卫：POP 被分支引用时三元组跳过 ──
TEST(UnusedCheckCleanerTest, branchTargetPopBlocksTriple)
{
    // ALOAD_0; INVOKESTATIC; POP; RETURN; GOTO -2（跳回 POP，pc4）。
    FixtureBuilder b = makeFixture();
    const ca::u16  cp = b.methodRef("java/util/Objects", "requireNonNull",
                                    "(Ljava/lang/Object;)Ljava/lang/Object;");
    Bytes code = checkBody(cp);
    appendBytes(code, {0xa8, 0xff, 0xfe});   // GOTO 自 pc6 落回 pc4（6-2）
    b.addMethod({"m", "()V", std::move(code), kAccPublic});
    const Bytes          in = b.assemble();
    const MethodPassRun  r  = runMethodPass(in, mjbase::pass::UnusedCheckCleaner{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    const std::vector<Op> ops = opsOf(inspect(r.out));
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[2], Op::POP);
}

// ── 注册面 ──
TEST(UnusedCheckCleanerTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("UnusedCheckCleaner"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("UnusedCheckCleaner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
