#pragma once
#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"
#include "intermediate_mapping.hpp"

namespace mjbase::mapping {

/// @brief 映射解析/写出错误类别。
///
/// 原 旧栈 通过 `mjbase::mapping::format::InvalidMappingException`
/// （std::exception 子类，仅携带一条 message 文本）表达「映射文本无效」；
/// 本模块按仓库 Result 值语义纪律改走错误通道，异常类不再迁移。分类：
/// - InvalidText：映射文本无效被拒载（原异常的唯一语义，原异常 message 文本
///   语义并入此值；现实现各格式为宽松跳坏行口径，实际不产生该值，为严格
///   入口与下游格式预留）。
/// - UnknownFormat：注册表中无此格式名（原 registry 静默返回空产物的路径，
///   改走错误通道）。
/// - Io：写出失败（sidecar 文件写路径的 Io 类别归并到此处）。
enum class MappingError : ca::u8
{
    InvalidText,
    UnknownFormat,
    Io,
};

/// @brief 映射文件格式的抽象接口（parse/format 一对入口）。
///
/// 实现方约定：行式格式遵循「静默跳过坏行」的宽松口径（不产生 Err）；
/// 需要错误定位的格式把自身错误类别归并到 MappingError 后经 Err 上报。
class MappingFileFormat
{
public:
    virtual ~MappingFileFormat() = default;

    virtual ca::str::Utf8StringRef getFormatName() const = 0;

    virtual ca::Result<IntermediateMapping, MappingError> parse(
        ca::str::Utf8StringRef mappingText) = 0;

    virtual ca::Result<ca::str::Utf8String, MappingError> format(
        const IntermediateMapping& mappings) = 0;
};

}   // namespace mjbase::mapping
