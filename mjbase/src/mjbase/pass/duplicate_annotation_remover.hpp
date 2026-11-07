#pragma once

#include "jbc/ir/units.hpp"

#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 清理重复注解：同目标同描述符的注解多实例去重（保留首个实例）。
///
/// JVM 规范不拒收同描述符重复条目，但 javac 与全部合法产物不会产出——重复
/// 实例是混淆器/字节码手术的残留，且反射面（getAnnotation 族只认首个）与
/// javac 重编译面（duplicate annotation 错误）都会失真。删除/修复面
/// fail-closed：判据（同目标同描述符）不满足即字节不动。Recaf 参考件
/// （DuplicateAnnotationRemovingTransformer，cleanup.duplicateanno）只取
/// 判据口径，架构按本仓 pass 契约自行设计。
///
/// 判据（Recaf 口径，按宿主分集、首个实例保留、后继实例删除）：
/// - 普通注解条目：键 = 类型描述符内容（同内容不同 CP 条目同样命中）；
/// - type annotation 条目：键 = 目标位全集（target_type + target_info 各
///   字段 + type_path + 描述符内容）——目标位不同的同描述符条目各保留；
/// - 参数注解条目：键 = (参数行下标, 描述符内容)，行数与行序保持；
/// - 分集粒度：类级一份集；字段/方法各一份集（同描述符在不同成员上各保留
///   首个）；Code 嵌套槽一份集；record 组件一份集/组件。visible 与
///   invisible 两族共享同一份集（Recaf visitor 同口径——同一描述符跨
///   RVA/RIA 两槽只留先到者）。
///
/// 覆盖面：三层 Runtime{Visible,Invisible}Annotations 与
/// Runtime{Visible,Invisible}TypeAnnotations（含 Code 嵌套槽，父 Code 槽
/// 连带置脏——meta 家族同款显式有界破例）、方法级参数注解两槽、record
/// 组件的注解与 type annotation 槽（父 Record 槽连带置脏）。
///
/// 与 Recaf 的一处显式差异：Recaf 不递归注解值面（值内嵌套注解不参与
/// 去重），本件同样不碰值面——判据面照抄，不扩面。
///
/// 槽内过滤、删空连槽摘除（参数注解部分摘除留槽、全空摘槽）；解码失败/
/// 属性形状不符时保守保持原样。PreservedAnalyses 条件语义：发生改写返回
/// none()，未发生返回 all()。幂等：去重后无重复实例，二跑零命中。
///
/// 本 pass opt-in：已注册默认表（DuplicateAnnotationRemover，scope:class），
/// 默认链不收——经管线配置直引启用或显式构造实例驱动。
///
/// @par 标签
/// - `scope:class`
/// - `action:sanitize`
/// - `stability:opt-in`
class DuplicateAnnotationRemover final : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

}   // namespace mjbase::pass
