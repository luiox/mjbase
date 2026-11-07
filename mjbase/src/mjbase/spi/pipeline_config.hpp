/// @file        pipeline_config.hpp
/// @brief       声明式管线配置面：JSON 文本 → 装载 + fail-closed 校验门 → 声明结构。
///
/// 单职责：只做「声明 → 结构」的装载与校验（JSON schema + 解析 + 校验门），
/// 执行编排消费产出的 LoadedPipelineConfig——直引 pass 条目可经
/// applyConfigTo 逐条投入 PipelineBuilder（复用 applyTo 段位分派），内建
/// 管线引用的展开与禁用名单的减除接线 phase 2（builder 无合并 API，见
/// 规格 §分阶段）。不注册 pass、不改注册表：三张注册表面（PassNameRegistry
/// 全集 / PassRegistry 内建管线集 / per-pass 参数 schema 声明表）在装载期
/// 只读。规格与 schema 全文：doc:pipeline-config-spec。
///
/// 校验门 fail-closed 全集（逐桶 detail 携带定位与口径）：
///   - BadJson / InvalidField / SchemaVersion（版本门风格对齐 analysis-db）；
///   - UnknownPass（序列与禁用名单均对 PassNameRegistry 全集查名；``<pack>.``
///     命名空间 pass 由宿主 installPack 注册进同一注册表后即可引用，校验
///     面自然覆盖 plugin 注册集）；
///   - UnknownParamKey / ParamTypeMismatch / ParamSchemaUndeclared（未声明
///     schema 的 pass 携带参数一律拒——schema 缺位不得静默丢参）；
///   - DuplicatePass（非幂等件逐一登记表裁决）/ DuplicateDisable；
///   - EmptySequence（声明空与禁用减空都拒）/ ScopeUnsupported（Cf 域无
///     容器管线槽位）；
///   - BuiltinRefNotExpandable / RegistryMismatch（装配期防御桶）。
///
/// 寿命与线程：入参 text 借用（装载完成即不再引用）；产物自持字符串
/// （外部边界，std::string 允许——对齐 PackError 的边界豁免先例）。装载读
/// 全局注册表，对齐注册表纪律：初始化阶段调用，非线程安全。
#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_registry.hpp"

namespace mjbase::spi {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// per-pass 参数的声明 kind（v1 标量族；复合值 phase 2，见规格）。
enum class PipelineConfigParamKind : ca::u8
{
    Bool = 0,   ///< JSON bool
    Int,        ///< JSON 整数（i64）
    Float,      ///< JSON 浮点（int 字面量按数值家族晋升接受）
    String,     ///< JSON 非空字符串
};

const char* to_string(PipelineConfigParamKind kind) noexcept;

/// @brief 单个参数值（标量家族，variant 存储；index 与
///        PipelineConfigParamKind 枚举序一一对应）。
struct PipelineConfigParamValue
{
    std::variant<bool, ca::i64, ca::f64, std::string> value{false};

    /// 按 variant 当前分支取声明 kind。
    PipelineConfigParamKind kind() const noexcept
    {
        return static_cast<PipelineConfigParamKind>(value.index());
    }

    bool operator==(const PipelineConfigParamValue& other) const { return value == other.value; }
};

/// 单个 per-pass 参数（key/value 对，声明序保存）。
struct PipelineConfigParam
{
    std::string             key;
    PipelineConfigParamValue value;

    bool operator==(const PipelineConfigParam& other) const
    {
        return key == other.key && value == other.value;
    }
};

/// @brief 序列条目：注册 pass 直引或内建管线前缀引用。
///
/// 同名解析序：先 PassNameRegistry（pass 名，含 ``<pack>.`` 命名空间），
/// 后 PassRegistry（内建管线名）——两表撞名时 pass 名赢（直引是常态引用
/// 面，registry 撞名有 DuplicateName 门，内建管线名不占 pass 名空间）。
struct PipelineConfigStep
{
    std::string name;                     ///< 注册 pass 名或内建管线名
    bool        builtinRef = false;       ///< true = 内建管线引用（PassRegistry 集）
    PassScope   scope      = PassScope::Method;   ///< registry.find 查得的段位（builtinRef 无意义）
    std::vector<PipelineConfigParam> params;      ///< per-pass 参数（声明序；内建引用恒空）

    bool operator==(const PipelineConfigStep& other) const
    {
        return name == other.name && builtinRef == other.builtinRef && scope == other.scope
               && params == other.params;
    }
};

/// @brief 装载产物：一条管线的声明结构（执行编排的装配输入）。
///
/// 幂等装载：同一文本对同一注册表重复 load 结果逐字段一致（纯函数，
/// 序列与禁用名单保持声明序）。
struct LoadedPipelineConfig
{
    int                             schemaVersion = 0;   ///< 版本门通过值（当前恒 1）
    std::vector<PipelineConfigStep> sequence;            ///< 声明序；装载期拒空
    std::vector<std::string>        disable;             ///< 显式禁用名单（声明序，已验名）

    bool operator==(const LoadedPipelineConfig& other) const
    {
        return schemaVersion == other.schemaVersion && sequence == other.sequence
               && disable == other.disable;
    }
};

/// 装载/校验/装配失败码（detail 携带定位前缀与口径说明，外部边界文本）。
enum class PipelineConfigErrorKind : ca::u8
{
    Ok = 0,
    BadJson,                 ///< JSON 语法错误（detail 带解析器行/列与消息）
    InvalidField,            ///< 字段缺失/类型不符/未知字段/重复字段/空串
    SchemaVersion,           ///< version 门不识别（supported 区间见 detail）
    UnknownPass,             ///< 序列/禁用名单名字对注册表全集未命中
    UnknownParamKey,         ///< 参数键未在该 pass 的声明 schema
    ParamTypeMismatch,       ///< 参数值家族与声明 kind 不符
    ParamSchemaUndeclared,   ///< 携带参数但该 pass 无声明 schema（fail-closed）
    DuplicatePass,           ///< 非幂等件（逐一登记表）重复出现
    DuplicateDisable,        ///< 禁用名单内部重复
    EmptySequence,           ///< 声明空序列或禁用减空
    ScopeUnsupported,        ///< Cf 域 pass 无容器管线槽位
    BuiltinRefNotExpandable, ///< 装配期遇内建管线引用（展开接线 phase 2）
    RegistryMismatch,        ///< 装配注册表与装载注册表不一致（防御桶）
};

const char* to_string(PipelineConfigErrorKind kind) noexcept;

/// 装载失败（kind + 可诊断文本）。
struct PipelineConfigError
{
    PipelineConfigErrorKind kind = PipelineConfigErrorKind::Ok;
    std::string             detail;   ///< 外部边界文本，std::string 允许
};

/// @brief 装载 + 校验一条管线配置（fail-closed 全门，见文件头桶清单）。
///
/// @param text      UTF-8 JSON 文本（借用视图，装载完成即不再引用）
/// @param registry  pass 名全集（defaultPassRegistry 或宿主经 installPack
///                  装入 pack pass 后的自持表）；只读
/// @param pipelines 内建管线集（registerBuiltinPipelines 后有效）；只读。
///                  未 bootstrap 时内建管线引用按 UnknownPass 拒
/// @return 成功返回自持的声明结构；失败返回首个命中桶（逐门顺序见规格）
Result<LoadedPipelineConfig, PipelineConfigError>
    loadPipelineConfig(std::string_view text, const PassNameRegistry& registry,
                       const PassRegistry& pipelines = PassRegistry::instance());

/// @brief 声明结构 → PipelineBuilder 装配（复用 applyTo 逐条段位分派）。
///
/// v1 口径：直引 pass 条目逐条 applyTo；disable 命中的直引条目跳过（名字
/// 精确匹配，一次命中移除全部同名条目）；内建管线引用 v1 不展开（builder
/// 无合并 API）→ Err(BuiltinRefNotExpandable)。与 loadPipelineConfig 用
/// 同一注册表；不一致走 RegistryMismatch 防御桶（load 后注册表变更）。
Result<void, PipelineConfigError> applyConfigTo(const PassNameRegistry&     registry,
                                                const LoadedPipelineConfig& config,
                                                PipelineBuilder&            builder);

}   // namespace mjbase::spi
