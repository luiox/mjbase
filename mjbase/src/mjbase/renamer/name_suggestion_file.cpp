#include "name_suggestion_file.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "libca/json/json_document.hpp"
#include "libca/json/json_handler.hpp"
#include "libca/json/json_parser.hpp"
#include "libca/json/json_reader.hpp"
#include "libca/json/json_value.hpp"
#include "libca/json/json_writer.hpp"

#include "mjbase/asm/analyzer/alias_analysis.hpp"

namespace mjbase::renamer {
namespace {

using asm_analyzer::aliasInterned;

ca::str::Utf8StringRef refOf(const char* text)
{
    return ca::str::Utf8StringRef::from_cstr(text);
}

ca::str::Utf8StringRef utf8Ref(std::string_view value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

std::string_view viewOf(const ca::str::Utf8StringRef& value)
{
    return std::string_view(reinterpret_cast<const char*>(value.data()), value.byte_length());
}

SuggestionFileError makeError(SuggestionFileErrorKind kind, const std::string& message)
{
    SuggestionFileError error;
    error.kind    = kind;
    error.message = message;
    return error;
}

/// 顶层字段定位前缀："version (line 2)"；行号未知（0）时退回裸字段名。
std::string atTopLevel(const char* field, ca::usize line)
{
    if (line == 0) return std::string(field);
    return std::string(field) + " (line " + std::to_string(line) + ")";
}

/// proposal 定位前缀："proposals[3] (line 12)"；行号未知（0）时退回裸下标。
std::string atProposal(ca::usize index, ca::usize line)
{
    std::string base = "proposals[" + std::to_string(index) + "]";
    if (line != 0) base += " (line " + std::to_string(line) + ")";
    return base;
}

/// DOM 无位置信息；用同一 libca parser 的事件流预扫一遍，记录顶层 key 与
/// proposals 数组各元素的起始行号，schema 错误消息据此带行号（两遍法）。
class SuggestionLocationTracker final : public ca::json::JsonHandler
{
public:
    void on_object_start(const ca::json::SourceLocation& loc) override
    {
        noteProposalElement(loc);
        ++depth_;
    }

    void on_object_end(const ca::json::SourceLocation&) override { --depth_; }

    void on_array_start(const ca::json::SourceLocation& loc) override
    {
        if (depth_ == 1 && expectingProposals_) {
            inProposalsArray_   = true;
            expectingProposals_ = false;
        }
        else {
            noteProposalElement(loc);
        }
        ++depth_;
    }

    void on_array_end(const ca::json::SourceLocation&) override
    {
        --depth_;
        if (depth_ == 1 && inProposalsArray_) inProposalsArray_ = false;
    }

    void on_object_key(ca::str::Utf8StringRef key, const ca::json::SourceLocation& loc) override
    {
        if (depth_ != 1) return;
        if (key.equals(refOf("version"))) {
            versionLine_ = loc.line;
        }
        else if (key.equals(refOf("source"))) {
            sourceLine_ = loc.line;
        }
        else if (key.equals(refOf("proposals"))) {
            proposalsLine_      = loc.line;
            expectingProposals_ = true;
        }
    }

    void on_null(const ca::json::SourceLocation& loc) override { noteProposalElement(loc); }
    void on_bool(bool, const ca::json::SourceLocation& loc) override { noteProposalElement(loc); }
    void on_int(ca::i64, const ca::json::SourceLocation& loc) override { noteProposalElement(loc); }
    void on_float(ca::f64, const ca::json::SourceLocation& loc) override
    {
        noteProposalElement(loc);
    }
    void on_string(ca::str::Utf8StringRef, const ca::json::SourceLocation& loc) override
    {
        noteProposalElement(loc);
    }

    void on_error(const ca::json::ParseError&) override { parseFailed_ = true; }

    bool      parseFailed() const { return parseFailed_; }
    ca::usize versionLine() const { return versionLine_; }
    ca::usize sourceLine() const { return sourceLine_; }
    ca::usize proposalsLine() const { return proposalsLine_; }
    ca::usize proposalLine(ca::usize index) const
    {
        return index < proposalLines_.size() ? proposalLines_[index] : 0;
    }

private:
    /// proposals 数组内的每个元素值事件（对象/数组/标量）各占一个槽位并记起始
    /// 行号——非对象元素同样占槽，行号才不因元素形态前移错位。
    void noteProposalElement(const ca::json::SourceLocation& loc)
    {
        if (depth_ == 2 && inProposalsArray_) proposalLines_.push_back(loc.line);
    }

    ca::i32                depth_              = 0;
    bool                   expectingProposals_ = false;
    bool                   inProposalsArray_   = false;
    bool                   parseFailed_        = false;
    ca::usize              versionLine_        = 0;
    ca::usize              sourceLine_         = 0;
    ca::usize              proposalsLine_      = 0;
    std::vector<ca::usize> proposalLines_;
};

/// proposal 对象的字段槽位（destination 指针法，重复字段在收集期检出）。
struct ProposalSlots
{
    const ca::json::JsonValue* kind       = nullptr;
    const ca::json::JsonValue* owner      = nullptr;
    const ca::json::JsonValue* name       = nullptr;
    const ca::json::JsonValue* desc       = nullptr;
    const ca::json::JsonValue* newName    = nullptr;
    const ca::json::JsonValue* confidence = nullptr;
    const ca::json::JsonValue* rationale  = nullptr;
    const ca::json::JsonValue* evidence   = nullptr;
};

/// 收集 proposal 对象成员到槽位；未知/重复字段即 schema 错误（context 为定位前缀）。
ca::core::Result<void, SuggestionFileError> collectProposalSlots(
    const ca::json::JsonValue& proposal, const std::string& context, ProposalSlots& slots)
{
    for (const auto& [key, value] : proposal.as_object()) {
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
        else if (key.equals(refOf("newName"))) {
            slot  = &slots.newName;
            known = "newName";
        }
        else if (key.equals(refOf("confidence"))) {
            slot  = &slots.confidence;
            known = "confidence";
        }
        else if (key.equals(refOf("rationale"))) {
            slot  = &slots.rationale;
            known = "rationale";
        }
        else if (key.equals(refOf("evidence"))) {
            slot  = &slots.evidence;
            known = "evidence";
        }
        else {
            return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                           context + "unknown proposal field: " +
                                               std::string(viewOf(key))));
        }
        if (*slot != nullptr) {
            return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                           context + std::string("duplicate proposal field: ") +
                                               known));
        }
        *slot = &value;
    }
    return ca::core::Ok();
}

/// 校验"可选字符串"槽：出现时必须是 string。未出现/空串返回 nullptr（空串视同缺省）。
ca::core::Result<const ca::json::JsonValue*, SuggestionFileError> optionalString(
    const ca::json::JsonValue* slot, const std::string& context, const char* field)
{
    if (slot == nullptr) return ca::core::Ok(static_cast<const ca::json::JsonValue*>(nullptr));
    if (!slot->is_string()) {
        return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                       context + "field '" + field + "' must be a string"));
    }
    if (slot->as_string().is_empty()) {
        return ca::core::Ok(static_cast<const ca::json::JsonValue*>(nullptr));
    }
    return ca::core::Ok(slot);
}

/// 要求"必填非空字符串"。
ca::core::Result<const ca::json::JsonValue*, SuggestionFileError> requiredString(
    const ca::json::JsonValue* slot, const std::string& context, const char* field)
{
    if (slot == nullptr) {
        return ca::core::Err(
            makeError(SuggestionFileErrorKind::Schema, context + std::string("missing ") + field));
    }
    if (!slot->is_string() || slot->as_string().is_empty()) {
        return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                       context + std::string(field) +
                                           " must be a non-empty string"));
    }
    return ca::core::Ok(slot);
}

ca::core::Result<NameSuggestionFile, SuggestionFileError> parseDocument(
    const ca::json::JsonDocument& document, const SuggestionLocationTracker& lines)
{
    const auto& root = document.root();
    if (!root.is_object()) {
        return ca::core::Err(
            makeError(SuggestionFileErrorKind::Schema, "suggestion file root must be an object"));
    }

    // 顶层字段收集：未知字段即错；重复字段按收集计数检出（DOM 保序保留重复成员）。
    const ca::json::JsonValue* version      = nullptr;
    const ca::json::JsonValue* source       = nullptr;
    const ca::json::JsonValue* proposals    = nullptr;
    ca::usize                  versionCount = 0, sourceCount = 0, proposalsCount = 0;
    for (const auto& [key, value] : root.as_object()) {
        if (key.equals(refOf("version"))) {
            version = &value;
            ++versionCount;
        }
        else if (key.equals(refOf("source"))) {
            source = &value;
            ++sourceCount;
        }
        else if (key.equals(refOf("proposals"))) {
            proposals = &value;
            ++proposalsCount;
        }
        else {
            return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                           "unknown suggestion file field: " +
                                               std::string(viewOf(key))));
        }
    }
    if (versionCount > 1 || sourceCount > 1 || proposalsCount > 1) {
        // 重复字段定位：报出字段名与（tracker 记录的首次）行号。
        const char* field =
            versionCount > 1 ? "version" : (sourceCount > 1 ? "source" : "proposals");
        const ca::usize line = versionCount > 1  ? lines.versionLine()
                               : sourceCount > 1 ? lines.sourceLine()
                                                 : lines.proposalsLine();
        return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                       "duplicate suggestion file field: " +
                                           atTopLevel(field, line)));
    }

    if (version == nullptr) {
        return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                       atTopLevel("version", lines.versionLine()) +
                                           ": missing version"));
    }
    if (!version->is_int()) {
        return ca::core::Err(makeError(
            SuggestionFileErrorKind::Schema,
            atTopLevel("version", lines.versionLine()) + ": version must be an integer"));
    }
    if (version->as_int() != static_cast<ca::i64>(kNameSuggestionFileVersion)) {
        return ca::core::Err(
            makeError(SuggestionFileErrorKind::Schema,
                      atTopLevel("version", lines.versionLine()) +
                          ": unsupported schema version " + std::to_string(version->as_int()) +
                          " (supported: " + std::to_string(kNameSuggestionFileVersion) +
                          "); refusing to load"));
    }
    if (source == nullptr) {
        return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                       atTopLevel("source", lines.sourceLine()) +
                                           ": missing source"));
    }
    if (!source->is_string() || source->as_string().is_empty()) {
        return ca::core::Err(
            makeError(SuggestionFileErrorKind::Schema,
                      atTopLevel("source", lines.sourceLine()) +
                          ": source must be a non-empty string"));
    }
    if (proposals == nullptr) {
        return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                       atTopLevel("proposals", lines.proposalsLine()) +
                                           ": missing proposals"));
    }
    if (!proposals->is_array()) {
        return ca::core::Err(
            makeError(SuggestionFileErrorKind::Schema,
                      atTopLevel("proposals", lines.proposalsLine()) +
                          ": proposals must be an array"));
    }

    NameSuggestionFile file;
    file.source = asm_analyzer::aliasInterned(viewOf(source->as_string()));

    ca::usize index = 0;
    for (const auto& proposalValue : proposals->as_array()) {
        const std::string context = atProposal(index, lines.proposalLine(index)) + ": ";
        if (!proposalValue.is_object()) {
            return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                           context + "proposal must be an object"));
        }

        ProposalSlots slots;
        auto          collected = collectProposalSlots(proposalValue, context, slots);
        if (collected.is_err()) return ca::core::Err(std::move(collected).unwrap_err());

        if (slots.kind == nullptr) {
            return ca::core::Err(
                makeError(SuggestionFileErrorKind::Schema, context + "missing kind"));
        }
        if (!slots.kind->is_string()) {
            return ca::core::Err(
                makeError(SuggestionFileErrorKind::Schema, context + "kind must be a string"));
        }
        ProposalKind kindValue = ProposalKind::Class;
        if (!proposalKindFromLabel(viewOf(slots.kind->as_string()), kindValue)) {
            return ca::core::Err(
                makeError(SuggestionFileErrorKind::Schema,
                          context + "unknown kind '" + std::string(viewOf(slots.kind->as_string())) +
                              "' (expected class|package|field|method)"));
        }

        auto nameValue = requiredString(slots.name, context, "name");
        if (nameValue.is_err()) return ca::core::Err(std::move(nameValue).unwrap_err());
        auto newNameValue = requiredString(slots.newName, context, "newName");
        if (newNameValue.is_err()) return ca::core::Err(std::move(newNameValue).unwrap_err());

        if (slots.confidence == nullptr) {
            return ca::core::Err(
                makeError(SuggestionFileErrorKind::Schema, context + "missing confidence"));
        }
        if (!slots.confidence->is_number()) {
            return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                           context + "confidence must be a number"));
        }
        const double confidenceValue = slots.confidence->as_float_or(-1.0);
        if (!(confidenceValue >= 0.0 && confidenceValue <= 1.0)) {
            return ca::core::Err(makeError(SuggestionFileErrorKind::Schema,
                                           context + "confidence out of [0,1]"));
        }

        // owner/desc：field/method 必填非空；class/package 必须缺省或空。
        auto ownerValue = optionalString(slots.owner, context, "owner");
        if (ownerValue.is_err()) return ca::core::Err(std::move(ownerValue).unwrap_err());
        auto descValue = optionalString(slots.desc, context, "desc");
        if (descValue.is_err()) return ca::core::Err(std::move(descValue).unwrap_err());
        const bool memberKind =
            kindValue == ProposalKind::Field || kindValue == ProposalKind::Method;
        if (memberKind) {
            if (ownerValue.unwrap() == nullptr || descValue.unwrap() == nullptr) {
                return ca::core::Err(
                    makeError(SuggestionFileErrorKind::Schema,
                              context + std::string(proposalKindLabel(kindValue)) +
                                  " proposal requires a non-empty owner and desc"));
            }
        }
        else if (ownerValue.unwrap() != nullptr || descValue.unwrap() != nullptr) {
            return ca::core::Err(
                makeError(SuggestionFileErrorKind::Schema,
                          context + std::string(proposalKindLabel(kindValue)) +
                              " proposal must not set owner/desc"));
        }

        auto rationaleValue = optionalString(slots.rationale, context, "rationale");
        if (rationaleValue.is_err()) return ca::core::Err(std::move(rationaleValue).unwrap_err());
        auto evidenceValue = optionalString(slots.evidence, context, "evidence");
        if (evidenceValue.is_err()) return ca::core::Err(std::move(evidenceValue).unwrap_err());

        NameProposal proposal(
            kindValue,
            ownerValue.unwrap() != nullptr ? viewOf(ownerValue.unwrap()->as_string())
                                           : std::string_view{},
            viewOf(nameValue.unwrap()->as_string()),
            descValue.unwrap() != nullptr ? viewOf(descValue.unwrap()->as_string())
                                          : std::string_view{},
            viewOf(newNameValue.unwrap()->as_string()),
            confidenceValue,
            std::string_view{});   // source 留空：由 provider 回填文件顶层 source
        if (rationaleValue.unwrap() != nullptr) {
            proposal.rationale = asm_analyzer::aliasInterned(viewOf(rationaleValue.unwrap()->as_string()));
        }
        if (evidenceValue.unwrap() != nullptr) {
            proposal.evidence = asm_analyzer::aliasInterned(viewOf(evidenceValue.unwrap()->as_string()));
        }
        file.proposals.push_back(std::move(proposal));
        ++index;
    }
    return ca::core::Ok(std::move(file));
}

}   // namespace

std::string_view proposalKindLabel(ProposalKind kind)
{
    switch (kind) {
    case ProposalKind::Class: return "class";
    case ProposalKind::Package: return "package";
    case ProposalKind::Field: return "field";
    case ProposalKind::Method: return "method";
    }
    return "unknown";
}

bool proposalKindFromLabel(std::string_view label, ProposalKind& out)
{
    if (label == "class") {
        out = ProposalKind::Class;
    }
    else if (label == "package") {
        out = ProposalKind::Package;
    }
    else if (label == "field") {
        out = ProposalKind::Field;
    }
    else if (label == "method") {
        out = ProposalKind::Method;
    }
    else {
        return false;
    }
    return true;
}

ca::core::Result<NameSuggestionFile, SuggestionFileError> parseNameSuggestionFile(
    std::string_view text)
{
    // 两遍法：DOM 读结构 + SAX 预扫记行号。
    const auto textRef = utf8Ref(text);
    auto document = ca::json::JsonReader::read(textRef);
    if (document.is_err()) {
        auto       parseError = std::move(document).unwrap_err();
        const auto message    = "invalid suggestion file JSON at line " +
                             std::to_string(parseError.location.line) + ": " +
                             parseError.message.to_std_string();
        return ca::core::Err(makeError(SuggestionFileErrorKind::JsonRead, message));
    }

    ca::str::Utf8StringArena arena;
    SuggestionLocationTracker tracker;
    ca::json::JsonParser      parser(textRef, tracker, arena);
    if (!parser.parse() || tracker.parseFailed()) {
        return ca::core::Err(
            makeError(SuggestionFileErrorKind::JsonRead, "invalid suggestion file JSON"));
    }
    return parseDocument(std::move(document).unwrap(), tracker);
}

ca::core::Result<NameSuggestionFile, SuggestionFileError> readNameSuggestionFile(
    std::string_view path)
{
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input.is_open()) {
        return ca::core::Err(makeError(SuggestionFileErrorKind::JsonRead,
                                       "failed to open suggestion file: " + std::string(path)));
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();

    auto parsed = parseNameSuggestionFile(text);
    if (parsed.is_err()) {
        auto error = std::move(parsed).unwrap_err();
        error.message = std::string(path) + ": " + error.message;
        return ca::core::Err(std::move(error));
    }
    return parsed;
}

ca::core::Result<std::string, SuggestionFileError> writeNameSuggestionText(
    const NameSuggestionFile& file)
{
    ca::json::JsonDocument document;
    auto&                  root = document.root();
    root                        = ca::json::JsonValue::make_object();
    root.set(refOf("version"),
             ca::json::JsonValue::make_int(static_cast<ca::i64>(file.version)));
    root.set(refOf("source"), ca::json::JsonValue::make_string(utf8Ref(file.source)));

    auto proposals = ca::json::JsonValue::make_array();
    for (const auto& proposal : file.proposals) {
        ca::json::JsonValue node = ca::json::JsonValue::make_object();
        node.set(refOf("kind"),
                 ca::json::JsonValue::make_string(utf8Ref(proposalKindLabel(proposal.kind))));
        if (!proposal.owner.empty()) {
            node.set(refOf("owner"), ca::json::JsonValue::make_string(utf8Ref(proposal.owner)));
        }
        node.set(refOf("name"), ca::json::JsonValue::make_string(utf8Ref(proposal.oldName)));
        if (!proposal.desc.empty()) {
            node.set(refOf("desc"), ca::json::JsonValue::make_string(utf8Ref(proposal.desc)));
        }
        node.set(refOf("newName"), ca::json::JsonValue::make_string(utf8Ref(proposal.newName)));
        node.set(refOf("confidence"), ca::json::JsonValue::make_float(proposal.confidence));
        if (!proposal.rationale.empty()) {
            node.set(refOf("rationale"),
                     ca::json::JsonValue::make_string(utf8Ref(proposal.rationale)));
        }
        if (!proposal.evidence.empty()) {
            node.set(refOf("evidence"),
                     ca::json::JsonValue::make_string(utf8Ref(proposal.evidence)));
        }
        proposals.append(std::move(node));
    }
    root.set(refOf("proposals"), std::move(proposals));

    ca::json::JsonWriterOptions options;
    options.pretty = true;
    return ca::core::Ok(ca::json::JsonWriter::write(document, options).to_std_string());
}

ca::core::Result<void, SuggestionFileError> writeNameSuggestionFile(
    std::string_view path, const NameSuggestionFile& file)
{
    auto text = writeNameSuggestionText(file);
    if (text.is_err()) return ca::core::Err(std::move(text).unwrap_err());

    std::ofstream out(std::string(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        return ca::core::Err(makeError(SuggestionFileErrorKind::Io,
                                       "failed to open suggestion file for writing: " +
                                           std::string(path)));
    }
    const auto payload = std::move(text).unwrap();
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (!out) {
        return ca::core::Err(makeError(SuggestionFileErrorKind::Io,
                                       "failed to write suggestion file: " +
                                           std::string(path)));
    }
    return ca::core::Ok();
}

// ─────────────────────────── FileSuggestionProvider ───────────────────────────

FileSuggestionProvider::FileSuggestionProvider(NameSuggestionFile file)
    : file_(std::move(file))
{}

std::string_view FileSuggestionProvider::name() const
{
    // "suggestion-file:<source>"（进程级驻留；同一 source 恒等视图）。
    return asm_analyzer::aliasInterned("suggestion-file:" + std::string(file_.source));
}

ca::core::Result<std::vector<NameProposal>, std::string> FileSuggestionProvider::propose(
    const RenameContext&)
{
    // 原样产出（provider 不做二次判定，合法性/消解全在引擎）；克隆交付保证
    // provider 可跨 run 复用（同一建议批可回放到多个容器）。
    std::vector<NameProposal> batch;
    batch.reserve(file_.proposals.size());
    for (const auto& proposal : file_.proposals) {
        NameProposal copy(proposal.kind,
                          proposal.owner,
                          proposal.oldName,
                          proposal.desc,
                          proposal.newName,
                          proposal.confidence,
                          proposal.source.empty() ? file_.source : proposal.source,
                          proposal.rationale,
                          proposal.evidence);
        batch.push_back(std::move(copy));
    }
    return ca::core::Ok(std::move(batch));
}

}   // namespace mjbase::renamer
