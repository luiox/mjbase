/// @file        classfile_support.hpp
/// @brief       workspace 模块内共享的 jbc classfile 低层助手（只读取面为主）。
///
/// class_mutation_service.cpp（写路径）与 workspace.cpp（查询路径）共同使用
/// 的骨架读取/属性定位/属性摘要工具。定位属性一律按名字逐槽比较字节——
/// 常量池 addUtf8 只追加不去重，新写属性名的 CP 索引可能与解析期同名条目
/// 不同，索引等值不可靠。
#pragma once

#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"

#include "mjbase/workspace/workspace_types.hpp"

namespace mjbase::workspace::support {

/// @brief 常量池 Utf8 条目文本；越界/非 Utf8 返回空视图。
inline std::string_view utf8Of(const jbc::core::ClassFile& cf, ca::u32 index) noexcept
{
    return cf.cp.utf8Of(index);
}

/// @brief 常量池 Utf8 条目文本的 owning 拷贝（跨界传递用）。
inline std::string utf8Text(const jbc::core::ClassFile& cf, ca::u32 index)
{
    const std::string_view view = cf.cp.utf8Of(index);
    return std::string(view);
}

/// @brief Class 条目 → 内部名；非 Class/越界返回空。
inline std::string classNameOf(const jbc::core::ClassFile& cf, ca::u32 classCp)
{
    if (!cf.cp.valid(classCp)) return {};
    const jbc::core::ConstantPool::Entry& entry = cf.cp.at(classCp);
    if (entry.tag != jbc::core::CpTag::Class) return {};
    return utf8Text(cf, entry.a);
}

/// @brief 常量池内已有的 Utf8 索引；无则 kNotFound（=0，0 号槽恒为占位）。
inline constexpr ca::u32 kUtf8NotFound = 0;

/// @brief 扫描式 Utf8 intern：命中已有条目返回原索引，miss 才 addUtf8 追加。
///
/// 属性名/调试表名等「要求与解析期条目等值」的场合必须走这里；纯新增数据
/// （常量值、新成员名）可直接 add* 追加，不影响正确性只影响体积。
inline ca::u32 internUtf8(jbc::core::ClassFile& cf, std::string_view text)
{
    const jbc::core::ConstantPool& pool = cf.cp;
    for (ca::u32 i = 1; i < pool.size(); ++i) {
        if (pool.isUtf8(i) && pool.utf8Equals(i, text)) return i;
    }
    return cf.cp.addUtf8(text);
}

/// @brief 在属性槽列表中按名字定位（逐槽比较名字字节）。
inline const jbc::core::AttrSlot* findAttr(const jbc::core::ClassFile& cf,
                                           const std::vector<jbc::core::AttrSlot>& attrs,
                                           std::string_view name)
{
    for (const jbc::core::AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

/// @brief findAttr 的可变版本（写路径）。
inline jbc::core::AttrSlot* findAttr(jbc::core::ClassFile& cf,
                                     std::vector<jbc::core::AttrSlot>& attrs,
                                     std::string_view name)
{
    for (jbc::core::AttrSlot& slot : attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
    }
    return nullptr;
}

/// @brief 成员方法体的 Code 槽定位并按需解码（lazy 状态 Untouched → Decoded，
///        写回仍是 copy-through）；无 Code 属性（abstract/native）返回 nullptr。
inline jbc::core::CodeAttr* ensureCode(jbc::core::ClassFile& cf, jbc::core::MemberInfo& member)
{
    jbc::core::AttrSlot* slot = findAttr(cf, member.attrs, "Code");
    if (slot == nullptr) return nullptr;
    if (!slot->decoded()) {
        if (cf.decodeAttr(*slot).is_err()) return nullptr;
    }
    // 槽本体非 const、由调用方持有；peek() 的 const 面在此收窄回写路径。
    return const_cast<jbc::core::CodeAttr*>(
        ca::core::tag_cast::cast<const jbc::core::CodeAttr>(slot->peek()));
}

/// @brief ensureCode 的 const 前置解码版（查询路径；slot 状态只做 lazy 解码）。
inline const jbc::core::CodeAttr* codeOf(jbc::core::ClassFile& cf, const jbc::core::MemberInfo& member)
{
    return ensureCode(cf, const_cast<jbc::core::MemberInfo&>(member));
}

/// @brief Signature 属性文本；无该属性返回空。
inline std::string signatureText(jbc::core::ClassFile& cf, std::vector<jbc::core::AttrSlot>& attrs)
{
    const jbc::core::AttrSlot* slot = findAttr(cf, attrs, "Signature");
    if (slot == nullptr) return {};
    // Signature 是 typed 属性（SignatureAttr，u2 utf8 载荷）。
    if (!slot->decoded()) {
        if (cf.decodeAttr(const_cast<jbc::core::AttrSlot&>(*slot)).is_err()) return {};
    }
    const auto* attr = ca::core::tag_cast::cast<jbc::core::SignatureAttr>(slot->peek());
    if (attr == nullptr) return {};
    return utf8Text(cf, attr->signatureCp);
}

/// @brief 成员 Exceptions 属性的异常类内部名列表；无该属性返回空。
///
/// typed ExceptionsAttr 体经公开解码入口读出（B3 分派接线前后同形）；槽已有
/// 体时（如 class_mutation_service 新建的 New 态槽，无源 span）按体的实际
/// 形态读。解码失败（半截体）按查询面口径返回空。
inline std::vector<std::string> exceptionNames(jbc::core::ClassFile& cf,
                                               jbc::core::MemberInfo& member)
{
    std::vector<std::string> names;
    const jbc::core::AttrSlot* slot = findAttr(cf, member.attrs, "Exceptions");
    if (slot == nullptr) return names;
    if (slot->decoded()) {
        if (const auto* typed =
                ca::core::tag_cast::cast<jbc::core::ExceptionsAttr>(slot->peek())) {
            names.reserve(typed->exceptionCps.size());
            for (const ca::u32 classCp : typed->exceptionCps) {
                names.push_back(classNameOf(cf, classCp));
            }
        }
        return names;
    }
    auto d = jbc::core::decodeExceptionsBody(cf, *slot);
    if (d.is_err()) return names;
    const auto body = std::move(d).unwrap();
    const auto& exc = static_cast<const jbc::core::ExceptionsAttr&>(*body);
    names.reserve(exc.exceptionCps.size());
    for (const ca::u32 classCp : exc.exceptionCps) {
        names.push_back(classNameOf(cf, classCp));
    }
    return names;
}

/// @brief 注解元素值 → 中性渲染文本（字符串不加引号、数值无后缀）。
/// @details 与 ConstantValueInfo 同一口径；枚举渲染 "type.const"，class 渲染
///          描述符原文，嵌套注解渲染类型描述符。
std::string annotationValueText(const jbc::core::ClassFile& cf,
                                const jbc::core::AnnotationsAttr::ElemValue& value);

/// @brief 递归展开一个注解元素（name 允许为空——数组项无键名）。
void collectAnnotationElement(const jbc::core::ClassFile& cf, std::string_view name,
                              const jbc::core::AnnotationsAttr::ElemValue& value,
                              AnnotationElement& out);

/// @brief 收集类/字段/方法上的 Runtime{Visible,Invisible}Annotations（简单形态）。
///        解码失败的槽跳过（查询面对畸形输入宽容）。
void collectAnnotations(jbc::core::ClassFile& cf, std::vector<jbc::core::AttrSlot>& attrs,
                        std::vector<AnnotationInfo>& out);

/// @brief 收集 Code 属性嵌套的 LVT/LVTT 行（ASM 口径：LVTT 行 desc=签名文本，
///        signature 恒空——与）。
void collectLocalVariables(jbc::core::ClassFile& cf, jbc::core::CodeAttr& code,
                           std::vector<LocalVariableInfo>& out);

/// @brief 字段 ConstantValue 属性摘要；无该属性/载荷不完整返回 nullopt。
std::optional<ConstantValueInfo> fieldConstantValue(jbc::core::ClassFile& cf,
                                                    jbc::core::MemberInfo& field);

/// @brief 常量池条目摘要：tag 名（JVM 规范常量名）+ 文本 + wide 续槽标记。
/// @param resolve 引用类条目（Fieldref/Methodref/InterfaceMethodref）是否
///        解析为 "owner.name:desc"；其余条目文本总是填充。
ConstantPoolEntryInfo constantPoolEntry(const jbc::core::ClassFile& cf, ca::u32 index, bool resolve);

/// @brief 常量池第 @p index 号 Utf8 是否为 @p prefix 开头的属性名。
inline bool attrNameIs(const jbc::core::ClassFile& cf, const jbc::core::AttrSlot& slot,
                       std::string_view prefix) noexcept
{
    return cf.cp.utf8StartsWith(slot.nameCp, prefix);
}

/// @brief 字段描述符 → 元素类内部名（"Ljava/lang/String;" → "java/lang/String"）；
///        数组/基本类型描述符原样返回（无对象类）。
inline std::string objectClassFromFieldDescriptor(std::string_view descriptor)
{
    if (descriptor.size() >= 3 && descriptor.front() == 'L' && descriptor.back() == ';') {
        return std::string(descriptor.substr(1, descriptor.size() - 2));
    }
    return {};
}

/// @brief LDC/数组类描述符 → 元素类内部名："Ljava/lang/String;" → "java/lang/String"，
///        "[[I" → "I" 的宿主类语义不成立时返回空；数组取最深元素的对象类。
///        参照旧栈：数组描述符只对「元素为对象类型」给出内部名（如
///        "[Ljava/lang/String;" → "java/lang/String"），基本类型数组返回空。
std::string objectClassFromArrayDescriptor(std::string_view descriptor);

// @brief Float/Double 位模式 → 中性文本渲染（ostringstream 默认精度）。
std::string renderFloatBits(ca::u32 bits);
std::string renderDoubleBits(ca::u64 bits);

/// @brief Float/Double 位模式 → 数值（搜索数值字面量用）。
inline float bitsToFloat(ca::u32 bits) noexcept
{
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline double bitsToDouble(ca::u64 bits) noexcept
{
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}   // namespace mjbase::workspace::support
