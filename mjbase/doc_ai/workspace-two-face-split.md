# workspace 两面拆分：资源容器 + 分析项目

状态：设计稿（评审中，未实施）。挂账来源：dev_plan「workspace 重组」条目；
拆分动机与耦合实测见该条。

## 1. 问题

`workspace::WorkspaceBase` 单 class 66 个虚方法绑死四族职责：

| 族 | 代表方法 | 依赖性质 |
|---|---|---|
| 资源装载 | openJar / openClass / addSupportingContainer / removeSupportingJar | jar/class 格式面（未来 dex/apk 同类） |
| 只读查询 | search ×5 / xrefs ×3 / classRelations / methodBytecode / getJasm | 一半纯单类结构视图，一半跨类图分析 |
| 编辑台账 | applyJasm / replaceClassBytes / undo / redo / savepoint / ledger / export | 字节级事务 + 语义验证门混排 |
| 高级操作 | remap / rename / pipeline / workflow / repair | 分析结果驱动的改写 |

容器本体 `model::ResourceContainer` 被私有成员（`working_`/`original_`）
封死：消费方无法自带容器实现、无法自定义分析组合。新增 dex/apk 只能往
WorkspaceBase 里继续堆方法族，且所有消费方被迫吃下整个 java-class 查询面。

消费面实测（mjt + mjt-py）：66 个虚方法全量触达（revision 68 处、
classInfo 24 处、undo 20 处），`mjt/src/shared/jar_workspace.hpp` 以
`class JarWorkspace : public WorkspaceBase` 整体继承——**任何拆分必须
facade 过渡零破坏**。

## 2. 目标形：两面对

```
┌────────────────────────────┐        ┌────────────────────────────┐
│ 资源容器面 ResourceSession  │        │ 分析项目面 AnalysisSession  │
│ （拥有容器与台账）          │◄──attach──（不拥有，外部挂上来）      │
│ 装载/挂载/字节读写/编辑事务  │        │ 跨类图分析/搜索/高级操作    │
│ 导出；验证门为可插拔钩子     │        │ 产出候选 → 经容器面提交     │
└────────────────────────────┘        └────────────────────────────┘
        ▲ JarResourceSession                      ▲ 消费方自定义绑哪些分析族
        ▲ DexResourceSession（未来）               ▲ 分析缓存按 revision 失效
```

判据一句话：**碰字节与事务的归容器面，需要跨类图（hierarchy/reference/
alias）或产出改写计划的归分析面**。

### 2.1 资源容器面（建议 `mjbase::resource::ResourceSession`）

- 装载/挂载：openJar（含条目名宽容解码注入）/ openClass /
  addSupportingJar / addSupportingContainer（角色标签）/ removeSupportingJar /
  supportingResources / isLoaded / artifactKind / sourcePath / primaryClassUri。
- 资源读：listResources / readResource / readClassBytes /
  manifestAttributes / resourceStrings。
- 单类结构视图（无跨类图依赖，jbc 会话读字节结构）：listClassSummaries /
  listClasses / classInfo / constantPool / methodInfo / fieldInfo /
  methodBytecode / methodInstructions / getMethodJasm / getJasm。
- 编辑事务：validateJasm / applyJasm / validateClassBytes / applyClassBytes /
  validateClassMutation / applyClassMutation / replaceClassBytes /
  revertClass / modifiedClasses / undo / redo / createSavepoint /
  restoreSavepoint / bindLedgerStore / flushLedger / compactLedger /
  exportJar / exportClass / revision / isDirty / canExport。
- JAR 级工具：diffJar / analyzeJarRepair / applyJarRepair。

### 2.2 分析项目面（建议 `mjbase::project::AnalysisSession`）

与既有持久文档层 `project::AnalysisProject`（工程 JSON：inputs 角色树 +
批注 + sidecar 引用）同域，补齐其运行时半边。

- 挂载：`AnalysisSession::attach(const resource::ResourceSession&, const
  AnalysisStackConfig&)`——不拥有容器；容器 revision 前进即按
  PreservedAnalyses 语义失效本面缓存（现 `analysisGraph()` 机制搬家）。
- AnalysisStackConfig = 消费方自定义绑哪些分析族：JDK 层次种子（jdkPath）、
  reference index、alias 闭包、搜索索引、进度 sink。不绑的分析不构建。
- 跨类查询：searchClasses / searchMembers / searchStrings / searchNumbers /
  searchInstructions / xrefsToMethod / xrefsToField / xrefsToClass /
  calleesOfMethod / classRelations / overridesOfMethod。
- 高级操作（产出改写计划/候选，经容器面事务提交）：
  analyzeRemap / applyRemap / analyzeRename / applyRename / validatePipeline /
  applyPipeline / runWorkflow / commitWorkflowCandidateJar。
- 静态查询 availablePasses / availablePipelines 留在装配层（spi 注册表
  查询面），不进两面接口。

### 2.3 验证门外置（可插拔）

编辑事务里的语义验证（spi sanitize + jasm 验证 + 候选容器即时层次重建）
不是容器本质职责——收窄为容器面构造时注入的 `IValidationGate`：

```
validate(candidate, scopeUris, mode) -> diagnostics
```

- 缺省门 = 现行为（DefaultValidationGate，由分析侧供给实现）。
- dex/apk 容器换各自的门；消费方可关门（mode=off 直通，现语义保留）。
- 分析面的高级操作 apply 族同样经容器面提交口——验证门单点复用。

### 2.4 dex/apk 插入位

- 容器面按格式分实现族：JarResourceSession（现 working_ 逻辑搬家）/
  DexResourceSession（未来，dex→类结构视图适配）。
- 能力协商：容器面暴露 `CapabilityFlags`（JvmClassStructures / JvmManifest /
  …）。分析面 attach 时按能力绑定：无 JvmClassStructures 的容器挂层次/引用
  图 = 显式 Err；资源字符串扫描等格式无关分析可单独绑。
- 新格式 = 新容器适配器 + 新验证门，**不再动任何既有面**。

## 3. 迁移战役（四阶段，全程套件钉行为）

| 阶段 | 内容 | 破坏面 |
|---|---|---|
| S1 | 两接口定义 + WorkspaceBase 双实现（`class WorkspaceBase : public IResourceSession, public IAnalysisSession`；`IWorkspace` 组合继承两面不动） | 零（mjt 编译不变） |
| S2 | workspace.cpp（4806 行）分体：ResourceSessionCore（容器态）+ AnalysisSessionCore（挂容器），WorkspaceBase 变薄壳委托 | 零（行为由套件钉死） |
| S3 | 两新接口面开放；mjt `JarWorkspace` 迁两面直连或保留 facade；mjt MCP 工具面 project_open / container_mount 直连容器面 | mjt 仓内 PR |
| S4 | 消费方清零后退役 IWorkspace / WorkspaceBase facade | mjbase 删除面 |

## 4. 与既有件的关系

- `project::EditLedgerStore` / `project_store`：容器面私有（字节台账）。
- `project::AnalysisProject`（工程 JSON 文档层）：不动，是分析面的持久半边
  （inputs 登记即 attach 清单的来源）。
- `project::analysis_cache`：分析面私有（跨类分析缓存）。
- `spi` 协作面（sanitize / resolver / pipeline builder）：装配层，两面经它
  组装，不新增 spi 对两面的反向依赖。
- `model::ResourceContainer`：维持纯数据态；容器面是它的会话态壳。
