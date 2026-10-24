/// @file        resource_helper.hpp
/// @brief       资源操作辅助工具：导入导出静态便捷方法。
///
/// 错误改 ModelResult 值语义
/// （mjbase::jar 的 Result<void, JarError> 经 toModelError 归一映射）。
#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "libca/core/result.hpp"

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_writer.hpp"

#include "resource_container.hpp"

namespace mjbase::model {

/// @brief 资源操作辅助工具类，提供常用的导入导出静态方法。
class ResourceHelper
{
public:
    /// loading 进度回调：(index 1-based, total, entry 名)。
    using ProgressCallback = std::function<void(int, int, std::string_view)>;

    /// @brief 从磁盘读取 class 文件并添加到容器。
    /// @param container 目标容器
    /// @param filePath  磁盘上的 class 文件路径
    static ModelResult<void> importFromClassFile(ResourceContainer& container,
                                                 std::string_view    filePath);
    /// @brief 从 jar 文件导入所有资源到容器。
    /// @param container 目标容器
    /// @param filePath  jar 文件路径
    /// @param reader    可选的自定义读取器，默认使用 JvmJarReader（兼容 data descriptor）
    static ModelResult<void> importFromJar(ResourceContainer& container, std::string_view filePath,
                                           std::unique_ptr<mjbase::jar::IJarReader> reader = nullptr);
    /// @brief 从 jar 文件导入（#1110 族条目名宽容解码注入面）。
    /// @param decoder 条目名宽容解码回调（opt-in；空 = 原始字节直通零漂移，
    ///                与 mjbase::jar::listJarEntries 同口径）。内部自建 JvmJarReader
    ///                并注入；自带 reader 的调用方走 reader 重载自行注入。
    static ModelResult<void> importFromJar(ResourceContainer&                       container,
                                           std::string_view                         filePath,
                                           const mjbase::jar::IJarReader::EntryNameDecoder& decoder);
    /// @brief 从 jar 文件导入，带 loading 进度回调（见
    /// JarFileImporter::importResourceWithProgress）。
    static ModelResult<void> importFromJar(ResourceContainer&      container,
                                           std::string_view        filePath,
                                           const ProgressCallback& progress,
                                           std::unique_ptr<mjbase::jar::IJarReader> reader = nullptr);
    /// @brief 解析 jar 的条目元数据（名 + 未压缩大小，不解压内容），
    /// 供"先显示列表、后台加载内容"的增量打开流程使用（见
    /// mjbase::jar::listJarEntries）。
    static ModelResult<std::vector<mjbase::jar::JarEntryInfo>> listJarEntries(std::string_view path);
    /// @brief 将容器中所有资源导出到 jar 文件。
    /// @param container 源容器
    /// @param filePath  输出 jar 路径
    /// @param writer    可选的自定义写入器，默认使用 JarWriter
    static ModelResult<void> exportToJar(ResourceContainer& container, std::string_view filePath,
                                         std::unique_ptr<mjbase::jar::IJarWriter> writer = nullptr);
    /// @brief 将容器中指定 class 资源导出到磁盘文件。
    /// @param container    源容器
    /// @param filePath     目标磁盘路径
    /// @param resourcePath 容器中资源的 URI
    static ModelResult<void> exportToClassFile(ResourceContainer& container,
                                               std::string_view   filePath,
                                               std::string_view   resourcePath);
};

}   // namespace mjbase::model
