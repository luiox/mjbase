/// @file constant_facts.hpp
/// @brief 共享方法体 AST 的块内常量事实。
#pragma once

#include "method_body.hpp"

namespace mjbase::ast {

/// Bits32 保留 DEX 未解释的位模式，直到已知类型的用途消费它。
using KnownConstant = std::variant<ca::i32, Null, Bits32>;

/// 从未知的局部变量状态开始，按语句顺序求值。
/// 每条语句对应一项，只有 Let 可产生已知事实，异常操作只给出正常返回时的值。
std::vector<std::optional<KnownConstant>> analyzeBlockConstants(const Block& block);

}   // namespace mjbase::ast
