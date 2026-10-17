/// @file        dos_time_util.hpp
/// @brief       DOS 时间/日期字段编码（zip 条目时间戳用）。
#pragma once

#include "libca/core/datatype.hpp"

namespace mjbase::jar {

/// @brief 当前本地时间编码为 DOS 时间字段。
ca::u16 current_dos_time();

/// @brief 当前本地日期编码为 DOS 日期字段。
ca::u16 current_dos_date();

}   // namespace mjbase::jar
