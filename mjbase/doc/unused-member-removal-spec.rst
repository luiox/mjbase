.. _unused-member-removal-spec:

未使用成员清理家族规格（EmptyClassRemover / UnusedClassRemover / UnusedFieldRemover / GlobalUnusedMethodRemover）
==================================================================================================================

状态：已落地（共享契约以 ``unused_member_remover.hpp`` 头注为权威面，本文承载家族分层、删除安全不变量与档位口径的成文整理），2026-11-08。

1. 动机与家族分层
-----------------

本家族把「可达性 / 引用索引分析证明不可达」的类、字段、方法转换为字节码删
除，四个 pass 按分析来源分层：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 件
     - 分析来源
     - 删除对象
   * - EmptyClassRemover
     - 无（只看单类结构）
     - 无字段无方法的空 class 资源
   * - UnusedClassRemover
     - ReferenceIndex + CallGraph + ResolvedCallGraph → 全局可达闭包
     - 闭包外的 class 资源
   * - UnusedFieldRemover
     - 自建全局可达闭包（Resolved 展开）
     - 可达类内不可达字段（按档位）
   * - GlobalUnusedMethodRemover
     - 同上
     - 可达类内不可达方法（按档位）

四件全部 ``scope:global`` / ``stability:opt-in`` （默认链不收），仅在确实
删除时返回 ``none()``\ ，否则 ``all()`` 。微观契约（根策略逐项、档位枚举、
keep 交互）在 ``unused_member_remover.hpp`` 头注；本文不复制。

2. 共享根策略
-------------

成员删除两件与类删除件共用同一套根策略，闭包根 ⊇ 无条件保留面：

- **资源 keep 名单**：module 描述符、Manifest 各类入口（Main / Premain /
  Agent / Launcher-Agent / JavaFX）、``META-INF/services`` 文件名与内容；
- Java ``public static main(String[])``\ ；
- 类初始化器（``<clinit>``\ ）保留；
- 成员删除两件另把「保留但不可达」面整体提升为根：keep metadata（
  Signature / 注解族）、keep 规则命中、native/abstract（无方法体语义承
  载）、构造器、资源 keep 类与 keep 规则类的全部声明成员——凡因策略面（而
  非可达性）保留的成员，其出边必须参与闭包传播，否则「保留但不可达」成员
  引用的死成员会被删出悬垂引用。

方法体展开用 ResolvedCallGraph，删除前保留虚调用和接口调用的覆写目标
（覆写 / 被覆写双向保留由 Resolved 展开承载）。删除候选 = 容器内声明 ∧
不在闭包。可达类闭包为空（library jar、无根）时整 pass 原样保留输入；分
析未注册时抛 ``logic_error`` （配置错误，稳定失败）。

3. 删除安全不变量与「调用点收尾」裁定
-------------------------------------

成员删除的安全论证与商业化 trim 工具的 mark-sweep 主流程同构：保留代码引
用到的成员在 mark 相必然已入保留集，sweep 后保留代码「天然无悬垂引用」。
依据是闭包对容器内引用事实的过近似——ReferenceIndex 覆盖 invoke 家族、字
段访问、MethodHandle、invokedynamic 引导参数、反射查找/执行、类型引用，
只从根前向展开：**可达性判死 ⇒ 全容器代码（含保留代码）无引用 ⇒ 删除后
invoke 指令面无需清扫**。

因此 ``MethodCallRemover`` 保持独立 spec 驱动设施定位：不是删除族的正确性
前置，可选地作为删除族的「前置增益」（删掉最后一个调用点使非私有成员变
死，供下一轮删除族受益）或独立用户面（删保留成员的无用调用点）；管线无
跨 pass 正确性顺序约束。

fail-closed 判定路径的主体在跨容器不可见的入边上：库消费方、外部子类、
ServiceLoader / agent 等容器外入口落在两道闸上——public 面（默认档保留）
与资源 keep 根（提升保留）；删除谓词内的 keep metadata / keep 规则守卫保
留为纵深防御。

4. 可见性档位（UnusedMemberAccessTier）
---------------------------------------

档位只决定「否则会被删除」的成员可见性候选面，不关闭任何既有硬底线（构造
器 / ``<clinit>``\ 、keep metadata、keep 规则命中、native/abstract、资源
keep 根在全部档位下照常保留）：

- ``PrivateOnly``\ ：旧策略，只候选 private 死成员；
- ``NonPublic``\ （默认）：private / package-private / protected 死成员全
  候选，public 面保守保留——public 是库消费方无需任何类关系即可引用的
  面，容器引用闭包看不见跨 JAR 入边，fail-closed 保留；
- ``All``\ ：public 死成员也候选。仅适用于闭程序（应用 JAR，全程序在容器
  内）；库型 JAR 开启会破坏外部链接。

5. keep 规则面
--------------

四个 pass 的构造器接受可选 ``KeepRuleSet`` （默认 nullptr = 无规则）。规
则集是「纯加法」输入：命中的类 / 成员在既有硬底线之上再保留一份，无法关
闭任何硬底线（``keep_rule.hpp`` 「规则只加不减」契约）。类规则命中 = 类
整体保留、其声明方法提升为可达性根（成员删除两件另提升其声明字段，与资
源 keep 同名类策略一致）；成员规则对「否则会被删除」的任意可见性成员产生
可观测差异（覆盖面随档位走）。运行时供给面（CLI / 配置解析）属 cli 批，
本层只提供程序化注入点。

6. fail-closed 与两阶段提交
---------------------------

- **UnusedClassRemover**：比较 classfile 头声明名而非资源路径（JAR 条目
  可能被重命名或与头不一致）；任一类名读取失败立即整 pass 放弃（返回
  ``all()``\ ，无部分删除）；全部解析完成后才提交删除。Multi-Release 族
  （``META-INF/versions/<N>/`` 变体）随主类同删同保，家族不拆。
- **成员删除两件**：所有 class 先完成解析和序列化，再一次提交（两阶段提
  交，作业级原子）；任一解析或序列化失败保持全部原始 class bytes。
- **载体**：成员删除在 libjbc 低层 DOM 上进行（erase MemberInfo——成员在
  classfile 中按 name/desc 经 CP 引用、无槽位索引依赖，删除结构安全）；常
  量池不压缩，失引条目保留（append-only 契约，writer 级差异由 golden 声明
  类目收口）。

7. 与既有规格篇的界
-------------------

UnusedClassRemover 的 opt-in 循环继承腐坏判据（环类 JVM 装载必败
``ClassCircularityError``\ ，判据独立于可达性）已独立成篇
:doc:`cycle-class-scan-spec`\ ——判据算法、边解析、重复名口径、keep 交互
与守卫豁免见该篇，本文不重复；家族侧只需知道：开启后环类加入删除集、「无
根整 pass 保留」守卫对环类删除不生效、keep 面照常只加不减。容器级删除与
改名共用作业级原子语义，改名面（``ClassRenamer`` ）的判据与
``ToolClassCombiner`` 的合并准入不在本文（见 :ref:`pass-catalog`\ §2）。

8. 测试面
---------

- ``unused_member_remover_test`` （家族套件，35 用例）：空类只删结构空、
  闭包外类删除、library jar 无根保留、覆写目标保留、service/manifest/
  module 入口保留、畸形类 fail-closed 无部分删除、keep metadata 命中保
  留、字段/方法两件只删不可达 private、注解命中保留、keep 规则五面（类规
  则三件保护、成员规则定点保护、预设模式保护框架回调、非命中零差异）、档
  位三态（默认档删非 public 保 public 面、PrivateOnly 旧行为、All 连
  public）、service 入口成员幸存、「保留但不可达」调用者保住被调用者、构
  造器可达保住被调用者、二跑严格 no-op、重建索引无入边悬垂、Multi-Release
  族同删同保。
- ``unused_member_remover_jvm_test`` （家族 JVM 真跑门）：清理后类字节在
  真实 JVM 装载运行，非 private 死成员删除后行为对拍。
- golden 对拍四道：EmptyClassRemover / UnusedClassRemover /
  UnusedFieldRemover / GlobalUnusedMethodRemover（um 语料，writer 级差异
  按声明类目收口）。
