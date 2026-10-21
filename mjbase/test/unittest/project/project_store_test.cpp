// 分析工程存取层：bundle 常量/目录
// 骨架 + create 的目录判别 + store.json 严格解析逐类拒载 + 确定性逐字节写出
// + section 命名规则 + 未知 format 收容（P1：清单拒载≠删数据）（C1）；
// section 内容原子读写原语：staged tmp + 换名单 section 原子写、
// content_hash/byte_size 双登记、先 size 后 hash 的 Damaged 判定材料（C2）；
// store.json manifest 读-改-写事务：staged tmp + 单次换名 + 重试、CP-1 首个
// 消费方（拒载即拒绝事务，P1 红线不绕行）、墓碑删除、upsert 与发布字节
// 复核（C3，成功/中断/损坏/重试耗尽各态）；§8.2 恢复扫描：sections/+
// store.json+tmp/ 全量盘点分类账（Ok/Damaged/孤儿两档）、扫描只读性、
// unloadable-manifest 孤儿零 prune 路径专测、CP-1 终裁（Io 入降级触发面）
// 的 Io 保守两断言（清单侧 + Damaged 侧）（C4）；§7.1 单写者锁 store.lock
// 全路径：独占创建获取/二次获取 Lock 报 owner 三元组/释放清场、死 pid 手写
// 锁的自动接管（§11-3 spawn→取 pid→kill 构造法）、不可解析 fail-closed
// （禁止自动接管）、stealLock 抢占 + 诊断记录、删除受阻 fail-closed、
// PID 探测纪律（daemon_state.cpp:348-368 平移）、恢复扫描与锁互不越界
// （C5）；R64 N1/N2 落刀：POSIX 释放属主核验（fstat/stat inode 比对后再删，
// 手造 inode 错位断言不误删抢占者新锁）、unreadable 分支放行 stealLock
// （unreadable fixture：无 stealLock 仍 fail-closed，有 stealLock 出口可达）
// （R64 收口）；R62 N1/N2 加固：flush 后孤儿 prune 动作面（工作清单现扫现验——
// 清单现读 fail-closed / 认领现验 / exists·可读 ec 全尊重 / 删除 ec 上浮，
// Never 档零触碰）+ 收容词根匹配平台分叉（Windows 大小写不敏感对齐 Win32
// 路径语义 / POSIX 敏感）双平台测试钉。
// 验收口径：doc_ai/analysis-project-store-design.md §11-1（骨架）、§11-7（C1）；
// §11 清单中归属 section 读写的写读往返/hash 损坏检测/tmp 半提交隔离/字节级
// 确定性（C2）；§6.3/§8.1 事务语义与 §11-1/11 的 flush 面（C3）；§8.2/§8.3
// 恢复扫描与 §11-4/5/6 的分类账面（C4，扫描只报告不改动）；§7.1/§7.2/§11-3
// 的锁面（C5）。
//
// 对应、语义与时序假设不变；仅命名空间（mjbase::project）与 gtest 套件名
// 随 mjbase 约定调整。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "mjbase/project/analysis_project.hpp"
#include "mjbase/project/project_store.hpp"
#include "libca/crypto/sha256.hpp"
#include "libca/fs/file_util.hpp"
#include "test_helper.hpp"

// 平台头最后：Windows SDK 的 WinBase.h 定义 CREATE_NEW 等宏，会撞 libca
// file_util.hpp 的 FileMode 常量名（product 侧同款纪律，见 project_store.cpp）。
#ifdef _WIN32
#include <share.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace mjbase::project;
using ca::str::Utf8String;
using ca::str::Utf8StringRef;

namespace {

Utf8StringRef S(const char* value)
{
    return Utf8StringRef::from_cstr(value);
}

Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

Utf8String makeStr(const std::string& value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

/// "sha256:" + 64 位 hex（用重复字符区分不同条目）。
std::string hashOf(char fill)
{
    return "sha256:" + std::string(64, fill);
}

/// 合法 jsonl 条目（字段取 §4.2 示例口径）。
StoreSectionEntry makeJsonlEntry(const char* name, char hashFill)
{
    StoreSectionEntry entry;
    entry.name          = Utf8String::from_cstr(name);
    entry.format        = SectionFormat::Jsonl;
    entry.formatTag     = Utf8String::from_cstr("jsonl");
    entry.sectionVersion = 1;
    entry.contentHash   = makeStr(hashOf(hashFill));
    entry.byteSize      = 12345;
    entry.hasRecordCount = true;
    entry.recordCount   = 100;
    entry.producedBy    = Utf8String::from_cstr("mjt alias-pass/2.3");
    entry.createdAt     = Utf8String::from_cstr("2026-10-03T08:00:00Z");
    entry.updatedAt     = Utf8String::from_cstr("2026-10-03T08:00:00Z");
    return entry;
}

/// 合法 json 条目。
StoreSectionEntry makeJsonEntry(const char* name, char hashFill)
{
    StoreSectionEntry entry = makeJsonlEntry(name, hashFill);
    entry.format            = SectionFormat::Json;
    entry.formatTag         = Utf8String::from_cstr("json");
    entry.hasRecordCount    = false;
    entry.recordCount       = 0;
    return entry;
}

std::string writeOrDie(const StoreManifest& manifest)
{
    auto written = writeStoreManifestText(manifest);
    EXPECT_FALSE(written.is_err());
    return written.is_err() ? std::string() : std::move(written).unwrap().to_std_string();
}

std::string readFileOrDie(const std::filesystem::path& path)
{
#ifdef _WIN32
    // store.lock 在持有期由写句柄打开；FileUtil 的默认共享模式读它会撞
    // sharing violation（libca 0.0.16 stdio 读路径）。测试读取显式
    // _SH_DENYNO，与锁持有者的共享模式对称。
    std::FILE* handle = ::_wfsopen(path.native().c_str(), L"rb", _SH_DENYNO);
    EXPECT_NE(nullptr, handle) << "path=" << path.string();
    if (handle == nullptr) return {};
    std::string bytes;
    char        chunk[4096];
    for (std::size_t n = 0; (n = std::fread(chunk, 1, sizeof(chunk), handle)) > 0;) {
        bytes.append(chunk, n);
    }
    const bool failed = std::ferror(handle) != 0;
    std::fclose(handle);
    EXPECT_FALSE(failed) << "path=" << path.string();
    return bytes;
#else
    auto content = ca::fs::FileUtil::read_all_text(path.string());
    EXPECT_FALSE(content.is_err());
    return content.is_err() ? std::string() : std::move(content).unwrap();
#endif
}

/// 清掉上一次 run 的同名临时 bundle，返回干净路径。
std::filesystem::path freshTempDir(const std::string& name)
{
    const auto  path = test::getTempOutPath(name);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path;
}

/// 解析错误断言：期望 kind + 返回错误消息文本。
std::string expectParseError(const std::string& text, ProjectStoreErrorKind kind)
{
    auto parsed = parseStoreManifestText(refOf(text));
    EXPECT_TRUE(parsed.is_err()) << "accepted: " << text;
    if (parsed.is_err()) {
        const auto  err = std::move(parsed).unwrap_err();
        EXPECT_EQ(kind, err.kind) << err.message.to_std_string();
        return err.message.to_std_string();
    }
    return {};
}

//-- C2 辅助 -------------------------------------------------------------------

/// 独立计算 "sha256:" + 64 hex（不经被测代码的参照实现，双登记核对基准；
/// 小写 canonical）。
std::string independentHash(const char* data, std::size_t size)
{
    ca::crypto::SHA256 digest;
    if (size > 0) digest.add(data, size);
    std::string hex = digest.get_hash();
    for (char& c : hex) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    return "sha256:" + hex;
}

std::string independentHash(const std::string& bytes)
{
    return independentHash(bytes.data(), bytes.size());
}

/// 从写结果构造真实清单条目（C3 flush 登记路径的最小手工模拟）。
StoreSectionEntry entryFrom(const char* name, SectionFormat format,
                            const SectionWriteResult& written, ca::u64 recordCount = 0)
{
    StoreSectionEntry entry;
    entry.name           = Utf8String::from_cstr(name);
    entry.format         = format;
    entry.formatTag      = Utf8String::from_cstr(format == SectionFormat::Json     ? "json"
                                                  : format == SectionFormat::Jsonl ? "jsonl"
                                                                                   : "opaque");
    entry.sectionVersion = 1;
    entry.contentHash    = written.contentHash.clone();
    entry.byteSize       = written.byteSize;
    if (format == SectionFormat::Jsonl) {
        entry.hasRecordCount = true;
        entry.recordCount    = recordCount;
    }
    entry.producedBy = Utf8String::from_cstr("mjt alias-pass/2.3");
    entry.createdAt  = Utf8String::from_cstr("2026-10-03T08:00:00Z");
    entry.updatedAt  = Utf8String::from_cstr("2026-10-03T08:00:00Z");
    return entry;
}

/// 写 section 或直接 FAIL（返回值语义：失败时返回空登记，调用方已 FAIL）。
SectionWriteResult writeSectionOrDie(const std::string& dir, const char* name,
                                     SectionFormat format, const std::string& bytes)
{
    auto written = writeSectionBytes(refOf(dir), S(name), format,
                                     reinterpret_cast<const ca::u8*>(bytes.data()),
                                     bytes.size());
    if (written.is_err()) {
        ADD_FAILURE() << std::move(written).unwrap_err().message.to_std_string();
        return SectionWriteResult{};
    }
    return std::move(written).unwrap();
}

/// tmp/ 目录下的现存条目数（txn 自清扫断言用）。
std::size_t tmpEntryCount(const std::filesystem::path& dir)
{
    std::size_t      count = 0;
    std::error_code  ec;
    for (auto it = std::filesystem::directory_iterator(dir / "tmp", ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        ++count;
    }
    return count;
}

//-- C5 辅助 -------------------------------------------------------------------

/// 当前进程 pid（锁内容与"锁被自己持有"场景构造用）。
long long currentProcessPid()
{
#ifdef _WIN32
    return static_cast<long long>(::_getpid());
#else
    return static_cast<long long>(::getpid());
#endif
}

/// §7.1 锁内容形状（手写 lock 文件用；三字段 + 尾随 LF，与引擎写出同形）。
std::string lockJson(long long pid, const char* createdAt, const char* owner)
{
    return std::string("{\"pid\":") + std::to_string(pid) + ",\"created_at\":\"" + createdAt +
           "\",\"owner\":\"" + owner + "\"}\n";
}

/// 手写 store.lock（走正常文件 API，§11 平台注意）。
std::string writeLockOrDie(const std::filesystem::path& dir, const std::string& content)
{
    const auto path = dir / kProjectStoreLockName;
    auto written = ca::fs::FileUtil::write_bytes(
        path.string(),
        ca::core::ByteSlice(reinterpret_cast<const ca::u8*>(content.data()), content.size()));
    EXPECT_FALSE(written.is_err()) << path.string();
    return content;
}

/// 诊断行汇成一段文本（断言消息面用）。
std::string joinDiagnostics(const std::vector<ca::str::Utf8String>& diagnostics)
{
    std::string joined;
    for (const auto& line : diagnostics) joined += line.to_std_string() + "\n";
    return joined;
}

/// §11-3 死 pid 构造法（主法）：spawn 子进程 → 取 pid → kill。返回 ≤ 0 表示
/// 构造失败（调用方 GTEST_SKIP——§11-3 允许的备选法是极不可能 pid + skip）。
/// 杀完后进程对象已完全回收（Windows 等待句柄、POSIX waitpid 收尸），探测面
/// 上不再有该 pid 的活进程。
long long spawnAndKillChildProcess()
{
#ifdef _WIN32
    STARTUPINFOW         si{};
    PROCESS_INFORMATION  pi{};
    si.cb = sizeof(si);
    // cmd /c ping：可控时长的可杀子进程（CREATE_NO_WINDOW 不闪控制台）。
    std::wstring cmdline = L"cmd.exe /c ping -n 30 127.0.0.1 > nul";
    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        return -1;
    }
    const long long pid    = static_cast<long long>(pi.dwProcessId);
    const HANDLE    handle = pi.hProcess;
    ::TerminateProcess(handle, 1);
    ::WaitForSingleObject(handle, 10000);
    DWORD exitCode = 0;
    ::GetExitCodeProcess(handle, &exitCode);
    ::CloseHandle(handle);
    ::CloseHandle(pi.hThread);
    return exitCode == STILL_ACTIVE ? -1 : pid;
#else
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // 子进程：睡到被杀（绝不返回 gtest 栈）。
        ::sleep(30);
        ::_exit(0);
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return static_cast<long long>(pid);
#endif
}

}   // namespace

//-- §11-1 骨架：create 布局 + #1096 零破坏 + 重开逐字节 -----------------------

TEST(MjProjectStoreTest, createBundleSkeletonLaysOutBundleAndStaysAnalysisProjectReadable)
{
    const auto  dir = freshTempDir("project_store_c1_skeleton.mjt-project");

    ASSERT_FALSE(createBundleSkeleton(refOf(dir.string())).is_err());

    // 目录骨架：sections/ + tmp/ + project.json + store.json（§3.2）。
    EXPECT_TRUE(std::filesystem::is_directory(dir / "sections"));
    EXPECT_TRUE(std::filesystem::is_directory(dir / "tmp"));
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "store.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "project.json"));

    // store.json：v1 空白清单；重开重写逐字节一致（§11-1 骨架口径）。
    const auto  fileBytes = readFileOrDie(dir / "store.json");
    auto reloaded = readStoreManifestFile(refOf(dir.string()));
    ASSERT_FALSE(reloaded.is_err());
    auto manifest = std::move(reloaded).unwrap();
    EXPECT_EQ(1U, manifest.version);
    EXPECT_EQ(0U, manifest.sections.size());
    EXPECT_TRUE(manifest.diagnostics.empty());
    EXPECT_EQ(kProjectStoreEngine, manifest.engine.to_std_string());
    auto rewritten = writeStoreManifestText(manifest);
    ASSERT_FALSE(rewritten.is_err());
    EXPECT_EQ(fileBytes, std::move(rewritten).unwrap().to_std_string());

    // project.json：字节 = writeAnalysisProjectText 产出；#1096 读者照常工作
    // （§4.3 零破坏线：bundle 内实体对旧库完全可见）。
    auto entity = readAnalysisProjectFile(refOf((dir / "project.json").string()));
    ASSERT_FALSE(entity.is_err());
    EXPECT_TRUE(std::move(entity).unwrap().empty());
    auto expectedEntity = writeAnalysisProjectText(AnalysisProject::createEmpty());
    ASSERT_FALSE(expectedEntity.is_err());
    EXPECT_EQ(readFileOrDie(dir / "project.json"),
              std::move(expectedEntity).unwrap().to_std_string());

    // 已存在的非空目录 → Open 错（消息含首个现存条目）。
    auto again = createBundleSkeleton(refOf(dir.string()));
    ASSERT_TRUE(again.is_err());
    const auto  againError = std::move(again).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Open, againError.kind);
    EXPECT_NE(std::string::npos, againError.message.to_std_string().find("not empty"));
}

TEST(MjProjectStoreTest, classifyStorePathDiscriminatesDirectoryBareAndMissing)
{
    const auto  dir   = freshTempDir("project_store_c1_classify.mjt-project");
    const auto  bare  = test::getTempOutPath("project_store_c1_bare.mjt-project");

    EXPECT_EQ(StorePathKind::Missing, classifyStorePath(refOf(dir.string())));

    ASSERT_FALSE(
        ca::fs::FileUtil::write_text(bare.string(), "{}\n").is_err());
    EXPECT_EQ(StorePathKind::BareFile, classifyStorePath(refOf(bare.string())));

    ASSERT_FALSE(createBundleSkeleton(refOf(dir.string())).is_err());
    EXPECT_EQ(StorePathKind::Directory, classifyStorePath(refOf(dir.string())));

    // 对裸文件路径 create → Open 错（bundle 是目录；裸文件模式归 C2）。
    auto onBare = createBundleSkeleton(refOf(bare.string()));
    ASSERT_TRUE(onBare.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Open, std::move(onBare).unwrap_err().kind);
}

//-- §4.2 确定性：任意构建序 → 逐字节一致；重开重写 → 逐字节一致 --------------

TEST(MjProjectStoreTest, manifestWriteIsDeterministicAndRoundtripsFieldExact)
{
    // 条目乱序插入（decode 先于 facts），depends 乱序插入（z 后于 a）。
    // （实体串 move-only：vector 装配一律 push_back + move。）
    StoreManifest first;
    first.engine = Utf8String::from_cstr("mbase-project-store/1");
    StoreSectionEntry decode = makeJsonEntry("decode.string", '1');
    StoreSectionEntry facts  = makeJsonlEntry("facts.alias", '2');
    facts.profile            = Utf8String::from_cstr("cha");
    SectionDep zDep;
    zDep.path        = Utf8String::from_cstr("samples/z-dep.jar");
    zDep.fingerprint = Utf8String::from_cstr("sha256:zz");
    SectionDep aDep;
    aDep.path        = Utf8String::from_cstr("samples/a-app.jar");
    aDep.fingerprint = Utf8String::from_cstr("sha256:aa");
    facts.depends.push_back(std::move(zDep));
    facts.depends.push_back(std::move(aDep));
    first.sections.push_back(std::move(decode));
    first.sections.push_back(std::move(facts));

    // 独立构建同一状态（不同插入序）→ 逐字节一致。
    StoreManifest second;
    second.engine  = Utf8String::from_cstr("mbase-project-store/1");
    StoreSectionEntry facts2 = makeJsonlEntry("facts.alias", '2');
    facts2.profile           = Utf8String::from_cstr("cha");
    SectionDep aDep2;
    aDep2.path        = Utf8String::from_cstr("samples/a-app.jar");
    aDep2.fingerprint = Utf8String::from_cstr("sha256:aa");
    SectionDep zDep2;
    zDep2.path        = Utf8String::from_cstr("samples/z-dep.jar");
    zDep2.fingerprint = Utf8String::from_cstr("sha256:zz");
    facts2.depends.push_back(std::move(aDep2));
    facts2.depends.push_back(std::move(zDep2));
    second.sections.push_back(std::move(facts2));
    second.sections.push_back(makeJsonEntry("decode.string", '1'));

    const auto  firstText  = writeOrDie(first);
    const auto  secondText = writeOrDie(second);
    EXPECT_EQ(firstText, secondText);

    // 写出键序固定（§4.2 表序），sections 按 name 字典序。
    const auto  factsAt = firstText.find("\"facts.alias\"");
    const auto  decodeAt = firstText.find("\"decode.string\"");
    ASSERT_NE(std::string::npos, factsAt);
    ASSERT_NE(std::string::npos, decodeAt);
    EXPECT_LT(decodeAt, factsAt);
    EXPECT_NE(std::string::npos, firstText.find("\"version\": 1"));
    EXPECT_NE(std::string::npos, firstText.find("\"kind\": \"mjt-project-store\""));

    // 解析回 → 字段逐字段相等 → 重写逐字节一致（同状态重开重写口径）。
    auto parsed = parseStoreManifestText(refOf(firstText));
    ASSERT_FALSE(parsed.is_err());
    auto back = std::move(parsed).unwrap();
    ASSERT_EQ(2U, back.sections.size());
    ASSERT_TRUE(back.diagnostics.empty());
    // 写出按字典序：decode.string 在前。
    EXPECT_EQ("decode.string", back.sections[0].name.to_std_string());
    EXPECT_EQ(SectionFormat::Json, back.sections[0].format);
    EXPECT_FALSE(back.sections[0].formatRetained);
    EXPECT_EQ(1U, back.sections[0].sectionVersion);
    EXPECT_EQ(hashOf('1'), back.sections[0].contentHash.to_std_string());
    EXPECT_EQ(12345U, back.sections[0].byteSize);
    EXPECT_FALSE(back.sections[0].hasRecordCount);
    EXPECT_EQ("mjt alias-pass/2.3", back.sections[0].producedBy.to_std_string());

    const auto& alias = back.sections[1];
    EXPECT_EQ("facts.alias", alias.name.to_std_string());
    EXPECT_EQ(SectionFormat::Jsonl, alias.format);
    EXPECT_TRUE(alias.hasRecordCount);
    EXPECT_EQ(100U, alias.recordCount);
    ASSERT_TRUE(alias.profile.has_value());
    EXPECT_EQ("cha", alias.profile->to_std_string());
    ASSERT_EQ(2U, alias.depends.size());
    // depends 按 path 字典序回写。
    EXPECT_EQ("samples/a-app.jar", alias.depends[0].path.to_std_string());
    EXPECT_EQ("sha256:aa", alias.depends[0].fingerprint.to_std_string());
    EXPECT_EQ("samples/z-dep.jar", alias.depends[1].path.to_std_string());
    EXPECT_EQ("sha256:zz", alias.depends[1].fingerprint.to_std_string());
    EXPECT_FALSE(alias.formatRetained);

    EXPECT_EQ(firstText, writeOrDie(back));

    // 文件级闭环：落盘 → readStoreManifestFile → 重写一致（§11-1 骨架）。
    const auto  dir = freshTempDir("project_store_c1_manifest_roundtrip.mjt-project");
    ASSERT_FALSE(createBundleSkeleton(refOf(dir.string())).is_err());
    // 骨架为空清单；把带条目的清单写进 store.json（模拟 flush 后状态）。
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(), firstText).is_err());
    auto fromDisk = readStoreManifestFile(refOf(dir.string()));
    ASSERT_FALSE(fromDisk.is_err());
    EXPECT_EQ(firstText, writeOrDie(std::move(fromDisk).unwrap()));
}

//-- §11-7 严格性：逐类违约拒载 -------------------------------------------------

TEST(MjProjectStoreTest, strictParseEnforcesManifestDiscipline)
{
    StoreManifest manifest;
    manifest.engine = Utf8String::from_cstr("mbase-project-store/1");
    manifest.sections.push_back(makeJsonlEntry("facts.alias", '7'));
    const auto  base = writeOrDie(manifest);
    ASSERT_NE(std::string::npos, base.find("\"record_count\": 100"));

    // 顶层未知字段**忽略**（加法演进，§4.2 演进规则）：解析成功、无诊断。
    std::string tolerant = base;
    tolerant.replace(tolerant.find("\"kind\""), 0, "\"future_knob\": {\"deep\": [1, 2]}, ");
    auto okParsed = parseStoreManifestText(refOf(tolerant));
    ASSERT_FALSE(okParsed.is_err());
    auto okManifest = std::move(okParsed).unwrap();
    ASSERT_EQ(1U, okManifest.sections.size());
    EXPECT_TRUE(okManifest.diagnostics.empty());

    // 条目未知字段拒载（Schema，条目字段集严格）。
    std::string entryUnknown = base;
    entryUnknown.replace(entryUnknown.find("\"name\""), 0, "\"nope\": 1, ");
    expectParseError(entryUnknown, ProjectStoreErrorKind::Schema);

    // duplicate-key 一律拒载：顶层 / 条目 / 未知键重复。
    std::string topDup = base;
    topDup.replace(topDup.find("\"kind\""), 0, "\"version\": 1, ");
    expectParseError(topDup, ProjectStoreErrorKind::Schema);
    std::string entryDup = base;
    entryDup.replace(entryDup.find("\"record_count\""), 0, "\"record_count\": 100, ");
    expectParseError(entryDup, ProjectStoreErrorKind::Schema);
    std::string unknownDup = base;
    unknownDup.replace(unknownDup.find("\"kind\""), 0, "\"future_knob\": 1, \"future_knob\": 2, ");
    expectParseError(unknownDup, ProjectStoreErrorKind::Schema);

    // kind 喂错拒载（Open；与 project.json 双向互斥，§4.2）。
    std::string wrongKind = base;
    const std::string kindAnchor = "\"mjt-project-store\"";
    wrongKind.replace(wrongKind.find(kindAnchor), kindAnchor.size(), "\"mjt-analysis-project\"");
    expectParseError(wrongKind, ProjectStoreErrorKind::Open);

    // 版本不识别拒载（Open；C4 打开期降级为全孤儿，不自动 prune）。
    std::string version2 = base;
    const std::string versionAnchor = "\"version\": 1";
    version2.replace(version2.find(versionAnchor), versionAnchor.size(), "\"version\": 2");
    const auto  versionMessage = expectParseError(version2, ProjectStoreErrorKind::Open);
    EXPECT_NE(std::string::npos, versionMessage.find("never auto-pruned"));

    // 缺 content_hash（Schema）。
    std::string noHash = base;
    const auto  hashAt = noHash.find("\"content_hash\"");
    ASSERT_NE(std::string::npos, hashAt);
    const auto  hashEnd = noHash.find(',', hashAt);
    noHash.erase(hashAt, hashEnd - hashAt + 1);
    expectParseError(noHash, ProjectStoreErrorKind::Schema);

    // 缺 sections（Open，结构缺件）。
    expectParseError("{\"version\": 1, \"kind\": \"mjt-project-store\"}",
                     ProjectStoreErrorKind::Open);
    // 缺 version / kind。
    expectParseError("{\"kind\": \"mjt-project-store\", \"sections\": []}",
                     ProjectStoreErrorKind::Open);
    expectParseError("{\"version\": 1, \"sections\": []}", ProjectStoreErrorKind::Open);
    // JSON 语法坏（Open）。
    expectParseError("{ not json ", ProjectStoreErrorKind::Open);

    // record_count 相容（§4.2）：jsonl 必有；json/opaque 禁止。
    std::string jsonlNoCount = base;
    const auto  countAt      = jsonlNoCount.find("\"record_count\"");
    ASSERT_NE(std::string::npos, countAt);
    const auto  countEnd = jsonlNoCount.find(',', countAt);
    jsonlNoCount.erase(countAt, countEnd - countAt + 1);
    expectParseError(jsonlNoCount, ProjectStoreErrorKind::Schema);

    StoreManifest jsonWithCount;
    jsonWithCount.sections.push_back(makeJsonEntry("decode.string", '3'));
    auto        jsonText = writeOrDie(jsonWithCount);
    std::string jsonPlusCount = jsonText;
    jsonPlusCount.replace(jsonPlusCount.find("\"byte_size\""), 0, "\"record_count\": 4, ");
    expectParseError(jsonPlusCount, ProjectStoreErrorKind::Schema);

    // content_hash 形状（"sha256:" + 64 hex）：解析与写出两侧同拒（Schema）。
    StoreManifest shortHash;
    auto         badEntry = makeJsonlEntry("facts.alias", '5');
    badEntry.contentHash  = Utf8String::from_cstr("sha256:abcd");
    shortHash.sections.push_back(std::move(badEntry));
    auto shortHashWritten = writeStoreManifestText(shortHash);
    ASSERT_TRUE(shortHashWritten.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Schema, std::move(shortHashWritten).unwrap_err().kind);
    const char* shortHashJson =
        "{\"version\":1,\"kind\":\"mjt-project-store\",\"sections\":[{\"name\":\"facts.alias\","
        "\"format\":\"jsonl\",\"section_version\":1,\"content_hash\":\"sha256:abcd\","
        "\"byte_size\":1,\"record_count\":1,\"depends\":[],\"produced_by\":\"p\","
        "\"created_at\":\"t\",\"updated_at\":\"t\"}]}";
    expectParseError(shortHashJson, ProjectStoreErrorKind::Schema);

    // 重复 section 名 / depends 重复 path / depends 条目未知字段。
    const std::string dupNameJson =
        "{\"version\":1,\"kind\":\"mjt-project-store\",\"sections\":["
        "{\"name\":\"facts.alias\",\"format\":\"jsonl\",\"section_version\":1,"
        "\"content_hash\":\"sha256:"
        + std::string(64, 'a') +
        "\",\"byte_size\":1,\"record_count\":1,\"depends\":[],\"produced_by\":\"p\","
        "\"created_at\":\"t\",\"updated_at\":\"t\"},"
        "{\"name\":\"facts.alias\",\"format\":\"jsonl\",\"section_version\":1,"
        "\"content_hash\":\"sha256:"
        + std::string(64, 'b') +
        "\",\"byte_size\":1,\"record_count\":1,\"depends\":[],\"produced_by\":\"p\","
        "\"created_at\":\"t\",\"updated_at\":\"t\"}]}";
    auto dupParsed = parseStoreManifestText(refOf(dupNameJson));
    ASSERT_TRUE(dupParsed.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Schema, std::move(dupParsed).unwrap_err().kind);

    const std::string dupDepJson =
        "{\"version\":1,\"kind\":\"mjt-project-store\",\"sections\":[{\"name\":\"facts.alias\","
        "\"format\":\"jsonl\",\"section_version\":1,\"content_hash\":\"sha256:"
        + std::string(64, 'c') +
        "\",\"byte_size\":1,\"record_count\":1,\"depends\":["
        "{\"path\":\"a.jar\",\"fingerprint\":\"sha256:x\"},{\"path\":\"a.jar\",\"fingerprint\":"
        "\"sha256:y\"}],\"produced_by\":\"p\",\"created_at\":\"t\",\"updated_at\":\"t\"}]}";
    expectParseError(dupDepJson, ProjectStoreErrorKind::Schema);

    const std::string unknownDepJson =
        "{\"version\":1,\"kind\":\"mjt-project-store\",\"sections\":[{\"name\":\"facts.alias\","
        "\"format\":\"jsonl\",\"section_version\":1,\"content_hash\":\"sha256:"
        + std::string(64, 'd') +
        "\",\"byte_size\":1,\"record_count\":1,\"depends\":[{\"path\":\"a.jar\","
        "\"fingerprint\":\"sha256:x\",\"extra\":1}],\"produced_by\":\"p\",\"created_at\":\"t\","
        "\"updated_at\":\"t\"}]}";
    expectParseError(unknownDepJson, ProjectStoreErrorKind::Schema);

    // 条目内 section 名非法 → Schema 且 error.section 指名（§6.1）。
    const std::string badNameJson =
        "{\"version\":1,\"kind\":\"mjt-project-store\",\"sections\":[{\"name\":\"Bad.Name\","
        "\"format\":\"json\",\"section_version\":1,\"content_hash\":\"sha256:"
        + std::string(64, 'e') +
        "\",\"byte_size\":1,\"depends\":[],\"produced_by\":\"p\",\"created_at\":\"t\","
        "\"updated_at\":\"t\"}]}";
    auto badNameParsed = parseStoreManifestText(refOf(badNameJson));
    ASSERT_TRUE(badNameParsed.is_err());
    const auto  badNameError = std::move(badNameParsed).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Schema, badNameError.kind);
    EXPECT_EQ("Bad.Name", badNameError.section.to_std_string());

    // 双向互斥的另一向：store.json 喂给 #1096 实体解析器 → 拒（零破坏线）。
    auto asEntity = parseAnalysisProjectText(refOf(base));
    ASSERT_TRUE(asEntity.is_err());
    EXPECT_NE(std::string::npos,
              std::move(asEntity).unwrap_err().message.to_std_string()
                  .find("unknown analysis project field"));
    // 实体 JSON 喂给 store 解析器 → 拒（Open，kind 不识别）。
    auto entityText = writeAnalysisProjectText(AnalysisProject::createEmpty());
    ASSERT_FALSE(entityText.is_err());
    const auto  entityBody = std::move(entityText).unwrap().to_std_string();
    expectParseError(entityBody, ProjectStoreErrorKind::Open);
}

//-- §11-7 收容：format 值不识别 → opaque 保留，原载串原样回写（P1/§9.3）--------

TEST(MjProjectStoreTest, unknownFormatValueIsRetainedVerbatimAsOpaque)
{
    const std::string foreignJson =
        "{\"version\":1,\"kind\":\"mjt-project-store\",\"engine\":\"future-engine/9\","
        "\"sections\":[{\"name\":\"future.thing\",\"format\":\"cbor\",\"section_version\":2,"
        "\"content_hash\":\"sha256:"
        + std::string(64, 'f') +
        "\",\"byte_size\":7,\"record_count\":3,\"depends\":[],\"produced_by\":\"future/1\","
        "\"created_at\":\"t\",\"updated_at\":\"t\"}]}";

    auto parsed = parseStoreManifestText(refOf(foreignJson));
    if (parsed.is_err()) {
        FAIL() << std::move(parsed).unwrap_err().message.to_std_string();
    }   // 不拒载（§4.2）
    auto manifest = std::move(parsed).unwrap();
    ASSERT_EQ(1U, manifest.sections.size());
    const auto& entry = manifest.sections[0];
    EXPECT_EQ(SectionFormat::Opaque, entry.format);   // 按 opaque 收容
    EXPECT_TRUE(entry.formatRetained);
    EXPECT_EQ("cbor", entry.formatTag.to_std_string());   // 原载串保留
    EXPECT_TRUE(entry.hasRecordCount);                    // 透传保留
    EXPECT_EQ(3U, entry.recordCount);
    ASSERT_EQ(1U, manifest.diagnostics.size());
    EXPECT_NE(std::string::npos, manifest.diagnostics[0].to_std_string().find("future.thing"));
    EXPECT_NE(std::string::npos, manifest.diagnostics[0].to_std_string().find("cbor"));

    // 重写：原载 format 串原样回写（不解释、不降级改写——§9.3 只保留）。
    const auto  first = writeOrDie(manifest);
    EXPECT_NE(std::string::npos, first.find("\"format\": \"cbor\""));
    EXPECT_NE(std::string::npos, first.find("\"record_count\": 3"));

    // 重开重写逐字节一致。
    auto reparsed = parseStoreManifestText(refOf(first));
    if (reparsed.is_err()) {
        FAIL() << std::move(reparsed).unwrap_err().message.to_std_string();
    }
    EXPECT_EQ(first, writeOrDie(std::move(reparsed).unwrap()));

    // 无 record_count 的收容条目同样成立。
    const std::string foreignPlain =
        "{\"version\":1,\"kind\":\"mjt-project-store\",\"sections\":[{\"name\":\"future.thing\","
        "\"format\":\"weird\",\"section_version\":1,\"content_hash\":\"sha256:"
        + std::string(64, '0') +
        "\",\"byte_size\":1,\"depends\":[],\"produced_by\":\"p\",\"created_at\":\"t\","
        "\"updated_at\":\"t\"}]}";
    auto plainParsed = parseStoreManifestText(refOf(foreignPlain));
    ASSERT_FALSE(plainParsed.is_err());
    const auto  plainManifest = std::move(plainParsed).unwrap();
    EXPECT_EQ(SectionFormat::Opaque, plainManifest.sections[0].format);
    EXPECT_FALSE(plainManifest.sections[0].hasRecordCount);
    const auto  plainWritten = writeOrDie(plainManifest);
    EXPECT_EQ(std::string::npos, plainWritten.find("record_count"));
}

//-- §3.3 命名规则（§11-12 的 C1 函数侧预覆盖；C7 收尾边界）---------------------

TEST(MjProjectStoreTest, sectionNameRulesRejectInvalidNames)
{
    struct Case
    {
        const char* name;
        bool        valid;
    };
    const std::string maxName = "a" + std::string(63, 'x');     // 64 字符，合法上界
    const std::string overName = maxName + "x";                 // 65 字符
    const std::vector<Case> cases = {
        {"a", true},
        {"facts.alias", true},
        {"cache.decompile_v2-x.1", true},
        {maxName.c_str(), true},
        {"", false},                       // 空名
        {"Abc", false},                    // 大写
        {"aBc", false},                    // 中段大写
        {"-abc", false},                   // 首字符非 [a-z0-9]
        {"a b", false},                    // 空格
        {"a/b", false},                    // 路径分隔符
        {overName.c_str(), false},         // 超长
        {"abc.", false},                   // 尾点（Windows 陷阱）
        {"con", false},                    // 保留设备名
        {"COM1", false},                   // 保留名（大小写不敏感）
        {"aux.helper", false},             // 首段保留名
        {"nul", false},
        {"..", false},
    };
    for (const auto& testCase : cases) {
        auto verdict = validateSectionName(S(testCase.name));
        if (testCase.valid) {
            EXPECT_FALSE(verdict.is_err()) << testCase.name;
        }
        else {
            ASSERT_TRUE(verdict.is_err()) << testCase.name;
            const auto  err = std::move(verdict).unwrap_err();
            EXPECT_EQ(ProjectStoreErrorKind::Schema, err.kind) << testCase.name;
            EXPECT_FALSE(err.message.is_empty()) << testCase.name;
        }
    }

    // 扩展名映射（§3.3）：json → .json、jsonl → .jsonl、opaque → .bin。
    EXPECT_STREQ(".json", sectionFileExtension(SectionFormat::Json));
    EXPECT_STREQ(".jsonl", sectionFileExtension(SectionFormat::Jsonl));
    EXPECT_STREQ(".bin", sectionFileExtension(SectionFormat::Opaque));
    EXPECT_EQ("facts.alias.jsonl",
              sectionFileName(S("facts.alias"), SectionFormat::Jsonl).to_std_string());
}

//-- Open kind 路由：缺失/坏清单 → Open（C4 恢复扫描的降级依据）-----------------

TEST(MjProjectStoreTest, readStoreManifestFileReportsMissingAndCorruptAsOpen)
{
    const auto  dir = freshTempDir("project_store_c1_read.mjt-project");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    ASSERT_TRUE(std::filesystem::is_directory(dir));

    auto missing = readStoreManifestFile(refOf(dir.string()));
    ASSERT_TRUE(missing.is_err());
    const auto  missingError = std::move(missing).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Open, missingError.kind);
    EXPECT_NE(std::string::npos, missingError.message.to_std_string().find("store.json"));

    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "store.json").string(), "garbage{").is_err());
    auto corrupt = readStoreManifestFile(refOf(dir.string()));
    ASSERT_TRUE(corrupt.is_err());
    const auto  corruptError = std::move(corrupt).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Open, corruptError.kind);
    // join 归一化分隔符，用 unix 形态比对目录前缀。
    EXPECT_NE(std::string::npos,
              corruptError.message.to_std_string().find(
                  ca::fs::PathUtil::to_unix_separators(dir.string())));
}

//-- C2 §5.1/§5.2/§6.3：写读往返 + 双登记字段（三格式、覆盖写）------------------

TEST(MjProjectStoreTest, sectionWriteReadRoundtripRegistersExactHashAndSize)
{
    const auto  dir    = freshTempDir("project_store_c2_roundtrip.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // jsonl（§5.1 默认格式）：两记录、末行 LF——字节纪律由生产者保证，此处
    // 即最终字节；原语透传不改写。
    const std::string jsonl = "{\"id\":1,\"name\":\"a\"}\n{\"id\":2,\"name\":\"b\"}\n";
    const auto        written =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, jsonl);

    // 双登记字段（§6.3/D3）：对发布精确字节的 SHA-256 + byte_size。
    EXPECT_EQ(jsonl.size(), written.byteSize);
    EXPECT_EQ(independentHash(jsonl), written.contentHash.to_std_string());
    EXPECT_EQ(7U + 64U, written.contentHash.byte_length());

    // 发布位置与字节（§3.2：sections/<name>.<ext>，扩展名随 format）。
    ASSERT_TRUE(std::filesystem::is_regular_file(dir / "sections" / "facts.alias.jsonl"));
    EXPECT_EQ(jsonl, readFileOrDie(dir / "sections" / "facts.alias.jsonl"));

    // 登记成清单条目 → 校验通过 → 读回逐字节。
    auto entry = entryFrom("facts.alias", SectionFormat::Jsonl, written, 2);
    EXPECT_FALSE(verifySectionBytes(refOf(dirStr), entry).is_err());
    auto readBack = readSectionBytes(refOf(dirStr), entry);
    ASSERT_FALSE(readBack.is_err());
    EXPECT_EQ(jsonl, std::move(readBack).unwrap().to_std_string());

    // json（整文档型）与 opaque（保留型，任意字节含非法 UTF-8 序列）。
    const std::string json = "{\n  \"peak\": true\n}\n";
    const auto        jsonWritten =
        writeSectionOrDie(dirStr, "decode.string", SectionFormat::Json, json);
    EXPECT_EQ(independentHash(json), jsonWritten.contentHash.to_std_string());
    ASSERT_TRUE(std::filesystem::is_regular_file(dir / "sections" / "decode.string.json"));
    auto jsonEntry = entryFrom("decode.string", SectionFormat::Json, jsonWritten);
    EXPECT_FALSE(verifySectionBytes(refOf(dirStr), jsonEntry).is_err());
    auto jsonRead = readSectionBytes(refOf(dirStr), jsonEntry);
    ASSERT_FALSE(jsonRead.is_err());
    EXPECT_EQ(json, std::move(jsonRead).unwrap().to_std_string());

    // opaque：字节容器语义——from_data_unchecked 不做 UTF-8 校验、不 mangling
    // （§5.1 任意字节原样；§6.2 readBytes 返回 Utf8String 仅作容器）。
    const ca::u8 blob[] = {0x00, 0xFF, 0x10, 0xC3, 0x28, 0x01, 0xAB, 0x7F, 0x80, 0xFE};
    const auto   opaqueWritten =
        writeSectionOrDie(dirStr, "cache.blob", SectionFormat::Opaque,
                          std::string(reinterpret_cast<const char*>(blob), sizeof(blob)));
    ASSERT_TRUE(std::filesystem::is_regular_file(dir / "sections" / "cache.blob.bin"));
    auto opaqueRead = readSectionBytes(refOf(dirStr),
                                       entryFrom("cache.blob", SectionFormat::Opaque,
                                                 opaqueWritten));
    ASSERT_FALSE(opaqueRead.is_err());
    const auto blobBack = std::move(opaqueRead).unwrap();
    ASSERT_EQ(sizeof(blob), blobBack.byte_length());
    EXPECT_EQ(0, std::memcmp(blob, blobBack.data(), sizeof(blob)));

    // 覆盖写 = 写即替换（§6.3）：文件整体换新、新登记通过、旧登记漂移成
    // Damaged 材料（§8.1 崩溃表"同名 section 已是新字节、旧清单 hash 失配"）。
    const std::string v2 = "{\"id\":9}\n";
    writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, v2);
    EXPECT_EQ(v2, readFileOrDie(dir / "sections" / "facts.alias.jsonl"));
    auto stale = verifySectionBytes(refOf(dirStr), entry);
    ASSERT_TRUE(stale.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Section, std::move(stale).unwrap_err().kind);
}

//-- C2 §5.2/§8.3：先 size 后 hash 的损坏判定（§11-4 判定材料）------------------

TEST(MjProjectStoreTest, verifySectionDetectsTruncationAppendCorruptionAndMissing)
{
    const auto  dir    = freshTempDir("project_store_c2_damage.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string payload = "{\"k\":1}\n{\"k\":2}\n";
    const auto  written = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, payload);
    const auto  entry   = entryFrom("facts.alias", SectionFormat::Jsonl, written, 2);
    const auto  sectionFile = dir / "sections" / "facts.alias.jsonl";
    EXPECT_FALSE(verifySectionBytes(refOf(dirStr), entry).is_err());

    // 截断：§5.2 先验 size 再验 hash——size 面先拦，消息含期望/实际字节数。
    const std::string truncated = payload.substr(0, payload.size() - 3);
    ASSERT_FALSE(ca::fs::FileUtil::write_text(sectionFile.string(), truncated).is_err());
    auto truncVerdict = verifySectionBytes(refOf(dirStr), entry);
    ASSERT_TRUE(truncVerdict.is_err());
    const auto  truncError = std::move(truncVerdict).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Section, truncError.kind);
    EXPECT_EQ("facts.alias", truncError.section.to_std_string());
    const std::string truncMessage = truncError.message.to_std_string();
    EXPECT_NE(std::string::npos, truncMessage.find("byte_size mismatch"));
    EXPECT_NE(std::string::npos,
              truncMessage.find("expected " + std::to_string(payload.size())));
    EXPECT_NE(std::string::npos,
              truncMessage.find("got " + std::to_string(truncated.size())));

    // 追加污染：同样 size 面先拦（截断/追加都能检出，§4.2 双登记理由）。
    ASSERT_FALSE(ca::fs::FileUtil::write_text(sectionFile.string(), payload + "pollut\n").is_err());
    auto appended = verifySectionBytes(refOf(dirStr), entry);
    ASSERT_TRUE(appended.is_err());
    const auto appendedError = std::move(appended).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Section, appendedError.kind);
    EXPECT_NE(std::string::npos, appendedError.message.to_std_string().find("byte_size mismatch"));

    // 同长字节翻转：size 面通过、hash 面拦截（§8.3：消息含期望/实际 hash 前缀）。
    std::string flipped = payload;
    ASSERT_EQ('1', flipped[5]);
    flipped[5] = '2';
    ASSERT_FALSE(ca::fs::FileUtil::write_text(sectionFile.string(), flipped).is_err());
    auto flippedVerdict = verifySectionBytes(refOf(dirStr), entry);
    ASSERT_TRUE(flippedVerdict.is_err());
    const auto  flippedError = std::move(flippedVerdict).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Section, flippedError.kind);
    const std::string flippedMessage = flippedError.message.to_std_string();
    EXPECT_NE(std::string::npos, flippedMessage.find("content_hash mismatch"));
    EXPECT_NE(std::string::npos, flippedMessage.find("expected sha256:"));
    EXPECT_NE(std::string::npos, flippedMessage.find("actual sha256:"));

    // 读路径同判（§6.3：Damaged 读必败并带原因）。
    auto damagedRead = readSectionBytes(refOf(dirStr), entry);
    ASSERT_TRUE(damagedRead.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Section, std::move(damagedRead).unwrap_err().kind);

    // 文件缺失：与真损坏同落 Damaged 一条路径（§8.1 崩溃表/R45-P3，不区分）。
    std::error_code ec;
    std::filesystem::remove(sectionFile, ec);
    auto missing = verifySectionBytes(refOf(dirStr), entry);
    ASSERT_TRUE(missing.is_err());
    const auto  missingError = std::move(missing).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Section, missingError.kind);
    EXPECT_NE(std::string::npos, missingError.message.to_std_string().find("missing"));

    // 登记漂移：文件完好、条目 hash 值错误（形状合法）→ hash 面拦截。
    ASSERT_FALSE(ca::fs::FileUtil::write_text(sectionFile.string(), payload).is_err());
    auto drifted = entryFrom("facts.alias", SectionFormat::Jsonl, written, 2);
    drifted.contentHash = makeStr(hashOf('e'));
    auto driftVerdict = verifySectionBytes(refOf(dirStr), drifted);
    ASSERT_TRUE(driftVerdict.is_err());
    const auto  driftError = std::move(driftVerdict).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Section, driftError.kind);
    EXPECT_NE(std::string::npos,
              driftError.message.to_std_string().find("content_hash mismatch"));
}

//-- C2 §8.1/§8.2：txn 半提交隔离 + 自清扫（§11 清单"tmp 半提交隔离"）-----------

TEST(MjProjectStoreTest, stagedHalfCommitResidueIsolatedFromPublishedState)
{
    const auto  dir    = freshTempDir("project_store_c2_halfcommit.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string v1 = "{\"v\":1}\n";
    const auto  written = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, v1);
    const auto  entry1  = entryFrom("facts.alias", SectionFormat::Jsonl, written, 1);

    // 手植崩溃残留：模拟 §8.1-4 中途崩溃（staged section 半成品 + 半截清单，
    // §8.1-6 窗形态）。正常路径到达不了这里，走正常文件 API（§11 平台注意）。
    const auto staleTxn = dir / "tmp" / "txn-999-stale";
    std::error_code ec;
    std::filesystem::create_directories(staleTxn / "sections", ec);
    ASSERT_FALSE(ca::fs::FileUtil::write_text((staleTxn / "sections" / "facts.alias.jsonl").string(),
                                              "half-written garbage").is_err());
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((staleTxn / "store.json").string(), "{\"version\":1").is_err());

    // 隔离：发布面不受 staged 残留影响——旧字节完好、校验照常通过。
    EXPECT_EQ(v1, readFileOrDie(dir / "sections" / "facts.alias.jsonl"));
    EXPECT_FALSE(verifySectionBytes(refOf(dirStr), entry1).is_err());

    // 残留之上照常覆盖写（staged 同名冲突不可能波及发布面）→ 发布面 = 新字节。
    const std::string v2 = "{\"v\":2}\n";
    const auto  rewritten = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, v2);
    EXPECT_EQ(v2, readFileOrDie(dir / "sections" / "facts.alias.jsonl"));
    auto entry2 = entryFrom("facts.alias", SectionFormat::Jsonl, rewritten, 1);
    EXPECT_FALSE(verifySectionBytes(refOf(dirStr), entry2).is_err());
    auto staleVerdict = verifySectionBytes(refOf(dirStr), entry1);
    ASSERT_TRUE(staleVerdict.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Section, std::move(staleVerdict).unwrap_err().kind);

    // 写原语只清**自己的** txn：他人/开期残留原样保留（清扫归 C4 §8.2-1），
    // 且自己不留新 txn——tmp/ 下始终只有手植的那一个。
    EXPECT_TRUE(std::filesystem::is_directory(staleTxn));
    EXPECT_EQ(1U, tmpEntryCount(dir));

    // 非法名在任何触盘动作前拒绝（Schema，零残迹）。
    auto invalid = writeSectionBytes(refOf(dirStr), S("Bad.Name"), SectionFormat::Jsonl, refOf(v2));
    ASSERT_TRUE(invalid.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Schema, std::move(invalid).unwrap_err().kind);
    EXPECT_EQ(1U, tmpEntryCount(dir));
    EXPECT_EQ(v2, readFileOrDie(dir / "sections" / "facts.alias.jsonl"));
}

//-- C2 §6.3/D3：字节级确定性 + 同名重写恢复 -----------------------------------

TEST(MjProjectStoreTest, sectionWriteIsByteDeterministicAndSelfHealing)
{
    const auto  dirA   = freshTempDir("project_store_c2_det_a.mjt-project");
    const auto  dirB   = freshTempDir("project_store_c2_det_b.mjt-project");
    const std::string dirAStr = dirA.string();
    const std::string dirBStr = dirB.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirAStr)).is_err());
    ASSERT_FALSE(createBundleSkeleton(refOf(dirBStr)).is_err());

    const std::string payload = "{\"k\":1}\n{\"k\":2}\n";
    const auto  first  = writeSectionOrDie(dirAStr, "facts.alias", SectionFormat::Jsonl, payload);
    const auto  second = writeSectionOrDie(dirAStr, "facts.alias", SectionFormat::Jsonl, payload);

    // 同 bundle 重复写：登记恒等、文件逐字节恒等（幂等覆盖，§8.1 重试语义）。
    EXPECT_EQ(first.contentHash.to_std_string(), second.contentHash.to_std_string());
    EXPECT_EQ(first.byteSize, second.byteSize);
    EXPECT_EQ(payload, readFileOrDie(dirA / "sections" / "facts.alias.jsonl"));

    // 跨 bundle 内容寻址（D3）：同字节同 hash——发布身份只由字节决定，无环境盐。
    const auto elsewhere = writeSectionOrDie(dirBStr, "facts.alias", SectionFormat::Jsonl, payload);
    EXPECT_EQ(first.contentHash.to_std_string(), elsewhere.contentHash.to_std_string());

    // 不同字节不同 hash（登记面可区分）。
    const std::string other = "{\"k\":3}\n";
    const auto  third = writeSectionOrDie(dirAStr, "facts.alias", SectionFormat::Jsonl, other);
    EXPECT_NE(first.contentHash.to_std_string(), third.contentHash.to_std_string());

    // 损坏后同名重写恢复（§8.3"同名 sectionWriter 重写 → flush → 转 Ok"的
    // 判定材料；本卡口径：字节发布后校验即恢复 Ok）。（R53-N1：原此处对同
    // 一 payload 冗余双写，删去结果被丢弃的一次，保留被 entryFrom 消费的登记
    // 来源这次写。）
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dirA / "sections" / "facts.alias.jsonl").string(),
                                              "x").is_err());
    auto healed = entryFrom("facts.alias", SectionFormat::Jsonl,
                            writeSectionOrDie(dirAStr, "facts.alias", SectionFormat::Jsonl, payload), 2);
    EXPECT_FALSE(verifySectionBytes(refOf(dirAStr), healed).is_err());
    EXPECT_EQ(payload, readFileOrDie(dirA / "sections" / "facts.alias.jsonl"));

    // 透传：无尾 LF 的 jsonl 字节原样发布——LF 纪律归生产者/C3，引擎不改写。
    const std::string noTrailingLf = "{\"a\":1}";
    writeSectionOrDie(dirAStr, "facts.raw", SectionFormat::Jsonl, noTrailingLf);
    EXPECT_EQ(noTrailingLf, readFileOrDie(dirA / "sections" / "facts.raw.jsonl"));
}

//-- R52/CP-2：section_version 上界与 u32 对齐 ---------------------------------

TEST(MjProjectStoreTest, sectionVersionBoundIsU32Aligned)
{
    StoreManifest manifest;
    manifest.sections.push_back(makeJsonlEntry("facts.alias", '7'));
    const auto        base   = writeOrDie(manifest);
    const std::string anchor = "\"section_version\": 1";

    // u32 上界 4294967295 合法：解析保留、重写保真（不静默收窄）。
    std::string withMax = base;
    withMax.replace(withMax.find(anchor), anchor.size(), "\"section_version\": 4294967295");
    auto parsedMax = parseStoreManifestText(refOf(withMax));
    ASSERT_FALSE(parsedMax.is_err());
    auto maxManifest = std::move(parsedMax).unwrap();
    EXPECT_EQ(4294967295U, maxManifest.sections[0].sectionVersion);
    EXPECT_EQ(withMax, writeOrDie(maxManifest));

    // 超界（u64 语义的更大值）→ Schema 拒载，消息指明 u32 纪律。
    std::string withOver = base;
    withOver.replace(withOver.find(anchor), anchor.size(), "\"section_version\": 4294967296");
    const auto overMessage = expectParseError(withOver, ProjectStoreErrorKind::Schema);
    EXPECT_NE(std::string::npos, overMessage.find("u32"));

    // 0 → Schema（下界 >= 1，§4.2）。
    std::string withZero = base;
    withZero.replace(withZero.find(anchor), anchor.size(), "\"section_version\": 0");
    expectParseError(withZero, ProjectStoreErrorKind::Schema);
}

//-- R52/CP-3：空串 engine/profile 归一化为不携带 -------------------------------

TEST(MjProjectStoreTest, emptyEngineAndProfileNormalizeToAbsent)
{
    const std::string raw =
        "{\"version\":1,\"kind\":\"mjt-project-store\",\"engine\":\"\","
        "\"sections\":[{\"name\":\"facts.alias\",\"format\":\"jsonl\",\"section_version\":1,"
        "\"content_hash\":\"sha256:" + std::string(64, 'a') +
        "\",\"byte_size\":1,\"record_count\":1,\"profile\":\"\",\"depends\":[],"
        "\"produced_by\":\"p\",\"created_at\":\"t\",\"updated_at\":\"t\"}]}";

    auto parsed = parseStoreManifestText(refOf(raw));
    ASSERT_FALSE(parsed.is_err());
    auto manifest = std::move(parsed).unwrap();
    // 空串 ≡ 未设置（R52/CP-3 归一化口径）：字段层不携带、非诊断事件。
    EXPECT_TRUE(manifest.engine.is_empty());
    EXPECT_FALSE(manifest.sections[0].profile.has_value());
    EXPECT_TRUE(manifest.diagnostics.empty());

    // 写出不携带两键；重开重写逐字节一致（canonical 稳定，§4.2 确定性）。
    const auto  first = writeOrDie(manifest);
    EXPECT_EQ(std::string::npos, first.find("\"engine\""));
    EXPECT_EQ(std::string::npos, first.find("\"profile\""));
    auto reparsed = parseStoreManifestText(refOf(first));
    ASSERT_FALSE(reparsed.is_err());
    EXPECT_EQ(first, writeOrDie(std::move(reparsed).unwrap()));

    // 对照：非空 engine/profile 照常携带（归一化只针对空串）。
    manifest.engine             = Utf8String::from_cstr(kProjectStoreEngine);
    manifest.sections[0].profile = Utf8String::from_cstr("cha");
    const auto  populated = writeOrDie(manifest);
    EXPECT_NE(std::string::npos,
              populated.find(std::string("\"engine\": \"") + kProjectStoreEngine + "\""));
    EXPECT_NE(std::string::npos, populated.find("\"profile\": \"cha\""));
}

//-- C3 §6.3/§8.1 事务成功态：三格式登记提交 + 自清扫 + 幂等 + 写即替换 ----------

TEST(MjProjectStoreTest, manifestTransactionCommitsRegistrationsIdempotently)
{
    const auto        dir    = freshTempDir("project_store_c3_txn_commit.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // 三格式发布（C2 原语在先）→ 登记（C3 事务挂接）。
    const std::string jsonl = "{\"id\":1,\"name\":\"a\"}\n{\"id\":2,\"name\":\"b\"}\n";
    const auto  jsonlWritten = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, jsonl);
    const std::string json   = "{\n  \"peak\": true\n}\n";
    const auto  jsonWritten = writeSectionOrDie(dirStr, "decode.string", SectionFormat::Json, json);
    const std::string blob  = std::string("raw\x01\x02", 5) + "bytes";
    const auto  blobWritten = writeSectionOrDie(dirStr, "cache.blob", SectionFormat::Opaque, blob);

    StoreManifestMutation first;
    first.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, jsonlWritten, 2));
    first.upserts.push_back(entryFrom("decode.string", SectionFormat::Json, jsonWritten));
    first.upserts.push_back(entryFrom("cache.blob", SectionFormat::Opaque, blobWritten));
    auto committed = commitStoreManifestTransaction(refOf(dirStr), first);
    ASSERT_FALSE(committed.is_err())
        << std::move(committed).unwrap_err().message.to_std_string();

    // 提交面：store.json 可载、条目按 name 字典序、登记字段与发布逐字段一致。
    auto reloaded = readStoreManifestFile(refOf(dirStr));
    ASSERT_FALSE(reloaded.is_err());
    auto onDisk = std::move(reloaded).unwrap();
    ASSERT_EQ(3U, onDisk.sections.size());
    ASSERT_TRUE(onDisk.diagnostics.empty());
    EXPECT_EQ("cache.blob", onDisk.sections[0].name.to_std_string());
    EXPECT_EQ("decode.string", onDisk.sections[1].name.to_std_string());
    const auto& alias = onDisk.sections[2];
    EXPECT_EQ("facts.alias", alias.name.to_std_string());
    EXPECT_EQ(SectionFormat::Jsonl, alias.format);
    EXPECT_EQ(jsonlWritten.contentHash.to_std_string(), alias.contentHash.to_std_string());
    EXPECT_EQ(jsonl.size(), alias.byteSize);
    EXPECT_TRUE(alias.hasRecordCount);
    EXPECT_EQ(2U, alias.recordCount);
    EXPECT_EQ("mjt alias-pass/2.3", alias.producedBy.to_std_string());
    EXPECT_FALSE(alias.createdAt.is_empty());
    EXPECT_FALSE(alias.updatedAt.is_empty());

    // 事务自清扫：tmp/ 无残迹（§8.1-7）。
    EXPECT_EQ(0U, tmpEntryCount(dir));

    // 幂等（§6.3 no-op 口径的同状态重写）：同一变更重提 → Ok、逐字节一致。
    const auto  bytesAfterFirst = readFileOrDie(dir / "store.json");
    auto again = commitStoreManifestTransaction(refOf(dirStr), first);
    ASSERT_FALSE(again.is_err()) << std::move(again).unwrap_err().message.to_std_string();
    EXPECT_EQ(bytesAfterFirst, readFileOrDie(dir / "store.json"));
    EXPECT_EQ(0U, tmpEntryCount(dir));

    // 重复 upsert（§6.3：一次 flush 周期内每名至多一个 writer）：同一变更集
    // 同名双 push → 校验面前置 Schema 拒绝（指名），且零触盘——store.json
    // 逐字节不变、tmp/ 零残迹。
    StoreManifestMutation duplicateUpsert;
    duplicateUpsert.upserts.push_back(
        entryFrom("facts.alias", SectionFormat::Jsonl, jsonlWritten, 2));
    duplicateUpsert.upserts.push_back(
        entryFrom("facts.alias", SectionFormat::Jsonl, jsonlWritten, 2));
    auto dupUpsertRefused = commitStoreManifestTransaction(refOf(dirStr), duplicateUpsert);
    ASSERT_TRUE(dupUpsertRefused.is_err());
    const auto  dupUpsertError = std::move(dupUpsertRefused).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Schema, dupUpsertError.kind);
    EXPECT_EQ("facts.alias", dupUpsertError.section.to_std_string());
    EXPECT_NE(std::string::npos,
              dupUpsertError.message.to_std_string().find("duplicate upsert"));
    EXPECT_EQ(bytesAfterFirst, readFileOrDie(dir / "store.json"));
    EXPECT_EQ(0U, tmpEntryCount(dir));

    // 写即替换（§6.3）：v2 发布 + 新登记 → 同名条目整体替换，其余不动。
    const std::string v2 = "{\"id\":9}\n";
    const auto  v2Written = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, v2);
    StoreManifestMutation second;
    second.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, v2Written, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), second).is_err());
    auto reloaded2 = readStoreManifestFile(refOf(dirStr));
    ASSERT_FALSE(reloaded2.is_err());
    auto afterReplace = std::move(reloaded2).unwrap();
    ASSERT_EQ(3U, afterReplace.sections.size());
    const auto& aliasV2 = afterReplace.sections[2];
    EXPECT_EQ(v2Written.contentHash.to_std_string(), aliasV2.contentHash.to_std_string());
    EXPECT_EQ(v2.size(), aliasV2.byteSize);
    EXPECT_EQ(1U, aliasV2.recordCount);
    EXPECT_EQ(jsonWritten.contentHash.to_std_string(),
              afterReplace.sections[1].contentHash.to_std_string());
    EXPECT_EQ(v2, readFileOrDie(dir / "sections" / "facts.alias.jsonl"));
    EXPECT_EQ(0U, tmpEntryCount(dir));
}

//-- C3 损坏态：清单拒载 → 事务拒绝（CP-1 消费）+ P1（零触盘、永不 prune）--------

TEST(MjProjectStoreTest, manifestTransactionRefusesUnloadableManifestAndNeverTouchesData)
{
    const auto        dir    = freshTempDir("project_store_c3_txn_unloadable.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // 基线：一段已登记数据 + 一段清单外现存文件 + 一段待登记的新发布。
    const std::string payload = "{\"v\":1}\n";
    const auto  written = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, payload);
    StoreManifestMutation baseline;
    baseline.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, written, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), baseline).is_err());
    // 手植清单外文件（降级态下它是"引擎不可解释的字节"，P1：只保留）。
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "extra.jsonl").string(), "{}\n").is_err());

    const std::string blob = "\xFE\xDCpayload";
    const auto  blobWritten = writeSectionOrDie(dirStr, "cache.blob", SectionFormat::Opaque, blob);
    StoreManifestMutation mutation;
    mutation.upserts.push_back(entryFrom("cache.blob", SectionFormat::Opaque, blobWritten));

    const std::string goodBytes = readFileOrDie(dir / "store.json");

    // 变体 a：JSON 语法坏（Open）。拒绝 + store.json 保持拒载原字节（未覆写）。
    const std::string corrupt = "{ not json ";
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(), corrupt).is_err());
    auto refusedA = commitStoreManifestTransaction(refOf(dirStr), mutation);
    ASSERT_TRUE(refusedA.is_err());
    const auto  errorA = std::move(refusedA).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Open, errorA.kind);
    const std::string messageA = errorA.message.to_std_string();
    EXPECT_NE(std::string::npos, messageA.find("refused"));
    EXPECT_NE(std::string::npos, messageA.find("never auto-pruned"));
    EXPECT_EQ(corrupt, readFileOrDie(dir / "store.json"));
    EXPECT_EQ(0U, tmpEntryCount(dir));

    // 变体 b：kind 喂错（Open）。
    const std::string kindAnchor  = "\"mjt-project-store\"";
    std::string       wrongKind = goodBytes;
    wrongKind.replace(wrongKind.find(kindAnchor), kindAnchor.size(), "\"mjt-analysis-project\"");
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(), wrongKind).is_err());
    auto refusedB = commitStoreManifestTransaction(refOf(dirStr), mutation);
    ASSERT_TRUE(refusedB.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Open, std::move(refusedB).unwrap_err().kind);
    EXPECT_EQ(wrongKind, readFileOrDie(dir / "store.json"));

    // 变体 c：版本不识别（Open）。
    const std::string versionAnchor = "\"version\": 1";
    std::string       version2      = goodBytes;
    version2.replace(version2.find(versionAnchor), versionAnchor.size(), "\"version\": 2");
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(), version2).is_err());
    auto refusedC = commitStoreManifestTransaction(refOf(dirStr), mutation);
    ASSERT_TRUE(refusedC.is_err());
    const auto  errorC = std::move(refusedC).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Open, errorC.kind);
    EXPECT_NE(std::string::npos, errorC.message.to_std_string().find("never auto-pruned"));
    EXPECT_EQ(version2, readFileOrDie(dir / "store.json"));

    // 变体 d：条目未知字段（解析期 Schema 整笔拒载——CP-1 与 Open 同权触发，
    // kind 保持 Schema 上抛，分类不降级不吞并）。
    std::string entryUnknown = goodBytes;
    entryUnknown.replace(entryUnknown.find("\"name\""), 0, "\"nope\": 1, ");
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "store.json").string(), entryUnknown).is_err());
    auto refusedD = commitStoreManifestTransaction(refOf(dirStr), mutation);
    ASSERT_TRUE(refusedD.is_err());
    const auto  errorD = std::move(refusedD).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Schema, errorD.kind);
    EXPECT_NE(std::string::npos, errorD.message.to_std_string().find("refused"));
    EXPECT_EQ(entryUnknown, readFileOrDie(dir / "store.json"));

    // 变体 e：store.json 缺失（Open）。P1 核心：事务不重建清单、不删任何
    // section 文件——降级孤儿保持"只报告、永久保留"。
    std::error_code ec;
    std::filesystem::remove(dir / "store.json", ec);
    ASSERT_FALSE(std::filesystem::exists(dir / "store.json"));
    auto refusedE = commitStoreManifestTransaction(refOf(dirStr), mutation);
    ASSERT_TRUE(refusedE.is_err());
    const auto  errorE = std::move(refusedE).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Open, errorE.kind);
    EXPECT_FALSE(std::filesystem::exists(dir / "store.json"));   // 未被重建
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "sections" / "extra.jsonl"));
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "sections" / "facts.alias.jsonl"));
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "sections" / "cache.blob.bin"));
    EXPECT_EQ(0U, tmpEntryCount(dir));

#ifdef _WIN32
    // 变体 f：清单在而读不出（Io）——fail-closed 拒绝、kind 保持 Io（CP-1
    // C4 终裁已落地：Io 入降级触发面；本用例 pin 事务侧行为——拒绝 + kind
    // 不吞并，裁定文本见 isManifestUnloadableKind 注释）。
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(), goodBytes).is_err());
    FILE* held = _fsopen((dir / "store.json").string().c_str(), "r", _SH_DENYRD);
    ASSERT_NE(nullptr, held);
    auto refusedF = commitStoreManifestTransaction(refOf(dirStr), mutation);
    ASSERT_TRUE(refusedF.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Io, std::move(refusedF).unwrap_err().kind);
    EXPECT_EQ(0U, tmpEntryCount(dir));
    std::fclose(held);
    // 句柄释放后才能读回（_SH_DENYRD 连自家断言的读也挡）。
    EXPECT_EQ(goodBytes, readFileOrDie(dir / "store.json"));
#endif

    // 清单恢复可载后，同一变更即可提交（拒绝不是永久损伤）。变体 f 是
    // Windows 专属形态（_fsopen 句柄保持），非 Windows 路径 store.json 仍
    // 处于变体 e 的删除态——heal 前提是"清单在且可载"，这里统一回填。
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(), goodBytes).is_err());
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), mutation).is_err());
    auto healed = readStoreManifestFile(refOf(dirStr));
    ASSERT_FALSE(healed.is_err());
    EXPECT_EQ(2U, std::move(healed).unwrap().sections.size());
}

//-- C3 中断态：txn 残留隔离 + 自清扫不碰他人残留 --------------------------------

TEST(MjProjectStoreTest, manifestTransactionSurvivesInterruptedTxnResidue)
{
    const auto        dir    = freshTempDir("project_store_c3_txn_interrupted.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string v1 = "{\"v\":1}\n";
    const auto  written = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, v1);
    StoreManifestMutation first;
    first.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, written, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), first).is_err());

    // 手植中断残留：§8.1-6 崩溃窗形态（staged 半截清单 + 半截 section）。
    const auto staleTxn = dir / "tmp" / "txn-999-stale";
    std::error_code ec;
    std::filesystem::create_directories(staleTxn / "sections", ec);
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((staleTxn / "store.json").string(), "{\"version\":1").is_err());
    ASSERT_FALSE(ca::fs::FileUtil::write_text((staleTxn / "sections" / "facts.alias.jsonl").string(),
                                              "half-written garbage").is_err());

    // 残留之上事务照常：发布 v2 + 登记 → 提交成功，发布面 = 新状态。
    const std::string v2 = "{\"v\":2}\n";
    const auto  v2Written = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, v2);
    StoreManifestMutation second;
    second.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, v2Written, 1));
    auto committed = commitStoreManifestTransaction(refOf(dirStr), second);
    ASSERT_FALSE(committed.is_err())
        << std::move(committed).unwrap_err().message.to_std_string();
    auto reloaded = readStoreManifestFile(refOf(dirStr));
    ASSERT_FALSE(reloaded.is_err());
    auto onDisk = std::move(reloaded).unwrap();
    ASSERT_EQ(1U, onDisk.sections.size());
    EXPECT_EQ(v2Written.contentHash.to_std_string(), onDisk.sections[0].contentHash.to_std_string());
    EXPECT_EQ(v2, readFileOrDie(dir / "sections" / "facts.alias.jsonl"));

    // 事务只清自己的 txn：他人/开期残留原样保留（清扫归 C4 §8.2-1），
    // 且自己不留新 txn——tmp/ 下始终只有手植的那一个。
    EXPECT_TRUE(std::filesystem::is_directory(staleTxn));
    EXPECT_EQ(1U, tmpEntryCount(dir));
}

//-- C3 重试耗尽态：换名撞句柄 3×50ms → Io、旧清单完好、修因重试成功 --------------

TEST(MjProjectStoreTest, manifestTransactionRenameRetryExhaustionReportsIoAndRecovers)
{
#ifdef _WIN32
    const auto        dir    = freshTempDir("project_store_c3_txn_retry.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string payload = "{\"r\":1}\n";
    const auto  written = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, payload);
    StoreManifestMutation mutation;
    mutation.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, written, 1));

    const std::string before = readFileOrDie(dir / "store.json");

    // 握住 store.json 句柄（CRT fopen：无 FILE_SHARE_DELETE）→ 提交换名撞
    // sharing violation → 3 次 × 50ms 重试耗尽 = Io（§8.1 既有口径）。
    FILE* held = _fsopen((dir / "store.json").string().c_str(), "r", _SH_DENYNO);
    ASSERT_NE(nullptr, held);
    auto exhausted = commitStoreManifestTransaction(refOf(dirStr), mutation);
    ASSERT_TRUE(exhausted.is_err());
    const auto  error = std::move(exhausted).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Io, error.kind);
    EXPECT_NE(std::string::npos, error.message.to_std_string().find("store manifest"));
    EXPECT_NE(std::string::npos, error.message.to_std_string().find("3 attempts"));

    // 提交点未过：旧清单逐字节完好；重试不依赖 staged 残留（R45/P2）——
    // 耗尽后 txn 自清扫，tmp/ 无残迹。
    EXPECT_EQ(before, readFileOrDie(dir / "store.json"));
    EXPECT_EQ(0U, tmpEntryCount(dir));
    std::fclose(held);

    // 修因重试（§6.3）：句柄释放后同一变更重跑全序列 → 成功。
    auto recovered = commitStoreManifestTransaction(refOf(dirStr), mutation);
    ASSERT_FALSE(recovered.is_err())
        << std::move(recovered).unwrap_err().message.to_std_string();
    auto reloaded = readStoreManifestFile(refOf(dirStr));
    ASSERT_FALSE(reloaded.is_err());
    auto onDisk = std::move(reloaded).unwrap();
    ASSERT_EQ(1U, onDisk.sections.size());
    EXPECT_EQ(written.contentHash.to_std_string(), onDisk.sections[0].contentHash.to_std_string());
    EXPECT_EQ(0U, tmpEntryCount(dir));
#else
    GTEST_SKIP() << "rename-over-open-handle blocking is Windows-specific";
#endif
}

//-- C3 挂接面：upsert 与发布字节复核（Damaged 当场拦截）+ 收容段不可覆写 ---------

TEST(MjProjectStoreTest, manifestTransactionVerifiesUpsertsAgainstPublishedBytes)
{
    const auto        dir    = freshTempDir("project_store_c3_txn_verify.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string payload = "{\"k\":1}\n{\"k\":2}\n";
    const auto  written = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, payload);
    const std::string before = readFileOrDie(dir / "store.json");

    // hash 谎报（writer bug）→ Damaged 判定当场拦截（Section 域、指名）。
    // （StoreSectionEntry move-only：漂移条目一律经 entryFrom 重造再改字段。）
    auto driftHash        = entryFrom("facts.alias", SectionFormat::Jsonl, written, 2);
    driftHash.contentHash = makeStr(hashOf('e'));
    StoreManifestMutation hashLie;
    hashLie.upserts.push_back(std::move(driftHash));
    auto hashRefused = commitStoreManifestTransaction(refOf(dirStr), hashLie);
    ASSERT_TRUE(hashRefused.is_err());
    const auto  hashError = std::move(hashRefused).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Section, hashError.kind);
    EXPECT_EQ("facts.alias", hashError.section.to_std_string());
    EXPECT_NE(std::string::npos,
              hashError.message.to_std_string().find("does not match the published section bytes"));
    EXPECT_EQ(before, readFileOrDie(dir / "store.json"));
    EXPECT_EQ(0U, tmpEntryCount(dir));

    // size 谎报 → §5.2 先 size 后 hash，size 面先拦。
    auto driftSize     = entryFrom("facts.alias", SectionFormat::Jsonl, written, 2);
    driftSize.byteSize = written.byteSize + 1;
    StoreManifestMutation sizeLie;
    sizeLie.upserts.push_back(std::move(driftSize));
    auto sizeRefused = commitStoreManifestTransaction(refOf(dirStr), sizeLie);
    ASSERT_TRUE(sizeRefused.is_err());
    const auto  sizeError = std::move(sizeRefused).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Section, sizeError.kind);
    EXPECT_NE(std::string::npos, sizeError.message.to_std_string().find("byte_size mismatch"));
    EXPECT_EQ(before, readFileOrDie(dir / "store.json"));

    // 发布中断形态（文件缺失）→ 同落 Damaged 一条路径（§8.1 崩溃表/R45-P3）。
    std::error_code ec;
    std::filesystem::remove(dir / "sections" / "facts.alias.jsonl", ec);
    StoreManifestMutation missing;
    missing.upserts.push_back(
        entryFrom("facts.alias", SectionFormat::Jsonl, written, 2));
    auto missingRefused = commitStoreManifestTransaction(refOf(dirStr), missing);
    ASSERT_TRUE(missingRefused.is_err());
    const auto  missingError = std::move(missingRefused).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Section, missingError.kind);
    EXPECT_NE(std::string::npos, missingError.message.to_std_string().find("missing"));
    EXPECT_EQ(before, readFileOrDie(dir / "store.json"));

    // §9.3 收容段不可覆写：清单载有不识别 format 的条目 → 同名 upsert = Schema。
    const std::string retainedJson =
        "{\"version\":1,\"kind\":\"mjt-project-store\",\"engine\":\"future-engine/9\","
        "\"sections\":[{\"name\":\"future.thing\",\"format\":\"cbor\",\"section_version\":2,"
        "\"content_hash\":\"sha256:" + std::string(64, 'f') +
        "\",\"byte_size\":7,\"record_count\":3,\"depends\":[],\"produced_by\":\"future/1\","
        "\"created_at\":\"t\",\"updated_at\":\"t\"}]}";
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "store.json").string(), retainedJson).is_err());
    SectionWriteResult fabricated;
    fabricated.contentHash = makeStr(hashOf('9'));
    fabricated.byteSize    = 7;
    StoreManifestMutation rewriteRetained;
    rewriteRetained.upserts.push_back(entryFrom("future.thing", SectionFormat::Opaque, fabricated));
    auto retainedRefused = commitStoreManifestTransaction(refOf(dirStr), rewriteRetained);
    ASSERT_TRUE(retainedRefused.is_err());
    const auto  retainedError = std::move(retainedRefused).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Schema, retainedError.kind);
    EXPECT_EQ("future.thing", retainedError.section.to_std_string());
    EXPECT_NE(std::string::npos, retainedError.message.to_std_string().find("retained verbatim"));
    EXPECT_EQ(retainedJson, readFileOrDie(dir / "store.json"));
    EXPECT_EQ(0U, tmpEntryCount(dir));
}

//-- C3 墓碑：removeSection 生效 + 未知名 Schema + Damaged 段显式删除恢复 ---------

TEST(MjProjectStoreTest, manifestTransactionRemovesSectionsByTombstone)
{
    const auto        dir    = freshTempDir("project_store_c3_txn_tombstone.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string jsonl = "{\"id\":1}\n";
    const auto  jsonlWritten = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, jsonl);
    const std::string json   = "{\n  \"peak\": true\n}\n";
    const auto  jsonWritten = writeSectionOrDie(dirStr, "decode.string", SectionFormat::Json, json);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, jsonlWritten, 1));
    setup.upserts.push_back(entryFrom("decode.string", SectionFormat::Json, jsonWritten));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());

    // 墓碑生效：条目 + 文件一并消失，其余 section 不受牵连。
    StoreManifestMutation removeOne;
    removeOne.removals.push_back(makeStr("facts.alias"));
    auto committed = commitStoreManifestTransaction(refOf(dirStr), removeOne);
    ASSERT_FALSE(committed.is_err())
        << std::move(committed).unwrap_err().message.to_std_string();
    auto survived = std::move(committed).unwrap();
    ASSERT_EQ(1U, survived.sections.size());
    EXPECT_EQ("decode.string", survived.sections[0].name.to_std_string());
    EXPECT_FALSE(std::filesystem::exists(dir / "sections" / "facts.alias.jsonl"));
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "sections" / "decode.string.json"));
    EXPECT_EQ(0U, tmpEntryCount(dir));

    // 未知名墓碑 → Schema（§6.3），清单不动。
    StoreManifestMutation removeUnknown;
    removeUnknown.removals.push_back(makeStr("nope.section"));
    auto unknownRefused = commitStoreManifestTransaction(refOf(dirStr), removeUnknown);
    ASSERT_TRUE(unknownRefused.is_err());
    const auto  unknownError = std::move(unknownRefused).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Schema, unknownError.kind);
    EXPECT_EQ("nope.section", unknownError.section.to_std_string());

    // 重复墓碑（变更集自身含混：第二次删除无从谈起）：同名 removals 双 push
    // → 校验面前置 Schema 拒绝（指名），且零触盘——首次墓碑已提交的清单
    // 逐字节不动、tmp/ 零残迹。
    const std::string afterTombstone = readFileOrDie(dir / "store.json");
    StoreManifestMutation duplicateRemoval;
    duplicateRemoval.removals.push_back(makeStr("decode.string"));
    duplicateRemoval.removals.push_back(makeStr("decode.string"));
    auto dupRemovalRefused = commitStoreManifestTransaction(refOf(dirStr), duplicateRemoval);
    ASSERT_TRUE(dupRemovalRefused.is_err());
    const auto  dupRemovalError = std::move(dupRemovalRefused).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Schema, dupRemovalError.kind);
    EXPECT_EQ("decode.string", dupRemovalError.section.to_std_string());
    EXPECT_NE(std::string::npos,
              dupRemovalError.message.to_std_string().find("duplicate removal"));
    EXPECT_EQ(afterTombstone, readFileOrDie(dir / "store.json"));
    EXPECT_EQ(0U, tmpEntryCount(dir));

    // 又删又写同名 → Schema（一笔事务内语义矛盾；替换走 upsert，重写收容段
    // 走"先显式 remove 再登记"）。
    const std::string v2 = "{\"id\":2}\n";
    const auto  v2Written = writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, v2);
    StoreManifestMutation contradiction;
    contradiction.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, v2Written, 1));
    contradiction.removals.push_back(makeStr("facts.alias"));
    auto conflictRefused = commitStoreManifestTransaction(refOf(dirStr), contradiction);
    ASSERT_TRUE(conflictRefused.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Schema, std::move(conflictRefused).unwrap_err().kind);

    // Damaged 形态（条目在、文件已缺——此前中断/损坏）的显式删除 = 恢复路径：
    // 删除幂等（缺失视为已删），条目照常摘除。
    std::error_code ec;
    std::filesystem::remove(dir / "sections" / "decode.string.json", ec);
    StoreManifestMutation removeDamaged;
    removeDamaged.removals.push_back(makeStr("decode.string"));
    auto damagedGone = commitStoreManifestTransaction(refOf(dirStr), removeDamaged);
    ASSERT_FALSE(damagedGone.is_err())
        << std::move(damagedGone).unwrap_err().message.to_std_string();
    auto empty = std::move(damagedGone).unwrap();
    EXPECT_EQ(0U, empty.sections.size());
    EXPECT_EQ(0U, tmpEntryCount(dir));
}

//-- C4 §8.2 恢复扫描：分类账 + 只读 + 孤儿两档 + CP-1 终裁 Io 保守 ------------

namespace {

/// bundle 全量文件快照（相对路径 → 字节；扫描只读性断言基准。测试内容全为
/// 文本，走 read_all_text 直比）。
struct SnapshotRow
{
    std::string rel;
    std::string bytes;
};

std::vector<SnapshotRow> snapshotBundle(const std::filesystem::path& dir)
{
    std::vector<SnapshotRow> snap;
    std::error_code          ec;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || fileEc) continue;
        SnapshotRow row;
        row.rel           = std::filesystem::relative(it->path(), dir, ec).generic_string();
        auto  content     = ca::fs::FileUtil::read_all_text(it->path().string());
        EXPECT_FALSE(content.is_err()) << row.rel;
        if (!content.is_err()) row.bytes = std::move(content).unwrap();
        snap.push_back(std::move(row));
    }
    EXPECT_FALSE(ec) << ec.message();
    std::sort(snap.begin(), snap.end(),
              [](const SnapshotRow& a, const SnapshotRow& b) { return a.rel < b.rel; });
    return snap;
}

void expectSnapshotEqual(const std::vector<SnapshotRow>& before,
                         const std::vector<SnapshotRow>& after)
{
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].rel, after[i].rel) << i;
        EXPECT_EQ(before[i].bytes, after[i].bytes) << before[i].rel;
    }
}

const StoreSectionEntry* findEntry(const std::vector<StoreSectionEntry>& rows, const char* name)
{
    for (const auto& row : rows) {
        if (row.name.equals(S(name))) return &row;
    }
    return nullptr;
}

const StoreOrphanRow* findOrphan(const std::vector<StoreOrphanRow>& rows, const char* file)
{
    for (const auto& row : rows) {
        if (row.fileName.equals(S(file))) return &row;
    }
    return nullptr;
}

}   // namespace

//-- C4 §8.2-3/4：Loadable 清单下的分类账（Ok/Damaged/孤儿/txn 盘点）------------

TEST(MjProjectStoreTest, recoveryScanClassifiesOkDamagedAndTwoTierOrphans)
{
    const auto        dir    = freshTempDir("project_store_c4_scan_ledger.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // 登记两个 section（事务提交通过 = 发布字节当时校验通过）。
    const std::string aliasBytes = "{\"id\":1}\n";
    const auto aliasWritten =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, aliasBytes);
    const std::string jsonBytes = "{\n  \"peak\": true\n}\n";
    const auto jsonWritten = writeSectionOrDie(dirStr, "decode.string", SectionFormat::Json, jsonBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, aliasWritten, 1));
    setup.upserts.push_back(entryFrom("decode.string", SectionFormat::Json, jsonWritten));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());

    // 提交后损坏 decode.string（截断）→ Damaged；手植孤儿 + txn 残留。
    const std::string truncated = jsonBytes.substr(0, jsonBytes.size() - 4);
    ASSERT_FALSE(ca::fs::FileUtil::write_text(
                     (dir / "sections" / "decode.string.json").string(), truncated).is_err());
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "extra.jsonl").string(), "{}\n").is_err());
    std::error_code ec;
    std::filesystem::create_directories(dir / "tmp" / "txn-999-stale", ec);
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "tmp" / "txn-999-stale" / "store.json")
                                                  .string(),
                                              "{\"v\":1").is_err());

    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err()) << std::move(scanned).unwrap_err().message.to_std_string();
    auto report = std::move(scanned).unwrap();

    // 清单态与登记面（§8.2-3：条目保留、状态上屏）。
    EXPECT_EQ(ManifestLoadState::Loadable, report.manifestState);
    ASSERT_EQ(1U, report.okSections.size());
    EXPECT_EQ("facts.alias", report.okSections[0].name.to_std_string());
    EXPECT_EQ(aliasWritten.contentHash.to_std_string(),
              report.okSections[0].contentHash.to_std_string());
    ASSERT_EQ(1U, report.damagedSections.size());
    ASSERT_EQ(1U, report.damagedReasons.size());
    EXPECT_EQ("decode.string", report.damagedSections[0].name.to_std_string());
    EXPECT_EQ(jsonWritten.contentHash.to_std_string(),
              report.damagedSections[0].contentHash.to_std_string());   // 条目原样保留
    EXPECT_EQ(ProjectStoreErrorKind::Section, report.damagedReasons[0].kind);
    EXPECT_NE(std::string::npos,
              report.damagedReasons[0].message.to_std_string().find("byte_size mismatch"));

    // 孤儿（清单可载档 → AfterFirstFlush，§8.2-4）+ txn 盘点（§8.2-1 只报告）。
    ASSERT_EQ(1U, report.orphans.size());
    EXPECT_EQ("extra.jsonl", report.orphans[0].fileName.to_std_string());
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, report.orphans[0].prunePolicy);
    ASSERT_EQ(1U, report.txnResidues.size());
    EXPECT_EQ("txn-999-stale", report.txnResidues[0].to_std_string());

    // 诊断汇流（openDiagnostics 数据源）：Damaged 行与孤儿行都有人读行。
    std::string allDiag;
    for (const auto& diagnostic : report.diagnostics) allDiag += diagnostic.to_std_string() + "\n";
    EXPECT_NE(std::string::npos, allDiag.find("damaged section 'decode.string'"));
    EXPECT_NE(std::string::npos, allDiag.find("orphan section file 'extra.jsonl'"));
    EXPECT_NE(std::string::npos, allDiag.find("8.2-4"));
}

//-- C4 扫描只读性（专测）：两种清单态下零触盘、txn 残留不代清 ------------------

TEST(MjProjectStoreTest, recoveryScanIsReadOnlyInBothManifestStates)
{
    const auto        dir    = freshTempDir("project_store_c4_scan_readonly.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string aliasBytes = "{\"id\":1}\n";
    const auto aliasWritten =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, aliasBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, aliasWritten, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "extra.jsonl").string(), "{}\n").is_err());
    std::error_code ec;
    std::filesystem::create_directories(dir / "tmp" / "txn-42-stale", ec);

    // Loadable 态扫描：bundle 逐文件逐字节不变（含孤儿与 txn 残留）。
    const auto before = snapshotBundle(dir);
    ASSERT_FALSE(before.empty());
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err()) << std::move(scanned).unwrap_err().message.to_std_string();
    EXPECT_EQ(ManifestLoadState::Loadable, std::move(scanned).unwrap().manifestState);
    expectSnapshotEqual(before, snapshotBundle(dir));

    // Unloadable 态扫描：降级孤儿与 txn 残留同样零触碰（P1 的物理面）。
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "store.json").string(), "{ not json ").is_err());
    const auto beforeU = snapshotBundle(dir);
    auto degraded = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(degraded.is_err()) << std::move(degraded).unwrap_err().message.to_std_string();
    EXPECT_EQ(ManifestLoadState::Unloadable, std::move(degraded).unwrap().manifestState);
    expectSnapshotEqual(beforeU, snapshotBundle(dir));
    EXPECT_TRUE(std::filesystem::is_directory(dir / "tmp" / "txn-42-stale"));   // 不代清
}

//-- C4 P1 红线专测：unloadable-manifest 孤儿零 prune 路径（含 CP-1 终裁 Io）----

TEST(MjProjectStoreTest, unloadableManifestOrphansAreAlwaysNeverPrune)
{
    const auto        dir    = freshTempDir("project_store_c4_never_prune.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string aliasBytes = "{\"id\":1}\n";
    const auto aliasWritten =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, aliasBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, aliasWritten, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "extra.jsonl").string(), "{}\n").is_err());

    const std::string goodBytes = readFileOrDie(dir / "store.json");

    // 每个拒载/读错变体同一断言面：Unloadable + kind 原样上浮 + 现存文件全部
    // 转孤儿且恒 Never（可 prune 工作清单恒空 = 零 prune 路径）。
    const auto scanUnloadable = [&](ProjectStoreErrorKind expectedKind) {
        auto scanned = scanBundleRecovery(refOf(dirStr));
        ASSERT_FALSE(scanned.is_err())
            << std::move(scanned).unwrap_err().message.to_std_string();
        auto report = std::move(scanned).unwrap();
        EXPECT_EQ(ManifestLoadState::Unloadable, report.manifestState);
        EXPECT_EQ(expectedKind, report.manifestError.kind);   // kind 不吞并
        EXPECT_TRUE(report.okSections.empty());
        EXPECT_TRUE(report.damagedSections.empty());
        ASSERT_EQ(2U, report.orphans.size());   // 现存文件全部转孤儿（登记不可信）
        EXPECT_EQ("extra.jsonl", report.orphans[0].fileName.to_std_string());
        EXPECT_EQ("facts.alias.jsonl", report.orphans[1].fileName.to_std_string());
        for (const auto& orphan : report.orphans) {
            EXPECT_EQ(OrphanPrunePolicy::Never, orphan.prunePolicy)
                << orphan.fileName.to_std_string();
        }
        std::string allDiag;
        for (const auto& diagnostic : report.diagnostics)
            allDiag += diagnostic.to_std_string() + "\n";
        EXPECT_NE(std::string::npos, allDiag.find("never auto-pruned"));
    };

    // 变体 a：store.json 缺失（Open）。
    std::error_code ec;
    std::filesystem::remove(dir / "store.json", ec);
    scanUnloadable(ProjectStoreErrorKind::Open);

    // 变体 b：JSON 语法坏（Open）。
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "store.json").string(), "{ not json ").is_err());
    scanUnloadable(ProjectStoreErrorKind::Open);

    // 变体 c：版本不识别（Open）。
    std::string version2 = goodBytes;
    const std::string versionAnchor = "\"version\": 1";
    version2.replace(version2.find(versionAnchor), versionAnchor.size(), "\"version\": 2");
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(), version2).is_err());
    scanUnloadable(ProjectStoreErrorKind::Open);

    // 变体 d：条目未知字段（解析期 Schema 整笔拒载）。
    std::string entryUnknown = goodBytes;
    entryUnknown.replace(entryUnknown.find("\"name\""), 0, "\"nope\": 1, ");
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "store.json").string(), entryUnknown).is_err());
    scanUnloadable(ProjectStoreErrorKind::Schema);

#ifdef _WIN32
    // 变体 e：清单在而读不出（Io）——CP-1 终裁的直接断言：Io 同落 Unloadable，
    // 孤儿恒 Never（宁可错判不可误 prune）。
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(), goodBytes).is_err());
    FILE* held = _fsopen((dir / "store.json").string().c_str(), "r", _SH_DENYRD);
    ASSERT_NE(nullptr, held);
    scanUnloadable(ProjectStoreErrorKind::Io);
    std::fclose(held);
#endif

    // 清单恢复可载 → 同一批文件回档：已登记字节回登记面（Ok），清单外文件回
    // AfterFirstFlush 档——档位跟随清单可载性，不是文件的固有属性。
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(), goodBytes).is_err());
    auto healed = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(healed.is_err()) << std::move(healed).unwrap_err().message.to_std_string();
    auto healedReport = std::move(healed).unwrap();
    EXPECT_EQ(ManifestLoadState::Loadable, healedReport.manifestState);
    ASSERT_EQ(1U, healedReport.okSections.size());
    EXPECT_EQ("facts.alias", healedReport.okSections[0].name.to_std_string());
    ASSERT_EQ(1U, healedReport.orphans.size());
    EXPECT_EQ("extra.jsonl", healedReport.orphans[0].fileName.to_std_string());
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, healedReport.orphans[0].prunePolicy);
}

//-- C4 §9.3 收容段：未知扩展名字节永不落 prune 档 -------------------------------

TEST(MjProjectStoreTest, recoveryScanKeepsRetainedBytesOutOfPrunableOrphans)
{
    const auto        dir    = freshTempDir("project_store_c4_retained.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // 正常段走事务登记；收容段手植未知扩展名文件（本引擎无 cbor 写路径——
    // 正常路径到达不了这里，走正常文件 API，§11 平台注意）。
    const std::string aliasBytes = "{\"id\":1}\n";
    const auto aliasWritten =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, aliasBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, aliasWritten, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());

    const std::string retainedBytes = "future-bytes\n";
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "sections" / "future.thing.cbor").string(),
                                              retainedBytes).is_err());

    // 清单追加收容条目（format "cbor"，原载串 + §9.3 收容语义）。
    auto loaded = readStoreManifestFile(refOf(dirStr));
    ASSERT_FALSE(loaded.is_err());
    auto manifest = std::move(loaded).unwrap();
    StoreSectionEntry retained;
    retained.name            = Utf8String::from_cstr("future.thing");
    retained.format          = SectionFormat::Opaque;
    retained.formatTag       = Utf8String::from_cstr("cbor");
    retained.formatRetained  = true;
    retained.sectionVersion  = 2;
    retained.contentHash     = makeStr(independentHash(retainedBytes));
    retained.byteSize        = retainedBytes.size();
    retained.producedBy      = Utf8String::from_cstr("future-engine/9");
    retained.createdAt       = Utf8String::from_cstr("t");
    retained.updatedAt       = Utf8String::from_cstr("t");
    manifest.sections.push_back(std::move(retained));
    auto text = writeStoreManifestText(manifest);
    ASSERT_FALSE(text.is_err());
    const std::string manifestBytes = std::move(text).unwrap().to_std_string();
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "store.json").string(), manifestBytes).is_err());

    // 词根匹配：future.thing.cbor 受验为 Ok，绝不出现在孤儿面。
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err()) << std::move(scanned).unwrap_err().message.to_std_string();
    auto report = std::move(scanned).unwrap();
    EXPECT_EQ(ManifestLoadState::Loadable, report.manifestState);
    ASSERT_EQ(2U, report.okSections.size());
    const auto* retainedRow = findEntry(report.okSections, "future.thing");
    ASSERT_NE(nullptr, retainedRow);
    EXPECT_TRUE(retainedRow->formatRetained);
    EXPECT_EQ("cbor", retainedRow->formatTag.to_std_string());
    EXPECT_TRUE(findEntry(report.okSections, "facts.alias") != nullptr);
    EXPECT_TRUE(report.orphans.empty());

    // 词根多匹配：首文件（字典序）受验，其余按 Never 孤儿上报——可能是真收容
    // 字节的变体扩展名，引擎无法辨别 → 永不 prune。
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "sections" / "future.thing.bin").string(),
                                              retainedBytes).is_err());
    auto ambigued = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(ambigued.is_err()) << std::move(ambigued).unwrap_err().message.to_std_string();
    auto ambiguedReport = std::move(ambigued).unwrap();
    EXPECT_TRUE(findEntry(ambiguedReport.okSections, "future.thing") != nullptr);
    ASSERT_EQ(1U, ambiguedReport.orphans.size());
    EXPECT_EQ("future.thing.cbor", ambiguedReport.orphans[0].fileName.to_std_string());
    EXPECT_EQ(OrphanPrunePolicy::Never, ambiguedReport.orphans[0].prunePolicy);

    // 收容段文件整体缺失 → Damaged（条目保留、状态上屏），孤儿面仍为空。
    std::error_code ec;
    std::filesystem::remove(dir / "sections" / "future.thing.cbor", ec);
    std::filesystem::remove(dir / "sections" / "future.thing.bin", ec);
    auto missing = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(missing.is_err()) << std::move(missing).unwrap_err().message.to_std_string();
    auto missingReport = std::move(missing).unwrap();
    ASSERT_EQ(1U, missingReport.damagedSections.size());
    EXPECT_EQ("future.thing", missingReport.damagedSections[0].name.to_std_string());
    ASSERT_EQ(1U, missingReport.damagedReasons.size());
    EXPECT_EQ(ProjectStoreErrorKind::Section, missingReport.damagedReasons[0].kind);
    EXPECT_NE(std::string::npos, missingReport.damagedReasons[0].message.to_std_string()
                                     .find("retained section has no matching file"));
    EXPECT_TRUE(missingReport.orphans.empty());
    EXPECT_TRUE(findEntry(missingReport.okSections, "facts.alias") != nullptr);
}

//-- C4 Damaged 面的 Io 保守（宁可错报不可漏报）---------------------------------

TEST(MjProjectStoreTest, recoveryScanReportsDamagedOnIoReadFailureInsteadOfOk)
{
#ifdef _WIN32
    const auto        dir    = freshTempDir("project_store_c4_io_damaged.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string aliasBytes = "{\"id\":1}\n";
    const auto aliasWritten =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, aliasBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, aliasWritten, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());

    // 握住已登记 section 文件的读句柄：扫描读不出 = 不能证明 Ok → Damaged 行
    // （reason kind 保持 Io，与 CP-1 同款"宁可错报不可漏报"保守）。
    FILE* held =
        _fsopen((dir / "sections" / "facts.alias.jsonl").string().c_str(), "r", _SH_DENYRD);
    ASSERT_NE(nullptr, held);
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err()) << std::move(scanned).unwrap_err().message.to_std_string();
    auto report = std::move(scanned).unwrap();
    EXPECT_EQ(ManifestLoadState::Loadable, report.manifestState);
    EXPECT_TRUE(report.okSections.empty());
    ASSERT_EQ(1U, report.damagedSections.size());
    EXPECT_EQ("facts.alias", report.damagedSections[0].name.to_std_string());
    ASSERT_EQ(1U, report.damagedReasons.size());
    EXPECT_EQ(ProjectStoreErrorKind::Io, report.damagedReasons[0].kind);
    EXPECT_TRUE(report.orphans.empty());   // 登记文件即使读不出也非孤儿
    std::fclose(held);

    // 句柄释放 → 同一文件回到 Ok（Io 错判是瞬态报告，非持久降级）。
    auto healed = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(healed.is_err()) << std::move(healed).unwrap_err().message.to_std_string();
    auto healedReport = std::move(healed).unwrap();
    ASSERT_EQ(1U, healedReport.okSections.size());
    EXPECT_EQ("facts.alias", healedReport.okSections[0].name.to_std_string());
    EXPECT_TRUE(healedReport.damagedSections.empty());
#else
    GTEST_SKIP() << "deny-read handle holding is Windows-specific";
#endif
}

//-- C4 prune 政策边界：登记面/损坏面绝不混入可 prune 工作清单 -------------------

TEST(MjProjectStoreTest, prunePolicyBoundaryRegisteredAndDamagedNeverMixIntoOrphans)
{
    const auto        dir    = freshTempDir("project_store_c4_policy_boundary.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // 三段登记：facts.alias（保持 Ok）、cache.blob（提交后截断 → Damaged）、
    // decode.string（提交后删文件 → Damaged-missing）。
    const std::string aliasBytes = "{\"id\":1}\n";
    const auto aliasWritten =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, aliasBytes);
    const std::string blob = std::string("raw\x01\x02", 5) + "bytes";
    const auto  blobWritten = writeSectionOrDie(dirStr, "cache.blob", SectionFormat::Opaque, blob);
    const std::string jsonBytes = "{\n  \"peak\": true\n}\n";
    const auto jsonWritten = writeSectionOrDie(dirStr, "decode.string", SectionFormat::Json, jsonBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, aliasWritten, 1));
    setup.upserts.push_back(entryFrom("cache.blob", SectionFormat::Opaque, blobWritten));
    setup.upserts.push_back(entryFrom("decode.string", SectionFormat::Json, jsonWritten));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());

    std::error_code ec;
    std::filesystem::remove(dir / "sections" / "decode.string.json", ec);
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "sections" / "cache.blob.bin").string(), "x")
                     .is_err());
    // 同名词根不同扩展的手植文件（格式替换半提交的孤儿形态）+ 纯孤儿。
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "facts.alias.json").string(), "{}\n")
            .is_err());
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "extra.jsonl").string(), "{}\n").is_err());

    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err()) << std::move(scanned).unwrap_err().message.to_std_string();
    auto report = std::move(scanned).unwrap();

    EXPECT_EQ(ManifestLoadState::Loadable, report.manifestState);
    ASSERT_EQ(1U, report.okSections.size());
    EXPECT_EQ("facts.alias", report.okSections[0].name.to_std_string());
    ASSERT_EQ(2U, report.damagedSections.size());
    EXPECT_EQ("cache.blob", report.damagedSections[0].name.to_std_string());
    EXPECT_EQ("decode.string", report.damagedSections[1].name.to_std_string());

    // 可 prune 工作清单 = 恰好两个手植清单外文件；登记面（Ok）与损坏面
    // （Damaged，含文件缺失）绝不出现在孤儿面——auto prune 的边界即此。
    ASSERT_EQ(2U, report.orphans.size());
    EXPECT_EQ("extra.jsonl", report.orphans[0].fileName.to_std_string());
    EXPECT_EQ("facts.alias.json", report.orphans[1].fileName.to_std_string());
    for (const auto& orphan : report.orphans) {
        EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, orphan.prunePolicy);
    }
    EXPECT_TRUE(findOrphan(report.orphans, "facts.alias.jsonl") == nullptr);
    EXPECT_TRUE(findOrphan(report.orphans, "cache.blob.bin") == nullptr);
    EXPECT_TRUE(findOrphan(report.orphans, "decode.string.json") == nullptr);
}

//-- C5 §7.1 获取/二次获取报 owner 三元组/释放清场 + Io 失败面 -------------------

TEST(MjProjectStoreTest, storeLockAcquireHoldsExclusiveReportsOwnerAndRemovesOnRelease)
{
    const auto        dir    = freshTempDir("project_store_c5_acquire.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // 获取：锁文件出现，内容单行 JSON 含本进程 pid + created_at + owner。
    auto acquired = acquireStoreLock(refOf(dirStr));
    ASSERT_FALSE(acquired.is_err()) << std::move(acquired).unwrap_err().message.to_std_string();
    auto lock = std::move(acquired).unwrap();
    EXPECT_TRUE(lock.holdsLock());
    EXPECT_TRUE(lock.diagnostics().empty());   // 纯获取（无接管）零诊断
    const auto lockPath = dir / kProjectStoreLockName;
    EXPECT_TRUE(std::filesystem::is_regular_file(lockPath));
    const std::string lockBytes = readFileOrDie(lockPath);
    EXPECT_NE(std::string::npos, lockBytes.find("\"pid\":" + std::to_string(currentProcessPid())));
    EXPECT_NE(std::string::npos, lockBytes.find("\"created_at\":\""));
    EXPECT_NE(std::string::npos, lockBytes.find("\"owner\":\"mjt project open\""));
    // 单行：唯一 LF 是尾随（§7.1 内容单行 JSON）。
    const std::size_t firstLf = lockBytes.find('\n');
    ASSERT_NE(std::string::npos, firstLf);
    EXPECT_EQ(lockBytes.size() - 1, firstLf);

    // 二次获取（同 bundle）= Lock 错，消息含 pid/created_at/owner 与路径
    // （§7.1 IDA "database is locked by" 同款体验；§11-3：消息含 pid）。
    auto again = acquireStoreLock(refOf(dirStr));
    ASSERT_TRUE(again.is_err());
    const auto  againError = std::move(again).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Lock, againError.kind);
    const std::string againMessage = againError.message.to_std_string();
    EXPECT_NE(std::string::npos, againMessage.find(std::to_string(currentProcessPid())));
    EXPECT_NE(std::string::npos, againMessage.find("created_at="));
    EXPECT_NE(std::string::npos, againMessage.find("owner='mjt project open'"));
    // 路径定位（bundle 名无分隔符——PathUtil::join 的分隔符口径不参与断言）。
    EXPECT_NE(std::string::npos, againMessage.find(dir.filename().string()));
    // 失败不留残迹：原锁原样（内容逐字节未动）。
    EXPECT_EQ(lockBytes, readFileOrDie(lockPath));

    // 释放：锁文件消失（§11-3 close 后 lock 文件消失）；release 幂等。
    lock.release();
    EXPECT_FALSE(lock.holdsLock());
    EXPECT_FALSE(std::filesystem::exists(lockPath));
    lock.release();   // 幂等，无副作用

    // 释放后可重新获取（同一 bundle 循环使用）。
    auto reacquired = acquireStoreLock(refOf(dirStr));
    ASSERT_FALSE(reacquired.is_err());
    std::move(reacquired).unwrap().release();
    EXPECT_FALSE(std::filesystem::exists(lockPath));

    // bundle 目录不存在 → Io（真实文件系统失败，不吞错不造目录）。
    const auto missing = freshTempDir("project_store_c5_missing_dir.mjt-project");
    auto onMissing = acquireStoreLock(refOf(missing.string()));
    ASSERT_TRUE(onMissing.is_err());
    const auto missingError = std::move(onMissing).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Io, missingError.kind);
    EXPECT_NE(std::string::npos, missingError.message.to_std_string().find("store.lock"));
}

//-- C5 §11-3 死 pid 手写锁的自动接管（spawn→取 pid→kill 构造法）------------------

TEST(MjProjectStoreTest, storeLockAutoTakeoverSucceedsExactlyOnceOnDeadPidLockFile)
{
    const auto        dir    = freshTempDir("project_store_c5_takeover.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // §11-3 死 pid 构造法（主法）：spawn 子进程 → 取 pid → kill。
    const long long deadPid = spawnAndKillChildProcess();
    ASSERT_GT(deadPid, 0) << "spawn failed; §11-3 fallback is improbable-pid + skip";
    if (storeLockPidAlive(deadPid)) {
        GTEST_SKIP() << "cannot prove child pid " << deadPid
                     << " death (fail-closed probe says alive); skip per section 11-3";
    }

    // 以死 pid 手写 lock 文件（正常文件 API，§11 平台注意）。
    const std::string ghost =
        writeLockOrDie(dir, lockJson(deadPid, "2026-01-01T00:00:00Z", "ghost-process"));

    // 自动接管成功：无需 stealLock——探测证明死 → 删旧 + 独占重试恰好一次。
    auto takeover = acquireStoreLock(refOf(dirStr));
    ASSERT_FALSE(takeover.is_err()) << std::move(takeover).unwrap_err().message.to_std_string();
    auto lock = std::move(takeover).unwrap();

    // 接管诊断记录事实（§7.1：死锁接管记 owner 全文 + 判死依据）。
    const std::string diag = joinDiagnostics(lock.diagnostics());
    EXPECT_NE(std::string::npos, diag.find("taken over"));
    EXPECT_NE(std::string::npos, diag.find("ghost-process"));
    EXPECT_NE(std::string::npos, diag.find(std::to_string(deadPid)));
    EXPECT_NE(std::string::npos, diag.find("PID probe"));

    // 锁内容已换成本进程（旧 ghost 字节不在盘上）。
    const auto lockPath = dir / kProjectStoreLockName;
    const std::string lockBytes = readFileOrDie(lockPath);
    EXPECT_NE(std::string::npos, lockBytes.find("\"pid\":" + std::to_string(currentProcessPid())));
    EXPECT_EQ(std::string::npos, lockBytes.find("ghost-process"));

    // 接管后的锁就是普通活锁：二次获取被拒（本人 pid 活）。
    auto again = acquireStoreLock(refOf(dirStr));
    ASSERT_TRUE(again.is_err());
    EXPECT_EQ(ProjectStoreErrorKind::Lock, std::move(again).unwrap_err().kind);

    lock.release();
    EXPECT_FALSE(std::filesystem::exists(lockPath));
}

//-- C5 R45/P5 不可解析 fail-closed：禁止自动接管 --------------------------------

TEST(MjProjectStoreTest, storeLockUnparseableContentIsFailClosedLiveLockRefusingAutoTakeover)
{
    const auto        dir    = freshTempDir("project_store_c5_unparseable.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto lockPath = dir / kProjectStoreLockName;

    // 不可解析变体全集（§7.1 崩溃窗的空/半截文件 + 形状违约）：任一都按活锁
    // fail-closed，注明 unparseable + 指路 stealLock，禁止自动接管。
    const std::vector<std::string> cases = {
        "",                                                  // 空文件（内容写完前崩溃）
        "{\"pid\":" ,                                        // 半截 JSON
        "not json at all\n",                                 // 非 JSON
        "{\"pid\":123,\"created_at\":\"t\"}\n",              // 缺 owner（读不出 owner 的锁）
        "{\"pid\":0,\"created_at\":\"t\",\"owner\":\"x\"}\n",// pid ≤ 0
        "{\"pid\":\"7\",\"created_at\":\"t\",\"owner\":\"x\"}\n", // pid 非整数
        "{\"pid\":5,\"created_at\":\"\",\"owner\":\"x\"}\n", // created_at 空
        "{\"pid\":5,\"created_at\":\"t\",\"owner\":\"\"}\n", // owner 空
        "{\"pid\":5,\"pid\":6,\"created_at\":\"t\",\"owner\":\"x\"}\n", // duplicate key
    };
    for (const std::string& payload : cases) {
        writeLockOrDie(dir, payload);
        auto refused = acquireStoreLock(refOf(dirStr));
        ASSERT_TRUE(refused.is_err()) << "payload: \"" << payload << "\"";
        const auto  error = std::move(refused).unwrap_err();
        EXPECT_EQ(ProjectStoreErrorKind::Lock, error.kind) << "payload: \"" << payload << "\"";
        const std::string message = error.message.to_std_string();
        EXPECT_NE(std::string::npos, message.find("unparseable")) << "payload: \"" << payload << "\"";
        EXPECT_NE(std::string::npos, message.find("stealLock")) << "payload: \"" << payload << "\"";
        // 禁止自动接管：文件原字节未动（还在、逐字节原样）。
        EXPECT_TRUE(std::filesystem::is_regular_file(lockPath)) << "payload: \"" << payload << "\"";
        EXPECT_EQ(payload, readFileOrDie(lockPath)) << "payload: \"" << payload << "\"";
    }

    // 对照：同批不可解析内容下 stealLock 是唯一出路（成功，见抢占专测）。
    auto withSteal = acquireStoreLock(refOf(dirStr), StoreLockOptions{true});
    ASSERT_FALSE(withSteal.is_err())
        << std::move(withSteal).unwrap_err().message.to_std_string();
    auto stolen = std::move(withSteal).unwrap();
    const std::string lockBytes = readFileOrDie(lockPath);
    EXPECT_NE(std::string::npos, lockBytes.find("\"pid\":" + std::to_string(currentProcessPid())));
    stolen.release();
    EXPECT_FALSE(std::filesystem::exists(lockPath));
}

//-- C5 §7.1 stealLock 抢占活持有者 + 诊断必须记录抢占事实 ------------------------

TEST(MjProjectStoreTest, storeLockStealLockPreemptsLiveOwnerAndRecordsPreemptionDiagnostic)
{
    const auto        dir    = freshTempDir("project_store_c5_steal.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto lockPath = dir / kProjectStoreLockName;

    // 活持有者场景：本进程 pid（探测必为活——§7.2 证明不了的才按活，本 pid
    // 正在跑测试本身，探测恒活）。
    const long long holderPid = currentProcessPid();
    ASSERT_TRUE(storeLockPidAlive(holderPid));
    const std::string liveOwner =
        writeLockOrDie(dir, lockJson(holderPid, "2026-02-02T08:09:10Z", "live-holder"));

    // 不带 stealLock → Lock 错（消息含 pid/created_at/owner），锁文件不动。
    auto refused = acquireStoreLock(refOf(dirStr));
    ASSERT_TRUE(refused.is_err());
    const auto  refusedError = std::move(refused).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Lock, refusedError.kind);
    const std::string refusedMessage = refusedError.message.to_std_string();
    EXPECT_NE(std::string::npos, refusedMessage.find(std::to_string(holderPid)));
    EXPECT_NE(std::string::npos, refusedMessage.find("2026-02-02T08:09:10Z"));
    EXPECT_NE(std::string::npos, refusedMessage.find("live-holder"));
    EXPECT_EQ(liveOwner, readFileOrDie(lockPath));

    // stealLock = true → 跳探测直接删旧重试一次，成功；诊断必须记录抢占事实
    // （§7.1）。
    auto preempted = acquireStoreLock(refOf(dirStr), StoreLockOptions{true});
    ASSERT_FALSE(preempted.is_err())
        << std::move(preempted).unwrap_err().message.to_std_string();
    auto lock = std::move(preempted).unwrap();
    const std::string diag = joinDiagnostics(lock.diagnostics());
    EXPECT_NE(std::string::npos, diag.find("stealLock"));
    EXPECT_NE(std::string::npos, diag.find("preempted"));
    EXPECT_NE(std::string::npos, diag.find("live-holder"));
    EXPECT_NE(std::string::npos, diag.find(std::to_string(holderPid)));
    EXPECT_NE(std::string::npos, diag.find("2026-02-02T08:09:10Z"));
    // 抢占后的锁内容归本进程；释放清场。
    const std::string lockBytes = readFileOrDie(lockPath);
    EXPECT_NE(std::string::npos, lockBytes.find("\"pid\":" + std::to_string(currentProcessPid())));
    EXPECT_EQ(std::string::npos, lockBytes.find("live-holder"));
    lock.release();
    EXPECT_FALSE(std::filesystem::exists(lockPath));

    // stealLock 对不可解析旧锁同样直接换掉（人工决策出口，§7.1）——诊断注明
    // 旧内容读不出（不虚构 owner 三元组）。
    writeLockOrDie(dir, "{\"pid\":");
    auto stolenUnparseable = acquireStoreLock(refOf(dirStr), StoreLockOptions{true});
    ASSERT_FALSE(stolenUnparseable.is_err())
        << std::move(stolenUnparseable).unwrap_err().message.to_std_string();
    auto stolen = std::move(stolenUnparseable).unwrap();
    const std::string stolenDiag = joinDiagnostics(stolen.diagnostics());
    EXPECT_NE(std::string::npos, stolenDiag.find("stealLock"));
    EXPECT_NE(std::string::npos, stolenDiag.find("unreadable/unparseable"));
    stolen.release();
    EXPECT_FALSE(std::filesystem::exists(lockPath));
}

//-- C5 接管的删除受阻面：fail-closed 不破坏、受阻解除后恢复 ----------------------

TEST(MjProjectStoreTest, storeLockRemovalFailureFailsClosedAndRecoversAfterBlockerCloses)
{
#ifdef _WIN32
    const auto        dir    = freshTempDir("project_store_c5_blocked.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto lockPath = dir / kProjectStoreLockName;

    const long long deadPid = spawnAndKillChildProcess();
    ASSERT_GT(deadPid, 0) << "spawn failed; §11-3 fallback is improbable-pid + skip";
    if (storeLockPidAlive(deadPid)) {
        GTEST_SKIP() << "cannot prove child pid " << deadPid
                     << " death (fail-closed probe says alive); skip per section 11-3";
    }
    const std::string ghost =
        writeLockOrDie(dir, lockJson(deadPid, "2026-01-01T00:00:00Z", "ghost-process"));

    // 外部句柄攥住锁文件（CRT fopen 无 FILE_SHARE_DELETE → 删除必撞 sharing
    // violation）：接管路径的删旧失败 = Io，fail-closed（不破坏、不循环、
    // 原字节不动）。
    FILE* blocker = _fsopen(lockPath.string().c_str(), "r", _SH_DENYNO);
    ASSERT_NE(nullptr, blocker);
    auto takeover = acquireStoreLock(refOf(dirStr));
    ASSERT_TRUE(takeover.is_err());
    const auto  takeoverError = std::move(takeover).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Io, takeoverError.kind);
    const std::string message = takeoverError.message.to_std_string();
    EXPECT_NE(std::string::npos, message.find("failed to remove store.lock"));
    // 路径定位（bundle 名无分隔符——PathUtil::join 的分隔符口径不参与断言）。
    EXPECT_NE(std::string::npos, message.find(dir.filename().string()));
    EXPECT_EQ(ghost, readFileOrDie(lockPath));   // 原字节未动
    std::fclose(blocker);

    // 受阻解除 → 同一死 pid 锁照常自动接管（恢复路径）。
    auto healed = acquireStoreLock(refOf(dirStr));
    ASSERT_FALSE(healed.is_err()) << std::move(healed).unwrap_err().message.to_std_string();
    auto lock = std::move(healed).unwrap();
    EXPECT_NE(std::string::npos, joinDiagnostics(lock.diagnostics()).find("taken over"));
    lock.release();
    EXPECT_FALSE(std::filesystem::exists(lockPath));
#else
    GTEST_SKIP() << "delete-blocking handle holding is Windows-specific";
#endif
}

//-- C5 §7.2 PID 探测纪律（fail-closed：证明不了死即活）--------------------------

TEST(MjProjectStoreTest, storeLockPidProbeFailClosedDiscipline)
{
    // 活：本进程（正在运行）。
    EXPECT_TRUE(storeLockPidAlive(currentProcessPid()));

    // 结构性死：pid ≤ 0 不可能是真进程（daemon_state.cpp 同款门）。
    EXPECT_FALSE(storeLockPidAlive(0));
    EXPECT_FALSE(storeLockPidAlive(-1));
    EXPECT_FALSE(storeLockPidAlive(-123456));

    // 结构性死：超出平台 pid 空间（Windows pid ≤ DWORD；POSIX pid_t 上限），
    // 范围门直接判死，不做系统调用。
    EXPECT_FALSE(storeLockPidAlive(1LL << 40));
    EXPECT_FALSE(storeLockPidAlive(std::numeric_limits<long long>::max()));

    // 极不可能 pid（§11-3 备选法的探测断言；理论撞上活进程则 skip——按设计
    // "探测断言失败即 skip 该用例"口径）。
#ifdef _WIN32
    constexpr long long kImprobable = 0xFFFFFFFE;   // Windows pid 空间顶端
#else
    constexpr long long kImprobable = (1LL << 30);  // 远超常规 pid_max
#endif
    if (storeLockPidAlive(kImprobable)) {
        GTEST_SKIP() << "improbable pid " << kImprobable
                     << " happened to be alive; skip per section 11-3";
    }
    SUCCEED() << "improbable pid " << kImprobable << " proven dead";
}

//-- C5 红线互不越界：恢复扫描对 store.lock 零触碰 --------------------------------

TEST(MjProjectStoreTest, recoveryScanLeavesStoreLockUntouched)
{
    const auto        dir    = freshTempDir("project_store_c5_scan_boundary.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // 手植 store.lock（活持有者形态）+ txn 残留 + 孤儿各一：扫描的盘点面是
    // sections/ + store.json + tmp/，store.lock 不在其中（C4 扫描注释的
    // "不触 store.lock" 钉死；锁路径与 prune 红线互不越界）。
    const std::string lockBytes = writeLockOrDie(
        dir, lockJson(currentProcessPid(), "2026-01-01T00:00:00Z", "live-holder"));
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "sections" / "extra.jsonl").string(), "{}\n")
                     .is_err());
    ASSERT_TRUE(ca::fs::FileUtil::create_directories((dir / "tmp" / "txn-1-2").string()));
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "tmp" / "txn-1-2" / "leftover.bin").string(),
                                              "x").is_err());

    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err()) << std::move(scanned).unwrap_err().message.to_std_string();
    auto report = std::move(scanned).unwrap();

    // 锁不在任何报告行里：诊断零提及、孤儿/txn 盘点不含 store.lock。
    std::string allDiagnostics;
    for (const auto& line : report.diagnostics) allDiagnostics += line.to_std_string() + "\n";
    EXPECT_EQ(std::string::npos, allDiagnostics.find("store.lock"));
    EXPECT_EQ(std::string::npos, allDiagnostics.find("live-holder"));
    ASSERT_EQ(1U, report.orphans.size());            // 只有 sections/extra.jsonl
    EXPECT_EQ("extra.jsonl", report.orphans[0].fileName.to_std_string());
    ASSERT_EQ(1U, report.txnResidues.size());
    EXPECT_EQ("txn-1-2", report.txnResidues[0].to_std_string());

    // 扫描后锁文件逐字节原样（扫描零触盘——对锁同样成立）。
    EXPECT_EQ(lockBytes, readFileOrDie(dir / kProjectStoreLockName));
}

//-- C5 R64-N1：POSIX 释放属主核验——手造 inode 错位，绝不误删抢占者新锁-----------

TEST(MjProjectStoreTest, storeLockPosixReleaseVerifiesOwnershipAndSparesPreemptorsLock)
{
#ifndef _WIN32
    const auto        dir    = freshTempDir("project_store_c5_posix_release.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto lockPath = dir / kProjectStoreLockName;

    // 本端持锁（句柄 = 本人独占创建的那个 inode）。
    auto acquired = acquireStoreLock(refOf(dirStr));
    ASSERT_FALSE(acquired.is_err()) << std::move(acquired).unwrap_err().message.to_std_string();
    auto lock = std::move(acquired).unwrap();

    // 手造并发抢占者（R64 处方的 inode 错位 fixture）：POSIX unlink 不受阻
    // ——外部删掉本人锁名、同路径落新内容（新 inode），即 stealLock 抢占者
    // 已把新锁放上路径的确定性模拟。
    std::error_code ec;
    std::filesystem::remove(lockPath, ec);
    ASSERT_FALSE(ec) << ec.message();
    const std::string preemptors = writeLockOrDie(
        dir, lockJson(currentProcessPid(), "2026-04-04T00:00:00Z", "preemptor"));

    // 释放：句柄 inode ≠ 路径现指 inode → 绝不删（与 Windows 内容比对对称），
    // 转诊断 "no longer ours … left untouched"。
    lock.release();
    EXPECT_FALSE(lock.holdsLock());
    ASSERT_TRUE(std::filesystem::is_regular_file(lockPath));
    EXPECT_EQ(preemptors, readFileOrDie(lockPath));   // 抢占者的新锁逐字节未动
    const std::string diag = joinDiagnostics(lock.diagnostics());
    EXPECT_NE(std::string::npos, diag.find("no longer ours"));
    EXPECT_NE(std::string::npos, diag.find("left untouched"));

    // 清场：抢占者锁非本测试句柄产物，手动收尾（不越权替产品代码删）。
    std::filesystem::remove(lockPath, ec);
    ASSERT_FALSE(ec) << ec.message();
#else
    GTEST_SKIP() << "POSIX release ownership verification is POSIX-specific";
#endif
}

//-- C5 R64-N2：unreadable 分支不吞 stealLock 出口（无 stealLock 仍 fail-closed）--

TEST(MjProjectStoreTest, storeLockUnreadableContentKeepsStealLockExitOpen)
{
    const auto        dir    = freshTempDir("project_store_c5_unreadable.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());
    const auto lockPath = dir / kProjectStoreLockName;
    const std::string ghost = writeLockOrDie(
        dir, lockJson(currentProcessPid(), "2026-05-05T00:00:00Z", "unreadable-holder"));

#ifdef _WIN32
    // 独占无共享句柄（deny read+write）→ 产品侧读锁字节撞 sharing violation
    // = "present but unreadable"（读得出字节即非本用例形态）。
    FILE* blocker = _fsopen(lockPath.string().c_str(), "r", _SH_DENYRW);
    ASSERT_NE(nullptr, blocker);
#else
    // chmod 000 → 非 root 读必 EACCES；root 读得穿则形态造不出，skip（§11
    // 平台注意：不断言做不到的事）。unlink/重建只看目录写权，不受影响。
    ASSERT_EQ(0, ::chmod(lockPath.c_str(), 0));
#endif

    // 夹具自证：此刻读锁字节必须失败（否则后续断言测的不是 unreadable 支）。
    auto probe = ca::fs::FileUtil::read_all_bytes(lockPath.string());
    if (!probe.is_err()) {
#ifdef _WIN32
        std::fclose(blocker);
#endif
        GTEST_SKIP() << "fixture did not produce an unreadable lock (read succeeded)";
    }

    // 无 stealLock → unreadable 分支按活锁 fail-closed（R45/P5 契约不变）：
    // Lock 错注明 unreadable 并指路 stealLock，文件不动。
    auto refused = acquireStoreLock(refOf(dirStr));
    ASSERT_TRUE(refused.is_err());
    const auto  refusedError = std::move(refused).unwrap_err();
    EXPECT_EQ(ProjectStoreErrorKind::Lock, refusedError.kind);
    const std::string refusedMessage = refusedError.message.to_std_string();
    EXPECT_NE(std::string::npos, refusedMessage.find("unreadable"));
    EXPECT_NE(std::string::npos, refusedMessage.find("stealLock"));
    EXPECT_TRUE(std::filesystem::is_regular_file(lockPath));

#ifdef _WIN32
    // stealLock → R64-N2：unreadable 分支不再吞出口——越过此门进入接管面。
    // 本夹具句柄同时阻断删除（CRT 句柄无 FILE_SHARE_DELETE），接管在删旧步
    // fail-closed 为 Io（而非 Lock-unreadable 拒绝）= 门已放行的直接证据；
    // 旧 owner 措辞如实注明读不出（不虚构三元组）。
    auto preempting = acquireStoreLock(refOf(dirStr), StoreLockOptions{true});
    ASSERT_TRUE(preempting.is_err());
    const auto        preemptError = std::move(preempting).unwrap_err();
    const std::string preemptMessage = preemptError.message.to_std_string();
    EXPECT_EQ(ProjectStoreErrorKind::Io, preemptError.kind);
    EXPECT_EQ(std::string::npos, preemptMessage.find("present but unreadable"));
    EXPECT_NE(std::string::npos, preemptMessage.find("failed to remove store.lock"));
    EXPECT_NE(std::string::npos, preemptMessage.find("unreadable content"));
    // 删除被阻 → 原字节未动（解除阻断后核）。
    std::fclose(blocker);
    EXPECT_EQ(ghost, readFileOrDie(lockPath));
#else
    // POSIX：chmod 000 只挡读不挡删/建 → stealLock 全路径走通：删旧 + 独占
    // 重试成功，锁诊断记录抢占事实并注明读不出（§7.1，不虚构 owner 三元组）。
    auto preempted = acquireStoreLock(refOf(dirStr), StoreLockOptions{true});
    ASSERT_FALSE(preempted.is_err())
        << std::move(preempted).unwrap_err().message.to_std_string();
    auto lock = std::move(preempted).unwrap();
    const std::string diag = joinDiagnostics(lock.diagnostics());
    EXPECT_NE(std::string::npos, diag.find("stealLock"));
    EXPECT_NE(std::string::npos, diag.find("unreadable content"));
    const std::string lockBytes = readFileOrDie(lockPath);
    EXPECT_NE(std::string::npos, lockBytes.find("\"pid\":" + std::to_string(currentProcessPid())));
    EXPECT_NE(ghost, lockBytes);   // 旧 ghost 字节已不在盘上
    lock.release();
    EXPECT_FALSE(std::filesystem::exists(lockPath));
#endif
}

//-- R62 N1：flush 后孤儿 prune 动作面（工作清单现扫现验）-----------------------

/// 收容条目构造（§9.3 形态；formatTag 原样携带，hash/size 由调用方给定）。
StoreSectionEntry makeRetainedEntry(const char* name, const std::string& bytes)
{
    StoreSectionEntry entry;
    entry.name           = Utf8String::from_cstr(name);
    entry.format         = SectionFormat::Opaque;
    entry.formatTag      = Utf8String::from_cstr("cbor");
    entry.formatRetained = true;
    entry.sectionVersion = 2;
    entry.contentHash    = makeStr(independentHash(bytes));
    entry.byteSize       = bytes.size();
    entry.producedBy     = Utf8String::from_cstr("future-engine/9");
    entry.createdAt      = Utf8String::from_cstr("t");
    entry.updatedAt      = Utf8String::from_cstr("t");
    return entry;
}

TEST(MjProjectStoreTest, pruneOrphansRemovesAfterFirstFlushOnlyAndNeverZeroTouch)
{
    const auto        dir    = freshTempDir("project_store_r62_prune_basic.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string aliasBytes = "{\"id\":1}\n";
    const auto aliasWritten =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, aliasBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, aliasWritten, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());

    // 现存三态：extra.jsonl（真实扫描 → AfterFirstFlush）、extra2.jsonl（扫描
    // 后整行降档 Never——文件在盘 + Never 政策，执行面必须按行政策零触碰）、
    // absent-since-scan.jsonl（扫描后已消失的幂等形态，手植 AFF 行）。
    const std::string extra1 = "{}\n";
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "extra.jsonl").string(), extra1).is_err());
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "extra2.jsonl").string(), extra1).is_err());
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err()) << std::move(scanned).unwrap_err().message.to_std_string();
    auto report = std::move(scanned).unwrap();
    ASSERT_EQ(2U, report.orphans.size());
    EXPECT_EQ("extra.jsonl", report.orphans[0].fileName.to_std_string());
    EXPECT_EQ("extra2.jsonl", report.orphans[1].fileName.to_std_string());
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, report.orphans[0].prunePolicy);
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, report.orphans[1].prunePolicy);
    report.orphans[1].prunePolicy = OrphanPrunePolicy::Never;   // 降档 = 手植 Never 行

    StoreOrphanRow neverPhantom;
    neverPhantom.fileName    = makeStr("phantom.bin");
    neverPhantom.prunePolicy = OrphanPrunePolicy::Never;
    StoreOrphanRow affAbsent;
    affAbsent.fileName    = makeStr("absent-since-scan.jsonl");
    affAbsent.prunePolicy = OrphanPrunePolicy::AfterFirstFlush;
    report.orphans.push_back(std::move(neverPhantom));
    report.orphans.push_back(std::move(affAbsent));

    // 手植 store.lock：prune 动作面对锁零触碰（C5 边界延伸到执行面）。
    const std::string lockBytes =
        writeLockOrDie(dir, lockJson(currentProcessPid(), "2026-01-01T00:00:00Z", "live-holder"));

    const auto pruned = pruneOrphanSections(refOf(dirStr), report);

    // AfterFirstFlush 档两行：一行真删、一行幂等（已缺 = 视为已删，诊断注明）。
    ASSERT_EQ(2U, pruned.pruned.size());
    EXPECT_EQ("absent-since-scan.jsonl", pruned.pruned[0].to_std_string());
    EXPECT_EQ("extra.jsonl", pruned.pruned[1].to_std_string());
    EXPECT_FALSE(std::filesystem::exists(dir / "sections" / "absent-since-scan.jsonl"));
    EXPECT_FALSE(std::filesystem::exists(dir / "sections" / "extra.jsonl"));
    ASSERT_EQ(1U, pruned.diagnostics.size());
    EXPECT_NE(std::string::npos, pruned.diagnostics[0].to_std_string().find("already absent"));

    // Never 档零触碰：在盘的 Never 文件原样、无一行诊断提及任何 Never 行。
    EXPECT_TRUE(pruned.skipped.empty());
    EXPECT_TRUE(std::filesystem::exists(dir / "sections" / "extra2.jsonl"));
    const std::string allDiagnostics = joinDiagnostics(pruned.diagnostics);
    EXPECT_EQ(std::string::npos, allDiagnostics.find("extra2.jsonl"));
    EXPECT_EQ(std::string::npos, allDiagnostics.find("phantom.bin"));

    // 登记文件与 store.lock 原样（登记字节永不自动删；锁不在工作清单）。
    EXPECT_EQ(aliasBytes, readFileOrDie(dir / "sections" / "facts.alias.jsonl"));
    EXPECT_EQ(lockBytes, readFileOrDie(dir / kProjectStoreLockName));

    // 空工作清单 = 零动作返回（连 sections/ 存在性都不探测的口径）。
    StoreRecoveryReport emptyReport;
    const auto nothing = pruneOrphanSections(refOf(dirStr), emptyReport);
    EXPECT_TRUE(nothing.pruned.empty());
    EXPECT_TRUE(nothing.skipped.empty());
    EXPECT_TRUE(nothing.diagnostics.empty());
}

TEST(MjProjectStoreTest, pruneOrphansSkipsUnverifiablePathsWithDiagnostic)
{
    const auto        dir    = freshTempDir("project_store_r62_prune_ec.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    // "不可 exists 的路径形态"注入：a.file 是普通文件，穿它的路径形态在
    // POSIX 上 stat 得 ENOTDIR（exists 的 ec 非空）；Windows 上同形态被
    // Win32 归一为 PATH_NOT_FOUND（实测 winerror=3，落入 STL not-found 集、
    // ec 为空）——两平台各自的现验分支都真跑真断（编译期分叉）。
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "sections" / "a.file").string(), "x").is_err());
    std::error_code dirEc;
    std::filesystem::create_directories(dir / "sections" / "a.dir", dirEc);
    ASSERT_FALSE(dirEc);

    StoreRecoveryReport report;
    StoreOrphanRow throughFile;
    throughFile.fileName    = makeStr("a.file/inner.jsonl");
    throughFile.prunePolicy = OrphanPrunePolicy::AfterFirstFlush;
    StoreOrphanRow directory;
    directory.fileName    = makeStr("a.dir");
    directory.prunePolicy = OrphanPrunePolicy::AfterFirstFlush;
    StoreOrphanRow neverUnderFile;
    neverUnderFile.fileName    = makeStr("a.file/never.bin");
    neverUnderFile.prunePolicy = OrphanPrunePolicy::Never;
    report.orphans.push_back(std::move(throughFile));
    report.orphans.push_back(std::move(directory));
    report.orphans.push_back(std::move(neverUnderFile));

    const auto pruned = pruneOrphanSections(refOf(dirStr), report);

    // 非普通文件（目录）：跳过 + 不触碰（§8.2 盘点面对齐；remove 绝不递归）。
    EXPECT_TRUE(std::filesystem::is_directory(dir / "sections" / "a.dir"));
    EXPECT_TRUE(std::filesystem::exists(dir / "sections" / "a.file"));
    const std::string allDiagnostics = joinDiagnostics(pruned.diagnostics);
    EXPECT_NE(std::string::npos, allDiagnostics.find("sections"));
    EXPECT_NE(std::string::npos, allDiagnostics.find("a.dir"));
    EXPECT_NE(std::string::npos, allDiagnostics.find("not a regular file"));

#ifdef _WIN32
    // Windows：exists = false 且 ec 为空（not-found 集）→ 幂等"已缺视为已删"
    // 档；诊断带路径与形态说明。skipped 恰一行（目录）。
    ASSERT_EQ(1U, pruned.skipped.size());
    EXPECT_EQ("a.dir", pruned.skipped[0].to_std_string());
    ASSERT_EQ(1U, pruned.pruned.size());
    EXPECT_EQ("a.file/inner.jsonl", pruned.pruned[0].to_std_string());
    EXPECT_NE(std::string::npos, allDiagnostics.find("a.file/inner.jsonl"));
    EXPECT_NE(std::string::npos, allDiagnostics.find("already absent"));
#else
    // POSIX：ENOTDIR → exists 的 ec 非空 → 带病跳过（R62 病灶的执行面收口：
    // "existence check failed" 分支真跑），绝不带病删。
    EXPECT_TRUE(pruned.pruned.empty());
    ASSERT_EQ(2U, pruned.skipped.size());
    EXPECT_EQ("a.dir", pruned.skipped[0].to_std_string());
    EXPECT_EQ("a.file/inner.jsonl", pruned.skipped[1].to_std_string());
    EXPECT_NE(std::string::npos, allDiagnostics.find("a.file/inner.jsonl"));
    EXPECT_NE(std::string::npos, allDiagnostics.find("existence check failed"));
#endif

    // Never 行零提及。
    EXPECT_EQ(std::string::npos, allDiagnostics.find("never.bin"));

    // sections/ 此刻缺失/不可验证 → 全部跳过（无"已 prune 过"的可靠基态，
    // 不谎报幂等已删）。
    std::error_code rmEc;
    std::filesystem::remove_all(dir / "sections", rmEc);
    ASSERT_FALSE(rmEc);
    StoreRecoveryReport staleReport;
    StoreOrphanRow stale;
    stale.fileName    = makeStr("gone.jsonl");
    stale.prunePolicy = OrphanPrunePolicy::AfterFirstFlush;
    staleReport.orphans.push_back(std::move(stale));
    const auto refused = pruneOrphanSections(refOf(dirStr), staleReport);
    EXPECT_TRUE(refused.pruned.empty());
    ASSERT_EQ(1U, refused.skipped.size());
    EXPECT_EQ("gone.jsonl", refused.skipped[0].to_std_string());
    const std::string refusedDiag = joinDiagnostics(refused.diagnostics);
    EXPECT_NE(std::string::npos, refusedDiag.find("unverifiable at prune time"));
    EXPECT_NE(std::string::npos, refusedDiag.find("gone.jsonl"));
}

TEST(MjProjectStoreTest, pruneOrphansRefusesAllWhileManifestUnreadableNow)
{
    const auto        dir    = freshTempDir("project_store_r62_prune_manifest.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string extra = "{}\n";
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "extra.jsonl").string(), extra).is_err());
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err());
    auto report = std::move(scanned).unwrap();
    ASSERT_EQ(1U, report.orphans.size());
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, report.orphans[0].prunePolicy);

    // 扫描后清单坏掉（现扫现验第 1 环）：此刻读不出清单 = 零可删（P1 红线）。
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "store.json").string(), "{ not json ").is_err());
    const auto pruned = pruneOrphanSections(refOf(dirStr), report);

    EXPECT_TRUE(pruned.pruned.empty());
    ASSERT_EQ(1U, pruned.skipped.size());
    EXPECT_EQ("extra.jsonl", pruned.skipped[0].to_std_string());
    const std::string diag = joinDiagnostics(pruned.diagnostics);
    EXPECT_NE(std::string::npos, diag.find("manifest unreadable at prune time"));
    EXPECT_NE(std::string::npos, diag.find("P1 red line"));
    EXPECT_NE(std::string::npos, diag.find("extra.jsonl"));
    // 文件原样保留（绝不带病删的物理面）。
    EXPECT_EQ(extra, readFileOrDie(dir / "sections" / "extra.jsonl"));
}

TEST(MjProjectStoreTest, pruneOrphansSkipsFilesNowClaimedByCurrentManifest)
{
    const auto        dir    = freshTempDir("project_store_r62_prune_claim.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string aliasBytes = "{\"id\":1}\n";
    const auto aliasWritten =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, aliasBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, aliasWritten, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());

    // 扫描时 extra.jsonl 是孤儿（清单可载档）；扫描到执行之间它被登记——
    // 陈旧工作清单的每一笔都必须被当前清单的认领面拦下（R62 N1 病灶闭合：
    // 认领处 ec 曾被忽略，登记文件可能误落 AfterFirstFlush 档，执行面是
    // 最后防线）。
    const std::string extraBytes = "{}\n";
    ASSERT_FALSE(
        ca::fs::FileUtil::write_text((dir / "sections" / "extra.jsonl").string(), extraBytes).is_err());
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err());
    auto staleReport = std::move(scanned).unwrap();
    ASSERT_EQ(1U, staleReport.orphans.size());
    EXPECT_EQ("extra.jsonl", staleReport.orphans[0].fileName.to_std_string());

    const auto extraWritten =
        writeSectionOrDie(dirStr, "extra", SectionFormat::Jsonl, extraBytes);   // 同字节幂等发布
    StoreManifestMutation lateClaim;
    lateClaim.upserts.push_back(entryFrom("extra", SectionFormat::Jsonl, extraWritten, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), lateClaim).is_err());

    const auto pruned = pruneOrphanSections(refOf(dirStr), staleReport);

    EXPECT_TRUE(pruned.pruned.empty());
    ASSERT_EQ(1U, pruned.skipped.size());
    EXPECT_EQ("extra.jsonl", pruned.skipped[0].to_std_string());
    const std::string diag = joinDiagnostics(pruned.diagnostics);
    EXPECT_NE(std::string::npos, diag.find("claimed by the current manifest now"));
    EXPECT_NE(std::string::npos, diag.find("extra.jsonl"));
    EXPECT_EQ(extraBytes, readFileOrDie(dir / "sections" / "extra.jsonl"));
}

TEST(MjProjectStoreTest, pruneOrphansCaseVariantClaimWindowsInsensitivePosixSensitive)
{
    const auto        dir    = freshTempDir("project_store_r62_prune_case.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string alphaBytes = "{\"a\":1}\n";
    const auto alphaWritten =
        writeSectionOrDie(dirStr, "facts.alpha", SectionFormat::Jsonl, alphaBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alpha", SectionFormat::Jsonl, alphaWritten, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());

#ifdef _WIN32
    // Windows：登记文件被人工改名成大小写变体（同一文件，Win32 路径语义
    // CI 解析）→ 扫描的认领集是登记名、孤儿面按精确串比对比中变体名（报告
    // 层误挂 AfterFirstFlush 的现症形态）→ prune 执行面的认领现验按 Win32
    // 同款大小写不敏感比中登记名，**拒绝删除**（N1×N2 的闭合面）。
    std::error_code renameEc;
    std::filesystem::rename(dir / "sections" / "facts.alpha.jsonl",
                            dir / "sections" / "Facts.Alpha.Jsonl", renameEc);
    ASSERT_FALSE(renameEc) << renameEc.message();
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err());
    auto report = std::move(scanned).unwrap();
    ASSERT_EQ(1U, report.orphans.size());
    EXPECT_EQ("Facts.Alpha.Jsonl", report.orphans[0].fileName.to_std_string());
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, report.orphans[0].prunePolicy);

    const auto pruned = pruneOrphanSections(refOf(dirStr), report);
    EXPECT_TRUE(pruned.pruned.empty());
    ASSERT_EQ(1U, pruned.skipped.size());
    EXPECT_EQ("Facts.Alpha.Jsonl", pruned.skipped[0].to_std_string());
    const std::string diag = joinDiagnostics(pruned.diagnostics);
    EXPECT_NE(std::string::npos, diag.find("claimed by the current manifest now"));
    // 同一文件（变体名解析回登记节）字节原样——登记字节永不自动删。
    EXPECT_EQ(alphaBytes, readFileOrDie(dir / "sections" / "Facts.Alpha.Jsonl"));
#else
    // POSIX：大小写敏感文件系统上变体名是**不同文件**——真孤儿照常 prune
    // （分叉的另一侧：POSIX 语义不得被 Windows 折叠污染），登记文件原样。
    const std::string junkBytes = "junk\n";
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "sections" / "Facts.Alpha.Jsonl").string(),
                                              junkBytes).is_err());
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err());
    auto report = std::move(scanned).unwrap();
    ASSERT_EQ(1U, report.orphans.size());
    EXPECT_EQ("Facts.Alpha.Jsonl", report.orphans[0].fileName.to_std_string());
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, report.orphans[0].prunePolicy);
    ASSERT_EQ(1U, report.okSections.size());
    EXPECT_EQ("facts.alpha", report.okSections[0].name.to_std_string());

    const auto pruned = pruneOrphanSections(refOf(dirStr), report);
    ASSERT_EQ(1U, pruned.pruned.size());
    EXPECT_EQ("Facts.Alpha.Jsonl", pruned.pruned[0].to_std_string());
    EXPECT_TRUE(pruned.skipped.empty());
    EXPECT_EQ(alphaBytes, readFileOrDie(dir / "sections" / "facts.alpha.jsonl"));
#endif
}

//-- R62 N2：收容词根匹配平台分叉（Windows 不敏感 / POSIX 敏感）双平台钉 --------

TEST(MjProjectStoreTest, recoveryScanRetainedStemCaseVariantTwoStateFork)
{
    const auto        dir    = freshTempDir("project_store_r62_stem_case.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string aliasBytes = "{\"id\":1}\n";
    const auto aliasWritten =
        writeSectionOrDie(dirStr, "facts.alias", SectionFormat::Jsonl, aliasBytes);
    StoreManifestMutation setup;
    setup.upserts.push_back(entryFrom("facts.alias", SectionFormat::Jsonl, aliasWritten, 1));
    ASSERT_FALSE(commitStoreManifestTransaction(refOf(dirStr), setup).is_err());

    // 收容条目 + 磁上大小写变体文件（与登记字节相同——Windows 侧若被认领
    // 即应受验为 Ok；POSIX 侧不认领 → 收容条目 Damaged + 变体文件真孤儿）。
    const std::string retainedBytes = "future-bytes\n";
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "sections" / "Future.Thing.Cbor").string(),
                                              retainedBytes).is_err());
    auto loaded = readStoreManifestFile(refOf(dirStr));
    ASSERT_FALSE(loaded.is_err());
    auto manifest = std::move(loaded).unwrap();
    manifest.sections.push_back(makeRetainedEntry("future.thing", retainedBytes));
    auto text = writeStoreManifestText(manifest);
    ASSERT_FALSE(text.is_err());
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(),
                                              std::move(text).unwrap().to_std_string()).is_err());

    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err()) << std::move(scanned).unwrap_err().message.to_std_string();
    auto report = std::move(scanned).unwrap();
    EXPECT_EQ(ManifestLoadState::Loadable, report.manifestState);
    EXPECT_TRUE(findEntry(report.okSections, "facts.alias") != nullptr);

#ifdef _WIN32
    // Windows：词根匹配大小写不敏感（对齐 Win32/\\?\ 路径语义）→ 变体名被
    // 收容认领、受验为 Ok——人工大小写变体不得绕过认领（R62 N2 病灶闭合），
    // 也绝不落入 prune 档。
    const auto* retainedRow = findEntry(report.okSections, "future.thing");
    ASSERT_NE(nullptr, retainedRow);
    EXPECT_TRUE(retainedRow->formatRetained);
    EXPECT_EQ("cbor", retainedRow->formatTag.to_std_string());
    EXPECT_TRUE(report.orphans.empty());
    EXPECT_TRUE(report.damagedSections.empty());
#else
    // POSIX：大小写敏感语义不变——变体名不匹配词根：收容条目 Damaged（无
    // 匹配文件），变体文件是清单外真孤儿（AfterFirstFlush 档）。
    ASSERT_EQ(1U, report.damagedSections.size());
    EXPECT_EQ("future.thing", report.damagedSections[0].name.to_std_string());
    ASSERT_EQ(1U, report.orphans.size());
    EXPECT_EQ("Future.Thing.Cbor", report.orphans[0].fileName.to_std_string());
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, report.orphans[0].prunePolicy);
#endif
}

TEST(MjProjectStoreTest, recoveryScanPosixCaseSensitiveStemCoexistence)
{
#ifdef _WIN32
    GTEST_SKIP() << "case-differing coexistence needs a POSIX filesystem; the Windows "
                    "folding side is pinned by recoveryScanRetainedStemCaseVariantTwoStateFork";
#else
    const auto        dir    = freshTempDir("project_store_r62_stem_coexist.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(createBundleSkeleton(refOf(dirStr)).is_err());

    const std::string retainedBytes = "future-bytes\n";
    // 两份大小写互异文件并存（POSIX 专属形态）：真收容字节 + 变体垃圾。
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "sections" / "future.thing.cbor").string(),
                                              retainedBytes).is_err());
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "sections" / "Future.Thing.Cbor").string(),
                                              "junk\n").is_err());
    auto loaded = readStoreManifestFile(refOf(dirStr));
    ASSERT_FALSE(loaded.is_err());
    auto manifest = std::move(loaded).unwrap();
    manifest.sections.push_back(makeRetainedEntry("future.thing", retainedBytes));
    auto text = writeStoreManifestText(manifest);
    ASSERT_FALSE(text.is_err());
    ASSERT_FALSE(ca::fs::FileUtil::write_text((dir / "store.json").string(),
                                              std::move(text).unwrap().to_std_string()).is_err());

    // 词根匹配敏感（R62/N2 POSIX 分叉实证）：变体垃圾名与收容词根
    // "future.thing." 是不同文件——不进候选面（candidates 仅真收容字节，
    // 受验通过 → 0 Damaged）；变体名无人认领 → 落一般孤儿面（清单可载 →
    // AfterFirstFlush 档）。"首候选失配 Damaged + 变体 Never"是多匹配
    // 语义（Windows 不敏感分叉）的推演，POSIX 敏感面下不成立。
    auto scanned = scanBundleRecovery(refOf(dirStr));
    ASSERT_FALSE(scanned.is_err()) << std::move(scanned).unwrap_err().message.to_std_string();
    auto report = std::move(scanned).unwrap();
    ASSERT_EQ(1U, report.okSections.size());
    EXPECT_EQ("future.thing", report.okSections[0].name.to_std_string());
    EXPECT_TRUE(report.damagedSections.empty());
    ASSERT_EQ(1U, report.orphans.size());
    EXPECT_EQ("Future.Thing.Cbor", report.orphans[0].fileName.to_std_string());
    EXPECT_EQ(OrphanPrunePolicy::AfterFirstFlush, report.orphans[0].prunePolicy);
#endif
}
