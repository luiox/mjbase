# indy / MethodHandle / VarHandle 解析（Indy Resolution）

## 背景

`invokedynamic` 的 callee 不在 CP 直接成员引用里：链接目标藏在 BootstrapMethods 的静态参数中，CallGraph 只能记成 owner 为空的保守边，lambda 方法体、record getter 等真实调用因此缺失。VarHandle 同理：编译器编码的是签名多态占位成员，被访问字段由运行时接收者决定。

本层把 BSM 契约读成指令级事实（`IndyIndex`），CallGraph 消费可确定的目标补边。**不执行 BSM、不模拟类加载、不对未知家族猜目标**。

## 覆盖边界

家族只由 BSM 句柄的 owner 与 name 决定，已知 owner 下的未知成员一律记 `Unknown`。已识别：LambdaMetafactory（展开 implMethod 句柄）、StringConcatFactory（仅家族事实）、ObjectMethods（record 字段 getter）、ConstantBootstraps（仅 `invoke` 展开首个句柄）、SwitchBootstraps（仅家族事实）。未知家族保留 CP 可读的 name/desc，`targets` 恒空，CallGraph 只留保守边。

BSM 参数缺失或类型不符时只丢对应字段，不伪造目标；CP 畸形、缺 BootstrapMethods 属性等按 `Unknown` 交付，不抛错（fail-closed）。不解析 ConstantDynamic——它是常量不是调用点；不展开 indy 的运行时参数，只读静态参数。

## CallGraph 织入

每条 indy 先产出保守边（owner 空、记家族），与展开与否无关；家族已知时再为可确定目标追加补充边，`kind` 由 reference_kind 折算，字段句柄不产调用边。展开的 Virtual/Interface 边与普通虚分派同语义：本层不解析具体目标，留给层次分析层。VarHandle 访问记读/写/读写分类，字段归属需要接收者常量流，属于字符串流分析层。
