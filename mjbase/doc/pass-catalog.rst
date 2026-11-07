.. _pass-catalog:

mjbase pass 族目录（pass-catalog）
==================================

状态：定稿（对照 main 分支 ``default_pass_registry.cpp``\ 的注册表逐件核对）。

本文是 pass 域的规格目录：记录当前注册在册的每一个 pass 现在成立的契约——职责、判据锚（钉形/门是什么）、拒绝面（fail-closed 桶）、验收门（测试怎么咬它）、与其他 pass 的链序关系（仅在头注显式声明时记）。微观契约（参数、窗口逐规则、放弃清单逐条）在 ``mjbase/src/mjbase/pass/``\ 下各头文件的 ``///`` 注释里，本文不复制。计划中的 pass、迁移叙事与批次排期不在本文范围。

契约基座（三级作用域、写回归管线、PreservedAnalyses 语义、sanitize 闸门）见 :ref:`pass-contract`\ ；本文只记各 pass 在这套契约下的具体形状。

覆盖面与分族
------------

覆盖面是 ``mjbase/src/mjbase/pass/default_pass_registry.cpp``\ 注册的全部 81 个 pass：21 个容器级（``GlobalPass``\ ）、21 个类级（``ClassPass``\ ）、39 个方法级（``MethodPass``\ ）。分族依据是注册表标签（``scope``\ /\ ``action``\ /\ ``stability``\ ）与各头注自报的家族名，按职责分十族；成员清单与族内分工全部以现状为准。

两个全族惯例：

- PreservedAnalyses 条件语义：发生改写返回 ``none()``\ ，未发生返回 ``all()``\ ；家族头注另有声明时从其声明。
- 稳定性：除两个 ``stability:default``\ 成员外全族 ``opt-in``\ ——默认链不收，须经 ``-p``\ 显式挂点或内置 ``phase.*``\ 管线装载。

验收门有三类，各节按实记：

- **单测**\ ：gtest 套件，方法级经共享驱动走与生产一致的管线路径（promote → run → 管线统一 lower），并对写回字节重建会话再 promote——验证真实写回产物而非内存态。
- **golden 对拍**\ ：语料与 golden 冻结在 ``mjbase/test_resource/pass_golden/``\ ，默认零差异逐字节比较；确有 writer 级正当差异的类目钉在 ``manifests/<Pass>.manifest.txt``\ ，对拍测试按声明类目改跑替代断言。三件 simplifier 与 JunkCodeInserter 无旧栈基线，走确定性自基线道（``golden_self``\ ）。
- **JVM 真跑门**\ ：变换后类字节在真实 JVM 上装载并做行为对拍，钉死语义等价。

.. _pass-catalog-global-gate:

1. 容器级门禁与恢复面
---------------------

容器级、面向整个 ``ResourceContainer``\ 的清洗、收紧与调试面重建。清洗件是管线最前的门，其余为 opt-in 恢复/治理件。

.. _pass-catalog-illegal-class-file-cleaner:

IllegalClassFileCleaner
^^^^^^^^^^^^^^^^^^^^^^^

- 职责：容器内 class bytes 的两级畸形清洗——libjclassfile 前置级全量规则面（属性级 CP 校验剔除、越界跳转/LDC 的 RETURN 桩替换、调试表修剪、BSM 表压缩与 condy 手术、无效 MethodHandle 清除）加上 libjbc 级两条结构规则的快路径兜底（非法成员描述符整条剔除、异常表垃圾条目剪除）。
- 判据锚：逐资源两级流水、级间以字节衔接；规则触发按「产物与输入的字节比较」判定，未触发的资源不重编码——合法输入零重编码、字节不动。
- 拒绝面：读/写失败零产出，级内任一步失败该级原样放行；libjclassfile 读失败不放弃资源，原 bytes 仍进 libjbc 级清洗（产出只会更干净）；容器不保存裸指针、不抛异常。
- 验收门：``illegal_class_file_cleaner_test``\ ，外加 golden 对拍（IllegalClassFileCleaner 道，jcf 规范形语料）。
- 链序：清洗件宜挂容器级管线最前（先清洗、后变换），产出供全部下游 pass 消费。

.. _pass-catalog-access-modifier-tightener:

AccessModifierTightener
^^^^^^^^^^^^^^^^^^^^^^^

- 职责：全容器普查 ``INVOKESTATIC``\ 与 ``GETSTATIC``\ /\ ``PUTSTATIC``\ 引用面，把仅被本类引用的 public static 成员收紧为 private。
- 判据锚：普查表记引用与 usedOuter；收紧条件 = 成员至少被引用一次且无外部类引用且当前 static + public。
- 拒绝面：空 bytes、不可解析、无类名的资源跳过；任一方法体升格失败的类整体跳过——普查证据不全时收紧是不安全方向；反射、Native、服务查找等容器外入口不在普查面内，故 opt-in、不得进默认链。
- 验收门：``access_modifier_tightener_test``\ ，外加 golden 对拍（AccessModifierTightener 道）。

.. _pass-catalog-stack-trace-recover:

StackTraceRecover
^^^^^^^^^^^^^^^^^

- 职责：重建最小调试面——SourceFile/SourceDebug 覆盖式写入、异常 handler 入口插 ``dup; invokevirtual printStackTrace()V``\ 、每个标签锚点合成一行 LNT 行号。
- 判据锚：源文件名 = 类内名点分 + ``.java``\ ；handler 的 invokevirtual owner 按 catch 类型聚合（单一类型取该类型，catch-all 或多类型共享取 ``java/lang/Throwable``\ ）；行号类级跨方法递增、超 65535 回绕。
- 拒绝面：任一类不可解析、属性解码失败、编辑或 lower 失败 → 整件放弃（两阶段提交，容器保持原样）；未锚定的 handler label 跳过。
- 验收门：``stack_trace_recover_test``\ 。
- 链序：非幂等（每轮追加一组打印调用与一批行号），opt-in 单轮使用；sourceFile 恒被改写 → 恒返回 ``none()``\ 。

.. _pass-catalog-local-variable-annotator:

LocalVariableAnnotator
^^^^^^^^^^^^^^^^^^^^^^

- 职责：局部变量表规范化——按 Strict 帧精确类型重写 LVT（名字 ``var<slot>``\ ，槽复用切分生命周期生成 ``var<slot>_<序号>``\ ），丢弃不再可信的 LVTT，给反编译器可读的变量名与类型面。
- 判据锚：libjbc Strict 帧引擎逐指令计算 locals/stack，按「槽位 + 赋入类型」切变量。
- 拒绝面：Strict 失败降 BestEffort 帧，仍失败则仅按存储指令推基本类型、引用类型保留原声明；升格、帧、写回失败逐方法跳过，解析或写回失败的类原样保留（``Stats``\ 计数观测）。
- 验收门：``local_variable_annotator_test``\ 。
- 链序：只改调试属性，指令、StackMapTable、maxes 一律不动。

.. _pass-catalog-enum-name-restorer:

EnumNameRestorer
^^^^^^^^^^^^^^^^

- 一行条目：枚举名恢复——扫容器内枚举类，从 ``<clinit>``\ 泄漏的字符串 ldc 真名按双形状判据（构造点窗口与 ``$values``\ 数组形状互证）产出 ``IntermediateMapping``\ 家族映射挂容器 AM，字节零改动；逐条判据（形态锚、双形状互证、名字口径）与边界申报见 :doc:`enum-name-restore-spec`\ （规格篇）。单测 ``enum_name_restorer_test``\ + JVM 真跑门 ``enum_name_restorer_jvm_test``\ 。
- 链序：只产映射不改名，改名执行交映射消费面组合；标签 ``scope:global``\ /\ ``stability:opt-in``\ 。

.. _pass-catalog-global-structure:

2. 容器级结构与成员面
---------------------

容器级直写组：类改名、工具类合并、未使用成员清理家族与调用点收尾件。删除与改名共用两阶段提交的作业级原子语义；删除家族共享同一套根策略与 keep 规则面。

未使用成员清理家族（EmptyClassRemover / UnusedClassRemover / UnusedFieldRemover / GlobalUnusedMethodRemover）的共享契约：根策略 = 资源 keep 名单（module 描述符、Manifest 各类入口、``META-INF/services``\ ）+ Java ``public static main``\ + 类初始化器；方法体展开用 ResolvedCallGraph；删除候选 = 容器内声明 ∧ 不在闭包。可达性判死 ⇒ 全容器代码（含保留代码）无引用 ⇒ 删除后 invoke 指令面无需清扫——\ ``MethodCallRemover``\ 是可选前置增益或独立用户面，不是删除族的正确性前置。成员删除两件把「保留但不可达」面（keep metadata、keep 规则命中、native/abstract、构造器、资源 keep 类的全部声明成员）整体提升为闭包根，防删出悬垂引用。

.. _pass-catalog-class-renamer:

ClassRenamer
^^^^^^^^^^^^

- 职责：按确定性规则把容器内全部类重命名为 ``<包>/Class<N>``\ ；base 与 ``META-INF/versions/<N>/``\ 变体同内名族只消耗一个编号、共享同一新名、各占版本化 URI。
- 判据锚：装载后按 (类内部名, URI) 字节序排序编号——URI 是同内名族内唯一的次序决胜键，编号不随容器迭代序漂移。
- 拒绝面：空 bytes、不可解析资源跳过；反射字符串与资源面不开启（旧栈默认口径）；任一序列化失败整 pass 抛出、容器保持原样（作业级原子）。
- 验收门：``class_renamer_test``\ + ``class_renamer_jvm_test``\ /\ ``class_renamer_mr_jvm_test``\ （JVM 真跑，Multi-Release 族），外加 golden 对拍（ClassRenamer 道，writer 级差异按声明类目收口）。
- 链序：改写面 = bytecode + annotations + signatures——签名走 libjbc 签名解析树 span 手术、注解走 typed 属性模型，非字符串替换。

.. _pass-catalog-tool-class-combiner:

ToolClassCombiner
^^^^^^^^^^^^^^^^^

- 职责：把满足准入的 static 工具类保守合并进合成宿主 ``mjbase/ToolClassMergedClass_<N>``\ ，供体删除、供体名全容器 owner 重映射。
- 判据锚：准入 = Object 直父、名不含 ``$``\ 、无接口、只含 static 字段/方法及构造器、非构造器方法简单体（无任何 invoke、无 ``PUTSTATIC``\ ）；宿主分桶按方法合计上限与撞名换新宿主。
- 拒绝面：候选少于 2 整体 no-op；名含 ``ToolClassMergedClass_``\ 的类永不入候选（幂等重入防护）；候选体含 ldc 宽常量形态整体落选、原类原样保留（不支持范围保持原资源）；宿主分支体 Strict 帧不可算即显式失败整批回滚。
- 验收门：``tool_class_combiner_test``\ + ``tool_class_combiner_jvm_test``\ （JVM 真跑），外加 golden 对拍（ToolClassCombiner 道）。
- 链序：帧链经 spi frame_rewire 同一链路；成员搬移走 libjbc adoptField/adoptMethod 成员级契约，供体删除后全容器重映射兜住残留引用。

.. _pass-catalog-empty-class-remover:

EmptyClassRemover
^^^^^^^^^^^^^^^^^

- 职责：删除没有字段和方法的空 class 资源；不做可达性分析。
- 判据锚：class 可完整解析且字段方法皆空。
- 拒绝面：非 class 资源、畸形字节、带成员的类原样；keep 规则命中的类不删（规则只加不减）。
- 验收门：``unused_member_remover_test``\ （家族套件），外加 golden 对拍（EmptyClassRemover 道，um 语料）与家族 JVM 真跑门 ``unused_member_remover_jvm_test``\ 。

.. _pass-catalog-unused-class-remover:

UnusedClassRemover
^^^^^^^^^^^^^^^^^^

- 职责：消费 ReferenceIndex + CallGraph + ResolvedCallGraph 建全局可达闭包，删除闭包外的 class 资源。
- 判据锚：闭包根 = 共享根策略；比较的是 classfile 头声明名而非资源路径。
- 拒绝面：任一类名读取失败立即整 pass 放弃（无部分删除）；可达类闭包为空（library jar、无根）整件原样；分析未注册抛 ``logic_error``\ ——配置错误的稳定失败。
- 验收门：家族套件 + golden 对拍（UnusedClassRemover 道）+ 家族 JVM 真跑门。
- 链序：删除整类与成员删除两件同一信任级，跨容器入边 fail-closed 由 public 档位与 keep 规则兜底。

.. _pass-catalog-unused-field-remover:

UnusedFieldRemover
^^^^^^^^^^^^^^^^^^

- 职责：自建可达闭包，按可见性档位删除可达类内不可达字段（默认 ``NonPublic``\ 档：private/package-private/protected 全候选，public 保守保留；``All``\ 档仅限全程序在容器内的闭程序）。
- 判据锚：删除候选 = 容器内声明 ∧ 不在闭包；keep 规则命中的类整类跳过且其声明字段提升为根。
- 拒绝面：硬底线（构造器、``<clinit>``\ 、keep metadata、native/abstract、资源 keep 根）全档位保留；全部类先完成解析和序列化再一次提交，任一失败保持全部原始 bytes。
- 验收门：家族套件 + golden 对拍（UnusedFieldRemover 道）+ 家族 JVM 真跑门。

.. _pass-catalog-global-unused-method-remover:

GlobalUnusedMethodRemover
^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：同上载体删除不可达方法；覆写/被覆写双向保留由 Resolved 展开承载。
- 判据锚：根 = 共享根策略 + 无条件保留面（构造器与 ``<clinit>``\ 、native/abstract、keep metadata、keep 规则命中、keep/资源 keep 类全部声明成员）。
- 拒绝面：与 UnusedFieldRemover 同口径；可见性档位语义同上。
- 验收门：家族套件 + golden 对拍（GlobalUnusedMethodRemover 道）+ 家族 JVM 真跑门。
- 链序：内联族（IdentityWrapperInliner / MethodInliner）头注显式声明壳方法删除延后至本件管线。

.. _pass-catalog-method-call-remover:

MethodCallRemover
^^^^^^^^^^^^^^^^^

- 职责：按程序化配置（``MethodCallRemovalConfig``\ ）点名删除成员调用点，另带无用结果白名单调用删除面——语义对齐商业化 trim 工具的 removeMethodCalls 设施。
- 判据锚：include/exclude 双名单（成员粒度，desc 空串 = 任意描述符）；可删形态 = 调用消费面为 ``pop``\ /\ ``pop2``\ /\ void 返回/死局部存储，删除后按实参宽度补弹、栈等价。
- 拒绝面：默认构造的配置两个面全关 → 完全 no-op；调用点前一条是 dup/dup2 族或 swap、返回类别与消费面不匹配、返回值被他消费、调用点或消费面被分支/异常表锚定——逐点保留；invokedynamic 永不删；任一 class 常量池存在引用该成员的 MethodHandle 条目 → 整成员放弃。
- 验收门：``remove_method_calls_test``\ + ``remove_method_calls_jvm_test``\ （JVM 真跑）。
- 链序：删除族的前置增益（删掉最后一个调用点使非私有成员变死）或独立用户面；管线无跨 pass 正确性顺序约束。

.. _pass-catalog-global-call:

3. 容器级调用面与常量面
-----------------------

容器级、改写调用点或常量布局的件；常量面为「只读收集 + 读点物化」两件组。多数成员的等价性论证依赖「容器即全程序」的封闭世界公理（各头注逐件声明），opt-in 由使用者担保。

.. _pass-catalog-devirtualize-single-target:

DevirtualizeSingleTarget
^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：CHA 证明虚调用点唯一活目标时，把 ``invokevirtual``\ /\ ``invokeinterface``\ 改写为 ``invokespecial``\ 直接分派（JVMS §6.5/§4.10.1.9 逐条对照的合法性论证）。
- 判据锚：消费 ResolvedCallGraph（仅 CHA 档）+ ReferenceIndex + Hierarchy；站点恰一条边、唯一目标容器内声明且具体非桥、调用者类 major ≥ 52、站点前一条指令恰是 ``ALOAD_0``\ 、ACC_PRIVATE 目标须与调用者同类。
- 拒绝面：RTA 档不适用（观测驱动的欠近似不作改写依据）；``<init>``\ 族、签名多态、接口目标、站点锚标签、字节码事实与分析不一致——逐点放弃；封闭世界公理外的运行期挂接子类不受支持。
- 验收门：``devirtualize_single_target_test``\ + ``devirtualize_single_target_jvm_test``\ （JVM 真跑）。
- 链序：栈效应与编码长度不变，帧与 StackMapTable 逐点不变，lower 重定位恒等。

.. _pass-catalog-identity-wrapper-inliner:

IdentityWrapperInliner
^^^^^^^^^^^^^^^^^^^^^^

- 职责：纯转发壳方法的 ``invokestatic``\ 调用点路径压缩到最终目标（形态 A 静态直通、形态 B 虚转发——接收者取原首实参）；壳体保留不删。
- 判据锚：壳体白名单 = 恰 nparams 条按序参数装载 + 1 条 invoke + 1 条匹配返回、异常表空、非 synchronized；壳→壳链一次压缩到底，环整族不进壳表。
- 拒绝面：special 与 invokedynamic 不进壳表；容器外目标只认 JDK 八个 box 类的装拆箱白名单；壳类子类的静态解析形态不进表；站点级逐点放弃、类级失败整类不写回。
- 验收门：``identity_wrapper_inliner_test``\ 。
- 链序：ArgumentArrayScalarizer 的前置（调用面先压恒等转发壳再标量化）；无引用壳的清除延后 GlobalUnusedMethodRemover 管线。

.. _pass-catalog-method-inliner:

MethodInliner
^^^^^^^^^^^^^

- 一行条目：预算内纯函数小方法体克隆进调用点；适用情况、逐面判据与前后 jasm 对照见 :doc:`method-inliner`\ （规格篇）。单测 ``method_inliner_test``\ + JVM 真跑门 ``method_inliner_jvm_test``\ 。

.. _pass-catalog-indy-site-inliner:

IndySiteInliner
^^^^^^^^^^^^^^^

- 一行条目：indy 站点内联——消费注入的 IndyResolver 产出，把混淆器家族的 invokedynamic 站点改写为直接成员调用；生成面语义两族（\ ``LambdaMetafactory``\ /\ ``StringConcatFactory``\ ）采集即排除，SPI 契约、改写判据矩阵与前后 jasm 对照见 :doc:`indy-inline-spec`\ （规格篇）。单测 ``indy_site_inliner_test``\ + JVM 真跑门 ``indy_site_inliner_jvm_test``\ 。
- 链序：注册工厂无 resolver 时严格 no-op，插件/宿主层经构造注入 resolver 后生效；标签 ``scope:global``\ /\ ``stability:opt-in``\ 。

.. _pass-catalog-redundant-invoke-remover:

RedundantInvokeRemover
^^^^^^^^^^^^^^^^^^^^^^

- 职责：容器内定义、MethodEffectModel 判 Pure、调用结果被紧随 ``POP``\ /\ ``POP2``\ 弹弃的方法——其可删调用点整组摘除。
- 判据锚：普查提名 = 「调用 + 宽度匹配弹弃 + 目标命中容器内定义集 + Pure 档」；ReadOnly 档不提名。
- 拒绝面：任一类资源不可解析整件放弃（普查全量性前提）；只删调用点不删方法定义——悬垂方法交 unused-member 族收尾。
- 验收门：``redundant_invoke_remover_test``\ 。
- 链序：应用面整体委派 MethodCallRemover 的点名删除路径；实参生产者残段交方法级局部清理族后续收敛。

.. _pass-catalog-argument-array-scalarizer:

ArgumentArrayScalarizer
^^^^^^^^^^^^^^^^^^^^^^^

- 职责：装箱混淆退化的单 ``Object[]``\ 参数签名还原为真参数列表——被调方取参窗改直接参数槽位、调用侧装箱窗拆成直接传参、签名经 MemberRemapPlan 全局同步。
- 判据锚：被调侧取参窗（``aload p; iconst_i; aaload; checkcast Ti``\ ）下标完整覆盖且数组不逃逸（入参定义边全部落在取参窗）；调用侧规范窗（长度常量 == n、下标序 0..n-1、实参组非空且有界）；容器内全部调用点都是规范窗。
- 拒绝面：实参多指令装箱链 fail-closed；CP owner ≠ 声明类的继承静态视图、他类同名同描述符声明、MethodHandle/indy 钉死、窗内锚定存活标签——任一失败整目标放弃；两阶段提交结构编辑先行、描述符同步随后。
- 验收门：``argument_array_scalarizer_test``\ 。
- 链序：IdentityWrapperInliner 是其前置切片。

.. _pass-catalog-global-str-pool-optimizer:

GlobalStrPoolOptimizer
^^^^^^^^^^^^^^^^^^^^^^

- 职责：容器内全部 String ldc 去重编号，迁入合成字符串池类 ``str/Pool``\ （静态数组 + ``<clinit>``\ 初始化 + ``getStr(I)``\ 取值），原 ldc 改「索引装载 + 池取值调用」。
- 判据锚：全量普查按遭遇序编号、clinit 写出序取字典序；池类 URI 已存在 → 整件跳过（幂等）。
- 拒绝面：任一类资源不可解析整件放弃（普查全量性）；改写方法 lower 或序列化失败整件放弃（两阶段提交）。改写是净栈中性替换，分支目标帧不漂移、无需帧再生。
- 验收门：``global_str_pool_optimizer_test``\ 。
- 链序：字符串内容面失效（改写返回 ``none()``\ ）——下游字符串常量假设须在本件之后重查。

.. _pass-catalog-static-value-collection:

StaticValueCollection
^^^^^^^^^^^^^^^^^^^^^

- 一行条目：静态常量收集（容器级只读件）——逐类评估 ``<clinit>``\ 走廊对本类 static 字段的常量/纯算术写，连同 static final 字段的既有 ConstantValue 属性面，产出 pinned 的 ``StaticValueView``\ （字节零改动）；走廊纯度、单写者普查、值面求值等逐桶判据见 :doc:`static-value-inlining-spec`\ （规格篇）。单测 ``static_value_collection_test``\ 。
- 链序：视图 pinned 发布供物化件消费，与写侧 StaticFieldDefaultInliner 同管线可链、互不依赖；标签 ``scope:global``\ /\ ``stability:opt-in``\ 。

.. _pass-catalog-static-value-inlining:

StaticValueInlining
^^^^^^^^^^^^^^^^^^^

- 一行条目：GETSTATIC 读点物化——消费收集件发布的视图，把容器内命中读点原位物化为等值常量装载（消一次字段读）；读点位置、类别匹配、volatile 复核、CP 容量逐桶判据与前后 jasm 对照见 :doc:`static-value-inlining-spec`\ （规格篇）。单测 ``static_value_inlining_test``\ + JVM 真跑门 ``static_value_inlining_jvm_test``\ 。
- 链序：链上没有收集件（或视图无内容）时严格 no-op，两件独立启停；标签 ``scope:global``\ /\ ``stability:opt-in``\ 。

.. _pass-catalog-meta-family:

4. 类级元数据清理家族
---------------------

类级、清除三层（类/字段/方法）非语义元数据的 14 件（``meta_info_remover.hpp``\ 家族 13 件 + 独立头文件的 LineNumberRemover）。家族共享契约：属性表落点是 backing 的 AttrSlot 表（backing 是 writeClass 的序列化源），access 标志位在 ``unit()``\ 有模型、按 :ref:`pass-contract`\ §1 双面落盘；注解过滤一律槽内过滤（解码 AttrBody → 按谓词删条目 → 槽置脏），删空连槽摘除；解析失败、属性形状不符时保守保持原样，不产出半截改写。Code 嵌套属性表的触碰是显式有界破例，仅 MetaInfoRemover（整槽摘除）与 IllegalTypeAnnotationRemover（嵌套槽内过滤）两成员，均不改指令载荷、父 Code 槽连带置脏驱动重编码。全家族 opt-in，golden 对拍按 meta 语义矩阵口径（纯标志位触发类目与旧栈 golden 逐字节一致）。

.. _pass-catalog-meta-info-remover:

MetaInfoRemover
^^^^^^^^^^^^^^^

- 职责：catch-all 清除已列明的 debug/framework 元数据——SourceFile/SourceDebug、三层 Runtime 注解与 type 注解、方法级参数注解、MethodParameters、AnnotationDefault、Code 嵌套 LNT/LVT/LVTT。
- 判据锚：列明槽整槽摘除 + 注解槽槽内过滤；record 组件的注解与 type 注解槽按成员级同规则槽内清空（父 Record 槽连带置脏）。
- 拒绝面：Signature、ConstantValue、Exceptions、未知属性（Raw）与一切指令语义保持不变。
- 验收门：``meta_info_remover_test``\ （家族套件）+ ``meta_info_remover_jvm_test``\ （JVM 真跑，家族），外加 golden 对拍（MetaInfoRemover 道）。
- 链序：Code 嵌套整槽摘除是家族破例面两成员之一。

.. _pass-catalog-final-remover:

FinalRemover
^^^^^^^^^^^^

- 职责：移除全部字段的 ``ACC_FINAL``\ 标志位（两面落盘，不处理 ConstantValue 与 final 语义）。
- 拒绝面：无 final 字段时零改动。
- 验收门：家族套件 + golden 对拍（FinalRemover 道）。

.. _pass-catalog-synthetic-bridge-remover:

SyntheticBridgeRemover
^^^^^^^^^^^^^^^^^^^^^^

- 职责：移除类/方法/字段的 synthetic 标志与方法的 bridge 标志（只清标志位，不推断编译器语义）。
- 拒绝面：无命中时零改动。
- 验收门：家族套件 + golden 对拍（SyntheticBridgeRemover 道）。

.. _pass-catalog-kotlin-metadata-remover:

KotlinMetadataRemover
^^^^^^^^^^^^^^^^^^^^^

- 职责：移除类级 ``Lkotlin/Metadata;``\ 注解与方法/字段上的 jetbrains 三注解（NotNull/JvmName/Nullable）。
- 判据锚：槽内过滤；类级只查 visible 注解槽（与旧实现一致）。
- 拒绝面：解析失败保持原样。
- 验收门：家族套件 + golden 对拍（KotlinMetadataRemover 道）。

.. _pass-catalog-line-number-remover:

LineNumberRemover
^^^^^^^^^^^^^^^^^

- 职责：删除每个方法 Code 嵌套的 LineNumberTable（混淆产物行号失真，且改写后原行首 pc 可能悬垂、阻碍 lower 重定位）。
- 拒绝面：保留 LVT/LVTT、指令载荷、异常表与 StackMapTable；无 LNT 时零改动。
- 验收门：``line_number_remover_test``\ 。
- 链序：改动经父 Code 槽置脏驱动重编码；独立头文件、注册在类级。

.. _pass-catalog-source-info-remover:

SourceInfoRemover
^^^^^^^^^^^^^^^^^

- 职责：清除类级 SourceFile 与 SourceDebugExtension 两槽。
- 拒绝面：不改指令与 Code；两属性缺席时零改动。
- 验收门：家族套件 + golden 对拍（SourceInfoRemover 道）。

.. _pass-catalog-signature-remover:

SignatureRemover
^^^^^^^^^^^^^^^^

- 职责：清除类/方法/字段三层的 Signature 槽（不改 descriptor 与指令）。
- 拒绝面：三层全无 Signature 时零改动。
- 验收门：家族套件 + golden 对拍（SignatureRemover 道）。

.. _pass-catalog-illegal-annotation-remover:

IllegalAnnotationRemover
^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：清理三层注解槽中描述符形状非法的注解（空描述符/码点长度 ≤ 3/非 ``L…;``\ 形/含控制字符）。
- 拒绝面：槽内过滤、删空连槽摘除；不查 type annotation 与参数注解。
- 验收门：家族套件 + golden 对拍（IllegalAnnotationRemover 道）。

.. _pass-catalog-illegal-signature-remover:

IllegalSignatureRemover
^^^^^^^^^^^^^^^^^^^^^^^

- 职责：清理三层中签名解析器拒绝的非法 Signature（类/方法/字段分道按对应文法判）。
- 拒绝面：仅摘解析失败的槽，合法签名原样。
- 验收门：家族套件 + golden 对拍（IllegalSignatureRemover 道）。

.. _pass-catalog-illegal-type-annotation-remover:

IllegalTypeAnnotationRemover
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：清理三层 type annotation 槽中描述符形状非法的条目（形状判定同 IllegalAnnotationRemover，施于条目的注解类型描述符）。
- 拒绝面：槽内过滤、删空连槽摘除；不查普通注解槽。
- 验收门：家族套件 + golden 对拍（IllegalTypeAnnotationRemover 道）。
- 链序：覆盖 Code 嵌套槽是家族破例面两成员之二。

.. _pass-catalog-illegal-source-info-remover:

IllegalSourceInfoRemover
^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：清除形状非法的 SourceFile/SourceDebugExtension（码点长度 > 100、非 ``.java``\ 结尾、基名为空/数字头/含非 ASCII 字母数字字节）。
- 拒绝面：合法值原样保留；两属性编码形态不同、读面分别处理。
- 验收门：家族套件 + golden 对拍（IllegalSourceInfoRemover 道）。

.. _pass-catalog-unknown-attribute-cleaner:

UnknownAttributeCleaner
^^^^^^^^^^^^^^^^^^^^^^^

- 职责：清理三层属性表中未知（非标准）属性槽——按属性名对 JVMS 标准属性名封闭集判定。
- 拒绝面：guard 集罩住 ConstantValue/Exceptions/SourceFile 等标准但未建模的属性防误伤；Code 嵌套属性不在本 pass 面。
- 验收门：家族套件 + golden 对拍（UnknownAttributeCleaner 道）。

.. _pass-catalog-illegal-access-remover:

IllegalAccessRemover
^^^^^^^^^^^^^^^^^^^^

- 职责：清除类/方法/字段/参数上明确非法的 access 标志位（按成员种类分列的非法位表，保留合法别名位；类非 enum 时清字段 ENUM 位；MethodParameters 载荷按 JVMS 布局校验后原位改写）。
- 拒绝面：其余形状保守跳过；全部两面落盘。
- 验收门：家族套件 + golden 对拍（IllegalAccessRemover 道）。

.. _pass-catalog-specific-annotation-remover:

SpecificAnnotationRemover
^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：移除黑名单描述符（旧栈封闭集）或形状非法的注解；查三层 visible/invisible 注解与参数注解两槽。
- 判据锚：谓词 = 形状判定 ∪ 黑名单集合；参数注解 typed 行模型为主，typed 解码拒绝的 legacy 形状按 JVMS 布局做二进制级条目摘除（Raw 兜底）。
- 拒绝面：布局不符/截尾整槽保守保持原样；部分摘除留槽（参数行数与行序保持）。
- 验收门：家族套件 + golden 对拍（SpecificAnnotationRemover 道）。

.. _pass-catalog-local-variable-table-rebuilder:

LocalVariableTableRebuilder
^^^^^^^^^^^^^^^^^^^^^^^^^^^

- 一行条目（调试表收口独立件，非本节清理家族 14 件成员）：变换后 LVT 按存活区间重算（保真口径：可信名/描述符沿用原表可验证项，落 ``var<槽位>``\ 兜底）或显式丢弃口径摘除 LVT/LVTT；与 LocalVariableAnnotator（规范化件）的边界、两口径与幂等声明见 ``local_variable_table_rebuilder.hpp``\ 头注。单测 ``local_variable_table_rebuilder_test``\ 。标签 ``scope:class``\ /\ ``stability:opt-in``\ （改写所有产物的调试表形态，默认链不收，经管线配置显式启用）。

.. _pass-catalog-long-annotation-remover:

LongAnnotationRemover
^^^^^^^^^^^^^^^^^^^^^

- 一行条目（体积判据独立件，非本节清理家族 14 件成员）：清理三层注解/type 注解/参数注解槽中描述符长度 ≥ 阈值（默认 150）的巨型垃圾注解——黑名单（SpecificAnnotationRemover）与形状判据（IllegalAnnotationRemover 族）之外的体积正交判据面；嵌套值面 tag='@' 条目整项删除、幸存条目值内递归修剪；Code 嵌套槽触碰沿家族破例口径（父 Code 槽连带置脏）；长度按码点数计（家族口径，与 Recaf 的 UTF-16 码元口径在 ASCII 描述符上逐字节一致）；不触碰 record 组件与 AnnotationDefault（Recaf 同口径，不扩面）。判据细节与阈值依据见 ``long_annotation_remover.hpp``\ 头注。单测 ``long_annotation_remover_test``\ 。标签 ``scope:class``\ /\ ``action:eliminate``\ /\ ``stability:opt-in``\ 。

.. _pass-catalog-duplicate-annotation-remover:

DuplicateAnnotationRemover
^^^^^^^^^^^^^^^^^^^^^^^^^^

- 一行条目（重复判据独立件，非本节清理家族 14 件成员）：同目标同描述符的多实例注解去重、首个实例保留——普通注解键为描述符内容、type annotation 键为目标位全集（不同目标位的同描述符条目各保留）、参数注解键为 (行下标, 描述符) 且行数行序保持；visible 与 invisible 两族共享一份集；分集粒度为类/成员/Code 嵌套槽/record 组件；不递归注解值面（Recaf 同口径）；Code 嵌套与 record 组件触碰沿家族破例口径（父槽连带置脏）。判据细节见 ``duplicate_annotation_remover.hpp``\ 头注。单测 ``duplicate_annotation_remover_test``\ 。标签 ``scope:class``\ /\ ``action:sanitize``\ /\ ``stability:opt-in``\ 。

.. _pass-catalog-illegal-varargs-remover:

IllegalVarargsRemover
^^^^^^^^^^^^^^^^^^^^^

- 一行条目（access 位修复独立件，非本节清理家族 14 件成员）：ACC_VARARGS 位与方法描述符的失配四象限裁决——位有 + 无阵列尾参（含无参方法）摘位（Recaf 对齐方向）、位无 + 阵列尾参置位（修复扩展方向：恢复反射 isVarArgs 与 javac 变参调用点解析，但会翻转合法非 varargs 数组尾参方法的语义面，故严格 opt-in、默认链永不收）；描述符逐 FieldType 解析判形，畸形描述符 fail-closed 双向不动；标志位两面落盘（unit + backing）。判据细节与置位方向语义影响见 ``illegal_varargs_remover.hpp``\ 头注。单测 ``illegal_varargs_remover_test``\ 。标签 ``scope:class``\ /\ ``action:sanitize``\ /\ ``stability:opt-in``\ 。

.. _pass-catalog-class-const-skeleton:

5. 类级常量与骨架面
-------------------

类级、动常量布局或成员物理序的三件，是容器级直写与方法级指令变换之间的骨架层。

.. _pass-catalog-static-field-default-inliner:

StaticFieldDefaultInliner
^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：``<clinit>``\ 中单次写入的常量内联进字段 ConstantValue 属性，并删除等价的「常量装载 + ``PUTSTATIC``\ 」指令对。
- 判据锚：预扫描按 (name,desc) 计数 ``PUTSTATIC``\ ，非单次写的字段整族保守跳过；常量识别按字段描述符定族且须是 ``PUTSTATIC``\ 的顺序前驱；owner 必须是本类。
- 拒绝面：erase 被分支/标签锚定拒绝时该字段保守放弃；落盘序 = erase + lower 成功之后才写字段面——lower 失败时 backing 未触碰，无半内联态。
- 验收门：``static_field_default_inliner_test``\ 。
- 链序：与 StaticArrayFieldScalarizer 串联可消化 clinit 常量表。

.. _pass-catalog-static-array-field-scalarizer:

StaticArrayFieldScalarizer
^^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：private 的一维数组字段（static 与实例同面），分配唯一、长度常量、访问全部是常量下标窗口时，拆成按访问下标命名的标量字段——分配与下标计算指令消失，读窗三条缩一条。
- 判据锚：分配窗恰一处（static 形或 ``<init>``\ 内实例形）+ 读写窗全覆盖 + 字段无整体引用；引用元素按读窗后继 checkcast 与写窗常量值证据精化（声明为 ``java/lang/Object``\ 时按证据定描述符）。
- 拒绝面：本类带 Nest 族属性整 pass 放弃（nest 语义下 private 可被 nest 内他类访问，扫描不闭包）；下标越界、缺分配窗、数组重赋、同下标证据不一致、新字段名冲突、窗内指令锚标签——整字段放弃（判定期排除、改写阶段零失败）。
- 验收门：``static_array_field_scalarizer_test``\ 。
- 链序：与 StaticFieldDefaultInliner 串联；落盘序 = 全部窗口改写并 lower 成功后才增删字段，任一步失败会话弃置、无半拆态。

.. _pass-catalog-class-member-sorter:

ClassMemberSorter
^^^^^^^^^^^^^^^^^

- 职责：类成员确定性重排——字段按 (描述符, 名称) 字典序；方法分 ``<clinit>``\ 组、名称字典序组、``<init>``\ 组三组各自稳定排序。
- 判据锚：排列构造后经类级结构编辑原语双面落盘（会话骨架与 backing MemberInfo 序列同排列，\ ``MethodUnit``\ 的 backingIndex 同步改指）。
- 拒绝面：排序未发生返回 ``all()``\ （不触发无谓重序列化；对旧实现无条件 ``none()``\ 的显式偏离声明）。
- 验收门：``class_member_sorter_test``\ ，外加 golden 对拍（ClassMemberSorter 道）。
- 链序：成员物理序变化对容器级与方法级分析透明（\ ``MethodId``\ 随骨架走）；opt-in。

.. _pass-catalog-method-window:

6. 方法级窗口化简与折叠族
-------------------------

方法级 peephole 主力：窗口匹配 + 等价替换，迭代到不动点。窗口替换的共享协议（逐位匹配谓词、标签迁移、srcPc 继承、右界优先轮次）由 ``window_rule.hpp``\ 的 ``replaceWindow``\ 承载；常量装载判定与物化由 ``method_rewrite_util.hpp``\ 共享。

.. _pass-catalog-constant-folder:

ConstantFolder
^^^^^^^^^^^^^^

- 职责：操作数栈上「常量装载 + 同类型二元运算」的极大链折叠为单条常量装载（i/l/f/d 四型算术、移位、按位）。
- 判据锚：虚拟操作数栈跟踪链自身压值，栈深回到 1 且至少消费一个运算即折叠点，贪心取极大链；整数按 u32/u64 回绕、移位量掩位、浮点按 IEEE 位物化（NaN/Inf 含）。
- 拒绝面：``idiv``\ /\ ``irem``\ 除数为常量 0 不折（保留运行期 ArithmeticException）；int/long 的 MIN/-1 除法取模保守不折；链中混入非常量、类型混用、不支持 op 即链终止。
- 验收门：``constant_folder_test``\ 。
- 链序：内置管线 ``phase.constant-fold``\ ；折叠产物为常量传播与跳转折叠族供弹。

.. _pass-catalog-strength-reducer:

StrengthReducer
^^^^^^^^^^^^^^^

- 职责：常量 2^k 的乘/除改写为移位（乘）与「移位-加法修正」除法序列（除，向零取整语义位精确），仅 int/long。
- 判据锚：前驱为同宽度编译期常量、值恰为正 2 的幂且指数在 1..<宽度。
- 拒绝面：0、负数、非 2 的幂、k 越界保守不动；float/double 与 ``irem``\ /\ ``lrem``\ 不在本 pass 面。
- 验收门：``strength_reducer_test``\ （差分矩阵：从重写后字节读出发射形状、按 JVM 环绕语义位精确模拟对拍）。
- 链序：内置管线 ``phase.strength-reduce``\ 。

.. _pass-catalog-stack-operation-simplifier:

StackOperationSimplifier
^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：冗余栈操作窗口（``dup;pop``\ → ∅、``pop;pop``\ → ``pop2``\ 、常量装载复制展开等）按 31 条规则迭代到不动点。
- 判据锚：asm/tmatcher 的 ``ReplaceRule``\ 组合子 + InsnPlanner 计划面；胜出纪律 = 位置优先扫描、Longest 裁决、命中区间不重叠、整轮批式落盘。
- 拒绝面：ldc 载荷类型不可判别的宽匹配语义（被匹配序列的栈形状自身保证折叠正确）；锚定窗按窗口粒度拒收、其余计划继续；全部计划被拒即无进展防死循环。
- 验收门：``stack_operation_test``\ 。
- 链序：内置管线 ``phase.stack-op-simplify``\ ；标签 ``stability:default``\ 。

.. _pass-catalog-stack-operation-expander:

StackOperationExpander
^^^^^^^^^^^^^^^^^^^^^^

- 一行条目：常量 producer 上 DUP 族按帧溯源展开，复制槽位改由原常量装载克隆独立供给（StackOperationSimplifier 对偶面）；适用形态、槽宽形判别、窗口纪律与前后对照见 :doc:`stack-op-expander`\ （规格篇）。单测 ``stack_operation_expander_test``\ 。
- 链序：内置管线 ``phase.stack-op-expand``\ ；组合顺序建议 expand → simplify；标签 ``stability:opt-in``\ 。

.. _pass-catalog-const-char-array-string-folder:

ConstCharArrayStringFolder
^^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：常量 char 数组构造的 ``new String(char[])``\ 整窗折叠为最终字符串的 ldc。
- 判据锚：完整连续形状（new + dup + 常量长度 + newarray + 逐元素常量填充 + ``<init>([C)V``\ ），长度/下标/元素全是可证明常量表达式（不含 ``idiv``\ /\ ``irem``\ ），下标任意次序但恰覆盖 [0, length)。
- 拒绝面：任意别名、逃逸、未覆盖/越界下标、非 ``T_CHAR``\ 数组、非法 surrogate、窗内锚定 incoming 标签——整组不动。
- 验收门：``const_char_array_string_folder_test``\ ，外加 golden 对拍（ConstCharArrayStringFolder 道）。
- 链序：与 CharArrToStrSimplifier 互补——那件保留数组语义（改写为 ldc + ``toCharArray``\ ），本件折叠整个构造串。

.. _pass-catalog-char-arr-to-str-simplifier:

CharArrToStrSimplifier
^^^^^^^^^^^^^^^^^^^^^^

- 职责：「定长 char 数组逐元素常量填充」序列改写为 ``ldc "<text>"; invokevirtual toCharArray()[C``\ （序列值与栈效应不变，保留数组语义）。
- 判据锚：``ICONST``\ /\ ``NEWARRAY T_CHAR``\ /\ (``DUP; ICONST_j; <常量>; CASTORE``\ ) × len 匹配矩阵；字符按 JVM int→char 截断收进 UTF-16 码元。
- 拒绝面：编码失败的码元序列整组失配；窗内锚定标签失配（首成员标签随改写迁移）。
- 验收门：``char_arr_to_str_simplifier_test``\ ，外加 golden 对拍（CharArrToStrSimplifier 道）。

.. _pass-catalog-fix-pattern-simplifier:

FixPatternSimplifier
^^^^^^^^^^^^^^^^^^^^

- 职责：替换已登记的固定混淆模式——位运算恒等式、legacy 死代码固定序列、整数比较常量偏移折叠、同 slot store/load 中转消除、分发器不透明位级恒等式（int 三条 + long 三条借位/进位形式）。
- 判据锚：每条规则一个短指令窗口 + 显式替换序列；每轮每规则至多一次替换（复刻旧栈轮次结构保证规则交互终态一致）。
- 拒绝面：窗口内部成员锚定存活标签即跳过该窗；规则 11 的 producer 同槽自匹配形态显式放弃（旧栈在该形态无限自匹配，属显式修正）；不动点轮次熔断越界即抛。
- 验收门：``fix_pattern_simplifier_test``\ + ``fix_pattern_simplifier_jvm_test``\ （JVM 真跑），外加确定性自基线对拍（golden_self FixPatternSimplifier 道）。
- 链序：分发器链式恒等式靠不动点轮次单 pass 收敛；恒等算子噪声归 ExpressionSimplifier——管线序建议本件在前。

.. _pass-catalog-expression-simplifier:

ExpressionSimplifier
^^^^^^^^^^^^^^^^^^^^

- 职责：可复用代数表达式窗口折叠（恒等元、交换/结合、特殊表达式——与 FixPatternSimplifier 的分工是代数恒等式对固定惯用模式）。
- 判据锚：全部规则是纯 peephole 窗口，经 ``window_rule::replaceWindow``\ 落盘（窗口头标签迁移、内部标签拒配、srcPc 继承、失败回滚）。
- 拒绝面：一条代数不成立的 legacy 规则移植时丢弃（已声明）；窗口内标签拒配即保持原样。
- 验收门：``expression_simplifier_test``\ ，外加确定性自基线对拍（golden_self ExpressionSimplifier 道）。
- 链序：管线序建议在 FixPatternSimplifier 之后。

.. _pass-catalog-type-cast-simplifier:

TypeCastSimplifier
^^^^^^^^^^^^^^^^^^

- 职责：原始类型 cast 常量折叠（15 条 JVM primitive cast）与已知 helper 折叠（``Float.intBitsToFloat``\ /\ ``Double.longBitsToDouble``\ 位图、``Integer.parseInt``\ /\ ``Long.parseUnsignedLong``\ ）。
- 判据锚：浮点→整数按 JVM 语义（NaN → 0、越界 → MIN/MAX、范围内向零截断）；常量按原始位图物化（保留 NaN payload 与 -0.0）。
- 拒绝面：未知 helper、非法字符串、越界数值一律不折。
- 验收门：``type_cast_simplifier_test``\ ，外加确定性自基线对拍（golden_self TypeCastSimplifier 道）。

.. _pass-catalog-enum-cast-check-remover:

EnumCastCheckRemover
^^^^^^^^^^^^^^^^^^^^

- 职责：移除枚举类首个方法（javac 的 values()）中可证明冗余的 ``INVOKEVIRTUAL``\ /\ ``CHECKCAST``\ 相邻对。
- 判据锚：ACC_ENUM + 会话首方法位 + 调用描述符字面等于当前枚举数组类型 + 紧随 ``CHECKCAST``\ ——字面口径与 golden 语料对齐（真实 javac 形态不会自然出现，保守走 no-op 面）。
- 拒绝面：调用无 cast、cast 隔有锚定标签不成对保持原样；批删被异常表锚定拒绝时整体零动作。
- 验收门：``enum_cast_check_remover_test``\ ，外加 golden 对拍（EnumCastCheckRemover 道）。

.. _pass-catalog-string-concat-indirector:

StringConcatIndirector
^^^^^^^^^^^^^^^^^^^^^^

- 职责：StringBuilder「构造 + append 链 + toString」的严格直线窗改写为单条 ``invokedynamic makeConcatWithConstants``\ （javac 9+ 拼接规范形）。
- 判据锚：识别窗逐指令相邻；每 append 实参为自包含纯计算链（反向需值抽取，栈操作/调用/字段/数组/分配不入链）；BSM 条目同形复用、indy CP 条目 intern。
- 拒绝面：类版本 < 53 拒收；condy 不认领；窗内任何成员锚定存活标签整窗拒收；``<init>(String)``\ 实参须为字符串常量；任一 intern/erase 失败整窗放弃（无半删）。常量不嵌配方（全 ``\u0001``\ 槽位），配方嵌入属后续精化挂账。
- 验收门：``string_concat_indirector_test``\ + ``string_concat_indirector_jvm_test``\ （JVM 真跑）。

.. _pass-catalog-method-propagation:

7. 方法级传播与跳转折叠族
-------------------------

方法级数据流传播（常量与引用）与「真值静态已定」的跳转/分发折叠。传播族以 def-use 到达定义或来源域为载体，折叠族共享「窗口内部边界锚标签拒收（栈消费保护）、窗口头标签放行」的守卫纪律。

.. _pass-catalog-local-constant-propagation:

LocalConstantPropagation
^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：直线段内的「常量装载 + store」槽位追踪——后续同类装载原位物化为常量生产者，IINC 按 32 位回绕递推。
- 判据锚：单遍直线扫描维护槽位追踪表；引用槽只认 ``ACONST_NULL``\ 生产者。
- 拒绝面：任一成员前置边界锚标签、控制流转移、return、athrow 即整体清空追踪（宁可漏折不可折错）；装载与追踪槽族严格匹配（旧栈快捷形误替换可产 Verifier 非法形态，属声明收紧）。
- 验收门：``local_constant_propagation_test``\ 。

.. _pass-catalog-constant-propagator:

ConstantPropagator
^^^^^^^^^^^^^^^^^^

- 职责：跨块常量传播——def-use 到达定义面：装载点到达定义唯一且 store 弹值为匹配槽族常量生产者时，装载原位物化为常量。
- 判据锚：分支/汇合不阻断、路径精确；引用槽同样只认 ``ACONST_NULL``\ 。
- 拒绝面：到达集合截断整件跳过（负向结论不建立在截断集合上）；store 前装载不误折（旧栈整方法单 store 面的正确性修正）；iinc 定义非常量生产者天然拦截。
- 验收门：``constant_propagator_test``\ 。
- 链序：与 LocalConstantPropagation 分工——那件是直线段窗口面、本件跨块路径精确面。

.. _pass-catalog-ref-copy-propagator:

RefCopyPropagator
^^^^^^^^^^^^^^^^^

- 职责：单来源不可变引用的 ``aload``\ 原位改写为来源指令（换槽 aload / ldc 引用常量 / ``aconst_null``\ ），折叠暂存槽链。
- 判据锚：来源域单值性——全路径该槽当前值由同一指令产生；来源穿透 store→load，多级暂存一步到底。
- 拒绝面：生产者白名单外（字段、调用、new、数组读取、dup 变体）一律保留；来源 Unknown/Param、读点不可达、帧数据流失败整件跳过。
- 验收门：``ref_copy_propagator_test``\ 。
- 链序：入参来源不可克隆的形态交 StoreLoadRelayEliminator 与冗余存储族收敛。

.. _pass-catalog-intrinsic-folding:

IntrinsicFolding
^^^^^^^^^^^^^^^^

- 职责：intrinsic 常量折叠——装箱/拆箱对整删、种子 ``java/util/Random``\ 五窗 LCG 折叠、白名单纯 JDK 函数在实参全常量时折叠为常量结果。
- 判据锚：白名单 = ``Math.abs``\ /\ ``max``\ /\ ``min``\ 、``String.length``\ /\ ``isEmpty``\ /\ ``substring``\ 、Integer/Long 位运算与解析格式化族；常量串按 CP MUTF-8 字节视图取值。
- 拒绝面：substring 仅折纯 ASCII 常量串（非 ASCII 形态保守不折——旧栈按字节错折的声明修正）；窗内锚定标签整窗跳过、首成员边界放行；「先删尾后改头」的事务序不留半折叠态。
- 验收门：``intrinsic_folding_test``\ 。

.. _pass-catalog-cons-jump-optimizer:

ConsJumpOptimizer
^^^^^^^^^^^^^^^^^

- 职责：常量条件跳转相邻窗折叠——谓词恒真改 ``GOTO``\ （复用原目标标签），恒假整窗删除。
- 判据锚：单槽 int 谓词窗、``ACONST_NULL``\ 空谓词窗、双槽 ``IF_ICMPxx``\ 窗；常量装载面 = 共享判定全集。
- 拒绝面：类型失配窗与 ``IF_ACMP``\ 族不入矩阵（旧栈对 Verifier 非法形态也折叠的防御怪癖显式拒绝）；窗口内部边界锚标签拒收（栈消费保护），窗口头标签放行；顺序末尾条件跳删除无承接者保守跳过。
- 验收门：``cons_jump_optimizer_test``\ 。
- 链序：常量跳转折叠族首件；「先删尾后改头」事务序不留半折叠态。

.. _pass-catalog-constant-switch-folder:

ConstantSwitchFolder
^^^^^^^^^^^^^^^^^^^^

- 职责：常量 selector + switch 相邻窗折叠——按 selector 值解析命中分支（lookup 精确/table 区间/未命中走 default），常量头原位改 ``GOTO``\ 复用命中目标标签、switch 删除。
- 拒绝面：switch 前置边界锚标签拒收（栈消费保护）、窗口头标签放行；顺序末尾 switch 保守跳过；目标标签解析失败拒收。
- 验收门：``constant_switch_folder_test``\ 。
- 链序：常量跳转折叠族第二件；折叠后未选中 case 标签失去引用者，不物化进字节码。

.. _pass-catalog-method-dead-store:

8. 方法级死代码与局部存储清理族
-------------------------------

方法级清理主力。分三个推理档位互补、幂等收敛：窗口面（相邻对，不跑数据流）、槽位面（方法级零读点）、def-use 面（到达定义与使用边，负向结论纪律 = 数据流失败或截断整件跳过）。默认链（内置管线 ``default-optimize``\ ）收本族三件：NopRemover → RedundantLoadValEliminator → UnusedCheckCleaner。

.. _pass-catalog-nop-remover:

NopRemover
^^^^^^^^^^

- 职责：删除方法体里的 ``NOP``\ 指令。
- 判据锚：NOP 是分支/switch 目标先重定向到顺序后继再删；末条且被引用的 NOP 保留。
- 拒绝面：被异常表边界锚定的 NOP 保留（对旧栈无条件删除的显式保守偏离，触发面为手工畸形样本）。
- 验收门：``nop_remover_test``\ ，外加 golden 对拍（NopRemover 道）。
- 链序：默认链收尾清理件，标签 ``stability:default``\ 。

.. _pass-catalog-redundant-load-val-eliminator:

RedundantLoadValEliminator
^^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：「常量装载 + 紧随 ``POP``\ /\ ``POP2``\ 」相邻对消除，迭代到不动点。
- 判据锚：匹配矩阵按返回类别对齐弹宽（int/long/string 常量配 POP，long/double 配 POP2）。
- 拒绝面：POP 前置边界锚存活标签跳过该对；跨行号边界的相邻对会消除（行号边界在 CodeDom 不可见，显式保守偏离声明）。
- 验收门：``redundant_load_val_eliminator_test``\ ，外加 golden 对拍（RedundantLoadValEliminator 道）。
- 链序：默认链成员；store 降级暴露的死装载由 UselessPopCleaner 后续切片。

.. _pass-catalog-unused-check-cleaner:

UnusedCheckCleaner
^^^^^^^^^^^^^^^^^^

- 职责：删除「``ALOAD_0``\ + ``Objects.requireNonNull``\ + ``POP``\ 」三指令组（this 的 VM 隐式非空保证删除行为保持）。
- 拒绝面：仅实例方法参与；组内第 2/3 成员锚定标签跳过，首成员异常表锚定整组跳过。
- 验收门：``unused_check_cleaner_test``\ ，外加 golden 对拍（UnusedCheckCleaner 道）。
- 链序：默认链成员。

.. _pass-catalog-useless-pop-cleaner:

UselessPopCleaner
^^^^^^^^^^^^^^^^^

- 职责：弹值即被丢弃的死生产链消除——相位一（窗口面）：局部装载 + ``POP``\ /\ ``POP2``\ 相邻对；相位二（帧溯源面）：纯常量生产者链整删（含分支汇合的多到达定义组）。
- 判据锚：相位二按 def-use 使用边判定「每个定义的全部使用边都指向本 pop」；DUP/DUP2 栈中继逐中继细化吸收（直线溯源认证 + 组栈平衡），X 族与 SWAP 重排形态保持 fail-closed。
- 拒绝面：窗口面锚定标签跳过该对；溯源常量与 def 侧互证不一致整组弃证；POP2 双 cat1 形态仅单侧可删时半删降级。
- 验收门：``useless_pop_cleaner_test``\ 。

.. _pass-catalog-pop-unused-local-variables:

PopUnUsedLocalVariables
^^^^^^^^^^^^^^^^^^^^^^^

- 职责：方法内无任何读取的槽位，其 store 降级为 ``pop``\ /\ ``pop2``\ （宽度由 store 族操作码定死，不依赖帧溯源）。
- 判据锚：读点审计按指令全集（不可达代码读点同样计入）：局部装载、两槽高半槽、iinc 与 ret 操作槽全入读集合。
- 拒绝面：槽位存在任何读点 → 该槽全部 store 原样；不删生产者（常量装载清理归 RedundantLoadValEliminator、局部装载清理归本家族后续件）；不做活跃区间分析。
- 验收门：``pop_unused_local_variables_test``\ 。

.. _pass-catalog-dead-local-store-remover:

DeadLocalStoreRemover
^^^^^^^^^^^^^^^^^^^^^

- 职责：槽位值链不可达「有意义汇点」（invoke/分支/字段写/monitor/athrow/return/xastore）的 store 整删或降级；直接喂常量的死 store 连常量生产者一起消失。
- 判据锚：analyzeDefUse 按签名播种入参；store 与 iinc 作中间定义续走，dup/swap 复制透明。
- 拒绝面：帧数据流失败或到达集合溢出整件跳过；无输入边的 store 只降级不整删。
- 验收门：``dead_local_store_remover_test``\ 。
- 链序：与 PopUnUsedLocalVariables 分工——那件判「零读点」、本件判「读点全不可达汇点」且能整删常量链。

.. _pass-catalog-dead-side-effect-free-call-remover:

DeadSideEffectFreeCallRemover
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：白名单无副作用调用（``System.nanoTime``\ /\ ``currentTimeMillis``\ /\ ``Math.random``\ 、常量接收者的 ``String.length``\ /\ ``isEmpty``\ 、Math/StrictMath 纯函数 31 名）的返回值经纯计算链最终被 pop 丢弃时整链删除。
- 判据锚：链成员 = 白名单调用 + 纯计算生产者（常量/算术/数值转换/ARRAYLENGTH/INSTANCEOF），def-use 定义边闭包 + 使用边 arity 对齐。
- 拒绝面：帧数据流失败或截断整件跳过；异常表方法逐窗口 fail-closed（handler 入口种子不可入链）；区间约束外分支不得中途进链。
- 验收门：``dead_side_effect_free_call_remover_test``\ 。

.. _pass-catalog-redundant-store-eliminater:

RedundantStoreEliminater
^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：值定义无任何使用边的 store 原位降级 ``pop``\ /\ ``pop2``\ 。
- 判据锚：到达定义按路径追踪（后续 store 覆盖天然杀死前定义；异常路径经精确异常边合并，handler 读点产生使用边）。
- 拒绝面：帧数据流失败或截断整件跳过；iinc 是读改写、天然产生使用边不受牵连。
- 验收门：``redundant_store_eliminater_test``\ 。
- 链序：与家族两件分工——PopUnUsedLocalVariables 判「零读点槽」、本件判「单次 store 定义的零使用」（到边粒度，槽内其他定义不受牵连）。

.. _pass-catalog-store-load-relay-eliminator:

StoreLoadRelayEliminator
^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：store 定义使用边恰为紧邻 reload 的「store/load 中转对」整删，值改由栈保持（混淆器打断模式匹配的 ``istore s; iload s``\ 形态）。
- 判据锚：def-use 单使用证明；五族全宽覆盖；删除暴露的新相邻对下一轮继续。
- 拒绝面：对内任一侧锚定标签整对拒收（reload 位锚入 = 分支跳入时期望弹值在栈）；后续任何读点即额外使用边保守保留（旧栈窗内多对面不健全、不复刻）。
- 验收门：``store_load_relay_eliminator_test``\ 。

.. _pass-catalog-write-only-array-cleaner:

WriteOnlyArrayCleaner
^^^^^^^^^^^^^^^^^^^^^

- 职责：只被元素存储消费的数组分配连存储窗整体删除（装箱还原收尾件）。
- 拒绝面：数组有任何其他消费面（arraylength、传参、返回、再存储）不删。
- 验收门：``write_only_array_cleaner_test``\ 。

.. _pass-catalog-unreachable-insn-remover:

UnreachableInsnRemover
^^^^^^^^^^^^^^^^^^^^^^

- 职责：帧可达性证明不可达的普通指令整批删除（处理链尾死块、死分发块、分支解析残留）。
- 判据锚：帧引擎一次（BestEffort 形状档），不可达集一次性 ``eraseBatch``\ （批内互引消解、批外引用前置校验）。
- 拒绝面：帧计算失败或帧数不一致整方法放弃；锚异常表标签的待删指令保守保留（死保护区清理交异常面批次）；锚分支引用标签的待删指令保留（防御门）。
- 验收门：``unreachable_insn_remover_test``\ 。
- 链序：反扁平化件（\ ``flattening_reverser.hpp``\ ）声明分发块不可达残留交本件收敛。

.. _pass-catalog-cse-chain-eliminater:

CseChainEliminater
^^^^^^^^^^^^^^^^^^

- 职责：同一直线段内两条结构全同的纯计算链先后写同一局部槽——第二条链连同其存储整组摘除（GVN/CSE 第一切片）。
- 判据锚：链结构全同 = 逐条同操作码、装载同槽号、LDC 同 CP 条目；区间内无标签锚定与控制流、无对操作数槽与目标槽的写入。
- 拒绝面：需值抽取遇链外指令整对拒收；condy 不认领；``IDIV``\ 族入链安全（链 1 已用同值求值、链 2 重算的异常面不可达）。
- 验收门：``cse_chain_eliminater_test``\ 。

.. _pass-catalog-uncontended-monitor-eliminater:

UncontendedMonitorEliminater
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：监控对象全程不逃逸方法、唯一用途是成对 monitor 进出的监控块整组摘除（混淆注入的 junk synchronized 无害化）。
- 判据锚：识别形 = 「NEW + ``<init>``\ + ASTORE 暂存」头 + 恰一 enter 一 exit；def-use 证明唯一 ASTORE 定义的全部使用边恰为两读两监控。
- 拒绝面：实参游程含副作用指令整头拒收；窗内及 enter..exit 区间内部锚定存活标签整组拒收；方法参数作监控接收者天然失配。
- 验收门：``uncontended_monitor_eliminator_test``\ 。

.. _pass-catalog-entry-slot-swap-normalizer:

EntrySlotSwapNormalizer
^^^^^^^^^^^^^^^^^^^^^^^

- 职责：还原混淆器插入的入口参数槽置换——删除搬移序言并把方法体局部访问映射回原槽；另认领「this 别名」形态（``load 0; store k``\ 前缀后槽 0 被复用）。
- 判据锚：搬移置换 = 连续 load 段 + 等长 store 段、符号整段后栈空且构成槽集合上的非恒等双射、序言无标签锚定；this 别名 = 槽 0 首访问是写且支配全部后续读。
- 拒绝面：序言删除被标签/异常表边界拒绝即跳过；方法体含 jsr/ret、同槽引用与基本类型混用、宽值占槽跳过。
- 验收门：``entry_slot_swap_normalizer_test``\ 。
- 链序：本地槽类型随变换改变——带分支的方法必须由管线注入 ``ITypeResolver``\ 重算 Strict 帧并再生 StackMapTable，不接线会留下按旧槽类型编码的帧。

.. _pass-catalog-method-cfg:

9. 方法级控制流与布局族
-----------------------

方法级控制流重构：反混淆（跳板消解、反扁平化、异常表收口）与布局整理（冗余跳转、GOTO 重排），外加控制流噪音注入件。本族多数成员的新分支目标/改写帧依赖管线帧链接线（\ ``PassPipelineOptions::typeResolver``\ ）。

.. _pass-catalog-junk-code-inserter:

JunkCodeInserter
^^^^^^^^^^^^^^^^

- 职责：在方法体安全边界插入确定性垃圾分支块（5 指令、栈效应净零、不可抛）——插入点与形态由位置决定，无 RNG、产物可复现可入 golden。
- 判据锚：每第 8 个位置且前驱是顺序 fallthrough、目标不是异常 handler 入口；maxStack 防御性抬升。
- 拒绝面：无条件转移之后不插（产出死代码无从验证帧）；handler 入口不插。未接 resolver 管线时新分支目标无帧、产出类会被 V50+ verifier 拒收——须与 typeResolver 联用。
- 验收门：``junk_code_inserter_test``\ + ``junk_code_jvm_test``\ （JVM 真跑），外加确定性自基线对拍（golden_self JunkCodeInserter 道）。
- 链序：A1 帧链首批真实消费者，内置管线 ``phase.junk-code-insert``\ 。

.. _pass-catalog-exception-trampoline-inliner:

ExceptionTrampolineInliner
^^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：异常流混淆的强制跳转编码「\ ``aconst_null; athrow``\ 保护区 + 紧邻 catch-any handler + 首条 ``pop``\ 」整组消解为直落控制流。
- 判据锚：保护区恰两条指令、handler 与保护区末锚同一指令、handler 首指令 ``POP``\ 、帧引擎证 ``aconst_null``\ 到达帧栈深为 0；指向保护区的分支重定向到 handler 体首指令。
- 拒绝面：catch 类型非 any、保护区含额外指令、handler 非紧贴、表项交叠/共享 handler 头、ATHROW 或 POP 为分支目标——逐项保守跳过，方法内其余跳板不受牵连。
- 验收门：``exception_trampoline_inliner_test``\ 。

.. _pass-catalog-flattening-reverser:

FlatteningReverser
^^^^^^^^^^^^^^^^^^

- 职责：控制流反扁平化——常量状态传播 + switch 分发静态解析，前驱 goto 直接连到 case 目标、分发块失去全部入边（通用引擎 + 构造期可注入形态表；相对更新 ``s += D``\ 形态走状态迹模拟）。
- 判据锚：全方法 switch 唯一、分发操作数可形态表解析、状态槽读写全量审计（读点唯一）、前驱常量链可静态求值命中案表。
- 拒绝面：多 switch、分发块居首、状态槽第二读点、前驱分支锚入分发块区间、案表自引用、迹模拟不定值、帧深等价比对不成立——整方法拒收、不做部分改写。
- 验收门：``flattening_reverser_test``\ 。
- 链序：不可达分发块交 UnreachableInsnRemover；状态写死存与中转弹跳交 store→load 转发清理面。

.. _pass-catalog-try-catch-cleanup:

TryCatchCleanup
^^^^^^^^^^^^^^^

- 职责：异常表条目收口——修复面（悬空标签/空区间条目删除）、透明重抛删除、恒不抛收口（域分析可达帧 + 保守可抛集合）、NPE 形改写、自捕抛改写。
- 判据锚：透明重抛须 handler 不被共享、正常流不可达、无重叠表项；NPE 形与自捕抛形有栈深门槛与锚入标签守卫。
- 拒绝面：域分析失败、JSR/RET、描述符或标签解析不齐——对应规则放弃、保守保留；乱序条目保留（合法类文件允许）；任一不确定整条保留。
- 验收门：``try_catch_cleanup_test``\ 。

.. _pass-catalog-redundant-jump-remover:

RedundantJumpRemover
^^^^^^^^^^^^^^^^^^^^

- 职责：分支目标落在紧邻顺序后继前置边界的跳转消解——``GOTO``\ 整条删除，单值条件跳原位换 ``POP``\ 、双值条件跳换 ``POP; POP``\ （两路径同点，条件值被丢弃）。
- 拒绝面：方法末指令无顺序后继天然不匹配（GOTO 删除无承接者保守跳过）；跳转自身锚标签不阻断（fallthrough 重定向承接、原位改写标签原地有效）。
- 验收门：``redundant_jump_remover_test``\ 。
- 链序：标签布局族首件；每轮快照全扫 + 外层不动点。

.. _pass-catalog-goto-rearranger:

GotoRearranger
^^^^^^^^^^^^^^

- 职责：GOTO 目标尾段整体搬移（libjbc ``moveRange``\ 区间搬移原语），使 GOTO 退化为可消除的 fallthrough 冗余跳转再按家族同款整删。
- 判据锚：首件窗口仅前向非邻接目标、尾段 = 目标标签至方法末；两步序列的全部失败条件在搬移前由门排除。
- 拒绝面：含 JSR/RET 方法整体不动；异常表锚定段内或 GOTO 边界拒动；目标非单前驱拒动；段内有段外回边拒动（宁窄勿错）。
- 验收门：``goto_rearranger_test``\ 。
- 链序：标签布局族第二件，衔接 RedundantJumpRemover 的邻接形态。

.. _pass-catalog-method-reorder:

10. 方法级算术重排面
--------------------

方法级、不改值只动布局的两件——把同槽算术与常量归拢，为窗口化简族拓宽命中面。

.. _pass-catalog-arithmetic-insn-rearranger:

ArithmeticInsnRearranger
^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：连续的「\ ``xLOAD s; CONST;``\ 可交换算术\ ``; xSTORE s``\ 」四元块按槽首现序分组重排（块内顺序不变、整块搬移、同槽保相对序）。
- 判据锚：块匹配四条连续指令（快捷形与泛形同判）；≥ 2 块才成窗。
- 拒绝面：窗口起点或任一内部边界锚标签整窗拒收（锚点随重排漂移会改变入边落点）。
- 验收门：``insn_rearranger_test``\ （同头文件两件共用套件）。

.. _pass-catalog-commutative-operator-reorder:

CommutativeOperatorReorder
^^^^^^^^^^^^^^^^^^^^^^^^^^

- 职责：int/long 交换律运算链内把常量操作数稳定后移（非常量在前、常量在后、算术原位），使常量相邻、常量折叠可命中。
- 判据锚：链形态 = 操作数 n+1 个 + 同一可交换算术 n 条；操作数与算术宽度一致。
- 拒绝面：链起点锚标签该起点不成链（fail-closed）；宽度不一致、形状不齐不成链。
- 验收门：``insn_rearranger_test``\ 。

非 pass 支撑件
--------------

``pass/``\ 目录内不注册为 pass 的支撑件，不设规格节：``window_rule.hpp``\ （窗口匹配 + 原子替换共享引擎）、``local_op.hpp``\ （局部变量指令解码）、``method_rewrite_util.hpp``\ （常量装载判定与物化共享面）、``default_pass_registry.cpp``\ （默认注册表组合根）、``builtin_pipelines.cpp``\ （内置命名管线装配——\ ``default-optimize``\ 与九个 ``phase.*``\ 管线）。注册面接口在 spi（\ :ref:`pass-contract`\ §1 的三级作用域基类），装配落在本目录、依赖方向 pass ⇒ spi 单向。
