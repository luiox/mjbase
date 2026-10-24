# 分析缓存落盘与多容器合并视图设计注记（P3 + P5）

状态：本文件是 mjbase 侧 P3（缓存落盘）与 P5（多容器合并视图 + 复合寻址扩展位）的实现依据，随代码同批落地。上游依据：mjt 仓 RFC 0001（分析工程 + 资源容器双层模型）§4.1/§5/§7/§10-P3/P5/§11 风险 6；工程登记结构见 analysis_project.hpp schema v2（doc_ai/analysis-project-schema-v2.md，其 §6 mjt 接线契约不受本批影响）；存储引擎原语见 project_store.hpp 头注。

## 一、缓存 section 布局与失效键（P3）

实现为 `mjbase::project::AnalysisCacheStore`（analysis_cache.hpp/.cpp），两族缓存 section，失效键完全沿用 store.json 既有字段，不自造体系。

**失效键映射**：`depends[]`（参与容器的 path+fingerprint 全集）+ `produced_by`（引擎与引擎版本自述串，如 "mjt decompile quiltflower/2.15.1"）+ `section_version`（缓存格式版本）+ `profile`（影响内容的 options 串，空串 ≡ 未设置，沿 R52/CP-3 归一口径）。命中判定 = 名字槽定位 + 清单条目四字段全值复核 + readSectionBytes 校验读（先 size 后 hash）；任一环节失配按未命中处理（缓存可重算，永不值得为它报错），清单不可载（isManifestUnloadableKind）除外——读路径报错、写路径拒绝，与台账引擎同一 fail-closed 纪律。

**section 名**：xref 缓存 `xref-<setd8>-<p8>-v<ver>`（opaque，整索引一槽；setd8 = depends 全集 canonical 序的 SHA-256 前 8 hex，p8 = profile 哈希前 8 hex）；decompile 缓存 `decompile-<cd8>-<e8>-<p8>-v<ver>`（jsonl，cd8 = 容器 digest 十六进制体前 8 位，e8 = produced_by 哈希前 8 hex）。名字只作槽定位，全值复核防短 digest 相撞；版本进名字使新旧格式各占一槽互不覆写。

**条目级 decompile 缓存**：槽 =（容器 digest、引擎与引擎版本、options profile）；记录行 `{"v":1,"class","entry","source"}` 严格解析（未知/重复字段、digest 形状错即段不可解释）。条目级粒度由记录的 entry digest 承担——条目未变即命中，编辑一个类只失效该类；槽名用登记口径的原始容器 digest（原容器不可变红线保证槽名稳定），工作副本条目 digest 进记录键。

**写路径与崩溃安全**：一切字节经 writeSectionBytes 原子写（staged tmp + 单次换名），清单事务 manifest last；崩溃窗留下的新文件是 AfterFirstFlush 档孤儿，由既有 scanBundleRecovery + pruneOrphanSections 收敛；Damaged 缓存段由「未命中重算 + 同名重写」自愈（§8.2-3 既有口径）。

**合并 flush（RFC 风险 6 的裁决）**：条目级 decompile 缓存的 put 一律驻内存 pending，flush 按槽分组并写（已登记槽旧记录合并、pending 同名覆盖），全部槽一笔 manifest 事务提交——N 条目/N 槽只付一次清单事务与一次换名重试窗，高频写场景不再有 3×50ms 耗尽面。xref 缓存是整索引重算的低频事件，即时单 section 单事务落盘。本设计把风险 6 的合并 flush 落为常态形态而非 TODO。

**陈旧槽退役**：xref 随 storeXref 退役同引擎（同 produced_by）的旧槽；decompile 随 flush 退役同容器同引擎（decompile-\<cd8\>-\<e8\>- 词根）的旧槽——同容器换引擎/profile/版本即换槽，旧槽随同一笔清单事务墓碑。异引擎槽互不干涉。

**mjt 接线契约（下一张卡注意点）**：其一，本类不获取 store.lock——单写者锁由工程会话持有（如 EditLedgerStore 绑定），缓存写路径（storeXref/flush）的前置条件是调用方已持锁，读路径无锁短句柄。其二，容器 digest 一律 sha256 登记口径（schema v2 §1.5/§1.6），decompile 槽键的 containerDigest 即工程登记 fingerprint；遇 fnv1a64 遗留登记由上层提示重算。其三，flush 挂点建议与 flushLedger 同点（工程关闭/显式 save/export 前），使缓存与台账同一崩溃窗语义。其四，decompile 记录的 entryDigest 是反编译时刻类字节的精确 SHA-256（ledgerDigestOfBytes 同口径），从工作副本实际字节算出。其五，一切查询现读清单不缓存清单视图，内存化索引是后续优化不是契约。

## 二、多容器合并视图（P5）

实现为 `mjbase::model::MountedContainerSet`（container_stack.hpp/.cpp），model 层能力：只认「容器 + 角色 + 稳定 id」，不知道 project.json——schema v2 登记（AnalysisInput 的 role/parent）到挂载的接线归上层消费方（workspace/mjt），本批不改 WorkspaceBase 公开签名（schema v2 §6 契约不动）。

**关键裁决（与 first-wins 语义的交互）**：解析序 = primary > library（按挂载序）> runtime（按挂载序）；同 URI 多容器出现时保留解析序首位条目，其余逐对记录进 shadowedEntries（winnerId + shadowedId），与 ResourceContainer::addResource 的 JVM first-wins 纪律（issue #962）同源。容器对象借用（非拥有），所有权归调用方。挂载/卸载都重建解析序与重复记录——卸载 = 重建，被遮蔽副本随之浮现，这是 JVM 移除 classpath 条目后下一副本可见的同款语义，卸载后引用一致性由重建保证（不维护增量遮蔽表）。

**可见性与查询**：visitRole 按角色过滤遍历（primary/library/runtime 可见性区分的查询面）；mergedView 按解析序把条目克隆进新 ResourceContainer（addResource first-wins + duplicateResourceUris 记录，resource_container.hpp 现状语义），产出直接喂既有索引面（ReferenceIndex::build、hierarchy、ReferenceScan 等）即 xref/索引跨容器归并——索引与扫描面不感知容器边界，这是「合并视图一个面、索引零改动」的裁决。

## 三、复合寻址扩展位（本期只落接口）

条目寻址统一为挂载链地址 `<containerId>!/entry/path`（ MountedContainerSet::kAddressSeparator）。本期实现：mountAddress 装配 + resolveAddress 单段解析（按首段分隔符拆容器 id，条目必须在该容器内命中，地址钉死容器不做跨栈 first-wins）。显式不做：entryUri 残留 `!/` 的嵌套钻井（WAR 内 JAR / fat jar 嵌套 jar / JMOD / JIMAGE）按 BadArgument 拒绝，fail-closed 不静默误拆。

后续扩展路径（TODO(composite-addressing)，落点 container_stack.cpp resolveAddress）：按 bang 链逐段下钻，每段解析为子容器、末端段为条目；嵌套成员作为派生容器缓存于 sections/（opaque），父容器 digest 变化即失效（RFC §5.3 的缓存化方案，与 §一 的槽机制同构，可复用 decompile/xref 的槽退役词根纪律）；multi-release JAR 条目按 JVM 规则归并版本桶（META-INF/versions/N 取 ≤ 目标版本的最高 N），索引 section 记录归并结果，编辑寻址带版本桶限定符（与 dev_plan P0-6 挂账同题）。工程登记侧的挂载树 parent 字段已在 schema v2 冻结，本扩展不触登记结构。
