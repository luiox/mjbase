#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"

namespace mjbase::project {

/// @file class_kind.hpp
/// @brief class 头形态分类（census kind 列；access + super 派生，不存储）。
///
/// 形态分类是纯 JVM 域知识的投影层切面：`entries.kind` 的
/// 登记语义不变，类级 kind 由 class 头 access 旗标与父类内部名现算。
/// 并列旗标按 Module > Annotation > Interface > Enum > Record > Class 取先
/// （对抗语料可能出现矛盾旗标组合，序恒定保证分类确定）。

/// @brief class 头形态类别。
enum class DbClassKind : ca::u8
{
    Class      = 0, ///< 常规类（各形态旗标均未命中）
    Interface  = 1, ///< ACC_INTERFACE（0x0200）
    Annotation = 2, ///< ACC_INTERFACE | ACC_ANNOTATION（0x0200 | 0x2000）
    Enum       = 3, ///< ACC_ENUM（0x4000）
    Record     = 4, ///< 父类 java/lang/Record 或 ACC_RECORD 伪位（0x10000）
    Module     = 5, ///< ACC_MODULE（0x8000，module-info）
};

/// @brief 类形态判定。
///
/// @param access    class 头 access_flags（JVMS 4.1；工具链伪位 0x10000 可选）
/// @param superName 父类内部名（可为空：java/lang/Object 或缺失）
DbClassKind classifyClassKind(ca::i32 access, std::string_view superName) noexcept;

/// @brief 形态类别稳定名（census kind 列值："class"/"interface"/...
///        小写单 token，与枚举一一对应，永不返回空）。
std::string_view classKindName(DbClassKind kind) noexcept;

}   // namespace mjbase::project
