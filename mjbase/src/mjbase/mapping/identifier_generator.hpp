#pragma once
#include <vector>
#include <deque>
#include <cstdint>
#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"

namespace mjbase::mapping {

enum class DictionaryMode
{
    Alphabet,
    AlphabetOnlyUpper,
    Incrementing,
    IL1,
    Ii,
    Number,
    KeywordAndNumber,
    KeywordAndBlank,
    Unicode,
    UnicodeAndPureBlank,
    UnicodeAndPureBlankPlus,
    VerySmallUnicode,
    CuteAndBlank,
    EmojiAndBlank
};

ca::str::Utf8StringRef dictionaryModeToString(DictionaryMode mode);
DictionaryMode         stringToDictionaryMode(ca::str::Utf8StringRef str);

class IdentifierGenerator
{
public:
    explicit IdentifierGenerator(DictionaryMode mode);

    void           setMode(DictionaryMode mode);
    DictionaryMode mode() const { return mode_; }
    ca::usize      size() const { return dictionary_.size(); }

    void                   generateDictionary(ca::i64 num, bool needStuff = false);
    ca::str::Utf8StringRef getName();

    static ca::str::Utf8String getRandomString(ca::i32 length);
    static ca::str::Utf8String getRandomString(ca::i32 min, ca::i32 max, bool uppercase);
    static ca::str::Utf8String getRandomName();
    static ca::str::Utf8String getMassiveString();

    using CodePoints = std::vector<ca::u32>;

private:
    DictionaryMode mode_;
    // -- arena 死亡点声明（memory-governance-plan §5-P1③）--
    // 本成员持有本生成器私有的 Utf8StringArena：generateDictionary() 各模式生成
    // 的名字全部 arena_.intern 后存入 dictionary_，getName() 返回的视图即指向
    // 它们。视图寿命 = 本对象寿命；死亡点：随持有者析构（使用点均为 pass 内的
    // 栈局部对象）。消费者不得把视图带过本对象的死亡点；需跨期持有则 intern
    // 进自己的 arena 或深拷贝。
    ca::str::Utf8StringArena           arena_;
    std::deque<ca::str::Utf8StringRef> dictionary_;

    static CodePoints          utf8ToCodePoints(ca::str::Utf8StringRef s);
    static ca::str::Utf8String codePointsToString(const CodePoints& cps);

    std::vector<ca::str::Utf8StringRef> generateFullCombination(const CodePoints& basicChars,
                                                                ca::i64           num);
    std::vector<ca::str::Utf8StringRef> generatePermutations(const CodePoints& basicChars,
                                                             ca::i64           num);

    void generateAlphabetDictionary(ca::i64 num);
    void generateAlphabetOnlyUpperDictionary(ca::i64 num);
    void generateNumberDictionary(ca::i64 num);
    void generateIL1Dictionary(ca::i64 num);
    void generateIiDictionary(ca::i64 num);
    void generateKeywordAndNumberDictionary(ca::i64 num);
    void generateKeywordAndBlankDictionary(ca::i64 num);
    void generateUnicodeDictionary(ca::i64 num);
    void generateUnicodeAndPureBlankDictionary(ca::i64 num);
    void generateUnicodeAndPureBlankPlusDictionary(ca::i64 num);
    void generateVerySmallUnicodeDictionary(ca::i64 num);
    void generateCuteAndBlankDictionary(ca::i64 num);
    void generateEmojiAndBlankDictionary(ca::i64 num);
};

}   // namespace mjbase::mapping
