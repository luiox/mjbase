# Slot 级 def-use 与字段访问事实

> 代码：`mjbase/src/mjbase/asm/analyzer/def_use.{hpp,cpp}`；
> 用例：`mjbase/test/unittest/asm/analyzer/def_use_test.cpp`。

## 背景

`origin_domain` 每个槽只保留一个来源，多来源汇合后退化为 Unknown；它能回答「值最初由谁产生」，不能回答「这个局部槽上有哪些写会到达这次读」。死存储删除、写后未读判定、分支合并后的多定义分析需要后者。

本模块在 `jbc::analysis::DomainEngine` 上搭一个并集值域：域值为有界定义集合，引擎负责控制流、cat2 槽布局、dup 复制与异常边，本模块只实现定义集合的合并和帧后处理。与方法体扫描相比，这套分层把栈机迁移逻辑留在引擎里，不复制第二份 per-opcode 表。

## 模型

```text
DefId ──> DefSet（有序、去重、上限 16） ──> DefUseValue（集合 + 槽宽）
                                                   │
                              DomainEngine<DefUseValue>
                                                   │
                         MethodDefUse：边表 / 局部访问 / 字段访问
```

- `DefId` 两类：`Parameter`（`id` 为入参局部槽号）与 `Instruction`（`id` 为 `InsnId::slot`）。完整句柄经 `MethodDefUse::instructionOf` 还原。
- `DefSet` 按 `(kind, id)` 排序；重复插入返回 false。超过 `kMaxDefs`（16）时保留字典序最小的 16 个并置 `truncated`，因此不同合并顺序得到同一集合。
- `DefUseValue` 的 `slots` 是槽宽；引擎按它做 cat2 的双槽弹压。

## 定义语义

(1) 局部写重定义该局部：store 家族与 iinc 的定义就是该指令本身，旧来源不保留。来源链问题归 `origin_domain`。
(2) load 是恒等搬运：局部槽的到达集合随值进入操作数栈；未写过的局部由引擎回退 `newValue`，本域识别 load 族并返回空集合，不把 load 伪造成定义。
(3) 入参由 `MethodSig` 播种为 `Parameter` 定义，`this` 为 local 0；不给签名时入参槽视为无定义。
(4) dup 族与 swap 只复制/重排槽，不产使用边；pop 丢弃值，产使用边。
(5) iinc 同时是局部读与写：读使用旧集合，写把定义换成 iinc 指令。
(6) 异常边采用引擎精确档：handler 入口的 locals 是 try 范围内各可抛指令 in-state 的合并；死 try 范围的 handler 不可达。

## 使用点

数据流成功后，后处理按可达指令枚举：

- 栈使用：按指令实际弹出的槽位数（`stackEffectOf` / 字段与调用描述符 / multianewarray 维度）从栈顶按**值**回退，cat2 底槽不单独计边。`DefUseEdge::index` 是从栈顶起第几个值。
- 局部读：load 族、iinc、ret；`DefUseEdge::slot = Local`，`index` 是局部槽号。
- 局部写：store 族、iinc；记进 `LocalAccess`，不带出边（写本身就是新定义）。

一条定义可能同时流向 load 的局部读与 load 之后消费者的栈使用，所以 `usesOf` 返回的是全部使用点，不是去重后的消费者。

## 字段访问事实

get/put 家族在可达指令上产 `FieldAccess`：owner/name/desc 驻留进结果自有的 `KeyPool`，读写方向、静态位、接收者到达定义（`objectDefs`）与存入值到达定义（`valueDefs`）一并记录。字段访问只覆盖方法内可达指令；全程序的直接字段事实由 `ReferenceIndex` 提供，两者分工见 [全程序字段访问图](field-access-graph.md)。

## 有界与负向结论

`truncated()` 为真表示集合溢出、边不完整。`hasUnresolvedFieldRefs()` 表示字段引用操作数无法解析。任何「没有使用 ⇒ 可删」的结论必须同时确认 `ok()`、`truncated()` 为假，并叠加调用方自己的扫描范围假设。

## 查询面

| 查询 | 说明 |
|---|---|
| `edges()` | 全部 def→use 边（扫描序） |
| `usesOf(DefId)` / `usesAt(InsnId)` | 按定义 / 按使用指令取边 |
| `localAccesses()` / `readsOfLocal(local)` | 局部读写记录 |
| `fieldAccesses()` | 方法内字段访问事实 |
| `instructionOf(DefId)` | Instruction 定义的完整句柄 |
| `summary()` | 引擎帧摘要（栈/局部容量、失败码） |

## 测试覆盖

用例覆盖定义集合的排序/去重/截断与插入顺序无关性、store→load 链、入参身份、分支合并双定义、死存储无使用、dup 不产使用边、cat2 槽配对、异常处理器继承抛出点 locals、死 try 范围 handler 不可达、字段读写方向的到达定义。
