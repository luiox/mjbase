# mjbase 开发约定

mjbase 是基于 libjbc 的跨类程序分析与变换库，按模块逐步重写旧栈的能力。
旧栈不在本仓（语义参考实现由私有侧维护），只作语义参考与对拍 oracle；
mjbase 不做兼容 shim。

**改动前必读 [`../doc_ai/migration-boundary.md`](../doc_ai/migration-boundary.md)**：
公私边界红线（混淆器家族设施不进公开仓）、按消费方驱动的迁移切片顺序，
以及迁移与重构逐批交替的解耦纪律。

## 边界

- libjbc：单类/单方法（classfile DOM、CodeDom、CodeEditor、帧/域引擎）。
- mjbase：跨类身份（keys）、类/方法/字段层次、调用图、引用索引、
  指令工程（asm 域）、pass 域。mjbase 不重新实现 libjbc 已有能力，
  也不把全局状态塞回 libjbc。

## 硬性规则（并行迁移期间强制）

1. **禁止 include 任何 `jbytecode/...` 头**：旧栈只能读源码参考语义，
   不引入依赖。mjbase 的字节码底座只有 `jbc/...`（libjbc）。
2. **构建错峰**：允许运行 xmake 构建与测试自验，但多 agent 不得同时构建——
   用目录锁错峰：构建前 `while ! mkdir /tmp/mjbase_build.lock 2>/dev/null; do
   sleep 3; done`，结束 `rmdir /tmp/mjbase_build.lock`（trap EXIT 保证释放）。
   一次只构建自己需要的 target，不做全量重 build。
3. **提交纪律**：一主题一提交，每个提交独立编译、独立通过测试、独立可回退；
   不提交测试产物与临时文件，提交前 `git status`。
4. **禁改共享文件**：`mjbase/xmake.lua`、`src/mjbase/keys.hpp/.cpp`、
   `src/mjbase/util.hpp/.cpp`、`AGENTS.md`、其他模块目录。需要新的共享
   设施时在报告中提出，由主线统一落；模块内部 helper 放自己目录。
5. **只写自己的目录**：`src/mjbase/<module>/` 与 `test/unittest/<module>/`。
   测试文件以 `_test.cpp` 结尾会被构建自动收编（glob）。

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

## 从旧栈移植的纪律

- 语义照旧、实现重写：按旧实现的公开语义与测试意图在新底座上重写，
  不逐行翻译（旧代码架在 InsnList/MethodNode 上，新底座是值语义句柄）。
- 旧实现的测试是验收清单：把关键用例的行为断言移植过来。
- 明确放弃的部分（依赖旧栈基础设施、或本批范围外）在报告中列出。
