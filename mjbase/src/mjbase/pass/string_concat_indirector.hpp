#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 字符串拼接 indy 化简器：java/lang/StringBuilder 构造 + append 链 +
///        toString 的直线形态改写为单条 invokedynamic
///        StringConcatFactory.makeConcatWithConstants（javac 9+ 拼接规范形）。
///
/// 识别窗（严格直线，逐指令相邻）：
///   [NEW StringBuilder; DUP; (LDC String)?; INVOKESPECIAL <init>;(String)?V;
///    (ARG; INVOKEVIRTUAL append(X))+ ; INVOKEVIRTUAL toString()Q-String;]
/// 改写：NEW 原位改写为 INVOKEDYNAMIC（参数随后按原位压栈），DUP/init/
/// append/toString 批量 erase（fallthrough 重定向链式闭合）。栈形净效应
/// 不变（弹全部拼接实参 + 压 String），结果消费面无感。
///
/// 实参认领面（fail-closed）：每 append 的实参为**自包含纯计算链**（反
/// 向需值抽取，cat2 记一个值）——常量推入（ICONST 族/BIPUSH/SIPUSH/
/// LCONST 族/FCONST 族/DCONST 族/LDC 数值与引用常量/ACONST_NULL；
/// condy 不认领，bootstrap 有求值副作用）、原始局部装载族（ILOAD 等；
/// ALOAD 静态型未知不入链）、二元纯算术/移位/位/比较、取负与数值转
/// 换。栈操作/调用/字段/数组/分配不入链，需求未尽遇链外指令整窗拒收
/// （fail-closed）。链顶值与 append 形参的匹配由原方法验证器保证，
/// indy 实参静态型取形参描述符；链指令保留原位，改写后在更低栈深执
/// 行（SB 接收者不再驻栈），原 maxStack 天然覆盖链内峰值；INDY 汇合
/// 点深度 = 入口栈深 + 全部实参槽宽，帧引擎抬升兜底。
///
/// 安全门：类版本 ≥ 53（Java 9，StringConcatFactory 入场线）；窗内任何
/// 成员（含 NEW 头）锚定存活标签即拒收——分支目标进入链体在擦除后语义
/// 不保，异常表 Boundary 锚定同拒（labelsAt 覆盖两角色）；<init>(String)
/// 实参须为 LDC 字符串常量（窗内 LDC 保留为 indy 实参，其余成员擦除）；
/// append 返回型须为 StringBuilder。BSM 条目经 appendBootstrapMethod 同形
/// 复用（配方的全实参形态 \\u0001×N），indy CP 条目经 internDynamic。
/// 任一 intern/erase 失败整窗放弃（eraseBatch 前置校验保证无半删）。
///
/// 拼接实参值语义不变（原栈值序列原样成为 indy 实参）；常量不嵌入配方
/// （全部 \\u0001 槽位）——配方嵌入属后续精化。发生改写返回 none()，否则
/// all()。maxStack/maxLocals 不变。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class StringConcatIndirector : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&          scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
