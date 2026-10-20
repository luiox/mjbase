/// @file        util_leaf_test.cpp
/// @brief       util 叶子件（Pair/Span/Triple/TimeUtil）编译冒烟 + 最小行为抽检。
///
/// 旧栈 对这批 header-only 叶子无单测；迁移验收以编译通过为主，
/// 抽检仅防静默腐烂，断言强度不作为语义契约。
#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "mjbase/util/pair.hpp"
#include "mjbase/util/span.hpp"
#include "mjbase/util/time_util.hpp"
#include "mjbase/util/triple.hpp"

using namespace mjbase::util;

TEST(MjUtilLeafTest, pairBasics)
{
    auto p = Pair<int, std::string>::of(1, "one");
    EXPECT_EQ(p.first, 1);
    EXPECT_EQ(p.second, "one");
    EXPECT_TRUE((Pair<int, int>(1, 2) == Pair<int, int>(1, 2)));
    EXPECT_FALSE((Pair<int, int>(1, 2) != Pair<int, int>(1, 2)));

    std::unordered_map<Pair<int, int>, int> m;   // std::hash 特化可用
    m[Pair<int, int>(1, 2)] = 3;
    EXPECT_EQ((m[Pair<int, int>(1, 2)]), 3);
}

TEST(MjUtilLeafTest, spanBasics)
{
    int       arr[] = {10, 20, 30};
    Span<int> s{arr, 3};
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.size, 3u);
    EXPECT_EQ(s.front(), 10);
    EXPECT_EQ(s.back(), 30);
    EXPECT_EQ(s[1], 20);
    int sum = 0;
    for (int* it = s.begin(); it != s.end(); ++it) sum += *it;
    EXPECT_EQ(sum, 60);
}

TEST(MjUtilLeafTest, tripleBasics)
{
    auto t = Triple<int, std::string, double>::of(1, "two", 3.0);
    EXPECT_EQ(t.first, 1);
    EXPECT_EQ(t.second, "two");
    EXPECT_EQ(t.third, 3.0);
    EXPECT_EQ(t.toString(), "(1,two,3.000000)");

    std::unordered_map<Triple<int, int, int>, int> m;   // std::hash 特化可用
    m[Triple<int, int, int>::of(1, 2, 3)] = 9;
    EXPECT_EQ((m[Triple<int, int, int>::of(1, 2, 3)]), 9);
}

TEST(MjUtilLeafTest, triplePoolCaches)
{
    auto a = TriplePool::of(7, 8, 9);
    auto b = TriplePool::of(7, 8, 9);
    EXPECT_EQ(a.first, 7);
    EXPECT_EQ(a.second, 8);
    EXPECT_EQ(a.third, 9);
    EXPECT_EQ(b.first, 7);
    EXPECT_EQ(b.second, 8);
    EXPECT_EQ(b.third, 9);
}

TEST(MjUtilLeafTest, timeUtilElapsed)
{
    auto start = TimeUtil::now();
    EXPECT_GE(TimeUtil::elapsedMs(start), 0);
    EXPECT_GE(TimeUtil::elapsedSec(start), 0.0);
    EXPECT_GE(TimeUtil::diffMs(start, TimeUtil::now()), 0);
}
