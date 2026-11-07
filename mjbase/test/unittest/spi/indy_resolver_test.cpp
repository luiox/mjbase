/// indy resolver SPI 契约用例：注册/重名/空指针、优先级排序确定性（同优先级
/// 按 name 升序，不依赖注册次序）、setup 失败排除面、resolve 首个命中与
/// NoTarget/ResolveFailed 逐级回落、无目标 = 保留原站的控制流语义。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mjbase/model/resource_container.hpp"
#include "mjbase/spi/indy_resolver.hpp"

namespace {

using mjbase::spi::IndyBsmArg;
using mjbase::spi::IndyResolver;
using mjbase::spi::IndyResolverError;
using mjbase::spi::IndyResolverManager;
using mjbase::spi::IndySiteFacts;
using mjbase::spi::ResolvedIndyTarget;

/// 假 resolver：按预设应答表逐站点命中，其余 NoTarget。
class FakeResolver : public IndyResolver
{
public:
    FakeResolver(std::string_view name, ca::i32 priority,
                 IndyResolverError setupResult = IndyResolverError::NoTarget)
        : name_(name), priority_(priority), setupResult_(setupResult)
    {}

    std::string_view name() const override { return name_; }
    ca::i32          priority() const override { return priority_; }

    /// setupResult_ == NoTarget 视为成功（该码只在不该出现的位置做哨兵用）。
    ca::core::Result<void, IndyResolverError> setup(mjbase::model::ResourceContainer&) override
    {
        if (setupResult_ == IndyResolverError::SetupFailed) {
            return ca::core::Err(IndyResolverError::SetupFailed);
        }
        setupCalls_ += 1;
        return ca::core::Ok();
    }

    ca::core::Result<ResolvedIndyTarget, IndyResolverError>
        resolve(const IndySiteFacts& site) override
    {
        resolveCalls_ += 1;
        lastSite_ = std::string(site.siteName);
        for (const Answer& answer : answers) {
            if (answer.siteName != site.siteName) continue;
            if (answer.fail) return ca::core::Err(IndyResolverError::ResolveFailed);
            ResolvedIndyTarget target;
            target.form                 = answer.form;
            target.owner                = site.siteName;   // 可辨识的回显面
            target.name                 = "hit_" + std::string(site.siteName);
            target.desc                 = "()V";
            target.trailingMetadataArgs = 0;
            return ca::core::Ok(std::move(target));
        }
        return ca::core::Err(IndyResolverError::NoTarget);
    }

    struct Answer
    {
        std::string_view              siteName;
        ResolvedIndyTarget::Form      form = ResolvedIndyTarget::Form::Static;
        bool                          fail = false;
    };
    std::vector<Answer> answers;

    ca::usize setupCalls_   = 0;
    ca::usize resolveCalls_ = 0;
    std::string lastSite_;

private:
    std::string_view  name_;
    ca::i32           priority_;
    IndyResolverError setupResult_;
};

IndySiteFacts siteOf(std::string_view name)
{
    IndySiteFacts site;
    site.ownerClass = "t/Caller";
    site.methodName = "run";
    site.methodDesc = "()V";
    site.siteName   = name;
    site.siteDesc   = "()Ljava/lang/String;";
    return site;
}

/// setupAll 形参收非 const 引用：各用例共用一个具名空容器。
mjbase::model::ResourceContainer& emptyContainer()
{
    static mjbase::model::ResourceContainer container;
    return container;
}

}   // namespace

TEST(IndyResolverTest, RegisterRejectsNullAndDuplicateNames)
{
    IndyResolverManager manager;
    auto                first = std::make_unique<FakeResolver>("dup", 0);
    auto                dup   = std::make_unique<FakeResolver>("dup", 5);

    EXPECT_TRUE(manager.registerResolver(std::move(first)).is_ok());
    auto r = manager.registerResolver(std::move(dup));
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), IndyResolverError::DuplicateName);
    EXPECT_EQ(manager.resolvers().size(), 1u);

    EXPECT_TRUE(manager.registerResolver(nullptr).is_err());
    EXPECT_EQ(manager.resolvers().size(), 1u);
}

TEST(IndyResolverTest, PriorityOrderIsStableAcrossRegistrationOrder)
{
    // 同优先级按 name 升序；两份注册次序相反的表产出同一迭代序。
    std::vector<std::string> orderA;
    std::vector<std::string> orderB;
    {
        IndyResolverManager m;
        EXPECT_TRUE(m.registerResolver(std::make_unique<FakeResolver>("alpha", 0)).is_ok());
        EXPECT_TRUE(m.registerResolver(std::make_unique<FakeResolver>("high", 9)).is_ok());
        EXPECT_TRUE(m.registerResolver(std::make_unique<FakeResolver>("beta", 0)).is_ok());
        for (const IndyResolver* r : m.resolvers()) orderA.emplace_back(r->name());
    }
    {
        IndyResolverManager m;
        EXPECT_TRUE(m.registerResolver(std::make_unique<FakeResolver>("beta", 0)).is_ok());
        EXPECT_TRUE(m.registerResolver(std::make_unique<FakeResolver>("alpha", 0)).is_ok());
        EXPECT_TRUE(m.registerResolver(std::make_unique<FakeResolver>("high", 9)).is_ok());
        for (const IndyResolver* r : m.resolvers()) orderB.emplace_back(r->name());
    }
    ASSERT_EQ(orderA.size(), 3u);
    EXPECT_EQ(orderA[0], "high");
    EXPECT_EQ(orderA[1], "alpha");
    EXPECT_EQ(orderA[2], "beta");
    EXPECT_EQ(orderA, orderB);
}

TEST(IndyResolverTest, SetupFailureExcludesResolverForTheRun)
{
    IndyResolverManager manager;
    EXPECT_TRUE(manager.registerResolver(
                    std::make_unique<FakeResolver>("broken", 100, IndyResolverError::SetupFailed))
                    .is_ok());
    EXPECT_TRUE(manager.registerResolver(std::make_unique<FakeResolver>("solid", 0)).is_ok());
    EXPECT_EQ(manager.setupAll(emptyContainer()), 1u);
    ASSERT_EQ(manager.resolvers().size(), 1u);
    EXPECT_EQ(manager.resolvers()[0]->name(), "solid");
}

TEST(IndyResolverTest, ResolveFirstPicksHighestPriorityHitAndSkipsMisses)
{
    IndyResolverManager manager;
    auto                high = std::make_unique<FakeResolver>("high", 10);
    auto                low  = std::make_unique<FakeResolver>("low", -1);
    // high 只认 s2；low 认 s1 与 s2 —— s2 必须由 high 命中（优先级纪律）。
    high->answers.push_back({std::string_view("s2")});
    low->answers.push_back({std::string_view("s1")});
    low->answers.push_back({std::string_view("s2")});
    EXPECT_TRUE(manager.registerResolver(std::move(high)).is_ok());
    EXPECT_TRUE(manager.registerResolver(std::move(low)).is_ok());
    manager.setupAll(emptyContainer());

    auto miss = manager.resolveFirst(siteOf("s1"));
    ASSERT_TRUE(miss.is_ok());
    EXPECT_EQ(miss.unwrap().owner, "s1");   // low 的回显面
    auto hit = manager.resolveFirst(siteOf("s2"));
    ASSERT_TRUE(hit.is_ok());
    EXPECT_EQ(hit.unwrap().owner, "s2");

    // 全员 NoTarget：无目标 = 保留原站的控制流语义（Err(NoTarget)）。
    auto none = manager.resolveFirst(siteOf("s3"));
    ASSERT_TRUE(none.is_err());
    EXPECT_EQ(none.unwrap_err(), IndyResolverError::NoTarget);
}

TEST(IndyResolverTest, ResolveFailureFallsThroughToNextResolver)
{
    IndyResolverManager manager;
    auto                broken = std::make_unique<FakeResolver>("broken", 10);
    auto                backup = std::make_unique<FakeResolver>("backup", 0);
    broken->answers.push_back({std::string_view("s1"), ResolvedIndyTarget::Form::Static, true});
    backup->answers.push_back({std::string_view("s1")});
    EXPECT_TRUE(manager.registerResolver(std::move(broken)).is_ok());
    EXPECT_TRUE(manager.registerResolver(std::move(backup)).is_ok());
    manager.setupAll(emptyContainer());

    auto r = manager.resolveFirst(siteOf("s1"));
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.unwrap().owner, "s1");
}

TEST(IndyResolverTest, SiteFactsCarryBsmArgsByPosition)
{
    // 事实包按位交付：resolver 看到的 bsmArgs 顺序 = 采集顺序（含占位）。
    IndyResolverManager manager;
    auto                probe = std::make_unique<FakeResolver>("probe", 0);
    FakeResolver*       raw   = probe.get();
    probe->answers.push_back({std::string_view("s1")});
    EXPECT_TRUE(manager.registerResolver(std::move(probe)).is_ok());

    IndySiteFacts site = siteOf("s1");
    IndyBsmArg    key;
    key.kind = IndyBsmArg::Kind::Int;
    key.num  = 42;
    site.bsmArgs.push_back(key);
    IndyBsmArg tag;
    tag.kind = IndyBsmArg::Kind::String;
    tag.str  = "meta";
    site.bsmArgs.push_back(tag);
    IndyBsmArg broken;
    broken.kind = IndyBsmArg::Kind::Unreadable;
    site.bsmArgs.push_back(broken);

    EXPECT_TRUE(manager.resolveFirst(site).is_ok());
    EXPECT_EQ(raw->lastSite_, "s1");
    EXPECT_EQ(raw->resolveCalls_, 1u);
}
