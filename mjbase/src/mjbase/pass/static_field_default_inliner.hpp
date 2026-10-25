#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// 静态字段默认值内联（legacy StaticFieldDefaultInliner 移植，mbase B 节
/// 治理版语义：多次 PUTSTATIC 预扫描计数，非单次写整族保守跳过）。
///
/// ## 规则
///
/// - 候选字段：本类 static 且无既有 ConstantValue 属性。
/// - 仅处理 `<clinit>()V`。预扫描 clinit 内 owner==本类的 PUTSTATIC 按
///   (name,desc) 计数；非单次写的字段跳过——同字段多次写入时最终值只在
///   最后一次，内联任一次都会产生错误 ConstantValue（mbase 治理修复的
///   原缺陷形态）。
/// - 常量识别按字段描述符定族：J/D ← LCONST_0/1、DCONST_0/1、LDC2_W 的
///   Long/Double；I/F ← ICONST 族、BIPUSH/SIPUSH、LDC/LDC_W 的 Int/Float
///   （constOf 语义视图）。要求常量装载是 PUTSTATIC 的顺序前驱
///   （javac 与混淆器 clinit 的标准形态）。
/// - PUTSTATIC 的 owner 必须是本类。
///
/// ## 双面落盘与一致性
///
/// - 指令面：CodeDom 逐对 erase（常量装载 + PUTSTATIC）+ lowerMethod 整体
///   重编码。erase 被拒（仍被分支/标签锚定，ErasePolicy::kRejectIfUsed）
///   时该字段保守放弃。
/// - 字段面：backing MemberInfo 新建 ConstantValue RawAttr 槽（同
///   tool_class_combiner rebuildConstantValue 口径：值条目 addInteger/
///   addLong/addFloatBits/addDoubleBits + 属性名 findUtf8/addUtf8）。
/// - 落盘顺序：erase + lower 成功之后才写字段面——lower 失败时 backing
///   Code/CP/字段均未触碰，writeClass 出原字节，无半内联态。
///
/// ## 与旧栈的语义面差异
///
/// - 旧栈树 DOM 顺位删除不看引用；本载体 erase 自带 StillUsed 防线，
///   被锚定的常量对不内联（更保守）。
/// - PUTSTATIC 计数只数 owner==本类的写入；旧栈按 (name,desc) 全数，
///   他类同名字段的写入会连带给本类字段挡刀（过保守），本版按字段身份
///   精确计数。
///
/// ## 失效
///
/// 任一字段内联成功 → none()（类元数据与 clinit 方法体都改）；无 clinit /
/// 无候选字段 / 无可内联对 → all()。
///
/// 标签：scope:class / action:propagate / stability:opt-in。
class StaticFieldDefaultInliner : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

}   // namespace mjbase::pass
