.. _method-inliner-spec:

方法内联规格（MethodInliner）
=============================

状态：定稿，已落地（v1 静态/私有面 #164，v2 扩面本批）。微观契约（预算常量、拼接形不变量、失败语义）在 ``method_inliner.hpp`` 头注；本文承载判定流程、逐面判据与前后 jasm 对照，读法：先 §2 判「这个站点吃不吃」，再 §4 看改写怎么落，§5 是每面的字节级实样。

.. _method-inliner-ecosystem:

1. 生态位
---------

MethodInliner 把容器内满足准入门的小方法体克隆进调用点、原位替换调用指令，消除一次跨方法跳转；服务对象是混淆器注入的静态纯函数辅助方法与转发链。opt-in：默认链不收，CLI ``-p method_inliner`` 显式挂点；注册名固定 ``method_inliner``\ （pass 名与行为绑定的回归锚，改名即破坏既有管线的位点选择）。壳方法本体保留，删除延后 GlobalUnusedMethodRemover 管线，与本 pass 无正确性顺序约束。

两条预算常量写死在头文件、不做配置面：``kMaxInlineCalleeInsns = 32``\ （被调体指令数上限，典型混淆器辅助函数/转发壳的体量上界）与 ``kMaxInlineCalleeBackEdges = 2``\ （被调体回边数上限，单循环含 continue 的浅形态；风险帽而非语义门，见 §6）。

.. _method-inliner-accept:

2. 适用情况：站点形态门
-----------------------

吃四类调用指令，逐类判据如下；所有判据只在容器内字节码上可证时放行——「运行时类路径可能补齐证据」的形态一律进 §3 拒绝面。

2.1 v1 基础面（静态与同类私有）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

- ``invokestatic``：被调 static，同类任意访问；跨类要求双方双 public。
- ``invokespecial``：仅同类 private 非 static 非 ``<init>`` 形态（super 调用与构造器不在面内，见 §7）。
- ``invokevirtual`` 声明为 private 的同类形态：JEP 181 起 javac 11+ 对 private 实例方法一律发 invokevirtual，该形态与 §2.2 的「private 声明不参与分派」证据路合并处理。

2.2 invokevirtual / invokeinterface 去虚化三路证据
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

virtual/interface 站点的分派目标必须静态唯一，三路证据全部有 JVM 强制语义背书（违反即加载期 VerifyError），故「证据成立 ⇒ 唯一目标」是全称命题而非统计近似。判定前先做声明者落解（§4.2 步骤 1），然后按序匹配：

- **声明方法 ACC_FINAL**：final 方法不可被任何子类覆盖（JVMS 4.6 方法覆盖约束 + HotSpot 加载期校验），接收者运行时类型无关。
- **符号 owner 自身 ACC_FINAL 且声明该方法**：final 类无子类（JVMS 4.1 加载期禁止 final 类被 extends），接收者运行时类型恰为该类。方法本身可以非 final——类的 final 性已收窄全部继承面。
- **声明为 ACC_PRIVATE**：private 不参与虚分派（JEP 181 nestmates 之后 private 成员的访问与分派均收窄到 nest 内，运行时无 override 通道）。覆盖三类 javac 形态：同类 private 经 invokevirtual、跨 nest private 经 invokevirtual、private 接口实例方法经 invokeinterface（§2.4）。

三路皆不中即拒绝。注意两类「看起来像证据但不收」的形态：容器内没有子类 ≠ 没有子类（类路径开放，超集不可证）；接口方法的 default/abstract 声明不参与第一路（接口方法禁 ACC_FINAL，JVMS 4.6 明文），第三路 private 声明是接口站点唯一的收编形态。

2.3 invokeinterface private 面
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

接口闭包内解析到 private 声明即收（private 不参与分派，声明者即唯一目标）。非 private 接口方法（default/abstract）拒绝：接口无 final 机制、实现类集合对容器外开放、无接收者精确类型数据流，唯一实现不可证（留账见 §7）。

2.4 nestmate 跨类 private
^^^^^^^^^^^^^^^^^^^^^^^^^

跨类 private 站点的访问合法性判据镜像 JVMS 4.7.29/5.4.4 与 HotSpot 验证口径，三步全过才放行：

1. 双方各自的 NestHost 属性目标同名（缺 NestHost 属性 = 自身为宿主，取自身名）；
2. 宿主类在容器内可装载；
3. 宿主的 NestMembers 属性逐名列出双方类名（宿主自身作为站点一方时豁免第 3 步——NestMembers 列的是成员不是宿主自己）。

任一步不成立 fail-closed：属性缺失、宿主不在容器、列名不含 claimant、宿主自身不在容器且双方名不等，全部拒绝。这条判据是「访问面」门，与被调是不是 static 无关；收编形态按调用指令分流：private static 走 invokestatic（无接收者、无守卫）、private 实例走 invokevirtual、private 接口方法走 invokeinterface。

2.5 protected 跨包
^^^^^^^^^^^^^^^^^^

protected 是非 public 跨包的唯一合法访问形态（JVM protected 规则的子类支可字节证明：调用方类沿容器内 super 链逐段上溯到声明类，链中断即拒）。包等价支（同包任意类）不收——运行时包等价依赖 defining loader 身份，字节码不可证，同包 package-private 跨类同此留账。protected 实例站点与 public 实例站点同走 §2.2 去虚化（protected 声明若非 final/private 仍可被覆盖，final 收窄后收）。

2.6 switch / 浅循环被调体
^^^^^^^^^^^^^^^^^^^^^^^^^

被调体含 ``tableswitch``/``lookupswitch``：多目标随体迁移（§4.2 步骤 6），目标落方法末边界仍拒（v1 门：克隆体的出口语义无法承载落在方法边界上的目标）。被调体含回边（goto/分支目标 ≤ 当前位置）：随体迁移，回边计数（switch 目标回边同帽计入）超 ``kMaxInlineCalleeBackEdges`` 拒收。循环纯度、预算、形状门与直线体同口径。

2.7 接收者 null 守卫
^^^^^^^^^^^^^^^^^^^^

所有实例被调（无论 v1 同类 special 还是 v2 去虚化面）拼接显式 null 守卫。依据：invoke 家族在调用点做接收者 null 检查（JVMS 6.5 invoke 家族条目），而克隆体白名单指令永不解引用接收者（跨类体禁成员引用；同类体经纯度门排除字段访问）——不补守卫则 null 接收者路径的 NPE 消失，行为不等价。守卫形见 §5 各例：``aload`` fresh 槽 → ``ifnonnull`` 越过 → ``aconst_null`` → ``athrow``。异常类型/抛出点/处理器路由与原调用逐位一致（调用点入边重锚后守卫整体落在与原调用相同的异常表覆盖关系内）；唯一弱漂移是 helpful-NPE 消息文本（裸 ``athrow`` null 无消息），该路径仅存在于接收者为 null 的错误程序，正确程序不可观测。

.. _method-inliner-reject:

3. 拒绝面清单（逐桶 fail-closed）
---------------------------------

任一桶命中 → 该调用点原样保留（字节逐位不变）；候选捕获/克隆/写回中途失败 → 整类放弃不写回（容器原字节保留）。逐桶与理由：

- **预算**：被调体指令数 > 32（RejectsCalleeOverBudget）。预算外站点保留是产品语义（混入大方法会膨胀调用方体量），不是安全限制。
- **异常表**：被调带异常处理器表（RejectsCalleeWithExceptionTable）。克隆后 try 区间映射需要区间代数，v1/v2 未建模。
- **synchronized**：被调 ACC_SYNCHRONIZED 或含 monitorenter/monitorexit（RejectsSynchronizedCallee）。监视器进入/退出点随克隆迁移后与异常表交互未建模。
- **jsr/ret**：旧版子例程形态，现代类版本不产（≥50 门已挡大半），捕获层直接拒。
- **直接递归**：被调体自调（RejectsSelfRecursiveCallee）；**间接递归**：调用边可达回被调自身（RejectsIndirectRecursion，MethodEffectModel 调用边不动点传播）。内联展开无穷递归无不动点。
- **纯度**：体含字段写（RejectsImpureCalleeFieldWrite）、字段读（RejectsReadOnlyCalleeFieldRead——跨类后解析上下文变调用方）、显式抛出、分配、indy、虚调用（RejectsVirtualCallInBody）、native/抽象声明。判据 = MethodEffectModel Pure 含调用边不动点。
- **去虚化证据缺失**：virtual/interface 站点三路证据皆不中（RejectsVirtualOverridableCallee）；或证据成立但克隆体含成员/类型引用指令（RejectsVirtualFinalMethodCrossClassBodyWithCall——跨类体白名单，改挂调用方访问上下文不安全）。
- **接口非 private 站点**：invokeinterface 解析到 default/abstract（RejectsInterfaceNonPrivateSite）。
- **访问面不可证**：跨类 private 且 nest 属性链断裂（RejectsNestmateMissingAttrs / RejectsNestmateStaleMembership）；protected 且调用方非子类（RejectsProtectedWithoutSubclass）；同包 package-private 跨类（loader 身份不可证）。
- **类初始化**：跨类 static 被调声明类带 ``<clinit>``（RejectsCrossClassStaticOwnerWithClinit）。invokestatic 触发声明类初始化（JVMS 5.5），内联后触发点消失；同类站点不拒（同触发面）。实例调用不触发初始化（JVMS 5.5 无此 bullet；接收者存在性已背书声明类已初始化、null 路径 NPE 先于初始化），故跨类实例面无此门。已知保守缺口：容器可解析超类链的 ``<clinit>`` 未查（头注声明）。
- **形状**：被调体自身帧不可算（栈形失衡/局部越界/BestEffort 预检失败）、分支/switch 目标落方法末边界（RejectsBranchToMethodEnd / RejectsSwitchTargetToMethodEnd）、回边超帽（RejectsDeepLoopCallee）、循环体不纯（RejectsImpureLoopCallee）。
- **调用点形状**：调用点是方法末指令（无顺序后继，统一出口无处锚）；调用点所在方法无 code（abstract/native）。
- **版本**：类版本 < 50（Strict SMT 语义前提）。
- **槽位**：落槽后 maxLocals 越 u16 界（kMaxLocalsLimit）。

.. _method-inliner-algo:

4. 算法
-------

两阶段：先全容器收集候选（纯捕获，不改容器），再逐类逐站点改写。候选表键 = (声明类内名, 方法名, 描述符)。

4.1 阶段一：候选捕获
^^^^^^^^^^^^^^^^^^^^^

对容器每个可解析类每个有码方法，按序做捕获前门，再逐指令快照：

1. **声明门**：非 abstract/native；访问位快照（public/private/protected/static/final）；owner final 性、owner ``<clinit>`` 有无快照。
2. **纯度门**：MethodEffectModel Pure（含调用边不动点传播）——体及其可达被调无字段访问、无分配、无显式抛出、无 indy、无虚调用。静态纯函数是本 pass 的服务对象，此门是等价性的根。
3. **形状预检**\ （BestEffort 帧数据流）：栈形失衡/局部越界/不可算形状在此拒收，调用点不受牵连（不升级为 caller 整类放弃）。
4. **预算门**：指令数 ≤ 32。
5. **逐指令快照**\ （op/wide/a/b + 载荷分类）：常量族 intern 原值快照；成员/类型引用记语义目标；local 族记原始槽位（quick 形归一：``iload_1`` → 槽 1）与 cat2 标志；分支/switch 记「指令序位」目标（posOf 表），同时计回边（目标序位 ≤ 当前序位即回边，超帽拒）；switch 记 keys（lookupswitch）与逐 case 目标序位；``LDC`` 载荷 MethodType/MethodHandle/Dynamic 拒收（NotModeled 口径）。
6. **localsNeeded**：``max(参数槽数, max(原槽位)+1)``——调用方需要的 fresh 槽总量（cat2 占双槽已计）。

4.2 阶段二：站点改写
^^^^^^^^^^^^^^^^^^^^^

对每个类每个方法按序快照扫描（句柄跨拼接稳定：槽位永不回收，位置现查），对每个 invoke 族站点依次过门、过门者拼接：

1. **声明者落解**\ （仅 virtual/interface）：从符号 owner 起沿容器内 super 链上溯（含自身），首个声明 (name, desc) 匹配方法的类即声明者。类链上遇容器外（悬挂）类：截断——悬挂边界之内的近者声明胜（JVMS 5.4.5 最近优先 + 链外不可见口径）；接口闭包遇悬挂：fail-closed 拒（most-specific 歧义判定需要完整闭包）。落解不出声明者（类链无声明/接口闭包无声明）拒绝。
2. **去虚化判定**：§2.2 三路证据按序匹配（声明方法 final → owner final 且声明 → 声明 private），命中即得目标声明者键；候选表按声明者键查（不是符号 owner 键——符号 owner 可能只是桥接转发者）。
3. **分派形门**：invokestatic ↔ static 被调；invokespecial ↔ private 非 static 被调；invokevirtual ↔ 非 static 被调；invokeinterface ↔ 非 static 且 private 被调。
4. **访问面门**\ （跨声明类站点）：双 public（v1 面）｜private + nestmate 三步判据（§2.4）｜protected + 子类链（§2.5）｜其余拒。同类站点免门（访问合法性由类自身保证）。
5. **初始化触发门**：跨类 static 被调且声明类带 ``<clinit>`` 拒（§3）。
6. **拼接**\ （调用点前插，全部走 dom 受控 API）：
   a. 参数落槽：按运行栈逆序弹出、正序落 fresh 槽（cat2 按描述符弹宽）；实例被调再落接收者（槽位布局：接收者 = base+0 起，与被调槽 0 对齐；参数依次 base+1 起，与被调槽 1..k 对齐——克隆体槽映射因此是恒等平移 k → base+k）。
   b. null 守卫（实例被调）：``aload 接收者槽`` → ``ifnonnull`` → ``aconst_null`` → ``athrow``，ifnonnull 的越过目标延后绑定。
   c. 克隆体逐指令物化：槽位平移 ``k → base+k``；quick 形归一泛化（``iload_1`` → ``iload``）；CP 载荷跨会话按语义 re-intern（常量值、成员引用、类引用）；return 族改写 ``goto`` 统一出口；分支/switch 目标延后补锚（指向克隆体内对应指令的新标签），switch 的 keys 原样随迁、逐 case 目标各建标签。
   d. 统一出口 = 调用点后继锚标签（原 invoke 压返回值形态逐点相同：克隆出口 GOTO 携返回值落此处）。
   e. 调用点入边标签重锚到首条插入指令（原调用点的控制流入全部改道进拼接序言）；摘除调用指令。
   f. ``maxLocals`` 采纳 ``base + localsNeeded``；wide 升位按槽号 > 0xFF 自动。
7. **写回链**：``prepareNestedAttrsForLayout``（nest 属性适配 lower 后布局）→ ``lower`` → Strict 帧重算 + maxes 采纳 + SMT 再生（frame_rewire 链）。Strict 不可算 → 异常 → 整类放弃。

4.3 失败语义与幂等
^^^^^^^^^^^^^^^^^^

三层失败半径：站点级（门不中 → 站点保留，其余站点继续）、方法级（克隆中途失败 → 返回放弃标记）、类级（帧重算/序列化失败 → 异常上抛 → 整类不写回，容器原字节保留）。改写粒度 = 方法（有改写的方法整方法重生成帧与栈图）。无站点命中返回 all()（PreservedAnalyses），有改写返回 none()；幂等：调用点已删除，二跑严格 no-op。

.. _method-inliner-jasm:

5. 前后 jasm 对照
-----------------

素材取自单测夹具（``method_inliner_test.cpp``），每个面一组；标注对应测试名。jasm 为示意写法（槽位用编号，标签字母序），与夹具断言的指令序逐条对应。

5.1 final 方法去虚化（同类）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``InlinesVirtualFinalMethodSameClass``。``F.fin`` 声明 ACC_FINAL → 第一路证据。

.. code:: text

   # before —— F.run(I)I（fin: iload_1; iconst_2; imul; ireturn，final）
   .method public run (I)I {
       aload_0
       iload_1
       invokevirtual F.fin (I)I
       ireturn
   }

   # after —— 拼接序言 + 克隆体 + 统一出口（base = 2）
   .method public run (I)I {
       aload_0
       iload_1
       istore 3          # 参数 x 落槽（被调槽 1 → 3）
       astore 2          # 接收者落槽（被调槽 0 → 2）
       aload 2           # null 守卫
       ifnonnull A
       aconst_null
       athrow
   A:
       iload 3           # 克隆体（槽平移恒等：k → 2+k）
       iconst_2
       imul
       goto B            # 原 ireturn → 统一出口
   B:
       ireturn
   }

5.2 final 类去虚化（跨类）
^^^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``InlinesVirtualMethodOfFinalClassCrossClass``。``FC`` 类级 ACC_FINAL、方法非 final → 第二路证据。调用方 static，接收者是任意实参——守卫必须保留。

.. code:: text

   # before —— CU.use(LFC;I)I（FC: .class public final；v(I)I = iload_1; iconst_3; iadd; ireturn）
   aload_1
   iload_2
   invokevirtual FC.v (I)I
   ireturn

   # after（base = 3）
   aload_1
   iload_2
   istore 4          # 参数 → 4
   astore 3          # 接收者 → 3
   aload 3
   ifnonnull A
   aconst_null
   athrow
   A:
   iload 4
   iconst_3
   iadd
   goto B
   B:
   ireturn

5.3 private 经 invokevirtual（同类）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``InlinesVirtualPrivateSameClass``。javac 11+ 同类 private 实例调用形态 → 第三路证据。拼接形与 §5.1 全同（base = 2，槽 2/3，守卫在位）。

5.4 nestmate 跨类 private static
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``InlinesNestmatePrivateStatic``。属性链判据是本面的访问门；static 被调无守卫。

.. code:: text

   # 夹具类形（三件套）
   .class NH { .attribute NestMembers { NA, NB } }        # 宿主：仅承载属性
   .class NA extends java/lang/Object {
       .attribute NestHost { NH }
       .method private static priv (I)I { iload_0; iconst_2; imul; ireturn }
   }
   .class NB extends java/lang/Object {
       .attribute NestHost { NH }
   }

   # before —— NB.m(I)I（跨 nest private static 调用形态）
   iload_0
   invokestatic NA.priv (I)I
   ireturn

   # after（base = 1：m 声明 maxLocals=1）
   iload_0
   istore 1          # 参数 → 1
   iload 1
   iconst_2
   imul
   goto A
   A:
   ireturn

5.5 nestmate 跨类 private 实例
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``InlinesNestmatePrivateInstance``。接收者 = 任意实参（``aload_1``），守卫不可省。

.. code:: text

   # before —— NB.m(LNA;I)I
   aload_1
   iload_2
   invokevirtual NA.priv (I)I
   ireturn

   # after（base = 3）
   aload_1
   iload_2
   istore 4          # 参数 → 4
   astore 3          # 接收者 → 3
   aload 3
   ifnonnull A
   aconst_null
   athrow
   A:
   iload 4
   iconst_2
   imul
   goto B
   B:
   ireturn

5.6 protected 跨包（子类链）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``InlinesProtectedStaticViaSubclass``。``S extends D``，``D.k`` protected static。子类链可证 → 访问门放行；static 零参 → 无落槽无守卫。

.. code:: text

   # before —— S.m()I
   invokestatic D.k ()I
   ireturn

   # after
   iconst_1          # D.k 体：iconst_1; ireturn
   goto A
   A:
   ireturn

5.7 private 接口方法经 invokeinterface
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``InlinesPrivateInterfaceMethod``。``PI.p`` private（接口自身声明）、``PI.use`` 同接口 default 方法调用之——声明者 = 调用方，免访问面门。

.. code:: text

   # before —— PI.use(I)I（invokeinterface count = 接收者 + 参数 = 2）
   aload_0
   iload_1
   invokeinterface PI.p (I)I 2
   ireturn

   # after（base = 2）
   aload_0
   iload_1
   istore 3
   astore 2
   aload 2
   ifnonnull A
   aconst_null
   athrow
   A:
   iload 3
   iconst_2
   imul
   goto B
   B:
   ireturn

5.8 浅循环被调体
^^^^^^^^^^^^^^^^

测试 ``InlinesLoopCalleeBody``。回边随体迁移：克隆循环头获新标签，回边 GOTO 重锚；回边数 1 ≤ 帽 2。

.. code:: text

   # before —— LP.f(I)I（被调：s=0; i=0; while (i<n) { s+=i; i++ } return s）
   .method public static f (I)I {
       iconst_0
       istore_1          # s
       iconst_0
       istore_2          # i
   A:  # 循环头（回边目标）
       iload_2
       iload_0
       if_icmpge B
       iload_1
       iload_2
       iadd
       istore_1
       iinc 2, 1
       goto A            # 回边
   B:
       iload_1
       ireturn
   }

   # before —— LP.m()I 调用点
   iconst_3
   invokestatic LP.f (I)I
   ireturn

   # after（base = 0：m 声明 maxLocals=0，fresh 槽从 0 起）
   .method public m ()I {
       iconst_3
       istore 0          # 参数 n → 0
       iconst_0
       istore 1          # s → 1
       iconst_0
       istore 2          # i → 2
   C:  # 克隆循环头（回边目标随迁至此）
       iload 2
       iload 0
       if_icmpge D       # 前向出口随体迁移
       iload 1
       iload 2
       iadd
       istore 1
       iinc 2, 1
       goto C            # 回边 → 克隆头
   D:
       iload 1
       goto E            # 原 ireturn → 统一出口
   E:
       ireturn
   }

5.9 switch 被调体
^^^^^^^^^^^^^^^^^

测试 ``InlinesSwitchCalleeBody``。tableswitch 三目标逐目标建标签；原被调 ireturn 改写的出口 GOTO 与 case 体 GOTO 同锚汇合，default 体直落。

.. code:: text

   # before —— SW2.f(I)I（被调）
   iload_0
   tableswitch { 1 → A, 2 → B, default → C }
   A:
   iconst_1
   goto D
   B:
   iconst_2
   goto D
   C:
   iconst_3          # default 直落
   D:
   ireturn

   # before —— SW2.m()I 调用点：iconst_2; invokestatic SW2.f; ireturn

   # after（base = 0）
   iconst_2
   istore 0          # 参数 → 0
   iload 0
   tableswitch { 1 → A, 2 → B, default → C }   # keys 随迁（low = 1）
   A:
   iconst_1
   goto D
   B:
   iconst_2
   goto D
   C:
   iconst_3
   D:
   goto E            # 原 ireturn → 统一出口（case 体 GOTO 与之同锚）
   E:
   ireturn

5.10 拒绝面字节保持（反证示例）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

测试 ``RejectsNestmateStaleMembership``：双方 NestHost 同名、宿主在容器，但宿主 NestMembers 不含 claimant——判据第 3 步不成立，站点原样保留，容器字节逐位不变。全部拒绝桶同口径断言（§3 括注测试名）。

.. _method-inliner-budget:

6. 预算与验证口径
-----------------

- **指令预算 32**：混淆器辅助函数/转发壳（参数装载 + 少量纯计算 + 返回）的体量上界；写死不做配置面，放宽须先过 JVM 真跑门（防「预算调大后帧重算面吃不住」的静默劣化）。
- **回边帽 2**：单循环（含 continue 的第二回边）或一层嵌套的浅形态；更深的循环巢拒收。这是风险帽不是语义门——克隆体对参数槽幂等（一次落定、循环内不重写参数槽是纯度门的推论），循环执行次数语义与原调用逐位一致。
- **验证口径（测试侧）**：单测「改写 → 重读会话 → 指令序列事实」roundtrip 断言每面拼接形（含槽位号逐条钉死）；拒绝面以字节逐位保持反证。JVM 硬验收（``method_inliner_jvm_test.cpp``）：javac 真样本 → pass 改写 → 产物 ``java -Xverify:all`` 真跑，stdout 与退出码逐位等价，另断言壳类/超预算类字节逐位保持与二跑幂等——「改写类过 verifier + 行为等价」的直接证据。javac 形态口径以 JDK 17 探针实测钉死（private static → invokestatic、private 实例 → invokevirtual、private 接口方法 → invokeinterface），断言不随 JDK 小版本漂移。

.. _method-inliner-nongoal:

7. 不做清单（留账）
-------------------

- invokeinterface 非 private 面（default/abstract）：唯一实现不可证，除非引入接收者精确类型数据流（new 后直调等局部形态），留后续批评估。
- 同包 package-private 跨类与 protected 的包等价支：运行时包等价依赖 defining loader 身份，字节不可证。
- invokespecial super 调用与 ``<init>``：super 链跨容器解析与接收者形态门另行取证后再议。
- 被调 LNT/LVT 调试元数据不随迁（旧栈 SKIP_DEBUG 同口径）。
- 内联体中的调用不递归展开（多级链靠管线重复运行收敛）；被调体含虚调用/indy 仍整体拒收。
- 类初始化触发时机只挡跨类 static 声明类自身 ``<clinit>`` 一层；容器可解析超类链的 ``<clinit>`` 未查，为已知保守缺口。
