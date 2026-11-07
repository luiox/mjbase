测试 fixture 纪律
=================

状态：定稿

测试里构造类、方法与指令序列一律从 jasm 文本装配出发（``mjbase/jasm`` 的
``JvmJasmService``，装配惯例范例见 ``method_inliner_test``），**禁止手写
classfile 字节**。被禁的形态包括但不限于：opcode 逐字节 ``putU8``/``putU16``
拼装、常量池/属性/帧字段的手工布局、以及 ``FixtureBuilder`` 一类字节级
builder 的引入——手拼字节拼错不报错、与 jasm 权威方言漂移、意图不可读，
且每批重复造轮子。

理由：jasm 是本仓指令工程的权威方言（``class_query(kind=jasm)`` 的返回与
``edit_apply`` 的 diff 基线都是它），测试 fixture 与权威方言同源，期望值
直接可读可改；字节层是装配器的输出物，不是测试的输入物。

需要 pc/槽位级精确期望（LVT 区间、帧类型推断、栈深）时，把期望值写进断言
对装配产物校验，不倒回头拼字节。

唯一例外有两类：

1. jasm 装配器刻意拒绝的非法形态（坏 ``StackMapTable`` 坏帧类等语义非法
   样本）——允许从装配出的合法类出发做**单点最小偏差**改动，改动前后各
   做一次 relaxed validate 自检；从零手拼整类始终禁止。
2. jasm 方言未覆盖的属性构造（模拟上游 pass 产出的中间实样，如
   ConstantValue 属性）——允许走 CF 级属性写面（``addUtf8``/``addInteger``
   + ``AttrSlot::makeNew`` + ``RawAttr``，SFDI 同款），前提：基类从 jasm
   装配出发、构造范围限于该属性本身、注释里写明模拟的是哪道上游工序。

源码与 jasm 的字符串书写规范（C++ raw string 定界符）：Java 源码用
``R"JAVA(...)JAVA"``，jasm 文本用 ``R"JASM(...)JASM"``——定界符即语言标签，
全文检索与评审一眼可辨，也避免文本内出现 ``)`` 时提前截断。
