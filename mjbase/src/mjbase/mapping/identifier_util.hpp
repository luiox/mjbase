#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::mapping {

struct IdentifierUtil
{
    static ca::str::Utf8StringRef alphabets();
    static ca::str::Utf8StringRef numbers();

    static const std::vector<ca::str::Utf8StringRef>&                 blankChars();
    static const std::vector<ca::str::Utf8StringRef>&                 javaKeyWords();
    static const std::unordered_map<ca::i32, ca::str::Utf8StringRef>& opcodeNameMap();

    static ca::str::Utf8String escapeUnicode(ca::str::Utf8StringRef utf8Str);
    static ca::str::Utf8String escapeCppNameString(ca::str::Utf8StringRef value);
    static ca::str::Utf8String escapeCommentString(ca::str::Utf8StringRef value);
    static ca::str::Utf8String unicodify(ca::str::Utf8StringRef value);

    static ca::str::Utf8StringRef getOpcodeString(ca::i32 opcode);

    static bool isValidJavaIdentifier(ca::str::Utf8StringRef name);
    static bool isValidJavaFullClassName(ca::str::Utf8StringRef fullName);

    static bool                isLegalJavaIdentifierChar(ca::u32 cp);
    static ca::str::Utf8String optimizeIllegalChar(ca::u32 cp);
    static ca::str::Utf8String optimizeIllegalString(ca::str::Utf8StringRef str);
    static ca::str::Utf8String optimizeIllegalDesc(ca::str::Utf8StringRef desc);

    static bool isPrimitiveDescChar(char c);

    // ── std::string 系列（外部边界：产品线 CLI 等消费 CpUtf8::text() 的场景）──
    // 转义规则与 escapeUnicode 一致（0x21..0x7E 保留，空格/控制符/高字节/backslash
    // 转 \uXXXX 大写），并对连续相同 token 做 run-length 压缩：
    //   连续 ≥ collapseThreshold 个相同 token → <token>...(省略N个)...<token>
    //   N = run 长度 - 2（首尾各保留 1 个）
    // 例：32767 个空格 → \u0020...(省略32765个)...\u0020
    static std::string printReadable(const std::string& raw, int collapseThreshold = 8);

    // 反向：把 \uXXXX（大写/小写 hex 均可）解码回原文 codepoint。
    // 非 \uXXXX 序列原样透传；非法 \uXXXX（hex 不足 4 位或越界）原样保留。
    static std::string unescapeUnicode(const std::string& escaped);
};

}   // namespace mjbase::mapping
