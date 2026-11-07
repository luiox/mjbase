/// @file        cycle_class_scan.hpp
/// @brief       循环继承腐坏判据：扫描集合的 super/接口边图环成员点名。
///
/// 判据口径（Recaf CycleClassRemovingTransformer 的组合式吸收：判据与删除
/// 分离，本件只产「腐坏类集合」）：一个类腐坏当且仅当沿继承边（super +
/// 接口，extends 与 implements 双边）至少一步能回到自身——自环（super 或
/// 任一接口等于自身）或处于边图上大小 ≥2 的强连通分量。腐坏类 JVM 装载
/// 必败（JVMS 5.3.5 ClassCircularityError），故该判据独立于入口可达性；
/// 环外挂靠类（环成员的子类、实现类）不判，对齐 Recaf isLoop 口径。
/// 判据消费方与 opt-in 接线语义见 doc:cycle-class-scan-spec。
///
/// 边只在扫描集合内解析：悬挂键（super/接口指向集合外的类）不可能闭合环，
/// 计入后截断（对齐 hierarchy 模块悬挂键口径）。同名重复扫描类按首个登记
/// 成节点（判据按名报告，重复名自然合并）；畸形类在 scanClassResources
/// 已被跳过（fail-open，不产出事实）。遍历全程显式栈帧 + 访问集（迭代
/// Tarjan），任何输入不挂死、不递归爆栈。
#pragma once

#include <string_view>
#include <vector>

#include "mjbase/analysis/class_scan.hpp"

namespace mjbase::analysis {

/// @brief 求扫描集合中处于继承环上的类内部名集合。
///
/// 环成员 = 自环（super 或任一接口等于自身）或 super/接口边图上大小 ≥2 的
/// 强连通分量成员；输出按节点登记序（即扫描序）排列，不含环外挂靠类。
/// 返回视图借用各扫描类会话符号池，@p scanned 存活期内有效。
std::vector<std::string_view> findInheritanceCycleClasses(
    const std::vector<ScannedClass>& scanned);

}   // namespace mjbase::analysis
