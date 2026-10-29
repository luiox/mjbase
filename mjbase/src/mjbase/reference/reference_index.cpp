#include "mjbase/reference/reference_index.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/cp_resolve.hpp"
#include "jbc/ir/change_set.hpp"
#include "jbc/ir/units.hpp"

#include "libca/core/tag_cast.hpp"

#include "mjbase/asm/analyzer/alias_analysis.hpp"
#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/analysis/cp_refs.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/reference/reflection_interpreter.hpp"

namespace mjbase::reference {

using jbc::core::AttrSlot;
using jbc::core::CpTag;
using jbc::core::HandleSem;
using jbc::core::Op;
using jbc::ir::SymbolId;
using ConstSem    = jbc::ir::ClassSession::ConstSem;
using MemberRefSem = mjbase::analysis::MemberRefSem;
using mjbase::asm_analyzer::aliasInterned;
using mjbase::analysis::resolveClassName;
using mjbase::analysis::resolveMemberRef;

namespace {

bool isInvokeFamily(Op op)
{
    return op == Op::INVOKEVIRTUAL || op == Op::INVOKESPECIAL || op == Op::INVOKESTATIC ||
           op == Op::INVOKEINTERFACE;
}

bool isFieldAccessFamily(Op op)
{
    return op == Op::GETSTATIC || op == Op::PUTSTATIC || op == Op::GETFIELD || op == Op::PUTFIELD;
}

bool isTypeInstruction(Op op)
{
    return op == Op::NEW || op == Op::ANEWARRAY || op == Op::CHECKCAST || op == Op::INSTANCEOF;
}

// ── 描述符/签名内的类型抽取（抽取结果是入参视图的子视图，随 intern 进
//    索引池后长期有效）─────────────────────────────────────────

void appendDescriptorTypes(std::string_view desc, std::vector<std::string_view>& out)
{
    for (ca::usize i = 0; i < desc.size(); ++i) {
        if (desc[i] != 'L') continue;
        const ca::usize start = i + 1;
        ca::usize       end   = start;
        while (end < desc.size() && desc[end] != ';') {
            ++end;
        }
        if (end >= desc.size()) break;
        out.push_back(desc.substr(start, end - start));
        i = end;
    }
}

void appendSignatureTypes(std::string_view signature, std::vector<std::string_view>& out)
{
    for (ca::usize i = 0; i < signature.size(); ++i) {
        if (signature[i] != 'L') continue;
        const ca::usize start = i + 1;
        ca::usize       end   = start;
        while (end < signature.size() && signature[end] != ';' && signature[end] != '<') {
            ++end;
        }
        if (end > start) out.push_back(signature.substr(start, end - start));
    }
}

bool looksLikeClassName(std::string_view value)
{
    if (value.empty()) return false;
    if (value.find('/') == std::string_view::npos && value.find('.') == std::string_view::npos) {
        return false;
    }
    if (value.find(' ') != std::string_view::npos || value.find('(') != std::string_view::npos ||
        value.find(')') != std::string_view::npos) {
        return false;
    }
    return true;
}

bool isReflectionMethodLookup(std::string_view owner, std::string_view name, std::string_view desc)
{
    return owner == "java/lang/Class" && (name == "getDeclaredMethod" || name == "getMethod") &&
           desc == "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;";
}

bool isReflectionConstructorLookup(std::string_view owner, std::string_view name,
                                   std::string_view desc)
{
    return owner == "java/lang/Class" &&
           (name == "getDeclaredConstructor" || name == "getConstructor") &&
           desc == "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;";
}

bool isReflectionFieldLookup(std::string_view owner, std::string_view name, std::string_view desc)
{
    return owner == "java/lang/Class" && (name == "getDeclaredField" || name == "getField") &&
           desc == "(Ljava/lang/String;)Ljava/lang/reflect/Field;";
}

// ── method handle（JVMS 4.4.8）─────────────────────────────────
//
// 句柄解析自 B8 8.4 起直用 libjbc cp_resolve 的严格口径原语
//（jbc::core::resolveHandleCp：reference_kind ∉1..9 及 kind-tag 不匹配即
// 解析失败），本文件不再持有本地副本。kind 域的 1..4 字段 / 5..9 方法分
// 流语义见 appendHandleReference 的分支注。

bool isLambdaMetafactoryHandle(const HandleSem& handle)
{
    return handle.owner == "java/lang/invoke/LambdaMetafactory" &&
           (handle.name == "metafactory" || handle.name == "altMetafactory");
}

// ── libjbc 未建模属性的 RawAttr 字节游标（大端，u1/u2/u4）──

struct RawCursor
{
    const ca::u8* data = nullptr;
    ca::usize     size = 0;
    ca::usize     off  = 0;

    bool u8(ca::u8& out)
    {
        if (off + 1 > size) return false;
        out = data[off++];
        return true;
    }
    bool u16(ca::u16& out)
    {
        if (off + 2 > size) return false;
        out = static_cast<ca::u16>((static_cast<ca::u16>(data[off]) << 8) | data[off + 1]);
        off += 2;
        return true;
    }
    bool u32(ca::u32& out)
    {
        if (off + 4 > size) return false;
        out = (static_cast<ca::u32>(data[off]) << 24) | (static_cast<ca::u32>(data[off + 1]) << 16) |
              (static_cast<ca::u32>(data[off + 2]) << 8) | static_cast<ca::u32>(data[off + 3]);
        off += 4;
        return true;
    }
    bool skip(ca::usize n)
    {
        if (off + n > size) return false;
        off += n;
        return true;
    }
};

const AttrSlot* findAttrByName(const std::vector<AttrSlot>&    attrs,
                               const jbc::core::ConstantPool& cp, std::string_view name)
{
    for (const AttrSlot& slot : attrs) {
        if (cp.utf8Of(slot.nameCp) == name) return &slot;
    }
    return nullptr;
}

/// 解码属性槽为 jbc 建模体（AnnotationsAttr / RawAttr 等）；失败返回 nullptr。
const jbc::core::AttrBody* decodeAttrBody(jbc::core::ClassFile& cf, const AttrSlot& slot)
{
    auto decoded = cf.decodeAttr(const_cast<AttrSlot&>(slot));
    if (decoded.is_err()) return nullptr;
    return decoded.unwrap();
}

const jbc::core::RawAttr* decodeRawAttr(jbc::core::ClassFile& cf, const AttrSlot& slot)
{
    const auto* body = decodeAttrBody(cf, slot);
    if (body == nullptr) return nullptr;
    return ca::core::tag_cast::dyn_cast<jbc::core::RawAttr>(body);
}

const jbc::core::AnnotationsAttr* decodeAnnotationsAttr(jbc::core::ClassFile&  cf,
                                                        const AttrSlot&        slot)
{
    const auto* body = decodeAttrBody(cf, slot);
    if (body == nullptr) return nullptr;
    return ca::core::tag_cast::dyn_cast<jbc::core::AnnotationsAttr>(body);
}

/// 解码属性槽为 typed SignatureAttr；失败/形态不符返回 nullptr。
const jbc::core::SignatureAttr* decodeSignatureAttr(jbc::core::ClassFile& cf,
                                                    const AttrSlot&       slot)
{
    const auto* body = decodeAttrBody(cf, slot);
    if (body == nullptr) return nullptr;
    return ca::core::tag_cast::dyn_cast<jbc::core::SignatureAttr>(body);
}

// ── 注解类型抽取：注解类型描述符 + 'e'（枚举 type_name）与 'c'（class 信息）
//    元素值，递归嵌套与数组。─────────────────────────────────

bool parseRawAnnotation(RawCursor& cur, const jbc::core::ConstantPool& cp,
                        std::vector<std::string_view>& out);

void appendAnnotationTypes(const jbc::core::AnnotationsAttr::Annotation& anno,
                           const jbc::core::ConstantPool&                cp,
                           std::vector<std::string_view>&                out)
{
    out.push_back(normalizeClassName(cp.utf8Of(anno.typeCp)));
    std::vector<const jbc::core::AnnotationsAttr::ElemValue*> pending;
    for (const auto& entry : anno.values) {
        pending.push_back(&entry.second);
    }
    while (!pending.empty()) {
        const jbc::core::AnnotationsAttr::ElemValue* elem = pending.back();
        pending.pop_back();
        if (elem->tag == 'e') {
            if (cp.isUtf8(elem->typeNameCp)) {
                out.push_back(normalizeClassName(cp.utf8Of(elem->typeNameCp)));
            }
        }
        else if (elem->tag == 'c') {
            // class_info_index 是 CONSTANT_Class 条目（JVMS 4.7.16.1），经类条目
            // 解析为内部名；直接 utf8Of 会落空。
            const std::string_view cn = resolveClassName(cp, elem->cp);
            if (!cn.empty()) out.push_back(normalizeClassName(cn));
        }
        else if (elem->tag == '@' && elem->nested) {
            out.push_back(normalizeClassName(cp.utf8Of(elem->nested->typeCp)));
        }
        for (const auto& item : elem->items) pending.push_back(&item);
    }
}

void appendAnnotationAttrTypes(const jbc::core::AnnotationsAttr& attr,
                               const jbc::core::ConstantPool&    cp,
                               std::vector<std::string_view>&    out)
{
    for (const auto& anno : attr.annos) appendAnnotationTypes(anno, cp, out);
}

/// RawAttr 字节形态的 element_value 解析（AnnotationDefault /
/// ParameterAnnotations 用；Record 组件内嵌注解已迁 typed 体直读）。
bool parseRawElementValue(RawCursor& cur, const jbc::core::ConstantPool& cp,
                          std::vector<std::string_view>& out)
{
    ca::u8 tag = 0;
    if (!cur.u8(tag)) return false;
    switch (tag) {
    case 'e': {   // enum_value：(type_name_index, const_name_index)
        ca::u16 typeNameCp = 0;
        ca::u16 constNameCp = 0;
        if (!cur.u16(typeNameCp) || !cur.u16(constNameCp)) return false;
        if (cp.isUtf8(typeNameCp)) out.push_back(normalizeClassName(cp.utf8Of(typeNameCp)));
        return true;
    }
    case 'c': {   // class_value：class_info_index（CONSTANT_Class 条目，返回内部名）
        ca::u16 classInfoCp = 0;
        if (!cur.u16(classInfoCp)) return false;
        const std::string_view cn = resolveClassName(cp, classInfoCp);
        if (!cn.empty()) out.push_back(normalizeClassName(cn));
        return true;
    }
    case '@': return parseRawAnnotation(cur, cp, out);
    case '[': {   // array_value：count + 逐元素
        ca::u16 count = 0;
        if (!cur.u16(count)) return false;
        for (ca::u16 i = 0; i < count; ++i) {
            if (!parseRawElementValue(cur, cp, out)) return false;
        }
        return true;
    }
    default: return cur.skip(2);   // 'B' 'C' 'D' 'F' 'I' 'J' 'S' 'Z' 's'：u2 常量索引
    }
}

/// RawAttr 字节形态的 annotation 结构解析（type_descriptor + 元素表）。
bool parseRawAnnotation(RawCursor& cur, const jbc::core::ConstantPool& cp,
                        std::vector<std::string_view>& out)
{
    ca::u16 typeCp = 0;
    ca::u16 count  = 0;
    if (!cur.u16(typeCp) || !cur.u16(count)) return false;
    if (cp.isUtf8(typeCp)) out.push_back(normalizeClassName(cp.utf8Of(typeCp)));
    for (ca::u16 i = 0; i < count; ++i) {
        ca::u16 elementNameCp = 0;   // element_name_index：不产生类型引用，跳过
        if (!cur.u16(elementNameCp)) return false;
        if (!parseRawElementValue(cur, cp, out)) return false;
    }
    return true;
}

bool parseRawAnnotationList(RawCursor& cur, const jbc::core::ConstantPool& cp,
                            std::vector<std::string_view>& out)
{
    ca::u16 count = 0;
    if (!cur.u16(count)) return false;
    for (ca::u16 i = 0; i < count; ++i) {
        if (!parseRawAnnotation(cur, cp, out)) return false;
    }
    return true;
}

}   // namespace

// ── 查询面（文本三段经池 intern 落键）────────────────────────

namespace {

MemberKey toMemberKey(KeyPool& pool, std::string_view owner, std::string_view name,
                      std::string_view desc)
{
    return memberKeyOf(pool, classKeyOf(pool, owner), name, desc);
}

const std::vector<MethodReference>   kEmptyMethodRefs;
const std::vector<FieldReference>    kEmptyFieldRefs;
const std::vector<TypeReference>     kEmptyTypeRefs;
const std::vector<MethodDeclaration> kEmptyMethodDecls;
const std::vector<FieldDeclaration>  kEmptyFieldDecls;

}   // namespace

void ReferenceIndex::clear()
{
    // 池只增不减（内容寻址驻留）：仅清事实表，文本驻留随索引存活期复用。
    classDecls_.clear();
    methodDecls_.clear();
    fieldDecls_.clear();
    methodRefs_.clear();
    fieldRefs_.clear();
    typeRefs_.clear();
    classDeclsByKey_.clear();
    methodDeclsByKey_.clear();
    fieldDeclsByKey_.clear();
    methodDeclsByOwner_.clear();
    fieldDeclsByOwner_.clear();
    methodRefsByTarget_.clear();
    fieldRefsByTarget_.clear();
    typeRefsByTarget_.clear();
    methodRefsByTargetOwner_.clear();
    fieldRefsByTargetOwner_.clear();
    methodRefsBySource_.clear();
    fieldRefsBySource_.clear();
    typeRefsBySource_.clear();
    methodRefsBySourceClass_.clear();
    fieldRefsBySourceClass_.clear();
    typeRefsBySourceClass_.clear();
}

void ReferenceIndex::build(const mjbase::model::ResourceContainer& container)
{
    clear();
    // alias 消费（文本契约）：共享别名摘要按 (owner,name,desc) 文本查询，
    // "class:X" 接收者身份按驻留视图消费（见头注键契约）。
    mjbase::asm_analyzer::AliasAnalysis aliases;
    aliases.build(container);

    const auto scanned = mjbase::analysis::scanClassResources(container);
    for (const auto& scannedClass : scanned) {
        indexSession(*scannedClass.session, &aliases);
    }
    resolveReflectionMemberDescriptors();
}

void ReferenceIndex::indexClassHeader(jbc::ir::ClassSession& session,
                                      std::string_view       className,
                                      const ReferenceSite&   classSite)
{
    jbc::core::ClassFile&      cf  = session.backing();
    const jbc::ir::ClassUnit&  unit = session.unit();
    const jbc::ir::SymbolPool& syms = unit.symbols();
    jbc::core::ConstantPool&   cp  = cf.cp;

    auto iv = [&](std::string_view text) { return pool_.str(pool_.intern(text)); };

    std::vector<std::string_view> types;

    auto appendClassTypeRef = [&](std::string_view target, TypeReferenceKind kind) {
        if (target.empty()) return;
        TypeReference ref;
        ref.site       = classSite;
        ref.targetType = iv(target);
        ref.kind       = kind;
        appendTypeReference(ref, false);
    };

    // 类声明（先到先得）。
    if (classDeclsByKey_.find(className) == classDeclsByKey_.end()) {
        ClassDeclaration decl;
        decl.name      = className;
        decl.superName = iv(syms.str(unit.superClass));
        decl.interfaces.reserve(unit.interfaces.size());
        for (const SymbolId iface : unit.interfaces) {
            decl.interfaces.push_back(iv(syms.str(iface)));
        }
        decl.access = unit.access;
        classDeclsByKey_.emplace(decl.name, classDecls_.size());
        classDecls_.push_back(std::move(decl));
    }

    const std::string_view superName = syms.str(unit.superClass);
    if (!superName.empty()) appendClassTypeRef(superName, TypeReferenceKind::ClassSuper);
    for (const SymbolId iface : unit.interfaces) {
        appendClassTypeRef(syms.str(iface), TypeReferenceKind::ClassInterface);
    }

    // ── 类级元数据（Signature/注解族走 jbc 建模体；其余从 RawAttr 字节按 JVMS 布局解析）──
    {
        // Signature（typed SignatureAttr）
        if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, "Signature")) {
            if (const auto* attr = decodeSignatureAttr(cf, *slot)) {
                types.clear();
                appendSignatureTypes(cp.utf8Of(attr->signatureCp), types);
                for (const std::string_view type : types) {
                    appendClassTypeRef(type, TypeReferenceKind::Signature);
                }
            }
        }
        // 类级 Runtime 注解（jbc 建模体；*TypeAnnotation 家族未建模，本批放弃）
        for (const std::string_view attrName :
             {std::string_view{"RuntimeVisibleAnnotations"},
              std::string_view{"RuntimeInvisibleAnnotations"}}) {
            if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, attrName)) {
                if (const auto* anno = decodeAnnotationsAttr(cf, *slot)) {
                    types.clear();
                    appendAnnotationAttrTypes(*anno, cp, types);
                    for (const std::string_view type : types) {
                        appendClassTypeRef(type, TypeReferenceKind::Annotation);
                    }
                }
            }
        }
        // EnclosingMethod：typed 体公开入口解码
        //（不触碰槽状态）。
        if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, "EnclosingMethod")) {
            auto d = decodeEnclosingMethodBody(cf, *slot);
            if (d.is_ok()) {
                const auto body = std::move(d).unwrap();
                const auto& em  = static_cast<const jbc::core::EnclosingMethodAttr&>(*body);
                appendClassTypeRef(resolveClassName(cp, em.classCp),
                                   TypeReferenceKind::StructuralMetadata);
            }
        }
        // NestHost / NestMembers / PermittedSubclasses：typed 体公开入口解码
        //（不触碰槽状态）。
        if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, "NestHost")) {
            auto d = decodeNestHostBody(cf, *slot);
            if (d.is_ok()) {
                const auto body = std::move(d).unwrap();
                const auto& nh =
                    static_cast<const jbc::core::NestHostAttr&>(*body);
                appendClassTypeRef(resolveClassName(cp, nh.hostClassCp),
                                   TypeReferenceKind::StructuralMetadata);
            }
        }
        for (const std::string_view attrName :
             {std::string_view{"NestMembers"}, std::string_view{"PermittedSubclasses"}}) {
            // 体同构但 typed 面各归各：两名字各走各的公开解码入口。
            if (attrName == "NestMembers") {
                if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, attrName)) {
                    auto d = decodeNestMembersBody(cf, *slot);
                    if (d.is_ok()) {
                        const auto body = std::move(d).unwrap();
                        const auto& nm =
                            static_cast<const jbc::core::NestMembersAttr&>(*body);
                        for (const ca::u32 memberCp : nm.memberCps) {
                            appendClassTypeRef(resolveClassName(cp, memberCp),
                                               TypeReferenceKind::StructuralMetadata);
                        }
                    }
                }
            }
            else {
                if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, attrName)) {
                    auto d = decodePermittedSubclassesBody(cf, *slot);
                    if (d.is_ok()) {
                        const auto body = std::move(d).unwrap();
                        const auto& ps =
                            static_cast<const jbc::core::PermittedSubclassesAttr&>(*body);
                        for (const ca::u32 memberCp : ps.permittedCps) {
                            appendClassTypeRef(resolveClassName(cp, memberCp),
                                               TypeReferenceKind::StructuralMetadata);
                        }
                    }
                }
            }
        }
        // InnerClasses：inner + outer 两个类名；
        // typed 体公开入口解码（同上），outer==0 匿名形态原样收集空名由
        // appendClassTypeRef 丢弃。
        if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, "InnerClasses")) {
            auto d = decodeInnerClassesBody(cf, *slot);
            if (d.is_ok()) {
                const auto body = std::move(d).unwrap();
                const auto& ic  = static_cast<const jbc::core::InnerClassesAttr&>(*body);
                for (const jbc::core::InnerClassesAttr::Row& row : ic.rows) {
                    appendClassTypeRef(resolveClassName(cp, row.innerClassInfoCp),
                                       TypeReferenceKind::StructuralMetadata);
                    appendClassTypeRef(resolveClassName(cp, row.outerClassInfoCp),
                                       TypeReferenceKind::StructuralMetadata);
                }
            }
        }
        // Record：组件描述符（StructuralMetadata）+ 组件内嵌注解（Annotation）。
        // typed 体公开入口解码（不触碰槽状态，分派接线前后同形）；组件
        // 嵌套 Signature/注解槽走各自 typed 解码面。
        if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, "Record")) {
            auto d = decodeRecordBody(cf, *slot);
            if (d.is_ok()) {
                const auto body = std::move(d).unwrap();
                const auto& rec = static_cast<const jbc::core::RecordAttr&>(*body);
                for (const jbc::core::RecordAttr::Component& component : rec.components) {
                    types.clear();
                    if (cp.isUtf8(component.descriptorCp)) {
                        appendDescriptorTypes(cp.utf8Of(component.descriptorCp), types);
                    }
                    for (const std::string_view type : types) {
                        appendClassTypeRef(type, TypeReferenceKind::StructuralMetadata);
                    }
                    for (const AttrSlot& nested : component.attrs) {
                        const std::string_view attrNameView = cp.utf8Of(nested.nameCp);
                        if (attrNameView == "Signature") {
                            if (const auto* sig = decodeSignatureAttr(cf, nested)) {
                                types.clear();
                                appendSignatureTypes(cp.utf8Of(sig->signatureCp), types);
                                for (const std::string_view type : types) {
                                    appendClassTypeRef(type, TypeReferenceKind::Signature);
                                }
                            }
                        }
                        else if (attrNameView == "RuntimeVisibleAnnotations" ||
                                 attrNameView == "RuntimeInvisibleAnnotations") {
                            if (const auto* anno = decodeAnnotationsAttr(cf, nested)) {
                                types.clear();
                                appendAnnotationAttrTypes(*anno, cp, types);
                                for (const std::string_view type : types) {
                                    appendClassTypeRef(type, TypeReferenceKind::Annotation);
                                }
                            }
                        }
                    }
                }
            }
        }
        // Module + ModuleMainClass。
        // typed 体公开入口解码（不触碰槽状态）；requires/
        // exports/opens 不产生类引用，结构跳过。
        if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, "ModuleMainClass")) {
            auto d = decodeModuleMainClassBody(cf, *slot);
            if (d.is_ok()) {
                auto body = std::move(d).unwrap();
                const auto& mc = static_cast<const jbc::core::ModuleMainClassAttr&>(*body);
                appendClassTypeRef(resolveClassName(cp, mc.mainClassCp),
                                   TypeReferenceKind::StructuralMetadata);
            }
        }
        if (const AttrSlot* slot = findAttrByName(cf.attrs, cp, "Module")) {
            auto d = decodeModuleBody(cf, *slot);
            if (d.is_ok()) {
                const auto body = std::move(d).unwrap();
                const auto& mod = static_cast<const jbc::core::ModuleAttr&>(*body);
                for (const ca::u32 useCp : mod.usesCps) {
                    appendClassTypeRef(resolveClassName(cp, useCp),
                                       TypeReferenceKind::StructuralMetadata);
                }
                for (const jbc::core::ModuleAttr::Provide& p : mod.provides) {
                    appendClassTypeRef(resolveClassName(cp, p.serviceCp),
                                       TypeReferenceKind::StructuralMetadata);
                    for (const ca::u32 withCp : p.withCps) {
                        appendClassTypeRef(resolveClassName(cp, withCp),
                                           TypeReferenceKind::StructuralMetadata);
                    }
                }
            }
        }
    }

    // ── 字段声明 + 字段级元数据（类级引用点）──
}

void ReferenceIndex::indexFieldUnit(jbc::ir::ClassSession& session,
                                    ca::usize              fieldIdx,
                                    std::string_view       className,
                                    const ReferenceSite&   classSite)
{
    jbc::core::ClassFile&      cf  = session.backing();
    const jbc::ir::ClassUnit&  unit = session.unit();
    const jbc::ir::SymbolPool& syms = unit.symbols();
    jbc::core::ConstantPool&   cp  = cf.cp;

    auto iv = [&](std::string_view text) { return pool_.str(pool_.intern(text)); };
    std::vector<std::string_view> types;

    {
        const jbc::ir::FieldUnit&    field = unit.fields[fieldIdx];
        // 成员增删后逻辑下标与物理位置不再恒等（design.md §34.1）：字段元数据
        // 一律经 backingIndex 定位物理 method/field_info。
        const jbc::core::MemberInfo& backing = cf.fields[field.backingIndex];
        const std::string_view            fieldName = syms.str(field.name);
        const std::string_view            fieldDesc = syms.str(field.desc);

        FieldDeclaration decl;
        decl.field  = FieldRefKey{className, iv(fieldName), iv(fieldDesc)};
        decl.access = field.access;
        const MemberKey declKey = toMemberKey(pool_, className, fieldName, fieldDesc);
        if (fieldDeclsByKey_.find(declKey) == fieldDeclsByKey_.end()) {
            fieldDeclsByKey_.emplace(declKey, fieldDecls_.size());
            fieldDecls_.push_back(decl);
            fieldDeclsByOwner_[className].push_back(decl);
        }

        auto appendFieldScopedTypeRef = [&](std::string_view target, TypeReferenceKind kind) {
            TypeReference ref;
            ref.site       = classSite;
            ref.targetType = iv(target);
            ref.kind       = kind;
            appendTypeReference(ref, false);
        };

        types.clear();
        appendDescriptorTypes(fieldDesc, types);
        for (const std::string_view type : types) {
            appendFieldScopedTypeRef(type, TypeReferenceKind::FieldDeclarationDescriptor);
        }
        if (const AttrSlot* slot =
                findAttrByName(backing.attrs, cp, "Signature")) {
            if (const auto* attr = decodeSignatureAttr(cf, *slot)) {
                types.clear();
                appendSignatureTypes(cp.utf8Of(attr->signatureCp), types);
                for (const std::string_view type : types) {
                    appendFieldScopedTypeRef(type, TypeReferenceKind::Signature);
                }
            }
        }
        for (const std::string_view attrName :
             {std::string_view{"RuntimeVisibleAnnotations"},
              std::string_view{"RuntimeInvisibleAnnotations"}}) {
            if (const AttrSlot* slot = findAttrByName(backing.attrs, cp, attrName)) {
                if (const auto* anno = decodeAnnotationsAttr(cf, *slot)) {
                    types.clear();
                    appendAnnotationAttrTypes(*anno, cp, types);
                    for (const std::string_view type : types) {
                        appendFieldScopedTypeRef(type, TypeReferenceKind::Annotation);
                    }
                }
            }
        }
    }
}

void ReferenceIndex::indexSession(jbc::ir::ClassSession&      session,
                                  const mjbase::asm_analyzer::AliasAnalysis* aliases)
{
    const jbc::ir::ClassUnit&  unit = session.unit();
    const jbc::ir::SymbolPool& syms = unit.symbols();

    const std::string_view className = pool_.str(pool_.intern(syms.str(unit.thisClass)));
    const ReferenceSite    classSite{className, {}, {}, -1, -1};

    indexClassHeader(session, className, classSite);
    for (ca::usize fi = 0; fi < unit.fields.size(); ++fi) {
        indexFieldUnit(session, fi, className, classSite);
    }

    // ── 方法声明（先全部入表，再逐方法体扫描）──
    for (const jbc::ir::MethodUnit& method : unit.methods) {
        appendMethodDeclaration(session, method, className);
    }

    const std::vector<jbc::ir::CodeVisit> visits = jbc::ir::collectCodeUnits(session);
    for (ca::usize mi = 0; mi < unit.methods.size() && mi < visits.size(); ++mi) {
        const jbc::ir::MethodUnit&    method  = unit.methods[mi];
        const std::string_view        methodName = syms.str(method.name);
        const std::string_view        methodDesc = syms.str(method.desc);
        const ReferenceSite           declarationSite{className, pool_.str(pool_.intern(methodName)), pool_.str(pool_.intern(methodDesc)), -1, -1};

        indexMethodMetadata(session, method, declarationSite);

        // 方法体事实（NoCode/Failed：抽象/原生/畸形 → 无方法体事实）
        if (visits[mi].kind != jbc::ir::CodeVisit::Kind::Ok) continue;
        indexMethodBody(session, *visits[mi].unit, className, method, aliases);
    }
}

void ReferenceIndex::appendMethodDeclaration(jbc::ir::ClassSession&     session,
                                             const jbc::ir::MethodUnit& method,
                                             std::string_view           className)
{
    auto iv = [&](std::string_view text) { return pool_.str(pool_.intern(text)); };
    const std::string_view methodName = iv(session.unit().symbols().str(method.name));
    const std::string_view methodDesc = iv(session.unit().symbols().str(method.desc));
    MethodDeclaration      decl;
    decl.method  = MethodRefKey{className, methodName, methodDesc};
    decl.access  = method.access;
    const MemberKey declKey = toMemberKey(pool_, className, methodName, methodDesc);
    if (methodDeclsByKey_.find(declKey) == methodDeclsByKey_.end()) {
        methodDeclsByKey_.emplace(declKey, methodDecls_.size());
        methodDecls_.push_back(decl);
        methodDeclsByOwner_[className].push_back(decl);
    }
}

void ReferenceIndex::indexMethodMetadata(jbc::ir::ClassSession&     session,
                                         const jbc::ir::MethodUnit& method,
                                         const ReferenceSite&       declarationSite)
{
    jbc::core::ClassFile&      cf  = session.backing();
    const jbc::ir::SymbolPool& syms = session.unit().symbols();
    jbc::core::ConstantPool&   cp  = cf.cp;
    const jbc::core::MemberInfo& backing = cf.methods[method.backingIndex];
    const std::string_view       methodDesc = syms.str(method.desc);

    auto iv = [&](std::string_view text) { return pool_.str(pool_.intern(text)); };
    std::vector<std::string_view> types;

    auto appendMethodScopedTypeRef = [&](std::string_view target, TypeReferenceKind kind) {
        TypeReference ref;
        ref.site       = declarationSite;
        ref.targetType = iv(target);
        ref.kind       = kind;
        appendTypeReference(ref, true);
    };

    types.clear();
    appendDescriptorTypes(methodDesc, types);
    for (const std::string_view type : types) {
        appendMethodScopedTypeRef(type, TypeReferenceKind::MethodDescriptor);
    }
    if (const AttrSlot* slot = findAttrByName(backing.attrs, cp, "Signature")) {
        if (const auto* attr = decodeSignatureAttr(cf, *slot)) {
            types.clear();
            appendSignatureTypes(cp.utf8Of(attr->signatureCp), types);
            for (const std::string_view type : types) {
                appendMethodScopedTypeRef(type, TypeReferenceKind::Signature);
            }
        }
    }
    // 方法级 Runtime 注解 + ParameterAnnotations + AnnotationDefault
    //（*TypeAnnotation / 局部变量注解族 libjbc 未建模，本批放弃）
    for (const std::string_view attrName :
         {std::string_view{"RuntimeVisibleAnnotations"},
          std::string_view{"RuntimeInvisibleAnnotations"}}) {
        if (const AttrSlot* slot = findAttrByName(backing.attrs, cp, attrName)) {
            if (const auto* anno = decodeAnnotationsAttr(cf, *slot)) {
                types.clear();
                appendAnnotationAttrTypes(*anno, cp, types);
                for (const std::string_view type : types) {
                    appendMethodScopedTypeRef(type, TypeReferenceKind::Annotation);
                }
            }
        }
    }
    for (const std::string_view attrName :
         {std::string_view{"RuntimeVisibleParameterAnnotations"},
          std::string_view{"RuntimeInvisibleParameterAnnotations"}}) {
        if (const AttrSlot* slot = findAttrByName(backing.attrs, cp, attrName)) {
            if (const auto* raw = decodeRawAttr(cf, *slot)) {
                RawCursor cur{raw->bytes().data(), raw->bytes().size(), 0};
                ca::u8    paramCount = 0;
                if (cur.u8(paramCount)) {
                    for (ca::u8 p = 0; p < paramCount; ++p) {
                        types.clear();
                        if (!parseRawAnnotationList(cur, cp, types)) break;
                        for (const std::string_view type : types) {
                            appendMethodScopedTypeRef(type, TypeReferenceKind::Annotation);
                        }
                    }
                }
            }
        }
    }
    if (const AttrSlot* slot = findAttrByName(backing.attrs, cp, "AnnotationDefault")) {
        if (const auto* raw = decodeRawAttr(cf, *slot)) {
            RawCursor cur{raw->bytes().data(), raw->bytes().size(), 0};
            types.clear();
            if (parseRawElementValue(cur, cp, types)) {
                for (const std::string_view type : types) {
                    appendMethodScopedTypeRef(type, TypeReferenceKind::Annotation);
                }
            }
        }
    }
}

void ReferenceIndex::indexOneMethod(jbc::ir::ClassSession&                 session,
                                    std::string_view                       methodName,
                                    std::string_view                       methodDesc,
                                    std::string_view                       className,
                                    const mjbase::asm_analyzer::AliasAnalysis* aliases)
{
    jbc::ir::ClassUnit& unit = session.unit();
    const ca::usize     idx  = unit.findMethod(methodName, methodDesc);
    if (idx == jbc::ir::kNoMethod) {
        return;   // 防御面：折叠终态存活的成员必在会话里（同键 add+remove 不会出现在 upsert 桶）
    }
    jbc::ir::MethodUnit& method = unit.methods[idx];
    appendMethodDeclaration(session, method, className);
    const ReferenceSite declarationSite{className,
                                        pool_.str(pool_.intern(methodName)),
                                        pool_.str(pool_.intern(methodDesc)), -1, -1};
    indexMethodMetadata(session, method, declarationSite);
    if (!jbc::ir::hasCode(session, idx)) return;
    auto p = jbc::ir::promoteMethod(session, idx);
    if (p.is_err()) return;   // Failed → 无方法体事实（F1 分流同款）
    indexMethodBody(session, *p.unwrap().get(), className, method, aliases);
}

void ReferenceIndex::indexMethodBody(jbc::ir::ClassSession&                session,
                                     const jbc::ir::CodeUnit&              code,
                                     std::string_view                      className,
                                     const jbc::ir::MethodUnit&            method,
                                     const mjbase::asm_analyzer::AliasAnalysis* aliases)
{
    const jbc::core::ConstantPool& cp    = session.backing().cp;
    const auto&                    order = code.dom.order();
    const std::string_view         methodName = pool_.str(pool_.intern(session.unit().symbols().str(method.name)));
    const std::string_view         methodDesc = pool_.str(pool_.intern(session.unit().symbols().str(method.desc)));

    auto iv = [&](std::string_view text) { return pool_.str(pool_.intern(text)); };

    auto makeSite = [&](ca::usize pos) {
        return ReferenceSite{className, methodName, methodDesc,
                             static_cast<ca::i32>(code.dom.peek(order[pos])->op),
                             static_cast<ca::i32>(pos)};
    };

    std::vector<std::string_view> types;

    auto appendMemberDescriptorTypes = [&](std::string_view desc, TypeReferenceKind kind,
                                           const ReferenceSite& site) {
        types.clear();
        appendDescriptorTypes(desc, types);
        for (const std::string_view type : types) {
            TypeReference ref;
            ref.site       = site;
            ref.targetType = iv(type);
            ref.kind       = kind;
            appendTypeReference(ref, true);
        }
    };

    auto appendHandleReference = [&](const HandleSem&   handle,
                                     const ReferenceSite& site, MethodReferenceKind methodKind) {
        TypeReference ownerRef;
        ownerRef.site       = site;
        ownerRef.targetType = iv(handle.owner);
        ownerRef.kind       = TypeReferenceKind::MethodHandleOwner;
        appendTypeReference(ownerRef, true);

        // kind 1..4 字段 / 5..9 方法（libjbc resolveHandleCp 已做域检与
        // kind-tag 匹配，能到这里的 handle.kind 必在 1..9 内）。
        if (handle.kind >= 5 && handle.kind <= 9) {
            MethodReference ref;
            ref.site        = site;
            ref.target      = MethodRefKey{iv(handle.owner), iv(handle.name), iv(handle.desc)};
            ref.isInterface = handle.isInterface;
            ref.kind        = methodKind;
            appendMethodReference(ref);
            appendMemberDescriptorTypes(handle.desc, TypeReferenceKind::MethodDescriptor, site);
        }
        else if (handle.kind >= 1 && handle.kind <= 4) {
            FieldReference ref;
            ref.site   = site;
            ref.target = FieldRefKey{iv(handle.owner), iv(handle.name), iv(handle.desc)};
            ref.kind   = FieldReferenceKind::MethodHandle;
            appendFieldReference(ref);
            appendMemberDescriptorTypes(handle.desc, TypeReferenceKind::FieldDescriptor, site);
        }
    };

    // 反射字符串/成员查找状态机（hasReflectionClassTarget
    // 跨指令存续，仅被 forName 命中或查找命中消费）。
    bool              hasLastStringConstant    = false;
    bool              hasReflectionClassTarget = false;
    bool              hasReflectionMemberName  = false;
    std::string_view  lastStringConstant;
    std::string_view  reflectionClassTarget;
    std::string_view  reflectionMemberName;

    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const jbc::ir::Insn* in = code.dom.peek(order[pos]);
        if (in == nullptr) continue;
        const Op op = in->op;
        bool     stringLdc = false;

        if (isInvokeFamily(op)) {
            jbc::ir::ClassSession::MemberSem sem;
            if (!session.memberOf(*in, sem)) continue;
            const ReferenceSite site = makeSite(pos);
            MethodReference     ref;
            ref.site        = site;
            ref.target      = MethodRefKey{iv(sem.owner), iv(sem.name), iv(sem.desc)};
            ref.isInterface = op == Op::INVOKEINTERFACE;
            appendMethodReference(ref);

            if (hasLastStringConstant && sem.owner == "java/lang/Class" &&
                sem.name == "forName" &&
                sem.desc == "(Ljava/lang/String;)Ljava/lang/Class;" &&
                looksLikeClassName(lastStringConstant)) {
                reflectionClassTarget    = normalizeClassName(lastStringConstant);
                hasReflectionClassTarget = true;
                hasReflectionMemberName  = false;
                TypeReference reflectionRef;
                reflectionRef.site       = site;
                reflectionRef.targetType = iv(reflectionClassTarget);
                reflectionRef.kind       = TypeReferenceKind::ReflectionString;
                appendTypeReference(reflectionRef, true);
            }
            else if (hasReflectionClassTarget && hasReflectionMemberName &&
                     isReflectionMethodLookup(sem.owner, sem.name, sem.desc)) {
                MethodReference reflectionRef;
                reflectionRef.site   = site;
                reflectionRef.target = MethodRefKey{iv(reflectionClassTarget),
                                                    iv(reflectionMemberName), {}};
                reflectionRef.reflectionArgDesc =
                    reflectionArgumentDescriptor(code, cp, pos);
                reflectionRef.kind = MethodReferenceKind::Reflection;
                appendMethodReference(reflectionRef);
                hasReflectionClassTarget = false;
                hasReflectionMemberName  = false;
            }
            else if (hasReflectionClassTarget &&
                     isReflectionConstructorLookup(sem.owner, sem.name, sem.desc)) {
                MethodReference reflectionRef;
                reflectionRef.site   = site;
                reflectionRef.target = MethodRefKey{iv(reflectionClassTarget),
                                                    aliasInterned("<init>"), {}};
                reflectionRef.reflectionArgDesc =
                    reflectionArgumentDescriptor(code, cp, pos);
                reflectionRef.kind = MethodReferenceKind::Reflection;
                appendMethodReference(reflectionRef);
                hasReflectionClassTarget = false;
                hasReflectionMemberName  = false;
            }
            else if (hasReflectionClassTarget && hasReflectionMemberName &&
                     isReflectionFieldLookup(sem.owner, sem.name, sem.desc)) {
                FieldReference reflectionRef;
                reflectionRef.site   = site;
                reflectionRef.target = FieldRefKey{iv(reflectionClassTarget),
                                                   iv(reflectionMemberName), {}};
                reflectionRef.kind   = FieldReferenceKind::Reflection;
                appendFieldReference(reflectionRef);
                hasReflectionClassTarget = false;
                hasReflectionMemberName  = false;
            }

            // 成员 owner 同时是类/类型使用：class 可达性的直接编码事实。
            TypeReference typeRef;
            typeRef.site       = site;
            typeRef.targetType = ref.target.owner;
            typeRef.kind       = TypeReferenceKind::MethodOwner;
            appendTypeReference(typeRef, true);
            appendMemberDescriptorTypes(sem.desc, TypeReferenceKind::MethodDescriptor, site);
        }
        else if (isFieldAccessFamily(op)) {
            jbc::ir::ClassSession::MemberSem sem;
            if (!session.memberOf(*in, sem)) continue;
            const ReferenceSite site = makeSite(pos);
            FieldReference      ref;
            ref.site   = site;
            ref.target = FieldRefKey{iv(sem.owner), iv(sem.name), iv(sem.desc)};
            appendFieldReference(ref);

            TypeReference typeRef;
            typeRef.site       = site;
            typeRef.targetType = ref.target.owner;
            typeRef.kind       = TypeReferenceKind::FieldOwner;
            appendTypeReference(typeRef, true);
            appendMemberDescriptorTypes(sem.desc, TypeReferenceKind::FieldDescriptor, site);
        }
        else if (isTypeInstruction(op)) {
            const ReferenceSite site = makeSite(pos);
            TypeReference       ref;
            ref.site       = site;
            ref.targetType = iv(session.classOf(*in));
            ref.kind       = TypeReferenceKind::TypeInstruction;
            appendTypeReference(ref, true);
        }
        else if (op == Op::MULTIANEWARRAY) {
            const ReferenceSite site = makeSite(pos);
            TypeReference       ref;
            ref.site       = site;
            ref.targetType = iv(resolveClassName(cp, in->cp));   // 数组描述符原样
            ref.kind       = TypeReferenceKind::MultiANewArray;
            appendTypeReference(ref, true);
            appendMemberDescriptorTypes(resolveClassName(cp, in->cp),
                                        TypeReferenceKind::MultiANewArray, site);
        }
        else if (op == Op::LDC || op == Op::LDC_W) {
            hasLastStringConstant = false;
            ConstSem csem;
            if (!session.constOf(*in, csem)) continue;
            if (csem.kind == ConstSem::Kind::String) {
                lastStringConstant      = iv(csem.str);
                reflectionMemberName    = lastStringConstant;
                hasReflectionMemberName = true;
                stringLdc               = true;
                // String LDC 的 continue 跳过循环尾的状态回写，此处直接落地。
                hasLastStringConstant   = true;
                continue;
            }
            if (csem.kind == ConstSem::Kind::MethodHandle) {
                HandleSem handle;
                if (jbc::core::resolveHandleCp(cp, in->cp, handle)) {
                    appendHandleReference(handle, makeSite(pos),
                                          MethodReferenceKind::MethodHandle);
                }
                continue;
            }
            if (csem.kind == ConstSem::Kind::Class) {
                const ReferenceSite site = makeSite(pos);
                TypeReference       ref;
                ref.site       = site;
                ref.targetType = iv(csem.str);
                ref.kind       = TypeReferenceKind::ClassConstant;
                appendTypeReference(ref, true);
                // 类常量作为潜在反射接收者：值已是 Class[] 元素（下一指令
                // AASTORE）或本方法已持有成员名时不追踪。
                const jbc::ir::Insn* next =
                    pos + 1 < order.size() ? code.dom.peek(order[pos + 1]) : nullptr;
                const bool classArrayStoreValue = next != nullptr && next->op == Op::AASTORE;
                if (!hasReflectionMemberName && !classArrayStoreValue) {
                    reflectionClassTarget    = ref.targetType;
                    hasReflectionClassTarget = true;
                }
            }
        }
        else if (op == Op::INVOKEDYNAMIC) {
            const ReferenceSite site = makeSite(pos);
            // indy 的 bsm 不在指令上：CP InvokeDynamic.a → BootstrapMethods 表项。
            const jbc::core::AttrSlot* bsmSlot =
                findAttrByName(session.backing().attrs, cp, "BootstrapMethods");
            // BootstrapMethods 是 jbc 建模属性；Raw 兜底（畸形）时无表可查，
            // 按无 bsm 处理（保守：只丢 indy 事实，不影响其他事实）。
            const jbc::core::AttrBody* bsmBody =
                bsmSlot != nullptr ? decodeAttrBody(session.backing(), *bsmSlot) : nullptr;
            const auto* modeled =
                bsmBody != nullptr
                    ? ca::core::tag_cast::dyn_cast<jbc::core::BootstrapMethodsAttr>(bsmBody)
                    : nullptr;
            HandleSem             bsmHandle;
            MethodReferenceKind   methodKind = MethodReferenceKind::MethodHandle;
            std::vector<ca::u32>  bsmArgs;
            if (modeled != nullptr && cp.valid(in->cp)) {
                const auto& indy = cp.at(in->cp);
                if (indy.tag == CpTag::InvokeDynamic && indy.a < modeled->entries.size()) {
                    const auto& entry = modeled->entries[indy.a];
                    if (jbc::core::resolveHandleCp(cp, entry.methodRef, bsmHandle)) {
                        appendHandleReference(bsmHandle, site, MethodReferenceKind::MethodHandle);
                        methodKind = isLambdaMetafactoryHandle(bsmHandle)
                                         ? MethodReferenceKind::Lambda
                                         : MethodReferenceKind::MethodHandle;
                    }
                    bsmArgs = entry.args;
                }
            }
            for (const ca::u32 argCp : bsmArgs) {
                if (!cp.valid(argCp)) continue;
                const auto& entry = cp.at(argCp);
                if (entry.tag == CpTag::MethodHandle) {
                    HandleSem handle;
                    if (jbc::core::resolveHandleCp(cp, argCp, handle)) {
                        appendHandleReference(handle, site, methodKind);
                    }
                }
                else if (entry.tag == CpTag::Class) {
                    TypeReference ref;
                    ref.site       = site;
                    ref.targetType = iv(cp.utf8Of(entry.a));
                    ref.kind       = TypeReferenceKind::ClassConstant;
                    appendTypeReference(ref, true);
                }
            }
        }
        // 其余指令：字符串状态复位。
        hasLastStringConstant = stringLdc;
    }

    // ── 反射数据流事实（域引擎快照 + fail-open）──
    ReflectionQueryContext ctx;
    ctx.code     = &code;
    ctx.cp       = &cp;
    ctx.owner    = className;
    ctx.name     = methodName;
    ctx.desc     = methodDesc;
    ctx.isStatic = (method.access & mjbase::hierarchy::kAccStatic) != 0;
    ctx.aliases  = aliases;
    const ReflectionSnapshot snapshot = snapshotReflectionFacts(ctx);
    if (!snapshot.ok) {
        return;   // fail-open（JSR/RET、畸形控制流）：保留 direct/lookup 事实，
                  // 不追加 reflection 数据流事实。
    }
    for (ca::usize pos = 0; pos < order.size(); ++pos) {
        const jbc::ir::Insn* in = code.dom.peek(order[pos]);
        if (in == nullptr || !isInvokeFamily(in->op)) continue;
        const auto receiverIt = snapshot.receivers.find(static_cast<ca::u32>(pos));
        if (receiverIt == snapshot.receivers.end()) continue;   // 不可达/栈深不足
        MemberRefSem ref;
        if (!resolveMemberRef(cp, in->cp, ref)) continue;

        ReflectionValue receiver = receiverIt->second;
        if (aliases != nullptr) {
            const auto* alias = aliases->reflectionReceiverOf(
                className, methodName, methodDesc, static_cast<ca::i32>(pos));
            if (alias != nullptr && alias->isKnown() &&
                alias->identity.substr(0, 6) == "class:") {
                receiver.kind  = ReflectionValue::Kind::Class;
                receiver.owner = aliasInterned(alias->identity.substr(6));
            }
        }
        ReflectionExecutionTarget target;
        if (!resolveReflectionExecution(in->op, ref.owner, ref.name, ref.desc, receiver, target)) {
            continue;
        }

        const ReferenceSite site = makeSite(pos);
        if (target.kind == ReflectionExecutionTarget::Kind::Field) {
            FieldReference reflectionRef;
            reflectionRef.site   = site;
            reflectionRef.target = FieldRefKey{iv(target.owner), iv(target.name), iv(target.desc)};
            reflectionRef.kind   = FieldReferenceKind::ReflectionAccess;
            appendFieldReference(reflectionRef);
        }
        else {
            MethodReference reflectionRef;
            reflectionRef.site   = site;
            reflectionRef.target = MethodRefKey{iv(target.owner), iv(target.name), iv(target.desc)};
            reflectionRef.reflectionArgDesc = iv(target.reflectionArgDesc);
            reflectionRef.kind              = MethodReferenceKind::ReflectionExecution;
            appendMethodReference(reflectionRef);
        }
    }
}

void ReferenceIndex::appendMethodReference(const MethodReference& ref)
{
    methodRefs_.push_back(ref);
    methodRefsByTarget_[toMemberKey(pool_, ref.target.owner, ref.target.name, ref.target.desc)]
        .push_back(ref);
    methodRefsByTargetOwner_[ref.target.owner].push_back(ref);
    methodRefsBySource_[toMemberKey(pool_, ref.site.sourceClass, ref.site.sourceMethod,
                                    ref.site.sourceDesc)]
        .push_back(ref);
    methodRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
}

void ReferenceIndex::appendFieldReference(const FieldReference& ref)
{
    fieldRefs_.push_back(ref);
    fieldRefsByTarget_[toMemberKey(pool_, ref.target.owner, ref.target.name, ref.target.desc)]
        .push_back(ref);
    fieldRefsByTargetOwner_[ref.target.owner].push_back(ref);
    fieldRefsBySource_[toMemberKey(pool_, ref.site.sourceClass, ref.site.sourceMethod,
                                   ref.site.sourceDesc)]
        .push_back(ref);
    fieldRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
}

void ReferenceIndex::appendTypeReference(const TypeReference& ref, bool methodScoped)
{
    typeRefs_.push_back(ref);
    typeRefsByTarget_[ref.targetType].push_back(ref);
    if (methodScoped) {
        typeRefsBySource_[toMemberKey(pool_, ref.site.sourceClass, ref.site.sourceMethod,
                                      ref.site.sourceDesc)]
            .push_back(ref);
    }
    typeRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
}

void ReferenceIndex::rebuildReferenceLookups()
{
    methodRefsByTarget_.clear();
    methodRefsByTargetOwner_.clear();
    methodRefsBySource_.clear();
    methodRefsBySourceClass_.clear();
    for (const auto& ref : methodRefs_) {
        methodRefsByTarget_[toMemberKey(pool_, ref.target.owner, ref.target.name, ref.target.desc)]
            .push_back(ref);
        methodRefsByTargetOwner_[ref.target.owner].push_back(ref);
        methodRefsBySource_[toMemberKey(pool_, ref.site.sourceClass, ref.site.sourceMethod,
                                        ref.site.sourceDesc)]
            .push_back(ref);
        methodRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }

    fieldRefsByTarget_.clear();
    fieldRefsByTargetOwner_.clear();
    fieldRefsBySource_.clear();
    fieldRefsBySourceClass_.clear();
    for (const auto& ref : fieldRefs_) {
        fieldRefsByTarget_[toMemberKey(pool_, ref.target.owner, ref.target.name, ref.target.desc)]
            .push_back(ref);
        fieldRefsByTargetOwner_[ref.target.owner].push_back(ref);
        fieldRefsBySource_[toMemberKey(pool_, ref.site.sourceClass, ref.site.sourceMethod,
                                       ref.site.sourceDesc)]
            .push_back(ref);
        fieldRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }
}

void ReferenceIndex::restore(std::vector<ClassDeclaration>  classDecls,
                             std::vector<MethodDeclaration> methodDecls,
                             std::vector<FieldDeclaration>  fieldDecls,
                             std::vector<MethodReference>   methodRefs,
                             std::vector<FieldReference>    fieldRefs,
                             std::vector<TypeReference>     typeRefs)
{
    clear();
    // 视图重驻留：缓存读出的文本来自读端缓冲，restore 全量 intern 进索引池并
    // 改写视图（契约见头注——restore 调用期内读端数据必须存活）。
    auto iv = [&](std::string_view text) { return pool_.str(pool_.intern(text)); };

    classDecls_ = std::move(classDecls);
    methodDecls_ = std::move(methodDecls);
    fieldDecls_ = std::move(fieldDecls);
    methodRefs_ = std::move(methodRefs);
    fieldRefs_ = std::move(fieldRefs);
    typeRefs_ = std::move(typeRefs);

    for (auto& decl : classDecls_) {
        decl.name = iv(decl.name);
        decl.superName = iv(decl.superName);
        for (auto& iface : decl.interfaces) iface = iv(iface);
    }
    for (auto& decl : methodDecls_) {
        decl.method.owner = iv(decl.method.owner);
        decl.method.name = iv(decl.method.name);
        decl.method.desc = iv(decl.method.desc);
    }
    for (auto& decl : fieldDecls_) {
        decl.field.owner = iv(decl.field.owner);
        decl.field.name = iv(decl.field.name);
        decl.field.desc = iv(decl.field.desc);
    }
    for (auto& ref : methodRefs_) {
        ref.site.sourceClass = iv(ref.site.sourceClass);
        ref.site.sourceMethod = iv(ref.site.sourceMethod);
        ref.site.sourceDesc = iv(ref.site.sourceDesc);
        ref.target.owner = iv(ref.target.owner);
        ref.target.name = iv(ref.target.name);
        ref.target.desc = iv(ref.target.desc);
        ref.reflectionArgDesc = iv(ref.reflectionArgDesc);
    }
    for (auto& ref : fieldRefs_) {
        ref.site.sourceClass = iv(ref.site.sourceClass);
        ref.site.sourceMethod = iv(ref.site.sourceMethod);
        ref.site.sourceDesc = iv(ref.site.sourceDesc);
        ref.target.owner = iv(ref.target.owner);
        ref.target.name = iv(ref.target.name);
        ref.target.desc = iv(ref.target.desc);
    }
    for (auto& ref : typeRefs_) {
        ref.site.sourceClass = iv(ref.site.sourceClass);
        ref.site.sourceMethod = iv(ref.site.sourceMethod);
        ref.site.sourceDesc = iv(ref.site.sourceDesc);
        ref.targetType = iv(ref.targetType);
    }

    // 声明派生查询与 indexSession 同构：first-seen wins，ByOwner 保持声明序。
    for (std::size_t i = 0; i < classDecls_.size(); ++i) {
        classDeclsByKey_.emplace(classDecls_[i].name, i);
    }
    for (std::size_t i = 0; i < methodDecls_.size(); ++i) {
        const auto& d = methodDecls_[i];
        methodDeclsByKey_.emplace(
            toMemberKey(pool_, d.method.owner, d.method.name, d.method.desc), i);
        methodDeclsByOwner_[d.method.owner].push_back(d);
    }
    for (std::size_t i = 0; i < fieldDecls_.size(); ++i) {
        const auto& d = fieldDecls_[i];
        fieldDeclsByKey_.emplace(
            toMemberKey(pool_, d.field.owner, d.field.name, d.field.desc), i);
        fieldDeclsByOwner_[d.field.owner].push_back(d);
    }

    // 方法/字段引用查询与 rebuildReferenceLookups 共用；type-ref 查询沿用
    // build 期的类级/方法级二分（site.sourceMethod 空视图 = 类级事实）。
    for (const auto& ref : methodRefs_) {
        methodRefsByTarget_[toMemberKey(pool_, ref.target.owner, ref.target.name, ref.target.desc)]
            .push_back(ref);
        methodRefsByTargetOwner_[ref.target.owner].push_back(ref);
        methodRefsBySource_[toMemberKey(pool_, ref.site.sourceClass, ref.site.sourceMethod,
                                        ref.site.sourceDesc)]
            .push_back(ref);
        methodRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }
    for (const auto& ref : fieldRefs_) {
        fieldRefsByTarget_[toMemberKey(pool_, ref.target.owner, ref.target.name, ref.target.desc)]
            .push_back(ref);
        fieldRefsByTargetOwner_[ref.target.owner].push_back(ref);
        fieldRefsBySource_[toMemberKey(pool_, ref.site.sourceClass, ref.site.sourceMethod,
                                       ref.site.sourceDesc)]
            .push_back(ref);
        fieldRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }
    for (const auto& ref : typeRefs_) {
        typeRefsByTarget_[ref.targetType].push_back(ref);
        const bool methodScoped = !ref.site.sourceMethod.empty();
        if (methodScoped) {
            typeRefsBySource_[toMemberKey(pool_, ref.site.sourceClass, ref.site.sourceMethod,
                                          ref.site.sourceDesc)]
                .push_back(ref);
        }
        typeRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }
}

// ── E2 局部重建（design.md §35.4）：ChangeSet 驱动的成员级补丁 ──

void ReferenceIndex::dropMethodFacts(std::string_view className, std::string_view name,
                                     std::string_view desc)
{
    const auto sameSite = [&](const ReferenceSite& s) {
        return s.sourceClass == className && s.sourceMethod == name && s.sourceDesc == desc;
    };
    methodDecls_.erase(std::remove_if(methodDecls_.begin(), methodDecls_.end(),
                                      [&](const MethodDeclaration& d) {
                                          return d.method.owner == className
                                                 && d.method.name == name
                                                 && d.method.desc == desc;
                                      }),
                       methodDecls_.end());
    methodRefs_.erase(std::remove_if(methodRefs_.begin(), methodRefs_.end(),
                                     [&](const MethodReference& r) {
                                         return sameSite(r.site);
                                     }),
                      methodRefs_.end());
    fieldRefs_.erase(std::remove_if(fieldRefs_.begin(), fieldRefs_.end(),
                                    [&](const FieldReference& r) {
                                        return sameSite(r.site);
                                    }),
                     fieldRefs_.end());
    typeRefs_.erase(std::remove_if(typeRefs_.begin(), typeRefs_.end(),
                                   [&](const TypeReference& r) {
                                       return !r.site.sourceMethod.empty()
                                              && sameSite(r.site);
                                   }),
                    typeRefs_.end());
}

void ReferenceIndex::dropClassScopedFacts(std::string_view className)
{
    classDecls_.erase(std::remove_if(classDecls_.begin(), classDecls_.end(),
                                     [&](const ClassDeclaration& d) {
                                         return d.name == className;
                                     }),
                      classDecls_.end());
    fieldDecls_.erase(std::remove_if(fieldDecls_.begin(), fieldDecls_.end(),
                                     [&](const FieldDeclaration& d) {
                                         return d.field.owner == className;
                                     }),
                      fieldDecls_.end());
    // 类作用域事实 = 方法体之外的全部类型引用（类头元数据 + 字段声明面）。
    typeRefs_.erase(std::remove_if(typeRefs_.begin(), typeRefs_.end(),
                                   [&](const TypeReference& r) {
                                       return r.site.sourceClass == className
                                              && r.site.sourceMethod.empty();
                                   }),
                    typeRefs_.end());
}

void ReferenceIndex::dropAllClassFacts(std::string_view className)
{
    dropClassScopedFacts(className);
    methodDecls_.erase(std::remove_if(methodDecls_.begin(), methodDecls_.end(),
                                      [&](const MethodDeclaration& d) {
                                          return d.method.owner == className;
                                      }),
                       methodDecls_.end());
    methodRefs_.erase(std::remove_if(methodRefs_.begin(), methodRefs_.end(),
                                     [&](const MethodReference& r) {
                                         return r.site.sourceClass == className;
                                     }),
                      methodRefs_.end());
    fieldRefs_.erase(std::remove_if(fieldRefs_.begin(), fieldRefs_.end(),
                                    [&](const FieldReference& r) {
                                        return r.site.sourceClass == className;
                                    }),
                     fieldRefs_.end());
    typeRefs_.erase(std::remove_if(typeRefs_.begin(), typeRefs_.end(),
                                   [&](const TypeReference& r) {
                                       return r.site.sourceClass == className;
                                   }),
                    typeRefs_.end());
}

void ReferenceIndex::rebuildDeclarationLookups()
{
    // 与 build/restore 同构：first-seen wins，ByOwner 保持声明序。
    classDeclsByKey_.clear();
    methodDeclsByKey_.clear();
    fieldDeclsByKey_.clear();
    methodDeclsByOwner_.clear();
    fieldDeclsByOwner_.clear();
    for (std::size_t i = 0; i < classDecls_.size(); ++i) {
        classDeclsByKey_.emplace(classDecls_[i].name, i);
    }
    for (std::size_t i = 0; i < methodDecls_.size(); ++i) {
        const auto& d = methodDecls_[i];
        methodDeclsByKey_.emplace(
            toMemberKey(pool_, d.method.owner, d.method.name, d.method.desc), i);
        methodDeclsByOwner_[d.method.owner].push_back(d);
    }
    for (std::size_t i = 0; i < fieldDecls_.size(); ++i) {
        const auto& d = fieldDecls_[i];
        fieldDeclsByKey_.emplace(
            toMemberKey(pool_, d.field.owner, d.field.name, d.field.desc), i);
        fieldDeclsByOwner_[d.field.owner].push_back(d);
    }
}

void ReferenceIndex::finalizeChangePatch()
{
    rebuildDeclarationLookups();
    rebuildReferenceLookups();
    resolveReflectionMemberDescriptors();
}

void ReferenceIndex::applyChangeSet(std::string_view              className,
                                    jbc::ir::ClassSession&        session,
                                    const jbc::ir::ChangeSet&     changes)
{
    const std::string_view klass = pool_.str(pool_.intern(className));

    // 类级变更（从零建出/类元数据实改）：整类重扫——先按变换前类名摘除全部
    // 旧事实，再按会话当前状态重扫（改名后新名落键）。
    if (changes.classChange() != jbc::ir::ClassChange::None) {
        dropAllClassFacts(klass);
        // first-seen-wins 判定依据声明派生图：摘除后先重建，重扫才不会被
        // 陈旧键挡掉。
        rebuildDeclarationLookups();
        indexSession(session, nullptr);
        finalizeChangePatch();
        return;
    }

    const auto ups  = changes.memberChanges();
    const auto gone = changes.removedMembers();

    bool fieldFaceDirty = false;
    for (const auto& r : gone) {
        if (r.member == jbc::ir::MemberKind::Field) fieldFaceDirty = true;
    }
    for (const auto& u : ups) {
        if (u.member == jbc::ir::MemberKind::Field) fieldFaceDirty = true;
    }

    // 阶段一：全部摘除先行（主表补丁）。
    if (fieldFaceDirty) {
        // 字段粒度：字段元数据产出的类型事实不含字段坐标（类作用域面），按键
        // 无法单独摘除——按类级事实面整体摘除（方法体事实不动）。
        dropClassScopedFacts(klass);
    }
    for (const auto& r : gone) {
        if (r.member == jbc::ir::MemberKind::Method) {
            dropMethodFacts(klass, r.name, r.desc);
        }
    }
    for (const auto& u : ups) {
        if (u.member == jbc::ir::MemberKind::Method) {
            dropMethodFacts(klass, u.name, u.desc);
        }
    }

    // 阶段二：声明派生图与主表对齐（first-seen-wins 的判定前提）。
    rebuildDeclarationLookups();

    // 阶段三：统一重扫变更面。
    if (fieldFaceDirty) {
        const ReferenceSite classSite{klass, {}, {}, -1, -1};
        indexClassHeader(session, klass, classSite);
        for (ca::usize fi = 0; fi < session.unit().fields.size(); ++fi) {
            indexFieldUnit(session, fi, klass, classSite);
        }
    }
    for (const auto& u : ups) {
        if (u.member == jbc::ir::MemberKind::Method) {
            indexOneMethod(session, u.name, u.desc, klass, nullptr);
        }
    }

    // 阶段四：收尾（声明图 + 引用图重建 + 反射描述符补全）。
    finalizeChangePatch();
}

void ReferenceIndex::resolveReflectionMemberDescriptors()
{
    bool changed = false;

    // 实参描述符匹配：声明序内唯一 "前缀 == 实参描述符 且 返回段长度一致"。
    auto methodDescForArgs = [&](std::string_view owner, std::string_view name,
                                 std::string_view argDesc) {
        const auto it = methodDeclsByOwner_.find(owner);
        std::string_view desc;
        bool             found = false;
        if (it == methodDeclsByOwner_.end() || argDesc.empty()) return desc;
        for (const auto& decl : it->second) {
            if (decl.method.name != name) continue;
            const std::string_view candidate = decl.method.desc;
            const auto             close     = candidate.find(')');
            if (close == std::string_view::npos) continue;
            if (argDesc.size() != close + 1 || candidate.substr(0, argDesc.size()) != argDesc) {
                continue;
            }
            if (found) return std::string_view{};
            desc  = candidate;
            found = true;
        }
        return desc;
    };

    auto uniqueMethodDesc = [&](std::string_view owner, std::string_view name) {
        const auto      it = methodDeclsByOwner_.find(owner);
        std::string_view desc;
        bool             found = false;
        if (it == methodDeclsByOwner_.end()) return desc;
        for (const auto& decl : it->second) {
            if (decl.method.name != name) continue;
            if (found) return std::string_view{};
            desc  = decl.method.desc;
            found = true;
        }
        return desc;
    };

    auto uniqueFieldDesc = [&](std::string_view owner, std::string_view name) {
        const auto      it = fieldDeclsByOwner_.find(owner);
        std::string_view desc;
        bool             found = false;
        if (it == fieldDeclsByOwner_.end()) return desc;
        for (const auto& decl : it->second) {
            if (decl.field.name != name) continue;
            if (found) return std::string_view{};
            desc  = decl.field.desc;
            found = true;
        }
        return desc;
    };

    for (auto& ref : methodRefs_) {
        if ((ref.kind != MethodReferenceKind::Reflection &&
             ref.kind != MethodReferenceKind::ReflectionExecution) ||
            !ref.target.desc.empty()) {
            continue;
        }
        std::string_view desc =
            methodDescForArgs(ref.target.owner, ref.target.name, ref.reflectionArgDesc);
        if (desc.empty()) desc = uniqueMethodDesc(ref.target.owner, ref.target.name);
        if (desc.empty()) continue;
        ref.target.desc = desc;
        changed         = true;
    }

    for (auto& ref : fieldRefs_) {
        if ((ref.kind != FieldReferenceKind::Reflection &&
             ref.kind != FieldReferenceKind::ReflectionAccess) ||
            !ref.target.desc.empty()) {
            continue;
        }
        const std::string_view desc = uniqueFieldDesc(ref.target.owner, ref.target.name);
        if (desc.empty()) continue;
        ref.target.desc = desc;
        changed         = true;
    }

    if (changed) rebuildReferenceLookups();
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesTo(MemberKey key) const
{
    const auto it = methodRefsByTarget_.find(key);
    return it == methodRefsByTarget_.end() ? kEmptyMethodRefs : it->second;
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesTo(
    std::string_view owner, std::string_view name, std::string_view desc) const
{
    return methodReferencesTo(toMemberKey(pool_, owner, name, desc));
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesTo(MemberKey key) const
{
    const auto it = fieldRefsByTarget_.find(key);
    return it == fieldRefsByTarget_.end() ? kEmptyFieldRefs : it->second;
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesTo(
    std::string_view owner, std::string_view name, std::string_view desc) const
{
    return fieldReferencesTo(toMemberKey(pool_, owner, name, desc));
}

const std::vector<TypeReference>& ReferenceIndex::typeReferencesTo(
    std::string_view targetType) const
{
    const auto it = typeRefsByTarget_.find(targetType);
    return it == typeRefsByTarget_.end() ? kEmptyTypeRefs : it->second;
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesFrom(MemberKey source) const
{
    const auto it = methodRefsBySource_.find(source);
    return it == methodRefsBySource_.end() ? kEmptyMethodRefs : it->second;
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesFrom(
    std::string_view owner, std::string_view name, std::string_view desc) const
{
    return methodReferencesFrom(toMemberKey(pool_, owner, name, desc));
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesFrom(MemberKey source) const
{
    const auto it = fieldRefsBySource_.find(source);
    return it == fieldRefsBySource_.end() ? kEmptyFieldRefs : it->second;
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesFrom(
    std::string_view owner, std::string_view name, std::string_view desc) const
{
    return fieldReferencesFrom(toMemberKey(pool_, owner, name, desc));
}

const std::vector<TypeReference>& ReferenceIndex::typeReferencesFrom(MemberKey source) const
{
    const auto it = typeRefsBySource_.find(source);
    return it == typeRefsBySource_.end() ? kEmptyTypeRefs : it->second;
}

const std::vector<TypeReference>& ReferenceIndex::typeReferencesFrom(
    std::string_view owner, std::string_view name, std::string_view desc) const
{
    return typeReferencesFrom(toMemberKey(pool_, owner, name, desc));
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesFromClass(
    std::string_view owner) const
{
    const auto it = methodRefsBySourceClass_.find(owner);
    return it == methodRefsBySourceClass_.end() ? kEmptyMethodRefs : it->second;
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesFromClass(
    std::string_view owner) const
{
    const auto it = fieldRefsBySourceClass_.find(owner);
    return it == fieldRefsBySourceClass_.end() ? kEmptyFieldRefs : it->second;
}

const std::vector<TypeReference>& ReferenceIndex::typeReferencesFromClass(
    std::string_view owner) const
{
    const auto it = typeRefsBySourceClass_.find(owner);
    return it == typeRefsBySourceClass_.end() ? kEmptyTypeRefs : it->second;
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesToOwner(
    std::string_view owner) const
{
    const auto it = methodRefsByTargetOwner_.find(owner);
    return it == methodRefsByTargetOwner_.end() ? kEmptyMethodRefs : it->second;
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesToOwner(
    std::string_view owner) const
{
    const auto it = fieldRefsByTargetOwner_.find(owner);
    return it == fieldRefsByTargetOwner_.end() ? kEmptyFieldRefs : it->second;
}

const MethodDeclaration* ReferenceIndex::declaredMethod(MemberKey key) const
{
    const auto it = methodDeclsByKey_.find(key);
    return it == methodDeclsByKey_.end() ? nullptr : &methodDecls_[it->second];
}

const MethodDeclaration* ReferenceIndex::declaredMethod(std::string_view owner,
                                                        std::string_view name,
                                                        std::string_view desc) const
{
    return declaredMethod(toMemberKey(pool_, owner, name, desc));
}

const FieldDeclaration* ReferenceIndex::declaredField(MemberKey key) const
{
    const auto it = fieldDeclsByKey_.find(key);
    return it == fieldDeclsByKey_.end() ? nullptr : &fieldDecls_[it->second];
}

const FieldDeclaration* ReferenceIndex::declaredField(std::string_view owner,
                                                      std::string_view name,
                                                      std::string_view desc) const
{
    return declaredField(toMemberKey(pool_, owner, name, desc));
}

const std::vector<MethodDeclaration>& ReferenceIndex::declaredMethodsIn(
    std::string_view owner) const
{
    const auto it = methodDeclsByOwner_.find(owner);
    return it == methodDeclsByOwner_.end() ? kEmptyMethodDecls : it->second;
}

const std::vector<FieldDeclaration>& ReferenceIndex::declaredFieldsIn(
    std::string_view owner) const
{
    const auto it = fieldDeclsByOwner_.find(owner);
    return it == fieldDeclsByOwner_.end() ? kEmptyFieldDecls : it->second;
}

const ClassDeclaration* ReferenceIndex::declaredClass(std::string_view owner) const
{
    const auto it = classDeclsByKey_.find(owner);
    return it == classDeclsByKey_.end() ? nullptr : &classDecls_[it->second];
}

}   // namespace mjbase::reference
