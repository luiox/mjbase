#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// 静态数组字段标量化（第一切片，#39）：private static 的原始类型数组
/// 字段，分配点唯一、长度编译期常数、所有访问都是常量下标窗口且无整体
/// 引用时，拆成按访问下标命名的 N 个标量字段——分配（newarray）与逐元素
/// 初始化存储、下标计算指令全部消失，读窗口从三条指令缩成一条 getstatic。
///
/// ## 候选与守卫（全部满足才拆，任一失败整字段放弃）
///
/// - 字段 private static，描述符为原始类型数组（[I [J [D [F [Z [B [C [S；
///   引用数组挂账后续切片）。
/// - 本类无 NestHost/NestMembers——nest 语义下 private 可被 nest 内他类
///   访问，本类扫描不闭包；有 nest 属性整 pass 放弃（fail-closed）。
/// - 分配窗口恰一处：`iconst_LEN; newarray <atype>; putstatic f`（atype
///   与描述符匹配）。putstatic f 的其他出现（重赋整个数组）→ 放弃。
/// - 读窗口 `getstatic f; iconst_i; xALOAD` 与写窗口 `getstatic f;
///   iconst_i; <单条常量装载>; xASTORE`（xALOAD/xASTORE 与元素描述符
///   匹配）。getstatic f 的其他出现（arraylength、传参、返回、再存储等
///   整体引用）→ 放弃。
/// - 有读写窗口时必须存在分配窗口——无分配的字段读是 null 数组语义
///   （NPE），拆成标量默认值不等价。
/// - 下标越 [0, LEN) → 放弃（越界抛 AIOOBE 的语义不能静默吞）。
/// - 新字段名 `<old>_<i>` 与本类现有字段名冲突 → 放弃。
/// - 窗口内任一指令被标签锚定（分支目标/异常表/调试边界）→ 放弃——
///   判定期排除，改写阶段零失败。
///
/// ## 改写与落盘
///
/// - 读窗口：getstatic 改指 `<old>_<i>`（元素描述符）；const_i 与 xALOAD
///   删除。写窗口：getstatic 与 const_i 删除；xASTORE 原位改 PUTSTATIC
///   `<old>_<i>`（常量装载保留原位）。分配窗口三条整体删除。
/// - 只生成被访问下标的标量字段（未访问元素本就无读点，随分配删除）；
///   新字段 access = private static（final 保留挂账）。
/// - 落盘序：全部窗口改写并 lowerMethod 成功后才 addField/removeField；
///   任一步失败会话弃置，backing 无半拆态。旧字段在引用清零后删除。
///
/// ## 语义面声明
///
/// - 写窗口的值第一切片只认单条常量装载（常量表场景）；多指令表达式值
///   挂账。实例字段（getfield/putfield，继承族约束）与引用数组（元素
///   类型推断）挂账后续切片。
/// - 改写后方法栈深只降不升，maxStack 偏大方向合法（JVMS 允许）。
///
/// ## 失效
///
/// 任一字段拆成功 → none()；无候选 / 全部放弃 → all()。
///
/// 标签：scope:class / action:rewrite / stability:opt-in。
class StaticArrayFieldScalarizer : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

}   // namespace mjbase::pass
