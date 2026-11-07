.. _stack-op-expander-spec:

DUP 族帧面展开规格（StackOperationExpander）
============================================

状态：定稿，已落地。微观契约（纪律门位、拒收口径、克隆语义）在 ``stack_operation_expander.hpp`` 头注；本文承载适用形态、槽宽形判别、窗口算法与前后对照，读法：先 §2 判「这条 DUP 吃不吃」，再 §3 看改写怎么落，§5 是逐形字节级实样。

1. 生态位
---------

StackOperationSimplifier 的对偶面：那边是收缩（``dup;pop``\ →\ ∅ 一类窗口折叠），本件是展开——常量 producer 上的 DUP 族复制语义按帧溯源还原，把「一条装载 + 复制」还原成多条独立装载。服务对象是后续窗口匹配：store/load 中转消除、比较折叠一类的匹配器要求每个操作数来源都是一条看得见的独立装载指令，复制形（javac 少发、混淆器常发）挡在它们面前。

邻接窗口面（simplifier 的 ``装载;dup``\ 规则族）有两处天然盲区：生产者必须与 DUP 词法紧贴（隔一条 ``iinc``\ 就不可见），且装载叶子按宽语义不含 ``ldc2_w``\ （cat2 装载只认 lconst/dconst 家族字面）。帧面把「谁喂了这个槽」从词法邻接解放为数据流事实：来源穿透栈复制（dup/swap）与 store→load，凡单一来源收敛到纯常量装载者皆在射程内。

消费形态：opt-in 管线 ``phase.stack-op-expand``\ （默认链不收，CLI ``-p``\ 显式挂点）；注册名 ``StackOperationExpander``\ （pass 名与行为绑定的回归锚）。组合顺序建议 expand → simplify：展开后简化器可把克隆面再折叠回等价形（§5.6）。

2. 适用形态：正门
-----------------

吃 DUP 全族六形，输入槽预算与复制区宽度由操作码定死；cat2 与否不作假设，由入口帧的槽宽事实判别（dup2 的两形、dup2_x2 的三形由此唯一确定，见 §3）。

放行四条门槛，全部满足才展开：

- DUP 指令可达且入口帧可得（帧分析失败的方法整方法保守不动）；
- 每个输入槽值的来源收敛为单一指令（``OriginValue``\ 为 Insn 形态——分支汇合多来源退化为未知即拒收，不同生产者装同一常量值的形态同样不收）；
- 来源是纯常量装载：int/long/float/double/string 常量、``ACONST_NULL``\ 与 ldc Class/MethodType/MethodHandle 元数据常量（与 UselessPopCleaner 帧面生产者判定同一覆盖面——那边管删、这边管克隆；condy 不认领，bootstrap 求值语义不做假设）；
- 宽度互证：来源生产者的常量类别（cat1/cat2）与帧槽宽一致（合法输入由 Verifier 保证，双查防畸形）。

按操作码分两种改写形：

- **形 A（dup / dup2）**\ ：原位消解——窗口就是 DUP 一条指令，替换为输入切片来源的克隆序列（deep→top 重发一遍输入），来源生产者保留原位。克隆是常量重发，与执行时机无关，因此生产者与 DUP 无需词法邻接：隔 ``iinc``\ 、穿 store→load 的来源都放行。
- **形 B（dup_x1 / dup_x2 / dup2_x1 / dup2_x2）**\ ：整窗重排——输出形对输入切片作了重排，只推不排无法原位表达，必须把输入生产者连同 DUP 一起吃进窗口整窗重发。额外门槛两条：整窗连续（输入切片的来源生产者恰为 DUP 之前 slice.size() 条指令、词法序与栈序一致）；窗口纪律（首条豁免，内点不得被分支/异常表标签锚定，见 §4）。

3. 拒绝面
---------

- 来源是入参（Parameter 身份）或局部装载：非常量，克隆重发不可靠（局部值可被中途改写）。
- 分支汇合来源退化 Unknown：不同路径喂不同值，展开对某条路径不可见。
- X 族输入切片含非常量槽：输出重排需要整组可重发。
- X 族窗内不连续（来源隔着 ``iinc``\ /中继指令）：窗内指令要随整窗删除，不可删成员断门。dup/dup2 的形 A 不受此门（对照：形 A 恰恰靠帧面吃隔距形态）。
- X 族窗内点被标签锚定（分支目标/异常表边界）。
- 共享来源喂多槽（如 ``iconst_1;dup;dup_x1``\ 顶两槽同源）：整窗连续性在位置核对上天然失配，拒收。
- cat2 被复制区/插入区边界劈开（VerifyError 形态）、宽度互证失配、帧分析失败、指令不可达。

4. 算法与窗口纪律
-----------------

每轮：跑 ``traceOrigins``\ 帧溯源 → 逐条可达 DUP 造展开计划 → 最长不重叠规划 → 自右向左批式落盘 → 有应用则迭代到不动点。展开只消灭 DUP 不制造 DUP，收敛有保证。

槽宽形判别以入口帧为准：从栈顶按槽预算读输入值序列（cat2 底槽行随值整体跳过），复制区预算（dup 族 1 槽、dup2 族 2 槽）必须被完整值填满——顶值 cat2 即 cat2 形，否则要求顶两值都 cat1。X 族输出形 = 复制区 ++ 插入区 ++ 复制区（各区 deep→top），例：dup_x1 输入 ``[v2,v1]``\ → 输出 ``[v1,v2,v1]``\ 。

窗口纪律与 stack_operation 套件同构，门位一一对应：

- **首条豁免**\ ：窗口首条被覆写为替换首条、保留其首边界标签语义——替换整体与原窗口栈面等价，从窗首进入的任何路径执行整段替换得到同一结果；
- **内部锚标签拒收**\ ：窗内点（含 DUP 落点）被 ``labelsAnchorAt``\ 锚定（分支目标与异常表边界两角色都算）即整窗拒收，按窗口粒度跳过、其余计划继续；
- **最长不重叠**\ ：计划起点降序、同起点长窗优先，自右向左执行，与已应用区间重叠者按窗口粒度跳过。

与 tmatcher 的分工申报：本件窗口是数据依赖窗（宽度与来源由帧给出，形状规则表达不了「隔 iinc 的来源」），不走 ``plansFor``\ 形状规则面；纪律门在同等位置重实现，语义与套件申报一致。编辑机械与 simplifier 的 ``applyPlan``\ 非空路径一致（覆写窗首、倒序擦除、链式插回克隆）。maxStack/maxLocals 不变：展开前后各程序点栈深逐一相同。

5. 前后对照
-----------

以下对照用助记形式（夹具为可验证栈形，消费尾巴只用于收口栈平衡）。

5.1 dup 隔 iinc（形 A 帧面盲区消除；pop 消费第二份克隆）：

.. code-block:: text

    before: iconst_5; iinc 0,+1; dup; pop; ireturn
    after:  iconst_5; iinc 0,+1; iconst_5; pop; ireturn

5.2 dup2 cat2 形（邻接窗口面的 ldc2_w 盲区；pop2 消费第二份克隆）：

.. code-block:: text

    before: ldc2_w K; dup2; pop2; lreturn
    after:  ldc2_w K; ldc2_w K; pop2; lreturn

5.3 dup2 双 cat1 形（两槽各克隆一条）：

.. code-block:: text

    before: iconst_1; iinc 0,+1; iconst_2; dup2; pop2; pop2; return
    after:  iconst_1; iinc 0,+1; iconst_2; iconst_1; iconst_2; pop2; pop2; return

5.4 dup_x1 整窗重排（输出形 [v1,v2,v1]）：

.. code-block:: text

    before: iconst_1; iconst_2; dup_x1; pop2; ireturn
    after:  iconst_2; iconst_1; iconst_2; pop2; ireturn

5.5 dup_x2 cat2 插入形（输出形 [v1,v2,v1]，v2 占双槽）：

.. code-block:: text

    before: ldc2_w K; iconst_3; dup_x2; pop; lstore 1; pop; return
    after:  iconst_3; ldc2_w K; iconst_3; pop; lstore 1; pop; return

5.6 组合面：展开后简化器再折叠回等价形（§5.3 夹具接一轮 simplifier）：

.. code-block:: text

    after-expander: iconst_1; iinc 0,+1; iconst_2; iconst_1; iconst_2; pop2; pop2; return
    after-simplifier: iconst_1; iinc 0,+1; pop; return

两形净栈效应同为零，等价形不等于原形（窗口折叠语义按 simplifier 规则面走）。

6. 边界与挂账
-------------

- SWAP 双常量换位（``ldc K1; ldc K2; swap``\ → 换序克隆）不在本件族内——SWAP 无「复制」语义，归属收缩面对账（simplifier 已有同族常量形规则），全类型泛化挂账。
- 共享来源 X 族的逐中继放宽挂账：UPC 帧面已把 DUP 中继别名折叠细化落地，本件的整窗连续门比它保守一档（中继在场即拒收），后续可按同一互证思路收窄。
- ``ldc``\ 元数据常量（Class/MethodType/MethodHandle）克隆重发已收；condy 保持不认领，与 UPC 口径联动。
