/// @file        relaxed_verifier.hpp
/// @brief       relaxed 校验：JVM 赋值兼容语义的逐方法数据流检查。
///
/// 准绳：JVM 验证器会接受的字节码不得报错。与 strict 路径（jhasm analyze 的
/// 精确类型匹配）的差异：
/// - 引用赋值按子类型判定——层次可解析（容器内已登记类，含接口实现）走
///   hierarchy；任一侧 java/lang/Object 即合法；层次不可解析（外部/JDK 类型
///   无字节码可查）不判非法（fail-open）。
/// - 数组按 JVM 协变：引用数组读方向协变（String[] 可作 Object[] 使用）；
///   primitive 基座数组按标量 Object 参与判定（int[] 是 Object，不是
///   Object[]）。
/// - known-null 诊断不产生（JVM 类型检查器不追踪确定性 null；解引用已知
///   null 的验证合法性归运行期）。
///
/// 底座：libjbytecode analysis（Analyzer + SimpleVerifier 子类），驱动方式照
/// libjhasm jvm_analyzer（每方法独立 verifier/Analyzer，异常折算为方法级
/// Error 诊断，消息形状一致）。诊断经 mjbase::jasm::Diagnostic 产出，由
/// JvmJasmService::validate 的 Relaxed 档消费。
#pragma once

#include <vector>

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/keys.hpp"

namespace mjbase::jasm {

/// @brief 对单类字节执行 relaxed 数据流检查；返回诊断（空 = 通过）。
///
/// @param classBytes 类字节（不可解析时产出一条 Error 诊断）
/// @param hierarchy  容器层次图（可空：全部外部类型按不可解析处置）；其键驻
///                   留池必须覆盖图内键且存活覆盖本调用
std::vector<Diagnostic> analyzeRelaxed(const std::vector<ca::u8>&                    classBytes,
                                       const mjbase::hierarchy::ClassHierarchy* hierarchy);

}   // namespace mjbase::jasm
