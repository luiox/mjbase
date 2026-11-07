#pragma once

#include "jbc/ir/units.hpp"

#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 巨型注解删除选项。
struct LongAnnotationRemoverOptions
{
    /// @brief 描述符长度阈值（判定口径：保留码点长度 < 阈值的注解，长度
    ///        ≥ 阈值即删）。默认 150 与 Recaf cleanup.longanno 同源（其
    ///        DEFAULT_MAX_LENGTH=150）。阈值依据：javac 产物的注解描述符
    ///        长度集中在个位数到几十（Lcom/example/Anno; 形），150 远在
    ///        正常产物分布之外；而混淆器垃圾注解（AAAAAA… 填充族）描述
    ///        符长度普遍数百起步——阈值落在两者之间的空档带，既不会误删
    ///        真实注解又能吃下填充族。可配置面供特殊语料（超长合法包名）
    ///        上调。
    ca::i32 maxDescLength = 150;
};

/// @brief 清理三层注解面中描述符长度 ≥ 阈值的巨型垃圾注解（体积启发判据）。
///
/// 与既有删除件的分工：SpecificAnnotationRemover 是黑名单判据（描述符集合
/// 成员判定）、IllegalAnnotationRemover/MetaInfoRemover 是形状/全量判据，
/// 本件是**体积启发**判据——三者正交，本件不查黑名单、不查形状合法性，
/// 只按描述符长度裁决。Recaf 参考件（LongAnnotationRemovingTransformer，
/// cleanup.longanno）只取判据口径，架构按本仓 pass 契约自行设计。
///
/// 判据（Recaf 口径）：注解类型描述符长度 < 阈值保留，≥ 阈值整条删除。
/// 覆盖面与 Recaf visitor 逐面对齐：
/// - 类/字段/方法三层的 Runtime{Visible,Invisible}Annotations 条目；
/// - 三层与 Code 嵌套的 Runtime{Visible,Invisible}TypeAnnotations 条目
///   （Code 嵌套 = cast/instanceof/new/localvar 目标形态，父 Code 槽连带
///   置脏——meta 家族同款显式有界破例）；
/// - 方法级 Runtime{Visible,Invisible}ParameterAnnotations 条目（typed
///   行模型为主；typed 解码拒绝的 legacy Raw 形状保守保持原样——
///   fail-closed，不产出半截改写）；
/// - **嵌套注解递归**（Recaf LongSubAnnoRemover 面）：键值对与数组元素中
///   tag='@' 的嵌套注解，其描述符超阈值时该键值对/数组元素整项删除，父级
///   注解保留（值表可为空），递归深入嵌套层与数组层。
///
/// 与 Recaf 的两处显式差异：
/// - 长度口径：Recaf 按 Java String.length()（UTF-16 码元数）；本件按码点
///   数（家族既有口径，IllegalAnnotationRemover 同源）。ASCII 描述符（垃圾
///   填充注解的实际形态）两口径逐字节一致，差异仅在增补平面字符名。
/// - Recaf 不覆盖 record 组件与 AnnotationDefault，本件同样不触碰
///   （判据面照抄，不扩面）。
///
/// 槽内过滤、删空连槽摘除；解码失败/属性形状不符时保守保持原样。
/// PreservedAnalyses 条件语义：发生改写返回 none()，未发生返回 all()。
/// 幂等：幸存条目二跑零命中。
///
/// 本 pass opt-in：已注册默认表（LongAnnotationRemover，scope:class），
/// 默认链不收——经管线配置直引启用或显式构造实例驱动。
///
/// @par 标签
/// - `scope:class`
/// - `action:eliminate`
/// - `stability:opt-in`
class LongAnnotationRemover final : public mjbase::spi::ClassPass
{
public:
    explicit LongAnnotationRemover(LongAnnotationRemoverOptions options = {});

    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;

private:
    LongAnnotationRemoverOptions options_;
};

}   // namespace mjbase::pass
