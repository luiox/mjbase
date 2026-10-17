# test_helper

Shared test utility library for morpher subprojects. Provides resource loading and output path resolution for tests.

`jdk_fixture.hpp` additionally provides shared JDK probing and cross-platform command execution:
Windows commands run through a temporary `.bat`; POSIX commands run through a temporary `sh`
script. Both stop at the first failing command and remove the temporary script before returning.

## API

```cpp
#include "test_helper.hpp"

namespace test {
    void setup(const std::string& subproject);

    std::filesystem::path getTopProjectPath();
    std::filesystem::path getCurrentProjectPath();

    bool                    hasResource(const std::string& filename);
    std::filesystem::path   getResourcePath(const std::string& filename);
    std::vector<uint8_t>    getResource(const std::string& filename);

    bool                    hasAsmTestResource(const std::string& filename);
    std::filesystem::path   getAsmTestResourcePath(const std::string& filename);
    std::vector<uint8_t>    getAsmTestResource(const std::string& filename);

    // 跨项目样本访问（按 .project_root_file 名字定位另一子项目的 test_resource）
    std::filesystem::path   getProjectPath(const std::string& projectName);
    std::filesystem::path   getProjectResourcePath(const std::string& projectName, const std::string& relPath);
    bool                    hasProjectResource(const std::string& projectName, const std::string& relPath);
    std::vector<uint8_t>    getProjectResource(const std::string& projectName, const std::string& relPath);

    std::filesystem::path   getDemoOutPath(const std::string& filename);
    std::filesystem::path   getOutPath(const std::string& filename);
    std::filesystem::path   getTempOutPath(const std::string& filename);
}
```

## How it works

### Project root

All test targets **must** set `set_rundir("$(projectdir)")` in xmake.lua, so CWD = project root.

### `.project_root_file` scanning

On `setup()`, test_helper recursively scans from CWD for all `.project_root_file` files
and builds a `name → absolute_path` map:

| File location | Content (name) | Resolved path |
|---|---|---|
| `.project_root_file` | `morpher` | *project root* |
| `libs/libjbytecode/.project_root_file` | `libjbytecode` | `libs/libjbytecode/` |
| `libs/libjbytecode/core/.project_root_file` | `libjbytecode_core` | `libs/libjbytecode/core/` |
| ... | ... | ... |

Blacklist (directories skipped during scan): `.`-prefix, `build`-prefix, `node_modules`.

### Resource loading

Resources are resolved to `<CurrentProjectPath>/test_resource/<filename>`.

Call `setup("project_name")` once in `main()` to initialize:
- `getResource()` reads and returns the file as `std::vector<uint8_t>`
- `getResourcePath()` returns the filesystem path
- `hasResource()` checks existence
- `getCurrentProjectPath()` returns the absolute path mapped to the project name
- `getTopProjectPath()` returns the project root absolute path

### 跨项目样本访问

当某个子项目（如 mj2x）需要消费另一子项目（如 mtest_set）的 `test_resource/`
样本时，通过这组按名字定位的原语跨项目取，无需各自硬编码相对路径：

- `getProjectPath(name)` → 该 `.project_root_file` 名字对应的项目根绝对路径
- `getProjectResourcePath(name, relPath)` → `<项目根>/test_resource/<relPath>`
- `hasProjectResource(name, relPath)` → 存在性检查
- `getProjectResource(name, relPath)` → 读取为 `std::vector<uint8_t>`

定位机制同 `getAsmTestResourcePath`：通过 `.project_root_file` 扫描建立的
`name → path` 映射查找项目名（如 `"mtest_set"` / `"mj2x"`）。统一样本供给门面
`mtest_set::sample`（`mtest_set/src/sample/`）即建立在这组原语之上。

### Adding a new subproject

1. Create `.project_root_file` with the project name as content
2. Create `test_resource/` directory under the subproject
3. No changes needed to test_helper

### Output paths

All output goes under `<TopProjectPath>/test/`:
- `getOutPath()` → `test/<filename>`
- `getDemoOutPath()` → `test/demo/<filename>`
- `getTempOutPath()` → `test/tmp/<filename>`

Directories are created automatically if missing.

## Usage in xmake

```lua
add_deps("test_helper")
set_rundir("$(projectdir)")
```

```cpp
#include "test_helper.hpp"

int main(int argc, char** argv) {
    test::setup("libjclassfile");
    // ...
    auto data = test::getResource("samples/Test.class");
}
```
