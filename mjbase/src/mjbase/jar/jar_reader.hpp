/// @file        jar_reader.hpp
/// @brief       JAR 读取接口：条目遍历、进度回调、条目名宽容解码、条目元数据列表。
///
/// 直迁自 mbase/jar/jar_reader（语义照旧）。差异点：
///   - 错误通道：mbase 对文件级失败 throw，本版改为 Result<void, JarError>；
///   - 字符串：条目名/路径用 ca::str::Utf8String 家族，原始字节经
///     from_data_unchecked 直通（zip 条目名零解码口径不变）；
///   - 文件 IO 走 ca::fs::FileUtil（UTF-8 路径边界，libjzip 输入不变）。
#pragma once

#include <functional>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "jzip/zip_anomaly.hpp"

#include "jar_error.hpp"

namespace mjbase::jar {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;
using ca::str::Utf8String;
using ca::str::Utf8StringRef;

/// JAR 读取接口。
/// 定义读取 jar 包条目的统一方式。
class IJarReader
{
public:
    /// 带进度的回调签名：(index 1-based, total, 条目名, 条目内容)。
    /// 供 loading 阶段进度反馈用（issue #332）。
    using ProgressConsumer =
        std::function<void(int /*index*/, int /*total*/, const Utf8String& /*name*/,
                           const std::vector<ca::u8>& /*data*/)>;

    /// @brief 条目名宽容解码回调（#1110 族，opt-in，对齐 D40 setLenientTextDecoder
    /// 先例：libjclassfile 注入回调、消费面注入 mjt text_codec 规范口径）。
    ///
    /// zip 条目名在 jzip 层是零解码原始字节（LFH/CEN read_string 直拷，GP bit
    /// 0x800 读入但未用）。注入本回调后，条目名原始字节（GBK 无 flag 名等）经
    /// 回调规范化为合法 UTF-8 再进入资源 URI；未注入时原始字节直通——缺省路径
    /// 逐字节零漂移。回调口径由注入方定（mjt 侧统一注入 text_codec.hpp 的
    /// toCanonicalUtf8：合法 UTF-8 原样 / GBK→UTF-8 / 残字节 U+FFFD）。
    using EntryNameDecoder = std::function<Utf8String(std::string_view)>;
    void setEntryNameDecoder(EntryNameDecoder fn) { entryNameDecoder_ = std::move(fn); }

    virtual ~IJarReader() = default;

    /// 读取 jar，每个条目调用一次 consumer。
    /// @param consumer 回调<条目路径, 条目内容>
    virtual Result<void, JarError> read(
        const std::function<void(const Utf8String&, const std::vector<ca::u8>&)>& consumer) = 0;

    /// @brief 带进度的读取：每个条目回调带 index/total。
    ///
    /// total 来自 zip central directory 一次性解析（archive.getLocalFiles().size()），
    /// 单次解析无重复 IO。默认实现转调 read（index/total 传 0，表示未知），供不关心
    /// 进度的实现兜底；JarReader/JvmJarReader 覆盖为真实进度。
    virtual Result<void, JarError> readWithProgress(const ProgressConsumer& consumer);

protected:
    /// 注入了解码回调则规范化条目名，否则原始字节直通（缺省零漂移的单一收口）。
    Utf8String decodeEntryName(std::string_view raw) const
    {
        if (entryNameDecoder_) return entryNameDecoder_(raw);
        return Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(raw.data()),
                                               raw.size());
    }

private:
    EntryNameDecoder entryNameDecoder_;
};

/// 使用 ForwardScanZipReader 读取 jar。
class JarReader : public IJarReader
{
public:
    explicit JarReader(Utf8StringRef path);
    Result<void, JarError> read(
        const std::function<void(const Utf8String&, const std::vector<ca::u8>&)>& consumer)
        override;
    Result<void, JarError> readWithProgress(const ProgressConsumer& consumer) override;

private:
    Utf8String path_;
};

/// 使用 JvmZipReader 读取 jar（兼容 JVM 边缘情况）。
class JvmJarReader : public IJarReader
{
public:
    explicit JvmJarReader(Utf8StringRef path);
    Result<void, JarError> read(
        const std::function<void(const Utf8String&, const std::vector<ca::u8>&)>& consumer)
        override;
    Result<void, JarError> readWithProgress(const ProgressConsumer& consumer) override;

    /// @brief 最近一次 read 的 ZIP 结构异常报告（读取前为空报告）。
    const jzip::ZipReadReport& lastReadReport() const { return lastReadReport_; }

private:
    Utf8String          path_;
    jzip::ZipReadReport lastReadReport_;
};

/// @brief jar 条目元数据（仅 central directory 信息，不含内容字节）。
struct JarEntryInfo
{
    Utf8String name;
    ca::u64    uncompressedSize = 0;
    /// @brief 压缩后尺寸（CEN 收养值；store 条目等于 uncompressedSize）。
    ca::u64    compressedSize   = 0;
};

/// @brief 解析 jar 的条目元数据：读文件 + 解析 ZIP 结构（central directory 名/大小，
/// 不解压任何条目）。与 JvmJarReader 同一解析语义，供"先显示列表、后台加载内容"的
/// 增量打开流程使用（mjdbg-editor）。
/// @param decoder 条目名宽容解码回调（opt-in，同 IJarReader::setEntryNameDecoder；
/// 空回调 = 原始字节直通）。供 list 类消费面与读取面保持同一解码口径。
Result<std::vector<JarEntryInfo>, JarError>
listJarEntries(const Utf8StringRef& path, const IJarReader::EntryNameDecoder& decoder = {});

}   // namespace mjbase::jar
