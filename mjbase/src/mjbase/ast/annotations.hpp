/// @file annotations.hpp
/// @brief 声明注解及其元素值的共享语义模型，见 doc:semantic-annotations。
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "mjbase/keys.hpp"

namespace mjbase::ast {

struct Annotation;

/// 浮点值保留原始位模式。
struct AnnotationFloat { ca::u32 bits; };
struct AnnotationDouble { ca::u64 bits; };
/// 描述符存于 KeyPool，支持基本类型、数组、对象和 void。
struct AnnotationClass { ca::u32 descriptor; };
/// 枚举常量以其字段身份表示。
struct AnnotationEnum { MemberKey constant; };

struct AnnotationValue
{
    using Nested = std::unique_ptr<Annotation>;
    using Array = std::vector<AnnotationValue>;
    std::variant<ca::i8, ca::i16, char16_t, bool, ca::i32, ca::i64,
                 AnnotationFloat, AnnotationDouble, std::u16string, AnnotationClass,
                 AnnotationEnum, Nested, Array> value;
};

/// 描述符及元素名以 MUTF-8 字节驻留在调用者的 KeyPool 中。
struct Annotation
{
    ca::u32 descriptor;
    std::vector<std::pair<ca::u32, AnnotationValue>> elements;
};

/// Binary 表示存于文件但不对运行时反射可见的声明注解。
enum class AnnotationVisibility : ca::u8 { Runtime, Binary };
struct DeclarationAnnotation
{
    AnnotationVisibility visibility;
    Annotation annotation;
};

enum class AnnotationError : ca::u8 { InvalidTarget, Read, Malformed, Unsupported,
                                       InvalidEdit, Write };

}   // namespace mjbase::ast
