# indy / MethodHandle / VarHandle 解析（Indy Resolution）

> 对应 issue #2。代码：`mjbase/src/mjbase/asm/classpath/indy.{hpp,cpp}`；
> 用例：`mjbase/test/unittest/asm/classpath/indy_test.cpp`。

## 背景

`invokedynamic` 的 callee 不是 CP 里的直接成员引用：CP InvokeDynamic 只给出名字、方法描述符和 BootstrapMethods 表下标，真正的链接目标藏在 BSM 的静态参数里，通常是 MethodHandle 常量。CallGraph 只能把它记成一条 owner 为空的保守边，lambda 方法体、record 的 `toString` getter 等真实调用因此缺失。VarHandle 同理：编译器编码的是 `VarHandle.get` 之类的签名多态占位成员，被访问的字段在运行时由接收者决定。

这一层把 BSM 契约读成指令级事实（`IndyIndex`），CallGraph 再消费其中可确定的目标补边。它不执行 BSM，不模拟类加载，也不对未知家族猜目标。

## BSM 分族

家族只由 BSM 方法句柄的 owner 与 name 决定。同名成员出现在其他 owner 下，或已知 owner 下的未知成员，一律记为 `Unknown`。

| 家族 | 触发成员 | 展开产物 |
|---|---|---|
| LambdaMetafactory | `metafactory` / `altMetafactory` | implMethod 句柄、SAM 类型、实例化类型、函数接口、捕获参数；alt 形式的桥方法类型 |
| StringConcatFactory | `makeConcat` / `makeConcatWithConstants` | 仅家族事实与捕获参数，无成员目标 |
| ObjectMethods | `bootstrap` | 全部 MethodHandle 静态参数（record 字段 getter） |
| ConstantBootstraps | `nullConstant` 等 9 个已知成员 | 仅 `invoke` 展开首个 MethodHandle 参数 |
| SwitchBootstraps | `typeSwitch` / `enumSwitch` | 仅家族事实，无成员目标 |
| Unknown | 其他 | 无目标，`hasUnknownFamilies()` 置位 |

`ConstantBootstraps` 的已知成员是 `nullConstant`、`primitiveClass`、`enumConstant`、`getStaticFinal`、`invoke`、`fieldVarHandle`、`staticFieldVarHandle`、`arrayVarHandle`、`explicitCast`。其中 `fieldVarHandle` 族需要上游常量流才能确定字段，本层只记家族事实。

未知家族仍保留 CP 可读的 name 与 desc，但 `targets` 恒空。CallGraph 对这类 indy 只保留保守边，不按名字猜测。

## lambda 展开

`invokedType`（indy 的 desc）按方法描述符解析：参数段是捕获参数（`capturedTypes`），返回类型是类类型时去掉 `L...;` 得到函数接口内部名（`functionalInterface`）。

BSM 静态参数按 JVMS 契约取位：

1. `args[0]`：SAM 方法类型（MethodType 常量）。
2. `args[1]`：implMethod（MethodHandle 常量），唯一调用目标。
3. `args[2]`：实例化方法类型。
4. `altMetafactory` 的 `args[3]` 起是 flags；`FLAG_MARKERS` 段跳过标记接口，`FLAG_BRIDGES` 段收集桥方法类型。

任一静态参数缺失或类型不符时只丢对应字段，不伪造目标。

## MethodHandle 分派

`reference_kind` 到分派语义的映射遵循 JVMS 4.4.8：

| reference_kind | 分派类别 | CallGraph 边 |
|---|---|---|
| 1..4 | Field（getField/getStatic/putField/putStatic） | 不产调用边 |
| 5 | Virtual | Virtual（未解析） |
| 6 | Static | Static |
| 7 | Special | Special |
| 8 | Constructor | Special |
| 9 | Interface | Interface（未解析） |

读取 MethodHandle 时校验 CP 形态：句柄条目必须指向 Fieldref/Methodref/InterfaceMethodref，owner 为 Class、名字与描述符非空，kind 在 1..9 内。任一不满足则丢弃该句柄，不影响同一指令上的其他事实。

## VarHandle 访问

命中条件是 `invokevirtual` 且 CP 目标的 owner 为 `java/lang/invoke/VarHandle`，成员名在访问模式表内。31 个模式分为三类：

- 只读：`get`、`getVolatile`、`getOpaque`、`getAcquire`。
- 只写：`set`、`setVolatile`、`setOpaque`、`setRelease`。
- 读写：`compareAndSet`、`compareAndExchange*`、`weakCompareAndSet*`、`getAndSet*`、`getAndAdd*`、`getAndBitwise*`。

`accessModeType` 等 VarHandle 普通方法不在表内，不记访问事实。字段归属（访问的是哪个字段）需要接收者的常量流，属于字符串/常量流分析层。

## CallGraph 织入

每条 `INVOKEDYNAMIC` 先产出原有的保守边：`kind == Dynamic`、callee 的 owner 段为空串、`indyFamily` 记录家族。这部分与展开失败与否无关，保证未知效果仍可被消费方识别。

家族已知时，再为每个可确定的调用目标追加补充边：

- 目标去重以整条指令为单位，同一 BSM 重复列出同一成员只发一条边。
- `kind` 由 MethodHandle 的 reference_kind 折算；`indyExpanded` 为真；`insn` 仍指向原 INVOKEDYNAMIC 指令，`session` 一致。
- 字段句柄（record getter、VarHandle 字段句柄）不产调用边。
- `includeExternalTargets == false` 时，未声明的展开目标单独丢弃；保守边是否保留仍按原规则判断，两者互不影响。

展开出的 Virtual/Interface 边与普通虚分派边同语义：本层不解析具体目标，留给层次分析层。

## API 与寿命

`IndyIndexBuilder` 接受类字节或已打开的 `ClassSession`。`addSession(nullptr)` 被忽略；同一会话指针重复喂入幂等。`build()` 逐方法升格，升格失败立即返回 `IndyBuildError`，部分结果不交付。

`IndyIndex` 是 move-only 值语义产物：

- 全部文本在构建时重新驻留进自有 `KeyPool`，查询结果不依赖输入会话寿命。
- 调用点定位用 `IndySite`（类、方法、描述符、`CodeDom::order()` 程序序下标），不用 `InsnId`，因此索引可以脱离会话持有。
- `resolutionsOf` / `varHandleAccessesOf` 返回索引内指针，按扫描序排列。

单条解析函数 `resolveIndyInsn` 与 `classifyVarHandleAccess` 直接借用传入会话的 CP 视图，供只需要即时结果的消费方使用。前者对 indy 恒返回真（结构畸形按 `Unknown` 交付），后者只在对上访问模式时返回真。

## 失败与边界

- 非 indy 指令：`resolveIndyInsn` 返回假并清空输出。
- indy 的 CP 条目畸形、缺 BootstrapMethods 属性、BSM 下标越界、BSM 句柄不可读：按 `Unknown` 交付，不抛错。
- 不解析 `ConstantDynamic`（condy）的 BSM：condy 是常量，不是调用点。
- 不执行 BSM，不展开用户自定义 BSM，不推断 BSM 返回的调用点类型之外的行为。
- 不展开 invokedynamic 的运行时参数，只读 BSM 静态参数。
- 不对 Virtual/Interface 展开边做层次解析。
- 不给 VarHandle 访问绑定字段；不在本层消费 `fieldVarHandle` 的字段参数。

## 复杂度

构建期对每条指令做常数次 CP 查看；BootstrapMethods 属性由 libjbc 解码后缓存，每个类只解码一次。展开目标数受 BSM 静态参数个数约束，通常为 1（lambda implMethod）或字段数（record）。`IndyIndex` 查询按方法键走哈希表，返回该方法的全部事实。

## 验证

`indy_test.cpp` 用手工 classfile（含 BootstrapMethods 与 Code 属性）覆盖：

- metafactory 的 implMethod、SAM 类型、实例化类型、函数接口与捕获参数；
- altMetafactory 的桥方法类型与 implMethod 共存；
- StringConcatFactory、ObjectMethods、未知 BSM 的家族事实与目标差异；
- VarHandle 读、写、读写模式分类，以及 `accessModeType` 不记事实；
- CallGraph 把 lambda indy 展开为真实静态边，展开边与原指令共用 `InsnId`；
- reference_kind 5 展开为 Virtual 且 `isVirtualDispatch()` 为真；
- 未知 BSM、record getter、字符串拼接只保留保守边或不产边。
