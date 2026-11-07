.. _refs-id-design:

refs 目标侧 id 化设计（analysis-db schema v5）
==============================================

状态：定稿（2026-11-05 状态回填）。schema v5 按本文 DDL 落地（现 schema 已随后续
批次递进），对抗语料 PoC 数字见本文 §5.1；消费随 luiox-repo 包词条推进。
analysis-db 总体设计与身份模型见 :ref:`analysis-db`\ 。原稿 2026-11-02。

1. 现状与问题
-------------

refs 表是分析事实层的直接引用存储。source 侧已按稳定 id 归一（source_class_id 外键 classes、source_member_id 外键 members），target 侧按名字 TEXT 三元组内联存储（target_class / target_name / target_desc），refs_target 索引按 (ref_kind, target_class, target_name, target_desc) 携带同一份全文。写入入口 replaceReferences 是单事务全量替换，来源类必须已登记，目标不校验登记状态——未入库的外部符号引用（JDK、supporting jar）靠这一点优雅表达。

名字 TEXT 键控的代价在两个实测场景被量化。性能实测批（rt.jar 19793 类 / 2.32M 引用；jdk17.jar 26637 类 / 3.49M 引用）测得 db 体积约为容器的 4.7-8 倍，refs 按名字 TEXT 键控是主因，id 化预期再省 30-50% 库容与相应查询内存流量。对抗实测（空格名混淆 jar MioInstaller，261 类 / 57116 引用 / db 373MB）显示混淆器用几万字符的空格名做字段名：17188 条字段引用 × 约 8.7KB 名字使 refs.target_name 原始文本达 147.8MB，refs_target 索引携带同样全名再翻倍，库膨胀约 230 倍（db 373,846,016 B 对 jar 本体 1.55 MiB，PoC 实测）。极端名字把 TEXT 键控的代价从线性放大到武器化。

本设计据此定案并修正了「等实测 db 体积再定」盲区：对抗样本的膨胀主体在 target_name（成员名）而非 target_class（owner 类名），只加 target_class_id 解决不了武器化场景，目标侧 id 化必须覆盖成员目标。

2. 候选方案
-----------

2.1 方案 A：可空目标 id 外键（定案）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

refs 增加两个可空外键列：target_class_id（引用 classes）与 target_member_id（引用 members），不引入第三套 id——复用身份层既有稳定 id。id 列只表达"写入时该目标已登记"这一登记状态投影：classes.internal_name 全局唯一、members 按 (class_id, kind, name, descriptor) 类内唯一、身份只增不删、重命名只走 renames 覆盖层不改 internal_name，所以 id 与名字互为冗余编码，可由 JOIN 互相重建且映射永不漂移。

目标整体二元化：Type 引用要么 owner 走 id、要么 owner 走文本；Field/Method 引用要么 owner 与成员都走 id（文本三列清空）、要么整体走文本（owner 已登记但成员未登记的少数派也整体走文本）。整目标二元化避免"owner 走 id、成员走文本"的混合行——那种行既进不了 id 索引也进不了文本索引，会从两个查询分支同时漏掉。

方案 A 的"仅 target_class_id"最小形态不成立：它只消 owner 类名文本，成员名与描述符文本原样保留，Mio 场景 147.8MB 的 target_name 一字不动。按对抗实测数据必须扩成 target_class_id + target_member_id 的完整形态。

2.2 方案 B：名字字典表
^^^^^^^^^^^^^^^^^^^^^^

全部 target 名字进 intern 字典（names 表：name_id 加 text 唯一），refs 存字典 id，登记与未登记目标统一处理，未登记目标的重复文本同样被去重——这是 B 对方案 A 的真实优势。代价有四：其一，SQLite 行表上的 text 唯一约束意味着文本在表与唯一索引各存一份，字典侧文本双份；其二，refs 全量替换与计划中的按方法切片替换都会产生孤儿字典行，需要额外的回收纪律（切片替换后全量回收代价 O(refs)，只能推迟到 checkpoint 等时机）；其三，查询输出要额外 JOIN 字典重建文本；其四，对抗下限劣化——已登记目标的成员名与 members 表各存一份，Mio 场景的诚实下限从"members 单份原文"劣化为"members 加字典双份原文"。

2.3 混合形态
^^^^^^^^^^^^

target_class_id 外键与 target_name/target_desc 进字典的组合不采纳：owner 列拆成 id 与字典 id 双轨、目标索引随分裂、两套机制各自带来约束与回收负担，复杂度不成比例。另注意 A 与 B 在叠加方向上不对称：A 先行不排除日后补字典——字典可以只接管文本分支的未登记文本，id 分支与表结构不动；B 先行则立刻为已登记目标付出字典与 members 的双份文本。当前没有实测证据表明未登记目标文本主导库容，按"不预先过度设计"的既定纪律，先 A，叠加 B 留作实测触发的后续项。

2.4 结论
--------

定案方案 A 完整形态。在两处有实测数字的语料（性能实测批基准语料、对抗样本）上，A 的收缩量都不劣于 B；A 不新增表、不引入回收纪律、对抗下限最优；B 的差异化收益（未登记目标去重）依赖尚无实测的语料形态，且可作为增量层后补。

3. schema 变更与迁移
--------------------

v5 的 refs DDL（草案）：

.. code-block:: sql

    CREATE TABLE refs(
      ref_id           INTEGER PRIMARY KEY AUTOINCREMENT,
      source_class_id  INTEGER NOT NULL REFERENCES classes(class_id) ON DELETE CASCADE,
      source_member_id INTEGER REFERENCES members(member_id) ON DELETE CASCADE,
      insn_index       INTEGER NOT NULL DEFAULT -1,
      ref_kind         INTEGER NOT NULL CHECK(ref_kind IN (0,1,2)),
      target_class_id  INTEGER REFERENCES classes(class_id),
      target_member_id INTEGER REFERENCES members(member_id),
      target_class     TEXT,
      target_name      TEXT NOT NULL DEFAULT '',
      target_desc      TEXT NOT NULL DEFAULT '',
      CHECK((target_class_id IS NULL) != (target_class IS NULL)),
      CHECK(target_member_id IS NULL OR ref_kind != 0),
      CHECK(target_member_id IS NULL OR (target_name = '' AND target_desc = ''))
    );
    CREATE INDEX refs_target_id ON refs(target_class_id, target_member_id)
      WHERE target_class_id IS NOT NULL;
    CREATE INDEX refs_target_text ON refs(ref_kind, target_class, target_name, target_desc)
      WHERE target_class_id IS NULL;

索引沿用清单：现有 schema 的 refs_source_class 与 refs_source_member 两条 source 侧索引本片段未列出，重建 refs 表时必须照抄——PoC 实测漏建后 referencesFromMember 从 0.575ms 退化为 1.672ms 全表扫。

行形状共四类：Type 引用登记目标 (class_id, NULL, NULL, '', '')；Type 引用未登记目标 (NULL, NULL, owner 名, '', '')；成员引用双登记 (class_id, member_id, NULL, '', '')；成员引用未登记 (NULL, NULL, owner 名, name, desc)。三条 CHECK 与代码校验同源，形状之外拒写。refs_target_id 服务 id 分支（referencesToClass 前缀探 owner，referencesToMethod/Field 双列精确探），refs_target_text 服务文本分支且只携带未登记行——基准语料稳态下该索引近似为空，外部符号与对抗文本集中于此。

写入路径沿用 replaceReferences 的全量替换与校验合同：输入仍收文本三元组，来源必须已登记，目标自由；applyReferenceBatch 在既有 source 解析 memo 旁并列两组 target 解析 memo（owner 名到 class_id，(class_id, kind, name, desc) 到 member_id），owner 与成员共解析则整目标落 id，任一未命中整体落文本。每个不同目标至多两次唯一索引探测，批内 memo 后总量受不同目标数约束。头文件公开契约零变更：DbReferenceRecord 与全部方法签名不动，唯一可见变化是 kAnalysisDbSchemaVersion 递增为 5。

schema 版本 v4 递增到 v5，fail-loud 版本门纪律延续：打开时 user_version 连同 application_id 与 meta.kind 一并校验，不识别的版本拒绝打开，不提供自动迁移。旧库处置选重导入、不做原地 backfill：backfill 是写完即弃的一次性代码而测试面要长期背；消费面只有 mjt 的整库导入桥，重建即重跑同一入口；整库导入成本经调优批实测为 rt.jar 36.3s、jdk17.jar 73.6s，重建代价可接受；仓内已有"历史库重导入属预期"的先例。v4 已随包词条 0.0.10 发布（mjt 现钉 0.0.13），真实 v4 库存在，版本门拒绝后的重建动线（自动重导入或提示用户）由 mjt 层决定。

计划中的按方法切片替换（增量分析审查的遗留项）与本设计兼容：切片删除走 source_member_id 既有索引，插入与整库替换走同一条 id/文本归一路径；方案 A 无字典行，切片替换不产生任何回收义务。

4. 查询面影响
-------------

按源反查已确立"先解析 id 再索引下探"的两步走形态（referencesFromClass/FromMember 先解析 class_id/member_id 再下探 source 索引，调优批实测 565ms 降到 1.76ms）；本设计把同一模式对称扩展到目标侧。referencesToMethod/Field 解析 member_id 后走 refs_target_id 双列探（id 分支），再 UNION refs_target_text 文本三元组探（文本分支）——两分支按 ref_id 互斥（任一行只会是 id 形状或文本形状），UNION ALL 不重不漏；文本分支承接"写入时未登记、其后才登记"的存量行与始终未登记的外部符号。referencesToClass 解析 class_id 后前缀探 refs_target_id，叠加文本分支。referencesFrom* 只走 source 索引，零改动。referenceCount 与普查投影的引用计数按 source_class_id 统计，零改动。

输出侧 kReferenceSelectSql 增加按目标 id 对 classes/members 的两个 LEFT JOIN 并以 COALESCE 回退文本列，DbReferenceRecord 的字段与排序键不变。类型化 API 吸收全部变化：mjt 导入桥、基准 harness 与未来的 MCP 查询工具都不感知存储形态。出口层（census/TSV）读侧不改：census 行不含引用目标文本，TSV 渲染消费的是 census 投影与引用记录的显示字段，两者形状均未变。

目标文本列按字节语义对待：对抗语料实测 target 名含大量非合法 UTF-8 字节流，任何按严格 UTF-8 解码文本列的消费端（如 Python sqlite3 文本模式的默认行为）会整行读失败，全部消费面一律走字节 API 读取（验收见本文 §7）。

5. 收益边界与量级推算（推算未验证）
-----------------------------------

基准语料（rt.jar 以主容器整库导入，目标基本全部登记）：refs 目标文本按每引用约 90B 计（owner 名约 45B 加成员名与描述符），2.32M 引用连表带索引约 400MB；id 化后 id 行每引用约 8-10B 加双列索引，refs 侧收缩到约 40-50MB。整库体积预期落入实测批给出的 30-50% 区间偏上限，验收以同 harness 复测为准。

对抗语料（MioInstaller）：373MB 中 refs.target_name 原始文本与其索引占约 295MB，其余约 77MB 为 members 原文及其唯一索引加基面。id 化后该 17188 条字段引用（目标成员均在库内登记）整体落 id，db 预期残余约 75-90MB，膨胀从约 100 倍降到约 20 倍量级；残余主项是 members.name 原文与其唯一索引，即"原文入库"的诚实下限，继续压缩只能走显示与出口层（普查渲染的超长单元格折叠策略已覆盖该层）。

已知边界：未登记目标的文本成本原样保留。典型 app 加 JDK 运行时的真实工作面里，指向未登记类的引用份额按 40-60% 估，方案 A 的实收省幅会低于基准语料口径（推算未验证）。重开条件：实测证明未登记文本主导库容时，叠加方案 B 作为文本分支的 intern 层，id 分支与表结构不动。

5.1 对抗语料 PoC 实测（2026-11-03）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

实测流程与数字：v4 基线 db 373,846,016 B（jar 本体 1.55 MiB，膨胀约 230 倍），refs 57116 行（Type 29888 / Field 20279 / Method 6949），target_name 原始文本 155,007,342 B（Field 引用占 96.9%，最长单名 21845 B）；按本设计 v5 DDL 迁移（0.6s 加 VACUUM 0.3s）后 82.3% 行落 id 分支、17.7% 落文本分支（外部 owner 与未登记成员的诚实代价，实测文本分支仅约 0.95 MiB），db 缩到 9,162,752 B（1/40.8，膨胀约 5.6 倍），全量 57116 行按外键 join 回三元组与基线聚合指纹逐行一致，另随机 200 条逐行比对一致。

查询计时：切片查询（referencesFromMember 前置）0.575 → 0.095ms；已登记类目标页扫 248.5 → 1.6ms；成员目标页扫 37.8 → 0.4ms；未登记外部符号查询仍是全表扫（33.9ms），收益仅来自表缩。§5 原推算「Mio 残余 75-90MB、膨胀约 20 倍」偏保守一个数量级：推算按每引用线性携带名字外推，实际混淆器把 226 个不同巨名复用在几千条引用上，members 层天然 intern，原文入库的诚实下限远小于线性估计。

6. 不做什么
-----------

不改 renames 覆盖层语义：重命名仍只动 renames，refs 是原始编码事实，目标 id 与名字的映射不受显示名影响，referencesTo* 仍按内部名查询。不动 entry 与类身份层：entries/classes/members/interfaces 的列不字典化、不增删，super_name 与 interfaces 的 TEXT 保留（量级在个位数 MB，不值得为它引入机制）。不为省空间牺牲"未登记引用可表达"：文本分支是常驻能力而非过渡态，外部符号引用的表达面与现状完全等价。不做原地迁移工具：fail-loud 加重导入是唯一路径。不引入第三套 id：target_class_id/target_member_id 复用身份层既有稳定 id，不新增分配器，不改变"引用目标按编码名寻址、登记 id 是冗余投影"的事实语义。

7. 验收
-------

同 harness 复测 rt.jar 与 jdk17.jar 的库体积、导入耗时与代表查询，确认落在 §5 推算区间。referencesToMethod/Field/Class 与 referencesFrom* 在 id 化前后对同语料逐行一致，含未登记目标与登记时序交错（先写引用后登记成员）的构造用例。MioInstaller 语料回归库体积与全部查询路径。schema 门对 v4 库拒绝打开并给出可诊断错误。

迁移验收加两条：重建后 source 侧两条索引存在且代表查询 EXPLAIN QUERY PLAN 无全表扫（PoC 实测漏建回归 0.575 → 1.672ms）；对抗语料的目标文本消费端走字节语义读取，按严格 UTF-8 解码的实现必须判为缺陷。
