#include "mjbase/pass/long_annotation_remover.hpp"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::AnnotationsAttr;
using jbc::core::AttrKind;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::MemberInfo;
using jbc::core::ParameterAnnotationsAttr;
using jbc::core::TypeAnnotationsAttr;

// ── 属性名常量（JVMS 4.7；meta 家族同款本地常量纪律，槽按名比较）。──
constexpr std::string_view kCode = "Code";
constexpr std::string_view kRva  = "RuntimeVisibleAnnotations";
constexpr std::string_view kRia  = "RuntimeInvisibleAnnotations";
constexpr std::string_view kRvta = "RuntimeVisibleTypeAnnotations";
constexpr std::string_view kRita = "RuntimeInvisibleTypeAnnotations";
constexpr std::string_view kRvpa = "RuntimeVisibleParameterAnnotations";
constexpr std::string_view kRipa = "RuntimeInvisibleParameterAnnotations";

/// 槽名是否等于给定名（逐字节，零分配；meta 家族同款）。
bool slotNameIs(const ClassFile& cf, const AttrSlot& slot, std::string_view name)
{
    return cf.cp.utf8Equals(slot.nameCp, name);
}

/// 按名查找属性槽；未命中返回 nullptr。
AttrSlot* findSlot(ClassFile& cf, std::vector<AttrSlot>& attrs, std::string_view name)
{
    for (AttrSlot& slot : attrs) {
        if (slotNameIs(cf, slot, name)) return &slot;
    }
    return nullptr;
}

/// Utf8 字节视图的码点数（延续字节不计；家族既有口径——Recaf 按 Java
/// String.length() 即 UTF-16 码元数，ASCII 形态两口径一致，见头注差异面）。
ca::usize codepointLength(std::string_view s)
{
    ca::usize count = 0;
    for (const char c : s) {
        if ((static_cast<ca::u8>(c) & 0xC0) != 0x80) ++count;
    }
    return count;
}

/// 描述符长度判定：码点数 ≥ 阈值即「巨型」（Recaf 口径：保留 < 阈值，
/// 删除 ≥ 阈值）。负阈值按无符号大数裁决——什么都删不掉（fail-closed 向）。
bool isLongDesc(const ClassFile& cf, const ca::u32 typeCp, const ca::i32 maxLen)
{
    return codepointLength(cf.utf8(typeCp)) >= static_cast<ca::usize>(maxLen);
}

/// 注解条目自身（顶层）是否巨型。
bool annoEntryIsLong(const ClassFile& cf, const AnnotationsAttr::Annotation& a,
                     const ca::i32 maxLen)
{
    return isLongDesc(cf, a.typeCp, maxLen);
}

//-- 嵌套注解递归判定与修剪（Recaf LongSubAnnoRemover 面）-------------------
//
// Recaf 的子注解访问器深入注解值与数组值两层递归：嵌套注解描述符超阈值时
// 该键值对（或数组元素）整项删除，父级注解保留。mjbase typed 模型里注解值
// 是 Annotation::values（键值对表）、数组是 ElemValue::items，两条递归规则：
// - Annotation.values 中 tag='@' 且嵌套描述符巨型的键值对整对删除；
// - '[' 数组 items 中 tag='@' 且嵌套描述符巨型的元素整项删除；
// 幸存嵌套注解自身再递归深入（子注解的子注解同判）。

bool pruneNestedLongValues(ClassFile&                                      cf,
                           std::vector<std::pair<ca::u32, AnnotationsAttr::ElemValue>>&
                                                                        values,
                           const ca::i32                                maxLen);
bool pruneNestedLongItems(ClassFile&                             cf,
                          std::vector<AnnotationsAttr::ElemValue>& items,
                          const ca::i32                          maxLen);

bool valuesContainLongNested(const ClassFile& cf,
                             const std::vector<std::pair<ca::u32, AnnotationsAttr::ElemValue>>&
                                 values,
                             const ca::i32 maxLen);
bool elemValueContainsLongNested(const ClassFile&            cf,
                                 const AnnotationsAttr::ElemValue& ev,
                                 const ca::i32              maxLen)
{
    if (ev.tag == '@') {
        if (ev.nested != nullptr && isLongDesc(cf, ev.nested->typeCp, maxLen)) return true;
        if (ev.nested != nullptr
            && valuesContainLongNested(cf, ev.nested->values, maxLen)) {
            return true;
        }
        return false;
    }
    if (ev.tag == '[') {
        for (const AnnotationsAttr::ElemValue& item : ev.items) {
            if (elemValueContainsLongNested(cf, item, maxLen)) return true;
        }
    }
    return false;
}

bool valuesContainLongNested(const ClassFile& cf,
                             const std::vector<std::pair<ca::u32, AnnotationsAttr::ElemValue>>&
                                 values,
                             const ca::i32 maxLen)
{
    for (const auto& pair : values) {
        if (elemValueContainsLongNested(cf, pair.second, maxLen)) return true;
    }
    return false;
}

/// 注解条目（任意层）是否命中巨型判据：顶层巨型或值面嵌套巨型。
bool annoEntryHit(const ClassFile& cf, const AnnotationsAttr::Annotation& a,
                  const ca::i32 maxLen)
{
    return annoEntryIsLong(cf, a, maxLen)
           || valuesContainLongNested(cf, a.values, maxLen);
}

/// 键值对是否巨型嵌套（tag='@' 且嵌套描述符超阈值）。
bool isLongNestedPair(const ClassFile&                                 cf,
                      const std::pair<ca::u32, AnnotationsAttr::ElemValue>& pair,
                      const ca::i32                                    maxLen)
{
    return pair.second.tag == '@' && pair.second.nested != nullptr
           && isLongDesc(cf, pair.second.nested->typeCp, maxLen);
}

/// 数组元素是否巨型嵌套（同判）。
bool isLongNestedItem(const ClassFile&                     cf,
                      const AnnotationsAttr::ElemValue&    item,
                      const ca::i32                        maxLen)
{
    return item.tag == '@' && item.nested != nullptr
           && isLongDesc(cf, item.nested->typeCp, maxLen);
}

/// 修剪键值对表：巨型嵌套键值对整对删除，幸存 '@' 项深入递归。返回是否有删除。
bool pruneNestedLongValues(ClassFile&                                      cf,
                           std::vector<std::pair<ca::u32, AnnotationsAttr::ElemValue>>&
                                                                        values,
                           const ca::i32                                maxLen)
{
    bool removed = false;
    for (auto& pair : values) {
        if (pair.second.tag == '@' && pair.second.nested != nullptr) {
            removed |= pruneNestedLongValues(cf, pair.second.nested->values, maxLen);
        }
        else if (pair.second.tag == '[') {
            removed |= pruneNestedLongItems(cf, pair.second.items, maxLen);
        }
    }
    const ca::usize before = values.size();
    values.erase(std::remove_if(values.begin(), values.end(),
                                [&cf, maxLen](const std::pair<ca::u32,
                                                             AnnotationsAttr::ElemValue>& p) {
                                    return isLongNestedPair(cf, p, maxLen);
                                }),
                 values.end());
    return removed || values.size() != before;
}

/// 修剪数组元素：巨型嵌套元素整项删除，幸存项递归深入。返回是否有删除。
bool pruneNestedLongItems(ClassFile&                       cf,
                          std::vector<AnnotationsAttr::ElemValue>& items,
                          const ca::i32                    maxLen)
{
    bool removed = false;
    for (AnnotationsAttr::ElemValue& item : items) {
        if (item.tag == '@' && item.nested != nullptr) {
            removed |= pruneNestedLongValues(cf, item.nested->values, maxLen);
        }
        else if (item.tag == '[') {
            removed |= pruneNestedLongItems(cf, item.items, maxLen);
        }
    }
    const ca::usize before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&cf, maxLen](const AnnotationsAttr::ElemValue& item) {
                                   return isLongNestedItem(cf, item, maxLen);
                               }),
                items.end());
    return removed || items.size() != before;
}

//-- 槽内过滤 ----------------------------------------------------------------

/// Runtime{Visible,Invisible}Annotations 两槽槽内过滤：顶层巨型条目整条删除
/// （槽置脏），幸存条目值面嵌套巨型项修剪；删空连槽摘除。解码失败/类型不符
/// 保守跳过。返回是否发生任何变化。
bool filterLongAnnotationSlots(ClassFile&             cf,
                               std::vector<AttrSlot>& attrs,
                               const ca::i32          maxLen)
{
    bool changed = false;
    for (std::size_t i = attrs.size(); i-- > 0;) {
        AttrSlot& slot = attrs[i];
        const bool target = slotNameIs(cf, slot, kRva) || slotNameIs(cf, slot, kRia);
        if (!target) continue;
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()) continue;
        if (std::move(decoded).unwrap()->kind() != AttrKind::Annotations) continue;
        const auto* body = ca::core::tag_cast::cast<const AnnotationsAttr*>(slot.peek());
        const bool anyHit =
            std::any_of(body->annos.begin(), body->annos.end(),
                        [&cf, maxLen](const AnnotationsAttr::Annotation& a) {
                            return annoEntryHit(cf, a, maxLen);
                        });
        if (!anyHit) continue;
        auto& annos = ca::core::tag_cast::cast<AnnotationsAttr*>(slot.mutate())->annos;
        annos.erase(std::remove_if(annos.begin(), annos.end(),
                                   [&cf, maxLen](const AnnotationsAttr::Annotation& a) {
                                       return annoEntryIsLong(cf, a, maxLen);
                                   }),
                    annos.end());
        for (AnnotationsAttr::Annotation& a : annos) {
            (void)pruneNestedLongValues(cf, a.values, maxLen);
        }
        if (annos.empty()) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
        }
        changed = true;
    }
    return changed;
}

/// Runtime{Visible,Invisible}TypeAnnotations 两槽槽内过滤：条目注解类型描述符
/// 巨型整条删除，幸存条目值面嵌套巨型项修剪；删空连槽摘除。成员级与 Code
/// 嵌套属性表共用本道（条目模型同构）。返回是否发生任何变化。
bool filterLongTypeAnnotationSlots(ClassFile&             cf,
                                   std::vector<AttrSlot>& attrs,
                                   const ca::i32          maxLen)
{
    bool changed = false;
    for (std::size_t i = attrs.size(); i-- > 0;) {
        AttrSlot& slot = attrs[i];
        const bool target = slotNameIs(cf, slot, kRvta) || slotNameIs(cf, slot, kRita);
        if (!target) continue;
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()) continue;
        if (std::move(decoded).unwrap()->kind() != AttrKind::TypeAnnotations) continue;
        const auto* body = ca::core::tag_cast::cast<const TypeAnnotationsAttr*>(slot.peek());
        const auto hit = [&cf, maxLen](const TypeAnnotationsAttr::Annotation& a) {
            return isLongDesc(cf, a.typeIndexCp, maxLen)
                   || valuesContainLongNested(cf, a.values, maxLen);
        };
        if (!std::any_of(body->annotations.begin(), body->annotations.end(), hit)) continue;
        auto& annos =
            ca::core::tag_cast::cast<TypeAnnotationsAttr*>(slot.mutate())->annotations;
        const auto topLong = [&cf, maxLen](const TypeAnnotationsAttr::Annotation& a) {
            return isLongDesc(cf, a.typeIndexCp, maxLen);
        };
        annos.erase(std::remove_if(annos.begin(), annos.end(), topLong), annos.end());
        for (TypeAnnotationsAttr::Annotation& a : annos) {
            (void)pruneNestedLongValues(cf, a.values, maxLen);
        }
        if (annos.empty()) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
        }
        changed = true;
    }
    return changed;
}

/// 方法 Code 嵌套 Runtime{Visible,Invisible}TypeAnnotations 槽内过滤：code
/// 目标形态（cast/instanceof/new/localvar）旧栈在方法向量上同面参与。改动
/// 落盘需父 Code 槽连带置脏（meta 家族同款显式有界破例）。返回是否发生变化。
bool filterCodeNestedLongTypeAnnotations(ClassFile& cf, MemberInfo& m, const ca::i32 maxLen)
{
    AttrSlot* codeSlot = findSlot(cf, m.attrs, kCode);
    if (codeSlot == nullptr) return false;
    auto decoded = cf.decodeCode(m);
    if (decoded.is_err()) return false;
    CodeAttr* code = decoded.unwrap();
    const bool changed = filterLongTypeAnnotationSlots(cf, code->attrs, maxLen);
    if (changed) {
        (void)codeSlot->mutate();
    }
    return changed;
}

/// Runtime{Visible,Invisible}ParameterAnnotations 两槽槽内过滤：typed 行模型
/// 为主（libjbc 0.4.16+ 建模），行数与行序保持（部分摘除留槽——与
/// SpecificAnnotationRemover 参数注解道同款行保持语义）、全空摘槽；typed
/// 解码拒绝的 legacy Raw 形状保守保持原样（fail-closed）。返回是否发生变化。
bool filterLongParameterAnnotationSlots(ClassFile&             cf,
                                        std::vector<AttrSlot>& attrs,
                                        const ca::i32          maxLen)
{
    bool changed = false;
    for (std::size_t i = attrs.size(); i-- > 0;) {
        AttrSlot& slot = attrs[i];
        const bool target = slotNameIs(cf, slot, kRvpa) || slotNameIs(cf, slot, kRipa);
        if (!target) continue;
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()) continue;
        const AttrKind slotKind = std::move(decoded).unwrap()->kind();
        if (slotKind != AttrKind::ParameterAnnotations) continue;   // Raw legacy 保守跳过
        const auto* ro = ca::core::tag_cast::cast<const ParameterAnnotationsAttr*>(slot.peek());
        const auto hit = [&cf, maxLen](const AnnotationsAttr::Annotation& a) {
            return annoEntryHit(cf, a, maxLen);
        };
        bool anyHit = false;
        for (const std::vector<AnnotationsAttr::Annotation>& row : ro->parameters) {
            if (std::any_of(row.begin(), row.end(), hit)) {
                anyHit = true;
                break;
            }
        }
        if (!anyHit) continue;
        auto& rows =
            ca::core::tag_cast::cast<ParameterAnnotationsAttr*>(slot.mutate())->parameters;
        for (std::vector<AnnotationsAttr::Annotation>& row : rows) {
            row.erase(std::remove_if(row.begin(), row.end(), hit), row.end());
            for (AnnotationsAttr::Annotation& a : row) {
                (void)pruneNestedLongValues(cf, a.values, maxLen);
            }
        }
        const bool allEmpty = std::all_of(rows.begin(), rows.end(),
                                          [](const std::vector<AnnotationsAttr::Annotation>&
                                              row) { return row.empty(); });
        if (allEmpty) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
        }
        changed = true;
    }
    return changed;
}

}   // namespace

// ── LongAnnotationRemover ──

LongAnnotationRemover::LongAnnotationRemover(LongAnnotationRemoverOptions options)
    : options_(options)
{}

mjbase::spi::PreservedAnalyses LongAnnotationRemover::run(jbc::ir::ClassSession&            session,
                                                          mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile&      cf     = session.backing();
    const ca::i32   maxLen = options_.maxDescLength;
    bool            changed = false;

    changed |= filterLongAnnotationSlots(cf, cf.attrs, maxLen);
    changed |= filterLongTypeAnnotationSlots(cf, cf.attrs, maxLen);
    for (MemberInfo& f : cf.fields) {
        changed |= filterLongAnnotationSlots(cf, f.attrs, maxLen);
        changed |= filterLongTypeAnnotationSlots(cf, f.attrs, maxLen);
    }
    for (MemberInfo& m : cf.methods) {
        changed |= filterLongAnnotationSlots(cf, m.attrs, maxLen);
        changed |= filterLongTypeAnnotationSlots(cf, m.attrs, maxLen);
        changed |= filterLongParameterAnnotationSlots(cf, m.attrs, maxLen);
        changed |= filterCodeNestedLongTypeAnnotations(cf, m, maxLen);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
