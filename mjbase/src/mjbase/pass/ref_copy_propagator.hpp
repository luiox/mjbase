#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 引用复制传播器：单来源不可变引用的 aload 原位改写为来源指令
///        （旧栈 mbase pass/opt/ref_copy_propagator 按 OriginDomain 载体重
///        推导）。
///
/// 适配形态：混淆器把引用值存入暂存槽再读回（`ldc "x"; astore s; …;
/// aload s`），打断生产者与消费方的模式匹配。引用来源值不可变时，读回
/// 与重放生产者等价：aload 原位改写为来源形态（aload 换槽 / ldc 引用
/// 常量 / aconst_null）。
///
/// 载体：traceOrigins 来源追踪（入参播种）。aload 读槽的单来源事实即
/// 「全路径该槽当前值由同一指令产生」——来源穿透 store→load，旧栈的
/// 「方法级恰一次 store」预扫描门槛被来源单值性自然内化（多来源汇合
/// 退化 Unknown → 保留）。来源槽再写失效同样内化：再写后读点的到达
/// 来源是新写者，旧来源事实不复存在。
///
/// 生产者白名单（值不可变的引用来源）：
///   - aload 族（局部引用装载；改写为同族换槽 aload，自赋值同槽跳过）；
///   - ldc 引用常量（CpTag String/Class/MethodType——同一常量池条目引用
///     恒等；int/float/double ldc 与 Handle/Dynamic 保守不认）；
///   - aconst_null。
///   getstatic/getfield（字段可变）、方法调用、new、数组读取、dup 变体
///   一律不认。
///
/// 相对旧实现偏离（逐条申报）：
/// 1. 载体换来源域：旧栈「方法级恰一次 store」的保守预扫描由来源单值性
///    内化，多 store 但各路径单来源的形态新栈可判（覆盖只宽不窄）。
/// 2. 来源域穿透 aload（aload 不是产值点）：改写直达最初来源而非中途
///    aload——多级暂存槽链一步折叠到底，覆盖更远；代价是 aload→aload
///    即时折叠不可判（最初来源为入参 Param 时不可克隆、其槽位可再写不
///    可回指，保守保留），该形态交 StoreLoadRelayEliminator / RSE 族
///    收敛。
/// 3. 旧栈克隆生产者节点；新栈原位改写 aload 的 op/a/cp 字段（栈效应同
///    为 1 引用，maxStack/maxLocals 不变；srcPc 保留读点自己的行号锚）。
/// 4. 单遍扫描不做不动点（与旧栈一致；ldc/aconst_null 改写后自身不构成
///    新的 aload 读点，改写不影响其他读点的到达来源）。
///
/// 安全门：帧数据流失败整件跳过；读点不可达、来源 Unknown/Param、来源
/// 指令不可见、白名单外一律保留（负向结论方向保守）。
///
/// 发生改写返回 none()，否则 all()。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class RefCopyPropagator : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&            scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
