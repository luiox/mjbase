/// @file        util.hpp
/// @brief       内部名/描述符的纯视图工具（无状态、无分配）。
///
/// 只放跨模块都要用的字符串视图函数；模块私有 helper 放各自目录。
/// 对齐 util/class_name_util 的语义，实现按新库约定重写。
#pragma once

#include <string_view>

namespace mjbase::util {

/// @brief 内部名 → 包名（"a/b/C" → "a/b"；无包返回空视图）。
std::string_view packageNameOf(std::string_view internalName) noexcept;

/// @brief 内部名 → 简单名（"a/b/C" → "C"；数组/描述符形态原样返回）。
std::string_view simpleNameOf(std::string_view internalName) noexcept;

/// @brief 内部名 → 外围类内部名（"a/B$Inner" → "a/B"；顶层类返回空）。
///
/// 以最后一个 '$' 切分；'$' 在合法简单名中可出现，此处与主流工具一致
/// 按 lastIndexOf 处理。
std::string_view enclosingClassOf(std::string_view internalName) noexcept;

/// @brief 类描述符 → 内部名（"La/b/C;" → "a/b/C"；非 L...; 返回空视图）。
std::string_view descriptorToInternalName(std::string_view descriptor) noexcept;

/// @brief 判断内部名是否 JDK/库类（用于调用图/可达性的噪音过滤）。
///
/// 前缀白名单：java/ javax/ jakarta/ sun/ com/sun/ jdk/ org/w3c/ org/xml/。
bool isLibraryClass(std::string_view internalName) noexcept;

}   // namespace mjbase::util
