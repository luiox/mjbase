/// @file dex.hpp
/// @brief DEX 适配器，由 mjbase_dex 提供，见 doc:semantic-ast-dex。
#pragma once

#include "libca/core/result.hpp"

#include "method_body.hpp"

namespace dex::core {
class DexFile;
struct EncodedMethod;
}

namespace mjbase::ast {

/// 从已读取的 DEX 中提升一个声明的方法体。method 来自 file 的 classData。
ca::core::Result<MethodBody, Error>
liftDexMethod(dex::core::DexFile& file, const dex::core::EncodedMethod& method, KeyPool& keys);

}   // namespace mjbase::ast
