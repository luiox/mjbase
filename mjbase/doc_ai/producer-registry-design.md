# analysis-db producer 注册表设计（G5）

状态：草案，2026-11-02。待评审，未落地。analysis-db 总体设计与身份模型见 [analysis-database.md](analysis-database.md)；本设计是 G 批次长计划 P6 的 G5 项，长计划定案见 #121 的 G 批次评论，对应蓝图第 4 步（producer 注册表、dirty 队列、局部事实存储与出边替换）的入口面。只定事实溯源与归属模型，不动任何事实的内容语义。

## 1. 现状

AnalysisDb 是分析事实源：SQLite 单文件（schema v4），投影面覆盖 inputs/entries 条目身份、classes/members/interfaces 类与成员清单、refs 直接引用、renames 重命名覆盖层、mask_rules 出口遮蔽配置与命中记账、census 普查投影。全部写入走单事务批量入口，任一行失败整批回滚。

当前把事实写进这个库的入口，函数级盘点如下。

| 写入口 | 仓内现有调用方 | 隐含的生产者身份 |
|---|---|---|
| `ensureInput` / `ingestEntries` | mjt 整库导入桥 `ingestIntoAnalysisDb`（mjt/src/workspace/analysis_ingest.cpp） | 无任何记录 |
| `ingestClasses` | 同上 | 无任何记录 |
| `replaceReferences` | 同上 | 无任何记录 |
| `replaceReferencesFromMember` | 仓内无生产调用方；方法级 pass 桥属私有侧管线消费面，私有侧适配器按同款锚接自家管线 | 无任何记录 |
| `setClassRename` / `setMemberRename` / `setRenames` / `clear*` / `replaceRenames` | 仓内仅测试调用；生产接线（覆盖层导入面）未落或属私有侧 | 无任何记录 |
| `replaceMaskRules` / `setMaskTrustLevel` | 仓内仅测试调用，规则配置写入面待接 | 无任何记录 |
| `recordMaskRuleHits` | mjt MCP mask 出口（mjt/mcp/src/tool/mask_exit.cpp，逐查询记账） | 无任何记录 |

生产者现在实际有三类进程语境：mjt workspace 整库导入桥（生产调用点在 mjt-py facade 的 census 流程，临时库即弃）、mjt MCP 工具面（工程 bundle 的 analysis.mjdb 命中记账）、mjbase 自身测试与 harness。按既定路线还会增加：semantic_fp 管线、库识别 L2/L3、渲染 producer，以及按 mjt-py/src/pysdk_bridge.hpp 注释明言的私有侧管线适配器（「方法级 pass 桥属私有侧管线消费面……私有侧适配器按同款锚接自家管线」）。

库对这些写入没有任何归属记录：两个生产者先后写同一个库之后，没有任何一个事实能回答它是哪条管线、哪个工具版本、哪次运行写入的。

## 2. 为什么不够

溯源缺失是硬缺口，具体四个面。第一，来源不可答：事实表是最后一次写入者覆盖语义（类 upsert、引用替换、覆盖层覆盖），多管线交错写入后，库内既没有谁写的也没有何时写的（refs 连时间戳都没有），审计只能靠外部记忆。第二，批次不可答：一次整库导入按现有写入顺序是条目、类、引用三段独立事务，中途失败后库内留下「前三段已提交、引用仍旧集合」的半程状态，没有任何行记录这次运行存在、进行到哪一步、写到多大规模。第三，冲突不可见：renames 覆盖层按 (target_kind, target_id) 整体覆盖，两个工具先后设置同一目标的显示名时后写覆盖先写且不留痕迹；mask_rules 同为整体替换。第四，可信级别无载体：mask 面已确立项目级信任级别（meta 键 mask_trust_level，default_action 翻转开关），但「这条事实来自可信工具还是脚本」没有落点，多生产者同库之后无法按来源分级处置。

蓝图第 4 步把 producer 注册表列为第一个落地件，原因在此：dirty 队列与局部事实存储都要求事实按 (producer, producer_version) 键控（analysis-database.md §4.2），没有注册表就没有键的登记端。本设计先把登记端与归属模型立起来，dirty 队列与局部事实存储按各自批次推进。

## 3. 设计

### 3.1 producers 表：生产者登记

生产者是「向本库写事实的身份」，注册幂等：稳定 key 命中即刷新元数据并复用 id。producer_id 单调分配，key 注册后不可变，无删除 API——与身份层只增不删纪律同构；改名或换工具指纹都是注册新生产者或刷新元数据，历史批次永远指向旧元数据也可经 updated_at 追溯。

```sql
CREATE TABLE producers(
  producer_id      INTEGER PRIMARY KEY AUTOINCREMENT,
  key              TEXT NOT NULL UNIQUE,          -- 稳定 id（点分 slug），注册后不可变
  name             TEXT NOT NULL,                 -- 展示名
  tool_fingerprint TEXT NOT NULL DEFAULT '',      -- 工具自报版本/构建指纹（自由文本）
  trust_level      INTEGER NOT NULL CHECK(trust_level IN (0,1)),  -- 0=untrusted 1=trusted
  created_at       TEXT NOT NULL,
  updated_at       TEXT NOT NULL
);
```

tool_fingerprint 由注册方自报（产品名 + 语义版本 + 构建标识的拼接是建议载荷），mjbase 不解析其内部结构；它回答「哪个版本的工具写的」，与事实内容指纹（semantic_fp，另一批次）正交。trust_level 两档，词表对齐既有 MaskTrustLevel（trusted/untrusted），但不复用同一 C++ 枚举——两者语义不同（项目出口默认动作 vs 事实来源可信级别），绑死一个类型会让两个轴被迫同步演化。

### 3.2 事实归属粒度：批次表 + 作用域锚（推荐）

两个候选。

方案 A，行级外键：classes/members/refs/entries 每张事实表加 NOT NULL producer_id 列，写入口逐行盖章。精确到行，代价有三。其一，语义噪声：编码事实的内容与写入者无关——同一字节码的正确扫描逐行产出相同 refs，「这行 ref 是谁写的」的正确答案本来就是「最后一次重算该源切片的运行」，行级归属把切片级问题拆到百万行级粒度上答。其二，成本落在最大表：refs 在基准语料 232 万行、jdk17 语料 349 万行，按 producer 过滤 refs 不是真实查询负载，不加索引则该过滤是全表扫，加索引则是百万行级索引税。其三，写路径侵入：每条 INSERT 预编译语句增加一次 bind，全部写入口与全部未来写入口永久背。

方案 B，批次表 + 作用域锚：三张新表，事实表不加列（renames 例外，见 3.4）。归属单位是写入运行（批次），作用域锚记录这次运行覆盖的事实区间；逐行问题经作用域回答。

```sql
CREATE TABLE batches(
  batch_id     INTEGER PRIMARY KEY AUTOINCREMENT,
  producer_id  INTEGER NOT NULL REFERENCES producers(producer_id),
  kind         TEXT NOT NULL,             -- full_import / refs_replace / refs_slice_replace / renames_write / rules_write
  started_at   TEXT NOT NULL,
  finished_at  TEXT NOT NULL DEFAULT '',  -- 空串 = 未收尾（失败/中断的残批，可见）
  class_count  INTEGER NOT NULL DEFAULT 0,
  entry_count  INTEGER NOT NULL DEFAULT 0,
  ref_count    INTEGER NOT NULL DEFAULT 0,
  note         TEXT NOT NULL DEFAULT ''
);
CREATE TABLE batch_scopes(
  batch_id   INTEGER NOT NULL REFERENCES batches(batch_id),
  scope_kind INTEGER NOT NULL CHECK(scope_kind IN (0,1,2,3)),
  scope_id   INTEGER NOT NULL,           -- refs_all 固定 0；input_id / class_id / member_id
  PRIMARY KEY(batch_id, scope_kind, scope_id)
) WITHOUT ROWID;
CREATE INDEX batch_scopes_owner ON batch_scopes(scope_kind, scope_id, batch_id);
```

作用域种类：0=refs_all（整库引用替换）、1=input（条目登记与重导入）、2=class（类事实 upsert 与 class 头引用切片）、3=member（成员引用切片）。成员登记的归属并入其所在类（ingestClasses 的 class 作用域行）。

归属解析规则：成员引用切片的归属 = 在 batch_scopes 探 (3, member_id) 与 (0, 0) 两个前缀、取 batch_id 较大者；class 头引用切片同构（(2, class_id) 对 (0,0)）；类事实归属只探 (2, class_id)（整库引用替换不触碰类行，refs_all 不回退参与类事实归属）；条目归属取 (1, input_id) 最新批。refs_all 作用域覆盖全部引用切片：整库替换之后，此前一切成员级归属被更新的 refs_all 批盖过，与 replaceReferences 清空重插的语义一致。解析是两次索引前缀探 + 取大，无全表扫。

批次与事务的关系：批次是逻辑写入运行，不是事务。openBatch 先落一行 started 批（自带事务），随后各写入口在各自事务内写事实与作用域行，closeBatch 收尾写 finished_at 与计数。现有整库导入的三段事务形状原样保留，一次导入一个批次；中途失败留下 finished_at 为空的残批，是合法审计状态（回答了「这次运行开始过、走到条目段」），不算脏数据。切片替换一次编辑一个 refs_slice_replace 批，编辑节奏是人与 agent 步进，批次量级无虞。

推荐方案 B。A 的唯一优势是行级精确，而编码事实的行级精确是伪需求；A 在最大表上的成本与语义噪声使其全面劣于 B。混合形态（refs 走批次、classes 走行级列）不采纳：两套归属机制并存，查询面与纪律双份，收益不成比例。

### 3.3 schema 变更与迁移

本变更是纯增量：三张新表加 renames 一列（见 3.4），不重排既有行、不改任何既有列语义。版本门纪律延续 fail-loud：打开时 user_version/application_id/meta.kind 一并校验，不识别的版本拒绝打开，旧库由上层显式重建。重建动线已有实测成本（rt.jar 36.3s、jdk17.jar 73.6s），renames 从既有覆盖层导入面重放，其 producer 归属为执行重放的登录生产者。因此推荐路线下「旧行回填」不存在：重建库从空注册表起步，v4 历史写入无法追溯、也不伪造归属。版本号动工时按当时主线现状取下一空闲值；本设计与其他在评设计稿无内容依赖，若目标侧 id 化先行合入，本表叠加其上、版本号顺延。

备选路线（原地升级）留作开放决策：打开时检测旧版本，单事务内 CREATE 三表 + renames 加列 + 回填一行 legacy 生产者（key="legacy"、trust=untrusted）与一个 backfill 批次，全部既有 renames 行 producer_id 落 legacy，再升 user_version。SQLite 单事务保证无半迁移；代价是迁移代码与测试面长期背，且扫描事实无批次历史可填，回填的溯源价值接近零（只有 renames 行有真实占位对象）。推荐维持 fail-loud。

### 3.4 与现有纪律的相容性

稳定 id 只增不删：producers 注册幂等、id 单调、无删除；batches/batch_scopes 只追加（refs_all 批不删除任何历史作用域行，覆盖靠解析规则取大表达）。条目复活、类 upsert、引用整库替换等既有身份纪律全部不动。

renames 覆盖来源：renames 是覆盖层与配置型小表，行级归属即语义（「这条覆盖是谁、何时设置的」就是行问题），v5 给它加 producer_id 列。覆盖行为不变（last-write-wins），注册表只让覆盖可见：覆盖来源 + updated_at 让「谁盖掉了谁」事后可查。是否按 trust 拦截 untrusted 覆盖 trusted 是策略问题，留开放决策，第一版不拦截。

并发：SQLite 单写者模型不变，一个 AnalysisDb 连接一个写者，注册表不引入新锁。多进程写同一库（MCP 进程与 pysdk 进程并发开写）现状靠 busy_timeout 与应用层错峰，注册表不改这一点；批次让交叉写入事后可观测（谁在什么时间开过批、各自写了什么区间），写仲裁不在第一版范围。

census 投影：censusClasses/censusContainer 不加 producer 维度。投影输入是内容事实（类与条目的编码属性），与写入者无关；「谁贡献了多少」由批次统计面回答。重开条件：出现同一输入的类事实按生产者分叉的真实负载（多管线各持一份事实待对比），再议投影维度。

### 3.5 API 面（mjbase 侧）

登记与批次（写入前，ReadWrite 连接）：

```cpp
struct DbProducerInput { ca::str::Utf8String key, name, toolFingerprint; DbProducerTrustLevel trustLevel; };
ca::Result<ca::i64, AnalysisDbError> registerProducer(const DbProducerInput& input);  // 幂等：key 命中即刷新元数据
ca::Result<std::vector<DbProducerInfo>, AnalysisDbError> listProducers() const;
ca::Result<ca::i64, AnalysisDbError> openBatch(ca::i64 producerId, DbBatchKind kind, const ca::str::Utf8StringRef& note);
ca::Result<void, AnalysisDbError> closeBatch(ca::i64 batchId, const DbBatchCounts& counts);
```

既有写入口的归属化：ingestEntries / ingestClasses / replaceReferences / replaceReferencesFromMember 增加首参 batchId；setRenames 族、replaceRenames、replaceMaskRules、setMaskTrustLevel 增加尾参 producerId（renames 行级盖章，mask 配置面归属）。签名一步到位，不保留匿名旧签名：消费方只有导入桥、MCP 出口、facade 与测试，面小且全部在本仓或已知适配面；隐式方案（连接绑定「当前生产者」状态）否决——忘了设置就是静默错归属，与 fail-loud 纪律相悖。ensureInput 不带批次：输入登记的归属随其后的 ingestEntries 批次（input 作用域行）。recordMaskRuleHits 不变：命中计数是聚合计数列，无行级事实可归属，不入批次。拒绝面：写入口收到未注册或不存在的 batchId → Invalid，不静默匿名写入。

查询面（只读连接可用）：

```cpp
ca::Result<std::vector<DbBatchInfo>, AnalysisDbError> listBatches(
    std::optional<ca::i64> producerId) const;                    // 含计数、作用域数、生产者 key
ca::Result<std::vector<DbBatchScope>, AnalysisDbError> batchScopes(ca::i64 batchId) const;
ca::Result<std::optional<DbOwnershipInfo>, AnalysisDbError> refSliceOwnership(
    DbMemberKind kind, ca::i64 memberId) const;                  // 解析规则见 3.2
ca::Result<std::optional<DbOwnershipInfo>, AnalysisDbError> classOwnership(ca::i64 classId) const;
ca::Result<std::vector<DbProducerStats>, AnalysisDbError> producerStats() const;  // 批次数、累计写入计数、首末批时间
```

### 3.6 mjt 侧接线

| 消费面 | 注册身份 | 批次用法 |
|---|---|---|
| workspace 整库导入桥 `ingestIntoAnalysisDb` | key=`mjt.workspace.ingest`，trusted | 一次导入一个 full_import 批：input 作用域 + 逐类作用域 + refs_all 作用域 |
| pysdk facade census 流程 | 经导入桥同一身份（临时即弃库同样落注册） | 同上 |
| mjt MCP mask 出口 | key=`mjt.mcp.mask_exit` | 命中记账不入批次；规则/信任级别写入接线时按 rules_write 批 |
| pysdk 脚本面（rename token、覆盖层导入） | key=`mjt.pysdk.facade`，untrusted（脚本来源） | renames 写入带 producerId；是否拦截留开放决策 |
| 私有侧 pass 桥适配器 | key=`private.` 前缀，自报指纹 | 切片替换一次编辑一个 refs_slice_replace 批 |

工具指纹由各消费面自报；私有侧适配器使用公开的注册面自报身份，mjbase 不验证其真伪（注册表回答的是「写方自称是谁」，可信级别的处置是消费方策略）。

## 4. 开放决策点

| 问题 | 当前倾向 | 需要裁决 |
|---|---|---|
| 归属粒度 | 批次 + 作用域锚（3.2 推荐），refs 无行级列 | 是否接受切片级（而非行级）的 refs 归属 |
| 旧库升级路径 | fail-loud + 重建，纪律延续 | 是否改判为原地加表迁移 + legacy 回填（代价见 3.3） |
| legacy 生产者默认值 | 推荐路线下不需要；原地路线下 key=`legacy`、untrusted | 同上 |
| trust_level 枚举 | 两档（trusted/untrusted），独立类型不复用 MaskTrustLevel | 是否需要多档（builtin/tool/script/agent 级差） |
| untrusted 事实的查询处置 | 全量返回，归属与统计面可见 | 是否提供按 trust 过滤的查询变体、是否默认过滤 |
| renames 跨生产者冲突 | last-write-wins 不变，覆盖来源可见 | 是否按 trust 拦截 untrusted 对 trusted 覆盖的改写 |
| 命中记账归属 | 不入批次（聚合计数，无行级归属对象） | 是否需要逐查询记账的溯源面 |
| key 命名空间 | 只约束唯一性与不可变性，`mjt.*` / `private.*` 是约定不是契约 | 前缀约定是否入公开契约 |
| 微编辑批次合并 | 一次切片替换一个批次 | 高频自动化编辑是否需要会话级聚批 |
| 审计查询的工具出口 | mjbase 只给 C++ 查询面 | producerStats 是否承诺 MCP 工具化（mjt 层决策） |

## 5. 验收

设计通过后的实现批次切分建议，每批独立 PR 独立可验。

| 批 | 仓 | 内容 | 独立验证 |
|---|---|---|---|
| 1 | mjbase | v5 DDL（producers/batches/batch_scopes）+ registerProducer/openBatch/closeBatch + 版本门递增 | 新库注册幂等、残批可见；旧 v4 库拒绝打开且错误可诊断 |
| 2 | mjbase | renames 行级 producer_id + renames 写入口归属化 | 覆盖来源逐行可查；覆盖重放后归属与 updated_at 正确 |
| 3 | mjbase | 事实写入口批次参数化 + 作用域行 + refSliceOwnership/producerStats | 归属解析逐用例：成员批与 refs_all 批取大、整库替换后切片归属翻转、批内回滚不留作用域行、崩溃注入后批次状态与事实一致 |
| 4 | mjt（经 luiox-repo 词条 bump） | 导入桥、MCP 出口、pysdk 接线注册身份并传批次 | workspace 套件 + pysdk 套件全绿；导入后 producers/batches 非空且计数与 AnalysisIngestStats 对账 |
| 5（可选） | mjt | producerStats 的 MCP 工具出口 | 按工具面验收 |

批 1-3 在 mjbase 测试面自闭环，对 mjt 无感（钉版不动即不暴露签名变更）；批 4 是唯一跨仓批，bump 词条时一次性适配。整批完成后对照蓝图第 4 步的剩余件（dirty 队列、局部事实存储、出边替换）各自立批。
