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

## 文档规范

三层按**权威来源**分工：扩展名即角色，不按目录分类。

| 载体 | 层 | 写什么 | 谁能引用它 |
|---|---|---|---|
| `///` Doxygen | 微观 | 单个符号的契约：参数、返回、失败模式、不变量；单模块的「背景/定位」 | 随代码移动 |
| `mjbase/doc/*.rst` | 宏观 | 跨模块设计、数据模型、schema、格式规格、pass 契约 | 代码/测试/构建脚本 |
| `*.md` | 人 + AI 方向 | README、AGENTS、dev_plan、迁移方法论、状态表 | 只有人和 AI 读 |

理由：markdown 撑不住代码耦合的规格，不是语法问题而是**没有可校验的引用面**——
无 label、无 include、`§N` 手写会漂；旧 doc_ai 目录里 5 个文档被删后引用全悬空，
没有任何工具报错，就是这么烂掉的。RST 白送这三样，规范才谈得上执行。

硬规则（`doc/check_refs.py` + Sphinx `-W` 在 CI 挡）：

1. 代码/测试/构建脚本只可引用 `mjbase/doc/*.rst`，路径必须存在。
2. 代码面出现 `.md` 路径即失败。两类豁免：流程文档白名单（AGENTS.md、dev_plan.md、
   README\*.md、CHANGELOG.md 等，校验存在性）；带仓名标记的跨仓出处（`libjbc`、
   `mbase`、`mjt`、`luiox`、`私有仓`），后者视为 prose 不校验存在性。
3. 引用定位用 label，形式 `doc:<label>`；被引用的位置必须有 label（不必给每节都放），
   命名 `<doc-slug>[-<section-slug>]`。文档内部自指可写「本文 §N」；
   **跨文档**的 `§N` 一律换成 `:ref:`，编号会随插入章节漂移。
4. 跨仓出处带仓名、不用路径形式（`libjbc design.md §35.4` 可，`../x.md` 不可）。
   本仓没有副本的规格写「私仓规格 <名>（本仓无副本）」。
5. 单模块的背景/定位注记写进该模块头文件的 Doxygen 组注释，不建独立文件——
   它跟代码的耦合就应该一起搬。判据：这段文字是否需要引用别处的代码或文档才说得清。

RST 写作约定（中文正文的坑，实测）：

- 表格一律 `.. list-table::`，**禁用 grid table**：docutils 按显示宽度校验 grid 边框，
  全角字形造成 "Malformed table data"，`-W` 下直接失败。
- 行内代码紧邻中文时必须写 `` ``x``\ 中文 ``（转义空格不能省）：docutils 判 end-string
  要求闭定界符后接空白或 ASCII 标点，紧随汉字（Lo）会报 start-string without end-string。
- 标题层级 `=` / `-` / `^`；代码块标 `.. code-block:: <lang>`；
  与代码同步的事实（schema_version、锁定依赖版本）集中一处定义，不跨文档复述。
- 首段写状态行：`状态：定稿/草案/部分落地（已落地面见本文 §X）`。

命令：

```sh
sphinx-build -b html -E -W --keep-going mjbase/doc mjbase/doc/_build   # -E：增量会吞掉旧警告
python mjbase/doc/check_refs.py                                       # 代码引用面校验
```

## 提交纪律

一主题一提交，每个提交独立编译、独立通过测试、独立可回退；
不提交测试产物与临时文件，提交前 `git status`。
