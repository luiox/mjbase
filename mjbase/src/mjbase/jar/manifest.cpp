#include "manifest.hpp"

#include <algorithm>
#include <cstddef>

namespace mjbase::jar {

static constexpr ca::usize kMaxLineLen = 72;

namespace {

// 追加 [data, data + n) 字节区间。
void appendRange(std::vector<ca::u8>& out, const ca::u8* data, ca::usize n)
{
    out.insert(out.end(), data, data + n);
}

// 追加 CRLF 行结束符。
void appendCrlf(std::vector<ca::u8>& out)
{
    out.push_back('\r');
    out.push_back('\n');
}

}   // namespace

Manifest::Manifest(const Manifest& other)
{
    mainAttributes_.reserve(other.mainAttributes_.size());
    for (const auto& [k, v] : other.mainAttributes_)
        mainAttributes_.emplace_back(k.clone(), v.clone());
}

Manifest& Manifest::operator=(const Manifest& other)
{
    if (this != &other) {
        std::vector<std::pair<Utf8String, Utf8String>> fresh;
        fresh.reserve(other.mainAttributes_.size());
        for (const auto& [k, v] : other.mainAttributes_)
            fresh.emplace_back(k.clone(), v.clone());
        mainAttributes_ = std::move(fresh);
    }
    return *this;
}

// 解析 MANIFEST.MF：主属性段，支持续行（以空格开头）和 key: value 格式。
// 按 '\n' 切行（末行可不带换行符）、只剥一个行尾 '\r'、空行即主属性段
// 结束（其后内容忽略）。
Manifest Manifest::fromBytes(const std::vector<ca::u8>& bytes)
{
    Manifest manifest;
    std::vector<ca::u8> pendingKey, pendingValue;

    // 将当前累积的 key:value 刷入 manifest（setMainAttribute：既有 key 原位
    // 覆写保插入序）。
    auto flush = [&]() {
        if (!pendingKey.empty()) {
            const auto keyData   = pendingKey.data();
            const auto valueData = pendingValue.data();
            manifest.setMainAttribute(
                Utf8StringRef(keyData, pendingKey.size(), pendingKey.size()),
                Utf8StringRef(valueData, pendingValue.size(), pendingValue.size()));
            pendingKey.clear();
            pendingValue.clear();
        }
    };

    const ca::usize total = bytes.size();
    ca::usize       pos   = 0;
    while (pos < total) {
        const ca::usize lineEnd = static_cast<ca::usize>(
            std::find(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end(),
                      static_cast<ca::u8>('\n')) -
            bytes.begin());
        const ca::usize next = (lineEnd == total) ? total : lineEnd + 1;

        ca::usize len = lineEnd - pos;
        if (len > 0 && bytes[pos + len - 1] == '\r') --len;   // 剥一个行尾 CR
        if (len == 0) {
            flush();
            break;   // 空行 = 主属性段结束
        }
        if (bytes[pos] == ' ') {   // 续行：追加到上一个 value
            appendRange(pendingValue, bytes.data() + pos + 1, len - 1);
            pos = next;
            continue;
        }
        const auto lineBegin = bytes.begin() + static_cast<std::ptrdiff_t>(pos);
        const auto lineSep   = bytes.begin() + static_cast<std::ptrdiff_t>(pos + len);
        const auto colonIt   = std::find(lineBegin, lineSep, static_cast<ca::u8>(':'));
        if (colonIt != lineSep) {   // key: value 分割（首个冒号）；无冒号的行整体忽略
            const ca::usize colon = static_cast<ca::usize>(colonIt - bytes.begin());
            flush();   // 先刷出上一个条目
            pendingKey.clear();
            appendRange(pendingKey, bytes.data() + pos, colon - pos);
            pendingValue.clear();
            if (colon + 1 < pos + len && bytes[colon + 1] == ' ')
                appendRange(pendingValue, bytes.data() + colon + 2, pos + len - (colon + 2));
            else
                appendRange(pendingValue, bytes.data() + colon + 1, pos + len - (colon + 1));
        }
        pos = next;
    }
    flush();
    return manifest;
}

std::vector<ca::u8> Manifest::toBytes() const
{
    // JAR-4：折行截断点必须落在 UTF-8 字符起始字节上，否则会把多字节字符切成两半
    // （截断后无法原样解析回）。从给定字节位置回退：连续字节（0b10xxxxxx）前移
    // 到所属字符的起始字节；ASCII 与字符起始字节均不会被回退。
    const auto charStart = [](const std::vector<ca::u8>& s, ca::usize pos) -> ca::usize {
        while (pos > 0 && pos < s.size() && (s[pos] & 0xC0) == 0x80) --pos;
        return pos;
    };

    std::vector<ca::u8> out;
    for (const auto& [key, value] : mainAttributes_) {
        const Utf8StringRef k(key);
        const Utf8StringRef v(value);
        const ca::usize     lineLen = k.byte_length() + 2 + v.byte_length();

        if (lineLen <= kMaxLineLen) {
            appendRange(out, k.data(), k.byte_length());
            appendRange(out, reinterpret_cast<const ca::u8*>(": "), 2);
            appendRange(out, v.data(), v.byte_length());
            appendCrlf(out);
        }
        else {
            // Wrap long lines: first line at most 72 bytes, continuation lines prefixed with
            // space（含前缀后不超过 72 字节）。截断点回退到 UTF-8 字符起始字节（JAR-4）。
            std::vector<ca::u8> line;
            line.reserve(lineLen);
            appendRange(line, k.data(), k.byte_length());
            appendRange(line, reinterpret_cast<const ca::u8*>(": "), 2);
            appendRange(line, v.data(), v.byte_length());

            ca::usize cut = charStart(line, kMaxLineLen);
            appendRange(out, line.data(), cut);
            appendCrlf(out);
            ca::usize pos = cut;
            while (pos < line.size()) {
                ca::usize end = charStart(line, std::min(line.size(), pos + (kMaxLineLen - 1)));
                // 前进保障：整窗都是续字节时 charStart 会回退到 pos 甚至越过 pos
                //（长串连续 0x80-0xBF 的敌意输入），end==pos 将使循环永不前进。
                // 此时放弃回退按字节硬切，至少推进 1 字节。
                if (end <= pos) end = std::min(line.size(), pos + 1);
                out.push_back(' ');
                appendRange(out, line.data() + pos, end - pos);
                appendCrlf(out);
                pos = end;
            }
        }
    }
    appendCrlf(out);
    return out;
}

}   // namespace mjbase::jar
