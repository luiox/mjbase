#pragma once

#include "libca/core/result.hpp"
#include "mapping_file_format.hpp"

namespace mjbase::mapping {

/// @brief shadow-alias sidecar 文件 schema 当前版本。
///
/// v1 契约（shadow name 面）：
/// ```
/// {
///   "version": 1,
///   "aliases": [
///     {"kind": "class",  "name": "<raw internal name>", "shadow": "<display name>"},
///     {"kind": "field",  "owner": "<internal>", "desc": "I",    "name": "<raw>", "shadow": "..."},
///     {"kind": "method", "owner": "<internal>", "desc": "()V",  "name": "<raw>", "shadow": "..."}
///   ]
/// }
/// ```
///
/// v2 契约（shadow 引用 token，**加法式 bump**）：在 v1 基础上
/// 每条 alias 增加可选 `"token"` 列（8 个小写 hex 字符，uint32 %08x 语义；
/// token 为空的条目写出时省略该列）。兼容分支：
/// - 读取侧接受 version 1（无 token 列，条目 token 留空）与 version 2；
///   其余版本拒载。v1 文件出现 token 列 → schema 错误（版本门控新列）。
/// - 写出恒为 version 2。加法式 = 旧文件照常加载，"旧读新拒"不得发生。
/// - token 表内唯一（生成侧 CSPRNG 碰撞重生成保证）；读取侧同口径 fail-closed：
///   手编文件两符号持同一非空 token8 → schema 错误拒载（诊断点名两条冲突
///   owner），不静默去重修复（#1156 落刀 N3）。空 token（v1 遗留）不参与。
///
/// 解析产物为 `IntermediateMapping`：oldName 侧 = 真名（raw），newName 侧 =
/// shadow 显示名，token 列随行（ClassMapping/FieldMapping/MethodMapping::token）。
/// 该产物只服务显示层（shadow table），与真实 remap 数据零混写（不进
/// MemberRemapPlan，不复用 remap 持久化路径）；其余映射格式对 token 恒空。
inline constexpr ca::u32 kShadowAliasFileVersion = 2;

/// @brief sidecar 解析/读写错误类别。
enum class ShadowAliasFileErrorKind : ca::u8
{
    JsonRead,  ///< JSON 语法错误 / 文件打不开
    Schema,    ///< schema 违约：字段缺失/类型/未知 kind/符号重复/版本不识别
    Io,        ///< 写出失败
};

/// @brief sidecar 诊断（人读消息；文件级 API 再补路径前缀）。
struct ShadowAliasFileError
{
    ShadowAliasFileErrorKind kind = ShadowAliasFileErrorKind::Schema;
    ca::str::Utf8String      message;
};

/// @brief shadow-alias sidecar 的映射格式（注册名 "Shadow-Alias"）。
///
/// 读写 roundtrip 走 `MappingFormatRegistry`（与真实 remap 格式并列注册，互不
/// 混写）。`parse()` 遵循注册表"静默跳过坏行"的宽松口径：schema 错误返回空
/// 映射（不产生 Err）；需要错误定位的加载方走 `parseShadowAliasText` 严格入口。
class ShadowAliasFormat final : public MappingFileFormat
{
public:
    ca::str::Utf8StringRef getFormatName() const override;

    ca::Result<IntermediateMapping, MappingError> parse(
        ca::str::Utf8StringRef mappingText) override;

    ca::Result<ca::str::Utf8String, MappingError> format(
        const IntermediateMapping& mappings) override;
};

/// @brief 严格解析 sidecar JSON 文本：任一 schema 错误即整文件拒载（零登记）。
ca::Result<IntermediateMapping, ShadowAliasFileError> parseShadowAliasText(
    const ca::str::Utf8StringRef& text);

/// @brief 从文件读取并严格解析 sidecar；错误消息含路径。
ca::Result<IntermediateMapping, ShadowAliasFileError> readShadowAliasFile(
    const ca::str::Utf8StringRef& path);

/// @brief 序列化 sidecar（pretty JSON，条目按真名字典序稳定排列；version 2，
///        token 非空的条目带 token 列，空则省略——#1156）。
///
/// 验收口径：parse(write(parse(x))) 关键字段恒等。
ca::Result<ca::str::Utf8String, ShadowAliasFileError> writeShadowAliasText(
    const IntermediateMapping& mappings);

/// @brief 序列化并写出到文件。
ca::Result<void, ShadowAliasFileError> writeShadowAliasFile(const ca::str::Utf8StringRef& path,
                                                            const IntermediateMapping& mappings);

}   // namespace mjbase::mapping
