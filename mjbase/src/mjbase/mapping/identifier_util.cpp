#include "identifier_util.hpp"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

namespace mjbase::mapping {

namespace {

bool isPrintableAscii(ca::u32 cp)
{
    return cp >= 0x20 && cp <= 0x7E;
}

ca::u32 decodeUtf8(const ca::u8*& ptr, const ca::u8* end, bool& valid)
{
    valid = true;
    if (ptr >= end) return 0;
    ca::u8 b = *ptr;
    if (b < 0x80) {
        ptr++;
        return b;
    }
    int bytes = 0;
    if ((b & 0xE0) == 0xC0)
        bytes = 2;
    else if ((b & 0xF0) == 0xE0)
        bytes = 3;
    else if ((b & 0xF8) == 0xF0)
        bytes = 4;
    else {
        valid = false;
        ptr++;
        return 0xFFFD;
    }
    if (ptr + bytes > end) {
        valid = false;
        ptr   = end;
        return 0xFFFD;
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
    if ((bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000) ||
        cp > 0x10FFFF) {
        valid = false;
        cp    = 0xFFFD;
    }
    ptr += bytes;
    return cp;
}

bool needEscape(ca::u32 cp)
{
    if (cp == 0x20 || cp == '\\') return true;
    if (cp < 0x20 || cp == 0x7F) return true;
    if (cp >= 0x21 && cp <= 0x7E) return false;
    return true;
}

void appendHex4(ca::str::Utf8StringBuilder& sb, ca::u32 cp)
{
    char buf[8];
    int  n = std::snprintf(buf, sizeof(buf), "%04X", cp & 0xFFFFu);
    sb.append(reinterpret_cast<const ca::u8*>(buf), static_cast<ca::usize>(n));
}

ca::str::Utf8String escapeOne(ca::u32 cp)
{
    ca::str::Utf8StringBuilder sb;
    sb.append_code_point('\\');
    sb.append_code_point('u');
    appendHex4(sb, cp);
    return sb.build();
}

}   // anonymous namespace

const std::vector<ca::str::Utf8StringRef>& IdentifierUtil::blankChars()
{
    static const std::vector<ca::str::Utf8StringRef> chars = {
        ca::str::Utf8StringRef::from_cstr("\u0020"), ca::str::Utf8StringRef::from_cstr("\u00a0"),
        ca::str::Utf8StringRef::from_cstr("\u1680"), ca::str::Utf8StringRef::from_cstr("\u180e"),
        ca::str::Utf8StringRef::from_cstr("\u2000"), ca::str::Utf8StringRef::from_cstr("\u2001"),
        ca::str::Utf8StringRef::from_cstr("\u2002"), ca::str::Utf8StringRef::from_cstr("\u2003"),
        ca::str::Utf8StringRef::from_cstr("\u2004"), ca::str::Utf8StringRef::from_cstr("\u2005"),
        ca::str::Utf8StringRef::from_cstr("\u2006"), ca::str::Utf8StringRef::from_cstr("\u2007"),
        ca::str::Utf8StringRef::from_cstr("\u2008"), ca::str::Utf8StringRef::from_cstr("\u2009"),
        ca::str::Utf8StringRef::from_cstr("\u200a"), ca::str::Utf8StringRef::from_cstr("\u200b"),
        ca::str::Utf8StringRef::from_cstr("\u200c"), ca::str::Utf8StringRef::from_cstr("\u200d"),
        ca::str::Utf8StringRef::from_cstr("\u200e"), ca::str::Utf8StringRef::from_cstr("\u200f"),
        ca::str::Utf8StringRef::from_cstr("\u202f"), ca::str::Utf8StringRef::from_cstr("\u205f"),
        ca::str::Utf8StringRef::from_cstr("\u3000"), ca::str::Utf8StringRef::from_cstr("\ufeff"),
    };
    return chars;
}

const std::vector<ca::str::Utf8StringRef>& IdentifierUtil::javaKeyWords()
{
    static const std::vector<ca::str::Utf8StringRef> keywords = {
        ca::str::Utf8StringRef::from_cstr("abstract"),
        ca::str::Utf8StringRef::from_cstr("assert"),
        ca::str::Utf8StringRef::from_cstr("boolean"),
        ca::str::Utf8StringRef::from_cstr("break"),
        ca::str::Utf8StringRef::from_cstr("byte"),
        ca::str::Utf8StringRef::from_cstr("case"),
        ca::str::Utf8StringRef::from_cstr("catch"),
        ca::str::Utf8StringRef::from_cstr("char"),
        ca::str::Utf8StringRef::from_cstr("class"),
        ca::str::Utf8StringRef::from_cstr("const"),
        ca::str::Utf8StringRef::from_cstr("continue"),
        ca::str::Utf8StringRef::from_cstr("default"),
        ca::str::Utf8StringRef::from_cstr("do"),
        ca::str::Utf8StringRef::from_cstr("double"),
        ca::str::Utf8StringRef::from_cstr("else"),
        ca::str::Utf8StringRef::from_cstr("enum"),
        ca::str::Utf8StringRef::from_cstr("extends"),
        ca::str::Utf8StringRef::from_cstr("final"),
        ca::str::Utf8StringRef::from_cstr("finally"),
        ca::str::Utf8StringRef::from_cstr("float"),
        ca::str::Utf8StringRef::from_cstr("for"),
        ca::str::Utf8StringRef::from_cstr("goto"),
        ca::str::Utf8StringRef::from_cstr("if"),
        ca::str::Utf8StringRef::from_cstr("implements"),
        ca::str::Utf8StringRef::from_cstr("import"),
        ca::str::Utf8StringRef::from_cstr("instanceof"),
        ca::str::Utf8StringRef::from_cstr("int"),
        ca::str::Utf8StringRef::from_cstr("interface"),
        ca::str::Utf8StringRef::from_cstr("long"),
        ca::str::Utf8StringRef::from_cstr("native"),
        ca::str::Utf8StringRef::from_cstr("new"),
        ca::str::Utf8StringRef::from_cstr("package"),
        ca::str::Utf8StringRef::from_cstr("private"),
        ca::str::Utf8StringRef::from_cstr("protected"),
        ca::str::Utf8StringRef::from_cstr("public"),
        ca::str::Utf8StringRef::from_cstr("return"),
        ca::str::Utf8StringRef::from_cstr("short"),
        ca::str::Utf8StringRef::from_cstr("static"),
        ca::str::Utf8StringRef::from_cstr("strictfp"),
        ca::str::Utf8StringRef::from_cstr("super"),
        ca::str::Utf8StringRef::from_cstr("switch"),
        ca::str::Utf8StringRef::from_cstr("synchronized"),
        ca::str::Utf8StringRef::from_cstr("this"),
        ca::str::Utf8StringRef::from_cstr("throw"),
        ca::str::Utf8StringRef::from_cstr("throws"),
        ca::str::Utf8StringRef::from_cstr("transient"),
        ca::str::Utf8StringRef::from_cstr("try"),
        ca::str::Utf8StringRef::from_cstr("void"),
        ca::str::Utf8StringRef::from_cstr("volatile"),
        ca::str::Utf8StringRef::from_cstr("while"),
    };
    return keywords;
}

const std::unordered_map<ca::i32, ca::str::Utf8StringRef>& IdentifierUtil::opcodeNameMap()
{
    static const std::unordered_map<ca::i32, ca::str::Utf8StringRef> map = {
        {0, ca::str::Utf8StringRef::from_cstr("NOP")},
        {1, ca::str::Utf8StringRef::from_cstr("ACONST_NULL")},
        {2, ca::str::Utf8StringRef::from_cstr("ICONST_M1")},
        {3, ca::str::Utf8StringRef::from_cstr("ICONST_0")},
        {4, ca::str::Utf8StringRef::from_cstr("ICONST_1")},
        {5, ca::str::Utf8StringRef::from_cstr("ICONST_2")},
        {6, ca::str::Utf8StringRef::from_cstr("ICONST_3")},
        {7, ca::str::Utf8StringRef::from_cstr("ICONST_4")},
        {8, ca::str::Utf8StringRef::from_cstr("ICONST_5")},
        {9, ca::str::Utf8StringRef::from_cstr("LCONST_0")},
        {10, ca::str::Utf8StringRef::from_cstr("LCONST_1")},
        {11, ca::str::Utf8StringRef::from_cstr("FCONST_0")},
        {12, ca::str::Utf8StringRef::from_cstr("FCONST_1")},
        {13, ca::str::Utf8StringRef::from_cstr("FCONST_2")},
        {14, ca::str::Utf8StringRef::from_cstr("DCONST_0")},
        {15, ca::str::Utf8StringRef::from_cstr("DCONST_1")},
        {16, ca::str::Utf8StringRef::from_cstr("BIPUSH")},
        {17, ca::str::Utf8StringRef::from_cstr("SIPUSH")},
        {18, ca::str::Utf8StringRef::from_cstr("LDC")},
        {21, ca::str::Utf8StringRef::from_cstr("ILOAD")},
        {22, ca::str::Utf8StringRef::from_cstr("LLOAD")},
        {23, ca::str::Utf8StringRef::from_cstr("FLOAD")},
        {24, ca::str::Utf8StringRef::from_cstr("DLOAD")},
        {25, ca::str::Utf8StringRef::from_cstr("ALOAD")},
        {46, ca::str::Utf8StringRef::from_cstr("IALOAD")},
        {47, ca::str::Utf8StringRef::from_cstr("LALOAD")},
        {48, ca::str::Utf8StringRef::from_cstr("FALOAD")},
        {49, ca::str::Utf8StringRef::from_cstr("DALOAD")},
        {50, ca::str::Utf8StringRef::from_cstr("AALOAD")},
        {51, ca::str::Utf8StringRef::from_cstr("BALOAD")},
        {52, ca::str::Utf8StringRef::from_cstr("CALOAD")},
        {53, ca::str::Utf8StringRef::from_cstr("SALOAD")},
        {54, ca::str::Utf8StringRef::from_cstr("ISTORE")},
        {55, ca::str::Utf8StringRef::from_cstr("LSTORE")},
        {56, ca::str::Utf8StringRef::from_cstr("FSTORE")},
        {57, ca::str::Utf8StringRef::from_cstr("DSTORE")},
        {58, ca::str::Utf8StringRef::from_cstr("ASTORE")},
        {79, ca::str::Utf8StringRef::from_cstr("IASTORE")},
        {80, ca::str::Utf8StringRef::from_cstr("LASTORE")},
        {81, ca::str::Utf8StringRef::from_cstr("FASTORE")},
        {82, ca::str::Utf8StringRef::from_cstr("DASTORE")},
        {83, ca::str::Utf8StringRef::from_cstr("AASTORE")},
        {84, ca::str::Utf8StringRef::from_cstr("BASTORE")},
        {85, ca::str::Utf8StringRef::from_cstr("CASTORE")},
        {86, ca::str::Utf8StringRef::from_cstr("SASTORE")},
        {87, ca::str::Utf8StringRef::from_cstr("POP")},
        {88, ca::str::Utf8StringRef::from_cstr("POP2")},
        {89, ca::str::Utf8StringRef::from_cstr("DUP")},
        {90, ca::str::Utf8StringRef::from_cstr("DUP_X1")},
        {91, ca::str::Utf8StringRef::from_cstr("DUP_X2")},
        {92, ca::str::Utf8StringRef::from_cstr("DUP2")},
        {93, ca::str::Utf8StringRef::from_cstr("DUP2_X1")},
        {94, ca::str::Utf8StringRef::from_cstr("DUP2_X2")},
        {95, ca::str::Utf8StringRef::from_cstr("SWAP")},
        {96, ca::str::Utf8StringRef::from_cstr("IADD")},
        {97, ca::str::Utf8StringRef::from_cstr("LADD")},
        {98, ca::str::Utf8StringRef::from_cstr("FADD")},
        {99, ca::str::Utf8StringRef::from_cstr("DADD")},
        {100, ca::str::Utf8StringRef::from_cstr("ISUB")},
        {101, ca::str::Utf8StringRef::from_cstr("LSUB")},
        {102, ca::str::Utf8StringRef::from_cstr("FSUB")},
        {103, ca::str::Utf8StringRef::from_cstr("DSUB")},
        {104, ca::str::Utf8StringRef::from_cstr("IMUL")},
        {105, ca::str::Utf8StringRef::from_cstr("LMUL")},
        {106, ca::str::Utf8StringRef::from_cstr("FMUL")},
        {107, ca::str::Utf8StringRef::from_cstr("DMUL")},
        {108, ca::str::Utf8StringRef::from_cstr("IDIV")},
        {109, ca::str::Utf8StringRef::from_cstr("LDIV")},
        {110, ca::str::Utf8StringRef::from_cstr("FDIV")},
        {111, ca::str::Utf8StringRef::from_cstr("DDIV")},
        {112, ca::str::Utf8StringRef::from_cstr("IREM")},
        {113, ca::str::Utf8StringRef::from_cstr("LREM")},
        {114, ca::str::Utf8StringRef::from_cstr("FREM")},
        {115, ca::str::Utf8StringRef::from_cstr("DREM")},
        {116, ca::str::Utf8StringRef::from_cstr("INEG")},
        {117, ca::str::Utf8StringRef::from_cstr("LNEG")},
        {118, ca::str::Utf8StringRef::from_cstr("FNEG")},
        {119, ca::str::Utf8StringRef::from_cstr("DNEG")},
        {120, ca::str::Utf8StringRef::from_cstr("ISHL")},
        {121, ca::str::Utf8StringRef::from_cstr("LSHL")},
        {122, ca::str::Utf8StringRef::from_cstr("ISHR")},
        {123, ca::str::Utf8StringRef::from_cstr("LSHR")},
        {124, ca::str::Utf8StringRef::from_cstr("IUSHR")},
        {125, ca::str::Utf8StringRef::from_cstr("LUSHR")},
        {126, ca::str::Utf8StringRef::from_cstr("IAND")},
        {127, ca::str::Utf8StringRef::from_cstr("LAND")},
        {128, ca::str::Utf8StringRef::from_cstr("IOR")},
        {129, ca::str::Utf8StringRef::from_cstr("LOR")},
        {130, ca::str::Utf8StringRef::from_cstr("IXOR")},
        {131, ca::str::Utf8StringRef::from_cstr("LXOR")},
        {132, ca::str::Utf8StringRef::from_cstr("IINC")},
        {133, ca::str::Utf8StringRef::from_cstr("I2L")},
        {134, ca::str::Utf8StringRef::from_cstr("I2F")},
        {135, ca::str::Utf8StringRef::from_cstr("I2D")},
        {136, ca::str::Utf8StringRef::from_cstr("L2I")},
        {137, ca::str::Utf8StringRef::from_cstr("L2F")},
        {138, ca::str::Utf8StringRef::from_cstr("L2D")},
        {139, ca::str::Utf8StringRef::from_cstr("F2I")},
        {140, ca::str::Utf8StringRef::from_cstr("F2L")},
        {141, ca::str::Utf8StringRef::from_cstr("F2D")},
        {142, ca::str::Utf8StringRef::from_cstr("D2I")},
        {143, ca::str::Utf8StringRef::from_cstr("D2L")},
        {144, ca::str::Utf8StringRef::from_cstr("D2F")},
        {145, ca::str::Utf8StringRef::from_cstr("I2B")},
        {146, ca::str::Utf8StringRef::from_cstr("I2C")},
        {147, ca::str::Utf8StringRef::from_cstr("I2S")},
        {148, ca::str::Utf8StringRef::from_cstr("LCMP")},
        {149, ca::str::Utf8StringRef::from_cstr("FCMPL")},
        {150, ca::str::Utf8StringRef::from_cstr("FCMPG")},
        {151, ca::str::Utf8StringRef::from_cstr("DCMPL")},
        {152, ca::str::Utf8StringRef::from_cstr("DCMPG")},
        {153, ca::str::Utf8StringRef::from_cstr("IFEQ")},
        {154, ca::str::Utf8StringRef::from_cstr("IFNE")},
        {155, ca::str::Utf8StringRef::from_cstr("IFLT")},
        {156, ca::str::Utf8StringRef::from_cstr("IFGE")},
        {157, ca::str::Utf8StringRef::from_cstr("IFGT")},
        {158, ca::str::Utf8StringRef::from_cstr("IFLE")},
        {159, ca::str::Utf8StringRef::from_cstr("IF_ICMPEQ")},
        {160, ca::str::Utf8StringRef::from_cstr("IF_ICMPNE")},
        {161, ca::str::Utf8StringRef::from_cstr("IF_ICMPLT")},
        {162, ca::str::Utf8StringRef::from_cstr("IF_ICMPGE")},
        {163, ca::str::Utf8StringRef::from_cstr("IF_ICMPGT")},
        {164, ca::str::Utf8StringRef::from_cstr("IF_ICMPLE")},
        {165, ca::str::Utf8StringRef::from_cstr("IF_ACMPEQ")},
        {166, ca::str::Utf8StringRef::from_cstr("IF_ACMPNE")},
        {167, ca::str::Utf8StringRef::from_cstr("GOTO")},
        {168, ca::str::Utf8StringRef::from_cstr("JSR")},
        {169, ca::str::Utf8StringRef::from_cstr("RET")},
        {170, ca::str::Utf8StringRef::from_cstr("TABLESWITCH")},
        {171, ca::str::Utf8StringRef::from_cstr("LOOKUPSWITCH")},
        {172, ca::str::Utf8StringRef::from_cstr("IRETURN")},
        {173, ca::str::Utf8StringRef::from_cstr("LRETURN")},
        {174, ca::str::Utf8StringRef::from_cstr("FRETURN")},
        {175, ca::str::Utf8StringRef::from_cstr("DRETURN")},
        {176, ca::str::Utf8StringRef::from_cstr("ARETURN")},
        {177, ca::str::Utf8StringRef::from_cstr("RETURN")},
        {178, ca::str::Utf8StringRef::from_cstr("GETSTATIC")},
        {179, ca::str::Utf8StringRef::from_cstr("PUTSTATIC")},
        {180, ca::str::Utf8StringRef::from_cstr("GETFIELD")},
        {181, ca::str::Utf8StringRef::from_cstr("PUTFIELD")},
        {182, ca::str::Utf8StringRef::from_cstr("INVOKEVIRTUAL")},
        {183, ca::str::Utf8StringRef::from_cstr("INVOKESPECIAL")},
        {184, ca::str::Utf8StringRef::from_cstr("INVOKESTATIC")},
        {185, ca::str::Utf8StringRef::from_cstr("INVOKEINTERFACE")},
        {186, ca::str::Utf8StringRef::from_cstr("INVOKEDYNAMIC")},
        {187, ca::str::Utf8StringRef::from_cstr("NEW")},
        {188, ca::str::Utf8StringRef::from_cstr("NEWARRAY")},
        {189, ca::str::Utf8StringRef::from_cstr("ANEWARRAY")},
        {190, ca::str::Utf8StringRef::from_cstr("ARRAYLENGTH")},
        {191, ca::str::Utf8StringRef::from_cstr("ATHROW")},
        {192, ca::str::Utf8StringRef::from_cstr("CHECKCAST")},
        {193, ca::str::Utf8StringRef::from_cstr("INSTANCEOF")},
        {194, ca::str::Utf8StringRef::from_cstr("MONITORENTER")},
        {195, ca::str::Utf8StringRef::from_cstr("MONITOREXIT")},
        {197, ca::str::Utf8StringRef::from_cstr("MULTIANEWARRAY")},
        {198, ca::str::Utf8StringRef::from_cstr("IFNULL")},
        {199, ca::str::Utf8StringRef::from_cstr("IFNONNULL")},
    };
    return map;
}

ca::str::Utf8StringRef IdentifierUtil::alphabets()
{
    static const auto s =
        ca::str::Utf8StringRef::from_cstr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    return s;
}

ca::str::Utf8StringRef IdentifierUtil::numbers()
{
    static const auto s = ca::str::Utf8StringRef::from_cstr("0123456789");
    return s;
}

ca::str::Utf8String IdentifierUtil::escapeUnicode(ca::str::Utf8StringRef utf8Str)
{
    auto*                      ptr = utf8Str.data();
    auto*                      end = ptr + utf8Str.byte_length();
    ca::str::Utf8StringBuilder sb;
    while (ptr < end) {
        bool    valid = true;
        ca::u32 cp    = decodeUtf8(ptr, end, valid);
        if (!valid) {
            sb.append(ca::str::Utf8StringRef::from_cstr("\\uFFFD"));
            continue;
        }
        if (needEscape(cp)) {
            sb.append_code_point('\\');
            sb.append_code_point('u');
            appendHex4(sb, cp);
        }
        else {
            sb.append_code_point(cp);
        }
    }
    return sb.build();
}

ca::str::Utf8String IdentifierUtil::escapeCppNameString(ca::str::Utf8StringRef value)
{
    auto*                      ptr = value.data();
    auto*                      end = ptr + value.byte_length();
    ca::str::Utf8StringBuilder sb;
    while (ptr < end) {
        bool    valid = true;
        ca::u32 cp    = decodeUtf8(ptr, end, valid);
        if (!valid) {
            sb.append_code_point('_');
            continue;
        }
        if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
            cp == '_') {
            sb.append_code_point(cp);
        }
        else {
            sb.append_code_point('u');
            char buf[16];
            int  n = std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(cp));
            sb.append(reinterpret_cast<const ca::u8*>(buf), static_cast<ca::usize>(n));
        }
    }
    auto built = sb.build();
    if (built.byte_length() > 0) {
        ca::u8 first = built.data()[0];
        if (first >= '0' && first <= '9') {
            ca::str::Utf8StringBuilder prefixed;
            prefixed.append_code_point('_');
            prefixed.append(built.ref());
            return prefixed.build();
        }
    }
    return built;
}

ca::str::Utf8String IdentifierUtil::escapeCommentString(ca::str::Utf8StringRef value)
{
    auto*                      ptr = value.data();
    auto*                      end = ptr + value.byte_length();
    ca::str::Utf8StringBuilder sb;
    while (ptr < end) {
        bool    valid = true;
        ca::u32 cp    = decodeUtf8(ptr, end, valid);
        if (!valid) {
            sb.append(ca::str::Utf8StringRef::from_cstr("\\uFFFD"));
            continue;
        }
        if (cp >= 32 && cp <= 126) {
            sb.append_code_point(cp);
        }
        else {
            sb.append_code_point('\\');
            sb.append_code_point('u');
            appendHex4(sb, cp);
        }
    }
    return sb.build();
}

ca::str::Utf8String IdentifierUtil::unicodify(ca::str::Utf8StringRef value)
{
    auto*                      ptr = value.data();
    auto*                      end = ptr + value.byte_length();
    ca::str::Utf8StringBuilder sb;
    while (ptr < end) {
        bool    valid = true;
        ca::u32 cp    = decodeUtf8(ptr, end, valid);
        if (!valid) {
            sb.append(ca::str::Utf8StringRef::from_cstr("\\uFFFD"));
            continue;
        }
        sb.append_code_point('\\');
        sb.append_code_point('u');
        appendHex4(sb, cp);
    }
    return sb.build();
}

ca::str::Utf8StringRef IdentifierUtil::getOpcodeString(ca::i32 opcode)
{
    auto& map = opcodeNameMap();
    auto  it  = map.find(opcode);
    if (it != map.end()) return it->second;
    return ca::str::Utf8StringRef::from_cstr("UNKNOWN");
}

bool IdentifierUtil::isValidJavaIdentifier(ca::str::Utf8StringRef name)
{
    if (name.is_empty()) return false;
    auto*   ptr     = name.data();
    auto*   end     = ptr + name.byte_length();
    bool    valid   = true;
    ca::u32 firstCp = decodeUtf8(ptr, end, valid);
    if (!valid) return false;
    if (!((firstCp >= 'a' && firstCp <= 'z') || (firstCp >= 'A' && firstCp <= 'Z') ||
          firstCp == '_' || firstCp == '$' || firstCp > 0x80))
        return false;
    while (ptr < end) {
        ca::u32 cp = decodeUtf8(ptr, end, valid);
        if (!valid) return false;
        if (!((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
              cp == '_' || cp == '$' || cp > 0x80))
            return false;
    }
    return true;
}

bool IdentifierUtil::isValidJavaFullClassName(ca::str::Utf8StringRef fullName)
{
    if (fullName.is_empty()) return false;
    if (fullName.ends_with(ca::str::Utf8StringRef::from_cstr("."))) return false;
    auto parts = fullName.split(ca::str::Utf8StringRef::from_cstr("."));
    for (const auto& part : parts) {
        if (part.is_empty() || !isValidJavaIdentifier(part)) return false;
    }
    return true;
}

bool IdentifierUtil::isLegalJavaIdentifierChar(ca::u32 cp)
{
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9'))
        return cp < 0x80;
    return cp == '_' || cp == '$';
}

ca::str::Utf8String IdentifierUtil::optimizeIllegalChar(ca::u32 cp)
{
    ca::str::Utf8StringBuilder sb;
    if (!isLegalJavaIdentifierChar(cp)) {
        sb.append_code_point('\\');
        sb.append_code_point('u');
        appendHex4(sb, cp);
    }
    else {
        sb.append_code_point(cp);
    }
    return sb.build();
}

ca::str::Utf8String IdentifierUtil::optimizeIllegalString(ca::str::Utf8StringRef str)
{
    auto*                      ptr = str.data();
    auto*                      end = ptr + str.byte_length();
    ca::str::Utf8StringBuilder sb;
    while (ptr < end) {
        bool    valid = true;
        ca::u32 cp    = decodeUtf8(ptr, end, valid);
        if (!valid) {
            sb.append(ca::str::Utf8StringRef::from_cstr("\\uFFFD"));
            continue;
        }
        sb.append(optimizeIllegalChar(cp).ref());
    }
    return sb.build();
}

bool IdentifierUtil::isPrimitiveDescChar(char c)
{
    return c == 'V' || c == 'I' || c == 'Z' || c == 'B' || c == 'C' || c == 'S' || c == 'D' ||
           c == 'F' || c == 'J';
}

ca::str::Utf8String IdentifierUtil::optimizeIllegalDesc(ca::str::Utf8StringRef desc)
{
    auto* ptr = desc.data();
    auto* end = ptr + desc.byte_length();
    if (ptr >= end) return {};
    ca::str::Utf8StringBuilder sb;
    sb.append_code_point(*ptr);
    ptr++;
    while (ptr < end) {
        ca::u8 ch = *ptr;
        if (isPrimitiveDescChar(static_cast<char>(ch))) {
            sb.append_code_point(ch);
            ptr++;
        }
        else if (ch == ')') {
            sb.append_code_point(ch);
            ptr++;
        }
        else {
            const ca::u8* semi = static_cast<const ca::u8*>(std::memchr(ptr, ';', end - ptr));
            if (!semi) break;
            const ca::u8* typeEnd = semi + 1;
            while (ptr < typeEnd) {
                ca::u8 c2 = *ptr;
                if (c2 == '/' || c2 == ';') {
                    sb.append_code_point(c2);
                }
                else {
                    bool    valid  = true;
                    auto    tmpPtr = ptr;
                    ca::u32 cp     = decodeUtf8(tmpPtr, end, valid);
                    if (valid && isLegalJavaIdentifierChar(cp)) {
                        for (auto p = ptr; p < tmpPtr; p++) sb.append_code_point(*p);
                    }
                    else {
                        sb.append(optimizeIllegalChar(cp).ref());
                    }
                    ptr = tmpPtr;
                    continue;
                }
                ptr++;
            }
        }
    }
    return sb.build();
}

// ── std::string 系列：printReadable / unescapeUnicode ──────────────────

std::string IdentifierUtil::printReadable(const std::string& raw, int collapseThreshold)
{
    if (raw.empty()) return {};

    // 1) 拆成 token 列表（复用本文件 anonymous namespace 的 decodeUtf8/needEscape）
    std::vector<std::string> tokens;
    const ca::u8*            ptr = reinterpret_cast<const ca::u8*>(raw.data());
    const ca::u8*            end = ptr + raw.size();
    while (ptr < end) {
        bool valid = true;
        ca::u32 cp = decodeUtf8(ptr, end, valid);   // 非法字节→0xFFFD，与 escapeUnicode 一致
        if (!needEscape(cp)) {
            tokens.emplace_back(1, static_cast<char>(cp));
        }
        else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04X", cp & 0xFFFFu);
            tokens.emplace_back(buf);
        }
    }
    if (tokens.empty()) return {};

    // 2) 游程压缩：连续相同 token ≥ threshold 且 run ≥ 3 时压成
    //    <token>...(省略N个)...<token>，N = run - 2
    std::string out;
    std::size_t i = 0;
    while (i < tokens.size()) {
        std::size_t runLen = 1;
        while (i + runLen < tokens.size() && tokens[i + runLen] == tokens[i]) {
            runLen++;
        }
        if (static_cast<int>(runLen) >= collapseThreshold && runLen >= 3) {
            out += tokens[i];
            char omitted[32];
            std::snprintf(omitted, sizeof(omitted), "...(省略%zu个)...", runLen - 2);
            out += omitted;
            out += tokens[i];
        }
        else {
            for (std::size_t k = 0; k < runLen; k++) out += tokens[i];
        }
        i += runLen;
    }
    return out;
}

std::string IdentifierUtil::unescapeUnicode(const std::string& escaped)
{
    std::string out;
    out.reserve(escaped.size());
    std::size_t i = 0;
    while (i < escaped.size()) {
        // 识别 \uXXXX
        if (escaped[i] == '\\' && i + 5 < escaped.size() + 1 && i + 1 < escaped.size() &&
            escaped[i + 1] == 'u') {
            // 需 i+2..i+5 共 4 个 hex 字符
            if (i + 5 < escaped.size() + 1 && i + 6 <= escaped.size()) {
                bool    allHex = true;
                ca::u32 cp     = 0;
                for (int k = 0; k < 4; k++) {
                    char c = escaped[i + 2 + k];
                    int  v;
                    if (c >= '0' && c <= '9')
                        v = c - '0';
                    else if (c >= 'a' && c <= 'f')
                        v = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F')
                        v = c - 'A' + 10;
                    else {
                        allHex = false;
                        break;
                    }
                    cp = (cp << 4) | static_cast<ca::u32>(v);
                }
                if (allHex && cp <= 0x10FFFF) {
                    // codepoint → UTF-8 编码追加
                    if (cp < 0x80) {
                        out.push_back(static_cast<char>(cp));
                    }
                    else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    else if (cp < 0x10000) {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    else {
                        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    i += 6;
                    continue;
                }
            }
        }
        // 非转义字符或非法 \u 序列：原样透传
        out.push_back(escaped[i]);
        i++;
    }
    return out;
}

}   // namespace mjbase::mapping
