/// @file        resource_io.hpp
/// @brief       资源导入导出：接口 + Jar/Class 导入导出器。
///
/// 有意重设计：
/// - 错误：mbase 以 throw std::runtime_error 报错，本模块全部改 ModelResult<T>
///   值语义，抛点消息文本 → 错误码的映射见各函数注释（码表见 ModelError）；
/// - jar 依赖换 mjbase::jar（错误走 Result<void, JarError>）——
///   JarError 经 toModelError 归一为本模块错误码；
/// - 压缩包解包面不随模块迁移：mjbase 全家零调用方，解包能力随需求
///   下沉消费方（mj2x 有下载后解压场景）。
#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jar/jar_error.hpp"
#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_writer.hpp"

#include "resource_container.hpp"
#include "resource_model.hpp"

namespace mjbase::model {

/// @brief JarError → ModelError 归一映射：FileNotFound 细分为 NotFound，
///        其余（OpenFailed/ReadFailed/WriteFailed）按 Io。
ModelError toModelError(const mjbase::jar::JarError& error) noexcept;

// ── 导入导出接口 ──────────────────────────────────────────────

/// @brief 资源导出器接口。
class IResourceExporter
{
public:
    virtual ~IResourceExporter() = default;
    /// @brief 将容器中所有资源导出。
    virtual ModelResult<void> exportResource(IResourceContainer& container) = 0;
};

/// @brief 资源导入器接口。
class IResourceImporter
{
public:
    virtual ~IResourceImporter() = default;
    /// @brief 从外部源导入资源到容器。
    virtual ModelResult<void> importResource(IResourceContainer& container) = 0;
};

// ── Jar 导入导出 ──────────────────────────────────────────────

/// @brief 将 jar 包内容导入到资源容器。
class JarFileImporter : public IResourceImporter
{
public:
    /// loading 进度回调：(index 1-based, total, entry 名)。供进度反馈用。
    using ProgressCallback = std::function<void(int, int, std::string_view)>;

    /// @brief 构造，指定 jar 读取器。
    explicit JarFileImporter(mjbase::jar::IJarReader& reader)
        : reader_(&reader)
    {}
    /// @brief 遍历 jar 条目，按名字/内容分拣入库；读取失败（JarError）经
    ///        toModelError 归一为 Err(NotFound/Io)。
    ModelResult<void> importResource(IResourceContainer& container) override;

    /// @brief 带进度的导入：经 readWithProgress 读条目，每个条目触发 progress 回调。
    ///
    /// @param container 目标容器
    /// @param progress  进度回调（每个 entry 一次，index/total/name）
    ModelResult<void> importResourceWithProgress(IResourceContainer&     container,
                                                 const ProgressCallback& progress);

private:
    mjbase::jar::IJarReader* reader_;
};

/// @brief 将资源容器中的资源写入 jar 包。
class JarFileExporter : public IResourceExporter
{
public:
    /// @brief 构造，指定 jar 写入器。
    explicit JarFileExporter(mjbase::jar::IJarWriter& writer)
        : writer_(&writer)
    {}
    /// @brief 遍历容器按类型写条目；写出失败（JarError）经 toModelError 归一为 Err。
    ModelResult<void> exportResource(IResourceContainer& container) override;

private:
    mjbase::jar::IJarWriter* writer_;
};

// ── Class 文件导入导出 ────────────────────────────────────────

/// @brief 将单个 class 文件导出到磁盘。
class SimpleClassFileExporter : public IResourceExporter
{
public:
    /// @brief 构造，指定容器内资源 URI 和目标磁盘路径。
    SimpleClassFileExporter(std::string_view uri, std::string_view path);
    /// @brief 错误码：BadArgument（空 URI 且容器非唯一资源）、NotFound（URI 不存在）、
    ///        TypeMismatch（目标资源非 ClassResource）、Io（打开/写出失败）。
    ModelResult<void> exportResource(IResourceContainer& container) override;

private:
    ca::str::Utf8String uri_;
    ca::str::Utf8String path_;
};

/// @brief 从磁盘导入单个 class 文件。
class SimpleClassFileImporter : public IResourceImporter
{
public:
    /// @brief 构造，指定磁盘文件路径。
    explicit SimpleClassFileImporter(std::string_view path)
        : path_(ownText(path))
    {}
    /// @brief 错误码：Io（文件打开失败）。
    ModelResult<void> importResource(IResourceContainer& container) override;

private:
    ca::str::Utf8String path_;
};

}   // namespace mjbase::model
