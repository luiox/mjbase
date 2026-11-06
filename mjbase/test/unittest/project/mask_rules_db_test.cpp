/// @file        mask_rules_db_test.cpp
/// @brief       AnalysisDb 出口遮蔽层数据面（schema v4）：mask_rules 整体
///              替换与校验、信任级别 meta、命中记账与只读拒写。
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "mjbase/project/analysis_db.hpp"
#include "mjbase/project/mask_evaluator.hpp"
#include "mjbase/project/mask_regex_engine.hpp"
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

std::filesystem::path freshDbPath(const std::string& name)
{
    const auto path = test::getTempOutPath("mask_rules_" + name + ".mjdb");
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::remove_all(path.string() + "-wal", ec);
    std::filesystem::remove_all(path.string() + "-shm", ec);
    std::filesystem::create_directories(path.parent_path(), ec);
    return path;
}

AnalysisDb openOrDie(const std::filesystem::path& path,
                     AnalysisDbOpenMode          mode = AnalysisDbOpenMode::ReadWrite)
{
    auto result = AnalysisDb::open(refOf(path.string()), mode);
    EXPECT_TRUE(result.is_ok()) << path.string();
    return std::move(result).unwrap();
}

template <typename T>
AnalysisDbErrorKind takeErrorKind(ca::Result<T, AnalysisDbError>&& result)
{
    EXPECT_TRUE(result.is_err());
    if (result.is_err()) return std::move(result).unwrap_err().kind;
    return AnalysisDbErrorKind::Sqlite;
}

DbMaskRuleInput maskRule(ca::i32 priority, const char* pattern, MaskAction action,
                         const char* note = "")
{
    DbMaskRuleInput input;
    input.priority = priority;
    input.pattern  = Utf8String::from_cstr(pattern);
    input.action   = action;
    input.note     = Utf8String::from_cstr(note);
    return input;
}

/// 多规则批次构造（DbMaskRuleInput 不可拷贝：MSVC initializer_list 逐元素
/// 拷贝构造不可用，显式移动构造 vector，与 DbRenameRecord 测试同纪律）。
template <typename... Rest>
std::vector<DbMaskRuleInput> ruleBatch(DbMaskRuleInput head, Rest... tail)
{
    std::vector<DbMaskRuleInput> rules;
    rules.push_back(std::move(head));
    (rules.push_back(std::move(tail)), ...);
    return rules;
}

}   // namespace

TEST(MjMaskRulesDbTest, FreshDbHasNoRulesAndTrustedDefault)
{
    const auto path = freshDbPath("fresh");
    auto      db    = openOrDie(path);
    EXPECT_EQ(kAnalysisDbSchemaVersion, db.schemaVersion());

    auto rules = db.listMaskRules();
    ASSERT_TRUE(rules.is_ok());
    EXPECT_TRUE(std::move(rules).unwrap().empty());

    auto trust = db.maskTrustLevel();
    ASSERT_TRUE(trust.is_ok());
    EXPECT_EQ(trust.unwrap(), MaskTrustLevel::Trusted);
}

TEST(MjMaskRulesDbTest, ReplaceAssignsMonotonicIdsAndOrdersByPriority)
{
    const auto path = freshDbPath("replace");
    auto      db    = openOrDie(path);

    auto first = db.replaceMaskRules(ruleBatch(maskRule(2, "beta-[0-9]+", MaskAction::Mask, "marker"),
                              maskRule(1, "hello", MaskAction::Allow, "greeting"),
                              maskRule(1, "alpha", MaskAction::Mask)));
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(first.unwrap().total, ca::usize{3});

    auto rules = db.listMaskRules();
    ASSERT_TRUE(rules.is_ok());
    auto loaded = std::move(rules).unwrap();
    ASSERT_EQ(loaded.size(), ca::usize{3});
    // (priority, rule_id) 升序：同 priority 段按分配序。
    EXPECT_EQ(loaded[0].priority, 1);
    EXPECT_EQ(loaded[0].pattern.to_std_string(), "hello");
    EXPECT_EQ(loaded[0].action, MaskAction::Allow);
    EXPECT_EQ(loaded[0].note.to_std_string(), "greeting");
    EXPECT_EQ(loaded[0].hitCount, 0);
    EXPECT_EQ(loaded[1].priority, 1);
    EXPECT_EQ(loaded[1].pattern.to_std_string(), "alpha");
    EXPECT_EQ(loaded[1].action, MaskAction::Mask);
    EXPECT_GT(loaded[1].ruleId, loaded[0].ruleId);
    EXPECT_EQ(loaded[2].priority, 2);
    EXPECT_EQ(loaded[2].pattern.to_std_string(), "beta-[0-9]+");

    // 整体替换：旧行清零、id 单调续分配、hit 计数随行清零。
    auto second = db.replaceMaskRules(ruleBatch(maskRule(1, "fresh", MaskAction::Mask)));
    ASSERT_TRUE(second.is_ok());
    const auto stats = second.unwrap();
    EXPECT_EQ(stats.total, ca::usize{1});
    EXPECT_EQ(stats.created, ca::usize{1});
    EXPECT_EQ(stats.retired, ca::usize{3});

    auto replaced = db.listMaskRules();
    ASSERT_TRUE(replaced.is_ok());
    auto rows = std::move(replaced).unwrap();
    ASSERT_EQ(rows.size(), ca::usize{1});
    EXPECT_GT(rows[0].ruleId, loaded[2].ruleId);
    EXPECT_EQ(rows[0].pattern.to_std_string(), "fresh");
}

TEST(MjMaskRulesDbTest, EmptyReplaceClearsRuleSet)
{
    const auto path = freshDbPath("clear");
    auto      db    = openOrDie(path);
    ASSERT_TRUE(db.replaceMaskRules(ruleBatch(maskRule(1, "a", MaskAction::Mask))).is_ok());
    auto cleared = db.replaceMaskRules({});
    ASSERT_TRUE(cleared.is_ok());
    EXPECT_EQ(cleared.unwrap().retired, ca::usize{1});
    auto rules = db.listMaskRules();
    ASSERT_TRUE(rules.is_ok());
    EXPECT_TRUE(std::move(rules).unwrap().empty());
}

TEST(MjMaskRulesDbTest, ValidationRejectsBadPatternBatchAtomically)
{
    const auto path = freshDbPath("validate");
    auto      db    = openOrDie(path);
    ASSERT_TRUE(db.replaceMaskRules(ruleBatch(maskRule(1, "good", MaskAction::Mask))).is_ok());

    // 空模式。
    EXPECT_EQ(takeErrorKind(db.replaceMaskRules(ruleBatch(maskRule(1, "", MaskAction::Mask)))),
              AnalysisDbErrorKind::Invalid);
    // NUL 字节（DbMaskRuleInput 不可拷贝，移动进批次）。
    DbMaskRuleInput nulRule = maskRule(1, "a b", MaskAction::Mask);
    nulRule.pattern         = Utf8String::from_data(reinterpret_cast<const ca::u8*>("a\0b"), 3);
    std::vector<DbMaskRuleInput> nulBatch;
    nulBatch.push_back(std::move(nulRule));
    EXPECT_EQ(takeErrorKind(db.replaceMaskRules(std::move(nulBatch))),
              AnalysisDbErrorKind::Invalid);
    // 超 kMaximumMaskPatternBytes。
    DbMaskRuleInput longRule = maskRule(1, "a", MaskAction::Mask);
    longRule.pattern         = Utf8String::from_data(
        reinterpret_cast<const ca::u8*>(std::string(kMaximumMaskPatternBytes + 1, 'a').c_str()),
        kMaximumMaskPatternBytes + 1);
    std::vector<DbMaskRuleInput> longBatch;
    longBatch.push_back(std::move(longRule));
    EXPECT_EQ(takeErrorKind(db.replaceMaskRules(std::move(longBatch))),
              AnalysisDbErrorKind::Invalid);
    // 编译不了的形态（最小>最大量词）。
    EXPECT_EQ(takeErrorKind(db.replaceMaskRules(ruleBatch(maskRule(1, "a{2,1}", MaskAction::Mask)))),
              AnalysisDbErrorKind::Invalid);
    // 条数超上限（DbMaskRuleInput 不可拷贝，逐条构造）。
    std::vector<DbMaskRuleInput> flood;
    for (ca::usize index = 0; index <= kMaximumMaskRules; ++index) {
        flood.push_back(maskRule(1, "a", MaskAction::Mask));
    }
    EXPECT_EQ(takeErrorKind(db.replaceMaskRules(std::move(flood))),
              AnalysisDbErrorKind::Invalid);

    // 整批校验前置：任何失败都不落库，既有规则面保持原样。
    auto rules = db.listMaskRules();
    ASSERT_TRUE(rules.is_ok());
    auto rows = std::move(rules).unwrap();
    ASSERT_EQ(rows.size(), ca::usize{1});
    EXPECT_EQ(rows[0].pattern.to_std_string(), "good");
}

TEST(MjMaskRulesDbTest, TrustLevelRoundTripsAndSurvivesReopen)
{
    const auto path = freshDbPath("trust");
    {
        auto db = openOrDie(path);
        ASSERT_TRUE(db.setMaskTrustLevel(MaskTrustLevel::Untrusted).is_ok());
        // 幂等：重复设置同值不报错。
        ASSERT_TRUE(db.setMaskTrustLevel(MaskTrustLevel::Untrusted).is_ok());
        auto trust = db.maskTrustLevel();
        ASSERT_TRUE(trust.is_ok());
        EXPECT_EQ(trust.unwrap(), MaskTrustLevel::Untrusted);
        ASSERT_TRUE(db.replaceMaskRules(ruleBatch(maskRule(1, "MARKER-[0-9]+", MaskAction::Mask))).is_ok());
        ASSERT_TRUE(db.recordMaskRuleHits({1, 1, 1}).is_ok());
        EXPECT_TRUE(db.save().is_ok());
        EXPECT_TRUE(db.close().is_ok());
    }
    auto db = openOrDie(path);
    auto trust = db.maskTrustLevel();
    ASSERT_TRUE(trust.is_ok());
    EXPECT_EQ(trust.unwrap(), MaskTrustLevel::Untrusted);
    auto rules = db.listMaskRules();
    ASSERT_TRUE(rules.is_ok());
    auto rows = std::move(rules).unwrap();
    ASSERT_EQ(rows.size(), ca::usize{1});
    EXPECT_EQ(rows[0].hitCount, 3) << "同批重复 id 按出现次数各累加一次";
}

TEST(MjMaskRulesDbTest, RecordHitsAccumulatesAndSkipsUnknownIds)
{
    const auto path = freshDbPath("hits");
    auto      db    = openOrDie(path);
    ASSERT_TRUE(db.replaceMaskRules(ruleBatch(maskRule(1, "one", MaskAction::Mask),
                              maskRule(2, "two", MaskAction::Allow)))
                    .is_ok());
    auto rules = db.listMaskRules();
    ASSERT_TRUE(rules.is_ok());
    const auto ids = std::move(rules).unwrap();
    ASSERT_EQ(ids.size(), ca::usize{2});

    ASSERT_TRUE(db.recordMaskRuleHits({ids[0].ruleId}).is_ok());
    ASSERT_TRUE(db.recordMaskRuleHits({ids[1].ruleId, ids[0].ruleId}).is_ok());
    // 未知 id（规则替换后的陈旧归因）静默跳过、不报错；空输入空操作。
    ASSERT_TRUE(db.recordMaskRuleHits({ids[1].ruleId + 999}).is_ok());
    ASSERT_TRUE(db.recordMaskRuleHits({}).is_ok());
    // 同批重复 id 按出现次数各累加（出口按行记账）。
    ASSERT_TRUE(db.recordMaskRuleHits({ids[1].ruleId, ids[1].ruleId}).is_ok());

    auto reloaded = db.listMaskRules();
    ASSERT_TRUE(reloaded.is_ok());
    const auto rows = std::move(reloaded).unwrap();
    EXPECT_EQ(rows[0].hitCount, 2);
    EXPECT_EQ(rows[1].hitCount, 3);
}

TEST(MjMaskRulesDbTest, ReadOnlyConnectionRejectsMaskWrites)
{
    const auto path = freshDbPath("readonly");
    {
        auto writer = openOrDie(path);
        ASSERT_TRUE(writer.replaceMaskRules(ruleBatch(maskRule(1, "a", MaskAction::Mask))).is_ok());
        EXPECT_TRUE(writer.save().is_ok());
        EXPECT_TRUE(writer.close().is_ok());
    }
    auto db = openOrDie(path, AnalysisDbOpenMode::ReadOnly);
    EXPECT_EQ(takeErrorKind(db.replaceMaskRules(ruleBatch(maskRule(2, "b", MaskAction::Mask)))),
              AnalysisDbErrorKind::ReadOnly);
    EXPECT_EQ(takeErrorKind(db.setMaskTrustLevel(MaskTrustLevel::Untrusted)),
              AnalysisDbErrorKind::ReadOnly);
    EXPECT_EQ(takeErrorKind(db.recordMaskRuleHits({1})), AnalysisDbErrorKind::ReadOnly);
    // 读面照常。
    auto rules = db.listMaskRules();
    ASSERT_TRUE(rules.is_ok());
    EXPECT_EQ(std::move(rules).unwrap().size(), ca::usize{1});
    auto trust = db.maskTrustLevel();
    ASSERT_TRUE(trust.is_ok());
    EXPECT_EQ(trust.unwrap(), MaskTrustLevel::Trusted);
}

TEST(MjMaskRulesDbTest, EvaluatorBuildsFromDbProjection)
{
    // 数据面 → 求值器投影同源：listMaskRules 行直接投影 MaskRuleSpec，
    // (priority, rule_id) 序即求值器匹配序。
    const auto path = freshDbPath("projection");
    auto      db    = openOrDie(path);
    ASSERT_TRUE(db.setMaskTrustLevel(MaskTrustLevel::Untrusted).is_ok());
    ASSERT_TRUE(db.replaceMaskRules(ruleBatch(maskRule(1, "MARKER-[0-9]+", MaskAction::Mask),
                                              maskRule(2, "benign", MaskAction::Allow)))
                    .is_ok());

    auto rules = db.listMaskRules();
    ASSERT_TRUE(rules.is_ok());
    // 行表落回具名量保活到 build() 之后：spec.pattern 是借用视图，指向行内
    // Utf8String 缓冲——range-for 临时直接死亡会让视图悬垂（linux 分配器把
    // freed 缓冲涂改成堆指针后"可编译但永不命中"，windows 侥幸存活）。
    auto ruleRows = std::move(rules).unwrap();
    std::vector<MaskRuleSpec> specs;
    for (const DbMaskRule& row : ruleRows) {
        MaskRuleSpec spec;
        spec.ruleId   = row.ruleId;
        spec.priority = row.priority;
        spec.pattern  = std::string_view(reinterpret_cast<const char*>(row.pattern.data()),
                                        row.pattern.byte_length());
        spec.action   = row.action;
        specs.push_back(spec);
    }
    auto built = MaskEvaluator::build(specs, db.maskTrustLevel().unwrap());
    ASSERT_TRUE(built.is_ok());
    auto evaluator = std::move(built).unwrap();

    const auto masked = evaluator.evaluate("value MARKER-9");
    EXPECT_EQ(masked.action, MaskAction::Mask);
    EXPECT_EQ(masked.ruleId, specs[0].ruleId);
    const auto allowed = evaluator.evaluate("plain benign value");
    EXPECT_EQ(allowed.action, MaskAction::Allow);
    EXPECT_EQ(allowed.ruleId, specs[1].ruleId);
    const auto fallback = evaluator.evaluate("other value");
    EXPECT_EQ(fallback.action, MaskAction::Mask);
    EXPECT_EQ(fallback.via, MaskDecisionVia::DefaultAction);
}
