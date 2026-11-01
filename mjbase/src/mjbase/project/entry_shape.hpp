#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"

namespace mjbase::project {

/// @file entry_shape.hpp
/// @brief jar 条目路径形态判定（硬壳容器形态切面，#39 / #121 B4）。
///
/// 纯字节级扫描：路径按 jar 条目路径的结构约定（'/' 分隔段、字节透传、
/// 无编码假设）逐 facet 检查，无正则。每条路径命中恰一个 facet（固定
/// 优先序，见 classifyEntryPathShape）；`regular` 旗标的登记口径 =
/// `isRegularEntryPath`（即 shape == Regular）。
///
/// B1 的 regular 最小口径原是 mjt 导入桥的内部函数（#121 B4 钩子），本
/// 头把它上提为公共面：布尔判定与 B1 逐字节一致（登记面零漂移），facet
/// 枚举是 census 汇总面的细化切面（伪装目录、非常规命名等硬壳形态）。

/// @brief 条目路径形态 facet。
enum class EntryPathShape : ca::u8
{
    Regular       = 0, ///< 常规路径形态（下列异常 facet 均未命中）
    Empty         = 1, ///< 空路径
    ControlChar   = 2, ///< 含控制字符或 DEL（字节 < 0x20 或 == 0x7F）
    Backslash     = 3, ///< 含反斜杠（Windows 路径分隔符混入）
    LeadingSlash  = 4, ///< 前导 '/'
    EmptySegment  = 5, ///< 含空段（连续斜杠）
    DotSegment    = 6, ///< 含 "." 或 ".." 段
    TrailingSlash = 7, ///< 尾随 '/'（目录占位条目形态）
};

/// @brief 逐 facet 形态判定，facet 取固定优先序：
///        Empty > ControlChar > Backslash > LeadingSlash > EmptySegment >
///        DotSegment > TrailingSlash > Regular（一条路径命中多个 facet 时
///        取优先序最前者；序恒定保证分布与布尔判定跨版本稳定）。
EntryPathShape classifyEntryPathShape(std::string_view path) noexcept;

/// @brief regular 旗标判定（== classifyEntryPathShape(path) == Regular）。
///
/// 口径：非空、无控制字符/DEL、无反斜杠、非前导或尾随 '/'、无空段
/// （连续斜杠）、无 "." / ".." 段。与 B1 导入桥的最小口径逐字节一致。
bool isRegularEntryPath(std::string_view path) noexcept;

/// @brief 形态 facet 稳定名（census 汇总面列值："regular"/"empty"/...
///        蛇形小写，与 facet 枚举一一对应，永不返回空）。
std::string_view entryPathShapeName(EntryPathShape shape) noexcept;

}   // namespace mjbase::project
