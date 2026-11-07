.. _pipeline-config-spec:

声明式管线配置规格（pipeline-config-spec）
==========================================

状态：定稿（phase 1：JSON 装载 + fail-closed 校验门 + 声明结构；参数注入与内建展开接线见 :ref:`pipeline-config-spec-phases`\ ）。

把「代码拼 pass 链」变成声明式：外部描述文件（JSON）回答「跑哪些 pass、什么顺序、每件什么参数」，装载面解析并校验后交付声明结构，执行编排消费该结构装配管线。本规格只冻结装载与校验面（\ ``mjbase/spi/pipeline_config.hpp``\ 头注为微观契约层）；执行编排、pass 契约与注册表纪律不在本文范围，见 :ref:`pass-contract`\ 。

.. _pipeline-config-spec-position:

定位与单职责
------------

装载面只做「声明 → 结构」：JSON schema 解析 + 校验门 + 产物结构，不注册 pass、不改注册表、不另起执行路径。三张注册表面在装载期只读：

- \ ``PassNameRegistry``\ 全集（\ ``defaultPassRegistry``\ 或宿主经 :ref:`plugin-api`\ 装配面装入 pack pass 后的自持表）——pass 名校验与段位查询；
- \ ``PassRegistry``\ 内建管线集（\ ``registerBuiltinPipelines``\ 之后有效）——内建管线前缀引用校验；
- per-pass 参数 schema 声明表（装载面内置，见 :ref:`pipeline-config-spec-phases`\ ）——参数键与值家族校验。

消费形态：宿主读配置文本 → \ ``loadPipelineConfig``\ 得声明结构 → 逐条投入 \ ``PipelineBuilder``\ （直引条目可经 \ ``applyConfigTo``\ 复用 \ ``applyTo``\ 段位分派）→ 既有 \ ``buildNestedPipeline``\ /\ ``runContainerPipeline``\ 驱动，不改变任何编排语义。

.. _pipeline-config-spec-schema:

JSON schema（version 1）
------------------------

顶层对象三个字段，字段严格（未知字段、重复字段、类型错一律拒，与 analysis-project 的工程文件同一纪律）：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 字段
     - 形态
     - 语义
   * - ``version``
     - 必填整数
     - schema 版本；版本门只识别 1（\ ``supported: 1..1``\ ），不识别即拒，detail 标注 supported 区间（对齐 analysis-db 版本门风格）
   * - ``passes``
     - 必填非空数组
     - 管线序列，声明序即执行序；条目两形态见下
   * - ``disable``
     - 可选字符串数组
     - 显式禁用名单；条目须为注册表已登记的 pass 名（非空字符串），名单内不重复

序列条目两形态：

- **字符串形**\ ：pass 注册名（含 \ ``<pack>.``\ 命名空间名）或内建管线名；
- **对象形**\ ：\ ``{"pass": "<名>", "params": {…}}``\ 。\ ``pass``\ 必填非空字符串；\ ``params``\ 可选对象（\ ``pass``\ 条目专属，内建管线引用携带即拒）。

参数值家族（phase 1 标量族）：\ ``Bool``\ （JSON bool）、\ ``Int``\ （JSON 整数）、\ ``Float``\ （JSON 浮点，int 字面量按数值家族晋升接受）、\ ``String``\ （非空字符串）。复合值（对象数组等）phase 2。参数对象内重复键拒；键的合法性按声明表逐键裁决（未知键拒、值家族与声明 kind 不符拒）。

示例（合法配置）：

.. code-block:: json

   {
     "version": 1,
     "passes": [
       "default-optimize",
       "NopRemover",
       {"pass": "LocalVariableAnnotator",
        "params": {"promoteTypes": false, "replaceNames": true, "prefix": "v"}},
       "dasho.ObfuscateStrings"
     ],
     "disable": ["UnusedCheckCleaner"]
   }

.. _pipeline-config-spec-api:

装载与装配 API
--------------

- \ ``loadPipelineConfig(text, registry, pipelines)``\ ：装载 + 校验全门（\ ``pipelines``\ 缺省取 \ ``PassRegistry::instance()``\ ），成功返回自持的 \ ``LoadedPipelineConfig``\ （版本、声明序序列、禁用名单；条目带注册表查得的段位与参数表），失败返回 \ ``PipelineConfigError``\ （kind + 可诊断 detail，定位前缀形如 \ ``passes[2]:``\ ）。
- \ ``applyConfigTo(registry, config, builder)``\ ：声明结构 → \ ``PipelineBuilder``\ 装配。直引 pass 条目逐条 \ ``applyTo``\ （复用既有段位分派）；\ ``disable``\ 命中的直引条目跳过；内建管线引用 phase 1 不展开（builder 无合并 API），装配期遇之即 \ ``Err(BuiltinRefNotExpandable)``\ 。
- 幂等装载：装载是纯函数（读注册表、无全局可变状态），同一文本对同一注册表重复装载逐字段一致（产物带相等比较面）。
- 线程纪律：装载读全局注册表，对齐注册表纪律——初始化阶段调用，非线程安全。

.. _pipeline-config-spec-gates:

校验门清单（fail-closed）
-------------------------

逐门顺序执行，命中即拒并携带定位与口径说明（kind 名与 \ ``pipeline_config.hpp``\ 的错误码枚举一致）：

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 门
     - 错误码
     - 裁决口径
   * - JSON 语法
     - ``BadJson``
     - 解析器行列号与消息原样带入 detail
   * - 顶层字段严格
     - ``InvalidField``
     - 未知字段、重复字段、缺字段、类型错（含条目形状：字符串或 \ ``pass``\ /\ ``params``\ 对象两形态之外一律拒）
   * - 版本门
     - ``SchemaVersion``
     - 只识别 1；非整数 \ ``version``\ 归 \ ``InvalidField``
   * - 空序列
     - ``EmptySequence``
     - \ ``passes``\ 空数组拒；\ ``disable``\ 把声明条目全数减空同样拒（内建引用不受禁用面影响）
   * - 未知名字
     - ``UnknownPass``
     - 序列与禁用名单逐名对 \ ``PassNameRegistry``\ 全集查名，未命中拒；查名序为先 pass 注册表、后内建管线集，未命中 detail 对点名字提示 pack 命名空间口径
   * - Cf 域
     - ``ScopeUnsupported``
     - classfile 级 pass 无容器管线编排槽位（\ ``applyTo``\ 的 \ ``ScopeMismatch``\ 在装载期前置），装载即拒
   * - 参数键
     - ``UnknownParamKey``
     - 逐键对声明表裁决；声明表未登记该 pass 时携带任何参数即 \ ``ParamSchemaUndeclared``\ 拒（schema 缺位不得静默丢参；不携带参数则与声明表无涉）
   * - 参数值
     - ``ParamTypeMismatch``
     - 值家族与声明 kind 不符；\ ``String``\ 恒非空；空 \ ``params``\ 对象恒合法
   * - 重复条目
     - ``DuplicatePass``
     - 非幂等件逐一登记表（现登记 \ ``StackTraceRecover``\ ——每轮追加打印调用与行号，\ ``JunkCodeInserter``\ ——逐轮叠加噪声块）重复出现拒；幂等件允许重复（清理件长链尾再收一轮是合法配方）；登记依据各 pass 在 :ref:`pass-catalog`\ 的链序行，后续批次按同一口径逐件增补
   * - 禁用名单
     - ``DuplicateDisable``
     - 名单内重复条目拒；禁用名单只收 pass 名（内建管线名不在禁用面）

.. _pipeline-config-spec-phases:

per-pass 参数：分阶段口径
-------------------------

per-pass 参数表按「声明先行、注入随后」分阶段交付；每阶段扩面不改 phase 1 的校验门语义。

.. list-table::
   :header-rows: 1
   :widths: auto

   * - 阶段
     - 交付面
     - 口径
   * - phase 1（本批）
     - 声明表 + 标量族校验 + 值承载
     - 声明表从 pass 头注配置面提炼，首件为 \ ``LocalVariableAnnotator``\ 的 \ ``LocalVariableAnnotatorOptions``\ 三参（\ ``promoteTypes``\ 与 \ ``replaceNames``\ 为 \ ``Bool``\ ，\ ``prefix``\ 为 \ ``String``\ ）；产物承载参数值，注入不接线——注册表工厂无参是既定契约，携带参数的直引装配 phase 2 落
   * - phase 2
     - 复合参数 + 工厂闭包注入 + 内建展开
     - 复合参数（\ ``MethodCallRemover``\ 的点名规格对象族）；参数 → 工厂闭包注入接线（对齐 :ref:`plugin-api`\ 的 config 闭包模型）；内建管线展开（builder 合并/前缀 API）与禁用名单对展开结果的减除
   * - phase 3
     - pack 参数 schema + 全量参数普查
     - pack 自治 schema 的宿主侧校验面；注册表全集逐件对头注/RST 配置面普查提炼，声明表补齐

内建管线引用与禁用名单
----------------------

内建管线是配置中的可引用前缀：序列条目命中 \ ``PassRegistry``\ 名（\ ``default-optimize``\ 或 \ ``phase.*``\ 族）即标记为引用条目，展开归执行编排（phase 2）。查名序为先 pass 注册表、后内建管线集——两表撞名时 pass 名赢（注册表撞名有 \ ``DuplicateName``\ 门，内建管线名不占 pass 名空间）。\ ``PassRegistry``\ 未 bootstrap 时内建引用按未知名拒。

禁用名单的语义是「从序列中减除」：phase 1 落地面为直引条目的名字级跳过（\ ``applyConfigTo``\ ），对内建展开结果的减除随 phase 2 接线。禁用名对 \ ``PassNameRegistry``\ 验名——禁用一个未注册名是配置错误，fail-closed。

与 plugin 面的咬合
------------------

pack 经 \ ``installPack``\ 注册的 \ ``<pack>.``\ 命名空间 pass 进入 \ ``PassNameRegistry``\ 后即被管线配置引用——装载校验对注册表全集查名，plugin 注册集无需单独通道。宿主未装配对应 pack 时引用按未知名拒，detail 提示点名的 pack 命名空间口径。装载面与 \ ``pack.hpp``\ 头注的宿主装配配方衔接：装配（\ ``installPack``\ ）在先，配置装载在后，共享同一张注册表。
