/// @file        mini_dsl_binding.cpp
/// @brief       DSL 变量绑定实现。
#include "mini_dsl_binding.hpp"

namespace mjbase::asm_matcher::dsl {

bool DslBinding::bind(std::string_view name, DslValue v)
{
    auto [it, ok] = values_.try_emplace(name, v);
    if (ok) return true;
    const DslValue& old = it->second;
    const bool same = old.kind == v.kind &&
                      (old.kind == DslValue::Kind::Int || old.kind == DslValue::Kind::Long
                           ? old.num == v.num
                           : old.real == v.real);
    return same;
}

ca::core::Result<DslValue, MatcherError> DslBinding::require(std::string_view name) const
{
    const auto it = values_.find(name);
    if (it == values_.end()) {
        return ca::core::Err(MatcherError{MatchError::UnboundVariable, 0});
    }
    return ca::core::Ok(it->second);
}

}   // namespace mjbase::asm_matcher::dsl
