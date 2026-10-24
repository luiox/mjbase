#pragma once

// test_helper：mjbase 测试基建门面。
//
// 通用的多项目测试布局机制（.project_root_file 扫描 / 资源定位 / 输出根约定）
// 已下沉到 libca/test（#912/#913），本头文件保持本仓历史 API 形态
// （namespace test::、camelCase），内部委托 ca::test 实现——既有测试 main
// 无需任何改动。
//
// 本仓私有件（不下沉）：
// - asm-test 特例定位（libjbytecode/asm-test 目录布局胶水）
// - 构建产物路径（build/artifacts 布局属产品构建知识）
// - jdk_fixture.hpp（架构在闭源 libjdk_finder 之上，另见该文件）
//
// 使用约定不变：测试 target 须 set_rundir("$(projectdir)")，
// main 里先 test::setup("<子项目名>")。

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "libca/test/test.hpp"

namespace test {

inline void setup(const std::string& subproject)
{
    ca::test::setup(subproject);
}

inline std::filesystem::path getTopProjectPath()
{
    return ca::test::top_project_path();
}
inline std::filesystem::path getCurrentProjectPath()
{
    return ca::test::current_project_path();
}

inline bool                  hasResource(const std::string& filename);
inline std::filesystem::path getResourcePath(const std::string& filename);
inline std::vector<uint8_t>  getResource(const std::string& filename);

/// 跨项目定位：按 .project_root_file 名字（如 "mtest_set" / "mj2x"）取项目根 / 资源。
/// 统一样本供给的底层原语——任何子项目按名字拿另一子项目的 test_resource 文件，
/// 无需硬编码相对路径。未注册名字回落 <top>/<name>。
inline std::filesystem::path getProjectPath(const std::string& projectName)
{
    return ca::test::project_path(projectName);
}
inline std::filesystem::path getProjectResourcePath(const std::string& projectName,
                                                    const std::string& relPath)
{
    return ca::test::project_resource_path(projectName, relPath);
}
inline bool hasProjectResource(const std::string& projectName, const std::string& relPath)
{
    return ca::test::has_project_resource(projectName, relPath);
}
inline std::vector<uint8_t> getProjectResource(const std::string& projectName,
                                               const std::string& relPath)
{
    return ca::test::project_resource(projectName, relPath);
}

inline std::filesystem::path getDemoOutPath(const std::string& filename)
{
    return ca::test::demo_out_path(filename);
}
inline std::filesystem::path getOutPath(const std::string& filename)
{
    return ca::test::out_path(filename);
}
inline std::filesystem::path getTempOutPath(const std::string& filename)
{
    return ca::test::temp_out_path(filename);
}

std::string           getBuildModeName();

/// 定位同 buildir 的伴生构建产物（如 mj2x-cli）。优先按测试可执行文件自身位置
/// 推导活动 buildir——xmake binary 目标同树同目录输出，自定义 `-o <dir>` 配置下
/// 依然正确；目标未随测试同树构建（文件不存在）时回退默认 buildir 布局
/// <top>/build/<plat>/<arch>/<mode>/。@p targetName 可带 .dll 后缀定位 shared
/// 目标产物（如 "mjt-obf-runtime-native.dll"）。
std::filesystem::path getBuildTargetPath(const std::string& targetName);
/// 定位同 buildir 的 artifacts 层伴生产物
/// <buildir>/artifacts/<plat>/<arch>/<mode>/<line>/<name>。活动 buildir 优先
/// （同 getBuildTargetPath，exe 在 binary/artifacts 层均可上溯定位），产物不存在
/// 时回退默认 buildir 布局 <top>/build/artifacts/...。
std::filesystem::path getArtifactTargetPath(const std::string& line, const std::string& targetName);

// ---- 当前项目资源：委托 ca::test ----

inline std::filesystem::path getResourcePath(const std::string& filename)
{
    return ca::test::resource_path(filename);
}
inline bool hasResource(const std::string& filename)
{
    return ca::test::has_resource(filename);
}
inline std::vector<uint8_t> getResource(const std::string& filename)
{
    return ca::test::resource(filename);
}

}   // namespace test
