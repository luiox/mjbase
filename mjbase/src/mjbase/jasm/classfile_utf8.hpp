#pragma once

/// @file classfile_utf8.hpp
/// @brief classfile 常量池 Utf8（modified UTF-8）↔ 标准 UTF-8 的边界转换。
///
/// mjbase 的名字规范形是标准 UTF-8（计划键/容器 URI/参数面/MCP 文本），而
/// 常量池 Utf8 条目是 modified UTF-8（JVM 规范 §4.4.7：增补平面字符按代理
/// 对各 3 字节编码，U+0000 为 C0 80）。两个物化方向共用此头：读侧解码
/// （cp → 规范形），写侧编码（规范形 → cp）。

#include <string>
#include <string_view>

namespace mjbase::jasm {

/// @brief 常量池 Utf8 字节重组为标准 UTF-8 字符串。
///
/// JVM modified UTF-8：增补平面字符以高低代理各 3 字节编码（CESU-8 形），
/// U+0000 以 C0 80 编码；严格 UTF-8 校验会拒绝合法类名。成对代理重组为
/// 4 字节标准形，C0 80 还原为 NUL，其余字节原样通过。
inline std::string classFileUtf8ToString(std::string_view value)
{
    auto byteAt = [&value](std::size_t index) { return static_cast<unsigned char>(value[index]); };
    std::string out;
    out.reserve(value.size());
    std::size_t i = 0;
    while (i < value.size()) {
        if (byteAt(i) == 0xC0 && i + 1 < value.size() && byteAt(i + 1) == 0x80) {
            out += '\0';
            i += 2;
            continue;
        }
        if (byteAt(i) == 0xED && i + 5 < value.size() && byteAt(i + 1) >= 0xA0 &&
            byteAt(i + 1) <= 0xAF && byteAt(i + 2) >= 0x80 && byteAt(i + 2) <= 0xBF &&
            byteAt(i + 3) == 0xED && byteAt(i + 4) >= 0xB0 && byteAt(i + 4) <= 0xBF &&
            byteAt(i + 5) >= 0x80 && byteAt(i + 5) <= 0xBF) {
            const unsigned short hi = static_cast<unsigned short>(
                0xD000u + (static_cast<unsigned short>(byteAt(i + 1) & 0x3F) << 6) +
                static_cast<unsigned short>(byteAt(i + 2) & 0x3F));
            const unsigned short lo = static_cast<unsigned short>(
                0xD000u + (static_cast<unsigned short>(byteAt(i + 4) & 0x3F) << 6) +
                static_cast<unsigned short>(byteAt(i + 5) & 0x3F));
            const unsigned int codepoint =
                0x10000u + ((static_cast<unsigned int>(hi - 0xD800u) << 10) |
                            static_cast<unsigned int>(lo - 0xDC00u));
            out += static_cast<char>(0xF0 | (codepoint >> 18));
            out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (codepoint & 0x3F));
            i += 6;
            continue;
        }
        out += value[i];
        ++i;
    }
    return out;
}

/// @brief 标准 UTF-8 字符串编码为常量池 modified UTF-8 字节。
///
/// 与解码互逆：U+0000 → C0 80，增补平面 4 字节序列 → 高低代理各 3 字节，
/// 其余序列原样通过。对已是 modified UTF-8 形态的输入（ED 开头代理对、
/// C0 80）逐字节恒等——非法 UTF-8 字节一律原样通过，因此该编码对混入的
/// 旧形态字节幂等，写侧可无脑套用。
inline std::string utf8StringToClassFile(std::string_view value)
{
    auto byteAt = [&value](std::size_t index) { return static_cast<unsigned char>(value[index]); };
    std::string out;
    out.reserve(value.size() + value.size() / 2);
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char lead = byteAt(i);
        // U+0000：modified UTF-8 用 C0 80 双字节形。
        if (lead == 0x00) {
            out += static_cast<char>(0xC0);
            out += static_cast<char>(0x80);
            ++i;
            continue;
        }
        // 增补平面 4 字节序列（F0..F4）→ 代理对各 3 字节。严格校验续字节，
        // 不完整或非法序列按非法字节逐字节透传。
        if (lead >= 0xF0 && lead <= 0xF4 && i + 3 < value.size() && byteAt(i + 1) >= 0x80 &&
            byteAt(i + 1) <= 0xBF && byteAt(i + 2) >= 0x80 && byteAt(i + 2) <= 0xBF &&
            byteAt(i + 3) >= 0x80 && byteAt(i + 3) <= 0xBF) {
            const unsigned int codepoint = (static_cast<unsigned int>(lead & 0x07) << 18) |
                                           (static_cast<unsigned int>(byteAt(i + 1) & 0x3F) << 12) |
                                           (static_cast<unsigned int>(byteAt(i + 2) & 0x3F) << 6) |
                                           static_cast<unsigned int>(byteAt(i + 3) & 0x3F);
            const unsigned int adjusted = codepoint - 0x10000u;
            const unsigned short hi =
                static_cast<unsigned short>(0xD800u + (adjusted >> 10));
            const unsigned short lo =
                static_cast<unsigned short>(0xDC00u + (adjusted & 0x3FFu));
            for (const unsigned short unit : {hi, lo}) {
                out += static_cast<char>(0xE0 | (unit >> 12));
                out += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (unit & 0x3F));
            }
            i += 4;
            continue;
        }
        out += value[i];
        ++i;
    }
    return out;
}

}   //namespace mjbase::jasm
