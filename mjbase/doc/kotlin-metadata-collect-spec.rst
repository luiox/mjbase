.. _kotlin-metadata-collect-spec:

Kotlin Metadata 收集件规格（KotlinMetadataCollector）
================================================================

状态：已落地（v1 解析口径见「边界申报」节）。本件为 Recaf 侦察第四吸收项
（``KotlinMetadataCollectionTransformer`` 的收集 + 描述符映射工具链能力面，
架构 mjbase 化重组），2026-11-07。

1. 动机与单职责
----------------------------------------------------------------

Kotlin 编译器把类/函数/属性的完整声明真相写进类级
``RuntimeVisibleAnnotations`` 的 ``Lkotlin/Metadata;`` 注解体（d1 protobuf
载荷 + d2 串表），混淆器可以改名类与成员，却通常既不重写也不摘除这段
注解——它构成可验证的 Kotlin 真名来源。本件逐类读该注解，解码产出

* Kotlin 声明模型（类/构造器/函数/属性 + 类型与可空性），与
* 「容器二进制名 ↔ Kotlin 真名」双向类名映射，

挂在容器 AnalysisManager 上交付。另配套一组**纯函数描述符映射工具**：Kotlin
模型 → JVM 描述符（``ktTypeDescriptor``/``ktFunctionDescriptor``）、kotlin
stdlib → JVM 类型替换（``kotlinToJvmDescriptor``）、收集真名 → 二进制名的
描述符反向重写（``reverseMapDescriptor``）。

**单职责红线**：只产分析与映射，零字节码改动（pass 恒返回
``PreservedAnalyses::all()``）。按收集面匹配成员并改名的「还原件」是独立
组合件，不在本件（后续批）。

2. 与 KotlinMetadataRemover 的边界
----------------------------------------------------------------

两者读同一注解槽、互不依赖：Remover（删除面）按
``Lkotlin/Metadata;`` 槽位过滤并删除类级可见注解；本件（收集面）读同一槽位
解码出模型。先收集后删除是合法组合次序；删除后本件对该容器产空结果。本件不
注册进默认 pass 表（``opt-in``），Remover 的默认注册面不受本批影响。

3. 解析链（fail-closed）
----------------------------------------------------------------

逐类独立解析，任一门失败该类零产出并返回 ``KotlinMetadataError`` 码（不产
半截模型；容器收集时失败类跳过、不影响其他类）；畸形类资源跳过（class_scan
fail-open 同口径）。

3.1 注解面
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

只读类级 ``RuntimeVisibleAnnotations`` 中 ``Lkotlin/Metadata;`` 条目（与
Remover 读面同域：不查 invisible 与 type annotation），直采结构化注解值：
``k``（kind）、``mv``（metadata 版本）、``xi``（extra int）、
``xs``/``pn``（extra string）、``d1``（编码载荷串表）、``d2``（字符串表）；
``bv`` 等 v1 不消费键忽略。
``k``/``d1``/``d2`` 任一缺席或元素类型不符即 ``BadAnnotationShape`` 拒。

3.2 d1 解码（BitEncoding 双形态）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

按 Kotlin ``BitEncoding.decodeBytes`` 语义：首串首字符 ``U+0000`` 为 UTF8
形态（逐字符低字节直取）；``U+FFFF`` 为 7to8 形态（去标记符，逐字符低字节
合并后逐字节 ``+0x7F mod 128``，再 7bit 重排回 8bit）；无标记符按裸 7to8。
CP 串按 modified-UTF8 语义解码为 UTF-16 码元后取低字节（``0xC0 0x80`` 特例
还原 ``U+0000``）。解码中途越界/形状不符即 ``BadData1Encoding`` 拒。

3.3 载荷切分与串表
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

解码后的字节流按 ``parseDelimitedWith`` 口径切两段：varint32 长度前缀的
``StringTableTypes`` 在前，主消息紧随其后。串表记录（record）按 ``range``
展开；逐索引取串优先级：``record.string`` → 预定义串表（Kotlin
``JvmNameResolverBase`` 的 44 条）→ ``d2[index]``；随后依次应用 substring
（前两个 ``substring_index``）、字符替换（前两个 ``replace_char``，ASCII
字节级）、操作（1 = 把 ``$`` 换成 ``.``，2 = 去首尾 ``L``/``;`` 再把
``$`` 换成 ``.``）。
``range`` 非正或展开总量超出 ``d2`` 大小即 ``BadStringTable`` 拒。

3.4 主消息（protobuf 线格式子集）
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

按 ``k`` 分派：1 = ``ProtoBuf.Class``，2/5 = ``ProtoBuf.Package``，3 =
``ProtoBuf.Function``；0/4（多文件类门面）v1 整类拒（``UnsupportedKind``）。
线格式只认 varint 与 length-delimited 两形态；截断、字段形状不符、索引越界
即 ``BadProto`` 拒。类名与类内引用名统一产出**内部名形**（``.`` 转 ``$``，
与 Recaf 口径一致）。

id 形类型引用（``supertype_id``/``return_type_id`` 等）经该消息自有
``TypeTable``（field 30）展开，首条 ``nullable`` 向表内条目传播可空性。
proto 允许 ``type_table`` 串行化于成员字段之后，故全部**两遍解析**：循环内
只收字段，表落定后再展开引用。

4. 与 Recaf 原件的设计差异
----------------------------------------------------------------

**吸收面**：收集判据链（注解 → BitEncoding → 串表 → proto → 模型）、
``mapKtDescriptor``/``mapToJvm``/``reverseMapDescriptor`` 的映射价值面、
stdlib 替换表同表搬运。**放弃面**：Recaf 经 ``AggregatedMappings`` 全局产物
与变换器编排的形态（mjbase 以容器 AM 惰性分析替代）；按收集面匹配成员改名的
执行面（还原件，后续批）；``@JvmName`` 注解兜底匹配（后续批）。

**两处口径增强**，相对 Recaf ``KotlinMetadataVisitor``：

1. 扩展接收者（``Function.receiver_type``）进函数描述符作 JVM 首参——Recaf
   丢弃 ``receiver_type``，扩展函数描述符因此缺首参；
2. id 形类型引用经 ``TypeTable`` 展开——Recaf 只读 inline 形，共享类型场景
   失配。

5. 描述符映射工具链口径
----------------------------------------------------------------

* ``ktTypeDescriptor``：Kotlin 模型 → L 形字段描述符。泛型一律擦除；星号
  投影与四类类型名全缺席落 ``Ljava/lang/Object;``；``kotlin/Array`` 特例取
  首实参递归拼 ``[<首参>``（首实参缺位退 ``Ljava/lang/Object;``）。
* ``ktFunctionDescriptor``：参数按声明序，``hasReceiver`` 时扩展接收者作首
  参；未给返回类型落 ``Ljava/lang/Object;``。
* ``kotlinToJvmDescriptor``：Recaf ``mapToJvm`` 同表替换
  （``Lkotlin/Int;`` 映 ``I``、``Lkotlin/Unit;`` 映 ``V``、
  ``Lkotlin/Any;`` 映 ``Ljava/lang/Object;``、collections 映
  ``java.util``、``FunctionN`` 映 ``kotlin.jvm.functions`` 等 56 条）；
  逐描述符段替换，未命中段原样保留，非描述符文法输入原样返回。
* ``reverseMapDescriptor``：描述符文法内逐 ``L…;`` 段查收集真名，命中即替
  换为容器二进制名（数组前缀保持）；未命中返回内容相同的串。

收集产物示例：``k=1`` 类 ``com/example/Host`` 内真名 ``com/example/Foo`` 的
嵌套类条目，使 ``reverseMapDescriptor("LFoo;")`` 得
``"Lcom/example/Host$Foo;"``——还原件据此把被混淆描述符对齐回 Kotlin 真名
域（本批只供映射查询，不执行改名）。

6. 消费方式
----------------------------------------------------------------

收集结果挂在容器 AM 上：``GlobalAnalysisManager::getResult
<KotlinMetadataCollectionAnalysis>(container)``（惰性计算并缓存；同容器二跑
直接命中缓存）。pass 壳 ``KotlinMetadataCollector`` 标签
``scope:global``/``action:collect``/``stability:opt-in``，**不进默认注册表**
（收集是分析面，本批 ``default_pass_registry`` 不动）；测试与外部消费方直接
对容器 AM 调 ``getResult``，或自行注册壳后触发。

7. 边界申报（v1）
----------------------------------------------------------------

1. **k=4（多文件类门面）整类拒**：``UnsupportedKind``；k=2/5 文件门面与
   k=3 lambda 的 ``name`` = 定义类二进制名（非 Kotlin 真名），如实产出。
2. **类消息只收声明主干**：``type_parameter``/``nested_class``/``enum_entry``
   /``sealed_subclass``/``context_receiver`` 等不进 v1 模型；属性接收者与
   setter 参数不进面（还原匹配面未定，申报边界）。
3. **构造器参数只认 inline 类型形**：构造器消息无自有 ``TypeTable``，id 形
   参数类型零产出（不拒类）。
4. **类型参数名按 Recaf 口径采串表 resolve**，字段 ``type_parameter``，非
   TypeParameter 列表索引；该面只进诊断模型，不进描述符映射面。
5. 畸形面全部整类拒（无部分产出）；串表 ``d2`` 越界、proto 截断、注解形状
   不符同口径。

8. 测试面
----------------------------------------------------------------

``test/unittest/pass/kotlin_metadata_collection_test.cpp``：jasm 装配夹具全
矩阵（JvmJasmService 汇编类骨架 + 注解体经 CF 级 typed
``AnnotationsAttr`` 写面构造——jasm 方言不表达注解元素值，异常形状单点例外
走 relaxed validate 双侧校验）。覆盖：k=1/2/3/5 四形态收集（真名/伴生/父类
/扩展函数/属性）、无注解类排除、畸形体逐码拒（shape/d1/串表/proto/kind）、
幂等二跑（同 AM 缓存命中 + 独立 AM 重算相等）、与 KotlinMetadataRemover 独
立共存、字节零改动；描述符工具面（L 形双向、stdlib 替换 56 表抽样、扩展接收
者首参、泛型擦除 + Array 特例、reverseMap 命中与未命中）。javac 真类消费闭
环在 JVM 探针内跑（JDK 缺席 GTEST_SKIP）。
