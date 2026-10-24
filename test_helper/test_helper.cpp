#include "test_helper.hpp"

#include <stdexcept>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#elif defined(__linux__)
#    include <unistd.h>
#elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#endif

// 本仓私有实现：构建产物路径（asm-test 特例定位已随 libjbytecode 拆出删除）。
// 通用机制（扫描/资源/输出根）全部由 ca::test 承担，见 test_helper.hpp。

namespace test {

std::string getBuildModeName()
{
#ifdef NDEBUG
    return "release";
#else
    return "debug";
#endif
}

namespace {

std::string build_platform()
{
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "macosx";
#else
    return "linux";
#endif
}

std::string build_arch()
{
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
#    ifdef _WIN32
    return "x64";
#    else
    return "x86_64";
#    endif
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return "unknown";
#endif
}

/// 目标产物文件名：Windows 下补 .exe；已带 .exe/.dll 后缀（shared 目标伴生产物，
/// 如 mjt-obf-runtime-native.dll）的名字原样采信，避免拼出 ".dll.exe"。
/// POSIX 下 .dll 后缀按 xmake shared 目标的平台产物名映射为 lib<base>.so /
/// lib<base>.dylib，保持「.dll 名 = shared 伴生」调用约定跨平台可用。
std::string exe_name(const std::string& targetName)
{
#ifdef _WIN32
    if (targetName.size() >= 4) {
        const auto suffix = targetName.substr(targetName.size() - 4);
        if (suffix == ".exe" || suffix == ".dll") {
            return targetName;
        }
    }
    return targetName + ".exe";
#else
    if (targetName.size() >= 4 && targetName.compare(targetName.size() - 4, 4, ".dll") == 0) {
        const auto base = targetName.substr(0, targetName.size() - 4);
#if defined(__APPLE__)
        return "lib" + base + ".dylib";
#else
        return "lib" + base + ".so";
#endif
    }
    return targetName;
#endif
}

/// 当前可执行文件绝对路径。Windows 用 GetModuleFileNameW（std::filesystem::path
/// 在 Windows 上原生宽字符存储，无编码转换损耗）；Linux 用 /proc/self/exe；
/// macOS 用 _NSGetExecutablePath。拿不到（罕见平台/受限环境）返回空。
std::filesystem::path current_executable_path()
{
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD actual = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (actual == 0) {
        return {};
    }
    while (actual == buffer.size()) {   // 缓冲区不足：扩容重试
        buffer.resize(buffer.size() * 2);
        actual = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (actual == 0) {
            return {};
        }
    }
    buffer.resize(actual);
    return {buffer};
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    ssize_t     len = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len <= 0) {
        return {};
    }
    buffer.resize(static_cast<size_t>(len));
    return {buffer};
#elif defined(__APPLE__)
    uint32_t size = 0;
    ::_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    return {buffer};
#else
    return {};
#endif
}

/// 测试可执行文件所在目录，即活动 buildir 的目标输出层
/// (<buildir>/<plat>/<arch>/<mode>/)。同一 buildir 的 binary 目标互为同目录
/// 兄弟，据此定位伴生产物不依赖"默认 buildir"假设。结果缓存；拿不到返回空。
const std::filesystem::path* active_target_dir()
{
    static const std::filesystem::path dir = [] {
        const auto exe = current_executable_path();
        return exe.empty() ? std::filesystem::path{} : exe.parent_path();
    }();
    return dir.empty() ? nullptr : &dir;
}

/// 逐级上溯 exe 所在目录，在活动 buildir 树内探测伴生产物。exe 既可能在 binary
/// 层（<buildir>/<plat>/<arch>/<mode>/，距 buildir 根三级），也可能经自定义产物
/// 目录落在 artifacts 层（<buildir>/artifacts/<plat>/
/// <arch>/<mode>/<line>/，距根五级），两种形态层级不同，故逐级上溯、由 @p layout
/// 拼出 buildir 根以下的布局段试探；产物存在才采信（防误指）。命中返回完整路径。
template <typename Layout>
std::filesystem::path probe_active_buildir(const std::filesystem::path& exeDir,
                                           const std::string& fileName, Layout layout)
{
    std::error_code ec;
    auto root = exeDir;
    for (int depth = 0; depth <= 6 && root.has_parent_path(); ++depth) {
        const auto path = layout(root) / fileName;
        if (std::filesystem::exists(path, ec)) {
            return path;
        }
        root = root.parent_path();
    }
    return {};
}

}   // anonymous namespace

std::filesystem::path getBuildTargetPath(const std::string& targetName)
{
    // 优先：活动 buildir。测试 exe 与伴生二进制同为 xmake binary 目标时在同一
    // 输出目录（<buildir>/<plat>/<arch>/<mode>/），按 exe 自身位置找兄弟目标在
    // 自定义 `-o <dir>` 配置下依然正确；伴生目标未随测试构建时不采信（防误指），
    // 回退下方默认 buildir 布局（历史行为）。exe 自身
    // 输出在 artifacts 层时（runner 类可执行），binary 层伴生目标上溯同 buildir 定位。
    if (const auto* dir = active_target_dir()) {
        const auto sibling = *dir / exe_name(targetName);
        std::error_code ec;
        if (std::filesystem::exists(sibling, ec)) {
            return sibling;
        }
        auto path = probe_active_buildir(*dir, exe_name(targetName),
                                         [](const std::filesystem::path& root) {
                                             return root / build_platform() / build_arch() /
                                                    getBuildModeName();
                                         });
        if (!path.empty()) {
            return path;
        }
    }
    return ca::test::top_project_path() / "build" / build_platform() / build_arch() /
           getBuildModeName() / exe_name(targetName);
}

std::filesystem::path getArtifactTargetPath(const std::string& line, const std::string& targetName)
{
    // 优先：活动 buildir。artifacts 层挂在 buildir 根下
    // （<buildir>/artifacts/<plat>/<arch>/<mode>/<line>/），比 binary 层多一级；
    // exe 无论在 binary 层还是 artifacts 层都逐级上溯探测，产物存在才采信
    // （防误指），否则回退下方默认 buildir 布局（历史行为）。
    if (const auto* dir = active_target_dir()) {
        auto path = probe_active_buildir(*dir, exe_name(targetName),
                                         [&line](const std::filesystem::path& root) {
                                             return root / "artifacts" / build_platform() /
                                                    build_arch() / getBuildModeName() / line;
                                         });
        if (!path.empty()) {
            return path;
        }
    }
    return ca::test::top_project_path() / "build" / "artifacts" / build_platform() / build_arch() /
           getBuildModeName() / line / exe_name(targetName);
}

}   // namespace test
