.. _kotlin-name-restoration-spec:

Kotlin 名还原映射生成件规格（KotlinNameRestorer）
================================================================

状态：已落地（v1 判据口径见「判据（逐槽）」节，边界见「边界申报」节）。本件
为 Recaf 侦察吸收项（``KotlinNameRestorationTransformer`` 还原判据能力的
组合式吸收 + 架构 mjbase 化重组），2026-11-08。

1. 动机与单职责
----------------------------------------------------------------

混淆器可以改名 Kotlin 类与成员，但通常既不重写也不摘除
``Lkotlin/Metadata;`` 注解——收集件（``KotlinMetadataCollector``）已把这段
注解解码为声明模型与「容器二进制名 ↔ Kotlin 真名」双向映射。本件消费该模型，
把「混淆名 → Kotlin 真名」落成可执行的家族改名映射（
``IntermediateMapping``），还原被混淆的类名、属性 backing field 名、属性
访问器名与函数名。

**单职责红线**：只产映射，零字节码改动——pass 恒返回
``PreservedAnalyses::all()``，改名执行交映射消费面（改名件/导出面）。
ENR（``EnumNameRestorer``）同款口径：映射生成与映射消费分离，本件不渲染、
不重写、不落盘。

2. 判据（逐槽）
----------------------------------------------------------------

逐类独立分析，单类零产出不影响其他类；畸形类资源跳过（class_scan fail-open
同口径，与收集件一致）。成员匹配的证据面是**描述符**：metadata 不携带混淆后
的名字，只有「Kotlin 视角的类型形状」可以和容器实际成员对上。

2.1 类名槽
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

类名真值源有两路，优先级为**注解 > metadata**：

1. 类级 ``RuntimeVisibleAnnotations`` 中 ``Lkotlin/jvm/JvmName;`` 的
   ``name`` 串元素（kotlinc 对文件门面类落注解的工序面）。名字须过
   ``IdentifierUtil::isValidJavaIdentifier`` 校验（非标识符形状按无注解
   处理，fail-open）；目标名 = 当前二进制名的包段前缀 + 注解名（根包无
   前缀），且目标名与当前名不同才产条目。
2. 注解缺席时用收集模型 fqName 真名（``.`` → ``$`` 的内部名换算由收集件
   完成，本件直取）。

注解优先的依据：k=2/5（文件门面）场景下 metadata 模型名恒等于定义类二进制
名（无真名信息），注解是唯一真值源；普通类场景注解与 fqName 冲突时，
``@JvmName`` 是编译器落盘的显式用户声明，证据强度高于 fqName 推断（差异
申报见第 3 节第 2 条）。

2.2 描述符双形态匹配
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Kotlin 模型描述符先经两步纯函数换算（均来自收集件工具面）：

1. ``reverseMapDescriptor``：描述符里的 Kotlin 真名段按本容器收集到的
   「真名 → 二进制名」反向重写（``Lapp/Cell;`` → ``Lapp/c;``，本容器已
   混淆时才有映射）。
2. ``kotlinToJvmDescriptor``：stdlib 替换（``Lkotlin/Int;`` → ``I``、
   ``Lkotlin/Unit;`` → ``V`` 等）。

成员描述符与**任一形态**相等即候选（Recaf ``getCandidates`` 同口径：真名段
可能是本容器已收集的，也可能已被 stdlib 替换覆盖，单形态会漏配）。

2.3 字段槽
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

属性 backing field：属性类型描述符（``ktTypeDescriptor``）双形态后与本类
声明字段唯一命中即产映射（新名 = 属性名）。属性名空或类型面不全（无类型，
收集件以 Object 兜底描述符表示）不进匹配——兜底形状的匹配证据不足，误配
风险大于漏配收益（差异申报第 6 条）。

2.4 方法槽：getter-setter 对与函数
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

属性访问器与函数共用唯一命中口径（候选集 = 本类声明方法按描述符双形态
过滤）：

* getter：``()T`` 唯一命中，新名按 kotlinc 约定表——属性名自带
  ``get``/``is``/``do`` 前缀时原样（``isActive`` → ``isActive``），否则
  ``get`` + 首字母大写（``value`` → ``getValue``）。当前名已等于属性名时
  跳过（已正名，Recaf 同口径保守面）。
* setter：``(T)V`` 唯一命中，**仅当属性可变**\（收集件
  ``KtVariable::hasSetter``，来自 ``Property.setter_parameter`` 字段 6
  在场性）——``val`` 属性上偶然同形的用户方法（如 ``render(String)V``）
  不得误恢复。新名 ``set`` + 首字母大写；``is`` 前缀属性按 kotlinc 约定
  去 ``is``\（``isActive`` → ``setActive``）。
* 函数：``ktFunctionDescriptor``\（扩展接收者已被收集件展开为 JVM 首参）
  双形态唯一命中，新名 = 函数名；无返回类型或任一参数缺类型不进匹配。

2.5 合成名消解
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

名字含 ``$``\（``access$`` 转发、``$default`` 桥、``$$delegate_0`` 委托面、
lambda 编号——Kotlin 用户标识符软保留 ``$``）或 ``<`` 开头（``<init>``/
``<clinit>``）的成员：不进候选集、不收映射。动机：编译器合成成员常与用户
成员同描述符（如 ``access$getX`` 转发与 getter 同为 ``()T``），不剔除会抢
走唯一候选槽使真成员失配；也不给合成成员落真名映射。代价：混淆名自带
``$`` 的条目不恢复——宁可漏恢复不可错恢复（差异申报第 5 条）。

2.6 冲突消解与映射面最小化
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

同槽多真值源的优先级在处理序中落实（先写者胜，同 ``(desc, oldName)`` 槽幂等
跳过）：属性 field > getter/setter > 函数。已正名（旧名已等于新名）
零条目——映射面只含真正的改名需求（映射面最小化）；描述符多命中或零命中
一律放弃该槽（唯一匹配口径，metadata 无序无偏序证据，不猜）。

3. 与 Recaf 原件的差异申报
----------------------------------------------------------------

组合式吸收，判据同源、架构与口径差异逐条申报如下：

1. **架构重组**：Recaf 单 transformer 直改 ASM 节点（读时改）；本件拆为
   「映射生成分析 + 映射消费面」，本件只产 ``IntermediateMapping``，字节
   零改动（mjbase 只读 pass 家族口径）。
2. **JvmName 优先级**：Recaf 仅在无 metadata 模型时读注解（模型在场即
   遮蔽注解）；本件注解优先于 fqName——文件门面（k=2/5）模型名恒等于
   二进制名，注解是唯一真值源，Recaf 口径下该场景类名恢复失效。
3. **setter 面**：Recaf 原件无 setter 匹配；本件补 setter 槽并以
   ``hasSetter`` mutability 门保真（见 2.4）。
4. **扩展接收者**：Recaf 在还原时自行把 receiver 拼进 JVM 首参；本件
   复用收集件已展开的首参描述符（描述符面对齐，无二次展开逻辑）。
5. **合成名消解**：Recaf 只排除 ``<`` 开头；本件含 ``$`` 一并剔除候选集
   （见 2.5，代价申报同处）。
6. **类型面不全不匹配**：Recaf 对缺类型成员用 Object 兜底描述符参与匹配；
   本件类型面不全的属性/函数不进匹配（收紧，防兜底形状误配）。
7. **根包前缀**：Recaf 空包类经「空包 + ``'/'`` + 名字」产 ``/Name`` 形状
   目标名；本件根包无前缀（``Bare`` 而非 ``/Bare``）。
8. **映射面最小化**：已正名条目（旧名 = 新名）零产出；Recaf 落映射前无
   此统一跳过（getter 有局部等名跳过，字段/函数无）。
9. **注解名校验**：Recaf 以 ``\w{1,50}`` 正则校验 ``@JvmName``；本件用
   ``IdentifierUtil::isValidJavaIdentifier``\（完整 JVM 标识符文法，比
   ``\w`` 严——拒 ``-`` 等合法正则字符）。

3.1 收集件配套扩展
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``KtVariable`` 增 ``hasSetter`` 位：``Property`` 消息 setter_parameter
（字段 6）在场即置位，缺席复位——``val`` 属性恒 false，参数/构造器变量面
恒 false。这是本件 setter mutability 门的模型面支撑，为最小扩展（不改
收集件既有解析口径与测试面）；收集件规格不受影响，扩展位以
``modelSurfaceExposesSetterPresence`` 单测直证。

4. 消费方式
----------------------------------------------------------------

注册面：``KotlinNameRestorer``\（``scope:global`` / ``action:recover`` /
``stability:opt-in``，默认链不收）。映射通道：全局分析
``KotlinNameRestorationAnalysis``\（``Result = IntermediateMapping``）挂在
容器 ``GlobalAnalysisManager``，惰性计算并缓存（依赖收集件分析经同一 AM
自举注册）。消费方经
``getResult<KotlinNameRestorationAnalysis>(container)`` 取映射后交改名执行
面（``asm_remapper::MappingRemapper`` 或 ``MappingFormatRegistry`` 导出）；
分析幂等，重复 ``getResult`` 同址。

5. 边界申报（v1）
----------------------------------------------------------------

1. 构造器名、伴生对象命名（``Companion``）、``internal`` 名段、匿名内部类
   等其它 metadata 恢复面不在 v1（后续批候选）。
2. 含 ``$`` 的混淆名不恢复（合成消解的既定代价，见 2.5）。
3. 描述符多命中/零命中放弃该槽，不做启发式消歧（唯一匹配口径）。
4. 非类资源与畸形类字节跳过，不报错（fail-open 同口径）；注解读面仅
   ``RuntimeVisibleAnnotations``\（与收集件同域，不查 invisible 与 type
   annotation）。
5. 类名槽要求模型有名（``hasName``）且目标名不同于当前名；d2 缺 fqName
   的门面类仅注解路可恢复。
6. 顶层声明（k=2/5 门面）恢复限于 backing field/访问器/函数的标准形状；
   ``@JvmName`` 落在成员上的改名（方法别名面）v1 不消费。
7. 映射不含行号/调试信息面（``IntermediateMapping`` 家族口径）。

6. 测试面
----------------------------------------------------------------

单测 ``mjbase/test/unittest/pass/kotlin_name_restoration_test.cpp``，夹具
纪律：类骨架 jasm 文本装配（``JvmJasmService``）；``Lkotlin/Metadata;`` 与
``Lkotlin/jvm/JvmName;`` 注解 jasm 方言无法表达，经 CF 级 typed
``AnnotationsAttr`` 写面挂载（夹具纪律例外②申报，模拟上游 kotlinc 工序）；
d1 载荷编码器为收集件单测同口径复制件（申报：不回改已落库测试抽公共
helper）。覆盖：var 属性三槽 + 函数全恢复、扩展接收者、容器真名段反向
重写 + stdlib 替换双形态、``is`` 前缀访问器、setter mutability 门、合成名
消解（``access$`` 同描述符转发不抢槽）、属性访问器冲突优先、JvmName 兜底
（无 metadata / 注解优先 / 根包）、已正名空映射、多命中放弃、无注解空映射、
``hasSetter`` 模型面、字节恒等、幂等二跑、注册挂名。
