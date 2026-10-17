/// @file        resource_io.cpp
/// @brief       资源导入导出实现（jar 分拣、class 文件 IO、libarchive 解包）。
#include "resource_io.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <archive.h>
#include <archive_entry.h>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::model {

// ── JarFileImporter ──────────────────────────────────────────

// JarError → ModelError 归一（见 resource_io.hpp 声明）。jar 层错误在 mbase
// 以异常穿透到调用方；本模块以码表收口，文件缺失细分为 NotFound。
ModelError toModelError(const mjbase::jar::JarError& error) noexcept
{
    if (error.kind == mjbase::jar::JarError::Kind::FileNotFound) return ModelError::NotFound;
    return ModelError::Io;
}

namespace {

// 按 entry 名/内容分拣为 Manifest / Class / Unknown（import 的两个入口共用）。
// entry 名在 mjbase::jar 边界上是 Utf8String（原始字节直通，零解码口径不变），
// 跨入容器时经借用视图零拷贝转换。
void classifyAndAdd(IResourceContainer& container, const ca::str::Utf8String& entryName,
                    const std::vector<ca::u8>& data)
{
    const auto name = entryName.ref();
    if (name == mjbase::jar::JarUtil::kManifestFileName) {
        container.addResource(
            name, std::make_unique<ManifestResource>(ManifestResource::fromBytes(data)));
        return;
    }
    if (mjbase::jar::JarUtil::isClassFile(name) && mjbase::jar::JarUtil::isClassFile(data)) {
        // 混淆器可能把类 entry 命名为 *.class/（尾斜杠），剥掉后再用作 URI/location，
        // 避免尾斜杠泄漏到类名派生与容器 key。
        if (name.byte_length() >= 7 &&
            name.slice(name.byte_length() - 7, name.byte_length()) == ".class/") {
            const auto normalized = name.slice(0, name.byte_length() - 1);
            container.addResource(normalized, std::make_unique<ClassResource>(normalized, data));
            return;
        }
        container.addResource(name, std::make_unique<ClassResource>(name, data));
        return;
    }
    container.addResource(name, std::make_unique<UnknownResource>(name, data));
}

}   // namespace

// 遍历 jar 条目，根据类型分拣为 ManifestResource / ClassResource / UnknownResource
ModelResult<void> JarFileImporter::importResource(IResourceContainer& container)
{
    // jar 模块错误走 Result<void, JarError>（mbase 为异常穿透），此处归一映射。
    auto r = reader_->read([&](const ca::str::Utf8String& entryName,
                               const std::vector<ca::u8>& data) {
        classifyAndAdd(container, entryName, data);
    });
    if (r.is_err()) return Err(toModelError(std::move(r).unwrap_err()));
    return Ok();
}

// 带进度的导入：每个条目回调 progress，单次解析无重复 IO（见 jar_reader readWithProgress）。
ModelResult<void> JarFileImporter::importResourceWithProgress(IResourceContainer&     container,
                                                              const ProgressCallback& progress)
{
    auto r = reader_->readWithProgress(
        [&](int index, int total, const ca::str::Utf8String& entryName,
            const std::vector<ca::u8>& data) {
            classifyAndAdd(container, entryName, data);
            if (progress) progress(index, total, entryName.ref());
        });
    if (r.is_err()) return Err(toModelError(std::move(r).unwrap_err()));
    return Ok();
}

// ── JarFileExporter ──────────────────────────────────────────

// 遍历容器中所有资源，按类型写入 jar 条目
ModelResult<void> JarFileExporter::exportResource(IResourceContainer& container)
{
    auto r = writer_->write([&](mjbase::jar::IEntryWriter& entryWriter) {
        container.visitResources([&](std::string_view, const IResource& resource) {
            if (auto* cls = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource)) {
                entryWriter.writeEntry(cls->getLocation(), cls->get());
            }
            else if (auto* mf = ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource)) {
                entryWriter.writeEntry(
                    ca::str::Utf8StringRef::from_cstr(mjbase::jar::JarUtil::kManifestFileName),
                    mf->get().toBytes());
            }
            else if (auto* unk = ca::core::tag_cast::dyn_cast<const UnknownResource>(&resource)) {
                entryWriter.writeEntry(unk->getLocation(), unk->get());
            }
        });
    });
    if (r.is_err()) return Err(toModelError(std::move(r).unwrap_err()));
    return Ok();
}

// ── SimpleClassFileExporter ──────────────────────────────────

SimpleClassFileExporter::SimpleClassFileExporter(std::string_view uri, std::string_view path)
    : uri_(ownText(uri))
    , path_(ownText(path))
{}

// 按 URI 查找 ClassResource 并写出到磁盘；空 URI 时使用容器中唯一的资源
ModelResult<void> SimpleClassFileExporter::exportResource(IResourceContainer& container)
{
    const ClassResource* classRes = nullptr;
    if (uri_.ref().is_empty()) {
        // 未指定 URI，取容器中唯一的资源（必须恰好一个且为 ClassResource）
        if (container.size() != 1)
            return Err(ModelError::BadArgument);   // mbase: "uri is empty and container size is not 1"
        container.visitResources([&](std::string_view, const IResource& res) {
            classRes = ca::core::tag_cast::dyn_cast<const ClassResource>(&res);
        });
        if (!classRes)
            return Err(ModelError::TypeMismatch);   // mbase: "the only resource is not ClassResource"
    }
    else {
        auto* res = container.getResource(uri_.ref());
        if (!res) return Err(ModelError::NotFound);   // mbase: "uri not found in container: <uri>"
        classRes = ca::core::tag_cast::dyn_cast<const ClassResource>(res);
        if (!classRes)
            return Err(ModelError::TypeMismatch);   // mbase: "resource is not ClassResource: <uri>"
    }
    const std::string pathText(path_.ref());
    std::ofstream     out(std::filesystem::u8path(pathText), std::ios::binary);
    if (!out) return Err(ModelError::Io);   // mbase: "failed to open: <path>"
    const auto& data = classRes->get();
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) return Err(ModelError::Io);   // mbase: "failed to flush: <path>"
    out.close();
    if (!out) return Err(ModelError::Io);   // mbase: "failed to close: <path>"
    return Ok();
}

// ── SimpleClassFileImporter ──────────────────────────────────

// 从磁盘读取 class 文件字节并作为 ClassResource 加入容器
ModelResult<void> SimpleClassFileImporter::importResource(IResourceContainer& container)
{
    std::ifstream in(std::filesystem::u8path(std::string(path_.ref())), std::ios::binary);
    if (!in) return Err(ModelError::Io);   // mbase: "failed to open: <path>"
    in.seekg(0, std::ios::end);
    auto size = static_cast<ca::usize>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<ca::u8> bytes(size);
    if (size > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    container.addResource(path_.ref(),
                          std::make_unique<ClassResource>(path_.ref(), std::move(bytes)));
    return Ok();
}

// ── ZipUtil ──────────────────────────────────────────────────

// 将归档条目解压到 targetDir。
//
// 使用 libarchive 的"自动嗅探"能力：support_filter_all + support_format_all
// 在 archive_read_open_filename 后会按内容（而非后缀）识别压缩与归档格式，
// 因此天然覆盖 zip / tar / tar.xz / tar.gz 等组合，无需手工 detectFormat。
namespace {

// 判断 child 是否位于 parent 目录之下（含 parent 自身）。两边都先规范化。
// 用 lexically+字符串前缀比较，避免 find("/a/b" in "/a/bc") 这类误命中。
bool isUnderDir(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path        c = fs::weakly_canonical(child, ec);
    fs::path        p = fs::weakly_canonical(parent, ec);
    if (ec) return false;
    // lexically_relative：若 c 不在 p 下，结果以 ".." 开头或为空。
    auto rel = c.lexically_relative(p);
    if (rel.empty()) return false;
    for (const auto& seg : rel) {
        if (seg == "..") return false;
    }
    return true;
}

// 把单个条目写到磁盘（等价于 archive_read_extract，但显式落到 targetDir，
// 避免依赖进程 CWD）。返回 Ok 表示继续下一条目。mbase throw 文本 → 错误码：
// "entry escapes target dir: <name>" → PathEscape；"cannot open output: <p>" → Io；
// read_data_block 失败（archive_error_string 文本）→ BadEntry。
ModelResult<void> writeEntryToDisk(struct archive* a, struct archive_entry* entry,
                                   const std::filesystem::path& targetDir)
{
    namespace fs      = std::filesystem;
    const char* pname = archive_entry_pathname(entry);
    if (!pname) return Ok();   // 跳过无名条目
    fs::path out = targetDir / pname;

    // 防目录穿越：条目名可能含 ".." 或绝对路径，解析后必须仍在 targetDir 之下。
    if (!isUnderDir(out, targetDir)) return Err(ModelError::PathEscape);

    if (archive_entry_filetype(entry) == AE_IFDIR) {
        std::error_code ec;
        fs::create_directories(out, ec);
    }
    else {
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        const void* buff   = nullptr;
        size_t      size   = 0;
        la_int64_t  offset = 0;
        // header 已由外层 read_next_header 读出，这里用 read_data_block 流式写文件内容。
        std::ofstream ofs(out, std::ios::binary);
        if (!ofs) return Err(ModelError::Io);
        while (true) {
            int r = archive_read_data_block(a, &buff, &size, &offset);
            if (r == ARCHIVE_EOF) break;
            if (r != ARCHIVE_OK) return Err(ModelError::BadEntry);
            if (size > 0)
                ofs.write(static_cast<const char*>(buff), static_cast<std::streamsize>(size));
        }
    }
    // 保持文件权限位（best-effort，失败忽略——CI/解压场景不强求权限还原）。
    std::error_code ign;
    fs::permissions(out, static_cast<fs::perms>(archive_entry_perm(entry)), ign);
    return Ok();
}

// 打开归档并逐条目解压到 target（绝对路径，已 canonical）。onlyEntry 非空时仅解压
// pathname 与之完全匹配的那一个条目，其余跳过；找不到匹配条目报 NotFound。
// mbase throw 文本 → 错误码映射：
// - "archive_read_new failed"                         → Io
// - "open archive failed: <p> — <msg>"（文件不存在）   → NotFound
// - "open archive failed: <p> — <msg>"（格式识别失败） → UnsupportedFormat
// - "read header failed: <msg>"                       → BadEntry
// - "extract failed at '<entry>': <err>"              → 随内层（PathEscape/Io/BadEntry）
// - "entry not found in archive '<p>': <entry>"       → NotFound
ModelResult<void> extractArchiveTo(const std::string&           archivePath,
                                   const std::filesystem::path& target,
                                   const std::string*           onlyEntry)
{
    struct archive* a = archive_read_new();
    if (!a) return Err(ModelError::Io);
    // RAII：无论成败都释放 archive 句柄。
    std::unique_ptr<struct archive, decltype(&archive_read_free)> guard(a, archive_read_free);

    archive_read_support_filter_all(a);   // xz / gzip / bzip2 / zstd ...
    archive_read_support_format_all(a);   // zip / tar / ...

    const int r = archive_read_open_filename(a, archivePath.c_str(), 10240);
    if (r != ARCHIVE_OK) {
        // mbase 两类失败同报 "open archive failed"；此处按文件存在性细分码，
        // 原消息文本（含 archive_error_string）不再进错误载荷。存在性检查按
        // 模块口径走 u8path（UTF-8 解释）。
        if (!std::filesystem::exists(std::filesystem::u8path(archivePath)))
            return Err(ModelError::NotFound);
        return Err(ModelError::UnsupportedFormat);
    }

    // 逐条目读取。read_next_header 正常读完返回 ARCHIVE_EOF（循环自然退出），
    // 只有返回 ARCHIVE_FATAL 等才视为失败——不要用 archive_errno 事后判断，
    // 它是"最近一次错误"，正常读完未必清零，会误报。
    struct archive_entry* entry = nullptr;
    bool                  found = false;
    for (;;) {
        const int hr = archive_read_next_header(a, &entry);
        if (hr == ARCHIVE_EOF) break;
        if (hr != ARCHIVE_OK) return Err(ModelError::BadEntry);
        if (onlyEntry) {
            const char* pname = archive_entry_pathname(entry);
            // 未读的条目数据会在下次 read_next_header 时被 libarchive 自动跳过。
            if (!pname || *onlyEntry != pname) continue;
            found = true;
        }
        auto wr = writeEntryToDisk(a, entry, target);
        if (wr.is_err()) {
            // mbase 在此包装 "extract failed at '<entry>': <err>"；条目名不进
            // 错误载荷（enum 码），错误类别由内层给出。
            return wr;
        }
        if (onlyEntry) break;   // 目标条目已提取，无需继续扫描
    }
    if (onlyEntry && !found) return Err(ModelError::NotFound);
    return Ok();
}

}   // namespace

// 使用 libarchive 解压到目标目录（支持 zip / tar / tar.xz / tar.gz 等）
ModelResult<void> ZipUtil::extract(std::string_view archivePath, std::string_view targetDir)
{
    namespace fs = std::filesystem;
    // mbase 直接 create_directories/weakly_canonical（filesystem_error 抛出）；
    // 此处改 error_code 并归一 Err(Io)。路径按 UTF-8 解释（u8path，与
    // SimpleClassFile* 的 mbase 口径一致）。
    std::error_code ec;
    const fs::path  target = fs::u8path(std::string(targetDir));
    fs::create_directories(target, ec);
    if (ec) return Err(ModelError::Io);
    const fs::path canonical = fs::weakly_canonical(target, ec);
    if (ec) return Err(ModelError::Io);
    return extractArchiveTo(std::string(archivePath), canonical, nullptr);
}

ModelResult<void> ZipUtil::unzip(std::string_view zipPath, std::string_view targetDir)
{
    return extract(zipPath, targetDir);
}

// 从压缩包 path 中提取名为 file 的单个条目到 destination 目录（保留条目内相对路径结构）。
// file 需与归档内条目 pathname 完全匹配（如 "com/foo/Bar.class"）。
ModelResult<void> ZipUtil::extractFile(std::string_view path, std::string_view file,
                                       std::string_view destination)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path  dest = fs::u8path(std::string(destination));
    fs::create_directories(dest, ec);
    if (ec) return Err(ModelError::Io);
    const fs::path canonical = fs::weakly_canonical(dest, ec);
    if (ec) return Err(ModelError::Io);
    const std::string entryName(file);
    return extractArchiveTo(std::string(path), canonical, &entryName);
}

}   // namespace mjbase::model
