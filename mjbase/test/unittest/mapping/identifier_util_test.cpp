#include <gtest/gtest.h>

#include <string>

#include "mjbase/mapping/identifier_util.hpp"

using mjbase::mapping::IdentifierUtil;

// ── printReadable：普通标识符原样输出 ──────────────────────────
TEST(MjMappingIdentifierUtilTest, NormalNameUnchanged)
{
    EXPECT_EQ(IdentifierUtil::printReadable("testMethod"), "testMethod");
    EXPECT_EQ(IdentifierUtil::printReadable("org/example/Demo"), "org/example/Demo");
    EXPECT_EQ(IdentifierUtil::printReadable("<init>"), "<init>");
}

TEST(MjMappingIdentifierUtilTest, EmptyInput)
{
    EXPECT_EQ(IdentifierUtil::printReadable(""), "");
}

// ── printReadable：控制字符/空格/DEL/高字节转义 ───────────────
TEST(MjMappingIdentifierUtilTest, ControlCharsEscaped)
{
    std::string in;
    in.push_back('\0');
    in.push_back('\x07');
    in.push_back('\t');
    EXPECT_EQ(IdentifierUtil::printReadable(in), "\\u0000\\u0007\\u0009");
}

TEST(MjMappingIdentifierUtilTest, SpaceEscaped)
{
    EXPECT_EQ(IdentifierUtil::printReadable("a b"), "a\\u0020b");
}

TEST(MjMappingIdentifierUtilTest, DelAndHighBytesEscaped)
{
    EXPECT_EQ(IdentifierUtil::printReadable(std::string(1, '\x7F')), "\\u007F");
    EXPECT_EQ(IdentifierUtil::printReadable("\xC3\xA9"), "\\u00E9");   // U+00E9 é
}

TEST(MjMappingIdentifierUtilTest, BackslashEscaped)
{
    EXPECT_EQ(IdentifierUtil::printReadable("\\"), "\\u005C");
}

// ── printReadable：长 run 省略（issue #320 原文场景）──────────
TEST(MjMappingIdentifierUtilTest, LongSpaceRunCollapsed)
{
    std::string spaces(32767, ' ');
    EXPECT_EQ(IdentifierUtil::printReadable(spaces), "\\u0020...(省略32765个)...\\u0020");
}

TEST(MjMappingIdentifierUtilTest, ShortRunNotCollapsed)
{
    std::string spaces(7, ' ');
    std::string expected;
    for (int i = 0; i < 7; i++) expected += "\\u0020";
    EXPECT_EQ(IdentifierUtil::printReadable(spaces), expected);
}

TEST(MjMappingIdentifierUtilTest, CustomThreshold)
{
    EXPECT_EQ(IdentifierUtil::printReadable(std::string(5, ' '), 3),
              "\\u0020...(省略3个)...\\u0020");
}

TEST(MjMappingIdentifierUtilTest, MixedNormalAndObfuscated)
{
    std::string in = "foo" + std::string(20, ' ') + "bar";
    EXPECT_EQ(IdentifierUtil::printReadable(in), "foo\\u0020...(省略18个)...\\u0020bar");
}

TEST(MjMappingIdentifierUtilTest, DifferentTokensSeparate)
{
    std::string in;
    in.push_back(' ');
    in.push_back('\t');
    in.push_back(' ');
    EXPECT_EQ(IdentifierUtil::printReadable(in), "\\u0020\\u0009\\u0020");
}

TEST(MjMappingIdentifierUtilTest, LongHighByteRun)
{
    std::string in;
    for (int i = 0; i < 10; i++) in += "\xC3\xA9";
    EXPECT_EQ(IdentifierUtil::printReadable(in), "\\u00E9...(省略8个)...\\u00E9");
}

// ── unescapeUnicode：反向转换 ─────────────────────────────────
TEST(MjMappingIdentifierUtilTest, BasicUnescape)
{
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\u0020"), " ");
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("a\\u0020b"), "a b");
}

TEST(MjMappingIdentifierUtilTest, HexCaseInsensitive)
{
    // 大写/小写 hex 都吃
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\u00E9"), "\xC3\xA9");   // é
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\u00e9"), "\xC3\xA9");
}

TEST(MjMappingIdentifierUtilTest, RoundTrip)
{
    // printReadable 的输出再 unescape 应还原原文（对于无 run 压缩的短串）
    std::string orig    = "a b\té";
    std::string escaped = IdentifierUtil::printReadable(orig, 100);   // threshold 高，不压缩
    std::string back    = IdentifierUtil::unescapeUnicode(escaped);
    EXPECT_EQ(back, orig);
}

TEST(MjMappingIdentifierUtilTest, NonEscapePassthrough)
{
    // 非 \uXXXX 序列原样透传
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("hello"), "hello");
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("a\\nb"), "a\\nb");   // \n 不是 \u，原样
}

TEST(MjMappingIdentifierUtilTest, InvalidEscapePreserved)
{
    // hex 不足 4 位或非法字符：原样保留
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\u12"), "\\u12");     // 不足 4 位
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\uXYZ"), "\\uXYZ");   // 非 hex（Z）
}

TEST(MjMappingIdentifierUtilTest, BmpBoundary)
{
    // U+FFFF（BMP 上界）：UTF-8 = EF BF BF
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\uFFFF"), "\xEF\xBF\xBF");
}

TEST(MjMappingIdentifierUtilTest, LoneSurrogatePreserved)
{
    // \uXXXX 只能表达 BMP（U+0000..U+FFFF）。surrogate 范围（D800-DFFF）
    // 解码成 lone surrogate 的 UTF-8 字节，与 printReadable/escapeUnicode
    // 保留 lone surrogate（混淆样本存活）的设计一致。
    // U+D800：UTF-8 = ED A0 80
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\uD800"), "\xED\xA0\x80");
}
