# 可达性根提供者（Root Provider）

> 代码：`mjbase/src/mjbase/reference/root_provider.{hpp,cpp}`、
> `entry_points.{hpp,cpp}`；用例：
> `mjbase/test/unittest/reference/root_provider_test.cpp`。

## 背景

`GlobalReachabilityOptions` 用三个槽（`rootClasses` / `rootMethods` /
`rootFields`）表达闭包根。根来源在持续增加：

- keep 规则与 public / public-protected 档位（已有 `applyKeepRoots`）；
- Java `main(String[])` 发现（原先内嵌在可达性实现里）；
- 资源 keep（module 描述符、Manifest 入口、services 文件）；
- 调用方自供的入口（CLI、产品契约）。

原先每多一种来源就多一条平行写入路径，删除 pass 无法把「还有哪些根来源」
当作数据对待，也无法在测试里单独替换一种来源。本面把根来源收编为**纯值
查询**。

## 模型

```text
ReferenceIndex ──collect()──> RootSet ──appendRoots()──> GlobalReachabilityOptions
       ▲                          ▲
       │                          │
  IRootProvider ────────────── 调用方持有的 provider 列表
```

- `RootSet`：三个 vector，分别对应可达性选项的三槽。
- `IRootProvider`：`RootSet collect(const ReferenceIndex&) const`。只读索引、
  不改状态、不做缓存、不依赖调用顺序。实现必须可重入。
- 组合：调用方持有 `std::vector<const IRootProvider*>`，调用
  `appendRoots(providers, index, options)`。**没有注册表、没有全局状态、没有
  回调**——provider 不知道彼此存在，也不知道可达性。
- `appendRoots(const RootSet&, options)` 单独暴露，便于测试和不经 provider
  的直接追加。

## 内置实现

### `KeepRootProvider`

按值持有一份 `KeepRuleSet` 与一个 `KeepTier`，`collect` 内部构造临时
`GlobalReachabilityOptions` 并调用既有 `applyKeepRoots`，再把三槽移出。

- 规则语义、档位门槛、包私有宿主限制、池内视图全部复用既有实现，本层不
  重写匹配逻辑。
- 规则按值持有：`KeepRootSpec` 的 `rules` 是借用指针；provider 需要能独立于
  配置解析现场存活（延迟收集、跨阶段传递），按值持有避免悬垂。
- 默认构造 / 空规则 + `KeepTier::None` 是恒等：不产根。

### `MainMethodRootProvider`

遍历 `ReferenceIndex::methodDeclarations()`，用共享谓词
`isJavaMainMethod` 收集入口。

`isJavaMainMethod` 从 `global_reachability.cpp` 的匿名命名空间抽到
`entry_points.hpp`，可达性内建发现与 provider 共用同一份判定，避免两处
语义漂移。判定条件：名字 `main`、描述符 `([Ljava/lang/String;)V`、访问位
同时含 `public` 与 `static`。

与 `GlobalReachabilityOptions::includeMainMethods` 的关系：两者可以同时
打开，候选集合相同；重复根由可达性的集合语义吸收，不改变闭包结果。需要
自定义 main 策略（例如只保留指定类的 main）的消费方应关闭内建发现、改用
自建 provider。

## 契约

### 只加不减

`appendRoots` 只追加，不清空既有根、不关闭任何既有保留依据。既有根的来源
与顺序保持在前，provider 产物按列表顺序追加。空 provider 列表是恒等变换。

### 不去重

可达性 build 的 `markClass` / `markMethod` / `markField` 是集合语义，重复根
只多一次哈希查找。provider 之间的重叠（档位与 main 命中同一方法、两个规则
命中同一类）合法且常见，本层不做二次去重——去重会引入「谁先谁后」的隐含
契约，却没有收益。

### 生命周期

`RootSet` 的三槽是借用视图。内置 provider 写入的视图一律指向
`ReferenceIndex` 池内的声明文本（与声明事实同源），因此：

- 调用方必须保证索引存活到 `GlobalReachability::build` 之后；
- 自定义 provider 必须遵守同一契约，或让视图指向自有的、生命周期覆盖消费
  点的存储；
- 不要把 `collect` 结果指向局部 `std::string`——悬垂视图会让根静默变空，
  症状是「keep 的类被误删且无诊断」。

`RootViewsPointAtIndexPoolText` 用例以指针相等钉死这一契约。

## 设计取舍

- **纯接口 + 调用方持有列表**，而不是 provider 注册表：注册表引入初始化
  顺序、跨线程生命周期和「谁清空」问题；本库的 API 纪律是 Result 值语义、
  无粘性状态、库内不回调用户业务。
- **规则按值持有**，而不是借用 `KeepRootSpec`：配置对象常是短命的解析产物，
  provider 常被延迟消费；复制一份规则表（规模受配置行数约束）换来明确的
  生命周期。
- **复用 `applyKeepRoots`**，而不是在 provider 里重写匹配：保持既有 keep
  语义与测试继续有效，provider 只是另一条入口。
- **不做资源 keep 的 provider**：module 描述符 / Manifest / services 的根
  依赖容器与资源解析，属于调用方上下文，不在本面伪造统一抽象。

## 非目标

- 不做层级 / 注解谓词的 keep 扩展（沿用 `KeepRuleSet` 的既有边界）。
- 不做 provider 缓存或增量失效：一次 `collect` 就是一次声明遍历，规模与
  声明数线性。
- 不改变 `GlobalReachability` 内核或 `applyKeepRoots` 的语义。
- 不把 provider 接入 `GlobalAnalysisManager`；需要时由消费 pass 自行组合。

## 验证

`root_provider_test.cpp` 覆盖：

- `isJavaMainMethod` 的名字/描述符/访问位判定；
- keep 类规则只产类根、成员规则只产成员根；
- public-protected 档位只收 public 类与 public/protected 成员，包私有类与
  私有成员不入根；
- 空 provider 恒等；
- main provider 只收启动入口；
- 多 provider 追加保留既有根、顺序正确，根真的进入可达闭包（不只填了
  vector）；
- 空指针 provider 跳过；
- 三槽视图与索引池文本同指针（生命周期契约）。
