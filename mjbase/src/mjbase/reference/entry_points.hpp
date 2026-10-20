/// @file        entry_points.hpp
/// @brief       入口点判定谓词：Java `main(String[])` 的单一事实来源。
///
/// 该判定原先内嵌在 GlobalReachability 的内部匿名命名空间里；root provider
/// 需要同一判定来发现 main 根，两处各写一份会出现语义漂移（例如一边校验
/// public static、另一边漏校验）。抽到本头后，可达性内建发现与 provider
/// 共用同一谓词，consumer 亦可直接查询。
#pragma once

#include "mjbase/reference/reference_index.hpp"

namespace mjbase::reference {

/// @brief 是否为 Java 启动入口：`public static void main(String[])`。
///
/// 只校验 JVM 启动器要求的两个访问位与精确描述符；返回类型由描述符尾部
/// `)V` 固定，无需另判。接口的 static main 同样命中——JVM 允许接口声明
/// 可启动的 main，保留它不会错误删除真实入口。
bool isJavaMainMethod(const MethodDeclaration& decl) noexcept;

}   // namespace mjbase::reference
