#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// 静态数组字段标量化（#39）：private static 的数组字段，分配点唯一、
/// 长度编译期常数、所有访问都是常量下标窗口且无整体引用时，拆成按访问
/// 下标命名的 N 个标量字段——分配（newarray/anewarray）与逐元素初始化
/// 存储、下标计算指令全部消失，读窗口从三条指令缩成一条 getstatic。
///
/// ## 候选与守卫（全部满足才拆，任一失败整字段放弃）
///
/// - 字段 private（static 与实例同面），描述符为一维数组：原始类型数组
///   （[I [J [D [F [Z [B [C [S）与一维对象数组（[Ljava/lang/String; 等）；
///   多维与数组元素（[[ 等）不支持。
/// - 本类无 NestHost/NestMembers——nest 语义下 private 可被 nest 内他类
///   访问，本类扫描不闭包；有 nest 属性整 pass 放弃（fail-closed）。
/// - 分配窗口恰一处：static 形 `iconst_LEN; newarray <atype>; putstatic f`
///   或 `iconst_LEN; anewarray <elem>; putstatic f`（元素面与描述符匹配）；
///   实例形前置接收者 aload（`aload recv; …; putfield f`）且只在 <init>
///   收录——运行一次语义（reset 类重执行分配的重置语义不闭包 → 放弃）。
///   实例分配窗四条整体删除（接收者 aload 的唯一消费者就是被删的
///   putfield，保留会悬出栈）。putstatic/putfield f 的其他出现（重赋
///   整个数组）→ 放弃。
/// - 读窗口 `getstatic/getfield f; iconst_i; xALOAD`（实例窗头前须邻接
///   接收者 aload，保持原位由改写后的 getfield 消费）与写窗口
///   `getstatic/getfield f; iconst_i; <单条常量装载>; xASTORE`（实例形同带
///   前置接收者，改写后由新 putfield 消费；xALOAD/xASTORE 与元素描述符
///   匹配；引用值的常量面为 aconst_null 与 ldc String/Class）。字段整体
///   引用的其他出现（arraylength、传参、返回、再存储、前驱非 aload 的
///   访问）→ 放弃。
/// - 引用元素类型精化（#39 切片 3）：读窗后继 checkcast 目标与写窗常量
///   值静态型（ldc String/Class）作为该下标的元素型证据；同下标证据不
///   一致 → 整字段放弃。声明元素为 java/lang/Object 时标量字段按证据
///   精化（`L<证据类>;`），证据位上的 checkcast 与新字段描述符一致（CCE
///   被写值面证明不可达）随读窗折叠；无证据下标沿用声明元素描述符。
///   声明元素非 Object 时证据必须与声明一致（无层级查询面，fail-closed）；
///   与声明一致的证据不登记、checkcast 保持既有「保留原位」语义。
/// - 有读写窗口时必须存在分配窗口——无分配的字段读是 null 数组语义
///   （NPE），拆成标量默认值不等价。
/// - 下标越 [0, LEN) → 放弃（越界抛 AIOOBE 的语义不能静默吞）。
/// - 新字段名 `<old>_<i>` 与本类现有字段名冲突 → 放弃。
/// - 窗口内任一指令被标签锚定（分支目标/异常表/调试边界）→ 放弃——
///   判定期排除，改写阶段零失败。
///
/// ## 改写与落盘
///
/// - 读窗口：getstatic/getfield 改指 `<old>_<i>`（元素描述符）；const_i
///   与 xALOAD 删除。写窗口：getstatic/getfield 与 const_i 删除；xASTORE
///   原位改 PUTSTATIC/PUTFIELD `<old>_<i>`（常量装载与接收者保留原位）。
///   分配窗口整体删除（实例形含接收者）。
/// - 只生成被访问下标的标量字段（未访问元素本就无读点，随分配删除）；
///   新字段 access = private static / private（随原字段静态性；final
///   保留挂账）。
/// - 落盘序：全部窗口改写并 lowerMethod 成功后才 addField/removeField；
///   任一步失败会话弃置，backing 无半拆态。旧字段在引用清零后删除。
///
/// ## 语义面声明
///
/// - 写窗口的值只认单条常量装载（常量表场景）；多指令表达式值挂账。
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
