/// @file        entry_shape_test.cpp
/// @brief       条目路径形态判定：facet 优先序、regular 口径与 B1 最小判定
///              逐字节一致、facet 稳定名。
#include <gtest/gtest.h>

#include <string>

#include "mjbase/project/entry_shape.hpp"

using mjbase::project::classifyEntryPathShape;
using mjbase::project::entryPathShapeName;
using mjbase::project::isRegularEntryPath;
using mjbase::project::EntryPathShape;

namespace {

EntryPathShape shapeOf(const std::string& path)
{
    return classifyEntryPathShape(path);
}

}   // namespace

TEST(MjEntryShapeTest, RegularPathsClassifyRegular)
{
    EXPECT_EQ(EntryPathShape::Regular, shapeOf("a/A.class"));
    EXPECT_EQ(EntryPathShape::Regular, shapeOf("META-INF/MANIFEST.MF"));
    EXPECT_EQ(EntryPathShape::Regular, shapeOf("com/example/pkg/Inner.class"));
    EXPECT_EQ(EntryPathShape::Regular, shapeOf("a.b.c/d-e_f.txt"));
    // 非 ASCII 资源名是合法路径形态（字节透传，不是异常 facet）。
    EXPECT_EQ(EntryPathShape::Regular, shapeOf("res/\xe4\xb8\xad\xe6\x96\x87.txt"));
    EXPECT_TRUE(isRegularEntryPath("a/A.class"));
}

TEST(MjEntryShapeTest, AnomalyFacetsClassifyByPriority)
{
    EXPECT_EQ(EntryPathShape::Empty, shapeOf(""));
    EXPECT_EQ(EntryPathShape::ControlChar, shapeOf(std::string("a/\x01" "b.class")));
    EXPECT_EQ(EntryPathShape::ControlChar, shapeOf("a/b\x7f.class"));
    EXPECT_EQ(EntryPathShape::ControlChar, shapeOf(std::string("a/\nb.class")));
    EXPECT_EQ(EntryPathShape::Backslash, shapeOf("com\\example\\A.class"));
    EXPECT_EQ(EntryPathShape::LeadingSlash, shapeOf("/a/A.class"));
    // 纯 "/" 既是前导又是尾随：优先序取 LeadingSlash。
    EXPECT_EQ(EntryPathShape::LeadingSlash, shapeOf("/"));
    EXPECT_EQ(EntryPathShape::EmptySegment, shapeOf("a//b.class"));
    EXPECT_EQ(EntryPathShape::DotSegment, shapeOf("a/./b.class"));
    EXPECT_EQ(EntryPathShape::DotSegment, shapeOf("a/../b.class"));
    EXPECT_EQ(EntryPathShape::TrailingSlash, shapeOf("pkg/dir/"));
    EXPECT_EQ(EntryPathShape::TrailingSlash, shapeOf("a/"));
}

TEST(MjEntryShapeTest, PriorityPicksHighestFacetWhenMultipleHit)
{
    // 控制字符压过反斜杠：facet 取优先序最前者，与出现位置无关。
    EXPECT_EQ(EntryPathShape::ControlChar,
              shapeOf(std::string("a\\b/\x02" "c.class")));
    EXPECT_EQ(EntryPathShape::Backslash, shapeOf("/a\\b.class"));
    // 中段空段压过尾随 '/'（"a//" 两处异常同时命中）。
    EXPECT_EQ(EntryPathShape::EmptySegment, shapeOf("a//"));
    // 点段压过尾随 '/'。
    EXPECT_EQ(EntryPathShape::DotSegment, shapeOf("a/../"));
    // 前导斜杠压过中段空段（首段为空即前导斜杠形态）。
    EXPECT_EQ(EntryPathShape::LeadingSlash, shapeOf("//a.class"));
}

TEST(MjEntryShapeTest, RegularFlagMatchesB1MinimalPredicateByteForByte)
{
    // B1 最小口径（mjt 导入桥原实现，#121 B4 上提时的逐字节对照集）：
    // 非空、无控制字符/DEL、无反斜杠、非前导或尾随 '/'、无空段、无点段。
    EXPECT_FALSE(isRegularEntryPath(""));
    EXPECT_FALSE(isRegularEntryPath("/lead"));
    EXPECT_FALSE(isRegularEntryPath("trail/"));
    EXPECT_FALSE(isRegularEntryPath("a//b"));
    EXPECT_FALSE(isRegularEntryPath("a/./b"));
    EXPECT_FALSE(isRegularEntryPath("a/../b"));
    EXPECT_FALSE(isRegularEntryPath("a\\b"));
    EXPECT_FALSE(isRegularEntryPath(std::string("a\x1f" "b")));
    EXPECT_FALSE(isRegularEntryPath("/"));
    // 合法形态全放行。
    EXPECT_TRUE(isRegularEntryPath("a"));
    EXPECT_TRUE(isRegularEntryPath("a/b/c.class"));
    EXPECT_TRUE(isRegularEntryPath("a/..b"));   // 含点但不是点段
    EXPECT_TRUE(isRegularEntryPath(".../a"));   // 同上
}

TEST(MjEntryShapeTest, FacetNamesAreStableTokens)
{
    using mjbase::project::EntryPathShape;
    EXPECT_EQ("regular", entryPathShapeName(EntryPathShape::Regular));
    EXPECT_EQ("empty", entryPathShapeName(EntryPathShape::Empty));
    EXPECT_EQ("control_char", entryPathShapeName(EntryPathShape::ControlChar));
    EXPECT_EQ("backslash", entryPathShapeName(EntryPathShape::Backslash));
    EXPECT_EQ("leading_slash", entryPathShapeName(EntryPathShape::LeadingSlash));
    EXPECT_EQ("empty_segment", entryPathShapeName(EntryPathShape::EmptySegment));
    EXPECT_EQ("dot_segment", entryPathShapeName(EntryPathShape::DotSegment));
    EXPECT_EQ("trailing_slash", entryPathShapeName(EntryPathShape::TrailingSlash));
}
