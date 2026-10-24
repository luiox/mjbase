#pragma once

#include "mjbase/spi/pass_pipeline.hpp"

namespace mjbase::pass {

/// @brief char[] → String 化简器：把「定长 char 数组逐元素常量填充」的构建
///        序列改写为字符串常量装载（移植自 mbase pass/opt/
///        char_arr_to_str_simplifier）。
///
/// 匹配矩阵：
///
/// ```
/// ICONST_n / BIPUSH / SIPUSH / LDC(Int)   // len ≥ 0
/// NEWARRAY T_CHAR
/// ( DUP; ICONST_j; ICONST_c / BIPUSH / SIPUSH / LDC(Int); CASTORE ) × len
/// ```
///
/// 改写产物：`LDC "<text>"; INVOKEVIRTUAL java/lang/String.toCharArray ()[C`
/// ——序列值语义不变（数组内容 = 字符串的 char 序列），栈效应不变（都是
/// 净推 1 个 char[]）。字符按 JVM int→char 截断语义收进 UTF-16 码元
/// （CASTORE 截断），经 UTF-16 → UTF-8 → MUTF-8 编码 intern（libca
/// Utf16String + libjstr utf8_to_mutf8；编码失败的码元序列按整组
/// 失配处理。
/// 不匹配）。
///
/// 边界守卫：除首成员外任何
/// 槽位的前置边界有存活标签锚定 → 整组不匹配（槽位被标签占据
/// 时模式天然失配）。首成员锚定标签时照常改写：标签迁移到替换头（标签
/// 留原位指向新序列头）。
///
/// 发生改写返回 none()，否则 all()。maxStack 不变（构建序 max 4 ≥ 产物
/// max 2，同样不回落 maxStack）。
///
/// @par 标签
/// - `scope:method`
/// - `action:rewrite`
/// - `stability:opt-in`
class CharArrToStrSimplifier : public mjbase::spi::MethodPass
{
public:
    mjbase::spi::PreservedAnalyses run(mjbase::spi::MethodScope&       scope,
                                       mjbase::spi::MethodAnalysisManager& mam) override;
};

}   // namespace mjbase::pass
