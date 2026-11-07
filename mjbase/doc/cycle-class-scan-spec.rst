.. _cycle-class-scan-spec:

循环继承腐坏判据与 UnusedClassRemover opt-in 接线规格
=====================================================

状态：定稿，随本批落地。微观契约（判据算法、边解析、重复名口径、keep 交互、
守卫豁免）在 ``cycle_class_scan.hpp`` 头注与 ``unused_member_remover.hpp``
的 ``UnusedClassRemover`` 注释；本文承载判据口径为什么这样定、与 Recaf 原件
（CycleClassRemovingTransformer）的吸收差异、接线语义与验收面。

.. _cycle-class-scan-niche:

1. 生态位：判据与删除分离的组合式吸收
--------------------------------------

真实混淆器与字节码手术事故会产出 ``.super`` 成环的类（A 的父类是 B、B 的父类
是 A，或 ``class A extends A`` 自指）。这类类在 JVM 装载时必败（JVMS 5.3.5
装载阶段沿 super 链解析即抛 ``ClassCircularityError``），是纯粹的容器垃圾；
Recaf 对此有一个独立删除 pass（CycleClassRemovingTransformer：图查询判
``isLoop`` → 标记删除）。

mjbase 的吸收不照搬一比一。Recaf 的形态是「独立删除 pass 挂管线」，而 mjbase
已有两块相关地基：可达性删除面（UnusedClassRemover，入口可达性判据）与层次图
（hierarchy 模块 ClassHierarchy——它是**清洁图**，登记时直接拒绝环边，不持有
腐坏数据，因此不能也不该拿它做腐坏输入的检测器）。本批把「继承环腐坏」做成
独立判据信号：分析件（``mjbase/analysis/cycle_class_scan``）产容器内环类集合，
删除动作由既有 UnusedClassRemover 以 opt-in 参数接线承载。收益：判据件可独立
单测与复用（其他消费方——jar 体检、诊断报告——不必挂删除 pass 即可取环集合），
删除面不新增注册段（UnusedClassRemover 已在册），既有判据的默认行为零变化。

.. _cycle-class-scan-criteria:

2. 判据口径
-----------

- **边定义**：super（extends）与接口（implements）双边，对齐 Recaf
  ``InheritanceVertex.isLoop`` 的父母闭包口径——两边都参与 JVM 子类型关系，
  任一边成环同样腐坏。
- **环成员定义**：自环（super 或任一接口等于自身）或 super/接口边图上大小
  ≥2 的强连通分量全体成员。等价于「沿继承边至少一步能回到自身」。
- **环外挂靠类不判**：环成员的子类、实现类（如 ``C extends A`` 而 A↔B 成环）
  不在判据输出内。装载层面它们同样必败，但删除裁决权在消费方——判据件只报
  环成员本身，对齐 Recaf 口径（Recaf 也只删 loop 顶点）。
- **边只在扫描集合内解析**：悬挂键（super/接口指向容器外的类）不可能闭合环，
  计入后截断，与 hierarchy 模块的悬挂键口径一致。
- **同名重复扫描类**：首个登记成节点，后续同名类不另立节点、其边不并入——
  判据按名报告，同一内部名视角下是同一类身份，口径确定且与登记序无关。
- **畸形类 fail-open**：``scanClassResources`` 跳过解析失败的类（不产出事实），
  判据只见可解析骨架；容器级原子性由消费方（pass 的 fail-closed 守卫）另行
  承担，两层语义不混。
- **算法与复杂度**：迭代 Tarjan 强连通分量（显式栈帧 + 访问集），O(V+E)，
  任何输入不挂死、不递归爆栈（对抗深链输入的既定纪律）。
- **输出契约**：环类内部名按节点登记序（即扫描序）输出；返回视图借用各扫描
  类会话符号池，扫描集合存活期内有效。

.. _cycle-class-scan-wiring:

3. UnusedClassRemover opt-in 接线
---------------------------------

构造参数 ``removeInheritanceCycles``\ （默认 false）承载接线，语义四条：

- **默认关 = 既有语义逐字节一致**：flag 关时不做环扫描、不进任何新判定分支，
  删除集与改写面与接线前完全一致（回归用例以「可达环全保 + 容器字节恒等」
  钉死）。
- **开启后环类入选删除集**：环名集合（剔除 keep 命中后）并入删除集——判据
  独立于可达性，入口可达的环成员同样删除（它装载必败，可达性证明不了它能跑）。
- **无根守卫豁免**：家族「无根整 pass 保留」守卫保护的是可达性误判下的库型
  罐；环类删除独立于可达性，opt-in 后不受该守卫门控——无根罐中的环类照删。
- **keep 面只加不减**：资源 keep 名单（module/Manifest/services）与 keep 规则
  命中的环类照常保留（keep 守卫优先于腐坏判据）；keep 命中的环类装载必败的
  腐坏状态不因保留而恶化，这是家族既有契约的直接推论。

容器级 fail-closed 原子性不变：任一 class 资源不可解析仍整 pass 放弃（返回
all()，无部分删除，环类也不删）。PreservedAnalyses 语义不变：发生删除返回
none()，未发生返回 all()。

.. _cycle-class-scan-recaf-diff:

4. 与 Recaf 原件的差异点
------------------------

- **形态**：Recaf 是注册在案的独立删除 transformer（逐类 ``markClassForRemoval``）；
  mjbase 是判据分析件 + 既有 remover 的 opt-in 参数，无新注册段。
- **图源**：Recaf 用 InheritanceGraphService 的既有图（图本身带环截断，查询时
  逐顶点回判）；mjbase 的清洁层次图拒绝腐坏输入，判据件自建一次性边图直判
  SCC，一遍得出全体环成员（Recaf 是逐类回查父母闭包）。
- **keep 面**：Recaf 无条件删（无 keep 概念）；mjbase 接线尊重资源 keep 名单
  与 keep 规则（只加不减家族契约）。
- **无根守卫**：Recaf 无此概念；mjbase opt-in 后显式豁免并在头注声明。
- **判据输出**：Recaf 只服务删除；mjbase 判据件是独立可复用信号（返回环名
  集合），删除只是它的一个 opt-in 消费方。

.. _cycle-class-scan-acceptance:

5. 验收面
---------

判据件（``cycle_class_scan_test``）：A↔B 双类环只报环成员、自环
（super=自己）、三类长环、环中含入口可达真类（判据不受可达性抑制且环外入口
不误报）、环外正常继承与悬挂 super 零命中、幂等二跑、同名重复合并（不并入
边）、畸形类 fail-open。

接线面（``unused_class_remover_cycle_test``）：opt-in 删除入口可达环成员、
默认档字节零变化回归、无根罐默认保留 vs opt-in 删除、keep 命中环类保留
（keep 守卫优先）、opt-in 二跑幂等、畸形容器 fail-closed 不受 opt-in 影响。
判据关闭的既有行为由家族套件（``unused_member_remover_test``）既有用例继续
覆盖，零改动。
