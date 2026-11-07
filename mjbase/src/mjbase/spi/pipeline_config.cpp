#include "mjbase/spi/pipeline_config.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "libca/json/json_document.hpp"
#include "libca/json/json_reader.hpp"
#include "libca/json/json_value.hpp"

#include "mjbase/spi/pipeline_builder.hpp"

namespace mjbase::spi {
namespace {

using ca::str::Utf8StringRef;

/// 当前 schema 版本（版本门 supported 区间上界；演进随 doc:pipeline-config-spec）。
constexpr int kPipelineConfigSchemaVersion = 1;

PipelineConfigError makeError(PipelineConfigErrorKind kind, std::string message)
{
    PipelineConfigError error;
    error.kind    = kind;
    error.detail  = std::move(message);
    return error;
}

Utf8StringRef viewOf(std::string_view text)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(text.data()), text.size());
}

std::string_view keyView(const Utf8StringRef& key)
{
    return std::string_view(reinterpret_cast<const char*>(key.data()), key.byte_length());
}

bool keyIs(const Utf8StringRef& key, const char* text) { return key.equals(text); }

/// 条目定位前缀："passes[2]: " / "disable[0]: "（DOM 无行号；schema 错误
/// 定位到序号即可，JSON 语法错误本身带行列——与 analysis-project 同一口径）。
std::string atPasses(ca::usize index)
{
    return "passes[" + std::to_string(index) + "]: ";
}

std::string atDisable(ca::usize index)
{
    return "disable[" + std::to_string(index) + "]: ";
}

//-- per-pass 参数 schema 声明表（v1） ------------------------------------

struct ParamSpecEntry
{
    std::string_view         key;
    PipelineConfigParamKind  kind;
};

struct PassParamSchema
{
    std::string_view               passName;
    std::vector<ParamSpecEntry>    params;
};

/// per-pass 参数 schema 声明表：从 pass 头注 Options 族的配置面提炼（口径
/// 按 doc:pass-contract；分阶段计划见 doc:pipeline-config-spec）。v1 只收
/// 标量面成员。fail-closed 配套规则：未进本表的 pass 携带参数一律
/// ParamSchemaUndeclared 拒——schema 缺位不得静默丢参；不携带参数则与本表
/// 无涉（注册表新增 pass 不受本表漂移影响）。
const std::vector<PassParamSchema>& paramSchemaTable()
{
    static const std::vector<PassParamSchema> table = {
        {"LocalVariableAnnotator",
         {{"promoteTypes", PipelineConfigParamKind::Bool},
          {"replaceNames", PipelineConfigParamKind::Bool},
          {"prefix",       PipelineConfigParamKind::String}},
        },
        // LocalVariableTableRebuilderOptions 标量面：仅 prefix（String）。
        // mode 是枚举（Rebuild/Drop），不在 v1 标量家族——JSON 编码随 phase 2
        // 「参数 → 工厂闭包注入」接线一并落（此前携带 mode 键按 UnknownParamKey
        // 拒，注册表工厂无参是既定契约，见 doc:pipeline-config-spec）。
        {"LocalVariableTableRebuilder",
         {{"prefix", PipelineConfigParamKind::String}},
        },
    };
    return table;
}

const PassParamSchema* findParamSchema(std::string_view passName)
{
    for (const PassParamSchema& schema : paramSchemaTable()) {
        if (schema.passName == passName) return &schema;
    }
    return nullptr;
}

//-- 非幂等件逐一登记表 ---------------------------------------------------

/// 重复出现的语义裁决表：幂等件允许重复（清理件在长链尾再收一轮是合法
/// 配方），非幂等件重复即 DuplicatePass。登记依据 pass-catalog 各件「链序」
/// 行：StackTraceRecover 每轮追加一组打印调用与一批行号（显式非幂等）；
/// JunkCodeInserter 逐轮叠加噪声块（加噪件重复即语义变更）。后续批次按
/// 同一口径逐件增补，未登记件一律按幂等放行。
const std::vector<std::string_view>& nonReentrantPasses()
{
    static const std::vector<std::string_view> list = {"StackTraceRecover", "JunkCodeInserter"};
    return list;
}

bool isNonReentrant(std::string_view passName)
{
    return std::find(nonReentrantPasses().begin(), nonReentrantPasses().end(), passName)
           != nonReentrantPasses().end();
}

//-- 序列条目解析 ---------------------------------------------------------

struct EntrySlots
{
    const ca::json::JsonValue* pass   = nullptr;
    const ca::json::JsonValue* params = nullptr;
};

/// 名字解析（先 pass 注册表全集、后内建管线集）。
Result<PipelineConfigStep, PipelineConfigError>
    resolveStepName(std::string_view name, const PassNameRegistry& registry,
                    const PassRegistry& pipelines, const std::string& context)
{
    auto found = registry.find(name);
    if (found.is_ok()) {
        const PassNameEntry entry = found.unwrap();
        PipelineConfigStep   step;
        step.name       = std::string(name);
        step.builtinRef = false;
        step.scope      = entry.scope;
        if (step.scope == PassScope::Cf) {
            return Err(makeError(PipelineConfigErrorKind::ScopeUnsupported,
                                 context + "cf-scope pass '" + std::string(name)
                                     + "' has no container-pipeline slot (Cf domain is "
                                       "outside the orchestration model)"));
        }
        return Ok(step);
    }
    if (pipelines.hasPipeline(std::string(name))) {
        PipelineConfigStep step;
        step.name       = std::string(name);
        step.builtinRef = true;
        return Ok(step);
    }
    std::string detail = context + "unknown pass '" + std::string(name) + "'";
    if (name.find('.') != std::string_view::npos) {
        detail += " (dotted names resolve against the pass registry; plugin `<pack>.` "
                  "names are referable after the host installs the pack)";
    }
    return Err(makeError(PipelineConfigErrorKind::UnknownPass, detail));
}

/// 参数对象 → 声明序参数表（对 findParamSchema 声明面校验键与值家族）。
Result<std::vector<PipelineConfigParam>, PipelineConfigError>
    parseParams(const ca::json::JsonValue& object, std::string_view passName,
                const std::string& context)
{
    const PassParamSchema* schema = findParamSchema(passName);
    const auto&            members = object.as_object();
    if (schema == nullptr) {
        if (!members.empty()) {
            return Err(makeError(PipelineConfigErrorKind::ParamSchemaUndeclared,
                                 context + "pass '" + std::string(passName)
                                     + "' carries params but has no declared param schema "
                                       "yet (per-pass param table lands in phases)"));
        }
        return Ok(std::vector<PipelineConfigParam>{});
    }

    std::vector<PipelineConfigParam> out;
    for (const auto& [key, value] : members) {
        const std::string_view keyText = keyView(key);
        for (const PipelineConfigParam& existing : out) {
            if (existing.key == keyText) {
                return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                     context + "duplicate param key '" + std::string(keyText)
                                         + "'"));
            }
        }

        const ParamSpecEntry* spec = nullptr;
        for (const ParamSpecEntry& entry : schema->params) {
            if (entry.key == keyText) {
                spec = &entry;
                break;
            }
        }
        if (spec == nullptr) {
            return Err(makeError(PipelineConfigErrorKind::UnknownParamKey,
                                 context + "unknown param key '" + std::string(keyText)
                                     + "' for pass '" + std::string(passName) + "'"));
        }

        PipelineConfigParam param;
        param.key = std::string(keyText);
        switch (spec->kind) {
        case PipelineConfigParamKind::Bool:
            if (!value.is_bool()) {
                return Err(makeError(PipelineConfigErrorKind::ParamTypeMismatch,
                                     context + "param '" + std::string(keyText)
                                         + "' must be a bool"));
            }
            param.value.value = value.as_bool();
            break;
        case PipelineConfigParamKind::Int:
            if (!value.is_int()) {
                return Err(makeError(PipelineConfigErrorKind::ParamTypeMismatch,
                                     context + "param '" + std::string(keyText)
                                         + "' must be an int"));
            }
            param.value.value = value.as_int();
            break;
        case PipelineConfigParamKind::Float:
            // 数值家族晋升：int 字面量按 float 声明接受（升格精确无损）。
            if (value.is_float()) {
                param.value.value = value.as_float();
            }
            else if (value.is_int()) {
                param.value.value = static_cast<ca::f64>(value.as_int());
            }
            else {
                return Err(makeError(PipelineConfigErrorKind::ParamTypeMismatch,
                                     context + "param '" + std::string(keyText)
                                         + "' must be a number"));
            }
            break;
        case PipelineConfigParamKind::String:
            if (!value.is_string() || value.as_string().is_empty()) {
                return Err(makeError(PipelineConfigErrorKind::ParamTypeMismatch,
                                     context + "param '" + std::string(keyText)
                                         + "' must be a non-empty string"));
            }
            param.value.value = value.as_string().to_std_string();
            break;
        }
        out.push_back(std::move(param));
    }
    return Ok(std::move(out));
}

/// 单个序列条目（字符串形或对象形）→ 校验后的步骤。
Result<PipelineConfigStep, PipelineConfigError>
    parseStep(const ca::json::JsonValue& value, const PassNameRegistry& registry,
              const PassRegistry& pipelines, const std::string& context)
{
    if (value.is_string()) {
        const std::string name = value.as_string().to_std_string();
        if (name.empty()) {
            return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                 context + "pass name must be a non-empty string"));
        }
        return resolveStepName(name, registry, pipelines, context);
    }
    if (!value.is_object()) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField,
                             context + "entry must be a string or an object"));
    }

    EntrySlots slots;
    for (const auto& [key, member] : value.as_object()) {
        const ca::json::JsonValue** slot  = nullptr;
        const char*                 known = nullptr;
        if (keyIs(key, "pass")) {
            slot  = &slots.pass;
            known = "pass";
        }
        else if (keyIs(key, "params")) {
            slot  = &slots.params;
            known = "params";
        }
        else {
            return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                 context + "unknown entry field: " + std::string(keyView(key))));
        }
        if (*slot != nullptr) {
            return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                 context + std::string("duplicate entry field: ") + known));
        }
        *slot = &member;
    }

    if (slots.pass == nullptr) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField,
                             context + "missing entry field 'pass'"));
    }
    if (!slots.pass->is_string() || slots.pass->as_string().is_empty()) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField,
                             context + "entry field 'pass' must be a non-empty string"));
    }
    const std::string name = slots.pass->as_string().to_std_string();

    // unwrap() const& 按值返回：带 params 的路径先 move 出来改，不走 unwrap 改写
    // （那会落在临时副本上）。
    Result<PipelineConfigStep, PipelineConfigError> step =
        resolveStepName(name, registry, pipelines, context);
    if (step.is_err()) return Err(std::move(step).unwrap_err());

    if (slots.params == nullptr) return step;

    PipelineConfigStep resolved = std::move(step).unwrap();
    if (resolved.builtinRef) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField,
                             context + "'params' is not allowed on builtin pipeline "
                                      "reference '" + name + "'"));
    }
    if (!slots.params->is_object()) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField,
                             context + "entry field 'params' must be an object"));
    }
    Result<std::vector<PipelineConfigParam>, PipelineConfigError> params =
        parseParams(*slots.params, name, context);
    if (params.is_err()) return Err(std::move(params).unwrap_err());
    resolved.params = std::move(params).unwrap();
    return Ok(std::move(resolved));
}

}   // namespace

const char* to_string(PipelineConfigParamKind kind) noexcept
{
    switch (kind) {
    case PipelineConfigParamKind::Bool:   return "Bool";
    case PipelineConfigParamKind::Int:    return "Int";
    case PipelineConfigParamKind::Float:  return "Float";
    case PipelineConfigParamKind::String: return "String";
    }
    return "?";
}

const char* to_string(PipelineConfigErrorKind kind) noexcept
{
    switch (kind) {
    case PipelineConfigErrorKind::Ok:                       return "Ok";
    case PipelineConfigErrorKind::BadJson:                  return "BadJson";
    case PipelineConfigErrorKind::InvalidField:             return "InvalidField";
    case PipelineConfigErrorKind::SchemaVersion:            return "SchemaVersion";
    case PipelineConfigErrorKind::UnknownPass:              return "UnknownPass";
    case PipelineConfigErrorKind::UnknownParamKey:          return "UnknownParamKey";
    case PipelineConfigErrorKind::ParamTypeMismatch:        return "ParamTypeMismatch";
    case PipelineConfigErrorKind::ParamSchemaUndeclared:    return "ParamSchemaUndeclared";
    case PipelineConfigErrorKind::DuplicatePass:            return "DuplicatePass";
    case PipelineConfigErrorKind::DuplicateDisable:         return "DuplicateDisable";
    case PipelineConfigErrorKind::EmptySequence:            return "EmptySequence";
    case PipelineConfigErrorKind::ScopeUnsupported:         return "ScopeUnsupported";
    case PipelineConfigErrorKind::BuiltinRefNotExpandable:  return "BuiltinRefNotExpandable";
    case PipelineConfigErrorKind::RegistryMismatch:         return "RegistryMismatch";
    }
    return "?";
}

Result<LoadedPipelineConfig, PipelineConfigError>
    loadPipelineConfig(std::string_view text, const PassNameRegistry& registry,
                       const PassRegistry& pipelines)
{
    // JSON 语法门（解析器自带行列号）。
    Result<ca::json::JsonDocument, ca::json::ParseError> parsed =
        ca::json::JsonReader::read(viewOf(text));
    if (parsed.is_err()) {
        const ca::json::ParseError parseError = std::move(parsed).unwrap_err();
        return Err(makeError(PipelineConfigErrorKind::BadJson,
                             "JSON syntax error at line "
                                 + std::to_string(parseError.location.line) + ", column "
                                 + std::to_string(parseError.location.column) + ": "
                                 + parseError.message.to_std_string()));
    }
    ca::json::JsonDocument    doc  = std::move(parsed).unwrap();
    const ca::json::JsonValue& root = doc.root();
    if (!root.is_object()) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField,
                             "top-level value must be an object"));
    }

    LoadedPipelineConfig config;

    // 顶层字段严格（未知/重复即拒；字段严格是 schema 契约的边界，analysis-project 同款）。
    const ca::json::JsonValue* versionSlot = nullptr;
    const ca::json::JsonValue* passesSlot  = nullptr;
    const ca::json::JsonValue* disableSlot = nullptr;
    for (const auto& [key, member] : root.as_object()) {
        if (keyIs(key, "version")) {
            if (versionSlot != nullptr) {
                return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                     "duplicate top-level field: version"));
            }
            versionSlot = &member;
        }
        else if (keyIs(key, "passes")) {
            if (passesSlot != nullptr) {
                return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                     "duplicate top-level field: passes"));
            }
            passesSlot = &member;
        }
        else if (keyIs(key, "disable")) {
            if (disableSlot != nullptr) {
                return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                     "duplicate top-level field: disable"));
            }
            disableSlot = &member;
        }
        else {
            return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                 "unknown top-level field: " + std::string(keyView(key))));
        }
    }

    // 版本门（对齐 analysis-db 版本门风格：不识别即拒，detail 标注 supported 区间）。
    if (versionSlot == nullptr) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField, "missing version"));
    }
    if (!versionSlot->is_int()) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField,
                             "version must be an integer"));
    }
    config.schemaVersion = static_cast<int>(versionSlot->as_int());
    if (config.schemaVersion != kPipelineConfigSchemaVersion) {
        return Err(makeError(PipelineConfigErrorKind::SchemaVersion,
                             "unsupported pipeline config schema version "
                                 + std::to_string(config.schemaVersion) + " (supported: "
                                 + std::to_string(kPipelineConfigSchemaVersion) + ".."
                                 + std::to_string(kPipelineConfigSchemaVersion) + ")"));
    }

    // 序列门：必填、数组、非空，逐条装载。
    if (passesSlot == nullptr) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField, "missing passes"));
    }
    if (!passesSlot->is_array()) {
        return Err(makeError(PipelineConfigErrorKind::InvalidField,
                             "passes must be an array"));
    }
    const auto& entries = passesSlot->as_array();
    if (entries.empty()) {
        return Err(makeError(PipelineConfigErrorKind::EmptySequence,
                             "passes must not be empty"));
    }
    for (ca::usize i = 0; i < entries.size(); ++i) {
        Result<PipelineConfigStep, PipelineConfigError> step =
            parseStep(entries[i], registry, pipelines, atPasses(i));
        if (step.is_err()) return Err(std::move(step).unwrap_err());
        config.sequence.push_back(std::move(step).unwrap());
    }

    // 禁用名单门：可省；条目非空字符串、对 pass 注册表验名、名单内不重复。
    if (disableSlot != nullptr) {
        if (!disableSlot->is_array()) {
            return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                 "disable must be an array of pass names"));
        }
        for (ca::usize i = 0; i < disableSlot->as_array().size(); ++i) {
            const ca::json::JsonValue& entry = disableSlot->as_array()[i];
            if (!entry.is_string() || entry.as_string().is_empty()) {
                return Err(makeError(PipelineConfigErrorKind::InvalidField,
                                     atDisable(i) + "must be a non-empty string"));
            }
            const std::string name = entry.as_string().to_std_string();
            if (!registry.has(name)) {
                return Err(makeError(PipelineConfigErrorKind::UnknownPass,
                                     atDisable(i) + "unknown pass '" + name + "'"));
            }
            for (const std::string& existing : config.disable) {
                if (existing == name) {
                    return Err(makeError(PipelineConfigErrorKind::DuplicateDisable,
                                         atDisable(i) + "duplicate disable entry '" + name
                                             + "'"));
                }
            }
            config.disable.push_back(name);
        }
    }

    // 非幂等件重复裁决（逐一登记表；内建引用不在裁决面——它是前缀引用不是实例）。
    for (ca::usize i = 0; i < config.sequence.size(); ++i) {
        const PipelineConfigStep& step = config.sequence[i];
        if (step.builtinRef || !isNonReentrant(step.name)) continue;
        for (ca::usize j = 0; j < i; ++j) {
            const PipelineConfigStep& prior = config.sequence[j];
            if (!prior.builtinRef && prior.name == step.name) {
                return Err(makeError(PipelineConfigErrorKind::DuplicatePass,
                                     atPasses(i) + "pass '" + step.name
                                         + "' is registered non-reentrant and appears more "
                                           "than once"));
            }
        }
    }

    // 禁用减空门：直引条目被禁用名全数移除后不得为空（内建引用不受禁用面影响）。
    bool effectiveEmpty = true;
    for (const PipelineConfigStep& step : config.sequence) {
        if (step.builtinRef) {
            effectiveEmpty = false;
            break;
        }
        const bool disabled =
            std::find(config.disable.begin(), config.disable.end(), step.name)
            != config.disable.end();
        if (!disabled) {
            effectiveEmpty = false;
            break;
        }
    }
    if (effectiveEmpty) {
        return Err(makeError(PipelineConfigErrorKind::EmptySequence,
                             "passes: disable list removes every declared entry"));
    }

    return Ok(std::move(config));
}

Result<void, PipelineConfigError> applyConfigTo(const PassNameRegistry&     registry,
                                                const LoadedPipelineConfig& config,
                                                PipelineBuilder&            builder)
{
    for (const PipelineConfigStep& step : config.sequence) {
        if (step.builtinRef) {
            return Err(makeError(PipelineConfigErrorKind::BuiltinRefNotExpandable,
                                 "builtin pipeline '" + step.name
                                     + "' cannot be expanded by applyConfigTo (builder "
                                       "merge API lands with the expansion wiring)"));
        }
        const bool disabled = std::find(config.disable.begin(), config.disable.end(),
                                        step.name)
                              != config.disable.end();
        if (disabled) continue;
        const Result<void, RegistryError> applied = applyTo(registry, step.name, builder);
        if (applied.is_err()) {
            return Err(makeError(PipelineConfigErrorKind::RegistryMismatch,
                                 std::string("applyTo rejected '") + step.name + "': "
                                     + to_string(applied.unwrap_err())));
        }
    }
    return Ok();
}

}   // namespace mjbase::spi
