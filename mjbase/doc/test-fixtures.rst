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

唯一例外：jasm 装配器刻意拒绝的非法形态（坏 ``StackMapTable`` 坏帧类等
语义非法样本）——允许从装配出的合法类出发做**单点最小偏差**改动，改动
前后各做一次 relaxed validate 自检；从零手拼整类始终禁止。
