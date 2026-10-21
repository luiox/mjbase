/// @file        andersen_points_to_test.cpp
/// @brief       analysis Andersen 求解器测试：copy 传播到不动点、链式字段加载、
///              堆字段分配敏感、初次 solve 后追加约束。
///
/// 移植，断言逐字等强；仅命名空间与 include 适配）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mjbase/analysis/andersen_points_to.hpp"

using mjbase::analysis::AndersenPointsToSolver;
using NameId = AndersenPointsToSolver::NameId;

TEST(MjAnalysisAndersenTest, PropagatesCopyConstraintsToFixedPoint)
{
    AndersenPointsToSolver solver;
    const NameId a      = solver.intern("a");
    const NameId b      = solver.intern("b");
    const NameId c      = solver.intern("c");
    const NameId d      = solver.intern("d");
    solver.addAddressOf(a, solver.intern("alloc:A"));
    solver.addAddressOf(b, solver.intern("alloc:B"));
    solver.addCopy(c, a);
    solver.addCopy(c, b);
    solver.addCopy(d, c);
    solver.addCopy(c, d);
    solver.solve();

    EXPECT_EQ(solver.pointsTo("d"), (std::vector<std::string>{"alloc:A", "alloc:B"}));
}

TEST(MjAnalysisAndersenTest, ResolvesChainedFieldLoads)
{
    AndersenPointsToSolver solver;
    const NameId b      = solver.intern("b");
    const NameId middle = solver.intern("middle");
    const NameId leaf   = solver.intern("leaf");
    const NameId a      = solver.intern("a");
    const NameId c      = solver.intern("c");
    const NameId f      = solver.intern("f");
    const NameId g      = solver.intern("g");
    solver.addAddressOf(b, solver.intern("alloc:Root"));
    solver.addAddressOf(middle, solver.intern("alloc:Middle"));
    solver.addAddressOf(leaf, solver.intern("alloc:Leaf"));
    solver.addStore(b, f, middle);
    solver.addStore(middle, g, leaf);
    solver.addLoad(a, b, f);
    solver.addLoad(c, a, g);
    solver.solve();

    EXPECT_EQ(solver.pointsTo("a"), std::vector<std::string>{"alloc:Middle"});
    EXPECT_EQ(solver.pointsTo("c"), std::vector<std::string>{"alloc:Leaf"});
}

TEST(MjAnalysisAndersenTest, KeepsHeapFieldsAllocationSensitive)
{
    AndersenPointsToSolver solver;
    const NameId left        = solver.intern("left");
    const NameId right       = solver.intern("right");
    const NameId leftValue   = solver.intern("leftValue");
    const NameId rightValue  = solver.intern("rightValue");
    const NameId leftResult  = solver.intern("leftResult");
    const NameId rightResult = solver.intern("rightResult");
    const NameId value       = solver.intern("value");
    solver.addAddressOf(left, solver.intern("alloc:Left"));
    solver.addAddressOf(right, solver.intern("alloc:Right"));
    solver.addAddressOf(leftValue, solver.intern("alloc:LeftValue"));
    solver.addAddressOf(rightValue, solver.intern("alloc:RightValue"));
    solver.addStore(left, value, leftValue);
    solver.addStore(right, value, rightValue);
    solver.addLoad(leftResult, left, value);
    solver.addLoad(rightResult, right, value);
    solver.solve();

    EXPECT_EQ(solver.pointsTo("leftResult"), std::vector<std::string>{"alloc:LeftValue"});
    EXPECT_EQ(solver.pointsTo("rightResult"), std::vector<std::string>{"alloc:RightValue"});
}

TEST(MjAnalysisAndersenTest, AcceptsConstraintsAfterInitialSolve)
{
    AndersenPointsToSolver solver;
    const NameId base   = solver.intern("base");
    const NameId result = solver.intern("result");
    const NameId slot   = solver.intern("slot");
    solver.addAddressOf(base, solver.intern("alloc:Base"));
    solver.addLoad(result, base, slot);
    solver.solve();
    EXPECT_TRUE(solver.pointsTo("result").empty());

    solver.addAddressOf(solver.intern("value"), solver.intern("alloc:Late"));
    solver.addStore(base, slot, solver.intern("value"));
    solver.solve();
    EXPECT_EQ(solver.pointsTo("result"), std::vector<std::string>{"alloc:Late"});
}
