/// @file        jar_writer.hpp
/// @brief       JAR 条目写入接口：回调式收集 → 排序 → 目录条目补全 → DEFLATE 写出。
///
差异点：错误通道 throw →
/// Result<void, JarError>；条目名/路径用 ca::str::Utf8String 家族（jzip 条目
/// 名域以 std::string 为边界类型，进入 jzip 前一次性转换）。
#pragma once

#include <functional>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "jar_error.hpp"

namespace mjbase::jar {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using ca::str::Utf8String;
using ca::str::Utf8StringRef;

/// JAR 条目写入器。
/// 由 IJarWriter::write 的回调持有，调用者通过该接口逐个写条目。
class IEntryWriter
{
public:
    virtual ~IEntryWriter() = default;

    /// 写入一个条目。
    /// @param entryName 条目路径
    /// @param content   条目内容
    ///
    /// 本版形参为借用视图
    /// （Utf8StringRef），条目名在收集器内单次复制驻留，语义等价且调用方
    /// 无需为复用再拷贝一份。
    virtual void writeEntry(Utf8StringRef entryName, std::vector<ca::u8> content) = 0;
};

/// JAR 写入接口。
class IJarWriter
{
public:
    virtual ~IJarWriter() = default;

    /// 写出完整 jar。
    /// @param handler 回调，handler 会被反复调用 writeEntry 直到返回
    virtual Result<void, JarError> write(const std::function<void(IEntryWriter&)>& handler) = 0;
};

/// @brief 使用 DirectZipWriter 写入 jar。
/// 与被删除的 JvmJarWriter 完全同名同义（后者从未做过任何 JVM 特殊处理）。
class JarWriter : public IJarWriter
{
public:
    /// @brief 构造，指定输出路径。
    explicit JarWriter(Utf8StringRef path);
    Result<void, JarError> write(const std::function<void(IEntryWriter&)>& handler) override;

private:
    Utf8String path_;
};

}   // namespace mjbase::jar
