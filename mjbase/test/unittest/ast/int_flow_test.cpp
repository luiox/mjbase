#include <gtest/gtest.h>

#include <utility>

#include "mjbase/ast/int_flow.hpp"

namespace {

using namespace mjbase::ast;

TEST(MjAstIntFlowTest, MergesLocalsAndStackArgumentsFromAllNormalEdges)
{
    const LocalId local{0};
    MethodBody body;
    body.localCount = 1;
    body.blocks = {
        {{Let{Literal{ca::i32{1}}}, Let{Literal{ca::i32{0}}}},
         Branch{Comparison::NotEqual, ValueId{0}, ValueId{1}, BlockId{1}, BlockId{2},
                LocalKind::Int}},
        {{Let{Literal{ca::i32{7}}}, Assign{local, ValueId{0}}, Let{Literal{ca::i32{11}}}},
         Jump{BlockId{3}}},
        {{Let{Literal{ca::i32{7}}}, Assign{local, ValueId{0}}, Let{Literal{ca::i32{11}}}},
         Jump{BlockId{3}}},
        {{Let{ReadLocal{local, LocalKind::Int}}, Let{BlockParameter{0}},
          Let{IntBinary{IntOp::Add, ValueId{0}, ValueId{1}}}},
         Return{ValueId{2}}},
        {{Let{Literal{ca::i32{42}}}}, Return{ValueId{0}}}
    };
    body.blocks[1].outgoingArguments = {ValueId{2}};
    body.blocks[2].outgoingArguments = {ValueId{2}};
    body.blocks[3].parameters = {LocalKind::Int};

    const auto agree = analyzeIntFlow(body);
    ASSERT_TRUE(agree.has_value());
    EXPECT_EQ(agree->branches[0], true);
    EXPECT_EQ(agree->values[3][0], 7);
    EXPECT_EQ(agree->values[3][1], 11);
    EXPECT_EQ(agree->values[3][2], 18);
    EXPECT_FALSE(agree->values[4][0]);

    body.blocks[2].statements[0] = Let{Literal{ca::i32{8}}};
    body.blocks[2].statements[2] = Let{Literal{ca::i32{12}}};
    const auto disagree = analyzeIntFlow(body);
    ASSERT_TRUE(disagree.has_value());
    EXPECT_EQ(disagree->branches[0], true);
    EXPECT_FALSE(disagree->values[3][0]);
    EXPECT_FALSE(disagree->values[3][1]);
    EXPECT_FALSE(disagree->values[3][2]);
}

TEST(MjAstIntFlowTest, LoopRetainsOnlyInvariantLocalsAtTheFixedPoint)
{
    const LocalId counter{0};
    const LocalId invariant{1};
    MethodBody body;
    body.localCount = 2;
    body.blocks = {
        {{Let{Literal{ca::i32{0}}}, Assign{counter, ValueId{0}},
          Let{Literal{ca::i32{7}}}, Assign{invariant, ValueId{2}}},
         Jump{BlockId{1}}},
        {{Let{ReadLocal{counter, LocalKind::Int}}, Let{Literal{ca::i32{3}}}},
         Branch{Comparison::Less, ValueId{0}, ValueId{1}, BlockId{2}, BlockId{3},
                LocalKind::Int}},
        {{Let{ReadLocal{counter, LocalKind::Int}}, Let{Literal{ca::i32{1}}},
          Let{IntBinary{IntOp::Add, ValueId{0}, ValueId{1}}}, Assign{counter, ValueId{2}}},
         Jump{BlockId{1}}},
        {{Let{ReadLocal{counter, LocalKind::Int}}, Let{ReadLocal{invariant, LocalKind::Int}}},
         Return{ValueId{1}}}
    };

    const auto flow = analyzeIntFlow(body);
    ASSERT_TRUE(flow.has_value());
    EXPECT_FALSE(flow->values[1][0]);
    EXPECT_FALSE(flow->branches[1]);
    EXPECT_FALSE(flow->values[3][0]);
    EXPECT_EQ(flow->values[3][1], 7);
}

TEST(MjAstIntFlowTest, WideWriteKillsBothLocalFactsAcrossTheEdge)
{
    const LocalId low{0};
    const LocalId high{1};
    MethodBody body;
    body.localCount = 2;
    body.blocks = {
        {{Let{Literal{ca::i32{9}}}, Assign{low, ValueId{0}}, Assign{high, ValueId{0}}},
         Jump{BlockId{1}}},
        {{Let{ReadLocal{low, LocalKind::Int}}, Let{ReadLocal{high, LocalKind::Int}},
          Let{Literal{ca::i32{0}}}, Assign{low, ValueId{2}, high}},
         Jump{BlockId{2}}},
        {{Let{ReadLocal{low, LocalKind::Int}}, Let{ReadLocal{high, LocalKind::Int}}},
         Return{ValueId{0}}}
    };

    const auto flow = analyzeIntFlow(body);
    ASSERT_TRUE(flow.has_value());
    EXPECT_EQ(flow->values[1][0], 9);
    EXPECT_EQ(flow->values[1][1], 9);
    EXPECT_FALSE(flow->values[2][0]);
    EXPECT_FALSE(flow->values[2][1]);
}

TEST(MjAstIntFlowTest, InterpretsRawDexBitsAtIntegerUses)
{
    MethodBody body;
    body.blocks = {
        {{Let{Literal{Bits32{0xffffffffu}}}, Let{Literal{Bits32{0}}}},
         Branch{Comparison::Less, ValueId{0}, ValueId{1}, BlockId{1}, BlockId{1}, LocalKind::Int}},
        {{}, Return{}}
    };

    const auto flow = analyzeIntFlow(body);
    ASSERT_TRUE(flow.has_value());
    EXPECT_FALSE(flow->values[0][0]);
    EXPECT_FALSE(flow->values[0][1]);
    EXPECT_EQ(flow->branches[0], true);

    std::get<Branch>(body.blocks[0].terminator).kind = LocalKind::Unknown;
    const auto untyped = analyzeIntFlow(body);
    ASSERT_TRUE(untyped.has_value());
    EXPECT_FALSE(untyped->branches[0]);
}

TEST(MjAstIntFlowTest, MergesMatchingRawAndIntegerValuesBeforeTypedUses)
{
    const LocalId local{0};
    MethodBody body;
    body.localCount = 2;
    body.blocks = {
        {{Let{ReadLocal{LocalId{1}, LocalKind::Int}}, Let{Literal{ca::i32{0}}}},
         Branch{Comparison::Equal, ValueId{0}, ValueId{1}, BlockId{1}, BlockId{2},
                LocalKind::Int}},
        {{Let{Literal{Bits32{0xffffffffu}}}, Assign{local, ValueId{0}}}, Jump{BlockId{3}}},
        {{Let{Literal{ca::i32{-1}}}, Assign{local, ValueId{0}}}, Jump{BlockId{3}}},
        {{Let{ReadLocal{local}}, Let{ReadLocal{local, LocalKind::Int}}, Let{BlockParameter{0}},
          Let{Literal{ca::i32{1}}}, Let{IntBinary{IntOp::Add, ValueId{1}, ValueId{3}}},
          Let{IntBinary{IntOp::Add, ValueId{2}, ValueId{3}}}},
         Return{ValueId{4}}}
    };
    body.blocks[1].outgoingArguments = {ValueId{0}};
    body.blocks[2].outgoingArguments = {ValueId{0}};
    body.blocks[3].parameters = {LocalKind::Int};

    const auto verify = [&](const MethodBody& candidate) {
        const auto flow = analyzeIntFlow(candidate);
        ASSERT_TRUE(flow.has_value());
        EXPECT_FALSE(flow->values[3][0]);
        EXPECT_EQ(flow->values[3][1], -1);
        EXPECT_EQ(flow->values[3][2], -1);
        EXPECT_EQ(flow->values[3][4], 0);
        EXPECT_EQ(flow->values[3][5], 0);
    };
    verify(body);
    std::swap(body.blocks[1], body.blocks[2]);
    verify(body);

    body.blocks[1].statements[0] = Let{Literal{ca::i32{-2}}};
    const auto disagree = analyzeIntFlow(body);
    ASSERT_TRUE(disagree.has_value());
    EXPECT_FALSE(disagree->values[3][1]);
    EXPECT_FALSE(disagree->values[3][2]);
}

TEST(MjAstIntFlowTest, RejectsInvalidTargetsArgumentsAndExceptionEdges)
{
    MethodBody body;
    body.blocks = {{{}, Jump{BlockId{1}}}, {{}, Return{}}};
    ASSERT_TRUE(analyzeIntFlow(body).has_value());

    body.blocks[1].parameters = {LocalKind::Int};
    EXPECT_FALSE(analyzeIntFlow(body));
    body.blocks[0].outgoingArguments = {ValueId{0}};
    EXPECT_FALSE(analyzeIntFlow(body));
    body.blocks[1].parameters.clear();
    body.blocks[0].outgoingArguments.clear();

    body.blocks[0].terminator = Jump{BlockId{2}};
    EXPECT_FALSE(analyzeIntFlow(body));
    body.blocks[0].terminator = Jump{BlockId{1}};
    body.blocks[0].handlers = {{BlockId{1}, std::nullopt}};
    EXPECT_FALSE(analyzeIntFlow(body));
}

}   // namespace
