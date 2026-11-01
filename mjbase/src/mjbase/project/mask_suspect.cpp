#include "mjbase/project/mask_suspect.hpp"

#include <utility>
#include <vector>

#include "mjbase/project/mask_regex_engine.hpp"

namespace mjbase::project {
namespace {

/// @brief 解码一个 UTF-8 序列。合法时输出码点并返回消耗字节数；非法时
///        返回 0 并由调用方按 1 字节步进跳过（同步走异常旗标）。
ca::u32 decodeCodePoint(std::string_view text, ca::usize offset, ca::usize& consumed)
{
    const auto byteAt = [&](ca::usize index) -> ca::u32 {
        return static_cast<ca::u8>(text[offset + index]);
    };
    const ca::u32 lead = byteAt(0);
    if (lead < 0x80) {
        consumed = 1;
        return lead;
    }
    ca::u32 codePoint   = 0;
    ca::usize continuation = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        codePoint   = lead & 0x1Fu;
        continuation = 1;
    }
    else if (lead >= 0xE0 && lead <= 0xEF) {
        codePoint   = lead & 0x0Fu;
        continuation = 2;
    }
    else if (lead >= 0xF0 && lead <= 0xF4) {
        codePoint   = lead & 0x07u;
        continuation = 3;
    }
    else {
        return 0;   ///< 0x80..0xC1 裸延续字节与 0xF5..0xFF 越界引导
    }
    for (ca::usize index = 1; index <= continuation; ++index) {
        if (offset + index >= text.size()) return 0;
        const ca::u32 next = byteAt(index);
        if ((next & 0xC0u) != 0x80u) return 0;
        codePoint = (codePoint << 6) | (next & 0x3Fu);
    }
    // 过长编码（ surrogates U+D800..U+DFFF 与 > U+10FFFF 由引导字节上界
    // 保证；E0 A0 段与 F0 90 段的下界在此显式复核）。
    if ((continuation == 2 && codePoint < 0x800u) ||
        (continuation == 3 && codePoint < 0x10000u)) {
        return 0;
    }
    consumed = continuation + 1;
    return codePoint;
}

bool isControlCodePoint(ca::u32 codePoint) noexcept
{
    return codePoint < 0x20u || (codePoint >= 0x7Fu && codePoint <= 0x9Fu);
}

bool isZeroWidthCodePoint(ca::u32 codePoint) noexcept
{
    return (codePoint >= 0x200Bu && codePoint <= 0x200Fu) ||
           (codePoint >= 0x202Au && codePoint <= 0x202Eu) ||
           (codePoint >= 0x2060u && codePoint <= 0x2064u) ||
           (codePoint >= 0x2066u && codePoint <= 0x2069u) ||
           codePoint == 0xFEFFu;
}

bool isAnomalousCodePoint(ca::u32 codePoint) noexcept
{
    return (codePoint >= 0xE000u && codePoint <= 0xF8FFu) ||          ///< BMP 私有区
           (codePoint >= 0xF0000u && codePoint <= 0xFFFFDu) ||        ///< 15 面私有区
           (codePoint >= 0x100000u && codePoint <= 0x10FFFDu) ||      ///< 16 面私有区
           (codePoint >= 0xFDD0u && codePoint <= 0xFDEFu) ||          ///< 非字符段
           (codePoint & 0xFFFEu) == 0xFFFEu;                          ///< 各面 U+xxFE/U+xxFF
}

/// 内建注入形态模式集：通用指令/协议形态的字面量，中性命名，不指向任何
/// 具体家族。大小写不敏感经 MaskRegex::compile 的显式 icase 传参（引擎
/// 适配面不加隐式旗标）。书写纪律：无嵌套量词、无重叠量词，短语有界。
constexpr const char* kBuiltinInjectionPatterns[] = {
    "ignore (all |any |the )?(previous|prior|above|earlier) (instructions|prompts|rules)",
    "disregard (all |any |the )?(previous|prior|above|earlier) (instructions|prompts|rules)",
    "reveal (your |the |its )?(system |hidden |initial )?(prompt|instructions)",
    "you are now (a |an |my )",
    "\\$\\{jndi:",
};

/// @brief 内建模式的一次性编译缓存（字面量集 → 已编译正则；magic static
///        线程安全）。编译失败只可能是本文件的静态缺陷：跳过该条并在
///        测试面以行为命中兜底验证。
const std::vector<MaskRegex>& builtinInjectionPatterns()
{
    static const std::vector<MaskRegex> patterns = [] {
        std::vector<MaskRegex> compiled;
        compiled.reserve(sizeof(kBuiltinInjectionPatterns) / sizeof(kBuiltinInjectionPatterns[0]));
        for (const char* pattern : kBuiltinInjectionPatterns) {
            auto built = MaskRegex::compile(std::string_view(pattern), /*icase=*/true);
            if (built.is_ok()) compiled.push_back(std::move(built).unwrap());
        }
        return compiled;
    }();
    return patterns;
}

bool matchesBuiltinInjectionPattern(std::string_view text)
{
    if (text.size() > kMaskMaxEvaluatedBytes) return false;
    for (const MaskRegex& pattern : builtinInjectionPatterns()) {
        if (pattern.matches(text)) return true;
    }
    return false;
}

}   // namespace

bool SuspectFlags::any() const noexcept
{
    return controlChars || zeroWidthChars || anomalousUnicode || injectionPattern;
}

SuspectFlags scanSuspect(std::string_view text)
{
    SuspectFlags flags;
    ca::usize offset = 0;
    while (offset < text.size()) {
        ca::usize consumed = 0;
        const ca::u32 codePoint = decodeCodePoint(text, offset, consumed);
        if (codePoint == 0 && consumed == 0) {
            flags.anomalousUnicode = true;
            ++offset;
            continue;
        }
        offset += consumed;
        if (isControlCodePoint(codePoint)) flags.controlChars = true;
        if (isZeroWidthCodePoint(codePoint)) flags.zeroWidthChars = true;
        if (isAnomalousCodePoint(codePoint)) flags.anomalousUnicode = true;
    }
    if (matchesBuiltinInjectionPattern(text)) flags.injectionPattern = true;
    return flags;
}

}   // namespace mjbase::project
