.. _try-catch-cleanup-spec:

异常表收口规格（TryCatchCleanup）
=================================

状态：已落地（判据口径以 ``try_catch_cleanup.hpp`` 头注为权威面，本文承载规则面逐槽整理、旧栈偏差申报与验收面），2026-11-08。

1. 动机与单职责
---------------

异常流混淆在异常表里留下三类可静态证明冗余的条目：透明重抛（捕获后立即原
样抛出，与不捕获逐位等价）、恒不抛（保护区内没有可执行且可能抛异常的指
令）、以及「必然抛出 + handler 立即吞掉」的改写形态（NPE 形、自捕抛形）。
本件把旧栈五件（RedundantTryCatchRemover / UselessTryCatchRemover /
InvalidTryCatchRemover / SelfCaughtThrowRewriter / TryCatchRepair）中语
义成立的面收拢为单 pass：一个修复面加四条改写规则，全部只动异常表与个别
指令改写，fail-closed——任一不确定整条保留。

「单职责红线」：乱序条目保留（合法类文件允许 handler 在地址序上先于保护
区）；throws 子句（Exceptions 属性）修剪不在本件（属性级元数据，与异常表
派发无关）；保护区指令面与 handler 体不做删除（透明重抛只删条目，handler
体成为不可达死块交死代码清理面）。标签：``scope:method`` /
``action:rewrite`` / ``stability:opt-in`` （默认链不收，CLI ``-p`` 显式
挂点）；发生删除/改写返回 ``none()``\ ，否则 ``all()`` 。

2. 规则面逐槽
-------------

.. _try-catch-cleanup-repair:

2.1 修复面
^^^^^^^^^^

start/handler 标签无法锚定（悬空）或保护区间为空的条目删除——结构上无法
参与合法派发的垃圾条目，不涉语义论证。

.. _try-catch-cleanup-rethrow:

2.2 透明重抛删除
^^^^^^^^^^^^^^^^

handler 体只把捕获异常原样重抛，认三种形状：

- 直接 ``athrow`` ；
- ``astore X; aload X; athrow`` 同槽恒等中转；
- ``invokestatic M; athrow`` 且 M 为当前类恒等静态包装——签名
  ``(LX;)LX;``\ ，方法体恰为 ``aload 0; areturn``\ ，无异常表、非
  synchronized。

改写前提（捕获后立即重抛 ≡ 不捕获）要求四条守卫同时成立：handler 不被其
他条目共享；handler 及其体在正常控制流下不可达（无来自可达代码的普通流
入）；无其他条目与保护区重叠或覆盖 handler 体。守卫不全即保留——旧栈
InvalidTryCatchRemover 的「handler 首指令 athrow 即删条目」无守卫版本不
复制，该面由本规则的完整守卫承担。

.. _try-catch-cleanup-impossible:

2.3 恒不抛收口
^^^^^^^^^^^^^^

保护区间内不存在任何「域分析可达且可能抛出异常」的指令时删除条目。可达性
取自域引擎到达帧（帧不可达 = 不可执行）；可能抛出集合对齐旧栈保守口径：
调用 / 分配 / 类型解析 / 字段与数组访问 / 监视器 / 除法，含普通局部 store
区间的整段保守计入。域分析（可达性 + 栈深）失败时该规则放弃、保守保留。

.. _try-catch-cleanup-npe:

2.4 NPE 形改写
^^^^^^^^^^^^^^

「必然抛出 + handler 立即吞掉跳续点 ≡ 直跳续点」。逐条判据：

- 保护区恰为 ``aconst_null; athrow`` 两条指令，无其他标签锚入保护区；
- catch 类型为 any 或 NPE；
- handler 形如 ``pop`` 后接续点——下一指令的锚定标签或下一指令 GOTO 的目
  标；
- 保护区栈深为零（入口有栈积压时直跳会把积压值留在栈上，产出 Verifier 非
  法形态——旧栈无栈深检查的缺失面，本件补入）；
- 无其他条目重叠。

成立时 ``aconst_null`` 原位改写为 ``goto 续点``\ 、``athrow`` 删除、条目
删除。

.. _try-catch-cleanup-self-caught:

2.5 自捕抛改写
^^^^^^^^^^^^^^

「构造异常立即抛、被自己的 catch 捕获 ≡ 直跳 handler」。逐条判据：

- 保护区恰为 ``new E; dup; invokespecial E.<init>()V; athrow`` 四条指令；
- catch 类型为 any 或 E；
- athrow 处栈深恰为 1（构造完成的 E 留在栈顶，恰为异常派发进入 handler
  的栈形状——直跳后 handler 的栈形与真实派发逐位一致）；
- athrow 无其他标签锚入；
- handler 在区外，无其他条目重叠。

成立时 ``athrow`` 原位改写为 ``goto handler``\ ，条目删除。

3. 与旧栈偏差申报
-----------------

1. 旧栈五件收拢为单 pass 一个修复面 + 四条规则；无守卫的
   InvalidTryCatchRemover 面不复制（handler 可被正常流到达或被共享时删条
   目会改行为），由带完整守卫的透明重抛规则承担。
2. 旧栈 TryCatchRepair 按「handler 地址序必须后于保护区」删除乱序条目不复
   制——合法类文件的 handler 允许在地址序上先于保护区。
3. NPE 形与自捕抛改写补入旧栈缺失的锚入标签守卫与栈深门槛（旧栈无栈深检
   查的 NPE 形在保护区入口有栈积压时会产出 Verifier 非法跳转）。
4. throws 子句修剪面不在本件。

4. 与 DuplicateCatchMerging 的界
--------------------------------

DuplicateCatchMerging（同族正交件）只缩表项数：表序相邻 + 同 handler 锚点
+ 同 catch 类型面 + 区间无间隙的两条目并为一条（保护区取并集），零指令改
写、handler 入口 pc 与 StackMapTable 帧表不动；不越本件的界（不做不可抛删
除、不做改写）。本件只收「语义冗余」面（删除 / 改写），不做表项合并——两
件可同管线串联、互不依赖，判据细节见 ``duplicate_catch_merging.hpp`` 头注
与 :ref:`pass-catalog`\ §9。

5. jasm 前后对照
----------------

NPE 形（单测 ``npeShapeRewritten`` 的夹具形状）：

.. code-block:: text

    // 混淆态：aconst_null 必抛，catch NPE 的 handler 只 pop 后跳续点
        aconst_null            // 保护区
        athrow
        pop                    // handler 入口
        goto L_end             // 续点
        ...
    L_end: ...
    异常表：[aconst_null, athrow) → catch NPE → handler@pop

    // 收口态：aconst_null 原位改写 goto 续点，athrow 删除，条目删除
        goto L_end             // 栈深 0 门成立，直跳等价
        pop                    // handler 体成为死块，交死代码清理面
        goto L_end
        ...

透明重抛（同槽恒等中转形状，``transparentAstoreAloadRemoved`` 夹具）：

.. code-block:: text

    // 混淆态：handler 只把异常原样重抛
        <保护区指令>
        astore_0               // handler 入口：捕获
        aload_0
        athrow                 // 原样重抛
    异常表：[区) → catch X → handler@astore_0

    // 收口态：条目删除（四守卫成立：不共享、正常流不可达、无重叠覆盖）
        <保护区指令>           // 异常沿真实调用栈上抛，与不捕获逐位等价

6. 边界申报
-----------

1. **乱序条目保留**：合法类文件允许 handler 地址序先于保护区，本件不做
   「乱序即删」的修复。
2. **恒不抛集合保守**：普通局部 store 区间整段保守计入可能抛出面（旧栈口
   径），宁可漏收口不可错删条目。
3. **域分析失败按规则放弃**：可达性 / 栈深分析失败、JSR/RET 结构、描述符
   或标签解析不齐——对应规则放弃、该条目保守保留，方法内其余条目不受牵
   连。
4. **handler 体不删**：透明重抛 / NPE 形收口后 handler 体成为不可达死块，
   清理交死代码面批次，本件不做指令面删除。
5. **改写形状封闭**：NPE 形与自捕抛形只认上列逐指令形状，混入任何额外指
   令（哪怕语义等价的变体）整组不认——形状门是本件与死代码面的分工边界。

7. 测试面
---------

``mjbase/test/unittest/pass/try_catch_cleanup_test``\ ，方法级经共享驱动
验证真实写回产物：透明重抛三形状删除（直接 athrow / 同槽 astore-aload /
恒等静态包装）与异 owner 包装保留、共享 handler 与正常流可达 handler 两
守卫拒收、恒不抛收口与含抛指令保留、NPE 形改写与栈积压 / 外部锚入两守卫
拒收、自捕抛改写与深栈拒收、空区间修复面删除、幂等、注册挂名。
