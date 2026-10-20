# 全程序字段访问图

> 代码：`mjbase/src/mjbase/reference/field_access_graph.{hpp,cpp}`；
> 用例：`mjbase/test/unittest/reference/field_access_graph_test.cpp`。

## 背景

`ReferenceIndex` 记录每条字段访问指令的精确 owner/name/desc，但只到「哪个方法体里有一条访问」。unused field 类消费者需要「哪些方法（含传递调用方）读写这个字段」。本模块把两个现成的全程序事实面拼起来：

- `ReferenceIndex`：直接字段访问事实 + 方法/字段声明；
- `CallGraph`：直接调用边。

不重新扫描字节码，也不把调用图或引用索引复制一份。

## 模型

```text
直接字段事实 ──> 方法摘要₀ = 直接读写集
调用边 ────────> 方法摘要 = 直接读写集 ∪ 各被调方法摘要（并集不动点）
                              │
                              └──> 字段摘要 = 读写方倒排
```

- 方法摘要的集合按字段索引升序保存并去重；被调方法摘要并入调用方，环上集合只增，必然收敛。
- 被调方即使不在扫描集合中也会进入方法表（无出边、无摘要），保证调用方的字段集不会因为目标缺失而凭空正确。
- 字段摘要按声明字段在前、引用引入的外部字段在后的顺序物化；`readers` / `writers` 按方法表顺序。

## 事实分类

| 引用种类 | 读 | 写 |
|---|---|---|
| getstatic / getfield | 是 | 否 |
| putstatic / putfield | 否 | 是 |
| MethodHandle 字段句柄 | 是 | 是 |
| ReflectionAccess（`Field.get/set` 执行点） | 是 | 是 |
| Reflection（仅成员查找） | 否 | 否 |

Reflection 查找只说明成员对象被找到，不说明执行；因此不计读写。MethodHandle 与反射访问点可能执行 get 或 set，两边都记。

## 保守边界

下列情形把调用方标记为 `hasUnknownCallees`，并沿调用边传递到所有（传递）调用方：

- 虚分派与接口分派：编码目标只是声明类型，实际重写目标未解析；
- invokedynamic：链接目标不在本层展开；
- 被调方未在扫描集合声明，或声明为 abstract/native：没有可分析的方法体。

标记只影响负向结论的可信度，不阻止已知事实照常记录。字段摘要仍列出已知读写方；消费者若要用 `unreadDeclaredFields` / `untouchedDeclaredFields` 做删除决策，必须同时确认 `hasUnknownEffects()` 为假，并自行承担扫描范围（外部类、反射字符串、native 代码）的假设。

`hasUnresolvedFieldRefs()` 表示至少一条字段引用缺 owner/name/desc，字段事实不完整。

## 查询面

| 查询 | 说明 |
|---|---|
| `methodSummary(MemberKey / 文本)` | 单方法读写集与 unknown 标记 |
| `fieldSummary(MemberKey / 文本)` | 单字段读写方与 `declared` |
| `readersOf` / `writersOf` | 字段的已知读写方法 |
| `unreadDeclaredFields()` | 声明字段中无已知读者的（写后未读候选） |
| `untouchedDeclaredFields()` | 声明字段中既无读者也无写者的 |
| `hasUnknownEffects()` | 任一方法带未解析被调方 |

所有键由图自有 `KeyPool` 驻留，查询结果借用池内文本；图销毁后失效。文本入参按内容寻址，与 `MemberKey` 查询等价。

## 输入契约

`build` 接收非 const `CallGraph&`：查询调用方的出边时，需要把本图池里的方法文本重新驻留进调用图的池（调用图的查询只收内容键）。驻留是只增操作，不改变调用图语义。索引与调用图必须来自同一批类字节；类集合不一致时，缺失的方法按外部目标处理并标记 unknown。

## 测试覆盖

用例覆盖直接读写倒排、经调用边传递的方法/字段摘要、只写字段、未读与未触碰字段、虚分派与外部调用的 unknown 标记及传递、清空语义。
