//pipeline_config：声明式管线配置装载面单元测试。
//
//验收口径：合法管线装载正例（含经 installPack 真实装配面的 plugin 命名空间
//引用、内建管线前缀引用、重复装载幂等）+ 拒绝面逐桶（未知 pass / 坏参数 /
//非幂等件重复 / 空序列 / 禁用减空 / 坏 JSON / 版本门 / 字段严格 / Cf 域）+
//applyConfigTo 装配面（段位分派 / 禁用跳过 / 内建引用拒 / 注册表错配防御）。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <variant>

#include "libca/core/result.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/plugin/pack.hpp"
#include "mjbase/spi/cf_pass.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_registry.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/pipeline_config.hpp"

namespace {

using namespace mjbase::spi;

//-- 最小探测 pass（只注册/装配，run 不触碰单元） -------------------------

class ProbeGlobalPass final : public GlobalPass
{
public:
    PreservedAnalyses run(mjbase::model::ResourceContainer& /*container*/,
                          GlobalAnalysisManager& /*am*/) override
    {
        return PreservedAnalyses::all();
    }
};

class ProbeCfPass final : public CfClassPass
{
public:
    void run(jbc::core::ClassFile& /*classFile*/, CfClassContext& /*context*/) override {}
};

//-- 最小 pass-pack：走 installPack 真实装配面注册 "dasho.ProbeGlobal" ----

class ProbePassPack final : public mjbase::plugin::IPassPack
{
public:
    ProbePassPack()
    {
        manifest_.name         = "dasho";
        manifest_.mjbaseCommit = mjbase::plugin::buildCommit();
        manifest_.description  = "pipeline-config probe pack";
    }

    const mjbase::plugin::PackManifest& manifest() const override { return manifest_; }

    ca::core::Result<void, mjbase::plugin::PackError>
        registerPasses(PassNameRegistry& registry, std::string_view /*config*/) override
    {
        // 名字实体 pack 自持（成员），寿命覆盖注册表存活期（plugin-api 契约）。
        dashoProbeName_ = mjbase::plugin::qualifiedName("dasho", "ProbeGlobal");
        const Result<void, RegistryError> registered = registry.registerGlobalPass(
            dashoProbeName_.ref(), [] { return std::make_unique<ProbeGlobalPass>(); },
            "probe pass for pipeline-config interlock test");
        if (registered.is_err()) {
            return mjbase::plugin::Err(mjbase::plugin::makePackError(
                mjbase::plugin::PackErrorKind::RegistrationFailed, dashoProbeName_.ref()));
        }
        return mjbase::plugin::Ok();
    }

private:
    mjbase::plugin::PackManifest manifest_;
    ca::str::Utf8String          dashoProbeName_;
};

//-- 装载辅助 -------------------------------------------------------------

/// 内建管线引用的测试名：专名注入全局 PassRegistry 表（对齐
/// pipeline_builder_test 的 zz- 专名惯例）。不调 registerBuiltinPipelines
/// ——PassRegistryTest 断言自己是首调者（first > 0），跨套件顺序敏感；
/// 本表专名自注册与 bootstrap 时序解耦，重入按受控覆盖语义安全。
const char* testBuiltinRefName()
{
    static const char* kName = "zz-cfg-builtin-ref";
    PassRegistry::instance().registerPipeline(
        kName, "pipeline-config builtin-ref probe", [] { return PipelineBuilder{}; },
        PassStability::OptIn);
    return kName;
}

constexpr const char* kMinimalConfig =
    R"json({"version":1,"passes":["NopRemover","UnusedCheckCleaner"]})json";

void expectRejected(const std::string& text, PipelineConfigErrorKind kind,
                    const char* needle = nullptr)
{
    const auto loaded = loadPipelineConfig(text, defaultPassRegistry());
    ASSERT_TRUE(loaded.is_err()) << "expected rejection, got a load: " << text;
    EXPECT_EQ(loaded.unwrap_err().kind, kind) << loaded.unwrap_err().detail;
    EXPECT_FALSE(loaded.unwrap_err().detail.empty()) << "detail must stay diagnosable";
    if (needle != nullptr) {
        EXPECT_NE(loaded.unwrap_err().detail.find(needle), std::string::npos)
            << "detail missing '" << needle << "': " << loaded.unwrap_err().detail;
    }
}

}   // namespace

//-- 正例 ----------------------------------------------------------------

TEST(PipelineConfigTest, LoadsMinimalConfigWithDeclaredOrder)
{
    const auto loaded = loadPipelineConfig(kMinimalConfig, defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    const LoadedPipelineConfig config = loaded.unwrap();
    EXPECT_EQ(config.schemaVersion, 1);
    ASSERT_EQ(config.sequence.size(), static_cast<ca::usize>(2));
    EXPECT_EQ(config.sequence[0].name, "NopRemover");
    EXPECT_EQ(config.sequence[1].name, "UnusedCheckCleaner");
    EXPECT_FALSE(config.sequence[0].builtinRef);
    EXPECT_EQ(config.sequence[0].scope, PassScope::Method);
    EXPECT_EQ(config.sequence[1].scope, PassScope::Method);
    EXPECT_TRUE(config.sequence[0].params.empty());
    EXPECT_TRUE(config.disable.empty());
}

TEST(PipelineConfigTest, LoadsBuiltinPipelinePrefixReference)
{
    const std::string text = std::string(R"json({"version":1,"passes":[")json")
                             + testBuiltinRefName()
                             + R"json(","NopRemover"]})json";
    const auto loaded = loadPipelineConfig(text, defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    const LoadedPipelineConfig config = loaded.unwrap();
    ASSERT_EQ(config.sequence.size(), static_cast<ca::usize>(2));
    EXPECT_TRUE(config.sequence[0].builtinRef);
    EXPECT_EQ(config.sequence[0].name, testBuiltinRefName());
    EXPECT_FALSE(config.sequence[1].builtinRef);
}

TEST(PipelineConfigTest, LoadsDeclaredParamsInDeclarationOrder)
{
    const auto loaded = loadPipelineConfig(
        R"json({"version":1,"disable":["JunkCodeInserter"],
                "passes":[{"pass":"LocalVariableAnnotator",
                           "params":{"promoteTypes":false,"replaceNames":true,"prefix":"v"}}]})json",
        defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    const LoadedPipelineConfig config = loaded.unwrap();
    ASSERT_EQ(config.sequence.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(config.sequence[0].scope, PassScope::Global);
    ASSERT_EQ(config.sequence[0].params.size(), static_cast<ca::usize>(3));
    EXPECT_EQ(config.sequence[0].params[0].key, "promoteTypes");
    EXPECT_EQ(config.sequence[0].params[0].value.kind(), PipelineConfigParamKind::Bool);
    ASSERT_TRUE(std::holds_alternative<bool>(config.sequence[0].params[0].value.value));
    EXPECT_FALSE(std::get<bool>(config.sequence[0].params[0].value.value));
    EXPECT_EQ(config.sequence[0].params[1].key, "replaceNames");
    EXPECT_EQ(config.sequence[0].params[2].key, "prefix");
    ASSERT_TRUE(std::holds_alternative<std::string>(config.sequence[0].params[2].value.value));
    EXPECT_EQ(std::get<std::string>(config.sequence[0].params[2].value.value), "v");
    ASSERT_EQ(config.disable.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(config.disable[0], "JunkCodeInserter");
}

TEST(PipelineConfigTest, RepeatedLoadIsFieldwiseIdentical)
{
    const std::string text =
        R"json({"version":1,"passes":["NopRemover",
                {"pass":"LocalVariableAnnotator","params":{"prefix":"var"}}]})json";
    const auto first  = loadPipelineConfig(text, defaultPassRegistry());
    const auto second = loadPipelineConfig(text, defaultPassRegistry());
    ASSERT_TRUE(first.is_ok()) << first.unwrap_err().detail;
    ASSERT_TRUE(second.is_ok()) << second.unwrap_err().detail;
    EXPECT_TRUE(first.unwrap() == second.unwrap());
}

TEST(PipelineConfigTest, DuplicateIdempotentPassIsAllowed)
{
    const auto loaded = loadPipelineConfig(
        R"json({"version":1,"passes":["NopRemover","UnusedCheckCleaner","NopRemover"]})json",
        defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    EXPECT_EQ(loaded.unwrap().sequence.size(), static_cast<ca::usize>(3));
}

TEST(PipelineConfigTest, EmptyParamsObjectOnUndeclaredPassIsAllowed)
{
    const auto loaded = loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"NopRemover","params":{}}]})json",
        defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
}

//-- 正例：plugin 命名空间咬合（installPack 真实装配面） -------------------

TEST(PipelineConfigPluginTest, PackPassReferableAfterInstallPack)
{
    PassNameRegistry registry;
    ProbePassPack    pack;
    auto             installed = mjbase::plugin::installPack(pack, registry);
    ASSERT_TRUE(installed.is_ok()) << std::move(installed).unwrap_err().detail;

    const auto loaded =
        loadPipelineConfig(R"json({"version":1,"passes":["dasho.ProbeGlobal"]})json",
                           registry);
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    const LoadedPipelineConfig config = loaded.unwrap();
    ASSERT_EQ(config.sequence.size(), static_cast<ca::usize>(1));
    EXPECT_FALSE(config.sequence[0].builtinRef);
    EXPECT_EQ(config.sequence[0].scope, PassScope::Global);
}

TEST(PipelineConfigPluginTest, UnknownDottedNameDetailHintsPackNamespace)
{
    expectRejected(R"json({"version":1,"passes":["dasho.Nope"]})json",
                   PipelineConfigErrorKind::UnknownPass, "plugin");
}

//-- 拒绝面：未知 pass / 空序列 / Cf 域 -----------------------------------

TEST(PipelineConfigTest, RejectsUnknownPassName)
{
    expectRejected(R"json({"version":1,"passes":["NoSuchPass"]})json",
                   PipelineConfigErrorKind::UnknownPass, "NoSuchPass");
}

TEST(PipelineConfigTest, RejectsUnknownBuiltinReference)
{
    expectRejected(R"json({"version":1,"passes":["phase.not-a-thing"]})json",
                   PipelineConfigErrorKind::UnknownPass);
}

TEST(PipelineConfigTest, RejectsEmptyPassesArray)
{
    expectRejected(R"json({"version":1,"passes":[]})json",
                   PipelineConfigErrorKind::EmptySequence);
}

TEST(PipelineConfigTest, RejectsDisableThatRemovesEveryDeclaredEntry)
{
    expectRejected(
        R"json({"version":1,"passes":["NopRemover"],"disable":["NopRemover"]})json",
        PipelineConfigErrorKind::EmptySequence);
}

TEST(PipelineConfigTest, RejectsCfScopePassWithNoOrchestrationSlot)
{
    PassNameRegistry registry;
    const auto       seeded = registry.registerCfClassPass(
        "ProbeCf", [] { return std::make_unique<ProbeCfPass>(); }, "probe");
    ASSERT_TRUE(seeded.is_ok());

    const auto loaded = loadPipelineConfig(
        R"json({"version":1,"passes":["ProbeCf"]})json", registry);
    ASSERT_TRUE(loaded.is_err());
    EXPECT_EQ(loaded.unwrap_err().kind, PipelineConfigErrorKind::ScopeUnsupported);
}

//-- 拒绝面：JSON 语法 / 版本门 / 字段严格 ---------------------------------

TEST(PipelineConfigTest, RejectsMalformedJson)
{
    expectRejected(R"json({"version":1,"passes":["NopRemover"])json",
                   PipelineConfigErrorKind::BadJson, "line");
}

TEST(PipelineConfigTest, RejectsNonObjectTopLevel)
{
    expectRejected(R"json([1,2,3])json", PipelineConfigErrorKind::InvalidField);
}

TEST(PipelineConfigTest, RejectsUnsupportedSchemaVersion)
{
    expectRejected(R"json({"version":2,"passes":["NopRemover"]})json",
                   PipelineConfigErrorKind::SchemaVersion, "supported: 1..1");
}

TEST(PipelineConfigTest, RejectsNonIntegerVersion)
{
    expectRejected(R"json({"version":"1","passes":["NopRemover"]})json",
                   PipelineConfigErrorKind::InvalidField);
}

TEST(PipelineConfigTest, RejectsMissingVersion)
{
    expectRejected(R"json({"passes":["NopRemover"]})json",
                   PipelineConfigErrorKind::InvalidField, "missing version");
}

TEST(PipelineConfigTest, RejectsMissingPasses)
{
    expectRejected(R"json({"version":1})json", PipelineConfigErrorKind::InvalidField,
                   "missing passes");
}

TEST(PipelineConfigTest, RejectsUnknownTopLevelField)
{
    expectRejected(R"json({"version":1,"passes":["NopRemover"],"strict":true})json",
                   PipelineConfigErrorKind::InvalidField, "strict");
}

TEST(PipelineConfigTest, RejectsDuplicateTopLevelField)
{
    expectRejected(
        R"json({"version":1,"passes":["NopRemover"],"passes":["NopRemover"]})json",
        PipelineConfigErrorKind::InvalidField, "duplicate top-level field");
}

//-- 拒绝面：条目形状 -----------------------------------------------------

TEST(PipelineConfigTest, RejectsNonStringNonObjectEntry)
{
    expectRejected(R"json({"version":1,"passes":[42]})json",
                   PipelineConfigErrorKind::InvalidField);
}

TEST(PipelineConfigTest, RejectsEmptyPassNameEntry)
{
    expectRejected(R"json({"version":1,"passes":[""]})json",
                   PipelineConfigErrorKind::InvalidField);
}

TEST(PipelineConfigTest, RejectsEntryObjectMissingPassField)
{
    expectRejected(R"json({"version":1,"passes":[{"params":{}}]})json",
                   PipelineConfigErrorKind::InvalidField, "missing entry field 'pass'");
}

TEST(PipelineConfigTest, RejectsUnknownEntryField)
{
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"NopRemover","when":"always"}]})json",
        PipelineConfigErrorKind::InvalidField, "when");
}

TEST(PipelineConfigTest, RejectsDuplicateEntryField)
{
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"NopRemover","pass":"NopRemover"}]})json",
        PipelineConfigErrorKind::InvalidField, "duplicate entry field");
}

TEST(PipelineConfigTest, RejectsNonObjectParams)
{
    const auto loaded = loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"LocalVariableAnnotator","params":3}]})json",
        defaultPassRegistry());
    ASSERT_TRUE(loaded.is_err());
    EXPECT_EQ(loaded.unwrap_err().kind, PipelineConfigErrorKind::InvalidField);
    EXPECT_NE(loaded.unwrap_err().detail.find("'params' must be an object"),
              std::string::npos);
}

TEST(PipelineConfigTest, RejectsParamsOnBuiltinReference)
{
    const std::string text =
        std::string(R"json({"version":1,"passes":[{"pass":")json") + testBuiltinRefName()
                    + R"json(","params":{}}]})json";
    expectRejected(text, PipelineConfigErrorKind::InvalidField, "'params' is not allowed");
}

//-- 拒绝面：per-pass 参数 ------------------------------------------------

TEST(PipelineConfigTest, RejectsUnknownParamKey)
{
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"LocalVariableAnnotator","params":{"nope":1}}]})json",
        PipelineConfigErrorKind::UnknownParamKey, "nope");
}

TEST(PipelineConfigTest, RejectsParamTypeMismatch)
{
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"LocalVariableAnnotator","params":{"prefix":42}}]})json",
        PipelineConfigErrorKind::ParamTypeMismatch, "prefix");
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"LocalVariableAnnotator","params":{"promoteTypes":"yes"}}]})json",
        PipelineConfigErrorKind::ParamTypeMismatch, "promoteTypes");
}

TEST(PipelineConfigTest, RejectsCompositeParamValueFamily)
{
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"LocalVariableAnnotator","params":{"prefix":["v"]}}]})json",
        PipelineConfigErrorKind::ParamTypeMismatch, "prefix");
}

TEST(PipelineConfigTest, RejectsEmptyStringParam)
{
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"LocalVariableAnnotator","params":{"prefix":""}}]})json",
        PipelineConfigErrorKind::ParamTypeMismatch);
}

TEST(PipelineConfigTest, RejectsDuplicateParamKey)
{
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"LocalVariableAnnotator","params":{"prefix":"a","prefix":"b"}}]})json",
        PipelineConfigErrorKind::InvalidField, "duplicate param key");
}

//-- 声明表扩面：LocalVariableTableRebuilder（#180 注册接线批） ------------

TEST(PipelineConfigTest, LoadsLocalVariableTableRebuilderPrefixParam)
{
    const auto loaded = loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"LocalVariableTableRebuilder",
                                        "params":{"prefix":"v"}}]})json",
        defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    const LoadedPipelineConfig config = loaded.unwrap();
    ASSERT_EQ(config.sequence.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(config.sequence[0].scope, PassScope::Class);
    ASSERT_EQ(config.sequence[0].params.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(config.sequence[0].params[0].key, "prefix");
    EXPECT_EQ(config.sequence[0].params[0].value.kind(), PipelineConfigParamKind::String);
    ASSERT_TRUE(std::holds_alternative<std::string>(config.sequence[0].params[0].value.value));
    EXPECT_EQ(std::get<std::string>(config.sequence[0].params[0].value.value), "v");
}

TEST(PipelineConfigTest, RejectsLocalVariableTableRebuilderUndeclaredAndMismatchedParams)
{
    // mode 是枚举面（Rebuild/Drop），不在 v1 标量家族：键按 UnknownParamKey
    // 拒，声明随 phase 2 注入接线落（doc:pipeline-config-spec 分阶段口径）。
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"LocalVariableTableRebuilder","params":{"mode":"drop"}}]})json",
        PipelineConfigErrorKind::UnknownParamKey, "mode");
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"LocalVariableTableRebuilder","params":{"prefix":42}}]})json",
        PipelineConfigErrorKind::ParamTypeMismatch, "prefix");
}

TEST(PipelineConfigTest, RejectsParamsOnPassWithoutDeclaredSchema)
{
    expectRejected(
        R"json({"version":1,"passes":[{"pass":"NopRemover","params":{"x":1}}]})json",
        PipelineConfigErrorKind::ParamSchemaUndeclared, "NopRemover");
}

//-- 拒绝面：非幂等件重复 / 禁用名单 ---------------------------------------

TEST(PipelineConfigTest, RejectsDuplicateNonReentrantPass)
{
    expectRejected(
        R"json({"version":1,"passes":["StackTraceRecover","StackTraceRecover"]})json",
        PipelineConfigErrorKind::DuplicatePass, "StackTraceRecover");
    expectRejected(
        R"json({"version":1,"passes":["JunkCodeInserter","NopRemover","JunkCodeInserter"]})json",
        PipelineConfigErrorKind::DuplicatePass, "JunkCodeInserter");
}

TEST(PipelineConfigTest, RejectsUnknownDisableEntry)
{
    expectRejected(
        R"json({"version":1,"passes":["NopRemover"],"disable":["NoSuchPass"]})json",
        PipelineConfigErrorKind::UnknownPass, "disable[0]");
}

TEST(PipelineConfigTest, RejectsNonStringDisableEntry)
{
    expectRejected(R"json({"version":1,"passes":["NopRemover"],"disable":[7]})json",
                   PipelineConfigErrorKind::InvalidField, "disable[0]");
}

TEST(PipelineConfigTest, RejectsDuplicateDisableEntry)
{
    expectRejected(
        R"json({"version":1,"passes":["NopRemover"],"disable":["NopRemover","NopRemover"]})json",
        PipelineConfigErrorKind::DuplicateDisable);
}

TEST(PipelineConfigTest, LoadsDisableThatSubsetsDeclaredEntries)
{
    const auto loaded = loadPipelineConfig(
        R"json({"version":1,"passes":["NopRemover","UnusedCheckCleaner"],"disable":["NopRemover"]})json",
        defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    const LoadedPipelineConfig config = loaded.unwrap();
    ASSERT_EQ(config.disable.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(config.disable[0], "NopRemover");
}

//-- 装配面：applyConfigTo ------------------------------------------------

TEST(PipelineConfigApplyTest, AssemblesSegmentsPerRegistryScope)
{
    const auto loaded = loadPipelineConfig(
        R"json({"version":1,"passes":["NopRemover","MetaInfoRemover","LocalVariableAnnotator"]})json",
        defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;

    PipelineBuilder builder;
    const auto      applied = applyConfigTo(defaultPassRegistry(), loaded.unwrap(), builder);
    ASSERT_TRUE(applied.is_ok()) << applied.unwrap_err().detail;
    EXPECT_EQ(builder.methodPassCount(), static_cast<ca::usize>(1));
    EXPECT_EQ(builder.classPassCount(), static_cast<ca::usize>(1));
    EXPECT_EQ(builder.globalPassCount(), static_cast<ca::usize>(1));
    const auto methodNames = builder.methodPassNames();
    ASSERT_EQ(methodNames.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(methodNames[0], "NopRemover");
    const auto globalNames = builder.globalPassNames();
    ASSERT_EQ(globalNames.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(globalNames[0], "LocalVariableAnnotator");
}

TEST(PipelineConfigApplyTest, SkipsDisabledDirectEntries)
{
    const auto loaded = loadPipelineConfig(
        R"json({"version":1,"passes":["NopRemover","UnusedCheckCleaner"],"disable":["NopRemover"]})json",
        defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;

    PipelineBuilder builder;
    const auto      applied = applyConfigTo(defaultPassRegistry(), loaded.unwrap(), builder);
    ASSERT_TRUE(applied.is_ok()) << applied.unwrap_err().detail;
    const auto names = builder.methodPassNames();
    ASSERT_EQ(names.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(names[0], "UnusedCheckCleaner");
}

TEST(PipelineConfigApplyTest, RefusesBuiltinReferenceExpansion)
{
    const std::string text = std::string(R"json({"version":1,"passes":[")json")
                             + testBuiltinRefName() + R"json("]})json";
    const auto        loaded = loadPipelineConfig(text, defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;

    PipelineBuilder builder;
    const auto      applied = applyConfigTo(defaultPassRegistry(), loaded.unwrap(), builder);
    ASSERT_TRUE(applied.is_err());
    EXPECT_EQ(applied.unwrap_err().kind, PipelineConfigErrorKind::BuiltinRefNotExpandable);
}

TEST(PipelineConfigApplyTest, DefendsAgainstRegistryMismatch)
{
    PassNameRegistry registry;
    ProbePassPack    pack;
    auto             installed = mjbase::plugin::installPack(pack, registry);
    ASSERT_TRUE(installed.is_ok()) << std::move(installed).unwrap_err().detail;
    const auto loaded =
        loadPipelineConfig(R"json({"version":1,"passes":["dasho.ProbeGlobal"]})json",
                           registry);
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;

    PipelineBuilder builder;
    const auto      applied = applyConfigTo(defaultPassRegistry(), loaded.unwrap(), builder);
    ASSERT_TRUE(applied.is_err());
    EXPECT_EQ(applied.unwrap_err().kind, PipelineConfigErrorKind::RegistryMismatch);
}
