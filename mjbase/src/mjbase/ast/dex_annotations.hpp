/// @file dex_annotations.hpp
/// @brief DEX 类声明注解适配器，见 doc:semantic-annotations-dex。
#pragma once

#include "libca/core/result.hpp"

#include "annotations.hpp"

namespace dex::core {
class DexFile;
}

namespace mjbase::ast {

/// 读取类声明上生效的 Runtime 与 Build 注解。System 注解由 DEX 后端保留。
/// 没有声明注解时返回空表，无法投影到 Java 注解模型的值返回 Unsupported。
ca::core::Result<std::vector<DeclarationAnnotation>, AnnotationError>
readDexClassAnnotations(dex::core::DexFile& file, ca::u32 classIndex, KeyPool& keys);

}   // namespace mjbase::ast
