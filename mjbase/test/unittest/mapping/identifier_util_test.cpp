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

// ── printReadable：长 run 省略 ────────────────────────────────
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

// ── 增补平面：代理对出码 + 重组（\uXXXX 格式只表达 BMP，census 转义契约
//    要求补充平面类名全码位域无损） ─────────────────────────────────

TEST(MjMappingIdentifierUtilTest, SupplementaryPlaneEscapedAsSurrogatePair)
{
    // U+1F600（规范形 4 字节 UTF-8）→ 相邻代理对 \uD83D\uDE00（大写 hex）。
    // 预期值是转义 token 的字面文本（反斜杠 + u + 4 hex），双写反斜杠。
    EXPECT_EQ(IdentifierUtil::escapeUnicode(
                  ca::str::Utf8StringRef::from_data(
                      reinterpret_cast<const ca::u8*>("\xF0\x9F\x98\x80"), 4)),
              "\\uD83D\\uDE00");
    EXPECT_EQ(IdentifierUtil::printReadable("\xF0\x9F\x98\x80"), "\\uD83D\\uDE00");

    // U+10000（码位域下界）与 U+10FFFF（上界）。
    EXPECT_EQ(IdentifierUtil::escapeUnicode(
                  ca::str::Utf8StringRef::from_data(
                      reinterpret_cast<const ca::u8*>("\xF0\x90\x80\x80"), 4)),
              "\\uD800\\uDC00");
    EXPECT_EQ(IdentifierUtil::escapeUnicode(
                  ca::str::Utf8StringRef::from_data(
                      reinterpret_cast<const ca::u8*>("\xF4\x8F\xBF\xBF"), 4)),
              "\\uDBFF\\uDFFF");

    // BMP 与代理对混排：混排类名逐码点出列。
    EXPECT_EQ(IdentifierUtil::escapeUnicode(
                  ca::str::Utf8StringRef::from_data(
                      reinterpret_cast<const ca::u8*>("a\xF0\x9F\x98\x80" "b"), 6)),
              "a\\uD83D\\uDE00b");
}

TEST(MjMappingIdentifierUtilTest, LoneSurrogateBytesStillEscapeAsSingleToken)
{
    // CESU-8 形 lone surrogate 字节（ED A0 80 = U+D800）原样出单 token：
    // 混淆样本存活约定不变（不成对不重组）。
    EXPECT_EQ(IdentifierUtil::escapeUnicode(
                  ca::str::Utf8StringRef::from_data(
                      reinterpret_cast<const ca::u8*>("\xED\xA0\x80"), 3)),
              "\\uD800");
}

TEST(MjMappingIdentifierUtilTest, UnescapeRecombinesSurrogatePairs)
{
    // 相邻高/低代元转义重组为规范形 4 字节 UTF-8（与 classfile 读侧配对
    // 重组同约定）：escape → unescape 对增补平面逐字节闭合。输入是转义
    // token 的字面文本（双写反斜杠）。
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\uD83D\\uDE00"), "\xF0\x9F\x98\x80");
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\uD800\\uDC00"), "\xF0\x90\x80\x80");
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\uDBFF\\uDFFF"), "\xF4\x8F\xBF\xBF");

    // 不成对（高代元后跟 BMP 码元）：高代元按 lone surrogate 存活，后续原样。
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\uD800\\u0041"), "\xED\xA0\x80" "A");
    // 高代元后跟非 \u 转义内容：同上。
    EXPECT_EQ(IdentifierUtil::unescapeUnicode("\\uD800x"), "\xED\xA0\x80" "x");
}

TEST(MjMappingIdentifierUtilTest, SupplementaryPlaneRoundTrip)
{
    // escape(unescape(x)) 全码位域逐字节闭合（census 转义契约）：
    // 控制字符、BMP 高位、增补平面、lone surrogate 混排样本。
    const std::string sample = std::string("a") + '\t' + '\x01' + "\xC3\xA9" +
                               "\xF0\x9F\x98\x80" + "\xED\xA0\x80" + "z";
    const std::string escaped = IdentifierUtil::escapeUnicode(
                                    ca::str::Utf8StringRef::from_data(
                                        reinterpret_cast<const ca::u8*>(sample.data()),
                                        sample.size()))
                                    .to_std_string();
    EXPECT_EQ(IdentifierUtil::unescapeUnicode(escaped), sample);
}
