.. _string-constant-spec:

字符串常量族规格（CharArrToStrSimplifier / ConstCharArrayStringFolder / StringConcatIndirector / GlobalStrPoolOptimizer）
=========================================================================================================================

状态：已落地（判据口径以各件 ``///`` 头注为权威面，本文承载四件分工、逐件判据整理与链序申报），2026-11-08。

1. 动机与四件分工
-----------------

字符串是混淆器最爱藏东西的地方：字符串内容被拆成 char 数组逐元素构造（打
断字符串池的常量折叠与文本检索）、拼接被拆成 StringBuilder 链、散落各类
的字符串常量让分析面无法一处收敛。本族四件从四个正交方向治理：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 件
     - 作用域
     - 方向
     - 语义目标
   * - CharArrToStrSimplifier
     - 方法级
     - 还原
     - 裸 char[] 字面构造 → ``ldc`` + ``toCharArray`` （保留数组语义）
   * - ConstCharArrayStringFolder
     - 方法级
     - 还原
     - ``new String(常量 char[])`` 整窗 → 最终字符串 ``ldc``
   * - StringConcatIndirector
     - 方法级
     - 还原
     - StringBuilder 拼接链 → indy ``makeConcatWithConstants`` （javac 9+ 规范形）
   * - GlobalStrPoolOptimizer
     - 容器级
     - 收敛
     - 全容器 String ldc 去重迁入合成字符串池类，ldc 改索引 + 取值调用

前三件是混淆「还原」方向（把非常量的构造形态收回常量 / 规范形），GSPO 是
分析面「收敛」方向（把常量面集中到一处——产物是混淆方向的，属 opt-in 治
理件而非还原件）。四件全部 ``stability:opt-in``\ ，发生改写返回 ``none()``
\ ，否则 ``all()`` 。

2. 逐件判据
-----------

.. _string-constant-cas:

2.1 CharArrToStrSimplifier（保留数组语义的还原）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

匹配矩阵：定长声明（``ICONST_n`` / BIPUSH / SIPUSH / LDC Int，len ≥ 0）+
``NEWARRAY T_CHAR`` + (``DUP; ICONST_j;`` 常量值; ``CASTORE`` ) × len。
改写产物 ``ldc "<text>"; invokevirtual String.toCharArray ()[C`` ——序列
值语义不变（数组内容 = 字符串的 char 序列）、栈效应不变（都是净推 1 个
char[]）。字符按 JVM int→char 截断语义收进 UTF-16 码元（CASTORE 截断），
经 UTF-16 → UTF-8 → MUTF-8 编码 intern；编码失败的码元序列按整组失配处
理。边界守卫：除首成员外任何槽位的前置边界有存活标签锚定 → 整组不匹配；
首成员锚定标签时照常改写（标签迁移到替换头）。

.. _string-constant-ccasf:

2.2 ConstCharArrayStringFolder（整窗折叠）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

只处理「完整、连续」的形状：``new java/lang/String`` + ``dup`` + 常量长度
表达式 + ``newarray char`` + 逐元素（``dup`` / 常量下标表达式 / 常量值表
达式 / ``castore`` ）+ ``invokespecial String.<init>([C)V`` 。长度、下标、
每个元素值都必须是可证明的整型常量表达式（常量装载与 ``iadd``\ /
``isub``\ /\ ``imul``\ /\ ``ixor``\ /\ ``iand``\ /\ ``ior``\ /\ ``ishl``
\ /\ ``ishr``\ /\ ``iushr``\ /\ ``ineg``\ /\ ``i2c``\ /\ ``i2b``\ /\ ``i2s``
的组合，不含可能陷入的 ``idiv``\ /\ ``irem`` ），表达式窗口内不允许任何其
他指令混入，求值栈严格清空（栈残留即失配）。下标允许任意次序与同下标二次
写（最后一次生效，Java 数组语义），但必须恰好覆盖 [0, length)。

拒绝面：任意别名、逃逸、未覆盖 / 越界下标、非 ``T_CHAR`` 数组、非法
surrogate（孤立代理对编不成合法字符串）、窗内任一成员锚定 incoming 标签
——整组保持原指令不动。替换结果：整窗折叠为单条 ``ldc "<解码内容>"``
（池下标超 u1 域升格 ``ldc_w``\ ；唯一 String 常量经 ``internString`` 追
加进池），首成员锚定标签迁移到新 ldc，其余成员 ``eraseBatch(kRejectIfUsed)``
原子删除（批内互指不算引用）。与 2.1 的互补：那件改写「裸数组字面构造」
（消费方拿数组），本件折叠「整个构造串」（消费方拿 String）。

.. _string-constant-sci:

2.3 StringConcatIndirector（拼接规范形）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

识别窗（严格直线、逐指令相邻）：``NEW StringBuilder; DUP; (LDC String)?;
INVOKESPECIAL <init>(String)?V; (ARG; INVOKEVIRTUAL append(X))+;
INVOKEVIRTUAL toString()`` 。改写：NEW 原位改写为 INVOKEDYNAMIC（实参随后
按原位压栈），DUP / init / append / toString 批量 erase（fallthrough 重
定向链式闭合）；栈形净效应不变（弹全部拼接实参 + 压 String）。

实参认领面（fail-closed）：每 append 的实参为「自包含纯计算链」（反向需值
抽取，cat2 记一个值）——常量推入（ICONST 族 / BIPUSH / SIPUSH / LCONST 族
/ FCONST 族 / DCONST 族 / LDC 数值与引用常量 / ``ACONST_NULL``\ ；condy
不认领，bootstrap 有求值副作用）、原始局部装载族（ALOAD 静态型未知不入
链）、二元纯算术 / 移位 / 位 / 比较、取负与数值转换。栈操作 / 调用 / 字段
/ 数组 / 分配不入链，需求未尽遇链外指令整窗拒收。

安全门：类版本 ≥ 53（``StringConcatFactory`` 入场线）；窗内任何成员（含
NEW 头）锚定存活标签即拒收（分支目标进入链体在擦除后语义不保，异常表
Boundary 锚定同拒）；``<init>(String)`` 实参须为 LDC 字符串常量；append
返回型须为 StringBuilder。BSM 条目经 ``appendBootstrapMethod`` 同形复用
（配方全实参形态 ``\u0001`` ×N），indy CP 条目经 ``internDynamic``\ ；任
一 intern/erase 失败整窗放弃（无半删）。常量不嵌入配方（全部 ``\u0001``
槽位）——配方嵌入属后续精化挂账。

.. _string-constant-gspo:

2.4 GlobalStrPoolOptimizer（容器级收敛）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

流程四步：

1. 池类已存在（``str/Pool`` URI 命中）→ 整件跳过（幂等）；
2. 全量普查：逐类资源建会话，收集全部 String ldc 常量内容去重编号（索引
   按遭遇序、clinit 写出序为字典序）；任一类资源不可解析 → 整件放弃（普
   查全量性前提）；非 class 资源忽略；
3. 改写：命中 ldc 原位改写为索引装载（``materializeIntConst`` 波段物化，
   BIPUSH/SIPUSH/LDC）+ 紧随 ``INVOKESTATIC str/Pool.getStr(I)Ljava/lang/String;``
   ——对是净栈中性替换（调用弹索引推引用），分支目标帧不漂移，无需帧再
   生；改写方法 maxStack 保守 +1；
4. 合成 helper：``str/Pool`` 类（public static ``String[] strPool``\ 、
   ``<init>``\ 、``<clinit>``\ （``new String[n]`` + 逐项 aastore）、
   private static ``getStr(I)``\ ），序列化后入容器。

安全门：任一改写方法 lower 或序列化失败 → 整件放弃（两阶段提交：改写批
次与 helper 入容器前全部序列化成功才动容器）。相对旧栈的偏离逐条申报在
头注（池类名中性化、clinit 计数走 ``materializeIntConst``\ 、maxStack 保
守上界、helper 全合成模型构建）。「改写使字符串内容面失效」——下游对字
符串常量的假设（内容检索、去重、本族前三件的窗口匹配）须在本件之后重查，
链序申报见 §3。

3. 互补与链序
-------------

- **CAS ↔ CCASF 互补**：两者吃同一族混淆形态（常量 char 数组构造）的不
  同消费面——CAS 认领「消费方拿数组」的裸构造（序列值不变，保留
  ``toCharArray`` 一步），CCASF 认领「消费方拿 String」的完整构造串（整
  窗消失）。同形态只会被其中一件命中，互不冲突。
- **SCI 与 GSPO 正交**：SCI 的 indy 产物里拼接常量进 BSM 配方（非 ldc 形
  态），GSPO 只迁 String ldc——SCI 先行不会给 GSPO 制造噪声，GSPO 也不会
  摸到 SCI 的配方常量。
- **GSPO 收口位置**：GSPO 使字符串内容面失效（原 ldc 全部消失），任何依
  赖字符串常量内容的 pass（含 CAS / CCASF / SCI 的匹配窗、字符串相关检
  索面）应在 GSPO 「之前」运行；GSPO 之后字符串只存在于池类
  ``<clinit>`` 与 ``getStr`` 取值调用一处。
- 三件方法级还原件全部幂等（还原产物不再构成匹配窗），GSPO 幂等靠池类
  URI 存在性门。

4. jasm 前后对照
----------------

CAS 与 CCASF 认领同一族形态的两个消费面（``"AB"`` 的 char 序列构造）：

.. code-block:: text

    // 混淆态 A（消费方拿数组）——CAS 窗
        iconst_2
        newarray char
        dup
        iconst_0
        bipush 65            // 'A'
        castore
        dup
        iconst_1
        bipush 66            // 'B'
        castore
    // CAS 产物（数组语义保留）
        ldc "AB"
        invokevirtual java/lang/String.toCharArray ()[C

    // 混淆态 B（消费方拿 String）——CCASF 整窗
        new java/lang/String
        dup
        iconst_2
        newarray char
        dup
        iconst_0
        bipush 65
        castore
        dup
        iconst_1
        bipush 66
        castore
        invokespecial java/lang/String.<init> ([C)V
    // CCASF 产物（整窗折叠）
        ldc "AB"

SCI 的拼接还原（``"a" + x + "b"`` 的 StringBuilder 形态）：

.. code-block:: text

    // 混淆态
        new java/lang/StringBuilder
        dup
        ldc "a"
        invokespecial java/lang/StringBuilder.<init> (Ljava/lang/String;)V
        aload_1              // append 实参：原始局部装载（入链）
        invokevirtual java/lang/StringBuilder.append (Ljava/lang/String;)Ljava/lang/StringBuilder;
        ldc "b"
        invokevirtual java/lang/StringBuilder.append (Ljava/lang/String;)Ljava/lang/StringBuilder;
        invokevirtual java/lang/StringBuilder.toString ()Ljava/lang/String;
    // SCI 产物
        aload_1              // 纯计算链指令保留原位、在更低栈深执行
        invokedynamic makeConcatWithConstants ("a", "b" 配方, \u0001 ×2)
                              // NEW 原位改写；DUP/init/append/toString 擦除

GSPO 的收敛（两类的 ldc 去重迁池）：

.. code-block:: text

    // 改写前：A 类 ldc "hello"、B 类 ldc "hello"（重复）
    // 改写后：两处均为
        iconst_0             // 去重后的索引（materializeIntConst 波段）
        invokestatic str/Pool.getStr (I)Ljava/lang/String;
    // 池类 str/Pool：<clinit> 组装 String[]{ "hello", … }，getStr(I) 取值

5. 边界申报
-----------

1. **CCASF 形状封闭**：表达式窗口混入任何非常量生产者、``idiv``\ /
   ``irem`` 陷入面、孤立 surrogate、下标不齐——整组不动，不做部分折叠。
2. **SCI 实参链封闭**：栈操作 / 调用 / 字段 / 数组 / 分配不入实参链，condy
   不认领；实参链指令保留原位（在更低栈深执行），原 maxStack 天然覆盖链
   内峰值。
3. **SCI 配方不嵌常量**：全部 ``\u0001`` 槽位（常量也作实参传入）——配方
   嵌入（``\u0001`` 与字面混合）属后续精化。
4. **GSPO 普查全量性**：任一类不可解析整件放弃（不做部分迁移）；池类已存
   在整件跳过（不合并既有池）。
5. **GSPO 字符串面失效**：改写后容器内不再有原字符串 ldc，下游字符串常量
   假设必须重查（§3 链序申报）。
6. **CAS 编码失败整组失配**：MUTF-8 编不出（畸形码元序列）不产出半截改
   写。

6. 测试面
---------

四套件全在 ``mjbase/test/unittest/pass/``\ ，方法级经共享驱动、容器级经
会话驱动验证真实写回产物：

- ``char_arr_to_str_simplifier_test``\ ：ASCII 与 UTF-16 内容还原、计数不
  齐不动、非常量元素不动、注册挂名；golden 对拍（CharArrToStrSimplifier
  道）。
- ``const_char_array_string_folder_test``\ ：ASCII 与混淆常量表达式折叠、
  乱序 / 重复下标、``idiv`` 表达式、未覆盖下标、未知常量生产者、孤立
  surrogate、非 char 数组、错误构造器、窗内锚标签——十一面全矩阵、注册
  挂名；golden 对拍（ConstCharArrayStringFolder 道）。
- ``string_concat_indirector_test``\ ：javac 规范形与无参链、双子链 BSM
  同形复用、纯计算链实参（含 long）、栈操作实参 / 链内标签 / StringBuffer
  / 版本门四路拒收、幂等与写回、注册挂名；``string_concat_indirector_jvm_test``
  （JVM 真跑门：变换前后行为对拍）。
- ``global_str_pool_optimizer_test``\ ：迁池与跨类去重、非 String ldc 不
  动、非 class 资源忽略、池类已存在跳过、不可解析整件放弃、无字符串
  no-op、二跑 no-op、注册挂名。
