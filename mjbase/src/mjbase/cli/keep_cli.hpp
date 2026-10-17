/// @file        keep_cli.hpp
/// @brief       keep 配置的命令行暴露面（审计 P0#1）。
///
/// 三条纪律：
///
/// 1. **只做搬运**：本面把命令行文本搬进 `CliOptions`，再把档位文本解析成
///    `reference::KeepTier`（经 `reference::keepTierFrom`）。规则的**读入**留
///    给命令侧既有 IO 路径（`--keep <file>` 是路径，不是规则内容）——避免
///    CLI 层替消费方决定文件编码与错误呈现。
/// 2. **fail-loud 不降级**：未知档位名 `Err(UnknownTier)`。命令行写错档位比
///    「静默按 none 处理」安全得多（后者会让库型 JAR 的 protected 面被删）。
/// 3. **帮助文本不本地化**：本面提供缺省 help 原文；需要本地化的命令可在
///    注册后用 `ArgParser::add` 覆盖同名 spec（canonical 名见下），或自行
///    渲染 `tr` 后传入。
///
/// canonical 名（消费方据以取值）：`--keep`、`--keep-tier`。
#pragma once

#include <string>
#include <string_view>

#include "libca/core/result.hpp"

#include "mjbase/cli/arg_parser.hpp"
#include "mjbase/cli/cli_runner.hpp"
#include "mjbase/reference/keep_rule.hpp"

namespace mjbase::cli {

// ── canonical option 名（解析取值与覆盖注册共用）──
inline constexpr std::string_view kOptKeep      = "--keep";
inline constexpr std::string_view kOptKeepTier  = "--keep-tier";

/// @brief 注册 keep 相关 option（命令侧一行接入）。
///
/// - `--keep <file>`：keep 规则 DSL 文件路径（String）
/// - `--keep-tier <name>`：`none` / `public` / `public-protected`（String）
///
/// 两个 option 都非必填；缺失时对应字段留空（= 不叠加该来源）。
inline void addKeepOptions(ArgParser& parser)
{
    parser.add({"--keep", "-k"}, OptKind::String, "<file>",
               "keep 规则 DSL 文件（class/member/preset 行式规则）");
    parser.add({"--keep-tier"}, OptKind::String, "<name>",
               "keep 档位：none | public | public-protected");
}

/// @brief 从解析结果取 keep 配置写入 `CliOptions`（未出现的项保持原值）。
///
/// 只覆盖两个 keep 字段，不触碰 input/output 等——命令可自由组合本面与自有
/// 注册项。
inline void applyKeepOptions(const ArgResult& result, CliOptions& options)
{
    if (result.has(std::string(kOptKeep))) {
        options.keepFile = result.getString(std::string(kOptKeep), options.keepFile);
    }
    if (result.has(std::string(kOptKeepTier))) {
        options.keepTier = result.getString(std::string(kOptKeepTier), options.keepTier);
    }
}

/// @brief 档位文本 → `reference::KeepTier`（空串 = None）。
///
/// 未知档位 `Err(KeepRuleErrorCode::UnknownTier)`：命令行档位写错必须稳定
/// 失败，不静默降级为 none。
inline ca::core::Result<reference::KeepTier, reference::KeepRuleErrorCode> keepTierOf(
    const CliOptions& options)
{
    return reference::keepTierFrom(options.keepTier);
}

/// @brief 档位文本 → `reference::KeepTier`（免 CliOptions 的直接形态）。
inline ca::core::Result<reference::KeepTier, reference::KeepRuleErrorCode> keepTierOf(
    std::string_view tierText)
{
    return reference::keepTierFrom(tierText);
}

}   // namespace mjbase::cli
