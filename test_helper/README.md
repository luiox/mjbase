# test_helper

Shared test utility library for subprojects. Provides resource loading and output path resolution for tests.

`jdk_fixture.hpp` additionally provides shared JDK probing and cross-platform command execution:
Windows commands run through a temporary `.bat`; POSIX commands run through a temporary `sh`
script. Both stop at the first failing command and remove the temporary script before returning.

## How it works

### Project root

All test targets **must** set `set_rundir("$(projectdir)")` in xmake.lua, so CWD = project root.

### `.project_root_file` scanning

On `setup()`, test_helper recursively scans from CWD for all `.project_root_file` files
and builds a `name → absolute_path` map (e.g. root `.project_root_file` names the top
project; `libs/libjbytecode/.project_root_file` names `libjbytecode`). Blacklist (skipped
during scan): `.`-prefix, `build`-prefix, `node_modules`.

### Resource loading

Resources are resolved to `<CurrentProjectPath>/test_resource/<filename>`. Call
`setup("project_name")` once in `main()`, then use `getResource` / `getResourcePath` /
`hasResource` / `getCurrentProjectPath` / `getTopProjectPath`.

Cross-project sample access (`getProjectPath` / `getProjectResourcePath` /
`hasProjectResource` / `getProjectResource`) resolves another subproject's
`test_resource/` through the same `.project_root_file` name map, no hardcoded relative
paths.

### Adding a new subproject

1. Create `.project_root_file` with the project name as content
2. Create `test_resource/` directory under the subproject
3. No changes needed to test_helper

### Output paths

All output goes under `<TopProjectPath>/test/`: `getOutPath()` → `test/<filename>`,
`getDemoOutPath()` → `test/demo/<filename>`, `getTempOutPath()` → `test/tmp/<filename>`.
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
    auto data = test::getResource("samples/Test.class");
}
```
