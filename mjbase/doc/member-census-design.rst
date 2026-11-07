.. _member-census-design:

member 普查面设计（censusMembers + member_census.tsv）
======================================================

状态：部分落地（2026-11-05 状态回填）。库侧投影已落地：DbMemberCensusRow 与
``censusMembers``\ （#150）；TSV 渲染与词条批随消费方节奏推进。analysis-db 总体
设计见 :ref:`analysis-db`\ ；类级普查契约见 ``analysis_db`` 的 censusClasses 与
mjt 侧 class_census 渲染层（schema v3，#131/#142）。原稿 2026-11-03。

1. 现状与问题
-------------

现有普查出口只有类级：mjbase 的 censusClasses 投影一行一类（形态、计数、条目关联、来源前缀），mjt 渲染为 class_census.tsv（schema v3，含 origin 列）。成员名字面的武器化在类级普查里不可见：混淆 jar 实测（261 类）有 148 个空格命名字段，其中 62 个字段名长 21845 字节，这些名字在 class_census 里只体现为 field_count 列的一个数字——名字本身、长度分布、形态全部丢失。S0 量化基线缺成员维度：混淆强度评估、改名工作量估算、折叠契约的压力测试都需要一行一成员的面。

成员事实在库里已齐备：members 表有稳定 member_id、class_id 关联、kind（字段/方法）、name、desc、access。缺的只是投影与渲染，不缺存储，本设计是纯投影批。

2. 投影面
---------

DbMemberCensusRow 字段清单：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 字段
     - 说明
   * - ownerInternalName
     - 所属类内部名（借用视图，指向 classes.internal_name）
   * - kind
     - Field / Method
   * - name
     - 成员名
   * - descriptor
     - 描述符（字段与方法的统一列）
   * - access
     - 原始访问标志
   * - native
     - 方法 native 旗标派生（字段恒 0）
   * - entryId
     - 所属类关联的条目（关联事实，活跃/墓碑语义随类）
   * - origin
     - 继承所属类的分类结果（对 owner 内部名走 classifyLibraryOrigin，与 censusClasses 同源同果）

origin 不另分类：成员级直接继承所属类的分类结果，重复分类既浪费又可能与类级不一致。censusMembers(inputId) 的可见域与 censusClasses 同契约：只含关联到本输入活跃条目的类下的成员，墓碑类连带其成员一起退出普查面。

行序契约：复合键 BINARY 升序，键序 = owner 内部名 → kind（Field 在前，编码序）→ 名字 → 描述符，键序定死在投影层、渲染层不排序。v1 范围 = 全部成员，字段与方法都出。

3. TSV 面
---------

member_census.tsv v1 列集与列序（列间单制表符）：

.. code-block:: text

    # member-census v1
    member_class	kind	name	descriptor	access	native	entry

- 折叠契约沿用类级：转义后超 kCensusCellFoldBytes 的单元格折叠为 ``#folded:len=<原始字节数>:sha256=<原始字节摘要>``\ ，21845 字节名字折叠后与类级同款约 187 字节；描述符与名字同阈值同契约。
- escapeUnicode 转义、UTF-8 无 BOM、LF 行尾、binary 写出、无时间戳——字节面契约逐条沿用，同输入两次导出逐字节一致。
- 行数上限：v1 不设上限（确定性优先），千万级语料的导出体量是已知风险面。
- 配套关系：class_census 的导出入口同时落 member_census.tsv（同一导出面、同一临时库生命周期），报告结构增加 memberCensus 路径与 members 计数；对账不变式 = 报告 members 计数 = member_census 行数 = Σ 类行 method_count + field_count。独立入口徒增两次导出的一致性维护负担。

4. 验收
-------

混淆语料回归：148 个空格命名字段全部出现在 member_census.tsv，超长单元格折叠为确定形，双跑逐字节一致。行序确定：复合键 BINARY 升序可断言。对账：member 行数与类级计数之和一致。折叠边界：4096 保字面、4097 折叠，与类级同款边界用例。
