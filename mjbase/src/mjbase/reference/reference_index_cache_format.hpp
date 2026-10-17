/// @file        reference_index_cache_format.hpp
/// @brief       ReferenceIndex 持久化 cache（.ridx）的二进制布局。
///
/// 文件由固定 Header 与 6 个 length-prefixed 主表段构成。所有多字节整数使用
/// little-endian 编码，字符串以 u64 长度前缀 + 裸 UTF-8 字节写入。段序与字段序
/// 是**持久化工件契约**：magic/version/6 段布局/字段顺序一律不动（字节级对拍
/// 基线），只允许追加新段或升版本。
///
/// 设计与 libjbc 之外的既有 hierarchy 缓存风格对齐：固定 magic + version 头、
/// 读取端做严格的越界校验、版本不匹配直接拒绝加载。
#pragma once

#include <cstdint>

#include "libca/core/datatype.hpp"

namespace mjbase::reference::ridx {

/// @brief 文件标识，ASCII "RIDX" 的小端序 u32。
inline constexpr ca::u32 kMagic = 0x58444952u;

/// @brief 主.次 编码，首版 1.0。
inline constexpr ca::u32 kVersion = 0x00010000u;

/// @brief 三个枚举值在文件中用单字节存储。
///
/// 写入端用 `static_cast<ca::u8>`，读取端先读 u8 再校验是否落在合法区间，
/// 越界的枚举字节视为损坏文件并拒绝加载。枚举字节值即 MethodReferenceKind/
/// FieldReferenceKind/TypeReferenceKind 的声明序（迁移时逐值保留，见
/// reference_index.hpp 枚举注释）。
inline constexpr ca::u8 kMethodKindMax = 4u;   // ReflectionExecution
inline constexpr ca::u8 kFieldKindMax  = 3u;   // ReflectionAccess
inline constexpr ca::u8 kTypeKindMax   = 14u;  // MethodHandleOwner

#pragma pack(push, 1)

/// @brief cache 文件头。
///
/// fingerprint 字段保存与此索引关联的 class 内容 SHA-256（XrefCache::fingerprint
/// 的产物，32 字节）。读取端不解释它，由上层用来比对当前 container 的指纹，
/// 不匹配则丢弃整个 cache 文件。
struct Header
{
    ca::u32 magic;                   ///< kMagic
    ca::u32 version;                 ///< kVersion
    ca::u8  fingerprintSha256[32];   ///< 关联 class 内容指纹
    ca::u64 classDeclsCount;         ///< ClassDeclaration 段条目数
    ca::u64 methodDeclsCount;        ///< MethodDeclaration 段条目数
    ca::u64 fieldDeclsCount;         ///< FieldDeclaration 段条目数
    ca::u64 methodRefsCount;         ///< MethodReference 段条目数
    ca::u64 fieldRefsCount;          ///< FieldReference 段条目数
    ca::u64 typeRefsCount;           ///< TypeReference 段条目数
};

#pragma pack(pop)

static_assert(sizeof(Header) == 88, "Header must be 88 bytes");

}   // namespace mjbase::reference::ridx
