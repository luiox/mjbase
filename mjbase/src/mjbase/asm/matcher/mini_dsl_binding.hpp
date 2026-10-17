/// @file        mini_dsl_binding.hpp
/// @brief       DSL 求值的变量绑定：类型化数值（I/J=i64，F/D=double）。
///
/// 旧实现（mbase mini_dsl_binding）把一切值装进 double——超 2^53 的 long
/// 会失真；此处按值域分离，发射前再按类型标签截断。旧 bindSlot/requireSlot
/// 从未被编译器接线，不迁。
#pragma once

#include <string_view>
#include <unordered_map>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "match_error.hpp"

namespace mjbase::asm_matcher::dsl {

/// 绑定值：整族走 i64，浮族走 double。
struct DslValue
{
    enum class Kind : ca::u8 { Int = 0, Long, Float, Double };
    Kind kind = Kind::Int;
    ca::i64 num = 0;
    double real = 0.0;
};

class DslBinding
{
public:
    void clear() noexcept { values_.clear(); }

    /// @brief 绑定并做线性一致性检查：同名重绑不同值返回 false。
    /// 名字是借用视图（编译产物的策略闭包持有原文）。
    bool bind(std::string_view name, DslValue v);

    /// 取绑定；未绑定 Err(UnboundVariable)。
    ca::core::Result<DslValue, MatcherError> require(std::string_view name) const;

private:
    std::unordered_map<std::string_view, DslValue> values_;
};

}   // namespace mjbase::asm_matcher::dsl
