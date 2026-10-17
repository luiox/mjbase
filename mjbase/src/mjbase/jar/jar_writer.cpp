#include "jar_writer.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "dos_time_util.hpp"

#include "jzip/decompressor.hpp"
#include "jzip/zip_io.hpp"
#include "libca/core/bytes.hpp"
#include "libca/fs/file_util.hpp"
#include "libca/zip/checksum.hpp"

namespace mjbase::jar {

namespace {

// 收集回调中所有的条目，用于后续统一排序和写入。
// 条目名按原始字节驻留（from_data_unchecked，与 mbase 的零解码直通一致）。
struct Collector : IEntryWriter
{
    std::vector<std::pair<Utf8String, std::vector<ca::u8>>> entries;
    void writeEntry(Utf8StringRef name, std::vector<ca::u8> data) override
    {
        entries.emplace_back(Utf8String::from_data_unchecked(name.data(), name.byte_length()),
                             std::move(data));   // JAR-3：条目名单次复制驻留，内容零额外拷贝
    }
};

// 将路径分隔符统一为 POSIX 风格（jzip 条目名域以 std::string 为边界类型）。
std::string normalize_path(const std::string& name)
{
    std::string out = name;
    for (auto& ch : out) {
        if (ch == '\\') ch = '/';
    }
    return out;
}

// fs 层错误 → jar 错误（写侧）：打开失败与写出失败分开（对应 mbase 的两种 throw，
// 写路径连 FileNotFound 都归结为"打不开输出 jar"）。
JarError::Kind writeErrorKind(ca::fs::FsError e)
{
    switch (e) {
    case ca::fs::FsError::OpenFailed:
    case ca::fs::FsError::PermissionDenied:
    case ca::fs::FsError::IsADirectory:
    case ca::fs::FsError::NotARegularFile:
    case ca::fs::FsError::FileNotFound:
        return JarError::Kind::OpenFailed;
    default:
        return JarError::Kind::WriteFailed;
    }
}

// 将收集的条目写入 jar：排序 → 生成目录条目 → 写入文件条目（DEFLATE 压缩）
Result<void, JarError> writeArchive(const Utf8StringRef& path, Collector& collector)
{
    auto dosTime = current_dos_time();
    auto dosDate = current_dos_date();

    // 按文件名排序，保证可重现构建（Utf8StringRef 的字节级字典序与
    // mbase 的 std::string 排序同序）。
    std::sort(collector.entries.begin(), collector.entries.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    jzip::ZipArchive                archive;
    std::unordered_set<std::string> dirs;

    for (auto& [name, data] : collector.entries) {
        auto entryName = normalize_path(name.to_std_string());

        // 为每级路径创建 STORED 目录条目（仅首次遇到时）
        auto slash = entryName.find('/');
        while (slash != std::string::npos) {
            std::string dirPath = entryName.substr(0, slash + 1);
            if (dirs.insert(dirPath).second) {
                auto dirEntry = std::make_unique<jzip::LocalFileHeader>();
                dirEntry->setFileName(dirPath);
                dirEntry->setCompressionMethod(0);   // STORED
                dirEntry->setCompressedSize(0);
                dirEntry->setUncompressedSize(0);
                dirEntry->setCrc32(0);
                dirEntry->setGeneralPurposeBitFlag(0x0800);   // bit11=UTF-8
                dirEntry->setLastModFileTime(dosTime);
                dirEntry->setLastModFileDate(dosDate);
                dirEntry->setOffset(0);
                archive.addPart(std::move(dirEntry));
            }
            slash = entryName.find('/', slash + 1);
        }

        // 跳过纯目录条目（已在上面处理）
        if (!entryName.empty() && entryName.back() == '/') continue;

        // 计算 CRC32 并对非空数据执行 DEFLATE 压缩
        auto       local = std::make_unique<jzip::LocalFileHeader>();
        ca::zip::Crc32 crcCalc;
        crcCalc.update(data.data(), data.size());
        ca::u32 crc = crcCalc.value();

        if (data.empty()) {
            local->setCompressionMethod(0);
            local->setCompressedSize(0);
            local->setUncompressedSize(0);
        }
        else {
            auto compressed = jzip::DeflateCompressor().compress(data);
            local->setCompressionMethod(8);
            local->setCompressedSize(static_cast<ca::u32>(compressed.size()));
            local->setUncompressedSize(static_cast<ca::u32>(data.size()));
            local->setFileData(std::move(compressed));
        }

        local->setFileName(entryName);
        local->setCrc32(crc);
        local->setLastModFileTime(dosTime);
        local->setLastModFileDate(dosDate);
        local->setGeneralPurposeBitFlag(0x0800);
        local->setOffset(0);
        archive.addPart(std::move(local));
    }
    auto outBytes = jzip::ZipIO::writeDirect(archive);
    // 写出与关闭校验由 FileUtil::write_bytes 承担：磁盘满/断连等写失败返回 Err，
    // 绝不静默返回一个截断的 jar。
    const auto written = ca::fs::FileUtil::write_bytes(
        path.to_std_string(), ca::core::ByteSlice(outBytes.data(), outBytes.size()));
    if (written.is_err()) {
        JarError err;
        err.kind = writeErrorKind(std::move(written).unwrap_err());
        err.path = Utf8String::from_data_unchecked(path.data(), path.byte_length());
        return Err(std::move(err));
    }
    return Ok();
}

}   // namespace

// ── JarWriter ────────────────────────────────────────────────

JarWriter::JarWriter(Utf8StringRef path)
    : path_(Utf8String::from_data_unchecked(path.data(), path.byte_length()))
{}

// 收集回调中的条目并写出为标准 jar
Result<void, JarError> JarWriter::write(const std::function<void(IEntryWriter&)>& handler)
{
    Collector collector;
    handler(collector);
    return writeArchive(path_.ref(), collector);
}

}   // namespace mjbase::jar
