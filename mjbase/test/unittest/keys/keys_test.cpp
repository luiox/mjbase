/// @file        keys_test.cpp
/// @brief       跨类身份原语测试：驻留去重、键相等、哈希、格式化。
#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

#include "mjbase/keys.hpp"

TEST(MjKeysTest, InternDeduplicatesByContent)
{
    mjbase::KeyPool pool;
    const ca::u32 a = pool.intern("java/lang/String");
    const ca::u32 b = pool.intern(std::string_view{"java/lang/String"});
    const ca::u32 c = pool.intern("java/lang/Object");
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    // 0 号槽为哨兵：真实内容从 1 起，size 计入哨兵（= 下一 id）。
    EXPECT_EQ(pool.size(), 3u);
    EXPECT_EQ(pool.str(a), "java/lang/String");
    EXPECT_EQ(pool.str(c), "java/lang/Object");
}

TEST(MjKeysTest, ZeroIdReservedAsUnset)
{
    mjbase::KeyPool pool;
    // 首个真实内容不会拿到 0；空串恒驻留为 0（与「未设置」同权）。
    EXPECT_NE(pool.intern("p/T"), 0u);
    EXPECT_EQ(pool.intern(std::string_view{}), 0u);
    EXPECT_EQ(pool.intern(""), 0u);
    EXPECT_FALSE(mjbase::ClassKey{}.valid());
    const auto cls = mjbase::classKeyOf(pool, "p/T");
    EXPECT_TRUE(cls.valid());
    EXPECT_EQ(pool.str(cls.id), "p/T");
}

TEST(MjKeysTest, InvalidIdReturnsEmptyView)
{
    mjbase::KeyPool pool;
    (void)pool.intern("x");
    EXPECT_TRUE(pool.str(0).empty());
    EXPECT_TRUE(pool.str(9999).empty());
}

TEST(MjKeysTest, ClassAndMemberKeys)
{
    mjbase::KeyPool pool;
    const mjbase::ClassKey cls = mjbase::classKeyOf(pool, "p/T");
    const mjbase::MemberKey m1 =
        mjbase::memberKeyOf(pool, cls, "m", "()V");
    const mjbase::MemberKey m2 =
        mjbase::memberKeyOf(pool, mjbase::classKeyOf(pool, "p/T"), "m", "()V");
    const mjbase::MemberKey other =
        mjbase::memberKeyOf(pool, cls, "m", "(I)V");
    EXPECT_EQ(m1, m2);
    EXPECT_NE(m1, other);

    std::string formatted;
    mjbase::formatMember(pool, m1, formatted);
    EXPECT_EQ(formatted, "p/T.m ()V");
}

TEST(MjKeysTest, MemberKeyUsableInHashContainers)
{
    mjbase::KeyPool pool;
    const auto cls = mjbase::classKeyOf(pool, "p/A");
    const auto a = mjbase::memberKeyOf(pool, cls, "f", "I");
    const auto b = mjbase::memberKeyOf(pool, cls, "g", "I");
    std::unordered_set<mjbase::MemberKey, mjbase::MemberKeyHash> set;
    set.insert(a);
    set.insert(a);
    set.insert(b);
    EXPECT_EQ(set.size(), 2u);
    EXPECT_EQ(set.count(a), 1u);
}
