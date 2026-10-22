# mjbase pass 契约（pass-contract）

pass 体系的语义与不变量：作用域、写回纪律、preserved analyses 复用条件、sanitize 闸门。只记契约，不锚行号。

## 1. 三级作用域与单元

| 作用域 | pass 基类 | IR 单元 | AnalysisManager |
|---|---|---|---|
| 方法级 | `MethodPass` | `MethodScope`（promote 后的 CodeUnit + 身份） | `MethodAnalysisManager` |
| 类级 | `ClassPass` | `jbc::ir::ClassSession` | `ClassAnalysisManager` |
| 容器级 | `GlobalPass` | `mjbase::model::ResourceContainer` | `GlobalAnalysisManager` |

单元契约：

- `MethodScope` = 跨类身份面（`MemberKey`/`MethodId`/符号池视图）+ `CodeUnit&`。scope 由驱动方堆分配并全程持有；分析缓存按 scope 地址键控，栈上复用地址会伪造跨方法命中——自造 scope 的调用方必须自证地址唯一。
- 类级 pass 经 `unit()` 改元数据、经会话语义面改指令操作数，不直接触碰 backing 的 Code 属性（lower 的职责）。做成员增删/重排这类结构变换时必须同时落 `unit()` 与 backing 两面（backing 是 writeClass 的序列化源），重排后修正受影响 MethodUnit 的 `backingIndex`；纯元数据 pass 只走 `unit()`。
- 容器级对 ClassResource 的写回必须走两阶段提交（§4），不允许逐类直写。

## 2. 方法级写回归管线驱动

写回权归管线（pipeline / adaptor / 单类入口），pass 只改 CodeUnit，**不得**自行调用 `lowerMethod`/`writeClass`。理由是单一写回者不变量：backing 只被管线触碰，`changed()` 为假时整类零 lower、copy-through 保真，LowerError 与帧重算失败集中在管线一处。pass 改方法元数据（name/desc/access）不需要额外提交，lower 链路自带元数据提交。

帧链接线同属管线职责：对 changed 方法 lower 前做悬垂行/悬垂帧目标适配，lower 后 Strict 帧重算与 StackMapTable 再生；Strict 不可算（含 jsr/ret）显式抛出，不伪造帧。谓词过滤在 promote 之前按身份面求值，被过滤方法不付解码成本。

## 3. 编排语义

- **阶段序**：容器级 [sanitize 闸门 → global passes] → 类级 → 方法级（类内逐方法）。类级管线先于方法级。
- **失败策略**：pass 抛出的异常不吞掉，前序变换不回滚、后续 pass 不运行，异常路径按 `none()` 失效缓存。这是「错误走 Result」总约定的显式豁免点，仅限 pass 编排层；模块边界入口把异常收敛为 `Result<_, PipelineError>`。
- **跨层失效联动**：类级改动清空方法级 AM 全缓存；分析缓存只在当前 class 作用域内有效，逐类一造、切换资源即消亡。
- **作业级原子**：容器级 adaptor 把每个 class 的重序列化结果暂存 pending，全部成功才统一写回，任一失败容器保持原样。
- **容错档位**：perMethod / perClass 独立，仅捕获 `std::exception`，非 std 异常向上传播；跳过单元记入 `PassFaultReport`。

## 4. PreservedAnalyses 复用条件

- PA 由 pass 准确返回，编排器只做失效与求交，不替 pass 猜。
- `all()` 专用于确认没改 IR 的只读 pass；`all()` 默认 `changed()==false`，改了 IR 的 pass 必须显式 `markChanged()` 或返回 `none()`——写回与子级缓存清空都以 `changed()` 为准，与保留集合正交。
- `preserve<T>()` 仅在能证明分析仍有效时用；传递失效由 Invalidator 兜底，派生分析被显式保留但依赖失效时连锁失效。
- 类/方法级缓存活不过当前 class；容器级缓存跨类有效，变更型容器级 pass 返回 `none()` 时全量失效。

## 5. Sanitize 闸门

位置在容器级管线最前（先于一切 pass）；单类入口不强制，`sanitizeClassFile` 面可复用。检查面是对原始 bytes 的只读扫描（CP 引用语义校验），非第二套解析器。默认档位拒绝即抛错终止（资源保留）；容错档位把该资源从容器剔除，其余类继续；空 bytes 资源放行。

## 6. 其他

- **cf 域 pass**（`CfPass`）：载体是低层 `ClassFile` DOM，适合骨架/属性级变换与只读预检；无 AnalysisManager 与失效协议，写回走 writeClass copy-through，与 §2 契约无关。
- **公共父类求解**：super-chain-only，接口维度不参与；不可判定（未登记、无交、悬挂）回退可配置的 `java/lang/Object`；继承环在建图时即拒绝。
- **身份与字符串纪律**：跨类身份统一驻留 `PassJobContext::identityPool()`；API 面字符串一律借用视图，`std::string` 只出现在输出/异常文本等外部边界。
