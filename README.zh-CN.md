# mjbase

`mjbase` 是基于 [libjbc](https://github.com/luiox/libjbc) 的跨类 JVM 字节码分析与变换库，是旧栈的重写继任者线：跨类身份（keys）、类/方法/字段层次、调用图与引用索引、指令工程（asm 域）、映射与重映射、AIR/SIR 树 IR，以及驱动它们的 pass 管线。

与 libjbc 的分界是刻意的：**libjbc 只管单类/单方法**（classfile DOM、CodeDom、CodeEditor、帧与支配引擎），**mjbase 管跨类**。mjbase 不重新实现 libjbc 已有能力，也不把全局状态塞回 libjbc。

## 构建

需要 xmake 3.x 与 C++17 编译器（Windows 用 MSVC，Linux 用 gcc/clang）。

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f -p windows          # Linux 用 -p linux
xmake build mjbase
```

依赖来自 [luiox-repo](https://github.com/luiox/luiox-repo) 与 xrepo：libca、libjbc、libjzip、libjstr、libgarena、libjclassfile、libjstd、libjhasm、libjblw、libjbytecode（仅 JASM 文本面的链接层传递依赖）、libarchive、zlib。

### 测试

```sh
xmake f --with_tests=y
xmake build mjbase_unittest
xmake run mjbase_unittest --gtest_brief=1
```

部分用例需要 JDK，缺失时 `GTEST_SKIP` 而非失败。产物落在项目根 `test/` 目录。

## 仓库布局

```
mjbase/src/mjbase/     库源码，每个模块一个目录
mjbase/test/           单元测试（gtest）
mjbase/test_resource/  静态测试输入
mjbase/doc_ai/         设计笔记与迁移模块地图
mjbase/dev_plan.md     只记待办项
test_helper/           测试基建（gtest 集成、JDK 夹具）
```

模块迁移状态与语义来源见 [`mjbase/doc_ai/module-map.md`](mjbase/doc_ai/module-map.md)。

## 约定

- **无 RTTI**：禁用 `dynamic_cast` / `typeid`，用 typed cast 家族
- **类型**：`ca::u32` / `ca::usize` / `Result<T, E>`；错误走 `Result` 值语义，不抛异常
- **字符串**：借用视图用 `std::string_view`（指向驻留池文本），不新增 `std::string` 存储
- **头文件**：模块内 `"file.hpp"`，跨模块 `"mjbase/module/file.hpp"`，禁 `../`
- **注释**：Doxygen `///`；`/** */` 禁止

完整开发约定见 [mjbase/AGENTS.md](mjbase/AGENTS.md)。

## 许可证

[MIT](LICENSE)
