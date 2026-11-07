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

// BadEntryName 错误载荷：path 装非法条目名本身（诊断指向出问题的条目，
// 而非输出 jar——写出失败时调用方拿到的是能定位资源的名字）。
JarError badEntryNameError(const std::string& entryName)
{
    JarError err;
    err.kind = JarError::Kind::BadEntryName;
    err.path = Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(entryName.data()),
                                               entryName.size());
    return err;
}

// 条目路径规范化（写侧唯一入口）：'\' → '/' 统一 POSIX 分隔符（jzip 条目名域
// 以 std::string 为边界类型）；折叠连续 '/'；剔除 "." 段；按段折叠 ".."
// （a/../b → b）。折叠后逃出根（../ 开头、a/../.. 等）或得到空路径（空名、
// "."、"a/../" 等）的条目名非法——jar 条目没有"根外"与"无名"形态，返回
// Err(BadEntryName) 拒写。尾斜杠（目录条目标记）原样保留。字节级处理：
// UTF-8 多字节序列不含 0x2F/0x5C，按字节切段不破坏多字节字符。
Result<std::string, JarError> normalize_path(const std::string& name)
{
    std::string unified = name;
    for (auto& ch : unified) {
        if (ch == '\\') ch = '/';
    }

    const bool trailingSlash = !unified.empty() && unified.back() == '/';
    std::vector<std::string> segments;
    ca::usize                segStart = 0;
    for (ca::usize i = 0; i <= unified.size(); ++i) {
        if (i != unified.size() && unified[i] != '/') continue;
        std::string segment = unified.substr(segStart, i - segStart);
        segStart            = i + 1;
        if (segment.empty() || segment == ".") continue;   // 连续 '/' 与 "." 段
        if (segment == "..") {
            if (segments.empty()) return Err(badEntryNameError(name));   // 逃出根
            segments.pop_back();
            continue;
        }
        segments.push_back(std::move(segment));
    }
    if (segments.empty()) return Err(badEntryNameError(name));   // 空路径形态

    std::string out;
    for (const auto& segment : segments) {
        out += segment;
        out += '/';
    }
    if (!trailingSlash) out.pop_back();
    return Ok(std::move(out));
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

// 将收集的条目写入 jar：排序 → 目录条目政策保持 → 写入文件条目（DEFLATE 压缩）
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
    std::unordered_set<std::string> writtenDirNames;

    for (auto& [name, data] : collector.entries) {
        auto entryNameResult = normalize_path(name.to_std_string());
        if (entryNameResult.is_err()) return Err(std::move(entryNameResult).unwrap_err());
        auto entryName = std::move(entryNameResult).unwrap();   // 非空（normalize 保证）

        // 目录条目政策：只保留调用方显式给出的目录条目（名字以 '/' 结尾，
        // 内容忽略），不为文件条目自动补父目录占位——输入 jar 没有目录条目
        // 时产物一个目录占位都不造（boar-main.jar 实证：旧实现为每个文件
        // 条目凭空补 'META-INF/'、'grunteon/' 等 STORED 占位，纯结构污染；
        // JVM 忽略目录条目，占位无任何语义收益）；输入已有的原样保留。
        if (entryName.back() == '/') {
            if (writtenDirNames.insert(entryName).second) {
                auto dirEntry = std::make_unique<jzip::LocalFileHeader>();
                dirEntry->setFileName(entryName);
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
            continue;
        }

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
