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
- [ ] P0-3 可配置无用调用删除（RemoveMethodCallsHandler 等价物）：
  scope:global，CLI 白名单供调用点，删除后同步调用图边；研究已派。
- [ ] P0-4 非私有成员删除 + 调用修复：GlobalUnusedMethodRemover 扩展到
  package-private/protected 死方法，删除后 invoke 指令与调用图边修复；
  跨 JAR 引用不可见时 fail-closed。研究已派。
- [ ] P0-5 实例化类保守根：RTA 式按 `new` 点位收窄可达类集合，喂
  UnusedClassRemover 的 rootClasses；与现有 ResolvedCallGraph 的关系待定。
- [ ] P0-6 多版本 JAR（Multi-Release）：META-INF/versions/N/ 条目版本分组，
  删除/重命名不得跨版本误伤。研究已派。
- [x] P1 强度削减与常量折叠已落地并注册种子：StrengthReducer（2^k 乘/除 →
  移位/移位-加法序列，序列峰值 maxStack 按宽度抬升 int +1 / long +2，
  maxStack ≥ 0xFFFD 预检不改写）、ConstantFolder（栈常量链 30 条指令，
  除零与 int/long MIN÷-1 红线保守不折）；余下 P1（GVN/CSE、去虚拟化、
  同步消除、StringBuilder 链化简）待批。
- [x] B 批 meta_info_remover 家族迁移（scope:class）：类级 13 件已收口 +
  两件挂账收口（record 组件注解清空 / 参数注解槽内过滤）。

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
- [ ] **FixPatternSimplifier**（236 行）：位运算固定模式 / 六种 `IF_ICMP*`
  安全折叠（非零偏移保留）/ 同 slot store/load 重写（clone producer）；
  matcher 底座已迁。样板批次选。
- [ ] ExpressionSimplifier（409 行）：表达式形状折叠。
- [ ] TypeCastSimplifier（328 行）：冗余 checkcast 消除，需类型分析面。
- [ ] ConsJumpOptimizer / ConstantSwitchFolder / ConstantStateMachineEliminator
  （628 行）：常量分支/switch/状态机折叠，依赖 asm/evaluator（已迁）。
- [ ] RedundantJumpRemover / GotoRearranger / ContinuousLabelMerger
  （449 行）：label 布局族——新载体无标签节点，逐件载体重论证（部分可能
  结构性吸收或改形为边界句柄重排），未论证前不迁。
- [ ] UnreachableInsnRemover（dead_code_remover.cpp 1594 行内非 NopRemover
  部分）：帧可达性证明删不可达指令；mjbase cfg/evaluator 面齐，最大单件，
  独立批。NopRemover 已由方法级四件批收口。
- [ ] StaticFieldDefaultInliner（121 行，scope:class）：clinit 单写常量 →
  字段 ConstantValue（backing 槽）+ 等价初始化删除；类级载体重定型首件
  候选（双面落盘纪律）。
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
  - [ ] pass 余量：类增删/重建类变换（挂账 libjbc A3/A4/A5：成员增删、
    整类构建）与结构化泛型/TypeAnnotation 读写 pass（挂账 libjbc B1/B2）
    暂不迁；方法级余量按 module-map 四批拆法继续。
- B7 设施请求：[x] renamer apply 面接线收口（引擎计划经 asm/remapper 域
  applyResourceRemapPlan+RemapPolicy 就地应用；apply 模式 E2E（javac 多类样本
  重读断言 + JVM 真跑门 + 反射读新名）与 remap_policy_matrix 应用面真语义用例
  落位，旧栈四件 apply 用例断言移植；类资源 URI 随动的双门口径差异已在
  module-map renamer 行声明）；AliasAnalysis 反射 receiver 闭包补「消费队列」
  形态（container 取出转手调用，当前 T13 类场景 fail-open）；resolveHandleCp
  （句柄→owner/name/desc/kind）评估下沉 ClassSession。
- E 系消费者对接（module-map 第八批）：[x] E3 类字节供给面 + 骨架扫描面
  （consumer/class_source + consumer/summary_scanner，14 用例全绿）；
  [x] E2 ChangeSet 消费方接线（2026-10-14，libjbc design §35）：ReferenceIndex
  `applyChangeSet` 按 ChangeSet 局部重建（方法粒度精确补丁 + 字段/类级事实面
  重扫），`PassPipelineOptions::changeSink` 在 GlobalToClassPassAdaptor 类级
  提交点逐类交付变更台账；局部重建与全量重扫六表逐项等价（成员级混合批次/
  类级/真实 NopRemover 管线三层钉死，red-side 翻红入账）。挂账：消费方接线
  （产品线 promote 分流、CLI 批）出现真实需求时再排；ContainerClassSource
  的容器结构冻结前提靠借用契约约束（无运行期守卫），出现违约场景再评估是否
  引入代际校验。
- 旧 matcher 与 tmatcher 的合并裁决（集成期）。
- 层次图 `.ihc` 序列化：暂不迁，出现真实消费方再按 libjbc 会话模型重设计。
