/// @file        mapping_remapper.hpp
/// @brief       基于 IntermediateMapping 的文本映射门面（类名/成员名/描述符）。
///
/// 旧栈对应物：mbase/asm/remapper/utf8_remapper 的 `Utf8Remapper`——继承
/// jbytecode::commons::Remapper（visitor 底座，mjbase 禁入）并 override map()
/// 查 IntermediateMapping，描述符/签名重写靠基类 non-virtual helper 回调。
/// 重设计为纯值门面：类名/成员名映射直接委托 IntermediateMapping 查询，
/// 描述符/签名重写为本模块自有纯函数（与 MemberRemapPlan 同一套扫描语义），
/// 重写结果驻留门面自有 KeyPool（内容寻址、视图随门面存活）。
///
/// 零拷贝：类名/成员名命中返回 IntermediateMapping 内部 arena 的视图，未命中
/// map() 返回空视图（is_empty 语义与旧栈一致）；描述符/签名重写结果驻留本
/// 门面池。取代旧栈的继承形态后，命令式改写器不再需要任何 visitor 挂点。
#pragma once

#include <string_view>
#include <unordered_map>

#include "libca/core/datatype.hpp"

#include "mjbase/keys.hpp"
#include "mjbase/mapping/intermediate_mapping.hpp"

namespace mjbase::asm_remapper {

class MappingRemapper
{
public:
    /// @brief 绑定一份映射；映射存活期须覆盖本门面使用期（内部仅持引用）。
    explicit MappingRemapper(const mjbase::mapping::IntermediateMapping& mappings) noexcept
        : mappings_(mappings)
    {}

    /// @brief 映射类内部名（含 package 前缀回退）；未命中返回空视图。
    std::string_view map(std::string_view internalName) const
    {
        const auto mapped = mappings_.getMappedClassName(refOf(internalName));
        return viewOf(mapped);
    }

    /// @brief 映射方法名（owner:name:desc 精确匹配，desc 必填）；未命中返回空视图。
    std::string_view mapMethod(std::string_view owner, std::string_view name,
                               std::string_view desc) const
    {
        const auto mapped = mappings_.getMappedMethodName(refOf(owner), refOf(name),
                                                          refOf(desc));
        return viewOf(mapped);
    }

    /// @brief 映射字段名（owner:name:desc 精确匹配；desc 空规则作通配）。
    std::string_view mapField(std::string_view owner, std::string_view name,
                              std::string_view desc) const
    {
        const auto mapped =
            mappings_.getMappedFieldName(refOf(owner), refOf(name), refOf(desc));
        return viewOf(mapped);
    }

    /// @brief 重写字段描述符内嵌对象类型；无命中原样返回输入。
    std::string_view mapFieldDescriptor(std::string_view desc) const;

    /// @brief 重写方法描述符内嵌对象类型；无命中原样返回输入。
    std::string_view mapMethodDescriptor(std::string_view desc) const;

    /// @brief 重写泛型签名文本（逐 `L...;` 类名，遇 `<` 嵌套不停）；无命中原样返回。
    std::string_view mapSignature(std::string_view signature) const;

    /// @brief 是否存在任一映射（快速短路）。
    bool hasMappings() const noexcept { return !mappings_.isEmpty(); }

    const mjbase::mapping::IntermediateMapping& mappings() const noexcept { return mappings_; }

private:
    /// @brief 借用视图 → Utf8StringRef（码点数取字节上界，manifest 同款保守口径）。
    static ca::str::Utf8StringRef refOf(std::string_view view) noexcept
    {
        return ca::str::Utf8StringRef(
            reinterpret_cast<const ca::u8*>(view.data()), view.size(), view.size());
    }

    static std::string_view viewOf(const ca::str::Utf8StringRef& ref) noexcept
    {
        return std::string_view(reinterpret_cast<const char*>(ref.data()),
                                ref.byte_length());
    }

    /// @brief 描述符/签名重写工作体：无命中返回空视图（调用方回落输入）。
    std::string_view remapTextCached(std::string_view input, bool method, bool signature) const;

    const mjbase::mapping::IntermediateMapping& mappings_;
    /// 重写结果驻留池（描述符/签名是合成文本，无映射侧驻留可借）。
    mutable KeyPool pool_;
    /// 输入文本 → 结果驻留 id 的记忆化（输入经池驻留为键）。
    mutable std::unordered_map<ca::u32, ca::u32> cache_;
};

}   // namespace mjbase::asm_remapper
