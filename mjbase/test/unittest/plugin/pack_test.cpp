/// @file        pack_test.cpp
/// @brief       plugin pass-pack 接口 v1：版本门 / manifest 校验 / 命名空间
///              验收 / 配置闭包 / 重复注册拒绝 / 零注册拒绝。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "mjbase/plugin/pack.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using mjbase::plugin::buildCommit;
using mjbase::plugin::installPack;
using mjbase::plugin::IPassPack;
using mjbase::plugin::PackError;
using mjbase::plugin::PackErrorKind;
using mjbase::plugin::PackManifest;
using mjbase::plugin::qualifiedName;
using mjbase::spi::MethodAnalysisManager;
using mjbase::spi::MethodPass;
using mjbase::spi::MethodScope;
using mjbase::spi::PassNameRegistry;
using mjbase::spi::PreservedAnalyses;

/// 最小方法级 pass：不动点，回 all()；暴露注册期闭包进去的 config 供断言。
class ConfigProbePass final : public MethodPass
{
public:
    explicit ConfigProbePass(std::string config) : config_(std::move(config)) {}

    PreservedAnalyses run(MethodScope&, MethodAnalysisManager&) override
    {
        return PreservedAnalyses::all();
    }

    const std::string& config() const { return config_; }

private:
    std::string config_;
};

/// 样板 pack：一个方法级 pass；名字/描述均为自持存储，配置闭包进工厂。
class SamplePack final : public IPassPack
{
public:
    explicit SamplePack(std::string commit = buildCommit(), std::string name = "sample")
        : commitStorage_(std::move(commit)), nameStorage_(std::move(name))
    {
        manifest_.name         = nameStorage_;
        manifest_.mjbaseCommit = commitStorage_;
        manifest_.description  = "sample pass-pack for interface tests";
        passName_              = qualifiedName(manifest_.name, "ConfigProbe");
    }

    const PackManifest& manifest() const override { return manifest_; }

    Result<void, PackError> registerPasses(PassNameRegistry& registry, std::string_view config) override
    {
        if (!config.empty() && config.front() == '!') {   // 约定：开头 '!' = pack 拒绝配置
            return Err(mjbase::plugin::makePackError(PackErrorKind::ConfigRejected,
                                                     "rejected by sample pack"));
        }
        configText_.assign(config.data(), config.size());
        seenAtRegistration_ = configText_;
        if (Result<void, mjbase::spi::RegistryError> r = registry.registerMethodPass(
                passName_, [this] { return std::make_unique<ConfigProbePass>(configText_); },
                "sample config probe");
            r.is_err()) {
            return Err(mjbase::plugin::makePackError(PackErrorKind::RegistrationFailed, passName_));
        }
        return Ok();
    }

    const std::string& seenAtRegistration() const { return seenAtRegistration_; }

private:
    PackManifest manifest_;
    std::string  commitStorage_;      ///< manifest_.mjbaseCommit 的实体
    std::string  nameStorage_;        ///< manifest_.name 的实体（借用视图纪律）
    std::string  passName_;           ///< 注册名实体，寿命覆盖注册表
    std::string  configText_;
    std::string  seenAtRegistration_;
};

/// 越界样板：越过 "<pack>." 前缀直接注册裸名字。
class RoguePack final : public IPassPack
{
public:
    const PackManifest& manifest() const override
    {
        static PackManifest mf{"rogue", buildCommit(), "registers outside its namespace"};
        return mf;
    }

    Result<void, PackError> registerPasses(PassNameRegistry& registry, std::string_view) override
    {
        registry.registerMethodPass("RoguePass", [] { return std::unique_ptr<MethodPass>{}; },
                                    "rogue");
        return Ok();
    }
};

/// 零注册样板：manifest 合法但一个 pass 都不交。
class EmptyPack final : public IPassPack
{
public:
    const PackManifest& manifest() const override
    {
        static PackManifest mf{"empty", buildCommit(), "registers nothing"};
        return mf;
    }

    Result<void, PackError> registerPasses(PassNameRegistry&, std::string_view) override
    {
        return Ok();
    }
};

TEST(PackTest, qualifiedNameFormat)
{
    EXPECT_EQ(qualifiedName("dasho", "StringDecrypt"), "dasho.StringDecrypt");
    EXPECT_EQ(qualifiedName("allatori", "Flow"), "allatori.Flow");
}

TEST(PackTest, installRegistersQualifiedNames)
{
    PassNameRegistry registry;
    SamplePack       pack;

    Result<void, PackError> r = installPack(pack, registry, "{}");
    ASSERT_TRUE(r.is_ok()) << r.unwrap_err().detail;

    ASSERT_TRUE(registry.has("sample.ConfigProbe"));
    const auto entry = registry.find("sample.ConfigProbe");
    ASSERT_TRUE(entry.is_ok());
    EXPECT_EQ(entry.unwrap().scope, mjbase::spi::PassScope::Method);

    auto created = registry.createMethodPass("sample.ConfigProbe");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
    EXPECT_TRUE(registry.createClassPass("sample.ConfigProbe").unwrap_err()
                == mjbase::spi::RegistryError::ScopeMismatch);
}

TEST(PackTest, configReachesFactoryClosure)
{
    PassNameRegistry registry;
    SamplePack       pack;
    const std::string config = R"({"keep":true})";

    ASSERT_TRUE(installPack(pack, registry, config).is_ok());
    ASSERT_EQ(pack.seenAtRegistration(), config);

    auto created = registry.createMethodPass("sample.ConfigProbe");
    ASSERT_TRUE(created.is_ok());
    // 所有权必须留在本作用域：unwrap() 的临时 unique_ptr 析构会删掉 pass，
    // 先取 get() 再让临时亡是悬垂。
    auto pass = std::move(created).unwrap();
    ASSERT_NE(pass, nullptr);
    auto* probe = static_cast<ConfigProbePass*>(pass.get());
    EXPECT_EQ(probe->config(), config);
}

TEST(PackTest, buildMismatchRejected)
{
    PassNameRegistry registry;
    SamplePack       pack("0000000000000000000000000000000000000000");

    Result<void, PackError> r = installPack(pack, registry, "{}");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().kind, PackErrorKind::BuildMismatch);
    EXPECT_FALSE(registry.has("sample.ConfigProbe"));
}

TEST(PackTest, invalidManifestRejected)
{
    for (const std::string& bad : {std::string{""}, std::string{"DashO"}, std::string{"a.b"},
                                   std::string{"da sho"}}) {
        PassNameRegistry registry;
        SamplePack       pack(buildCommit(), bad);
        Result<void, PackError> r = installPack(pack, registry, "{}");
        ASSERT_TRUE(r.is_err()) << bad;
        EXPECT_EQ(r.unwrap_err().kind, PackErrorKind::InvalidManifest) << bad;
    }
}

TEST(PackTest, configRejectedPassthrough)
{
    PassNameRegistry registry;
    SamplePack       pack;

    Result<void, PackError> r = installPack(pack, registry, "!");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().kind, PackErrorKind::ConfigRejected);
    EXPECT_FALSE(registry.has("sample.ConfigProbe"));
}

TEST(PackTest, duplicateInstallRejected)
{
    PassNameRegistry registry;
    SamplePack       pack;

    ASSERT_TRUE(installPack(pack, registry, "{}").is_ok());
    Result<void, PackError> r = installPack(pack, registry, "{}");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().kind, PackErrorKind::RegistrationFailed);
}

TEST(PackTest, namespaceViolationDetected)
{
    PassNameRegistry registry;
    RoguePack        pack;

    Result<void, PackError> r = installPack(pack, registry, "");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().kind, PackErrorKind::NamespaceViolation);
    EXPECT_EQ(r.unwrap_err().detail, "RoguePass");
}

TEST(PackTest, zeroRegistrationRejected)
{
    PassNameRegistry registry;
    EmptyPack        pack;

    Result<void, PackError> r = installPack(pack, registry, "");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().kind, PackErrorKind::InvalidManifest);
}

}   // namespace
