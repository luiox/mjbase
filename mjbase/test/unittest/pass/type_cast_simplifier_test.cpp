#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "mjbase/pass/method_rewrite_util.hpp"
#include "mjbase/pass/type_cast_simplifier.hpp"

#include "fix_pattern_fixture.hpp"

namespace mjbase::test {

namespace {

using jbc::core::Op;
using mjbase::pass::TypeCastSimplifier;

const jbc::ir::Insn* insnAt(const DomFixture& fx, ca::usize index)
{
    const auto& order = fx.session->unit().methods[0].code->dom.order();
    if (index >= order.size()) return nullptr;
    return fx.session->unit().methods[0].code->dom.peek(order[index]);
}

}   // namespace

TEST(MjTypeCastSimplifierTest, FoldsIntToByte)
{
    DomFixture fx("CastI2B", "run", "()I", 1, 0, [](Body& b) {
        b.ldcInt(200).op(Op::I2B).op(Op::IRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    EXPECT_EQ(ops[1], Op::IRETURN);
    const jbc::ir::Insn* folded = insnAt(fx, 0);
    ASSERT_NE(folded, nullptr);
    EXPECT_EQ(folded->a, -56);
}

TEST(MjTypeCastSimplifierTest, FoldsIntToCharUnsigned)
{
    DomFixture fx("CastI2C", "run", "()I", 1, 0, [](Body& b) {
        b.op(Op::ICONST_M1).op(Op::I2C).op(Op::IRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LDC);
    EXPECT_EQ(ops[1], Op::IRETURN);
    const jbc::ir::Insn* folded = insnAt(fx, 0);
    ASSERT_NE(folded, nullptr);
    ca::i32 value = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::intValueOf(*fx.session, *folded, value));
    EXPECT_EQ(value, 65535);
}

TEST(MjTypeCastSimplifierTest, FoldsIntToShort)
{
    DomFixture fx("CastI2S", "run", "()I", 1, 0, [](Body& b) {
        b.ldcInt(0x12345).op(Op::I2S).op(Op::IRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::SIPUSH);
    const jbc::ir::Insn* folded = insnAt(fx, 0);
    ASSERT_NE(folded, nullptr);
    EXPECT_EQ(folded->a, 0x2345);
}

TEST(MjTypeCastSimplifierTest, FoldsIntToLong)
{
    DomFixture fx("CastI2L", "run", "()J", 2, 0, [](Body& b) {
        b.op(Op::ICONST_1).op(Op::I2L).op(Op::LRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LCONST_1);
    EXPECT_EQ(ops[1], Op::LRETURN);
}

TEST(MjTypeCastSimplifierTest, FoldsFloatToIntTruncating)
{
    DomFixture fx("CastF2I", "run", "()I", 1, 0, [](Body& b) {
        b.ldcFloat(1.5f).op(Op::F2I).op(Op::IRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_1);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjTypeCastSimplifierTest, FoldsFloatNaNToZero)
{
    DomFixture fx("CastF2INaN", "run", "()I", 1, 0, [](Body& b) {
        b.ldcFloat(std::numeric_limits<float>::quiet_NaN()).op(Op::F2I).op(Op::IRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_0);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjTypeCastSimplifierTest, FoldsDoubleOverflowToLongMax)
{
    DomFixture fx("CastD2LMax", "run", "()J", 2, 0, [](Body& b) {
        b.ldcDouble(1e20).op(Op::D2L).op(Op::LRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LDC2_W);
    EXPECT_EQ(ops[1], Op::LRETURN);
    const jbc::ir::Insn* folded = insnAt(fx, 0);
    ASSERT_NE(folded, nullptr);
    ca::i64 value = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::longValueOf(*fx.session, *folded, value));
    EXPECT_EQ(value, std::numeric_limits<ca::i64>::max());
}

TEST(MjTypeCastSimplifierTest, FoldsIntBitsToFloat)
{
    DomFixture fx("CastIntBits", "run", "()F", 1, 0, [](Body& b) {
        b.ldcInt(0x3f800000)
            .invokeStatic("java/lang/Float", "intBitsToFloat", "(I)F")
            .op(Op::FRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::FCONST_1);
    EXPECT_EQ(ops[1], Op::FRETURN);
}

TEST(MjTypeCastSimplifierTest, FoldsParseInt)
{
    DomFixture fx("CastParseInt", "run", "()I", 1, 0, [](Body& b) {
        b.ldcString("123")
            .invokeStatic("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I")
            .op(Op::IRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::BIPUSH);
    EXPECT_EQ(ops[1], Op::IRETURN);
    const jbc::ir::Insn* folded = insnAt(fx, 0);
    ASSERT_NE(folded, nullptr);
    EXPECT_EQ(folded->a, 123);
}

TEST(MjTypeCastSimplifierTest, LeavesInvalidParseIntUntouched)
{
    DomFixture fx("CastParseIntInvalid", "run", "()I", 1, 0, [](Body& b) {
        b.ldcString("12a")
            .invokeStatic("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I")
            .op(Op::IRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::LDC);
    EXPECT_EQ(ops[1], Op::INVOKESTATIC);
    EXPECT_EQ(ops[2], Op::IRETURN);
}

TEST(MjTypeCastSimplifierTest, FoldsParseUnsignedLongHex)
{
    DomFixture fx("CastParseUnsigned", "run", "()J", 3, 0, [](Body& b) {
        b.ldcString("ffffffffffffffff")
            .bipush(16)
            .invokeStatic("java/lang/Long", "parseUnsignedLong",
                          "(Ljava/lang/String;I)J")
            .op(Op::LRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::LDC2_W);
    EXPECT_EQ(ops[1], Op::LRETURN);
    const jbc::ir::Insn* folded = insnAt(fx, 0);
    ASSERT_NE(folded, nullptr);
    ca::i64 value = 0;
    ASSERT_TRUE(mjbase::pass::method_rewrite_util::longValueOf(*fx.session, *folded, value));
    EXPECT_EQ(value, -1);
}

TEST(MjTypeCastSimplifierTest, FoldsDoubleNaNToIntZero)
{
    DomFixture fx("CastD2INaN", "run", "()I", 2, 0, [](Body& b) {
        b.ldcDouble(std::numeric_limits<double>::quiet_NaN()).op(Op::D2I).op(Op::IRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], Op::ICONST_0);
    EXPECT_EQ(ops[1], Op::IRETURN);
}

TEST(MjTypeCastSimplifierTest, LeavesCastOnLocalUntouched)
{
    DomFixture fx("CastLocalNoFold", "run", "()J", 2, 1, [](Body& b) {
        b.iload(0).op(Op::I2L).op(Op::LRETURN);
    });

    fx.run(TypeCastSimplifier{});

    const auto ops = opsOf(fx);
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], Op::ILOAD);
    EXPECT_EQ(ops[1], Op::I2L);
    EXPECT_EQ(ops[2], Op::LRETURN);
}

}   // namespace mjbase::test
