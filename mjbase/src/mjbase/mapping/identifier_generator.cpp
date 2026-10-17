#include "identifier_generator.hpp"
#include "identifier_util.hpp"
#include <algorithm>
#include <random>
#include <queue>
#include <cmath>
#include <cstring>

namespace mjbase::mapping {

using CodePoints        = IdentifierGenerator::CodePoints;
using Utf8StringBuilder = ca::str::Utf8StringBuilder;

namespace {

// #1044 同款确定性回退种子（"idg1" 的 ASCII 编码）。
//
// 选值理由：本文件全部随机消费点只生成"名字/排版噪声"（EmojiAndBlank 字典的
// 空格装饰、needStuff 洗牌、getRandomString 字符采样），非安全用途——不需要
// 熵，只需要任意但固定的平台无关常量保证"同输入同输出"可复现（#1027 D1
// 确定性契约）。std::random_device 兜底会让同进程两次生成产生不同字典，
// 破坏确定性，故一律替换为本常量。
//
// getRandomString/getRandomName 是 static API：引擎用 thread_local 持久化，
// 逐次调用推进状态保证结果可变（避免常量种子把 API 退化成每次同串），
// 同时整个运行期零熵源，进程级输出仍确定。
constexpr std::uint32_t kIdentifierSeed = 0x69646731UL;   // "idg1"

}   // namespace

ca::str::Utf8StringRef dictionaryModeToString(DictionaryMode mode)
{
    switch (mode) {
    case DictionaryMode::Alphabet: return ca::str::Utf8StringRef::from_cstr("Alphabet");
    case DictionaryMode::AlphabetOnlyUpper:
        return ca::str::Utf8StringRef::from_cstr("AlphabetOnlyUpper");
    case DictionaryMode::Incrementing: return ca::str::Utf8StringRef::from_cstr("Incrementing");
    case DictionaryMode::IL1: return ca::str::Utf8StringRef::from_cstr("IL1");
    case DictionaryMode::Ii: return ca::str::Utf8StringRef::from_cstr("Ii");
    case DictionaryMode::Number: return ca::str::Utf8StringRef::from_cstr("Number");
    case DictionaryMode::KeywordAndNumber:
        return ca::str::Utf8StringRef::from_cstr("KeywordAndNumber");
    case DictionaryMode::KeywordAndBlank:
        return ca::str::Utf8StringRef::from_cstr("KeywordAndBlank");
    case DictionaryMode::Unicode: return ca::str::Utf8StringRef::from_cstr("Unicode");
    case DictionaryMode::UnicodeAndPureBlank:
        return ca::str::Utf8StringRef::from_cstr("UnicodeAndPureBlank");
    case DictionaryMode::UnicodeAndPureBlankPlus:
        return ca::str::Utf8StringRef::from_cstr("UnicodeAndPureBlankPlus");
    case DictionaryMode::VerySmallUnicode:
        return ca::str::Utf8StringRef::from_cstr("VerySmallUnicode");
    case DictionaryMode::CuteAndBlank: return ca::str::Utf8StringRef::from_cstr("CuteAndBlank");
    case DictionaryMode::EmojiAndBlank: return ca::str::Utf8StringRef::from_cstr("EmojiAndBlank");
    }
    return {};
}

DictionaryMode stringToDictionaryMode(ca::str::Utf8StringRef str)
{
    if (str.equals("Alphabet")) return DictionaryMode::Alphabet;
    if (str.equals("AlphabetOnlyUpper")) return DictionaryMode::AlphabetOnlyUpper;
    if (str.equals("Incrementing")) return DictionaryMode::Incrementing;
    if (str.equals("IL1")) return DictionaryMode::IL1;
    if (str.equals("Ii")) return DictionaryMode::Ii;
    if (str.equals("Number")) return DictionaryMode::Number;
    if (str.equals("KeywordAndNumber")) return DictionaryMode::KeywordAndNumber;
    if (str.equals("KeywordAndBlank")) return DictionaryMode::KeywordAndBlank;
    if (str.equals("Unicode")) return DictionaryMode::Unicode;
    if (str.equals("UnicodeAndPureBlank")) return DictionaryMode::UnicodeAndPureBlank;
    if (str.equals("UnicodeAndPureBlankPlus")) return DictionaryMode::UnicodeAndPureBlankPlus;
    if (str.equals("VerySmallUnicode")) return DictionaryMode::VerySmallUnicode;
    if (str.equals("CuteAndBlank")) return DictionaryMode::CuteAndBlank;
    if (str.equals("EmojiAndBlank")) return DictionaryMode::EmojiAndBlank;
    return DictionaryMode::Alphabet;
}

IdentifierGenerator::IdentifierGenerator(DictionaryMode mode)
    : mode_(mode)
{}

void IdentifierGenerator::setMode(DictionaryMode mode)
{
    mode_ = mode;
}

ca::str::Utf8StringRef IdentifierGenerator::getName()
{
    auto name = dictionary_.front();
    dictionary_.pop_front();
    return name;
}

CodePoints IdentifierGenerator::utf8ToCodePoints(ca::str::Utf8StringRef s)
{
    CodePoints result;
    auto*      ptr = s.data();
    auto*      end = ptr + s.byte_length();
    while (ptr < end) {
        bool   valid = true;
        ca::u8 b     = *ptr;
        if (b < 0x80) {
            result.push_back(b);
            ptr++;
        }
        else {
            int bytes = 0;
            if ((b & 0xE0) == 0xC0)
                bytes = 2;
            else if ((b & 0xF0) == 0xE0)
                bytes = 3;
            else if ((b & 0xF8) == 0xF0)
                bytes = 4;
            else {
                result.push_back(0xFFFD);
                ptr++;
                continue;
            }
            if (ptr + bytes > end) {
                result.push_back(0xFFFD);
                break;
            }
            ca::u32 cp = 0;
            switch (bytes) {
            case 2:
                cp = (b & 0x1F) << 6;
                cp |= (ptr[1] & 0x3F);
                break;
            case 3:
                cp = (b & 0x0F) << 12;
                cp |= (ptr[1] & 0x3F) << 6;
                cp |= (ptr[2] & 0x3F);
                break;
            case 4:
                cp = (b & 0x07) << 18;
                cp |= (ptr[1] & 0x3F) << 12;
                cp |= (ptr[2] & 0x3F) << 6;
                cp |= (ptr[3] & 0x3F);
                break;
            }
            if ((bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) ||
                (bytes == 4 && cp < 0x10000) || cp > 0x10FFFF)
                cp = 0xFFFD;
            result.push_back(cp);
            ptr += bytes;
        }
    }
    return result;
}

ca::str::Utf8String IdentifierGenerator::codePointsToString(const CodePoints& cps)
{
    Utf8StringBuilder sb;
    for (auto cp : cps) sb.append_code_point(cp);
    return sb.build();
}

std::vector<ca::str::Utf8StringRef> IdentifierGenerator::generateFullCombination(
    const CodePoints& basicChars, ca::i64 num)
{
    std::vector<ca::str::Utf8StringRef> pool;
    std::queue<std::vector<ca::u32>>    queue;
    for (auto cp : basicChars) queue.push({cp});

    while (!queue.empty() && static_cast<ca::i64>(pool.size()) < num) {
        auto s = queue.front();
        queue.pop();
        auto built = codePointsToString(s);
        pool.push_back(arena_.intern(built));
        for (auto cp : basicChars) {
            auto next = s;
            next.push_back(cp);
            queue.push(std::move(next));
        }
    }
    return pool;
}

std::vector<ca::str::Utf8StringRef> IdentifierGenerator::generatePermutations(
    const CodePoints& basicChars, ca::i64 num)
{
    auto n = static_cast<ca::i32>(std::ceil(std::log(static_cast<double>(num)) / std::log(2.0)));
    std::queue<std::vector<ca::u32>> queue;
    queue.push({});
    for (ca::i32 i = 0; i < n; i++) {
        ca::usize size = queue.size();
        for (ca::usize j = 0; j < size; j++) {
            auto current = queue.front();
            queue.pop();
            for (auto cp : basicChars) {
                auto next = current;
                next.push_back(cp);
                queue.push(std::move(next));
            }
        }
    }
    std::vector<ca::str::Utf8StringRef> pool;
    ca::i64                             count = 0;
    while (!queue.empty() && count < num) {
        auto built = codePointsToString(queue.front());
        pool.push_back(arena_.intern(built));
        queue.pop();
        count++;
    }
    return pool;
}

void IdentifierGenerator::generateAlphabetDictionary(ca::i64 num)
{
    auto cps  = utf8ToCodePoints(IdentifierUtil::alphabets());
    auto pool = generateFullCombination(cps, num);
    dictionary_.assign(pool.begin(), pool.end());
}

void IdentifierGenerator::generateAlphabetOnlyUpperDictionary(ca::i64 num)
{
    auto str  = ca::str::Utf8StringRef::from_cstr("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    auto cps  = utf8ToCodePoints(str);
    auto pool = generateFullCombination(cps, num);
    dictionary_.assign(pool.begin(), pool.end());
}

void IdentifierGenerator::generateNumberDictionary(ca::i64 num)
{
    auto cps  = utf8ToCodePoints(IdentifierUtil::numbers());
    auto pool = generateFullCombination(cps, num);
    dictionary_.assign(pool.begin(), pool.end());
}

void IdentifierGenerator::generateIL1Dictionary(ca::i64 num)
{
    CodePoints cps = {
        static_cast<ca::u32>('I'), static_cast<ca::u32>('l'), static_cast<ca::u32>('1')};
    auto pool = generatePermutations(cps, num);
    dictionary_.assign(pool.begin(), pool.end());
}

void IdentifierGenerator::generateIiDictionary(ca::i64 num)
{
    CodePoints cps  = {static_cast<ca::u32>('I'), static_cast<ca::u32>('i')};
    auto       pool = generatePermutations(cps, num);
    dictionary_.assign(pool.begin(), pool.end());
}

void IdentifierGenerator::generateKeywordAndNumberDictionary(ca::i64 num)
{
    auto& keywords = IdentifierUtil::javaKeyWords();
    auto  numCps   = utf8ToCodePoints(IdentifierUtil::numbers());
    auto  need     = num / static_cast<ca::i64>(keywords.size()) + 1;
    auto  namePool = generateFullCombination(numCps, need);
    for (const auto& kw : keywords) {
        for (const auto& n : namePool) {
            Utf8StringBuilder sb;
            sb.append(kw);
            sb.append(n);
            dictionary_.push_back(arena_.intern(sb.build()));
            if (static_cast<ca::i64>(dictionary_.size()) >= num) return;
        }
    }
}

void IdentifierGenerator::generateKeywordAndBlankDictionary(ca::i64 num)
{
    auto&      keywords = IdentifierUtil::javaKeyWords();
    auto&      blanks   = IdentifierUtil::blankChars();
    CodePoints blankCps;
    for (const auto& b : blanks) {
        auto cps = utf8ToCodePoints(b);
        blankCps.insert(blankCps.end(), cps.begin(), cps.end());
    }
    auto need     = num / static_cast<ca::i64>(keywords.size()) + 1;
    auto namePool = generateFullCombination(blankCps, need);
    for (const auto& kw : keywords) {
        for (const auto& n : namePool) {
            Utf8StringBuilder sb;
            sb.append(kw);
            sb.append(n);
            dictionary_.push_back(arena_.intern(sb.build()));
            if (static_cast<ca::i64>(dictionary_.size()) >= num) return;
        }
    }
}

void IdentifierGenerator::generateUnicodeDictionary(ca::i64 num)
{
    CodePoints basicUnicode;
    for (ca::u32 i = 0; i < 0x900; i++) basicUnicode.push_back(0x0100 + i);
    auto pool = generateFullCombination(basicUnicode, num);
    dictionary_.assign(pool.begin(), pool.end());
}

void IdentifierGenerator::generateUnicodeAndPureBlankDictionary(ca::i64 num)
{
    (void)num;
}

void IdentifierGenerator::generateUnicodeAndPureBlankPlusDictionary(ca::i64 num)
{
    (void)num;
}

void IdentifierGenerator::generateVerySmallUnicodeDictionary(ca::i64 num)
{
    CodePoints cps;
    for (ca::u32 cp = 0x0300; cp <= 0x036F; cp++) cps.push_back(cp);
    auto pool = generateFullCombination(cps, num);
    dictionary_.assign(pool.begin(), pool.end());
}

void IdentifierGenerator::generateCuteAndBlankDictionary(ca::i64 num)
{
    auto&      blanks = IdentifierUtil::blankChars();
    CodePoints blankCps;
    for (const auto& b : blanks) {
        auto cps2 = utf8ToCodePoints(b);
        blankCps.insert(blankCps.end(), cps2.begin(), cps2.end());
    }
    auto              raw = generateFullCombination(blankCps, num);
    Utf8StringBuilder prefix;
    prefix.append_code_point(0x0020);
    static const ca::u32 cute[] = {
        0x0028, 0x02C3, 0x0336, 0x0348, 0x0300, 0x0BF0, 0x02C2, 0x0336, 0x0348, 0x0301, 0x0029};
    for (auto cp : cute) prefix.append_code_point(cp);
    auto prefixStr = prefix.build();
    for (const auto& s : raw) {
        Utf8StringBuilder sb;
        sb.append(prefixStr.ref());
        sb.append(s);
        dictionary_.push_back(arena_.intern(sb.build()));
    }
}

void IdentifierGenerator::generateEmojiAndBlankDictionary(ca::i64 num)
{
    CodePoints emojis;
    struct Range
    {
        ca::i32 start;
        ca::i32 end;
    };
    static const Range ranges[] = {
        {0x1F600, 0x1F64F},
        {0x1F300, 0x1F5FF},
        {0x1F680, 0x1F6C0},
        {0x1F700, 0x1F77F},
    };
    for (const auto& r : ranges)
        for (ca::i32 cp = r.start; cp <= r.end; cp++) emojis.push_back(static_cast<ca::u32>(cp));

    auto         raw = generateFullCombination(emojis, num);
    std::mt19937 rng(kIdentifierSeed);   // 常量种子：同 (mode, num) 同字典（见文件头注释）。
    for (const auto& s : raw) {
        Utf8StringBuilder sb;
        int               leadingSpaces = std::uniform_int_distribution<int>(0, 4)(rng);
        for (int i = 0; i < leadingSpaces; i++) sb.append_code_point(0x0020);
        for (auto cp : emojis) sb.append_code_point(cp);
        sb.append(s);
        int trailingSpaces = std::uniform_int_distribution<int>(0, 19)(rng);
        for (int i = 0; i < trailingSpaces; i++) sb.append_code_point(0x0020);
        dictionary_.push_back(arena_.intern(sb.build()));
    }
}

void IdentifierGenerator::generateDictionary(ca::i64 num, bool needStuff)
{
    dictionary_.clear();
    switch (mode_) {
    case DictionaryMode::Alphabet: generateAlphabetDictionary(num); break;
    case DictionaryMode::AlphabetOnlyUpper: generateAlphabetOnlyUpperDictionary(num); break;
    case DictionaryMode::Number: generateNumberDictionary(num); break;
    case DictionaryMode::IL1: generateIL1Dictionary(num); break;
    case DictionaryMode::Ii: generateIiDictionary(num); break;
    case DictionaryMode::KeywordAndNumber: generateKeywordAndNumberDictionary(num); break;
    case DictionaryMode::KeywordAndBlank: generateKeywordAndBlankDictionary(num); break;
    case DictionaryMode::Unicode: generateUnicodeDictionary(num); break;
    case DictionaryMode::UnicodeAndPureBlank: generateUnicodeAndPureBlankDictionary(num); break;
    case DictionaryMode::UnicodeAndPureBlankPlus:
        generateUnicodeAndPureBlankPlusDictionary(num);
        break;
    case DictionaryMode::VerySmallUnicode: generateVerySmallUnicodeDictionary(num); break;
    case DictionaryMode::CuteAndBlank: generateCuteAndBlankDictionary(num); break;
    case DictionaryMode::EmojiAndBlank: generateEmojiAndBlankDictionary(num); break;
    default: break;
    }
    if (needStuff && dictionary_.size() > 1) {
        std::vector<ca::str::Utf8StringRef> vec(dictionary_.begin(), dictionary_.end());
        std::shuffle(vec.begin(), vec.end(), std::mt19937(kIdentifierSeed));
        dictionary_.assign(vec.begin(), vec.end());
    }
}

ca::str::Utf8String IdentifierGenerator::getRandomString(ca::i32 length)
{
    static const char charSet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";
    static constexpr ca::i32               charSetLen = static_cast<ca::i32>(sizeof(charSet) - 1);
    // static API：thread_local 持久引擎，逐次调用结果可变且零熵源（见文件头注释）。
    static thread_local std::mt19937       rng(kIdentifierSeed);
    std::uniform_int_distribution<ca::i32> dist(0, charSetLen - 1);
    Utf8StringBuilder                      sb;
    for (ca::i32 i = 0; i < length; i++)
        sb.append_code_point(static_cast<ca::u8>(charSet[dist(rng)]));
    return sb.build();
}

ca::str::Utf8String IdentifierGenerator::getRandomString(ca::i32 min, ca::i32 max, bool uppercase)
{
    static const char        alphabet[] = "abcdefghijklmnopqrstuvwxyz";
    static constexpr ca::i32 alphaLen   = static_cast<ca::i32>(sizeof(alphabet) - 1);
    // static API：thread_local 持久引擎，逐次调用结果可变且零熵源（见文件头注释）。
    static thread_local std::mt19937 rng(kIdentifierSeed);
    auto                     len = std::uniform_int_distribution<ca::i32>(min, max)(rng);
    Utf8StringBuilder        sb;
    for (ca::i32 i = 0; i < len; i++) {
        char c = alphabet[std::uniform_int_distribution<ca::i32>(0, alphaLen - 1)(rng)];
        if (uppercase && std::uniform_int_distribution<ca::i32>(0, 1)(rng))
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        sb.append_code_point(static_cast<ca::u8>(c));
    }
    return sb.build();
}

ca::str::Utf8String IdentifierGenerator::getRandomName()
{
    return getRandomString(3, 6, false);
}

ca::str::Utf8String IdentifierGenerator::getMassiveString()
{
    Utf8StringBuilder sb;
    for (ca::i32 i = 0; i < 32767; i++) sb.append_code_point(0x0020);
    return sb.build();
}

}   // namespace mjbase::mapping
