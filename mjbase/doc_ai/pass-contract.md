# mjbase pass 契约（pass-contract）

mjbase::spi 的 pass 体系定型文档：pass 分类、作用域单元、写回纪律、
preserved analyses 复用条件与 sanitize 闸门位置。本文只记语义/不变量/契约，
不锚文件行号。

## 1. 三级作用域与单元

| 作用域 | pass 基类 | IR 单元 | AnalysisManager |
|---|---|---|---|
| 方法级 | `MethodPass` | `MethodScope`（promote 后的 CodeUnit + 身份） | `MethodAnalysisManager` |
| 类级 | `ClassPass` | `jbc::ir::ClassSession`（backing + ClassUnit 同生共死） | `ClassAnalysisManager` |
| 容器级 | `GlobalPass` | `mjbase::model::ResourceContainer` | `GlobalAnalysisManager` |

单元契约：

- **MethodScope** = `MethodTarget`（跨类身份面）+ `CodeUnit&`。
  - 身份面含 `MemberKey key`（owner/name/desc 驻留作业身份池）、
    `MethodId method`（会话内稳定身份，与物理下标解耦）、name/desc 视图
    （会话符号池借用，会话存活期内有效）与 `ClassSession&`。
  - 方法级 pass 的单位是 **promote 后的 CodeUnit + MemberKey 身份**。
  - scope 由驱动方堆分配并在类作用域内全程持有（地址互异）。分析缓存按
    scope 地址键控，栈上复用地址会伪造跨方法命中——任何绕过驱动方自造
    scope 的调用方必须自证地址唯一。
- **ClassSession** 是类级唯一载体：类级 pass 通过 `unit()` 改骨架元数据、
  通过会话语义面（setMember/setClass/…）改指令操作数；不直接触碰
  backing 的 Code 属性（那是 lower 的职责）。
- **ResourceContainer** 是容器级唯一载体；容器级 pass 对 ClassResource
  的写回必须走两阶段提交（见 §4），不允许逐类直写。
- **类级结构变换的双面落盘义务**（pass 试点实证补录）：类级 pass 做
  成员增删/重排这类骨架结构变换时，必须同时落 unit() 与 backing 两面
  （backing 是 writeClass 的序列化源，只改 unit() 不落盘），且重排后必须
  修正受影响 MethodUnit 的 `backingIndex`——单元位置与 backing 槽位在
  结构变换后不再自然一致。这是结构级 pass 与纯元数据 pass 的分界：
  后者只走 unit() 即可。

## 2. 方法级写回：管线统一驱动 lower（裁决记录）

方法级变换的写回权归属**管线**（`runMethodPipelineOverSession` /
`ClassToMethodPassAdaptor` / 单类入口的裸方法路径），不由 pass 自驱：

1. 驱动方逐方法 `hasCode` 分流（abstract/native 无代码是预期形态，直接
   跳过，不进错误路径）→ `promoteMethod` → 运行方法管线 →
   `PreservedAnalyses::changed()` 为真时 `lowerMethod`。
2. pass 只改 CodeUnit（与 ClassUnit 元数据），**不得**自行调用
   `lowerMethod`/`writeClass`。
3. 理由：
   - 单一写回者不变量——backing 只被管线触碰，pass 无法绕过 changed()
     判定与容错档位偷偷落盘；
   - changed()==false 时整类零 lower，copy-through 保真不被打破；
   - LowerError / relocation 失败（如 LineNumberTable pcmap miss、
     未建模嵌套属性）集中在管线一处，容错档位不需要每个 pass 重实现。
4. pass 改方法元数据（name/desc/access）不需要额外提交：lower 链路自带
   元数据提交，只改元数据不改指令不会静默丢失。
5. 帧链接线（A1 消费面，与 lower 同属管线职责，pass 不得自驱）：管线
   选项注入 typeResolver 时，对每个 changed 方法在 lower 前做嵌套属性
   布局适配（LNT/LVT 悬垂行重映射到下一存活 pc；既有 SMT 帧目标或
   Uninitialized 载荷悬垂时整体摘除），lower 后做 Strict 帧重算 →
   maxes 覆盖采用（二次 lower 落 maxes）→ StackMapTable 再生挂回
   （frame_rewire 两入口）。Strict 不可算（含 jsr/ret）显式抛出——
   不伪造帧；版本 < 50 的类整体跳过（SMT 无定义）。未注入 resolver 时
   若变换造成既有 SMT 帧目标悬垂，管线显式失败（不产出缺帧的 V50+
   class）；不注入且无帧悬垂时写回保持 relocation-only。

谓词过滤（`MethodPredicate`）在 **promote 之前**按身份面（MethodTarget）
求值：被过滤方法不付解码成本，与 CLI 目标过滤语义一致。

## 3. 编排语义

- **阶段序**：容器级 [sanitize 闸门 → global passes]；类级 [类级 passes]；
  方法级在类内逐方法（显式 adaptor 或入口裸方法路径）。类级管线先于方法级。
- **顺序执行 + PA 聚合**：`PassManager` 顺序运行 pass；每个 pass 后按其
  PreservedAnalyses 失效缓存，整体返回各 pass 保留集合的**交集**。
- **失败策略（异常语义）**：pass 抛出的异常不在编排器中吞掉；
  已完成的前序变换不回滚，后续 pass 不再运行；异常路径保守按
  `none()` 失效当前作用域缓存并清理子级缓存。模块边界的两个入口
  （单类/容器 run 管线）把异常收敛为 `Result<_, PipelineError>`；
  编排内部的失败语言仍是异常（容错档位以 try/catch 实现）。
  这是「mjbase 错误走 Result」总约定上的显式豁免点，仅限 pass 域编排层。
- **配置错误稳定失败**：`addPass(nullptr)` 抛 invalid_argument；adaptor
  取不到 pinned proxy 抛 logic_error；未注册分析取结果抛 logic_error。
- **跨层失效联动**：
  - 类级 pass 声明改动（或异常路径）→ 方法级 AM 全缓存清空（经 pinned
    proxy）；方法级 pass 改动 → 类级缓存按 PA 常规失效。
  - 类/方法级分析缓存只在当前 class 作用域内有效：AM 与每类作用域 arena
    逐类一造，切换资源即消亡，不因地址复用跨类命中。
- **作业级原子（两阶段提交）**：容器级 adaptor 把每个 class 的重序列化
  结果暂存 pending，全部成功后统一写回；任一失败（默认档位）容器保持
  原样。`ClassResourcePendingCommit` 供直写型 global pass 手工复刻同一契约。
- **进度回调**：容器级 adaptor 每 class 发一次 `update`（Pipeline 阶段，
  index 1..N / total，先数后跑）；sink 默认 stdout 策略（TTY \r 刷新 +
  100ms 节流、非 TTY 静默），update/finishPhase 为虚可拦截。
- **容错档位**：perMethod / perClass 相互独立，仅捕获 `std::exception`；
  非 std 异常任何档位都向上传播。跳过单元记入 `PassFaultReport`，行格式
  `SKIP <scope>=<target> reason=<reason> detail=<detail>`；reason 取值
  sanitize_error / reader_error / pass_error / writer_error（方法级统一
  pass_error）。

## 4. PreservedAnalyses 复用条件

- pass 必须准确返回 PA；编排器只做失效与求交，不替 pass 猜。
- `all()`：确认没有改变 IR 的只读/分析 pass 专用。**注意**：all() 默认
  changed()==false；改了 IR 的 pass 必须显式 `markChanged()` 或返回
  none()/部分集合——写回（lower/writeClass）与子级缓存清空都以 changed()
  为准，与分析保留集合正交。
- `none()`：改了 IR 且无法精确列出保留集时的安全默认（隐含 changed()）。
- `preserve<T>()`：仅在能证明该分析仍然有效时使用（如变换不破坏 CFG
  拓扑则可 preserve CFG 分析）；传递失效（依赖链）由 AnalysisManager 的
  Invalidator 兜底——派生分析即使被显式保留，依赖被失效也会连锁失效。
- 复用的收益边界：类/方法级缓存活不过当前 class；容器级缓存跨类有效，
  变更型容器级 pass 返回 none() 时全量失效。

## 5. Sanitize 闸门位置与语义

- **位置**：容器级管线最前（先于一切 pass，包括自行 readClass 的容器级
  global pass）；单类入口当前不强制（调用方可在类管线前自查，闸门面
  `sanitizeClassFile` 可直接复用）。
- **检查面**：CP 引用语义校验（只读扫描原始 bytes，非第二套解析器）——
  BadHeader / BadCpTag / LengthOutOfRange / RefIndexOutOfRange /
  RefTagMismatch / BadMethodHandleKind / BootstrapIndexOutOfRange /
  CpCycle。诊断条数有上限（敌意样本防护）。
- **拒绝语义**：默认档位抛 runtime_error 终止作业（资源保留）；容错档位
  记 sanitize_error 诊断并把该资源**从容器剔除**（下游与导出不再见），
  其余类继续。空 bytes 资源放行（与 adaptor 的跳过契约一致）。

## 6. classfile 级 pass（cf 域）

`CfPass`/`CfClassPass` 载体为 `jbc::core::ClassFile`（低层 DOM）：
- 与指令级 pass 平行，适合骨架级/属性级变换与只读预检；无 AnalysisManager
  与失效协议（未建模分析缓存），pass 间不交换分析结果。
- 初始化/收尾由调用方显式编排（doInitialization / doFinalization 遍历
  全部 pass），顺序运行。
- 该域与 §2 的写回契约无关：cf pass 直接改低层 DOM，写回走 writeClass
  copy-through（触碰区段重编码、未触碰区段保真）。

## 7. 公共父类求解（resolver）

`CommonSuperClassResolver` 挂 `ClassHierarchy`，语义要点：
- 求解面 super-chain-only（最近公共祖先）；接口维度从不参与。
- 不可判定回退逐条：任一类型未登记 → fallback；两链无交（含悬挂键截断）
  → fallback；继承链成环在建图时即被拒绝（Err(Cycle)），求解面无环前置。
- fallback 缺省 `java/lang/Object`，可改设；改设作废查询缓存。
- 查询缓存按归一化无序对键控；结果视图指向层次图驻留池或 resolver 自有
  查询池，寿命 = 两者中先死亡者。

## 8. 身份与字符串纪律

- 跨类身份（ClassKey/MemberKey）统一驻留 `PassJobContext::identityPool()`
  （KeyPool；「一个分析任务持有一个池」）。
- 分析/中间字符串经各级 AM 的 `arena()`（作业级或每类作用域）；
  scope.name/desc 等视图借用会话符号池，存活期 = 会话。
- API 面字符串一律借用视图（string_view / Utf8StringRef）；自有存储用
  ca::str 家族；std::string 只出现在输出/异常文本等外部边界与一次性拼接。

