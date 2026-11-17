/// @file effects.hpp
/// @brief 共享方法体的局部堆效应与符号调用边。
#pragma once

#include "method_body.hpp"

namespace mjbase::ast {

enum class LocalEffect : ca::u8 { Pure, ReadOnly, Impure };

struct EffectSummary
{
    LocalEffect local = LocalEffect::Pure;
    std::vector<MemberKey> callees;
};

/// 按块和语句顺序扫描整个方法，保留不可达块中的调用和重复调用。
/// 局部效应不含被调者的效应。调用传播、同步方法声明、隐式异常及类初始化
/// 时机由使用该摘要的分析或变换另行处理。
EffectSummary summarizeEffects(const MethodBody& body);

}   // namespace mjbase::ast
