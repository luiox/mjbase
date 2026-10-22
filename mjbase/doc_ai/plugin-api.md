# plugin API（pass-pack 接口）

独立变换仓（dasho / allatori / …）与宿主之间唯一的缝：pack 向宿主注册 pass 工厂，宿主装配管线并驱动。接口随 mjbase 发布，pack 仓只依赖 mjbase 公开头，不碰旧栈与任何产品线内部。

## 依赖红线

pack 仓允许的 include 面是 `mjbase/**` 公开头加 pack 自身代码，测试与语料同受此约束。依赖红线由各仓自查，宿主侧 CI grep 复核。

## 契约

(1) 同源门：pack 与宿主必须链同一 mjbase 提交构建。构建脚本把 git 提交哈希编译进 `buildCommit()`，pack 在 `PackManifest::mjbaseCommit` 声明自己的构建戳，installPack 与宿主自身构建戳比对，不一致即拒绝。接口演进因此不需要人工版本号簿记——注入面的任何变化都体现为 mjbase 提交变化，同源门自动拦截异源装配。

(2) 名字空间强制：pack 名是单段 `[a-z0-9_]`，其全部 pass 注册为 `"<pack>.<Pass>"`。installPack 对装配前后注册表差集逐名验收前缀，越界即 NamespaceViolation。独立仓并发演化时的撞名防线不依赖 pack 自觉。

(3) 配置通道：宿主在装配期传 UTF-8 JSON 文本（借用视图，仅调用期有效），pack 自行解析并把配置闭包进工厂。注册表工厂无参是既定契约，pack 级配置靠闭包注入，不给注册表加配置维。配置 schema 与版本由 pack 自治，解析拒绝走 ConfigRejected。

(4) 寿命纪律：注册表键是借用视图、不复制存储，pass 名与 description 的字符串实体必须由 pack 自持，寿命覆盖注册表存活期。pack 实例由宿主以引用持有，接口无虚析构，宿主不经接口 delete。

(5) 错误与线程：实现不抛异常跨界，失败一律走 `Err(PackError)`，RegistrationFailed 的 detail 携带被拒 pass 名。装配只在宿主初始化阶段发生，注册表非线程安全。

(6) 部分注册不回滚：installPack 中途失败时注册表可能留下部分条目，宿主丢弃整表重建，不做单 pass 撤销。

(7) pass 语义：pack 内 pass 遵守 spi 通行契约（PreservedAnalyses 约定、帧链接线经 `PassPipelineOptions::typeResolver`、fail-closed 口径），与内置 pass 无差别受管线驱动。

## 宿主装配配方

新建宿主自持的 PassNameRegistry（不用全局默认表）；逐 pack 调 `installPack(pack, registry, config)`，Err 即装配失败；按用户选择的名字经 `spi::applyTo` 投入 PipelineBuilder；建 PassJobContext 后 `buildNestedPipeline` 组装，宿主层次扫描结果经 StrictTypeResolverAdapter 挂入 `PassPipelineOptions::typeResolver`；最后 `runContainerPipeline` / `runClassPipeline` 驱动，容器写回由宿主收口。

## pack 仓交付物

一个 IPassPack 具体实现（manifest + registerPasses）、一个 pack 访问函数（静态链接期 `IPassPack& <pack>_pack()`，动态库阶段为对应导出函数）、pack 自治的单测与语料。

## 动态库装载

不设 C 接口层。pack 与宿主用同一工具链、链同一 mjbase 提交构建，C++ 类型跨界成立的前提就是同源门校验的内容：动态库装载路径同样经 installPack，构建戳不一致的库直接拒绝，不进入装配。
