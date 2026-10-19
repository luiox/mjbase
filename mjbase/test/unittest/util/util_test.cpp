/// @file        util_test.cpp
/// @brief       内部名/描述符视图工具测试。
#include <gtest/gtest.h>

#include <string_view>

#include "mjbase/util.hpp"

using mjbase::util::descriptorToInternalName;
using mjbase::util::enclosingClassOf;
using mjbase::util::isLibraryClass;
using mjbase::util::packageNameOf;
using mjbase::util::simpleNameOf;

TEST(MjUtilTest, PackageAndSimpleName)
{
    EXPECT_EQ(packageNameOf(std::string_view{"a/b/C"}), "a/b");
    EXPECT_EQ(simpleNameOf(std::string_view{"a/b/C"}), "C");
    EXPECT_TRUE(packageNameOf(std::string_view{"C"}).empty());
    EXPECT_EQ(simpleNameOf(std::string_view{"C"}), "C");
}

TEST(MjUtilTest, EnclosingClass)
{
    EXPECT_EQ(enclosingClassOf(std::string_view{"a/B$Inner"}), "a/B");
    EXPECT_EQ(enclosingClassOf(std::string_view{"a/B$Inner$Deep"}), "a/B$Inner");
    EXPECT_TRUE(enclosingClassOf(std::string_view{"a/B"}).empty());
    // '$' 落在包名段：按无外围类处理（不产 "a" 这种半截类名）。
    EXPECT_TRUE(enclosingClassOf(std::string_view{"a$b/C"}).empty());
}

TEST(MjUtilTest, DescriptorToInternalName)
{
    EXPECT_EQ(descriptorToInternalName(std::string_view{"La/b/C;"}), "a/b/C");
    EXPECT_TRUE(descriptorToInternalName(std::string_view{"I"}).empty());
    EXPECT_TRUE(descriptorToInternalName(std::string_view{"La/b/C"}).empty());
    EXPECT_TRUE(descriptorToInternalName(std::string_view{"[La/b/C;"}).empty());
}

TEST(MjUtilTest, LibraryClassPrefixes)
{
    EXPECT_TRUE(isLibraryClass(std::string_view{"java/lang/String"}));
    EXPECT_TRUE(isLibraryClass(std::string_view{"com/sun/proxy/X"}));
    EXPECT_TRUE(isLibraryClass(std::string_view{"jdk/internal/Y"}));
    EXPECT_FALSE(isLibraryClass(std::string_view{"cc/ares/Main"}));
    EXPECT_FALSE(isLibraryClass(std::string_view{"javaxx/A"}));
}
