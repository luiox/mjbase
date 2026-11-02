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

TEST(MjEntryShapeTest, RegularFlagMatchesB1MinimalPredicatePlusB4Extensions)
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
    // B4 补充（空格名混淆样本实测的空洞形态）：超长与段首尾空白。
    EXPECT_FALSE(isRegularEntryPath(std::string(256, 'a')));
    EXPECT_FALSE(isRegularEntryPath(std::string(300, ' ') + ".class"));
    EXPECT_FALSE(isRegularEntryPath(" /lead-space.class"));
    EXPECT_FALSE(isRegularEntryPath("a/trail-space.class "));
    EXPECT_FALSE(isRegularEntryPath("a/ /b.class"));
    // 合法形态全放行（段内空格是普通文件名形态，不判）。
    EXPECT_TRUE(isRegularEntryPath("a"));
    EXPECT_TRUE(isRegularEntryPath("a/b/c.class"));
    EXPECT_TRUE(isRegularEntryPath("a/..b"));   // 含点但不是点段
    EXPECT_TRUE(isRegularEntryPath(".../a"));   // 同上
    EXPECT_TRUE(isRegularEntryPath("my file.class"));
    EXPECT_TRUE(isRegularEntryPath("res/my notes.txt"));
    EXPECT_TRUE(isRegularEntryPath(std::string(255, 'a')));
}

TEST(MjEntryShapeTest, OverlongFacetBySegmentAndPathThresholds)
{
    using mjbase::project::kOverlongPathBytes;
    using mjbase::project::kOverlongSegmentBytes;
    // 单段超限：255 放行、256 拨 Overlong（实测样本的全空格段 21845 同形态）。
    EXPECT_EQ(EntryPathShape::Regular, shapeOf(std::string(255, 'a')));
    EXPECT_EQ(EntryPathShape::Overlong, shapeOf(std::string(kOverlongSegmentBytes + 1, 'a')));
    EXPECT_EQ(EntryPathShape::Overlong, shapeOf(std::string(300, ' ') + ".class"));
    EXPECT_EQ(EntryPathShape::Overlong, shapeOf(std::string(21845, ' ')));
    // 全路径超限而各段不超限：Linux PATH_MAX 口径（末段不带尾随斜杠）。
    std::string longPath;
    while (longPath.size() <= kOverlongPathBytes) longPath += "ab/";
    longPath += "end.class";
    EXPECT_EQ(EntryPathShape::Overlong, shapeOf(longPath));
    // 多段深路径、总量不超限：仍 Regular。
    EXPECT_EQ(EntryPathShape::Regular, shapeOf("a/b/c/d/e/f/g/h/i/j.class"));
}

TEST(MjEntryShapeTest, WhitespaceEdgeFacetLeadingTrailingOnly)
{
    // 段首/段尾空格判 WhitespaceEdge；段中空格是普通文件名形态。
    EXPECT_EQ(EntryPathShape::WhitespaceEdge, shapeOf(" leading.class"));
    EXPECT_EQ(EntryPathShape::WhitespaceEdge, shapeOf("a/trailing.class "));
    EXPECT_EQ(EntryPathShape::WhitespaceEdge, shapeOf("a/ /b.class"));
    EXPECT_EQ(EntryPathShape::WhitespaceEdge, shapeOf(" "));
    EXPECT_EQ(EntryPathShape::Regular, shapeOf("my file.class"));
    EXPECT_EQ(EntryPathShape::Regular, shapeOf("a/b c/d.class"));
    EXPECT_EQ(EntryPathShape::Regular, shapeOf("a/trail .class"));   // 扩展名前的段内空格
}

TEST(MjEntryShapeTest, NewFacetsYieldToHigherPriorityOnOverlap)
{
    // 结构异常压过超长/空白：优先序 TrailingSlash > Overlong > WhitespaceEdge。
    EXPECT_EQ(EntryPathShape::DotSegment, shapeOf("a/../" + std::string(300, 'b')));
    EXPECT_EQ(EntryPathShape::TrailingSlash, shapeOf("a/" + std::string(300, 'b') + "/"));
    // 超长压过空白：全空格超长段取 Overlong 而非 WhitespaceEdge。
    EXPECT_EQ(EntryPathShape::Overlong, shapeOf(std::string(300, ' ') + ".class"));
    EXPECT_EQ(EntryPathShape::WhitespaceEdge, shapeOf(" "));
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
    EXPECT_EQ("overlong", entryPathShapeName(EntryPathShape::Overlong));
    EXPECT_EQ("whitespace_edge", entryPathShapeName(EntryPathShape::WhitespaceEdge));
}
