# 字符串/常量流分析（string_flow）

面向 issue #3：为下游 folding 类 pass 提供「哪些字符串常量只进不逃逸」，为解密内联类 pass 提供「哪段代码把哪段输入解成了什么」。分析只产出事实，不改写方法体。

## 结构

流格只记录值的来源令牌，不携带字符串内容：字面量记常量池索引与载入点，builder/数组记分配点，已识别调用记调用点，整数常量记值。控制流 worklist、cat2 槽布局、局部变量与栈迁移全部交给 `jbc::analysis::DomainEngine`，令牌随槽移动与合并。两个不同来源在汇合点相遇时合并为 Top（未知），因此循环与多路径不会留下伪造的确定值。

求值发生在数据流收敛之后。`Evaluator` 以到达帧为回溯依据，按令牌递归求值：字面量读常量池，builder 扫全方法的变更指令，数组按分配长度与元素写入重建，调用点按方法描述符取实参。递归带活动集与记忆化，遇到环或预算耗尽即返回不可求值。

## 识别的来源

| 形态 | 令牌 | 求值要点 |
| --- | --- | --- |
| `ldc` String | Literal | 常量池 modified UTF-8 解码 |
| `new StringBuilder/StringBuffer` | Builder | 收集 `<init>` 与 append/insert/reverse/setLength/setCharAt/delete/replace |
| `String.valueOf/copyValueOf/concat` | Call | 按描述符参数类别分别处理引用、char、int、boolean、char[] |
| `new String(char[])` / `new String(char[],int,int)` | Call | 数组闭环求值后构造 |
| `String.toCharArray/getBytes` | Call | 作为数组来源参与后续变换 |
| `new char[N]` / `new byte[N]` | Array | 常量长度决定初始内容，常量下标写入逐元素覆盖 |

`StringBuilder` 的变更按指令顺序聚合，跨语句、跨局部变量都成立。从分配点到最后一次变更之间出现分支、返回、抛出，或存在与该区间相交的回边时，闭环标记为不可求值——变更可能被跳过或重复执行。

## 编码管线

Base64：识别 `Base64.getDecoder()/getUrlDecoder()` 与 `Base64.Decoder.decode(String|byte[])`，标准与 URL-safe 字母表都接受，缺省 padding 自动补齐，解码交给 `libca_crypto`。解码结果经 `new String(byte[], charset)` 落成文本时，字符集只接受 UTF-8、US-ASCII、ISO-8859-1（字面量名或 `StandardCharsets` 常量）；不写 charset 的 `new String(byte[])` 依赖平台默认值，静态不可知，不做。

字符数组变换：`caload`/`baload` 在数组令牌上产生元素令牌，元素与整数常量的 `IXOR/IADD/ISUB` 产生变换令牌，`castore`/`bastore` 把它应用回数组。常量下标逐元素应用；非常量下标视为循环体对整个数组执行一次同样的变换。只有至少一次全数组变换才产出管线事实，输入记录变换前的完整内容。

数组逃逸检查保证闭环性：数组令牌到达未识别的消费点（未知调用、字段/数组存储、返回等）时，整段求值标记为不可求值。

## 事实面

`StringFlowSource`：`site`（ldc / 构造调用 / `toString` / `valueOf`）、`origin`、`value`、`complete`、`escaped`。`value` 是 modified UTF-8 字节，可直接交给 `ConstantPool::addUtf8`；空串与不可求值都表现为空视图，用 `complete` 区分。

`StringFlowPipeline`：`site`、`kind`（Base64Decode/Xor/CharShift）、`input`、`output`。Base64 的输入是编码串；数组变换的输入只在全数组循环变换时记录。逐元素常量写入不构成管线。

`escaped` 的判定扫描每条指令的到达栈：槽位搬运、类型检查、局部变量读写与空值比较算中性使用；其余字符串消费者（builder 变更、字符串构造、`valueOf`、Base64 解码）不算逃逸；返回值、字段与数组存储、监视器、未知调用算逃逸。

数据流失败（畸形方法体、无不动点）时 `summary().ok=false`，事实列表为空。结果自持驻留池，移动后视图片仍有效，但生命周期不超过结果本身。

## 终止

三个层次共同保证终止：流格合并单调收敛到 Top；引擎对访问次数设上限，超限报 `NoFixpoint`；求值器的记忆化、活动集与预算（50000 次令牌求值）阻止递归展开。自引用循环（循环头把字符串局部变量不断替换为新调用结果）在测试里锚定为正常收敛。

## 明确不做

(1) 跨方法、跨字段的字符串流；参数与字段加载一律是未知值。

(2) 依赖运行时 charset 的字节解码，以及反射、资源读取等外部输入。

(3) 数组内容依赖多次循环迭代累积的形态（例如每轮用上一轮结果作为下一轮输入）；同一 store 指令只应用一次变换。

(4) 非字符串调用返回值的建模；未知调用产出的引用是 Top。

## 验证

`mjbase/test/unittest/asm/analyzer/string_flow_test.cpp` 覆盖：字面量来源与 return 逃逸、builder 链与跨局部变量聚合、Base64 管线（输入/输出/逃逸位）、char[] 常量填充、char 偏移循环（含 `i2c` 与全数组变换）、默认 charset 的不可求值、自引用循环的终止与事实数量。测试用手写常量池与方法体，不依赖 classfile。
