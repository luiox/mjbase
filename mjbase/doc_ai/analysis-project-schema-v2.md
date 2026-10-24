# 分析工程 schema v2 与编辑台账设计定稿

状态：本文件是 mjbase 侧 P0（schema v2 冻结）与 P2（台账引擎 + 压缩）的实现依据；
v2 全字段与台账行格式以本文为准冻结，实现代码（analysis_project、edit_ledger、
edit_ledger_store、workspace 换底）与之同步落地。上游依据：mjt 仓 RFC 0001
（分析工程 + 资源容器双层模型）§2/§3/§4/§6/§10/§11；实体 schema v1 见
analysis_project.hpp 头注；存储引擎见 project_store.hpp 头注。RFC §2.2 引用的
设计源 analysis-project-store-design.md 在私有仓，本仓无副本。

## 一、project.json v2（实体 schema）

### 1.1 版本策略：懒升级

version 写 2 当且仅当实体携带 v2 特性；否则恒写 1。v2 特性 = 任一 input 的
role ∈ {library, runtime}，或任一 input 携带非空 parent。理由：RFC §4.1 要求
bundle 内 project.json 对 #1096 冻结 v1 读者保持可读（零破坏边界）。懒升级把
零破坏窗口拉到最长——未用 v2 特性的工程被 v2 库反复打开、flush 都不产生 v1
读者读不了的字节。parse(write(parse(x))) 恒等在两个版本各自成立：v1 文本读入
（无 v2 特性）写出仍是 v1 逐字节一致；v2 文本读入写出仍是 v2 确定序。

### 1.2 v1 读者遇 v2：严格拒载（裁决）

v1 读者（#1096 冻结版，版本门不识别 2 即拒）遇 v2 文件整文件拒载，本仓不改
v1 代码、不提供「读 v1 子集」降级。理由三点。第一，v1 读者字段严格（inputs
条目未知字段即拒载），v2 的 parent 字段本来就进不了 v1 解析器，「读子集」
等于给冻结版开洞，破坏其「严格性当场抓住 writer bug」的契约。第二，v2 语义
降级读会静默出错：role=runtime 与 primary/library 的解析优先级不同（RFC §5.2
角色化容器栈），挂载树改变条目寻址，按 Support 降级读会产出错误分析结果而无
任何信号，违反 fail-closed 纪律（同 project_store「宁可错判不可误 prune」句
式）。第三，迁移路径单向显式：升级用一次性 migrate（RFC §4.2 第 3 点），拒
绝自动降级写；老二进制拿到明确报错好过静默误读。错误消息沿用既有版本门格式，
supported 标注 1..2。

v2 读者读 v1 文件全量支持（向后兼容）：version 1 或 2 都载入；version 1 文件
中出现 v2 值（role=library/runtime 或 parent 非空）按 Schema 拒载——版本门是
兼容契约的边界，version 标 1 就必须真是 v1。

### 1.3 role 扩展

v2 role 枚举 = primary | support | library | runtime。v1 的 support 保留为
合法值且写出保真（v1 文件原样回写仍是 support、仍是 version 1），不静默归一
成 library——静默改值会破坏 v1 往返恒等。新代码（mjt 波次 5 起）用
library/runtime；support 只来自遗留文件。语义：primary = 主分析对象；
library = 依赖库（解析序在 primary 之后）；runtime = 运行时镜像即 JDK
（解析序最后）；support = 遗留辅助参考，语义等同 library 但不参与角色化解析
栈排序的新契约（RFC §5.2 解析顺序 primary > library（挂载序） > runtime，
support 由上层按 library 处理并在登记时提示重算为 library）。

### 1.4 挂载树 parent 字段

inputs 条目新增可选字段 parent（字符串，引用同文件内另一 input 的 path）；
空 = 顶层挂载。校验：parent 引用的 path 必须已登记；parent 链不得成环（含
自环）。嵌套容器的派生字节缓存于 sections/（opaque，RFC §5.3），本 schema 只
冻结登记结构，派生缓存归后续批次。

### 1.5 容器 digest 口径：sha256（裁决）

容器与条目身份 digest 统一 "sha256:" + 64 hex 小写，与 store content_hash 同
形。现状两套口径（workspace 容器指纹 fnv1a64:16hex、store 校验 sha256:64hex）
并存的问题与切换动机见 RFC §3 决议与风险 4。落点三条。第一，台账引擎（本设
计第二、三节）的 base_container 与 entries 前后 digest 一律 sha256，由引擎强
制校验前缀与长度。第二，工程登记（project.json 的 fingerprint）库层保持口径
不解释（v1 起契约），但 v2 登记一律写 sha256；workspace 侧新增的容器指纹计
算（canonical 序列化 + SHA-256）按 1.6 口径。第三，fnv1a64 遗留前缀兼容读：
读到 fnv1a64 前缀的登记照常载入（不拒载、不静默重算），由上层提示重算；不
自动迁移（RFC 风险 4：不静默降级）。

### 1.6 容器 canonical 序列化与容器 digest（本设计新增口径）

容器态的规范字节序列：把容器全部条目按 uri 字典序排列，逐项拼接
len(uri, u64 小端) + uri 字节 + len(payload, u64 小端) + payload 字节。容器
digest = 对该序列求 SHA-256 加前缀。压缩后的基线 section 字节 = 该序列本身
（见 3.2），故基线容器 digest 恒等于其 section 的 content_hash，一个口径两处
验证。workspace 侧遗留的 fnv1a64 容器指纹不改动（addSupportingJar 去重等内
部消费照旧），工程登记与台账面走新口径。

## 二、编辑台账（edit-ledger）行格式 v1（冻结）

### 2.1 行模型

edit-ledger section 为 jsonl，每行一个自描述 JSON 对象，行级版本字段 v=1
（RFC 风险 1 缓解：行级演进不触清单版本）。三种行类型，字段严格（未知字段、
重复字段、类型错、digest 形状错即整段拒载）：

事务行（row=txn）：

    {"v":1,"row":"txn","txn":7,"parent":6,"label":"rename Foo->Bar",
     "base_container":"sha256:…","entries":[
       {"uri":"com/x/A.class","before":"sha256:…","after":"sha256:…",
        "op":"replace","payload":{"kind":"overlay","ref":"overlay-7-0"}}],
     "produced_by":"mjt edit_apply rename","created_at":"2026-10-23T00:00:00Z"}

锚行（row=anchor，压缩产物，见第三节）与书签行（row=bookmark）：

    {"v":1,"row":"anchor","txn":5,"baseline":"edit-baseline-3",
     "container":"sha256:…","previous":{"txn":2,"container":"sha256:…"},
     "compacted_txns":3,"created_at":"…"}
    {"v":1,"row":"bookmark","name":"sp1","txn":7,"created_at":"…"}

字段口径：txn ≥ 1 全局单调（被放弃分支的事务号不复用）；parent = 0 表示链根
（基线态），否则必须指向同段内存在的事务行；entries 按 uri 字典序；op ∈
replace|add|remove；before/after 为 "sha256:"+64hex，add 的 before 与 remove
的 after 恒为空串；payload 仅 replace/add 携带，kind 恒 overlay（未知 kind
拒载——引擎解不开的载荷不能假装能重放），ref 为 overlay section 名；锚行
previous 仅在非首次压缩时携带（补链：新基线 digest → 上一基线 digest → 原始
容器登记指纹的追溯链，RFC §6.4「新基线 digest + 被压缩区间记录」）。

### 2.2 活动链与分支

状态 = 基线容器字节 ⊕ 沿 parent 链重放到头事务。undo = 头指针沿 parent 回退；
redo = 头指针前进到被记住的 redo 目标；在历史中段提交新事务时，原尾部分支被
放弃（对齐旧实现 commit 清 redo 栈的语义），放弃分支的事务行不落盘、其事务号
不复用。落盘的活动链恒为线形：锚行（若有）+ txn 递增的事务行 + 书签行。

### 2.3 条目 digest 链完整性

追加事务时引擎校验每个条目的 before 与「该 uri 在链上的上一次 touch 的
after」一致（无 touch 则等于基线中该 uri 的 digest）——重放确定性由此锁定，
写入方算错当场拒绝而非落成明日 Damaged。

## 三、落盘形态：section 命名、代际与 flush 协议

### 3.1 section 命名（代际化）

台账引擎占用的 section 名族：edit-ledger-\<n\>（jsonl，台账第 n 代）、
edit-baseline-\<n\>（opaque，第 n 代压缩基线的 canonical 序列化字节）、
overlay-\<txn\>-\<seq\>（opaque，单条目 after 字节）。活动代 = 清单中
edit-ledger-\<n\> 的最大 n。同号配对只在压缩落点成立：后续 flush 只换代
台账、不动基线，锚行引用的 edit-baseline-\<k\>（k ≤ 活动代 n）跨 flush 代
保持引用，直至下次压缩才换代。装载不走名字同号配对，走锚行 content_hash
双门：基线 section 字节的 SHA-256 必须等于锚行 container digest，不等 =
Damaged 拒开；无锚行（纯事务链）时基线取原始容器（工程登记的输入，不可变
红线保证其字节恒可重读）。台账与基线 section 永不覆写已登记字节；overlay
是唯一例外，见 3.4 的名字相撞裁决。

### 3.2 flush 协议（manifest last）

pending 状态（未提交事务的 overlay 字节与台账行）驻内存。flush 序：逐条
writeSectionBytes 落 overlay section（新名，孤儿安全）→ 新代 edit-ledger-
\<g+1\> 全量写出（活动链 + 书签）→ 一笔 manifest 事务 upsert 全部新 section
→ 成功后释放内存 pending。崩溃任一点：清单未换 = 旧代完整，新文件是
AfterFirstFlush 档孤儿（既有恢复扫描 + pruneOrphanSections 收敛）；重试同字
节幂等。flush 不写基线（锚行跨代引用基线，见 3.1）；待释放清单随本事务
一并退役，相交名以重写收场（见 3.4）。

### 3.3 打开与恢复

打开时读清单定位活动代，readSectionBytes 校验后整段解析台账；任一行违约 =
Section 错拒开（fail-closed：台账损毁宁可拒绝打开也不丢编辑历史，基线与工程
实体不受影响）。清点本引擎名族下未被活动代引用的已登记 section 与全部孤儿
（含上次崩溃残留），登记为待释放清单，在下次 flush/压缩事务中随 manifest 删
除；活动代台账文件恒在引用面（空链会话亦然——它是当前链的落盘载体，换代
只发生在下次 flush/压缩事务里）。未登记文件交给既有 pruneOrphanSections。

### 3.4 删除纪律：只删非活动代

任何 manifest 事务的 removals 只指向非活动代 section（活动代只能被「注册更
新代」退役），且与 upserts 不相交（同事务又删又写是矛盾变异，清单事务拒
绝）。名字相撞裁决：新会话采纳空链或压缩后链时事务号自 1 重新生长，新
overlay 名可能与待释放名单中的旧名相撞——相撞名以 upsert 重写收场、不进
removals（旧名在待释放名单意味着已无链引用，覆写只伤已退役数据，崩溃窗不
伤活动链；「永不覆写」纪律仅余台账与基线两个例外面，见 3.1）。主事务提交
后待释放名单即清账，旧代台账/基线与失引 overlay 并入提交后的独立清理事务
退役；已被主事务消费的名字不再二次墓碑。墓碑先删文件后提清单的崩溃窗因此
只伤已退役数据：清单回读出的活动代恒完整。被跳过（崩溃）的删除由下一轮
flush 重试，幂等。

## 四、书签、undo/redo、revert 的台账语义

savepoint = 书签（名字 → 事务号），零字节代价，随 flush 落盘为书签行（跨
flush 存活是相对旧克隆实现的声明语义变化，RFC 风险 3 点名）。restore = 反向
应用头到书签之间的事务得到候选容器，再走一次普通提交（记一条反向事务，undo
窗不受损）。undo/redo = 头指针回退/前进，不记新事务；提交于中段历史时放弃尾
分支并清 redo 目标，指向被放弃分支的书签随之失效（旧克隆模型书签不随分支失
效，此为换底的声明语义差异，restore 得 NotFound 同形错误）。revert_class =
取该 uri 原始字节走普通提交（before=当前 digest，after=原始 digest），台账面
表现为「沿链反向归一到基线字节」的新事务；若该 uri 从未被 touch 则空 diff
零提交，与旧实现 revision 不动一致。

## 五、台账压缩（compaction）

### 5.1 保留点

保留点 P：基线推进到的事务号，压缩后 state(P) = 新基线，活动链截为
[P, head]。约束集：P ≥ 当前锚事务号；P ≤ 最老存活书签事务号（自动压缩书签
零失效的充要条件——书签 ≥ P 才可从新基线重放达到）；undo 深度 = 链上 [P,
head] 事务行数。

「undo 深度不缩短」的可操作形式化：RFC §6.4 字面（压缩前后可用步数不减）与
基线推进互斥——推进超过旧锚必然截掉最深历史。本设计将其落为：自动压缩后
undo 深度不低于保留参数 min_undo_depth，且自动压缩绝不触碰书签存活；窗口深
于策略承诺的部分是否收敛由保留点决定，压缩报告列明被压缩区间。手动压缩显式
携带 keep_undo_depth（允许 0 = 压到书签约束允许的最晚处）。

### 5.2 手动压缩

compact(keep_recent_bookmarks, keep_undo_depth)：先按 keep_recent_bookmarks
裁书签（按创建序保留最近 N 个，被丢弃者列名进报告——显式丢弃是手动路径专属
能力），再取 P = clamp(head − keep_undo_depth, 当前锚, 最老存活书签事务号)，
从「基线 ⊕ 重放到 P」物化新基线（调用方物料化 canonical 字节，引擎校验其
SHA-256 即锚行 container）。

### 5.3 自动压缩

触发点：flush 与 savepoint 之后。阈值：活动事务行数 ≥
compact_max_transactions，或台账行字节 ≥ compact_max_ledger_bytes，或距上次
压缩距离（head − 锚）≥ compact_max_distance；任一命中且保留点可推进
（5.1 约束集非空）才执行，报告 skipped 与理由。auto_compact=false 整体关
闭。阈值入 store.json 顶层可选字段 ledger_config（顶层只增不删纪律：旧读者
忽略，缺省 = 内置默认；auto_compact 默认 true，compact_max_transactions
256，compact_max_ledger_bytes 4 MiB，compact_max_distance 512，
min_undo_depth 64）。config 结构内部宽松（未知键忽略、写出丢弃）——引擎私
有调参，误留旧键不值得拒载整个清单。

### 5.4 压缩步骤与崩溃安全

序：物化 state(P) → writeSectionBytes 落 edit-baseline-\<g+1\>（孤儿安全）→
writeSectionBytes 落新代 edit-ledger-\<g+1\>（锚行 + 保留行 + 存活书签行）→
manifest 事务 upsert 新对并顺带退役既有待释放名单（相交名以重写收场，3.4）
→ 内存换锚换代 → 旧代台账/基线与仅被截掉区间引用的 overlay 进入待释放清
单，随提交后的清理事务删除（只删非活动代，3.4）。任一步崩溃：清单未换 =
旧基线 + 旧台账完整（RFC §6.4 红线），新文件归孤儿收敛。

### 5.5 补链

锚行记录新基线 digest、被压缩区间事务数与 previous（上一锚事务号 + 上一基线
digest）。导出条目的 digest 追溯链：entries 前后 digest → 头事务 → … → 锚
行 container（新基线）→ previous.container（旧基线）→ … → 原始容器登记指
纹，与基线 section 的 content_hash 双重可验。

## 六、workspace 换底契约（mjt 波次 2b 接线注意点）

WorkspaceBase 公开签名不变（undo/redo/restoreSavepoint/createSavepoint/
revertClass 同形同错误面），实现从整容器克隆换为台账头指针与反向应用。
revision_ 语义不变（提交/undo/redo/restore 后递增的全局计数，与台账事务号无
涉）；事务号由引擎独立单调分配。新增两个公开面：bindLedgerStore(bundleDir)
（绑定工程 bundle：开锁、装载活动代、采纳在册链与书签、工作副本物化为
基线 ⊕ 重放）与 flushLedger()（3.2 协议；无绑定 = no-op）。导出路径
（exportJar/exportClass）前置 flushLedger；打开新主资源清空会话史（旧语义）
且若绑定向先 flush 旧容器台账再换基线重开链。savepoint 跨 flush 存活、指向
被放弃分支的书签失效两项是已声明的语义差异（第四节）。digest 面：mjt 侧登
记容器与台账 base_container 一律走 1.6 sha256 口径；遇到 fnv1a64 遗留登记
提示重算，不静默。
