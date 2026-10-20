/// @file        string_flow.cpp
/// @brief       字符串/常量流分析实现：流格 + 到达帧上的闭环求值 + 逃逸扫描。
#include "string_flow.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "libca/crypto/base64.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/code_dom.hpp"

#include "mjbase/asm/insn/insn_op.hpp"
#include "mjbase/keys.hpp"

#include "const_domain.hpp"

namespace mjbase::asm_analyzer {

namespace {

using ca::i32;
using ca::u16;
using ca::u32;
using ca::u8;
using ca::usize;
using jbc::analysis::DomainResult;
using jbc::analysis::FrameV;
using jbc::analysis::SlotV;
using jbc::analysis::VKind;
using jbc::core::ConstantPool;
using jbc::core::CpTag;
using jbc::core::Op;
using jbc::ir::CodeUnit;
using jbc::ir::Insn;
using jbc::ir::InsnId;

constexpr std::string_view kStringOwner = "java/lang/String";
constexpr std::string_view kBuilderOwner = "java/lang/StringBuilder";
constexpr std::string_view kBufferOwner = "java/lang/StringBuffer";
constexpr std::string_view kAbsBuilderOwner = "java/lang/AbstractStringBuilder";
constexpr std::string_view kBase64Owner = "java/util/Base64";
constexpr std::string_view kBase64DecoderOwner = "java/util/Base64$Decoder";
constexpr std::string_view kStandardCharsetsOwner = "java/nio/charset/StandardCharsets";

constexpr usize kMaxTextUnits = 1u << 20;   ///< 字符串/数组求值规模上限
constexpr i32 kMaxArrayLength = 1 << 20;

/// newarray 的 atype 码（JVMS 6.5 newarray）。
constexpr i32 kAtypeChar = 5;
constexpr i32 kAtypeByte = 8;

struct MethodRef
{
    std::string_view owner;
    std::string_view name;
    std::string_view desc;
    bool ok = false;
};

MethodRef methodRefOf(const ConstantPool& cp, u32 cpIdx)
{
    MethodRef m;
    if (!cp.valid(cpIdx)) {
        return m;
    }
    const CpTag tag = cp.tagOf(cpIdx);
    if (tag != CpTag::Methodref && tag != CpTag::InterfaceMethodref &&
        tag != CpTag::InvokeDynamic) {
        return m;
    }
    bool descOk = false;
    bool nameOk = false;
    bool ownerOk = false;
    m.desc = jbc::analysis::detail::memberDesc(cp, cpIdx, descOk);
    m.name = jbc::analysis::detail::memberName(cp, cpIdx, nameOk);
    m.owner = jbc::analysis::detail::refOwnerOf(cp, cpIdx, ownerOk);
    m.ok = descOk && nameOk && ownerOk && !m.owner.empty() && !m.name.empty();
    return m;
}

MethodRef fieldRefOf(const ConstantPool& cp, u32 cpIdx)
{
    MethodRef m;
    if (!cp.valid(cpIdx) || cp.tagOf(cpIdx) != CpTag::Fieldref) {
        return m;
    }
    bool descOk = false;
    bool nameOk = false;
    bool ownerOk = false;
    m.desc = jbc::analysis::detail::memberDesc(cp, cpIdx, descOk);
    m.name = jbc::analysis::detail::memberName(cp, cpIdx, nameOk);
    m.owner = jbc::analysis::detail::refOwnerOf(cp, cpIdx, ownerOk);
    m.ok = descOk && nameOk && ownerOk && !m.owner.empty() && !m.name.empty();
    return m;
}

std::vector<std::string_view> paramsOf(std::string_view desc)
{
    std::vector<std::string_view> out;
    if (desc.empty() || desc.front() != '(') {
        return out;
    }
    usize pos = 1;
    while (pos < desc.size() && desc[pos] != ')') {
        const std::string_view t = jbc::analysis::detail::oneType(desc, pos);
        if (t.empty()) {
            return {};
        }
        out.push_back(t);
        pos += t.size();
    }
    return out;
}

/// 方法返回类型是否为数组（描述符 ')' 后首字符 '['）。
bool returnsArray(std::string_view desc) noexcept
{
    const usize close = desc.find(')');
    return close != std::string_view::npos && close + 1 < desc.size() &&
           desc[close + 1] == '[';
}

bool isBuilderOwner(std::string_view owner) noexcept
{
    return owner == kBuilderOwner || owner == kBufferOwner || owner == kAbsBuilderOwner;
}

bool isBuilderMutationName(std::string_view name) noexcept
{
    return name == "append" || name == "insert" || name == "reverse" ||
           name == "setLength" || name == "setCharAt" || name == "delete" ||
           name == "deleteCharAt" || name == "replace";
}

bool isStringProducerName(std::string_view name) noexcept
{
    return name == "valueOf" || name == "copyValueOf" || name == "concat" ||
           name == "toCharArray" || name == "getBytes" || name == "intern" ||
           name == "toString";
}

bool isNeutralUse(Op op) noexcept
{
    switch (op) {
    case Op::NOP:
    case Op::ACONST_NULL:
    case Op::ILOAD:
    case Op::LLOAD:
    case Op::FLOAD:
    case Op::DLOAD:
    case Op::ALOAD:
    case Op::ILOAD_0:
    case Op::ILOAD_1:
    case Op::ILOAD_2:
    case Op::ILOAD_3:
    case Op::LLOAD_0:
    case Op::LLOAD_1:
    case Op::LLOAD_2:
    case Op::LLOAD_3:
    case Op::FLOAD_0:
    case Op::FLOAD_1:
    case Op::FLOAD_2:
    case Op::FLOAD_3:
    case Op::DLOAD_0:
    case Op::DLOAD_1:
    case Op::DLOAD_2:
    case Op::DLOAD_3:
    case Op::ALOAD_0:
    case Op::ALOAD_1:
    case Op::ALOAD_2:
    case Op::ALOAD_3:
    case Op::ISTORE:
    case Op::LSTORE:
    case Op::FSTORE:
    case Op::DSTORE:
    case Op::ASTORE:
    case Op::ISTORE_0:
    case Op::ISTORE_1:
    case Op::ISTORE_2:
    case Op::ISTORE_3:
    case Op::LSTORE_0:
    case Op::LSTORE_1:
    case Op::LSTORE_2:
    case Op::LSTORE_3:
    case Op::FSTORE_0:
    case Op::FSTORE_1:
    case Op::FSTORE_2:
    case Op::FSTORE_3:
    case Op::DSTORE_0:
    case Op::DSTORE_1:
    case Op::DSTORE_2:
    case Op::DSTORE_3:
    case Op::ASTORE_0:
    case Op::ASTORE_1:
    case Op::ASTORE_2:
    case Op::ASTORE_3:
    case Op::POP:
    case Op::POP2:
    case Op::DUP:
    case Op::DUP_X1:
    case Op::DUP_X2:
    case Op::DUP2:
    case Op::DUP2_X1:
    case Op::DUP2_X2:
    case Op::SWAP:
    case Op::CHECKCAST:
    case Op::INSTANCEOF:
    case Op::IFNULL:
    case Op::IFNONNULL:
    case Op::IF_ACMPEQ:
    case Op::IF_ACMPNE:
    case Op::GOTO:
    case Op::GOTO_W:
    case Op::ARRAYLENGTH:
    case Op::IALOAD:
    case Op::LALOAD:
    case Op::FALOAD:
    case Op::DALOAD:
    case Op::AALOAD:
    case Op::BALOAD:
    case Op::CALOAD:
    case Op::SALOAD:
    case Op::IASTORE:
    case Op::LASTORE:
    case Op::FASTORE:
    case Op::DASTORE:
    case Op::AASTORE:
    case Op::BASTORE:
    case Op::CASTORE:
    case Op::SASTORE: return true;
    default: return false;
    }
}

/// 逃逸扫描的中性集：数组元素读写对字符串引用是真实使用（写入即逃逸），
/// 不能沿用内存槽迁移视角的中性判定。
bool isEscapeNeutral(Op op) noexcept
{
    switch (op) {
    case Op::IALOAD:
    case Op::LALOAD:
    case Op::FALOAD:
    case Op::DALOAD:
    case Op::AALOAD:
    case Op::BALOAD:
    case Op::CALOAD:
    case Op::SALOAD:
    case Op::IASTORE:
    case Op::LASTORE:
    case Op::FASTORE:
    case Op::DASTORE:
    case Op::AASTORE:
    case Op::BASTORE:
    case Op::CASTORE:
    case Op::SASTORE:
    case Op::ARRAYLENGTH: return false;
    default: return isNeutralUse(op);
    }
}

bool isLdcString(const ConstantPool& cp, const Insn& in) noexcept
{
    if (in.op != Op::LDC && in.op != Op::LDC_W) {
        return false;
    }
    return cp.valid(in.cp) && cp.tagOf(in.cp) == CpTag::String;
}

/// modified UTF-8 → UTF-16；兼容标准 UTF-8 的四字节序列。
bool decodeMutf8(std::string_view in, std::u16string& out)
{
    out.clear();
    out.reserve(in.size());
    usize i = 0;
    while (i < in.size()) {
        const u8 b = static_cast<u8>(in[i]);
        if (b < 0x80) {
            out.push_back(b);
            ++i;
            continue;
        }
        if ((b & 0xE0) == 0xC0) {
            if (i + 1 >= in.size()) {
                return false;
            }
            const u8 b1 = static_cast<u8>(in[i + 1]);
            if ((b1 & 0xC0) != 0x80) {
                return false;
            }
            const u16 cp = static_cast<u16>(((b & 0x1F) << 6) | (b1 & 0x3F));
            if (cp != 0 && cp < 0x80) {
                return false;   // 非 NUL 的 overlong
            }
            out.push_back(cp);
            i += 2;
            continue;
        }
        if ((b & 0xF0) == 0xE0) {
            if (i + 2 >= in.size()) {
                return false;
            }
            const u8 b1 = static_cast<u8>(in[i + 1]);
            const u8 b2 = static_cast<u8>(in[i + 2]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
                return false;
            }
            const u16 cp = static_cast<u16>(((b & 0x0F) << 12) | ((b1 & 0x3F) << 6) |
                                            (b2 & 0x3F));
            if (cp < 0x800) {
                return false;
            }
            out.push_back(cp);
            i += 3;
            continue;
        }
        if ((b & 0xF8) == 0xF0) {
            if (i + 3 >= in.size()) {
                return false;
            }
            const u8 b1 = static_cast<u8>(in[i + 1]);
            const u8 b2 = static_cast<u8>(in[i + 2]);
            const u8 b3 = static_cast<u8>(in[i + 3]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
                return false;
            }
            const u32 cp = (static_cast<u32>(b & 0x07) << 18) |
                           (static_cast<u32>(b1 & 0x3F) << 12) |
                           (static_cast<u32>(b2 & 0x3F) << 6) | (b3 & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) {
                return false;
            }
            const u32 v = cp - 0x10000;
            out.push_back(static_cast<u16>(0xD800 + (v >> 10)));
            out.push_back(static_cast<u16>(0xDC00 + (v & 0x3FF)));
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

/// UTF-16 → modified UTF-8（每个 UTF-16 码元独立编码，代理对呈 CESU-8）。
std::string encodeMutf8(std::u16string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (const u16 c : in) {
        if (c == 0) {
            out.push_back(static_cast<char>(0xC0));
            out.push_back(static_cast<char>(0x80));
        } else if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

enum class CharsetCode : u8 { Unknown = 0, Utf8, Ascii, Latin1 };

CharsetCode charsetFromName(std::u16string_view name) noexcept
{
    std::string s;
    s.reserve(name.size());
    for (const u16 c : name) {
        if (c >= 0x80) {
            return CharsetCode::Unknown;
        }
        char ch = static_cast<char>(c);
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
        s.push_back(ch);
    }
    if (s == "utf-8" || s == "utf8" || s == "unicode-1-1-utf-8") {
        return CharsetCode::Utf8;
    }
    if (s == "us-ascii" || s == "ascii" || s == "ansi_x3.4-1968" || s == "646" ||
        s == "iso-ir-6") {
        return CharsetCode::Ascii;
    }
    if (s == "iso-8859-1" || s == "iso8859-1" || s == "iso_8859-1" || s == "latin1" ||
        s == "8859_1" || s == "819") {
        return CharsetCode::Latin1;
    }
    return CharsetCode::Unknown;
}

/// 严格标准 UTF-8 字节 → UTF-16（非法序列返回 false，不替换、不猜）。
bool decodeUtf8(const std::vector<u8>& bytes, std::u16string& out)
{
    out.clear();
    out.reserve(bytes.size());
    usize i = 0;
    while (i < bytes.size()) {
        const u8 b = bytes[i];
        if (b < 0x80) {
            out.push_back(b);
            ++i;
            continue;
        }
        if ((b & 0xE0) == 0xC0) {
            if (i + 1 >= bytes.size() || (bytes[i + 1] & 0xC0) != 0x80) {
                return false;
            }
            const u16 cp = static_cast<u16>(((b & 0x1F) << 6) | (bytes[i + 1] & 0x3F));
            if (cp < 0x80) {
                return false;
            }
            out.push_back(cp);
            i += 2;
            continue;
        }
        if ((b & 0xF0) == 0xE0) {
            if (i + 2 >= bytes.size() || (bytes[i + 1] & 0xC0) != 0x80 ||
                (bytes[i + 2] & 0xC0) != 0x80) {
                return false;
            }
            const u16 cp = static_cast<u16>(((b & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) |
                                            (bytes[i + 2] & 0x3F));
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
                return false;
            }
            out.push_back(cp);
            i += 3;
            continue;
        }
        if ((b & 0xF8) == 0xF0) {
            if (i + 3 >= bytes.size() || (bytes[i + 1] & 0xC0) != 0x80 ||
                (bytes[i + 2] & 0xC0) != 0x80 || (bytes[i + 3] & 0xC0) != 0x80) {
                return false;
            }
            const u32 cp = (static_cast<u32>(b & 0x07) << 18) |
                           (static_cast<u32>(bytes[i + 1] & 0x3F) << 12) |
                           (static_cast<u32>(bytes[i + 2] & 0x3F) << 6) |
                           (bytes[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) {
                return false;
            }
            const u32 v = cp - 0x10000;
            out.push_back(static_cast<u16>(0xD800 + (v >> 10)));
            out.push_back(static_cast<u16>(0xDC00 + (v & 0x3FF)));
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

/// UTF-16 → 标准 UTF-8；孤立代理返回 false。
bool encodeUtf8(std::u16string_view text, std::vector<u8>& out)
{
    out.clear();
    out.reserve(text.size());
    for (usize i = 0; i < text.size(); ++i) {
        const u16 c = text[i];
        u32 cp = c;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= text.size() || text[i + 1] < 0xDC00 || text[i + 1] > 0xDFFF) {
                return false;
            }
            cp = 0x10000 + ((static_cast<u32>(c - 0xD800) << 10) | (text[i + 1] - 0xDC00));
            ++i;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            return false;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<u8>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<u8>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<u8>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<u8>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<u8>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<u8>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<u8>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<u8>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<u8>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<u8>(0x80 | (cp & 0x3F)));
        }
    }
    return true;
}

bool decodeCharset(const std::vector<u8>& bytes, CharsetCode cs, std::u16string& out)
{
    switch (cs) {
    case CharsetCode::Utf8: return decodeUtf8(bytes, out);
    case CharsetCode::Ascii:
        out.clear();
        for (const u8 b : bytes) {
            if (b >= 0x80) {
                return false;
            }
            out.push_back(b);
        }
        return true;
    case CharsetCode::Latin1:
        out.clear();
        out.reserve(bytes.size());
        for (const u8 b : bytes) {
            out.push_back(b);
        }
        return true;
    case CharsetCode::Unknown: break;
    }
    return false;
}

bool encodeCharset(std::u16string_view text, CharsetCode cs, std::vector<u8>& out)
{
    switch (cs) {
    case CharsetCode::Utf8: return encodeUtf8(text, out);
    case CharsetCode::Ascii:
        out.clear();
        for (const u16 c : text) {
            if (c >= 0x80) {
                return false;
            }
            out.push_back(static_cast<u8>(c));
        }
        return true;
    case CharsetCode::Latin1:
        out.clear();
        for (const u16 c : text) {
            if (c > 0xFF) {
                return false;
            }
            out.push_back(static_cast<u8>(c));
        }
        return true;
    case CharsetCode::Unknown: break;
    }
    return false;
}

/// Base64 解码（标准/URL-safe，允许缺省 padding；其余严格交给 libca_crypto）。
bool decodeBase64(const std::u16string& text, bool urlSafe, std::vector<u8>& out)
{
    std::string ascii;
    ascii.reserve(text.size() + 3);
    for (const u16 c : text) {
        if (c >= 0x80) {
            return false;
        }
        char ch = static_cast<char>(c);
        if (urlSafe) {
            if (ch == '-') {
                ch = '+';
            } else if (ch == '_') {
                ch = '/';
            }
        }
        ascii.push_back(ch);
    }
    while (ascii.size() % 4 != 0) {
        ascii.push_back('=');
    }
    auto decoded = ca::crypto::base64_decode(ascii);
    if (decoded.is_err()) {
        return false;
    }
    auto bytes = std::move(decoded).unwrap();
    out.assign(bytes.as_ptr(), bytes.as_ptr() + bytes.len());
    return true;
}

// ---------------------------------------------------------------------------
// 流格：只记来源令牌，不承载字符串内容
// ---------------------------------------------------------------------------

struct StringFlowValue
{
    enum class K : u8 {
        Top = 0,          ///< 未知
        Int,              ///< int 常量（char 填充/循环下标的可求值输入）
        Literal,          ///< ldc String（cp + 载入点）
        Builder,          ///< new StringBuilder/StringBuffer（分配点）
        NewString,        ///< new String（未初始化，<init> 翻转后成 Call）
        Array,            ///< new char[]/byte[]（分配点）
        Call,             ///< 已识别调用的产出点
        Elem,             ///< 数组动态读（src = 数组来源令牌）
        Xform,            ///< 逐元素变换表达式（src = 数组来源，i = 常量，xop = 运算）
        Charset,          ///< StandardCharsets 常量（i = CharsetCode）
        Decoder,          ///< Base64.getDecoder/getUrlDecoder（i = URL-safe）
    };

    K k = K::Top;
    u8 slots = 1;
    i32 i = 0;             ///< Int 值 / Xform 常量 / Charset 码 / Decoder 标志
    u32 cp = 0;            ///< Literal 的 CP String 索引
    InsnId site;           ///< 产出点（Literal 载入点 / 分配点 / 调用点）
    K srcKind = K::Top;    ///< Elem/Xform 的数组来源令牌类别
    InsnId src;            ///< Elem/Xform 的数组来源点
    u8 xop = 0;            ///< Xform 的 Op 码

    static StringFlowValue topOf(u8 width = 1) noexcept
    {
        StringFlowValue v;
        v.slots = width;
        return v;
    }
    static StringFlowValue ofInt(i32 value) noexcept
    {
        StringFlowValue v;
        v.k = K::Int;
        v.i = value;
        return v;
    }
    static StringFlowValue ofLiteral(u32 cpIdx, InsnId at) noexcept
    {
        StringFlowValue v;
        v.k = K::Literal;
        v.cp = cpIdx;
        v.site = at;
        return v;
    }
    static StringFlowValue ofSite(K kind, InsnId at) noexcept
    {
        StringFlowValue v;
        v.k = kind;
        v.site = at;
        return v;
    }
    static StringFlowValue ofElem(K srcKind, InsnId srcSite) noexcept
    {
        StringFlowValue v;
        v.k = K::Elem;
        v.srcKind = srcKind;
        v.src = srcSite;
        return v;
    }
    static StringFlowValue ofXform(Op op, i32 key, K srcKind, InsnId srcSite) noexcept
    {
        StringFlowValue v;
        v.k = K::Xform;
        v.i = key;
        v.xop = static_cast<u8>(op);
        v.srcKind = srcKind;
        v.src = srcSite;
        return v;
    }
    static StringFlowValue ofSmall(K kind, i32 value) noexcept
    {
        StringFlowValue v;
        v.k = kind;
        v.i = value;
        return v;
    }
};

bool sameToken(const StringFlowValue& a, const StringFlowValue& b) noexcept
{
    return a.k == b.k && a.site.slot == b.site.slot && a.cp == b.cp;
}

/// 候选字符串来源的令牌键（逃逸扫描用）。
struct TokenKey
{
    StringFlowValue::K k = StringFlowValue::K::Top;
    u32 site = 0;
    u32 cp = 0;

    bool operator==(const TokenKey& o) const noexcept
    {
        return k == o.k && site == o.site && cp == o.cp;
    }
};

struct TokenKeyHash
{
    usize operator()(const TokenKey& key) const noexcept
    {
        usize h = static_cast<usize>(key.k) * 0x9E3779B1u;
        h ^= static_cast<usize>(key.site) + 0x9E3779B9u + (h << 6) + (h >> 2);
        h ^= static_cast<usize>(key.cp) + 0x85EBCA6Bu + (h << 6) + (h >> 2);
        return h;
    }
};

TokenKey tokenKeyOf(const StringFlowValue& v) noexcept
{
    return TokenKey{v.k, v.site.slot, v.cp};
}

class StringFlowDomain final : public jbc::analysis::IValueDomain<StringFlowValue>
{
public:
    explicit StringFlowDomain(const ConstantPool& cp) noexcept
        : cp_(&cp)
        , numbers_(cp)
    {}

    StringFlowValue top() override { return StringFlowValue::topOf(); }

    StringFlowValue merge(const StringFlowValue& a, const StringFlowValue& b) override
    {
        if (a.k == StringFlowValue::K::Top) {
            return StringFlowValue::topOf(joinSlots(a.slots, b.slots));
        }
        if (b.k == StringFlowValue::K::Top) {
            return StringFlowValue::topOf(joinSlots(a.slots, b.slots));
        }
        if (equal(a, b)) {
            return a;
        }
        return StringFlowValue::topOf(joinSlots(a.slots, b.slots));
    }

    StringFlowValue newValue(const Insn& producer, InsnId at, VKind shape) override
    {
        const auto unknown = [&] { return StringFlowValue::topOf(widthOf(shape)); };
        switch (producer.op) {
        case Op::ICONST_M1:
        case Op::ICONST_0:
        case Op::ICONST_1:
        case Op::ICONST_2:
        case Op::ICONST_3:
        case Op::ICONST_4:
        case Op::ICONST_5:
            return StringFlowValue::ofInt(static_cast<i32>(producer.op) -
                                          static_cast<i32>(Op::ICONST_0));
        case Op::BIPUSH:
        case Op::SIPUSH: return StringFlowValue::ofInt(producer.a);
        case Op::LDC:
        case Op::LDC_W: {
            if (!cp_->valid(producer.cp)) {
                return unknown();
            }
            switch (cp_->tagOf(producer.cp)) {
            case CpTag::String: return StringFlowValue::ofLiteral(producer.cp, at);
            case CpTag::Integer: return StringFlowValue::ofInt(cp_->at(producer.cp).ival);
            default: return unknown();
            }
        }
        case Op::NEW: {
            const std::string_view owner = jbc::analysis::detail::classNameOf(*cp_, producer.cp);
            if (isBuilderOwner(owner)) {
                return StringFlowValue::ofSite(StringFlowValue::K::Builder, at);
            }
            if (owner == kStringOwner) {
                return StringFlowValue::ofSite(StringFlowValue::K::NewString, at);
            }
            return unknown();
        }
        case Op::NEWARRAY:
            if (producer.a == kAtypeChar || producer.a == kAtypeByte) {
                return StringFlowValue::ofSite(StringFlowValue::K::Array, at);
            }
            return unknown();
        case Op::GETSTATIC: {
            const MethodRef f = fieldRefOf(*cp_, producer.cp);
            if (!f.ok || f.owner != kStandardCharsetsOwner) {
                return unknown();
            }
            if (f.name == "UTF_8") {
                return StringFlowValue::ofSmall(StringFlowValue::K::Charset,
                                                static_cast<i32>(CharsetCode::Utf8));
            }
            if (f.name == "US_ASCII") {
                return StringFlowValue::ofSmall(StringFlowValue::K::Charset,
                                                static_cast<i32>(CharsetCode::Ascii));
            }
            if (f.name == "ISO_8859_1") {
                return StringFlowValue::ofSmall(StringFlowValue::K::Charset,
                                                static_cast<i32>(CharsetCode::Latin1));
            }
            return unknown();
        }
        case Op::INVOKESTATIC: {
            const MethodRef m = methodRefOf(*cp_, producer.cp);
            if (m.ok && m.owner == kBase64Owner &&
                (m.name == "getDecoder" || m.name == "getUrlDecoder") &&
                shape == VKind::Ref) {
                return StringFlowValue::ofSmall(StringFlowValue::K::Decoder,
                                                m.name == "getUrlDecoder" ? 1 : 0);
            }
            if (m.ok && producesTrackedRef(m, shape)) {
                if (returnsArray(m.desc)) {
                    arrayCalls_.insert(at.slot);
                }
                return StringFlowValue::ofSite(StringFlowValue::K::Call, at);
            }
            return unknown();
        }
        case Op::INVOKEVIRTUAL:
        case Op::INVOKESPECIAL:
        case Op::INVOKEINTERFACE: {
            const MethodRef m = methodRefOf(*cp_, producer.cp);
            if (m.ok && producesTrackedRef(m, shape)) {
                if (returnsArray(m.desc)) {
                    arrayCalls_.insert(at.slot);
                }
                return StringFlowValue::ofSite(StringFlowValue::K::Call, at);
            }
            return unknown();
        }
        default: return unknown();
        }
    }

    StringFlowValue newParameter(u32, VKind shape) override
    {
        return StringFlowValue::topOf(widthOf(shape));
    }

    StringFlowValue newException(const jbc::ir::CodeUnit::Exc&) override
    {
        return StringFlowValue::topOf();
    }

    StringFlowValue initializedForm(const StringFlowValue& receiver, const Insn& call,
                                    InsnId id) override
    {
        if (receiver.k == StringFlowValue::K::NewString) {
            return StringFlowValue::ofSite(StringFlowValue::K::Call, id);
        }
        // builder 的 <init> 内容由求值器按 <init> 实参补齐，令牌保持分配点身份。
        (void)call;
        return receiver;
    }

    StringFlowValue binary(Op op, const StringFlowValue& a, const StringFlowValue& b,
                           VKind result, const Insn& at, InsnId id) override
    {
        const auto unknown = [&] { return StringFlowValue::topOf(widthOf(result)); };

        if (a.k == StringFlowValue::K::Int && b.k == StringFlowValue::K::Int) {
            const auto folded =
                numbers_.binary(op, ConstLattice::ofInt(a.i), ConstLattice::ofInt(b.i),
                                VKind::Int, at, id);
            return folded.isInt() ? StringFlowValue::ofInt(static_cast<i32>(folded.asI64()))
                                  : unknown();
        }
        if (op == Op::CALOAD || op == Op::BALOAD) {
            if (a.k == StringFlowValue::K::Array ||
                (a.k == StringFlowValue::K::Call && isArrayCall(a.site))) {
                return StringFlowValue::ofElem(a.k, a.site);
            }
            return unknown();
        }
        if (op == Op::IXOR || op == Op::IADD || op == Op::ISUB) {
            if (a.k == StringFlowValue::K::Elem && b.k == StringFlowValue::K::Int) {
                return StringFlowValue::ofXform(op, b.i, a.srcKind, a.src);
            }
            if (b.k == StringFlowValue::K::Elem && a.k == StringFlowValue::K::Int) {
                return StringFlowValue::ofXform(op, a.i, b.srcKind, b.src);
            }
        }
        return unknown();
    }

    StringFlowValue unary(Op op, const StringFlowValue& a, VKind result, const Insn& at,
                          InsnId id) override
    {
        if (a.k == StringFlowValue::K::Xform && (op == Op::I2C || op == Op::I2B)) {
            return a;   // 掩码由 castore/bastore 按元素宽度施加
        }
        if (a.k == StringFlowValue::K::Int) {
            const auto folded =
                numbers_.unary(op, ConstLattice::ofInt(a.i), VKind::Int, at, id);
            if (folded.isInt()) {
                return StringFlowValue::ofInt(static_cast<i32>(folded.asI64()));
            }
        }
        return StringFlowValue::topOf(widthOf(result));
    }

    ca::u8 width(const StringFlowValue& v) override { return v.slots; }

    bool equal(const StringFlowValue& a, const StringFlowValue& b) override
    {
        if (a.k != b.k || a.slots != b.slots) {
            return false;
        }
        switch (a.k) {
        case StringFlowValue::K::Top: return true;
        case StringFlowValue::K::Int:
        case StringFlowValue::K::Charset:
        case StringFlowValue::K::Decoder: return a.i == b.i;
        case StringFlowValue::K::Literal: return a.cp == b.cp && a.site.slot == b.site.slot;
        case StringFlowValue::K::Builder:
        case StringFlowValue::K::NewString:
        case StringFlowValue::K::Array:
        case StringFlowValue::K::Call: return a.site.slot == b.site.slot;
        case StringFlowValue::K::Elem:
            return a.srcKind == b.srcKind && a.src.slot == b.src.slot;
        case StringFlowValue::K::Xform:
            return a.srcKind == b.srcKind && a.src.slot == b.src.slot && a.i == b.i &&
                   a.xop == b.xop;
        }
        return false;
    }

private:
    static u8 joinSlots(u8 a, u8 b) noexcept { return a == b ? a : (a < b ? a : b); }

    static u8 widthOf(VKind shape) noexcept
    {
        return shape == VKind::Long || shape == VKind::Double ? 2 : 1;
    }

    static bool producesTrackedRef(const MethodRef& m, VKind shape) noexcept
    {
        if (shape != VKind::Ref) {
            return false;
        }
        if (isBuilderOwner(m.owner)) {
            return m.name == "toString" || isBuilderMutationName(m.name);
        }
        if (m.owner == kStringOwner) {
            return isStringProducerName(m.name);
        }
        return m.owner == kBase64DecoderOwner && m.name == "decode";
    }

    bool isArrayCall(InsnId site) const noexcept
    {
        return arrayCalls_.count(site.slot) != 0;
    }

    const ConstantPool* cp_;
    ConstDomain numbers_;
    /// 返回数组的被追踪调用点（toCharArray/getBytes/Base64 decode 等）。
    std::unordered_set<u32> arrayCalls_;
};

// ---------------------------------------------------------------------------
// 求值：在到达帧上回溯令牌，产出闭环字符串值
// ---------------------------------------------------------------------------

enum class ElemKind : u8 { None = 0, Char, Byte };

struct Eval
{
    enum class K : u8 { Unknown = 0, Text, Chars, Bytes, Int, Charset, Decoder };

    K k = K::Unknown;
    std::u16string text;      ///< Text / Chars
    std::vector<u8> bytes;    ///< Bytes
    i32 i = 0;                ///< Int / CharsetCode / Decoder.urlSafe
    bool complete = false;

    bool fromBuilder = false;
    bool fromValueOf = false;
    bool fromChars = false;
    bool fromBytes = false;
    bool fromBase64 = false;
    bool fromXor = false;
    bool fromShift = false;
    bool fullTransform = false;   ///< 至少一次全数组循环变换

    std::u16string base64Input;
    bool base64HasInput = false;
    std::u16string transformInput;
    bool transformHasInput = false;
};

void absorbProv(Eval& dst, const Eval& src)
{
    dst.fromBuilder |= src.fromBuilder;
    dst.fromValueOf |= src.fromValueOf;
    dst.fromChars |= src.fromChars;
    dst.fromBytes |= src.fromBytes;
    dst.fromBase64 |= src.fromBase64;
    dst.fromXor |= src.fromXor;
    dst.fromShift |= src.fromShift;
    dst.fullTransform |= src.fullTransform;
    if (!dst.base64HasInput && src.base64HasInput) {
        dst.base64Input = src.base64Input;
        dst.base64HasInput = true;
    }
    if (!dst.transformHasInput && src.transformHasInput) {
        dst.transformInput = src.transformInput;
        dst.transformHasInput = true;
    }
}

Eval textEval(std::u16string text)
{
    Eval e;
    e.k = Eval::K::Text;
    e.text = std::move(text);
    e.complete = true;
    return e;
}

std::u16string u16FromAscii(std::string_view s)
{
    std::u16string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<u16>(static_cast<u8>(c)));
    }
    return out;
}

class Evaluator
{
public:
    Evaluator(const ConstantPool& cp, const CodeUnit& unit,
              const DomainResult<StringFlowValue>& result)
        : cp_(&cp)
        , unit_(&unit)
        , frames_(result.frames)
    {
        const auto& order = unit.dom.order();
        posOf_.reserve(order.size() * 2);
        for (usize i = 0; i < order.size(); ++i) {
            posOf_.emplace(order[i].slot, i);
        }
    }

    Eval eval(const StringFlowValue& v)
    {
        if (budget_ == 0) {
            return Eval{};
        }
        --budget_;
        switch (v.k) {
        case StringFlowValue::K::Top:
        case StringFlowValue::K::NewString:
        case StringFlowValue::K::Elem:
        case StringFlowValue::K::Xform: return Eval{};
        case StringFlowValue::K::Int:
        case StringFlowValue::K::Charset:
        case StringFlowValue::K::Decoder: {
            Eval e;
            e.complete = true;
            if (v.k == StringFlowValue::K::Int) {
                e.k = Eval::K::Int;
            } else if (v.k == StringFlowValue::K::Charset) {
                e.k = Eval::K::Charset;
            } else {
                e.k = Eval::K::Decoder;
            }
            e.i = v.i;
            return e;
        }
        case StringFlowValue::K::Literal: return evalLiteral(v);
        case StringFlowValue::K::Builder: return evalBuilder(v.site);
        case StringFlowValue::K::Array: return evalArrayLike(v);
        case StringFlowValue::K::Call: {
            const TokenKey key = tokenKeyOf(v);
            if (const auto it = memo_.find(key); it != memo_.end()) {
                return it->second;
            }
            if (active_.count(key) != 0) {
                return Eval{};
            }
            active_.insert(key);
            Eval e = evalCall(v.site);
            active_.erase(key);
            memo_.emplace(key, e);
            return e;
        }
        }
        return Eval{};
    }

    /// 来源令牌是否到达未识别的消费点。
    bool escaped(const StringFlowValue& token) const
    {
        const TokenKey key = tokenKeyOf(token);
        for (const InsnId& id : unit_->dom.order()) {
            const Insn* in = unit_->dom.peek(id);
            if (in == nullptr) {
                continue;
            }
            const FrameV<StringFlowValue>* f = frameOf(id);
            if (f == nullptr) {
                continue;
            }
            for (usize i = 0; i < f->stack.size(); ++i) {
                const SlotV<StringFlowValue>& row = f->stack[i];
                if (row.bottom || !(tokenKeyOf(row.val) == key)) {
                    continue;
                }
                if (!rowTouched(*in, i, f->stack.size())) {
                    continue;
                }
                if (isEscapeNeutral(in->op) || isRecognizedConsumer(*in)) {
                    continue;
                }
                return true;
            }
        }
        return false;
    }

    const ConstantPool& cp() const noexcept { return *cp_; }

private:
    const FrameV<StringFlowValue>* frameOf(InsnId id) const
    {
        const auto it = posOf_.find(id.slot);
        if (it == posOf_.end() || it->second >= frames_.size()) {
            return nullptr;
        }
        const FrameV<StringFlowValue>& f = frames_[it->second];
        return f.reachable ? &f : nullptr;
    }

    const Insn* insnOf(InsnId id) const { return unit_->dom.peek(id); }

    /// 指令从栈顶读取的槽数（含接收者）；-1 = 描述符不可知。
    i32 touchedSlots(const Insn& in) const
    {
        switch (in.op) {
        case Op::INVOKEVIRTUAL:
        case Op::INVOKESPECIAL:
        case Op::INVOKESTATIC:
        case Op::INVOKEINTERFACE:
        case Op::INVOKEDYNAMIC: {
            const MethodRef m = methodRefOf(*cp_, in.cp);
            if (!m.ok) {
                return -1;
            }
            i32 slots = 0;
            for (const std::string_view t : paramsOf(m.desc)) {
                const i32 n = jbc::analysis::detail::typeSlots(t);
                if (n < 0) {
                    return -1;
                }
                slots += n;
            }
            const bool isStatic = in.op == Op::INVOKESTATIC || in.op == Op::INVOKEDYNAMIC;
            return slots + (isStatic ? 0 : 1);
        }
        case Op::PUTSTATIC: {
            const MethodRef f = fieldRefOf(*cp_, in.cp);
            return f.ok ? jbc::analysis::detail::typeSlots(f.desc) : -1;
        }
        case Op::PUTFIELD: {
            const MethodRef f = fieldRefOf(*cp_, in.cp);
            if (!f.ok) {
                return -1;
            }
            const i32 n = jbc::analysis::detail::typeSlots(f.desc);
            return n < 0 ? -1 : n + 1;
        }
        case Op::MULTIANEWARRAY: return in.a;
        default: return mjbase::asm_insn::minStackDepthOf(in.op);
        }
    }

    /// 到达帧中的第 rowIndex 行（0 起）是否属于本条指令的读取操作数。
    /// 令牌停在栈深处而指令只读栈顶时不算使用——这是逃逸/污染扫描的
    /// 核心判定：按 in-state 栈里「恰好存在令牌」判会大量误报。
    bool rowTouched(const Insn& in, usize rowIndex, usize stackSize) const
    {
        const i32 touched = touchedSlots(in);
        if (touched < 0) {
            return true;   // 不可知按使用处理（保守）
        }
        if (touched == 0) {
            return false;
        }
        return rowIndex + static_cast<usize>(touched) >= stackSize;
    }

    bool operandsOf(InsnId id, const Insn& in, const StringFlowValue*& receiver,
                    std::vector<const StringFlowValue*>& args) const
    {
        const FrameV<StringFlowValue>* f = frameOf(id);
        if (f == nullptr) {
            return false;
        }
        const MethodRef m = methodRefOf(*cp_, in.cp);
        if (!m.ok) {
            return false;
        }
        const std::vector<std::string_view> params = paramsOf(m.desc);
        if (params.empty() && m.desc.size() < 3) {
            return false;
        }
        const bool isStatic = in.op == Op::INVOKESTATIC || in.op == Op::INVOKEDYNAMIC;
        u32 argSlots = 0;
        for (const std::string_view t : params) {
            const i32 n = jbc::analysis::detail::typeSlots(t);
            if (n < 0) {
                return false;
            }
            argSlots += static_cast<u32>(n);
        }
        const u32 need = argSlots + (isStatic ? 0 : 1);
        if (f->stack.size() < need) {
            return false;
        }
        const usize base = f->stack.size() - need;
        receiver = nullptr;
        if (!isStatic) {
            if (f->stack[base].bottom) {
                return false;
            }
            receiver = &f->stack[base].val;
        }
        args.clear();
        args.reserve(params.size());
        usize pos = base + (isStatic ? 0 : 1);
        for (const std::string_view t : params) {
            const i32 n = jbc::analysis::detail::typeSlots(t);
            if (n == 2) {
                args.push_back(nullptr);   // cat2 实参不进入字符串求值
                pos += 2;
            } else {
                if (pos >= f->stack.size() || f->stack[pos].bottom) {
                    return false;
                }
                args.push_back(&f->stack[pos].val);
                ++pos;
            }
        }
        return true;
    }

    bool receiverOf(InsnId id, const Insn& in, const StringFlowValue*& receiver) const
    {
        std::vector<const StringFlowValue*> args;
        return operandsOf(id, in, receiver, args);
    }

    static bool sameSite(InsnId a, InsnId b) noexcept { return a.slot == b.slot; }

    std::optional<InsnId> builderRoot(const StringFlowValue& v, int depth = 0) const
    {
        if (depth > 16) {
            return std::nullopt;
        }
        if (v.k == StringFlowValue::K::Builder) {
            return v.site;
        }
        if (v.k != StringFlowValue::K::Call) {
            return std::nullopt;
        }
        const Insn* in = insnOf(v.site);
        if (in == nullptr) {
            return std::nullopt;
        }
        const MethodRef m = methodRefOf(*cp_, in->cp);
        if (!m.ok || !isBuilderOwner(m.owner) || !isBuilderMutationName(m.name)) {
            return std::nullopt;
        }
        const StringFlowValue* recv = nullptr;
        if (!receiverOf(v.site, *in, recv) || recv == nullptr) {
            return std::nullopt;
        }
        return builderRoot(*recv, depth + 1);
    }

    Eval evalLiteral(const StringFlowValue& v)
    {
        if (!cp_->valid(v.cp) || cp_->tagOf(v.cp) != CpTag::String) {
            return Eval{};
        }
        const auto& entry = cp_->at(v.cp);
        std::u16string text;
        if (!decodeMutf8(cp_->utf8Of(entry.a), text)) {
            return Eval{};
        }
        return textEval(std::move(text));
    }

    Eval evalCall(InsnId site)
    {
        const Insn* in = insnOf(site);
        if (in == nullptr) {
            return Eval{};
        }
        const MethodRef m = methodRefOf(*cp_, in->cp);
        if (!m.ok) {
            return Eval{};
        }
        if (isBuilderOwner(m.owner)) {
            if (m.name == "toString" || isBuilderMutationName(m.name)) {
                const StringFlowValue* recv = nullptr;
                if (!receiverOf(site, *in, recv) || recv == nullptr) {
                    return Eval{};
                }
                const auto root = builderRoot(*recv);
                return root ? evalBuilder(*root) : Eval{};
            }
            return Eval{};
        }
        if (m.owner == kStringOwner) {
            std::vector<const StringFlowValue*> args;
            const StringFlowValue* recv = nullptr;
            if (!operandsOf(site, *in, recv, args)) {
                return Eval{};
            }
            if (m.name == "<init>") {
                return constructString(m, args);
            }
            if (m.name == "valueOf" || m.name == "copyValueOf") {
                return valueOfCall(m, args);
            }
            if (m.name == "concat") {
                Eval out = evalTextToken(recv);
                if (!out.complete || args.size() != 1) {
                    return Eval{};
                }
                Eval rhs = evalTextToken(args[0]);
                if (!rhs.complete) {
                    return Eval{};
                }
                out.text += rhs.text;
                absorbProv(out, rhs);
                return out;
            }
            if (m.name == "toCharArray") {
                const Eval receiver = evalTextToken(recv);
                if (!receiver.complete) {
                    return Eval{};
                }
                Eval e;
                e.k = Eval::K::Chars;
                e.text = receiver.text;
                e.complete = true;
                absorbProv(e, receiver);
                return e;
            }
            if (m.name == "getBytes") {
                return getBytesCall(recv, args);
            }
            if (m.name == "intern" || m.name == "toString") {
                return evalTextToken(recv);
            }
            return Eval{};
        }
        if (m.owner == kBase64DecoderOwner && m.name == "decode") {
            return decodeCall(site, *in);
        }
        return Eval{};
    }

    Eval evalTextToken(const StringFlowValue* token)
    {
        if (token == nullptr) {
            return Eval{};
        }
        Eval e = eval(*token);
        if (!e.complete || e.k != Eval::K::Text) {
            return Eval{};
        }
        return e;
    }

    Eval constructString(const MethodRef& m, const std::vector<const StringFlowValue*>& args)
    {
        const std::vector<std::string_view> params = paramsOf(m.desc);
        if (params.empty()) {
            return textEval({});
        }
        const auto charsFrom = [&](std::u16string text) {
            Eval e = textEval(std::move(text));
            e.fromChars = true;
            return e;
        };
        const auto argInt = [&](usize idx, i32& out) {
            if (idx >= args.size() || args[idx] == nullptr ||
                args[idx]->k != StringFlowValue::K::Int) {
                return false;
            }
            out = args[idx]->i;
            return true;
        };

        if (params.size() == 1 && params[0] == "[C") {
            Eval src = charsOf(args.empty() ? nullptr : args[0]);
            if (!src.complete) {
                return Eval{};
            }
            Eval e = charsFrom(std::move(src.text));
            absorbProv(e, src);
            return e;
        }
        if (params.size() == 3 && params[0] == "[C" && params[1] == "I" && params[2] == "I") {
            Eval src = charsOf(args.empty() ? nullptr : args[0]);
            i32 off = 0;
            i32 count = 0;
            if (!src.complete || !argInt(1, off) || !argInt(2, count) || off < 0 ||
                count < 0 || static_cast<usize>(off) + static_cast<usize>(count) >
                    src.text.size()) {
                return Eval{};
            }
            Eval e = charsFrom(src.text.substr(static_cast<usize>(off),
                                               static_cast<usize>(count)));
            absorbProv(e, src);
            return e;
        }
        if (params.size() == 1 && params[0] == "Ljava/lang/String;") {
            Eval src = evalTextToken(args.empty() ? nullptr : args[0]);
            if (!src.complete) {
                return Eval{};
            }
            Eval e = textEval(src.text);
            absorbProv(e, src);
            return e;
        }
        if (params.size() == 1 && params[0] == "[B") {
            return Eval{};   // 平台默认 charset，静态不可知
        }
        if (params.size() == 2 && params[0] == "[B" &&
            (params[1] == "Ljava/lang/String;" || params[1] == "Ljava/nio/charset/Charset;")) {
            return bytesToString(args.empty() ? nullptr : args[0],
                                 args.size() < 2 ? nullptr : args[1]);
        }
        if (params.size() == 4 && params[0] == "[B" && params[1] == "I" && params[2] == "I" &&
            (params[3] == "Ljava/lang/String;" || params[3] == "Ljava/nio/charset/Charset;")) {
            Eval src = bytesOf(args.empty() ? nullptr : args[0]);
            i32 off = 0;
            i32 count = 0;
            if (!src.complete || !argInt(1, off) || !argInt(2, count) || off < 0 ||
                count < 0 || static_cast<usize>(off) + static_cast<usize>(count) >
                    src.bytes.size()) {
                return Eval{};
            }
            std::vector<u8> slice(src.bytes.begin() + off, src.bytes.begin() + off + count);
            src.bytes = std::move(slice);
            return decodeBytesWithCharset(src, args.size() < 4 ? nullptr : args[3]);
        }
        return Eval{};
    }

    Eval bytesToString(const StringFlowValue* bytesToken, const StringFlowValue* charsetToken)
    {
        Eval src = bytesOf(bytesToken);
        if (!src.complete) {
            return Eval{};
        }
        return decodeBytesWithCharset(src, charsetToken);
    }

    Eval decodeBytesWithCharset(Eval src, const StringFlowValue* charsetToken)
    {
        const CharsetCode cs = charsetOf(charsetToken);
        std::u16string text;
        if (!decodeCharset(src.bytes, cs, text)) {
            return Eval{};
        }
        Eval e = textEval(std::move(text));
        e.fromBytes = true;
        absorbProv(e, src);
        return e;
    }

    Eval valueOfCall(const MethodRef& m, const std::vector<const StringFlowValue*>& args)
    {
        const std::vector<std::string_view> params = paramsOf(m.desc);
        if (params.empty()) {
            return Eval{};
        }
        const std::string_view t = params[0];
        Eval e;
        if (t == "Ljava/lang/Object;" || t == "Ljava/lang/CharSequence;") {
            e = evalTextToken(args.empty() ? nullptr : args[0]);
        } else if (t == "C") {
            if (!args.empty() && args[0] != nullptr && args[0]->k == StringFlowValue::K::Int) {
                e = textEval(std::u16string(1, static_cast<u16>(args[0]->i & 0xFFFF)));
            }
        } else if (t == "I") {
            if (!args.empty() && args[0] != nullptr && args[0]->k == StringFlowValue::K::Int) {
                e = textEval(u16FromAscii(std::to_string(args[0]->i)));
            }
        } else if (t == "Z") {
            if (!args.empty() && args[0] != nullptr && args[0]->k == StringFlowValue::K::Int) {
                e = textEval(u16FromAscii(args[0]->i != 0 ? "true" : "false"));
            }
        } else if (t == "[C" && (m.name == "valueOf" || m.name == "copyValueOf")) {
            Eval src = charsOf(args.empty() ? nullptr : args[0]);
            if (src.complete) {
                if (params.size() == 1) {
                    e = textEval(src.text);
                    e.fromChars = true;
                    absorbProv(e, src);
                } else if (params.size() == 3 && params[1] == "I" && params[2] == "I" &&
                           args.size() >= 3 && args[1] != nullptr && args[2] != nullptr &&
                           args[1]->k == StringFlowValue::K::Int &&
                           args[2]->k == StringFlowValue::K::Int) {
                    const i32 off = args[1]->i;
                    const i32 count = args[2]->i;
                    if (off >= 0 && count >= 0 &&
                        static_cast<usize>(off) + static_cast<usize>(count) <=
                            src.text.size()) {
                        e = textEval(src.text.substr(static_cast<usize>(off),
                                                     static_cast<usize>(count)));
                        e.fromChars = true;
                        absorbProv(e, src);
                    }
                }
            }
        }
        if (!e.complete) {
            return Eval{};
        }
        e.fromValueOf = true;
        return e;
    }

    Eval getBytesCall(const StringFlowValue* receiver,
                      const std::vector<const StringFlowValue*>& args)
    {
        const Eval src = evalTextToken(receiver);
        if (!src.complete) {
            return Eval{};
        }
        CharsetCode cs = CharsetCode::Unknown;
        if (!args.empty() && args[0] != nullptr) {
            cs = charsetOf(args[0]);
        }
        std::vector<u8> bytes;
        if (!encodeCharset(src.text, cs, bytes)) {
            return Eval{};
        }
        Eval e;
        e.k = Eval::K::Bytes;
        e.bytes = std::move(bytes);
        e.complete = true;
        absorbProv(e, src);
        return e;
    }

    Eval decodeCall(InsnId site, const Insn& in)
    {
        const StringFlowValue* recv = nullptr;
        std::vector<const StringFlowValue*> args;
        if (!operandsOf(site, in, recv, args) || recv == nullptr || args.empty()) {
            return Eval{};
        }
        const Eval decoder = eval(*recv);
        if (!decoder.complete || decoder.k != Eval::K::Decoder) {
            return Eval{};
        }
        const Eval input = eval(*args[0]);
        if (!input.complete) {
            return Eval{};
        }
        std::u16string encodedText;
        if (input.k == Eval::K::Text) {
            encodedText = input.text;
            for (const u16 c : encodedText) {
                if (c >= 0x80) {
                    return Eval{};
                }
            }
        } else if (input.k == Eval::K::Bytes) {
            encodedText.reserve(input.bytes.size());
            for (const u8 b : input.bytes) {
                encodedText.push_back(static_cast<u16>(b));
            }
        } else {
            return Eval{};
        }
        std::vector<u8> bytes;
        if (!decodeBase64(encodedText, decoder.i != 0, bytes)) {
            return Eval{};
        }
        Eval e;
        e.k = Eval::K::Bytes;
        e.bytes = std::move(bytes);
        e.complete = true;
        e.fromBase64 = true;
        e.base64HasInput = input.k == Eval::K::Text;
        if (e.base64HasInput) {
            e.base64Input = encodedText;
        }
        return e;
    }

    CharsetCode charsetOf(const StringFlowValue* token)
    {
        if (token == nullptr) {
            return CharsetCode::Unknown;
        }
        if (token->k == StringFlowValue::K::Charset) {
            return static_cast<CharsetCode>(token->i);
        }
        const Eval e = eval(*token);
        if (!e.complete || e.k != Eval::K::Text) {
            return CharsetCode::Unknown;
        }
        return charsetFromName(e.text);
    }

    Eval appendArg(std::string_view type, const StringFlowValue* token)
    {
        if (token == nullptr) {
            return Eval{};
        }
        if (type == "Ljava/lang/String;" || type == "Ljava/lang/CharSequence;" ||
            type == "Ljava/lang/Object;") {
            return evalTextToken(token);
        }
        if (type == "C") {
            if (token->k == StringFlowValue::K::Int) {
                return textEval(std::u16string(1, static_cast<u16>(token->i & 0xFFFF)));
            }
            return Eval{};
        }
        if (type == "I") {
            if (token->k == StringFlowValue::K::Int) {
                return textEval(u16FromAscii(std::to_string(token->i)));
            }
            return Eval{};
        }
        if (type == "Z") {
            if (token->k == StringFlowValue::K::Int) {
                return textEval(u16FromAscii(token->i != 0 ? "true" : "false"));
            }
            return Eval{};
        }
        if (type == "[C") {
            return charsOf(token);
        }
        if (type == "Ljava/lang/StringBuffer;" || type == "Ljava/lang/StringBuilder;" ||
            type == "Ljava/lang/AbstractStringBuilder;") {
            const auto root = builderRoot(*token);
            return root ? evalBuilder(*root) : Eval{};
        }
        return Eval{};
    }

    Eval evalBuilder(InsnId root)
    {
        const TokenKey key{StringFlowValue::K::Builder, root.slot, 0};
        if (const auto it = memo_.find(key); it != memo_.end()) {
            return it->second;
        }
        if (active_.count(key) != 0 || budget_ == 0) {
            return Eval{};
        }
        --budget_;
        active_.insert(key);

        Eval out = textEval({});
        out.fromBuilder = true;
        bool ok = true;

        struct Mutation
        {
            MethodRef ref;   // 按值持有：循环内局部 m 的地址在下一轮即失效
            std::vector<const StringFlowValue*> args;
        };
        std::vector<Mutation> mutations;
        usize firstPos = static_cast<usize>(-1);
        usize lastPos = 0;

        const auto& order = unit_->dom.order();
        for (usize pos = 0; pos < order.size(); ++pos) {
            const InsnId id = order[pos];
            const Insn* in = insnOf(id);
            if (in == nullptr || (in->op != Op::INVOKEVIRTUAL && in->op != Op::INVOKESPECIAL &&
                                  in->op != Op::INVOKEINTERFACE)) {
                continue;
            }
            const MethodRef m = methodRefOf(*cp_, in->cp);
            if (!m.ok || !isBuilderOwner(m.owner)) {
                continue;
            }
            if (m.name != "<init>" && !isBuilderMutationName(m.name)) {
                continue;
            }
            const StringFlowValue* recv = nullptr;
            std::vector<const StringFlowValue*> args;
            if (!operandsOf(id, *in, recv, args) || recv == nullptr) {
                continue;
            }
            const auto recvRoot = builderRoot(*recv);
            if (!recvRoot || !sameSite(*recvRoot, root)) {
                continue;
            }
            Mutation mut;
            mut.ref = m;
            mut.args = std::move(args);
            mutations.push_back(std::move(mut));
            firstPos = firstPos == static_cast<usize>(-1) ? pos : firstPos;
            lastPos = pos;
        }

        // 分配点到最后一次变更之间出现分支/回跳，变更就可能被跳过或重复；
        // 内容不可静态确定的 builder 一律标 incomplete（fail-closed）。
        const auto rootIt = posOf_.find(root.slot);
        const usize rootPos = rootIt == posOf_.end() ? firstPos : rootIt->second;
        if (!mutations.empty() && rootPos != static_cast<usize>(-1) &&
            builderControlFlowAffected(rootPos, lastPos)) {
            ok = false;
        }

        for (const Mutation& mut : mutations) {
            const MethodRef& m = mut.ref;
            if (m.name == "<init>") {
                if (mut.args.empty()) {
                    continue;
                }
                const std::vector<std::string_view> initParams = paramsOf(m.desc);
                if (initParams.empty()) {
                    ok = false;
                    continue;
                }
                if (initParams.size() == 1 &&
                    (initParams[0] == "I" || initParams[0] == "J")) {
                    continue;   // 容量构造，内容为空（容量取值不影响内容）
                }
                if (mut.args.size() == 1 && mut.args[0] != nullptr &&
                    (initParams[0] == "Ljava/lang/String;" ||
                     initParams[0] == "Ljava/lang/CharSequence;")) {
                    const Eval src = evalTextToken(mut.args[0]);
                    if (!src.complete) {
                        ok = false;
                        continue;
                    }
                    out.text = src.text;
                    absorbProv(out, src);
                    continue;
                }
                ok = false;
                continue;
            }

            const std::vector<std::string_view> params = paramsOf(m.desc);
            const auto argInt = [&](usize idx, i32& value) {
                if (idx >= mut.args.size() || mut.args[idx] == nullptr ||
                    mut.args[idx]->k != StringFlowValue::K::Int) {
                    return false;
                }
                value = mut.args[idx]->i;
                return true;
            };
            const auto checkBounds = [&](i32 off, i32 count, usize size) {
                return off >= 0 && count >= 0 &&
                       static_cast<usize>(off) + static_cast<usize>(count) <= size;
            };

            if (m.name == "append") {
                if (params.size() == 1) {
                    Eval val = appendArg(params[0], mut.args.empty() ? nullptr : mut.args[0]);
                    if (!val.complete) {
                        ok = false;
                        continue;
                    }
                    out.text += val.text;
                    absorbProv(out, val);
                } else if (params.size() == 3 && params[0] == "[C") {
                    Eval val = charsOf(mut.args.empty() ? nullptr : mut.args[0]);
                    i32 off = 0;
                    i32 count = 0;
                    if (!val.complete || !argInt(1, off) || !argInt(2, count) ||
                        !checkBounds(off, count, val.text.size())) {
                        ok = false;
                        continue;
                    }
                    out.text += val.text.substr(static_cast<usize>(off),
                                                static_cast<usize>(count));
                    absorbProv(out, val);
                } else if (params.size() == 3 &&
                           (params[0] == "Ljava/lang/CharSequence;" ||
                            params[0] == "Ljava/lang/String;")) {
                    Eval val = evalTextToken(mut.args.empty() ? nullptr : mut.args[0]);
                    i32 off = 0;
                    i32 count = 0;
                    if (!val.complete || !argInt(1, off) || !argInt(2, count) ||
                        !checkBounds(off, count, val.text.size())) {
                        ok = false;
                        continue;
                    }
                    out.text += val.text.substr(static_cast<usize>(off),
                                                static_cast<usize>(count));
                    absorbProv(out, val);
                } else {
                    ok = false;
                }
                continue;
            }
            if (m.name == "insert") {
                if (params.size() == 2) {
                    i32 off = 0;
                    Eval val = appendArg(params[1], mut.args.size() < 2 ? nullptr : mut.args[1]);
                    if (!val.complete || !argInt(0, off) || off < 0 ||
                        static_cast<usize>(off) > out.text.size()) {
                        ok = false;
                        continue;
                    }
                    out.text.insert(static_cast<usize>(off), val.text);
                    absorbProv(out, val);
                } else {
                    ok = false;
                }
                continue;
            }
            if (m.name == "reverse") {
                std::reverse(out.text.begin(), out.text.end());
                continue;
            }
            if (m.name == "setLength") {
                i32 len = 0;
                if (!argInt(0, len) || len < 0 || static_cast<usize>(len) > kMaxTextUnits) {
                    ok = false;
                    continue;
                }
                out.text.resize(static_cast<usize>(len), 0);
                continue;
            }
            if (m.name == "setCharAt") {
                i32 off = 0;
                i32 ch = 0;
                if (!argInt(0, off) || !argInt(1, ch) || off < 0 ||
                    static_cast<usize>(off) >= out.text.size()) {
                    ok = false;
                    continue;
                }
                out.text[static_cast<usize>(off)] = static_cast<u16>(ch & 0xFFFF);
                continue;
            }
            if (m.name == "deleteCharAt") {
                i32 off = 0;
                if (!argInt(0, off) || off < 0 ||
                    static_cast<usize>(off) >= out.text.size()) {
                    ok = false;
                    continue;
                }
                out.text.erase(static_cast<usize>(off), 1);
                continue;
            }
            if (m.name == "delete") {
                i32 start = 0;
                i32 end = 0;
                if (!argInt(0, start) || !argInt(1, end) || start < 0 || end < start ||
                    static_cast<usize>(end) > out.text.size()) {
                    ok = false;
                    continue;
                }
                out.text.erase(static_cast<usize>(start), static_cast<usize>(end - start));
                continue;
            }
            if (m.name == "replace") {
                i32 start = 0;
                i32 end = 0;
                Eval val = evalTextToken(mut.args.size() < 3 ? nullptr : mut.args[2]);
                if (!val.complete || !argInt(0, start) || !argInt(1, end) || start < 0 ||
                    end < start || static_cast<usize>(end) > out.text.size()) {
                    ok = false;
                    continue;
                }
                out.text.replace(static_cast<usize>(start),
                                 static_cast<usize>(end - start), val.text);
                absorbProv(out, val);
                continue;
            }
            ok = false;
        }

        // builder 逃逸/未知变更：内容可能被外部改写，闭环不成立。
        if (ok && builderTainted(root)) {
            ok = false;
        }
        out.complete = ok;
        active_.erase(key);
        memo_.emplace(key, out);
        return out;
    }

    /// 窗口内存在分支/返回/抛出：变更可能被跳过或重复执行。
    bool builderControlFlowAffected(usize from, usize to) const
    {
        const auto& order = unit_->dom.order();
        for (usize pos = from; pos <= to && pos < order.size(); ++pos) {
            const Insn* in = insnOf(order[pos]);
            if (in == nullptr) {
                continue;
            }
            const jbc::core::Shape sh = jbc::core::shapeOf(in->op);
            if (sh == jbc::core::Shape::Branch2 || sh == jbc::core::Shape::Branch4 ||
                sh == jbc::core::Shape::TableSwitch || sh == jbc::core::Shape::LookupSwitch) {
                return true;
            }
            switch (in->op) {
            case Op::IRETURN:
            case Op::LRETURN:
            case Op::FRETURN:
            case Op::DRETURN:
            case Op::ARETURN:
            case Op::RETURN:
            case Op::ATHROW: return true;
            default: break;
            }
        }
        // 回跳覆盖变更点：循环体会重复执行变更，同样不可静态定值。
        for (const InsnId& id : order) {
            const Insn* in = insnOf(id);
            if (in == nullptr) {
                continue;
            }
            const jbc::core::Shape sh = jbc::core::shapeOf(in->op);
            if (sh != jbc::core::Shape::Branch2 && sh != jbc::core::Shape::Branch4 &&
                sh != jbc::core::Shape::TableSwitch &&
                sh != jbc::core::Shape::LookupSwitch) {
                continue;
            }
            const auto selfIt = posOf_.find(id.slot);
            if (selfIt == posOf_.end()) {
                continue;
            }
            const usize selfPos = selfIt->second;
            const auto overlaps = [&](jbc::ir::LabelId label) {
                const auto target = unit_->dom.resolveLabel(label);
                if (target.is_err() || target.unwrap().isEnd()) {
                    return false;
                }
                const auto it = posOf_.find(target.unwrap().insn.slot);
                if (it == posOf_.end() || it->second > selfPos) {
                    return false;   // 前跳不是回边
                }
                // 循环区间与窗口相交即视为受影响（保守）。
                return it->second <= to && selfPos >= from;
            };
            if (overlaps(in->target)) {
                return true;
            }
            for (const jbc::ir::LabelId& label : in->targets) {
                if (overlaps(label)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool builderTainted(InsnId root) const
    {
        for (const InsnId& id : unit_->dom.order()) {
            const Insn* in = insnOf(id);
            if (in == nullptr) {
                continue;
            }
            const FrameV<StringFlowValue>* f = frameOf(id);
            if (f == nullptr) {
                continue;
            }
            for (usize i = 0; i < f->stack.size(); ++i) {
                const SlotV<StringFlowValue>& row = f->stack[i];
                if (row.bottom) {
                    continue;
                }
                const auto r = builderRoot(row.val);
                if (!r || !sameSite(*r, root)) {
                    continue;
                }
                if (!rowTouched(*in, i, f->stack.size())) {
                    continue;
                }
                if (isNeutralUse(in->op) || isBuilderConsumer(*in)) {
                    continue;
                }
                return true;
            }
        }
        return false;
    }

    bool isBuilderConsumer(const Insn& in) const
    {
        if (in.op != Op::INVOKEVIRTUAL && in.op != Op::INVOKESPECIAL &&
            in.op != Op::INVOKEINTERFACE) {
            return false;
        }
        const MethodRef m = methodRefOf(*cp_, in.cp);
        if (!m.ok || !isBuilderOwner(m.owner)) {
            return false;
        }
        return m.name == "<init>" || m.name == "toString" || isBuilderMutationName(m.name);
    }

    ElemKind elemKindOf(const StringFlowValue& token) const
    {
        if (token.k == StringFlowValue::K::Array) {
            const Insn* in = insnOf(token.site);
            if (in == nullptr || in->op != Op::NEWARRAY) {
                return ElemKind::None;
            }
            if (in->a == kAtypeChar) {
                return ElemKind::Char;
            }
            if (in->a == kAtypeByte) {
                return ElemKind::Byte;
            }
            return ElemKind::None;
        }
        if (token.k != StringFlowValue::K::Call) {
            return ElemKind::None;
        }
        const Insn* in = insnOf(token.site);
        if (in == nullptr) {
            return ElemKind::None;
        }
        const MethodRef m = methodRefOf(*cp_, in->cp);
        if (!m.ok) {
            return ElemKind::None;
        }
        if (m.owner == kStringOwner && m.name == "toCharArray") {
            return ElemKind::Char;
        }
        if (m.owner == kStringOwner && m.name == "getBytes") {
            return ElemKind::Byte;
        }
        if (m.owner == kBase64DecoderOwner && m.name == "decode") {
            return ElemKind::Byte;
        }
        return ElemKind::None;
    }

    bool isArrayConsumer(const Insn& in) const
    {
        if (in.op != Op::INVOKESPECIAL && in.op != Op::INVOKESTATIC &&
            in.op != Op::INVOKEVIRTUAL) {
            return false;
        }
        const MethodRef m = methodRefOf(*cp_, in.cp);
        if (!m.ok) {
            return false;
        }
        if (m.owner == kStringOwner) {
            return m.name == "<init>" || m.name == "valueOf" || m.name == "copyValueOf";
        }
        if (isBuilderOwner(m.owner) && m.name == "append") {
            return true;
        }
        return m.owner == kBase64DecoderOwner && m.name == "decode";
    }

    bool arrayTainted(const StringFlowValue& token) const
    {
        for (const InsnId& id : unit_->dom.order()) {
            const Insn* in = insnOf(id);
            if (in == nullptr) {
                continue;
            }
            const FrameV<StringFlowValue>* f = frameOf(id);
            if (f == nullptr) {
                continue;
            }
            for (usize i = 0; i < f->stack.size(); ++i) {
                const SlotV<StringFlowValue>& row = f->stack[i];
                if (row.bottom || !sameToken(row.val, token)) {
                    continue;
                }
                if (!rowTouched(*in, i, f->stack.size())) {
                    continue;
                }
                if (isNeutralUse(in->op) || isArrayConsumer(*in)) {
                    continue;
                }
                return true;
            }
        }
        return false;
    }

    Eval evalArrayLike(const StringFlowValue& token)
    {
        const ElemKind kind = elemKindOf(token);
        if (kind == ElemKind::None) {
            return Eval{};
        }
        const TokenKey key{token.k, token.site.slot, token.cp};
        if (const auto it = arrayMemo_.find(key); it != arrayMemo_.end()) {
            return it->second;
        }
        if (arrayActive_.count(key) != 0 || budget_ == 0) {
            return Eval{};
        }
        --budget_;
        arrayActive_.insert(key);

        Eval out;
        out.k = kind == ElemKind::Char ? Eval::K::Chars : Eval::K::Bytes;
        out.complete = true;
        bool ok = true;

        if (token.k == StringFlowValue::K::Array) {
            const Insn* in = insnOf(token.site);
            const FrameV<StringFlowValue>* f = frameOf(token.site);
            if (in == nullptr || in->op != Op::NEWARRAY || f == nullptr || f->stack.empty() ||
                f->stack.back().bottom) {
                ok = false;
            } else {
                const StringFlowValue& lenTok = f->stack.back().val;
                if (lenTok.k != StringFlowValue::K::Int || lenTok.i < 0 ||
                    lenTok.i > kMaxArrayLength) {
                    ok = false;
                } else if (kind == ElemKind::Char) {
                    out.text.assign(static_cast<usize>(lenTok.i), 0);
                } else {
                    out.bytes.assign(static_cast<usize>(lenTok.i), 0);
                }
            }
        } else {
            const Eval base = eval(token);
            if (!base.complete) {
                ok = false;
            } else if (kind == ElemKind::Char &&
                       (base.k == Eval::K::Text || base.k == Eval::K::Chars)) {
                out = base;
                out.k = Eval::K::Chars;
            } else if (kind == ElemKind::Byte && base.k == Eval::K::Bytes) {
                out = base;
            } else {
                ok = false;
            }
        }

        if (ok && arrayTainted(token)) {
            ok = false;
        }

        const auto setElement = [&](usize idx, u16 value) {
            if (idx >= out.text.size()) {
                return false;
            }
            out.text[idx] = value;
            return true;
        };
        const auto setByte = [&](usize idx, u8 value) {
            if (idx >= out.bytes.size()) {
                return false;
            }
            out.bytes[idx] = value;
            return true;
        };
        const auto applyTransform = [&](i32 prev, Op op, i32 key) {
            if (op == Op::IXOR) {
                return prev ^ key;
            }
            if (op == Op::IADD) {
                return prev + key;
            }
            return prev - key;
        };
        const auto noteTransform = [&](Op op) {
            if (op == Op::IXOR) {
                out.fromXor = true;
            } else {
                out.fromShift = true;
            }
        };

        if (ok) {
            for (const InsnId& id : unit_->dom.order()) {
                const Insn* in = insnOf(id);
                if (in == nullptr) {
                    continue;
                }
                const bool charStore = kind == ElemKind::Char && in->op == Op::CASTORE;
                const bool byteStore = kind == ElemKind::Byte && in->op == Op::BASTORE;
                if (!charStore && !byteStore) {
                    continue;
                }
                const FrameV<StringFlowValue>* f = frameOf(id);
                if (f == nullptr || f->stack.size() < 3) {
                    continue;
                }
                const usize n = f->stack.size();
                if (f->stack[n - 1].bottom || f->stack[n - 2].bottom ||
                    f->stack[n - 3].bottom) {
                    continue;
                }
                if (!sameToken(f->stack[n - 3].val, token)) {
                    continue;
                }
                const StringFlowValue& idxTok = f->stack[n - 2].val;
                const StringFlowValue& valTok = f->stack[n - 1].val;
                const bool isXform = valTok.k == StringFlowValue::K::Xform &&
                                     valTok.srcKind == token.k &&
                                     sameSite(valTok.src, token.site);

                if (idxTok.k == StringFlowValue::K::Int) {
                    const i32 idx = idxTok.i;
                    if (idx < 0) {
                        ok = false;
                        continue;
                    }
                    if (valTok.k == StringFlowValue::K::Int) {
                        if (kind == ElemKind::Char) {
                            ok = setElement(static_cast<usize>(idx),
                                            static_cast<u16>(valTok.i & 0xFFFF)) && ok;
                        } else {
                            ok = setByte(static_cast<usize>(idx),
                                         static_cast<u8>(valTok.i & 0xFF)) && ok;
                        }
                    } else if (isXform) {
                        noteTransform(static_cast<Op>(valTok.xop));
                        if (kind == ElemKind::Char) {
                            if (static_cast<usize>(idx) >= out.text.size()) {
                                ok = false;
                                continue;
                            }
                            out.text[static_cast<usize>(idx)] = static_cast<u16>(
                                applyTransform(out.text[static_cast<usize>(idx)],
                                               static_cast<Op>(valTok.xop), valTok.i) &
                                0xFFFF);
                        } else {
                            if (static_cast<usize>(idx) >= out.bytes.size()) {
                                ok = false;
                                continue;
                            }
                            out.bytes[static_cast<usize>(idx)] = static_cast<u8>(
                                applyTransform(out.bytes[static_cast<usize>(idx)],
                                               static_cast<Op>(valTok.xop), valTok.i) &
                                0xFF);
                        }
                    } else {
                        ok = false;
                    }
                } else if (isXform) {
                    noteTransform(static_cast<Op>(valTok.xop));
                    out.fullTransform = true;
                    if (!out.transformHasInput) {
                        out.transformInput = out.text;
                        if (kind == ElemKind::Byte) {
                            out.transformInput.clear();
                            out.transformInput.reserve(out.bytes.size());
                            for (const u8 b : out.bytes) {
                                out.transformInput.push_back(b);
                            }
                        }
                        out.transformHasInput = true;
                    }
                    if (kind == ElemKind::Char) {
                        for (char16_t& c : out.text) {
                            c = static_cast<u16>(
                                applyTransform(c, static_cast<Op>(valTok.xop), valTok.i) &
                                0xFFFF);
                        }
                    } else {
                        for (u8& b : out.bytes) {
                            b = static_cast<u8>(
                                applyTransform(b, static_cast<Op>(valTok.xop), valTok.i) &
                                0xFF);
                        }
                    }
                } else {
                    ok = false;
                }
                if (out.text.size() > kMaxTextUnits || out.bytes.size() > kMaxTextUnits) {
                    ok = false;
                    break;
                }
            }
        }

        out.complete = ok;
        arrayActive_.erase(key);
        arrayMemo_.emplace(key, out);
        return out;
    }

    Eval charsOf(const StringFlowValue* token)
    {
        if (token == nullptr) {
            return Eval{};
        }
        Eval e = evalArrayLike(*token);
        if (!e.complete || e.k != Eval::K::Chars) {
            return Eval{};
        }
        return e;
    }

    Eval bytesOf(const StringFlowValue* token)
    {
        if (token == nullptr) {
            return Eval{};
        }
        Eval e = evalArrayLike(*token);
        if (!e.complete || e.k != Eval::K::Bytes) {
            return Eval{};
        }
        return e;
    }

    bool isRecognizedConsumer(const Insn& in) const
    {
        if (in.op != Op::INVOKEVIRTUAL && in.op != Op::INVOKESPECIAL &&
            in.op != Op::INVOKESTATIC && in.op != Op::INVOKEINTERFACE) {
            return false;
        }
        const MethodRef m = methodRefOf(*cp_, in.cp);
        if (!m.ok) {
            return false;
        }
        if (isBuilderOwner(m.owner)) {
            return m.name == "<init>" || m.name == "toString" ||
                   isBuilderMutationName(m.name);
        }
        if (m.owner == kStringOwner) {
            return m.name == "<init>" || isStringProducerName(m.name);
        }
        if (m.owner == kBase64DecoderOwner) {
            return m.name == "decode";
        }
        return m.owner == kBase64Owner &&
               (m.name == "getDecoder" || m.name == "getUrlDecoder");
    }

    const ConstantPool* cp_;
    const CodeUnit* unit_;
    const std::vector<FrameV<StringFlowValue>>& frames_;
    std::unordered_map<u32, usize> posOf_;
    std::unordered_map<TokenKey, Eval, TokenKeyHash> memo_;
    std::unordered_map<TokenKey, Eval, TokenKeyHash> arrayMemo_;
    std::unordered_set<TokenKey, TokenKeyHash> active_;
    std::unordered_set<TokenKey, TokenKeyHash> arrayActive_;
    u32 budget_ = 50000;
};

StringOrigin classifyOrigin(const Eval& e)
{
    if (e.fromBase64) {
        return StringOrigin::Base64Decode;
    }
    if (e.fromXor) {
        return StringOrigin::Xor;
    }
    if (e.fromShift) {
        return StringOrigin::CharShift;
    }
    if (e.fromChars) {
        return StringOrigin::CharArray;
    }
    if (e.fromBytes) {
        return StringOrigin::ByteArray;
    }
    if (e.fromBuilder) {
        return StringOrigin::Concat;
    }
    if (e.fromValueOf) {
        return StringOrigin::ValueOf;
    }
    return StringOrigin::Unknown;
}

bool isStringSink(const ConstantPool& cp, const Insn& in)
{
    if (in.op != Op::INVOKESPECIAL && in.op != Op::INVOKEVIRTUAL &&
        in.op != Op::INVOKESTATIC && in.op != Op::INVOKEINTERFACE) {
        return false;
    }
    const MethodRef m = methodRefOf(cp, in.cp);
    if (!m.ok) {
        return false;
    }
    if (m.owner == kStringOwner) {
        return m.name == "<init>" || m.name == "valueOf" || m.name == "copyValueOf";
    }
    return isBuilderOwner(m.owner) && m.name == "toString";
}

}   // namespace

// ---------------------------------------------------------------------------
// 结果对象
// ---------------------------------------------------------------------------

struct StringFlowResult::Impl
{
    jbc::analysis::FrameSummary summary;
    KeyPool pool;
    std::vector<StringFlowSource> sources;
    std::vector<StringFlowPipeline> pipelines;
};

StringFlowResult::StringFlowResult()
    : impl_(std::make_unique<Impl>())
{}

StringFlowResult::StringFlowResult(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{}

StringFlowResult::~StringFlowResult() = default;

StringFlowResult::StringFlowResult(StringFlowResult&& other) noexcept = default;

StringFlowResult& StringFlowResult::operator=(StringFlowResult&& other) noexcept = default;

const jbc::analysis::FrameSummary& StringFlowResult::summary() const noexcept
{
    return impl_->summary;
}

bool StringFlowResult::ok() const noexcept { return impl_->summary.ok; }

const std::vector<StringFlowSource>& StringFlowResult::sources() const noexcept
{
    return impl_->sources;
}

const std::vector<StringFlowPipeline>& StringFlowResult::pipelines() const noexcept
{
    return impl_->pipelines;
}

const StringFlowSource* StringFlowResult::sourceAt(jbc::ir::InsnId site) const noexcept
{
    for (const StringFlowSource& s : impl_->sources) {
        if (s.site.slot == site.slot) {
            return &s;
        }
    }
    return nullptr;
}

const StringFlowPipeline* StringFlowResult::pipelineAt(jbc::ir::InsnId site) const noexcept
{
    for (const StringFlowPipeline& p : impl_->pipelines) {
        if (p.site.slot == site.slot) {
            return &p;
        }
    }
    return nullptr;
}

StringFlowResult analyzeStringFlow(const ConstantPool& cp, const CodeUnit& unit)
{
    auto impl = std::make_unique<StringFlowResult::Impl>();

    StringFlowDomain dom(cp);
    jbc::analysis::DomainEngine<StringFlowValue> engine(cp, dom);
    DomainResult<StringFlowValue> result = engine.analyze(unit);
    impl->summary = result.summary;
    if (!result.summary.ok) {
        return StringFlowResult(std::move(impl));
    }

    Evaluator eval(cp, unit, result);
    const auto& order = unit.dom.order();

    for (const InsnId& id : order) {
        const Insn* in = unit.dom.peek(id);
        if (in == nullptr) {
            continue;
        }
        StringFlowValue token;
        Eval value;
        bool literal = false;

        if (isLdcString(cp, *in)) {
            literal = true;
            token = StringFlowValue::ofLiteral(in->cp, id);
            value = eval.eval(token);
        } else if (isStringSink(cp, *in)) {
            token = StringFlowValue::ofSite(StringFlowValue::K::Call, id);
            value = eval.eval(token);
        } else {
            continue;
        }

        StringFlowSource source;
        source.site = id;
        source.complete = value.complete && value.k == Eval::K::Text;
        if (literal) {
            source.origin = StringOrigin::Literal;
        } else if (source.complete) {
            source.origin = classifyOrigin(value);
        }
        if (source.complete) {
            const std::string utf8 = encodeMutf8(value.text);
            const u32 idInPool = impl->pool.intern(utf8);
            source.value = impl->pool.str(idInPool);
        }
        source.escaped = eval.escaped(token);
        impl->sources.push_back(source);

        const bool encoded = source.complete &&
                             (value.fromBase64 ||
                              ((value.fromXor || value.fromShift) && value.fullTransform));
        if (!encoded) {
            continue;
        }
        StringFlowPipeline pipeline;
        pipeline.site = id;
        if (value.fromBase64) {
            pipeline.kind = StringOrigin::Base64Decode;
        } else if (value.fromXor) {
            pipeline.kind = StringOrigin::Xor;
        } else {
            pipeline.kind = StringOrigin::CharShift;
        }
        if (source.complete) {
            pipeline.output = source.value;
        }
        if (value.fromBase64 && value.base64HasInput) {
            const u32 inId = impl->pool.intern(encodeMutf8(value.base64Input));
            pipeline.input = impl->pool.str(inId);
        } else if (!value.fromBase64 && value.transformHasInput) {
            const u32 inId = impl->pool.intern(encodeMutf8(value.transformInput));
            pipeline.input = impl->pool.str(inId);
        }
        impl->pipelines.push_back(pipeline);
    }

    return StringFlowResult(std::move(impl));
}

}   // namespace mjbase::asm_analyzer
