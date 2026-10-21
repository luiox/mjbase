/// @file        resource_model.hpp
/// @brief       资源模型：ResourceType 枚举 + IResource 接口与三实现 + ModelError。
///
/// 资源是「URI + 类型化载荷」的最小抽象——Class（class 字节）、Manifest
/// （MANIFEST.MF 解析物）、Unknown（原样字节）。类型识别用 ca::core::tag_cast：
/// 每个叶子类型把 TypeOf<T> 特化到 ResourceType 标签值，与运行期 getType()
/// 比较，零 RTTI。
///
/// 设计约定：
/// - 错误统一走 ModelResult<T>（= Result<T, ModelError>）值语义，错误码定义
///   见 ModelError，各分支的归因写在 resource_io 的对应返回处；
/// - 字符串：借用的路径/URI 一律借用视图（容器/工具入参 std::string_view，
///   资源 location 返回 Utf8StringRef——与 mjbase::jar 的写条目口径对齐）；
///   自有存储用 ca::str 家族（Utf8String，原始字节保真不校验），不新增
///   std::string 存储；
/// - jar 依赖用 mjbase::jar（接口同名，错误走 Result<void, JarError>）。
#pragma once

#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jar/jar_util.hpp"
#include "mjbase/jar/manifest.hpp"

namespace mjbase::model {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief 资源类型枚举（同时充当 tag_cast 的类型标签值）。
enum class ResourceType : ca::u8
{
    Class,    ///< class 文件
    Manifest, ///< MANIFEST.MF 文件
    Jar,      ///< jar 包
    Unknown,  ///< 其他未知类型
    SIZE,     ///< 哨兵：枚举个数（查表用）
};

/// @brief ResourceType → 短名（"Class"/"Manifest"/"Jar"/"Unknown"）。
inline std::string_view to_string(ResourceType type) noexcept
{
    constexpr std::array<std::string_view, static_cast<ca::usize>(ResourceType::SIZE)> kNames = {
        "Class", "Manifest", "Jar", "Unknown"};
    return kNames[static_cast<ca::usize>(type)];
}

/// @brief 字符串 → ResourceType；未知名归 Unknown。
inline ResourceType resourceTypeFromString(std::string_view name) noexcept
{
    if (name == "Class") return ResourceType::Class;
    if (name == "Manifest") return ResourceType::Manifest;
    if (name == "Jar") return ResourceType::Jar;
    return ResourceType::Unknown;
}

/// @brief model 模块错误码。
///
/// 各码归因具体分支（按 resource_io / helper 的返回点定义）：
enum class ModelError : ca::u8
{
    Io,                ///< 磁盘文件打开/写出失败
    BadArgument,       ///< 参数约束不满足（如未指定 URI 而容器不是恰好 1 个资源）
    NotFound,          ///< 容器 URI / 归档文件 / 归档条目不存在
    TypeMismatch,      ///< 期望 ClassResource 实为其他类型
    BadEntry,          ///< 归档头/条目数据读取失败
    PathEscape,        ///< 归档条目路径逃逸目标目录（zip-slip 防线）
    UnsupportedFormat, ///< 归档格式/过滤器嗅探失败
};

/// @brief 错误码 → 短名（日志/断言输出用）。
inline const char* to_string(ModelError code) noexcept
{
    switch (code) {
    case ModelError::Io: return "Io";
    case ModelError::BadArgument: return "BadArgument";
    case ModelError::NotFound: return "NotFound";
    case ModelError::TypeMismatch: return "TypeMismatch";
    case ModelError::BadEntry: return "BadEntry";
    case ModelError::PathEscape: return "PathEscape";
    case ModelError::UnsupportedFormat: return "UnsupportedFormat";
    }
    return "?";
}

/// @brief model 模块 Result 别名：错误统一走 ModelError 值语义，不抛异常。
template <typename T>
using ModelResult = Result<T, ModelError>;

/// @brief 借用视图 → 自有 Utf8String（原始字节保真，不校验 UTF-8——jar 条目名
///        与磁盘路径可能是任意字节；空视图得空串）。
inline ca::str::Utf8String ownText(std::string_view view) noexcept
{
    return ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(view.data()),
                                                    view.size());
}

/// @brief 资源对象通用接口。
class IResource
{
public:
    virtual ~IResource() = default;
    /// @brief 获取资源 URI（jar 内路径或磁盘路径）。
    ///
    /// 返回借用视图：指向资源自持文本，资源存活期内有效（不再按值拷贝返回）；
    /// Utf8StringRef 可直接喂给 mjbase::jar 的写条目接口。
    virtual ca::str::Utf8StringRef getLocation() const = 0;
    /// @brief 获取资源类型。
    virtual ResourceType getType() const { return ResourceType::Unknown; }
};

/// @brief class 文件资源。
class ClassResource : public IResource
{
public:
    /// @brief 构造，指定路径和字节内容。
    ClassResource(std::string_view path, std::vector<ca::u8> content)
        : path_(ownText(path))
        , content_(std::move(content))
    {}
    ca::str::Utf8StringRef getLocation() const override { return path_.ref(); }
    ResourceType          getType() const override { return ResourceType::Class; }

    /// @brief 获取 class 字节数据。
    const std::vector<ca::u8>& get() const { return content_; }
    /// @brief 替换 class 字节数据。
    void set(std::vector<ca::u8> bytes) { content_ = std::move(bytes); }

private:
    ca::str::Utf8String path_;   ///< URI/location 自持文本（原始字节保真）
    std::vector<ca::u8> content_;
};

/// @brief MANIFEST.MF 资源。
class ManifestResource : public IResource
{
public:
    /// @brief 构造，封装 Manifest 对象。
    explicit ManifestResource(mjbase::jar::Manifest manifest)
        : manifest_(std::move(manifest))
    {}
    /// @brief 从字节数据构造（Manifest::fromBytes 宽松解析，不因畸形内容失败）。
    static ManifestResource fromBytes(const std::vector<ca::u8>& bytes)
    {
        return ManifestResource(mjbase::jar::Manifest::fromBytes(bytes));
    }
    /// @brief URI 恒为 jar 规范的 MANIFEST.MF 路径。
    ca::str::Utf8StringRef getLocation() const override
    {
        return ca::str::Utf8StringRef::from_cstr(mjbase::jar::JarUtil::kManifestFileName);
    }
    ResourceType getType() const override { return ResourceType::Manifest; }
    /// @brief 获取 Manifest 对象引用。
    const mjbase::jar::Manifest& get() const { return manifest_; }
    /// @brief 替换 Manifest 数据。
    void set(mjbase::jar::Manifest manifest) { manifest_ = std::move(manifest); }

private:
    mjbase::jar::Manifest manifest_;
};

/// @brief 未知类型资源。
class UnknownResource : public IResource
{
public:
    /// @brief 构造，指定路径和内容。
    UnknownResource(std::string_view path, std::vector<ca::u8> content)
        : path_(ownText(path))
        , content_(std::move(content))
    {}
    ca::str::Utf8StringRef getLocation() const override { return path_.ref(); }
    /// @brief 获取原始字节数据。
    const std::vector<ca::u8>& get() const { return content_; }
    /// @brief 替换原始字节数据。
    void set(std::vector<ca::u8> bytes) { content_ = std::move(bytes); }

private:
    ca::str::Utf8String path_;   ///< URI/location 自持文本（原始字节保真）
    std::vector<ca::u8> content_;
};

}   // namespace mjbase::model

namespace ca::core::tag_cast {

template<>
struct TypeOf<mjbase::model::ClassResource>
{
    static constexpr auto value = mjbase::model::ResourceType::Class;
};

template<>
struct TypeOf<mjbase::model::ManifestResource>
{
    static constexpr auto value = mjbase::model::ResourceType::Manifest;
};

template<>
struct TypeOf<mjbase::model::UnknownResource>
{
    static constexpr auto value = mjbase::model::ResourceType::Unknown;
};
}   // namespace ca::core::tag_cast
