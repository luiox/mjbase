# Slot 级 def-use 与字段访问事实

> 代码：`mjbase/src/mjbase/asm/analyzer/def_use.{hpp,cpp}`。

## 背景

`origin_domain` 每个槽只保留一个来源，多来源汇合后退化为 Unknown，回答不了「这个局部槽上有哪些写会到达这次读」。死存储删除、写后未读判定需要后者。

本模块在 `jbc::analysis::DomainEngine` 上搭并集值域：栈机迁移、cat2 槽布局、dup 与异常边全由引擎负责，本模块只实现定义集合的合并与帧后处理，不复制第二份 per-opcode 表。

## 模型要点

- `DefId` 两类：入参（`Parameter`）与指令（`Instruction`）。局部写重定义该局部，load 是恒等搬运不产定义，dup/swap 只复制槽不产使用边。
- 集合有序去重且有上限，溢出置 `truncated` 并保留确定的子集——不同合并顺序得到同一集合。

## 负向结论纪律

任何「没有使用 ⇒ 可删」的结论必须同时确认：`ok()` 为真、`truncated()` 为假、`hasUnresolvedFieldRefs()` 为假，并叠加调用方自己的扫描范围假设。三条任一不满足时边集不完整，结论不可用。

## 字段访问与分工

方法内 get/put 产 `FieldAccess`（方向、静态位、接收者与存入值的到达定义）；全程序直接字段事实由 `ReferenceIndex` 提供，见 [field-access-graph.md](field-access-graph.md)。
