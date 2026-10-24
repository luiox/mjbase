// 编辑台账行模型（schema v2 交付 2 的纯数据层）：三型行（txn/anchor/bookmark）
// 的严格解析与确定性写出、行级违约逐类拒载、活动链结构校验、段内定序、
// 容器 canonical 序列化与 digest 口径（容器 digest = 基线 section
// content_hash，一口径两处验证）。
//
// 设计定稿见 doc_ai/analysis-project-schema-v2.md §1.6/§2（上游 RFC 0001
// §6.2/§6.4）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mjbase/project/edit_ledger.hpp"
#include "test_helper.hpp"

using namespace mjbase::project;
using ca::str::Utf8String;
using ca::str::Utf8StringRef;

namespace {

Utf8StringRef S(const char* value)
{
    return Utf8StringRef::from_cstr(value);
}

Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

Utf8String makeStr(const std::string& value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

/// "sha256:" + 64 位 hex（用重复字符区分不同条目；测试面合法形状即可）。
std::string hashOf(char fill)
{
    return "sha256:" + std::string(64, fill);
}

LedgerEntry makeReplaceEntry(const char* uri, char beforeFill, char afterFill, const char* ref)
{
    LedgerEntry entry;
    entry.uri        = Utf8String::from_cstr(uri);
    entry.before     = Utf8String::from_cstr(hashOf(beforeFill).c_str());
    entry.after      = Utf8String::from_cstr(hashOf(afterFill).c_str());
    entry.op         = LedgerEntryOp::Replace;
    entry.payloadRef = Utf8String::from_cstr(ref);
    return entry;
}

LedgerRow makeTxnRow(ca::u64 txn, ca::u64 parent)
{
    LedgerRow row;
    row.kind               = LedgerRow::Kind::Transaction;
    row.transaction.txn    = txn;
    row.transaction.parent = parent;
    row.transaction.label  = Utf8String::from_cstr("apply JASM");
    row.transaction.baseContainer = Utf8String::from_cstr(hashOf('a').c_str());
    row.transaction.entries.push_back(makeReplaceEntry("a.class", '1', '2', "overlay-1-0"));
    row.transaction.producedBy = Utf8String::from_cstr("mjbase-workspace/1");
    row.transaction.createdAt  = Utf8String::from_cstr("2026-10-23T00:00:00Z");
    return row;
}

LedgerRow makeBookmarkRow(const char* name, ca::u64 txn)
{
    LedgerRow row;
    row.kind             = LedgerRow::Kind::Bookmark;
    row.bookmark.name    = Utf8String::from_cstr(name);
    row.bookmark.txn     = txn;
    row.bookmark.createdAt = Utf8String::from_cstr("2026-10-23T00:00:01Z");
    return row;
}

LedgerRow makeAnchorRow(ca::u64 txn)
{
    LedgerRow row;
    row.kind                       = LedgerRow::Kind::Anchor;
    row.anchor.txn                 = txn;
    row.anchor.baselineSection     = Utf8String::from_cstr("edit-baseline-1");
    row.anchor.container           = Utf8String::from_cstr(hashOf('b').c_str());
    row.anchor.previousTxn         = 0;
    row.anchor.compactedTxns       = 2;
    row.anchor.createdAt           = Utf8String::from_cstr("2026-10-23T00:00:02Z");
    return row;
}

/// 行集合装配（LedgerRow move-only，initializer_list 会拷贝，禁止）。
void addRows(std::vector<LedgerRow>& rows, LedgerRow row) { rows.push_back(std::move(row)); }

/// 解析错误断言：期望 Section kind + 返回错误消息文本。
std::string expectRowError(const std::string& line, ca::usize lineNumber = 1)
{
    auto parsed = parseLedgerRow(line, lineNumber);
    EXPECT_TRUE(parsed.is_err()) << "accepted: " << line;
    if (parsed.is_err()) {
        const auto  err = std::move(parsed).unwrap_err();
        EXPECT_EQ(ProjectStoreErrorKind::Section, err.kind) << err.message.to_std_string();
        return err.message.to_std_string();
    }
    return {};
}

}   // namespace

TEST(MjLedgerRowTest, transactionRowRoundtripsByteExactly)
{
    auto row  = makeTxnRow(3, 2);
    auto text = serializeLedgerRow(row);
    ASSERT_FALSE(text.is_err());
    const auto  body = std::move(text).unwrap().to_std_string();
    EXPECT_NE(std::string::npos, body.find("\"row\":\"txn\""));
    EXPECT_NE(std::string::npos, body.find("\"v\":1"));
    EXPECT_NE(std::string::npos, body.find("\"base_container\""));
    EXPECT_NE(std::string::npos, body.find("\"payload\":{\"kind\":\"overlay\",\"ref\":\"overlay-1-0\"}"));

    auto parsed = parseLedgerRow(body, 1);
    ASSERT_FALSE(parsed.is_err());
    const auto& back = std::move(parsed).unwrap();
    ASSERT_EQ(LedgerRow::Kind::Transaction, back.kind);
    EXPECT_EQ(3U, back.transaction.txn);
    EXPECT_EQ(2U, back.transaction.parent);
    EXPECT_EQ("apply JASM", back.transaction.label.to_std_string());
    EXPECT_EQ(1U, back.transaction.entries.size());
    EXPECT_EQ("a.class", back.transaction.entries[0].uri.to_std_string());
    EXPECT_EQ(LedgerEntryOp::Replace, back.transaction.entries[0].op);
    EXPECT_EQ("overlay-1-0", back.transaction.entries[0].payloadRef.to_std_string());

    // 往返再写出逐字节一致（确定性口径）。
    auto rewritten = serializeLedgerRow(back);
    ASSERT_FALSE(rewritten.is_err());
    EXPECT_EQ(body, std::move(rewritten).unwrap().to_std_string());
}

TEST(MjLedgerRowTest, addAndRemoveEntryShapesRoundtrip)
{
    // Add：before 恒缺省（写出省略）；Remove：after/payload 恒缺省。
    LedgerRow row = makeTxnRow(1, 0);
    row.transaction.entries.clear();

    LedgerEntry added;
    added.uri        = Utf8String::from_cstr("a.class");
    added.after      = Utf8String::from_cstr(hashOf('2').c_str());
    added.op         = LedgerEntryOp::Add;
    added.payloadRef = Utf8String::from_cstr("overlay-1-0");
    row.transaction.entries.push_back(std::move(added));

    LedgerEntry removed;
    removed.uri    = Utf8String::from_cstr("b.class");
    removed.before = Utf8String::from_cstr(hashOf('1').c_str());
    removed.op     = LedgerEntryOp::Remove;
    row.transaction.entries.push_back(std::move(removed));

    auto text = serializeLedgerRow(row);
    ASSERT_FALSE(text.is_err());
    const auto  body = std::move(text).unwrap().to_std_string();
    auto parsed = parseLedgerRow(body, 1);
    ASSERT_FALSE(parsed.is_err());
    const auto& back = std::move(parsed).unwrap();
    ASSERT_EQ(2U, back.transaction.entries.size());
    EXPECT_TRUE(back.transaction.entries[0].before.is_empty());
    EXPECT_EQ(LedgerEntryOp::Add, back.transaction.entries[0].op);
    EXPECT_TRUE(back.transaction.entries[1].after.is_empty());
    EXPECT_TRUE(back.transaction.entries[1].payloadRef.is_empty());
    EXPECT_EQ(LedgerEntryOp::Remove, back.transaction.entries[1].op);
}

TEST(MjLedgerRowTest, anchorAndBookmarkRowsRoundtrip)
{
    auto anchorText = serializeLedgerRow(makeAnchorRow(4));
    ASSERT_FALSE(anchorText.is_err());
    const auto  anchorBody = std::move(anchorText).unwrap().to_std_string();
    EXPECT_NE(std::string::npos, anchorBody.find("\"row\":\"anchor\""));
    EXPECT_NE(std::string::npos, anchorBody.find("\"baseline\":\"edit-baseline-1\""));
    EXPECT_NE(std::string::npos, anchorBody.find("\"compacted_txns\":2"));
    // 首次压缩（previous_txn = 0）不携带 previous 对象。
    EXPECT_EQ(std::string::npos, anchorBody.find("\"previous\""));

    auto anchorParsed = parseLedgerRow(anchorBody, 1);
    ASSERT_FALSE(anchorParsed.is_err());
    EXPECT_EQ(LedgerRow::Kind::Anchor, std::move(anchorParsed).unwrap().kind);

    // 带补链的锚（previous 对象）。
    auto chained              = makeAnchorRow(9);
    chained.anchor.previousTxn = 4;
    chained.anchor.previousContainer = Utf8String::from_cstr(hashOf('b').c_str());
    auto chainedText = serializeLedgerRow(chained);
    ASSERT_FALSE(chainedText.is_err());
    auto chainedParsed = parseLedgerRow(std::move(chainedText).unwrap().to_std_string(), 1);
    ASSERT_FALSE(chainedParsed.is_err());
    EXPECT_EQ(4U, std::move(chainedParsed).unwrap().anchor.previousTxn);

    auto bookmarkText = serializeLedgerRow(makeBookmarkRow("before-edit", 3));
    ASSERT_FALSE(bookmarkText.is_err());
    auto bookmarkParsed = parseLedgerRow(std::move(bookmarkText).unwrap().to_std_string(), 1);
    ASSERT_FALSE(bookmarkParsed.is_err());
    const auto& bookmark = std::move(bookmarkParsed).unwrap();
    ASSERT_EQ(LedgerRow::Kind::Bookmark, bookmark.kind);
    EXPECT_EQ("before-edit", bookmark.bookmark.name.to_std_string());
    EXPECT_EQ(3U, bookmark.bookmark.txn);
}

TEST(MjLedgerRowTest, strictParseRejectsViolationsByClass)
{
    const std::string digestA = hashOf('1');
    const std::string digestB = hashOf('2');
    const std::string txnLine =
        std::string("{\"v\":1,\"row\":\"txn\",\"txn\":1,\"parent\":0,"
                    "\"base_container\":\"") + digestA + "\","
                    "\"entries\":[{\"uri\":\"a.class\",\"before\":\"" + digestA +
                    "\",\"after\":\"" + digestB +
                    "\",\"payload\":{\"kind\":\"overlay\",\"ref\":\"overlay-1-0\"},"
                    "\"op\":\"replace\"}],"
                    "\"produced_by\":\"mjbase-workspace/1\",\"created_at\":\"2026\"}";

    struct Violation
    {
        const char*    name;
        std::string    before;
        std::string    after;
    };
    const std::vector<Violation> violations = {
        {"bad digest shape",
         "\"" + digestA + "\",\"after",
         "\"sha256:zz\",\"after"},
        {"unknown payload kind",
         "\"payload\":{\"kind\":\"overlay\"",
         "\"payload\":{\"kind\":\"inline\""},
        {"unknown entry field",
         "\"uri\":\"a.class\",\"before",
         "\"uri\":\"a.class\",\"surprise\":1,\"before"},
        {"unknown row field",
         "\"parent\":0,\"base_container\"",
         "\"parent\":0,\"mood\":\"fine\",\"base_container\""},
        {"unknown op",
         "\"op\":\"replace\"}",
         "\"op\":\"mutate\"}"},
    };
    for (const auto& violation : violations) {
        const auto  at = txnLine.find(violation.before);
        ASSERT_NE(std::string::npos, at) << violation.name << ": anchor missing";
        auto broken = txnLine;
        broken.replace(at, violation.before.size(), violation.after);
        expectRowError(broken);
    }

    // remove 条目携带 payload 拒载（Remove 恒无 payload）。
    const std::string removeWithPayload =
        std::string("{\"v\":1,\"row\":\"txn\",\"txn\":1,\"parent\":0,"
                    "\"base_container\":\"") +
        digestA + "\","
                    "\"entries\":[{\"uri\":\"a.class\",\"before\":\"" + digestA +
                    "\",\"payload\":{\"kind\":\"overlay\",\"ref\":\"overlay-1-0\"},"
                    "\"op\":\"remove\"}],"
                    "\"produced_by\":\"x\",\"created_at\":\"y\"}";
    expectRowError(removeWithPayload);

    // 版本面：缺 v、过新 v。
    expectRowError("{\"row\":\"txn\"}");
    expectRowError(std::string("{\"v\":2,\"row\":\"txn\",\"txn\":1,\"parent\":0,"
                               "\"base_container\":\"") +
                   digestA + "\",\"entries\":[],\"produced_by\":\"x\",\"created_at\":\"y\"}");

    // 非 JSON / 非对象 / 未知 row 型。
    expectRowError("{oops");
    expectRowError("[1,2,3]");
    expectRowError(std::string("{\"v\":1,\"row\":\"snapshot\"}"));
}

TEST(MjLedgerRowTest, rowsParsingIsAllOrNothing)
{
    std::vector<LedgerRow> rows;
    addRows(rows, makeTxnRow(1, 0));
    addRows(rows, makeTxnRow(2, 1));
    auto good = serializeLedgerRows(rows);
    ASSERT_FALSE(good.is_err());
    const auto  body = std::move(good).unwrap();
    ASSERT_NE(0U, body.byte_length());

    auto parsed = parseLedgerRows(body.ref());
    ASSERT_FALSE(parsed.is_err());
    EXPECT_EQ(2U, std::move(parsed).unwrap().size());

    // 空行跳过；尾行 LF 载入等价。
    auto withBlank = parseLedgerRows(refOf(std::string("\n") + body.to_std_string()));
    ASSERT_FALSE(withBlank.is_err());
    EXPECT_EQ(2U, std::move(withBlank).unwrap().size());

    // 任一行违约整段拒载（零半登记）。
    auto poisoned = body.to_std_string();
    poisoned.insert(poisoned.find_last_of('\n'), "\n{\"v\":1,\"row\":\"txn\"}");
    auto rejected = parseLedgerRows(refOf(poisoned));
    ASSERT_TRUE(rejected.is_err());
    const auto  err = std::move(rejected).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Section, err.kind);
    EXPECT_NE(std::string::npos, err.message.to_std_string().find("line 3"));
}

TEST(MjLedgerRowTest, chainValidationEnforcesOrderAndLinks)
{
    // 合法链：锚行首 + txn 升序 + parent 已出现（含指向锚事务）+ 书签指向段内。
    std::vector<LedgerRow> valid;
    addRows(valid, makeAnchorRow(4));
    addRows(valid, makeTxnRow(5, 4));
    addRows(valid, makeTxnRow(6, 5));
    addRows(valid, makeBookmarkRow("sp", 5));
    ASSERT_FALSE(validateLedgerChain(valid).is_err());

    // 压缩后的链基点：首个存活事务的 parent = 锚事务也合法。
    std::vector<LedgerRow> anchoredParent;
    addRows(anchoredParent, makeAnchorRow(2));
    {
        auto orphaned = makeTxnRow(3, 2);
        addRows(anchoredParent, std::move(orphaned));
    }
    ASSERT_FALSE(validateLedgerChain(anchoredParent).is_err());

    // 锚不在行首。
    std::vector<LedgerRow> anchorLate;
    addRows(anchorLate, makeTxnRow(5, 0));
    addRows(anchorLate, makeAnchorRow(4));
    auto anchorLateError = validateLedgerChain(anchorLate);
    ASSERT_TRUE(anchorLateError.is_err());
    EXPECT_NE(std::string::npos,
              std::move(anchorLateError).unwrap_err().message.to_std_string().find("first row"));

    // 事务新于锚的要求：锚 txn 4 之后出现 txn 4 拒载。
    std::vector<LedgerRow> anchorCollision;
    addRows(anchorCollision, makeAnchorRow(4));
    addRows(anchorCollision, makeTxnRow(4, 0));
    auto collisionError = validateLedgerChain(anchorCollision);
    ASSERT_TRUE(collisionError.is_err());
    EXPECT_NE(std::string::npos,
              std::move(collisionError).unwrap_err().message.to_std_string().find("newer than anchor"));

    // parent 未在段内（悬垂链）。
    std::vector<LedgerRow> orphanParent;
    addRows(orphanParent, makeTxnRow(5, 3));
    auto orphanError = validateLedgerChain(orphanParent);
    ASSERT_TRUE(orphanError.is_err());
    EXPECT_NE(std::string::npos,
              std::move(orphanError).unwrap_err().message.to_std_string().find("unresolved parent"));

    // parent 不早于自身。
    std::vector<LedgerRow> futureParent;
    addRows(futureParent, makeTxnRow(5, 5));
    ASSERT_TRUE(validateLedgerChain(futureParent).is_err());

    // txn 重复。
    std::vector<LedgerRow> duplicated;
    addRows(duplicated, makeTxnRow(5, 0));
    addRows(duplicated, makeTxnRow(5, 0));
    ASSERT_TRUE(validateLedgerChain(duplicated).is_err());

    // 书签指向段外。
    std::vector<LedgerRow> strayBookmark;
    addRows(strayBookmark, makeTxnRow(5, 0));
    addRows(strayBookmark, makeBookmarkRow("sp", 9));
    auto strayError = validateLedgerChain(strayBookmark);
    ASSERT_TRUE(strayError.is_err());
    EXPECT_NE(std::string::npos,
              std::move(strayError).unwrap_err().message.to_std_string().find("unknown txn"));
}

TEST(MjLedgerRowTest, sortLedgerRowsPinsAnchorFirstThenTxnThenBookmarks)
{
    std::vector<LedgerRow> rows;
    addRows(rows, makeBookmarkRow("zeta", 6));
    addRows(rows, makeTxnRow(6, 5));
    addRows(rows, makeTxnRow(5, 0));
    addRows(rows, makeBookmarkRow("alpha", 5));
    addRows(rows, makeAnchorRow(4));
    sortLedgerRows(rows);
    ASSERT_EQ(5U, rows.size());
    EXPECT_EQ(LedgerRow::Kind::Anchor, rows[0].kind);
    EXPECT_EQ(4U, rows[0].anchor.txn);
    EXPECT_EQ(LedgerRow::Kind::Transaction, rows[1].kind);
    EXPECT_EQ(5U, rows[1].transaction.txn);
    EXPECT_EQ(6U, rows[2].transaction.txn);
    EXPECT_EQ(LedgerRow::Kind::Bookmark, rows[3].kind);
    EXPECT_EQ("alpha", rows[3].bookmark.name.to_std_string());
    EXPECT_EQ("zeta", rows[4].bookmark.name.to_std_string());
}

TEST(MjLedgerRowTest, canonicalContainerDigestMatchesSectionHashDiscipline)
{
    // canonical 序列化（§1.6）：uri 字典序、u64 小端长度前缀；容器 digest 与
    // section content_hash 同一口径（同字节同 SHA-256）。
    const std::vector<ca::u8> payloadA = {'a', 'b', 'c'};
    const std::vector<ca::u8> payloadB = {0x00, 0xff, 0x10, 0x20};

    std::vector<std::pair<Utf8String, std::vector<ca::u8>>> sorted;
    sorted.emplace_back(Utf8String::from_cstr("a/Alpha.class"), payloadA);
    sorted.emplace_back(Utf8String::from_cstr("b/Beta.class"), payloadB);
    sorted.emplace_back(Utf8String::from_cstr("data.txt"), std::vector<ca::u8>{});

    const auto  bytes       = serializeContainerEntries(sorted);
    const auto  sectionHash = sectionContentHash(bytes.data(), bytes.size());

    ContainerDigestBuilder builder;
    for (const auto& entry : sorted) {
        builder.add(entry.first.ref().to_std_string(), entry.second.data(), entry.second.size());
    }
    EXPECT_TRUE(sectionHash.ref().equals(builder.finish().ref()));

    // 乱序投喂 digest 不同（口径不含排序容错，写出层定序）。
    ContainerDigestBuilder reversed;
    for (auto it = sorted.rbegin(); it != sorted.rend(); ++it) {
        reversed.add(it->first.ref().to_std_string(), it->second.data(), it->second.size());
    }
    EXPECT_FALSE(sectionHash.ref().equals(reversed.finish().ref()));

    // 遍历与定位：逐条目还原 + 单条目命中/未命中。
    std::vector<std::pair<std::string, std::vector<ca::u8>>> walked;
    auto walkedStatus = forEachContainerEntry(bytes.data(), bytes.size(),
        [&](std::string_view uri, const ca::u8* payload, ca::usize payloadSize) {
            walked.emplace_back(std::string(uri),
                                std::vector<ca::u8>(payload, payload + payloadSize));
            return true;
        });
    ASSERT_FALSE(walkedStatus.is_err());
    ASSERT_EQ(3U, walked.size());
    EXPECT_EQ("a/Alpha.class", walked[0].first);
    EXPECT_EQ(payloadB, walked[1].second);
    EXPECT_TRUE(walked[2].second.empty());

    std::vector<ca::u8> hit;
    auto hitStatus = findContainerEntry(bytes.data(), bytes.size(), "b/Beta.class", hit);
    ASSERT_FALSE(hitStatus.is_err());
    EXPECT_TRUE(std::move(hitStatus).unwrap());
    EXPECT_EQ(payloadB, hit);

    auto missStatus = findContainerEntry(bytes.data(), bytes.size(), "missing.class", hit);
    ASSERT_FALSE(missStatus.is_err());
    EXPECT_FALSE(std::move(missStatus).unwrap());

    // 截断 = Section 错（fail-closed）。
    auto truncated = forEachContainerEntry(
        bytes.data(), 3, [&](std::string_view, const ca::u8*, ca::usize) { return true; });
    ASSERT_TRUE(truncated.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Section, std::move(truncated).unwrap_err().kind);
}

TEST(MjLedgerRowTest, digestHelpersShapeAndOverlayNames)
{
    EXPECT_TRUE(isValidLedgerDigest(hashOf('0')));
    EXPECT_TRUE(isValidLedgerDigest("sha256:" + std::string(64, 'A')));   // 大写载入前合法
    EXPECT_FALSE(isValidLedgerDigest("sha256:zz"));
    EXPECT_FALSE(isValidLedgerDigest("md5:" + std::string(64, '0')));
    EXPECT_FALSE(isValidLedgerDigest(hashOf('0').substr(1)));

    EXPECT_EQ("overlay-12-3", overlaySectionName(12, 3).to_std_string());
    EXPECT_EQ("copied", copyLedgerText("copied").to_std_string());

    // ledgerDigestOfBytes 与 sectionContentHash 同口径。
    const ca::u8    sample[] = {'m', 'j', 'b', 'a', 's', 'e'};
    EXPECT_TRUE(ledgerDigestOfBytes(sample, sizeof(sample))
                    .ref()
                    .equals(sectionContentHash(sample, sizeof(sample))));
}
