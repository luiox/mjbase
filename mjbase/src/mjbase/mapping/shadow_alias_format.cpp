#include "shadow_alias_format.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "libca/json/json_document.hpp"
#include "libca/json/json_handler.hpp"
#include "libca/json/json_parser.hpp"
#include "libca/json/json_reader.hpp"
#include "libca/json/json_value.hpp"
#include "libca/json/json_writer.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::mapping {
namespace {

using ca::str::Utf8String;
using ca::str::Utf8StringRef;

Utf8StringRef refOf(const char* text)
{
    return Utf8StringRef::from_cstr(text);
}

std::string toStd(const Utf8StringRef& value)
{
    return value.to_std_string();
}

ShadowAliasFileError makeError(ShadowAliasFileErrorKind kind, const std::string& message)
{
    ShadowAliasFileError error;
    error.kind    = kind;
    error.message = Utf8String::from_data(reinterpret_cast<const ca::u8*>(message.data()),
                                          message.size());
    return error;
}

/// alias 条目定位前缀："aliases[3]: "。DOM 无行号；sidecar 由工具写出、
/// JSON 语法错误本身已带行号，schema 定位到条目序号即可（与建议文件的
/// 行号级定位 deliberate 简化，见 hpp v1 契约注释）。
std::string atAlias(ca::usize index)
{
    return "aliases[" + std::to_string(index) + "]: ";
}

/// alias 对象的字段槽位（指针法，重复字段在收集期检出）。
struct AliasSlots
{
    const ca::json::JsonValue* kind   = nullptr;
    const ca::json::JsonValue* owner  = nullptr;
    const ca::json::JsonValue* desc   = nullptr;
    const ca::json::JsonValue* name   = nullptr;
    const ca::json::JsonValue* shadow = nullptr;
    const ca::json::JsonValue* token  = nullptr;  ///< v2 新列（#1156）
};

ca::Result<void, ShadowAliasFileError> collectAliasSlots(const ca::json::JsonValue& alias,
                                                         const std::string&     context,
                                                         AliasSlots&            slots)
{
    for (const auto& [key, value] : alias.as_object()) {
        const ca::json::JsonValue** slot  = nullptr;
        const char*                 known = nullptr;
        if (key.equals(refOf("kind"))) {
            slot  = &slots.kind;
            known = "kind";
        }
        else if (key.equals(refOf("owner"))) {
            slot  = &slots.owner;
            known = "owner";
        }
        else if (key.equals(refOf("desc"))) {
            slot  = &slots.desc;
            known = "desc";
        }
        else if (key.equals(refOf("name"))) {
            slot  = &slots.name;
            known = "name";
        }
        else if (key.equals(refOf("shadow"))) {
            slot  = &slots.shadow;
            known = "shadow";
        }
        else if (key.equals(refOf("token"))) {
            slot  = &slots.token;
            known = "token";
        }
        else {
            return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                     context + "unknown alias field: " + toStd(key)));
        }
        if (*slot != nullptr) {
            return ca::Err(makeError(
                ShadowAliasFileErrorKind::Schema,
                context + std::string("duplicate alias field: ") + known));
        }
        *slot = &value;
    }
    return ca::Ok();
}

/// 必填非空字符串槽。
ca::Result<const ca::json::JsonValue*, ShadowAliasFileError> requiredString(
    const ca::json::JsonValue* slot, const std::string& context, const char* field)
{
    if (slot == nullptr) {
        return ca::Err(
            makeError(ShadowAliasFileErrorKind::Schema, context + std::string("missing ") + field));
    }
    if (!slot->is_string() || slot->as_string().is_empty()) {
        return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                 context + std::string(field) + " must be a non-empty string"));
    }
    return ca::Ok(slot);
}

/// 可选字符串槽：未出现/空串 → nullptr。
ca::Result<const ca::json::JsonValue*, ShadowAliasFileError> optionalString(
    const ca::json::JsonValue* slot, const std::string& context, const char* field)
{
    if (slot == nullptr) return ca::Ok(static_cast<const ca::json::JsonValue*>(nullptr));
    if (!slot->is_string()) {
        return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                 context + "field '" + field + "' must be a string"));
    }
    if (slot->as_string().is_empty())
        return ca::Ok(static_cast<const ca::json::JsonValue*>(nullptr));
    return ca::Ok(slot);
}

struct AliasCompareKey
{
    std::string owner;
    std::string desc;
    std::string name;
};

bool operator==(const AliasCompareKey& a, const AliasCompareKey& b)
{
    return a.owner == b.owner && a.desc == b.desc && a.name == b.name;
}

AliasCompareKey keyOf(const Utf8StringRef& owner, const Utf8StringRef& desc,
                      const Utf8StringRef& name)
{
    return {toStd(owner), toStd(desc), toStd(name)};
}

/// #1156 token8 形状：恰 8 个字符，全部 ∈ [0-9a-f]（uint32 %08x 语义）。
bool isValidToken8(const Utf8StringRef& value)
{
    if (value.byte_length() != 8) return false;
    const std::string text = toStd(value);
    return std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

ca::Result<IntermediateMapping, ShadowAliasFileError> parseDocument(
    const ca::json::JsonDocument& document)
{
    const auto& root = document.root();
    if (!root.is_object()) {
        return ca::Err(
            makeError(ShadowAliasFileErrorKind::Schema, "shadow alias file root must be an object"));
    }

    const ca::json::JsonValue* version = nullptr;
    const ca::json::JsonValue* aliases = nullptr;
    ca::usize                  versionCount = 0, aliasesCount = 0;
    for (const auto& [key, value] : root.as_object()) {
        if (key.equals(refOf("version"))) {
            version = &value;
            ++versionCount;
        }
        else if (key.equals(refOf("aliases"))) {
            aliases = &value;
            ++aliasesCount;
        }
        else {
            return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                     "unknown shadow alias file field: " + toStd(key)));
        }
    }
    if (versionCount > 1 || aliasesCount > 1) {
        return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                 "duplicate shadow alias file field"));
    }
    if (version == nullptr) {
        return ca::Err(makeError(ShadowAliasFileErrorKind::Schema, "missing version"));
    }
    // 加法式版本门（#1156）：v1（无 token 列）照常加载（token 留空），v2 = v1 +
    // 可选 token 列；其余版本拒载。v1 文件出现 token 列在条目级报 schema 错误
    // （版本门控新列，防手编文件误标版本）。
    constexpr ca::i64 kOldestSupportedVersion = 1;
    const ca::i64 requested = version->is_int() ? version->as_int() : -1;
    if (!version->is_int() || requested < kOldestSupportedVersion ||
        requested > static_cast<ca::i64>(kShadowAliasFileVersion)) {
        return ca::Err(makeError(
            ShadowAliasFileErrorKind::Schema,
            "unsupported schema version " + std::to_string(requested) + " (supported: " +
                std::to_string(kOldestSupportedVersion) + "-" +
                std::to_string(kShadowAliasFileVersion) + "); refusing to load"));
    }
    const bool hasTokenColumn = requested >= 2;
    if (aliases == nullptr) {
        return ca::Err(makeError(ShadowAliasFileErrorKind::Schema, "missing aliases"));
    }
    if (!aliases->is_array()) {
        return ca::Err(
            makeError(ShadowAliasFileErrorKind::Schema, "aliases must be an array"));
    }

    IntermediateMapping mappings;
    std::vector<AliasCompareKey> seenClasses;
    std::vector<AliasCompareKey> seenFields;
    std::vector<AliasCompareKey> seenMethods;
    // token 表内唯一台账（#1156 落刀 N3）：非空 token8 → 首见 owner。
    std::unordered_map<std::string, std::string> seenTokens;

    ca::usize index = 0;
    for (const auto& aliasValue : aliases->as_array()) {
        const std::string context = atAlias(index);
        if (!aliasValue.is_object()) {
            return ca::Err(
                makeError(ShadowAliasFileErrorKind::Schema, context + "alias must be an object"));
        }

        AliasSlots slots;
        auto       collected = collectAliasSlots(aliasValue, context, slots);
        if (collected.is_err()) return ca::Err(std::move(collected).unwrap_err());

        if (slots.kind == nullptr) {
            return ca::Err(makeError(ShadowAliasFileErrorKind::Schema, context + "missing kind"));
        }
        if (!slots.kind->is_string()) {
            return ca::Err(
                makeError(ShadowAliasFileErrorKind::Schema, context + "kind must be a string"));
        }
        const auto kind = slots.kind->as_string();
        const bool isClass = kind.equals(refOf("class"));
        const bool isField = kind.equals(refOf("field"));
        const bool isMethod = kind.equals(refOf("method"));
        if (!isClass && !isField && !isMethod) {
            return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                     context + "unknown kind '" + toStd(kind) +
                                         "' (expected class|field|method)"));
        }

        auto nameValue = requiredString(slots.name, context, "name");
        if (nameValue.is_err()) return ca::Err(std::move(nameValue).unwrap_err());
        auto shadowValue = requiredString(slots.shadow, context, "shadow");
        if (shadowValue.is_err()) return ca::Err(std::move(shadowValue).unwrap_err());

        // owner/desc：field/method 必填非空；class 必须缺省或空。
        auto ownerValue = optionalString(slots.owner, context, "owner");
        if (ownerValue.is_err()) return ca::Err(std::move(ownerValue).unwrap_err());
        auto descValue = optionalString(slots.desc, context, "desc");
        if (descValue.is_err()) return ca::Err(std::move(descValue).unwrap_err());
        if (isClass) {
            if (ownerValue.unwrap() != nullptr || descValue.unwrap() != nullptr) {
                return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                         context + "class alias must not set owner/desc"));
            }
        }
        else if (ownerValue.unwrap() == nullptr || descValue.unwrap() == nullptr) {
            return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                     context + toStd(kind) + " alias requires non-empty owner and desc"));
        }

        // token 列（#1156）：可选；仅 v2 允许出现；8 位小写 hex。
        Utf8StringRef token = Utf8StringRef();
        if (slots.token != nullptr) {
            if (!hasTokenColumn) {
                return ca::Err(makeError(
                    ShadowAliasFileErrorKind::Schema,
                    context + "token column requires schema version 2 (file declares " +
                        std::to_string(requested) + ")"));
            }
            auto tokenValue = optionalString(slots.token, context, "token");
            if (tokenValue.is_err()) return ca::Err(std::move(tokenValue).unwrap_err());
            if (tokenValue.unwrap() != nullptr) {
                const auto tokenText = tokenValue.unwrap()->as_string();
                if (!isValidToken8(tokenText)) {
                    return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                             context +
                                                 "token must be 8 lowercase hex chars"));
                }
                token = tokenText;
            }
        }

        // 解析产物 intern 进 IntermediateMapping 自带 arena（hpp 注释：arena() 借给
        // 外部格式解析器复用）；DOM 字符串视图不跨出本函数。
        auto& arena = mappings.arena();
        const auto raw    = arena.intern(nameValue.unwrap()->as_string());
        const auto shadow = arena.intern(shadowValue.unwrap()->as_string());

        // token 表内唯一 fail-closed（#1156 落刀 N3）：生成侧 CSPRNG 碰撞重生成
        // 保证的表内唯一，手编文件可破——两符号持同一非空 token8 属数据破损，
        // 硬报错拒载（本仓哲学：不静默去重"修复"）；诊断点名两条冲突条目的
        // owner（class 条目以自身真名充当 owner 口径）。空 token（v1 遗留）不参与。
        const std::string tokenOwner =
            isClass ? toStd(raw) : toStd(ownerValue.unwrap()->as_string());
        if (!token.is_empty()) {
            const auto slot = seenTokens.emplace(toStd(token), tokenOwner);
            if (!slot.second) {
                return ca::Err(makeError(
                    ShadowAliasFileErrorKind::Schema,
                    context + "duplicate token '" + toStd(token) + "' (entries: '" +
                        slot.first->second + "' and '" + tokenOwner + "'); refusing to load"));
            }
        }

        if (isClass) {
            AliasCompareKey key = keyOf(Utf8StringRef(), Utf8StringRef(), raw);
            if (std::find(seenClasses.begin(), seenClasses.end(), key) != seenClasses.end()) {
                return ca::Err(makeError(ShadowAliasFileErrorKind::Schema,
                                         context + "duplicate class alias: " + toStd(raw)));
            }
            seenClasses.push_back(std::move(key));
            mappings.addClass(raw, shadow, token);
        }
        else {
            const auto owner = arena.intern(ownerValue.unwrap()->as_string());
            const auto desc  = arena.intern(descValue.unwrap()->as_string());
            AliasCompareKey  key = keyOf(owner, desc, raw);
            auto&            seen = isField ? seenFields : seenMethods;
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                return ca::Err(makeError(
                    ShadowAliasFileErrorKind::Schema,
                    context + "duplicate " + toStd(kind) + " alias: " + toStd(owner) + "." +
                        toStd(raw) + ":" + toStd(desc)));
            }
            seen.push_back(std::move(key));
            if (isField) {
                mappings.addField(owner, desc, raw, shadow, token);
            }
            else {
                mappings.addMethod(owner, desc, raw, shadow, token);
            }
        }
        ++index;
    }
    return ca::Ok(std::move(mappings));
}

std::string escapeJsonErrorPath(const Utf8StringRef& path)
{
    return toStd(path);
}

}   // namespace

ca::str::Utf8StringRef ShadowAliasFormat::getFormatName() const
{
    return ca::str::Utf8StringRef::from_cstr("Shadow-Alias");
}

ca::Result<IntermediateMapping, MappingError> ShadowAliasFormat::parse(Utf8StringRef mappingText)
{
    // 注册表宽松口径：schema 错误 → 空映射（显示层自然失配，不致命）；
    // 需要错误定位的加载方走 parseShadowAliasText 严格入口。
    auto parsed = parseShadowAliasText(mappingText);
    if (parsed.is_err()) return ca::Ok(IntermediateMapping());
    return ca::Ok(std::move(parsed).unwrap());
}

ca::Result<ca::str::Utf8String, MappingError> ShadowAliasFormat::format(
    const IntermediateMapping& mappings)
{
    auto text = writeShadowAliasText(mappings);
    if (text.is_err()) return ca::Ok(ca::str::Utf8String());
    return ca::Ok(std::move(text).unwrap());
}

ca::Result<IntermediateMapping, ShadowAliasFileError> parseShadowAliasText(
    const Utf8StringRef& text)
{
    auto document = ca::json::JsonReader::read(text);
    if (document.is_err()) {
        auto       parseError = std::move(document).unwrap_err();
        const auto message    = "invalid shadow alias JSON at line " +
                                std::to_string(parseError.location.line) + ": " +
                                toStd(parseError.message.ref());
        return ca::Err(makeError(ShadowAliasFileErrorKind::JsonRead, message));
    }
    return parseDocument(std::move(document).unwrap());
}

ca::Result<IntermediateMapping, ShadowAliasFileError> readShadowAliasFile(
    const Utf8StringRef& path)
{
    std::ifstream input(path.to_std_string(), std::ios::binary);
    if (!input.is_open()) {
        return ca::Err(makeError(ShadowAliasFileErrorKind::JsonRead,
                                 "failed to open shadow alias file: " + escapeJsonErrorPath(path)));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();

    auto parsed = parseShadowAliasText(
        Utf8StringRef::from_string_view(std::string_view(text.data(), text.size())));
    if (parsed.is_err()) {
        auto              error = std::move(parsed).unwrap_err();
        ca::str::Utf8StringBuilder builder;
        builder.append(path).append(": ").append(error.message.ref());
        error.message = builder.build_or_empty();
        return ca::Err(std::move(error));
    }
    return parsed;
}

namespace {

/// (name, desc) 字典序比较（generator 与 format() 共用的稳定序口径）。
bool memberLess(const std::string& aName, const std::string& aDesc, const std::string& bName,
                const std::string& bDesc)
{
    if (aName != bName) return aName < bName;
    return aDesc < bDesc;
}

}   // namespace

ca::Result<ca::str::Utf8String, ShadowAliasFileError> writeShadowAliasText(
    const IntermediateMapping& mappings)
{
    // 对象装配走 make_object_from 聚合构造（key 为已知唯一字面量，免去逐字段
    // set() 的去重查找；也是 JSON DOM 装配的推荐形态——门禁台账 InsnList_set_call
    // 针对的是 InsnList::set，此处不增其计数）。
    ca::json::JsonValue::ObjectStorage rootMembers;
    rootMembers.emplace_back(
        refOf("version"),
        ca::json::JsonValue::make_int(static_cast<ca::i64>(kShadowAliasFileVersion)));

    // 稳定序骨架 = owner 键并集（class 条目键 ∪ field/method 条目 owner 键）排序。
    // 只按 getClasses() 分组会静默丢掉 owner 无 class 条目的成员条目（手编/局部
    // sidecar + 任一 prune 移除触发重写即丢数据，R27 P2 实锤）；并集后 class 循环
    // 靠 getClassMapping 空指针跳过、成员循环靠映射组空指针跳过，仍只输出真实
    // 存在的条目——生成器产出的 sidecar（owner 键 ⊆ class 键）输出逐字节不变。
    std::set<std::string> ownerKeys;
    for (const auto& [key, value] : mappings.getClasses()) ownerKeys.insert(toStd(key));
    for (const auto& [key, value] : mappings.getFields()) ownerKeys.insert(toStd(key));
    for (const auto& [key, value] : mappings.getMethods()) ownerKeys.insert(toStd(key));
    const std::vector<std::string> classNames(ownerKeys.begin(), ownerKeys.end());

    auto aliases = ca::json::JsonValue::make_array();
    for (const auto& className : classNames) {
        const auto internal = Utf8StringRef::from_string_view(className);
        auto*      cm       = mappings.getClassMapping(internal);
        if (cm == nullptr) continue;
        ca::json::JsonValue::ObjectStorage members;
        members.emplace_back(refOf("kind"), ca::json::JsonValue::make_string(refOf("class")));
        members.emplace_back(refOf("name"), ca::json::JsonValue::make_string(cm->oldName));
        members.emplace_back(refOf("shadow"), ca::json::JsonValue::make_string(cm->newName));
        // token 列（#1156）：非空才落（空 = 旧格式/未生成条目，加法式省略）。
        if (!cm->token.is_empty()) {
            members.emplace_back(refOf("token"), ca::json::JsonValue::make_string(cm->token));
        }
        aliases.append(ca::json::JsonValue::make_object_from(std::move(members)));
    }
    for (const auto& className : classNames) {
        const auto internal = Utf8StringRef::from_string_view(className);

        if (auto* fields = mappings.getClassFieldMappings(internal)) {
            std::vector<const FieldMapping*> sorted;
            for (const auto& fm : *fields) sorted.push_back(&fm);
            std::sort(sorted.begin(), sorted.end(), [](const FieldMapping* a, const FieldMapping* b) {
                return memberLess(toStd(a->oldName), toStd(a->desc), toStd(b->oldName),
                                  toStd(b->desc));
            });
            for (const auto* fm : sorted) {
                ca::json::JsonValue::ObjectStorage members;
                members.emplace_back(refOf("kind"), ca::json::JsonValue::make_string(refOf("field")));
                members.emplace_back(refOf("owner"), ca::json::JsonValue::make_string(fm->ownerName));
                members.emplace_back(refOf("desc"), ca::json::JsonValue::make_string(fm->desc));
                members.emplace_back(refOf("name"), ca::json::JsonValue::make_string(fm->oldName));
                members.emplace_back(refOf("shadow"), ca::json::JsonValue::make_string(fm->newName));
                if (!fm->token.is_empty()) {
                    members.emplace_back(refOf("token"),
                                         ca::json::JsonValue::make_string(fm->token));
                }
                aliases.append(ca::json::JsonValue::make_object_from(std::move(members)));
            }
        }
        if (auto* methods = mappings.getClassMethodMappings(internal)) {
            std::vector<const MethodMapping*> sorted;
            for (const auto& mm : *methods) sorted.push_back(&mm);
            std::sort(sorted.begin(), sorted.end(), [](const MethodMapping* a, const MethodMapping* b) {
                return memberLess(toStd(a->oldName), toStd(a->desc), toStd(b->oldName),
                                  toStd(b->desc));
            });
            for (const auto* mm : sorted) {
                ca::json::JsonValue::ObjectStorage members;
                members.emplace_back(refOf("kind"), ca::json::JsonValue::make_string(refOf("method")));
                members.emplace_back(refOf("owner"), ca::json::JsonValue::make_string(mm->ownerName));
                members.emplace_back(refOf("desc"), ca::json::JsonValue::make_string(mm->desc));
                members.emplace_back(refOf("name"), ca::json::JsonValue::make_string(mm->oldName));
                members.emplace_back(refOf("shadow"), ca::json::JsonValue::make_string(mm->newName));
                if (!mm->token.is_empty()) {
                    members.emplace_back(refOf("token"),
                                         ca::json::JsonValue::make_string(mm->token));
                }
                aliases.append(ca::json::JsonValue::make_object_from(std::move(members)));
            }
        }
    }
    rootMembers.emplace_back(refOf("aliases"), std::move(aliases));

    ca::json::JsonDocument document;
    document.root() = ca::json::JsonValue::make_object_from(std::move(rootMembers));

    ca::json::JsonWriterOptions options;
    options.pretty = true;
    return ca::Ok(ca::json::JsonWriter::write(document, options));
}

ca::Result<void, ShadowAliasFileError> writeShadowAliasFile(const Utf8StringRef&       path,
                                                            const IntermediateMapping& mappings)
{
    auto text = writeShadowAliasText(mappings);
    if (text.is_err()) return ca::Err(std::move(text).unwrap_err());

    std::ofstream out(path.to_std_string(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return ca::Err(makeError(ShadowAliasFileErrorKind::Io,
                                 "failed to open shadow alias file for writing: " +
                                     escapeJsonErrorPath(path)));
    }
    const auto& payload = std::move(text).unwrap();
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.byte_length()));
    if (!out) {
        return ca::Err(makeError(ShadowAliasFileErrorKind::Io,
                                 "failed to write shadow alias file: " + escapeJsonErrorPath(path)));
    }
    return ca::Ok();
}

}   // namespace mjbase::mapping
