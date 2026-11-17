#include <gtest/gtest.h>

#include "mjbase/ast/control_flow.hpp"

namespace {

using namespace mjbase::ast;

TEST(MjAstControlFlowTest, HandlerEdgesParticipateInReachabilityAndDominance)
{
    MethodBody body;
    body.blocks.resize(6);
    body.blocks[0].terminator = Branch{Comparison::Equal, {0}, {1}, {1}, {2}};
    body.blocks[1].terminator = Jump{{3}};
    body.blocks[1].handlers.push_back(ExceptionHandler{{5}, std::nullopt});
    body.blocks[2].terminator = Jump{{3}};
    body.blocks[3].terminator = Return{};
    body.blocks[4].terminator = Jump{{3}};   // Unreachable predecessor of the join.
    body.blocks[5].terminator = Jump{{3}};

    auto built = ControlFlowGraph::build(body);
    ASSERT_TRUE(built.is_ok());
    const auto graph = std::move(built).unwrap();
    const auto& targets = graph.successors({1});
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].index, 3u);
    EXPECT_EQ(targets[1].index, 5u);

    const CfgTraversal traversal(graph);
    EXPECT_TRUE(traversal.isReachable({5}));
    EXPECT_FALSE(traversal.isReachable({4}));
    EXPECT_EQ(traversal.reversePostorder().size(), 5u);

    const DominatorTree dominators(graph);
    ASSERT_TRUE(dominators.idom({3}));
    ASSERT_TRUE(dominators.idom({5}));
    EXPECT_EQ(dominators.idom({3})->index, 0u);
    EXPECT_EQ(dominators.idom({5})->index, 1u);
    EXPECT_TRUE(dominators.dominates({1}, {5}));
    EXPECT_FALSE(dominators.dominates({1}, {3}));
    EXPECT_FALSE(dominators.idom({4}));
    ASSERT_EQ(dominators.frontier({5}).size(), 1u);
    EXPECT_EQ(dominators.frontier({5}).front().index, 3u);
}

TEST(MjAstControlFlowTest, LoopHeaderHasAFrontierAndParallelEdgesAreDeduplicated)
{
    MethodBody body;
    body.blocks.resize(4);
    body.blocks[0].terminator = Jump{{1}};
    body.blocks[1].terminator = Branch{Comparison::Equal, {0}, {1}, {3}, {2}};
    body.blocks[2].terminator = Branch{Comparison::Equal, {0}, {1}, {1}, {1}};
    body.blocks[3].terminator = Return{};

    auto built = ControlFlowGraph::build(body);
    ASSERT_TRUE(built.is_ok());
    const auto graph = std::move(built).unwrap();
    ASSERT_EQ(graph.successors({2}).size(), 1u);
    EXPECT_EQ(graph.successors({2}).front().index, 1u);

    const DominatorTree dominators(graph);
    ASSERT_TRUE(dominators.idom({1}));
    ASSERT_TRUE(dominators.idom({2}));
    EXPECT_EQ(dominators.idom({1})->index, 0u);
    EXPECT_EQ(dominators.idom({2})->index, 1u);
    EXPECT_TRUE(dominators.dominates({1}, {3}));
    ASSERT_EQ(dominators.frontier({1}).size(), 1u);
    EXPECT_EQ(dominators.frontier({1}).front().index, 1u);
    ASSERT_EQ(dominators.frontier({2}).size(), 1u);
    EXPECT_EQ(dominators.frontier({2}).front().index, 1u);
}

TEST(MjAstControlFlowTest, RejectsOutOfRangeNormalAndHandlerTargets)
{
    MethodBody body;
    body.blocks.resize(1);
    body.blocks[0].terminator = Jump{{1}};
    auto badJump = ControlFlowGraph::build(body);
    ASSERT_TRUE(badJump.is_err());
    EXPECT_EQ(badJump.unwrap_err(), ControlFlowError::InvalidTarget);

    body.blocks[0].terminator = Return{};
    body.blocks[0].handlers.push_back(ExceptionHandler{{1}, std::nullopt});
    auto badHandler = ControlFlowGraph::build(body);
    ASSERT_TRUE(badHandler.is_err());
    EXPECT_EQ(badHandler.unwrap_err(), ControlFlowError::InvalidTarget);
}

TEST(MjAstControlFlowTest, EntryBackedgeHasAMergeWithMethodEntry)
{
    MethodBody body;
    body.blocks.resize(2);
    body.blocks[0].terminator = Jump{{1}};
    body.blocks[1].terminator = Jump{{0}};

    auto built = ControlFlowGraph::build(body);
    ASSERT_TRUE(built.is_ok());
    const DominatorTree dominators(std::move(built).unwrap());
    ASSERT_EQ(dominators.frontier({0}).size(), 1u);
    EXPECT_EQ(dominators.frontier({0}).front().index, 0u);
    ASSERT_EQ(dominators.frontier({1}).size(), 1u);
    EXPECT_EQ(dominators.frontier({1}).front().index, 0u);
}

}   // namespace
