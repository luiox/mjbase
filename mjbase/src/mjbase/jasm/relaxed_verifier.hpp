/// @file        relaxed_verifier.hpp
/// @brief       relaxed 校验：JVM 赋值兼容语义的逐方法数据流检查。
///
/// 准绳：JVM 验证器会接受的字节码不得报错。底座：libjbc FrameEngine Strict
/// 帧域（形状/宽度/合并数据流）+ 本模块的宽松类别扫描（JVMS 4.10.1 操作数
/// 类别面，引用赋值按 JVM 赋值兼容语义、层次缺席 fail-open）。与 strict
/// 档（jhasm analyze 精确门）的分工：strict 在引用赋值点精确核名且数组不吃
/// 协变；relaxed 放行协变捷径。
///
/// 语义规则（逐条）：
/// 1. 引用赋值：同名恒真；任一侧 java/lang/Object 恒真；层次可解析走
///    hierarchy（含接口实现）；任一侧未登记（外部/JDK 类型无字节码可查）
///    fail-open 不判非法；检查点在类别扫描的赋值处（实参/接收者/字段值/
///    areturn）。
/// 2. 数组协变：同维引用基座递归（String[] 可赋给 CharSequence[]，元素侧
///    同一 fail-open 口径）；深维数组赋给浅维引用目标（String[] → Object）
///    与 primitive 基座数组按标量 Object（int[] 是 Object）放行。
/// 3. 引用合并：Strict 域合并（Top 吸收 / null 万能引用底 / 跨类公共父类）
///    由引擎内置；公共父类求解同维引用元素数组归并基座后按全维度回包
///    （String[] × CharSequence[] → CharSequence[]）。
/// 4. known-null：null 的合并与赋值合法性由帧域/扫描内置（Null 通配引用
///    目标），诊断不产生。
/// 5. aastore 元素一致性：扫描只核 ref 形态不核元素类型（fail-open
///    收窄）——非法性由真 JVM 校验器兜底，relaxed 档不代位。
/// 6. 诊断文本：错误消息用 jbc 面文本（`Method <name><desc>: …` 前缀
///    契约保持）；帧引擎/扫描全部产出 Error，无 Warning 源。
///
/// 诊断经 mjbase::jasm::Diagnostic 产出，由 JvmJasmService::validate 的
/// Relaxed 档消费。
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
