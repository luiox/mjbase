# 调用链与调用树视图（CallGraph Views）

> 对应 issue #6。代码：`mjbase/src/mjbase/asm/classpath/call_graph.{hpp,cpp}`；
> 用例：`mjbase/test/unittest/asm/classpath/call_graph_test.cpp`。

## 背景

`CallGraph` 已提供调用点（`outgoingSitesOf` / `incomingSitesOf`）、去重邻接
（`calleesOf` / `callersOf`）与可达闭包（`reachableClosure`）。这些是集合/
边级别的查询，回答不了两类常见问题：

- 「从这个入口出发，有哪些根到叶的调用路径？」（诊断、报告、规则解释）
- 「把它展开成一棵可折叠的调用树长什么样？」（UI、人工审计）

本视图层补齐这两类查询。它是**调用图的只读投影**：不新增分析事实、不解析
虚分派、不改变任何既有查询语义。

## 调用链（`enumerateChains` / `CallChainCursor`）

### 语义

一条 `CallChain` 是根到叶的**简单方法序列**：

- 边来自 `calleesOf`：同一对被调方法在多个调用点重复出现时只算一条边，
  因此链是方法序列而非调用点序列；需要逐调用点事实时回到
  `outgoingSitesOf`。
- callee 顺序沿用调用点的扫描序（即 `calleesOf` 的返回序），DFS 先序产出，
  同一次构建内结果确定。
- 「叶」有三种：没有出边、全部出边都指回当前路径上的祖先（环）、或已到
  `maxDepth` 上限。只有叶帧产出链；回溯过程中的前缀路径不会重复产出。
- 环在「路径内首次重复」处截断：不产出含重复方法的链，也不额外产出一条
  cycle 标记的链。需要展示环请用调用树。
- `maxDepth` 是最大边数（根为 0 层）：达到上限的节点按叶处理，链停在边界。
  例如 `maxDepth = 1` 时，`a → b → d` 产出 `[a, b]`。
- 根不在图中：游标立即耗尽，不产出空链。
- `next()` 返回 `false` 表示枚举结束，且不修改传入的 `out`（便于复用缓冲）。

### 惰性

游标不建全量快照：它只持有当前 DFS 路径（每帧一份去重后的 callee 列表）与
路径上的方法集合。内存与当前深度成正比，而不是与路径总数成正比。

路径总数的最坏情况是简单路径的指数级（稠密图），这是「枚举所有路径」问题
本身的性质，不是实现缺陷。消费方按需拉取、随时停止；需要硬上限时用调用树
的 `maxNodes`，或自行计数后停止 `next()`。

### 生命周期

`CallChainCursor` 借用 `CallGraph`（内部持指针），图销毁后不得继续使用。
游标可移动、不可复制；默认构造的游标是耗尽态，可安全接收移动赋值。

典型用法：

```cpp
CallChainOptions options;
options.maxDepth = 8;
CallChain chain;
auto cursor = graph.enumerateChains(entryKey, options);
while (cursor.next(chain)) {
    report(chain.methods);   // methods[0] 为根
}
```

## 调用树（`buildCallTree`）

### 语义

调用树区分「方法」与「方法的某次出现」：

- 每次出现是一个 `CallTreeNode`。同一方法出现在多个分支时保留多个节点，
  前端可用 `methodId`（同一 `MemberKey` 首次出现时分配的去重号）折叠展示。
- 节点下标即先序分配号（根为 0），因此 `nodes` 天然是先序数组。
- `parent` 给出父节点下标，根为 `kNoCallTreeParent`；`depth` 为到根的边数。
- 回到当前路径祖先的边保留为 **`cycle = true` 的叶节点**（不展开），让 UI
  能画出回边；这与链枚举「直接跳过环」不同。
- 重入但不成环（同一方法在不同分支各出现一次）是普通节点，`cycle = false`。
- 根不在图中：返回空树（`nodes` 为空，`truncated = false`）。
- `maxDepth`：超过上限的节点不创建，边界节点按叶处理（与链一致）。
- `maxNodes`：节点总数上限（含根）。达到上限后停止展开并置
  `truncated = true`；已生成节点保持完整，不视为错误。上限恰好等于完整树
  规模时不置位。默认 4096。

### 与链枚举的取舍

| | 链枚举 | 调用树 |
|---|---|---|
| 环 | 跳过，不产出重复方法 | 保留 cycle 叶节点 |
| 重入 | 不同路径各产出一条链 | 每次出现一个节点，`methodId` 去重 |
| 输出 | 根到叶序列流 | 一次性先序数组 |
| 上限 | `maxDepth` | `maxDepth` + `maxNodes`（默认有上限） |
| 适用 | 报告/规则解释/逐条消费 | UI/人工审计/需要完整结构 |

## 非目标

- 不解析虚分派 / 接口分派：边仍是 `CallGraph` 的未解析编码目标。层次感知
  的路径枚举应在 `ResolvedCallGraph` 上另做，不混入本层。
- 不做最短路径、权重、支配关系或路径条件。路径条件收集是独立分析层。
- 不提供反向路径枚举；反向查询沿用 `callersOf` / `incomingSitesOf`。
- 不对调用点去重以外的重复做合并：同一方法序列不会因调用点数量重复产出。

## 复杂度

- 单条链产出：`O(路径长度 + 各帧 callee 列表构建)`；`calleesOf` 每次调用
  现场构建去重列表。
- 树：`O(输出节点数 + 每个节点的出边扫描)`；受 `maxNodes` 约束。
- 游标与树都不修改图，可对同一图并发创建多个只读视图。

## 验证

`call_graph_test.cpp` 的视图用例覆盖：

- DFS 先序与简单路径：`a→{b,c}`、`b/c→d`、`d→a` 的环图；
- 环截断后继续沿未访问分支展开；
- 深度上限 1/2 的边界叶语义；
- 叶根、缺失根、`next()` 结束不改 `out`；
- `calleesOf` 去重（同一方法两次调用只产出一条链）；
- 树节点的先序下标、父/深度/`methodId`、cycle 叶、重入节点共享 `methodId`；
- `maxDepth` 边界、`maxNodes` 截断、上限恰好等于完整规模不置位；
- 叶根单节点与缺失根空树。
