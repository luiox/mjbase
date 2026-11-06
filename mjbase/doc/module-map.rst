.. _module-map:

mjbase 模块地图
===============

以 mbase 仓（约 368 文件 / 8.8 万行）为语义来源，在 libjbc 底座上逐模块重写。状态：✅ 在仓 ｜ ➖ 有意不迁、已退役或已出仓（理由注明）。

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
     - libjbc 设计文档（新设计）
     - KeyPool 驻留池 + ClassKey/MemberKey/MemberKeyHash；跨类身份原语
   * - util
     - ✅
     - mbase/util/class_name_util
     - 内部名/描述符纯视图函数 + isLibraryClass

asm 域
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
     - 常量域、来源域、可空性事实域、跳转走向事实（branch_facts）、跨路径整数约束事实（path_conditions：分支谓词贴边、must 交集、死分支修剪，见 path_conditions 头注）、字符串/常量流事实（string_flow：来源、逃逸、Base64/XOR/char 偏移管线）+ 入参常量种子 + slot 级 def-use（有界到达定义集、异常边、字段访问事实，见 slot_def_use 头注）
     - jbc::analysis::DomainEngine + IValueDomain；引擎缺口：分支条件钩子、CHECKCAST 族 operand-aware 钩子；string_flow 设计见 string_flow 头注
   * - asm/tmatcher
     - ✅
     - asm/tmatcher
     - 新代匹配 DSL：combinator、capture、替换规则骨架
     - CodeDom::order 窗口匹配
   * - asm/matcher
     - ➖
     - asm/matcher
     - 旧代 pattern_matcher 语义
     - 旧 matcher 全族退役，StackOperationSimplifier 迁 tmatcher 载体（#99）
   * - asm/evaluator
     - ➖
     - asm/evaluator
     - 具体求值栈机（数值子集 + 分支 + 步数熔断）
     - 已摘除，求值栈机由 libjsim 接位（#98）
   * - asm/classpath
     - ✅
     - asm/classpath
     - 调用边提取（MemberKey 键）、可达、库类过滤、indy/MethodHandle 解析与 BSM 展开边、VarHandle 读写事实、惰性调用链枚举 + 调用树视图（简单路径/环叶/去重号/节点上限）
     - promote + memberOf + keys + BootstrapMethodsAttr
   * - asm/stmt
     - ✅
     - 新设计（issue #5）
     - 保守结构化语句层：块级 if/while/for/do-while、语句↔指令窗口双映射、叶子整窗写回
     - asm_cfg 快照 + CodeEditor insert/eraseBatch；见 statement_ir 头注
   * - asm/sem
     - ✅
     - 新设计（issue #115）
     - 语义设施层四段：只读视图 + 计划 + 策略 + 驱动。视图：FrameView（Strict 逐指令到达帧 + MethodSig 播种 + ITypeResolver 注入面 + UninitThis 探针）、SlotProvenanceView（栈/局部槽到达定义查询 + def→use 边，DUP 族按「复制槽不产定义、产使用边」补边 + 切面槽干净判据）、RegionView（SESE 候选区域枚举：入口支配集 ∩ 出口锚后支配 + 单入口单出口过滤 + 异常同质/直线/jsr 旗标）、LivenessView（局部槽后向 may-liveness + 出口活跃集，异常 handler 边并入，写后杀/回边收敛）。计划：OutlinePlan（直线区域 outline：copy-set 最小化 + 槽重编号覆盖读写全集 + DUP 边界规范化 + goto 入边参数装载随入边走 + LVT 越切点宽松开关 + 自足槽 MjOutlineStamp 盖章）与 MergePlan（outline$ 合成方法的 inline 对偶，fail-closed 识别码 7 枚），一律纯数据 + validate/apply 分离，判据与落地口径见 outline_plan.hpp 头注。策略：region_select（RegionSelectRequest/RegionDecision：事实面预检 + 规模次之排序 + 冲突保守拒绝 + planOutline 准入探针；插拔 = RegionSelectPolicy 档位 + RegionSelectFn 函数形状）。驱动：driveOutlines 逐轮「选区 → 识别 → 落地 → 重建视图」闭环（maxRounds 上限、单轮失败即停、已 apply 轮次不回滚）+ driveMethods 方法级编排。行为保持门：JVM 真跑对拍 + 指令序列规范化对拍（非字节级）。
     - static
   * - hierarchy
     - ✅
     - hierarchy
     - 类/方法/字段层次内存图、ancestors/overrides（.ihc 序列化不迁）
     - 会话扫描 this/super/interfaces + keys

资源与映射
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

叶子与服务面
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
     - class_name_util、dos_time_util
   * - cli
     - ➖
     - cli
     - ArgParser 框架 + i18n embed
     - 模块整体退役，命令行解析轮归 libca ``ca::opt``\ （#107）；i18n embed 走 ``embed-lang`` 产物同步
   * - jasm
     - ✅
     - jasm
     - JVM/资源两级服务
     - GAM 依赖按无缓存逐调用重建重设计；jbytecode/jdasm 脱钩

analysis 域化与 pass 契约
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

reference 与 workspace
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
     - reference_index（.ridx 缓存读写）/reflection_interpreter/reflection_binding_analyzer/global_reachability + asm/classpath/resolved_call_graph + field_access_graph（调用边传播的字段读写摘要，见 field_access_graph 头注）；根来源 provider 化（keep 规则/档位 + main 发现，共享 entry_points 谓词）
     - alias 经 aliasMemberKey 建键消费；.ridx 字节级保留；binding_analyzer fail-closed；类级引用扫描（NestHost/NestMembers/PermittedSubclasses/Record 组件描述符与嵌套 Signature/注解/ModuleMainClass+Module 的 mainClass·uses·provides）走 libjbc typed 解码面；provider 只读索引产值，无注册表/回调
   * - workspace
     - ➖
     - workspace
     - 工作副本与编辑门面
     - 模块整体退役，工作副本面移交下游 mjt 自理（#103）

remapper 与 renamer
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

树 IR：air + sir
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
     - ➖
     - air
     - 17 件（opcode/node/pool/annotation/class/builder/writer/pass/stack/printer/expr_bridge）
     - 出仓定案：deobf 工具面 IR 驻留 mjt 的 ``mjt::deobf::air``\ ，实现口径与单测随该仓维护，mjbase 不另建
   * - sir
     - ➖
     - sir
     - 23 件（ir/def_use/liveness/point_liveness/verifier/printer/dot_dumper/pass/opt/destructor/builder/writer）
     - 出仓定案：驻留 mjt 的 ``mjt::deobf::sir``\ ；真实 SSA→栈机 lower 归该仓

消费者对接协议
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
     - ➖
     - libjbc 设计文档 IClassSource + mbase workspace/summary_scan 口径
     - class_source（IClassSource 协议 + Memory/Container 双实现）+ summary_scanner（SummaryScanner → ClassSummary）
     - 模块退役：全仓零消费方（#105）

分析数据库与出口遮蔽
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
     - 新设计（#121 + #132）
     - entry_shape（条目路径形态 facet 全集 10 项，#136 补 Overlong/WhitespaceEdge）+ class_kind（类头形态投影派生，access + super 现算）+ library_origin（包前缀三档分类：Jdk/Known/Unknown，官方 10 + 第三方 16 根，kLibraryOriginRuleVersion 规则版本契约）
     - census 投影取值面；纯字节码域知识，只判不改、不改写任何字节
   * - project/mask\_\*
     - ✅
     - 新设计
     - mask_types + mask_regex_engine + mask_suspect + mask_evaluator（出口遮蔽判定族：规则数据面 schema v4、suspect 四类旗标扫描、求值器判定序）
     - 纯函数判定面无 IO；出口接线归上层消费方

pass 变换族
------------------

pass 变换族按 spi 契约落成，登记处以 ``default_pass_registry`` / ``builtin_pipelines`` 的注册表为准，本文不复制清单以免与代码漂移。载体三类：容器级直写（class_renamer 走会话 remapper 面；unused_member_remover 家族四件 empty/unused_class/unused_field/global_unused_method 走 reference 面 + libjbc DOM 成员手术；tool_class_combiner 静态工具类并入宿主 + 供体删除 + 全容器 owner 重映射 + 常量字段 ConstantValue 重建，其 ldc 宽常量候选按 NotModeled 保守保留原资源，见 golden manifest 声明类目与各 pass 自己的头注）；类级骨架（meta_info_remover 家族类级 13 件，属性面落 backing 三层属性表 = 槽摘除/槽内过滤/MethodParameters Raw 原位改写，access 位双面落盘，Code 嵌套属性表两处有界破例，口径见 meta_info_cleanup 头注；illegal_class_file_cleaner 全量规则经 libjclassfile 前置级接通，两级流水，mjbase → libjclassfile 是合法依赖边）；方法级指令变换（CodeDom + CodeEditor 载体，含 junk_code_inserter 经 PassPipelineOptions::typeResolver 帧链接线，以及 constant_propagator / method_inliner / dead_local_store_remover / fix_pattern_simplifier 等主力）。golden 对拍与 JVM 真跑门逐件挂在各 pass 的测试面。

有意不迁
--------

- asm/cache（死代码候选）、info/ClassUnionFind（同）。
- crash_class_visitor / crash_method_visitor / frame_skipping_visitor（零消费方，jbytecode visitor 形态，libjbc 禁 visitor）。
- analyzer 死岛四件（code_analyzer/code_rewriter/code_reference_value/ value_utils，零消费方互锁）+ destination_interpreter（零消费方；多来源producer 集若未来有消费方按域插件重设计）+ dataflow_analyzer/ simple_interpreter/super_interpreter/subroutine 等 8 件旧栈驱动胶水（职责由 DomainEngine/FrameQuery/branch_facts 承担）。
- info（InfoUtil）：能力被 hierarchy 会话扫描 + libjbc classfile DOM 覆盖，唯一 jbytecode 依赖点（ClassReader）；出现真实消费方时按 jbc 重写~50 行提取器即可，不单开模块。

