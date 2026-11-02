# 分析数据库设计（analysis-db）

状态：草案，2026-10-21。待评审，未落地。

本文定义 mjbase 分析数据库的存储模型、增量分析边界和编辑语义。目标是用一个可查询、可增量更新、崩溃安全的项目数据库替代 `project_store` 的整文件 section 模型；`project_store` 的可靠写入原语保留为自研引擎的备选实现和导入导出层。

## 1. 背景与目标

mjbase 的分析结果目前全部驻留内存：def-use、string flow、路径条件、调用图、字段访问图和引用索引都没有持久化面，反编译文本和 IR 也只活在会话里。`project` 模块只覆盖输入登记、批注和 shadow sidecar 引用；`project_store` 以整文件 section 为单位读写，适合归档，不适合在线查询和增量更新。mjt 侧尚未接入 `project`，`ProjectStore::open/flush` 也未落地，现在重构不需要背兼容包袱。

目标：

(1) 项目可持久化：分析结果、重命名、注释、书签和编辑工作副本在关闭后仍可恢复。(2) 随时可查询：打开项目不需要全量重分析；查询走索引，不解析整份 JSON。(3) 增量分析：改一个方法只重算该方法的局部事实；全局视图从缓存事实增量更新或重算。(4) 用户数据不丢：重命名、注释和编辑历史不因重分析、删除方法或版本切换而静默消失。(5) 单文件存储：运行期允许 WAL 临时文件，保存或关闭后收敛为单个项目文件。(6) 崩溃安全：任意写入中断后，数据库要么是旧状态，要么是新状态，没有半提交。(7) 单写多读：一个 MCP 进程写，多个查询或只读会话可以并发读。

非目标：

(1) 多用户协同编辑或网络数据库服务。(2) 把 SQL 暴露为公开 API；SQL 只是内部实现。(3) 替代 mjbase 的分析算法；数据库只负责持久化和索引。(4) 复刻 IDA 的 `.id0/.id1/.i64` 文件布局。

术语：

| 术语 | 含义 |
|---|---|
| 局部事实 | 只依赖单个方法体或类结构的分析结果，可按方法或类内容寻址 |
| 全局事实 | 跨方法的边、倒排、摘要和可达性，依赖多个方法 |
| 方法语义指纹 | 规范化方法 IR 的哈希；常量池索引替换为解析值 |
| 渲染指纹 | 语义指纹加调试信息和注解，供反编译等展示型 producer |
| 生成（generation） | 一次分析提交的状态编号；本地事实按内容跨代复用，读者通过事务快照看到一致状态 |
| settle | 处理 dirty 集合、更新全局视图并提交新 generation 的过程 |

## 2. 关键决策

数据库是项目事实源，`project.json` 和 shadow sidecar 降为导入导出格式。运行时不再用 JSON 文件承载批注、重命名或分析事实，避免双写分叉。

事实按三层存放：

(1) 身份与清单层：输入、类/成员身份、版本、分析配置和 generation。(2) 用户覆盖层：重命名、注释、书签、类型覆盖和编辑工作副本。(3) 派生事实层：局部事实 blob、全局边/倒排/摘要、按需计算的视图。

事实键使用方法语义指纹，不使用 class 或 jar 的字节哈希。一次方法体编辑会导致整个 classfile 重编码和常量池重排；用 class hash 会把同类的所有方法判脏。语义指纹把常量池索引换成解析值，只有真正改到的方法变脏。

局部事实按方法增量；全局事实是派生层。第一版在保存或查询需要时从缓存的局部事实重建全局视图，成本是 O(edges) 而不是 O(instructions)；热点路径再引入 worklist 增量传播。全量重建保留为正确性兜底。

存储引擎首选 SQLite 单文件，备选是 append-only segment log 加索引和 compaction。选型在 spike 后确认，见第 5 节。

## 3. 总体结构

项目运行时是一个 SQLite 数据库文件 `.mjdb`，打开期间有 WAL 和 SHM 伴随文件；保存或关闭时 checkpoint 回主文件，需要紧凑快照时用 `VACUUM INTO`。工作副本、用户覆盖和派生事实在同一个事务边界内协调。

```text
.mjdb
├─ meta / revisions / generations
├─ inputs / classes / members / class_versions / method_versions
├─ blobs                        工作副本与 payload 字节
├─ renames / comments / anchors 用户覆盖与锚点
├─ local_facts                  方法/类局部事实
├─ edges / summaries            全局事实与派生摘要
├─ dirty / runs                 增量队列与分析状态
└─ identity_map                 派生输入或外部改名后的身份映射
```

原始输入 jar 默认按路径加指纹引用，不复制进数据库；需要自包含工程时再选择打包输入。工作副本不直接改原始 jar，而是以类 blob 或补丁形式入库；导出时再生成新的 jar。这样分析目标始终是稳定版本，用户编辑是可回滚的覆盖。

查询与写入分离：一个读写连接负责编辑和分析提交，多个只读连接或只读进程读取最近提交的 generation。WAL 提供读不阻塞写，写不阻塞读。

## 4. 数据模型

### 4.1 身份

类和方法使用稳定 ID，名字和描述符是可变的显示属性。`class_id` 在类第一次进入项目时分配；`member_id` 在成员第一次出现时分配；方法的方法体版本另用 `method_versions` 记录。重命名只更新 `renames`，不改变 ID。

方法身份是类加名字加描述符。改名保留同一个 `member_id`；描述符变化视为删除旧方法并新增方法，旧方法上的注释转为 dangling，不静默迁移。类改名同理：`class_id` 不变，显示名走重命名覆盖。

如果输入本身被替换并导致名字变化，按类名、类指纹和方法体指纹尝试匹配；匹配成功的更新当前名字并保留 ID，匹配失败的新建 ID，旧 ID 进入 tombstone。成功的匹配写入 `identity_map`，供注释和重命名迁移。

### 4.2 指纹

`class_fp` 是整个 class 字节的 SHA-256，只用于输入版本识别和类级结构比较，不作为事实键。

`semantic_fp` 是方法语义指纹，计算口径：

(1) 方法签名、访问标志和异常表参与哈希。(2) 指令序列按规范化 IR 遍历；opcode、分支目标和异常范围归一为方法内指令序号。(3) 常量池操作数使用解析后的值：字符串字节、类名、成员引用、方法句柄、动态调用点及其 bootstrap 参数，不使用常量池索引。(4) max stack、max locals 和 Code 属性中影响语义的字段参与哈希。(5) 调试信息、注解和不影响语义的属性不参与。

`render_fp` 是渲染指纹，在 `semantic_fp` 基础上加入 LNT、LVT、LVTT、SourceFile、参数名和注解，供反编译文本、伪代码和展示型缓存使用。producer 声明自己依赖哪一种指纹，事实键随之选择。

事实键统一为 `(target_id, fingerprint, config_hash, producer, producer_version)`。`config_hash` 是分析选项、pass 集合和相关阈值的结构化哈希，不用自由文本 profile。producer 版本变化等价于配置变化，旧事实保留到 GC。

方法摘要不是纯局部事实：它依赖自身方法体和被调方法的摘要，不能只按自身 fingerprint 缓存。摘要条目额外记录 `deps_hash`（被调方摘要的哈希）或 generation 标记，避免被调方变化后错误命中旧摘要。

### 4.3 表

| 表 | 作用 | 关键列与约束 |
|---|---|---|
| `meta` | 数据库自描述与当前状态 | key 主键；schema_version、kind、current_gen、tool_version、config_hash |
| `inputs` | 输入登记 | input_id 主键；path、role、sha256、size、mtime；path 唯一 |
| `classes` | 类身份与当前版本 | class_id 主键；input_id、internal_name、current_class_fp、current_rev |
| `members` | 成员身份与当前名字 | member_id 主键；class_id、kind、name、desc；类内当前名字唯一 |
| `class_versions` | 类的工作副本版本 | (class_id, rev) 主键；blob_id、class_fp、source、created_at |
| `method_versions` | 方法版本历史 | (method_id, rev) 主键；class_rev、semantic_fp、render_fp、parent_rev |
| `method_state` | 方法当前状态 | method_id 主键；rev、semantic_fp、render_fp、summary_hash、analysis_state |
| `blobs` | 字节内容 | blob_id 主键；sha256、codec、bytes；sha256 加 codec 唯一 |
| `renames` | 显示名覆盖 | (target_kind, target_id) 主键；new_name、token、updated_at |
| `comments` | 用户注释 | comment_id 主键；target_kind、target_id、insn_uid、base_pc、insn_hash、text、author、timestamps、orphaned |
| `insn_anchors` | 指令锚点映射 | (method_id, rev, uid) 主键；pc、insn_hash、origin_uid |
| `local_facts` | 局部事实 | (method_id, semantic_fp, config_hash, producer, producer_version) 主键；codec、payload、created_at |
| `edges` | 全局边与倒排 | edge_id 主键；src_method_id、src_semantic_fp、src_pc、kind、dst_owner/name/desc、dst_method_id |
| `summaries` | 方法/字段摘要 | (method_id, kind, config_hash, producer_version, deps_hash) 主键；payload |
| `dirty` | 待处理集合 | method_id 主键；reason、since_rev、priority |
| `runs` | 分析任务 | run_id 主键；generation、config_hash、status、started_at、finished_at |
| `generations` | 分析状态与提交标记 | gen 主键；state、created_at |
| `revisions` | 工程版本 | rev 主键；parent_rev、kind、label、created_at |
| `identity_map` | 身份迁移 | (old_kind, old_id) 主键；new_kind、new_id、reason、created_at |

`local_facts` 按内容指纹跨 revision 复用；`edges` 和 `summaries` 是当前状态表，更新在写事务内按源方法替换。generation 是提交状态编号，不是全局表的物理分区；需要跨会话历史快照时使用只读备份或 `VACUUM INTO`，不复制整表。

`classes.current_rev` 和 `method_state.rev` 指向最后一次内容变化的 revision；未变化的版本不复制行，查询按当前指针取事实。`method_state.summary_hash` 指向当前摘要，摘要内容按 `deps_hash` 寻址，调用方摘要不变时可以复用。

索引按查询面建立：`edges` 需要目标三元组、源方法加源指纹、目标方法三个方向；`comments` 需要目标、方法加指令 UID 两个方向；`classes` 和 `members` 需要当前名字查找；`local_facts` 需要按方法、按 producer 和按指纹查找。大 payload 使用 `zstd` 压缩，codec 随行记录；小 payload 是否压缩由 spike 定阈值。

### 4.4 指令锚点

指令锚点以稳定 `insn_uid` 为主，`base_pc` 加 `insn_hash` 为校验和重定位辅助。原始输入的 pc 在基版本内稳定；编辑后新版本通过指令 diff 建立旧 UID 到新 UID 的映射，未改动指令保留 UID，被删指令的注释标记 dangling 但不删除，新增指令分配新 UID。

`insn_anchors` 记录每个方法版本中 UID 到 pc 和指令哈希的映射，`origin_uid` 指回原始输入中的指令。重新分析不改变锚点；方法体编辑才触发 diff 和映射更新。外部输入替换时用 `base_pc` 加 `insn_hash` 匹配，匹配不到的注释保留为 dangling。

指令序号只用于显示和窗口定位，不单独作为锚点。插入或删除一条指令会让后续序号全部漂移。

## 5. 存储引擎

### 5.1 SQLite 单文件

SQLite 提供 B-tree 索引、事务、崩溃恢复、WAL 并发读和单文件存储，覆盖本设计的全部硬需求。打开时数据库文件和 WAL/SHM 伴随文件并存；正常关闭或显式保存时 checkpoint 回主文件；`VACUUM INTO` 生成紧凑快照，`PRAGMA incremental_vacuum` 回收空闲页。

建议的初始配置：

(1) `journal_mode=WAL`，一个写连接，多个读连接。(2) `foreign_keys=ON`，`busy_timeout` 设为可接受的上限。(3) `application_id` 和 `user_version` 写入自描述与 schema 版本。(4) 初始 `page_size` 与 `auto_vacuum` 在创建库时定，之后不随意改。(5) 用户覆盖层使用更严格的同步级别；派生事实可以放宽，具体档位在 spike 中实测。

大 payload 用 zstd BLOB，边和倒排用普通行。查询通过预编译语句和类型化 API，不把 SQL 文本暴露给调用方。只读模式用于分享和并发查询；只读连接不触发分析。

WAL 依赖共享内存，不适合网络盘。第一版只支持本地盘；网络盘场景要么用 rollback journal 的只读副本，要么走导出文件，不承诺在线写。

### 5.2 备选：自研 segment 引擎

如果 SQLite 依赖不被接受，备选方案是 append-only segment log 加索引和 generation manifest。写入追加新 segment，索引记录键到 segment 偏移，提交时原子换 manifest，compact 时合并 segment 并删除未被引用的旧段。可以复用 `project_store` 的锁、原子换名和恢复扫描原语。

这个方案的控制力更强，不需要外部依赖，但索引、崩溃恢复、并发读和 compaction 都要自己实现。查询面越多，自研成本越高；如果要做，先把查询面收敛到方法事实、xrefs 和调用边三类，再逐步扩展。

不建议照抄 IDA 的文件布局。`.id0/.id1/.i64` 的本质是分页 B-tree、空闲页复用和打包压缩；SQLite 已经提供这些机制，没有必要复刻文件数量和页面格式。

### 5.3 选型结论

先做 SQLite spike，验证打开、查询、单方法编辑、GC、崩溃恢复和 Windows 本地盘行为。spike 通过后，`project_store` 的 section/manifest 运行时退役，只保留旧格式导入导出；spike 不通过或依赖被否决，再按 5.2 实现 segment 引擎。

## 6. 增量分析

### 6.1 事实分层

局部事实按方法或类键控，可由内容指纹直接命中缓存：

| producer | 作用域 | 依赖指纹 | 典型 payload |
|---|---|---|---|
| `def_use` | 方法 | semantic_fp | def-use 边、局部访问、字段访问事实 |
| `string_flow` | 方法 | semantic_fp | 字符串来源、编码管线、逃逸位 |
| `path_conditions` | 方法 | semantic_fp | 分支路径条件与死分支事实 |
| `statement_ir` | 方法 | semantic_fp | 语句树与语句到指令窗口映射 |
| `render` | 方法 | render_fp | 反编译文本、伪代码、展示缓存 |

这些 producer 的接口和数据形状见 [slot-def-use.md](slot-def-use.md)、[string-flow.md](string-flow.md)、[path-conditions.md](path-conditions.md)、[statement-ir.md](statement-ir.md)。

全局事实跨方法，按 generation 提交：

| 事实 | 依赖 | 持久化策略 |
|---|---|---|
| 引用索引与 xrefs | 输入类集合、类指纹 | 持久化边表；按源方法替换 |
| 调用图与调用点 | 类指纹、indy 解析配置 | 持久化边表和调用点；按源方法替换 |
| 字段访问图 | 引用索引、调用图 | 持久化摘要与倒排；按 dirty 传播重算 |
| 可达性与入口根 | 调用图、根配置 | 可按需计算；需要缓存时按 generation 存 |
| 调用链/调用树 | 调用图 | 按需计算的视图，不持久化 |

调用图与 indy 解析的语义见 [call-graph-views.md](call-graph-views.md)、[indy-resolution.md](indy-resolution.md)、[root-provider.md](root-provider.md)。

### 6.2 变更失效规则

| 变更 | 失效范围 | 处理 |
|---|---|---|
| 显示重命名 | 无 | 覆盖层更新，事实不动 |
| 方法体指令变化 | 该方法局部事实、出边、xrefs、字段访问、方法摘要 | 重跑该方法；替换贡献；摘要变化时传播到调用方 |
| 成员增删或描述符变化 | 该类、相关调用点解析、调用图 | 类级重建；必要时全局重建 |
| 继承/接口变化 | 类型层次、调用图、可达性 | 全局解析，重建受影响视图 |
| 外部输入被替换 | 变化的类及其全局贡献 | 按类 diff；能匹配的复用，匹配不了的类重建 |
| 分析配置或 producer 版本变化 | 该 producer 的全部事实 | 新 generation，旧事实保留待 GC |
| 仅调试信息变化 | 依赖 render_fp 的事实 | 展示缓存失效；语义事实不动 |

### 6.3 处理流程

一次方法体编辑按以下顺序处理：

(1) 编辑事务写入新的类 blob、方法版本、semantic_fp/render_fp、dirty 行，并重定位注释锚点。事务提交后，工作副本和新版本对外可见。(2) 局部分析 worker 取出 dirty 方法，重算该方法的局部事实，写入 `local_facts`。(3) 在同一个写事务里删除该方法旧指纹对应的出边、xrefs 和字段访问行，插入新行；这些行带 `src_semantic_fp`，便于按源方法精确替换。(4) 比较新旧方法摘要。摘要没变就停止传播；摘要变了，沿反向调用边把调用方标记为摘要脏。(5) settle 阶段重算 dirty 摘要和字段访问倒排；全局视图可以从缓存的局部事实重建，也可以按边增量更新。(6) 提交本批 generation，更新 `meta.current_gen`；已打开的读事务由 WAL 保持旧快照，跨会话快照用显式只读备份。

第一版只把方法体变化做成严格的方法级增量；摘要传播和全局视图采用重建缓存或简单 worklist。等实测证明重建成本不可接受，再补复杂的增量图算法。

### 6.4 一致性与 generation

增量结果必须与同一 revision、同一 config 下的全量重建结果一致。提供 `analyze --full` 作为正确性兜底和测试 oracle；随机编辑序列要验证增量结果与全量结果逐事实一致。

新分析以批为单位提交：一个写事务里更新局部事实或全局边/摘要，并递增 `meta.current_gen`。SQLite 事务保证读者看到提交前或提交后的完整数据库状态，已打开的读事务由 WAL 保持旧快照；状态里可能有 stale 标记，但不会出现半行或混合版本。generation 是提交状态编号，不是全局表的物理分区；跨会话历史快照用显式只读备份或 `VACUUM INTO`，不复制整表。

查询可以声明新鲜度：允许 stale 时返回上次提交的结果并带 stale 标记；要求 fresh 时等待 settle 或触发按需分析；需要一致快照时保持读事务或打开只读备份。MCP 层把 settle 做成后台 job，不用同步阻塞编辑。

## 7. 编辑、重命名与版本

重命名是覆盖层操作，不触发重分析。`renames` 记录目标 ID、新名字和 token，查询和展示时解析；分析事实始终按原始身份和语义指纹键控。导出时把重命名应用到类字节，生成派生 jar，并记录 `identity_map`。工程默认继续分析原始输入，不把派生 jar 当新的分析目标；用户显式切换输入时再走外部输入替换流程。

方法体编辑是真正的语义变化。编辑事务必须同时写入工作副本版本、方法指纹、dirty 标记和锚点映射，四者在同一个数据库事务里提交。分析事实的更新可以在之后异步提交，但查询要能看到 stale 状态。

结构性编辑扩大失效范围：成员增删、描述符变化、继承关系变化和类级别修改会改变解析结果，需要类级或全局重建。重建仍然从局部事实开始，只对受影响的解析面重算，不做无条件的全库字节码扫描。

外部输入替换按类 diff：类名和类指纹都相同的复用；内容变化的类新建版本并重分析；名字变化但方法体可匹配的写 `identity_map`；匹配不了的类新建 ID，旧注释保留为 dangling。旧类版本和未引用事实不立即删除，用户确认后再 GC。

工程版本轴复用现有 workspace 的 revision 和 savepoint 语义。每次保存产生一个 revision；工作副本的类 blob 和补丁入库；注释和重命名是当前覆盖层，不随 revision 回滚，除非用户显式做用户数据快照。内容寻址的局部事实在 undo 回到旧方法指纹时直接命中缓存，不需要重算。

## 8. 查询与 API

mjbase 暴露类型化 C++ API，不暴露 SQL。查询结果带稳定 ID、当前显示名、generation 和 stale 标记；调用方不需要知道表结构。

查询面分四类：

| 类别 | 典型接口 |
|---|---|
| 身份与展示 | 按名字/ID 查类、成员、方法；解析当前显示名；列重命名 |
| 局部事实 | 取方法 def-use、string flow、路径条件、语句树、反编译文本 |
| 全局事实 | 查 xrefs、调用者/被调者、字段读写方、方法/字段摘要、可达性 |
| 工程状态 | dirty 方法、stale 事实、分析任务、generation、revision、GC 状态 |

编辑面包括应用类编辑、设置/清除重命名、增删注释、保存、关闭、分析、settle、GC、导入和导出。所有错误走 `Result<T, ProjectError>`，不抛异常。

MCP 层增加项目级工具：`project_open`、`project_close`、`project_save`、`project_info`、`project_analyze`、`project_status`、`project_gc`、`project_export`、`project_import`。现有查询工具在打开项目后接受项目上下文；具体工具命名由 mjt 层决定，不进入 mjbase 契约。

只读打开用于分享和并发查询：不获取写锁，不触发分析，只读最近提交的 generation。只读连接遇到需要新分析的数据时返回 stale，不自行升级为写连接。

## 9. 与现有模块的关系

`analysis_project` 的 v1 JSON 解析和写出保留为导入导出面，负责把旧 `project.json` 迁入数据库。批注、输入和 shadow 引用导入后，运行时以数据库为准；导出可以生成 v2 JSON 或保留 v1 兼容格式。

`project_store` 的 section/manifest 运行时退役。`writeSectionBytes`、锁、原子换名和恢复扫描只在自研 segment 引擎时保留；SQLite 路线不需要在它之上继续实现 `ProjectStore::open/flush`。本文替代其运行时存储部分。

shadow alias 的 v1/v2 解析保留为导入导出；运行时的重命名事实进入 `renames`，sidecar 不再作为唯一事实源。`.ridx` 继续作为引用索引的整文件缓存，或者作为派生事实导入数据库；它不是用户数据，也不是主存储。

workspace 的 revision、savepoint、undo/redo 和 export 语义映射到数据库的 revisions 和 working copy。类编辑仍然走现有的 `CodeEditor` 和会话写回路径；数据库只负责把编辑结果、版本和锚点持久化。

各个分析模块增加持久化适配层：producer 名字、版本、指纹类型、编码器和解码器注册到事实注册表。分析算法不感知数据库，数据库也不解释 payload 语义。`module-map.md` 中 `project` 的状态在实现落地后从“已迁”改为“存储重设计”，避免继续把原型当完成面。

## 10. 实施步骤与验收

第一步，事实清单与查询负载。列出所有要持久化的 producer、作用域、指纹依赖、payload 体量、查询模式和更新频率；准备包含混淆 jar、大 jar 和多版本 jar 的基准语料。验收标准是每个事实都有明确的键、失效规则和查询入口。

第二步，存储引擎 spike。用 SQLite 建最小 schema，导入 shadow alias 和批注，写入一个局部事实（def-use）和一个全局边表（调用边），验证打开、查询、单方法编辑、GC、崩溃注入和 Windows 本地盘行为。验收标准是单方法编辑不重写全局表，打开不重新解码字节码，崩溃后数据库一致；同时记录库体积、打开时间和查询延迟，决定是否继续 SQLite。

第三步，身份、指纹与锚点。实现稳定 ID、semantic_fp/render_fp、重命名覆盖和指令锚点。验收标准是常量池重排和无关方法重编码不改变指纹，改一条指令只有一个方法 dirty，重命名零重分析，注释在方法编辑后仍能定位或标记 dangling。

第四步，局部事实与增量流水线。实现 producer 注册表、dirty 队列、局部事实存储和出边替换。验收标准是随机编辑序列下局部事实与全量重建一致，查询不会在未标记的情况下返回 stale 事实。

第五步，全局事实与 generation。实现调用图/字段访问图的持久化、摘要传播、settle 和 generation 切换。验收标准是增量结果与全量重建逐事实一致，摘要未变化时传播提前截断，读者只看到完整 generation。

第六步，MCP 集成与迁移。实现项目打开/关闭/保存/导出/导入、旧 `project.json` 导入、shadow 导入和 MCP 工具接线。验收标准是端到端打开项目、查询、编辑、重开、导出单文件；注释和重命名在重分析、删除方法、undo 后仍可查询；GC 后无未引用 payload。

构建和测试沿用仓库纪律：delete-then-rebuild，一次构建一个 target，测试输出不提交。分析数据库的测试需要覆盖崩溃注入、随机编辑、并发只读和旧格式导入，不能只测正常路径。

## 11. 开放问题

| 问题 | 当前倾向 | 需要确认 |
|---|---|---|
| SQLite 依赖 | 首选，单文件 C 库，消费方是项目数据库 | 是否符合公开仓依赖政策 |
| 网络盘 | 第一版不支持在线写 | 是否需要只读副本或导出方案 |
| 方法语义指纹 | 解析常量池操作数，indy/condy 纳入 bootstrap 参数 | 规范化细节和实现成本 |
| 全局增量 | 第一版从缓存局部事实重建，后续 worklist | 重建延迟是否可接受 |
| 外部改名匹配 | 类名、类指纹、方法体指纹组合匹配 | 匹配失败时的用户交互 |
| 分析状态与快照 | SQLite 用事务快照和显式备份，自研引擎用 manifest 代 | 默认保留几代、何时 GC |
| 工作副本存储 | 类 blob 入库，原始输入按引用 | 是否需要自包含输入打包 |
| 已删指令的注释 | 保留为 dangling，展示在孤立注释视图 | UI 是否需要自动迁移 |
| schema 迁移 | `user_version` 加顺序迁移，不承诺降级 | 公开仓首个稳定版本的兼容窗口 |
| 事实编码归属 | 各 producer 自带编码器和版本 | 注册表接口和版本升级规则 |

## 12. 验收不变量

(1) 同一 revision、同一 config 下，增量分析结果等于全量重建结果。(2) 单方法编辑只重算该方法的局部事实；全局视图不重新扫描全部字节码。(3) 显示重命名不触发任何分析重算。(4) 用户注释和重命名在重分析、删除方法、undo 和外部输入替换后仍可查询，无法定位时保留为 dangling。(5) 崩溃注入后数据库要么是旧 generation，要么是新 generation，没有半提交。(6) 保存或关闭后主文件可独立打开；GC 后没有未引用的 payload。(7) 查询走索引，不解析整个事实集合；stale 结果必须显式标记。(8) 只读连接不获取写锁，不触发分析，能并发读取已提交 generation。
