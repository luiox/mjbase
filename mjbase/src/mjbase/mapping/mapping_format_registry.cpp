#include "mapping_format_registry.hpp"

#include <algorithm>
#include <initializer_list>
#include <unordered_map>
#include <utility>

#include "shadow_alias_format.hpp"
#include "tiny_v2_mapping.hpp"

namespace mjbase::mapping {
namespace {

using Utf8String        = ca::str::Utf8String;
using Utf8StringBuilder = ca::str::Utf8StringBuilder;
using Utf8StringRef     = ca::str::Utf8StringRef;

Utf8StringRef ref(const char* text)
{
    return Utf8StringRef::from_cstr(text);
}

std::string hexEncode(const std::string& text)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string           result;
    result.reserve(text.size() * 2);
    for (const auto ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

int hexDigit(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string hexDecode(const std::string& text)
{
    if ((text.size() & 1u) != 0) return {};
    std::string result;
    result.reserve(text.size() / 2);
    for (ca::usize i = 0; i < text.size(); i += 2) {
        const auto high = hexDigit(text[i]);
        const auto low  = hexDigit(text[i + 1]);
        if (high < 0 || low < 0) return {};
        result.push_back(static_cast<char>((high << 4) | low));
    }
    return result;
}

std::vector<Utf8StringRef> linesOfRef(Utf8StringRef text);
ca::usize                  findByte(Utf8StringRef text, ca::u8 needle, ca::usize start);
Utf8StringRef              trimRef(Utf8StringRef text);

bool isXmlMappingFormat(Utf8StringRef format)
{
    return format.equals(ref("YGuard")) || format.equals(ref("Allatori"));
}

void appendEmbeddedSources(Utf8StringBuilder& out, Utf8StringRef target,
                           const IntermediateMapping& mappings)
{
    const bool xml = isXmlMappingFormat(target);
    for (const auto& [format, source] : mappings.sourceSnapshots()) {
        if (format == target.to_std_string()) continue;
        out.append(xml ? "<!-- mjbase-source " : "# mjbase-source ");
        out.append(Utf8StringRef::from_cstr(format.c_str()));
        out.append("\t");
        const auto encoded = hexEncode(source);
        out.append(Utf8StringRef::from_cstr(encoded.c_str()));
        out.append(xml ? " -->\n" : "\n");
    }
}

void restoreEmbeddedSources(IntermediateMapping& mappings, Utf8StringRef text)
{
    constexpr const char* lineMarker = "# mjbase-source ";
    constexpr const char* xmlMarker  = "<!-- mjbase-source ";
    for (const auto line : linesOfRef(text)) {
        auto        value = trimRef(line).to_std_string();
        std::string payload;
        if (value.rfind(lineMarker, 0) == 0) {
            payload = value.substr(std::char_traits<char>::length(lineMarker));
        }
        else if (value.rfind(xmlMarker, 0) == 0 && value.size() >= 3 &&
                 value.compare(value.size() - 3, 3, "-->") == 0) {
            payload = value.substr(std::char_traits<char>::length(xmlMarker));
            payload.resize(payload.size() - 3);
            while (!payload.empty() && payload.back() == ' ') payload.pop_back();
        }
        else {
            continue;
        }
        const auto split = payload.rfind('\t');
        if (split == std::string::npos) continue;
        const auto format  = payload.substr(0, split);
        const auto decoded = hexDecode(payload.substr(split + 1));
        if (format.empty() || decoded.empty()) continue;
        mappings.rememberSource(Utf8StringRef::from_cstr(format.c_str()),
                                Utf8StringRef::from_cstr(decoded.c_str()));
    }
}

bool asciiSpace(ca::u8 ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

Utf8StringRef trimRef(Utf8StringRef text)
{
    ca::usize start = 0;
    while (start < text.byte_length() && asciiSpace(text.byte_at(start))) start++;

    ca::usize end = text.byte_length();
    while (end > start && asciiSpace(text.byte_at(end - 1))) end--;
    return text.slice(start, end);
}

ca::usize findByte(Utf8StringRef text, ca::u8 needle, ca::usize start = 0)
{
    for (ca::usize i = start; i < text.byte_length(); ++i) {
        if (text.byte_at(i) == needle) return i;
    }
    return Utf8StringRef::npos;
}

ca::usize findRef(Utf8StringRef text, Utf8StringRef needle, ca::usize start = 0)
{
    if (needle.is_empty()) return start <= text.byte_length() ? start : Utf8StringRef::npos;
    if (needle.byte_length() > text.byte_length() ||
        start > text.byte_length() - needle.byte_length()) {
        return Utf8StringRef::npos;
    }

    for (ca::usize i = start; i <= text.byte_length() - needle.byte_length(); ++i) {
        bool found = true;
        for (ca::usize j = 0; j < needle.byte_length(); ++j) {
            if (text.byte_at(i + j) != needle.byte_at(j)) {
                found = false;
                break;
            }
        }
        if (found) return i;
    }
    return Utf8StringRef::npos;
}

ca::usize rfindByte(Utf8StringRef text, ca::u8 needle, ca::usize end = Utf8StringRef::npos)
{
    if (end == Utf8StringRef::npos || end > text.byte_length()) end = text.byte_length();
    while (end > 0) {
        end--;
        if (text.byte_at(end) == needle) return end;
    }
    return Utf8StringRef::npos;
}

bool endsWithRef(Utf8StringRef text, Utf8StringRef suffix)
{
    return text.byte_length() >= suffix.byte_length() &&
           text.slice(text.byte_length() - suffix.byte_length(), text.byte_length()).equals(suffix);
}

std::vector<Utf8StringRef> splitRef(Utf8StringRef text, ca::u8 delimiter)
{
    std::vector<Utf8StringRef> parts;
    ca::usize                  start = 0;
    for (ca::usize i = 0; i < text.byte_length(); ++i) {
        if (text.byte_at(i) != delimiter) continue;
        parts.push_back(text.slice(start, i));
        start = i + 1;
    }
    parts.push_back(text.slice(start, text.byte_length()));
    return parts;
}

std::vector<Utf8StringRef> splitWhitespaceRef(Utf8StringRef text)
{
    std::vector<Utf8StringRef> parts;
    ca::usize                  i = 0;
    while (i < text.byte_length()) {
        while (i < text.byte_length() && asciiSpace(text.byte_at(i))) i++;
        ca::usize start = i;
        while (i < text.byte_length() && !asciiSpace(text.byte_at(i))) i++;
        if (i > start) parts.push_back(text.slice(start, i));
    }
    return parts;
}

std::vector<Utf8StringRef> linesOfRef(Utf8StringRef text)
{
    auto lines = splitRef(text, '\n');
    for (auto& line : lines) {
        if (!line.is_empty() && line.byte_at(line.byte_length() - 1) == '\r') {
            line = line.slice(0, line.byte_length() - 1);
        }
    }
    return lines;
}

Utf8String normalizeClassNameRef(Utf8StringRef name)
{
    return name.replace_all(ref("."), ref("/"));
}

Utf8String dotClassNameRef(Utf8StringRef name)
{
    return name.replace_all(ref("/"), ref("."));
}

Utf8String unescapeSimpleRef(Utf8StringRef text)
{
    ca::str::Utf8StringBuilder out;
    bool                       escaped = false;
    for (ca::usize i = 0; i < text.byte_length(); ++i) {
        ca::u8 c = text.byte_at(i);
        if (escaped) {
            out.append_code_point(c);
            escaped = false;
        }
        else if (c == '\\') {
            escaped = true;
        }
        else {
            out.append_code_point(c);
        }
    }
    if (escaped) out.append_code_point('\\');
    return out.build();
}

void appendSimpleEscaped(Utf8StringBuilder& out, Utf8StringRef text)
{
    for (ca::usize i = 0; i < text.byte_length(); ++i) {
        auto c = text.byte_at(i);
        if (c == ' ' || c == '\\') out.append_code_point('\\');
        out.append_code_point(c);
    }
}

void appendDotClassName(Utf8StringBuilder& out, Utf8StringRef name)
{
    for (ca::usize i = 0; i < name.byte_length(); ++i) {
        auto c = name.byte_at(i);
        out.append_code_point(c == '/' ? '.' : c);
    }
}

Utf8String descriptorForJavaTypeRef(
    Utf8StringRef type, const std::unordered_map<Utf8StringRef, Utf8StringRef>& classMap)
{
    type                 = trimRef(type);
    ca::usize dimensions = 0;
    while (endsWithRef(type, ref("[]"))) {
        dimensions++;
        type = type.slice(0, type.byte_length() - 2);
    }

    Utf8StringBuilder out;
    for (ca::usize i = 0; i < dimensions; ++i) out.append("[");

    if (type.equals("void"))
        out.append("V");
    else if (type.equals("boolean"))
        out.append("Z");
    else if (type.equals("byte"))
        out.append("B");
    else if (type.equals("char"))
        out.append("C");
    else if (type.equals("short"))
        out.append("S");
    else if (type.equals("int"))
        out.append("I");
    else if (type.equals("float"))
        out.append("F");
    else if (type.equals("long"))
        out.append("J");
    else if (type.equals("double"))
        out.append("D");
    else {
        auto internal = normalizeClassNameRef(type);
        auto mappedIt = classMap.find(internal.ref());
        out.append("L");
        out.append(mappedIt == classMap.end() ? internal.ref() : mappedIt->second);
        out.append(";");
    }
    return out.build();
}

Utf8StringRef mappedClassOrSelf(const IntermediateMapping& mappings, Utf8StringRef internalName)
{
    auto mapped = mappings.getMappedClassName(internalName);
    return mapped.is_empty() ? internalName : mapped;
}

void appendMappedDescriptor(Utf8StringBuilder& out, const IntermediateMapping& mappings,
                            Utf8StringRef desc)
{
    for (ca::usize i = 0; i < desc.byte_length(); ++i) {
        auto c = desc.byte_at(i);
        if (c != 'L') {
            out.append_code_point(c);
            continue;
        }

        auto end = findByte(desc, ';', i + 1);
        if (end == Utf8StringRef::npos) {
            out.append_code_point(c);
            continue;
        }

        out.append("L");
        out.append(mappedClassOrSelf(mappings, desc.slice(i + 1, end)));
        out.append(";");
        i = end;
    }
}

bool appendJavaTypeFromDescriptor(Utf8StringBuilder& out, const IntermediateMapping& mappings,
                                  Utf8StringRef desc, ca::usize& pos)
{
    ca::usize dimensions = 0;
    while (pos < desc.byte_length() && desc.byte_at(pos) == '[') {
        dimensions++;
        pos++;
    }
    if (pos >= desc.byte_length()) return false;

    switch (desc.byte_at(pos++)) {
    case 'V': out.append("void"); break;
    case 'Z': out.append("boolean"); break;
    case 'B': out.append("byte"); break;
    case 'C': out.append("char"); break;
    case 'S': out.append("short"); break;
    case 'I': out.append("int"); break;
    case 'F': out.append("float"); break;
    case 'J': out.append("long"); break;
    case 'D': out.append("double"); break;
    case 'L':
    {
        auto end = findByte(desc, ';', pos);
        if (end == Utf8StringRef::npos) return false;
        appendDotClassName(out, mappedClassOrSelf(mappings, desc.slice(pos, end)));
        pos = end + 1;
        break;
    }
    default: return false;
    }

    for (ca::usize i = 0; i < dimensions; ++i) out.append("[]");
    return true;
}

std::vector<Utf8StringRef> sortedClassNames(const IntermediateMapping& mappings)
{
    std::vector<Utf8StringRef> names;
    names.reserve(mappings.getClasses().size() + mappings.getFields().size() +
                  mappings.getMethods().size());
    for (const auto& [key, cm] : mappings.getClasses()) names.push_back(cm.oldName);
    for (const auto& [owner, fields] : mappings.getFields()) names.push_back(owner);
    for (const auto& [owner, methods] : mappings.getMethods()) names.push_back(owner);
    std::sort(names.begin(), names.end(), [](Utf8StringRef lhs, Utf8StringRef rhs) {
        return lhs.compare(rhs) < 0;
    });
    names.erase(std::unique(names.begin(),
                            names.end(),
                            [](Utf8StringRef lhs, Utf8StringRef rhs) { return lhs.equals(rhs); }),
                names.end());
    return names;
}

ca::u8 asciiLower(ca::u8 ch)
{
    return ch >= 'A' && ch <= 'Z' ? static_cast<ca::u8>(ch - 'A' + 'a') : ch;
}

bool containsAsciiInsensitive(Utf8StringRef text, Utf8StringRef needle)
{
    if (needle.is_empty()) return true;
    if (needle.byte_length() > text.byte_length()) return false;
    for (ca::usize i = 0; i <= text.byte_length() - needle.byte_length(); ++i) {
        bool matched = true;
        for (ca::usize j = 0; j < needle.byte_length(); ++j) {
            if (asciiLower(text.byte_at(i + j)) != asciiLower(needle.byte_at(j))) {
                matched = false;
                break;
            }
        }
        if (matched) return true;
    }
    return false;
}

Utf8StringRef attrRef(Utf8StringRef text, Utf8StringRef name)
{
    ca::usize pos = 0;
    while (pos < text.byte_length()) {
        auto found = findRef(text, name, pos);
        if (found == Utf8StringRef::npos) return {};
        auto afterName = found + name.byte_length();
        if (afterName < text.byte_length() && text.byte_at(afterName) == '=') {
            auto quotePos = afterName + 1;
            if (quotePos < text.byte_length() &&
                (text.byte_at(quotePos) == '"' || text.byte_at(quotePos) == '\'')) {
                auto quote = text.byte_at(quotePos);
                auto end   = findByte(text, quote, quotePos + 1);
                return end == Utf8StringRef::npos ? Utf8StringRef{} : text.slice(quotePos + 1, end);
            }
        }
        pos = afterName;
    }
    return {};
}

Utf8StringRef firstAttrRef(Utf8StringRef text, std::initializer_list<Utf8StringRef> names)
{
    for (auto name : names) {
        auto value = attrRef(text, name);
        if (!value.is_empty()) return value;
    }
    return {};
}

Utf8String xmlUnescapeRef(Utf8StringRef text)
{
    Utf8StringBuilder out;
    for (ca::usize i = 0; i < text.byte_length(); ++i) {
        if (text.byte_at(i) != '&') {
            out.append_code_point(text.byte_at(i));
            continue;
        }
        auto semi = findByte(text, ';', i + 1);
        if (semi == Utf8StringRef::npos) {
            out.append_code_point('&');
            continue;
        }
        auto entity = text.slice(i, semi + 1);
        if (entity.equals("&amp;"))
            out.append("&");
        else if (entity.equals("&quot;"))
            out.append("\"");
        else if (entity.equals("&apos;"))
            out.append("'");
        else if (entity.equals("&lt;"))
            out.append("<");
        else if (entity.equals("&gt;"))
            out.append(">");
        else
            out.append(entity);
        i = semi;
    }
    return out.build();
}

void appendXmlEscaped(Utf8StringBuilder& out, Utf8StringRef text)
{
    for (ca::usize i = 0; i < text.byte_length(); ++i) {
        switch (text.byte_at(i)) {
        case '&': out.append("&amp;"); break;
        case '"': out.append("&quot;"); break;
        case '\'': out.append("&apos;"); break;
        case '<': out.append("&lt;"); break;
        case '>': out.append("&gt;"); break;
        default: out.append_code_point(text.byte_at(i)); break;
        }
    }
}

class TinyV1Mapping final : public MappingFileFormat
{
public:
    Utf8StringRef getFormatName() const override { return Utf8StringRef::from_cstr("Tiny-V1"); }

    ca::Result<IntermediateMapping, MappingError> parse(Utf8StringRef mappingText) override
    {
        IntermediateMapping mappings;
        for (auto rawLine : linesOfRef(mappingText)) {
            auto line = trimRef(rawLine);
            if (line.is_empty() || line.starts_with(ref("#")) || line.starts_with(ref("v1\t")))
                continue;
            auto parts = splitRef(line, '\t');
            if (parts.empty()) continue;

            if (parts[0].equals("CLASS") && parts.size() >= 3) {
                mappings.addClass(parts[1], parts[2]);
            }
            else if (parts[0].equals("FIELD") && parts.size() >= 5) {
                mappings.addField(parts[1], parts[2], parts[3], parts[4]);
            }
            else if (parts[0].equals("METHOD") && parts.size() >= 5) {
                mappings.addMethod(parts[1], parts[2], parts[3], parts[4]);
            }
        }
        return ca::Ok(std::move(mappings));
    }

    ca::Result<Utf8String, MappingError> format(const IntermediateMapping& mappings) override
    {
        ca::str::Utf8StringBuilder sb;
        sb.append("v1\tintermediary\tnamed\n");
        for (const auto& [key, cm] : mappings.getClasses()) {
            sb.append("CLASS\t");
            sb.append(cm.oldName);
            sb.append("\t");
            sb.append(cm.newName);
            sb.append("\n");
        }
        for (const auto& [owner, fields] : mappings.getFields()) {
            for (const auto& fm : fields) {
                sb.append("FIELD\t");
                sb.append(fm.ownerName);
                sb.append("\t");
                sb.append(fm.desc);
                sb.append("\t");
                sb.append(fm.oldName);
                sb.append("\t");
                sb.append(fm.newName);
                sb.append("\n");
            }
        }
        for (const auto& [owner, methods] : mappings.getMethods()) {
            for (const auto& mm : methods) {
                sb.append("METHOD\t");
                sb.append(mm.ownerName);
                sb.append("\t");
                sb.append(mm.desc);
                sb.append("\t");
                sb.append(mm.oldName);
                sb.append("\t");
                sb.append(mm.newName);
                sb.append("\n");
            }
        }
        return ca::Ok(sb.build());
    }
};

class SimpleMapping final : public MappingFileFormat
{
public:
    Utf8StringRef getFormatName() const override { return Utf8StringRef::from_cstr("Simple"); }

    ca::Result<IntermediateMapping, MappingError> parse(Utf8StringRef mappingText) override
    {
        IntermediateMapping mappings;
        for (auto rawLine : linesOfRef(mappingText)) {
            auto line = trimRef(rawLine);
            if (line.is_empty() || line.starts_with(ref("#"))) continue;
            auto parts = splitWhitespaceRef(line);
            if (parts.size() < 2) continue;

            auto oldBase = unescapeSimpleRef(parts[0]);
            if (parts.size() >= 3) {
                auto desc = unescapeSimpleRef(parts[1]);
                auto dot  = rfindByte(oldBase.ref(), '.');
                if (dot == Utf8StringRef::npos) continue;
                auto newName = unescapeSimpleRef(parts[2]);
                mappings.addField(oldBase.slice(0, dot),
                                  desc.ref(),
                                  oldBase.slice(dot + 1, oldBase.byte_length()),
                                  newName.ref());
                continue;
            }

            auto newName = unescapeSimpleRef(parts[1]);
            auto dot     = rfindByte(oldBase.ref(), '.');
            if (dot == Utf8StringRef::npos) {
                mappings.addClass(oldBase.ref(), newName.ref());
                continue;
            }

            auto owner      = oldBase.slice(0, dot);
            auto identifier = oldBase.slice(dot + 1, oldBase.byte_length());
            auto descStart  = findByte(identifier, '(');
            if (descStart != Utf8StringRef::npos) {
                mappings.addMethod(owner,
                                   identifier.slice(descStart, identifier.byte_length()),
                                   identifier.slice(0, descStart),
                                   newName.ref());
            }
            else {
                mappings.addField(owner, Utf8StringRef{}, identifier, newName.ref());
            }
        }
        return ca::Ok(std::move(mappings));
    }

    ca::Result<Utf8String, MappingError> format(const IntermediateMapping& mappings) override
    {
        Utf8StringBuilder out;
        for (const auto& [key, cm] : mappings.getClasses()) {
            appendSimpleEscaped(out, cm.oldName);
            out.append(" ");
            appendSimpleEscaped(out, cm.newName);
            out.append("\n");
        }
        for (const auto& [owner, fields] : mappings.getFields()) {
            for (const auto& fm : fields) {
                appendSimpleEscaped(out, fm.ownerName);
                out.append(".");
                appendSimpleEscaped(out, fm.oldName);
                if (!fm.desc.is_empty()) {
                    out.append(" ");
                    appendSimpleEscaped(out, fm.desc);
                }
                out.append(" ");
                appendSimpleEscaped(out, fm.newName);
                out.append("\n");
            }
        }
        for (const auto& [owner, methods] : mappings.getMethods()) {
            for (const auto& mm : methods) {
                appendSimpleEscaped(out, mm.ownerName);
                out.append(".");
                appendSimpleEscaped(out, mm.oldName);
                out.append(mm.desc);
                out.append(" ");
                appendSimpleEscaped(out, mm.newName);
                out.append("\n");
            }
        }
        return ca::Ok(out.build());
    }
};

class ProGuardMapping final : public MappingFileFormat
{
public:
    Utf8StringRef getFormatName() const override { return Utf8StringRef::from_cstr("Proguard"); }

    ca::Result<IntermediateMapping, MappingError> parse(Utf8StringRef mappingText) override
    {
        IntermediateMapping                              mappings;
        ca::str::Utf8StringArena                         typeArena;
        std::unordered_map<Utf8StringRef, Utf8StringRef> originalToObfuscated;

        for (auto rawLine : linesOfRef(mappingText)) {
            auto line = trimRef(rawLine);
            if (line.is_empty() || line.starts_with(ref("#")) || rawLine.is_empty() ||
                rawLine.byte_at(0) == ' ')
                continue;
            auto arrow = findRef(line, ref(" -> "));
            if (arrow == Utf8StringRef::npos || !endsWithRef(line, ref(":"))) continue;
            auto original    = normalizeClassNameRef(line.slice(0, arrow));
            auto obfuscated  = normalizeClassNameRef(line.slice(arrow + 4, line.byte_length() - 1));
            auto originalKey = typeArena.intern(original.ref());
            auto obfuscatedKey                = typeArena.intern(obfuscated.ref());
            originalToObfuscated[originalKey] = obfuscatedKey;
            mappings.addClass(obfuscated.ref(), original.ref());
        }

        Utf8StringRef currentObfuscated;
        for (auto rawLine : linesOfRef(mappingText)) {
            auto line = trimRef(rawLine);
            if (line.is_empty() || line.starts_with(ref("#"))) continue;
            auto arrow = findRef(line, ref(" -> "));
            if (arrow == Utf8StringRef::npos) continue;

            if (!rawLine.is_empty() && rawLine.byte_at(0) != ' ' && endsWithRef(line, ref(":"))) {
                auto obfuscated =
                    normalizeClassNameRef(line.slice(arrow + 4, line.byte_length() - 1));
                currentObfuscated = typeArena.intern(obfuscated.ref());
                continue;
            }
            if (currentObfuscated.is_empty()) continue;

            auto left      = trimRef(line.slice(0, arrow));
            auto rightName = trimRef(line.slice(arrow + 4, line.byte_length()));

            auto firstColon = findByte(left, ':');
            if (firstColon != Utf8StringRef::npos) {
                auto secondColon = findByte(left, ':', firstColon + 1);
                if (secondColon != Utf8StringRef::npos) {
                    left = trimRef(left.slice(secondColon + 1, left.byte_length()));
                }
            }

            auto openParen = findByte(left, '(');
            if (openParen != Utf8StringRef::npos) {
                auto space = findByte(left, ' ');
                if (space == Utf8StringRef::npos) continue;
                auto returnDesc =
                    descriptorForJavaTypeRef(left.slice(0, space), originalToObfuscated);
                auto nameStart = space + 1;
                auto argsStart = findByte(left, '(', nameStart);
                auto argsEnd   = findByte(left, ')', argsStart + 1);
                if (argsStart == Utf8StringRef::npos || argsEnd == Utf8StringRef::npos) continue;

                Utf8StringBuilder desc;
                desc.append("(");
                auto args = left.slice(argsStart + 1, argsEnd);
                if (!trimRef(args).is_empty()) {
                    for (auto arg : splitRef(args, ',')) {
                        auto argDesc = descriptorForJavaTypeRef(trimRef(arg), originalToObfuscated);
                        desc.append(argDesc.ref());
                    }
                }
                desc.append(")");
                desc.append(returnDesc.ref());
                auto descText = desc.build();
                mappings.addMethod(
                    currentObfuscated, descText.ref(), rightName, left.slice(nameStart, argsStart));
            }
            else {
                auto space = findByte(left, ' ');
                if (space == Utf8StringRef::npos) continue;
                auto desc = descriptorForJavaTypeRef(left.slice(0, space), originalToObfuscated);
                mappings.addField(currentObfuscated,
                                  desc.ref(),
                                  rightName,
                                  left.slice(space + 1, left.byte_length()));
            }
        }
        return ca::Ok(std::move(mappings));
    }

    ca::Result<Utf8String, MappingError> format(const IntermediateMapping& mappings) override
    {
        Utf8StringBuilder out;
        for (const auto& owner : sortedClassNames(mappings)) {
            auto* cm      = mappings.getClassMapping(owner);
            auto  newName = cm ? cm->newName : owner;
            appendDotClassName(out, newName);
            out.append(" -> ");
            appendDotClassName(out, owner);
            out.append(":\n");
            if (auto* fields = mappings.getClassFieldMappings(owner)) {
                for (const auto& fm : *fields) {
                    ca::usize pos = 0;
                    out.append("    ");
                    if (!appendJavaTypeFromDescriptor(out, mappings, fm.desc, pos)) continue;
                    out.append(" ");
                    out.append(fm.newName);
                    out.append(" -> ");
                    out.append(fm.oldName);
                    out.append("\n");
                }
            }
            if (auto* methods = mappings.getClassMethodMappings(owner)) {
                for (const auto& mm : *methods) {
                    if (mm.desc.byte_length() < 3 || mm.desc.byte_at(0) != '(') continue;
                    Utf8StringBuilder args;
                    ca::usize         pos      = 1;
                    bool              firstArg = true;
                    bool              valid    = true;
                    while (pos < mm.desc.byte_length() && mm.desc.byte_at(pos) != ')') {
                        if (!firstArg) args.append(",");
                        if (!appendJavaTypeFromDescriptor(args, mappings, mm.desc, pos)) {
                            valid = false;
                            break;
                        }
                        firstArg = false;
                    }
                    if (!valid || pos >= mm.desc.byte_length() || mm.desc.byte_at(pos) != ')')
                        continue;
                    pos++;

                    Utf8StringBuilder returnType;
                    if (!appendJavaTypeFromDescriptor(returnType, mappings, mm.desc, pos)) continue;
                    auto argText    = args.build();
                    auto returnText = returnType.build();
                    out.append("    ");
                    out.append(returnText.ref());
                    out.append(" ");
                    out.append(mm.newName);
                    out.append("(");
                    out.append(argText.ref());
                    out.append(") -> ");
                    out.append(mm.oldName);
                    out.append("\n");
                }
            }
        }
        return ca::Ok(out.build());
    }
};

class SrgMapping final : public MappingFileFormat
{
public:
    Utf8StringRef getFormatName() const override { return Utf8StringRef::from_cstr("SRG"); }

    ca::Result<IntermediateMapping, MappingError> parse(Utf8StringRef mappingText) override
    {
        IntermediateMapping mappings;
        for (auto rawLine : linesOfRef(mappingText)) {
            auto line = trimRef(rawLine);
            if (line.is_empty() || line.starts_with(ref("#"))) continue;
            auto args = splitWhitespaceRef(line);
            if (args.empty()) continue;

            if (args[0].equals("PK:") && args.size() >= 3) {
                mappings.addPackage(args[1], args[2]);
            }
            else if (args[0].equals("CL:") && args.size() >= 3) {
                mappings.addClass(args[1], args[2]);
            }
            else if (args[0].equals("FD:") && args.size() >= 3) {
                auto splitPos = rfindByte(args[1], '/');
                if (splitPos == Utf8StringRef::npos) continue;
                auto owner      = args[1].slice(0, splitPos);
                auto name       = args[1].slice(splitPos + 1, args[1].byte_length());
                auto desc       = args.size() >= 5 ? args[2] : Utf8StringRef{};
                auto renamedKey = args.size() >= 5 ? args[3] : args[2];
                auto renamedPos = rfindByte(renamedKey, '/');
                auto renamed    = renamedPos == Utf8StringRef::npos
                                      ? renamedKey
                                      : renamedKey.slice(renamedPos + 1, renamedKey.byte_length());
                mappings.addField(owner, desc, name, renamed);
            }
            else if (args[0].equals("MD:") && args.size() >= 4) {
                auto splitPos = rfindByte(args[1], '/');
                if (splitPos == Utf8StringRef::npos) continue;
                auto owner      = args[1].slice(0, splitPos);
                auto name       = args[1].slice(splitPos + 1, args[1].byte_length());
                auto renamedKey = args[3];
                auto renamedPos = rfindByte(renamedKey, '/');
                auto renamed    = renamedPos == Utf8StringRef::npos
                                      ? renamedKey
                                      : renamedKey.slice(renamedPos + 1, renamedKey.byte_length());
                mappings.addMethod(owner, args[2], name, renamed);
            }
        }
        return ca::Ok(std::move(mappings));
    }

    ca::Result<Utf8String, MappingError> format(const IntermediateMapping& mappings) override
    {
        Utf8StringBuilder out;
        // PK 行按旧名字典序输出（与下方 CL 的 sortedClassNames 同口径）：
        // 底座是 unordered_map，迭代序随实现漂移，逐行对账需要确定序
        // 。
        std::vector<const PackageMapping*> packages;
        packages.reserve(mappings.getPackages().size());
        for (const auto& [key, pm] : mappings.getPackages()) {
            packages.push_back(&pm);
        }
        std::sort(packages.begin(),
                  packages.end(),
                  [](const PackageMapping* lhs, const PackageMapping* rhs) {
                      return lhs->oldName.compare(rhs->oldName) < 0;
                  });
        for (const auto* pm : packages) {
            out.append("PK: ");
            out.append(pm->oldName);
            out.append(" ");
            out.append(pm->newName);
            out.append("\n");
        }
        // CL 行按旧名字典序输出（与 FD/MD 的 sortedClassNames 同口径）：
        // 底座是 unordered_map，迭代序随实现漂移，跨实现（如 obf 旧核心与
        // renamer 引擎）逐行对账需要确定序。
        for (const auto& owner : sortedClassNames(mappings)) {
            const auto* cm = mappings.getClassMapping(owner);
            if (cm == nullptr) continue;
            out.append("CL: ");
            out.append(cm->oldName);
            out.append(" ");
            out.append(cm->newName);
            out.append("\n");
        }
        for (const auto& owner : sortedClassNames(mappings)) {
            auto* cm      = mappings.getClassMapping(owner);
            auto  newName = cm ? cm->newName : owner;
            auto* fields  = mappings.getClassFieldMappings(owner);
            if (!fields) continue;
            // 组内按 (oldName, desc) 排序：插入序来自收集序，逐行对账需要确定序。
            auto sortedFields = *fields;
            std::sort(sortedFields.begin(),
                      sortedFields.end(),
                      [](const FieldMapping& lhs, const FieldMapping& rhs) {
                          if (!lhs.oldName.equals(rhs.oldName))
                              return lhs.oldName.compare(rhs.oldName) < 0;
                          return lhs.desc.compare(rhs.desc) < 0;
                      });
            for (const auto& fm : sortedFields) {
                out.append("FD: ");
                out.append(fm.ownerName);
                out.append("/");
                out.append(fm.oldName);
                if (fm.hasType()) {
                    // 带类型字段输出 desc token（旧 desc + 映射后 desc）：SRG 解析
                    // 两侧均支持；丢 desc 会让 parse 产物丢类型、按 (owner,name,desc)
                    // 的查找（getMappedFieldName）失配。
                    out.append(" ");
                    out.append(fm.desc);
                }
                out.append(" ");
                out.append(newName);
                out.append("/");
                out.append(fm.newName);
                if (fm.hasType()) {
                    out.append(" ");
                    appendMappedDescriptor(out, mappings, fm.desc);
                }
                out.append("\n");
            }
        }
        for (const auto& owner : sortedClassNames(mappings)) {
            auto* cm      = mappings.getClassMapping(owner);
            auto  newName = cm ? cm->newName : owner;
            auto* methods = mappings.getClassMethodMappings(owner);
            if (!methods) continue;
            auto sortedMethods = *methods;
            std::sort(sortedMethods.begin(),
                      sortedMethods.end(),
                      [](const MethodMapping& lhs, const MethodMapping& rhs) {
                          if (!lhs.oldName.equals(rhs.oldName))
                              return lhs.oldName.compare(rhs.oldName) < 0;
                          return lhs.desc.compare(rhs.desc) < 0;
                      });
            for (const auto& mm : sortedMethods) {
                out.append("MD: ");
                out.append(mm.ownerName);
                out.append("/");
                out.append(mm.oldName);
                out.append(" ");
                out.append(mm.desc);
                out.append(" ");
                out.append(newName);
                out.append("/");
                out.append(mm.newName);
                out.append(" ");
                appendMappedDescriptor(out, mappings, mm.desc);
                out.append("\n");
            }
        }
        return ca::Ok(out.build());
    }
};

class EnigmaMapping final : public MappingFileFormat
{
public:
    Utf8StringRef getFormatName() const override { return Utf8StringRef::from_cstr("Enigma"); }

    ca::Result<IntermediateMapping, MappingError> parse(Utf8StringRef mappingText) override
    {
        IntermediateMapping                                              mappings;
        std::vector<std::pair<ca::str::Utf8String, ca::str::Utf8String>> classStack;

        for (auto rawLine : linesOfRef(mappingText)) {
            auto trimmed = trimRef(rawLine);
            if (trimmed.is_empty() || trimmed.starts_with(ref("COMMENT"))) continue;
            ca::usize indent = 0;
            while (indent < rawLine.byte_length() && rawLine.byte_at(indent) == '\t') indent++;
            while (classStack.size() > indent) classStack.pop_back();

            auto args = splitWhitespaceRef(trimmed);
            if (args.empty()) continue;

            if (args[0].equals("CLASS") && args.size() >= 2) {
                auto oldName = normalizeClassNameRef(args[1]);
                auto newName = args.size() >= 3 && !args[2].equals("-")
                                   ? normalizeClassNameRef(args[2])
                                   : oldName.clone();
                if (!classStack.empty()) {
                    ca::str::Utf8StringBuilder oldBuilder;
                    oldBuilder.append(classStack.back().first.ref())
                        .append("$")
                        .append(oldName.ref());
                    oldName = oldBuilder.build();

                    ca::str::Utf8StringBuilder newBuilder;
                    newBuilder.append(classStack.back().second.ref())
                        .append("$")
                        .append(newName.ref());
                    newName = newBuilder.build();
                }
                mappings.addClass(oldName.ref(), newName.ref());
                classStack.push_back({std::move(oldName), std::move(newName)});
            }
            else if ((args[0].equals("FIELD") || args[0].equals("METHOD")) && args.size() >= 4 &&
                     !classStack.empty()) {
                if (args[2].equals("-")) continue;
                if (args[0].equals("FIELD")) {
                    mappings.addField(classStack.back().first.ref(), args[3], args[1], args[2]);
                }
                else {
                    mappings.addMethod(classStack.back().first.ref(), args[3], args[1], args[2]);
                }
            }
        }
        return ca::Ok(std::move(mappings));
    }

    ca::Result<Utf8String, MappingError> format(const IntermediateMapping& mappings) override
    {
        Utf8StringBuilder out;
        for (const auto& [key, cm] : mappings.getClasses()) {
            out.append("CLASS ");
            out.append(cm.oldName);
            out.append(" ");
            out.append(cm.newName);
            out.append("\n");
            if (auto* fields = mappings.getClassFieldMappings(cm.oldName)) {
                for (const auto& fm : *fields) {
                    out.append("\tFIELD ");
                    out.append(fm.oldName);
                    out.append(" ");
                    out.append(fm.newName);
                    out.append(" ");
                    out.append(fm.desc);
                    out.append("\n");
                }
            }
            if (auto* methods = mappings.getClassMethodMappings(cm.oldName)) {
                for (const auto& mm : *methods) {
                    out.append("\tMETHOD ");
                    out.append(mm.oldName);
                    out.append(" ");
                    out.append(mm.newName);
                    out.append(" ");
                    out.append(mm.desc);
                    out.append("\n");
                }
            }
        }
        return ca::Ok(out.build());
    }
};

class JadxMapping final : public MappingFileFormat
{
public:
    Utf8StringRef getFormatName() const override
    {
        return Utf8StringRef::from_cstr("Jadx (Legacy)");
    }

    ca::Result<IntermediateMapping, MappingError> parse(Utf8StringRef mappingText) override
    {
        IntermediateMapping mappings;
        for (auto rawLine : linesOfRef(mappingText)) {
            auto line = trimRef(rawLine);
            if (line.is_empty() || line.starts_with(ref("#"))) continue;
            auto args = splitWhitespaceRef(line);
            if (args.size() < 3) continue;
            auto renamed = args.size() >= 4 && args[2].equals("=") ? args[3] : args[2];

            if (args[0].equals("c")) {
                auto              oldName = normalizeClassNameRef(args[1]);
                auto              slash   = rfindByte(oldName.ref(), '/');
                Utf8StringBuilder newName;
                if (slash != Utf8StringRef::npos) {
                    newName.append(oldName.slice(0, slash + 1));
                }
                newName.append(renamed);
                auto mappedName = newName.build();
                mappings.addClass(oldName.ref(), mappedName.ref());
            }
            else if (args[0].equals("f")) {
                auto spec  = args[1];
                auto colon = findByte(spec, ':');
                auto dot =
                    rfindByte(spec, '.', colon == Utf8StringRef::npos ? spec.byte_length() : colon);
                if (colon == Utf8StringRef::npos || dot == Utf8StringRef::npos) continue;
                auto owner = normalizeClassNameRef(spec.slice(0, dot));
                mappings.addField(owner.ref(),
                                  spec.slice(colon + 1, spec.byte_length()),
                                  spec.slice(dot + 1, colon),
                                  renamed);
            }
            else if (args[0].equals("m")) {
                auto spec  = args[1];
                auto paren = findByte(spec, '(');
                auto dot =
                    rfindByte(spec, '.', paren == Utf8StringRef::npos ? spec.byte_length() : paren);
                if (paren == Utf8StringRef::npos || dot == Utf8StringRef::npos) continue;
                auto owner = normalizeClassNameRef(spec.slice(0, dot));
                mappings.addMethod(owner.ref(),
                                   spec.slice(paren, spec.byte_length()),
                                   spec.slice(dot + 1, paren),
                                   renamed);
            }
        }
        return ca::Ok(std::move(mappings));
    }

    ca::Result<Utf8String, MappingError> format(const IntermediateMapping& mappings) override
    {
        Utf8StringBuilder out;
        for (const auto& [key, cm] : mappings.getClasses()) {
            auto slash = rfindByte(cm.newName, '/');
            out.append("c ");
            appendDotClassName(out, cm.oldName);
            out.append(" = ");
            out.append(slash == Utf8StringRef::npos
                           ? cm.newName
                           : cm.newName.slice(slash + 1, cm.newName.byte_length()));
            out.append("\n");
        }
        for (const auto& [owner, fields] : mappings.getFields()) {
            for (const auto& fm : fields) {
                out.append("f ");
                appendDotClassName(out, fm.ownerName);
                out.append(".");
                out.append(fm.oldName);
                out.append(":");
                out.append(fm.desc);
                out.append(" = ");
                out.append(fm.newName);
                out.append("\n");
            }
        }
        for (const auto& [owner, methods] : mappings.getMethods()) {
            for (const auto& mm : methods) {
                out.append("m ");
                appendDotClassName(out, mm.ownerName);
                out.append(".");
                out.append(mm.oldName);
                out.append(mm.desc);
                out.append(" = ");
                out.append(mm.newName);
                out.append("\n");
            }
        }
        return ca::Ok(out.build());
    }
};

class XmlObfuscatorMapping final : public MappingFileFormat
{
public:
    explicit XmlObfuscatorMapping(Utf8StringRef name)
        : name_(name)
    {}

    Utf8StringRef getFormatName() const override { return name_; }

    ca::Result<IntermediateMapping, MappingError> parse(Utf8StringRef mappingText) override
    {
        IntermediateMapping                              mappings;
        ca::str::Utf8StringArena                         ownerArena;
        std::unordered_map<Utf8StringRef, Utf8StringRef> oldToNewOwner;

        for (auto rawLine : linesOfRef(mappingText)) {
            auto line = trimRef(rawLine);
            if (line.is_empty()) continue;

            if (containsAsciiInsensitive(line, ref("<class"))) {
                auto oldAttr =
                    firstAttrRef(line, {ref("old"), ref("name"), ref("source"), ref("original")});
                auto newAttr =
                    firstAttrRef(line, {ref("new"), ref("map"), ref("target"), ref("obfuscated")});
                if (oldAttr.is_empty() || newAttr.is_empty()) continue;
                auto oldText          = xmlUnescapeRef(oldAttr);
                auto newText          = xmlUnescapeRef(newAttr);
                auto oldName          = normalizeClassNameRef(oldText.ref());
                auto newName          = normalizeClassNameRef(newText.ref());
                auto oldKey           = ownerArena.intern(oldName.ref());
                auto newKey           = ownerArena.intern(newName.ref());
                oldToNewOwner[oldKey] = newKey;
                mappings.addClass(newName.ref(), oldName.ref());
            }
            else if (containsAsciiInsensitive(line, ref("<field")) ||
                     containsAsciiInsensitive(line, ref("<method"))) {
                bool isMethod  = containsAsciiInsensitive(line, ref("<method"));
                auto ownerAttr = firstAttrRef(line, {ref("class"), ref("owner"), ref("parent")});
                auto oldAttr =
                    firstAttrRef(line, {ref("old"), ref("name"), ref("source"), ref("original")});
                auto newAttr =
                    firstAttrRef(line, {ref("new"), ref("map"), ref("target"), ref("obfuscated")});
                auto descAttr =
                    firstAttrRef(line, {ref("desc"), ref("descriptor"), ref("signature")});
                if (ownerAttr.is_empty() || oldAttr.is_empty() || newAttr.is_empty()) continue;
                auto ownerText       = xmlUnescapeRef(ownerAttr);
                auto owner           = normalizeClassNameRef(ownerText.ref());
                auto ownerIt         = oldToNewOwner.find(owner.ref());
                auto obfuscatedOwner = ownerIt == oldToNewOwner.end()
                                           ? ownerArena.intern(owner.ref())
                                           : ownerIt->second;
                auto oldName         = xmlUnescapeRef(oldAttr);
                auto newName         = xmlUnescapeRef(newAttr);
                auto desc            = xmlUnescapeRef(descAttr);
                if (isMethod) {
                    mappings.addMethod(obfuscatedOwner, desc.ref(), newName.ref(), oldName.ref());
                }
                else {
                    mappings.addField(obfuscatedOwner, desc.ref(), newName.ref(), oldName.ref());
                }
            }
        }
        return ca::Ok(std::move(mappings));
    }

    ca::Result<Utf8String, MappingError> format(const IntermediateMapping& mappings) override
    {
        Utf8StringBuilder out;
        out.append("<mappings format=\"");
        appendXmlEscaped(out, name_);
        out.append("\">\n");
        for (const auto& [key, cm] : mappings.getClasses()) {
            out.append("  <class old=\"");
            appendXmlEscaped(out, cm.newName);
            out.append("\" new=\"");
            appendXmlEscaped(out, cm.oldName);
            out.append("\" />\n");
        }
        for (const auto& [owner, fields] : mappings.getFields()) {
            auto* cm        = mappings.getClassMapping(owner);
            auto  className = cm ? cm->newName : owner;
            for (const auto& fm : fields) {
                out.append("  <field class=\"");
                appendXmlEscaped(out, className);
                out.append("\" old=\"");
                appendXmlEscaped(out, fm.newName);
                out.append("\" new=\"");
                appendXmlEscaped(out, fm.oldName);
                out.append("\" desc=\"");
                appendXmlEscaped(out, fm.desc);
                out.append("\" />\n");
            }
        }
        for (const auto& [owner, methods] : mappings.getMethods()) {
            auto* cm        = mappings.getClassMapping(owner);
            auto  className = cm ? cm->newName : owner;
            for (const auto& mm : methods) {
                out.append("  <method class=\"");
                appendXmlEscaped(out, className);
                out.append("\" old=\"");
                appendXmlEscaped(out, mm.newName);
                out.append("\" new=\"");
                appendXmlEscaped(out, mm.oldName);
                out.append("\" desc=\"");
                appendXmlEscaped(out, mm.desc);
                out.append("\" />\n");
            }
        }
        out.append("</mappings>\n");
        return ca::Ok(out.build());
    }

private:
    Utf8StringRef name_;
};

}   // namespace

MappingFormatRegistry MappingFormatRegistry::withStandardFormats()
{
    MappingFormatRegistry registry;
    registerStandardMappingFormats(registry);
    return registry;
}

bool MappingFormatRegistry::registerFormat(Utf8StringRef name, MappingFormatFactory factory)
{
    if (name.is_empty() || !factory) return false;
    factories_[arena_.intern(name)] = std::move(factory);
    return true;
}

bool MappingFormatRegistry::hasFormat(Utf8StringRef name) const
{
    return factories_.find(name) != factories_.end();
}

std::unique_ptr<MappingFileFormat> MappingFormatRegistry::createFormat(Utf8StringRef name) const
{
    auto it = factories_.find(name);
    return it == factories_.end() ? nullptr : it->second();
}

std::vector<Utf8StringRef> MappingFormatRegistry::formatNames() const
{
    std::vector<Utf8StringRef> names;
    names.reserve(factories_.size());
    for (const auto& [name, factory] : factories_) names.push_back(name);
    std::sort(names.begin(), names.end(), [](Utf8StringRef lhs, Utf8StringRef rhs) {
        return lhs.compare(rhs) < 0;
    });
    return names;
}

ca::Result<IntermediateMapping, MappingError> MappingFormatRegistry::parse(
    Utf8StringRef name, ca::str::Utf8StringRef mappingText) const
{
    auto format = createFormat(name);
    if (!format) return ca::Err(MappingError::UnknownFormat);
    auto parsed = format->parse(mappingText);
    if (parsed.is_err()) return ca::Err(std::move(parsed).unwrap_err());
    auto mappings = std::move(parsed).unwrap();
    restoreEmbeddedSources(mappings, mappingText);
    mappings.rememberSource(name, mappingText);
    return ca::Ok(std::move(mappings));
}

ca::Result<ca::str::Utf8String, MappingError> MappingFormatRegistry::format(
    Utf8StringRef name, const IntermediateMapping& mappings) const
{
    if (const auto* source = mappings.unmodifiedSource(name)) {
        return ca::Ok(
            ca::str::Utf8String(reinterpret_cast<const ca::u8*>(source->data()), source->size()));
    }
    auto writer = createFormat(name);
    if (!writer) return ca::Err(MappingError::UnknownFormat);
    auto written = writer->format(mappings);
    if (written.is_err()) return ca::Err(std::move(written).unwrap_err());
    auto output = std::move(written).unwrap();
    if (mappings.sourceSnapshots().empty()) return ca::Ok(std::move(output));
    Utf8StringBuilder withSources;
    withSources.append(output.ref());
    appendEmbeddedSources(withSources, name, mappings);
    return ca::Ok(withSources.build());
}

void registerStandardMappingFormats(MappingFormatRegistry& registry)
{
    registry.registerFormat(Utf8StringRef::from_cstr("Tiny-V1"),
                            [] { return std::make_unique<TinyV1Mapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("Tiny-V2"),
                            [] { return std::make_unique<TinyV2Mapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("Tiny-v2"),
                            [] { return std::make_unique<TinyV2Mapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("Simple"),
                            [] { return std::make_unique<SimpleMapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("Proguard"),
                            [] { return std::make_unique<ProGuardMapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("ProGuard"),
                            [] { return std::make_unique<ProGuardMapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("R8"),
                            [] { return std::make_unique<ProGuardMapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("SRG"),
                            [] { return std::make_unique<SrgMapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("Enigma"),
                            [] { return std::make_unique<EnigmaMapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("Jadx (Legacy)"),
                            [] { return std::make_unique<JadxMapping>(); });
    registry.registerFormat(Utf8StringRef::from_cstr("YGuard"),
                            [] { return std::make_unique<XmlObfuscatorMapping>(ref("YGuard")); });
    registry.registerFormat(Utf8StringRef::from_cstr("Allatori"),
                            [] { return std::make_unique<XmlObfuscatorMapping>(ref("Allatori")); });
    // shadow-alias sidecar（#1072）：仅服务显示层，与真实 remap 格式零混写。
    registry.registerFormat(Utf8StringRef::from_cstr("Shadow-Alias"),
                            [] { return std::make_unique<ShadowAliasFormat>(); });
}

}   // namespace mjbase::mapping
