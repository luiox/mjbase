/// @file jvm_annotations.hpp
/// @brief JVM 声明注解适配器，见 doc:semantic-annotations-jvm。
#pragma once

#include <functional>
#include <string_view>

#include "jbc/ir/units.hpp"
#include "libca/core/result.hpp"

#include "annotations.hpp"

namespace mjbase::ast {

/// 成员名和描述符用于精确选择重载。Class 目标使用空名与空描述符。
struct JvmAnnotationTarget
{
    enum class Kind : ca::u8 { Class, Field, Method };
    Kind kind = Kind::Class;
    std::string_view name;
    std::string_view descriptor;
};

/// 查询一个声明上的注解。未挂载属性时返回空表。
ca::core::Result<std::vector<Annotation>, AnnotationError>
readJvmAnnotations(jbc::ir::ClassSession& session, JvmAnnotationTarget target,
                   AnnotationVisibility visibility, KeyPool& keys);

/// 返回 true 提交该分组的变更，返回 false 保留原始字节。
using JvmAnnotationEditor = std::function<bool(std::vector<Annotation>&, KeyPool&)>;

/// 在独立的 classfile 会话中替换所选声明的一组注解，并返回完整 classfile。
/// 回调可以修改、增加或删除注解。失败时调用者仍持有原始输入。
ca::core::Result<std::vector<ca::u8>, AnnotationError>
rewriteJvmAnnotations(jbc::core::ByteView bytes, JvmAnnotationTarget target,
                      AnnotationVisibility visibility, const JvmAnnotationEditor& edit);

}   // namespace mjbase::ast
