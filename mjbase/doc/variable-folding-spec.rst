.. _variable-folding-spec:

局部变量折叠族规格（RedundantLoadValEliminator / StoreLoadRelayEliminator / DeadLocalStoreRemover / RedundantStoreEliminater / RefCopyPropagator）
==================================================================================================================================================

状态：已落地（判据口径以各件 ``///`` 头注为权威面，本文承载家族分工、逐件判据整理与边界申报），2026-11-08。

1. 动机与家族边界
-----------------

混淆器普遍在「生产者 → 消费方」之间插入局部槽位噪声打断模式匹配：引用值先
``astore`` 暂存再 ``aload`` 读回、计算结果先 ``istore`` 中转再 ``iload``
取出、状态槽反复死写。本族五件把这些噪声按三个推理档位收敛回直接形态——

- 窗口面（相邻对，不跑数据流）：形态自身保证折叠正确，判据最窄、成本最低；
- def-use 到达定义面：负向结论（「无使用」「到不了汇点」）由到达定义与使用
  边承载，数据流失败或截断时整件跳过；
- 来源域面：aload 读点的到达来源单值性承载「重放生产者等价」的论证。

五件全部 ``scope:method`` / ``stability:opt-in``（默认链不收，CLI ``-p``
显式挂点），PreservedAnalyses 条件语义统一：发生改写返回 ``none()`` ，未
发生返回 ``all()`` ；maxStack/maxLocals 只降不升（消除与降级不改栈峰值）。
微观契约（匹配矩阵逐行、旧栈偏离逐条）在各头注（``redundant_load_val_eliminator.hpp``\ 、
``store_load_relay_eliminator.hpp``\ 、``dead_local_store_remover.hpp``\ 、
``redundant_store_eliminater.hpp``\ 、``ref_copy_propagator.hpp`` ），本文不复制。

2. 三档位与家族分工
-------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 件
     - 档位
     - 判什么
     - 处置
   * - RedundantLoadValEliminator
     - 窗口面
     - 常量装载 + 紧随 ``POP``\ /\ ``POP2`` 相邻对
     - 整对删除，迭代到不动点
   * - StoreLoadRelayEliminator
     - def-use 面
     - store 定义使用边恰为紧邻 reload（单使用证明）
     - 整对删除，值改由栈保持
   * - DeadLocalStoreRemover
     - def-use 面
     - 槽位值链不可达「有意义汇点」的 store
     - 整删（常量链）或降级 ``POP``\ /\ ``POP2``
   * - RedundantStoreEliminater
     - def-use 面
     - 单次 store 定义零使用边
     - 原位降级 ``POP``\ /\ ``POP2``，不删指令
   * - RefCopyPropagator
     - 来源域面
     - 单来源不可变引用的 ``aload`` 读点
     - 原位改写为来源指令

家族外两个邻接件与本族互补但不入本文：PopUnUsedLocalVariables（槽位面，判
「方法内零读点」的槽，不跑数据流）与 UselessPopCleaner（窗口面 + 帧溯源面，
弹值即被丢弃的死生产链）——分工口径见各自头注与 :ref:`pass-catalog`\ §8。
三件 def-use 判据的粒度差异是家族分工的核心：SLRE 判「一对」（store 与紧邻
reload）、RSE 判「一次定义」（到边粒度，槽内其他定义不受牵连）、DLSR 判
「一个槽」（值链沿全部传递闭包不可达汇点）。

3. 逐件判据
-----------

.. _variable-folding-rlve:

3.1 RedundantLoadValEliminator（窗口面）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

匹配矩阵按返回类别对齐弹宽：

- ``POP`` ：前驱是 int / long / string 常量装载（ICONST 族 / BIPUSH /
  SIPUSH / LDC Int / LCONST / LDC2_W Long / LDC String）；
- ``POP2`` ：前驱是 long / double 常量装载（LCONST_0/1、DCONST_0/1、
  LDC2_W Long/Double）。

LDC2_W/LDC_W 一律折叠为 LDC 节点、写回时按类别展开，classfile 可观测行为即
上述全集。迭代到不动点：前半段删除暴露出的新相邻对（交错形态
``[ICONST;ICONST;POP;POP]`` 第二轮清）下一轮继续消除。边界守卫：``POP``
前置边界有存活标签锚定（分支目标 / 异常表边界）时跳过该对；首成员（常量装
载）锚定标签时照常处理——fallthrough 删除把引用重定向到 ``POP`` 。

.. _variable-folding-slre:

3.2 StoreLoadRelayEliminator（def-use 单使用面）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

适配形态：混淆器在计算与消费之间插入 ``istore s; iload s`` 中转对。对是恒
栈中性的（store 弹多少 load 推回多少），删除后值留在栈上。判据 = store
定义的使用边集合恰为一条边、且该边指向紧邻的 reload——handler 读点（异常
边精确合并）、iinc 读改写、后续任何读点都会产生额外使用边 → 保守保留。
五族（i/l/f/d/a）全宽覆盖，快捷形（``istore_n``\ /\ ``iload_n`` ）同权；
删除暴露出的新相邻对下一轮继续消。边界守卫：对内任一侧锚标签整对拒收——
reload 位锚入 = 分支跳入时期望弹值在栈，删对后该路径栈形破坏；store 位锚
入涉及边界重锚，按家族「不重锚异常表边界」惯例整对保留。

相对旧栈标签段窗口载体的偏离四条（逐条申报，详见头注）：载体换 def-use 单
使用证明；旧栈窗内多对面不健全（删一对后窗内后续同槽读点观察到陈旧槽值，
``2v+2`` 变 ``2v+1`` 形态）不复刻，新栈单使用判据天然拒绝；新增上述锚标签
守卫；宽度族从仅 ILOAD/ISTORE 扩到五族全宽。

.. _variable-folding-dlsr:

3.3 DeadLocalStoreRemover（汇点可达面）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

载体 analyzeDefUse（按方法签名播种入参——参数值有定义边，参数槽的存储不会
被误判死槽）。槽位有意义 ⟺ 某次读的到达定义沿 def→use 边闭包能到达有意义
汇点：invoke 族 / 分支与 switch / putfield / putstatic / monitorenter /
monitorexit / athrow / xreturn / xastore。store 与 iinc 作为中间定义续走
（槽位间转存穿透），dup/swap 复制透明——消费边直连原定义；读点到达集合溢
出或无定义边（看不见来源）→ 保守记有意义。

处置是联合不动点、两约束迭代互约：

- 死槽 store 整删 ⟺ 其弹值的全部到达定义都是纯常量生产者（int/long/
  float/double/string 常量装载 + ``ACONST_NULL`` ）且属最终整删集；
- 常量属整删集 ⟺ 其全部使用边都指向最终整删的 store（降级为 ``POP`` 的
  store 仍在消费弹值，生产者必须保留）。

不满足整删的死 store 原位降级 ``POP``\ /\ ``POP2`` （宽度按 store 族操作
码，弹宽不变恒栈中性）；弹值定义含运算 / 装载转发 / 参数时不整删——装载是
恒等搬运、定义归 store，隔距离的装载对交装载清理件与管线下一轮窗口面收敛。

.. _variable-folding-rse:

3.4 RedundantStoreEliminater（零使用边面）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

适配形态：状态槽反复覆写——同槽连续 store 中间无读取，前一次写入在所有路径
上都被覆写（或直到方法结束都无人读）。判据 = store 定义的使用边为空：后续
store 覆盖在数据流里天然杀死前定义，异常路径经精确异常边合并，handler 读取
同样产生使用边。处置原位降级（宽度按 store 族操作码，LSTORE/DSTORE 弹双槽
→ ``POP2`` ），不删指令、不动结构。iinc 是读改写、天然产生使用边不受牵连
（旧栈的 iinc 槽整槽排除在此内化）。

相对旧栈线性序载体（store/load 下标区间推理）的修正：旧栈按位置区间判定，
控制流从区间内跳出到区间后的读取会漏判（误删活存储）；到达定义按路径追踪，
该形态正确保留——单测钉死（``jumpOutToLaterLoadKeepsBothStores`` ）。

.. _variable-folding-rcp:

3.5 RefCopyPropagator（来源域面）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

载体 traceOrigins 来源追踪（入参播种）。aload 读槽的单来源事实即「全路径
该槽当前值由同一指令产生」——来源穿透 store→load，多级暂存槽链一步折叠到
底；多来源汇合退化 Unknown → 保留，来源槽再写失效同样内化（再写后读点的
到达来源是新写者）。生产者白名单（值不可变的引用来源）：

- aload 族（改写为同族换槽 aload，自赋值同槽跳过）；
- ldc 引用常量（CpTag String/Class/MethodType——同一常量池条目引用恒等；
  int/float/double ldc 与 Handle/Dynamic 保守不认）；
- ``aconst_null`` 。

getstatic/getfield（字段可变）、方法调用、new、数组读取、dup 变体一律不认。
处置是原位改写 aload 的 op/a/cp 字段（栈效应同为 1 引用，srcPc 保留读点自
己的行号锚）；单遍扫描不做不动点（ldc/``aconst_null`` 改写后自身不构成新
的 aload 读点）。覆盖边界：来源为入参 Param 时不可克隆（其槽位可再写不可
回指）保守保留，aload→aload 即时折叠不可判的形态交 SLRE / RSE 族收敛。

4. 链上演示：一个方法的三段收敛
-------------------------------

混淆态（``ldc`` 与 ``areturn`` 之间被暂存中转打断）：

.. code-block:: text

    .method public static demo ()Ljava/lang/String; {
        code: {
            ldc "x"            // 生产者
            astore_0           // 暂存槽 0（混淆器插入的中转）
            aload_0            // 读回——打断模式匹配
            areturn
        }
    }

RefCopyPropagator 把 ``aload_0`` 原位改写为来源形态 ``ldc "x"`` （同一 CP
条目），暂存槽失去全部读点：

.. code-block:: text

            ldc "x"
            astore_0
            ldc "x"
            areturn

RedundantStoreEliminater 判 ``astore_0`` 定义零使用边，原位降级 ``pop``
（astore 弹 1 引用，恒栈中性）：

.. code-block:: text

            ldc "x"
            pop
            ldc "x"
            areturn

RedundantLoadValEliminator 吃「LDC(String) + ``POP`` 」相邻对，一轮收敛：

.. code-block:: text

            ldc "x"
            areturn

三件独立启停、各自幂等——上例任意单件运行都产出合法等价字节，链序只是收
敛速度；组合次序建议 RCP → RSE → RLVE（生产者改写暴露死存储，降级暴露死
装载对）。中转对的另一面由 SLRE 承担，形如：

.. code-block:: text

            bipush 9
            istore_0
            iload_0            // 中转对：单使用证明成立 → 整删
            ireturn

删除后 ``bipush 9; ireturn`` ——值改由栈保持，槽 0 不再被写入（写值无任何
其他观察点）。

5. 负向结论纪律与安全门
-----------------------

def-use 与来源域两档位的判据本质是负向结论（「无使用」「到不了汇点」「来
源唯一」），家族因此共享同一条纪律：帧数据流失败或到达集合溢出（truncated）
时「整件跳过」——负向结论不建立在截断集合上。窗口面（RLVE）不跑数据流，
守卫只有锚标签一条，形态自身保证折叠正确。所有拒绝面字节不动：单对 / 单定
义 / 单读点保留不影响同方法其余改写。

6. 边界申报
-----------

1. **跨行号边界**：RLVE 对跨行号边界的相邻对照常消除——行号边界在 CodeDom
   不可见（LNT 是 backing 属性），显式保守偏离（旧行为不折叠）。
2. **RLVE 不删生产者链**：只折叠「常量 + 弹弃」对；非常量生产者的死链归
   UselessPopCleaner（窗口面 + 帧溯源面）。
3. **DLSR 隔距离装载不追**：弹值定义含装载转发时不整删（装载是恒等搬运、
   定义归 store），该残留交装载清理面收敛，本件不做跨距离重写。
4. **RCP 单遍**：不做不动点；多级 aload 暂存链靠来源域穿透一步到底，不靠
   多轮迭代。Param 来源与 aload→aload 即时折叠不可判的形态显式保留。
5. 「无输入边的 store」（不可达 / 无帧事实）在 DLSR 只降级不整删——整删的
   常量链论证需要定义边证据。

7. 测试面
---------

五套件全在 ``mjbase/test/unittest/pass/``\ ，方法级经共享驱动（
``method_pass_test_util.hpp``\ ：promote → run → 管线统一 lower，对写回
字节重建会话再 promote）验证真实写回产物：

- ``redundant_load_val_eliminator_test``\ ：int/string/long/double 与
  LDC2_W 宽形态折叠、不动点嵌套清、非常量不动、分支目标锚 ``POP`` 拒收、
  注册挂名。
- ``store_load_relay_eliminator_test``\ ：五族与快捷形整删、外部读点 /
  handler 读点 / 对内任一侧锚标签三面保留、不动点收敛、幂等、注册挂名。
- ``dead_local_store_remover_test``\ ：常量链整删、只喂 ``POP`` 仍判死、
  有意义读点保留、参数槽保留、运算生产者降级、共享常量被活消费逐出、槽间
  传递链死亡、分支汇合定义整删、分析失败整件跳过、注册挂名。
- ``redundant_store_eliminater_test``\ ：覆写死写与尾部死写降级、中间读点
  保留、跳出区间到后续读取保留（旧栈漏判修正钉死）、iinc 保留、cat2 降级
  ``POP2``\ 、分析失败跳过、幂等、注册挂名。
- ``ref_copy_propagator_test``\ ：ldc / ``aconst_null`` 来源改写、多级暂
  存一步折叠、自赋值跳过、Param 来源保留、多来源汇合保留、调用生产者保
  留、来源槽再写后穿透、幂等、注册挂名。
