/// @file        value.cpp
/// @brief       具体值与引用身份的实现。
#include "value.hpp"

namespace mjbase::asm_evaluator {

Value Value::top()
{
    return Value{};
}

Value Value::ofI32(ca::i32 v)
{
    Value out;
    out.kind = Kind::I32;
    out.i32  = v;
    return out;
}

Value Value::ofI64(ca::i64 v)
{
    Value out;
    out.kind = Kind::I64;
    out.i64  = v;
    return out;
}

Value Value::ofF32(ca::f32 v)
{
    Value out;
    out.kind = Kind::F32;
    out.f32  = v;
    return out;
}

Value Value::ofF64(ca::f64 v)
{
    Value out;
    out.kind = Kind::F64;
    out.f64  = v;
    return out;
}

Value Value::ofNull()
{
    Value out;
    out.kind    = Kind::Ref;
    out.refNull = true;
    return out;
}

bool Value::sameRef(const Value& other) const noexcept
{
    if (kind != Kind::Ref || other.kind != Kind::Ref) return false;
    if (refNull || other.refNull) return refNull && other.refNull;
    return refId == other.refId;
}

bool Value::operator==(const Value& other) const noexcept
{
    if (kind != other.kind) return false;
    switch (kind) {
    case Kind::Top: return true;
    case Kind::I32: return i32 == other.i32;
    case Kind::I64: return i64 == other.i64;
    case Kind::F32:
        // 浮点按位比较：+0.0/-0.0 与 NaN 的身份语义由测试显式承担。
        return f32 == other.f32 || (f32 != f32 && other.f32 != other.f32);
    case Kind::F64:
        return f64 == other.f64 || (f64 != f64 && other.f64 != other.f64);
    case Kind::Ref: return sameRef(other);
    }
    return false;
}

Value RefFactory::array(ca::i32 length)
{
    Value out;
    out.kind      = Value::Kind::Ref;
    out.refNull   = false;
    out.refIsArray = true;
    out.refLength = length;
    out.refId     = nextId_++;
    return out;
}

Value RefFactory::object()
{
    Value out;
    out.kind    = Value::Kind::Ref;
    out.refNull = false;
    out.refId   = nextId_++;
    return out;
}

}   // namespace mjbase::asm_evaluator
