#include "shadow_token.hpp"

#include <algorithm>
#include <vector>

namespace mjbase::renamer {
namespace {

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isLowerHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool isShadowSegmentChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/// 恰好 8 位小写 hex；成功消费 8 字节到 out。
bool take8Hex(std::string_view input, std::size_t& cursor, std::string& out)
{
    if (cursor + 8 > input.size()) return false;
    for (std::size_t i = 0; i < 8; ++i) {
        if (!isLowerHex(input[cursor + i])) return false;
    }
    out = std::string(input.substr(cursor, 8));
    cursor += 8;
    return true;
}

/// 诊断里的长名截断（脏名可达 ~1600 字符，消息保可读；给出总长供定位）。
std::string brief(std::string_view value)
{
    constexpr std::size_t kMax = 96;
    if (value.size() <= kMax) return std::string(value);
    return std::string(value.substr(0, kMax)) + "…(" + std::to_string(value.size()) +
           " bytes total)";
}

std::string quoted(std::string_view value)
{
    return "'" + brief(value) + "'";
}

/// 类条目按 (Class_ 序号, token) 定位：影子名精确 `Class_<index>`（不规范化，
/// 手滑前导零查无此条 → unknown-handle 响亮报错），token 必须逐字节相等。
///
/// 返回条目拷贝（optional 按值）——不得返回指向 findByShadowName 临时 vector
/// 内部的指针（悬垂 UB，mjt 9e72e9f 实修，本移植自始按值交付）。
std::optional<ShadowAliasEntry> findClassByHandle(const ShadowAliasTable& table,
                                                  const std::string&      index,
                                                  const std::string&      token,
                                                  bool&                   shadowNameExisted)
{
    const auto shadow = "Class_" + index;
    shadowNameExisted = false;
    for (const auto& entry : table.findByShadowName(shadow)) {
        if (entry.kind != ShadowKind::Class) continue;
        shadowNameExisted = true;
        if (entry.token8.size() == token.size() &&
            std::equal(token.begin(), token.end(), entry.token8.begin())) {
            return entry;
        }
    }
    return std::nullopt;
}

void appendRepairAction(ShadowSlotError error, std::string& text)
{
    switch (error) {
    case ShadowSlotError::UnknownHandle:
        text += "; re-fetch the reference with raw_to_shadow/shadow_to_raw "
                "(stale sidecar or wrong token)";
        break;
    case ShadowSlotError::MalformedHandle:
        text += "; rebuild the reference per ${Class_N.token8}[.member_shadow.token8] syntax";
        break;
    case ShadowSlotError::RawNotFound:
        text += "; do not hand-type obfuscated names, fetch the reference with raw_to_shadow";
        break;
    case ShadowSlotError::AmbiguousMember:
        text += "; supply the raw descriptor to disambiguate the overload "
                "(descriptor slots accept embedded class handles; "
                "three-part member handles are future work)";
        break;
    case ShadowSlotError::MissingSidecar:
        text += "; generate a shadow sidecar first (MCP shadow_generate, or `mjt shadow -i <jar>`)";
        break;
    case ShadowSlotError::None:
        break;
    }
}

ShadowSlotResult failure(ShadowSlotError error, const std::string& detail)
{
    ShadowSlotResult result;
    result.error        = error;
    std::string text    = shadowSlotErrorCode(error);
    text               += ": ";
    text               += detail;
    appendRepairAction(error, text);
    result.message = std::move(text);
    return result;
}

/// raw/EscapedLiteral 共用：恒等放行 + （可选）存在性核验。
ShadowSlotResult resolveRawIdentity(const ShadowAliasTable& table,
                                    const std::string&      name,
                                    bool                    member,
                                    const std::string&      owner,
                                    bool                    verifyRaw)
{
    ShadowSlotResult result;
    result.raw = name;
    if (!verifyRaw || table.empty()) return result;

    if (member) {
        for (const auto& entry : table.findByRawName(name)) {
            if (entry.kind == ShadowKind::Class) continue;
            if (entry.owner != owner) continue;
            result.kind = entry.kind;
            return result;
        }
        return failure(ShadowSlotError::RawNotFound,
                       "no member named " + quoted(name) + " is registered under owner " +
                           quoted(owner) + " in the loaded shadow sidecar");
    }
    if (table.classEntry(name) != nullptr) return result;
    return failure(ShadowSlotError::RawNotFound,
                   "bare name " + quoted(name) +
                       " is not registered in the loaded shadow sidecar");
}

}   // namespace

ShadowRefParts parseShadowRef(std::string_view input)
{
    ShadowRefParts parts;
    if (input.compare(0, 3, "$${") == 0) {
        parts.form    = ShadowRefForm::EscapedLiteral;
        parts.literal = std::string(input.substr(1));
        return parts;
    }
    if (input.compare(0, 2, "${") != 0) {
        parts.form = ShadowRefForm::Raw;
        return parts;
    }

    std::size_t cursor = 2;
    // 前缀字面 "Class_"（大小写敏感；手滑 `${CLASS_…}` 属形状违约）。
    if (input.compare(cursor, 6, "Class_") != 0) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    cursor += 6;
    const auto indexBegin = cursor;
    while (cursor < input.size() && isDigit(input[cursor])) ++cursor;
    if (cursor == indexBegin) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    parts.index = std::string(input.substr(indexBegin, cursor - indexBegin));
    if (cursor >= input.size() || input[cursor] != '.') {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    ++cursor;
    if (!take8Hex(input, cursor, parts.classToken)) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    if (cursor < input.size() && input[cursor] == '}') {
        if (cursor + 1 != input.size()) {
            parts.form = ShadowRefForm::Malformed;
            return parts;
        }
        parts.form = ShadowRefForm::ClassRef;
        return parts;
    }
    // MemberRef 后缀：`.影子名.token8}`。
    if (cursor >= input.size() || input[cursor] != '.') {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    ++cursor;
    const auto shadowBegin = cursor;
    while (cursor < input.size() && isShadowSegmentChar(input[cursor])) ++cursor;
    if (cursor == shadowBegin) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    parts.memberShadow = std::string(input.substr(shadowBegin, cursor - shadowBegin));
    if (cursor >= input.size() || input[cursor] != '.') {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    ++cursor;
    if (!take8Hex(input, cursor, parts.memberToken)) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    if (cursor + 1 != input.size() || input[cursor] != '}') {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    parts.form = ShadowRefForm::MemberRef;
    return parts;
}

const char* shadowSlotErrorCode(ShadowSlotError error)
{
    switch (error) {
    case ShadowSlotError::UnknownHandle: return "unknown-handle";
    case ShadowSlotError::MalformedHandle: return "malformed-handle";
    case ShadowSlotError::RawNotFound: return "raw-not-found";
    case ShadowSlotError::AmbiguousMember: return "ambiguous-member";
    case ShadowSlotError::MissingSidecar: return "missing-sidecar";
    case ShadowSlotError::None: return "";
    }
    return "";
}

std::optional<ShadowAliasEntry> resolveShadowHandleEntry(const ShadowAliasTable& table,
                                                         const std::string&      handle,
                                                         ShadowSlotResult&       result)
{
    result = ShadowSlotResult{};
    const auto parts = parseShadowRef(handle);
    if (parts.form != ShadowRefForm::ClassRef && parts.form != ShadowRefForm::MemberRef) {
        return std::nullopt;   // 非句柄形状：错误位留空，调用方按非句柄输入分派
    }
    if (table.empty()) {
        result = failure(ShadowSlotError::MissingSidecar,
                         "reference " + quoted(handle) +
                             " requires a loaded shadow sidecar, but none is loaded");
        return std::nullopt;
    }
    bool shadowNameExisted = false;
    auto classEntry = findClassByHandle(table, parts.index, parts.classToken, shadowNameExisted);
    if (!classEntry.has_value()) {
        std::string detail = "no class entry matches ${Class_" + parts.index + "." +
                             parts.classToken + "} in the loaded shadow sidecar";
        if (shadowNameExisted) detail += " (class shadow name exists but token mismatches)";
        result = failure(ShadowSlotError::UnknownHandle, detail);
        return std::nullopt;
    }
    const auto ownerView = classEntry->rawName;
    if (parts.form == ShadowRefForm::ClassRef) {
        result.error      = ShadowSlotError::None;
        result.raw        = ownerView;
        result.kind       = ShadowKind::Class;
        result.fromHandle = true;
        return classEntry;
    }

    // 二元 handle：类 + 成员影子名定位；成员 token 作后缀核验（防手滑
    // 错位到同影子名的另一枚），不参与候选选择。
    std::vector<ShadowAliasEntry> candidates;
    for (const auto& entry : table.findByShadowName(parts.memberShadow)) {
        if (entry.kind == ShadowKind::Class) continue;
        if (entry.owner != ownerView) continue;
        candidates.push_back(entry);
    }
    if (candidates.empty()) {
        result = failure(ShadowSlotError::UnknownHandle,
                         "member shadow " + quoted(parts.memberShadow) +
                             " is not registered under owner " + brief(ownerView) + " (" +
                             std::to_string(ownerView.size()) + " bytes)");
        return std::nullopt;
    }
    if (candidates.size() > 1) {
        // 重载歧义：二元 handle 区分不了（表内同影子名多条 desc）。
        result = failure(ShadowSlotError::AmbiguousMember,
                         "reference " + quoted(handle) + " matches " +
                             std::to_string(candidates.size()) +
                             " member entries under owner " + brief(ownerView) +
                             "; the binary handle cannot distinguish overloads");
        return std::nullopt;
    }
    const auto& member = candidates.front();
    const bool  tokenMatches =
        member.token8.size() == parts.memberToken.size() &&
        std::equal(parts.memberToken.begin(), parts.memberToken.end(), member.token8.begin());
    if (!tokenMatches) {
        result = failure(ShadowSlotError::UnknownHandle,
                         "member token " + quoted(parts.memberToken) + " mismatches for " +
                             quoted(parts.memberShadow) + " under owner " + brief(ownerView));
        return std::nullopt;
    }
    result.error      = ShadowSlotError::None;
    result.raw        = member.rawName;
    result.descriptor = member.desc;
    result.kind       = member.kind;
    result.fromHandle = true;
    return member;
}

ShadowSlotResult resolveShadowSlot(const ShadowAliasTable& table,
                                   const std::string&      input,
                                   const ShadowSlotQuery&  query)
{
    if (query.position == ShadowSlotPosition::Descriptor) {
        return resolveShadowDescriptor(table, input);
    }
    if (query.position == ShadowSlotPosition::JasmBody) {
        return resolveShadowJasmBody(table, input);
    }
    // 其余位置：Name（缺省）；无未接线分支。

    const auto parts = parseShadowRef(input);
    switch (parts.form) {
    case ShadowRefForm::Raw:
        return resolveRawIdentity(table, input, query.member, query.owner, query.verifyRaw);

    case ShadowRefForm::EscapedLiteral:
        // `$${…}` = 字面 `${…}`：剥 `$` 后按 raw 语义（恒等/核验同 raw 分支）。
        return resolveRawIdentity(table, parts.literal, query.member, query.owner, query.verifyRaw);

    case ShadowRefForm::Malformed:
        if (table.empty()) {
            // 引用形状但无表可查：缺 sidecar 硬报错（§6 末行），不静默回落。
            return failure(ShadowSlotError::MissingSidecar,
                           "reference-shaped input " + quoted(input) +
                               " requires a loaded shadow sidecar, but none is loaded");
        }
        return failure(ShadowSlotError::MalformedHandle,
                       quoted(input) + " does not match the reference grammar "
                                       "${Class_N.token8}[.member_shadow.token8]");

    case ShadowRefForm::ClassRef:
    case ShadowRefForm::MemberRef: {
        ShadowSlotResult result;
        const auto       entry = resolveShadowHandleEntry(table, input, result);
        if (!entry.has_value() && result.error == ShadowSlotError::None) {
            // 不可达：ClassRef/MemberRef 形状经 resolveShadowHandleEntry 必有
            // 结果或错误（空表 → MissingSidecar）。防御性兜底。
            return failure(ShadowSlotError::UnknownHandle,
                           "reference " + quoted(input) + " did not resolve");
        }
        return result;
    }
    }
    return failure(ShadowSlotError::MalformedHandle, "unreachable reference form");
}


ShadowSlotResult resolveShadowDescriptor(const ShadowAliasTable& table,
                                         const std::string&      input)
{
    ShadowSlotResult result;
    std::string      out;
    std::size_t      cursor      = 0;
    ca::usize        expansions  = 0;

    // 单遍词法走查 + 轻量描述符文法校验（垃圾入 fail-loud，不静默替换）。
    // 返回 true = 校验与展开完成（result.raw 就绪）；false = 已填 Malformed
    // 类失败。
    const auto emitTypeSegment = [&]() -> bool {
        // 前置 '[' 已由调用方消费；此处 input[cursor] ∈ {基类型, 'L', 'V'?}。
        const char c = input[cursor];
        if (c == 'L') {
            ++cursor;
            const auto nameBegin = cursor;
            while (cursor < input.size() && input[cursor] != ';') {
                // 泛型签名标记：fail-loud（Sig 属性 v2 另批）。
                if (input[cursor] == '<' || input[cursor] == '>') {
                    result = failure(ShadowSlotError::MalformedHandle,
                                     "generic signature marker '" + std::string(1, input[cursor]) +
                                         "' inside descriptor " + quoted(input) +
                                         " is not accepted by the descriptor slot (planned); "
                                         "pass the erased descriptor");
                    return false;
                }
                ++cursor;
            }
            if (cursor >= input.size()) {
                result = failure(ShadowSlotError::MalformedHandle,
                                 "unterminated object type segment in descriptor " +
                                     quoted(input));
                return false;
            }
            const auto content = input.substr(nameBegin, cursor - nameBegin);
            ++cursor;   // ';'
            const auto parts = parseShadowRef(content);
            switch (parts.form) {
            case ShadowRefForm::Raw:
                out += 'L';
                out += content;
                out += ';';
                break;
            case ShadowRefForm::EscapedLiteral:
                // 字面请求：剥 $ 后的段按普通类名输出（与 Name 槽剥皮语义一致）。
                out += 'L';
                out += parts.literal;
                out += ';';
                break;
            case ShadowRefForm::ClassRef: {
                ShadowSlotResult entryResult;
                const auto entry =
                    resolveShadowHandleEntry(table, std::string(content), entryResult);
                if (!entry.has_value()) {
                    result = std::move(entryResult);
                    return false;
                }
                out += 'L';
                out += entry->rawName;
                out += ';';
                ++expansions;
                break;
            }
            case ShadowRefForm::MemberRef:
                result = failure(ShadowSlotError::MalformedHandle,
                                 "member reference " + quoted(content) +
                                     " cannot appear in a type position of descriptor " +
                                     quoted(input));
                return false;
            case ShadowRefForm::Malformed:
                result = failure(ShadowSlotError::MalformedHandle,
                                 "malformed reference " + quoted(content) +
                                     " inside descriptor " + quoted(input));
                return false;
            }
            return true;
        }
        // 基类型（含 V 的调用方约束在外面）。
        static constexpr std::string_view kPrimitives = "BCDFIJSZ";
        if (kPrimitives.find(c) == std::string_view::npos) {
            result = failure(ShadowSlotError::MalformedHandle,
                             "byte " + std::string(1, c) + " at offset " +
                                 std::to_string(cursor) + " does not fit a descriptor: " +
                                 quoted(input));
            return false;
        }
        out += c;
        ++cursor;
        return true;
    };

    const auto parseFieldType = [&]() -> bool {
        while (cursor < input.size() && input[cursor] == '[') {
            out += '[';
            ++cursor;
        }
        return emitTypeSegment();
    };

    bool ok = false;
    if (cursor < input.size() && input[cursor] == '(') {
        // 方法描述符：( 参数类型* ) 返回类型（V 或字段类型）。
        out += '(';
        ++cursor;
        while (cursor < input.size() && input[cursor] != ')') {
            if (!parseFieldType()) return result;
        }
        if (cursor >= input.size()) {
            result = failure(ShadowSlotError::MalformedHandle,
                             "unterminated parameter list in descriptor " + quoted(input));
            return result;
        }
        out += ')';
        ++cursor;
        if (cursor < input.size() && input[cursor] == 'V') {
            out += 'V';
            ++cursor;
        }
        else {
            ok = parseFieldType();
            if (!ok) return result;
        }
    }
    else {
        ok = parseFieldType();
        if (!ok) return result;
    }
    if (cursor != input.size()) {
        result = failure(ShadowSlotError::MalformedHandle,
                         "trailing bytes after descriptor " + quoted(input) + " at offset " +
                             std::to_string(cursor));
        return result;
    }
    result.error      = ShadowSlotError::None;
    result.raw        = std::move(out);
    result.fromHandle = expansions > 0;
    return result;
}


ShadowSlotResult resolveShadowJasmBody(const ShadowAliasTable& table,
                                       const std::string&      input)
{
    ShadowSlotResult result;
    if (input.empty()) {
        result.raw  = input;
        result.error = ShadowSlotError::None;
        return result;
    }

    // ── 尾随描述符切分 ──
    std::string refPart;
    std::string descPart;      // 括号内（方法）或尾 token（字段）；空 = 无
    std::string separator;     // 引用部与描述符之间的原样分隔（" (" / " "）
    bool        hasDesc = false;
    if (input.back() == ')') {
        const auto paren = input.rfind(" (");
        if (paren == std::string::npos || paren + 2 >= input.size() - 1) {
            return failure(ShadowSlotError::MalformedHandle,
                           "operand '" + brief(input) +
                               "' ends with ')' but has no ' (desc)' group");
        }
        refPart   = input.substr(0, paren);
        descPart  = input.substr(paren + 2, input.size() - paren - 3);
        separator = " (";
        hasDesc   = true;
    }
    else {
        const auto space = input.rfind(' ');
        if (space != std::string::npos && space + 1 < input.size()) {
            refPart   = input.substr(0, space);
            descPart  = input.substr(space + 1);
            separator = " ";
            hasDesc   = true;
        }
        else {
            refPart = input;
        }
    }

    // ── 引用部解析 ──
    // 产物：resolvedRef（替换后引用部）；expansions 计数。
    // 分派序：整体 parseShadowRef（ClassRef/MemberRef/EscapedLiteral 直达）；
    // Malformed 且带 '${' 前缀 → 复合拆分（首个 '}' 前 = owner handle，余部
    // ".name"）；其余 Raw（owner.name 复合或纯裸名零漂移直通）。
    std::string resolvedRef;
    ca::usize   expansions = 0;

    // name 段：裸名直通；嵌套 MemberRef handle 展开并核验与 raw owner 自洽。
    // 失败时已填 result，返回 false。
    const auto resolveNamePart = [&](const std::string& namePart,
                                     const std::string& rawOwner,
                                     std::string&       out) -> bool {
        if (namePart.rfind("${", 0) != 0) {
            out = namePart;
            return true;
        }
        if (parseShadowRef(namePart).form != ShadowRefForm::MemberRef) {
            result = failure(ShadowSlotError::MalformedHandle,
                             "malformed member handle in jasm operand: " +
                                 quoted(namePart));
            return false;
        }
        ShadowSlotResult entryResult;
        const auto       entry = resolveShadowHandleEntry(table, namePart, entryResult);
        if (!entry.has_value()) {
            result = std::move(entryResult);
            return false;
        }
        if (entry->owner != rawOwner) {
            result = failure(ShadowSlotError::MalformedHandle,
                             "member handle owner " + brief(entry->owner) +
                                 " conflicts with operand owner " + brief(rawOwner));
            return false;
        }
        out = entry->rawName;
        ++expansions;
        return true;
    };

    const auto parts = parseShadowRef(refPart);
    if (parts.form == ShadowRefForm::ClassRef) {
        ShadowSlotResult entryResult;
        const auto       entry = resolveShadowHandleEntry(table, refPart, entryResult);
        if (!entry.has_value()) return entryResult;
        resolvedRef = entry->rawName;
        ++expansions;
    }
    else if (parts.form == ShadowRefForm::MemberRef) {
        // 整体成员句柄：展开为 rawOwner.rawName 复合。
        ShadowSlotResult entryResult;
        const auto       entry = resolveShadowHandleEntry(table, refPart, entryResult);
        if (!entry.has_value()) return entryResult;
        resolvedRef = std::string(entry->owner) + "." + std::string(entry->rawName);
        ++expansions;
    }
    else if (parts.form == ShadowRefForm::EscapedLiteral) {
        resolvedRef = parts.literal;
    }
    else if (parts.form == ShadowRefForm::Malformed && refPart.rfind("${", 0) == 0) {
        // 复合拆分：首个 '}' 前为 owner handle（须 ClassRef 形状）；表查询
        // 错误原样直传——unknown-handle 不得被复合语境吞成 malformed。
        const auto close = refPart.find('}');
        if (close == std::string::npos) {
            return failure(ShadowSlotError::MalformedHandle,
                           "malformed reference " + quoted(refPart) +
                               " in jasm operand (no closing brace)");
        }
        const auto handlePart = refPart.substr(0, close + 1);
        const auto rest       = refPart.substr(close + 1);
        if (rest.empty() || rest[0] != '.') {
            return failure(ShadowSlotError::MalformedHandle,
                           "malformed reference " + quoted(refPart) +
                               " in jasm operand (expected .name after handle)");
        }
        if (parseShadowRef(handlePart).form != ShadowRefForm::ClassRef) {
            return failure(ShadowSlotError::MalformedHandle,
                           "malformed owner handle " + quoted(handlePart) +
                               " in jasm operand");
        }
        ShadowSlotResult ownerResult;
        const auto       ownerEntry =
            resolveShadowHandleEntry(table, handlePart, ownerResult);
        if (!ownerEntry.has_value()) return ownerResult;
        const std::string rawOwner(ownerEntry->rawName);
        std::string       resolvedName;
        if (!resolveNamePart(rest.substr(1), rawOwner, resolvedName)) return result;
        resolvedRef = rawOwner + "." + resolvedName;
        ++expansions;
    }
    else if (parts.form == ShadowRefForm::Malformed) {
        return failure(ShadowSlotError::MalformedHandle,
                       "malformed reference " + quoted(refPart) + " in jasm operand");
    }
    else {
        // Raw：复合 owner.name（name 可嵌 MemberRef handle）或纯裸名。
        const auto dot = refPart.find('.');
        if (dot == std::string::npos) {
            resolvedRef = refPart;   // 类操作数裸名：零漂移直通
        }
        else {
            const auto ownerPart = refPart.substr(0, dot);
            const auto namePart  = refPart.substr(dot + 1);
            std::string resolvedName;
            if (!resolveNamePart(namePart, ownerPart, resolvedName)) return result;
            resolvedRef = ownerPart + "." + resolvedName;
        }
    }

    // ── 描述符部展开 ──
    std::string resolvedDesc;
    if (hasDesc) {
        const auto descResult = resolveShadowDescriptor(table, descPart);
        if (descResult.error != ShadowSlotError::None) return descResult;
        resolvedDesc = descResult.raw;
        if (descResult.fromHandle) ++expansions;
    }

    result.error      = ShadowSlotError::None;
    result.raw        = resolvedRef + separator + (hasDesc ? resolvedDesc : "") +
                 (separator == " (" ? ")" : "");
    result.fromHandle = expansions > 0;
    return result;
}

std::string shadowRefHandleOf(const ShadowAliasTable& table, const ShadowAliasEntry& entry)
{
    const std::string token(entry.token8);
    if (token.size() != 8) return {};
    if (entry.kind == ShadowKind::Class) {
        // 类影子名必须呈 `Class_<纯数字>` 规范形（§2 文法前缀）；手工名不发句柄。
        const std::string shadow(entry.shadowName);
        if (shadow.size() <= 6 || shadow.compare(0, 6, "Class_") != 0) return {};
        for (std::size_t i = 6; i < shadow.size(); ++i) {
            if (!isDigit(shadow[i])) return {};
        }
        return "${" + shadow + "." + token + "}";
    }

    // 成员条目：owner 类条目须存在且可发句柄。
    const auto* ownerEntry = table.classEntry(entry.owner);
    if (ownerEntry == nullptr) return {};
    const std::string ownerHandle = shadowRefHandleOf(table, *ownerEntry);
    if (ownerHandle.empty()) return {};
    const std::string memberShadow(entry.shadowName);
    if (memberShadow.empty()) return {};
    for (const char c : memberShadow) {
        if (!isShadowSegmentChar(c)) return {};
    }
    return ownerHandle.substr(0, ownerHandle.size() - 1) + "." + memberShadow + "." + token + "}";
}

}   // namespace mjbase::renamer
