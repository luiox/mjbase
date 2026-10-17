/// @file        reference_index_cache_writer.hpp
/// @brief       ReferenceIndex → `.ridx` cache 落盘（持久化工件契约面）。
///
/// std::string 只出现在本边界：路径与指纹入参、字节写出口。事实结构体的
/// string_view 字段按字节写出，不落新存储。
#pragma once

#include <string>

#include "libca/core/datatype.hpp"

namespace mjbase::reference {

class ReferenceIndex;

/// @brief 将 ReferenceIndex 序列化到 `.ridx` cache 文件。
///
/// 布局见 reference_index_cache_format.hpp（字节级契约：段序/字段序/编码不动）。
/// 写入采用临时文件 + rename 的原子替换（Windows 上 rename 不覆盖既有目标，
/// 先 remove 再换入），避免并发写入者互相覆盖出半截文件。
///
/// @param outputPath 目标文件路径。
/// @param index 已构建好的引用索引。
/// @param fingerprint 关联的 class 内容 SHA-256（hex 字符串，64 字符），
///                    写入 Header 供读取端比对失效。
/// @return 成功写入返回 true；I/O 错误或 fingerprint 长度非法返回 false。
bool writeReferenceIndexCache(const char* outputPath,
                              const ReferenceIndex& index,
                              const std::string& fingerprint);

}   // namespace mjbase::reference
