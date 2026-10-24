#include "edit_ledger.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <utility>

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

ProjectStoreError makeRowError(ca::usize lineNumber, const std::string& message)
{
    return makeError(ProjectStoreErrorKind::Section,
                     "edit-ledger line " + std::to_string(lineNumber) + ": " + message);
}

/// digest 形状校验 + 小写归一（载入面：行内 digest 统一小写存储）。
bool normalizedDigest(const Utf8StringRef& text, Utf8String& out)
{
    const std::string raw = toStd(text);
    if (!isValidLedgerDigest(raw)) return false;
    std::string lowered = raw;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    out = copyOf(lowered);
    return true;
}

/// 必填非空字符串槽。
bool requiredString(const ca::json::JsonValue* slot, const char* field, Utf8String& out,
                    ca::usize line, std::string* message)
{
    if (slot == nullptr) {
        *message = std::string("missing ") + field;
        return false;
    }
    if (!slot->is_string() || slot->as_string().is_empty()) {
        *message = std::string(field) + " must be a non-empty string";
        return false;
    }
    out = copyOf(toStd(slot->as_string()));
    return true;
}

/// 必填 digest 槽（非空 + 形状 + 小写归一）。
bool requiredDigest(const ca::json::JsonValue* slot, const char* field, Utf8String& out,
                    ca::usize line, std::string* message)
{
    if (!requiredString(slot, field, out, line, message)) return false;
    if (!isValidLedgerDigest(toStd(out.ref()))) {
        *message = std::string(field) + " must be \"sha256:\" + 64 hex (got \"" + toStd(out.ref()) +
                   "\")";
        return false;
    }
    Utf8String lowered;
    normalizedDigest(out.ref(), lowered);
    out = std::move(lowered);
    return true;
}

/// 可空 digest 槽（出现即校验；缺席 = ok）。
bool optionalDigest(const ca::json::JsonValue* slot, const char* field, Utf8String& out,
                    ca::usize line, std::string* message)
{
    if (slot == nullptr) return true;
    return requiredDigest(slot, field, out, line, message);
}

const char* opTag(LedgerEntryOp op)
{
    switch (op) {
    case LedgerEntryOp::Replace: return "replace";
    case LedgerEntryOp::Add: return "add";
    case LedgerEntryOp::Remove: return "remove";
    }
    return "replace";
}

bool parseOp(const Utf8StringRef& tag, LedgerEntryOp& op)
{
    if (tag.equals(refOf("replace"))) {
        op = LedgerEntryOp::Replace;
        return true;
    }
    if (tag.equals(refOf("add"))) {
        op = LedgerEntryOp::Add;
        return true;
    }
    if (tag.equals(refOf("remove"))) {
        op = LedgerEntryOp::Remove;
        return true;
    }
    return false;
}

ca::Result<LedgerEntry, ProjectStoreError> parseEntry(const ca::json::JsonValue& value,
                                                      ca::usize                  line,
                                                      const std::string&         context)
{
    if (!value.is_object()) {
        return ca::Err(makeRowError(line, context + "entry must be an object"));
    }
    const ca::json::JsonValue* uri     = nullptr;
    const ca::json::JsonValue* before  = nullptr;
    const ca::json::JsonValue* after   = nullptr;
    const ca::json::JsonValue* op      = nullptr;
    const ca::json::JsonValue* payload = nullptr;
    for (const auto& [key, member] : value.as_object()) {
        const ca::json::JsonValue** slot  = nullptr;
        const char*                 known = nullptr;
        if (key.equals(refOf("uri"))) {
            slot  = &uri;
            known = "uri";
        }
        else if (key.equals(refOf("before"))) {
            slot  = &before;
            known = "before";
        }
        else if (key.equals(refOf("after"))) {
            slot  = &after;
            known = "after";
        }
        else if (key.equals(refOf("op"))) {
            slot  = &op;
            known = "op";
        }
        else if (key.equals(refOf("payload"))) {
            slot  = &payload;
            known = "payload";
        }
        else {
            return ca::Err(makeRowError(line, context + "unknown entry field: " + toStd(key)));
        }
        if (*slot != nullptr) {
            return ca::Err(
                makeRowError(line, context + std::string("duplicate entry field: ") + known));
        }
        *slot = &member;
    }

    LedgerEntry entry;
    std::string message;
    if (!requiredString(uri, "uri", entry.uri, line, &message)) {
        return ca::Err(makeRowError(line, context + message));
    }
    if (op == nullptr || !op->is_string()) {
        return ca::Err(makeRowError(line, context + "missing op"));
    }
    if (!parseOp(op->as_string(), entry.op)) {
        return ca::Err(makeRowError(line, context + "unknown op '" + toStd(op->as_string()) +
                                           "' (expected replace|add|remove)"));
    }
    // 语义侧 digest：缺席/空串合法（Add 的 before、Remove 的 after），出现则
    // 必须是合法 digest 且与 op 相容。
    if (before != nullptr && !before->is_string()) {
        return ca::Err(makeRowError(line, context + "before must be a string"));
    }
    if (after != nullptr && !after->is_string()) {
        return ca::Err(makeRowError(line, context + "after must be a string"));
    }
    if (before != nullptr && !before->as_string().is_empty()) {
        if (!requiredDigest(before, "before", entry.before, line, &message)) {
            return ca::Err(makeRowError(line, context + message));
        }
    }
    if (entry.op == LedgerEntryOp::Add && !entry.before.is_empty()) {
        return ca::Err(makeRowError(line, context + "add entry must not carry before"));
    }
    if (after != nullptr && !after->as_string().is_empty()) {
        if (!requiredDigest(after, "after", entry.after, line, &message)) {
            return ca::Err(makeRowError(line, context + message));
        }
    }
    if (entry.op == LedgerEntryOp::Remove && !entry.after.is_empty()) {
        return ca::Err(makeRowError(line, context + "remove entry must not carry after"));
    }
    if (entry.op == LedgerEntryOp::Remove && payload != nullptr) {
        return ca::Err(makeRowError(line, context + "remove entry must not carry payload"));
    }
    if (entry.op == LedgerEntryOp::Replace &&
        (entry.before.is_empty() || entry.after.is_empty())) {
        return ca::Err(makeRowError(line, context + "replace entry requires before and after"));
    }
    if (entry.op == LedgerEntryOp::Add && entry.after.is_empty()) {
        return ca::Err(makeRowError(line, context + "add entry requires after"));
    }
    if (entry.op == LedgerEntryOp::Remove && entry.before.is_empty()) {
        return ca::Err(makeRowError(line, context + "remove entry requires before"));
    }

    if (entry.op != LedgerEntryOp::Remove) {
        if (payload == nullptr) {
            return ca::Err(makeRowError(line, context + "missing payload"));
        }
        if (!payload->is_object()) {
            return ca::Err(makeRowError(line, context + "payload must be an object"));
        }
        const ca::json::JsonValue* kind = nullptr;
        const ca::json::JsonValue* ref  = nullptr;
        for (const auto& [key, member] : payload->as_object()) {
            if (key.equals(refOf("kind"))) {
                kind = &member;
            }
            else if (key.equals(refOf("ref"))) {
                ref = &member;
            }
            else {
                return ca::Err(
                    makeRowError(line, context + "unknown payload field: " + toStd(key)));
            }
        }
        if (kind == nullptr || !kind->is_string()) {
            return ca::Err(makeRowError(line, context + "missing payload kind"));
        }
        // 未知 payload kind 拒载：引擎解不开的载荷不能假装能重放（fail-closed）。
        if (!kind->as_string().equals(refOf("overlay"))) {
            return ca::Err(makeRowError(line, context + "unknown payload kind '" +
                                                    toStd(kind->as_string()) + "'"));
        }
        if (!requiredString(ref, "payload ref", entry.payloadRef, line, &message)) {
            return ca::Err(makeRowError(line, context + message));
        }
    }
    return ca::Ok(std::move(entry));
}

ca::Result<LedgerRow, ProjectStoreError> parseTypedRow(const ca::json::JsonValue& root,
                                                       const ca::json::JsonValue& rowTag,
                                                       ca::usize                  line)
{
    LedgerRow parsed;
    if (rowTag.as_string().equals(refOf("txn"))) {
        parsed.kind = LedgerRow::Kind::Transaction;
        const ca::json::JsonValue* txn        = nullptr;
        const ca::json::JsonValue* parent     = nullptr;
        const ca::json::JsonValue* label      = nullptr;
        const ca::json::JsonValue* base       = nullptr;
        const ca::json::JsonValue* entries    = nullptr;
        const ca::json::JsonValue* producedBy = nullptr;
        const ca::json::JsonValue* createdAt  = nullptr;
        for (const auto& [key, member] : root.as_object()) {
            const ca::json::JsonValue** slot  = nullptr;
            const char*                 known = nullptr;
            if (key.equals(refOf("v")) || key.equals(refOf("row"))) continue;
            if (key.equals(refOf("txn"))) {
                slot  = &txn;
                known = "txn";
            }
            else if (key.equals(refOf("parent"))) {
                slot  = &parent;
                known = "parent";
            }
            else if (key.equals(refOf("label"))) {
                slot  = &label;
                known = "label";
            }
            else if (key.equals(refOf("base_container"))) {
                slot  = &base;
                known = "base_container";
            }
            else if (key.equals(refOf("entries"))) {
                slot  = &entries;
                known = "entries";
            }
            else if (key.equals(refOf("produced_by"))) {
                slot  = &producedBy;
                known = "produced_by";
            }
            else if (key.equals(refOf("created_at"))) {
                slot  = &createdAt;
                known = "created_at";
            }
            else {
                return ca::Err(makeRowError(line, "unknown txn row field: " + toStd(key)));
            }
            if (*slot != nullptr) {
                return ca::Err(
                    makeRowError(line, std::string("duplicate txn row field: ") + known));
            }
            *slot = &member;
        }
        auto& txnRow = parsed.transaction;
        if (txn == nullptr || !txn->is_int() || txn->as_int() < 1) {
            return ca::Err(makeRowError(line, "txn must be an integer >= 1"));
        }
        txnRow.txn = static_cast<ca::u64>(txn->as_int());
        if (parent != nullptr) {
            if (!parent->is_int() || parent->as_int() < 0) {
                return ca::Err(makeRowError(line, "parent must be an integer >= 0"));
            }
            txnRow.parent = static_cast<ca::u64>(parent->as_int());
        }
        std::string message;
        if (label != nullptr) {
            if (!label->is_string()) {
                return ca::Err(makeRowError(line, "label must be a string"));
            }
            txnRow.label = copyOf(toStd(label->as_string()));
        }
        if (!requiredDigest(base, "base_container", txnRow.baseContainer, line, &message)) {
            return ca::Err(makeRowError(line, message));
        }
        if (entries == nullptr || !entries->is_array()) {
            return ca::Err(makeRowError(line, "entries must be an array"));
        }
        for (const auto& item : entries->as_array()) {
            auto entry = parseEntry(item, line, "");
            if (entry.is_err()) return ca::Err(std::move(entry).unwrap_err());
            txnRow.entries.push_back(std::move(entry).unwrap());
        }
        if (!requiredString(producedBy, "produced_by", txnRow.producedBy, line, &message)) {
            return ca::Err(makeRowError(line, message));
        }
        if (!requiredString(createdAt, "created_at", txnRow.createdAt, line, &message)) {
            return ca::Err(makeRowError(line, message));
        }
        return ca::Ok(std::move(parsed));
    }
    if (rowTag.as_string().equals(refOf("anchor"))) {
        parsed.kind = LedgerRow::Kind::Anchor;
        const ca::json::JsonValue* txn       = nullptr;
        const ca::json::JsonValue* baseline  = nullptr;
        const ca::json::JsonValue* container = nullptr;
        const ca::json::JsonValue* previous  = nullptr;
        const ca::json::JsonValue* compacted = nullptr;
        const ca::json::JsonValue* createdAt = nullptr;
        for (const auto& [key, member] : root.as_object()) {
            const ca::json::JsonValue** slot  = nullptr;
            const char*                 known = nullptr;
            if (key.equals(refOf("v")) || key.equals(refOf("row"))) continue;
            if (key.equals(refOf("txn"))) {
                slot  = &txn;
                known = "txn";
            }
            else if (key.equals(refOf("baseline"))) {
                slot  = &baseline;
                known = "baseline";
            }
            else if (key.equals(refOf("container"))) {
                slot  = &container;
                known = "container";
            }
            else if (key.equals(refOf("previous"))) {
                slot  = &previous;
                known = "previous";
            }
            else if (key.equals(refOf("compacted_txns"))) {
                slot  = &compacted;
                known = "compacted_txns";
            }
            else if (key.equals(refOf("created_at"))) {
                slot  = &createdAt;
                known = "created_at";
            }
            else {
                return ca::Err(makeRowError(line, "unknown anchor row field: " + toStd(key)));
            }
            if (*slot != nullptr) {
                return ca::Err(
                    makeRowError(line, std::string("duplicate anchor row field: ") + known));
            }
            *slot = &member;
        }
        auto& anchor = parsed.anchor;
        if (txn == nullptr || !txn->is_int() || txn->as_int() < 0) {
            return ca::Err(makeRowError(line, "anchor txn must be an integer >= 0"));
        }
        anchor.txn = static_cast<ca::u64>(txn->as_int());
        std::string message;
        if (!requiredString(baseline, "baseline", anchor.baselineSection, line, &message)) {
            return ca::Err(makeRowError(line, message));
        }
        if (!requiredDigest(container, "container", anchor.container, line, &message)) {
            return ca::Err(makeRowError(line, message));
        }
        if (previous != nullptr) {
            if (!previous->is_object()) {
                return ca::Err(makeRowError(line, "previous must be an object"));
            }
            for (const auto& [key, member] : previous->as_object()) {
                if (key.equals(refOf("txn"))) {
                    if (!member.is_int() || member.as_int() < 0) {
                        return ca::Err(makeRowError(line, "previous.txn must be an integer >= 0"));
                    }
                    anchor.previousTxn = static_cast<ca::u64>(member.as_int());
                }
                else if (key.equals(refOf("container"))) {
                    if (!requiredDigest(&member, "previous.container", anchor.previousContainer,
                                        line, &message)) {
                        return ca::Err(makeRowError(line, message));
                    }
                }
                else {
                    return ca::Err(
                        makeRowError(line, "unknown previous field: " + toStd(key)));
                }
            }
            if (anchor.previousTxn == 0 || anchor.previousContainer.is_empty()) {
                return ca::Err(
                    makeRowError(line, "previous requires txn and container"));
            }
        }
        if (compacted != nullptr) {
            if (!compacted->is_int() || compacted->as_int() < 0) {
                return ca::Err(makeRowError(line, "compacted_txns must be an integer >= 0"));
            }
            anchor.compactedTxns = static_cast<ca::u64>(compacted->as_int());
        }
        if (!requiredString(createdAt, "created_at", anchor.createdAt, line, &message)) {
            return ca::Err(makeRowError(line, message));
        }
        return ca::Ok(std::move(parsed));
    }
    if (rowTag.as_string().equals(refOf("bookmark"))) {
        parsed.kind = LedgerRow::Kind::Bookmark;
        const ca::json::JsonValue* name      = nullptr;
        const ca::json::JsonValue* txn       = nullptr;
        const ca::json::JsonValue* createdAt = nullptr;
        for (const auto& [key, member] : root.as_object()) {
            const ca::json::JsonValue** slot  = nullptr;
            const char*                 known = nullptr;
            if (key.equals(refOf("v")) || key.equals(refOf("row"))) continue;
            if (key.equals(refOf("name"))) {
                slot  = &name;
                known = "name";
            }
            else if (key.equals(refOf("txn"))) {
                slot  = &txn;
                known = "txn";
            }
            else if (key.equals(refOf("created_at"))) {
                slot  = &createdAt;
                known = "created_at";
            }
            else {
                return ca::Err(makeRowError(line, "unknown bookmark row field: " + toStd(key)));
            }
            if (*slot != nullptr) {
                return ca::Err(
                    makeRowError(line, std::string("duplicate bookmark row field: ") + known));
            }
            *slot = &member;
        }
        auto& bookmark = parsed.bookmark;
        std::string    message;
        if (!requiredString(name, "name", bookmark.name, line, &message)) {
            return ca::Err(makeRowError(line, message));
        }
        if (txn == nullptr || !txn->is_int() || txn->as_int() < 0) {
            return ca::Err(makeRowError(line, "bookmark txn must be an integer >= 0"));
        }
        bookmark.txn = static_cast<ca::u64>(txn->as_int());
        if (!requiredString(createdAt, "created_at", bookmark.createdAt, line, &message)) {
            return ca::Err(makeRowError(line, message));
        }
        return ca::Ok(std::move(parsed));
    }
    return ca::Err(makeRowError(line, "unknown row type '" + toStd(rowTag.as_string()) +
                                           "' (expected txn|anchor|bookmark)"));
}

ca::json::JsonValue entryJson(const LedgerEntry& entry)
{
    ca::json::JsonValue::ObjectStorage members;
    members.emplace_back(refOf("uri"), ca::json::JsonValue::make_string(entry.uri.ref()));
    if (entry.op != LedgerEntryOp::Add) {
        members.emplace_back(refOf("before"),
                             ca::json::JsonValue::make_string(entry.before.ref()));
    }
    if (entry.op != LedgerEntryOp::Remove) {
        members.emplace_back(refOf("after"), ca::json::JsonValue::make_string(entry.after.ref()));
        ca::json::JsonValue::ObjectStorage payloadMembers;
        payloadMembers.emplace_back(refOf("kind"), ca::json::JsonValue::make_string(refOf("overlay")));
        payloadMembers.emplace_back(refOf("ref"),
                                    ca::json::JsonValue::make_string(entry.payloadRef.ref()));
        members.emplace_back(refOf("payload"),
                             ca::json::JsonValue::make_object_from(std::move(payloadMembers)));
    }
    members.emplace_back(refOf("op"), ca::json::JsonValue::make_string(refOf(opTag(entry.op))));
    return ca::json::JsonValue::make_object_from(std::move(members));
}

ca::Result<ca::str::Utf8String, ProjectStoreError> serializeRow(const LedgerRow& row)
{
    ca::json::JsonValue::ObjectStorage members;
    members.emplace_back(refOf("v"), ca::json::JsonValue::make_int(
                                          static_cast<ca::i64>(kLedgerRowVersion)));
    switch (row.kind) {
    case LedgerRow::Kind::Transaction: {
        const auto& txn = row.transaction;
        members.emplace_back(refOf("row"), ca::json::JsonValue::make_string(refOf("txn")));
        members.emplace_back(refOf("txn"), ca::json::JsonValue::make_int(
                                                static_cast<ca::i64>(txn.txn)));
        members.emplace_back(refOf("parent"), ca::json::JsonValue::make_int(
                                                   static_cast<ca::i64>(txn.parent)));
        if (!txn.label.is_empty()) {
            members.emplace_back(refOf("label"),
                                 ca::json::JsonValue::make_string(txn.label.ref()));
        }
        members.emplace_back(refOf("base_container"),
                             ca::json::JsonValue::make_string(txn.baseContainer.ref()));
        auto entries = ca::json::JsonValue::make_array();
        for (const auto& entry : txn.entries) entries.append(entryJson(entry));
        members.emplace_back(refOf("entries"), std::move(entries));
        members.emplace_back(refOf("produced_by"),
                             ca::json::JsonValue::make_string(txn.producedBy.ref()));
        members.emplace_back(refOf("created_at"),
                             ca::json::JsonValue::make_string(txn.createdAt.ref()));
        break;
    }
    case LedgerRow::Kind::Anchor: {
        const auto& anchor = row.anchor;
        members.emplace_back(refOf("row"), ca::json::JsonValue::make_string(refOf("anchor")));
        members.emplace_back(refOf("txn"), ca::json::JsonValue::make_int(
                                                static_cast<ca::i64>(anchor.txn)));
        members.emplace_back(refOf("baseline"),
                             ca::json::JsonValue::make_string(anchor.baselineSection.ref()));
        members.emplace_back(refOf("container"),
                             ca::json::JsonValue::make_string(anchor.container.ref()));
        if (anchor.previousTxn != 0) {
            ca::json::JsonValue::ObjectStorage previousMembers;
            previousMembers.emplace_back(
                refOf("txn"), ca::json::JsonValue::make_int(
                                   static_cast<ca::i64>(anchor.previousTxn)));
            previousMembers.emplace_back(
                refOf("container"),
                ca::json::JsonValue::make_string(anchor.previousContainer.ref()));
            members.emplace_back(refOf("previous"),
                                 ca::json::JsonValue::make_object_from(
                                     std::move(previousMembers)));
        }
        members.emplace_back(refOf("compacted_txns"),
                             ca::json::JsonValue::make_int(
                                 static_cast<ca::i64>(anchor.compactedTxns)));
        members.emplace_back(refOf("created_at"),
                             ca::json::JsonValue::make_string(anchor.createdAt.ref()));
        break;
    }
    case LedgerRow::Kind::Bookmark: {
        const auto& bookmark = row.bookmark;
        members.emplace_back(refOf("row"), ca::json::JsonValue::make_string(refOf("bookmark")));
        members.emplace_back(refOf("name"),
                             ca::json::JsonValue::make_string(bookmark.name.ref()));
        members.emplace_back(refOf("txn"), ca::json::JsonValue::make_int(
                                                static_cast<ca::i64>(bookmark.txn)));
        members.emplace_back(refOf("created_at"),
                             ca::json::JsonValue::make_string(bookmark.createdAt.ref()));
        break;
    }
    }
    ca::json::JsonDocument document;
    document.root() = ca::json::JsonValue::make_object_from(std::move(members));
    ca::json::JsonWriterOptions options;
    options.pretty = false;
    return ca::Ok(ca::json::JsonWriter::write(document, options));
}

}   // namespace

bool isValidLedgerDigest(std::string_view text)
{
    constexpr std::string_view kPrefix = "sha256:";
    if (text.size() != kPrefix.size() + 64) return false;
    if (text.substr(0, kPrefix.size()) != kPrefix) return false;
    for (const char c : text.substr(kPrefix.size())) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

ca::Result<LedgerRow, ProjectStoreError> parseLedgerRow(std::string_view line, ca::usize lineNumber)
{
    auto document = ca::json::JsonReader::read(Utf8StringRef::from_string_view(line));
    if (document.is_err()) {
        auto       parseError = std::move(document).unwrap_err();
        const auto message    = "not valid JSON at column " +
                                std::to_string(parseError.location.column) + ": " +
                                toStd(parseError.message.ref());
        return ca::Err(makeRowError(lineNumber, message));
    }
    auto        parsedDocument = std::move(document).unwrap();
    const auto& root           = parsedDocument.root();
    if (!root.is_object()) {
        return ca::Err(makeRowError(lineNumber, "row must be a JSON object"));
    }
    const ca::json::JsonValue* version = nullptr;
    const ca::json::JsonValue* rowTag  = nullptr;
    for (const auto& [key, member] : root.as_object()) {
        if (key.equals(refOf("v"))) {
            version = &member;
        }
        else if (key.equals(refOf("row"))) {
            rowTag = &member;
        }
    }
    if (version == nullptr || !version->is_int()) {
        return ca::Err(makeRowError(lineNumber, "missing row version v"));
    }
    if (version->as_int() != static_cast<ca::i64>(kLedgerRowVersion)) {
        return ca::Err(makeRowError(lineNumber, "unsupported row version " +
                                                     std::to_string(version->as_int()) +
                                                     " (supported: " +
                                                     std::to_string(kLedgerRowVersion) + ")"));
    }
    if (rowTag == nullptr || !rowTag->is_string()) {
        return ca::Err(makeRowError(lineNumber, "missing row type"));
    }
    return parseTypedRow(root, *rowTag, lineNumber);
}

ca::Result<ca::str::Utf8String, ProjectStoreError> serializeLedgerRow(const LedgerRow& row)
{
    return serializeRow(row);
}

ca::Result<std::vector<LedgerRow>, ProjectStoreError> parseLedgerRows(
    const Utf8StringRef& text)
{
    const std::string body = toStd(text);
    std::vector<LedgerRow> rows;
    ca::usize              lineNumber = 0;
    ca::usize              start      = 0;
    while (start <= body.size()) {
        ca::usize end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        ++lineNumber;
        if (end > start) {
            const std::string lineText = body.substr(start, end - start);
            auto              parsed   = parseLedgerRow(lineText, lineNumber);
            if (parsed.is_err()) return ca::Err(std::move(parsed).unwrap_err());
            rows.push_back(std::move(parsed).unwrap());
        }
        if (end == body.size()) break;
        start = end + 1;
    }
    return ca::Ok(std::move(rows));
}

ca::Result<ca::str::Utf8String, ProjectStoreError> serializeLedgerRows(
    const std::vector<LedgerRow>& rows)
{
    std::string body;
    for (const auto& row : rows) {
        auto serialized = serializeRow(row);
        if (serialized.is_err()) return ca::Err(std::move(serialized).unwrap_err());
        const auto line = std::move(serialized).unwrap();
        body.append(reinterpret_cast<const char*>(line.data()), line.byte_length());
        body.push_back('\n');
    }
    return ca::Ok(copyOf(body));
}

void sortLedgerRows(std::vector<LedgerRow>& rows)
{
    std::stable_sort(rows.begin(), rows.end(), [](const LedgerRow& a, const LedgerRow& b) {
        auto rank = [](const LedgerRow& row) {
            switch (row.kind) {
            case LedgerRow::Kind::Anchor: return 0;
            case LedgerRow::Kind::Transaction: return 1;
            case LedgerRow::Kind::Bookmark: return 2;
            }
            return 3;
        };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        if (rank(a) == 1) return a.transaction.txn < b.transaction.txn;
        if (rank(a) == 2) return toStd(a.bookmark.name.ref()) < toStd(b.bookmark.name.ref());
        return false;
    });
}

ca::Result<void, ProjectStoreError> validateLedgerChain(const std::vector<LedgerRow>& rows)
{
    std::set<ca::u64> txnIds;
    ca::usize         lineNumber = 0;
    bool              anchorSeen = false;
    ca::u64           minTxn     = 0;
    for (const auto& row : rows) {
        ++lineNumber;
        if (row.kind == LedgerRow::Kind::Anchor) {
            if (lineNumber != 1) {
                return ca::Err(makeRowError(lineNumber, "anchor row must be the first row"));
            }
            anchorSeen = true;
            minTxn     = row.anchor.txn;
            continue;
        }
        if (row.kind == LedgerRow::Kind::Transaction) {
            const auto& txn = row.transaction;
            if (!txnIds.insert(txn.txn).second) {
                return ca::Err(makeRowError(lineNumber, "duplicate txn id: " +
                                                            std::to_string(txn.txn)));
            }
            // parent 指向段内更早事务、锚事务（压缩后的链基点 = 基线态）或 0。
            if (txn.parent != 0 && txnIds.count(txn.parent) == 0 &&
                !(anchorSeen && txn.parent == minTxn)) {
                return ca::Err(makeRowError(lineNumber, "txn " + std::to_string(txn.txn) +
                                                            " references unresolved parent " +
                                                            std::to_string(txn.parent)));
            }
            // 行序按 txn 升序装载后，parent 必须已出现过（更早行）。
            if (txn.parent != 0 && txn.parent >= txn.txn) {
                return ca::Err(makeRowError(lineNumber, "txn " + std::to_string(txn.txn) +
                                                            " parent must precede it"));
            }
            if (anchorSeen && txn.txn <= minTxn) {
                return ca::Err(makeRowError(lineNumber,
                                            "txn " + std::to_string(txn.txn) +
                                                " must be newer than anchor " +
                                                std::to_string(minTxn)));
            }
            continue;
        }
        // 书签：txn 必须指向段内事务（锚事务也在段内语义中——书签指向基线态）。
        if (row.bookmark.txn != 0 && row.bookmark.txn != minTxn &&
            txnIds.count(row.bookmark.txn) == 0) {
            return ca::Err(makeRowError(lineNumber, "bookmark '" + toStd(row.bookmark.name.ref()) +
                                                        "' references unknown txn " +
                                                        std::to_string(row.bookmark.txn)));
        }
    }
    return ca::Ok();
}

ca::str::Utf8String overlaySectionName(ca::u64 txn, ca::u32 seq)
{
    return copyOf(std::string(kOverlaySectionPrefix) + std::to_string(txn) + "-" +
                  std::to_string(seq));
}

ca::str::Utf8String copyLedgerText(std::string_view text)
{
    return ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(text.data()),
                                          text.size());
}

void ContainerDigestBuilder::feed(const ca::u8* data, ca::usize size)
{
    if (size > 0) digest_.add(data, static_cast<std::size_t>(size));
}

void ContainerDigestBuilder::feedLength(ca::usize value)
{
    const auto wide = static_cast<ca::u64>(value);
    for (ca::usize index = 0; index < sizeof(wide); ++index) {
        const auto byte = static_cast<ca::u8>((wide >> (index * 8U)) & 0xffU);
        feed(&byte, 1);
    }
}

void ContainerDigestBuilder::add(std::string_view uri, const ca::u8* payload,
                                 ca::usize payloadSize)
{
    feedLength(uri.size());
    feed(reinterpret_cast<const ca::u8*>(uri.data()), uri.size());
    feedLength(payloadSize);
    feed(payload, payloadSize);
    ++entries_;
}

ca::str::Utf8String ContainerDigestBuilder::finish()
{
    std::string hex = digest_.get_hash();
    for (char& c : hex) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return copyOf("sha256:" + hex);
}

std::vector<ca::u8> serializeContainerEntries(
    const std::vector<std::pair<ca::str::Utf8String, std::vector<ca::u8>>>& sortedEntries)
{
    // canonical 字节流：len(uri, u64 LE) + uri + len(payload, u64 LE) + payload
    // 逐项拼接（与 ContainerDigestBuilder 同一拼接规则——digest 对本字节序列
    // 求 SHA-256，基线 section 落盘字节即本序列）。载荷字节任意，序列是二进制
    // 而非 UTF-8 文本，返回字节向量。
    std::string body;
    auto        feedBytes = [&body](const ca::u8* data, ca::usize size) {
        body.append(reinterpret_cast<const char*>(data), size);
    };
    auto feedLength = [&body](ca::usize value) {
        const auto wide = static_cast<ca::u64>(value);
        for (ca::usize index = 0; index < sizeof(wide); ++index) {
            const auto byte = static_cast<ca::u8>((wide >> (index * 8U)) & 0xffU);
            body.push_back(static_cast<char>(byte));
        }
    };
    for (const auto& [uri, payload] : sortedEntries) {
        feedLength(uri.byte_length());
        feedBytes(uri.data(), uri.byte_length());
        feedLength(payload.size());
        feedBytes(payload.data(), payload.size());
    }
    return std::vector<ca::u8>(reinterpret_cast<const ca::u8*>(body.data()),
                               reinterpret_cast<const ca::u8*>(body.data()) + body.size());
}

ca::Result<void, ProjectStoreError> forEachContainerEntry(
    const ca::u8* data, ca::usize size,
    const std::function<bool(std::string_view uri, const ca::u8* payload,
                             ca::usize payloadSize)>& visitor)
{
    ca::usize cursor = 0;
    auto readLength = [&](ca::usize* out) -> bool {
        if (cursor + sizeof(ca::u64) > size) return false;
        ca::u64 wide = 0;
        for (ca::usize index = 0; index < sizeof(wide); ++index) {
            wide |= static_cast<ca::u64>(data[cursor + index]) << (index * 8U);
        }
        cursor += sizeof(wide);
        *out = static_cast<ca::usize>(wide);
        return true;
    };
    while (cursor < size) {
        ca::usize uriLength = 0;
        ca::usize payloadLength = 0;
        if (!readLength(&uriLength) || cursor + uriLength > size) {
            return ca::Err(makeError(ProjectStoreErrorKind::Section,
                                     "canonical container bytes are truncated (uri header)"));
        }
        const std::string uri(reinterpret_cast<const char*>(data) + cursor, uriLength);
        cursor += uriLength;
        if (!readLength(&payloadLength) || cursor + payloadLength > size) {
            return ca::Err(makeError(ProjectStoreErrorKind::Section,
                                     "canonical container bytes are truncated (payload header)"));
        }
        const ca::u8* payload = data + cursor;
        cursor += payloadLength;
        if (!visitor(uri, payload, payloadLength)) return ca::Ok();
    }
    return ca::Ok();
}

ca::Result<bool, ProjectStoreError> findContainerEntry(const ca::u8* data, ca::usize size,
                                                       std::string_view             uri,
                                                       std::vector<ca::u8>& payload)
{
    bool found = false;
    auto visitor = [&](std::string_view entryUri, const ca::u8* data2, ca::usize payloadSize) {
        if (entryUri == uri) {
            payload.assign(data2, data2 + payloadSize);
            found = true;
            return false;
        }
        return true;
    };
    auto scanned = forEachContainerEntry(data, size, visitor);
    if (scanned.is_err()) return ca::Err(std::move(scanned).unwrap_err());
    return ca::Ok(found);
}

}   // namespace mjbase::project
