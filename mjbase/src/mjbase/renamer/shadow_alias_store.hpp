#pragma once

#include "libca/core/result.hpp"

#include "mjbase/project/analysis_db.hpp"
#include "mjbase/renamer/shadow_alias.hpp"

namespace mjbase::renamer {

/// @file shadow_alias_store.hpp
/// @brief ShadowAliasTable 与分析数据库重命名覆盖层的适配。
///
/// shadow 名字属于用户数据：生成后必须跨会话保留。本适配把整表搬进
/// `renames`（原子替换），加载时再还原成 ShadowAliasTable。sidecar 文件只在
/// 导入导出时出现，运行时以数据库为准。

/// @brief 用 shadow 表原子替换数据库中的全部重命名覆盖（单事务）。
///        身份登记保留，class_id / member_id 不重排。
ca::Result<void, project::AnalysisDbError> importShadowAliases(
    project::AnalysisDb& db, const ShadowAliasTable& table);

/// @brief 把数据库中的全部重命名覆盖还原为 shadow 表（空库返回空表）。
ca::Result<ShadowAliasTable, project::AnalysisDbError> loadShadowAliases(
    const project::AnalysisDb& db);

}   // namespace mjbase::renamer
