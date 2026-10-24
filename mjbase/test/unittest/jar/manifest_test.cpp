/// JAR-4 回归：Manifest::toBytes 折行不得切断 UTF-8 多字节字符。
///
/// 4 用例逐一对应，断言等强度）；
/// 适配点仅命名空间与 Utf8String 字节级构造（from_data_unchecked 直通）。
#include "mjbase/jar/manifest.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

ca::str::Utf8StringRef S(const char* s) { return ca::str::Utf8StringRef::from_cstr(s); }

/// 字节串 → Utf8String（原始字节直通，不校验 UTF-8 合法性——与实现同口径）。
ca::str::Utf8String B(const std::string& s)
{
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(s.data()), s.size());
}

/// 逐行校验输出字节：每行（含续行前导空格）≤ 72 字节，且总行数 ≥ minLines。
void expectWrapped(const std::vector<ca::u8>& bytes, int minLines)
{
    const std::string all(bytes.begin(), bytes.end());
    std::size_t pos   = 0;
    int         lineNo = 0;
    while (pos < all.size()) {
        const auto crlf = all.find("\r\n", pos);
        ASSERT_NE(crlf, std::string::npos);
        const auto line = all.substr(pos, crlf - pos);
        EXPECT_LE(line.size(), 72u) << "line " << lineNo << " exceeds 72 bytes";
        ++lineNo;
        pos = crlf + 2;
    }
    EXPECT_GE(lineNo, minLines);
}

/// 72 字节内的 ASCII 长行：不折行，roundtrip 一致。
TEST(MjManifestToBytesTest, ShortAsciiRoundtrip)
{
    mjbase::jar::Manifest m;
    m.setMainAttribute(S("Manifest-Version"), S("1.0"));
    m.setMainAttribute(S("Implementation-Title"), S("demo"));

    const auto back = mjbase::jar::Manifest::fromBytes(m.toBytes());
    EXPECT_EQ(back.getMainAttribute(S("Manifest-Version")), B("1.0"));
    EXPECT_EQ(back.getMainAttribute(S("Implementation-Title")), B("demo"));
}

/// 多字节字符恰好跨越 72 字节截断点：折行必须回退到字符起始字节，
/// roundtrip 后值逐字节一致（会把「中」切成两半导致解析失败/值错）。
TEST(MjManifestToBytesTest, WrapDoesNotSplitUtf8MultibyteChar)
{
    // 值 = 70 个 ASCII 'x' + '中'（3 字节）。"Key: " 前缀 5 字节 + 70 = 75 > 72，
    // 首行截断点 72 正好落在 '中' 的第 2 个字节上。
    std::string value(70, 'x');
    value += "\xE4\xB8\xAD";   // U+4E2D '中' UTF-8

    mjbase::jar::Manifest m;
    m.setMainAttribute(S("Key"), B(value));

    const auto bytes = m.toBytes();
    // 首行 75 字节超窗 → 必须折成多行。
    expectWrapped(bytes, 2);

    const auto back = mjbase::jar::Manifest::fromBytes(bytes);
    EXPECT_EQ(back.getMainAttribute(S("Key")), B(value));
}

/// 续行截断点也落在多字节字符中间：多段折行后 roundtrip 一致。
TEST(MjManifestToBytesTest, WrapContinuationDoesNotSplitUtf8MultibyteChar)
{
    // 值足够长触发多段续行（> 72 + 71*2），且每段边界附近放多字节字符。
    std::string value(70, 'x');
    value += "\xE4\xB8\xAD";                    // 首行截断点附近
    value += std::string(68, 'y');
    value += "\xF0\x9F\x98\x80";                // 第二段截断点附近（4 字节 emoji）
    value += std::string(70, 'z');

    mjbase::jar::Manifest m;
    m.setMainAttribute(S("Key"), B(value));

    const auto back = mjbase::jar::Manifest::fromBytes(m.toBytes());
    EXPECT_EQ(back.getMainAttribute(S("Key")), B(value));
}

/// 回归：值中含长串连续 UTF-8 续字节（0x80-0xBF，恶意/畸形输入）。
/// charStart 回退越过折行窗口起点后 end==pos，折行循环永不前进 → 死循环。
/// 修复后每行至少前进 1 字节，toBytes 终止且 roundtrip 逐字节一致。
/// 注：run 前置一个 ASCII 字符，截断点回退停在该字符上，roundtrip 保持逐字节。
TEST(MjManifestToBytesTest, WrapHandlesRunOfContinuationBytesWithoutHanging)
{
    std::string value = "x";
    for (int i = 0; i < 200; ++i) {
        value += static_cast<char>(0x80 + (i % 0x40));   // 0x80..0xBF 续字节区间
    }
    value += "tail";

    mjbase::jar::Manifest m;
    m.setMainAttribute(S("Key"), B(value));

    const auto bytes = m.toBytes();   // 修复前：在此死循环

    expectWrapped(bytes, 1);

    const auto back = mjbase::jar::Manifest::fromBytes(bytes);
    EXPECT_EQ(back.getMainAttribute(S("Key")), B(value));
}

}   // namespace
