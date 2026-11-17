#include <gtest/gtest.h>

#include <limits>

#include "mjbase/ast/constant_facts.hpp"

namespace {

using namespace mjbase::ast;

void expectInt(const std::optional<KnownConstant>& fact, ca::i32 expected)
{
    ASSERT_TRUE(fact.has_value());
    const auto* integer = std::get_if<ca::i32>(&*fact);
    ASSERT_NE(integer, nullptr);
    EXPECT_EQ(*integer, expected);
}

TEST(MjAstConstantFactsTest, SnapshotsValuesAcrossWritesAndOpaqueOperations)
{
    const LocalId x{0};
    const Block block{
        {Let{ReadLocal{x, LocalKind::Int}},
         Let{Literal{ca::i32{5}}},
         Assign{x, ValueId{1}},
         Let{ReadLocal{x, LocalKind::Int}},
         CallStatement{Call{{}, Dispatch::Static, std::nullopt, {}}},
         Let{ReadLocal{x, LocalKind::Int}},
         Let{Call{{}, Dispatch::Static, std::nullopt, {}}},
         Let{FieldRead{{}, std::nullopt}},
         Assign{x, ValueId{6}},
         Let{ReadLocal{x, LocalKind::Int}},
         Let{IntBinary{IntOp::Add, ValueId{3}, ValueId{5}}}},
        Return{ValueId{10}}
    };

    const auto facts = analyzeBlockConstants(block);
    ASSERT_EQ(facts.size(), block.statements.size());
    EXPECT_FALSE(facts[0]);
    EXPECT_FALSE(facts[2]);
    expectInt(facts[3], 5);
    EXPECT_FALSE(facts[4]);
    expectInt(facts[5], 5);
    EXPECT_FALSE(facts[6]);
    EXPECT_FALSE(facts[7]);
    EXPECT_FALSE(facts[8]);
    EXPECT_FALSE(facts[9]);
    expectInt(facts[10], 10);
}

TEST(MjAstConstantFactsTest, RespectsTypedReadsAndWideClobbers)
{
    const LocalId low{0};
    const LocalId high{1};
    const Block block{
        {Let{Literal{Bits32{0}}},
         Assign{low, ValueId{0}},
         Let{ReadLocal{low}},
         Let{ReadLocal{low, LocalKind::Int}},
         Let{ReadLocal{low, LocalKind::Reference}},
         Let{ReadLocal{low, LocalKind::Float}},
         Assign{high, ValueId{3}},
         Let{ReadLocal{high, LocalKind::Reference}},
         Let{ReadLocal{high, LocalKind::Int}},
         Let{ReadLocal{LocalId{2}, LocalKind::Long}},
         Assign{low, ValueId{9}, high},
         Let{ReadLocal{high, LocalKind::Int}},
         Let{ReadLocal{low, LocalKind::Int}}},
        Return{ValueId{8}}
    };

    const auto facts = analyzeBlockConstants(block);
    ASSERT_TRUE(facts[2]);
    const auto* bits = std::get_if<Bits32>(&*facts[2]);
    ASSERT_NE(bits, nullptr);
    EXPECT_EQ(bits->bits, 0u);
    expectInt(facts[3], 0);
    ASSERT_TRUE(facts[4]);
    EXPECT_TRUE(std::holds_alternative<Null>(*facts[4]));
    EXPECT_FALSE(facts[5]);
    EXPECT_FALSE(facts[7]);
    expectInt(facts[8], 0);
    EXPECT_FALSE(facts[11]);
    EXPECT_FALSE(facts[12]);
}

TEST(MjAstConstantFactsTest, UsesJavaIntegerWrapShiftAndDivisionSemantics)
{
    const auto minimum = (std::numeric_limits<ca::i32>::min)();
    const auto maximum = (std::numeric_limits<ca::i32>::max)();
    const Block block{
        {Let{Literal{Bits32{0x80000000u}}},
         Let{Literal{ca::i32{-1}}},
         Let{Literal{ca::i32{0}}},
         Let{Literal{ca::i32{33}}},
         Let{Literal{maximum}},
         Let{Literal{ca::i32{1}}},
         Let{IntBinary{IntOp::Divide, ValueId{0}, ValueId{1}}},
         Let{IntBinary{IntOp::Remainder, ValueId{0}, ValueId{1}}},
         Let{IntBinary{IntOp::Divide, ValueId{0}, ValueId{2}}},
         Let{IntBinary{IntOp::Remainder, ValueId{0}, ValueId{2}}},
         Let{IntBinary{IntOp::UnsignedShiftRight, ValueId{1}, ValueId{3}}},
         Let{IntBinary{IntOp::ShiftRight, ValueId{0}, ValueId{1}}},
         Let{IntBinary{IntOp::Add, ValueId{4}, ValueId{5}}}},
        Return{ValueId{12}}
    };

    const auto facts = analyzeBlockConstants(block);
    expectInt(facts[6], minimum);
    expectInt(facts[7], 0);
    EXPECT_FALSE(facts[8]);
    EXPECT_FALSE(facts[9]);
    expectInt(facts[10], maximum);
    expectInt(facts[11], -1);
    expectInt(facts[12], minimum);
}

}   // namespace
