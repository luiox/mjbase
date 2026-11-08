/// @file        semantic_flag_rules.hpp
/// @brief       JVMS 语义级 flag 规则门：defineClass 即拒的修饰位组合，
///              结构层先行拒收（fail-closed）。
///
/// 背景（issue #179）：verify 既有面只测字节结构（malformed=0），JVMS 4.x
/// 的语义级 flag 规则不查——接口字段修饰位必须为 ACC_PUBLIC/ACC_STATIC/
/// ACC_FINAL 组合，非法组合（如 ``0x100A`` private static synthetic）JVM
/// defineClass 即 ClassFormatError，verify 却全绿，给出虚假安全信号。
///
/// 规则库与 JVM 行为逐位对齐：oracle 是 JDK 17 HotSpot classFileParser.cpp
/// 的 ``verify_legal_class/field/method_modifiers`` 加 ``<clinit>``/接口
/// ``<init>`` 特例（16 条规则，逐条见 .cpp 注释）；JVMS 4.1/4.5/4.6 的
/// "must" 文本只收 JVM 真正强制执行的子集——JVM 会接受的字节码不得报错
/// （与 relaxed verifier 准绳互为反向面），因此 reserved bit 非零、接口
/// 字段 ACC_MANDATED 这类 JVM 不查的位不设规则。版本窗随 HotSpot：
/// ACC_STRICT 自 major 61 退役、接口方法 52.0 起走现代（public/private
/// 恰一）分支、``<clinit>`` 静态强制自 51.0 起。
///
/// 诊断全部 Error（``DiagnosticStage::Validate``），消息沿用 HotSpot
/// ClassFormatError 文本形态（十六进制位面原样呈现，如 ``0x100A``）——
/// 与 JDK defineClass 对拍可直读互认。本门与数据流档正交：JvmJasmService::
/// validate 在 Off 之外的两档（Relaxed/Strict）都先跑本门；Off 档按契约
/// 直通零诊断。命中即返回，不再叠加数据流分析（错误类不必再算帧）。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"

#include "jvm_jasm_service.hpp"

namespace mjbase::jasm {

/// @brief 对单类字节执行 JVMS 语义级 flag 检查；返回诊断（空 = 通过）。
///
/// @param classBytes 类字节；字节不可解析时返回空（解析失败由 validate
///                   的读取闸先行报告，本门不重复归因）
std::vector<Diagnostic> analyzeSemanticFlags(const std::vector<ca::u8>& classBytes);

}   // namespace mjbase::jasm
