/// @file        summary_scan.hpp
/// @brief       ClassSummary 扫描入口（libjbc 后端）。
///
/// 旧栈有 legacy（jbytecode SKIP 路径）与 jbc 双后端字段级对拍（对拍即回归门）；
/// mjbase 只有 libjbc 一个底座，对拍面随之收敛为单后端字段契约：
/// 对同一 (uri, class 字节) 产出 internalName/superName/access/version/
/// interfaceCount/fieldCount/methodCount 字段（resourceUri 原样回填）。
/// 失败路径统一走 DataLoss。
#pragma once

#include <string>

#include "mjbase/model/resource_model.hpp"
#include "mjbase/workspace/workspace_types.hpp"

namespace mjbase::workspace {

/// @brief libjbc 后端：骨架扫描（不解任何属性体，preserveSourceBytes=false）。
/// @param uri 资源 URI（原样填入 ClassSummary::resourceUri）。
/// @param resource class 字节资源。
/// @return ClassSummary；readClass 失败转 DataLoss。
WorkspaceResult<ClassSummary> decodeClassSummary(const std::string& uri,
                                                 const model::ClassResource& resource);

}   // namespace mjbase::workspace
