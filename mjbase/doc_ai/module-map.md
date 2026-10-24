# mjbase 模块地图

以 mbase（`../mbase/src/mbase/`，约 368 文件 / 8.8 万行）为语义来源，
在 libjbc 底座上逐模块重写。状态：✅ 已迁 ｜ 🔨 迁移中 ｜ ⬜ 未开始 ｜
➖ 有意不迁（理由注明）。

## 地基（主线持有，模块 agent 禁改）

| 模块 | 状态 | 来源 | 说明 |
|---|---|---|---|
| keys | ✅ | design.md §9（新设计） | KeyPool 驻留池 + ClassKey/MemberKey/MemberKeyHash；跨类身份原语 |
| util | ✅ | mbase/util/class_name_util | 内部名/描述符纯视图函数 + isLibraryClass |

## 第一批（asm 域优先 + 其直接前置）

| 模块 | 状态 | 来源（mbase） | 迁移范围 | 底座映射 |
|---|---|---|---|---|
| asm/cfg | ✅ | asm/cfg | 基本块、CFG 后继/前驱/异常边、迭代支配树、可达集 | CodeEditor::blocks + resolveLabel + CodeUnit::exc |
| asm/insn | ✅ | asm/insn | opcode 分类谓词、栈效应表、指令工厂 | jbc::ir::Insn / CodeEditor 工厂 |
| asm/analyzer | ✅ | asm/analyzer | 常量域、来源域、可空性事实域、跳转走向事实（branch_facts）、跨路径整数约束事实（path_conditions：分支谓词贴边、must 交集、死分支修剪，见 doc_ai/path-conditions.md）、字符串/常量流事实（string_flow：来源、逃逸、Base64/XOR/char 偏移管线）+ 入参常量种子 + slot 级 def-use（有界到达定义集、异常边、字段访问事实，见 doc_ai/slot-def-use.md） | jbc::analysis::DomainEngine + IValueDomain；引擎缺口挂账：分支条件钩子、CHECKCAST 族 operand-aware 钩子；string_flow 设计见 [string-flow.md](string-flow.md) |
| asm/tmatcher | ✅ | asm/tmatcher | 新代匹配 DSL：combinator、capture、替换规则骨架 | CodeDom::order 窗口匹配 |
| asm/matcher | ✅ | asm/matcher | 旧代 pattern_matcher 语义 | 同上 |
| asm/evaluator | ✅ | asm/evaluator | 具体求值栈机（数值子集 + 分支 + 步数熔断） | CodeDom 只读遍历 |
| asm/classpath | ✅ | asm/classpath | 调用边提取（MemberKey 键）、可达、库类过滤、indy/MethodHandle 解析与 BSM 展开边、VarHandle 读写事实、惰性调用链枚举 + 调用树视图（简单路径/环叶/去重号/节点上限） | promote + memberOf + keys + BootstrapMethodsAttr |
| asm/stmt | ✅ | 新设计（issue #5） | 保守结构化语句层：块级 if/while/for/do-while、语句↔指令窗口双映射、叶子整窗写回 | asm_cfg 快照 + CodeEditor insert/eraseBatch；见 doc_ai/statement-ir.md |
| hierarchy | ✅ | hierarchy | 类/方法/字段层次内存图、ancestors/overrides（.ihc 序列化不迁） | 会话扫描 this/super/interfaces + keys |

## 第二批（资源与映射叶子，直迁）

| 模块 | 状态 | 来源（mbase） | 迁移范围 | 说明 |
|---|---|---|---|---|
| jar | ✅ | jar | jar_reader/jar_writer/manifest/jar_util + dos_time_util（随迁叶） | manifest 保序字节级输出契约保留；throw 改 Result<_,JarError> |
| model | ✅ | model | resource_model/container/io/helper | libarchive 后端保留（tar/xz/gz/zst 语义不丢）；throw 改 ModelError 枚举；路径统一 u8path |
| mapping | ✅ | mapping | 全 16 文件（7 格式注册表 + TinyV2 + shadow-alias + identifier_*） | 格式输出字节级等价（怪异行为原样）；invalid_mapping_exception 删除改 MappingError |
| project | ✅ | project | analysis_project（schema v1）+ project_store（事务/锁/恢复） | 契约值原封（schema/kind 串/sha256 形状/engine 自述串/锁语义）；平台分支照搬 |

## 第三批（叶子与 CLI/JASM 面）

| 模块 | 状态 | 来源（mbase） | 迁移范围 | 说明 |
|---|---|---|---|---|
| expr | ✅ | expr | 全 18 文件（树/解析/求值/化简/MBA 生成） | 180 用例全移植；对 hierarchy/model 零依赖属实 |
| log | ✅ | log | log_init 门面 | CA_LOG 门面接 libca_log |
| i18n | ✅ | i18n.hpp | 兜底入口 | key 空间保持 mbase.* 字面前缀（.lang 逐字节照搬纪律） |
| util 叶子 | ✅ | util/pair,span,triple,time_util | util/ 子目录 | class_name_util、dos_time_util 前批复迁 |
| cli | ✅ | cli | ArgParser 框架 + i18n embed | 2 个 globalPipeline 用例待 spi/pass 批接线后恢复 |
| jasm | ✅ | jasm | JVM/资源两级服务 | GAM 依赖按无缓存逐调用重建重设计；jbytecode/jdasm 脱钩 |

## 第四批（analysis 域化 + pass 契约重定型）

| 模块 | 状态 | 来源（mbase） | 迁移范围 | 说明 |
|---|---|---|---|---|
| analysis | ✅ | analysis | alias（域插件+两跑）/andersen/exception_flow/method_effect/method_frame | 反射身份契约逐字保留；摘要键改 MemberKey（aliasMemberKey 建键）；method_frame 驱动器归 DomainEngine |
| spi | ✅（重定型） | spi + common_super_class_provider | pass 契约三级单元 + 编排 + sanitizer + resolver | 编排语义零变化；契约见 doc_ai/pass-contract.md；帧链接线已落地（PassPipelineOptions::typeResolver 注入，Strict 重算 + maxes 采用 + SMT 再生，见 pass-contract §2） |

## 第五批（reference 重设计 + workspace）

| 模块 | 状态 | 来源（mbase） | 迁移范围 | 说明 |
|---|---|---|---|---|
| reference | ✅（重设计） | reference | reference_index（.ridx 缓存读写）/reflection_interpreter/reflection_binding_analyzer/global_reachability + asm/classpath/resolved_call_graph + field_access_graph（调用边传播的字段读写摘要，见 doc_ai/field-access-graph.md）；根来源 provider 化（keep 规则/档位 + main 发现，共享 entry_points 谓词） | alias 经 aliasMemberKey 建键消费；.ridx 字节级保留；binding_analyzer fail-closed；类级引用扫描（NestHost/NestMembers/PermittedSubclasses/Record 组件描述符与嵌套 Signature/注解/ModuleMainClass+Module 的 mainClass·uses·provides）走 libjbc typed 解码面；provider 只读索引产值，无注册表/回调 |
| workspace | ✅ | workspace | workspace/i_workspace/workspace_types/class_mutation_service/classfile_support/reference_scan/summary_scan | 符号标签指令家族（含 tableswitch/lookupswitch）经 libjbc CodeDom 往返 |

## 第六批（remapper + renamer）

| 模块 | 状态 | 来源（mbase） | 迁移范围 | 说明 |
|---|---|---|---|---|
| remapper | ✅ | asm/remapper | member_remapper/member_remap_plan/resource_remapper + mapping_remapper（原 utf8_remapper 去 visitor 重设计） | 24 用例；indy SAM 改名对齐旧 oracle；BSM 编号空间与 CP 引用悬挂两处移植期真 bug 已修；Nest 族三件（NestHost/NestMembers/PermittedSubclasses）改 typed 体手术（命中才 intern + 置脏重编码，成员重映射断言走 libjbc typed 解码）；Record 属性 typed 体手术（组件名/描述符映射 + 组件嵌套 Signature/注解复用成员级手术路径，2026-10-16）；ModuleMainClass 单槽与 Module 的 uses/provides 段 typed 体手术（requires/exports/opens 不触碰，2026-10-16）；新增 nestmate + sealed 样本与 record 样本改名后 JVM 真跑门（nest 宿主/成员/permitted 反射钉死；record 组件 isRecord/getName/getType/getGenericType 与组件注解反射钉死） |
| renamer | ✅ | renamer | rename_engine/shadow_alias/shadow_token/name_suggestion_file/name_proposal/name_provider + rename_context | 106 用例；shadow 建键走 aliasMemberKey；shadow_token 悬垂指针 UB 修复随迁（mjt 9e72e9f 同源）；apply 面接线收口：引擎计划按登记序搬运成 MemberRemapPlan 经 applyResourceRemapPlan+RemapPolicy 就地应用（旧栈 step 8 同一条路径，不另起手术代码），非空计划应用零变化 ApplyFailed（旧栈同口径，容器不动）、空计划 zero-applied 成功；默认 RemapPolicy{} 只改字节码面、类资源 URI 不随动（remapper 批 bytecode+resources 双门口径，与旧栈 URI 单随 bytecode 门为已声明的语义面差异，需随动显式开 resources）；javac 多类样本 apply 重读断言（声明类 CP 条目/成员槽/跨类调用点/InnerClasses·NestHost·NestMembers 走 typed 面/未触及类字节不动）+ JVM 真跑门（行为与原名一致 + 反射读新名）+ 旧栈四件 apply 用例断言移植收口 |


## 第七批（树 IR 本体：air + sir）

| 模块 | 状态 | 来源（mbase） | 迁移范围 | 说明 |
|---|---|---|---|---|
| air | ✅ | air | 17 件（opcode/node/pool/annotation/class/builder/writer/pass/stack/printer/expr_bridge） | 112 用例；builder 输入基座按总设计 §3.4(3) 从 jbytecode tree 换 ClassSession/CodeUnit；builder 裸池读清零（multianewarray 经 classOf 语义读、catch 类型经 backing().className 公共读面、ldc 复杂常量经 constOf 分派与旧栈同权 Nop，craft 用例钉死）；writer 写回保真收口（整体重建 + 同位类型族恢复 + 逐方法折叠等价门 + adaptor 驱动 lower + Strict 帧链；ldc 复杂常量/JSR/RET/invokedynamic fail-closed 拒写；恒等往返/变更写回/语料门三道验收）；元数据面（LNT/LVT/LVTT/SourceFile）读侧提升进 AIR——坐标一律指令下标（方法末尾边界记指令总数）、畸形行显式裁剪、LVTT 签名列独立入表；写回源 pc 影线按节点 originalOffset 精确锚定（插入/删除/重排不失位），悬垂行走 prepare 容差收敛（吸附/合并/尾部裁行）+ lower pcmap 精确重定向（miss 即 Err），SourceFile 类级保留；注解与 ConstantValue 仍不转换（旧 TODO 口径）；异常表 End 边界解析与单方法路径 maxes 已修（2026-10-13，回归钉在 air_builder_test） |
| sir | ✅ | sir | 23 件（ir/def_use/liveness/point_liveness/verifier/printer/dot_dumper/pass/opt/destructor/builder/writer） | 46 用例；builder/writer 真实现直迁；Switch 表读取修正旧栈潜伏 bug（旧读 op1/op2 实际越界，按 AIR 实际编码读表）；真实 SSA→栈机 lower 挂账 libjbc A1 |

## 第八批（消费者对接协议：E 系，新设计）

| 模块 | 状态 | 来源 | 迁移范围 | 说明 |
|---|---|---|---|---|
| consumer | ✅（E3） | libjbc design.md §9 IClassSource 草案重定型 + mbase workspace/summary_scan 口径 | class_source（IClassSource 协议 + Memory/Container 双实现）+ summary_scanner（SummaryScanner → ClassSummary） | 产品线消费的类字节供给面与骨架扫描面：错误走 ConsumerError 值语义，fail-closed（缺名 Err(NotFound)、容器带病资源 Err(BadClassFile)、扫描损坏字节不产半截摘要）；Recovered（尾部降级）照常出骨架；ClassSummary 口径与 mbase listClassSummaries 逐字段对拍（version 编码 `(minor << 16) \| major`），计数之外再交付成员签名与方法 hasCode 分流位；ContainerClassSource 内部名 first-wins 并记 duplicateNames（与容器 URI 级重复表互补）；14 用例（class_source_test 8 + summary_parity_test 6，截断逐字节全 sweep）。E1（ReferenceSite/reference_index）已随第五批落地 |

## 后续批次（未开始）

- pass：试点三件（illegal_class_file_cleaner / access_modifier_tightener /
  class_member_sorter）、方法体小批四件（nop_remover /
  redundant_load_val_eliminator / char_arr_to_str_simplifier /
  unused_check_cleaner，局部栈操作波次，golden 对拍收口）与控制流首件
  （junk_code_inserter，经 PassPipelineOptions::typeResolver 帧链接线，
  libjbc A1 首个真实消费方，自基线 + JVM 真跑验收）已按 spi 契约落成。
  illegal cleaner 全量规则已于 F 线清洗批经 libjclassfile 前置级接通
  （§3.0 敌意输入前置清洗级，两级流水，mjbase → libjclassfile 合法依赖边），
  旧栈清洗规则挂账清空。剩余按实测体量分四批：A 容器级直写组（**已收口**：
  class_renamer 经会话 remapper 面迁成；未使用成员清理家族四件
  empty_class_remover / unused_class_remover / unused_field_remover /
  global_unused_method_remover 经 reference 面 + libjbc DOM 成员手术迁成，
  um 道 golden 对拍 + JVM 真跑门收口。普查砍单：meta_info_remover 家族实为
  scope:class（AsmClassPass），归 B 类级骨架组；tool_class_combiner 随
  libjbc §34 成员级增删/从零建类契约落地迁成（static 工具类并入宿主 +
  供体删除 + 全容器 owner 重映射 + 分支体宿主帧再生 + 常量字段
  ConstantValue 重建，cc 道 golden 对拍 + JVM 真跑门收口；ldc 宽常量候选
  按 NotModeled 口径保守保留原资源——旧栈照常合并，为迁移声明的语义面
  差异，见 golden manifest 声明类目与 pass 头注挂账）→
  B 类级骨架组（**已收口**：meta_info_remover 家族类级 13 件
  （MetaInfoRemover / FinalRemover / SyntheticBridgeRemover /
  KotlinMetadataRemover / SourceInfoRemover / SignatureRemover /
  IllegalAnnotationRemover / IllegalSignatureRemover /
  IllegalTypeAnnotationRemover / IllegalSourceInfoRemover /
  UnknownAttributeCleaner / IllegalAccessRemover / SpecificAnnotationRemover）
  迁成——属性面落 backing 三层属性表（槽摘除 / 槽内过滤 / MethodParameters
  Raw 原位改写），access 位双面落盘，Code 嵌套属性表有界破例两处
  （MetaInfoRemover 整槽摘除 debug 表与嵌套注解槽、IllegalTypeAnnotationRemover
  嵌套槽内过滤，父 Code 槽连带置脏）；tree 道 8 语料 golden 对拍（meta 语义
  矩阵口径，纯标志位触发类目允许与旧栈 golden 逐字节一致）+ JVM 真跑门
  （行为一致 + 反射按剥离后口径钉死）收口。能力差距挂账（见家族头注）
  已收口：record 组件注解清空（typed RecordAttr 组件槽内过滤 + 父 Record
  槽连带置脏）、SpecificAnnotationRemover 的参数注解槽内过滤（Raw 面
  JVMS 4.7.20 布局条目摘除）。普查砍单：家族 method 级八件（LineNumberRemover /
  LocalVariableInfoRemover / ParametersInfoRemover / ExceptionInfoRemover /
  FrameInfoRemover / IllegalVarargsRemover / IllegalLocalVariableInfoRemover /
  LongIndyNameOptimizer）为 scope:method，归 C 方法级组；
  unused_member_remover 已由批 A 收口，不重迁）→
  C 方法级指令变换组
  （constant_propagator/dead_code_remover/method_inliner 等主力 12-15 个，
  CodeDom+CodeEditor 载体，按局部栈操作→数据流→控制流三小波，每 pass 先抽
  golden vector）→ D 依赖指令重编码/属性级的硬骨头（LVT 重建、stack map
  等 4-6 个，向 libjbc 单独立项，不硬写）。
  控制流余量挂账 libjbc A1（管线接线点已就绪，消费形态见 junk_code_inserter）。
- crash_class_visitor / crash_method_visitor / frame_skipping_visitor：零消费方
  且为 jbytecode visitor 形态（libjbc 禁 visitor），**有意不迁**。
- pass / spi：最后一批，依赖前面全部（spi 已迁，pass 余量见上）。

## 有意不迁

- asm/cache（死代码候选）、info/ClassUnionFind（同）。
- crash_class_visitor / crash_method_visitor / frame_skipping_visitor（零消费方，
  jbytecode visitor 形态，libjbc 禁 visitor）。
- analyzer 死岛四件（code_analyzer/code_rewriter/code_reference_value/
  value_utils，零消费方互锁）+ destination_interpreter（零消费方；多来源
  producer 集若未来有消费方按域插件重设计）+ dataflow_analyzer/
  simple_interpreter/super_interpreter/subroutine 等 8 件旧栈驱动胶水
  （职责由 DomainEngine/FrameQuery/branch_facts 承担，见 B4 收口提交）。
- info（InfoUtil）：能力被 hierarchy 会话扫描 + libjbc classfile DOM 覆盖，
  唯一 jbytecode 依赖点（ClassReader）；出现真实消费方时按 jbc 重写
  ~50 行提取器即可，不单开模块。
- 旧 matcher 与 tmatcher 在 mjbase 是否合并：第一批先各自迁移，
  集成期裁决（合并进 tmatcher 一代）。
