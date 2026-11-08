.. _reach-closure:

全程序方法可达闭包入分析数据库设计（analysis-db schema v9 #159，v10 RTA 精度档后置批）
======================================================================================

状态：定稿，已落地（CHA 档随 v9 落地；RTA 精度档与 indy 补充边射入随 v10 落地）。analysis-db 总体设计与身份模型见 :ref:`analysis-db`\ ；本设计已落地定案（全程序方法可达闭包）：闭包输入源、CHA 语义口径、RTA 精度档口径、落库形态、失效与版本门全部裁决，PoC 数字见 §7。既有内存态可达性设施（reference 模块 GlobalReachability / ResolvedCallGraph）不动；本设计把闭包作为派生事实落入分析库，供 callgraph 类工具与普查投影扩展消费。

1. 现状与问题
-------------

分析库（schema v8）已具备三块地基：refs 目标侧 id 化（v5，target_class_id / target_member_id 可空外键，调用边是整数对，按源反查索引 refs_source_member 在位）、producer 注册表（v6，producers / batches / batch_scopes 三表与作用域锚语义）、方法版本指纹（v8，method_versions 的 fp_version 算法版本字节模式）。层次图（hierarchy 模块 ClassHierarchy / MethodHierarchy）提供 override 等价族展开，indy / MethodHandle 展开边在 asm/classpath call_graph 落地（#2 收口）。

缺的是「全程序方法可达闭包」这份派生事实的持久落点。现有 GlobalReachability 是内存态、容器为输入的保守闭包，随进程消亡；mjt MCP 查询工具与普查投影要回答「哪些方法可达」时只能重新扫描字节码重建。#159 要求把闭包作为 producer 批次落入分析库：可达成员集 + 输入图指纹（失效判定）+ 输入覆盖度标记（known-gap 自报）。

已知边界先行声明：分析库 refs 当前 direct-only，lambda / method_handle / reflection 形态的边在 refs 表无 kind 分类；闭包在这些点欠近似（结果批次必须带输入覆盖度标记，refs kind 分类线落地后闭包重算即收益，两线互喂——#159 票面原文）。

.. _reach-closure-input:

2. 决策点 1：闭包输入源 = refs 为主图，indy 边为补充射入
--------------------------------------------------------

两案：分析库 refs 表（持久、id 化、direct-only）vs classpath call_graph 视图（有 indy 展开边、内存态）。裁决：混合口径，refs 为主图，indy 展开边为补充射入，补充边在批次元数据中声明覆盖度。

refs 为主图的理由：闭包的失效判定吃输入图指纹，指纹必须锚定持久状态（refs + inputs），内存态图没有可指纹化的持久身份；闭包批次是持久派生事实，输入源必须同为持久层，否则「指纹不匹配重算」的失效语义无从谈起。refs 的整数对图遍历不碰文本（v5 的既得收益），member 级邻接按 source_member_id 反查索引在位。

indy 补充边不进 refs 表：indy / MethodHandle 展开边是 call_graph 批的分析产物（BSM 可解析时的展开目标），不是字节码编码事实；把它写进 refs 会污染「refs 是原始字节码直接编码事实」的分层语义（analysis_db.hpp 文件头明文）。PoC 实测 refs 已包含 BSM MethodHandle 的编码目标行（ReferenceIndex 对 indy 指令的 handle 引用照常入库，见 §7 M6），call_graph indyExpanded 边与 refs 编码 handle 行高度重叠，补充射入的净增量与闭包差集见 PoC 数字——这决定了「补充射入」是低成本增量面而非第二张图。

.. _reach-closure-cha:

3. 决策点 3：CHA 语义口径
-------------------------

虚调用 / 接口调用的目标集 = 层次子树全部 override，hierarchy 模块展开，与既有 ResolvedCallGraph CHA 档同口径：

- invokevirtual / invokeinterface：编码目标（owner, name, desc）先落解到声明者（沿 owner 的 super 链上溯首个声明；类链不中则接口闭包内声明者兜底），再按声明者的 override 等价族扩展，族内过滤出具体方法（非 abstract、非 static、非 private、非 ``<init>``/``<clinit>``\ ）。族内含祖先链声明是保守余量（运行时不会命中祖先声明，但可达性是单调集合，超集无害）；CHA 档对族全收，子树收窄由 RTA 精度档承担（见下）。

RTA 精度档口径（closure_batches.precision = 1，schema v10）：在 CHA 之上做实例化集合收窄。入口可达方法体（worklist 已出队者）的 Type 引用行（new/checkcast/instanceof/ldc class 全族，refs 无 opcode 位按「可能 new」保守处理——实例化集合是真 new 目标集的超集，方向安全）把目标类加入已实例化集合，fixpoint 迭代（晚发现的实例化回灌早先挂起的调用点）。虚分派族条目仅当其声明类落在某已实例化类型的祖先闭包（super 链 + 接口闭包，自含）内才可达：继承未覆写条目由此保留（祖先可行即条目可行），闭包内零实例化证据的调用点零出边（相对 CHA 的精度收益来源）。静态与 ``<clinit>`` 不经分派、不收窄；private/``<init>`` 等实例成员按同口径收窄（无实例化证据即无接收者实例，super 调用链的接收者亦落在已实例化子类的祖先闭包内）。种子不为其宿主类供实例化证据（canonical RTA 口径）。铁律（对齐 reference::ResolvedCallGraph RTA 档）：RTA 档结果不直接作删除依据——反射建实例、classpath 外实例化按欠近似处理，删除判死用 CHA 档或 keep 根面。
- invokestatic / invokespecial：精确编码目标（special 的 super 调用沿链上溯落解）。
- default 方法冲突：多接口 default 同签名冲突（JVM IncompatibleClassChangeError 场景）不做 most-specific 裁决，族内具体声明全部入目标集（保守并集，宁过近似不丢真实执行边；MethodHierarchy::virtualDispatch 的单继承链 Unresolved 口径不适用于闭包面，闭包不吃「唯一目标」语义）。
- clinit 触发边进闭包口径：Type 引用（new/checkcast/instanceof/ldc class 全族）、Field 引用（get/put 家族）、直连方法引用（static/special）一律按「可能首触」保守处理——目标类的 ``<clinit>`` 入闭包，且按 JVM 初始化次序传递沿 super 链把祖先类 ``<clinit>`` 一并入闭包。refs 无 opcode 位，static 与 instance 首触不可分，宁可多收（refs kind 分类线的 opcode 位属于「两线互喂」的后续项，落地后可收窄）。虚分派族目标不触发 clinit（分派本身不触发初始化，实例创建面由 Type 引用承载）。
- 种子 = 调用方给定的 member_id 集合；基座内置两档：entry_points（\ ``public static void main(String[])`` 发现，对齐 entry_points.hpp 的 isJavaMainMethod 谓词）与 keep_roots（keep 档位语义在 members 表上的投影：public 类的 public/protected 具体方法，对齐 KeepRootProvider 的 PublicProtected 档位语义，不含 abstract）。外种子（显式根清单）经同一 member_id 接口进入。

.. _reach-closure-store:

4. 决策点 2：落库形态 = 可达成员集（member_id + 批次行），不存边路径
--------------------------------------------------------------------

「从哪个种子可达」不作为 scope 维度（基线口径）：per-seed 闭包是十万级方法 × 种子数的行膨胀，且消费面（callgraph 工具、普查投影）要的是整体可达集；种子级归属留待真实消费面提出需求再议。不存边路径：可达集是集合事实，路径重建是查询期遍历的职责。

两张新表 + 既有 producer 注册表承载：

.. code:: sql

   -- 批次元数据（一闭包计算运行一行）
   CREATE TABLE closure_batches(
     batch_id          INTEGER PRIMARY KEY REFERENCES batches(batch_id),
     graph_fingerprint BLOB NOT NULL CHECK(length(graph_fingerprint)=32),
     fp_version        INTEGER NOT NULL CHECK(fp_version>=1),
     seed_mask         INTEGER NOT NULL DEFAULT 0,
     seed_count        INTEGER NOT NULL DEFAULT 0,
     coverage_mask     INTEGER NOT NULL DEFAULT 0,
     indy_edge_count   INTEGER NOT NULL DEFAULT 0,
     member_count      INTEGER NOT NULL DEFAULT 0,
     computed_at       TEXT NOT NULL,
     precision         INTEGER NOT NULL DEFAULT 0
   );
   -- 可达成员集（闭包结果本体）
   CREATE TABLE closure_members(
     batch_id  INTEGER NOT NULL REFERENCES batches(batch_id) ON DELETE CASCADE,
     member_id INTEGER NOT NULL REFERENCES members(member_id) ON DELETE CASCADE,
     PRIMARY KEY(batch_id, member_id)
   ) WITHOUT ROWID;
   CREATE INDEX closure_members_member ON closure_members(member_id);

- 闭包批次走既有注册表：registerProducer（建议 key：CHA 档 ``mjbase.reachability.cha``、RTA 档 ``mjbase.reachability.rta``\ ，trusted 档）→ openBatch（kind = ``closure_write``\ ，DbBatchKind 新词）→ 写事实 → closeBatch（counts.memberCount 回填闭包行数）。scope 落类域：batch_scopes 逐类落 (2, class_id) 作用域行，闭包覆盖的类域经既有作用域锚语义回答「这份派生事实覆盖哪些类」。
- graph_fingerprint = 输入图指纹（§5）；fp_version 算法版本字节（kReachabilityClosureFpVersion = 1），仿 method_versions fp_version 模式——算法升级后旧行自证版本位，跨版本不匹配即失效。precision 精度档列（v10）：0 = CHA 全族保守展开（缺省档）、1 = RTA 实例化集合收窄（§3 RTA 口径）。
- seed_mask 位词表：bit0 = entry_points（main 发现）、bit1 = keep_roots 档位、bit2 = 显式外种子；seed_count 记种子总数。coverage_mask 位词表：bit0 = refs direct-only（恒置位，v9 的固有覆盖度）、bit1 = indy 补充边已射入（v10 起由计算选项驱动：调用方把 call_graph indyExpanded 边落解成 member_id 对传入，计算面去重后并入方法邻接并置位）；indy_edge_count 记去重后的补充边数（indy 展开边是 BSM 可解析时的精确执行目标，直连射入不经族展开）。lambda / method_handle / reflection 边缺失不自报列（refs kind 分类落地前是全局已知缺口，coverage_mask bit0 即其占位），分类线落地后扩位。
- 存储与查询形态：closure_members 每行 16B 量级（WITHOUT ROWID 主键即数据），十万级方法语料闭包集的存储增量 PoC 实测 §7 M7；查询「member 是否可达（最新有效批）」与「闭包全集」都走 (batch_id, member_id) 主键与 closure_members_member 反查索引。
- 「最新有效批」的选取是消费面约定不是存储约束：同 fingerprint 多批并存时取 batch_id 最大且已收尾者；残批（finished_at 空串）跳过——与注册表审计语义一致。

.. _reach-closure-invalidate:

5. 决策点 4：失效 = 指纹不匹配整体重算，schema v9 fail-loud
-----------------------------------------------------------

输入图指纹（32 字节 SHA-256）覆盖闭包计算的全部输入：

F = SHA-256( fp_version ‖ inputs 投影 ‖ classes 聚合 ‖ interfaces 聚合 ‖ members 聚合 ‖ refs 聚合 )

- inputs 投影 = (input_id, path, size, sha256) 逐行升序拼接（inputs revision，票面口径）。
- classes / members 聚合 = (COUNT(\*), MAX(id), SUM(id))——闭包的节点域与 CHA 层次基底（super/interfaces 事实随类行）。
- refs 聚合 = (COUNT(\*), MAX(ref_id), SUM(ref_id))——refs 全部写入口（整库替换、成员切片替换）都是删后重插，AUTOINCREMENT 保证 ref_id 单调不复用：任一行增删必然改变三元组之一。同内容重写也会改变 ref_id 集（假阳性失效→重算，fail-safe 方向），假阴性（改了图而聚合不变）经公开写入口不可达。
- 聚合口径是变更敏感而非碰撞抗性定位：指纹匹配 = 「输入状态自上次计算以来未动」，用于复用判定；不为内容寻址，不承诺密码学抗碰撞（重算代价低，误判方向是重算）。
- 计算输入判别段（v10）：精度档非缺省或 indy 射入清单非空时，图载荷尾部追加判别段（档位字节 + indy 边清单 sort+unique 后定宽编码）再整体哈希——精度档折进批次行 precision 列参与三匹配，indy 清单逐边影响闭包集故折进指纹。判别段只在非缺省档存在：CHA 无射入批的载荷与 v9 逐字节一致（CHA 批次回归门）。

失效语义：指纹不匹配 → 拒复用，整体重算落新批次；不做增量修补（增量闭包修补需要脏传播面，成本与收益不匹配，整体重算在 PoC 规模秒级完成）。复用判定 = 指纹 + seed_mask + precision 三匹配（v9 双匹配、v10 增精度档轴）——种子档位与精度档都是计算输入的一部分，同图不同档位的闭包集不同，不可互复用（档位落在 closure_batches.seed_mask / precision 两列，fresh 判定在批次行上比对）；indy 射入清单则折进指纹载荷（逐边影响闭包集，仅靠 indy_edge_count 撑不起互斥，见 §5 判别段）。改名/编辑后类与成员身份稳定（id 只增不删），refs 与类事实的变更经聚合捕获；重命名覆盖层不参与指纹（显示名与可达性无关）。

schema 版本 v8 → v9，fail-loud 版本门纪律延续：\ ``kAnalysisDbSchemaVersion = 9``\ ，打开时 user_version 连同 application_id 与 meta.kind 一并校验，不识别的版本拒绝打开（旧 v8 库拒绝，给可诊断文案，对齐 0.0.14-0.0.17 的版本门风格），不提供自动迁移，旧库处置 = 重导入重建。closure 两表与 batches.kind 新词只进 v9 DDL，不 touch 既有表列序。

6. 计算面与查询面
-----------------

计算面落 ``mjbase/src/mjbase/project/reachability_closure.{hpp,cpp}``\ （与 analysis_db 同模块，消费其读取面与写入面；不新增 target）：

- 入口 ``computeAndStoreReachabilityClosure(db, producerId, options)``\ ：指纹、seed_mask 与 precision 三匹配已收尾批 → 复用不重算（reused=true）；未命中 → 装载图、算闭包、落批。种子三档叠加（entry_points / keep_roots / 显式 member_id 清单，未知 id 拒绝），全空拒绝；precision 选档（CHA 缺省 / RTA，§3 RTA 口径）；indy 补充边经 options 射入（两端都必须是已登记方法，未知 id 拒绝，去重后并入邻接并自报 coverage bit1）。
- 主算法：worklist 闭包（FIFO）。层次等价族从 DB 类事实构建并查集（super 边 + 接口边合并同签名虚方法，同 MethodHierarchy override 族语义），族内具体方法按族缓存一次预算；CHA 档方法边一律保守族展开（refs 无 opcode 位，静态/私有目标族退化为自身；M5 实测保守口径与带 opcode 口径差集 +927 行 / +3.1%，方向恒为超集，§7）；RTA 档按 §3 口径实例化集合收窄（Type 引用行证据 + 挂起直连目标与已探族回灌两路 fixpoint 回灌）；clinit 触发边按 §3 口径（Type/Field 引用 + 方法边，沿 super 链传递，两档同口径）。邻接获取按 PoC 数据裁决（§7 M2）：per-member 预编译 SQL 反查 vs 批量快照装载。
- 确定性：种子升序入队、邻接按 target 升序去重、FIFO worklist、族展开升序去重、indy 射入清单 sort+unique、RTA 回灌按装载序——同输入双算成员集逐字节一致（验收项，双档各自成立）。
- 原子写：openBatch（closure_write 批）后，closure_batches / closure_members / batch_scopes 单事务落库，closeBatch 收尾；中途失败留残批不写事实行——批次是逻辑运行记录不是事务，残批合法。
- 查询面（最小集，不做过）：\ ``listClosureBatches()``\ （批次 + 指纹 + seed/coverage 元数据 + member_count，即 stats 面；inputId 维度不预设——闭包批次是全库粒度，「最新有效批」取 batch_id 最大且已收尾者）、\ ``closureMembersPaged(batchId, limit, offset)``\ （member_id 升序分页，契约同既有引用分页）、\ ``isClosureMember(batchId, memberId)``\ 、\ ``freshClosureBatch(fingerprint)``\ （复用判定面）。

7. PoC 数字（混淆语料 A）
-------------------------

语料：混淆语料 A（公开仓 huzpsb/JavaObfuscatorTest 的本地样本，4179 类的重度混淆 jar）。口径：mjbase AnalysisDb 整库装载（类事实 + refs 全量），闭包计算面为 PoC 原型（worklist + 并查集族 + clinit 触发）。两档种子：entry_points = main 发现；keep_roots 档 = public 类 × public/protected 具体方法（keep_roots 档位投影）。Windows 本机（MSVC x64 release，单进程单线程）。

语料与图规模：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 计量项
     - 数字
     - 说明
   * - 类 / 成员 / 方法
     - 4179 / 54178 / 43157
     - AnalysisDb 整库装载
   * - refs 行
     - 785449
     - method 147063 / field 61433 / type 576971
   * - 方法边分支
     - id 分支 78752 / 文本分支 68300
     - 文本分支链落解命中 6156；id 分支悬空 0
   * - 容器扫描 / 类装载
     - 165 ms / 599 ms
     - \ 
   * - refs 装载（分块口径）
     - 627.7 s
     - 20 类/块 index build 累计 611.8 s；全容器单次 build >20 分钟未收敛（§8）
   * - CG 构建（对照图）
     - 3.6 s
     - classpath call_graph，失败 0；indy 展开边 2074
   * - 邻接源覆盖
     - DB 29501 / CG 27920 / indy 1822
     - 有出边的方法源数（三方口径）
   * - 种子
     - entry 6 / keep_roots 21347
     - main 发现 / PublicProtected 投影

闭包与一致性：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 计量项
     - 数字
     - 说明
   * - D1 直接闭包一致性
     - DB 997 / CG 994；仅 DB 3 / 仅 CG 0
     - DB 图直接闭包 ⊇ CG 图，差集来自 clinit 首触边（CG 直闭不含 Type/Field 首触）；无 CG 有 DB 无的反例
   * - M2 邻接获取
     - per-member SQL 27.4 ms（762 行）vs 批量快照 1.0 ms（997 行）
     - 批量快照装载胜出，闭包面采用批量口径（§6）
   * - M3 entry 闭包
     - 9372 行 / 24.0 ms
     - 方法的 21.7%；族展开入队 7970 / clinit 入队 769
   * - M4 keep_roots 闭包
     - 29999 行 / 62.9 ms
     - 方法的 69.5%；族展开入队 19000 / clinit 入队 1089
   * - M5 保守口径差集
     - 30926 行（+927，+3.1%）/ 83.1 ms
     - 无 opcode 全族展开 vs 带 opcode CHA；方向恒为超集，过近似可控
   * - M6 indy 补充边净增量
     - 0
     - 2074 条展开边全部被 refs 编码 handle 行覆盖（§2 裁决的实测支撑）
   * - M8 确定性
     - 双算逐成员一致
     - 同库双算成员集相等
   * - M9 指纹聚合
     - 142.3 ms
     - §5 全部聚合的装载 + SHA-256
   * - M7 落库增量
     - +0.35 MiB / 29999 行
     - v9 草案 DDL 干跑（closure_batches + closure_members + scopes）

结论：闭包计算本身秒级以内（keep 档 63 ms），成本集中在 refs 装载（§8 规模观察，重导入动线由导入桥消化）；保守口径的过近似 +3.1% 可控；indy 补充射入在本语料净增量为零，coverage_mask bit1 缺省 0 的口径成立。

8. 已知边界
-----------

- refs direct-only：lambda / method_handle / reflection 形态的边在 refs 表无 kind 分类。BSM MethodHandle 的编码目标行已在 refs（ReferenceIndex 对 indy 指令的 handle 引用入库），但 lambda bridge/接口适配边与反射执行边缺失；闭包在这些点欠近似，结果批次带 coverage_mask 标记，refs kind 分类线落地后重算即收益。
- 方法体外的控制流不敏感：闭包是集合级保守闭包，不做路径可行性、别名或反射字符串解算（reflection 数据流事实在 ReferenceIndex 有产出口，本批不接线，留 RTA 后置批与反射织入线）。
- 外部符号不入闭包：refs 文本分支的未登记目标（JDK、supporting jar）不是 members 行，无 member_id 节点；闭包域 = 库内已登记成员，文本分支只保留其已登记 owner 类的 clinit 触发（§6）。PoC 的文本分支行数与命中数（§7）给出该域的边界量级。
- 虚分派族含祖先链声明的保守余量（§3）。
- RTA 精度档的实例化证据是 Type 引用行全族（new/checkcast/instanceof/ldc class 不可分，refs 无 opcode 位）——实例化集合是真 new 目标集的超集，收窄后仍不过近似；反射建实例与 classpath 外实例化（反序列化、SPI、外部代码持内部类型实例回灌）不在证据集内，RTA 闭包在这些点欠近似——铁律：RTA 结果不直接作删除依据，删除判死用 CHA 档或 keep 根面（coverage bit0 自报该覆盖度缺口）。
- clinit 触发按「可能首触」保守处理（refs 无 opcode 位），宁多收不少收。
- ReferenceIndex 全容器 build 的规模特征（PoC 实测观察，非本批改动项）：混淆语料 A（4179 类）全容器单次 build 超过 20 分钟未收敛（含 org/objectweb/asm/ClassReader 单类 20.8s 量级的病态大类），20 类/块分块（209 块）index build 累计 611.8 s、分块装载全流程 627.7 s（§7）——闭包批次的重算动线若走「重导入 + 重算」路径，重导入成本由导入桥（分块或增量切片）消化，闭包计算面本身秒级（§7）；该观察同时是 refs kind 分类线与扫描面性能批的输入。

9. 不做什么
-----------

RTA 精度档（实例化集合收窄）已随 v10 落地（§3 RTA 口径）；入口可达性驱动的边裁剪是闭包 worklist 的固有形态（不可达方法的边不展开）。不改 refs 表：无 opcode/kind 扩列（两线互喂的独立批），闭包的 opcode 需求由计算面在装载期以保守口径满足（CHA 全族展开 / RTA 实例化集合收窄），schema 层不引入第三种事实形态。不做增量闭包修补：指纹不匹配整体重算。不做 per-seed 闭包维度：无消费面需求，行膨胀不成比例。不做原地迁移：v8 库 fail-loud 拒绝，重导入重建。不动 GlobalReachability / ResolvedCallGraph：内存态可达性设施与 DB 闭包并存，语义口径对齐（§3）但代码不分层复用——前者吃容器，后者吃库，输入域不同。

10. 验收
--------

- 闭包 producer 注册 + 批次落库 + 失效往返单测：指纹不匹配拒复用、匹配复用（同一库连算两次，第二次命中复用）。
- 闭包 ⊇ 直接边传递闭包的一致性断言（运行时生成语料，jasm 汇编，不依赖外部语料文件）：同语料双算对照 + 虚分派展开差集断言（CHA 闭包 ⊇ 直接闭包，且差集非空用例）。
- clinit 触发用例（new / static 首触 → 目标类与 super 链 clinit 入闭包）、default 方法冲突用例（冲突族全部入集）、指纹失效往返、版本门对 v8 库拒绝并给可诊断文案、producer 批次归属与 scope 对账、确定性（双算逐字节同）。
- RTA 精度档验收：同图同种子 RTA ⊆ CHA 且零 Type 证据时真子集（虚分派目标全体收窄）、Type 证据到达后收窄解除回 CHA 全集、晚到实例化的挂起回灌（边先于触发处理仍可达）、静态/`<clinit>` 不收窄；indy 补充边验收：射入目标直连可达、coverage bit1 + indy_edge_count 写读往返、端点未登记拒绝、同射入集 canonical 去重复用；复用门验收：同图同种子双档不互串复用（指纹判别段 + precision 列双保险），fresh 判定三轴逐轴钉死。
- 全量 unittest 与基线红集合对比零新增红。
- PoC 规模数字入本文档 §7（已完成）；schema 变更走版本门 fail-loud（v9）。
