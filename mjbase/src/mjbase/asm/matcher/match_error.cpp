/// @file        match_error.cpp
/// @brief       matcher 错误类别名。
#include "match_error.hpp"

namespace mjbase::asm_matcher {

const char* to_string(MatchError e) noexcept
{
    switch (e) {
    case MatchError::Ok: return "ok";
    case MatchError::DslSyntax: return "dsl syntax";
    case MatchError::DslSemantic: return "dsl semantic";
    case MatchError::UnknownOpcode: return "unknown opcode";
    case MatchError::UnboundVariable: return "unbound variable";
    case MatchError::UnsupportedExpr: return "unsupported expr";
    case MatchError::EmitUnsupported: return "emit unsupported";
    case MatchError::EditRejected: return "edit rejected";
    }
    return "?";
}

}   // namespace mjbase::asm_matcher
