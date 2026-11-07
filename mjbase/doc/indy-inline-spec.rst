.. _indy-inline-spec:

indy 站点内联规格（IndyResolver SPI + IndySiteInliner）
=======================================================

状态：定稿，已落地。微观契约（resolver 视图寿命、判据逐条、放弃清单）在
``indy_resolver.hpp``\ 与 ``indy_site_inliner.hpp``\ 两处头注；本文承载 SPI
契约摘要、调用形态改写矩阵与前后 jasm 对照，读法：先 §2 定「resolver 是
什么」，再 §4 判「站点吃不吃」，§5 看改写怎么落，§6 是字节级实样。

.. _indy-inline-ecosystem:

1. 生态位与双层分工
--------------------

mjbase 全库此前的 invokedynamic 面只有反方向件（``StringConcatIndirector``
造 indy 拼接站），没有还原面。本批装机制不装知识：通用层（``mjbase/spi``
的 resolver 契约 + ``mjbase/pass`` 的内联 pass）只负责站点事实采集、
resolver 迭代次序、目标判据复核与字节改写；具体还原知识由插件/宿主层以
resolver 实现供给（混淆器家族的键链解密、互证判据等不进通用层）。机制
借鉴 Recaf 的 ``InvokeDynamicResolver``\ /\ ``InvokeDynamicResolverManager``
\ /\ ``InvokeDynamicInliningTransformer``（只读参考），接口形态按 mjbase
惯例重塑：错误走 ``Result`` 值语义、文本走借用视图、注册面显式拒绝重名。

生成面语义的两族站点——``LambdaMetafactory``\ /\ ``StringConcatFactory``
——在采集阶段直接排除（§3），不经 resolver、不改字节。

.. _indy-inline-spi:

2. SPI 契约（IndyResolver）
---------------------------

resolver 是「混淆器家族知识」的唯一持有者，两段契约：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 段
     - 签名
     - 语义
   * - setup
     - ``setup(container) → Result<void, IndyResolverError>``
     - 容器级准备（扫 BSM 表、驻留解密键等），一次运行调用一次；失败
       ``SetupFailed`` 时该 resolver 本轮整体排除，不影响其余 resolver。
   * - resolve
     - ``resolve(facts) → Result<ResolvedIndyTarget, IndyResolverError>``
     - 逐站点纯查询。命中返回 ``Ok(target)``\ ；不归本 resolver 返回
       ``Err(NoTarget)``\ ；内部失败返回 ``Err(ResolveFailed)``。任何路径
       都不产生副作用——判据复核与字节落笔归 pass。

事实包 ``IndySiteFacts`` 交付站点只读面：调用者类/方法三段、indy 调用点
name/desc、BSM 方法句柄、BSM 静态参数按序列表（九族 CP 常量值形态；
结构畸形的条目以 ``Unreadable`` 占位，位置对应不因读失败错位）。目标
``ResolvedIndyTarget`` 四段加一数：owner/name/desc、调用形态
（``Static``\ /\ ``Virtual``\ /\ ``Interface``\ ）、尾部元数据实参个数。

管理器 ``IndyResolverManager`` 定迭代纪律：priority 降序、同优先级按
``name()`` 升序——同一站点在多次运行间命中同一 resolver，顺序不依赖注册
次序；逐级回落（``NoTarget``\ /\ ``ResolveFailed`` 都换下一个），全员未
命中即 ``Err(NoTarget)`` = 保留原站。文本视图寿命：resolve 形参视图只在
单次调用内有效（借用采集会话），返回目标视图与 resolver 对象等寿——动
态物化的文本由 resolver 自行驻留自有存储（``KeyPool``\ /
``Utf8StringArena`` 家族）。

.. _indy-inline-collection:

3. 站点采集与生成面排除
------------------------

pass 逐类装载会话、逐方法升格后扫描 ``INVOKEDYNAMIC``\ ，家族判定消费
``asm_classpath`` 的 BSM 解析面（``resolveIndyInsn``\ ）：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 家族
     - 处置
   * - LambdaMetafactory / StringConcatFactory
     - 生成面语义：直接跳过，resolver 不被询问，站点字节原样保留。
   * - 其余（含 Unknown）
     - 候选站点：采集事实包 → resolver → §4 判据 → §5 落笔。

.. _indy-inline-matrix:

4. 调用形态改写矩阵
--------------------

设站点描述符参数段为 Ps、返回段 Rs；目标四段 owner/name/desc 与形态；
``k`` = 尾部元数据实参个数。描述符等形 = 结构逐位同（kind / primitive /
类内部名 / 数组维数），不做装箱、拆箱、加宽或 ``checkcast`` 适配——适配
器是后续组合件，本件形态不合即拒。

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 门
     - 判据
   * - 尾参越界
     - ``k ≤ |Ps|``
   * - Static
     - represented = Ps 去尾 k 个；``|represented| = |Pt|``\ ，逐参数等形；
       ``Rs = Rt``\ 。落笔 ``invokestatic`` + Methodref。
   * - Virtual
     - ``|represented| = |Pt| + 1``\ ；首参数 = 接收者（非数组类形且内部名
       = owner）\ ；余参数与 Pt 逐等形；``Rs = Rt``\ 。落笔
       ``invokevirtual`` + Methodref。
   * - Interface
     - 判据同 Virtual。落笔 ``invokeinterface`` + InterfaceMethodref
       （count 按目标描述符重算，intern 面 libjbc 自理）。
   * - 特殊名防御
     - 目标名 ``<init>``\ /\ ``<clinit>`` 一律拒（构造器不在直接调用面）。
   * - v1 不表达面
     - ``invokespecial``\ （super 调用/构造器）与字段句柄族（getfield/
       putfield/getstatic/putstatic）：resolver 契约的三态 Form 表达不出，
       结构性 fail-closed。

.. _indy-inline-apply:

5. 改写落笔
------------

两路落笔，栈/帧不变量一致：改写前后站点弹推形状相同（弹 represented、
压 Rs），StackMapTable 逐点不动，Code 长度差由管线统一 lower 重定位消化。

- **k = 0（原位改写）**\ ：只改 opcode 与 CP 引用，无插入；站点指令保留
  原位，标签锚定与帧边界不受影响。
- **k > 0（实参暂存洗牌）**\ ：全部站点实参按逆参序存入 fresh 局部（槽号
  自 ``maxLocals`` 起、槽宽按描述符），再按 represented 正参序装载回栈，
  然后原位改写调用指令；``maxLocals`` 相应抬升，maxStack 不抬（洗牌峰值
  不超过原站点全参数在栈的峰值）。洗牌改变站点前置边界形态：站点指令上
  锚定有标签（分支目标/异常表/调试边界）时拒绝该站点。fresh 槽号受显式
  局部编码上界约束（IR 不产 WIDE），越界即拒。

放弃清单（保守保留，字节原样）：resolver 无目标或失败、任一判据门不过、
站点锚定标签且需洗牌、描述符解析失败、插入/改写/intern 拒绝；装载/升格/
lower/序列化失败按整类放弃（容器不动）。整类无改写不写回——「无目标保留
原站」在容器面就是逐字节不动。幂等：改写后站点已是直接调用，二次运行无
候选站点，严格 no-op。

.. _indy-inline-jasm:

6. 前后 jasm 对照
------------------

静态 exact 面（站点 ``(I)I``\ ，resolver 应答 static ``T1.real(I)I``\ ）：

.. code-block:: text

    改写前（run）                          改写后（run）
    iload_0                                iload_0
    invokedynamic enc (I)I {...} { }       invokestatic t/T1.real (I)I
    ireturn                                ireturn

尾参洗牌面（站点 ``(I Ljava/lang/String;)I``\ ，resolver 应答 static
``T4.real2(I)I`` + trailing = 1\ ）——元数据实参存 fresh 局部后丢弃，目标
实参按序装载：

.. code-block:: text

    改写前（run）                                  改写后（run）
    iload_0                                        iload_0
    ldc "meta"                                     istore 2
    invokedynamic dec (ILjava/lang/String;)I       astore 3
            {...} { }                              iload 0（→ iload_0 紧凑形）
    ireturn                                        invokestatic t/T4.real2 (I)I
                                                   ireturn

（jasm 反汇编按槽号选紧凑/显式形态，上表按语义对齐。）

.. _indy-inline-accept:

7. 验收门
----------

- **单测**\ （``indy_resolver_test``\ ）：SPI 契约面——注册/重名/空指针、
  优先级排序跨注册次序稳定、setup 失败排除、首个命中与逐级回落、无目标
  控制流语义。
- **单测**\ （``indy_site_inliner_test``\ ，jasm 夹具）：测试内假 resolver
  走 SPI 全流程，正例三形态改写后 jasm 对照 + 结构断言（opcode 与目标三
  段）；拒绝面逐字节保留（无目标、形态不匹配、尾参越界、返回/参数不等
  形、ResolveFailed、生成面两族在 resolver 有应答时仍不动）；洗牌面
  指令形与 maxLocals 抬升；幂等二跑。
- **JVM 真跑门**\ （``indy_site_inliner_jvm_test``\ ）：javac 真样本
  （lambda + 字符串拼接）字节不动；自定义 indy（BSM 自带）与尾参洗牌、
  虚调用形态的 jasm 真跑件经测试 resolver 内联后 ``-Xverify:all`` 真跑，
  stdout 与原样本逐字节一致；幂等二跑。JDK 不可用整测跳过。
