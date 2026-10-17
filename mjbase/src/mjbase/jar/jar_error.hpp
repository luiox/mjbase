/// @file        jar_error.hpp
/// @brief       jar 模块统一错误类型：读/写失败的结构化错误码 + 出错路径。
///
/// mbase 的 jar 层用 throw std::runtime_error 报告文件级失败；mjbase 按仓库
/// 错误处理约定改为 Result 值语义通道，本类型即错误载荷。path 保留原始字节
/// （from_data_unchecked 直通），与条目名/路径的零解码口径一致。
#pragma once

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::jar {

using ca::str::Utf8String;
using ca::str::Utf8StringRef;

/// @brief jar 读/写错误：种类 + 出错文件路径（诊断用）。
struct JarError
{
    enum class Kind : ca::u8 {
        FileNotFound,   ///< 输入文件不存在
        OpenFailed,     ///< 文件打开失败（读/写共用）
        ReadFailed,     ///< 读取失败（mbase 未区分此失败，直迁后更细一档）
        WriteFailed,    ///< 写出失败：磁盘满/断连等 IO 错误
    };

    Kind kind = Kind::OpenFailed;
    Utf8String path;   ///< 出错文件路径（原始字节直通，不校验 UTF-8）
};

}   // namespace mjbase::jar
