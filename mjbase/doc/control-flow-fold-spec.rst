.. _control-flow-fold-spec:

控制流折叠族规格（FlatteningReverser / ConsJumpOptimizer / ConstantSwitchFolder / GotoRearranger）
==================================================================================================

状态：已落地（判据口径以各件 ``///`` 头注为权威面，本文承载四件分工、共享纪律与逐件判据整理），2026-11-08。

1. 动机与四件分工
-----------------

控制流混淆的两类产物在本族收口：其一是「真值静态已定的分发残段」——常量化
不透明谓词、常量状态机分发，谓词值在静态期已定死，分支与 switch 只是无谓的
分发形式；其二是「跳转布局噪声」——无条件跳转把本可直落的顺序流人为搬走。
四件的分工：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 件
     - 吃什么
     - 产出什么
   * - ConsJumpOptimizer
     - 「常量装载 + 常量条件跳转」相邻窗
     - 恒真改 ``GOTO``\ ，恒假整窗删除
   * - ConstantSwitchFolder
     - 「int 常量装载 + switch」相邻窗
     - 常量头改 ``GOTO`` 复用命中分支目标，switch 删除
   * - FlatteningReverser
     - ``while(true) + switch`` 扁平化状态机
     - 前驱 goto 直连 case 目标，分发块失去全部入边
   * - GotoRearranger
     - 前向非邻接 GOTO（目标尾段布局）
     - 尾段搬移使 GOTO 退化为可删的 fallthrough 冗余跳转

四件全部 ``scope:method`` / ``stability:opt-in`` （默认链不收，CLI ``-p``
显式挂点），发生改写返回 ``none()``\ ，否则 ``all()``\ ，maxStack 不升
（窗口内峰值只降）。带新分支目标或改写帧的件依赖管线帧链接线（
``PassPipelineOptions::typeResolver``\ ），见 §5。

2. 折叠纪律（ConsJump / ConstSwitch 共享守卫）
----------------------------------------------

两件折叠器吃的是「相邻窗口」，共享同一套守卫纪律：

- 「窗口内部边界锚标签拒收」（栈消费保护）：折叠会取消窗口内的一次（双槽
  两次）栈消费；外部路径若从窗口中部锚入，其栈上的值正等待这次消费，折叠
  会让它失去消费者（VerifyError）。双槽窗的中位装载与条件跳转的前置边界、
  switch 的前置边界都在此列。
- **窗口头标签放行**：从头进入重放整个窗口，净栈效应为零（装载与消费成对
  消失），改写后仍为零；头部标签原地有效。
- **事务序**：改写按「先删尾、后改头」落笔——尾部（条件跳转 / 中位装载 /
  switch）删除在前且失败即中止本轮，头部改写最后发生，不留「头已改、尾未
  删」的半折叠态。
- **顺序末尾保守跳过**：方法末的删除没有承接者（fallthrough 重定向无落点）
  ——ConsJump 对顺序末尾条件跳删除保守跳过，ConstSwitch 对顺序末尾 switch
  同口径；这是对旧栈树模型照删形态的显式收紧。
- **不动点**：每轮快照全扫 + 外层不动点，本轮折叠暴露出的新相邻窗口下一轮
  继续。

.. _control-flow-fold-cons-jump:

2.1 ConsJumpOptimizer 三窗矩阵
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

常量装载面 = ``method_rewrite_util::intValueOf`` 全集（ICONST/BIPUSH/
SIPUSH/LDC 与 LDC_W 装载 int）：

- 单槽窗 ``[int 常量; IFEQ/IFNE/IFLT/IFGE/IFGT/IFLE]`` ：谓词按常量求值，
  恒真 → 窗口头改写 ``GOTO`` （复用原跳转目标标签）、条件跳转删除；恒假 →
  整窗删除（直落 fallthrough）。
- 单槽空窗 ``[ACONST_NULL; IFNULL/IFNONNULL]`` ：null 判定同上折叠。
- 双槽窗 ``[int 常量; int 常量; IF_ICMPxx]`` ：比较谓词按两常量求值，恒真
  → 首常量改写 ``GOTO`` 、其余两指令删除；恒假 → 三指令全删。

类型失配窗（int 常量配 IFNULL 族、``ACONST_NULL`` 配数值谓词）与
``IF_ACMP`` 引用比较族「不入矩阵」——旧栈对这类 Verifier 非法形态也按谓词
表折叠（防御怪癖），本实现显式拒绝：类型不匹配的窗口不做语义假设。

.. _control-flow-fold-const-switch:

2.2 ConstantSwitchFolder
^^^^^^^^^^^^^^^^^^^^^^^^

窗口 ``[int 常量; LOOKUPSWITCH/TABLESWITCH]``\ ，按 selector 值解析命中分
支（``switchCaseLabelOf``\ ：lookup 按 keys 精确匹配，table 按 ``low + i``
连续区间，未命中走 default），常量头原位改写 ``GOTO`` 复用命中目标标签，
switch 删除。目标标签解析失败（畸形 switch 的悬空目标）拒收。折叠后未被选
中的 case 标签失去引用者——CodeDom 中留为无引用标签，不物化进字节码，无需
显式销毁。幂等：折叠产物不再含常量 selector 窗，二次运行严格 no-op。

.. _control-flow-fold-reverser:

3. FlatteningReverser（反扁平化）
---------------------------------

.. _control-flow-fold-engine:

3.1 引擎与形态表
^^^^^^^^^^^^^^^^

扁平化混淆器把方法的真实功能块拆散成平级块、用状态机串联：状态变量 ``s``
（int 局部）在各功能块尾部写常量状态后 ``goto D``\ ；分发块 ``D`` 是
``iload s`` 混常量折叠步收尾于 lookupswitch/tableswitch；功能块尾再写下一
状态回到 D——CFG 成环，反编译器只能渲染状态机形态。反扁平化的推理骨架与
变体无关，因此本件分两层：

- 「引擎」（与变体无关）：全方法 switch 唯一性审计、分发操作数解析、状态
  槽读写全量审计、逐写点常量状态求值命中案表、入边守卫、帧深等价门、事务
  性统一改写。
- 「形态表」（构造期可注入的数据）：「哪些指令形态算状态装载 / 常量折叠
  步 / 状态写入」。默认表是保守首版——前驱写形态认 intConst 直存 + 单级中
  转弹跳（``istore t; iload t`` ），分发规格认单步 ``{iadd, isub, ixor}``
  偏移（含零步裸 iload），操作数序通用化：常量全在右、全在左（镜像）与混
  合操作数链（装载居中）由推值栈模拟统一识别。新变体 = 加表行或调规格，不
  改引擎；形态表若膨胀到自成体系，整体外移配方层、引擎留在本仓。

改写只动前驱 goto 的目标字段（复用 switch 的 case 标签句柄），状态链原样
保留；分发块失去全部入边变不可达，交 UnreachableInsnRemover 收敛。
``istore t; iload t`` 中转弹跳与状态写序列是死存噪声，交 store→load 转发
清理面（SLRE 族），本件不动。

.. _control-flow-fold-audit:

3.2 审计链
^^^^^^^^^^

改写前的全量审计，任一不成立整方法拒收：

1. 全方法 switch 恰一个（多 switch 读同一状态 = 多汇合点，无法单值传播）；
2. 分发块不居方法首指令（入口边不可静态解析）；
3. 分发操作数可形态表解析（状态槽单次装载 + 常量折叠步，步数 ≤ maxSteps）；
4. 状态槽读点唯一（恰为分发块读点；相对更新形态的 ``iload s`` 是读改写的
   一部分，按登记位置豁免；iinc 不整族禁用，其闭环由覆盖审计承担——不在
   迹上的 iinc 写点拒收）；
5. 状态槽写点全部可解析且写点后继是 ``goto D`` （交叠定值拒收；状态槽作为
   long/double 高半槽拒收）；
6. 非前驱分支 / switch 目标锚入分发块区间（含块头）、物理直落进块头、案表
   目标（含 default）落在分发块区间内（自引用）——全部拒收。

.. _control-flow-fold-relative:

3.3 相对状态更新与迹模拟
^^^^^^^^^^^^^^^^^^^^^^^^

前驱块尾不写绝对状态 K 而是相对更新 ``s += D`` （iinc 紧凑形，或
``[iload s; intConst D; iadd/isub/ixor; istore s]``\ ，IADD/IXOR 的常量在
左镜像同认；ISUB 的 D 在左是 ``D - s`` 形，不在族内）。相对写点的当前值引
擎解不出来，按状态机本身做数据流求解：

1. 写点分类：形态表命中 = 绝对 K；相对形态命中 = ``s += D``\ ；皆不命中 =
   拒收；
2. 案表供源：每个案表目标（含 default）从目标起「直线」扫描到首个状态写
   点；控制转移 / 分发块头 / 方法尾 = 无直线供源（终态块），块内分支形态
   fail-closed；
3. 入口锚：不被任何案表供源的绝对写点，唯一才可锚定（入口边必经方法头侧
   进入，不取道分发块；多候选拒收）；
4. 迹模拟：``v = fold(s)`` → 案表目标 → 供源写点 w → ``s' = K`` （绝对）
   或 ``s op D`` （相对）；同一写点被两个不同值走到（路径敏感 / 多 key 同
   案）→ 拒收；环闭合即停；迹终止后任一相对写点未定值 → 拒收。

.. _control-flow-fold-stackgate:

3.4 栈等价门
^^^^^^^^^^^^

状态链净栈效应为 0，goto 点栈深 = 前驱块入口栈深；原路径经 switch 派发
（switch 弹掉派发值）进入 case 目标，入口栈深同为该值。帧引擎逐前驱比对
goto 点与 case 入口的到达栈深，不等即整方法放弃。

.. _control-flow-fold-tx:

3.5 事务性与幂等
^^^^^^^^^^^^^^^^

改写阶段只做 goto 目标字段赋值（先全量取可变句柄后统一落笔，任一取柄失败
零副作用返回）。二次运行时写点审计因「goto 不指向 D」整族拒收，过程无增
量副作用——幂等。整方法拒收不做部分改写（部分改写会留下既丢分发、又留死
写的中间态）。

.. _control-flow-fold-goto-rearranger:

4. GotoRearranger（布局收敛）
-----------------------------

窗口（宁窄勿错）：方法内 GOTO/GOTO_W 「前向非邻接」跳过中间段 M、目标标签
L 锚定的尾段 S = [L, 方法末)。两步序列：用 libjbc ``moveRange``
（kHeadFollows）把 S 整体搬到 GOTO 之后——L 随段迁移锚定 GOTO 的新顺序后
继，GOTO 的目标与紧邻后继重合，按家族同款 fallthrough 等价整删（
RedundantJumpRemover 的边界形态）。可观测价值：无条件跳转次数 -1，方法尾
布局改写（尾段提前、M 段沉底）。kHeadFollows 是刻意的：L 的语义就是「段入
口」，必须随段迁移才能使 GOTO 目标与新顺序后继重合；kHeadStays 会把入口
标签留在原段位承接 M 的 fallthrough，与搬移意图相反。

保守门（每条可证伪）：

- 仅前向非邻接目标——邻接形态是 RedundantJumpRemover 的既有窗口不重收；后
  向目标（回边）搬尾段不可达，moveRange 只做重排、搬了跳转依旧；
- 含 JSR/RET 的方法整体不动——RET 的后继是栈上动态地址，前驱集无法在指令
  枚举面上保守闭包（单前驱门会漏计子程序边）；
- 异常表门——任一 entry 的 start/end/handler 锚定段内 [L, 末) 或锚定 GOTO
  自身前置边界 → 拒动（跨段边界的保护区搬移后覆盖集改变）；
- 单前驱门——L 的前驱恰好只有该 GOTO（M 尾 fallthrough 在 M 沉底后变方法
  末悬空 fallthrough，verifier 拒载）；
- 环形门——段内任一指令的控制目标锚定段外 → 拒动（跨段循环的循环体跨段重
  排需逐边证明帧链与布局效果，首件不证）。

两步序列原子性：「搬块 + 删 GOTO」的全部失败条件在搬移前由门与形态排除；
moveRange 本身校验先于变更、原子落盘。防御层：erase 意外 Err 时不回滚——
搬移后的布局仍是语义等价的合法形态，该 GOTO 的目标已变邻接、被本件窗口排
除，无死循环。帧面不做显式帧操作（与 RedundantJumpRemover 同口径）：删
GOTO 换来的 fallthrough 与 GOTO 同为恒等传函，各分支目标入边集与传函不
变，帧语义由管线 lower 后 rewireFramesAfterLower 重算承载。每轮快照全扫
+ 外层不动点，每个接受候选净删 1 条指令，循环终止。

5. 管线组合与帧链
-----------------

- ConsJump / ConstSwitch 的折叠产物为下游布局面供弹：未选中的分支失去引用
  者，RedundantJumpRemover / GotoRearranger 的窗口随之收窄；建议次序为常
  量折叠族（ConstantFolder / LocalConstantPropagation / ConstantPropagator）
  → 本族折叠件 → 布局面。
- FlatteningReverser 的残留（不可达分发块、状态写中转弹跳）分别交
  UnreachableInsnRemover 与 store→load 转发清理面收敛，头注声明该链序。
- GotoRearranger 衔接 RedundantJumpRemover 的邻接形态（标签布局族第二件）；
  两件同口径不做显式帧操作。
- 保守起见，带分支结构改写的件（FlatteningReverser / GotoRearranger）须
  与管线帧链接线（``typeResolver`` 注入）联用——未接线管线下帧重算缺位，
  产出类会被 V50+ verifier 拒收（JunkCodeInserter 同款约束口径）。

6. jasm 前后对照
----------------

ConsJump 双槽恒假窗（谓词真值静态已定）：

.. code-block:: text

    // 混淆态：sipush 100 与 bipush 50 比较，100 < 50 恒假
        sipush 100
        bipush 50
        if_icmplt L_true      // 三指令整窗删除，直落 fallthrough
        ...
    L_true:

    // 折叠态：三指令消失，控制流直落

ConstantSwitch selector 恒定（lookup 精确命中）：

.. code-block:: text

    // 混淆态
        iconst_2
        lookupswitch { 1: L_a, 2: L_b, default: L_x }
    // 折叠态
        goto L_b              // 常量头原位改写，switch 删除
                              // L_a/L_x 标签失去引用者，不物化进字节码

FlatteningReverser 的状态机消解（合成值示例，改写只动 goto 目标字段）：

.. code-block:: text

    // 混淆态
        入口: ldc 111111111; istore s; goto D
        D:    iload s; lookupswitch { 111111111: A, -222222222: B, default: X }
        A:    <真实块 A 体>; ldc -222222222; istore t; iload t; istore s; goto D
        B:    <真实块 B 体>; iconst_1; ireturn
        X:    iconst_0; ireturn
    // 改写态
        入口: ldc 111111111; istore s; goto A
        A:    <真实块 A 体>; ldc -222222222; istore t; iload t; istore s; goto B
                              // D 失去全部入边变不可达；状态链原样保留

GotoRearranger 的布局改写（地址序示意，非 jasm 逐指令对照）：

.. code-block:: text

    // 混淆态：GOTO 前向跳过 M 段到尾段 S
        [GOTO → L] [M 段] L: [S 段…方法末]
    // 搬移后：S 提前，GOTO 目标与新顺序后继重合
        [S 段…方法末] [M 段（沉底）] → GOTO 已删
                              // 前提：L 单前驱、段无外向回边、异常表不触段

7. 边界申报
-----------

1. **类型失配与引用比较不折**：ConsJump 对 Verifier 非法形态（类型失配
   窗、``IF_ACMP`` 族）显式拒绝，不复刻旧栈防御怪癖。
2. **顺序末尾不删**：两折叠器对顺序末尾的删除保守跳过（无承接者）。
3. **FlatteningReverser 整方法粒度**：任一审计不成立整方法拒收，不做部分
   改写；变体扩展靠形态表注入（构造期），不在判据面内逐变体写死。
4. **GotoRearranger 首件窗口窄**：仅前向非邻接、尾段至方法末、目标单前驱、
   无外向回边、无 JSR/RET、异常表不触段——多前驱目标块与跨段循环的布局改
   写整体排除（宁窄勿错），扩面交后续批次。
5. **帧链接线前提**：未接 ``typeResolver`` 的管线不要挂本族带分支结构改写
   的件（FlatteningReverser / GotoRearranger），产出会因帧缺失被 verifier
   拒载。

8. 测试面
---------

四套件全在 ``mjbase/test/unittest/pass/``\ ，方法级经共享驱动验证真实写回
产物：

- ``cons_jump_optimizer_test``\ ：三窗恒真改 GOTO / 恒假删除、窗内锚标签
  拒收、头标签放行、类型失配与 ``IF_ACMP`` 不入矩阵、分支跳入条件的菱形形
  态不动、注册挂名。
- ``constant_switch_folder_test``\ ：lookup/table 命中改 GOTO、未命中走
  default、锚标签两态、非常量 selector 不动、幂等、注册挂名。
- ``flattening_reverser_test``\ ：lookup/table 分发解析、xor 偏移穿透、相
  对更新（iinc / isub 带分发偏移 / 镜像 iadd）迹模拟解析与路径冲突拒收、
  镜像与混合操作数链、第二读点 / 交叠写 / 分支锚入 / 直落块头 / 第二
  switch 五路拒收、状态槽 iinc 拒收、幂等、自定义形态表行扩展面、注册挂
  名。
- ``goto_rearranger_test``\ ：尾段搬移 + GOTO 删除、GOTO_W 形、不动点链、
  外部分支锚入 GOTO 边界的重定向、多前驱 / 回边 / 异常表触段三路拒动、邻
  接与后向目标不认领、注册挂名。
