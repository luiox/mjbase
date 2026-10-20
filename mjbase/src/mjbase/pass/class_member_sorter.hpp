#pragma once

#include "jbc/ir/units.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief 类成员排序器（移植自旧栈 pass/opt/class_member_sorter）。
///
/// 排序键（与旧实现逐条对齐）：
///   - 字段：先按描述符、再按名称的字典序（稳定排序，同键保原相对序）；
///   - 方法：`<clinit>` 组最前，其次按名称字典序的其余方法，`<init>` 组垫底
///     （三个组各自稳定，组内保原相对序——对齐 Java 源 `addFirst(clinit)` +
///     `addAll(inits)` 的分组语义）。
///
/// 载体与写回（新底座重定型）：排序落在 ClassSession 的会话骨架
/// （unit().fields / unit().methods）与 backing（ClassFile::fields/methods 的
/// MemberInfo 序列）两侧，同排列同步重排；双面落盘与 MethodUnit.backingIndex
/// 同步由类级结构编辑原语（mjbase::spi::reorderClassFields / reorderClassMethods，
/// pass-contract §1 双面落盘义务的 API 化）承担，本 pass 只构造排列与声明 PA。
/// MethodUnit.backingIndex 随重排改指新物理位，保证后续 promote/lower 的正确性
/// （成员条目整体移动，Code 属性内容不受触碰，方法稳定身份 MethodId 随骨架走）。
/// 排序未发生时返回 all()（不触发无谓重序列化）；发生排序返回 none()（类/成员
/// 级分析按契约失效）。
///
/// 与旧实现的一处显式偏离：旧实现无条件返回 none()（已序类也被标记改动、
/// 白付重序列化），与其自身头注「发生排序时返回 none()……否则返回 all()」
/// 相悖；本实现按声明的条件语义落地。
///
/// 输入必须是可解析 class（会话已建立即满足）；`opt-in`，不得进入默认优化链。
///
/// @par 标签
/// - `scope:class`
/// - `action:reorder`
/// - `stability:opt-in`
class ClassMemberSorter : public mjbase::spi::ClassPass
{
public:
    mjbase::spi::PreservedAnalyses run(jbc::ir::ClassSession&            session,
                                       mjbase::spi::ClassAnalysisManager& cam) override;
};

}   // namespace mjbase::pass
