# mjbase 开发约定

mjbase 是基于 libjbc 的跨类程序分析与变换库。

## 边界

- libjbc：单类/单方法（classfile DOM、CodeDom、CodeEditor、帧/域引擎）。
- mjbase：跨类身份（keys）、类/方法/字段层次、调用图、引用索引、
  指令工程（asm 域）、pass 域。mjbase 不重新实现 libjbc 已有能力，
  也不把全局状态塞回 libjbc。

## 代码风格（对齐仓库根 AGENTS.md）

- 命名空间 `mjbase`（子模块 `mjbase::asm_...` 等，见模块目录）。
- 头文件引用：模块内 `"file.hpp"`；跨模块 `"mjbase/<module>/file.hpp"`
  （include 根是 `src/`，一律写全限定形式）；
  libjbc 用 `"jbc/jbc.hpp"` 或具体头；libca 用 `"libca/core/result.hpp"` 等。
  禁止 `../`。
- 类型：`ca::u32/ca::usize/Result<T,E>`；错误走 `Result` 值语义，
  不抛异常、不静默吞错。
- 字符串：API 用 `std::string_view` 借用视图（KeyPool 驻留后长期有效）；
  不新增 `std::string` 存储。
- 注释：Doxygen `///` 行注释；`/** */` 禁止。无 RTTI。
- 测试：gtest，`TEST(Mj<Module>Test, Name)`。构造指令序列直接用
  `jbc::ir::CodeDom`（append + createLabel 组合，不需要 classfile）；
  需要类字节时参考 libjbc 独立仓（luiox/libjbc）`demo/control_flow.cpp` 的手工字节模式。

## 提交纪律

一主题一提交，每个提交独立编译、独立通过测试、独立可回退；
不提交测试产物与临时文件，提交前 `git status`。
