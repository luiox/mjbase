#pragma once

#include "jbc/ir/units.hpp"

#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 修复方法 ACC_VARARGS 位与描述符的失配（位与描述符四象限裁决）。
///
/// JVMS 4.6 对 ACC_VARARGS 的约束是单向的：置位方法描述符的尾参必须是数组
/// 类型；反向不成立（数组尾参方法可以不置位——合法 Java 的 `f(String[])`）。
/// 混淆器两向都造：给无数组尾参的方法置位（非法态），或把 varargs 方法的
/// 位抹掉（合法态但破坏 javac 重编译的调用点解析与反射 isVarArgs 事实）。
/// Recaf 参考件（IllegalVarargsRemovingTransformer，cleanup.illegalvarargs）
/// 只做摘位方向；本件按修复语义做成双向失配修复（任务口径）：
///
/// - 位有 + desc 无阵列尾参（含无参方法）→ 摘位（Recaf 对齐方向）；
/// - 位无 + desc 有阵列尾参 → 置位（修复方向，Recaf 之外的扩展——语义影响
///   见下）；位与 desc 匹配的两侧象限原样不动。
///
/// **置位方向的语义影响须知**：数组尾参方法本可以合法不置位（非 varargs
/// 的 `f(String[])` 源形态），置位会把它的反射 isVarArgs 翻转、javac 调用
/// 点解析改走变参通道。本件因此严格 opt-in，供「混淆器抹位破坏重编译」的
/// 语料按需启用；默认链永不收。
///
/// 判据面：方法描述符参数段解析（`(` 起逐 FieldType 消费至 `)`；尾参含
/// 数组维度 `[` 即阵列尾参）。描述符形状非法（无 `(`/`)`、参数段残缺）
/// fail-closed 跳过——两向都不改。修复面两面落盘（unit() + backing，
/// pass-contract §1 双面义务——标志位两面都有模型）。
///
/// PreservedAnalyses 条件语义：发生修复返回 none()，未发生返回 all()。
/// 幂等：修复到位后四象限全部匹配，二跑零命中。
///
/// 本 pass opt-in：已注册默认表（IllegalVarargsRemover，scope:class），
/// 默认链不收——经管线配置直引启用或显式构造实例驱动。
///
/// @par 标签
/// - `scope:class`
/// - `action:sanitize`
/// - `stability:opt-in`
class IllegalVarargsRemover final : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

}   // namespace mjbase::pass
