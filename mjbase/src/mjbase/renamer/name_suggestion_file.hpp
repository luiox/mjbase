/// @file        name_suggestion_file.hpp
/// @brief       AI 建议文件 schema（严格 v1 JSON）+ FileSuggestionProvider。
///
/// 旧栈对应物：renamer/name_suggestion_file。
/// schema 形状、错误分类、行号定位、零应用拒载语义照旧；字符串面按 mjbase
/// 约定改驻留视图（NameSuggestionFile.source 与各 proposal 字段经
/// aliasInterned 驻留，文件顶层 source 落各条 source）。IO 面保持
/// std::fstream 字节读写，路径/文本入参改 std::string_view 借用视图。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/renamer/name_proposal.hpp"
#include "mjbase/renamer/name_provider.hpp"

namespace mjbase::renamer {

/// @brief 建议文件 schema 当前版本（顶层 version 字段；不识别的版本拒载）。
///
/// v1 契约：
/// ```
/// {
///   "version": 1,
///   "source": "ai:xxx",              // 建议来源标签（非空），填充各条建议的 source
///   "proposals": [
///     {
///       "kind": "class|package|field|method",
///       "owner": "...",              // field/method 必填非空；class/package 必须缺省或空
///       "name": "oldName",           // 必填非空
///       "desc": "...",               // field/method 必填非空；class/package 必须缺省或空
///       "newName": "...",            // 必填非空
///       "confidence": 0.92,          // 必填，数值 ∈ [0,1]
///       "rationale": "...",          // 可选：建议理由（审计留痕）
///       "evidence": "..."            // 可选：证据（审计留痕）
///     }
///   ]
/// }
/// ```
inline constexpr ca::u32 kNameSuggestionFileVersion = 1;

/// @brief 建议文件解析/读写错误类别。
enum class SuggestionFileErrorKind : ca::u8
{
    JsonRead,  ///< JSON 语法错误 / 文件打不开
    Schema,    ///< schema 违约：字段缺失/类型/越界/未知字段/重复字段/版本不识别
    Io,        ///< 写出失败
};

/// @brief 建议文件诊断（人读消息；schema 错误含行号定位，文件级 API 再补路径前缀；
///        任一 schema 错误即整文件拒载——零应用口径）。
struct SuggestionFileError
{
    SuggestionFileErrorKind kind = SuggestionFileErrorKind::Schema;
    std::string             message;
};

/// @brief AI 建议文件的 DTO（schema 是引擎输入契约）。
///
/// proposals 已映射到 NameProposal 契约：文件顶层 source 落各条 source，
/// rationale/evidence 透传至同名字段；文件层只做 schema 形状校验，合法性/
/// 存在性/撞名消解全在 RenamerEngine。
struct NameSuggestionFile
{
    ca::u32                   version = kNameSuggestionFileVersion;
    std::string_view          source      = {};   ///< 驻留视图（aliasInterned）
    std::vector<NameProposal> proposals;          ///< 建议批（原样交付引擎）
};

/// @brief ProposalKind → schema 标签（"class"/"package"/"field"/"method"）。
std::string_view proposalKindLabel(ProposalKind kind);

/// @brief schema 标签 → ProposalKind；未知标签返回 false。
bool proposalKindFromLabel(std::string_view label, ProposalKind& out);

/// @brief 解析 v1 建议 JSON 文本并执行严格 schema 校验。
///
/// 任一 schema 错误即整文件拒载（零应用）；错误消息带顶层字段/proposal 序号
/// 及行号。未知/重复字段一律拒绝（严格 v1）。
ca::core::Result<NameSuggestionFile, SuggestionFileError> parseNameSuggestionFile(
    std::string_view text);

/// @brief 从文件读取并解析建议文件；错误消息含路径与行号。
ca::core::Result<NameSuggestionFile, SuggestionFileError> readNameSuggestionFile(
    std::string_view path);

/// @brief 序列化为审阅稿 JSON（pretty）。验收口径：parse(write(parse(x))) 关键字段恒等。
ca::core::Result<std::string, SuggestionFileError> writeNameSuggestionText(
    const NameSuggestionFile& file);

/// @brief 序列化并写出到文件。
ca::core::Result<void, SuggestionFileError> writeNameSuggestionFile(std::string_view path,
                                                                    const NameSuggestionFile& file);

/// @brief 建议文件 provider：持有已解析建议批，propose() 原样产出。
///
/// provider 不做二次判定（合法性/消解全在引擎）；建议缺省 source 由文件顶层
/// source 回填。可跨 run 复用（propose 交付克隆批）。
class FileSuggestionProvider final : public NameProvider
{
public:
    explicit FileSuggestionProvider(NameSuggestionFile file);

    /// @brief provider 标签（"suggestion-file:<source>"；驻留视图）。
    std::string_view name() const override;

    /// @brief 原样产出一批建议；context 只读查询，本 provider 不消费。
    ca::core::Result<std::vector<NameProposal>, std::string> propose(
        const RenameContext& context) override;

private:
    NameSuggestionFile file_;
};

}   // namespace mjbase::renamer
