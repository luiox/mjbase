#pragma once

#include "jbc/analysis/frame.hpp"
#include "jbc/ir/units.hpp"

namespace mjbase::spi {

struct MethodScope;

/// @brief 变换管线的方法体写回辅助面：嵌套属性布局适配 + A1 帧链再接线。
///
/// 职责归属（pass-contract §2：backing 只被管线触碰）：两个入口都只由
/// runMethodPipelineOverSession 在 lower 前后调用，pass 不得自行调用。
///
/// - prepareNestedAttrsForLayout（lower 前调）：指令布局将变时，把 backing
///   Code 嵌套属性预适配到「pcmap 可重定位」形态。悬垂收敛规则（写回侧
///   行号/变量元数据的显式策略，裁掉而非静默错位）：
///   - LNT 行起点悬垂（其 pc 不再属于任何存活指令）→ 向前吸附到下一个存活
///     pc（行号留原位语义：行所辖首条存活指令承接该行号）；之后无存活指令
///     → 整行裁去（该行号所辖代码不复存在）。
///   - LVT/LVTT 行起点悬垂 → 同规则向前吸附；终点（start+length）悬垂 →
///     向前吸附到下一个存活 pc、之后无存活收敛到旧 code 末尾（lower 同款
///     映射到新长度）——区间端点向外吸附意味着区间吞并被删段，即跨被改
///     指令的作用域合并为一段；收缩到空区间（length 0）保持行不丢。
///   - SMT 帧目标或 Uninitialized 载荷悬垂时整体摘除该属性（帧内容无法搬
///     运，摘除后由 rewireFramesAfterLower 再生；调用方须保证其后随调用，
///     否则方法缺帧）。
/// - rewireFramesAfterLower（lower 后调）：A1 链消费入口。Strict 帧重算
///   （布局已定，srcPc 即新 pc）→ applyComputedMaxes 覆盖采用 → 第二次
///   lower 落 maxes → buildStackMapTable 再生 SMT 并挂回 Code（替换/补位）。
///   版本 < 50 的类整体跳过（SMT 无定义，调用方语义 = 保持 relocation 后
///   现状）。Strict 不可算（含 jsr/ret）显式抛出——不伪造帧，容错档位可
///   据此跳过该方法。
///
/// 失败语言：异常（pass 域编排层豁免点，pass-contract §3），文本带方法定位；
/// 模块入口把异常收敛为 Result 的既有管线语义不变。
namespace frame_rewire {

/// lower 前的嵌套属性布局适配；@p code 为本方法已变换、待 lower 的 CodeUnit。
/// 返回是否摘除了既有 SMT（调用方据此保证 rewire 后随）。
bool prepareNestedAttrsForLayout(jbc::ir::ClassSession& session, const jbc::ir::CodeUnit& code);

/// lower 后的 A1 帧链再接线（Strict 重算 + maxes 采用 + SMT 再生挂回）。
void rewireFramesAfterLower(jbc::ir::ClassSession& session, const MethodScope& scope,
                            jbc::analysis::ITypeResolver& resolver);

}   // namespace frame_rewire

}   // namespace mjbase::spi
