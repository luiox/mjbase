#include <gtest/gtest.h>

#include "libca/str/utf8_string.hpp"
#include "mjbase/mapping/identifier_generator.hpp"

using namespace mjbase::mapping;
using S = ca::str::Utf8StringRef;

// ── IdentifierGenerator ─────────────────────────────────────────
TEST(MjMappingIdentifierGeneratorTest, alphabetMode)
{
    IdentifierGenerator gen(DictionaryMode::Alphabet);
    gen.generateDictionary(10);
    EXPECT_GT(gen.size(), 0);
    auto name = gen.getName();
    EXPECT_GT(name.byte_length(), 0);
}

TEST(MjMappingIdentifierGeneratorTest, numberMode)
{
    IdentifierGenerator gen(DictionaryMode::Number);
    gen.generateDictionary(5);
    EXPECT_GT(gen.size(), 0);
    EXPECT_GT(gen.getName().byte_length(), 0);
}

TEST(MjMappingIdentifierGeneratorTest, il1Mode)
{
    IdentifierGenerator gen(DictionaryMode::IL1);
    gen.generateDictionary(5);
    EXPECT_GT(gen.size(), 0);
}

TEST(MjMappingIdentifierGeneratorTest, getRandomStringLength)
{
    auto s = IdentifierGenerator::getRandomString(8);
    EXPECT_EQ(s.byte_length(), 8);
}

TEST(MjMappingIdentifierGeneratorTest, getRandomName)
{
    auto name = IdentifierGenerator::getRandomName();
    EXPECT_GT(name.byte_length(), 0);
}

TEST(MjMappingIdentifierGeneratorTest, stringToDictionaryMode)
{
    auto mode = stringToDictionaryMode(S::from_cstr("alphabet"));
    EXPECT_EQ(mode, DictionaryMode::Alphabet);
}
