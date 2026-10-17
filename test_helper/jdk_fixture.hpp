// jdk_fixture.hpp — 测试侧 JDK 工具链辅助
//
// 在 libs/libjdk_finder 的探测能力之上，提供测试专用功能：
//   - runBat：临时脚本执行（Windows .bat / POSIX sh，失败立即早退）
//   - compileJava：javac 编译（带 -encoding UTF-8）
//   - runJavaMain：java 运行 main 类
//   - probeClang：clang 探测（PATH 优先 + 常见安装位置，#1122 可见化）
//
// JDK/MSVC 探测本身（JAVA_HOME 解析、vcvarsall 定位等）已在 libjdk_finder，
// 产品代码（mj2x-cli --compile）与测试代码共用同一份探测，本文件不再重复。
//
// 供 mtest_set / mbase / mj2x 等所有需要 "javac 编译 + java 运行" 的测试复用。

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "jdk_finder/jdk_finder.hpp"
#include "test_helper.hpp"

namespace test::jdk {

namespace fs = std::filesystem;

/// 子进程执行结果：exitCode + 合并的 stdout/stderr。
struct ExecResult
{
    int         exitCode = -1;
    std::string out;   // 合并的 stdout+stderr
};

/// 带错误码的安全存在性检查（避免异常）。
inline bool fileExists(const fs::path& p)
{
    std::error_code ec;
    return fs::exists(p, ec);
}

/// 路径转当前 shell 可安全使用的引用字符串。
inline std::string q(const fs::path& p)
{
    std::string s = p.string();
#ifdef _WIN32
    for (auto& c : s)
        if (c == '/') c = '\\';
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
#endif
}

/// 通过临时脚本执行命令。Windows 使用 .bat 绕开 cmd /C 引号解析坑，POSIX
/// 使用 sh；每行失败后立即以该退出码终止。
/// 返回合并的 stdout+stderr 和退出码。
inline ExecResult runBat(const std::vector<std::string>& lines)
{
    ExecResult r;
    // 临时脚本放到 test/tmp，避免 cwd 影响。
    fs::path tmpDir = test::getTempOutPath("jdk_fixture_runbat.keep").parent_path();
#ifdef _WIN32
    constexpr const char* kScriptExtension = ".bat";
#else
    constexpr const char* kScriptExtension = ".sh";
#endif
    fs::path script = tmpDir / ("jdk_fixture_" +
                                std::to_string(std::hash<std::thread::id>{}(
                                    std::this_thread::get_id())) +
                                "_" + std::to_string(std::rand()) + kScriptExtension);
    {
        std::ofstream f(script);
        if (!f) {
            r.out = "[jdk_fixture] cannot create temporary script: " + script.string();
            return r;
        }
#ifdef _WIN32
        f << "@echo off\n";
        f << "chcp 65001 >nul 2>&1\n";   // UTF-8 输出，避免中文系统 GBK 乱码
        for (auto& l : lines) {
            f << l << "\n";
            // 每行后早退：失败立即终止，避免后续行误判成功。
            f << "if errorlevel 1 exit /b %errorlevel%\n";
        }
        f << "exit /b %errorlevel%\n";
#else
        f << "#!/bin/sh\n";
        for (const auto& l : lines) {
            f << l << "\n";
            f << "status=$?\n";
            f << "if [ $status -ne 0 ]; then exit $status; fi\n";
        }
        f << "exit 0\n";
#endif
    }
    std::string full =
#ifdef _WIN32
        q(script) + " 2>&1";
#else
        "sh " + q(script) + " 2>&1";
#endif
    FILE*       pipe =
#ifdef _WIN32
        _popen(full.c_str(), "rb");
#else
        popen(full.c_str(), "r");
#endif
    if (!pipe) {
        r.out = "[jdk_fixture] popen failed for: " + full;
        std::error_code ec;
        fs::remove(script, ec);
        return r;
    }
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), pipe)) r.out.append(buf, n);
#ifdef _WIN32
    r.exitCode = _pclose(pipe);
#else
    int status = pclose(pipe);
    if (status == -1) r.exitCode = -1;
    else if (WIFEXITED(status)) r.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.exitCode = 128 + WTERMSIG(status);
    else r.exitCode = status;
#endif
    std::error_code ec;
    fs::remove(script, ec);
    return r;
}

/// 测试侧 JDK 工具链：在 jdk_finder::JdkLocation 之上补 javac/java 可执行路径，
/// 供 compileJava / runJavaMain 使用。available=false 时 reason 填说明。
struct JdkToolchain
{
    bool                       available = false;
    std::string                reason;
    fs::path                   javaHome;
    fs::path                   javac;          // <home>/bin/javac(.exe)
    fs::path                   java;           // <home>/bin/java(.exe)
    fs::path                   jdkInclude;     // <home>/include（native 测试可校验 jni.h）
    fs::path                   jdkIncludeWin;  // <home>/include/win32
};

/// probeJdk 的可选配置（透传给 jdk_finder::findJdk）。
struct ProbeOptions
{
    /// JAVA_HOME / MORPHER_JDK_HOME 都没命中时的额外回落候选。
    std::vector<fs::path> fallbackHomes;
    /// 是否要求 jni.h 存在（native 编译测试用）。默认 false。
    bool requireJniHeader = false;
};

/// 探测 JDK 并解析 javac/java 路径。底层委托 jdk_finder::findJdk。
inline JdkToolchain probeJdk(const ProbeOptions& opts = {})
{
    JdkToolchain tc;
    jdk_finder::FindOptions fopts;
    fopts.fallbackHomes      = opts.fallbackHomes;
    fopts.requireJniHeader   = opts.requireJniHeader;
    auto loc = jdk_finder::findJdk(fopts);
    tc.javaHome      = loc.javaHome;
    tc.jdkInclude    = loc.jdkInclude;
    tc.jdkIncludeWin = loc.jdkIncludePlat;
    tc.reason        = loc.reason;
    if (!loc.available) return tc;
    tc.javac = jdk_finder::jdkExecutable(loc, "javac");
    tc.java  = jdk_finder::jdkExecutable(loc, "java");
    if (tc.javac.empty() || tc.java.empty()) {
        tc.available = false;
        tc.reason    = "javac 或 java 不存在: " + tc.javaHome.string();
        return tc;
    }
    tc.available = true;
    return tc;
}

// ---------------------------------------------------------------------------
// clang 探测（#1122）：llvm exec 列（native matrix LLVM 列 / llvm 样本列）的
// GTEST_SKIP 依赖 clang，此前只认 PATH 命中——无 clang 机器整列跳过且不可见，
// 缺口长期隐形。本探测把"无 clang"的口径从"PATH 无"扩为"PATH 与常见安装位置
// 均无"，并把探测结果以单行摘要打进测试输出（找到/未找到 + 版本 + 来源），
// 让 SKIP 从隐形变可见且有因。
//
// 语义纪律：探测不改变 llvm 列的跳过条件本身——仍是"无 clang"，只是查找面
// 变宽；真无 clang 的机器（CI runner）行为与基线逐字一致。
//
// 查找顺序：
//   1. PATH（`clang --version` 可执行即命中，source=PATH）；
//   2. MORPHER_CLANG_HOME 显式覆盖（指向 clang.exe 或其所在/bin 目录）；
//   3. 常见安装位置（Windows：LLVM 标准安装、MATLAB 自带工具链
//      <ProgramFiles>/MATLAB/R*/toolbox/sldrt/clang/win64/clang.exe，多版本取
//      字典序最大；POSIX：/usr/local/bin、Homebrew llvm）逐候选 `--version` 验证。
// MORPHER_DISABLE_CLANG_PROBE=1 可整体禁用（回落"仅 PATH 且视为未找到"），
// 供门禁取"未启用 clang"基线或排障时使用。
// ---------------------------------------------------------------------------

/// clang 探测结果。available=false 时 reason 填说明（供 SKIP 消息引用）。
struct ClangProbe
{
    bool        available = false;
    std::string reason;      // 未命中时的说明
    fs::path    clangPath;   // 命中的 clang 可执行文件（PATH 命中时为空，使用时仍走 PATH）
    std::string version;     // `--version` 输出首行（如 "clang version 16.0.6"）
    std::string source;      // 命中来源：PATH / MORPHER_CLANG_HOME / LLVM install / MATLAB bundled
};

namespace detail {

/// 取输出首行（去 \r；空输出回退 "unknown"）。
inline std::string firstLine(const std::string& out)
{
    size_t end = out.find('\n');
    std::string line = (end == std::string::npos) ? out : out.substr(0, end);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line.empty() ? "unknown" : line;
}

/// 逐候选验证：候选须为常规文件且 `<exe> --version` 退出码 0，即命中。
inline bool verifyClangCandidate(const fs::path& exe, const char* source, ClangProbe& probe)
{
    std::error_code ec;
    if (exe.empty() || !fs::is_regular_file(exe, ec)) return false;
    const auto r = runBat({q(exe) + " --version"});
    if (r.exitCode != 0) return false;
    probe.available = true;
    probe.clangPath = exe;
    probe.version   = firstLine(r.out);
    probe.source    = source;
    return true;
}

}   // namespace detail

inline ClangProbe probeClang()
{
    ClangProbe probe;

    // 0. 显式禁用开关：门禁取"未启用 clang"基线 / 排障用。
    if (const char* disabled = std::getenv("MORPHER_DISABLE_CLANG_PROBE");
        disabled != nullptr && disabled[0] != '\0' && std::string(disabled) != "0") {
        probe.reason = "disabled by MORPHER_DISABLE_CLANG_PROBE=1";
        return probe;
    }

    // 1. PATH：既有跳过条件的原始口径，保持最高优先。未命中不在此处定稿
    //    reason——全途径收口见函数尾（中途定稿会使收口分支不可达）。
    {
        const auto r = runBat({q("clang") + " --version"});
        if (r.exitCode == 0) {
            probe.available = true;
            probe.version   = detail::firstLine(r.out);
            probe.source    = "PATH";
            return probe;
        }
    }

    // 2. MORPHER_CLANG_HOME 显式覆盖：指向 clang 可执行文件或其所在/bin 目录。
    bool homeProbed = false;
    if (const char* home = std::getenv("MORPHER_CLANG_HOME"); home != nullptr && home[0] != '\0') {
        homeProbed     = true;
        const fs::path root(home);
#ifdef _WIN32
        const std::array<fs::path, 4> candidates = {
            root / "clang.exe", root, root / "bin" / "clang.exe", root / "bin"};
#else
        const std::array<fs::path, 4> candidates = {
            root / "clang", root, root / "bin" / "clang", root / "bin"};
#endif
        for (const auto& candidate : candidates) {
            if (detail::verifyClangCandidate(candidate, "MORPHER_CLANG_HOME", probe)) return probe;
        }
    }

#ifdef _WIN32
    // 3a. LLVM 标准安装（ProgramFiles/ProgramFiles(x86) 下的 LLVM/bin）。
    {
        std::vector<fs::path> roots;
        if (const char* pf = std::getenv("ProgramFiles"); pf != nullptr && pf[0] != '\0')
            roots.emplace_back(pf);
        if (const char* pf = std::getenv("ProgramFiles(x86)"); pf != nullptr && pf[0] != '\0')
            roots.emplace_back(pf);
        for (const auto& root : roots) {
            if (detail::verifyClangCandidate(root / "LLVM" / "bin" / "clang.exe", "LLVM install",
                                             probe))
                return probe;
        }
    }
    // 3b. MATLAB 自带工具链：<ProgramFiles>/MATLAB/R*/toolbox/sldrt/clang/win64/clang.exe
    // （#879 实证 16.0.6 x86_64-pc-windows-msvc 可驱动 llvm 后端编译链；多版本取字典序最大）。
    {
        std::vector<fs::path> matlabRoots;
        for (const char* key : {"ProgramFiles", "ProgramFiles(x86)"}) {
            if (const char* pf = std::getenv(key); pf != nullptr && pf[0] != '\0')
                matlabRoots.emplace_back(pf);
        }
        for (const auto& root : matlabRoots) {
            const fs::path base = root / "MATLAB";
            std::error_code     ec;
            if (!fs::exists(base, ec)) continue;
            std::vector<fs::path> versions;
            for (const auto& entry : fs::directory_iterator(base, ec)) {
                const std::string name = entry.path().filename().string();
                if (name.rfind("R", 0) == 0) versions.push_back(entry.path());
            }
            std::sort(versions.begin(), versions.end(), std::greater<fs::path>());
            for (const auto& version : versions) {
                if (detail::verifyClangCandidate(
                        version / "toolbox" / "sldrt" / "clang" / "win64" / "clang.exe",
                        "MATLAB bundled", probe))
                    return probe;
            }
        }
    }
#else
    // 3c. POSIX 常见非 PATH 位置（Homebrew llvm / /usr/local）。
    for (const auto& candidate :
         {fs::path("/opt/homebrew/opt/llvm/bin/clang"), fs::path("/usr/local/bin/clang")}) {
        if (detail::verifyClangCandidate(candidate, "common install", probe)) return probe;
    }
#endif

    // 全途径未命中：reason 按实际探测覆盖面收口。此前 reason 停在 PATH 首步的
    // "not on PATH"、尾部兜底因 reason 恒非空不可达——SKIP 消息声称已探常见
    // 安装位置而原因只写 "not on PATH"，措辞与实际探测面不符（#1191）。
    probe.reason = "not on PATH";
    if (homeProbed) probe.reason += "; MORPHER_CLANG_HOME 未命中 clang --version";
    probe.reason += "; no common install location matched";
    return probe;
}

/// 探测结果单行摘要（打测试输出用，找到/未找到 + 版本 + 来源 + 路径）。
inline std::string formatClangProbe(const ClangProbe& probe)
{
    if (probe.available) {
        std::string line = "clang found: " + probe.version + " [source=" + probe.source + "]";
        if (!probe.clangPath.empty()) line += " [path=" + probe.clangPath.string() + "]";
        return line;
    }
    return "clang NOT found: " + probe.reason + " — llvm exec 列将整列跳过 (#1122)";
}

/// clang 命中非 PATH 位置时，给子进程脚本前置 PATH 注入行——mj2x-cli --compile
/// 与 --c-compiler/--llvm-tool clang 均按 PATH 解析 clang（jdk_finder::commandExists
/// / ca::process::Command）。PATH 命中或未找到返回空（未找到时调用方本就该 SKIP）。
inline std::vector<std::string> clangPathPrelude(const ClangProbe& probe)
{
    if (!probe.available || probe.clangPath.empty()) return {};
    const std::string dir = probe.clangPath.parent_path().string();
    if (dir.empty()) return {};
#ifdef _WIN32
    return {"set \"PATH=" + dir + ";%PATH%\""};
#else
    return {"PATH='" + dir + ":$PATH'; export PATH"};
#endif
}

/// 便捷形态：注入行 + 命令行拼成一个脚本行集（命令在前为空时等价单命令）。
inline std::vector<std::string> clangPathPrelude(const ClangProbe& probe,
                                                 const std::string& command)
{
    auto lines  = clangPathPrelude(probe);
    lines.push_back(command);
    return lines;
}

/// 编译一个 Java 源文件到指定输出目录。成功返回 true；失败把合并输出写入 *err。
/// 统一带 -encoding UTF-8（避免 GBK 系统下含中文源码失败）。
inline bool compileJava(const JdkToolchain& tc, const fs::path& javaFile,
                        const fs::path& outDir, std::string* err = nullptr)
{
    if (!tc.available) {
        if (err) *err = "JDK 不可用";
        return false;
    }
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) {
        if (err) *err = "无法创建 javac 输出目录: " + outDir.string() + ": " + ec.message();
        return false;
    }
    auto r = runBat({q(tc.javac) + " -encoding UTF-8 -d " + q(outDir) + " " + q(javaFile)});
    if (r.exitCode != 0) {
        if (err) *err = r.out;
        return false;
    }
    return true;
}

/// 运行 Java main 类，返回合并输出。args 为命令行参数（已按 cmd 规则组装好的字符串）。
inline ExecResult runJavaMain(const JdkToolchain& tc, const fs::path& classpath,
                              const std::string& mainClass,
                              const std::vector<std::string>& args = {})
{
    ExecResult r;
    if (!tc.available) {
        r.out = "JDK 不可用";
        return r;
    }
    std::string cmd = q(tc.java) + " -cp " + q(classpath) + " " + mainClass;
    for (auto& a : args) cmd += " " + a;
    return runBat({cmd});
}

// ---------------------------------------------------------------------------
// 共享 JVM 行为等价 harness：写 class(es)+Runner.java → javac → java → 抓 stdout。
//
// 收敛此前散在 4 处的同一份逻辑（mbase runJvmMainClasses、mjt-obf obf_jvm_fixture::runJvm、
// mtest_set semantic_jvm、mj2x）。层级放 test::jdk（只依赖 jdk_finder+filesystem，不碰
// libjbytecode/libjzip），供全线复用。JDK 不可用返回 nullopt，调用方 GTEST_SKIP。
// ---------------------------------------------------------------------------

/// 把控制台捕获的 CRLF 归一化为 LF，使跨平台 stdout 比对一致。
inline std::string normalizeOutput(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        out.push_back(s[i]);
    }
    return out;
}

/// 运行规格：一组命名 class bytes + Runner.main 里的调用片段 + 选项。
struct RunnerSpec
{
    /// {内部名(可含包目录，不带 .class), bytes}。单类样本常用 {"T", bytes}。
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> classes;
    std::string                     runnerBody;
    std::vector<std::string>        arguments;      ///< 传给 Runner 的命令行参数
    std::vector<std::string>        jvmArgs;        ///< 额外 JVM 参数（如 -Dmjt.literal.native=<dll>）
    std::vector<fs::path>           extraJavaSources;   ///< 额外一起编译的 .java 绝对路径（如 indy bootstrap）
    bool                            verify = true;   ///< true=-Xverify:all，false=-noverify
    std::string                     scratchPrefix = "morpher_jvm_";
};

/// 运行终态分类。此前 runWithRunner 把三种失败混同为 nullopt、被调用方
/// GTEST_SKIP 吞掉——"被测产物运行失败"（如 keychain Phase 3 key2 不自洽的首链
/// AIOOBE）与"环境无 JDK"不可同日而语：前者是必须显红的缺陷（P0 在 CI 全绿
/// 下存活两周的直接原因），后者才是合法 skip。
enum class JvmRunStatus
{
    Ok,             ///< Runner 正常退出（exit==0），output 为归一化 stdout
    JdkUnavailable, ///< 环境无可用 JDK（tc.available == false）
    CompileFailed,  ///< javac 编译失败（Runner.java 或额外源）
    RunFailed,      ///< java 运行失败（exit != 0）
};

/// runWithRunnerOutcome 的结果。
struct JvmRunOutcome
{
    JvmRunStatus status = JvmRunStatus::JdkUnavailable;
    std::string  output;     ///< Ok: 归一化 stdout；失败: 合并的诊断输出
    int          exitCode = -1;
    bool         ok() const { return status == JvmRunStatus::Ok; }
};

/// 执行 RunnerSpec，带终态分类。成功返回归一化后的 stdout；失败时 output
/// 携带关键诊断（javac 报错 / JVM 异常栈）。
inline JvmRunOutcome runWithRunnerOutcome(const JdkToolchain& tc, const RunnerSpec& spec)
{
    JvmRunOutcome r;
    if (!tc.available) {
        r.status = JvmRunStatus::JdkUnavailable;
        r.output = tc.reason;
        return r;
    }
    const fs::path dir =
        fs::temp_directory_path() /
        (spec.scratchPrefix +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto            cleanup = [&] { std::error_code ec; fs::remove_all(dir, ec); };
    std::error_code ec;
    fs::create_directories(dir, ec);

    for (const auto& [name, bytes] : spec.classes) {
        const auto classFile = dir / (name + ".class");
        fs::create_directories(classFile.parent_path(), ec);
        std::ofstream(classFile, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    const auto src = dir / "Runner.java";
    std::ofstream(src) << "public class Runner { public static void main(String[] a) { "
                       << spec.runnerBody << " } }\n";

    // 额外源与主源合并成单条 javac 命令一次编译：逐文件单独 javac 时
    // 无法解析彼此依赖（如 KeyChainKeyFactory 依赖 KeyChainKey 接口）。
    std::string extraCmd = q(tc.javac) + " -encoding UTF-8 -d " + q(dir);
    for (const auto& extra : spec.extraJavaSources) extraCmd += " " + q(fs::absolute(extra));
    if (!spec.extraJavaSources.empty()) {
        auto er = runBat({extraCmd});
        if (er.exitCode != 0) {
            r.status   = JvmRunStatus::CompileFailed;
            r.exitCode = er.exitCode;
            r.output   = er.out;
            cleanup();
            return r;
        }
    }
    auto compileResult = runBat({q(tc.javac) + " -encoding UTF-8 -cp " + q(dir) + " -d " +
                                 q(dir) + " " + q(src)});
    if (compileResult.exitCode != 0) {
        r.status   = JvmRunStatus::CompileFailed;
        r.exitCode = compileResult.exitCode;
        r.output   = compileResult.out;
        cleanup();
        return r;
    }
    // JVM 参数必须在主类之前（java <jvmArgs> -cp ... Runner <arguments>），
    // 放在主类后会被当作 Runner 的程序参数，-D 系统属性不生效。
    std::string run = q(tc.java);
    for (const auto& arg : spec.jvmArgs) run += " " + arg;
    run += std::string(spec.verify ? " -Xverify:all -cp " : " -noverify -cp ") + q(dir) +
           " Runner";
    for (const auto& arg : spec.arguments) run += " " + arg;
    auto runResult = runBat({run});
    if (runResult.exitCode != 0) {
        r.status   = JvmRunStatus::RunFailed;
        r.exitCode = runResult.exitCode;
        r.output   = runResult.out;
        cleanup();
        return r;
    }
    cleanup();
    r.status   = JvmRunStatus::Ok;
    r.exitCode = runResult.exitCode;
    r.output   = normalizeOutput(runResult.out);
    return r;
}

/// 执行 RunnerSpec，返回归一化后的 stdout。任一环节失败返回 nullopt，
/// 失败时把合并输出写入 *diagnostic（若非空）。
///
/// .. 兼容保留：终态不区分 JDK 缺失与编译/运行失败。新代码请用
/// runWithRunnerOutcome（分类）或测试侧 runRunnerChecked（SKIP/FAIL 语义），
/// 避免"产物缺陷被吞成 skip"。
inline std::optional<std::string> runWithRunner(const JdkToolchain& tc, const RunnerSpec& spec,
                                                std::string* diagnostic = nullptr)
{
    auto r = runWithRunnerOutcome(tc, spec);
    if (r.ok()) return r.output;
    if (diagnostic) *diagnostic = r.output;
    return std::nullopt;
}

/// 便捷重载：内部用 probeJdk() 的缓存工具链。
inline std::optional<std::string> runWithRunner(const RunnerSpec& spec,
                                                std::string* diagnostic = nullptr)
{
    static const auto tc = probeJdk();
    return runWithRunner(tc, spec, diagnostic);
}

}   // namespace test::jdk
