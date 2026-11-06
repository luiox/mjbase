# 可达性根提供者（Root Provider）

## 背景

闭包根的来源在持续增加（keep 规则、main 发现、资源入口、调用方自供）。原先每多一种来源就多一条平行写入路径，删除 pass 无法把「还有哪些根来源」当作数据，测试里也无法单独替换一种来源。本面把根来源收编为纯值查询：`IRootProvider::collect` 产 `RootSet`，调用方持 provider 列表 `appendRoots` 追加——没有注册表、没有全局状态、没有回调。

## 契约

- **只加不减**：`appendRoots` 只追加，不清空既有根、不关闭既有保留依据；空列表是恒等。
- **不去重**：可达性 build 是集合语义，重复根只多一次哈希；provider 间重叠合法，二次去重只会引入「谁先谁后」的隐含契约。
- **生命周期**：`RootSet` 三槽是借用视图，内置 provider 一律指向 `ReferenceIndex` 池内文本，索引必须存活到 build 之后。自定义 provider 不得让视图指向局部 `std::string`——悬垂视图让根静默变空，症状是「keep 的类被误删且无诊断」。
- **可重入**：collect 只读索引、不改状态、不缓存、不依赖调用顺序。

## 设计取舍与非目标

不用注册表（初始化顺序、跨线程生命周期、谁清空三类问题），不用借用 `KeepRootSpec`（配置常是短命解析产物，按值持有换明确生命周期），keep 匹配复用 `applyKeepRoots` 不重写。不做资源 keep 的 provider（依赖容器与调用方上下文，不伪造统一抽象）；不做缓存与增量失效；不接入 `GlobalAnalysisManager`。
