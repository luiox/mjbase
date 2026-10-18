#include "project_store.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <iomanip>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "libca/crypto/sha256.hpp"
#include "libca/fs/file_util.hpp"
#include "libca/fs/path_util.hpp"
#include "libca/json/json_document.hpp"
#include "libca/json/json_reader.hpp"
#include "libca/json/json_value.hpp"
#include "libca/json/json_writer.hpp"
#include "libca/str/charset.hpp"

#include "analysis_project.hpp"

// 平台头最后：Windows SDK 的 WinBase.h 定义 CREATE_NEW 等宏，会撞 libca
// file_util.hpp 的 FileMode 常量名——所有用到这些名字的头必须先于本块。
#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mjbase::project {
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

Utf8String copyOf(const Utf8StringRef& value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                 value.byte_length());
}

Utf8String copyOf(const std::string& value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

ProjectStoreError makeError(ProjectStoreErrorKind kind, const std::string& message)
{
    ProjectStoreError error;
    error.kind    = kind;
    error.message = copyOf(message);
    return error;
}

/// 系统错误文案的 UTF-8 安全化（错误消息构造**不得抛**——全库 Result 纪律）。
///
/// std::error_code::message() 的编码跟随系统 ANSI 代码页（中文 Windows =
/// GBK），直塞 Utf8String::from_data 会因严格 UTF-8 校验抛异常（C3 重试
/// 用例首例踩响：换名撞句柄的 ec 文案在中文系统上非法 UTF-8）。策略：纯
/// ASCII 原样；非 ASCII 在 Windows 走 CP_ACP→UTF-8（CharsetConverter 内置
/// 表，无 iconv 依赖），转换失败/其他平台逐字节折叠为 '?'——诊断定位靠
/// 消息里的路径与重试计数，不依赖这段尾巴。
std::string utf8SafeSystemText(const std::string& raw)
{
    bool asciiOnly = true;
    for (const char c : raw) {
        if (static_cast<unsigned char>(c) > 0x7F) {
            asciiOnly = false;
            break;
        }
    }
    if (asciiOnly) return raw;
#ifdef _WIN32
    auto converted = ca::str::CharsetConverter::local_to_utf8(raw);
    if (converted.is_ok()) return std::move(converted).unwrap();
#endif
    std::string folded;
    folded.reserve(raw.size());
    for (const char c : raw) {
        folded.push_back(static_cast<unsigned char>(c) > 0x7F ? '?' : c);
    }
    return folded;
}

/// section 域错误：error.section 非空（§6.1）。
ProjectStoreError makeSectionError(ProjectStoreErrorKind kind, const Utf8StringRef& section,
                                   const std::string& message)
{
    ProjectStoreError error;
    error.kind    = kind;
    error.message = copyOf(message);
    error.section = copyOf(section);
    return error;
}

/// 条目定位前缀："sections[2]: "。DOM 无行号；清单由工具写出、JSON 语法
/// 错误本身已带行号，schema 定位到条目序号即可（analysis_project.cpp 同款）。
std::string atEntry(ca::usize index)
{
    return std::string("sections[") + std::to_string(index) + std::string("]: ");
}

const char* formatTagOf(SectionFormat format)
{
    switch (format) {
    case SectionFormat::Json: return "json";
    case SectionFormat::Jsonl: return "jsonl";
    case SectionFormat::Opaque: return "opaque";
    }
    return "opaque";
}

bool parseFormatTag(const Utf8StringRef& tag, SectionFormat& format)
{
    if (tag.equals(refOf("json"))) {
        format = SectionFormat::Json;
        return true;
    }
    if (tag.equals(refOf("jsonl"))) {
        format = SectionFormat::Jsonl;
        return true;
    }
    if (tag.equals(refOf("opaque"))) {
        format = SectionFormat::Opaque;
        return true;
    }
    return false;
}

//-- 通用槽位辅助（analysis_project.cpp 同款纪律） ----------------------------

ca::Result<const ca::json::JsonValue*, ProjectStoreError> requiredString(
    const ca::json::JsonValue* slot, const std::string& context, const char* field)
{
    if (slot == nullptr) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 context + std::string("missing ") + field));
    }
    if (!slot->is_string() || slot->as_string().is_empty()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 context + std::string(field) + " must be a non-empty string"));
    }
    return ca::Ok(slot);
}

/// 可选字符串槽：未出现/空串 → nullptr。
ca::Result<const ca::json::JsonValue*, ProjectStoreError> optionalString(
    const ca::json::JsonValue* slot, const std::string& context, const char* field)
{
    if (slot == nullptr) return ca::Ok(static_cast<const ca::json::JsonValue*>(nullptr));
    if (!slot->is_string()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 context + "field '" + std::string(field) + "' must be a string"));
    }
    if (slot->as_string().is_empty())
        return ca::Ok(static_cast<const ca::json::JsonValue*>(nullptr));
    return ca::Ok(slot);
}

/// 必填 int 槽，附带谓词校验（无捕获 lambda → 函数指针）。
ca::Result<ca::i64, ProjectStoreError> requiredInt(
    const ca::json::JsonValue* slot, const std::string& context, const char* field,
    bool (*valid)(ca::i64), const char* rule)
{
    if (slot == nullptr) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 context + std::string("missing ") + field));
    }
    if (!slot->is_int()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 context + std::string(field) + " must be an integer"));
    }
    const ca::i64 value = slot->as_int();
    if (!valid(value)) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 context + std::string(field) + " " + rule + " (got " +
                                     std::to_string(value) + ")"));
    }
    return ca::Ok(value);
}

//-- content_hash 形状（"sha256:" + 64 hex）---------------------------------

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isValidContentHash(const std::string& hash)
{
    if (hash.size() != 7 + 64) return false;
    if (hash.compare(0, 7, "sha256:") != 0) return false;
    for (std::size_t i = 7; i < hash.size(); ++i) {
        if (!isHexDigit(hash[i])) return false;
    }
    return true;
}

ProjectStoreError badContentHashError(const std::string& context, const std::string& hash)
{
    return makeError(ProjectStoreErrorKind::Schema,
                     context + "content_hash must be \"sha256:\" + 64 hex (got \"" + hash + "\")");
}

//-- 命名校验字节规则（§3.3）------------------------------------------------

bool isLowerAlnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool isNameChar(char c)
{
    return isLowerAlnum(c) || c == '.' || c == '_' || c == '-';
}

bool isReservedDeviceName(const std::string& segment)
{
    // 已限小写字符集，仍显式大小写不敏感匹配（防未来放宽，§3.3）。
    std::string lower;
    lower.reserve(segment.size());
    for (const char c : segment) {
        lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    static const char* kReserved[] = {"con",   "prn",  "aux",  "nul",  "com1", "com2", "com3",
                                      "com4",  "com5", "com6", "com7", "com8", "com9", "lpt1",
                                      "lpt2",  "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8",
                                      "lpt9"};
    for (const char* name : kReserved) {
        if (lower == name) return true;
    }
    return false;
}

std::filesystem::path nativePath(const std::string& path)
{
    return std::filesystem::u8path(path);
}

/// 在全新为空的目录内落一个初始文件（plain 写；见 createBundleSkeleton 注）。
ca::Result<void, ProjectStoreError> writeInitialFile(const std::string& path,
                                                     const std::string& content,
                                                     const char* what)
{
    auto written = ca::fs::FileUtil::write_text(path, content);
    if (written.is_err()) {
        auto       fsError = std::move(written).unwrap_err();
        const auto message = std::string("failed to write initial ") + what + " '" + path +
                             "': " + ca::fs::to_string(fsError);
        return ca::Err(makeError(ProjectStoreErrorKind::Io, message));
    }
    return ca::Ok();
}

//-- depends 条目解析 ------------------------------------------------------

ca::Result<SectionDep, ProjectStoreError> parseDepend(const ca::json::JsonValue& value,
                                                      const std::string& context)
{
    if (!value.is_object()) {
        return ca::Err(
            makeError(ProjectStoreErrorKind::Schema, context + "depend must be an object"));
    }
    const ca::json::JsonValue* path        = nullptr;
    const ca::json::JsonValue* fingerprint = nullptr;
    for (const auto& [key, member] : value.as_object()) {
        const ca::json::JsonValue** slot  = nullptr;
        const char*                 known = nullptr;
        if (key.equals(refOf("path"))) {
            slot  = &path;
            known = "path";
        }
        else if (key.equals(refOf("fingerprint"))) {
            slot  = &fingerprint;
            known = "fingerprint";
        }
        else {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     context + "unknown depend field: " + toStd(key)));
        }
        if (*slot != nullptr) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     context + std::string("duplicate depend field: ") + known));
        }
        *slot = &member;
    }
    auto pathValue = requiredString(path, context, "path");
    if (pathValue.is_err()) return ca::Err(std::move(pathValue).unwrap_err());
    auto fingerprintValue = requiredString(fingerprint, context, "fingerprint");
    if (fingerprintValue.is_err()) return ca::Err(std::move(fingerprintValue).unwrap_err());

    SectionDep depend;
    depend.path        = copyOf(pathValue.unwrap()->as_string());
    depend.fingerprint = copyOf(fingerprintValue.unwrap()->as_string());
    return ca::Ok(std::move(depend));
}

ca::Result<StoreSectionEntry, ProjectStoreError> parseSectionEntry(
    const ca::json::JsonValue& value, const std::string& context)
{
    if (!value.is_object()) {
        return ca::Err(
            makeError(ProjectStoreErrorKind::Schema, context + "section entry must be an object"));
    }

    const ca::json::JsonValue* name        = nullptr;
    const ca::json::JsonValue* format      = nullptr;
    const ca::json::JsonValue* sectionVer  = nullptr;
    const ca::json::JsonValue* contentHash = nullptr;
    const ca::json::JsonValue* byteSize    = nullptr;
    const ca::json::JsonValue* recordCount = nullptr;
    const ca::json::JsonValue* profile     = nullptr;
    const ca::json::JsonValue* depends     = nullptr;
    const ca::json::JsonValue* producedBy  = nullptr;
    const ca::json::JsonValue* createdAt   = nullptr;
    const ca::json::JsonValue* updatedAt   = nullptr;
    for (const auto& [key, member] : value.as_object()) {
        const ca::json::JsonValue** slot  = nullptr;
        const char*                 known = nullptr;
        if (key.equals(refOf("name"))) {
            slot  = &name;
            known = "name";
        }
        else if (key.equals(refOf("format"))) {
            slot  = &format;
            known = "format";
        }
        else if (key.equals(refOf("section_version"))) {
            slot  = &sectionVer;
            known = "section_version";
        }
        else if (key.equals(refOf("content_hash"))) {
            slot  = &contentHash;
            known = "content_hash";
        }
        else if (key.equals(refOf("byte_size"))) {
            slot  = &byteSize;
            known = "byte_size";
        }
        else if (key.equals(refOf("record_count"))) {
            slot  = &recordCount;
            known = "record_count";
        }
        else if (key.equals(refOf("profile"))) {
            slot  = &profile;
            known = "profile";
        }
        else if (key.equals(refOf("depends"))) {
            slot  = &depends;
            known = "depends";
        }
        else if (key.equals(refOf("produced_by"))) {
            slot  = &producedBy;
            known = "produced_by";
        }
        else if (key.equals(refOf("created_at"))) {
            slot  = &createdAt;
            known = "created_at";
        }
        else if (key.equals(refOf("updated_at"))) {
            slot  = &updatedAt;
            known = "updated_at";
        }
        else {
            // 条目字段集严格（§4.2）：未知字段即整文件拒载。
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     context + "unknown section entry field: " + toStd(key)));
        }
        if (*slot != nullptr) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     context + std::string("duplicate section entry field: ") +
                                         known));
        }
        *slot = &member;
    }

    StoreSectionEntry entry;

    auto nameValue = requiredString(name, context, "name");
    if (nameValue.is_err()) return ca::Err(std::move(nameValue).unwrap_err());
    entry.name = copyOf(nameValue.unwrap()->as_string());
    auto nameValidated = validateSectionName(entry.name.ref());
    if (nameValidated.is_err()) {
        auto nameError = std::move(nameValidated).unwrap_err();
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Schema, entry.name.ref(),
            context + "invalid section name: " + nameError.message.to_std_string()));
    }

    // format：值不识别 → opaque 收容保留（§4.2/§9.3），原载串存 formatTag。
    auto formatValue = requiredString(format, context, "format");
    if (formatValue.is_err()) return ca::Err(std::move(formatValue).unwrap_err());
    entry.formatTag = copyOf(formatValue.unwrap()->as_string());
    if (!parseFormatTag(entry.formatTag.ref(), entry.format)) {
        entry.format         = SectionFormat::Opaque;
        entry.formatRetained = true;
    }

    // 上界与字段类型 u32 对齐（R52/CP-2）：u32 最大值 4294967295 合法，
    // 超出（含 u64 语义的更大值）按 Schema 拒载，不截断不静默收窄。
    auto versionValue = requiredInt(
        sectionVer, context, "section_version",
        [](ca::i64 v) { return v >= 1 && v <= 0xffffffffLL; },
        "must be a u32 in [1, 4294967295]");
    if (versionValue.is_err()) return ca::Err(std::move(versionValue).unwrap_err());
    entry.sectionVersion = static_cast<ca::u32>(versionValue.unwrap());

    auto hashValue = requiredString(contentHash, context, "content_hash");
    if (hashValue.is_err()) return ca::Err(std::move(hashValue).unwrap_err());
    entry.contentHash = copyOf(hashValue.unwrap()->as_string());
    if (!isValidContentHash(toStd(entry.contentHash.ref()))) {
        return ca::Err(badContentHashError(context, toStd(entry.contentHash.ref())));
    }

    auto sizeValue = requiredInt(
        byteSize, context, "byte_size", [](ca::i64 v) { return v >= 0; }, "must be >= 0");
    if (sizeValue.is_err()) return ca::Err(std::move(sizeValue).unwrap_err());
    entry.byteSize       = static_cast<ca::u64>(sizeValue.unwrap());
    entry.hasRecordCount = recordCount != nullptr;
    if (recordCount != nullptr) {
        auto recordsValue = requiredInt(
            recordCount, context, "record_count", [](ca::i64 v) { return v >= 0; },
            "must be >= 0");
        if (recordsValue.is_err()) return ca::Err(std::move(recordsValue).unwrap_err());
        entry.recordCount = static_cast<ca::u64>(recordsValue.unwrap());
    }
    // record_count 相容规则（§4.2）：jsonl 必有；json/opaque 禁止；收容条目透传。
    if (!entry.formatRetained) {
        if (entry.format == SectionFormat::Jsonl && !entry.hasRecordCount) {
            return ca::Err(makeError(
                ProjectStoreErrorKind::Schema,
                context + "missing record_count (required for jsonl sections)"));
        }
        if (entry.format != SectionFormat::Jsonl && entry.hasRecordCount) {
            return ca::Err(makeError(
                ProjectStoreErrorKind::Schema,
                context + "record_count is only allowed on jsonl sections (got one on \"" +
                    toStd(entry.formatTag.ref()) + "\")"));
        }
    }

    auto profileValue = optionalString(profile, context, "profile");
    if (profileValue.is_err()) return ca::Err(std::move(profileValue).unwrap_err());
    if (profileValue.unwrap() != nullptr) {
        entry.profile = copyOf(profileValue.unwrap()->as_string());
    }

    if (depends == nullptr || !depends->is_array()) {
        return ca::Err(
            makeError(ProjectStoreErrorKind::Schema, context + "depends must be an array"));
    }
    std::set<std::string> seenDepPaths;
    for (const auto& item : depends->as_array()) {
        auto depend = parseDepend(item, context);
        if (depend.is_err()) return ca::Err(std::move(depend).unwrap_err());
        auto parsed = std::move(depend).unwrap();
        if (!seenDepPaths.insert(toStd(parsed.path.ref())).second) {
            return ca::Err(makeError(
                ProjectStoreErrorKind::Schema,
                context + "duplicate depend path: " + toStd(parsed.path.ref())));
        }
        entry.depends.push_back(std::move(parsed));
    }

    auto producerValue = requiredString(producedBy, context, "produced_by");
    if (producerValue.is_err()) return ca::Err(std::move(producerValue).unwrap_err());
    entry.producedBy = copyOf(producerValue.unwrap()->as_string());

    auto createdValue = requiredString(createdAt, context, "created_at");
    if (createdValue.is_err()) return ca::Err(std::move(createdValue).unwrap_err());
    entry.createdAt = copyOf(createdValue.unwrap()->as_string());

    auto updatedValue = requiredString(updatedAt, context, "updated_at");
    if (updatedValue.is_err()) return ca::Err(std::move(updatedValue).unwrap_err());
    entry.updatedAt = copyOf(updatedValue.unwrap()->as_string());

    return ca::Ok(std::move(entry));
}

/// 写出前校验（Schema）：与解析纪律同口径，抓 writer bug。
ca::Result<void, ProjectStoreError> validateEntryForWrite(const StoreSectionEntry& entry,
                                                          const std::string& context)
{
    auto nameValidated = validateSectionName(entry.name.ref());
    if (nameValidated.is_err()) {
        auto nameError = std::move(nameValidated).unwrap_err();
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Schema, entry.name.ref(),
            context + "invalid section name: " + nameError.message.to_std_string()));
    }
    const bool tagMatchesFormat =
        entry.formatRetained ? !entry.formatTag.is_empty()
                             : entry.formatTag.equals(refOf(formatTagOf(entry.format)));
    if (!tagMatchesFormat) {
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Schema, entry.name.ref(),
            context + "formatTag must carry the manifest format string (retained entries keep "
                      "the original tag; known formats use the canonical tag)"));
    }
    if (!isValidContentHash(toStd(entry.contentHash.ref()))) {
        return ca::Err(badContentHashError(context, toStd(entry.contentHash.ref())));
    }
    if (!entry.formatRetained) {
        if (entry.format == SectionFormat::Jsonl && !entry.hasRecordCount) {
            return ca::Err(makeSectionError(ProjectStoreErrorKind::Schema, entry.name.ref(),
                                            context + "missing record_count (required for jsonl "
                                                      "sections)"));
        }
        if (entry.format != SectionFormat::Jsonl && entry.hasRecordCount) {
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, entry.name.ref(),
                context + "record_count is only allowed on jsonl sections"));
        }
    }
    if (entry.producedBy.is_empty() || entry.createdAt.is_empty() || entry.updatedAt.is_empty()) {
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Schema, entry.name.ref(),
            context + "produced_by/created_at/updated_at must be non-empty"));
    }
    std::set<std::string> seenDepPaths;
    for (const auto& depend : entry.depends) {
        if (depend.path.is_empty() || depend.fingerprint.is_empty()) {
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, entry.name.ref(),
                context + "depend path/fingerprint must be non-empty"));
        }
        if (!seenDepPaths.insert(toStd(depend.path.ref())).second) {
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, entry.name.ref(),
                context + "duplicate depend path: " + toStd(depend.path.ref())));
        }
    }
    return ca::Ok();
}

ca::json::JsonValue dependJson(const SectionDep& depend)
{
    ca::json::JsonValue::ObjectStorage members;
    members.emplace_back(refOf("path"), ca::json::JsonValue::make_string(depend.path.ref()));
    members.emplace_back(refOf("fingerprint"),
                         ca::json::JsonValue::make_string(depend.fingerprint.ref()));
    return ca::json::JsonValue::make_object_from(std::move(members));
}

}   // namespace

//-- section 命名校验（§3.3）--------------------------------------------------

ca::Result<void, ProjectStoreError> validateSectionName(const Utf8StringRef& name)
{
    const auto*       data = reinterpret_cast<const char*>(name.data());
    const std::size_t size = name.byte_length();

    if (size == 0) {
        return ca::Err(makeSectionError(ProjectStoreErrorKind::Schema, name,
                                        "section name must be non-empty"));
    }
    if (size > 64) {
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Schema, name,
            "section name must be at most 64 characters (got " + std::to_string(size) + ")"));
    }
    if (!isLowerAlnum(data[0])) {
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Schema, name,
            "section name must match ^[a-z0-9][a-z0-9._-]{0,63}$ (bad first character)"));
    }
    for (std::size_t i = 1; i < size; ++i) {
        if (!isNameChar(data[i])) {
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, name,
                "section name must match ^[a-z0-9][a-z0-9._-]{0,63}$ (bad character at index " +
                    std::to_string(i) + ")"));
        }
    }
    if (data[size - 1] == '.') {
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Schema, name,
            "section name must not end with '.' (Windows trailing dot trap)"));
    }

    // 首个 '.' 之前的段不在 Windows 保留设备名集内（§3.3）。
    const std::size_t firstDot = name.index_of('.');
    const std::string firstSegment =
        firstDot == static_cast<std::size_t>(-1) ? std::string(data, size)
                                                 : std::string(data, firstDot);
    if (isReservedDeviceName(firstSegment)) {
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Schema, name,
            "section name first segment '" + firstSegment +
                "' is a reserved Windows device name"));
    }
    return ca::Ok();
}

const char* sectionFileExtension(SectionFormat format)
{
    switch (format) {
    case SectionFormat::Json: return ".json";
    case SectionFormat::Jsonl: return ".jsonl";
    case SectionFormat::Opaque: return ".bin";
    }
    return ".bin";
}

Utf8String sectionFileName(const Utf8StringRef& name, SectionFormat format)
{
    std::string file = toStd(name);
    file += sectionFileExtension(format);
    return copyOf(file);
}

//-- 路径判别与目录骨架（§3.2/§3.3）--------------------------------------------

StorePathKind classifyStorePath(const Utf8StringRef& path)
{
    std::error_code ec;
    const auto      target = nativePath(toStd(path));
    if (!std::filesystem::exists(target, ec) || ec) return StorePathKind::Missing;
    if (std::filesystem::is_directory(target, ec) && !ec) return StorePathKind::Directory;
    if (std::filesystem::is_regular_file(target, ec) && !ec) return StorePathKind::BareFile;
    return StorePathKind::Other;
}

ca::Result<void, ProjectStoreError> createBundleSkeleton(const Utf8StringRef& dir)
{
    const std::string dirArg = toStd(dir);
    const auto        kind   = classifyStorePath(dir);
    if (kind == StorePathKind::BareFile) {
        return ca::Err(makeError(
            ProjectStoreErrorKind::Open,
            "cannot create bundle: '" + dirArg +
                "' is a bare project file; bundles are directories (see upgradeFromFile, C2)"));
    }
    if (kind == StorePathKind::Other) {
        return ca::Err(makeError(ProjectStoreErrorKind::Open,
                                 "cannot create bundle: '" + dirArg +
                                     "' exists and is neither a directory nor a regular file"));
    }

    std::error_code ec;
    if (kind == StorePathKind::Directory) {
        // 存在则必须为空（§6.2 create 语义）。
        auto it = std::filesystem::directory_iterator(nativePath(dirArg), ec);
        if (ec) {
            return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                     "cannot inspect bundle directory '" + dirArg +
                                         "': " + utf8SafeSystemText(ec.message())));
        }
        if (it != std::filesystem::directory_iterator()) {
            const std::string firstEntry = it->path().filename().u8string();
            return ca::Err(makeError(
                ProjectStoreErrorKind::Open,
                "cannot create bundle: directory '" + dirArg +
                    "' exists and is not empty (first entry: '" + firstEntry + "')"));
        }
    }
    else {
        std::filesystem::create_directories(nativePath(dirArg), ec);
        if (ec) {
            return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                     "failed to create bundle directory '" + dirArg +
                                         "': " + utf8SafeSystemText(ec.message())));
        }
    }

    // 子目录骨架：sections/ + tmp/（tmp 恒在 bundle 内——同卷 MUST，§8.1/R45-P6）。
    for (const char* sub : {kProjectStoreSectionsDirName, kProjectStoreTmpDirName}) {
        std::filesystem::create_directories(nativePath(ca::fs::PathUtil::join(dirArg, sub)), ec);
        if (ec) {
            return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                     std::string("failed to create bundle subdirectory '") + sub +
                                         "' under '" + dirArg + "': " + utf8SafeSystemText(ec.message())));
        }
    }

    // 初始清单：v1 空白（确定性写出，验收 §11-1 骨架的重开逐字节口径）。
    StoreManifest initial;
    initial.engine    = copyOf(kProjectStoreEngine);
    auto manifestText = writeStoreManifestText(initial);
    if (manifestText.is_err()) return ca::Err(std::move(manifestText).unwrap_err());
    auto wroteManifest =
        writeInitialFile(ca::fs::PathUtil::join(dirArg, kProjectStoreManifestName),
                         std::move(manifestText).unwrap().to_std_string(), "store.json");
    if (wroteManifest.is_err()) return ca::Err(std::move(wroteManifest).unwrap_err());

    // 初始实体：空工程，字节 = writeAnalysisProjectText 产出（§4.3 零破坏线）。
    auto entityText = writeAnalysisProjectText(AnalysisProject::createEmpty());
    if (entityText.is_err()) {
        auto            entityError = std::move(entityText).unwrap_err();
        const auto      message =
            "failed to serialize initial project.json: " + entityError.message.to_std_string();
        return ca::Err(makeError(ProjectStoreErrorKind::Io, message));
    }
    auto wroteEntity =
        writeInitialFile(ca::fs::PathUtil::join(dirArg, kProjectStoreEntityName),
                         std::move(entityText).unwrap().to_std_string(), "project.json");
    if (wroteEntity.is_err()) return ca::Err(std::move(wroteEntity).unwrap_err());
    return ca::Ok();
}

//-- store.json 严格解析（§4.2）------------------------------------------------

ca::Result<StoreManifest, ProjectStoreError> parseStoreManifestText(const Utf8StringRef& text)
{
    auto document = ca::json::JsonReader::read(text);
    if (document.is_err()) {
        auto       parseError = std::move(document).unwrap_err();
        const auto message    = "store manifest is not valid JSON at line " +
                                std::to_string(parseError.location.line) + ": " +
                                toStd(parseError.message.ref());
        return ca::Err(makeError(ProjectStoreErrorKind::Open, message));
    }
    auto        parsedDocument = std::move(document).unwrap();
    const auto& root           = parsedDocument.root();
    if (!root.is_object()) {
        return ca::Err(
            makeError(ProjectStoreErrorKind::Open, "store manifest root must be an object"));
    }

    const ca::json::JsonValue* version  = nullptr;
    const ca::json::JsonValue* kind     = nullptr;
    const ca::json::JsonValue* engine   = nullptr;
    const ca::json::JsonValue* sections = nullptr;
    // 顶层未知字段忽略（加法演进，§4.2），但重复键一律拒载——未知键重复也要抓。
    std::set<std::string>      seenIgnoredKeys;
    for (const auto& [key, value] : root.as_object()) {
        const ca::json::JsonValue** slot  = nullptr;
        const char*                 known = nullptr;
        if (key.equals(refOf("version"))) {
            slot  = &version;
            known = "version";
        }
        else if (key.equals(refOf("kind"))) {
            slot  = &kind;
            known = "kind";
        }
        else if (key.equals(refOf("engine"))) {
            slot  = &engine;
            known = "engine";
        }
        else if (key.equals(refOf("sections"))) {
            slot  = &sections;
            known = "sections";
        }
        else {
            const std::string ignored = toStd(key);
            if (!seenIgnoredKeys.insert(ignored).second) {
                return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                         "duplicate store manifest field: " + ignored));
            }
            continue;
        }
        if (*slot != nullptr) {
            return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                     std::string("duplicate store manifest field: ") + known));
        }
        *slot = &value;
    }

    // kind·版本不识别 / 结构缺件 → Open（§6.1；打开期降级路径按 kind 路由）。
    if (version == nullptr) {
        return ca::Err(makeError(ProjectStoreErrorKind::Open, "missing version"));
    }
    if (!version->is_int() ||
        version->as_int() != static_cast<ca::i64>(kProjectStoreFileVersion)) {
        const ca::i64 actual = version->is_int() ? version->as_int() : -1;
        return ca::Err(makeError(
            ProjectStoreErrorKind::Open,
            "unsupported store manifest version " + std::to_string(actual) +
                " (supported: " + std::to_string(kProjectStoreFileVersion) +
                "); refusing to load (manifest-unloadable: section files degrade to orphans, "
                "never auto-pruned)"));
    }
    if (kind == nullptr || !kind->is_string() ||
        !kind->as_string().equals(refOf(kProjectStoreKind))) {
        return ca::Err(makeError(
            ProjectStoreErrorKind::Open,
            std::string("kind must be \"") + kProjectStoreKind + "\" (got " +
                (kind != nullptr && kind->is_string() ? "\"" + toStd(kind->as_string()) + "\""
                                                      : std::string("non-string")) +
                "); this is not a store manifest"));
    }
    if (sections == nullptr || !sections->is_array()) {
        return ca::Err(makeError(ProjectStoreErrorKind::Open, "sections must be an array"));
    }

    auto engineValue = optionalString(engine, "", "engine");
    if (engineValue.is_err()) return ca::Err(std::move(engineValue).unwrap_err());

    StoreManifest manifest;
    if (engineValue.unwrap() != nullptr) {
        manifest.engine = copyOf(engineValue.unwrap()->as_string());
    }

    std::set<std::string> seenNames;
    ca::usize             entryIndex = 0;
    for (const auto& item : sections->as_array()) {
        const std::string context = atEntry(entryIndex);
        auto              entry   = parseSectionEntry(item, context);
        if (entry.is_err()) return ca::Err(std::move(entry).unwrap_err());
        auto parsed = std::move(entry).unwrap();
        if (!seenNames.insert(toStd(parsed.name.ref())).second) {
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, parsed.name.ref(),
                context + "duplicate section name: " + toStd(parsed.name.ref())));
        }
        if (parsed.formatRetained) {
            // §4.2：值不识别时按 opaque 保留处理，记 openDiagnostics，不拒载。
            manifest.diagnostics.push_back(copyOf(
                context + "section '" + toStd(parsed.name.ref()) + "': unrecognized format \"" +
                toStd(parsed.formatTag.ref()) + "\"; retained verbatim as opaque"));
        }
        manifest.sections.push_back(std::move(parsed));
        ++entryIndex;
    }
    return ca::Ok(std::move(manifest));
}

ca::Result<StoreManifest, ProjectStoreError> readStoreManifestFile(const Utf8StringRef& bundleDir)
{
    const std::string path = ca::fs::PathUtil::join(toStd(bundleDir), kProjectStoreManifestName);
    if (!ca::fs::FileUtil::exists(path)) {
        return ca::Err(makeError(
            ProjectStoreErrorKind::Open,
            std::string("bundle manifest missing: '") + path +
                "' (manifest-unloadable: section files degrade to orphans, never auto-pruned)"));
    }
    auto content = ca::fs::FileUtil::read_all_text(path);
    if (content.is_err()) {
        auto       fsError = std::move(content).unwrap_err();
        const auto message =
            "failed to read bundle manifest '" + path + "': " + ca::fs::to_string(fsError);
        return ca::Err(makeError(ProjectStoreErrorKind::Io, message));
    }
    const auto body = std::move(content).unwrap();
    auto       parsed = parseStoreManifestText(Utf8StringRef::from_string_view(
        std::string_view(body.data(), body.size())));
    if (parsed.is_err()) {
        auto                       error = std::move(parsed).unwrap_err();
        ca::str::Utf8StringBuilder builder;
        builder.append(copyOf(path)).append(": ").append(error.message.ref());
        error.message = builder.build_or_empty();
        return ca::Err(std::move(error));
    }
    return parsed;
}

//-- 确定性写出（§4.2）----------------------------------------------------------

ca::Result<ca::str::Utf8String, ProjectStoreError> writeStoreManifestText(
    const StoreManifest& manifest)
{
    if (manifest.version != kProjectStoreFileVersion) {
        return ca::Err(makeError(ProjectStoreErrorKind::Schema,
                                 "cannot write store manifest version " +
                                     std::to_string(manifest.version) + " (supported: " +
                                     std::to_string(kProjectStoreFileVersion) + ")"));
    }

    // name 字典序（§4.2：sections 按字典序写出；解析容忍任意序，写出定序）。
    std::vector<const StoreSectionEntry*> sorted;
    sorted.reserve(manifest.sections.size());
    for (const auto& entry : manifest.sections) sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(),
              [](const StoreSectionEntry* a, const StoreSectionEntry* b) {
                  return toStd(a->name.ref()) < toStd(b->name.ref());
              });

    ca::json::JsonValue::ObjectStorage rootMembers;
    rootMembers.emplace_back(
        refOf("version"),
        ca::json::JsonValue::make_int(static_cast<ca::i64>(kProjectStoreFileVersion)));
    rootMembers.emplace_back(refOf("kind"),
                             ca::json::JsonValue::make_string(refOf(kProjectStoreKind)));
    if (!manifest.engine.is_empty()) {
        rootMembers.emplace_back(refOf("engine"),
                                 ca::json::JsonValue::make_string(manifest.engine.ref()));
    }

    std::set<std::string> seenNames;
    auto                  sectionsJson = ca::json::JsonValue::make_array();
    for (const StoreSectionEntry* entry : sorted) {
        const std::string context = atEntry(sectionsJson.size());
        if (!seenNames.insert(toStd(entry->name.ref())).second) {
            return ca::Err(makeSectionError(ProjectStoreErrorKind::Schema, entry->name.ref(),
                                            context + "duplicate section name: " +
                                                           toStd(entry->name.ref())));
        }
        auto validated = validateEntryForWrite(*entry, context);
        if (validated.is_err()) return ca::Err(std::move(validated).unwrap_err());

        ca::json::JsonValue::ObjectStorage members;
        members.emplace_back(refOf("name"), ca::json::JsonValue::make_string(entry->name.ref()));
        members.emplace_back(refOf("format"),
                             ca::json::JsonValue::make_string(entry->formatTag.ref()));
        members.emplace_back(
            refOf("section_version"),
            ca::json::JsonValue::make_int(static_cast<ca::i64>(entry->sectionVersion)));
        members.emplace_back(refOf("content_hash"),
                             ca::json::JsonValue::make_string(entry->contentHash.ref()));
        members.emplace_back(
            refOf("byte_size"),
            ca::json::JsonValue::make_int(static_cast<ca::i64>(entry->byteSize)));
        if (entry->hasRecordCount) {
            members.emplace_back(
                refOf("record_count"),
                ca::json::JsonValue::make_int(static_cast<ca::i64>(entry->recordCount)));
        }
        if (entry->profile.has_value() && !entry->profile->is_empty()) {
            members.emplace_back(refOf("profile"),
                                 ca::json::JsonValue::make_string(entry->profile->ref()));
        }
        // depends 按 path 字典序（§4.2）。
        std::vector<const SectionDep*> sortedDeps;
        sortedDeps.reserve(entry->depends.size());
        for (const auto& depend : entry->depends) sortedDeps.push_back(&depend);
        std::sort(sortedDeps.begin(), sortedDeps.end(),
                  [](const SectionDep* a, const SectionDep* b) {
                      return toStd(a->path.ref()) < toStd(b->path.ref());
                  });
        auto dependsJson = ca::json::JsonValue::make_array();
        for (const SectionDep* depend : sortedDeps) dependsJson.append(dependJson(*depend));
        members.emplace_back(refOf("depends"), std::move(dependsJson));
        members.emplace_back(refOf("produced_by"),
                             ca::json::JsonValue::make_string(entry->producedBy.ref()));
        members.emplace_back(refOf("created_at"),
                             ca::json::JsonValue::make_string(entry->createdAt.ref()));
        members.emplace_back(refOf("updated_at"),
                             ca::json::JsonValue::make_string(entry->updatedAt.ref()));
        sectionsJson.append(ca::json::JsonValue::make_object_from(std::move(members)));
    }
    rootMembers.emplace_back(refOf("sections"), std::move(sectionsJson));

    ca::json::JsonDocument document;
    document.root() = ca::json::JsonValue::make_object_from(std::move(rootMembers));

    ca::json::JsonWriterOptions options;
    options.pretty = true;
    options.indent = 2;
    return ca::Ok(ca::json::JsonWriter::write(document, options));
}

//-- section 内容读写原语（C2：§5.1/§5.2/§8.1 的 section 层切片）----------------

namespace {

/// 当前进程 pid（txn 目录名单调，§8.1-1：txn-<pid>-<mono>，无 uuid 依赖）。
unsigned long currentPid()
{
#ifdef _WIN32
    return static_cast<unsigned long>(::_getpid());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

/// 精确字节 SHA-256 → "sha256:" + 64 hex（§4.2 content_hash 口径；小写
/// canonical，get_hash 之上防御性归一，保证清单形态稳定）。
Utf8String sha256HexOf(const ca::u8* data, ca::usize size)
{
    ca::crypto::SHA256 digest;
    if (size > 0) digest.add(data, static_cast<std::size_t>(size));
    std::string hex = digest.get_hash();
    for (char& c : hex) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return copyOf("sha256:" + hex);
}

/// hash 诊断前缀（§8.3：失配消息含期望/实际 hash 前缀，不全量铺开）。
std::string hashPrefix(const Utf8StringRef& hash)
{
    const std::string full = toStd(hash);
    return full.size() <= 7 + 16 ? full : full.substr(0, 7 + 16) + "...";
}

/// 提交换名（§8.1：撞 Windows 读者句柄重试 3 次 × 50ms 退避，耗尽 = Io）。
/// `what` 供错误消息命名提交对象（section 文件 / store manifest），重试
/// 口径全引擎一处（C2 section 原语与 C3 manifest 事务共用）。
ca::Result<void, ProjectStoreError> renameWithRetry(const std::string& from,
                                                    const std::string& to, const char* what)
{
    std::error_code ec;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ec.clear();
        std::filesystem::rename(nativePath(from), nativePath(to), ec);
        if (!ec) return ca::Ok();
    }
    return ca::Err(makeError(ProjectStoreErrorKind::Io,
                             std::string("failed to publish ") + what + ": rename '" + from +
                                 "' -> '" + to + "' still failing after 3 attempts: " +
                                 utf8SafeSystemText(ec.message())));
}

/// 读入 + 校验公共主体（§5.2：先验 byte_size 再验 content_hash；§7.3 单次
/// 读入、不持久持句柄）。显式文件名变体：常规路径按清单条目 format→ext 映射
/// 定文件名；恢复扫描对收容条目（format 值不识别，§9.3）按词根匹配到的实际
/// 文件名校验——收容段的真实扩展名本引擎不识别，不能拿自身映射找文件。
ca::Result<Utf8String, ProjectStoreError> readVerifiedSectionAt(const std::string& bundleDir,
                                                                const StoreSectionEntry& entry,
                                                                const std::string& fileName)
{
    const std::string path = ca::fs::PathUtil::join(
        ca::fs::PathUtil::join(bundleDir, kProjectStoreSectionsDirName), fileName);
    if (!ca::fs::FileUtil::exists(path)) {
        // §8.1 崩溃表：文件缺失（removeSection 墓碑中途/半提交遗留）与真损坏
        // 同落"→ Damaged"一条路径，判定口径不区分二者（R45/P3）。
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Section, entry.name.ref(),
            "section content missing: '" + path + "' (registered byte_size " +
                std::to_string(entry.byteSize) + ", content_hash " +
                hashPrefix(entry.contentHash.ref()) +
                "; treat as Damaged, rewrite the section to recover)"));
    }
    auto content = ca::fs::FileUtil::read_all_bytes(path);
    if (content.is_err()) {
        auto       fsError = std::move(content).unwrap_err();
        const auto message =
            "failed to read section file '" + path + "': " + ca::fs::to_string(fsError);
        return ca::Err(makeError(ProjectStoreErrorKind::Io, message));
    }
    auto bytes = std::move(content).unwrap();
    if (bytes.len() != entry.byteSize) {
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Section, entry.name.ref(),
            "section byte_size mismatch for '" + toStd(entry.name.ref()) + "': expected " +
                std::to_string(entry.byteSize) + ", got " + std::to_string(bytes.len()) +
                " (registered content_hash " + hashPrefix(entry.contentHash.ref()) + ")"));
    }
    const Utf8String actual = sha256HexOf(bytes.as_ptr(), bytes.len());
    if (!actual.ref().equals(entry.contentHash.ref())) {
        return ca::Err(makeSectionError(
            ProjectStoreErrorKind::Section, entry.name.ref(),
            "section content_hash mismatch for '" + toStd(entry.name.ref()) + "': expected " +
                hashPrefix(entry.contentHash.ref()) + ", actual " + hashPrefix(actual.ref())));
    }
    // §6.2 readBytes 的设计返回类型是 Utf8String，此处仅作字节容器使用：
    // opaque section 是"任意字节原样"（§5.1），故走 unchecked 构造、不做
    // UTF-8 校验（字节正确性刚由 content_hash 证明，格式合法性归各格式读路径）。
    Utf8String out = Utf8String::from_data_unchecked(bytes.as_ptr(), bytes.len());
    return ca::Ok(std::move(out));
}

/// 常规入口：按清单条目 format→ext 映射定文件名（§3.3）；收容条目走
/// readVerifiedSectionAt 的显式文件名变体（恢复扫描词根匹配，§9.3）。
ca::Result<Utf8String, ProjectStoreError> readVerifiedSection(const std::string& bundleDir,
                                                              const StoreSectionEntry& entry)
{
    return readVerifiedSectionAt(bundleDir, entry,
                                 sectionFileName(entry.name.ref(), entry.format).to_std_string());
}

}   // namespace

ca::Result<SectionWriteResult, ProjectStoreError> writeSectionBytes(
    const Utf8StringRef& bundleDir, const Utf8StringRef& name, SectionFormat format,
    const ca::u8* data, ca::usize size)
{
    // 触盘前先拒绝非法名（§3.3：Schema 错不静默改写；零半步残迹）。
    auto nameValidated = validateSectionName(name);
    if (nameValidated.is_err()) return ca::Err(std::move(nameValidated).unwrap_err());
    if (size > 0 && data == nullptr) {
        return ca::Err(makeSectionError(ProjectStoreErrorKind::Schema, name,
                                        "writeSectionBytes: null data with nonzero size"));
    }

    const std::string dirArg      = toStd(bundleDir);
    const std::string fileName    = sectionFileName(name, format).to_std_string();
    const std::string sectionsDir = ca::fs::PathUtil::join(dirArg, kProjectStoreSectionsDirName);
    const std::string targetPath  = ca::fs::PathUtil::join(sectionsDir, fileName);

    // §8.1-1 staged：tmp/txn-<pid>-<mono>/，恒在 bundle 内（同卷 MUST，R45/P6）。
    const auto  mono   = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string txnDir = ca::fs::PathUtil::join(
        ca::fs::PathUtil::join(dirArg, kProjectStoreTmpDirName),
        "txn-" + std::to_string(currentPid()) + "-" + std::to_string(mono));
    std::error_code ec;
    if (std::filesystem::exists(nativePath(txnDir), ec) && !ec) {
        // pid+mono 跨 run 理论撞名：同名 txn 必为陈旧残留（steady 时钟单调、
        // 本进程不可能持有同刻活 txn），清之（§8.1 重试"清扫旧 txn"同款）。
        ec.clear();
        std::filesystem::remove_all(nativePath(txnDir), ec);
        if (ec) {
            return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                     "failed to clear stale staging directory '" + txnDir +
                                         "': " + utf8SafeSystemText(ec.message())));
        }
    }
    const std::string stagedSections =
        ca::fs::PathUtil::join(txnDir, kProjectStoreSectionsDirName);
    ec.clear();
    std::filesystem::create_directories(nativePath(stagedSections), ec);
    if (ec) {
        return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                 "failed to create staging directory '" + stagedSections +
                                     "': " + utf8SafeSystemText(ec.message())));
    }
    // 目标 sections/ 缺失则补齐（幂等；正常 bundle 已有）。
    ec.clear();
    std::filesystem::create_directories(nativePath(sectionsDir), ec);
    if (ec) {
        return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                 "failed to ensure sections directory '" + sectionsDir +
                                     "': " + utf8SafeSystemText(ec.message())));
    }

    // §8.1-2 写 staged 字节（精确透传，无编码/序列化变换）。
    const std::string stagedPath = ca::fs::PathUtil::join(stagedSections, fileName);
    auto written = ca::fs::FileUtil::write_bytes(stagedPath, ca::core::ByteSlice(data, size));
    if (written.is_err()) {
        auto       fsError = std::move(written).unwrap_err();
        const auto message = "failed to stage section bytes for '" + toStd(name) +
                             "': " + ca::fs::to_string(fsError);
        std::error_code cleanupEc;
        std::filesystem::remove_all(nativePath(txnDir), cleanupEc);
        return ca::Err(makeError(ProjectStoreErrorKind::Io, message));
    }

    // §8.1-4 提交换名 = 唯一提交点；失败清扫本次 txn（重试不依赖 staged
    // 残留，R45/P2），sections/ 无残迹、旧字节完好。
    auto renamed = renameWithRetry(stagedPath, targetPath, "section file");
    std::error_code cleanupEc;
    std::filesystem::remove_all(nativePath(txnDir), cleanupEc);
    if (renamed.is_err()) return ca::Err(std::move(renamed).unwrap_err());

    // 双登记字段（§6.3/D3）：对发布的精确字节算 hash + size（写路径逐字节
    // 透传，内存口径 == 落盘口径）。manifest 登记归 C3。
    SectionWriteResult result;
    result.byteSize    = static_cast<ca::u64>(size);
    result.contentHash = sha256HexOf(data, size);
    return ca::Ok(std::move(result));
}

ca::Result<SectionWriteResult, ProjectStoreError> writeSectionBytes(
    const Utf8StringRef& bundleDir, const Utf8StringRef& name, SectionFormat format,
    const Utf8StringRef& bytes)
{
    return writeSectionBytes(bundleDir, name, format,
                             reinterpret_cast<const ca::u8*>(bytes.data()),
                             bytes.byte_length());
}

ca::Result<void, ProjectStoreError> verifySectionBytes(const Utf8StringRef& bundleDir,
                                                       const StoreSectionEntry& entry)
{
    auto verified = readVerifiedSection(toStd(bundleDir), entry);
    if (verified.is_err()) return ca::Err(std::move(verified).unwrap_err());
    return ca::Ok();
}

ca::Result<Utf8String, ProjectStoreError> readSectionBytes(const Utf8StringRef& bundleDir,
                                                           const StoreSectionEntry& entry)
{
    return readVerifiedSection(toStd(bundleDir), entry);
}

//-- store.json manifest 事务（C3：§6.3 flush 的 manifest 切片、§8.1 编排）------

namespace {

/// §8.1-1/6/7 的 manifest 切片：txn staged → 全量确定性字节落 staged → 单次
/// 换名（唯一提交点）→ 自清扫。三段式与 writeSectionBytes 同口径（txn 目录
/// 命名 txn-<pid>-<mono>、tmp 恒在 bundle 内同卷 MUST R45/P6、撞读者句柄
/// 重试 3×50ms 耗尽 = Io、只清自己的 txn——他人/开期残留归 open 期恢复扫描
/// §8.2-1 的动作面，scanBundleRecovery 只盘点不代清）。
ca::Result<void, ProjectStoreError> stageAndCommitManifest(const std::string& bundleDir,
                                                           const std::string& manifestText)
{
    const auto  mono   = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string txnDir = ca::fs::PathUtil::join(
        ca::fs::PathUtil::join(bundleDir, kProjectStoreTmpDirName),
        "txn-" + std::to_string(currentPid()) + "-" + std::to_string(mono));
    std::error_code ec;
    if (std::filesystem::exists(nativePath(txnDir), ec) && !ec) {
        // pid+mono 跨 run 理论撞名：同名 txn 必为陈旧残留，先清后建（C2 同款）。
        ec.clear();
        std::filesystem::remove_all(nativePath(txnDir), ec);
        if (ec) {
            return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                     "failed to clear stale staging directory '" + txnDir +
                                         "': " + utf8SafeSystemText(ec.message())));
        }
    }
    ec.clear();
    std::filesystem::create_directories(nativePath(txnDir), ec);
    if (ec) {
        return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                 "failed to create staging directory '" + txnDir +
                                     "': " + utf8SafeSystemText(ec.message())));
    }

    // §8.1-6 写 staged：store.json 重写出（全量、确定性）→ staged/store.json。
    const std::string stagedPath = ca::fs::PathUtil::join(txnDir, kProjectStoreManifestName);
    auto written = ca::fs::FileUtil::write_bytes(
        stagedPath, ca::core::ByteSlice(reinterpret_cast<const ca::u8*>(manifestText.data()),
                                        manifestText.size()));
    if (written.is_err()) {
        auto       fsError = std::move(written).unwrap_err();
        const auto message = "failed to stage manifest bytes: " + ca::fs::to_string(fsError);
        std::error_code cleanupEc;
        std::filesystem::remove_all(nativePath(txnDir), cleanupEc);
        return ca::Err(makeError(ProjectStoreErrorKind::Io, message));
    }

    // §8.1-6 单次换名 staged/store.json → <bundle>/store.json = 提交点。
    // 不走 bundle 根的 atomic_write_text：其同目录临时残留游离于 §8.2 恢复
    // 扫描之外（R45/P4）。失败清扫本次 txn——旧 store.json 完好，重试不依赖
    // staged 残留（R45/P2：重跑全序列时 staged 按同字节重新写出，幂等）。
    const std::string targetPath = ca::fs::PathUtil::join(bundleDir, kProjectStoreManifestName);
    auto renamed = renameWithRetry(stagedPath, targetPath, "store manifest");
    std::error_code cleanupEc;
    std::filesystem::remove_all(nativePath(txnDir), cleanupEc);
    if (renamed.is_err()) return ca::Err(std::move(renamed).unwrap_err());
    return ca::Ok();
}

}   // namespace

namespace {

/// 条目深拷贝（实体串 move-only，Utf8String 拷贝赋值已删除；事务以 const&
/// 接变更集、不消费调用方状态——flush 重试语义要求同一变更集可整笔重放）。
StoreSectionEntry cloneEntry(const StoreSectionEntry& entry)
{
    StoreSectionEntry copy;
    copy.name           = entry.name.clone();
    copy.format         = entry.format;
    copy.formatTag      = entry.formatTag.clone();
    copy.formatRetained = entry.formatRetained;
    copy.sectionVersion = entry.sectionVersion;
    copy.contentHash    = entry.contentHash.clone();
    copy.byteSize       = entry.byteSize;
    copy.hasRecordCount = entry.hasRecordCount;
    copy.recordCount    = entry.recordCount;
    if (entry.profile.has_value()) copy.profile = entry.profile->clone();
    copy.depends.reserve(entry.depends.size());
    for (const auto& depend : entry.depends) {
        SectionDep depCopy;
        depCopy.path        = depend.path.clone();
        depCopy.fingerprint = depend.fingerprint.clone();
        copy.depends.push_back(std::move(depCopy));
    }
    copy.producedBy = entry.producedBy.clone();
    copy.createdAt  = entry.createdAt.clone();
    copy.updatedAt  = entry.updatedAt.clone();
    return copy;
}

}   // namespace

ca::Result<StoreManifest, ProjectStoreError> commitStoreManifestTransaction(
    const Utf8StringRef& bundleDir, const StoreManifestMutation& mutation)
{
    const std::string dirArg = toStd(bundleDir);

    //-- 1. 读当前清单（§8.1 前置：清单是唯一真清单，事务从可载状态出发）--------
    //
    // CP-1 消费点（R52 处方；**C4 终裁已落地**）：isManifestUnloadableKind
    // 判定"清单整笔不可载"（Open ∪ 解析期 Schema 整笔拒载 ∪ Io 读取失败
    // ——C4 终裁把 Io 划入触发面：证明不了清单可载，就不得把现存 section
    // 文件定性为"可载场景半提交遗留"；宁可错判不可误 prune，裁定理由见
    // isManifestUnloadableKind 注释）。unloadable → 降级孤儿态，**拒绝
    // 事务**：若在该状态覆写 store.json，现存 section 文件会被新清单重注册
    // 为"清单可载"场景的 §8.2-4 孤儿（flush 后可自动 prune），P1 红线
    // （unloadable-manifest 孤儿永不自动 prune）即被绕过。错误 kind 原样
    // 上浮不吞并——终裁合流的是处置政策面，不改错误类别。
    auto current = readStoreManifestFile(bundleDir);
    if (current.is_err()) {
        auto        error = std::move(current).unwrap_err();
        const bool  unloadable = isManifestUnloadableKind(error.kind);
        const char* route =
            unloadable
                ? "manifest is unloadable (degraded-orphan state per design section 8.2-2; "
                  "CP-1 final ruling, C4: Open, parse-time Schema and Io read failures all "
                  "count - prefer false-unloadable over a wrong prune)"
                : "manifest error outside the unloadable surface (unexpected kind; fail-closed "
                  "refusal)";
        ca::str::Utf8StringBuilder builder;
        builder.append(copyOf("store manifest transaction refused: "))
               .append(route)
               .append("; store.json left untouched, unloadable-manifest orphans are never "
                       "auto-pruned (P1); original error: ")
               .append(error.message.ref());
        error.message = builder.build_or_empty();
        return ca::Err(std::move(error));
    }
    StoreManifest manifest = std::move(current).unwrap();

    //-- 2. 校验面（零触盘）：变更集自洽 ----------------------------------------
    std::set<std::string> upsertNames;
    for (const auto& entry : mutation.upserts) {
        const std::string name = toStd(entry.name.ref());
        // §6.3：一次 flush 周期内每名至多一个 writer。
        if (!upsertNames.insert(name).second) {
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, entry.name.ref(),
                "manifest transaction: duplicate upsert for section '" + name +
                    "' (at most one writer per name per flush)"));
        }
        // §9.3 只保留纪律：收容段（清单载有不识别 format）字节不可解释，
        // 不可改写——先显式 remove，再另行登记新段。
        auto existing = std::find_if(manifest.sections.begin(), manifest.sections.end(),
                                     [&entry](const StoreSectionEntry& registered) {
                                         return registered.name.equals(entry.name.ref());
                                     });
        if (existing != manifest.sections.end() && existing->formatRetained) {
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, entry.name.ref(),
                "manifest transaction: section '" + name +
                    "' is retained verbatim (unrecognized manifest format); it cannot be "
                    "rewritten in place - remove it explicitly first, then register anew"));
        }
    }
    std::set<std::string> removalNames;
    for (const auto& tombstone : mutation.removals) {
        if (upsertNames.count(toStd(tombstone.ref())) > 0) {
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, tombstone.ref(),
                "manifest transaction: section '" + toStd(tombstone.ref()) +
                    "' appears in both upserts and removals (contradictory mutation)"));
        }
        // 重复墓碑名 = 变更集自身含混（第二次删除无从谈起），前置拒绝。
        if (!removalNames.insert(toStd(tombstone.ref())).second) {
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, tombstone.ref(),
                "manifest transaction: duplicate removal for section '" +
                    toStd(tombstone.ref()) + "'"));
        }
        const bool registered =
            std::any_of(manifest.sections.begin(), manifest.sections.end(),
                        [&tombstone](const StoreSectionEntry& entry) {
                            return entry.name.equals(tombstone.ref());
                        });
        if (!registered) {
            // §6.3：对不存在名报 Schema 错。
            return ca::Err(makeSectionError(
                ProjectStoreErrorKind::Schema, tombstone.ref(),
                "manifest transaction: cannot remove unregistered section '" +
                    toStd(tombstone.ref()) + "' (unknown-name tombstone is a Schema error)"));
        }
    }

    //-- 3. upsert 与发布字节逐笔复核（§5.2 先 size 后 hash；Damaged 判定路径）--
    //
    // 挂接纪律（C3 消费 C2 原语）：登记条目的 content_hash/byte_size 来自
    // writeSectionBytes 的 SectionWriteResult；此处对 sections/ 发布文件复核，
    // 失配/缺失 = Damaged 判定（§8.3）当场拦截——清单是引擎自产封闭记录，
    // writer bug / 发布中断在 flush 当场抓住，不落成明日 Damaged。
    for (const auto& entry : mutation.upserts) {
        auto verified = verifySectionBytes(bundleDir, entry);
        if (verified.is_err()) {
            auto sectionError = std::move(verified).unwrap_err();
            ca::str::Utf8StringBuilder builder;
            builder.append(copyOf("manifest transaction: upsert registration does not match the "
                                  "published section bytes; "))
                   .append(sectionError.message.ref());
            sectionError.message = builder.build_or_empty();
            return ca::Err(std::move(sectionError));
        }
    }

    //-- 4. 删除墓碑文件（先删文件后提清单，§8.1 崩溃表：中途崩溃落 Damaged）----
    for (const auto& tombstone : mutation.removals) {
        auto existing = std::find_if(manifest.sections.begin(), manifest.sections.end(),
                                     [&tombstone](const StoreSectionEntry& entry) {
                                         return entry.name.equals(tombstone.ref());
                                     });
        // 删除幂等：文件已缺（此前中断/损坏形态）视为已删，条目照常摘除
        // （Damaged 段的显式删除是设计认可的恢复路径）；失败 = Io，中止于
        // 清单提交之前（旧清单完好）。
        const std::string sectionFile = ca::fs::PathUtil::join(
            ca::fs::PathUtil::join(dirArg, kProjectStoreSectionsDirName),
            sectionFileName(existing->name.ref(), existing->format).to_std_string());
        std::error_code ec;
        std::filesystem::remove(nativePath(sectionFile), ec);
        if (ec) {
            return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                     "manifest transaction: failed to remove section file '" +
                                         sectionFile + "': " + utf8SafeSystemText(ec.message())));
        }
        manifest.sections.erase(existing);
    }

    //-- 5. 应用变更（写即替换，§6.3）：同名整体替换，未登记追加 -----------------
    for (const auto& entry : mutation.upserts) {
        auto existing = std::find_if(manifest.sections.begin(), manifest.sections.end(),
                                     [&entry](const StoreSectionEntry& registered) {
                                         return registered.name.equals(entry.name.ref());
                                     });
        if (existing != manifest.sections.end()) {
            *existing = cloneEntry(entry);
        }
        else {
            manifest.sections.push_back(cloneEntry(entry));
        }
    }

    //-- 6. §8.1-6/7：全量确定性写出 → staged → 单次换名（提交点）→ 自清扫 -------
    auto text = writeStoreManifestText(manifest);
    if (text.is_err()) {
        auto error = std::move(text).unwrap_err();
        ca::str::Utf8StringBuilder builder;
        builder.append(copyOf("manifest transaction: ")).append(error.message.ref());
        error.message = builder.build_or_empty();
        return ca::Err(std::move(error));
    }
    auto committed = stageAndCommitManifest(dirArg, std::move(text).unwrap().to_std_string());
    if (committed.is_err()) return ca::Err(std::move(committed).unwrap_err());
    return ca::Ok(std::move(manifest));
}

//-- 恢复扫描（C4：§8.2 全量盘点分类账；只读——扫描零触盘）----------------------

namespace {

/// 目录盘点：普通文件名 + 非普通条目名（各自字典序）。目录缺失 = 存在标志
/// 假 + 空集（骨架期合法形态，不算失败）；枚举失败 = Io（分类账做不出来）。
struct DirectoryListing
{
    bool                     exists = false;
    std::vector<std::string> files;  ///< 普通文件名（字典序）
    std::vector<std::string> others; ///< 非普通条目（目录/符号链接等，字典序）
};

ca::Result<DirectoryListing, ProjectStoreError> listDirectoryEntries(const std::string& dirPath)
{
    DirectoryListing listing;
    std::error_code  ec;
    if (!std::filesystem::exists(nativePath(dirPath), ec)) {
        if (ec) {
            return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                     "recovery scan: cannot inspect directory '" + dirPath +
                                         "': " + utf8SafeSystemText(ec.message())));
        }
        return ca::Ok(std::move(listing));   // 缺失 = 空盘点（报告补诊断，不失败）
    }
    listing.exists = true;
    for (auto it = std::filesystem::directory_iterator(nativePath(dirPath), ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) {
            return ca::Err(makeError(ProjectStoreErrorKind::Io,
                                     "recovery scan: failed to enumerate directory '" + dirPath +
                                         "': " + utf8SafeSystemText(ec.message())));
        }
        std::error_code kindEc;
        const bool      regular = it->is_regular_file(kindEc) && !kindEc;
        const std::string name  = it->path().filename().u8string();
        if (regular) {
            listing.files.push_back(name);
        }
        else {
            listing.others.push_back(name);
        }
    }
    std::sort(listing.files.begin(), listing.files.end());
    std::sort(listing.others.begin(), listing.others.end());
    return ca::Ok(std::move(listing));
}

/// 引擎自产前缀的字面前缀比较（tmp/ 的 txn-* 盘点用；词根/登记名认领的
/// 大小写语义见 stemPrefixMatches/claimFileNameEquals——R62/N2 平台分叉）。
bool hasPrefix(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

/// ASCII 大小写折叠（R62/N2 平台分叉的实现基元）。词根与登记名都出自 §3.3
/// 名字语法（ASCII 小写集），文件名一侧可能携带人工引入的任意大小写；UTF-8
/// 的 ≥0x80 字节不受 ASCII 折叠影响，逐字节比较对多字节序列仍成立。
char asciiFoldChar(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// §9.3 收容词根匹配（R62/N2 平台分叉）：Windows 侧大小写不敏感（对齐
/// Win32/`\\?\` 路径的大小写不敏感语义——人工大小写变体文件不得绕过收容
/// 认领，也不得因大小写形态被误判清单外孤儿），POSIX 保持敏感（大小写敏感
/// 文件系统语义不变：变体名是不同文件，照常进孤儿面）。
bool stemPrefixMatches(const std::string& fileName, const std::string& stemPrefix)
{
    if (fileName.size() < stemPrefix.size()) return false;
#ifdef _WIN32
    for (std::size_t i = 0; i < stemPrefix.size(); ++i) {
        if (asciiFoldChar(fileName[i]) != asciiFoldChar(stemPrefix[i])) return false;
    }
    return true;
#else
    return fileName.compare(0, stemPrefix.size(), stemPrefix) == 0;
#endif
}

/// 登记文件名与现存文件名的认领比对（R62/N1 prune 现扫现验的认领面）：
/// Windows 大小写不敏感——认领处 exists() 本就按 Win32 语义大小写不敏感地
/// 解析，认领判定若只做精确串比较，已登记字节会以变体名误挂 prune 档；
/// POSIX 精确相等（变体名在大小写敏感文件系统上是不同文件）。
bool claimFileNameEquals(const std::string& candidate, const std::string& registeredName)
{
    if (candidate.size() != registeredName.size()) return false;
#ifdef _WIN32
    for (std::size_t i = 0; i < registeredName.size(); ++i) {
        if (asciiFoldChar(candidate[i]) != asciiFoldChar(registeredName[i])) return false;
    }
    return true;
#else
    return candidate == registeredName;
#endif
}

}   // namespace

ca::Result<StoreRecoveryReport, ProjectStoreError> scanBundleRecovery(
    const Utf8StringRef& bundleDir)
{
    const std::string dirArg        = toStd(bundleDir);
    const std::string sectionsDir   = ca::fs::PathUtil::join(dirArg, kProjectStoreSectionsDirName);
    const std::string tmpDir        = ca::fs::PathUtil::join(dirArg, kProjectStoreTmpDirName);

    StoreRecoveryReport report;

    //-- 1. 清单载入（§8.2-2/3 分流；CP-1 终裁口径）-----------------------------
    //
    // 读取/解析任何失败（Open/Schema/Io）同落 Unloadable——C4 终裁后触发面
    // 即 readStoreManifestFile 的全部失败类别，Loadable ⇔ 载入成功。kind 原样
    // 保留供诊断；政策面（孤儿恒 Never）按状态分流，不再按 kind 二次细分。
    StoreManifest manifest;   ///< Loadable 时持有载入结果（登记面分类的唯一依据）
    auto manifestLoaded = readStoreManifestFile(bundleDir);
    if (manifestLoaded.is_err()) {
        report.manifestError = std::move(manifestLoaded).unwrap_err();
        report.manifestState = ManifestLoadState::Unloadable;
        report.diagnostics.push_back(
            copyOf("recovery scan: store manifest unloadable (degraded-orphan state per design "
                   "section 8.2-2; section files are report-only, never auto-pruned): " +
                   report.manifestError.message.to_std_string()));
    }
    else {
        manifest = std::move(manifestLoaded).unwrap();
        for (const auto& diagnostic : manifest.diagnostics) {
            report.diagnostics.push_back(diagnostic.clone());
        }
        report.manifestState = ManifestLoadState::Loadable;
    }

    //-- 2. sections/ 盘点 -------------------------------------------------------
    auto sectionsListed = listDirectoryEntries(sectionsDir);
    if (sectionsListed.is_err()) return ca::Err(std::move(sectionsListed).unwrap_err());
    auto sectionsListing = std::move(sectionsListed).unwrap();
    if (!sectionsListing.exists) {
        report.diagnostics.push_back(copyOf(
            "recovery scan: sections/ directory missing under '" + dirArg +
            "' (bundle structure incomplete); nothing to inventory there"));
    }
    for (const auto& other : sectionsListing.others) {
        // 非普通文件不建行不触碰（§8.2 的盘点对象是 section 文件）。
        report.diagnostics.push_back(
            copyOf("recovery scan: non-regular entry '" + other +
                   "' under sections/ (not a section file); left untouched"));
    }

    //-- 3. 登记面逐条目分类（Loadable 才有登记面；§8.2-3）----------------------
    std::vector<const StoreSectionEntry*> entries;
    if (report.manifestState == ManifestLoadState::Loadable) {
        // 载入成功才有条目可分类；Unloadable 不做条目重建（宁孤儿不错报，§8.2-2）。
        entries.reserve(manifest.sections.size());
        for (const auto& entry : manifest.sections) entries.push_back(&entry);
    }
    std::sort(entries.begin(), entries.end(),
              [](const StoreSectionEntry* a, const StoreSectionEntry* b) {
                  return toStd(a->name.ref()) < toStd(b->name.ref());
              });

    // 已被登记面认领的文件（孤儿判定 = 现存全集 − 认领集）。登记文件即使判
    // Damaged 也属认领面——它不是"清单外文件"，绝不能混进 prune 档。
    std::set<std::string> claimedFiles;
    for (const StoreSectionEntry* entry : entries) {
        if (!entry->formatRetained) {
            const std::string fileName =
                sectionFileName(entry->name.ref(), entry->format).to_std_string();
            // R62/N1 病灶收口：认领处 exists 的 ec 不再忽略——检查失败（病态
            // 权限边界）= 不能证明"清单外"，按认领收编（宁可错认不可误 prune
            // 的同款 fail-closed：误收编的代价至多一行 Damaged 报告，误落
            // AfterFirstFlush 档的代价是登记字节可被自动删除）。诊断注明现症。
            std::error_code existsEc;
            const bool claimed = std::filesystem::exists(
                nativePath(ca::fs::PathUtil::join(sectionsDir, fileName)), existsEc);
            if (claimed || existsEc) {
                claimedFiles.insert(fileName);
            }
            if (existsEc) {
                report.diagnostics.push_back(copyOf(
                    "recovery scan: existence check failed for registered file '" + fileName +
                    "' under sections/ (" + utf8SafeSystemText(existsEc.message()) +
                    "); conservatively claimed (never prunable while unverifiable)"));
            }
            auto verified = readVerifiedSectionAt(dirArg, *entry, fileName);
            if (verified.is_err()) {
                report.damagedSections.push_back(cloneEntry(*entry));
                report.damagedReasons.push_back(std::move(verified).unwrap_err());
            }
            else {
                report.okSections.push_back(cloneEntry(*entry));
            }
            continue;
        }

        // 收容条目（§9.3）：真实扩展名本引擎不识别，按 `<name>.` 词根匹配
        // 现存文件——词根命中的文件永不落入 prune 档（宁可错认不可误删）。
        // R62/N2：匹配大小写语义按平台分叉（stemPrefixMatches——Windows
        // 不敏感对齐 Win32 路径语义，人工大小写变体文件照常被认领；POSIX
        // 保持敏感，变体名是不同文件照常进孤儿面）。
        const std::string stemPrefix = toStd(entry->name.ref()) + ".";
        std::vector<const std::string*> candidates;
        for (const auto& file : sectionsListing.files) {
            if (stemPrefixMatches(file, stemPrefix)) candidates.push_back(&file);
        }
        if (candidates.empty()) {
            report.damagedSections.push_back(cloneEntry(*entry));
            report.damagedReasons.push_back(makeSectionError(
                ProjectStoreErrorKind::Section, entry->name.ref(),
                "retained section has no matching file under sections/ (expected stem '" +
                    stemPrefix + "*'; registered byte_size " + std::to_string(entry->byteSize) +
                    ", content_hash " + hashPrefix(entry->contentHash.ref()) +
                    "; treat as Damaged)"));
            continue;
        }
        claimedFiles.insert(*candidates.front());
        auto verified = readVerifiedSectionAt(dirArg, *entry, *candidates.front());
        if (verified.is_err()) {
            report.damagedSections.push_back(cloneEntry(*entry));
            report.damagedReasons.push_back(std::move(verified).unwrap_err());
        }
        else {
            report.okSections.push_back(cloneEntry(*entry));
        }
        for (std::size_t i = 1; i < candidates.size(); ++i) {
            // 词根多匹配：首文件受验，其余按 Never 孤儿上报（可能是真收容字节
            // 的变体扩展名，引擎无法辨别 → 永不 prune）。已在此处置，同步认领，
            // 不再进入步骤 4 的一般孤儿面（否则会被二次计数并误挂 prune 档）。
            claimedFiles.insert(*candidates[i]);
            StoreOrphanRow extra;
            extra.fileName    = copyOf(*candidates[i]);
            extra.prunePolicy = OrphanPrunePolicy::Never;
            report.orphans.push_back(std::move(extra));
            report.diagnostics.push_back(
                copyOf("recovery scan: extra file '" + *candidates[i] +
                       "' matches retained section stem '" + stemPrefix +
                       "*'; report-only, never auto-pruned (cannot tell retained bytes apart)"));
        }
    }

    //-- 4. 孤儿面（§8.2-2/§8.2-4 两档；文件名字典序 = sectionsListing.files 序）-
    const OrphanPrunePolicy loadablePolicy =
        report.manifestState == ManifestLoadState::Loadable ? OrphanPrunePolicy::AfterFirstFlush
                                                            : OrphanPrunePolicy::Never;
    for (const auto& file : sectionsListing.files) {
        if (claimedFiles.count(file) > 0) continue;
        StoreOrphanRow row;
        row.fileName    = copyOf(file);
        row.prunePolicy = loadablePolicy;
        report.orphans.push_back(std::move(row));
        report.diagnostics.push_back(copyOf(
            std::string("recovery scan: orphan section file '") + file +
            "' under sections/: " +
            (loadablePolicy == OrphanPrunePolicy::AfterFirstFlush
                 ? std::string("prunable after first successful flush (design section 8.2-4)")
                 : std::string("never auto-pruned (unloadable manifest, design section 8.2-2; "
                               "P1 red line)"))));
    }
    // 步骤 3 的收容词根额外匹配行先于本循环入列——按文件名整列定序，保住
    // "同状态重扫逐字段一致"（报告头注）。
    std::sort(report.orphans.begin(), report.orphans.end(),
              [](const StoreOrphanRow& a, const StoreOrphanRow& b) {
                  return toStd(a.fileName.ref()) < toStd(b.fileName.ref());
              });

    //-- 5. Damaged 诊断行（条目保留、状态上屏，§8.2-3；恢复 = 同名重写，§8.3）--
    for (std::size_t i = 0; i < report.damagedSections.size(); ++i) {
        const auto& entry  = report.damagedSections[i];
        const auto& reason = report.damagedReasons[i];
        report.diagnostics.push_back(copyOf("recovery scan: damaged section '" +
                                            toStd(entry.name.ref()) + "': " +
                                            reason.message.to_std_string()));
    }

    //-- 6. tmp/ 盘点（§8.2-1 只盘点不清扫——清扫挂点 ProjectStore::open）--------
    auto tmpListed = listDirectoryEntries(tmpDir);
    if (tmpListed.is_err()) return ca::Err(std::move(tmpListed).unwrap_err());
    auto tmpListing = std::move(tmpListed).unwrap();
    std::vector<std::string> tmpNames = tmpListing.files;
    tmpNames.insert(tmpNames.end(), tmpListing.others.begin(), tmpListing.others.end());
    std::sort(tmpNames.begin(), tmpNames.end());
    for (const auto& name : tmpNames) {
        if (hasPrefix(name, "txn-")) {
            report.txnResidues.push_back(copyOf(name));
        }
        else {
            // 设计布局之外（tmp/ 只该有 txn-* 暂存）：只报告，扫描不触碰。
            report.diagnostics.push_back(copyOf(
                "recovery scan: unexpected entry '" + name + "' under tmp/ (layout defines "
                "only txn-* staging); left untouched"));
        }
    }

    return ca::Ok(std::move(report));
}

//-- flush 后孤儿 prune（R62/N1：现扫现验；动作语义见 hpp 块注）------------------

namespace {

/// 当前清单的认领面（现扫现验第 2 环的比对材料）：非收容条目的登记文件名
/// 集 + 收容条目的 `<name>.` 词根集，比对大小写语义按平台分叉（同 N2）。
struct FreshClaimSet
{
    std::vector<std::string> registeredNames; ///< 非收容条目的 `<name>.<ext>`
    std::vector<std::string> retainedStems;   ///< 收容条目的 `<name>.`

    bool claimed(const std::string& candidate) const
    {
        for (const auto& name : registeredNames) {
            if (claimFileNameEquals(candidate, name)) return true;
        }
        for (const auto& stem : retainedStems) {
            if (stemPrefixMatches(candidate, stem)) return true;
        }
        return false;
    }
};

FreshClaimSet freshClaimSetOf(const StoreManifest& manifest)
{
    FreshClaimSet claims;
    claims.registeredNames.reserve(manifest.sections.size());
    for (const auto& entry : manifest.sections) {
        if (entry.formatRetained) {
            claims.retainedStems.push_back(toStd(entry.name.ref()) + ".");
        }
        else {
            claims.registeredNames.push_back(
                sectionFileName(entry.name.ref(), entry.format).to_std_string());
        }
    }
    return claims;
}

/// 可读探针（现扫现验的"可读"半边）：只读句柄打开成功即证明当前权限边界下
/// 可读（不做全量读——孤儿字节可能任意大）；打开失败 = 病态权限边界的现症，
/// 带病不删。std::ifstream 的 path 重载在 Windows 走宽字符（UTF-8 路径安全）。
bool pathReadable(const std::string& path)
{
    std::ifstream probe(nativePath(path), std::ios::in | std::ios::binary);
    return probe.is_open();
}

}   // namespace

StorePruneReport pruneOrphanSections(const Utf8StringRef& bundleDir,
                                     const StoreRecoveryReport& report)
{
    StorePruneReport outcome;
    const std::string dirArg      = toStd(bundleDir);
    const std::string sectionsDir = ca::fs::PathUtil::join(dirArg, kProjectStoreSectionsDirName);

    //-- 工作清单：仅 AfterFirstFlush 档（Never 档零触碰：不进清单、不 stat、
    //    不诊断；清单为空时零动作返回，连 sections/ 的存在性都不探测）--------
    std::vector<const StoreOrphanRow*> work;
    for (const auto& row : report.orphans) {
        if (row.prunePolicy == OrphanPrunePolicy::AfterFirstFlush) work.push_back(&row);
    }
    std::sort(work.begin(), work.end(),
              [](const StoreOrphanRow* a, const StoreOrphanRow* b) {
                  return toStd(a->fileName.ref()) < toStd(b->fileName.ref());
              });
    if (work.empty()) return outcome;

    //-- 现验前置：sections/ 此刻可盘点（缺失/检查失败 = 无"已 prune 过"的可靠
    //    基态，全部跳过，不谎报幂等已删）-------------------------------------
    std::error_code sectionsEc;
    if (!std::filesystem::exists(nativePath(sectionsDir), sectionsEc)) {
        const std::string reason =
            sectionsEc ? utf8SafeSystemText(sectionsEc.message()) : "directory missing";
        for (const StoreOrphanRow* row : work) {
            const std::string path = ca::fs::PathUtil::join(sectionsDir, toStd(row->fileName.ref()));
            outcome.skipped.push_back(copyOf(row->fileName.ref()));
            outcome.diagnostics.push_back(copyOf(
                "orphan prune: skipped '" + path + "': sections/ unverifiable at prune time (" +
                reason + "); never prune on unverified state"));
        }
        return outcome;
    }

    //-- 现验第 1 环：清单此刻可读（读不出 = 零可删；P1 红线 + CP-1 终裁同款
    //    fail-closed：证明不了清单可载，就不得动任何 section 字节）------------
    auto manifestNow = readStoreManifestFile(bundleDir);
    if (manifestNow.is_err()) {
        const std::string reason = std::move(manifestNow).unwrap_err().message.to_std_string();
        for (const StoreOrphanRow* row : work) {
            const std::string path = ca::fs::PathUtil::join(sectionsDir, toStd(row->fileName.ref()));
            outcome.skipped.push_back(copyOf(row->fileName.ref()));
            outcome.diagnostics.push_back(copyOf(
                "orphan prune: skipped '" + path + "': manifest unreadable at prune time (" +
                reason + "); nothing prunable while the manifest cannot be read (P1 red line)"));
        }
        return outcome;
    }
    const FreshClaimSet claims = freshClaimSetOf(std::move(manifestNow).unwrap());

    for (const StoreOrphanRow* row : work) {
        const std::string fileName = toStd(row->fileName.ref());
        const std::string path     = ca::fs::PathUtil::join(sectionsDir, fileName);

        //-- 现验第 2 环：此刻仍不被当前清单认领（扫描到执行之间清单可能已变化；
        //    登记字节永不该自动删；认领面含收容词根，大小写语义同 N2）---------
        if (claims.claimed(fileName)) {
            outcome.skipped.push_back(copyOf(fileName));
            outcome.diagnostics.push_back(copyOf(
                "orphan prune: skipped '" + path +
                "': claimed by the current manifest now (registered bytes are never "
                "auto-pruned; the manifest may have changed since the scan)"));
            continue;
        }

        //-- 现验第 3 环：此刻存在、为普通文件、可读（R62 病灶的执行面收口）----
        // 探测必须走 status() 而非 exists()：两大家族标准库的 exists(p, ec)
        // 都把 ENOTDIR（路径穿普通文件）折叠成 not_found 并清 ec——病态路径
        // 形态会被谎报成"幂等已删"，违背"绝不带病删"红线。status() 面保留
        // errno，且两家族对 not-found 族都置 ec，故"干净已缺"与"病态探测"
        // 按 errc 值分类：no_such_file_or_directory（ENOENT / Win32 的
        // FILE_NOT_FOUND 与 PATH_NOT_FOUND 同映射此值）= 幂等已缺；
        // 其余 ec（ENOTDIR、EACCES 等）= 带病跳过。平台分叉恰好落回测试
        // 文档化的语义：POSIX 病态形态带病跳过，Win32 归一形态幂等已缺。
        std::error_code probeEc;
        const std::filesystem::file_status probeStatus =
            std::filesystem::status(nativePath(path), probeEc);

        //-- 3a. 干净已缺 = 幂等（not-found 族 errc；Win32 的 FILE/PATH_NOT_FOUND
        //      与 POSIX ENOENT 同映射 no_such_file_or_directory）--------------
        if (probeEc == std::errc::no_such_file_or_directory
            || (!probeEc && probeStatus.type() == std::filesystem::file_type::not_found)) {
            // 已缺 = 此前中断/已被处置的幂等形态："文件已缺视为已删"的
            // 既有设计口径；诊断逐笔注明，不与真实删除混报。
            outcome.pruned.push_back(copyOf(fileName));
            outcome.diagnostics.push_back(copyOf(
                "orphan prune: '" + path +
                "' already absent; counted as pruned (idempotent, prior-interruption form)"));
            continue;
        }

        //-- 3b. 带病探测（ENOTDIR / EACCES 等）= 绝不带病删 ------------------
        if (probeEc) {
            outcome.skipped.push_back(copyOf(fileName));
            outcome.diagnostics.push_back(copyOf(
                "orphan prune: skipped '" + path + "': existence check failed (" +
                utf8SafeSystemText(probeEc.message()) + "); never prune on unverified state"));
            continue;
        }

        //-- 3c. 非普通文件（目录等）= 永不触碰 -------------------------------
        if (probeStatus.type() != std::filesystem::file_type::regular) {
            outcome.skipped.push_back(copyOf(fileName));
            outcome.diagnostics.push_back(copyOf(
                "orphan prune: skipped '" + path +
                "': not a regular file at prune time; non-regular entries are never "
                "touched (design section 8.2)"));
            continue;
        }
        if (!pathReadable(path)) {
            outcome.skipped.push_back(copyOf(fileName));
            outcome.diagnostics.push_back(copyOf(
                "orphan prune: skipped '" + path +
                "': not readable at prune time (pathological permission boundary symptom); "
                "never prune a file that cannot be read"));
            continue;
        }

        //-- 动作：删除（remove 的 ec 上浮为跳过诊断，绝不带病删）---------------
        std::error_code removeEc;
        std::filesystem::remove(nativePath(path), removeEc);
        if (removeEc) {
            outcome.skipped.push_back(copyOf(fileName));
            outcome.diagnostics.push_back(copyOf(
                "orphan prune: skipped '" + path + "': remove failed (" +
                utf8SafeSystemText(removeEc.message()) + "); file left in place"));
            continue;
        }
        outcome.pruned.push_back(copyOf(fileName));
    }
    return outcome;
}

//-- 单写者锁（C5：§7.1 全路径 + §7.2 PID fail-closed 过期判定）------------------

namespace {

/// RFC 3339 UTC 时间戳（daemon_protocol.cpp mjtraceUtcNow 同款口径）。仅诊断
/// 用——§7.2 明言 v1 不做时间戳/启动时间启发式，created_at 只是锁的人读上下文。
std::string rfc3339UtcNow()
{
    std::time_t now = std::time(nullptr);
    std::tm     utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

/// owner 串的最小 JSON 转义（引号/反斜杠/控制字符）——owner 是调用方给定串，
/// 不得能破坏锁文件的单行 JSON 形状。
std::string jsonEscape(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            }
            else {
                out += c;
            }
        }
    }
    return out;
}

std::string lockFilePathOf(const std::string& bundleDir)
{
    return ca::fs::PathUtil::join(bundleDir, kProjectStoreLockName);
}

/// 锁文件内容（§7.1 单行 JSON：pid/created_at/owner；尾随 LF 供 POSIX 文本
/// 工具友好，读取方容忍）。
std::string buildLockContent(ca::i64 pid, const std::string& createdAt, const std::string& owner)
{
    return "{\"pid\":" + std::to_string(pid) + ",\"created_at\":\"" + jsonEscape(createdAt) +
           "\",\"owner\":\"" + jsonEscape(owner) + "\"}\n";
}

/// 读锁文件字节（失败 = 无值；调用方按各自路径裁决，不在此吞错）。
std::optional<std::string> readLockBytes(const std::string& path)
{
    auto content = ca::fs::FileUtil::read_all_bytes(path);
    if (content.is_err()) return std::nullopt;
    auto bytes = std::move(content).unwrap();
    return std::string(reinterpret_cast<const char*>(bytes.as_ptr()),
                       static_cast<std::size_t>(bytes.len()));
}

/// 锁 owner 记录（store.lock 的解析产物；§7.1 三字段）。
struct LockOwnerRecord
{
    ca::i64     pid       = 0;
    std::string createdAt;
    std::string owner;
    bool        valid = false; ///< 三字段齐备且形状合法
};

/// 解析锁 owner（R45/P5 fail-closed 口径）：pid 必须是正整数、created_at/
/// owner 必须是非空字符串——任一不备即 invalid（"读不出 owner 的锁"禁止自动
/// 接管）。未知键忽略（演进宽容），重复键 invalid（同 ca::json 解析纪律）。
LockOwnerRecord parseLockOwner(const std::string& bytes)
{
    LockOwnerRecord record;
    auto document = ca::json::JsonReader::read(Utf8StringRef::from_string_view(
        std::string_view(bytes.data(), bytes.size())));
    if (document.is_err()) return record;
    auto        parsedDocument = std::move(document).unwrap();
    const auto& root           = parsedDocument.root();
    if (!root.is_object()) return record;

    const ca::json::JsonValue* pid       = nullptr;
    const ca::json::JsonValue* createdAt = nullptr;
    const ca::json::JsonValue* owner     = nullptr;
    for (const auto& [key, value] : root.as_object()) {
        const ca::json::JsonValue** slot = nullptr;
        if (key.equals(refOf("pid"))) {
            slot = &pid;
        }
        else if (key.equals(refOf("created_at"))) {
            slot = &createdAt;
        }
        else if (key.equals(refOf("owner"))) {
            slot = &owner;
        }
        else {
            continue;   // 未知键忽略：三字段合同仍按 fail-closed 把关
        }
        if (*slot != nullptr) return record;   // duplicate key → invalid
        *slot = &value;
    }
    if (pid == nullptr || createdAt == nullptr || owner == nullptr) return record;
    if (!pid->is_int() || pid->as_int() <= 0) return record;
    if (!createdAt->is_string() || createdAt->as_string().is_empty()) return record;
    if (!owner->is_string() || owner->as_string().is_empty()) return record;
    record.pid       = pid->as_int();
    record.createdAt = toStd(createdAt->as_string());
    record.owner     = toStd(owner->as_string());
    record.valid     = true;
    return record;
}

/// owner 三元组的人读形态（§7.1 Lock 错消息与接管/抢占诊断共用一处措辞）。
std::string ownerSuffix(const LockOwnerRecord& record)
{
    return "pid=" + std::to_string(record.pid) + ", created_at='" + record.createdAt +
           "', owner='" + record.owner + "'";
}

/// R45/P5：接管失败的消息重读当前锁文件报新赢家（竞态赢家此时已写入新内容，
/// 报告它而非旧值）。重读失败/不可解析时如实注明（仍属 fail-closed 面）。
std::string currentWinnerSuffix(const std::string& lockPath)
{
    auto bytes = readLockBytes(lockPath);
    if (!bytes.has_value()) {
        return "current lock owner could not be re-read (unreadable)";
    }
    const auto record = parseLockOwner(*bytes);
    if (!record.valid) {
        return "current lock owner re-read but unparseable (treated as live, fail-closed)";
    }
    return "current lock winner (re-read): " + ownerSuffix(record);
}

/// 不可解析锁的原文前缀（诊断定位用；控制字符/非 ASCII 折叠 '?'，限 48 字节）。
std::string rawPrefixForDiagnostic(const std::string& bytes)
{
    std::string prefix;
    prefix.reserve(std::min<std::size_t>(bytes.size(), 48));
    for (std::size_t i = 0; i < bytes.size() && i < 48; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        prefix.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    }
    if (bytes.size() > 48) prefix += "...";
    return prefix;
}

#if !defined(_WIN32)
/// POSIX 侧属主核验（R64-N1，与 Windows 的内容逐字节比对对称）：句柄所指
/// inode（fstat）与路径现指 inode（stat）比对 st_dev/st_ino——一致 = 路径上
/// 仍是本人独占创建的那个文件；不一致 = 名字已被并发 stealLock 抢占者 unlink
/// 后换成新 inode，绝不删（防误伤抢占者，半截字节无从内容比对时这是唯一
/// 可用基准）；路径已消失（ENOENT 等）= 无可删之物。句柄侧 fstat 失败属
/// 未知态，按"不是我们的"保守不删（fail-closed 同向：错不删好过错删）。
enum class PosixLockOwnership
{
    Ours,      ///< 路径现指 inode 与本人句柄一致，可删
    Replaced,  ///< 名字已被换新 inode（或句柄态未知）——绝不删
    Gone,      ///< 路径已消失——无可删之物
};

PosixLockOwnership posixLockOwnershipAtPath(std::FILE* handle, const std::string& path)
{
    struct stat handleStat = {};
    struct stat pathStat   = {};
    if (::fstat(::fileno(handle), &handleStat) != 0) {
        return PosixLockOwnership::Replaced;
    }
    if (::stat(path.c_str(), &pathStat) != 0) {
        return PosixLockOwnership::Gone;
    }
    return (handleStat.st_dev == pathStat.st_dev && handleStat.st_ino == pathStat.st_ino)
               ? PosixLockOwnership::Ours
               : PosixLockOwnership::Replaced;
}
#endif

/// 独占创建的结果分类。
enum class LockCreateOutcome
{
    Acquired, ///< 独占创建 + 写内容 + fflush 成功（句柄交调用方持有）
    Exists,   ///< 已存在（O_EXCL 冲突）——§7.1 失败路径入口
    Failed,   ///< 其余创建/写失败（真实文件系统失败）——Io 面
};

/// 独占创建 + 写内容 + fflush（§7.1 获取序：独占创建 = O_EXCL 语义，§6.4
/// 处方"纯 CRT 无平台分支"——POSIX `fopen("wbx")`、Windows `_wfsopen(L"wbx",
/// _SH_DENYNO)`（fopen 同一 mode 解析器，'x' → _O_EXCL；走宽路径保 UTF-8
/// 目录名的正确性，与 nativePath/u8path 的全库路径纪律一致）。写失败不留
/// 半截锁——尽力删除后报 Failed。
LockCreateOutcome tryCreateLock(const std::string& path, const std::string& content,
                                std::FILE** outHandle)
{
    *outHandle = nullptr;
#ifdef _WIN32
    const std::wstring widePath = nativePath(path).wstring();
    std::FILE*         handle   = ::_wfsopen(widePath.c_str(), L"wbx", _SH_DENYNO);
#else
    std::FILE* handle = std::fopen(path.c_str(), "wbx");
#endif
    if (handle == nullptr) {
        return errno == EEXIST ? LockCreateOutcome::Exists : LockCreateOutcome::Failed;
    }
    const std::size_t written = std::fwrite(content.data(), 1, content.size(), handle);
    if (written != content.size() || std::fflush(handle) != 0) {
#ifdef _WIN32
        std::fclose(handle);
        std::error_code removeEc;
        std::filesystem::remove(nativePath(path), removeEc);   // 半截锁不留盘（尽力而为）
#else
        // 写失败清场同样属主核验（R64 附注顺带收口）：半截字节无从内容比对，
        // inode 比对是唯一基准——独占创建到清场之间名字已被并发抢占者换成
        // 新 inode 时绝不删（此时本人旧 inode 已被抢占者 unlink，孤儿随 close
        // 释放，路径上不留半截锁）；名字已消失同样无可清。
        const PosixLockOwnership ownership = posixLockOwnershipAtPath(handle, path);
        std::fclose(handle);
        if (ownership == PosixLockOwnership::Ours) {
            std::error_code removeEc;
            std::filesystem::remove(nativePath(path), removeEc);   // 半截锁不留盘（尽力而为）
        }
#endif
        return LockCreateOutcome::Failed;
    }
    *outHandle = handle;
    return LockCreateOutcome::Acquired;
}

/// Lock 错消息的固定尾巴（§7.1 IDA "database is locked by" 同款体验的定位面）。
ProjectStoreError lockError(const std::string& message)
{
    return makeError(ProjectStoreErrorKind::Lock, message);
}

/// errno 的人读文案（自写最小映射：strerror 的 MSVC C4996/POSIX 变体双平台
/// 收口；文案恒 ASCII，未知码回落数字——诊断定位靠路径与上下文）。
std::string errnoText(int error)
{
    switch (error) {
    case EEXIST: return "file already exists";
    case EACCES: return "access denied";
    case ENOENT: return "no such file or directory";
    case EIO:    return "i/o error";
    case ENOSPC: return "no space left on device";
    default:     return "errno=" + std::to_string(error);
    }
}

}   // namespace

bool storeLockPidAlive(const ca::i64 pid)
{
#ifdef _WIN32
    if (pid <= 0 || pid > static_cast<ca::i64>(0xFFFFFFFFu)) return false;
    // daemon_state.cpp:348-368 纪律平移：用最小特权查询权，保护/高完整性进程
    // 也能探；只有"未知 pid"证明进程已死，其余失败（ACCESS_DENIED 或瞬时错）
    // 必须报活——证明不了死就不得动它的锁（§7.2）。
    const HANDLE process =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return ::GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD      exitCode = 0;
    const bool queried  = ::GetExitCodeProcess(process, &exitCode) != 0;
    ::CloseHandle(process);
    // STILL_ACTIVE = 运行中；查询失败同样证明不了死 → 报活。
    return !queried || exitCode == STILL_ACTIVE;
#else
    if (pid <= 0 || pid > static_cast<ca::i64>(std::numeric_limits<pid_t>::max())) return false;
    // ESRCH = 死；EPERM（活但无权限）与其他错误一律活（fail-closed，§7.2）。
    return ::kill(static_cast<pid_t>(pid), 0) != -1 || errno != ESRCH;
#endif
}

StoreLock::~StoreLock()
{
    release();
}

StoreLock::StoreLock(StoreLock&& other) noexcept
    : handle_(other.handle_), lockFilePath_(std::move(other.lockFilePath_)),
      writtenContent_(std::move(other.writtenContent_)),
      diagnostics_(std::move(other.diagnostics_))
{
    other.handle_ = nullptr;
}

StoreLock& StoreLock::operator=(StoreLock&& other) noexcept
{
    if (this != &other) {
        release();
        handle_         = other.handle_;
        lockFilePath_   = std::move(other.lockFilePath_);
        writtenContent_ = std::move(other.writtenContent_);
        diagnostics_    = std::move(other.diagnostics_);
        other.handle_   = nullptr;
    }
    return *this;
}

void StoreLock::release()
{
    if (handle_ == nullptr) return;
    const std::string lockPath = toStd(lockFilePath_.ref());
#ifdef _WIN32
    // Windows：自身独占句柄（CRT fopen 无 FILE_SHARE_DELETE）在持期间阻止
    // 删除——必须先关句柄再删文件。close→remove 窗口内可能出现 stealLock
    // 抢占者的新锁：删除前重读并逐字节比对本人写入内容，不是本人的字节绝不删
    // （防误伤抢占者）；删不掉/已换人转诊断不外抛（析构纪律，§7.1 释放面）。
    std::fclose(handle_);
    handle_ = nullptr;
    auto    current = readLockBytes(lockPath);
    if (current.has_value() && *current == toStd(writtenContent_.ref())) {
        std::error_code ec;
        std::filesystem::remove(nativePath(lockPath), ec);
        if (ec) {
            diagnostics_.push_back(copyOf(
                "store.lock release: file remove failed after handle close ('" + lockPath +
                "'): " + utf8SafeSystemText(ec.message()) +
                "; lock file left in place (still ours by content)"));
        }
    }
    else if (current.has_value()) {
        diagnostics_.push_back(copyOf(
            "store.lock release: file content no longer ours (replaced after handle close); "
            "left untouched"));
    }
    else {
        diagnostics_.push_back(
            copyOf("store.lock release: file already gone at handle close ('" + lockPath + "')"));
    }
#else
    // POSIX：先删名后关句柄（unlink 语义——名字消失后竞争者的独占创建落成
    // 新 inode，本句柄 close 只释放旧 inode；教科书安全序）。删除前属主核验
    // （R64-N1，与 Windows 内容比对对称）：fstat 句柄 vs stat 路径比对 inode，
    // 名字已被并发 stealLock 抢占者换成新 inode 时绝不删（防误伤抢占者）；
    // 删除失败/不是本人/已消失一律转诊断（析构纪律，§7.1 释放面）。
    const PosixLockOwnership ownership = posixLockOwnershipAtPath(handle_, lockPath);
    if (ownership == PosixLockOwnership::Ours) {
        std::error_code ec;
        std::filesystem::remove(nativePath(lockPath), ec);
        if (ec) {
            diagnostics_.push_back(copyOf("store.lock release: file remove failed ('" + lockPath +
                                          "'): " + utf8SafeSystemText(ec.message())));
        }
    }
    else if (ownership == PosixLockOwnership::Replaced) {
        diagnostics_.push_back(copyOf(
            "store.lock release: file no longer ours (replaced by a concurrent preemptor); "
            "left untouched"));
    }
    else {
        diagnostics_.push_back(
            copyOf("store.lock release: file already gone at handle close ('" + lockPath + "')"));
    }
    std::fclose(handle_);
    handle_ = nullptr;
#endif
}

ca::Result<StoreLock, ProjectStoreError> acquireStoreLock(const Utf8StringRef& bundleDir,
                                                          const StoreLockOptions& options,
                                                          const char*             owner)
{
    const std::string dirArg   = toStd(bundleDir);
    const std::string lockPath = lockFilePathOf(dirArg);
    const std::string content =
        buildLockContent(static_cast<ca::i64>(currentPid()), rfc3339UtcNow(),
                         owner != nullptr ? owner : kProjectStoreLockOwnerDefault);

    //-- 获取：独占创建（O_EXCL）→ 写内容 → fflush（句柄此后持有到 close）------
    std::FILE*        handle = nullptr;
    const LockCreateOutcome first = tryCreateLock(lockPath, content, &handle);
    if (first == LockCreateOutcome::Acquired) {
        StoreLock lock;
        lock.handle_         = handle;
        lock.lockFilePath_   = copyOf(lockPath);
        lock.writtenContent_ = copyOf(content);
        return ca::Ok(std::move(lock));
    }
    if (first == LockCreateOutcome::Failed) {
        // 真实文件系统失败（非 O_EXCL 冲突）：不删任何东西，原样上浮。
        return ca::Err(makeError(
            ProjectStoreErrorKind::Io,
            "failed to create store.lock '" + lockPath + "': " + errnoText(errno)));
    }

    //-- 失败路径（已存在）：读 owner → fail-closed 裁决 → 接管/拒绝 -----------
    auto existingBytes = readLockBytes(lockPath);
    const LockOwnerRecord record =
        existingBytes.has_value() ? parseLockOwner(*existingBytes) : LockOwnerRecord{};
    if (!existingBytes.has_value() && !options.stealLock) {
        // 读不出 owner = 按"活锁" fail-closed（R45/P5：独占创建成功与内容写完
        // 之间的崩溃窗会留下空/半截 lock 文件；读不出就不得动它）。stealLock
        // 的人工决策出口与下方不可解析支同门放行（R64-N2：fail-closed 只禁
        // 自动接管，不禁人工抢占；此分支被吞掉 stealLock 出口会形成"已用
        // stealLock 仍被指去 stealLock"的死循环指引）。
        return ca::Err(lockError(
            "store.lock '" + lockPath +
            "' is present but unreadable (treated as a LIVE lock, fail-closed per R45/P5: "
            "a crash between exclusive-create and content-write leaves empty/half-written "
            "lock files; auto-takeover refused, message: unparseable); use "
            "OpenOptions::stealLock to preempt explicitly"));
    }
    if (!record.valid && !options.stealLock) {
        // 不可解析 = 按活锁 fail-closed（R45/P5）：注明 unparseable + 指路
        // stealLock，禁止自动接管。
        return ca::Err(lockError(
            "store.lock '" + lockPath +
            "' is unparseable (treated as a LIVE lock, fail-closed per R45/P5; auto-takeover "
            "refused); use OpenOptions::stealLock to preempt explicitly; raw prefix: \"" +
            rawPrefixForDiagnostic(*existingBytes) + "\""));
    }
    if (!options.stealLock && storeLockPidAlive(record.pid)) {
        // 活 = Lock 错（消息含 pid/created_at/owner，§7.1）。
        return ca::Err(lockError("bundle is locked by another process: '" + lockPath +
                                 "'; " + ownerSuffix(record) +
                                 " (single-writer lock, design section 7.1; takeover refused "
                                 "because the owner process is alive per the fail-closed PID "
                                 "probe, section 7.2)"));
    }

    //-- 接管（死锁接管 / stealLock 抢占）：删旧 → 重试独占创建恰好一次 ---------
    // 接管面（stealLock 可携带不可解析/读不出的旧锁）统一的旧 owner 措辞：
    // 可解析报全文；可读但不可解析注明原文前缀；读不出如实注明（绝不虚构
    // pid=0 之类的占位三元组）。
    const std::string previousOwnerText =
        record.valid
            ? ownerSuffix(record)
            : (existingBytes.has_value()
                   ? "unreadable/unparseable content (raw prefix: \"" +
                         rawPrefixForDiagnostic(*existingBytes) + "\")"
                   : "unreadable content (lock bytes could not be read)");
    std::error_code ec;
    std::filesystem::remove(nativePath(lockPath), ec);
    if (ec) {
        return ca::Err(makeError(
            ProjectStoreErrorKind::Io,
            "failed to remove store.lock '" + lockPath +
                "' of the " + (options.stealLock ? "current" : "proven-dead") + " owner (" +
                previousOwnerText + ") during takeover: " + utf8SafeSystemText(ec.message()) +
                " (fail-closed: no destructive bypass, no retry loop)"));
    }
    std::FILE*        retryHandle = nullptr;
    const LockCreateOutcome retry = tryCreateLock(lockPath, content, &retryHandle);
    if (retry == LockCreateOutcome::Acquired) {
        // 接管成功也必须留下事实记录（§7.1：stealLock 诊断必须记录抢占事实；
        // 死锁接管同样记 owner 全文 + 判死依据）。
        StoreLock lock;
        lock.handle_         = retryHandle;
        lock.lockFilePath_   = copyOf(lockPath);
        lock.writtenContent_ = copyOf(content);
        if (options.stealLock) {
            lock.diagnostics_.push_back(copyOf(
                "store.lock preempted via stealLock at '" + lockPath + "': previous owner " +
                previousOwnerText +
                "; skip-pid-probe preemption by user decision (design section 7.1)"));
        }
        else {
            lock.diagnostics_.push_back(copyOf(
                "store.lock taken over at '" + lockPath + "': previous owner " +
                previousOwnerText +
                " proven dead by the fail-closed PID probe (design section 7.2); old lock "
                "removed, exclusive re-create succeeded (exactly-once)"));
        }
        return ca::Ok(std::move(lock));
    }
    if (retry == LockCreateOutcome::Exists) {
        // 竞态输（再失败 = Lock，不循环）：消息重读当前锁文件报新赢家（R45/P5）。
        return ca::Err(lockError(
            "lost the takeover race for store.lock '" + lockPath + "' after removing the " +
            (options.stealLock ? "previous" : "proven-dead") + " owner's lock (" +
            previousOwnerText + "); exactly-one retry per design section 7.1; " +
            currentWinnerSuffix(lockPath)));
    }
    return ca::Err(makeError(ProjectStoreErrorKind::Io,
                             "failed to re-create store.lock '" + lockPath +
                                 "' during takeover: " + errnoText(errno)));
}

}   // namespace mjbase::project
