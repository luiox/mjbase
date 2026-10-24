# 全程序字段访问图

> 代码：`mjbase/src/mjbase/reference/field_access_graph.{hpp,cpp}`。

## 背景

`ReferenceIndex` 只到「哪个方法体里有一条访问」，unused field 类消费者需要「哪些方法（含传递调用方）读写这个字段」。本模块把 `ReferenceIndex`（直接字段事实）与 `CallGraph`（直接调用边）两个现成事实面按调用边求并集不动点拼起来，不重新扫描字节码，也不复制任何一份输入。

## 事实分类

| 引用种类 | 读 | 写 |
|---|---|---|
| getstatic / getfield | 是 | 否 |
| putstatic / putfield | 否 | 是 |
| MethodHandle 字段句柄 | 是 | 是 |
| ReflectionAccess（`Field.get/set` 执行点） | 是 | 是 |
| Reflection（仅成员查找） | 否 | 否 |

Reflection 查找只说明成员对象被找到，不说明执行，因此不计读写。

## 负向结论纪律

虚/接口分派、invokedynamic、被调方缺失或无方法体会沿调用边把 `hasUnknownEffects` 传递给全部传递调用方。用 `unreadDeclaredFields` / `untouchedDeclaredFields` 做删除决策必须同时确认 `hasUnknownEffects()` 为假，并自行承担扫描范围（外部类、反射字符串、native）假设；`hasUnresolvedFieldRefs()` 为真时字段事实本身不完整。

`build` 接收非 const `CallGraph&`：查询需要把图自有池的方法文本驻留进调用图，驻留是只增操作，不改语义。索引与调用图必须来自同一批类字节；键与查询结果的寿命 = 图自有 KeyPool。
