#include "analysis_project.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "libca/json/json_document.hpp"
#include "libca/json/json_handler.hpp"
#include "libca/json/json_parser.hpp"
#include "libca/json/json_reader.hpp"
#include "libca/json/json_value.hpp"
#include "libca/json/json_writer.hpp"

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

AnalysisProjectError makeError(AnalysisProjectErrorKind kind, const std::string& message)
{
    AnalysisProjectError error;
    error.kind    = kind;
    error.message = Utf8String::from_data(reinterpret_cast<const ca::u8*>(message.data()),
                                          message.size());
    return error;
}

/// 条目定位前缀："inputs[2]: " / "annotations[5]: "。DOM 无行号；工程文件由
/// 工具写出、JSON 语法错误本身已带行号，schema 定位到条目序号即可（与
/// Shadow-Alias 同一 deliberate 简化）。
std::string atEntry(const char* array, ca::usize index)
{
    return std::string(array) + "[" + std::to_string(index) + "]: ";
}

//-- 通用槽位辅助（Shadow-Alias 同款纪律） --------------------------------

ca::Result<const ca::json::JsonValue*, AnalysisProjectError> requiredString(
    const ca::json::JsonValue* slot, const std::string& context, const char* field)
{
    if (slot == nullptr) {
        return ca::Err(
            makeError(AnalysisProjectErrorKind::Schema, context + std::string("missing ") + field));
    }
    if (!slot->is_string() || slot->as_string().is_empty()) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                 context + std::string(field) + " must be a non-empty string"));
    }
    return ca::Ok(slot);
}

/// 可选字符串槽：未出现/空串 → nullptr。
ca::Result<const ca::json::JsonValue*, AnalysisProjectError> optionalString(
    const ca::json::JsonValue* slot, const std::string& context, const char* field)
{
    if (slot == nullptr) return ca::Ok(static_cast<const ca::json::JsonValue*>(nullptr));
    if (!slot->is_string()) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                 context + "field '" + std::string(field) + "' must be a string"));
    }
    if (slot->as_string().is_empty())
        return ca::Ok(static_cast<const ca::json::JsonValue*>(nullptr));
    return ca::Ok(slot);
}

/// 必填 int 槽，附带谓词校验（谓词消息自带口径说明）。
ca::Result<ca::i64, AnalysisProjectError> requiredInt(
    const ca::json::JsonValue* slot, const std::string& context, const char* field,
    const std::function<bool(ca::i64)>& valid, const char* rule)
{
    if (slot == nullptr) {
        return ca::Err(
            makeError(AnalysisProjectErrorKind::Schema, context + std::string("missing ") + field));
    }
    if (!slot->is_int()) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                 context + std::string(field) + " must be an integer"));
    }
    const ca::i64 value = slot->as_int();
    if (!valid(value)) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                 context + std::string(field) + " " + rule + " (got " +
                                     std::to_string(value) + ")"));
    }
    return ca::Ok(value);
}

//-- 枚举互转 -------------------------------------------------------------

const char* roleTag(AnalysisInputRole role)
{
    return role == AnalysisInputRole::Primary ? "primary" : "support";
}

bool parseRole(const Utf8StringRef& tag, AnalysisInputRole& role)
{
    if (tag.equals(refOf("primary"))) {
        role = AnalysisInputRole::Primary;
        return true;
    }
    if (tag.equals(refOf("support"))) {
        role = AnalysisInputRole::Support;
        return true;
    }
    return false;
}

const char* targetKindTag(AnnotationTargetKind kind)
{
    switch (kind) {
    case AnnotationTargetKind::Class: return "class";
    case AnnotationTargetKind::Method: return "method";
    case AnnotationTargetKind::Field: return "field";
    case AnnotationTargetKind::Instruction: return "instruction";
    }
    return "class";
}

bool parseTargetKind(const Utf8StringRef& tag, AnnotationTargetKind& kind)
{
    if (tag.equals(refOf("class"))) {
        kind = AnnotationTargetKind::Class;
        return true;
    }
    if (tag.equals(refOf("method"))) {
        kind = AnnotationTargetKind::Method;
        return true;
    }
    if (tag.equals(refOf("field"))) {
        kind = AnnotationTargetKind::Field;
        return true;
    }
    if (tag.equals(refOf("instruction"))) {
        kind = AnnotationTargetKind::Instruction;
        return true;
    }
    return false;
}

//-- target 解析（addAnnotation 与此共用 isValidAnnotationTarget 校验） ----

struct TargetSlots
{
    const ca::json::JsonValue* kind   = nullptr;
    const ca::json::JsonValue* owner  = nullptr;
    const ca::json::JsonValue* name   = nullptr;
    const ca::json::JsonValue* desc   = nullptr;
    const ca::json::JsonValue* index  = nullptr;
};

ca::Result<AnnotationTarget, AnalysisProjectError> parseTarget(
    const ca::json::JsonValue& value, const std::string& context)
{
    if (!value.is_object()) {
        return ca::Err(
            makeError(AnalysisProjectErrorKind::Schema, context + "target must be an object"));
    }
    TargetSlots slots;
    for (const auto& [key, member] : value.as_object()) {
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
        else if (key.equals(refOf("name"))) {
            slot  = &slots.name;
            known = "name";
        }
        else if (key.equals(refOf("desc"))) {
            slot  = &slots.desc;
            known = "desc";
        }
        else if (key.equals(refOf("index"))) {
            slot  = &slots.index;
            known = "index";
        }
        else {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     context + "unknown target field: " + toStd(key)));
        }
        if (*slot != nullptr) {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     context + std::string("duplicate target field: ") + known));
        }
        *slot = &member;
    }

    if (slots.kind == nullptr) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Schema, context + "missing target kind"));
    }
    if (!slots.kind->is_string()) {
        return ca::Err(
            makeError(AnalysisProjectErrorKind::Schema, context + "target kind must be a string"));
    }
    AnnotationTarget target;
    if (!parseTargetKind(slots.kind->as_string(), target.kind)) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                 context + "unknown target kind '" +
                                     toStd(slots.kind->as_string()) +
                                     "' (expected class|method|field|instruction)"));
    }

    auto owner = requiredString(slots.owner, context, "target owner");
    if (owner.is_err()) return ca::Err(std::move(owner).unwrap_err());
    target.owner = copyOf(owner.unwrap()->as_string());

    // name/desc：member 类必填非空；class 必须缺省/空。
    auto name = optionalString(slots.name, context, "name");
    if (name.is_err()) return ca::Err(std::move(name).unwrap_err());
    auto desc = optionalString(slots.desc, context, "desc");
    if (desc.is_err()) return ca::Err(std::move(desc).unwrap_err());
    const bool isMember = target.kind != AnnotationTargetKind::Class;
    if (isMember) {
        if (name.unwrap() == nullptr || desc.unwrap() == nullptr) {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     context + std::string(targetKindTag(target.kind)) +
                                         " target requires non-empty name and desc"));
        }
        target.name = copyOf(name.unwrap()->as_string());
        target.desc = copyOf(desc.unwrap()->as_string());
    }
    else if (name.unwrap() != nullptr || desc.unwrap() != nullptr) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                 context + "class target must not set name/desc"));
    }

    // index：instruction 必填 ≥0；其余必须缺省或 -1。
    if (target.kind == AnnotationTargetKind::Instruction) {
        auto index = requiredInt(slots.index, context, "target index",
                                 [](ca::i64 v) { return v >= 0; }, "must be >= 0");
        if (index.is_err()) return ca::Err(std::move(index).unwrap_err());
        target.index = index.unwrap();
    }
    else if (slots.index != nullptr) {
        if (!slots.index->is_int() || slots.index->as_int() != -1) {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     context +
                                         "target index is only allowed on instruction targets"));
        }
    }
    return ca::Ok(std::move(target));
}

//-- 写出装配辅助 ---------------------------------------------------------

ca::json::JsonValue targetJson(const AnnotationTarget& target)
{
    ca::json::JsonValue::ObjectStorage members;
    members.emplace_back(refOf("kind"),
                         ca::json::JsonValue::make_string(refOf(targetKindTag(target.kind))));
    members.emplace_back(refOf("owner"), ca::json::JsonValue::make_string(target.owner.ref()));
    if (target.kind != AnnotationTargetKind::Class) {
        members.emplace_back(refOf("name"), ca::json::JsonValue::make_string(target.name.ref()));
        members.emplace_back(refOf("desc"), ca::json::JsonValue::make_string(target.desc.ref()));
    }
    if (target.kind == AnnotationTargetKind::Instruction) {
        members.emplace_back(refOf("index"), ca::json::JsonValue::make_int(target.index));
    }
    return ca::json::JsonValue::make_object_from(std::move(members));
}

ca::json::JsonValue annotationJson(const AnalysisAnnotation& annotation)
{
    ca::json::JsonValue::ObjectStorage members;
    members.emplace_back(refOf("id"), ca::json::JsonValue::make_int(
                                          static_cast<ca::i64>(annotation.id)));
    members.emplace_back(refOf("target"), targetJson(annotation.target));
    members.emplace_back(refOf("text"), ca::json::JsonValue::make_string(annotation.text.ref()));
    members.emplace_back(refOf("author"), ca::json::JsonValue::make_string(annotation.author.ref()));
    members.emplace_back(refOf("created_at"),
                         ca::json::JsonValue::make_string(annotation.createdAt.ref()));
    return ca::json::JsonValue::make_object_from(std::move(members));
}

}   // namespace

//-- AnalysisProject ------------------------------------------------------

AnalysisProject AnalysisProject::createEmpty()
{
    return AnalysisProject();
}

bool AnalysisProject::addInput(AnalysisInput input)
{
    const std::string key = input.path.to_std_string();
    if (key.empty() || inputIndex_.count(key) > 0) return false;
    inputIndex_.emplace(key, inputs_.size());
    inputs_.push_back(std::move(input));
    return true;
}

ca::usize AnalysisProject::removeInput(const Utf8StringRef& path)
{
    const auto  it = inputIndex_.find(toStd(path));
    if (it == inputIndex_.end()) return 0;
    const auto  victim = it->second;
    inputs_.erase(inputs_.begin() + static_cast<std::ptrdiff_t>(victim));
    inputIndex_.clear();
    for (ca::usize i = 0; i < inputs_.size(); ++i) {
        inputIndex_.emplace(inputs_[i].path.to_std_string(), i);
    }
    return 1;
}

const AnalysisInput* AnalysisProject::findInput(const Utf8StringRef& path) const
{
    const auto  it = inputIndex_.find(toStd(path));
    if (it == inputIndex_.end()) return nullptr;
    return &inputs_[it->second];
}

void AnalysisProject::setShadowSidecar(ShadowSidecarRef ref)
{
    shadowSidecar_ = std::move(ref);
}

bool isValidAnnotationTarget(const AnnotationTarget& target)
{
    if (target.owner.is_empty()) return false;
    const bool isMember = target.kind != AnnotationTargetKind::Class;
    if (isMember) {
        if (target.name.is_empty() || target.desc.is_empty()) return false;
    }
    else {
        if (!target.name.is_empty() || !target.desc.is_empty()) return false;
    }
    if (target.kind == AnnotationTargetKind::Instruction) return target.index >= 0;
    return target.index == -1;
}

ca::u64 AnalysisProject::addAnnotation(AnnotationTarget              target,
                                       const Utf8StringRef&          text,
                                       const Utf8StringRef&          author,
                                       const Utf8StringRef&          createdAt)
{
    if (!isValidAnnotationTarget(target)) return 0;
    if (text.is_empty() || author.is_empty() || createdAt.is_empty()) return 0;

    AnalysisAnnotation annotation;
    annotation.id        = nextAnnotationId_++;
    annotation.target    = std::move(target);
    annotation.text      = copyOf(text);
    annotation.author    = copyOf(author);
    annotation.createdAt = copyOf(createdAt);

    const auto  index = annotations_.size();
    annotations_.push_back(std::move(annotation));
    annotationIndex_.emplace(annotations_[index].id, index);
    ownerIndex_[annotations_[index].target.owner.to_std_string()].push_back(index);
    return annotations_[index].id;
}

bool AnalysisProject::removeAnnotation(ca::u64 id)
{
    const auto  it = annotationIndex_.find(id);
    if (it == annotationIndex_.end()) return false;
    const auto  victim = it->second;

    annotations_.erase(annotations_.begin() + static_cast<std::ptrdiff_t>(victim));
    // 人工规模实体量：重建索引换实现简单性（与 ShadowAliasTable 的 arena 纪律
    // 不同，本表值语义、无热路径）。
    annotationIndex_.clear();
    ownerIndex_.clear();
    for (ca::usize i = 0; i < annotations_.size(); ++i) {
        annotationIndex_.emplace(annotations_[i].id, i);
        ownerIndex_[annotations_[i].target.owner.to_std_string()].push_back(i);
    }
    return true;
}

const AnalysisAnnotation* AnalysisProject::findAnnotation(ca::u64 id) const
{
    const auto  it = annotationIndex_.find(id);
    if (it == annotationIndex_.end()) return nullptr;
    return &annotations_[it->second];
}

std::vector<const AnalysisAnnotation*> AnalysisProject::annotationsOf(
    const Utf8StringRef& owner) const
{
    std::vector<const AnalysisAnnotation*> result;
    const auto                             it = ownerIndex_.find(toStd(owner));
    if (it == ownerIndex_.end()) return result;
    result.reserve(it->second.size());
    for (const auto  index : it->second) result.push_back(&annotations_[index]);
    std::sort(result.begin(), result.end(),
              [](const AnalysisAnnotation* a, const AnalysisAnnotation* b) { return a->id < b->id; });
    return result;
}

void AnalysisProject::clear()
{
    inputs_.clear();
    inputIndex_.clear();
    shadowSidecar_.reset();
    annotations_.clear();
    annotationIndex_.clear();
    ownerIndex_.clear();
    nextAnnotationId_ = 1;
}

//-- 严格解析 ---------------------------------------------------------------

ca::Result<AnalysisProject, AnalysisProjectError> parseAnalysisProjectText(
    const Utf8StringRef& text)
{
    auto document = ca::json::JsonReader::read(text);
    if (document.is_err()) {
        auto       parseError = std::move(document).unwrap_err();
        const auto message    = "invalid analysis project JSON at line " +
                                std::to_string(parseError.location.line) + ": " +
                                toStd(parseError.message.ref());
        return ca::Err(makeError(AnalysisProjectErrorKind::JsonRead, message));
    }
    auto parsedDocument = std::move(document).unwrap();
    const auto& root = parsedDocument.root();
    if (!root.is_object()) {
        return ca::Err(
            makeError(AnalysisProjectErrorKind::Schema, "analysis project root must be an object"));
    }

    const ca::json::JsonValue* version          = nullptr;
    const ca::json::JsonValue* kind             = nullptr;
    const ca::json::JsonValue* inputs           = nullptr;
    const ca::json::JsonValue* shadowSidecar    = nullptr;
    const ca::json::JsonValue* nextAnnotationId = nullptr;
    const ca::json::JsonValue* annotations      = nullptr;
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
        else if (key.equals(refOf("inputs"))) {
            slot  = &inputs;
            known = "inputs";
        }
        else if (key.equals(refOf("shadow_sidecar"))) {
            slot  = &shadowSidecar;
            known = "shadow_sidecar";
        }
        else if (key.equals(refOf("next_annotation_id"))) {
            slot  = &nextAnnotationId;
            known = "next_annotation_id";
        }
        else if (key.equals(refOf("annotations"))) {
            slot  = &annotations;
            known = "annotations";
        }
        else {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     "unknown analysis project field: " + toStd(key)));
        }
        if (*slot != nullptr) {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     std::string("duplicate analysis project field: ") + known));
        }
        *slot = &value;
    }

    if (version == nullptr) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Schema, "missing version"));
    }
    if (!version->is_int() || version->as_int() != static_cast<ca::i64>(kAnalysisProjectFileVersion)) {
        const ca::i64 actual = version->is_int() ? version->as_int() : -1;
        return ca::Err(makeError(
            AnalysisProjectErrorKind::Schema,
            "unsupported schema version " + std::to_string(actual) +
                " (supported: " + std::to_string(kAnalysisProjectFileVersion) + "); refusing to load"));
    }
    if (kind == nullptr || !kind->is_string() || !kind->as_string().equals(refOf(kAnalysisProjectKind))) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                 std::string("kind must be \"") + kAnalysisProjectKind + "\""));
    }
    if (inputs == nullptr || !inputs->is_array()) {
        return ca::Err(
            makeError(AnalysisProjectErrorKind::Schema, "inputs must be an array"));
    }
    if (annotations == nullptr || !annotations->is_array()) {
        return ca::Err(
            makeError(AnalysisProjectErrorKind::Schema, "annotations must be an array"));
    }

    AnalysisProject project;

    //-- inputs ------------------------------------------------------------
    std::set<std::string> seenPaths;
    ca::usize             inputIndex = 0;
    for (const auto& entry : inputs->as_array()) {
        const std::string context = atEntry("inputs", inputIndex);
        if (!entry.is_object()) {
            return ca::Err(
                makeError(AnalysisProjectErrorKind::Schema, context + "input must be an object"));
        }
        const ca::json::JsonValue* path        = nullptr;
        const ca::json::JsonValue* fingerprint = nullptr;
        const ca::json::JsonValue* role        = nullptr;
        for (const auto& [key, value] : entry.as_object()) {
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
            else if (key.equals(refOf("role"))) {
                slot  = &role;
                known = "role";
            }
            else {
                return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                         context + "unknown input field: " + toStd(key)));
            }
            if (*slot != nullptr) {
                return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                         context + std::string("duplicate input field: ") + known));
            }
            *slot = &value;
        }
        auto pathValue = requiredString(path, context, "path");
        if (pathValue.is_err()) return ca::Err(std::move(pathValue).unwrap_err());
        auto fingerprintValue = requiredString(fingerprint, context, "fingerprint");
        if (fingerprintValue.is_err()) return ca::Err(std::move(fingerprintValue).unwrap_err());
        auto roleValue = requiredString(role, context, "role");
        if (roleValue.is_err()) return ca::Err(std::move(roleValue).unwrap_err());

        AnalysisInput input;
        input.path        = copyOf(pathValue.unwrap()->as_string());
        input.fingerprint = copyOf(fingerprintValue.unwrap()->as_string());
        if (!parseRole(roleValue.unwrap()->as_string(), input.role)) {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     context + "unknown role '" +
                                         toStd(roleValue.unwrap()->as_string()) +
                                         "' (expected primary|support)"));
        }
        if (!seenPaths.insert(input.path.to_std_string()).second) {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     context + "duplicate input path: " + input.path.to_std_string()));
        }
        project.addInput(std::move(input));
        ++inputIndex;
    }

    //-- shadow_sidecar（可选单槽） ------------------------------------------
    if (shadowSidecar != nullptr) {
        if (!shadowSidecar->is_object()) {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     "shadow_sidecar must be an object"));
        }
        const ca::json::JsonValue* sidecarPath        = nullptr;
        const ca::json::JsonValue* sidecarFingerprint = nullptr;
        for (const auto& [key, value] : shadowSidecar->as_object()) {
            const ca::json::JsonValue** slot  = nullptr;
            const char*                 known = nullptr;
            if (key.equals(refOf("path"))) {
                slot  = &sidecarPath;
                known = "path";
            }
            else if (key.equals(refOf("fingerprint"))) {
                slot  = &sidecarFingerprint;
                known = "fingerprint";
            }
            else {
                return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                         "unknown shadow_sidecar field: " + toStd(key)));
            }
            if (*slot != nullptr) {
                return ca::Err(
                    makeError(AnalysisProjectErrorKind::Schema,
                              std::string("duplicate shadow_sidecar field: ") + known));
            }
            *slot = &value;
        }
        auto pathValue = requiredString(sidecarPath, "", "shadow_sidecar path");
        if (pathValue.is_err()) return ca::Err(std::move(pathValue).unwrap_err());
        auto fingerprintValue = optionalString(sidecarFingerprint, "", "fingerprint");
        if (fingerprintValue.is_err()) return ca::Err(std::move(fingerprintValue).unwrap_err());

        ShadowSidecarRef ref;
        ref.path = copyOf(pathValue.unwrap()->as_string());
        if (fingerprintValue.unwrap() != nullptr) {
            ref.fingerprint = copyOf(fingerprintValue.unwrap()->as_string());
        }
        project.setShadowSidecar(std::move(ref));
    }

    //-- annotations（单遍收集；next_annotation_id 校验放在 id 集合之后） --------
    std::set<ca::u64>               seenIds;
    ca::u64                         maxId = 0;
    std::vector<AnalysisAnnotation> loaded;
    ca::usize                       annotationIndex = 0;
    for (const auto& entry : annotations->as_array()) {
        const std::string context = atEntry("annotations", annotationIndex);
        const ca::json::JsonValue* id        = nullptr;
        const ca::json::JsonValue* target    = nullptr;
        const ca::json::JsonValue* text      = nullptr;
        const ca::json::JsonValue* author    = nullptr;
        const ca::json::JsonValue* createdAt = nullptr;
        for (const auto& [key, value] : entry.as_object()) {
            const ca::json::JsonValue** slot  = nullptr;
            const char*                 known = nullptr;
            if (key.equals(refOf("id"))) {
                slot  = &id;
                known = "id";
            }
            else if (key.equals(refOf("target"))) {
                slot  = &target;
                known = "target";
            }
            else if (key.equals(refOf("text"))) {
                slot  = &text;
                known = "text";
            }
            else if (key.equals(refOf("author"))) {
                slot  = &author;
                known = "author";
            }
            else if (key.equals(refOf("created_at"))) {
                slot  = &createdAt;
                known = "created_at";
            }
            else {
                return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                         context + "unknown annotation field: " + toStd(key)));
            }
            if (*slot != nullptr) {
                return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                         context + std::string("duplicate annotation field: ") + known));
            }
            *slot = &value;
        }
        auto idValue = requiredInt(id, context, "id", [](ca::i64 v) { return v > 0; },
                                   "must be > 0");
        if (idValue.is_err()) return ca::Err(std::move(idValue).unwrap_err());
        if (target == nullptr) {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema, context + "missing target"));
        }
        auto parsedTarget = parseTarget(*target, context);
        if (parsedTarget.is_err()) return ca::Err(std::move(parsedTarget).unwrap_err());
        auto textValue = requiredString(text, context, "text");
        if (textValue.is_err()) return ca::Err(std::move(textValue).unwrap_err());
        auto authorValue = requiredString(author, context, "author");
        if (authorValue.is_err()) return ca::Err(std::move(authorValue).unwrap_err());
        auto createdAtValue = requiredString(createdAt, context, "created_at");
        if (createdAtValue.is_err()) return ca::Err(std::move(createdAtValue).unwrap_err());

        const ca::u64 annotationId = static_cast<ca::u64>(idValue.unwrap());
        if (!seenIds.insert(annotationId).second) {
            return ca::Err(makeError(AnalysisProjectErrorKind::Schema,
                                     context + "duplicate annotation id: " +
                                         std::to_string(annotationId)));
        }
        maxId = std::max(maxId, annotationId);
        AnalysisAnnotation annotation;
        annotation.id        = annotationId;
        annotation.target    = std::move(parsedTarget).unwrap();
        annotation.text      = copyOf(textValue.unwrap()->as_string());
        annotation.author    = copyOf(authorValue.unwrap()->as_string());
        annotation.createdAt = copyOf(createdAtValue.unwrap()->as_string());
        loaded.push_back(std::move(annotation));
        ++annotationIndex;
    }

    //-- next_annotation_id（必填，≥ max(id)+1 防 id 复用） --------------------
    auto nextId = requiredInt(nextAnnotationId, "", "next_annotation_id",
                              [](ca::i64 v) { return v > 0; }, "must be > 0");
    if (nextId.is_err()) return ca::Err(std::move(nextId).unwrap_err());
    if (static_cast<ca::u64>(nextId.unwrap()) < maxId + 1) {
        return ca::Err(makeError(
            AnalysisProjectErrorKind::Schema,
            "next_annotation_id " + std::to_string(nextId.unwrap()) +
                " would reuse annotation ids (in-use max: " + std::to_string(maxId) + ")"));
    }

    // 文件 id 是事实（绕过 addAnnotation 的单调分配）；next_annotation_id 已校验。
    for (auto& annotation : loaded) {
        const auto  slot = project.annotations_.size();
        project.annotations_.push_back(std::move(annotation));
        project.annotationIndex_.emplace(project.annotations_[slot].id, slot);
        project.ownerIndex_[project.annotations_[slot].target.owner.to_std_string()].push_back(slot);
    }
    project.nextAnnotationId_ = static_cast<ca::u64>(nextId.unwrap());

    return ca::Ok(std::move(project));
}

ca::Result<AnalysisProject, AnalysisProjectError> readAnalysisProjectFile(
    const Utf8StringRef& path)
{
    std::ifstream input(path.to_std_string(), std::ios::binary);
    if (!input.is_open()) {
        return ca::Err(makeError(AnalysisProjectErrorKind::JsonRead,
                                 "failed to open analysis project file: " + toStd(path)));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();

    auto parsed = parseAnalysisProjectText(
        Utf8StringRef::from_string_view(std::string_view(text.data(), text.size())));
    if (parsed.is_err()) {
        auto                     error = std::move(parsed).unwrap_err();
        ca::str::Utf8StringBuilder builder;
        builder.append(path).append(": ").append(error.message.ref());
        error.message = builder.build_or_empty();
        return ca::Err(std::move(error));
    }
    return parsed;
}

//-- 写出 -------------------------------------------------------------------

ca::Result<ca::str::Utf8String, AnalysisProjectError> writeAnalysisProjectText(
    const AnalysisProject& project)
{
    // 确定序：inputs 按路径字典序、annotations 按 id 升序——与插入序无关。
    std::vector<const AnalysisInput*> sortedInputs;
    sortedInputs.reserve(project.inputCount());
    for (const auto& input : project.inputs()) sortedInputs.push_back(&input);
    std::sort(sortedInputs.begin(), sortedInputs.end(),
              [](const AnalysisInput* a, const AnalysisInput* b) {
                  return a->path.to_std_string() < b->path.to_std_string();
              });

    std::vector<const AnalysisAnnotation*> sortedAnnotations;
    sortedAnnotations.reserve(project.annotationCount());
    for (const auto& annotation : project.annotations()) sortedAnnotations.push_back(&annotation);
    std::sort(sortedAnnotations.begin(), sortedAnnotations.end(),
              [](const AnalysisAnnotation* a, const AnalysisAnnotation* b) {
                  return a->id < b->id;
              });

    ca::json::JsonValue::ObjectStorage rootMembers;
    rootMembers.emplace_back(refOf("version"),
                             ca::json::JsonValue::make_int(
                                 static_cast<ca::i64>(kAnalysisProjectFileVersion)));
    rootMembers.emplace_back(refOf("kind"),
                             ca::json::JsonValue::make_string(refOf(kAnalysisProjectKind)));

    auto inputsJson = ca::json::JsonValue::make_array();
    for (const auto* input : sortedInputs) {
        ca::json::JsonValue::ObjectStorage members;
        members.emplace_back(refOf("path"), ca::json::JsonValue::make_string(input->path.ref()));
        members.emplace_back(refOf("fingerprint"),
                             ca::json::JsonValue::make_string(input->fingerprint.ref()));
        members.emplace_back(refOf("role"),
                             ca::json::JsonValue::make_string(refOf(roleTag(input->role))));
        inputsJson.append(ca::json::JsonValue::make_object_from(std::move(members)));
    }
    rootMembers.emplace_back(refOf("inputs"), std::move(inputsJson));

    if (project.shadowSidecar() != nullptr) {
        ca::json::JsonValue::ObjectStorage members;
        members.emplace_back(refOf("path"),
                             ca::json::JsonValue::make_string(project.shadowSidecar()->path.ref()));
        if (!project.shadowSidecar()->fingerprint.is_empty()) {
            members.emplace_back(
                refOf("fingerprint"),
                ca::json::JsonValue::make_string(project.shadowSidecar()->fingerprint.ref()));
        }
        rootMembers.emplace_back(refOf("shadow_sidecar"),
                                 ca::json::JsonValue::make_object_from(std::move(members)));
    }

    ca::u64 maxId = 0;
    auto    annotationsJson = ca::json::JsonValue::make_array();
    for (const auto* annotation : sortedAnnotations) {
        maxId = std::max(maxId, annotation->id);
        annotationsJson.append(annotationJson(*annotation));
    }
    rootMembers.emplace_back(refOf("next_annotation_id"),
                             ca::json::JsonValue::make_int(static_cast<ca::i64>(
                                 std::max(project.nextAnnotationId(), maxId + 1))));
    rootMembers.emplace_back(refOf("annotations"), std::move(annotationsJson));

    ca::json::JsonDocument document;
    document.root() = ca::json::JsonValue::make_object_from(std::move(rootMembers));

    ca::json::JsonWriterOptions options;
    options.pretty = true;
    return ca::Ok(ca::json::JsonWriter::write(document, options));
}

ca::Result<void, AnalysisProjectError> writeAnalysisProjectFile(const Utf8StringRef&   path,
                                                                const AnalysisProject& project)
{
    auto text = writeAnalysisProjectText(project);
    if (text.is_err()) return ca::Err(std::move(text).unwrap_err());

    std::ofstream out(path.to_std_string(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Io,
                                 "failed to open analysis project file for writing: " +
                                     toStd(path)));
    }
    const auto& payload = std::move(text).unwrap();
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.byte_length()));
    if (!out) {
        return ca::Err(makeError(AnalysisProjectErrorKind::Io,
                                 "failed to write analysis project file: " + toStd(path)));
    }
    return ca::Ok();
}

}   // namespace mjbase::project
