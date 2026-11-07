#include "mjbase/pass/duplicate_annotation_remover.hpp"

#include <algorithm>
#include <cstddef>
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
using jbc::core::RecordAttr;
using jbc::core::TypeAnnotationsAttr;

// ── 属性名常量（JVMS 4.7；meta 家族同款本地常量纪律，槽按名比较）。──
constexpr std::string_view kCode    = "Code";
constexpr std::string_view kRecord  = "Record";
constexpr std::string_view kRva     = "RuntimeVisibleAnnotations";
constexpr std::string_view kRia     = "RuntimeInvisibleAnnotations";
constexpr std::string_view kRvta    = "RuntimeVisibleTypeAnnotations";
constexpr std::string_view kRita    = "RuntimeInvisibleTypeAnnotations";
constexpr std::string_view kRvpa    = "RuntimeVisibleParameterAnnotations";
constexpr std::string_view kRipa    = "RuntimeInvisibleParameterAnnotations";

bool slotNameIs(const ClassFile& cf, const AttrSlot& slot, std::string_view name)
{
    return cf.cp.utf8Equals(slot.nameCp, name);
}

AttrSlot* findSlot(ClassFile& cf, std::vector<AttrSlot>& attrs, std::string_view name)
{
    for (AttrSlot& slot : attrs) {
        if (slotNameIs(cf, slot, name)) return &slot;
    }
    return nullptr;
}

/// 描述符内容是否已在 seen 集（同内容不同 CP 条目同判——键是描述符内容，
/// Recaf Set<String> 语义）。
bool seenContains(const std::vector<std::string_view>& seen, const std::string_view desc)
{
    return std::find(seen.begin(), seen.end(), desc) != seen.end();
}

/// type annotation 目标位全集相等判定：键 = target_type + target_info 各字段
/// + type_path + 描述符内容。与解码器逐字段对齐——解码器对非活跃字段回零
/// （struct 默认值），全字段等值判定在活跃字段之外恒真，不引入假阳性。
bool typeAnnoTargetEquals(const ClassFile&                        cf,
                          const TypeAnnotationsAttr::Annotation&   a,
                          const TypeAnnotationsAttr::Annotation&   b)
{
    if (a.targetType != b.targetType) return false;
    if (a.typeParameterIndex != b.typeParameterIndex) return false;
    if (a.boundIndex != b.boundIndex) return false;
    if (a.supertypeIndex != b.supertypeIndex) return false;
    if (a.formalParameterIndex != b.formalParameterIndex) return false;
    if (a.throwsIndex != b.throwsIndex) return false;
    if (a.catchIndex != b.catchIndex) return false;
    if (a.offset != b.offset) return false;
    if (a.typeArgumentIndex != b.typeArgumentIndex) return false;
    if (a.localVars.size() != b.localVars.size()) return false;
    for (ca::usize i = 0; i < a.localVars.size(); ++i) {
        const jbc::core::TypeAnnotationsAttr::LocalVarEntry& la = a.localVars[i];
        const jbc::core::TypeAnnotationsAttr::LocalVarEntry& lb = b.localVars[i];
        if (la.startPc != lb.startPc || la.length != lb.length || la.slot != lb.slot) {
            return false;
        }
    }
    if (a.typePath.size() != b.typePath.size()) return false;
    for (ca::usize i = 0; i < a.typePath.size(); ++i) {
        if (a.typePath[i].kind != b.typePath[i].kind
            || a.typePath[i].typeArgIndex != b.typePath[i].typeArgIndex) {
            return false;
        }
    }
    return cf.utf8(a.typeIndexCp) == cf.utf8(b.typeIndexCp);
}

/// 普通注解两槽去重（RVA + RIA 共享一份 seen 集，槽序 = 属性表序）：键 =
/// 描述符内容，首个实例保留、后继删除。正向扫描登记 seen（「首个」语义），
/// 命中条目原位摘除，删空连槽摘除。解码失败/类型不符槽保守跳过（不登记、
/// 不改动——该槽条目不参与后续槽的 seen 集）。返回是否发生任何变化。
bool dedupeAnnotationSlots(ClassFile& cf, std::vector<AttrSlot>& attrs)
{
    std::vector<std::string_view> seen;
    bool                          changed = false;
    for (std::size_t i = 0; i < attrs.size();) {
        AttrSlot& slot = attrs[i];
        const bool target = slotNameIs(cf, slot, kRva) || slotNameIs(cf, slot, kRia);
        if (!target) {
            ++i;
            continue;
        }
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()
            || std::move(decoded).unwrap()->kind() != AttrKind::Annotations) {
            ++i;
            continue;
        }
        const auto* body = ca::core::tag_cast::cast<const AnnotationsAttr*>(slot.peek());
        std::vector<bool> keep(body->annos.size(), true);
        bool              anyDup = false;
        for (ca::usize k = 0; k < body->annos.size(); ++k) {
            const std::string_view desc = cf.utf8(body->annos[k].typeCp);
            if (seenContains(seen, desc)) {
                keep[k] = false;
                anyDup  = true;
            }
            else {
                seen.push_back(desc);
            }
        }
        if (!anyDup) {
            ++i;
            continue;
        }
        auto& annos = ca::core::tag_cast::cast<AnnotationsAttr*>(slot.mutate())->annos;
        for (ca::usize k = annos.size(); k-- > 0;) {
            if (!keep[k]) annos.erase(annos.begin() + static_cast<std::ptrdiff_t>(k));
        }
        if (annos.empty()) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            continue;   // 槽已移除，不前进（后继槽回落到当前下标）
        }
        changed = true;
        ++i;
    }
    return changed;
}

/// type annotation 两槽去重（RVTA + RITA 共享一份 seen 集）：键 = 目标位全集
/// + 描述符内容，目标位不同的同描述符条目各保留首个。集范围由调用方定
/// （成员级一份 / Code 嵌套一份）。返回是否发生任何变化。
bool dedupeTypeAnnotationSlots(ClassFile& cf, std::vector<AttrSlot>& attrs)
{
    std::vector<const TypeAnnotationsAttr::Annotation*> seen;
    bool                                                changed = false;
    for (std::size_t i = 0; i < attrs.size();) {
        AttrSlot& slot = attrs[i];
        const bool target = slotNameIs(cf, slot, kRvta) || slotNameIs(cf, slot, kRita);
        if (!target) {
            ++i;
            continue;
        }
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()
            || std::move(decoded).unwrap()->kind() != AttrKind::TypeAnnotations) {
            ++i;
            continue;
        }
        const auto* body = ca::core::tag_cast::cast<const TypeAnnotationsAttr*>(slot.peek());
        const auto  isDup = [&cf, &seen](const TypeAnnotationsAttr::Annotation& a) {
            for (const TypeAnnotationsAttr::Annotation* prior : seen) {
                if (typeAnnoTargetEquals(cf, *prior, a)) return true;
            }
            return false;
        };
        std::vector<bool> keep(body->annotations.size(), true);
        bool              anyDup = false;
        for (ca::usize k = 0; k < body->annotations.size(); ++k) {
            if (isDup(body->annotations[k])) {
                keep[k] = false;
                anyDup  = true;
            }
            else {
                seen.push_back(&body->annotations[k]);
            }
        }
        if (!anyDup) {
            ++i;
            continue;
        }
        auto& annos =
            ca::core::tag_cast::cast<TypeAnnotationsAttr*>(slot.mutate())->annotations;
        for (ca::usize k = annos.size(); k-- > 0;) {
            if (!keep[k]) annos.erase(annos.begin() + static_cast<std::ptrdiff_t>(k));
        }
        if (annos.empty()) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            continue;
        }
        changed = true;
        ++i;
    }
    return changed;
}

/// 参数注解两槽去重（RVPa + RIPA 共享一份 seen 集）：键 = (参数行下标, 描述符
/// 内容)，行数与行序保持；部分摘除留槽、全空摘槽。typed 行模型为主，typed
/// 解码拒绝的 legacy Raw 形状保守跳过。返回是否发生任何变化。
bool dedupeParameterAnnotationSlots(ClassFile& cf, std::vector<AttrSlot>& attrs)
{
    using SeenKey = std::pair<ca::usize, std::string_view>;
    std::vector<SeenKey> seen;
    const auto rowSeen = [&seen](const ca::usize row, const std::string_view desc) {
        for (const SeenKey& key : seen) {
            if (key.first == row && key.second == desc) return true;
        }
        return false;
    };
    bool changed = false;
    for (std::size_t i = 0; i < attrs.size();) {
        AttrSlot& slot = attrs[i];
        const bool target = slotNameIs(cf, slot, kRvpa) || slotNameIs(cf, slot, kRipa);
        if (!target) {
            ++i;
            continue;
        }
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()) {
            ++i;
            continue;
        }
        if (std::move(decoded).unwrap()->kind() != AttrKind::ParameterAnnotations) {
            ++i;
            continue;
        }
        const auto* ro = ca::core::tag_cast::cast<const ParameterAnnotationsAttr*>(slot.peek());
        std::vector<std::vector<bool>> keep(ro->parameters.size());
        bool                           anyDup  = false;
        bool                           anyKept = false;
        for (ca::usize r = 0; r < ro->parameters.size(); ++r) {
            keep[r].assign(ro->parameters[r].size(), true);
            for (ca::usize k = 0; k < ro->parameters[r].size(); ++k) {
                const std::string_view desc = cf.utf8(ro->parameters[r][k].typeCp);
                if (rowSeen(r, desc)) {
                    keep[r][k] = false;
                    anyDup     = true;
                }
                else {
                    seen.emplace_back(r, desc);
                    anyKept = true;
                }
            }
        }
        if (!anyDup) {
            ++i;
            continue;
        }
        auto& rows =
            ca::core::tag_cast::cast<ParameterAnnotationsAttr*>(slot.mutate())->parameters;
        for (ca::usize r = 0; r < rows.size(); ++r) {
            for (ca::usize k = rows[r].size(); k-- > 0;) {
                if (!keep[r][k]) rows[r].erase(rows[r].begin() + static_cast<std::ptrdiff_t>(k));
            }
        }
        changed = true;
        if (!anyKept) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
    return changed;
}

/// 方法 Code 嵌套 Runtime{Visible,Invisible}TypeAnnotations 槽去重（集范围 =
/// 本 Code 属性）；改动需父 Code 槽连带置脏。返回是否发生变化。
bool dedupeCodeNestedTypeAnnotations(ClassFile& cf, MemberInfo& m)
{
    AttrSlot* codeSlot = findSlot(cf, m.attrs, kCode);
    if (codeSlot == nullptr) return false;
    auto decoded = cf.decodeCode(m);
    if (decoded.is_err()) return false;
    CodeAttr* code = decoded.unwrap();
    const bool changed = dedupeTypeAnnotationSlots(cf, code->attrs);
    if (changed) {
        (void)codeSlot->mutate();
    }
    return changed;
}

/// record 组件注解面去重：每个组件一份集（Recaf RecordDupAnnoRemover 同粒度
/// ——组件的普通注解与 type annotation 各自组内去重），父 Record 槽连带置脏
/// （组件嵌套槽改动对 writeClass 不可见，除非 Record 槽本身置脏重编码）。
/// 无 Record 槽/解码失败保守保持原样。返回是否发生变化。
bool dedupeRecordComponentAnnotationSlots(ClassFile& cf)
{
    AttrSlot* recordSlot = findSlot(cf, cf.attrs, kRecord);
    if (recordSlot == nullptr) return false;
    auto d = decodeRecordBody(cf, *recordSlot);
    if (d.is_err()) return false;
    auto  body = std::move(d).unwrap();
    auto& rec  = static_cast<RecordAttr&>(*body);
    bool  changed = false;
    for (RecordAttr::Component& component : rec.components) {
        changed |= dedupeAnnotationSlots(cf, component.attrs);
        changed |= dedupeTypeAnnotationSlots(cf, component.attrs);
    }
    if (!changed) return false;
    *recordSlot = AttrSlot::makeNew(recordSlot->nameCp, std::move(body));
    return true;
}

}   // namespace

// ── DuplicateAnnotationRemover ──

mjbase::spi::PreservedAnalyses DuplicateAnnotationRemover::run(
    jbc::ir::ClassSession& session, mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf = session.backing();
    bool       changed = false;

    changed |= dedupeAnnotationSlots(cf, cf.attrs);
    changed |= dedupeTypeAnnotationSlots(cf, cf.attrs);
    changed |= dedupeRecordComponentAnnotationSlots(cf);
    for (MemberInfo& f : cf.fields) {
        changed |= dedupeAnnotationSlots(cf, f.attrs);
        changed |= dedupeTypeAnnotationSlots(cf, f.attrs);
    }
    for (MemberInfo& m : cf.methods) {
        changed |= dedupeAnnotationSlots(cf, m.attrs);
        changed |= dedupeTypeAnnotationSlots(cf, m.attrs);
        changed |= dedupeParameterAnnotationSlots(cf, m.attrs);
        changed |= dedupeCodeNestedTypeAnnotations(cf, m);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
