#include "jar_reader.hpp"

#include <utility>

#include "libca/core/bytes.hpp"
#include "libca/fs/file_util.hpp"

#include "jzip/zip_io.hpp"

namespace mjbase::jar {

namespace {

// fs 层错误 → jar 错误（读侧）：FileNotFound 与打开失败分开，读取失败单独成类。
JarError toJarError(ca::fs::FsError e, const Utf8StringRef& path)
{
    JarError err;
    switch (e) {
    case ca::fs::FsError::FileNotFound: err.kind = JarError::Kind::FileNotFound; break;
    case ca::fs::FsError::ReadFailed:   err.kind = JarError::Kind::ReadFailed;   break;
    default:                            err.kind = JarError::Kind::OpenFailed;   break;
    }
    err.path = Utf8String::from_data_unchecked(path.data(), path.byte_length());
    return err;
}

// 读取整个文件到内存（两个 reader 与 listJarEntries 共用，避免重复样板）。
Result<ca::core::Bytes, JarError> readFileBytes(const Utf8StringRef& path)
{
    auto bytes = ca::fs::FileUtil::read_all_bytes(path.to_std_string());
    if (bytes.is_err()) return Err(toJarError(std::move(bytes).unwrap_err(), path));
    return Ok(std::move(bytes).unwrap());
}

}   // namespace

// ── IJarReader 基类默认实现：转调无进度 read，index/total 传 0（未知）──
Result<void, JarError> IJarReader::readWithProgress(const ProgressConsumer& consumer)
{
    return read([&](const Utf8String& name, const std::vector<ca::u8>& data) {
        consumer(0, 0, name, data);   // 0/0 表示进度未知
    });
}

JarReader::JarReader(Utf8StringRef path)
    : path_(Utf8String::from_data_unchecked(path.data(), path.byte_length()))
{}

// 使用 standard zip 读取器解析 jar，逐条目回调 consumer
Result<void, JarError> JarReader::read(
    const std::function<void(const Utf8String&, const std::vector<ca::u8>&)>& consumer)
{
    auto file = readFileBytes(path_.ref());
    if (file.is_err()) return Err(std::move(file).unwrap_err());
    const auto bytes = std::move(file).unwrap();

    auto archive = jzip::ZipIO::readStandard(jzip::MemorySegment(bytes.as_ptr(), bytes.len()));
    for (const auto* local : archive.getLocalFiles())
        consumer(decodeEntryName(local->getFileName()), jzip::ZipIO::decompressLocalFile(*local));
    return Ok();
}

// 带进度的版本：单次解析，total 来自 getLocalFiles().size()，无重复 IO。
Result<void, JarError> JarReader::readWithProgress(const ProgressConsumer& consumer)
{
    auto file = readFileBytes(path_.ref());
    if (file.is_err()) return Err(std::move(file).unwrap_err());
    const auto bytes = std::move(file).unwrap();

    auto archive = jzip::ZipIO::readStandard(jzip::MemorySegment(bytes.as_ptr(), bytes.len()));
    const auto& locals = archive.getLocalFiles();
    const int   total  = static_cast<int>(locals.size());
    int         index  = 0;
    for (const auto* local : locals) {
        ++index;   // 1-based
        consumer(index, total, decodeEntryName(local->getFileName()),
                 jzip::ZipIO::decompressLocalFile(*local));
    }
    return Ok();
}

JvmJarReader::JvmJarReader(Utf8StringRef path)
    : path_(Utf8String::from_data_unchecked(path.data(), path.byte_length()))
{}

// 使用 JVM 兼容 zip 读取器解析 jar，逐条目回调 consumer
Result<void, JarError> JvmJarReader::read(
    const std::function<void(const Utf8String&, const std::vector<ca::u8>&)>& consumer)
{
    auto file = readFileBytes(path_.ref());
    if (file.is_err()) return Err(std::move(file).unwrap_err());
    const auto bytes = std::move(file).unwrap();

    auto archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes.as_ptr(), bytes.len()));
    lastReadReport_ = archive.getReadReport();
    for (const auto* local : archive.getLocalFiles())
        consumer(decodeEntryName(local->getFileName()), jzip::ZipIO::decompressLocalFile(*local));
    return Ok();
}

Result<void, JarError> JvmJarReader::readWithProgress(const ProgressConsumer& consumer)
{
    auto file = readFileBytes(path_.ref());
    if (file.is_err()) return Err(std::move(file).unwrap_err());
    const auto bytes = std::move(file).unwrap();

    auto        archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes.as_ptr(), bytes.len()));
    lastReadReport_     = archive.getReadReport();
    const auto& locals  = archive.getLocalFiles();
    const int   total   = static_cast<int>(locals.size());
    int         index   = 0;
    for (const auto* local : locals) {
        ++index;
        consumer(index, total, decodeEntryName(local->getFileName()),
                 jzip::ZipIO::decompressLocalFile(*local));
    }
    return Ok();
}

// 仅解析条目元数据：读文件 + 解析 ZIP 结构，不解压任何条目。local 头经 CEN 关联后
// 携带权威未压缩大小（zip_model 的 adopt 语义），与 JvmJarReader 的条目顺序一致。
Result<std::vector<JarEntryInfo>, JarError>
listJarEntries(const Utf8StringRef& path, const IJarReader::EntryNameDecoder& decoder)
{
    auto file = readFileBytes(path);
    if (file.is_err()) return Err(std::move(file).unwrap_err());
    const auto bytes = std::move(file).unwrap();

    auto archive = jzip::ZipIO::readJvm(jzip::MemorySegment(bytes.as_ptr(), bytes.len()));
    const auto& locals = archive.getLocalFiles();
    std::vector<JarEntryInfo> out;
    out.reserve(locals.size());
    for (const auto* local : locals) {
        JarEntryInfo info;
        const auto&  rawName = local->getFileName();
        info.name = decoder ? decoder(rawName)
                            : Utf8String::from_data_unchecked(
                                  reinterpret_cast<const ca::u8*>(rawName.data()), rawName.size());
        info.uncompressedSize = static_cast<ca::u64>(local->getUncompressedSize());
        info.compressedSize   = static_cast<ca::u64>(local->getCompressedSize());
        out.push_back(std::move(info));
    }
    return Ok(std::move(out));
}

}   // namespace mjbase::jar
