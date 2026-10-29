# mjbase 开发计划

迁移状态总览见 [doc_ai/module-map.md](doc_ai/module-map.md)；本文件只记
当前批次的待办项。

## opt pass 完整集波次（mjbase-pass-wave2）

私有侧审计报告（A0–A4 × B1–B5 覆盖矩阵 + 混淆器 trim 对标 + P0×5/P1×7/P2×4
缺口）。本波次逐项落地：

- [x] P0-1/P0-2 keep 规则 DSL + 保守预设模式库（`reference/keep_rule`：
  行式 class/member/preset、glob 语义、八预设穷举单入口 keepPresetFrom、
  「只加不减」不变量）+ keep 配置→可达性根翻译面与 CLI 暴露
  （rootClasses/rootMethods/rootFields 三槽；生命期契约：根视图一律指向
  ReferenceIndex 池内声明文本——悬垂 std::string 曾致 UnusedClassRemover
  的 keep 类根静默丢失）+ 四个删除器接入（Empty/UnusedClass/UnusedField/
  GlobalUnusedMethod）。
- [x] P0-3 可配置无用调用删除（RemoveMethodCallsHandler 等价物）：
  scope:global，CLI 白名单供调用点，删除后同步调用图边；研究已派。
  → 已落地（#29，MethodCallRemover）：按名单点名删除成员调用点 + 无用
  结果白名单面；默认无配置完全 no-op（opt-in），配置为程序化注入
  （MethodCallRemovalConfig）；CLI/配置解析供给面属 cli 批挂账。
- [x] P0-4 非私有成员删除 + 调用修复：UnusedFieldRemover / GlobalUnusedMethodRemover
  按 `UnusedMemberAccessTier` 三档放开（PrivateOnly 旧策略 / NonPublic 默认删
  package-private/protected、public 面 fail-closed 保守 / All 闭程序激进）；
  无条件保留面（keep metadata、keep 规则命中、资源 keep 类与 keep 规则类的全部
  声明成员、构造器、native/abstract）提升为闭包根，堵「保留但不可达代码引用被
  删成员」的悬垂引用；裁定「可达性判死 ⇒ 保留代码无引用 ⇒ 删除后 invoke 指令面
  无需清扫」（删除族与 MethodCallRemover 无正确性顺序约束），调用图边经
  PreservedAnalyses 失效链重建。
- [x] P0-5 实例化类保守根：RTA 式按 `new` 点位收窄可达类集合，喂
  UnusedClassRemover 的 rootClasses；与现有 ResolvedCallGraph 的关系待定。
  → 已落地（#26）：ResolvedCallGraph 增 RTA 收窄面（实例化点位收窄可达
  类集合）+ keep_roots 根翻译接线（rootClasses 供给 UnusedClassRemover）；
  resolved_call_graph_rta_test / keep_roots_rta_test + RTA JVM 真跑门收口。
- [x] P0-6 多版本 JAR（Multi-Release）：META-INF/versions/N/ 条目版本分组，
  删除/重命名不得跨版本误伤。研究已派。
  → 已落地：容器层族视图（resource_container 的
  multiReleaseVersionOfClassUri / internalNameFromClassUri /
  buildClassFamilyIndex，缺 base 族并存告警）；ClassRenamer 族契约
  （排序 (内名,URI) 全序、同族共享同一新名只计一个编号、写回保持版本化
  URI 形状——修复「族坍缩成单条目」破坏点）；内名选择器确定序（base 优先、
  同级 URI 字节序，workspace resolveClassUri）+ replaceClassBytes 单条 URI
  契约；MANIFEST Multi-Release 主属性守护（ensureMultiReleaseManifest，
  exportToJar 接线，字节级保序）；删除族契约（类级同进退 / 成员级逐份一致
  结算）与同内名重复声明 first-seen 结算以测试锁死；JVM 门真跑
  --release 11 三源根 × 多运行时矩阵（11/17/21/25）。明确不做：base 缺失时
  最小 N 变体升格 base 的布局重构（并存 + 告警，不改写 URI 布局）。
- [x] P1 强度削减与常量折叠已落地并注册种子：StrengthReducer（2^k 乘/除 →
  移位/移位-加法序列，序列峰值 maxStack 按宽度抬升 int +1 / long +2，
  maxStack ≥ 0xFFFD 预检不改写）、ConstantFolder（栈常量链 30 条指令，
  除零与 int/long MIN÷-1 红线保守不折）；去虚拟化已落地（#31，
  DevirtualizeSingleTarget：CHA 证明唯一活目标时 invokevirtual/
  invokeinterface 改写 invokespecial，JVMS 合法性依据与放弃清单见
  pass 头注，stability:opt-in）；余下 P1（GVN/CSE、同步消除、
  StringBuilder 链化简）待批。
- [x] B 批 meta_info_remover 家族迁移（scope:class）：类级 13 件已收口 +
  两件挂账收口（record 组件注解清空 / 参数注解槽内过滤）。
- [x] 字段数组标量化第一切片（#39，StaticArrayFieldScalarizer，新增件
  ——mbase 无对应物）：private static 原始类型数组字段，分配点唯一
  （iconst_LEN; newarray; putstatic）+ 全访问常量下标窗口（读
  getstatic/iconst/xALOAD 三连、写 getstatic/iconst/单条常量/xASTORE
  四连）且无整体引用时，拆成按访问下标的 `<old>_<i>` 标量字段——
  分配与下标计算指令消失，读窗口三条缩一条。守卫：nest 属性整 pass
  放弃（private 跨类不闭包）；无分配有访问放弃（null 数组 NPE 语义）；
  多分配/裸重赋/越界下标/名冲突/标签锚定逐字段放弃。改写判定两阶段
  （判定零改写、失败面全排除后才动 CodeDom），落盘序 lower 全部改动
  方法成功才 addField/removeField（A3/A4 通道，class_structure_editor
  同口径）。与 StaticFieldDefaultInliner 串联可消化 clinit 常量表
  （拆成单写常量标量后接 CV 内联）。实例字段与引用数组挂账后续切片。
  12 单测（roundtrip 语义断言：writeClass 重读后指令序列/字段面），
  全量 1617 仅 2 例并行会话 constant_folder 本地红态（非本件面）。
- [x] 恒等包装不动点内联首件（#37 前置，IdentityWrapperInliner，新增件
  ——mbase method_inliner 的 wrapper 特化上收）：识别纯转发壳（静态直通
  / 虚转发两形态，JDK 装拆箱为公共目标特例）并把 invokestatic 调用点
  一步改写到路径压缩后的最终目标；壳→壳链压缩、环与死链整族拒收。
  调用点单指令改写（不克隆壳体），栈形逐点不变、形态 B 接收者
  P0 <: T 由壳体自身验证保证；synchronized 壳与异常表非空拒收。
  访问门 public/同类 private/同包包私有（protected 跨包子类不认），
  容器外目标只认 JDK 八 box 类白名单。壳体保留，删除延后
  GlobalUnusedMethodRemover。自包含两阶段不消费注册分析；
  15 容器级单测，全量 1631 绿（3 既有跳过）。
- [x] 异常跳板消解首件（反扁平化层助，ExceptionTrampolineInliner，新增件——异常流混淆三层形态（跳板/不透明算术/扁平化）的首层）：「aconst_null; athrow → 紧邻 catch-any handler → pop」强制跳转整组还原直落——摘表项 + eraseBatch 删三条（kRetargetToFallthrough，入口分支重定向 handler 体首指令）。栈等价门用帧引擎 BestEffort 证跳板入口到达帧栈深 0（athrow 清栈语义由直落替代后不可残留）；catch 非 any / 保护区非恰两条 / handler 非紧贴 / handler 头非 POP / 表项交叠或共享 handler / athrow 与 pop 为分支目标 / 批内指令锚定他源 Boundary 标签逐面拒收。摘表项后显式销毁三枚边界标签再批删（dom 层 Boundary 保护），批删被拒以 createLabel 重建标签原位回插（锚点不变语义等价）；事务式逐项独立。16 方法级单测（含链式跳板与 ldc/istore 状态更新 handler 形态），全量 1647 绿（3 既有跳过）。
- [x] 不透明算术折叠首件（#44，FixPatternSimplifier 增补规则 12–14）：三条
  位级恒等式窗口——(x^K)+((x&K)<<1)=x+K（半加器进位）、(x^K)-((K&~x)<<1)=x-K
  （借位）、(x+K)-((x&K)<<1)=x^K——整窗替换为 iload/ldc + 单条 iadd/isub/ixor；
  补码回绕下位级恒等（JVM 整数语义无 UB 面）。x 为单局部槽（同槽校验，
  iload_n 与泛型同权）、K 为 int 常量（同值校验，跨 CP 条目同权）。栈操作
  噪声（dup_x2/pop/swap/dup2）窗口内消化；iconst_0 恒等算子噪声归
  ExpressionSimplifier（管线序本件在前）。落地形态为 window_rule 框架的
  规则表扩展（不动点轮次/边界守卫/写回纪律全复用），非新 pass。9 单测
  （三窗口/链式收敛留噪声/异槽/异值/移位量 2/内部标签阻断/幂等），全量
  1656 绿（3 既有跳过）。
- [x] long 族规则 15-20（#44 收口，RFC 0002 B8 8.7 切片）：窗口形态与
  int 族不互为转写（实证：dup2_x2 在 all-cat2 三值栈产出 [x,v,K,v]，与
  libjbc dupSlots 及真 JVM 实测一致；无 dup2_x4、swap 对 cat2 非法）。
  15 半加器/16 借位/17 消进位（lload+ldc2_w K 窗）、18 lload;dup2;lxor→
  lconst_0、19 lload;dup2;lcmp→iconst_0、20 i2l;l2i→空。真 JVM 基线/变换
  对拍硬验收（中途抓出 dup2_x2 深度与 lsub「深减顶」两处语义错修正）。
  逐规则正反用例（同槽/同值/移位量/重装载槽四拒收面+链式+幂等），
  FixPatternSimplifier 两套件 57/57 绿（mjbase PR #72）。
- [x] 控制流反扁平化首件（#45，FlatteningReverser，三层链收尾件）：常量
  状态传播 + switch 分发静态解析——每个「写常量状态; goto 分发块」前驱的
  goto 直接改写到 switch 案表命中的功能块入口（未命中走 default，default
  是合法状态语义），分发块失去全部入边变不可达交死代码清理。分层为
  通用引擎（唯一 switch 审计/分发操作数折叠步解析/状态槽读写全量审计/
  入边守卫（区间内锚定、物理直落块头、案表自引用）/帧深等价门/事务性
  统一改写——先全量取柄后落笔零副作用）+ 可注入形态表（前驱块尾形态行
  构造期可替换、分发折叠步规格 maxSteps/binops 可配；变体扩展面在表不在
  引擎，默认表只是保守首版：直存 + 单级中转弹跳、{iadd,isub,ixor} 单步
  偏移）。安全门：第二读点/写点为空/写点后继非 goto D（交叠定值）/iinc
  触碰状态槽/两槽值高半槽占用/分发块居方法首指令全部整方法拒收，不做
  部分改写；u32 环绕求值无符号 UB。反向操作数形态已落地（形态 B：
  [intConst 列; iload s; binop 链; switch]，常量在左 v = C op K，整链
  一致识别，应用序 = 近载端常量配远端 binop；分发块头与装载位分离，
  入边守卫用块头——4 单测：镜像单步 ISUB/XOR、镜像两步链（注入
  maxSteps=2 规格）、混合链拒收）。挂账变体收敛为：混合操作数链（部分步常量在左部分在右）；相对状态
  更新已收口（32aa2c7，账目补记见后）。
  12 单测（lookup/table/xor 正面含 default 回落与双前驱成环/六拒绝面/
  幂等逐字节一致/形态表注入扩展面证明不写死/注册面），全量 1671 绿
  （1668 过 + 3 既有跳过）。

## opt pass 余量批次裁决（W0 备料，mjt-deobf 消费面缺口对账）

mjt-deobf 侧尚在 mbase 栈消费的 opt pass 逐件裁决；"结构性吸收" = 新载体
语义下无独立变换面，登记后从迁移清单除名。体量实测（mbase cpp 行数）已注。

- [x] **StackOperationSimplifier**（stack_operation.cpp 内，方法级窗口折叠）：
  `dup;pop`→∅、`pop;pop`→`pop2`、`ldc;dup`→`ldc;ldc` 等规则迭代到不动点，
  cat1/cat2 分槽；消费 asm/matcher 底座（已迁）。**样板批首选**。
  → 已落地（2bb8d81）：32 规则 1:1 对齐，CodeDom 载体；wide ldc 与
  Err 保守停机两处声明见头注释；19 单测 + 全量 1228 绿。golden 对拍
  待批（extractor 段 + corpus + compare test）。
- [ ] StackOperationExpander：OriginalSource 帧分析展开常量 producer 上的
  DUP 族，依赖帧面，随帧链接线批次。
- [x] **FixPatternSimplifier**（236 行）：位运算固定模式 / 六种 `IF_ICMP*`
  安全折叠（非零偏移保留）/ 同 slot store/load 重写（clone producer）；
  matcher 底座已迁。样板批次选。
  → 已落地（#25，8b98945，window_rule 载体）：位运算族 + 死代码族 +
  整数比较常量偏移折叠 + 同 slot store/load 中转消除。
- [x] ExpressionSimplifier（409 行）：表达式形状折叠。
  → 已落地（#25，8b98945）：代数 peephole——恒等元 + 交换/结合 +
  mul-by--1/shift 等旧规则集（丢弃一条代数不成立的 legacy 规则）。
- [x] TypeCastSimplifier（328 行）：原始类型 cast 常量折叠 + 已知 JDK
  helper（Float/Double 位图、Integer/Long 解析）折叠（mbase 原实现即
  cast/helper 折叠面，非 checkcast 消除）。
  → 已落地（#25，8b98945）。
- [x] ConsJumpOptimizer / ConstantSwitchFolder / ConstantStateMachineEliminator
  （628 行）：常量分支/switch/状态机折叠，依赖 asm/evaluator（已迁）。
  → ConsJumpOptimizer 已落地（首件，scope:method）：常量装载 + 常量条件跳转
    相邻窗口折叠——单槽 [int 常量; IFEQ..IFLE]、空窗 [ACONST_NULL;
    IFNULL/IFNONNULL]、双槽 [int 常量; int 常量; IF_ICMPxx]。谓词恒真 → 窗口
    头改 GOTO 复用原目标标签（dom.get 原位改写）；恒假 → 整窗删除。栈安全
    守卫 = 窗口内部边界锚标签拒收（外部路径从中部进入时其栈值等待窗口内
    消费），头标签放行（整窗重放净栈效应为零）——对齐旧栈
    windowExternallyReferenced 的可观测面，收紧点：顺序末尾条件跳转无承接者
    时保守跳过（旧栈树模型无此约束照删）；类型失配窗（int 常量配空谓词/
    ACONST_NULL 配数值谓词/ACMP 配双 int 常量）显式拒绝——旧栈按谓词表折
    Verifier 非法形态属防御怪癖，不保留。12 单测（三种窗 × 恒真/恒假、内部
    边界拒收、头标签放行重定向、三种类型失配拒绝、幂等、注册面），全量
    1712 绿（1709 过 + 3 既有跳过）。
  → ConstantSwitchFolder 已落地（第二件，scope:method）：int 常量装载 +
    LOOKUPSWITCH/TABLESWITCH 相邻窗折叠——按 selector 值解析命中分支
    （switchCaseLabelOf 已从 flattening_reverser 私有提升至
    method_rewrite_util 共享：lookup 精确匹配 / table low+i 区间，未命中走
    default），常量头原位改 GOTO 复用命中目标标签，switch 删除。switch
    前置边界锚标签拒收（selector 弹消费保护），头标签放行（头部改写后
    仍在，标签原地有效）。悬空目标（resolveLabel 失败）与顺序末尾 switch
    无承接者均拒收。折叠后未选中的 case 标签留为无引用标签，不物化字节。
    9 单测（lookup 命中/未命中、table 命中/出界、switch 前锚拒收、头锚
    放行、非常量 selector 不动、幂等、注册面），全量 1721 绿（1718 过 +
    3 既有跳过）。
  → ConstantStateMachineEliminator 结构性吸收，不独立迁移：其可解析形态
    面（头 [常量; istore s; goto D] + D: iload s; switch + 各 case 尾
    [常量; istore s; goto D]/终态返回 + 状态链行走 + 外部跳转入岛拒收）
    已被 FlatteningReverser 引擎覆盖——默认前驱形态表与分发折叠步规格正是
    同族形状（PR #49 端到端即该标准形态）。两处载体差异论证：旧栈纯体
    白名单（禁调用/字段/除法/数组/monitor，树模型无等价性判据的保守面）
    对应新栈帧深等价门（更一般）；旧栈「直线化 + 状态尾随改写内删」对应
    新栈 goto 重定向 + 局部存储清理家族收敛（UselessPopCleaner/PopUnUsed
    /RedundantLoadVal 联合不动点已证，见 pop_unused 端到端）。旧栈遇任意
    try-catch 全局弃置，新栈按标签粒度入边守卫，覆盖面只宽不窄。独立成件
    与引擎重叠，违反件职责单一。ConsJump 三件套至此收口。
- [x] RedundantJumpRemover / GotoRearranger / ContinuousLabelMerger
  （449 行）：label 布局族——新载体无标签节点，逐件载体重论证（部分可能
  结构性吸收或改形为边界句柄重排），未论证前不迁。
  → RedundantJumpRemover 已落地（首件，scope:method）：目标落在顺序后继
    前置边界的跳转消解。载体重论证：旧栈「jump 后紧跟 LabelNode」在新载体
    即 labelsAt(order[i+1]) 含跳转目标——边界句柄判定，无标签节点遍历。
    GOTO/GOTO_W 整删（fallthrough 等价）；条件跳原位换 POP/POP;POP（分支
    与汇合重合，条件值必被丢弃，弹宽对齐谓词元数：单值族 1、icmp/acmp
    族 2，双值经 insertAfter 补第二条）。方法末指令无顺序后继天然不匹配
    （旧栈的末尾标签形态在合法 classfile 不可构造——条件跳必须有后继、
    goto→len 被 promote 拒收，End 绑定路径不设）。7 单测（GOTO 删除/单值
    条件跳/双值 icmp/双值 acmp/空谓词/远目标不动/注册面），全量 1728 绿
    （1725 过 + 3 既有跳过）。
  → 余项载体重论证收口：
    ContinuousLabelMerger 不迁——同界标签别名合并在旧栈是树模型开销优化
    （减少 LabelNode、加速后续 pass），其 classfile 可观测面恒等（分支偏移
    不变，仅目标别名重定向；LNT 引用替换在旧栈可改 LNT 属性，新载体 LNT
    是 backing 不可见面）。新载体无标签节点、无引用计数成本，多个标签同
    锚一界各随引用者物化同一 pc，合并不产生任何字节级改写。除非未来出现
    以标签身份为键的分析面需要规范化，本件在新载体无落点。
    GotoRearranger 已落地（标签布局族第二件，mjbase PR #71）：载体原语
    jbc move-range（libjbc#10，libjbc PR #12 CodeDom::moveRange 机械层 +
    CodeEditor::moveRange 策略层）到位后按台账语义重论证实现——前向非邻接
    GOTO 目标尾段经 moveRange 尾哨兵搬移使跳转变 fallthrough 等价整删；
    保守门五条（前向非邻接/JSR-RET 整体不动/异常表锚定拒动/目标单前驱/
    环形拒动），帧面与 RedundantJumpRemover 同口径 Strict 重算兜底。
    13 单测全经生产管线真链验证。标签布局族首件已落地，
    族内分析至此收口。
- [x] UnreachableInsnRemover 家族（dead_code_remover.cpp 1594 行内非 NopRemover
  部分，切片迁移）：帧可达性证明删不可达指令。NopRemover 已由方法级四件批收口。
  → 首件已落地（结构级 UnreachableInsnRemover，scope:method）：帧引擎 BestEffort
    reachability 一次性判死；锚异常表标签的不可达指令保守保留（不重锚边界，
    死保护区连同表项交异常面清理批次）；标签两阶段政策——无引用标签批删前
    销毁（防 Boundary StillUsed 拒收），集内互引分支标签批删成功后销毁；批删走
    kRejectIfUsed（守卫已排除批外引用，无需承接者重定向，顺带正确处理死块
    延伸到方法末尾形态）。8 单测（死岛/死块内互引/异常标签保守/种子 handler
    保留/无死代码/幂等/与 FlatteningReverser 端到端：死分发块 + 不可达 default
    块清空/注册面），全量 1679 绿（1676 过 + 3 既有跳过）。
  → 局部存储清理家族首件已落地（PopUnUsedLocalVariables，scope:method）：方法内
    无任何读取的槽位，其 store 降级 pop/pop2。读点审计按指令全集做（不可达读点
    同样计入，保守口径），覆盖 iinc/ret 与两槽装载高半槽；宽度由 store 族操作码
    自身决定（JVMS 定死每族弹槽数）——载体差异：不需要旧栈 OriginalSource 帧
    定宽前提。只改操作码字段不删指令、不删生产者（常量装载清理归
    RedundantLoadValEliminator，局部装载清理归 UselessPopCleaner 切片）。9 单测
    （int/long/短形态/iinc 读点/两槽保护/全保留/幂等/三件链端到端：状态槽 store
    降级 + 槽 2 被 A 块存活 iload 保护/注册面），全量 1688 绿（1685 过 + 3 既有
    跳过）。
  → UselessPopCleaner 首切片已落地（相邻窗口载体，scope:method）：局部装载 +
    紧随 POP/POP2 的相邻对消除（宽度匹配：cat1 配 POP、cat2 配 POP2）。载体
    论证：旧栈帧溯源认弹值生产者（load 自身即生产者、非穿透），新栈现有两域
    均不落此语义——OriginDomain 穿透 store→load、DefUseDomain 把 store 定义带
    过 load；相邻窗口 + 不动点在栈纪律下覆盖同一可观测面（POP 弹栈顶，纯装载
    与紧邻 pop 之间无「不碰栈顶」的指令，隔标签即被边界守卫拒收），配对删除
    恒栈中性、不读槽内容。独立成件而非并入 RedundantLoadValEliminator 矩阵：
    golden 对拍钉死后者与旧栈逐语义等价，hb_pop_pairs 语料含装载;pop 形态，
    并矩阵即未声明语义偏离。11 单测（cat1/cat2 折叠、常量对/运算生产者/弹宽
    失配不入矩阵、不动点交错形态、装载侧标签重定向、POP 侧标签守卫、注册
    面），另有家族收敛端到端（pop_unused 测试内）：FR + UIR + PULV + 常量清理
    + UPC + PULV + 常量清理 七趟把状态链残段清空（无状态槽访问、无 pop、无
    常量残段；默认块随死分发块由 UIR 清理）。全量 1700 绿（1697 过 + 3 既有
    跳过）。
  → 余项挂账：DeadLocalStoreRemover/RedundantStoreEliminater/
    DeadSideEffectFreeCallRemover/RedundantInvokeRemover/UnusedResultCallEliminater
    依赖帧溯源（生产者定位/槽位活跃性），随帧面批次逐件论证；UselessPopCleaner
    帧载体形态（非相邻生产者、POP2 双 cat1 半删降 POP、DUP/DUP2 被 pop 吸收、
    多来源汇合）同批挂账。
  → UselessPopCleaner 帧溯源面已落地（def_use 到达定义载体）：弹值的全部
    到达定义均为纯常量生产者（int/long/float/double/string 常量装载 +
    ACONST_NULL）且每个定义的全部使用边指向本 pop → 生产者组 + pop 整删；
    POP2 顶值 cat2 整删、双 cat1 两侧整删/单侧半删原位降 POP（与旧栈
    canRemoveForPop 逐形态对齐，含仅底值可删的降级）。载体论证：跨距离/
    跨路径对窗口面不可见，def-use 合并到达定义天然覆盖（分支汇合多定义
    同组处理）；帧面不需要窗口面标签守卫（边覆盖进入 pop 的全部路径），
    删除仍走 fallthrough 重定向。安全门：DUP/DUP2 族/SWAP 任一在场禁用
    帧面（复制类无使用边，删生产者留悬空中继破坏栈元数）——逐中继细化
    已落地（RFC 0002 B8 8.6，mjbase PR #70）：jbc dupSlots 中继拷贝沿用
    原定义（透明、永不成为 def）是整族禁用的根因；细化后从 pop 回溯模拟
    输入槽需求，DUP/DUP2 拷贝别名折叠为一份原件、溯源常量与 def 侧互证、
    共享连通分组组栈平衡非零整组弃；X 族/SWAP/cat2 双槽/链上标签锚定等
    保持 fail-closed。方法无中继 = 既有纯 def 路逐字节不变；分析失败/
    truncated 跳过；不播种入参（参数弹值无到达定义边，
    保守不动）；局部装载不作帧面生产者（def-use 里定义归 store，隔距离
    装载对删除需连 store 活跃性一起判，归 DeadLocalStoreRemover 合同，
    不在本件重复）；LDC(class/MT/MH) 纯生产者挂账帧面余件。10 单测
    （常量/ACONST_NULL/非相邻双 pop、POP2 long/double/双 cat1、半删顶侧
    参数底值/底侧 store 转发顶值、中继守卫、多定义汇合），全量 1737 绿
    （1734 过 + 3 既有跳过）。
  → DeadLocalStoreRemover 已落地（帧溯源面主件，def-use 载体重推导）：
    槽位有意义 ⟺ 某次读的到达定义沿 def→use 边闭包到达有意义汇点
    （invoke/分支/switch/字段写/monitor/athrow/带值返回/数组存；store 与
    iinc 作中间定义续走穿透槽位间转存，dup/swap 复制透明消费边直连原
    定义）。处置为联合不动点：死槽 store 整删 ⟺ 弹值到达定义全部是纯
    常量生产者且属整删集；常量属整删集 ⟺ 全部使用边指向整删 store
    （降级 POP 的 store 仍在消费，生产者保留）；不满足则原位降级
    POP/POP2（宽度按 store 族操作码，method_rewrite_util::storeLocalSpan
    自 PULV 提升共享）。载体差异（均较旧栈保守或声明强化）：签名播种
    入参（参数槽保护）；读点到达集合溢出/无定义边保守记有意义；截断
    整件跳过；运算生产者不整删（def-use 里运算有定义边、旧栈 origin
    透明不进链——旧栈同形态也只降级，可观测面一致）；装载对残段
    [load;pop] 由管线窗口面下一轮收敛（7 单测之一钉死）。11 单测（直喂
    常量整删、读点喂 pop 仍死、汇点保槽、参数槽、运算降级、共享常量
    逐出（dup 透明边）、转存链穿透、cat2 族、分支汇合多定义整删、分析
    失败跳过、注册面），全量 1748 绿（1745 过 + 3 既有跳过）。
  → DeadSideEffectFreeCallRemover 已落地（帧溯源面批次，白名单声明表 +
    def-use 链闭包载体重推导）：白名单 = 零参无副作用返回 cat2 的 JDK
    静态调用（System.nanoTime/currentTimeMillis ()J、Math.random ()D，
    扩展只增条目）；链成员 = 白名单调用叶子 + 纯计算生产者（常量装载/
    算术/数值转换/arraylength/instanceof），输入边 arity 缺失守卫（看不
    见的来源不可删）；触发条件 = 链含白名单调用且弹值定义全入链（纯常量
    链归 UPC 帧面不重复）；共享消费者逐出不动点级联；区间门 = [链首,
    pop] 只含链成员与 pop 且内部无存活标签（区间外分支不得中途进入；
    布局 wrap 保守拒收）。安全门：方法含异常表整件跳过（handler 入口
    值无定义边，精细化挂账）、分析失败/截断跳过、签名播种入参。13 单测
    （直弃/random 白名单/隔 LADD 运算/L2I+IADD 长整尾链、纯常量链不动、
    非白名单不动、dup2 转存外部消费逐出、区间内外来指令/内部标签、
    异常表跳过、幂等、写回一致、注册面），全量 1761 绿（1758 过 + 3
    既有跳过）。
  → RedundantStoreEliminater 已落地（帧溯源面批次，def-use 载体重推导）：
    store 定义零使用边（栈/局部分量均无）→ 原位降级 pop/pop2（宽度按
    store 族）。与旧栈线性序载体（store/load 下标区间）的差异：旧栈按
    位置判「区间内无读取即覆写」，控制流从区间内跳出到区间后的读取会
    误删活存储（位置孔洞）；到达定义按路径追踪，该形态正确保留（单测
    钉死为修正而非偏离）；尾部死写（方法结束前无人读）旧栈因「无下一
    store 界」不处置，零使用边粒度天然覆盖（修正扩展，单测声明）；iinc
    读改写产生使用边，旧栈 iinc 槽整槽排除在 def-use 下自然内化。与
    PULV（零读点槽）/DLSR（槽级汇点闭包）分工互补：本件到边粒度，槽内
    其他定义不牵连。安全门：分析失败/截断整件跳过。10 单测（覆写降级、
    尾部死写、读取保槽、跳出到区间后读取双 store 全保（旧栈孔洞）、
    iinc 边保护、cat2 宽度 POP2、参数槽、分析失败跳过、幂等、注册面），
    全量 1771 绿（1768 过 + 3 既有跳过）。
  → UnusedResultCallEliminater 收口（结构吸收进 DeadSideEffectFreeCall
    Remover，不另立件）：旧栈 URCE = 相邻窗 [常量实参..; 白名单调用; pop]
    删除，白名单 = String.length/isEmpty（常量字符串接收者）+
    Math/StrictMath 31 个纯函数名（全参常量）。DSFCR 引擎（白名单声明
    表 + def-use 链闭包 + arity 边守卫 + 区间门）覆盖相邻窗为子集：
    白名单扩展为三类条目（零参精确/带参精确 String 虚调用/Math 家族），
    带参条目期望输入值数按描述符（虚调用含接收者），全部输入定义入链
    且边数对齐——旧栈「全参常量」要求收敛为链成员资格（纯计算实参同
    样入链，按纯性论证为文档化安全扩展；参数实参/接收者天然失配保留）。
    Math.random 零参条目旧栈 URCE 因 producers.empty() 不收、DSFCR 首版
    已收——两表并一后并集闭环。6 新单测（常量串 length 整删、Math.max
    两常量整删、Math.abs(J) 配 POP2、参数实参/参数接收者保留、纯计算
    实参扩展声明），全量 1777 绿（1774 过 + 3     （1774 过 + 3 既有跳过）。
  → RedundantInvokeRemover 已落地（帧溯源面批次收口，全局件「普查 +
    委派」载体重推导）：普查 = 容器全类装载（任一不可解析整件放弃）→
    定义集（全部类声明方法三元组）→ 逐方法 promote 扫「调用 + 紧随宽度
    匹配弹弃」（返回 cat2 配 POP2/其余配 POP）且目标入定义集 →
    MethodEffectModel isPure 提名（ReadOnly 不提名，与旧栈 isPure 档
    对齐）；应用 = 提名成员装 MethodCallRemover includeCalls 委派点名
    面（栈等价改写/实参补弹宽/句柄钉死/两阶段提交全复用，且覆盖死局部
    消费面——旧栈仅 pop/pop2 面，超集安全）。与旧栈「调用换 ICONST_1
    留实参残段」的差异：点名面单件摘净调用与消费面，实参生产者残段由
    局部清理族后续收敛。只删调用点不删定义（悬垂方法归 unused-member
    族收尾）。8 单测（纯弹弃删、只读/非纯/返回被消费/容器外目标保留、
    跨类全应用、被调方不改写、注册面），全量 1785 绿（1782 过 + 3
    既有跳过）。GotoRearranger 挂账的 jbc move-range 原语已收口
    （libjbc#10，libjbc PR #12，0.4.19）。
- [x] LabelDeadVarStoreEliminator（LDVSE，#18 组员，120 行）：标签锚定面的
  store/load 中转对消除。复刻落地为 StoreLoadRelayEliminator（scope:method）：
  载体换 analyzeDefUse——store 定义的局部读边（slot=Local）恰为紧邻 reload
  → 整对删除、值留栈（栈消费边是 reload 恒等搬运的转发，不计入）。旧栈
  标签段窗口 + 指令白名单是「无其他观察点」的近似载体，不复刻；其窗内
  多对面不健全（删对后窗内后续同槽读点观察陈旧槽值，构造反例 2v+2 变
  2v+1），新栈单使用判据天然拒绝。新增双侧锚标签守卫（分支目标/异常表
  边界整对拒收，对齐「不重锚异常表边界」惯例）；五族全宽覆盖（旧栈仅
  int）。12 单测（int/long/引用/快捷形删除、多读点/handler 读点保留、
  双侧锚拒收、不动点、幂等、注册面），全量 1930 绿（1927 过 + 3 既有
  跳过）。
- [x] RefCopyPropagator（#19 首件，scope:method）：单来源不可变引用的
  aload 原位改写为来源指令。载体换 traceOrigins 来源域——aload 读槽的单
  来源事实即全路径同源，旧栈「方法级恰一次 store」预扫描门槛被来源单值
  性内化（覆盖只宽不窄）；来源域穿透 aload，多级暂存槽链一步折叠到最初
  来源（比旧栈更远）。白名单 = aload 族/ldc 引用常量（String/Class/
  MethodType 同条目恒等）/aconst_null；Param 来源不可克隆且槽可再写不可
  回指保守保留，aload→aload 即时折叠不可判（交 StoreLoadRelayEliminator
  /RSE 族收敛）。双侧守卫 fail-closed，单遍无不动点（与旧栈一致）。10
  单测（ldc/null 直达改写、多级暂存穿透、再写穿透、Param/汇合未知/
  白名单外/自赋值保留、幂等、注册面），全量 1940 绿（1937 过 + 3 既有
  跳过）。
- [x] GlobalStrPoolOptimizer（#19 组员，scope:global）：容器内 String ldc
  去重迁移进合成字符串池类，ldc 原位改写为「索引装载 + getStr(I) 调用」。
  载体换会话合成模型——makeSession(NewClassSeed) → addMethod →
  promote/append/lower 全合成 helper（旧栈 visit 流式 writer）；改写对
  净栈中性（分支目标帧不漂移，无需帧再生），maxStack 每方法保守 +1（任一
  点位中间深度不超旧 max+1）。偏离逐条申报：池类名改中性 `str/Pool`——
  旧栈类名携带来源仓身份，公开面禁现；clinit 计数由旧栈无条件 SIPUSH
  （n>32767 溢出）改 materializeIntConst 波段（ICONST/BIPUSH/SIPUSH/
  LDC）；索引按遭遇序编号、clinit 字典序写出（对齐旧栈）。安全门：任一
  类不可解析整件放弃（fail-closed），两阶段提交（改写批次与 helper 全部
  序列化成功才动容器），池类已存在幂等整件跳过。9 单测（迁移与改写含
  波段与 maxStack、跨类去重、非 String ldc 保留、非 class 资源忽略、
  池类已存在/二轮幂等、不可解析放弃、无字符串不动、helper 形状、注册
  面），全量 1949 绿（1946 过 + 3 既有跳过）。
- [x] ArithmeticInsnRearranger + CommutativeOperatorReorder（#20 重排家族，
  scope:method）：连续 xLOAD;CONST;可交换算术;xSTORE 四元块按槽首现序
  分组重排 + 交换律链常量操作数后移（常量相邻供折叠命中）。载体换
  CodeDom 快照 + 原位内容置换（srcPc 随内容搬移；旧栈 clone 重建整列，
  可观测指令序一致）；快捷形与泛形同判（旧栈 tree 规范化等价面），常量
  面补 LDC_W/LDC2_W。标签守卫 fail-closed：窗口/链起点与内部任一边界
  锚标签拒收（锚点随重排漂移改变入边落点；旧栈列表语义对链首锚点隐式
  重锚，不复刻）；旧栈头注宣称的 getstatic I/J 操作数实现未覆盖，以
  旧实现代码为准不补声称面；入口门槛按指令计数（旧栈含标签/行号列表
  项）。17 单测（跨槽归组 int/long/快捷形、单块/同槽恒等/幂等、锚标签
  守卫两件、非交换律/异宽操作数/形状不齐保留、常量后移 int/long、注册
  面），全量 1966 绿（1963 过 + 3 既有跳过）。
- [x] StackTraceRecover（#20 组员，scope:global）：堆栈可读性恢复三件套——
  SourceFile/SourceDebug 覆盖式写入（name.replace('/', '.') + '.java'，
  SourceDebug 同值 RawAttr u2 载荷承载）、异常 handler 聚合注入
  dup;invokevirtual printStackTrace()V（单型取该型，catch-all/多型共享取
  java/lang/Throwable——硬编码 Exception 会让 catch-all handler 入栈类型
  不可赋值产生 VerifyError；聚合键取 handler 锚定指令，旧栈 LabelNode 按
  pc 去重，IR 侧 LabelId 异同不作数）、每标签锚点一行 LNT（行号类级跨方法
  递增，>65535 回绕；行按旧 pc 坐标前置落位，lowerMethod 按 pcmap 重定向
  注入漂移）。偏离逐条申报：GlobalPass 整件 fail-closed（任一类不可解析或
  任一步失败整批放弃，旧栈逐类 best-effort 不复刻，对齐容器两阶段提交
  口径）；LNT 直写 backing Code 属性嵌套槽（旧栈 LineNumberNode 树节点
  不复存在），外层 Code 槽手工置脏走 writer 重编码；maxStack 保守 +1
  （旧栈 ASM 自动重算不复刻）；未锚定 handler 跳过；非幂等（覆盖式写入 +
  恒 none()，对齐旧栈）。10 单测（单型/catch-all/多型冲突共享/同型共享
  注入与 owner、LNT 行写入与跨方法递增、source 属性写入与既有覆盖、
  不可解析放弃、注册面），全量 1976 绿（1973 过 + 3 既有跳过）。
- [x] 重载拆分 shadow 生成策略（#22 视图层，新增件）：同名异签方法组内按
  desc 字典序生成 `<原名>$N` shadow 名写入 ShadowAliasTable（字节码零写入，
  显示层专用）；拆分键 = (owner, name, desc) 表既有三元组，descriptor 只
  参与组内排序与反查；仅 size>=2 组拆分；候选撞类内真实方法/字段名或作用域
  内已分配 shadow → N 递增跳过；已登记成员保留原 shadow（幂等，手工名不丢，
  不占号）；类/组/组内三级字典序保证容器遍历序无关。批量指派单事务：全部
  规格校验过才落表（未知类/方法、空名、撞真实名、撞他成员 shadow 含同批
  先到、异 shadow 重指派、同成员异 shadow 重复规格全走显式 Err，表零改动），
  同 shadow 幂等重指派 Ok(0)；收尾 backfillTokens 同机械占位名生成。与
  rename 的优先级：shadow 永不进 RenamePlan/字节码（表头注释既有红线），
  真实改名后条目经 prune 存在性口径存活/失配。5 单测（desc 序编号与反查、
  真实名/shadow 撞号递增、手工名保留与幂等、字节零写入、批量事务全校验
  面），全量 1981 绿（1978 过 + 3 既有跳过）。
- [x] WriteOnlyArrayCleaner（#37 第三件收口，scope:method，新增件）：只写
  数组分配连存储窗整体删除——S1 直通形 [size; NEWARRAY/ANEWARRAY; (DUP;
  int常量; 元素常量; XASTORE)*; POP] 与 S2 暂存形 [size; 分配; ASTORE n;
  (ALOAD n; int常量; 元素常量; XASTORE)*] 连续窗形状匹配（k>=0，含
  分配;pop 与 分配;astore 死分配特形）；元素/下标生产者限单条纯常量装载
  （DeadLocalStoreRemover 帧面同族口径），size 限紧邻前驱纯 int 常量。
  安全门 fail-closed：窗内（含窗首与 size 生产者）任一锚标签弃、任一异常
  范围与窗相交弃（分配/存储抛出可观测，全含亦弃）、S2 局部 n 窗外任一
  读写出现弃（各类型 load/store 族 + IINC + RET 全扫）、MULTIANEWARRAY
  不收、eraseBatch 全有或全无失败整件放弃。裁决收口：管线串联用例钉死
  调用方装箱窗由 ArgumentArrayScalarizer 自删零残段（挂账的「残段归既有
  删除面覆盖」假设以实证收口：不成立也不需要——scalarizer 自删其窗），
  窗外只写哑数组 survive scalarizer 由本件串联清理；旧栈无对应件
  （dead_code_remover 把分配归副作用族不删），本件为新增件。纯删除
  maxStack/maxLocals 不变。13 单测（S1/S2/原始数组/k=0/快捷形删除五件、
  读取逃逸/非常量元素/非常量 size/异常域相交/窗内锚标保留五件、管线串联
  裁决、注册面），全量 1994 绿（1991 过 + 3 既有跳过）。
- [x] ArgumentArrayScalarizer 切片 1（#37 首件，scope:global）：装箱参数数组
  标量化——static ([Ljava/lang/Object;)R 候选（句柄钉死/同类同签名唯一门）
  被调方取参窗 [aload p; intConst i; aaload; checkcast Ti] 重建为直接局部
  取参（槽平移 >p 加 n-1，junk 擦除），调用方装箱窗 [iconst n; anewarray
  Object; (dup; arg; iconst_j; aastore)×n; invokestatic] 剥壳为顺序实参，
  MemberRemapPlan 描述符同步（声明+全引用+句柄重写复用既有 remapper，
  字节码先行两阶段提交）。安全门：下标 0..n-1 全覆盖、参数槽触碰审计、
  逃逸审计（def_use 参数定值使用边须全落窗头 aload/窗内 aaload 两类
  位点——aload 原样压栈使 aaload 处再记一条栈用边）、argMatchesTi 首切片
  静态可判面（ACONST_NULL 任意引用 Ti/LDC 字符串常量限 String/Object/
  ALOAD 槽类型不可见只认 Object，精化归切片 2）、删点不锚标签、CP owner
  须等声明类、apply 失败候选整族标记死亡（残余位点静默跳过）。promote
  重建语义下分析期 CodeUnitRef 进 plan 结构保活、apply 期句柄复用不再
  升格。8 单测（全链还原/null 实参/重复取参同槽、类型失配/逃逸/下标缺口
  /非窗位点弃收、注册面），全量 1797 绿（1794 过 + 3 既有跳过）。实例
  形态与死数组清理归切片 3。
- [x] ArgumentArrayScalarizer 切片 2（#37，scope:global）：实参类型对齐面——
  argMatchesTi 精化三支：ldc Class 常量按常量类对齐（Ti ∈ {常量类,
  Object}）；aload 实参槽类型提升——调用方描述符入参逐槽展开（对象型去
  L/; 壳出内部名，数组保留描述符形，J/D 宽 2 只推槽号；实例方法槽 0 =
  this 取调用方类），读点到达定值经 def_use 证据（恰为同槽入参定值、
  单定义、未截断——槽未被重存储/多前驱合流）时按声明型对齐，否则维持
  Ti == Object；数值 CP 常量入引用位 fail-closed 拒收（未装箱不可验证）。
  调用方 def-use 惰性构建（仅 aload 实参需要证据时，单方法至多一次）。
  多指令实参形态（装箱工厂 valueOf 等需窗形扩展）挂账切片 3，与实例
  形态/死数组清理一并。5 单测（Class 常量对齐+失配、static 入参直传、
  槽重存储拒收、实例 this 直传），全量 1802 绿（1799 过 + 3 既有跳过）。
- [x] ArgumentArrayScalarizer 切片 3（#37，scope:global）：多指令实参窗形
  扩展 + 实例形态——调用侧装箱窗从固定步长改顺序扫描（窗头锚定回扫，
  元素边界回溯容嵌套同形序列，实参组至多 kMaxArgInsns 有界）；实参组
  静态可判面新增双指令装箱链 [单值生产者; wrapper.valueOf(prim) /
  String.valueOf(...)]（产出型由调用决定，生产者只做宽度形态校验：
  基本型 op 族与入参字母匹配，B/S/C/Z 走 I 面），其余多指令形态
  fail-closed；实例候选落地（参数槽基 1，逃逸审计 sig 带静态性，调用
  位点收 invokestatic/virtual/special 且静态性须与候选一致，槽平移/
  窗头改写/描述符同步对 paramSlot 全泛型）。死数组清理仍挂账（载体
  待重论证：标量化后 anewarray 残段属帧溯源族既有删除面覆盖范围）。
  3 单测（装箱链还原+产出型失配拒收、实例形态 invokevirtual 全链），
  全量 1805 绿（1802 过 + 3 既有跳过）。
- [x] InstanceArrayFieldScalarizer 并入 StaticArrayFieldScalarizer（#39
  第二切片，scope:class）：实例字段同面——private（去 static-only 门）
  getfield/putfield 窗形带接收者 aload（读/写窗保持原位，分配窗四条
  整体删：接收者 aload 唯一消费者是被删 putfield，保留悬栈）；实例
  分配只在 <init> 收录（运行一次语义；reset 类重执行分配的重置语义
  不闭包 fail-closed）；新标量字段 access 随原字段静态性。4 单测
  （实例 roundtrip、非 <init> 分配弃、arraylength 弃、前驱非 aload
  邻接破坏弃），全量 1809 绿（1806 过 + 3 既有跳过）。
- [x] UPC 帧面 LDC 元数据常量生产者（帧面余件收窄）：isFrameRemovable
  ConstProducer 认领 ldc Class/MethodType/MethodHandle——元数据常量无
  求值副作用，弹弃即纯删；Dynamic/condy 不认领（bootstrap 链有求值
  副作用，fail-closed）。1 单测（ldc Class; pop 收敛），本地构建面被
  并行会话 libjhasm 迁移占用，CI 门验证。
- [x] libjbc 0.4.15 → 0.4.18 升级适配（迁移批 B1）：typed 属性面扩容
  （0.4.16 起注解族四件 ParameterAnnotations/AnnotationDefault/
  MethodParameters/ConstantValue typed 解码，Raw 槽字节手术消费面换道）
  + mount 面（0.4.16 增 attach 族）+ consumer 模块移除（0.4.17，本仓未
  消费）+ MethodParameters 接收者行 gate（0.4.18）。消费面适配三处：
  meta_info_remover MethodParameters 清位改双道（typed 行模型主道 + Raw
  字节道，布局判定 u1 count 标准形优先、u2 非标形兜底——JVMS 4.7.24
  parameters_count 实为 u1，javac 产物与旧栈 writer 皆此形，历史夹具的
  u2 计数形是非标产物，注释口径一并纠正）；Runtime{Visible,Invisible}-
  ParameterAnnotations 槽过滤接 typed 行模型（Annotation.typeCp 按 utf8
  过闸，全删擦槽、部分删 mutate 行）；tool_class_combiner ConstantValue
  回读、workspace fieldConstantValue（其 cast<RawAttr> 无检查，typed 槽
  上 UB 致 WorkspaceCore 指令族用例 SEH——cast 契约是 isa 守护后使用，
  kind 分道先于 cast）改 kind 分道双读。测试面：MethodParameters 夹具
  改 u1 标准形（13 字节）+ 断言偏移随行；3 个读回断言块接 slotPayload-
  Bytes（typed 重编码去槽头/Raw 直取/源 span 兜底）。全量 1810 绿
  （1807 过 + 3 既有跳过）。
- [x] libjhasm 0.0.1 → 0.3.2 升级适配（迁移批 B3）：jasm 文本面后端整体
  换道——libjhasm 0.3.x 自身已把后端切到 libjbc（0.3.0，libjblw/
  libjbytecode 在 jhasm 内退役），本仓随之去 libjblw 直连依赖。
  消费面适配：JvmCompilerOptions 改 majorVersion/minorVersion u16 直载
  （去 jblw JavaVersion 与 maxStackHint，后者零消费方实证删除）；
  requireconfs 去 libjblw/libjbytecode 嵌套钉定五条；unittest 闭包去
  libjblw；libjbytecode 后随 RFC 0002 B8 8.7 全摘（mjbase PR #72，
  relaxed_verifier 直驱 libjbc FrameEngine 后 jbytecode 符号归零）。升级途中两笔 libjhasm 端修复合入（PR #2/#4，0.3.1/0.3.2）：
  strict 复核三面（字段操作此前按方法描述符 ')' 定位字段描述符恒跳过、
  虚分发接收者/ARETURN 缺名字互赋）+ anewarray 落码组件口径（JVMS 6.5，
  javac 同形；打印按 CP 原样，两种既有形态均可往返，JDK21 加载验证过）。
  包安装陷阱记录：xmake git 直连包的源缓存（~/.xmake/cache/packages/
  .../source）不随 install 清理——换版本重装若源缓存滞留旧 checkout，
  会静默按旧源构建（"Invalid type descriptor" 假阳性即此）， purge 须
  同时清 cache/packages 源缓存与 packages 安装目录。全量 1810 绿
  （1807 过 + 3 既有跳过）。遗留：帧发射缺异常处理器入口帧（libjhasm
  issue #3，独立切片）；relaxed_verifier 改写已随 RFC 0002 B8 8.7 落地
  （mjbase PR #72：不走纯转发 jhasm::analyze——其 refCompatible 硬编码
  拒数组协变面会破 pinned 语义，改直驱 libjbc FrameEngine Strict 档 +
  自写 RelaxedResolver，对外 analyzeRelaxed 签名不变，六条语义差异
  头注存档；libjbytecode 随之全摘）。
- [x] StaticFieldDefaultInliner（121 行，scope:class）：clinit 单写常量 →
  字段 ConstantValue（backing 槽）+ 等价初始化删除；类级载体重定型首件
  候选（双面落盘纪律）。
  → 已落地：载体定型——字段面 backing MemberInfo 新建 ConstantValue
  RawAttr 槽（tool_class_combiner rebuildConstantValue 同口径），指令面
  promoteMethod → CodeDom 逐对 erase（ErasePolicy::kRejectIfUsed +
  labelsAnchorAt 前置分流）→ lowerMethod；落盘序 lower 成功才写字段面，
  失败时会话弃置即无半内联态。语义对齐 mbase 治理版（多次 PUTSTATIC
  预扫描计数非单次写整族跳过）；计数精确化：只数 owner==本类（旧栈按
  name+desc 全数，他类同名字段连带挡刀），pass 头注声明差异。8 单测
  （iconst/bipush/sipush/ldc × I/F/J/D、多次写字节恒等、既有 CV 不改写、
  非 static 与他类 owner 跳过、非常量前驱跳过、无 clinit、注册面
  scope:class）；全量 1605 绿。
- [x] UnusedLabelRemover：**结构性吸收**（裁决 2026-10-18）——新栈 CodeDom
  无标签节点，标签是按引用物化的边界句柄；LNT/LVT 锚定指令位（AIR
  originalOffset 批三），旧栈「孤立 LabelNode」在新载体无可序列化形态，
  无对拍对象。从迁移清单除名，不实现。

## 待办

- [x] **2026-10-19 air/sir 出仓定案（morpher 仓内副本退役）**：deobf 工具面 IR
  （AIR 节点指令层 + SIR 沿擎）按归属裁决不入 mjbase 独立仓——本波次以上各
  「AIR/SIR 批」计量均为彼时 morpher 仓内载体的完成记录；代码以
  `mjt::deobf::air/sir` 命名空间驻留 mjt-deobf/src/mjt/deobf/{air,sir}，
  单测随 mjt_deobf_unittest。批四（sir 真实 SSA→栈机 lower）随之改隶
  mjt-deobf，不再排 mjbase 波次。独立仓 0.0.1 首发为纯框架面（不含 air/sir）。

- model / jar / mapping / project 批次：零旧栈依赖，直迁或按需重设计。
- B8 挂账（air/sir）拆四批推进：
  - [x] 批一：builder 读接缝收回（libjbc 零改动）。multianewarray 描述符改经
    classOf 语义读（classOf/setClass 同族覆盖 multianewarray，旧「不覆盖」
    判断已失真）；异常表 catch 类型改经 backing().className 公共读面做
    Class→Utf8 一跳解析（替代裸池字节遍历；AIR typeIndex 是字符串表坐标，
    驻留走 internString，非 libjbc CodeUnit 重放的 internClass 同款）；
    ldc 的 MethodType/MethodHandle/Dynamic 经 constOf 分派识别后与旧栈同权
    归一 Nop（旧栈常量节点对三种 kind 零载荷、不读池），craft 字节用例钉死；
    builder 侧裸池读清零。
  - [x] 批二：air_writer 写回保真（2026-10-16，libjbc 零改动）。AirWriter::
    writeMethod 整体重建（局部 CodeDom 全量构建后整体替换单元域，异常表以
    新域标签重建）；类型族载荷（Load/Store/BinOp/UnaryOp/Return 与
    ANEWARRAY 形态的 NewArray）按同位约定从源域同位指令恢复（fold 结构等价
    才复用，偏离即 Err(FoldDivergence) 拒写）；ldc MethodType/MethodHandle/
    Dynamic、JSR/RET、invokedynamic 沿批一口径 fail-closed
    （Err(UnmodeledSemantic)，模型不动）；writeClass 逐方法折叠等价门（等价
    方法零触碰，字节逐位保真）+ 差异方法整体重建；AirSessionAdaptor 有改动
    时对被重建方法驱动与 SPI 管线同一条 lower + Strict 帧链（嵌套属性布局
    适配 → lower → Strict 重算 + maxes 采用 + SMT 再生），resolver 未注入且
    SMT 悬垂时拒绝产出缺帧类；重建域按同位继承源 pc，lower 的 LNT/LVT/SMT
    重定向锚保持有效（精确升版留批三）。mjbase_unittest 1286/1284/2（批前
    1278/1276/2，+8：writer 用例 4→9、新增真机门 2 与语料门 1）。挂账：
    invokedynamic 的 BSM/调用点参数面（拒写，随 sir/lower 后续批评估）；
    分支 i16 溢出显式失败、不自动拓宽 GOTO_W。
  - [x] 批三：元数据面——LNT/LVT/LVTT/SourceFile 的 AIR 提升（2026-10-16，
    libjbc 零改动）。读侧：builder 经 backing typed 属性面把 Code 嵌套
    LNT/LVT/LVTT 提升进 AirMethod（LVTT 签名列独立入
    AirLocalVar::signatureIndex、描述符列记 0），类级 SourceFile 提升
    sourceFileIndex；坐标一律换算为 AIR 指令下标（方法末尾边界记指令总数），
    畸形行（起点/终点不在指令边界）显式裁去，不猜不钳；提升只读解码不置脏，
    等价门零触碰路径的 copy-through 保真不受扰。写侧：重建域源 pc 影线从
    同位继承升版为按节点 originalOffset 精确锚定（AirNode 置
    kFlagHasSourcePc，合成/新增节点不置位）——AIR 侧插入/删除/重排后，族
    载荷恢复与 lower 的 pcmap 锚都按身份对准；悬垂行收敛策略显式化（LNT
    起点向前吸附下一存活 pc、之后无存活裁行；LVT 端点向外吸附即跨被删段
    合并、尾部收敛 code 末尾），lower 仍 miss 即 Err，无静默错位；SourceFile
    等类级属性由会话所有天然保留。恒空断言翻新为真提升断言（含畸形行裁剪
    钉死）；新增 writer 元数据门五例（恒等重建逐位还原/插入平移/悬垂吸附/
    尾删裁行/重排精确互换）；真机恒等门补行级守卫、变更门补元数据逐位不动
    断言；语料门补覆盖面与重定向成功率计量（82 类全带 SourceFile、1066
    方法全带 LNT、LNT 2144 行/LVT 2273 行、带元数据类重读恒等 74/74）并立
    diverged==0 硬门。mjbase_unittest 1293/1291/2（批前 1286/1284/2，+7：
    builder 元数据用例 +2、writer 元数据门 +5）。挂账：注解与 ConstantValue
    的 AIR 面仍不转换（旧 TODO 口径不变）；originalOffset 为 u16，超 64K
    偏移截断沿旧口径未扩。
  - [ ] 批四：sir 真实 SSA→栈机 lower——已改隶 mjt-deobf（air/sir 出仓定案，2026-10-19），挂起不排 mjbase 波次。
- [x] Nest 族消费面迁移（2026-10-14，libjbc design §32.10）：libjbc 将
  NestHost/NestMembers/PermittedSubclasses 立为 typed 属性并收编 CP 压缩封闭
  集后，mjbase 消费面同批接线不留半接线态——member_remapper 三件改 typed 体
  手术（公开解码入口 + 命中才 intern + 置脏重编码，NestMembers/
  PermittedSubclasses 经类清单模板共用一条手术路径），Module 族保持 Raw；
  reference_index 类级引用扫描翻 typed 解码入口（Module 族 u2 表读取保留）；
  member_remap_plan_test 结构元数据断言从裸字节读改 typed 读；新增
  member_remap_jvm_test：nestmate + sealed 单文件样本四类改名落盘后 JVM 真跑
  （getNestHost/getNestMembers/getPermittedSubclasses/isSealed 反射钉死，
  JDK 缺 sealed 支持时显式跳过）。mjbase_unittest 1269/1267/2（批前
  1268/1266/2，+1 新用例）。挂账：Record 类属性 libjbc 仍未建模（B4 余量），
  Module 族 typed 面随 libjbc 后续批次。
- [x] Record 消费面迁移（2026-10-16，libjbc design §32.11）：libjbc 将
  Record 立为 typed 属性（AttrKind=17，组件带嵌套属性表）并收编 CP 压缩
  封闭集后，mjbase 消费面同批接线不留半接线态——member_remapper
  remapRecordAttr 翻 typed 体手术（组件名按字段/访问器规则择一、描述符走
  字段描述符映射面、组件嵌套 Signature/注解槽复用成员级同一条手术路径，
  命中才 intern + makeNew 置脏重编码），原 Raw 转型面退役；reference_index
  Record 读点翻 typed 解码入口（组件描述符 + 组件嵌套 Signature/注解同源
  收口）；member_remap_plan_test 结构元数据 Record 断言改 typed 读；新增
  member_remap_jvm_test record 真跑门：record 样本四类改名落盘后 JVM 真跑
  （isRecord/getRecordComponents/getName/getType/getGenericType 读新名、
  组件注解反射原样，JDK < 16 显式跳过）。mjbase_unittest 1275：1273/2
  （批前 1274：1272/2，+1 新用例）。MetaInfoRemover 对 record 组件嵌套
  注解面的清空接线已收口（挂账批：组件注解两族槽槽内清空 + 父 Record 槽
  连带置脏）。
- [x] Module 族消费面迁移（2026-10-16，libjbc design §32.12）：libjbc 将
  Module/ModulePackages/ModuleMainClass 立为 typed 属性（AttrKind
  18..20）并收编 CP 压缩封闭集后，mjbase 消费面同批接线不留半接线态——
  member_remapper 的 ModuleMainClass 单槽字节手术翻 typed 体手术
  （remapModuleMainClassAttr：公开解码入口 + 命中才 intern + makeNew 置
  脏重编码），Module 的 uses/provides 段翻 typed 体手术（requires/
  exports/opens 不触碰，与旧栈口径一致，原 Raw 转型面退役）；
  reference_index 的 ModuleMainClass/Module 读点翻 typed 解码入口（结构
  元数据收集面不变：mainClass + uses + provides 服务与实现），Raw u2 表
  读取面 readU2Vector 随之退役；member_remap_plan_test Module 族断言改
  typed 读。ModuleHashes/ModuleResolution/ModuleTarget（JDK 内部件，
  libjbc 不建模）无消费触点，不受影响。
- pass 分四批推进（module-map 后续批次节有拆法）。开工前置设施已落地：
  - [x] pass 名字注册面：`spi/pass_name_registry`（PassNameRegistry：name→factory
    + PassScope 四值标签（Method/Class/Global/Cf），重复注册 Err(DuplicateName)
    不覆盖，defaultPassRegistry 惰性种子注册三试点；装配序按 pass-contract §9
    留待第二个管线消费方，CLI -p 挂点属 cli 批）；
  - [x] 类级结构编辑原语：`spi/class_structure_editor`（reorderClassFields /
    reorderClassMethods 双面落盘 + backingIndex 全量同步，恒等排列零改动；
    成员增删经 libjbc A3/A4 通道真实现，错误族按库契约收口
    InvalidSeed/InvalidHandle/CpAppendFailed/Unsupported）；class_member_sorter
    已改为消费原语，手搓 reorderBy 删除、行为不变；
  - [x] structural_pass_util 升格共享 util（`test/unittest/shared/`，sorter 与
    结构编辑原语 / 注册面测试共用）。
  - [x] golden 对拍管道（轨道 B 首件）：旧栈侧提取器
    （mbase/unittest/pass/golden/，MJBASE_PASS_GOLDEN_REGEN=1 驱动，双规范形
    语料道 + golden 提取 + 不动点自检，再生成字节幂等）+ mjbase 侧逐字节对拍
    （test/unittest/pass/golden/，registry 按名构造，manifest 声明差异清单）。
    三试点全过：sorter 5/9 字节一致 + 4 声明（CLASSFILE_REENCODE）、tightener
    8/9 + 1 声明（TREE_SKIP_FRAMES_REENCODE）、cleaner 9/9 零声明。基线在
    `test_resource/pass_golden/`；管道说明与加新 pass 步骤见
    [doc_ai/pass-golden-pipeline.md](doc_ai/pass-golden-pipeline.md)。
  - [x] 方法体小批四件（nop_remover / redundant_load_val_eliminator /
    char_arr_to_str_simplifier / unused_check_cleaner）：spi MethodPass 契约
    落成 + registry 种子注册 + 单测 + golden 对拍收口（详见
    doc_ai/pass-golden-pipeline.md 结果表）；POP/POP2 折叠语义按 classfile
    可观测行为对齐（LDC2_W 装载的 cat2 参与 POP2 吸收）。
  - [x] 控制流首件 junk_code_inserter + 管线帧链接线：libjbc A1 链首个真实
    消费方（`PassPipelineOptions::typeResolver` 注入 → lower 前嵌套属性适配
    → Strict 帧重算 → maxes 采用 → SMT 再生挂回；`spi/frame_rewire` +
    `spi/strict_type_resolver`）；无旧栈对标走自基线道 + JVM 真跑验收；
    输出类经真机 verifier 执行与原类一致（2 样本）。libjbc 侧随修：栈上
    cat2 交付序与 SMT 条目空间对齐（值槽在前，frame 交付层归一）。
  - [x] F 线清洗批：illegal_class_file_cleaner 改两级流水——libjclassfile
    前置级（IllegalStrippingTransformer 全量规则面：属性级 CP 校验剔除 /
    越界跳转与 LDC 桩替 / LVT·LVTT·LNT 修剪 / BSM 压缩与 condy 池手术 /
    无效 MethodHandle 出池；中间层设计 §3.0 敌意输入前置清洗级裁决落地）
    → libjbc 级既有 2 条结构规则兜底（libjclassfile 读失败路径单独生效）。
    规则触发按字节比较，合法输入字节逐位不动；mjbase → libjclassfile
    依赖边随之开通（mjbase target add_deps）。旧栈清洗规则挂账清空；
    craft 畸形样本逐规则红→绿验收 + golden/proguard 冒烟不回归。
  - [x] 批 A 容器级直写组（module-map A 批，5 件全收口）：class_renamer 经
    会话 remapper 面（见提交历史）；未使用成员清理家族四件
    （empty_class_remover / unused_class_remover / unused_field_remover /
    global_unused_method_remover）经 mjbase reference 面
    （ReferenceIndex/CallGraph/ResolvedCallGraph/GlobalReachability）+ libjbc
    低层 DOM 成员 erase（成员按 name/desc 经 CP 引用、无槽位依赖）+ 两阶段
    提交（ClassResourcePendingCommit）落成；keep 根策略（module 描述符 /
    Manifest 入口键 / services 文件名与内容 / main / clinit / 非 private /
    keep metadata）与旧栈逐条对齐。golden 走新 um 道（专用 mini 语料 +
    services/Manifest keep 根，见 doc_ai/pass-golden-pipeline.md 结果表）；
    改写面 JVM 真跑门收口。普查砍单：meta_info_remover 家族（scope:class）
    归 B 批；tool_class_combiner 已迁成——宿主合并经 libjbc §34 adopt 面
    （字段 adoptField + ConstantValue 重建、方法 IR 重放、供体删除）+
    全容器 owner 重映射（reference 面）+ 分支体宿主帧再生（frame_rewire
    同一链路），两阶段提交作业级原子；ldc 宽常量候选按 NotModeled 口径
    保守保留原资源（旧栈照常合并，迁移声明的语义面差异，golden manifest
    声明类目收口）。原挂账「libjbc adoptMethod 分支体重放双重失败 +
    pass 侧规避层」已收口：libjbc 修复 CP 三读门（只认携带 CP 操作数的
    Invoke/FieldAccess/CpConst 三族）与前向分支占位标签（CodeDom 新增
    begin/endDeferredTargets 受控延后绑定构建态，重放收尾全量补锚定
    校验，正常编辑路径防呆不变），lookupswitch key 表随迁一并补齐；
    §34.3(2) 契约按可实现形态重写。本 pass 回收为 adoptMethod 直调
    （规避层整体删除），cc 道 golden 全绿零漂移 + JVM 真跑门照跑。
  - [x] 批 B 类级骨架组（module-map B 批收口）：meta_info_remover 家族类级
    13 件迁成——属性面落 backing 三层属性表（槽摘除 / 槽内过滤 /
    MethodParameters Raw 原位改写，JVMS u2 count 与旧栈 writer 遗留 u1 count
    布局双道），access 位双面落盘，Code 嵌套属性表有界破例两处
    （MetaInfoRemover 整槽摘除 debug 表与嵌套注解槽、IllegalTypeAnnotationRemover
    嵌套槽内过滤，父 Code 槽连带置脏）。tree 道 8 语料 golden 对拍（meta 语义
    矩阵口径：纯标志位触发类目允许与旧栈 golden 逐字节一致）+ JVM 真跑门
    （行为一致 + 反射按剥离后口径钉死）+ red-side 自检（13 件 run 一并钉死
  no-op → 翻红 → 复原 → 复绿）收口。能力差距挂账（见家族头注）已收口：
  record 组件注解清空 + SpecificAnnotationRemover 参数注解槽内过滤
  （Raw 面 JVMS 4.7.20 布局条目摘除）。普查砍单：家族 method 级八件为
  scope:method，归 C 方法级组；unused_member_remover 已由批 A 收口，不重迁。
  - [x] pass 余量：libjbc 侧挂账已全部解除（2026-10-28 台账校准）——
    成员增删 addMethod/addField（A3/A4）、整类构建 makeSession(NewClassSeed)
    （A5）、结构化泛型/TypeAnnotation 读写面 ir/sig_attr.hpp +
    ir/type_annotation_attr.hpp（B1/B2）均已在 libjbc 落地（units.hpp/
    ir 头实证）。结构化 attr pass 消费面迁移已落地（RFC 0002 B8 8.3，
    mjbase PR #73）：Signature 槽扫描式改写收口为 jbc::type::signature
    解析树 span 手术（嵌套链部件数不一致按 ASM SignatureRemapper 同语义
    坍缩扁平新名），TypeAnnotations/ParameterAnnotations/AnnotationDefault
    从不改写/Raw 手术迁 typed 结构化读写，class_renamer 开
    bytecodeAndMetadata；整槽摘除族保留字节面（无需结构决策）。类增删/
    重建类变换消费按需排期；方法级余量按 module-map 四批拆法继续。
- B7 设施请求：[x] renamer apply 面接线收口（引擎计划经 asm/remapper 域
  applyResourceRemapPlan+RemapPolicy 就地应用；apply 模式 E2E（javac 多类样本
  重读断言 + JVM 真跑门 + 反射读新名）与 remap_policy_matrix 应用面真语义用例
  落位，旧栈四件 apply 用例断言移植；类资源 URI 随动的双门口径差异已在
  module-map renamer 行声明）；AliasAnalysis 反射 receiver 闭包补「消费队列」
  形态（container 取出转手调用，当前 T13 类场景 fail-open——注意该缺口
  属 AliasAnalysis 接收者数据流建模：容器介导的 Class 流不在图上，与
  句柄解析下沉无关，两者是同条挂账的独立子项）；resolveHandleCp
  （句柄→owner/name/desc/kind）下沉已闭环（RFC 0002 B8 8.4）：评估结论
  下沉 jbc/core 纯池自由函数（mjbase 两处实现语义同构但输出形态分叉，
  reference 版为零拷贝 view+严格 kind 基准），libjbc 0.4.20 落
  cp_resolve.hpp（libjbc PR #14/#15），mjbase 侧 reference_index/
  member_remapper 两处换装 + cp_refs.hpp 转发头（mjbase PR #74；
  行为收紧：kind 域外从「误走方法分支」变「跳过」，readClass 读闸下
  畸形类不可达，remapper 侧污染接缝真红→绿用例钉死）。
- E 系消费者对接（module-map 第八批）：[x→退役] E3 类字节供给面 + 骨架扫描面
  （consumer/class_source + consumer/summary_scanner，14 用例全绿；
  2026-10-29 退役：全仓零消费方——mjt 从未 include，唯一消费面是自测；
  按仓规「没有消费方的模块不进仓」整体移除，IClassSource 协议待真实
  消费方出现时按需求重建）；
  [x] E2 ChangeSet 消费方接线（2026-10-14，libjbc design §35）：ReferenceIndex
  `applyChangeSet` 按 ChangeSet 局部重建（方法粒度精确补丁 + 字段/类级事实面
  重扫），`PassPipelineOptions::changeSink` 在 GlobalToClassPassAdaptor 类级
  提交点逐类交付变更台账；局部重建与全量重扫六表逐项等价（成员级混合批次/
  类级/真实 NopRemover 管线三层钉死，red-side 翻红入账）。挂账：消费方接线
  （产品线 promote 分流、CLI 批）出现真实需求时再排；ContainerClassSource
  的容器结构冻结前提靠借用契约约束（无运行期守卫），出现违约场景再评估是否
  引入代际校验。
- [x] StaticArrayFieldScalarizer 引用元素类型精化（#39 切片 3，既有件增强）：读窗后继 checkcast 目标与写窗常量值静态型（ldc String→java/lang/String、ldc Class→java/lang/Class，aconst_null 不产证据）作为该下标元素型证据；同下标证据不一致 → 整字段放弃。声明元素为 java/lang/Object 时标量字段按证据精化（L<证据类>;），无证据下标沿用声明元素描述符；证据位上的 checkcast 与新字段描述符一致（CCE 被写值面证明不可达）随读窗折叠，与声明元素型一致的证据不登记、checkcast 保持保留原位语义。声明元素非 Object 时证据必须与声明一致（无层级查询面 fail-closed）——顺带收口既有改写面会把 ldc Class 写入 String[] 产出验证错误描述符的潜在缺口。锚标签门补 castId 位；窄化声明证据门 fail-closed；6 用例钉死精化/冲突放弃/Class 精化/写读证据冲突/窄化声明放弃/逐下标独立。全量 2000 绿。
- [x] #17 收口审计与 golden 面补齐：三件 simplifier（expression / fix_pattern / type_cast）本体已于 8b98945 迁入并经 #44 扩族（规则 12-20），规则面与验收七面（位模式核心 / 六 IF_ICMP* 安全折叠与非常量 0 保留 / hash 碰撞保留 / 数组分配 pop 保持 / 同 slot 重写 / 异 slot 保持 / roundtrip）逐面核对既有单测全覆盖。本次补齐验收的 golden 语料面：新增 corpus/smp 道（hb_fixpat/hb_expr/hb_cast 三语料，CodeDom 生成器内联在 simplifier_self_baseline_test，确定性构建 + write∘read 不动点自检），三件 pass × 三语料 golden_self 自基线对拍（MJBASE_PASS_GOLDEN_REGEN=1 再生成；旧栈无可取基线——fix_pattern 规则集已分叉，走 JunkCodeInserter 同款自基线纪律）；同名义语料钉死必有改写面（防空转 pin），跨语料 copy-through 钉死「不命中形状字节原样」。构建门全量 2004 绿。
- window_rule 替换组合下沉裁决（2026-10-29）：replaceWindow **不下沉 libjbc**——库边界守「单类读写与分析底座」，窗口替换组合协议留 mjbase/pass/window_rule；后续任何下沉提案一律先证「≥2 独立消费方或跨仓契约需求」再议，无消费方不扩库。
- [x] DeadSideEffectFreeCallRemover 异常表精细化（帧面批次收尾件，既有件增强）：
  摘「含异常表整件跳过」——弹值消费只在正常流，异常抛出点栈清空，handler
  入口帧由帧引擎合成为 [exceptionRef] 且无指令定义，以其为种子的窗口在
  closeChain 处不可入链（负向结论不受异常表影响），整件跳过属过度保守
  （真实类大半带异常表，跳过使白名单链删除在多数方法上失效）。链区间
  的异常表语义由两道门保证：区间标签门既有覆盖（try 端点/handler 入口
  锚定在 pop 或非头链成员即拒收窗口——标签锚定检查本就覆盖 Boundary
  角色）+ 应用面 eraseBatch 前置校验（链头被 exc 边界锚定的形态，编辑器
  在任何变更之前整组 StillUsed 拒绝——组级原子，替代原逐条 erase 的
  半删风险面；被拒窗口保持原样，其余窗口继续应用）。4 单测（try 外链
  删除、try 内链删除（端点锚删除面外存活指令）、边界锚 pop 区间门拒收、
  边界锚链头 eraseBatch 拒收），替换原「异常表跳过」用例。全量 2007 绿
  （2004 过 + 3 既有跳过）。
- [x] FlatteningReverser 相对状态更新收口账目补记（2026-10-28 落地
  32aa2c7，本条为账目翻账）：前驱状态写不再限于绝对常量——新增相对更
  新形态 iinc s,D（s+=D 紧凑形）与 [iload s; intConst D; iadd/isub/ixor;
  istore s]（IADD/IXOR 常量在左镜像同值同认；ISUB 的 D 在左是 D-s 形
  fail-closed）。相对写点当前值由引擎级状态迹模拟求解：案表目标（含
  default）直线供源扫描到首个状态写点，入口锚取不被案表供源的唯一绝对
  写点，沿 fold(s) → 案表 → 供源写点追踪定值，环闭合即停；同写点双值
  （路径敏感）、迹终端点、多入口锚拒收。FR 挂账余项收敛为混合操作数链。
- [x] StringConcatIndirector（P1 批首件，新增件）：java/lang/StringBuilder
  构造 + append 链 + toString 的直线形态改写单条 invokedynamic
  StringConcatFactory.makeConcatWithConstants（javac 9+ 拼接规范形）。
  窗识别逐指令相邻：NEW(SB); DUP; (LDC String)?; INVOKESPECIAL <init>
  (()V|(Ljava/lang/String;)V); (ARG; INVOKEVIRTUAL append(恰一参))+;
  INVOKEVIRTUAL toString——INDY 落位在 toString 位（栈机按序执行，实参
  指令保留原位先压栈，new/dup/init/append 面经 eraseBatch 组级原子整擦
  后栈序恰好成立；INDY 落 NEW 位是栈序反的）。实参认领面 v1 fail-closed：
  每 append 实参须为恰一条自描述型单指令（ICONST 族/BIPUSH/SIPUSH/
  LDC-Int→I、LCONST 族/LDC2_W→J、FCONST 族→F、DCONST 族→D、LDC
  String/Class/MethodType/MethodHandle→对应引用型、ACONST_NULL→Object、
  原始型局部装载→自述型；ALOAD/计算形/condy 不认领，整窗拒收）——帧
  面计算形实参挂账。安全门：类版本 ≥53（StringConcatFactory 入场线）；
  窗内任何成员（擦除面 + 实参位）锚定存活标签（分支/异常表）整窗拒收；
  异类 NEW（StringBuffer）不动。BSM 条目经 appendBootstrapMethod 同形
  复用（配方 = ×N 全实参槽位形态，常量嵌入属后续精化）；indy CP
  条目走底层池面 addInvokeDynamic（session.internDynamic 是 condy tag 17
  语义，v53 类 JVM 拒载——indy 是 tag 18）。maxes 面：改写后窗体最深点
  = 窗口入口栈深 + 全部实参槽宽（帧引擎 BestEffort 一次；javac 可给出
  比 SB 新形更小的 maxStack 且管线不重算，不抬终类 VerifyError 栈溢出
  ——真跑钉死）。9 单测（javac 形折叠/无参链/同形两窗 BSM 复用/计算实
  参拒收/窗内标签拒收/异类 NEW 不动/版本门不动/幂等写回/注册面）+
  1 JVM 真跑门（javac --release 8 编译 SB 链样本，v52 门负证 + 抬 major
  过门折叠，变换前后真机执行 stdout 逐字节一致且走 StringConcatFactory
  bootstrap）。全量 2017 绿（2014 过 + 3 既有跳过）。
- [x] UncontendedMonitorEliminater（P1 批次件，新增件）：方法内受限监视
  器对整组摘除——混淆器注入的 junk synchronized 块无害化。识别形逐指令
  相邻：[NEW X; DUP; X.<init>()V; ASTORE L] … [ALOAD L; MONITORENTER]
  … [ALOAD L; MONITOREXIT]，整组 8 指令 eraseBatch 组级原子摘除。组按
  定义圈定而非按槽位（ASTORE 定义的 Local 使用边恰 2 且都是监控接收
  者、读点到达定义恰为该唯一定义、use 全集恰为两读两监控）——同一槽
  位被多组先后复用时各组独立成组逐一摘除。安全门全有全无 fail-closed：
  <init> 须 ()V（带实参形态的实参指令在窗外，擦除后栈失衡拒收，带实参
  精化挂账）；组成员或 enter..exit 区间内部锚定存活标签（分支/异常表）
  整组拒收（跳入体内绕过 enter 的路径原类触发 IMSE，摘除即语义变化）；
  监控点栈定义边须全部汇于本 ASTORE；方法参数接收者（def = Parameter）
  天然失配；analyzeDefUse 失败/截断整方法放弃。语义论证：对象身份不出
  方法，锁状态对他线程不可观测，单线程 monitor 簿记（重入计数/持锁态）
  亦不可观测；NEW 非空无 NPE。maxStack/maxLocals 不变。9 单测（junk 摘
  除/参数接收者拒收/逃逸读拒收/体内标签拒收/带参 init 拒收/不成对拒收/
  同槽两组双摘/幂等写回/注册面），全量 2023 绿（2023 过 + 3 既有跳过）。
- [x] UncontendedMonitorEliminater 带参 <init> 精化（收口上条挂账）：
  识别形放宽 [NEW; DUP; (ARG)*; INVOKESPECIAL X.<init>(...)V; ASTORE
  L]——实参游程逐条认领无副作用指令、可整组擦除（ICONST/BIPUSH/
  SIPUSH/FCONST/DCONST 常量推入、ACONST_NULL、LDC/LDC2_W 非 condy
  （bootstrap 可抛不认领）、局部装载族含紧凑形；ALOAD 引用装载无类型
  约束可认领）；游程首个不认领指令须为本类 <init>（desc 不限），否则
  整头拒收——调用/字段访问/分配/dup 形/iinc 实参擦除即语义变化
  fail-closed。实参入组统一过标签锚定门与组级原子 eraseBatch（擦除面
  只降栈深，maxStack 不抬即安全）。3 单测新增（副作用实参拒收/常量双
  实参摘除/装载实参摘除），全量 2025 绿（2025 过 + 3 既有跳过）。
- [x] CseChainEliminater（P1 批末件 GVN/CSE 第一切片，新增件）：同一
  直线段内两条结构全同的纯计算链先后写同一局部槽，第二条链连同其存
  储整组摘除——首链已把同值写入槽内，重算不可观测。链自存储点反向
  需值抽取（cat2 记一个值）：局部装载（记录槽号）/常量推入（LDC/
  LDC2_W 非 condy，记录 CP 槽）/二元纯算术与比较（2→1）/取负与转换
  （1→1）；调用/字段/数组/栈操作/控制流不入链，遇链外指令（含任何
  存储）整对拒收（外层栈供源挂账）。链结构全同 = 逐条同操作码、装
  载同槽号、LDC 同 CP 条目。安全门 fail-closed：区间 [链1 头..链2 存
  储) 无标签锚定无控制流（同一基本块直线段，链1 必然先执行完毕）；
  区间内写入面仅允许链1 存储点触目标槽，任何触操作数槽即拒收（iinc
  同存储计）——链为操作数纯函数，写入即失证；IDIV/LDIV/IREM/LREM
  入链：首链同值已完成求值，除零异常若可能已在首链抛出，重算异常面
  不可达。就近取同链：区间门失败即放弃该链 2（更早同值链区间更宽只
  会撞同一批破坏点）；claimed 集防组交叠。删除面只降栈深。11 单测
  （装载链/计算链/除法链摘除、操作数槽覆写/目标槽覆写/异链/区间分支
  标签拒收、跨调用区间摘除、三链保守首轮单对、幂等写回、注册面），
  全量 2036 绿（2036 过 + 3 既有跳过）。后续精化挂账：外层栈供源链
  形、getstatic 纯面（需字段写推断）、跨基本块支配关系泛化。
- [x] FlatteningReverser 混合操作数链收口（FR 挂账末项）：分发操作数
  解析从「形态 A/B 两分支各自整链一致」泛化为**通用线性链**——自
  switch 向上需值游走（binop 需求 +1、常量/装载供给 1，需求归零即链
  首），链为自包含表达式，推值与 binop 任意交错；常量全在右（A）、全
  在左（B 镜像）与混合操作数链（装载居中、部分步常量在左部分在右）
  同族识别，语义由小栈模拟统一求值（代码序弹栈、深者为左，u32 环绕）。
  装载恰一；需求未尽遇链外指令整族拒收 fail-closed；零步裸装载保持合
  法。默认规格 maxSteps=1 语义不变，两步混合链经注入 maxSteps=2 规格
  覆盖。相对状态更新形态（写点侧）不动。原「混合链拒收」用例翻转为
  正面解析，另加双常量夹装载混合链（(C2-K)^C1）正面；FR 两套件 22 绿，
  全量 2037 绿（2037 过 + 3 既有跳过）。
- [x] StringConcatIndirector 帧面计算形实参收口（上上条挂账）：append
  实参认领从「恰一条自描述型单指令」升版为**自包含纯计算链**前向贪
  心认领（验证器保证链为自包含表达式、净效应恰 +1 值且 append 紧随）——
  常量推入（condy 不认领）/原始局部装载族（ALOAD 静态型未知不入链）/
  二元纯算术移位位比较/取负与数值转换；栈操作/调用/字段/数组/分配不
  入链，binop 需两值在栈、收尾深度恰 1（形状不闭合整窗拒收 fail-closed）。
  indy 实参静态型取 append 形参描述符（链顶值匹配由原方法验证器保证，
  免链型代数）；链指令保留原位——改写后在更低栈深执行（SB 接收者不
  再驻栈），原 maxStack 天然覆盖链内峰值，INDY 汇合点 = 入口栈深 + 全
  部实参槽宽（帧引擎抬升兜底不变）。单指令实参为链长 1 特例，既有用
  例语义不变。原「计算形实参不认领」用例翻转正面，另加 long 链（J 槽
  宽）/单指令与链混合双 append/栈操作实参拒收三面；SB 套件 12 绿，全
  量 2040 绿（2040 过 + 3 既有跳过）。
- [x] libjbc 0.4.23 收编：内联包定义追加 0.4.22/0.4.23 条目（既有条目
  只追加不修改），消费 pin 0.4.21→0.4.23。0.4.22 行为面 = 精确档异常边
  发射域放宽到范围内可达指令（JVM 保守口径）——try 范围内无可抛指令
  不再剪 handler，def_use 死 try 范围用例对齐：handler 的 local 读保
  留且定义归 try 前 store（原「handler 不可达仅正常路径读」钉的是旧窄
  口径，翻面更名 ConservativeSeed）；0.4.23 = 仓库瘦身 + 类构建挂载面
  测试收编，纯文档/测试面无 API 变化。全量 2043 用例 2040 绿（+3 既有
  跳过）。
- [x] asm/evaluator 退役：产品面零消费（唯一消费面是自测 30 例与
  strength_reducer_test 差分求值），libjsim（独立仓，框架完备路线：
  202 opcode 默认 handler + 接管面 + 结构化 Bail）接位。三条差分矩阵
  换「发射形状逐操作数读取 + C++ 位精确模拟」——k/mask/符号移位量
  从重写后字节读出，按 JVM 环绕与算术移位语义重放发射序列，对照原始
  体闭式语义（u32 wrap 乘 / 向零取整除）；差分求值语料待 libjsim 落
  包后回归其一致性套件。全量 2013 用例 2010 绿（+3 既有跳过）。
- 层次图 `.ihc` 序列化：暂不迁，出现真实消费方再按 libjbc 会话模型重设计。
- [x] 旧 matcher 退役、StackOperationSimplifier 迁 tmatcher 载体（合并裁
  决兑现）：asm/matcher 全族 15 文件（pattern_matcher / match_step /
  mini_dsl 三层）与两套件删除，唯一产品消费方 stack_operation 改为
  ReplaceRule 组合子 + InsnPlanner 计划面——31 条规则（10 消除 + 21 改
  写）迭代到不动点，计划自右向左落盘，执行期纪律门（首条豁免）按窗口
  粒度拒收锚定窗。偏离申报：①胜出纪律「规则注册序优先 + 窗内倒序首
  中」→「位置优先 + Longest 不重叠」，修复 `[装载;装载;pop2]` 上短窗
  抢吃留死装载（栈面 +1）缺陷，双装载整组消费翻正面用例钉死；②旧表
  `[dup_x1;swap]=>[dup]` 栈面不等价，整条退役（`[a,b]` 上 LHS 终形
  `[b,b,a]` 顶为 a、RHS 终形 `[a,b,b]` 顶为 b，非死值尾部可观测，旧夹
  具 return 死值尾遮蔽差异），`dup_x1` 面保留 sound 的双装载两窗；③异
  常表锚定窗从整方法保守终止改为窗口粒度跳过（原挂账兑现）；④套件夹
  具全量换可验证字节码 jasm 文本汇编（iconst/lconst 专用形，异常表面
  ClassBytesBuilder 标签装配），pop2/swap 落点深度与返回栈深不再依赖
  汇编器宽放——不可验证栈形（如裸 `装载;pop2`）不再作为夹具。stack 套
  件 19 绿，全量 1973 用例 1970 绿（+3 既有跳过）。
- [x] workspace 退役（重组方案作废，改整体移交）：原重组挂账（资源容器
  服务 + 分析项目会话两面）评审否决——裁决是 workspace 概念不留在基座：
  WorkspaceBase 单 class 66 个虚方法把容器形态绑死在 class/JAR 视图，
  model::ResourceContainer 被私有成员封死，dex/apk 容器形态无插入位；
  「资源容器 + 分析项目」的正确分层属下游消费方自理，基座不预设。处置：
  引擎 12 源文件 + 5 测试文件整体移交 mjt（ns mjbase::workspace →
  mjt::workspace，兄弟域经别名保持正文零改动），本仓模块目录、测试
  目录与构建 glob 全量删除；基座内零消费方（实测无 include），下游
  mjt 侧 66 虚方法消费面随迁不受影响。
- [x] spi⇄pass 解环：环只在 spi/pass_name_registry.cpp 与
  spi/pass_registry.cpp 两 .cpp 的组合根（种子段 52 个 pass 头 + 内置
  管线段 11 个 pass 头）。整段迁 pass/default_pass_registry.cpp 与
  pass/builtin_pipelines.cpp，接口声明留在 spi 原位——消费面 include
  与符号链接零改动。spi/*.cpp 对 mjbase/pass/* 的 include 计数 = 0。
- [x] asm⇄analysis⇄reference 解环：两处归位 + 依赖方向回正——
  ①resolved_call_graph（含 RTA 包装）整迁 reference/（它零依 asm 面，
  纯分析组合件住错地），ns asm_classpath→reference，RCG 的 CallGraph
  依赖保持 asm_classpath 前向声明引用；②alias_analysis 整迁
  asm/analyzer/（frame_query 变同模块，ns analysis→asm_analyzer），其对
  andersen/class_scan/cp_refs 的消费转为 asm⇒analysis 单向；③remapper
  对 reflection_interpreter 的反射改写面消费保留为 asm⇒reference 单向。
  终态零环：analysis⇏asm、spi⇏pass，全部跨模块边单向
  （asm⇒{analysis,reference}、reference⇒asm、pass⇒spi）。
