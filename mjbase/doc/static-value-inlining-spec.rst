.. _static-value-inlining-spec:

静态常量收集与 GETSTATIC 读点物化规格（StaticValueCollection / StaticValueInlining）
====================================================================================

状态：定稿，随本批落地。微观契约（走廊准入集、预算常量、拒绝面逐条口径）在
``static_value_collection.hpp`` 头注与 ``static_value_inlining.hpp`` 头注；本文承载
两件职责边界、视图传递机制、判定流程与前后 jasm 对照，读法：先 §2 分清两件各自
吃什么产什么，再 §4 判「这个读点吃不吃」，§5 是收集侧逐桶判据，§7 是字节级实样。

.. _static-value-inlining-ecosystem:

1. 生态位
---------

真实混淆器普遍把常量散进伴生类的 static 字段（grunt 罐实证：2516 个类持有 93.8 万
个 GETSTATIC 站点），读点侧每次字段访问都要付一次类初始化检查与一次字段解析。本批
把 Recaf 侦察的第一吸收项（StaticValueCollectionTransformer /
StaticValueInliningTransformer）按「每件只做一件事，多组合」的形态收编为两件独立的
opt-in pass：收集件是容器级只读件，产出「字段身份 → 常量值」视图；物化件消费该
视图，把容器内 GETSTATIC 读点原位物化为等值常量装载。两件都默认链不收，CLI ``-p``
显式挂点；注册名 ``StaticValueCollection`` / ``StaticValueInlining``（scope 均为
global）。

与既有写侧件 StaticFieldDefaultInliner（clinit 单写折叠进字段 ConstantValue 属性，
类级）组合成链而不互相依赖、互不修改：SFDI 把 clinit 常量对折叠进 ConstantValue
并删除等价初始化指令后，收集件走 ConstantValue 属性面照收（§5.5）——同一条管线里
「写侧折叠 → 视图发布 → 读点物化」三段各自独立成立。

.. _static-value-inlining-split:

2. 两件职责边界
---------------

- **StaticValueCollection（收集件）**：容器级只读件。逐类评估 ``<clinit>``
  对本类 static 字段的常量/纯算术写（常量走廊求值），连同既有 ConstantValue
  属性面，产出 StaticValueView（键 = owner/name/desc 三段，值 = Int/Long/Float/
  Double/Str 五族）。字节零改动，恒返回 all()。
- **StaticValueInlining（物化件）**：容器级直写件。容器内 GETSTATIC 读点按视图
  原位物化为等值常量装载（短形态工厂 / ldc 族），两阶段提交，发生改写返回
  none()。不产视图、不做求值——所有「值从哪来」的判断都在收集件。

.. _static-value-inlining-view:

3. 视图传递与独立启停
---------------------

视图经 GlobalAnalysisManager 的 pinned 面传递：收集件
``setPinned<StaticValueViewAnalysis>(view)``，物化件 ``getPinned<>`` 消费。走
pinned 是刻意的两个原因：

- 视图必须在收集件返回之后的全部后续 pass（含中间变更件的 none() 全量失效）之下
  存活——普通结果缓存做不到，pinned 结果按契约不受 invalidate/clear 影响；
- 「链上没有收集件就没有视图」：物化件查不到 pinned 视图（或视图为空）即严格
  no-op 返回 all()，绝不自行重算——两件独立启停的组合语义由此保证。

收集件无内容时不发布并撤钉（幂等二跑覆盖式重发布，语义恒定）。

.. _static-value-inlining-site:

4. 适用形态：读点站点判据
-------------------------

物化件吃的站点是容器内任意方法（``<clinit>`` / ``<init>`` 除外，见 §5.2）中的
``getstatic``，逐条判据：

- **视图命中**：(owner, name, desc) 三段精确查视图，未命中保留。三个自然落桶：
  owner 在容器外（视图只由容器内类产出）、字段未被收集面发布（判据未过 §5 任意
  桶）、经父类解析的继承读点——GETSTATIC 的 CP 操作数 owner 是解析起点类名而非
  声明类名，视图键按声明身份建键，继承读点键不命中即保守保留。
- **类别匹配（cat1/cat2）**：值类别须与字段描述符族一致（族表见 §5.4）。防御桶：
  收集面已保证一致，物化面复核防两件版本漂移。
- **volatile 复核**：owner 在容器内时对字段旗标二次复核，ACC_VOLATILE 保留读点
  （内存语义桶的物化件侧双保险，收集件同桶先拒）。
- **CP 容量**：常量物化前按 0xFFFF 红线预检（对齐 ConstantFolder 口径），超界
  保留读点。

所有拒绝面字节不动：单条读点保留不影响同方法其他读点。

.. _static-value-inlining-collect:

5. 收集判据（fail-closed 逐桶）
-------------------------------

.. _static-value-inlining-corridor:

5.1 走廊纯度门（类级）
^^^^^^^^^^^^^^^^^^^^^^

一个类的 clinit 要进走廊求值，指令集必须 ⊆ {常量装载（int/long/float/double/
String 五族），32 条纯算术二元 op（与 ConstantFolder 的 binOpOf 支持集逐条对齐），
NOP，PUTSTATIC（owner==本类），RETURN}，且异常表为空。走廊外的任何指令——invoke
家族、分支与 switch、foreign 静态访问、监视器、athrow、aconst_null、数组操作、
转换与取负——整类不发布（fail closed：本类一个条目都不出，而非只拒涉事字段）。

这条门同时是物化件安全性的根基：发布类的 clinit 无 invoke、无 foreign 访问、无
监视器、无抛出 ⇒ 类初始化没有可观察效应，物化件删掉一个 GETSTATIC 读点不会丢掉
「触发该类初始化」的副作用；分支缺席还使 clinit 的同类调用闭包坍缩为 clinit
自身，物化件无需闭包游走（§5.2）。

父链纯度：super 链逐级同判（链上类的 clinit 无 / 为走廊形 ⇒ 纯，继续上行）；链上
类不在容器且名字非 ``java/`` 前缀 ⇒ 拒（该父类的初始化效应不可证）；
``java/`` 截断（JDK 基座类 clinit 无可观察效应，Recaf 同口径）；父链环 ⇒ 拒。
门结果跨类 memo 复用。

.. _static-value-inlining-initface:

5.2 初始化语义面（物化件）
^^^^^^^^^^^^^^^^^^^^^^^^^^

``<clinit>`` 与 ``<init>`` 方法内的 GETSTATIC 读点一律跳过：clinit 内读点可能落在
写点之前（前向引用语义），构造器可能参与类初始化环。Recaf 原件还要把「clinit 可达
的同类方法闭包」整个加入跳过集；本批靠 §5.1 的走廊门（clinit 无 invoke）把该闭包
坍缩为 clinit 自身，跳过面收敛到初始化方法名集合，正确性不降。

.. _static-value-inlining-census:

5.3 单写者普查（容器全域）
^^^^^^^^^^^^^^^^^^^^^^^^^^

收集件对全容器做一次 PUTSTATIC 普查：(owner, name, desc) 的写点总数必须恰为 1 且
该写点落在 owner 类的 ``<clinit>()V`` 内。总数 > 1 是多次写冲突桶（最终值只在最后
一次写之后确定）；唯一写点不在 owner clinit 是非 clinit 写桶（含他类写、同类其他
方法写、构造器写）。两桶都按字段拒，不牵连同类其他字段。

.. _static-value-inlining-value:

5.4 值面与族表
^^^^^^^^^^^^^^

值读取：折叠后的 clinit 内，PUTSTATIC 的顺序前驱必须恰为一条匹配族常量装载。求值
不做自研算术——收集件在 scratch 会话（重解析字节，不回传容器）上跑既有
ConstantFolder，把走廊内常量算术链折叠为单条装载后按「装载 + PUTSTATIC」对读取；
除零 / int·long 的 MIN÷-1 红线、NaN 位图、移位掩宽等算术语义全部继承
ConstantFolder，本批零重复实现。折叠未发生的对（红线保守停）读不到常量装载，字段
自然落拒。

族表（两件共用同一张表，``static_value_family_kind`` 单源）：I/B/C/S/Z → Int（栈
面 int）、J → Long、F → Float、D → Double、``Ljava/lang/String;`` → Str。值按位图
存储（Float 低 32 位 / Double 64 位原始 IEEE-754 位，保 NaN payload 与 -0.0）。
族不匹配按字段拒。

字段面：字段必须存在、static、非 volatile（volatile 拒——语义是内存可见性收敛
面，物化会绕过 happens-before 边）；描述符不入族表拒。

.. _static-value-inlining-budget:

5.5 走廊预算
^^^^^^^^^^^^

clinit 指令数超过 ``kMaxCorridorInsns``（20000，Recaf max-steps 同量级）整类拒
（求值不收敛桶）。

.. _static-value-inlining-cv:

5.6 ConstantValue 属性面（链式收编）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

static + final + 非 volatile + 容器全域零写点的字段，其既有 ConstantValue 属性照
收进视图（解码面 ConstantValueAttr 与 Raw 槽双路，CP 值条目 Integer/Long/Float/
Double/String 五形态）。final 是硬判据——JVM 只对 static final 字段生效该属性
（JVMS 4.7.2），非 final 挂该属性的值运行时不可信。这一面是「SFDI 在前道工序产出
→ 本件照收 → 物化件读点消化」链的组合胶水，也是 javac 常量字段
（``static final int X = 5;`` 产 ConstantValue 不产 clinit 写）的原生形态。

.. _static-value-inlining-writeback:

6. 改写落盘与帧再生
-------------------

物化件是容器级直写（与 MethodCallRemover 同款骨架）：会话内 promote → CodeDom
原位改写（``dom.get`` 取可变槽，替换为常量装载）→ 嵌套属性布局适配 → lowerMethod
→ 分支体/异常表存在时 Strict 帧再生 → writeClass → 两阶段提交（全部类序列化成功
才动容器）。替换指令与 GETSTATIC 同类别同弹宽，栈形状恒等；单指令替换不引入新
控制流，偏移与 StackMapTable 由 lower relocation 链路承接。升格失败的方法保守不动；
lower / write 失败抛错交管线容错档位处置（容器尚未提交，无半改写态）。

.. _static-value-inlining-jasm:

7. 前后 jasm 对照
-----------------

素材取自单测夹具（``static_value_inlining_test.cpp``），标注对应测试名。jasm 为
示意写法，与夹具断言的指令序逐条对应。

7.1 cat1 跨类读点物化
^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``MaterializesCollectedCrossClassReads``。伴生类 ``T`` 的 clinit 写
``a = 42``（收集件发布 Int(42)），读方 ``R.m`` 跨类读：

.. code:: text

   # before —— T 的 clinit（收集件只读，改写后原样）
   .method static <clinit> ()V {
       bipush 42
       putstatic T.a I
       ldc "hello-const"
       putstatic T.s Ljava/lang/String;
       return
   }

   # before / after —— R.m（读点被物化）
   .method public static m ()I {
       getstatic T.a I        #  after: bipush 42
       ireturn
   }

7.2 cat2 与 String 读点物化
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

同测试。long 读点物化为 ``ldc2_w``（弹宽与 getstatic 同为 cat2，栈形状恒等）；
String 读点物化为 ``ldc``（字符串常量 intern 进读方常量池，命中既有池不膨胀）：

.. code:: text

   # before / after —— R.l
   .method public static l ()J {
       getstatic T.e J        #  after: ldc2_w 72057594037927936
       lreturn
   }

   # before / after —— R.n
   .method public static n ()Ljava/lang/String; {
       getstatic T.s Ljava/lang/String;    #  after: ldc "hello-const"
       areturn
   }

7.3 初始化语义面保留形态
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``SkipsReadsInsideClinit`` / ``SkipsReadsInsideInit``。clinit / 构造器内的
读点字节不动：

.. code:: text

   # before / after —— U.<clinit>（前向引用形态，读点保留）
   .method static <clinit> ()V {
       getstatic T.a I
       pop
       return
   }

.. _static-value-inlining-reject:

8. 拒绝面总表（判据 ↔ 单测名）
------------------------------

收集侧（``static_value_collection_test.cpp``）：

- volatile 字段拒（同场非 volatile 字段不受牵连）—— ``RejectsVolatileField``
- 多次写冲突拒 —— ``RejectsMultipleWrites``
- 非 clinit 写（同类其他方法）拒 —— ``RejectsNonClinitWrite``
- 他类写点拒 —— ``RejectsForeignClassWrite``
- 走廊不纯（clinit 含 invoke）整类拒 —— ``RejectsImpureCorridor``
- 父链 clinit 不纯拒 —— ``RejectsWhenSuperClinitImpure``
- 父类容器外且非 java/ 拒 —— ``RejectsWhenSuperOutsideContainer``
- 走廊预算（2 万）拒 / 预算内发布 —— ``RejectsOnCorridorBudget``
- 值族失配按字段拒 —— ``RejectsFamilyMismatchPair``
- ConstantValue 非 final 拒 —— ``NonFinalConstantValueNotHarvested``
- 不可解析类整件放弃 —— ``UnparseableClassAbortsWholePass``
- 只读面（容器字节恒等）—— ``ContainerBytesUntouched``
- 幂等二跑（覆盖式重发布）—— ``IdempotentRepublish``；无内容撤钉 ——
  ``EmptyResultUnpins``

物化侧（``static_value_inlining_test.cpp``）：

- 独立启停（无收集件严格 no-op）—— ``InlinerAloneIsStrictNoop``
- clinit 内读点保留 —— ``SkipsReadsInsideClinit``
- 构造器内读点保留 —— ``SkipsReadsInsideInit``
- volatile 旗标复核保留（手工视图注入反证）——
  ``VolatileFieldReadKeptByInlinerRecheck``
- cat1/cat2 类别失配保留（手工视图注入反证）—— ``CategoryMismatchReadKept``
- 幂等二跑（读点已消失）—— ``SecondChainRunIsNoop``

.. _static-value-inlining-boundary:

9. 边界与留账
-------------

- 与 StaticFieldDefaultInliner：链式组合（§1），不改其文件；两件对同一条 clinit
  常量对分别走「写侧折叠」与「读侧视图」，互不依赖。
- 数组型 static 字段不在族表（读点物化值面无 ldc 形态），GETSTATIC 数组读点保留；
  Recaf 的数组元素物化面（analyzer 驱动的第二趟）不在本批范围。
- aconst_null 形态的 null 常量不发布（走廊准入集不含 aconst_null）；需要时随走廊
  扩面批次加族。
- 接口维度：类初始化的 superinterface 默认方法触发面不在父链判据内（Recaf 同口径，
  super-chain-only），与 pass-contract §6 公共父类求解的接口不参与声明一致。
- 引用型常量（Class/MethodType/MethodHandle ldc）不在走廊准入集，clinit 含即整类
  拒；伴生常量类的真实形态（纯常量）不受影响。
