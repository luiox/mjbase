#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 方法内受限监视器清除器：监控对象全程不逃逸出方法、唯一用途是
///        monitorenter/monitorexit 的成对监控块整组摘除（混淆器注入的
///        junk synchronized 块的无害化）。
///
/// 识别形（全部逐指令相邻 + 无标签锚定）：
///   [NEW X; DUP; (ARG)*; INVOKESPECIAL X.<init>(...)V; ASTORE L]   头
///   ... [ALOAD L; MONITORENTER] ... [ALOAD L; MONITOREXIT]          成对尾
/// 实参游程 ARG 逐条认领无副作用指令（常量推入，condy 不认领；局部装载
/// 族含紧凑形）——可整组擦除；任何调用/字段访问/分配/dup 形/iinc 不认领，
/// 首个不认领指令不是本类 <init> 即整头拒收。整组 eraseBatch 摘除（组级
/// 原子：异常表边界锚定任一成员由编辑器前置校验整组拒绝）。
///
/// 语义论证：对象由 NEW 诞生、唯一存储 L、L 的全部读点都是监控接收者
/// （def-use：enter/exit 的栈定义边汇于唯一 ASTORE 定义，ASTORE 的全部
/// 使用边恰为两读点与两监控点）——身份不出方法，锁状态对任何其他线程
/// 不可观测，单线程内 monitor 簿记（重入计数/持锁态）也不可观测；NEW
/// 非空无 NPE，enter 在全部路径先于 exit（体内跳入标签门拒绝）无 IMSE
/// 变化。
///
/// 安全门：恰一 enter 一 exit（ASTORE 定义的 Local 使用边恰 2 且都是
/// 监控接收者、读点的到达定义恰为该唯一 ASTORE、use 全集恰为两读两
/// 监控）；实参游程全数无副作用可擦（副作用实参指令擦除即语义变化，
/// fail-closed 拒收）；窗内任何成员（含头段与
/// 两监控点）及 enter..exit 区间内部锚定存活标签（分支/异常表）整组
/// 拒收；方法参数作监控接收者（def = Parameter）天然失配。
///
/// 组按定义圈定而非按槽位：同一槽位被多组先后复用（ASTORE 覆写）时
/// 各组独立成组、逐一摘除。maxStack/maxLocals 不变。发生摘除返回
/// none()，否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:eliminate`
/// - `stability:opt-in`
class UncontendedMonitorEliminater : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
