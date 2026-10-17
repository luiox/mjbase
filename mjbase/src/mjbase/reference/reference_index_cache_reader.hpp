/// @file        reference_index_cache_reader.hpp
/// @brief       `.ridx` cache → ReferenceIndex 还原（严格校验、不产半截状态）。
///
/// std::string 只出现在本边界：指纹输出与读端内部字符串存储。还原期间读端
/// 缓冲在函数作用域内存活，restore 的视图重驻留在其完成前发生。
#pragma once

#include <string>

#include "libca/core/datatype.hpp"

namespace mjbase::reference {

class ReferenceIndex;

/// @brief 从内存中的二进制 cache 还原 ReferenceIndex。
///
/// 读取端完全校验 magic / version / 越界 / 枚举字节区间，遇到任何损坏立即返回
/// false 且不修改 out 的状态。fingerprint 字段从 Header 还原为 64 字符
/// lowercase hex 字符串，由上层用来与当前 container 指纹比对，不匹配时整个
/// cache 视为失效。还原经 ReferenceIndex::restore 完成（六主表视图全部重驻留
/// 进索引自有池，读端缓冲随之可释放）。
///
/// @param out 还原目标。仅在函数返回 true 时被替换。
/// @param outFingerprint 输出关联的 class 内容 SHA-256 hex 字符串。
/// @param data cache 文件完整字节。
/// @param size 字节数。
/// @return 成功还原返回 true；magic/version 不符、越界、枚举越界等返回 false。
bool loadReferenceIndexCache(ReferenceIndex& out,
                             std::string& outFingerprint,
                             const void* data,
                             ca::usize size);

}   // namespace mjbase::reference
