#include "mjbase/pass/intrinsic_folding.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "jstd/random.hpp"

#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/pass/method_rewrite_util.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::Op;
using jbc::ir::Insn;
using jbc::ir::InsnId;

// ── JDK 语义位运算辅助（无符号域计算规避有符号溢出 UB）──────────────

ca::i64 javaLongRotateLeft(ca::i64 v, int d)
{
    using U         = ca::u64;
    constexpr int N = 64;
    const int     cnt = d & (N - 1);
    if (cnt == 0) return v;
    const U u = static_cast<U>(v);
    return static_cast<ca::i64>((u << cnt) | (u >> (N - cnt)));
}

ca::i64 javaLongRotateRight(ca::i64 v, int d)
{
    using U         = ca::u64;
    constexpr int N = 64;
    const int     cnt = d & (N - 1);
    if (cnt == 0) return v;
    const U u = static_cast<U>(v);
    return static_cast<ca::i64>((u >> cnt) | (u << (N - cnt)));
}

ca::i32 javaIntRotateLeft(ca::i32 v, int d)
{
    using U         = ca::u32;
    constexpr int N = 32;
    const int     cnt = d & (N - 1);
    if (cnt == 0) return v;
    const U u = static_cast<U>(v);
    return static_cast<ca::i32>((u << cnt) | (u >> (N - cnt)));
}

ca::i32 javaIntRotateRight(ca::i32 v, int d)
{
    using U         = ca::u32;
    constexpr int N = 32;
    const int     cnt = d & (N - 1);
    if (cnt == 0) return v;
    const U u = static_cast<U>(v);
    return static_cast<ca::i32>((u >> cnt) | (u << (N - cnt)));
}

ca::i64 javaLongReverse(ca::i64 v)
{
    ca::u64 x = static_cast<ca::u64>(v);
    x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
    x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    x = ((x >> 8) & 0x00FF00FF00FF00FFULL) | ((x & 0x00FF00FF00FF00FFULL) << 8);
    x = ((x >> 16) & 0x0000FFFF0000FFFFULL) | ((x & 0x0000FFFF0000FFFFULL) << 16);
    x = (x >> 32) | (x << 32);
    return static_cast<ca::i64>(x);
}

ca::i32 javaIntReverse(ca::i32 v)
{
    ca::u32 x = static_cast<ca::u32>(v);
    x = ((x >> 1) & 0x55555555U) | ((x & 0x55555555U) << 1);
    x = ((x >> 2) & 0x33333333U) | ((x & 0x33333333U) << 2);
    x = ((x >> 4) & 0x0F0F0F0FU) | ((x & 0x0F0F0F0FU) << 4);
    x = ((x >> 8) & 0x00FF00FFU) | ((x & 0x00FF00FFU) << 8);
    x = (x >> 16) | (x << 16);
    return static_cast<ca::i32>(x);
}

ca::i64 javaLongReverseBytes(ca::i64 v)
{
    ca::u64 x = static_cast<ca::u64>(v);
    x = ((x >> 8) & 0x00FF00FF00FF00FFULL) | ((x & 0x00FF00FF00FF00FFULL) << 8);
    x = ((x >> 16) & 0x0000FFFF0000FFFFULL) | ((x & 0x0000FFFF0000FFFFULL) << 16);
    x = (x >> 32) | (x << 32);
    return static_cast<ca::i64>(x);
}

ca::i32 javaIntReverseBytes(ca::i32 v)
{
    ca::u32 x = static_cast<ca::u32>(v);
    x = ((x >> 8) & 0x00FF00FFU) | ((x & 0x00FF00FFU) << 8);
    x = (x >> 16) | (x << 16);
    return static_cast<ca::i32>(x);
}

ca::i32 javaLongBitCount(ca::i64 v)
{
    ca::u64 x = static_cast<ca::u64>(v);
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    x = x + (x >> 8);
    x = x + (x >> 16);
    x = x + (x >> 32);
    return static_cast<ca::i32>(x & 0x7FULL);
}

ca::i32 javaIntBitCount(ca::i32 v)
{
    ca::u32 x = static_cast<ca::u32>(v);
    x = x - ((x >> 1) & 0x55555555U);
    x = (x & 0x33333333U) + ((x >> 2) & 0x33333333U);
    x = (x + (x >> 4)) & 0x0F0F0F0FU;
    x = x + (x >> 8);
    x = x + (x >> 16);
    return static_cast<ca::i32>(x & 0x3FU);
}

ca::i32 javaLongNumberOfLeadingZeros(ca::i64 v)
{
    if (v == 0) return 64;
    ca::u64 x = static_cast<ca::u64>(v);
    ca::i32 n = 0;
    if ((x >> 32) == 0) { n += 32; x <<= 32; }
    if ((x >> 48) == 0) { n += 16; x <<= 16; }
    if ((x >> 56) == 0) { n += 8; x <<= 8; }
    if ((x >> 60) == 0) { n += 4; x <<= 4; }
    if ((x >> 62) == 0) { n += 2; x <<= 2; }
    if ((x >> 63) == 0) { n += 1; }
    return n;
}

ca::i32 javaIntNumberOfLeadingZeros(ca::i32 v)
{
    if (v == 0) return 32;
    ca::u32 x = static_cast<ca::u32>(v);
    ca::i32 n = 0;
    if ((x >> 16) == 0) { n += 16; x <<= 16; }
    if ((x >> 24) == 0) { n += 8; x <<= 8; }
    if ((x >> 28) == 0) { n += 4; x <<= 4; }
    if ((x >> 30) == 0) { n += 2; x <<= 2; }
    if ((x >> 31) == 0) { n += 1; }
    return n;
}

ca::i32 javaLongNumberOfTrailingZeros(ca::i64 v)
{
    if (v == 0) return 64;
    ca::u64 x = static_cast<ca::u64>(v);
    ca::i32 n = 0;
    if ((x & 0xFFFFFFFFULL) == 0) { n += 32; x >>= 32; }
    if ((x & 0xFFFFULL) == 0) { n += 16; x >>= 16; }
    if ((x & 0xFFULL) == 0) { n += 8; x >>= 8; }
    if ((x & 0xFULL) == 0) { n += 4; x >>= 4; }
    if ((x & 0x3ULL) == 0) { n += 2; x >>= 2; }
    if ((x & 0x1ULL) == 0) { n += 1; }
    return n;
}

ca::i32 javaIntNumberOfTrailingZeros(ca::i32 v)
{
    if (v == 0) return 32;
    ca::u32 x = static_cast<ca::u32>(v);
    ca::i32 n = 0;
    if ((x & 0xFFFFU) == 0) { n += 16; x >>= 16; }
    if ((x & 0xFFU) == 0) { n += 8; x >>= 8; }
    if ((x & 0xFU) == 0) { n += 4; x >>= 4; }
    if ((x & 0x3U) == 0) { n += 2; x >>= 2; }
    if ((x & 0x1U) == 0) { n += 1; }
    return n;
}

ca::i64 javaLongHighestOneBit(ca::i64 v)
{
    if (v == 0) return 0;
    return static_cast<ca::i64>(static_cast<ca::u64>(1ULL)
                                << (63 - javaLongNumberOfLeadingZeros(v)));
}

ca::i32 javaIntHighestOneBit(ca::i32 v)
{
    if (v == 0) return 0;
    return static_cast<ca::i32>(1U << (31 - javaIntNumberOfLeadingZeros(v)));
}

// v & -v：无符号域取负，规避 MIN_VALUE 的有符号取负 UB。
ca::i64 javaLongLowestOneBit(ca::i64 v)
{
    const ca::u64 u = static_cast<ca::u64>(v);
    return static_cast<ca::i64>(u & (~u + 1));
}

ca::i32 javaIntLowestOneBit(ca::i32 v)
{
    const ca::u32 u = static_cast<ca::u32>(v);
    return static_cast<ca::i32>(u & (~u + 1));
}

// ── JDK Math.max/min 浮点语义（NaN 传染、±0 有向）──────────────────

template <typename T>
T javaFloatingMax(T a, T b)
{
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == static_cast<T>(0) && b == static_cast<T>(0)) {
        return (!std::signbit(a) || !std::signbit(b)) ? static_cast<T>(0) : a;
    }
    return a >= b ? a : b;
}

template <typename T>
T javaFloatingMin(T a, T b)
{
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == static_cast<T>(0) && b == static_cast<T>(0)) {
        return (std::signbit(a) || std::signbit(b)) ? static_cast<T>(-0) : a;
    }
    return a <= b ? a : b;
}

// ── 字符串面：CP 常量串是 MUTF-8 字节视图 ──────────────────────────

// MUTF-8/CESU-8 无跨槽续字节歧义：非续字节（b & 0xC0 != 0x80）恰为码元
// 起始，计数即 String.length() 的码元数。
ca::usize mutf8CodeUnitCount(std::string_view s)
{
    ca::usize n = 0;
    for (const char ch : s) {
        if ((static_cast<ca::u8>(ch) & 0xC0) != 0x80) ++n;
    }
    return n;
}

bool isPureAscii(std::string_view s)
{
    for (const char ch : s) {
        if (static_cast<ca::u8>(ch) >= 0x80) return false;
    }
    return true;
}

// JDK Integer.parseInt / Long.parseLong 严格十进制：可选正负号 + 全数字，
// 溢出与尾随杂质拒收（from_chars 对 '-' 原生处理，'+' 显式跳过）。
bool parseJavaIntStrict(std::string_view text, ca::i32& out)
{
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last  = first + text.size();
    if (*first == '+') {
        ++first;
        if (first == last) return false;
    }
    ca::i32           value  = 0;
    const std::from_chars_result r = std::from_chars(first, last, value, 10);
    if (r.ec != std::errc{} || r.ptr != last) return false;
    out = value;
    return true;
}

bool parseJavaLongStrict(std::string_view text, ca::i64& out)
{
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last  = first + text.size();
    if (*first == '+') {
        ++first;
        if (first == last) return false;
    }
    ca::i64           value  = 0;
    const std::from_chars_result r = std::from_chars(first, last, value, 10);
    if (r.ec != std::errc{} || r.ptr != last) return false;
    out = value;
    return true;
}

// ── 窗口守卫与折叠落笔 ─────────────────────────────────────────────

// [from, to] 闭区间成员的前置边界是否锚有存活标签（内部进入保护）。
bool anyInteriorAnchored(jbc::ir::CodeDom& dom, const std::vector<InsnId>& ids,
                         ca::usize from, ca::usize to)
{
    for (ca::usize p = from; p <= to; ++p) {
        if (method_rewrite_util::labelsAnchorAt(dom, ids[p])) return true;
    }
    return false;
}

/// 折叠窗 [headPos..tailPos]：头改写为 mat（srcPc 影线保留），尾→头+1
/// 依次删除。先删尾保证头部删除的承接者（同 ConsJumpOptimizer 落笔纪律），
/// 任一删除失败返回 false，头未改——无半折叠态。
bool foldWindow(jbc::ir::CodeDom& dom, jbc::ir::CodeEditor& editor,
                const std::vector<InsnId>& ids, ca::usize headPos, ca::usize tailPos,
                const Insn& mat)
{
    for (ca::usize p = tailPos; p > headPos; --p) {
        if (editor.erase(ids[p], jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                .is_err()) {
            return false;
        }
    }
    auto got = dom.get(ids[headPos]);
    if (got.is_err()) return false;
    Insn* g = std::move(got).unwrap();
    g->op     = mat.op;
    g->wide   = mat.wide;
    g->a      = mat.a;
    g->b      = mat.b;
    g->cp     = mat.cp;
    g->target = mat.target;
    g->targets.clear();
    g->keys.clear();
    return true;
}

/// 整窗删除变体（装箱对 / toCharArray+valueOf 对）：头也删。头前置边界
/// 的锚定标签按 fallthrough 重定向——从头进入重放整窗、净栈效应零，
/// 窗后承接者即原消费点。
bool eraseWindow(jbc::ir::CodeEditor& editor, const std::vector<InsnId>& ids,
                 ca::usize headPos, ca::usize tailPos)
{
    for (ca::usize p = tailPos + 1; p > headPos; --p) {
        if (editor.erase(ids[p - 1], jbc::ir::CodeEditor::ErasePolicy::kRetargetToFallthrough)
                .is_err()) {
            return false;
        }
    }
    return true;
}

// ── 装箱族 ─────────────────────────────────────────────────────────

struct BoxPair
{
    const char* owner;
    const char* valueOfDesc;
    const char* unboxName;
    const char* unboxDesc;
};

constexpr BoxPair kBoxPairs[] = {
    {"java/lang/Integer",   "(I)Ljava/lang/Integer;",   "intValue",     "()I"},
    {"java/lang/Long",      "(J)Ljava/lang/Long;",      "longValue",    "()J"},
    {"java/lang/Float",     "(F)Ljava/lang/Float;",     "floatValue",   "()F"},
    {"java/lang/Double",    "(D)Ljava/lang/Double;",    "doubleValue",  "()D"},
    {"java/lang/Boolean",   "(Z)Ljava/lang/Boolean;",   "booleanValue", "()Z"},
    {"java/lang/Short",     "(S)Ljava/lang/Short;",     "shortValue",   "()S"},
    {"java/lang/Byte",      "(B)Ljava/lang/Byte;",      "byteValue",    "()B"},
    {"java/lang/Character", "(C)Ljava/lang/Character;", "charValue",    "()C"},
};

// ── 参数读取 ───────────────────────────────────────────────────────

struct ArgVal
{
    enum class Kind : ca::u8 { Int, Long, Float, Double, Str };
    Kind             kind = Kind::Int;
    ca::i64          num  = 0;   // Int/Long
    double           fnum = 0.0; // Float/Double
    std::string_view str;        // Str（CP 视图，会话存活期有效）
};

/// 方法描述符参数族解析：B/C/S/I/Z 在栈上同 int 装载面 → Int；J/F/D 同族；
/// Ljava/lang/String; → Str。其余（数组、其他对象）白名单外 → false。
bool paramKindsOf(std::string_view desc, std::vector<ArgVal::Kind>& out)
{
    out.clear();
    if (desc.empty() || desc.front() != '(') return false;
    ca::usize pos = 1;
    while (pos < desc.size() && desc[pos] != ')') {
        switch (desc[pos]) {
        case 'B':
        case 'C':
        case 'S':
        case 'I':
        case 'Z': out.push_back(ArgVal::Kind::Int); ++pos; break;
        case 'J': out.push_back(ArgVal::Kind::Long); ++pos; break;
        case 'F': out.push_back(ArgVal::Kind::Float); ++pos; break;
        case 'D': out.push_back(ArgVal::Kind::Double); ++pos; break;
        case 'L':
            if (desc.compare(pos, 18, "Ljava/lang/String;") == 0) {
                out.push_back(ArgVal::Kind::Str);
                pos += 18;
                break;
            }
            return false;
        default: return false;
        }
    }
    return pos < desc.size() && desc[pos] == ')';
}

/// 从 ids[headPos] 起按族逐参读取（virtual 接收者由调用方排在 kinds 首位）。
bool readArgs(jbc::ir::ClassSession& session, jbc::ir::CodeDom& dom,
              const std::vector<InsnId>& ids, ca::usize headPos,
              const std::vector<ArgVal::Kind>& kinds, std::vector<ArgVal>& out)
{
    out.clear();
    for (ca::usize k = 0; k < kinds.size(); ++k) {
        const Insn* in = dom.peek(ids[headPos + k]);
        if (in == nullptr) return false;
        ArgVal v;
        v.kind = kinds[k];
        switch (kinds[k]) {
        case ArgVal::Kind::Int: {
            ca::i32 n = 0;
            if (!method_rewrite_util::intValueOf(session, *in, n)) return false;
            v.num = n;
            break;
        }
        case ArgVal::Kind::Long: {
            ca::i64 n = 0;
            if (!method_rewrite_util::longValueOf(session, *in, n)) return false;
            v.num = n;
            break;
        }
        case ArgVal::Kind::Float: {
            float f = 0.0f;
            if (!method_rewrite_util::floatValueOf(session, *in, f)) return false;
            v.fnum = f;
            break;
        }
        case ArgVal::Kind::Double: {
            double d = 0.0;
            if (!method_rewrite_util::doubleValueOf(session, *in, d)) return false;
            v.fnum = d;
            break;
        }
        case ArgVal::Kind::Str:
            if (!method_rewrite_util::stringValueOf(session, *in, v.str)) return false;
            break;
        }
        out.push_back(v);
    }
    return true;
}

// ── 折叠结果与白名单求值 ───────────────────────────────────────────

struct FoldResult
{
    enum class Kind : ca::u8 { Int, Long, Float, Double, Str };
    Kind        kind = Kind::Int;
    ca::i64     num  = 0;
    double      fnum = 0.0;
    std::string str;
};

/// 白名单纯函数求值。owner/name/desc 为 CP 引用语义面，args 已按描述符
/// 参数族读齐（virtual 接收者在 args[0]）。任何族/域不合一律 false。
bool computeIntrinsic(std::string_view owner, std::string_view name,
                      std::string_view desc, bool isStatic,
                      const std::vector<ArgVal>& args, FoldResult& out)
{
    // String 接收者族（virtual；接收者为常量串）。
    if (!isStatic && owner == "java/lang/String" && !args.empty()
        && args[0].kind == ArgVal::Kind::Str) {
        const std::string_view s = args[0].str;
        if (name == "length" && desc == "()I") {
            out.kind = FoldResult::Kind::Int;
            out.num  = static_cast<ca::i64>(mutf8CodeUnitCount(s));
            return true;
        }
        if (name == "isEmpty" && desc == "()Z") {
            out.kind = FoldResult::Kind::Int;
            out.num  = s.empty() ? 1 : 0;
            return true;
        }
        if (name == "substring" && desc == "(II)Ljava/lang/String;" && args.size() == 3
            && args[1].kind == ArgVal::Kind::Int && args[2].kind == ArgVal::Kind::Int) {
            // 纯 ASCII 才折：字节号 == 码元号。非 ASCII 形态保守不折
            //（声明修正：旧栈按字节错折）。
            if (!isPureAscii(s)) return false;
            const ca::i64 start = args[1].num;
            const ca::i64 end   = args[2].num;
            if (start < 0 || end < start || end > static_cast<ca::i64>(s.size())) {
                return false;
            }
            out.kind = FoldResult::Kind::Str;
            out.str  = std::string(s.substr(static_cast<ca::usize>(start),
                                            static_cast<ca::usize>(end - start)));
            return true;
        }
        return false;
    }

    if (!isStatic) return false;

    // Math 静态族。
    if (owner == "java/lang/Math") {
        if (name == "abs" && args.size() == 1) {
            if (desc == "(I)I" && args[0].kind == ArgVal::Kind::Int) {
                const ca::i32 v = static_cast<ca::i32>(args[0].num);
                out             = {FoldResult::Kind::Int,
                                   v < 0 && v != std::numeric_limits<ca::i32>::min()
                                       ? -static_cast<ca::i64>(v)
                                       : v};
                return true;
            }
            if (desc == "(J)J" && args[0].kind == ArgVal::Kind::Long) {
                const ca::i64 v = args[0].num;
                out             = {FoldResult::Kind::Long,
                                   v < 0 && v != std::numeric_limits<ca::i64>::min()
                                       ? -v
                                       : v};
                return true;
            }
            if (desc == "(F)F" && args[0].kind == ArgVal::Kind::Float) {
                out = {FoldResult::Kind::Float, 0,
                       static_cast<double>(std::fabs(static_cast<float>(args[0].fnum)))};
                return true;
            }
            if (desc == "(D)D" && args[0].kind == ArgVal::Kind::Double) {
                out = {FoldResult::Kind::Double, 0, std::fabs(args[0].fnum)};
                return true;
            }
            return false;
        }
        if ((name == "max" || name == "min") && args.size() == 2) {
            const bool isMax = name == "max";
            if (desc == "(II)I" && args[0].kind == ArgVal::Kind::Int
                && args[1].kind == ArgVal::Kind::Int) {
                const ca::i32 a = static_cast<ca::i32>(args[0].num);
                const ca::i32 b = static_cast<ca::i32>(args[1].num);
                out             = {FoldResult::Kind::Int,
                                   isMax ? (std::max)(a, b) : (std::min)(a, b)};
                return true;
            }
            if (desc == "(JJ)J" && args[0].kind == ArgVal::Kind::Long
                && args[1].kind == ArgVal::Kind::Long) {
                out = {FoldResult::Kind::Long,
                       isMax ? (std::max)(args[0].num, args[1].num)
                             : (std::min)(args[0].num, args[1].num)};
                return true;
            }
            if (desc == "(FF)F" && args[0].kind == ArgVal::Kind::Float
                && args[1].kind == ArgVal::Kind::Float) {
                const float a = static_cast<float>(args[0].fnum);
                const float b = static_cast<float>(args[1].fnum);
                out           = {FoldResult::Kind::Float, 0,
                                 static_cast<double>(isMax ? javaFloatingMax(a, b)
                                                           : javaFloatingMin(a, b))};
                return true;
            }
            if (desc == "(DD)D" && args[0].kind == ArgVal::Kind::Double
                && args[1].kind == ArgVal::Kind::Double) {
                out = {FoldResult::Kind::Double, 0,
                       isMax ? javaFloatingMax(args[0].fnum, args[1].fnum)
                             : javaFloatingMin(args[0].fnum, args[1].fnum)};
                return true;
            }
            return false;
        }
        return false;
    }

    // Integer 静态族。
    if (owner == "java/lang/Integer") {
        if (name == "parseInt" && desc == "(Ljava/lang/String;)I" && args.size() == 1
            && args[0].kind == ArgVal::Kind::Str) {
            ca::i32 v = 0;
            if (!parseJavaIntStrict(args[0].str, v)) return false;
            out = {FoldResult::Kind::Int, v};
            return true;
        }
        if (name == "signum" && desc == "(I)I" && args.size() == 1
            && args[0].kind == ArgVal::Kind::Int) {
            const ca::i32 v = static_cast<ca::i32>(args[0].num);
            out             = {FoldResult::Kind::Int, v > 0 ? 1 : v < 0 ? -1 : 0};
            return true;
        }
        if (name == "rotateLeft" && desc == "(II)I" && args.size() == 2
            && args[0].kind == ArgVal::Kind::Int && args[1].kind == ArgVal::Kind::Int) {
            out = {FoldResult::Kind::Int,
                   javaIntRotateLeft(static_cast<ca::i32>(args[0].num),
                                     static_cast<ca::i32>(args[1].num))};
            return true;
        }
        if (name == "rotateRight" && desc == "(II)I" && args.size() == 2
            && args[0].kind == ArgVal::Kind::Int && args[1].kind == ArgVal::Kind::Int) {
            out = {FoldResult::Kind::Int,
                   javaIntRotateRight(static_cast<ca::i32>(args[0].num),
                                      static_cast<ca::i32>(args[1].num))};
            return true;
        }
        if (name == "toString" && desc == "(II)Ljava/lang/String;" && args.size() == 2
            && args[0].kind == ArgVal::Kind::Int && args[1].kind == ArgVal::Kind::Int) {
            ca::i64 radix = args[1].num;
            if (radix < 2 || radix > 36) radix = 10;
            const ca::i32 v = static_cast<ca::i32>(args[0].num);
            if (v == 0) {
                out = {FoldResult::Kind::Str, 0, 0.0, std::string("0")};
                return true;
            }
            static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
            const bool        neg      = v < 0;
            const ca::u64     t        = neg
                                             ? static_cast<ca::u64>(-static_cast<ca::i64>(v))
                                             : static_cast<ca::u64>(v);
            std::string r;
            for (ca::u64 x = t; x != 0; x /= static_cast<ca::u64>(radix)) {
                r.push_back(digits[x % static_cast<ca::u64>(radix)]);
            }
            if (neg) r.push_back('-');
            std::reverse(r.begin(), r.end());
            out = {FoldResult::Kind::Str, 0, 0.0, std::move(r)};
            return true;
        }
        if (args.size() == 1 && args[0].kind == ArgVal::Kind::Int) {
            const ca::i32 v = static_cast<ca::i32>(args[0].num);
            if (name == "reverse" && desc == "(I)I") {
                out = {FoldResult::Kind::Int, javaIntReverse(v)};
                return true;
            }
            if (name == "reverseBytes" && desc == "(I)I") {
                out = {FoldResult::Kind::Int, javaIntReverseBytes(v)};
                return true;
            }
            if (name == "bitCount" && desc == "(I)I") {
                out = {FoldResult::Kind::Int, javaIntBitCount(v)};
                return true;
            }
            if (name == "numberOfLeadingZeros" && desc == "(I)I") {
                out = {FoldResult::Kind::Int, javaIntNumberOfLeadingZeros(v)};
                return true;
            }
            if (name == "numberOfTrailingZeros" && desc == "(I)I") {
                out = {FoldResult::Kind::Int, javaIntNumberOfTrailingZeros(v)};
                return true;
            }
            if (name == "highestOneBit" && desc == "(I)I") {
                out = {FoldResult::Kind::Int, javaIntHighestOneBit(v)};
                return true;
            }
            if (name == "lowestOneBit" && desc == "(I)I") {
                out = {FoldResult::Kind::Int, javaIntLowestOneBit(v)};
                return true;
            }
        }
        // Integer.toBinaryString/toHexString/toOctalString/toString(I)。
        if ((name == "toBinaryString" || name == "toHexString"
             || name == "toOctalString" || name == "toString")
            && desc == "(I)Ljava/lang/String;" && args.size() == 1
            && args[0].kind == ArgVal::Kind::Int) {
            const ca::u32 x = static_cast<ca::u32>(static_cast<ca::i32>(args[0].num));
            std::string    r;
            if (name == "toBinaryString") {
                if (x == 0) { r = "0"; }
                else {
                    for (ca::u32 t = x; t != 0; t >>= 1) r.push_back(t & 1 ? '1' : '0');
                    std::reverse(r.begin(), r.end());
                }
            }
            else if (name == "toHexString") {
                if (x == 0) { r = "0"; }
                else {
                    static const char hex[] = "0123456789abcdef";
                    for (ca::u32 t = x; t != 0; t >>= 4) r.push_back(hex[t & 0xF]);
                    std::reverse(r.begin(), r.end());
                }
            }
            else if (name == "toOctalString") {
                if (x == 0) { r = "0"; }
                else {
                    for (ca::u32 t = x; t != 0; t >>= 3) {
                        r.push_back(static_cast<char>('0' + (t & 0x7)));
                    }
                    std::reverse(r.begin(), r.end());
                }
            }
            else {
                const ca::i32 v = static_cast<ca::i32>(args[0].num);
                if (v == 0) { r = "0"; }
                else {
                    const bool    neg = v < 0;
                    ca::u64 t = neg
                                    ? static_cast<ca::u64>(-static_cast<ca::i64>(v))
                                    : static_cast<ca::u64>(v);
                    while (t != 0) {
                        r.push_back(static_cast<char>('0' + (t % 10)));
                        t /= 10;
                    }
                    if (neg) r.push_back('-');
                    std::reverse(r.begin(), r.end());
                }
            }
            out = {FoldResult::Kind::Str, 0, 0.0, std::move(r)};
            return true;
        }
        return false;
    }

    // Long 静态族。
    if (owner == "java/lang/Long") {
        if (name == "parseLong" && desc == "(Ljava/lang/String;)J" && args.size() == 1
            && args[0].kind == ArgVal::Kind::Str) {
            ca::i64 v = 0;
            if (!parseJavaLongStrict(args[0].str, v)) return false;
            out = {FoldResult::Kind::Long, v};
            return true;
        }
        if (name == "signum" && desc == "(J)I" && args.size() == 1
            && args[0].kind == ArgVal::Kind::Long) {
            const ca::i64 v = args[0].num;
            out             = {FoldResult::Kind::Int, v > 0 ? 1 : v < 0 ? -1 : 0};
            return true;
        }
        if (name == "rotateLeft" && desc == "(JI)J" && args.size() == 2
            && args[0].kind == ArgVal::Kind::Long && args[1].kind == ArgVal::Kind::Int) {
            out = {FoldResult::Kind::Long,
                   javaLongRotateLeft(args[0].num, static_cast<ca::i32>(args[1].num))};
            return true;
        }
        if (name == "rotateRight" && desc == "(JI)J" && args.size() == 2
            && args[0].kind == ArgVal::Kind::Long && args[1].kind == ArgVal::Kind::Int) {
            out = {FoldResult::Kind::Long,
                   javaLongRotateRight(args[0].num, static_cast<ca::i32>(args[1].num))};
            return true;
        }
        if (name == "toString" && desc == "(JI)Ljava/lang/String;" && args.size() == 2
            && args[0].kind == ArgVal::Kind::Long && args[1].kind == ArgVal::Kind::Int) {
            ca::i64 radix = args[1].num;
            if (radix < 2 || radix > 36) radix = 10;
            ca::i64     v   = args[0].num;
            if (v == 0) { out = {FoldResult::Kind::Str, 0, 0.0, std::string("0")}; return true; }
            static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
            const bool        neg      = v < 0;
            ca::u64 t = neg ? static_cast<ca::u64>(-(v + 1)) + 1 : static_cast<ca::u64>(v);
            std::string r;
            while (t != 0) {
                r.push_back(digits[t % static_cast<ca::u64>(radix)]);
                t /= static_cast<ca::u64>(radix);
            }
            if (neg) r.push_back('-');
            std::reverse(r.begin(), r.end());
            out = {FoldResult::Kind::Str, 0, 0.0, std::move(r)};
            return true;
        }
        if (args.size() == 1 && args[0].kind == ArgVal::Kind::Long) {
            const ca::i64 v = args[0].num;
            if (name == "reverse" && desc == "(J)J") {
                out = {FoldResult::Kind::Long, javaLongReverse(v)};
                return true;
            }
            if (name == "reverseBytes" && desc == "(J)J") {
                out = {FoldResult::Kind::Long, javaLongReverseBytes(v)};
                return true;
            }
            if (name == "bitCount" && desc == "(J)I") {
                out = {FoldResult::Kind::Int, javaLongBitCount(v)};
                return true;
            }
            if (name == "numberOfLeadingZeros" && desc == "(J)I") {
                out = {FoldResult::Kind::Int, javaLongNumberOfLeadingZeros(v)};
                return true;
            }
            if (name == "numberOfTrailingZeros" && desc == "(J)I") {
                out = {FoldResult::Kind::Int, javaLongNumberOfTrailingZeros(v)};
                return true;
            }
            if (name == "highestOneBit" && desc == "(J)J") {
                out = {FoldResult::Kind::Long, javaLongHighestOneBit(v)};
                return true;
            }
            if (name == "lowestOneBit" && desc == "(J)J") {
                out = {FoldResult::Kind::Long, javaLongLowestOneBit(v)};
                return true;
            }
            if ((name == "toBinaryString" || name == "toHexString"
                 || name == "toOctalString" || name == "toString")
                && desc == "(J)Ljava/lang/String;") {
                const ca::u64 x = static_cast<ca::u64>(v);
                std::string   r;
                if (x == 0) { r = "0"; }
                else {
                    int              shift = 0;
                    static const char hex[] = "0123456789abcdef";
                    if (name == "toBinaryString") shift = 1;
                    else if (name == "toHexString") shift = 4;
                    else if (name == "toOctalString") shift = 3;
                    if (shift != 0) {
                        for (ca::u64 t = x; t != 0; t >>= static_cast<ca::u64>(shift)) {
                            r.push_back(shift == 1 ? (t & 1 ? '1' : '0')
                                                   : shift == 4 ? hex[t & 0xF]
                                                                : static_cast<char>('0' + (t & 0x7)));
                        }
                        std::reverse(r.begin(), r.end());
                    }
                    else {
                        const bool neg = v < 0;
                        ca::u64 t = neg ? static_cast<ca::u64>(-(v + 1)) + 1 : x;
                        while (t != 0) {
                            r.push_back(static_cast<char>('0' + (t % 10)));
                            t /= 10;
                        }
                        if (neg) r.push_back('-');
                        std::reverse(r.begin(), r.end());
                    }
                }
                out = {FoldResult::Kind::Str, 0, 0.0, std::move(r)};
                return true;
            }
        }
        return false;
    }

    return false;
}

/// 折叠结果物化为替换指令（头改写时 srcPc 影线保留，不取 mat.srcPc）。
Insn materializeResult(jbc::ir::ClassSession& session, const FoldResult& r)
{
    Insn mat;
    switch (r.kind) {
    case FoldResult::Kind::Int:
        mat = method_rewrite_util::materializeIntConst(session,
                                                       static_cast<ca::i32>(r.num));
        break;
    case FoldResult::Kind::Long:
        mat = method_rewrite_util::materializeLongConst(session, r.num);
        break;
    case FoldResult::Kind::Float:
        mat = method_rewrite_util::materializeFloatConst(session,
                                                         static_cast<float>(r.fnum));
        break;
    case FoldResult::Kind::Double:
        mat = method_rewrite_util::materializeDoubleConst(session, r.fnum);
        break;
    case FoldResult::Kind::Str:
        mat.op    = Op::LDC;
        mat.cp    = session.internString(r.str);
        mat.srcPc = jbc::ir::kNoPc;
        break;
    }
    return mat;
}

// ── 三清扫 ─────────────────────────────────────────────────────────

/// 装箱/拆箱对清扫：INVOKESTATIC Box.valueOf(p) + INVOKEVIRTUAL
/// Box.pValue() 相邻窗整对删除。
bool tryBoxUnbox(jbc::ir::ClassSession& session, jbc::ir::CodeDom& dom,
                 jbc::ir::CodeEditor& editor, const std::vector<InsnId>& ids)
{
    for (ca::usize i = 0; i + 1 < ids.size(); ++i) {
        const Insn* a = dom.peek(ids[i]);
        const Insn* b = dom.peek(ids[i + 1]);
        if (a == nullptr || b == nullptr) continue;
        if (a->op != Op::INVOKESTATIC || b->op != Op::INVOKEVIRTUAL) continue;
        jbc::ir::ClassSession::MemberSem sa;
        jbc::ir::ClassSession::MemberSem sb;
        if (!session.memberOf(*a, sa) || !session.memberOf(*b, sb)) continue;
        for (const BoxPair& p : kBoxPairs) {
            if (sa.owner != p.owner || sa.name != "valueOf" || sa.desc != p.valueOfDesc) {
                continue;
            }
            if (sb.owner != p.owner || sb.name != p.unboxName || sb.desc != p.unboxDesc) {
                break;   // owner 已命中：name/desc 不配即失配，勿再试他族
            }
            // 拆箱前置边界锚定 → 外部路径持 Integer 进入，删除使其失去消费者。
            if (method_rewrite_util::labelsAnchorAt(dom, ids[i + 1])) break;
            return eraseWindow(editor, ids, i, i + 1);
        }
    }
    return false;
}

/// 种子随机清扫：NEW Random; DUP; 常量种子; INVOKESPECIAL <init>(J)V;
/// INVOKEVIRTUAL nextXxx 五窗折叠为结果常量（LCG 权威 jstd::Random）。
bool tryRandom(jbc::ir::ClassSession& session, jbc::ir::CodeDom& dom,
               jbc::ir::CodeEditor& editor, const std::vector<InsnId>& ids)
{
    if (ids.size() < 5) return false;
    for (ca::usize i = 0; i + 4 < ids.size(); ++i) {
        const Insn* m0 = dom.peek(ids[i]);
        const Insn* m1 = dom.peek(ids[i + 1]);
        const Insn* m2 = dom.peek(ids[i + 2]);
        const Insn* m3 = dom.peek(ids[i + 3]);
        const Insn* m4 = dom.peek(ids[i + 4]);
        if (m0 == nullptr || m1 == nullptr || m2 == nullptr || m3 == nullptr
            || m4 == nullptr) {
            continue;
        }
        if (m0->op != Op::NEW) continue;
        if (session.classOf(*m0) != "java/util/Random") continue;
        if (m1->op != Op::DUP) continue;
        ca::i64 seed = 0;
        if (!method_rewrite_util::longValueOf(session, *m2, seed)) continue;
        jbc::ir::ClassSession::MemberSem si;
        jbc::ir::ClassSession::MemberSem sc;
        if (m3->op != Op::INVOKESPECIAL || !session.memberOf(*m3, si)) continue;
        if (si.owner != "java/util/Random" || si.name != "<init>" || si.desc != "(J)V") {
            continue;
        }
        if (m4->op != Op::INVOKEVIRTUAL || !session.memberOf(*m4, sc)) continue;
        if (sc.owner != "java/util/Random") continue;

        Insn mat;
        if (sc.name == "nextInt" && sc.desc == "()I") {
            mat = method_rewrite_util::materializeIntConst(session,
                                                           jstd::Random(seed).nextInt());
        }
        else if (sc.name == "nextLong" && sc.desc == "()J") {
            mat = method_rewrite_util::materializeLongConst(session,
                                                            jstd::Random(seed).nextLong());
        }
        else if (sc.name == "nextFloat" && sc.desc == "()F") {
            mat = method_rewrite_util::materializeFloatConst(
                session, jstd::Random(seed).nextFloat());
        }
        else if (sc.name == "nextDouble" && sc.desc == "()D") {
            mat = method_rewrite_util::materializeDoubleConst(
                session, jstd::Random(seed).nextDouble());
        }
        else if (sc.name == "nextBoolean" && sc.desc == "()Z") {
            mat = method_rewrite_util::materializeIntConst(session,
                                                           jstd::Random(seed).next(1));
        }
        else {
            continue;
        }
        // DUP 与种子、构造、消费的前置边界锚定 → 整窗拒收（物化前判，
        // 不污染常量池）。
        if (anyInteriorAnchored(dom, ids, i + 1, i + 4)) continue;
        if (!foldWindow(dom, editor, ids, i, i + 4, mat)) return false;
        return true;
    }
    return false;
}

/// 白名单纯函数清扫：参数全常量的 INVOKESTATIC / INVOKEVIRTUAL 窗折叠为
/// 结果常量；String.toCharArray + String.valueOf([C) 恒等对整对删除。
bool tryStdCons(jbc::ir::ClassSession& session, jbc::ir::CodeDom& dom,
                jbc::ir::CodeEditor& editor, const std::vector<InsnId>& ids)
{
    std::vector<ArgVal::Kind> kinds;
    std::vector<ArgVal::Kind> full;
    std::vector<ArgVal>       args;
    if (ids.size() < 2) return false;
    for (ca::usize i = 1; i < ids.size(); ++i) {
        const Insn* inv = dom.peek(ids[i]);
        if (inv == nullptr) continue;
        if (inv->op != Op::INVOKESTATIC && inv->op != Op::INVOKEVIRTUAL) continue;
        jbc::ir::ClassSession::MemberSem sem;
        if (!session.memberOf(*inv, sem)) continue;
        const bool isStatic = inv->op == Op::INVOKESTATIC;

        // toCharArray + valueOf([C) 恒等对：内容等价于接收者生产者本身，
        // 整对删除（接收者生产者留在窗前）。
        if (isStatic && sem.owner == "java/lang/String" && sem.name == "valueOf"
            && sem.desc == "([C)Ljava/lang/String;") {
            const Insn* prev = dom.peek(ids[i - 1]);
            jbc::ir::ClassSession::MemberSem ps;
            if (prev != nullptr && prev->op == Op::INVOKEVIRTUAL
                && session.memberOf(*prev, ps) && ps.owner == "java/lang/String"
                && ps.name == "toCharArray" && ps.desc == "()[C") {
                // valueOf 前置边界锚定 → 接收者生产者外的进入路径失配。
                if (method_rewrite_util::labelsAnchorAt(dom, ids[i])) continue;
                return eraseWindow(editor, ids, i - 1, i);
            }
            continue;
        }

        if (!isStatic && sem.owner != "java/lang/String") continue;
        if (!paramKindsOf(sem.desc, kinds)) continue;
        const ca::usize argc = kinds.size() + (isStatic ? 0 : 1);
        if (argc == 0 || i < argc) continue;
        const ca::usize headPos = i - argc;
        // 窗口内部边界锚定（含 invoke 自身）→ 外部路径持部分实参进入，拒收。
        if (anyInteriorAnchored(dom, ids, headPos + 1, i)) continue;
        full = kinds;
        if (!isStatic) full.insert(full.begin(), ArgVal::Kind::Str);
        if (!readArgs(session, dom, ids, headPos, full, args)) continue;
        FoldResult r;
        if (!computeIntrinsic(sem.owner, sem.name, sem.desc, isStatic, args, r)) continue;
        if (!foldWindow(dom, editor, ids, headPos, i, materializeResult(session, r))) {
            return false;
        }
        return true;
    }
    return false;
}

}   // namespace

mjbase::spi::PreservedAnalyses IntrinsicFolding::run(mjbase::spi::MethodScope&       scope,
                                                     mjbase::spi::MethodAnalysisManager& mam)
{
    (void)mam;
    jbc::ir::ClassSession& session = scope.session;
    jbc::ir::CodeDom&      dom     = scope.code.dom;
    jbc::ir::CodeEditor    editor(scope.code);

    bool modified = false;
    bool any      = true;
    while (any) {
        any = false;
        // 每轮快照：折叠只发生在单窗，快照句柄全程有效；折叠暴露的新窗
        // 由下一轮快照接续。
        const std::vector<InsnId> ids = dom.order();
        if (tryBoxUnbox(session, dom, editor, ids)) {
            modified = any = true;
            continue;
        }
        if (tryRandom(session, dom, editor, ids)) {
            modified = any = true;
            continue;
        }
        if (tryStdCons(session, dom, editor, ids)) {
            modified = any = true;
            continue;
        }
    }
    return modified ? mjbase::spi::PreservedAnalyses::none()
                    : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
