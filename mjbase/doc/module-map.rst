.. _module-map:

mjbase 模块地图
===============

以 mbase（\ ``../mbase/src/mbase/``\ ，约 368 文件 / 8.8 万行）为语义来源，在 libjbc 底座上逐模块重写。状态：✅ 已迁 ｜ 🔨 迁移中 ｜ ⬜ 未开始 ｜➖ 有意不迁（理由注明）。

地基（主线持有，模块 agent 禁改）
---------------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源
     - 说明
   * - keys
     - ✅
     - libjbc design.md §9（新设计）
     - KeyPool 驻留池 + ClassKey/MemberKey/MemberKeyHash；跨类身份原语
   * - util
     - ✅
     - mbase/util/class_name_util
     - 内部名/描述符纯视图函数 + isLibraryClass

第一批（asm 域优先 + 其直接前置）
---------------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源（mbase）
     - 迁移范围
     - 底座映射
   * - asm/cfg
     - ✅
     - asm/cfg
     - 基本块、CFG 后继/前驱/异常边、迭代支配树、可达集
     - CodeEditor::blocks + resolveLabel + CodeUnit::exc
   * - asm/insn
     - ✅
     - asm/insn
     - opcode 分类谓词、栈效应表、指令工厂
     - jbc::ir::Insn / CodeEditor 工厂
   * - asm/analyzer
     - ✅
     - asm/analyzer
     - 常量域、来源域、可空性事实域、跳转走向事实（branch_facts）、跨路径整数约束事实（path_conditions：分支谓词贴边、must 交集、死分支修剪，见 path-conditions 头注）、字符串/常量流事实（string_flow：来源、逃逸、Base64/XOR/char 偏移管线）+ 入参常量种子 + slot 级 def-use（有界到达定义集、异常边、字段访问事实，见 slot-def-use 头注）
     - jbc::analysis::DomainEngine + IValueDomain；引擎缺口挂账：分支条件钩子、CHECKCAST 族 operand-aware 钩子；string_flow 设计见 string-flow 头注
   * - asm/tmatcher
     - ✅
     - asm/tmatcher
     - 新代匹配 DSL：combinator、capture、替换规则骨架
     - CodeDom::order 窗口匹配
   * - asm/matcher
     - ✅
     - asm/matcher
     - 旧代 pattern_matcher 语义
     - 同上
   * - asm/evaluator
     - ✅
     - asm/evaluator
     - 具体求值栈机（数值子集 + 分支 + 步数熔断）
     - CodeDom 只读遍历
   * - asm/classpath
     - ✅
     - asm/classpath
     - 调用边提取（MemberKey 键）、可达、库类过滤、indy/MethodHandle 解析与 BSM 展开边、VarHandle 读写事实、惰性调用链枚举 + 调用树视图（简单路径/环叶/去重号/节点上限）
     - promote + memberOf + keys + BootstrapMethodsAttr
   * - asm/stmt
     - ✅
     - 新设计（issue #5）
     - 保守结构化语句层：块级 if/while/for/do-while、语句↔指令窗口双映射、叶子整窗写回
     - asm_cfg 快照 + CodeEditor insert/eraseBatch；见 statement-ir 头注
   * - asm/sem
     - ✅
     - 新设计（issue #115）
     - 语义设施层首批只读视图（A1）：FrameView（Strict 逐指令到达帧 + MethodSig 播种 + ITypeResolver 注入面 + UninitThis 探针）、SlotProvenanceView（栈/局部槽到达定义查询 + def→use 边（DUP 族按「复制槽不产定义、产使用边」补边）+ 切面槽干净判据）、RegionView（SESE 候选区域枚举：入口支配集 ∩ 出口锚后支配 + 单入口单出口过滤 + 异常同质/直线/jsr 旗标）；最小 outline（A2）：OutlinePlan 处方（纯数据 + validate/apply 分离）——直线区域判据识别（planOutline 消费三视图）+ 落地（建合成 private static synthetic 方法 + invokestatic 调用点替换 + LNT/LVT 行拆分迁移 + 两侧 lower/SMT 再生/maxes 采用）；A3：LivenessView（局部槽后向 may-liveness + 出口活跃集，异常 handler 边并入，写后杀/回边收敛）+ copy-set 最小化（判据 3 放开：写回集 = 被写 ∩ 出口后活跃须空；def-use 恒等搬运剔除「先写后读/恒被重写」槽，槽重编号覆盖读写全集）+ §3.4 DUP 规范化（tier-2：边界 DUP 原值升格 Stack 入参 + 调用点 DUP 复位插回原位；tier-3：dup_x1/SWAP 族跨切点结构性拒绝）+ goto 入边参数装载随入边走（A2 VerifyError 缺陷修复，标签锚定首条装载）+ LVT 越切点宽松开关（validate 默认拒绝，applier 显式裁剪档记 lvtDrops），JVM 真跑行为对拍门；A4：merge/inline 对偶（planInline 识别 outline$ 合成方法调用点 → MergePlan::apply 回迁体 + 调用点装载链剥离 + 复位 DUP 消解 + LNT/LVT 行逆搬运 + N 删除），fail-closed 识别码 7 枚（NoInlineCandidate/NotOutlineSynthetic/CalleeNotStraightLine/CalleeTryCovered/CallSiteCountUnsupported/CallSiteShapeImpure/CalleeShapeImpure），往返语义等价门（JVM 实跑对拍 + 指令序列规范化对拍，非字节级；自足槽 fresh 分配为已知不可恢复项）；A5：插入原语独立化（insert_prim 设施：标签重锚批量插入 insertAnchoredAtLabel——goto 入边首条装载锚定的承载原语 + fallthrough 批量插入 insertBeforeInsn + copy-set 前导装载 copySetLoadPreamble（描述符落局部/栈序列消费）+ DUP 复位发射 entryDupResetInsn（§3.4 tier-2 调用点侧栈形重排）+ 值形态原语（descSlots/ValueFamily/loadOpOf/returnOpOf/explicitLocalOp/localOperandSlot/localOpSlotWidth/isControlTransfer）——A4 挂账的 outline/merge 逐字重复辅助随批上收，行为逐位不变以 A2–A4 既有测试为等价门，原语直接单测不借 outline 间接测）；A5b：outline 侧盖章（自足槽 exact 恢复——planOutline 按体首现序记自足槽 (nSlot, parentSlot) 盖章行（selfStamps），apply 落 N 的 Code 嵌套 MjOutlineStamp 属性（RawAttr 承载 u16 计数 + 对序列；JVMS 4.7.1 未知嵌套属性 JVM 忽略面 + decodeAttr 兜底 RawAttr），planInline 识别侧嵌套属性白名单增盖章档、槽逆映射消费后自足槽原槽号 exact 对回，无章回落 fresh（A4 口径，外来 inline 形态兼容），半截/重复章 fail-closed；往返断言升级「原文槽号精确对回」，去 A4「fresh 槽撞对」已知形态豁免 + JVM 真跑盖章门）；A6：区域挑选策略（策略层首批——设施先行策略后加，本批起策略归编排层纪律落地）：region_select 策略接口（纯数据 + 纯函数值语义：RegionSelectRequest 候选集 + RegionView/FrameView/LivenessView 事实面全部指针可缺省（缺哪面跳过哪层预检）→ RegionDecision 挑选决策 + 优先序（批量驱动复用，A7 方向）+ RegionSelectNote 全拒便签）+ v1 默认保守策略 selectRegion（事实面预检与 planOutline v1 判据同向：方法级 jsr（判据 5）/ 候选级直线旗标（判据 1）/ 异常同质且 handler 集空（判据 4 A2 收紧）/ 末条指令 goto（v1 出口形态）/ UninitThis 存活（判据 6）/ 写回集为空（判据 3：store/iinc 写槽 ∩ 出口锚后活跃）；规模次之排序（成员数升序最小切口、首成员块号破平，rankRegions 纯面供批量驱动复用）+ 冲突保守拒绝（等尺寸部分重叠对全拒；嵌套/重复不冲突——真实枚举两候选恒不相交或嵌套，防御档）+ 准入探针（planOutline 识别包成探针按优先序逐个探测，outlineWhere/runJvmGateWhere 枚举形态的策略化）；插拔 = RegionSelectPolicy 纯数据档位 + RegionSelectFn 策略函数形状，v2 起按消费方加策略不加核心；planOutline/merge 既有行为零改动，策略级 JVM 真跑端到端小门）；A7：区域批量驱动闭环（outline_driver——策略选区到连续 outline 的闭环，驱动器为新消费方不改旧设施）：driveOutlines 逐轮 selectRegion → planOutline → apply → 从 session 重取方法体重建 FrameView/RegionView/LivenessView → 再选（apply 之间必须重枚举，失效戳语义禁复用旧视图），把 planOutline 识别包成准入探针注入 selectRegion（A6 预留的消费形态，选中候选恒可交付识别）；输入 = session+MethodId+签名 + RegionSelectPolicy/RegionSelectFn 档位 + maxRounds 迭代上限（显式参数缺省保守防组合爆炸）+ 命名序号起点；输出 = 已 apply 轮次序列（每轮候选数/选中/成员数/策略便签/OutlineOutcome）+ 终止原因便签（候选耗尽/迭代上限/策略全拒/识别失败/落地失败/视图重建失败/父方法不可用）；保守门 fail-closed：单轮失败即停，已 apply 轮次不回滚（逐轮独立合法，同 OutlinePlan::apply 口径）；已知形态注记：v1 策略面存在「接力候选」——apply 后调用点块（装载+invokestatic+转发 goto）自身仍满足预检与出口形，干净臂语料每轮可再切一层，上限是唯一护栏（行为保持由 JVM 门多方法连续 outline 对拍钉死）；单测逐面钉死（零候选收敛/单候选切层/嵌套梯内层优先多轮/上限 0 语义/判据 6 与判据 5 全拒）+ javac 语料 4 轮连续 outline -Xverify:all 行为对拍门；A8：策略 v2 排除档——批量驱动收敛（RegionSelectPolicy 增 excludeOutlineRelay 档位，缺省 false = v1 不排除、A7 接力现状保持：排除「合成接力块」候选——apply 调用点块形态（复位 DUP/局部装载 + lone invokestatic + 转发 goto），判定事实与 planInline 的 NotOutlineSynthetic 同源（同类 + outline$ 名前缀 + private
     - static
   * - hierarchy
     - ✅
     - hierarchy
     - 类/方法/字段层次内存图、ancestors/overrides（.ihc 序列化不迁）
     - 会话扫描 this/super/interfaces + keys

第二批（资源与映射叶子，直迁）
------------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源（mbase）
     - 迁移范围
     - 说明
   * - jar
     - ✅
     - jar
     - jar_reader/jar_writer/manifest/jar_util + dos_time_util（随迁叶）
     - manifest 保序字节级输出契约保留；throw 改 Result<\_,JarError>
   * - model
     - ✅
     - model
     - resource_model/container/io/helper
     - 解包面不随迁（mjbase 全家零调用方，能力随需求下沉消费方）；throw 改 ModelError 枚举；路径统一 u8path
   * - mapping
     - ✅
     - mapping
     - 全 16 文件（7 格式注册表 + TinyV2 + shadow-alias + identifier\_*）
     - 格式输出字节级等价（怪异行为原样）；invalid_mapping_exception 删除改 MappingError
   * - project
     - ✅
     - project
     - analysis_project（schema v1/v2 登记解析）+ project_store（事务/锁/恢复/section 引擎）；新设计部分：edit_ledger / edit_ledger_store（编辑台账行模型与存储）+ analysis_cache（xref/decompile 缓存 section 落盘），设计见 :ref:`schema-v2`、:ref:`container-cache`
     - 契约值原封（schema/kind 串/sha256 形状/engine 自述串/锁语义）；平台分支照搬；analysis_db 与出口遮蔽面见第九批

第三批（叶子与 CLI/JASM 面）
----------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源（mbase）
     - 迁移范围
     - 说明
   * - expr
     - ✅
     - expr
     - 全 18 文件（树/解析/求值/化简/MBA 生成）
     - 180 用例全移植；对 hierarchy/model 零依赖属实
   * - log
     - ✅
     - log
     - log_init 门面
     - CA_LOG 门面接 libca_log
   * - i18n
     - ✅
     - i18n.hpp
     - 兜底入口
     - key 空间保持 mbase.\* 字面前缀（.lang 逐字节照搬纪律）
   * - util 叶子
     - ✅
     - util/pair,span,triple,time_util
     - util/ 子目录
     - class_name_util、dos_time_util 前批复迁
   * - cli
     - ✅
     - cli
     - ArgParser 框架 + i18n embed
     - 2 个 globalPipeline 用例待 spi/pass 批接线后恢复
   * - jasm
     - ✅
     - jasm
     - JVM/资源两级服务
     - GAM 依赖按无缓存逐调用重建重设计；jbytecode/jdasm 脱钩

第四批（analysis 域化 + pass 契约重定型）
-----------------------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源（mbase）
     - 迁移范围
     - 说明
   * - analysis
     - ✅
     - analysis
     - alias（域插件+两跑）/andersen/exception_flow/method_effect/method_frame
     - 反射身份契约逐字保留；摘要键改 MemberKey（aliasMemberKey 建键）；method_frame 驱动器归 DomainEngine
   * - spi
     - ✅（重定型）
     - spi + common_super_class_provider
     - pass 契约三级单元 + 编排 + sanitizer + resolver
     - 编排语义零变化；契约见 :ref:`pass-contract`；帧链接线已落地（PassPipelineOptions::typeResolver 注入，Strict 重算 + maxes 采用 + SMT 再生，见 :ref:`pass-contract-pipeline`）

第五批（reference 重设计 + workspace）
--------------------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源（mbase）
     - 迁移范围
     - 说明
   * - reference
     - ✅（重设计）
     - reference
     - reference_index（.ridx 缓存读写）/reflection_interpreter/reflection_binding_analyzer/global_reachability + asm/classpath/resolved_call_graph + field_access_graph（调用边传播的字段读写摘要，见 field-access-graph 头注）；根来源 provider 化（keep 规则/档位 + main 发现，共享 entry_points 谓词）
     - alias 经 aliasMemberKey 建键消费；.ridx 字节级保留；binding_analyzer fail-closed；类级引用扫描（NestHost/NestMembers/PermittedSubclasses/Record 组件描述符与嵌套 Signature/注解/ModuleMainClass+Module 的 mainClass·uses·provides）走 libjbc typed 解码面；provider 只读索引产值，无注册表/回调
   * - workspace
     - ✅
     - workspace
     - workspace/i_workspace/workspace_types/class_mutation_service/classfile_support/reference_scan/summary_scan
     - 符号标签指令家族（含 tableswitch/lookupswitch）经 libjbc CodeDom 往返

第六批（remapper + renamer）
----------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源（mbase）
     - 迁移范围
     - 说明
   * - remapper
     - ✅
     - asm/remapper
     - member_remapper/member_remap_plan/resource_remapper + mapping_remapper（原 utf8_remapper 去 visitor 重设计）
     - 24 用例；indy SAM 改名对齐旧 oracle；BSM 编号空间与 CP 引用悬挂两处移植期真 bug 已修；Nest 族三件（NestHost/NestMembers/PermittedSubclasses）改 typed 体手术（命中才 intern + 置脏重编码，成员重映射断言走 libjbc typed 解码）；Record 属性 typed 体手术（组件名/描述符映射 + 组件嵌套 Signature/注解复用成员级手术路径，2026-10-16）；ModuleMainClass 单槽与 Module 的 uses/provides 段 typed 体手术（requires/exports/opens 不触碰，2026-10-16）；新增 nestmate + sealed 样本与 record 样本改名后 JVM 真跑门（nest 宿主/成员/permitted 反射钉死；record 组件 isRecord/getName/getType/getGenericType 与组件注解反射钉死）
   * - renamer
     - ✅
     - renamer
     - rename_engine/shadow_alias/shadow_token/name_suggestion_file/name_proposal/name_provider + rename_context
     - 106 用例；shadow 建键走 aliasMemberKey；shadow_token 悬垂指针 UB 修复随迁（mjt 9e72e9f 同源）；apply 面接线收口：引擎计划按登记序搬运成 MemberRemapPlan 经 applyResourceRemapPlan+RemapPolicy 就地应用（旧栈 step 8 同一条路径，不另起手术代码），非空计划应用零变化 ApplyFailed（旧栈同口径，容器不动）、空计划 zero-applied 成功；默认 RemapPolicy{} 只改字节码面、类资源 URI 随类改名移动（类 URI 门 = bytecode 口径，与旧栈及 morpher 9507ecf39 先例一致；resources 门只管 services/manifest/文本资源面）；javac 多类样本 apply 重读断言（声明类 CP 条目/成员槽/跨类调用点/InnerClasses·NestHost·NestMembers 走 typed 面/未触及类字节不动）+ JVM 真跑门（行为与原名一致 + 反射读新名）+ 旧栈四件 apply 用例断言移植收口

第七批（树 IR 本体：air + sir）
-------------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源（mbase）
     - 迁移范围
     - 说明
   * - air
     - ✅
     - air
     - 17 件（opcode/node/pool/annotation/class/builder/writer/pass/stack/printer/expr_bridge）
     - 112 用例；builder 输入基座按总设计 §3.4(3) 从 jbytecode tree 换 ClassSession/CodeUnit；builder 裸池读清零（multianewarray 经 classOf 语义读、catch 类型经 backing().className 公共读面、ldc 复杂常量经 constOf 分派与旧栈同权 Nop，craft 用例钉死）；writer 写回保真收口（整体重建 + 同位类型族恢复 + 逐方法折叠等价门 + adaptor 驱动 lower + Strict 帧链；ldc 复杂常量/JSR/RET/invokedynamic fail-closed 拒写；恒等往返/变更写回/语料门三道验收）；元数据面（LNT/LVT/LVTT/SourceFile）读侧提升进 AIR——坐标一律指令下标（方法末尾边界记指令总数）、畸形行显式裁剪、LVTT 签名列独立入表；写回源 pc 影线按节点 originalOffset 精确锚定（插入/删除/重排不失位），悬垂行走 prepare 容差收敛（吸附/合并/尾部裁行）+ lower pcmap 精确重定向（miss 即 Err），SourceFile 类级保留；注解与 ConstantValue 仍不转换（旧 TODO 口径）；异常表 End 边界解析与单方法路径 maxes 已修（2026-10-13，回归钉在 air_builder_test）
   * - sir
     - ✅
     - sir
     - 23 件（ir/def_use/liveness/point_liveness/verifier/printer/dot_dumper/pass/opt/destructor/builder/writer）
     - 46 用例；builder/writer 真实现直迁；Switch 表读取修正旧栈潜伏 bug（旧读 op1/op2 实际越界，按 AIR 实际编码读表）；真实 SSA→栈机 lower 挂账 libjbc A1

第八批（消费者对接协议：E 系，新设计）
--------------------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源
     - 迁移范围
     - 说明
   * - consumer
     - ✅（E3）
     - libjbc design.md §9 IClassSource 草案重定型 + mbase workspace/summary_scan 口径
     - class_source（IClassSource 协议 + Memory/Container 双实现）+ summary_scanner（SummaryScanner → ClassSummary）
     - 产品线消费的类字节供给面与骨架扫描面：错误走 ConsumerError 值语义，fail-closed（缺名 Err(NotFound)、容器带病资源 Err(BadClassFile)、扫描损坏字节不产半截摘要）；Recovered（尾部降级）照常出骨架；ClassSummary 口径与 mbase listClassSummaries 逐字段对拍（version 编码 ``(minor << 16) | major``\ ），计数之外再交付成员签名与方法 hasCode 分流位；ContainerClassSource 内部名 first-wins 并记 duplicateNames（与容器 URI 级重复表互补）；14 用例（class_source_test 8 + summary_parity_test 6，截断逐字节全 sweep）。E1（ReferenceSite/reference_index）已随第五批落地

第九批（分析数据库与出口遮蔽，新设计）
--------------------------------------

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 模块
     - 状态
     - 来源
     - 迁移范围
     - 说明
   * - project/analysis_db
     - ✅
     - 新设计（:ref:`analysis-db`）
     - 单文件 SQLite 工程库：稳定身份与重命名覆盖层、条目身份层（schema v3）、分析事实层、普查投影 census、出口遮蔽数据面（schema v4）、引用与层次闭包查询
     - 概览与已落地面见 :ref:`analysis-db`；引擎 #117、PRAGMA 调优 #128、条目身份 #122、切片替换 #139、行偏移分页 #141、census #131/#142、遮蔽数据面 #126
   * - project/entry_shape · class_kind · library_origin
     - ✅
     - 新设计（#121 B4 钩子 + #132 L1）
     - entry_shape（条目路径形态 facet 全集 10 项，#136 补 Overlong/WhitespaceEdge）+ class_kind（类头形态投影派生，access + super 现算）+ library_origin（包前缀三档分类：Jdk/Known/Unknown，官方 10 + 第三方 16 根，kLibraryOriginRuleVersion 规则版本契约）
     - census 投影取值面；纯字节码域知识，只判不改、不改写任何字节
   * - project/mask\_\*
     - ✅
     - 新设计（#121 §4）
     - mask_types + mask_regex_engine + mask_suspect + mask_evaluator（出口遮蔽判定族：规则数据面 schema v4、suspect 四类旗标扫描、求值器判定序）
     - 纯函数判定面无 IO；出口接线归上层消费方

后续批次（未开始）
------------------

- pass：试点三件（illegal_class_file_cleaner / access_modifier_tightener / class_member_sorter）、方法体小批四件（nop_remover / redundant_load_val_eliminator / char_arr_to_str_simplifier / unused_check_cleaner，局部栈操作波次，golden 对拍收口）与控制流首件（junk_code_inserter，经 PassPipelineOptions::typeResolver 帧链接线，libjbc A1 首个真实消费方，自基线 + JVM 真跑验收）已按 spi 契约落成。illegal cleaner 全量规则已于 F 线清洗批经 libjclassfile 前置级接通（§3.0 敌意输入前置清洗级，两级流水，mjbase → libjclassfile 合法依赖边），旧栈清洗规则挂账清空。剩余按实测体量分四批：A 容器级直写组（\ **已收口**\ ：class_renamer 经会话 remapper 面迁成；未使用成员清理家族四件empty_class_remover / unused_class_remover / unused_field_remover / global_unused_method_remover 经 reference 面 + libjbc DOM 成员手术迁成，um 道 golden 对拍 + JVM 真跑门收口。普查砍单：meta_info_remover 家族实为scope:class（AsmClassPass），归 B 类级骨架组；tool_class_combiner 随libjbc §34 成员级增删/从零建类契约落地迁成（static 工具类并入宿主 +供体删除 + 全容器 owner 重映射 + 分支体宿主帧再生 + 常量字段ConstantValue 重建，cc 道 golden 对拍 + JVM 真跑门收口；ldc 宽常量候选按 NotModeled 口径保守保留原资源——旧栈照常合并，为迁移声明的语义面差异，见 golden manifest 声明类目与 pass 头注挂账）→B 类级骨架组（\ **已收口**\ ：meta_info_remover 家族类级 13 件（MetaInfoRemover / FinalRemover / SyntheticBridgeRemover / KotlinMetadataRemover / SourceInfoRemover / SignatureRemover / IllegalAnnotationRemover / IllegalSignatureRemover / IllegalTypeAnnotationRemover / IllegalSourceInfoRemover / UnknownAttributeCleaner / IllegalAccessRemover / SpecificAnnotationRemover）迁成——属性面落 backing 三层属性表（槽摘除 / 槽内过滤 / MethodParameters Raw 原位改写），access 位双面落盘，Code 嵌套属性表有界破例两处（MetaInfoRemover 整槽摘除 debug 表与嵌套注解槽、IllegalTypeAnnotationRemover嵌套槽内过滤，父 Code 槽连带置脏）；tree 道 8 语料 golden 对拍（meta 语义矩阵口径，纯标志位触发类目允许与旧栈 golden 逐字节一致）+ JVM 真跑门（行为一致 + 反射按剥离后口径钉死）收口。能力差距挂账（见家族头注）已收口：record 组件注解清空（typed RecordAttr 组件槽内过滤 + 父 Record槽连带置脏）、SpecificAnnotationRemover 的参数注解槽内过滤（Raw 面JVMS 4.7.20 布局条目摘除）。普查砍单：家族 method 级八件（LineNumberRemover / LocalVariableInfoRemover / ParametersInfoRemover / ExceptionInfoRemover / FrameInfoRemover / IllegalVarargsRemover / IllegalLocalVariableInfoRemover / LongIndyNameOptimizer）为 scope:method，归 C 方法级组；unused_member_remover 已由批 A 收口，不重迁）→C 方法级指令变换组（constant_propagator/dead_code_remover/method_inliner 等主力 12-15 个，CodeDom+CodeEditor 载体，按局部栈操作→数据流→控制流三小波，每 pass 先抽golden vector）→ D 依赖指令重编码/属性级的硬骨头（LVT 重建、stack map等 4-6 个，向 libjbc 单独立项，不硬写）。控制流余量挂账 libjbc A1（管线接线点已就绪，消费形态见 junk_code_inserter）。
- crash_class_visitor / crash_method_visitor / frame_skipping_visitor：零消费方且为 jbytecode visitor 形态（libjbc 禁 visitor），\ **有意不迁**\ 。
- pass / spi：最后一批，依赖前面全部（spi 已迁，pass 余量见上）。

有意不迁
--------

- asm/cache（死代码候选）、info/ClassUnionFind（同）。
- crash_class_visitor / crash_method_visitor / frame_skipping_visitor（零消费方，jbytecode visitor 形态，libjbc 禁 visitor）。
- analyzer 死岛四件（code_analyzer/code_rewriter/code_reference_value/ value_utils，零消费方互锁）+ destination_interpreter（零消费方；多来源producer 集若未来有消费方按域插件重设计）+ dataflow_analyzer/ simple_interpreter/super_interpreter/subroutine 等 8 件旧栈驱动胶水（职责由 DomainEngine/FrameQuery/branch_facts 承担，见 B4 收口提交）。
- info（InfoUtil）：能力被 hierarchy 会话扫描 + libjbc classfile DOM 覆盖，唯一 jbytecode 依赖点（ClassReader）；出现真实消费方时按 jbc 重写~50 行提取器即可，不单开模块。
- 旧 matcher 与 tmatcher 在 mjbase 是否合并：第一批先各自迁移，集成期裁决（合并进 tmatcher 一代）。
