#include "mjbase/pass/meta_info_remover.hpp"

#include <algorithm>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/bytes.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/type/signature.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjbase::pass {

namespace {

using jbc::core::AnnotationsAttr;
using jbc::core::AttrKind;
using jbc::core::AttrSlot;
using jbc::core::ByteSink;
using jbc::core::ByteView;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::Cursor;
using jbc::core::MemberInfo;
using jbc::core::MethodParametersAttr;
using jbc::core::ParameterAnnotationsAttr;
using jbc::core::RawAttr;
using jbc::core::RecordAttr;
using jbc::core::TypeAnnotationsAttr;

// ── JVM 访问标志位（JVMS 表；libjbc 未提供旗标常量，与 unused_member_remover
//    同款本地常量纪律；u16 载体位，RECORD 0x10000 超 u16 恒零不列）。──
constexpr ca::u16 kAccPublic       = 0x0001;
constexpr ca::u16 kAccPrivate      = 0x0002;
constexpr ca::u16 kAccProtected    = 0x0004;
constexpr ca::u16 kAccStatic       = 0x0008;
constexpr ca::u16 kAccFinal        = 0x0010;
constexpr ca::u16 kAccSuper        = 0x0020;
constexpr ca::u16 kAccVolatile     = 0x0040;
constexpr ca::u16 kAccBridge       = 0x0040;
constexpr ca::u16 kAccVarargs      = 0x0080;
constexpr ca::u16 kAccNative       = 0x0100;
constexpr ca::u16 kAccInterface    = 0x0200;
constexpr ca::u16 kAccAbstract     = 0x0400;
constexpr ca::u16 kAccStrict       = 0x0800;
constexpr ca::u16 kAccSynthetic    = 0x1000;
constexpr ca::u16 kAccAnnotation   = 0x2000;
constexpr ca::u16 kAccEnum         = 0x4000;
constexpr ca::u16 kAccModule       = 0x8000;

// ── 属性名常量（JVMS 4.7；属性槽按名比较，CP 只追加下索引比较不可靠）。
//    sourceDebug 在 classfile 的序列化名是 SourceDebugExtension（4.7.15）。──
constexpr std::string_view kSourceFile                = "SourceFile";
constexpr std::string_view kSourceDebug               = "SourceDebugExtension";
constexpr std::string_view kSignature                 = "Signature";
constexpr std::string_view kCode                      = "Code";
constexpr std::string_view kLineNumberTable           = "LineNumberTable";
constexpr std::string_view kLocalVariableTable        = "LocalVariableTable";
constexpr std::string_view kLocalVariableTypeTable    = "LocalVariableTypeTable";
constexpr std::string_view kMethodParameters          = "MethodParameters";
constexpr std::string_view kAnnotationDefault         = "AnnotationDefault";
constexpr std::string_view kRva                       = "RuntimeVisibleAnnotations";
constexpr std::string_view kRia                       = "RuntimeInvisibleAnnotations";
constexpr std::string_view kRvta                      = "RuntimeVisibleTypeAnnotations";
constexpr std::string_view kRita                      = "RuntimeInvisibleTypeAnnotations";
constexpr std::string_view kRvpa                      = "RuntimeVisibleParameterAnnotations";
constexpr std::string_view kRipa                      = "RuntimeInvisibleParameterAnnotations";
constexpr std::string_view kRecord                    = "Record";

/// 槽名是否等于给定名（逐字节，零分配）。
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

/// 摘除属性表中所有命中名单的槽；返回是否发生摘除。
bool removeNamedSlots(ClassFile&                                   cf,
                      std::vector<AttrSlot>&                       attrs,
                      std::initializer_list<std::string_view>      names)
{
    bool changed = false;
    for (std::size_t i = attrs.size(); i-- > 0;) {
        for (const std::string_view name : names) {
            if (slotNameIs(cf, attrs[i], name)) {
                attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                break;
            }
        }
    }
    return changed;
}

/// Utf8 字节视图的码点数（延续字节不计；与 ca::str::Utf8StringRef::length()
/// 同口径——旧栈 desc.length() 判定按码点）。
ca::usize codepointLength(std::string_view s)
{
    ca::usize count = 0;
    for (const char c : s) {
        if ((static_cast<ca::u8>(c) & 0xC0) != 0x80) ++count;
    }
    return count;
}

/// 注解描述符形状判定（旧栈 isMalformedAnnotationDesc 逐条对齐）：空 / 码点
/// 长度 ≤ 3 / 不以 'L' 开头 / 不以 ';' 结尾 / 含换行、NUL、空格。
bool isMalformedAnnotationDesc(std::string_view desc)
{
    if (desc.empty()) return true;
    if (codepointLength(desc) <= 3) return true;
    if (desc.front() != 'L') return true;
    if (desc.back() != ';') return true;
    if (desc.find('\n') != std::string_view::npos) return true;
    if (desc.find('\0') != std::string_view::npos) return true;
    if (desc.find(' ') != std::string_view::npos) return true;
    return false;
}

/// 旧栈 SpecificAnnotationRemover 黑名单描述符封闭集（原表原样，含重复条目
/// ——集合语义，重复不影响判定）。
constexpr std::string_view kAnnotationBlacklist[] = {
    "Ltech/origincloud/OriginShieldMetadata;",
    "Ljvavx/annotation/Nullable;",
    "Ljvavx/annotation/NotNull;",
    "Ltech/origincloud/obf/ObfuscationSourceDebug;",
    "Ltech/origincloud/Mapping;",
    "Ltech/origincloud/obf/OriginShield;",
    "Ltech/skidonk/obfuscator/annotations/NativeObfuscation$Inline;",
    "Ltech/origincloud/obf/ObfuscationMetadata;",
    "Ljvavx/annotation/NotNull;",
    "Ltech/skidonion/obfuscator/annotations/NativeObfuscation;",
    "Ljvavx/annotation/WhoCaresNull;",
    "Lorg/jetbrains/annotations/Nullable;",
    "Lorg/jetbrains/annotations/NotNull;",
    "Ltech/origincloud/obf/Mapping;",
    "Lorg/jetbrains/annotations/NullableOrNotNull;",
    "Ljvavx/annotation/NullableOrNotNull;",
    "Lorg/jetbrains/annotations/WhoCaresNull;",
};

bool isInBlacklist(std::string_view desc)
{
    return std::find(std::begin(kAnnotationBlacklist), std::end(kAnnotationBlacklist), desc)
           != std::end(kAnnotationBlacklist);
}

bool isKotlinMetadataDesc(std::string_view desc)
{
    return desc == "Lkotlin/Metadata;";
}

bool isJetbrainsAnnotationDesc(std::string_view desc)
{
    return desc == "Lorg/jetbrains/annotations/NotNull;"
           || desc == "Lorg/jetbrains/annotations/JvmName;"
           || desc == "Lorg/jetbrains/annotations/Nullable;";
}

/// 判定是否要移除某个槽内注解条目（谓词组合的公共形态）。
enum class AnnoFilter : ca::u8
{
    All,          ///< 全部移除（MetaInfoRemover 面）
    Malformed,    ///< 形状非法（IllegalAnnotationRemover 面）
    Blacklisted,  ///< 形状非法 ∪ 黑名单（SpecificAnnotationRemover 面）
    Kotlin,       ///< @kotlin/Metadata（KotlinMetadataRemover 类级面）
    Jetbrains,    ///< jetbrains 三注解（KotlinMetadataRemover 成员面）
};

bool annoFilterMatches(AnnoFilter filter, std::string_view desc)
{
    switch (filter) {
    case AnnoFilter::All: return true;
    case AnnoFilter::Malformed: return isMalformedAnnotationDesc(desc);
    case AnnoFilter::Blacklisted:
        return isMalformedAnnotationDesc(desc) || isInBlacklist(desc);
    case AnnoFilter::Kotlin: return isKotlinMetadataDesc(desc);
    case AnnoFilter::Jetbrains: return isJetbrainsAnnotationDesc(desc);
    }
    return false;
}

/// 对属性表中名单内槽位做槽内过滤（默认 Runtime{Visible,Invisible}Annotations
/// 两槽）：命中谓词的条目删除（槽置脏），删空连槽摘除。解码失败/类型不符保守
/// 跳过。名单收窄用于 KotlinMetadataRemover 的类级「只查 visible 槽」面。
/// 返回是否发生任何变化。
bool filterAnnotationSlots(ClassFile&                              cf,
                           std::vector<AttrSlot>&                  attrs,
                           const AnnoFilter                        filter,
                           std::initializer_list<std::string_view> slotNames = {kRva, kRia})
{
    bool changed = false;
    for (std::size_t i = attrs.size(); i-- > 0;) {
        AttrSlot& slot = attrs[i];
        const bool target = std::any_of(slotNames.begin(), slotNames.end(),
                                        [&](const std::string_view name) {
                                            return slotNameIs(cf, slot, name);
                                        });
        if (!target) continue;
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()) continue;
        if (std::move(decoded).unwrap()->kind() != AttrKind::Annotations) continue;
        const auto* body = ca::core::tag_cast::cast<const AnnotationsAttr*>(slot.peek());
        const bool any = std::any_of(
            body->annos.begin(), body->annos.end(),
            [&](const AnnotationsAttr::Annotation& a) {
                return annoFilterMatches(filter, cf.utf8(a.typeCp));
            });
        if (!any) continue;
        auto& annos = ca::core::tag_cast::cast<AnnotationsAttr*>(slot.mutate())->annos;
        annos.erase(std::remove_if(annos.begin(), annos.end(),
                                   [&](const AnnotationsAttr::Annotation& a) {
                                       return annoFilterMatches(filter, cf.utf8(a.typeCp));
                                   }),
                    annos.end());
        if (annos.empty()) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
        }
        changed = true;
    }
    return changed;
}

/// 对属性表中的 Runtime{Visible,Invisible}TypeAnnotations 槽做槽内过滤：
/// 命中谓词的 type annotation 条目删除（槽置脏），删空连槽摘除。解码失败/
/// 类型不符保守跳过。返回是否发生任何变化。
bool filterTypeAnnotationSlots(ClassFile&             cf,
                               std::vector<AttrSlot>& attrs,
                               const AnnoFilter       filter)
{
    bool changed = false;
    for (std::size_t i = attrs.size(); i-- > 0;) {
        AttrSlot& slot = attrs[i];
        if (!slotNameIs(cf, slot, kRvta) && !slotNameIs(cf, slot, kRita)) continue;
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()) continue;
        if (std::move(decoded).unwrap()->kind() != AttrKind::TypeAnnotations) continue;
        const auto* body = ca::core::tag_cast::cast<const TypeAnnotationsAttr*>(slot.peek());
        const bool any = std::any_of(
            body->annotations.begin(), body->annotations.end(),
            [&](const TypeAnnotationsAttr::Annotation& a) {
                return annoFilterMatches(filter, cf.utf8(a.typeIndexCp));
            });
        if (!any) continue;
        auto& annos = ca::core::tag_cast::cast<TypeAnnotationsAttr*>(slot.mutate())->annotations;
        annos.erase(std::remove_if(annos.begin(), annos.end(),
                                   [&](const TypeAnnotationsAttr::Annotation& a) {
                                       return annoFilterMatches(filter, cf.utf8(a.typeIndexCp));
                                   }),
                    annos.end());
        if (annos.empty()) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
        }
        changed = true;
    }
    return changed;
}

/// 方法 Code 嵌套属性表里的 Runtime{Visible,Invisible}TypeAnnotations 槽内
/// 过滤：code 目标形态（cast/instanceof/new/localvar 等）在旧栈树模型里平铺
/// 在方法向量上，与成员级同面参与过滤。改动落盘需父 Code 槽连带置脏
/// （pass-contract §1：嵌套属性改动对 writeClass 不可见，除非 Code 槽本身
/// 标脏重编码）。返回是否发生任何变化。
bool filterCodeNestedTypeAnnotationSlots(ClassFile& cf, MemberInfo& m, const AnnoFilter filter)
{
    AttrSlot* codeSlot = findSlot(cf, m.attrs, kCode);
    if (codeSlot == nullptr) return false;
    auto decoded = cf.decodeCode(m);
    if (decoded.is_err()) return false;
    CodeAttr* code = decoded.unwrap();
    const bool changed = filterTypeAnnotationSlots(cf, code->attrs, filter);
    if (changed) {
        (void)codeSlot->mutate();
    }
    return changed;
}

/// 对成员做 MetaInfoRemover 的 Code 嵌套 meta 信息整槽摘除：LNT/LVT/LVTT
/// （旧面 clearLineNumbers + localVariables 清空：行号与局部变量表全表消失，
/// 全表删除无 pc 悬垂面）与 Runtime{Visible,Invisible}TypeAnnotations（旧面
/// clearMethodMeta 清空 localVariable 注解与指令级 type 注解向量——code 目标
/// 形态只在 Code 槽出现，整槽摘除即等价）。指令载荷/异常表/SMT/maxes 不触碰，
/// Code 槽置脏驱动重编码。无 Code/解码失败/无可摘槽时保持原样。
bool removeCodeNestedDebugInfo(ClassFile& cf, MemberInfo& m)
{
    AttrSlot* codeSlot = findSlot(cf, m.attrs, kCode);
    if (codeSlot == nullptr) return false;
    auto decoded = cf.decodeCode(m);
    if (decoded.is_err()) return false;
    CodeAttr* code = decoded.unwrap();
    bool      changed = false;
    for (std::size_t i = code->attrs.size(); i-- > 0;) {
        const AttrSlot& nested = code->attrs[i];
        const bool hit = slotNameIs(cf, nested, kLineNumberTable)
                         || slotNameIs(cf, nested, kLocalVariableTable)
                         || slotNameIs(cf, nested, kLocalVariableTypeTable)
                         || slotNameIs(cf, nested, kRvta)
                         || slotNameIs(cf, nested, kRita);
        if (hit) {
            code->attrs.erase(code->attrs.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
        }
    }
    if (changed) {
        (void)codeSlot->mutate();
    }
    return changed;
}

/// Record 属性（typed RecordAttr，JVMS 4.7.30）组件嵌套注解槽清空：每个组件
/// 的 Runtime{Visible,Invisible}Annotations 与 Runtime{Visible,Invisible}-
/// TypeAnnotations 槽按与成员级同规则做槽内过滤（filter = All 时条目全删、
/// 删空连槽摘除）；组件 Signature 与未知属性不触碰。typed 体经公开解码入口
/// 读出（remapper 域 Record 手术同款：命中才 makeNew 置 New 走重编码，写回
/// 侧组件嵌套置脏与 Code 嵌套同一闸门，未触碰组件嵌套槽照旧 copy-through）。
/// 无 Record 槽/解码失败/类型不符保守保持原样。返回是否发生任何变化。
bool clearRecordComponentAnnotationSlots(ClassFile& cf)
{
    AttrSlot* recordSlot = findSlot(cf, cf.attrs, kRecord);
    if (recordSlot == nullptr) return false;
    auto d = decodeRecordBody(cf, *recordSlot);
    if (d.is_err()) return false;
    auto  body = std::move(d).unwrap();
    auto& rec  = static_cast<RecordAttr&>(*body);
    bool  changed = false;
    for (RecordAttr::Component& component : rec.components) {
        changed |= filterAnnotationSlots(cf, component.attrs, AnnoFilter::All);
        changed |= filterTypeAnnotationSlots(cf, component.attrs, AnnoFilter::All);
    }
    if (!changed) return false;
    *recordSlot = AttrSlot::makeNew(recordSlot->nameCp, std::move(body));
    return true;
}

/// SourceFile 槽载荷值（u2 CP 索引 → Utf8；载荷不足/索引无效为空视图）。
std::string_view sourceFileValue(const ClassFile& cf, const AttrSlot& slot)
{
    const auto view = cf.source().spanOrEmpty(slot.data);
    if (view.size() < 2) return {};
    const ca::u32 idx = (static_cast<ca::u32>(view[0]) << 8) | static_cast<ca::u32>(view[1]);
    return cf.utf8(idx);
}

/// SourceDebugExtension 槽载荷值（内联 Utf8 字节，非 CP 间接；旧栈
/// sourceDebug 的序列化名即 JVMS 4.7.15 的 SourceDebugExtension）。
std::string_view sourceDebugValue(const ClassFile& cf, const AttrSlot& slot)
{
    const auto view = cf.source().spanOrEmpty(slot.data);
    return {reinterpret_cast<const char*>(view.data()), view.size()};
}

/// 非法 debug 信息判定（旧栈 isIllegalDebugInfo 逐条对齐）：空合法；码点
/// 长度 > 100 / 不以 ".java" 结尾 / 基名空 / 基名首字节数字 / 基名含非 ASCII
/// 字母数字字节 → 非法。
bool isIllegalDebugInfo(std::string_view info)
{
    if (info.empty()) return false;
    if (codepointLength(info) > 100) return true;
    constexpr std::string_view suffix = ".java";
    if (info.size() < suffix.size()) return true;
    if (info.substr(info.size() - suffix.size()) != suffix) return true;
    const std::size_t baseLen = info.size() - suffix.size();
    if (baseLen == 0) return true;
    const auto isAlnum = [](ca::u8 c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (!isAlnum(static_cast<ca::u8>(info[0])) || (info[0] >= '0' && info[0] <= '9')) {
        return true;
    }
    for (std::size_t i = 0; i < baseLen; ++i) {
        if (!isAlnum(static_cast<ca::u8>(info[i]))) return true;
    }
    return false;
}

/// Signature 槽载荷是否为合法签名（与旧栈 ASM SignatureReader 分道对齐：
/// 类/方法用类/方法签名文法，字段用类型签名文法）；空载荷合法（旧栈空串
/// 保持）。解码失败按合法处理（fail-closed 保守，不动输入）。
bool signatureSlotIsValid(ClassFile& cf, AttrSlot& slot, jbc::type::SignatureForm form)
{
    auto decoded = cf.decodeAttr(slot);
    if (decoded.is_err()) return true;
    if (std::move(decoded).unwrap()->kind() != AttrKind::Signature) return true;
    const auto* body   = ca::core::tag_cast::cast<const jbc::core::SignatureAttr*>(slot.peek());
    const auto  view   = cf.utf8(body->signatureCp);
    if (view.empty()) return true;
    switch (form) {
    case jbc::type::SignatureForm::Class:
        return jbc::type::parseClassSignature(view).is_ok();
    case jbc::type::SignatureForm::Method:
        return jbc::type::parseMethodSignature(view).is_ok();
    case jbc::type::SignatureForm::Field:
        return jbc::type::parseFieldSignature(view).is_ok();
    }
    return true;
}

/// 非法签名槽摘除（按成员种类的文法分道）；返回是否发生摘除。
bool removeIllegalSignatureSlot(ClassFile& cf, std::vector<AttrSlot>& attrs,
                                jbc::type::SignatureForm form)
{
    AttrSlot* slot = findSlot(cf, attrs, kSignature);
    if (slot == nullptr) return false;
    if (signatureSlotIsValid(cf, *slot, form)) return false;
    for (std::size_t i = attrs.size(); i-- > 0;) {
        if (slotNameIs(cf, attrs[i], kSignature)) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

/// JVMS 标准属性名封闭集（UnknownAttributeCleaner 的 guard 面：名单外摘除、
/// 名单内保留——libjbc 未建模的标准属性（ConstantValue/Exceptions 等）解码
/// 为 Raw，但语义面在，必须豁免）。
constexpr std::string_view kKnownAttrNames[] = {
    "ConstantValue",          "Code",
    "StackMapTable",          "Exceptions",
    "InnerClasses",           "EnclosingMethod",
    "Synthetic",              "Signature",
    "SourceFile",             "SourceDebugExtension",
    "LineNumberTable",        "LocalVariableTable",
    "LocalVariableTypeTable", "Deprecated",
    kRva,                     kRia,
    kRvpa,                    kRipa,
    kRvta,                    kRita,
    "AnnotationDefault",      "BootstrapMethods",
    "MethodParameters",       "Module",
    "ModulePackages",         "ModuleMainClass",
    "ModuleResolution",       "NestHost",
    "NestMembers",            "PermittedSubclasses",
    kRecord,                  "CharacterRangeTable",
};

bool isKnownAttrName(std::string_view name)
{
    return std::find(std::begin(kKnownAttrNames), std::end(kKnownAttrNames), name)
           != std::end(kKnownAttrNames);
}

/// 三层属性表的未知属性槽摘除（名单外摘除）。返回是否发生摘除。
bool removeUnknownAttrSlots(const ClassFile& cf, std::vector<AttrSlot>& attrs)
{
    bool changed = false;
    for (std::size_t i = attrs.size(); i-- > 0;) {
        if (slotNameIs(cf, attrs[i], kCode)) continue;   // Code（含嵌套）不触碰
        if (isKnownAttrName(cf.utf8(attrs[i].nameCp))) continue;
        attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
    }
    return changed;
}

/// 槽载荷字节副本：未解码走源 span；已解码 Raw 直取；typed 体经 encodeAttr
/// 重编码（Modified 重编码、Decoded 应与源恒同）去 6 字节槽头取 body。
/// typed 解码失败的 legacy 形态（body 为空）自然落源 span 道。取不到返回空。
std::vector<ca::u8> slotPayloadCopy(const ClassFile& cf, AttrSlot& slot)
{
    if (slot.decoded()) {
        if (slot.peek()->kind() == AttrKind::Raw) {
            return ca::core::tag_cast::cast<const RawAttr*>(slot.peek())->bytes();
        }
        ByteSink out;
        if (encodeAttr(cf, slot, out, cf.source()).is_err()) return {};
        std::vector<ca::u8> full = std::move(out.bytes());
        if (full.size() < 6) return {};
        const ca::u32 len = (static_cast<ca::u32>(full[2]) << 24)
                            | (static_cast<ca::u32>(full[3]) << 16)
                            | (static_cast<ca::u32>(full[4]) << 8)
                            | static_cast<ca::u32>(full[5]);
        if (full.size() != 6 + static_cast<std::size_t>(len)) return {};
        return std::vector<ca::u8>(full.begin() + 6, full.end());
    }
    const auto view = cf.source().spanOrEmpty(slot.data);
    return std::vector<ca::u8>(view.data(), view.data() + view.size());
}

/// MethodParameters 载荷布局判定：JVMS 标准 u1 count（1+4n，javac 产物与
/// 旧栈 writer 同形）优先；非标 u2 count（2+4n，历史夹具/未知产物）兜底；
/// base = count 字段宽，其余形状 base=0。
void methodParamLayoutOf(const std::vector<ca::u8>& bytes, ca::u32& count, std::size_t& base)
{
    count = 0;
    base  = 0;
    if (!bytes.empty()) {
        const ca::u32 n = bytes[0];
        if (bytes.size() == 1 + n * 4) {
            count = n;
            base  = 1;
            return;
        }
    }
    if (bytes.size() >= 2) {
        const ca::u32 n = (static_cast<ca::u32>(bytes[0]) << 8) | static_cast<ca::u32>(bytes[1]);
        if (bytes.size() == 2 + n * 4) {
            count = n;
            base  = 2;
        }
    }
}

/// MethodParameters 槽内非法参数访问位清除（JVMS 4.7.24：u1 count +
/// {u2 name_index, u2 access_flags}*）。0.4.16+ typed 面按行模型原位清位
/// （重编码保 trailing）；typed 拒收的形状落 Raw 字节双道（u1 标准形与
/// u2 非标形都接，其余保守跳过）。返回是否发生改写。
bool clearIllegalParameterAccess(ClassFile& cf, std::vector<AttrSlot>& attrs)
{
    AttrSlot* slot = findSlot(cf, attrs, kMethodParameters);
    if (slot == nullptr) return false;
    // 旧 ILLEGAL_PARAM_ACCESS 表的 u16 位并集（SUPER/VOLATILE/VARARGS 三常量
    // 承载 module 语境别名位 OPEN/TRANSITIVE/STATIC_PHASE 与 SYNCHRONIZED/
    // BRIDGE/TRANSIENT；FINAL/SYNTHETIC/MANDATED 幸存；RECORD/DEPRECATED 位
    // 超 u16 恒零无面）。
    const ca::u16 kIllegalParamMask = kAccPublic | kAccPrivate | kAccProtected | kAccStatic
                                      | kAccSuper | kAccVolatile | kAccVarargs | kAccNative
                                      | kAccInterface | kAccAbstract | kAccStrict
                                      | kAccAnnotation | kAccEnum;
    // 0.4.16+ typed 面：well-formed u1 布局 typed 解码命中，行模型原位清位
    // （重编码保 trailing）；解码拒绝的形状落源字节双道。
    if (auto decoded = cf.decodeAttr(*slot); decoded.is_ok()) {
        if (std::move(decoded).unwrap()->kind() == AttrKind::MethodParameters) {
            const auto* ro = ca::core::tag_cast::cast<const MethodParametersAttr*>(slot->peek());
            bool needWrite = false;
            for (const MethodParametersAttr::Row& row : ro->rows) {
                if (static_cast<ca::u16>(row.accessFlags & ~kIllegalParamMask) != row.accessFlags) {
                    needWrite = true;
                }
            }
            if (!needWrite) return false;
            auto* mut = ca::core::tag_cast::cast<MethodParametersAttr*>(slot->mutate());
            for (MethodParametersAttr::Row& row : mut->rows) {
                row.accessFlags = static_cast<ca::u16>(row.accessFlags & ~kIllegalParamMask);
            }
            return true;
        }
    }
    std::vector<ca::u8> bytes = slotPayloadCopy(cf, *slot);
    // 布局判定：JVMS 标准 u1 count（1 + 4n）优先，非标 u2 count（2 + 4n）兜底。
    ca::u32     count = 0;
    std::size_t base  = 0;
    methodParamLayoutOf(bytes, count, base);
    if (base == 0) return false;
    bool needWrite = false;
    for (ca::u32 i = 0; i < count; ++i) {
        const std::size_t off = base + i * 4 + 2;
        const ca::u16     acc = static_cast<ca::u16>((static_cast<ca::u16>(bytes[off]) << 8)
                                             | static_cast<ca::u16>(bytes[off + 1]));
        if (static_cast<ca::u16>(acc & ~kIllegalParamMask) != acc) needWrite = true;
    }
    if (!needWrite) return false;
    std::vector<ca::u8>& out = bytes;
    for (ca::u32 i = 0; i < count; ++i) {
        const std::size_t off    = base + i * 4 + 2;
        const ca::u16     acc    = static_cast<ca::u16>((static_cast<ca::u16>(out[off]) << 8)
                                              | static_cast<ca::u16>(out[off + 1]));
        const ca::u16     masked = static_cast<ca::u16>(acc & ~kIllegalParamMask);
        out[off]     = static_cast<ca::u8>(masked >> 8);
        out[off + 1] = static_cast<ca::u8>(masked);
    }
    // 写回：已解码 Raw 原位；typed 拒收的 legacy（body 空）以 Raw 接管载荷
    // （setDecoded 后 mutate 置 Modified，写回走重编码）。
    if (slot->decoded()) {
        ca::core::tag_cast::cast<RawAttr*>(slot->mutate())->bytes() = std::move(bytes);
        return true;
    }
    slot->setDecoded(std::make_unique<RawAttr>(std::move(bytes)));
    (void)slot->mutate();
    return true;
}

// ── Runtime{Visible,Invisible}ParameterAnnotations 的 Raw 槽内过滤 ─────────
//
// libjbc 未建模该族（attr_codec 按名分派不含参数注解，解码为 Raw），槽内
// 过滤没有 typed 读取面；路线与 MethodParameters 原位改写一致——Raw 载荷上
// 做二进制级结构手术（remapper 域 AnnotationDefault 的原始注解字节改写同
// 款），读写复用 libjbc 公开 Cursor/ByteSink，annotation 结构按 JVMS 4.7.20
// 测量（u1 参数数 + {u2 注解数 + 条目}*，条目 = type_index + 键值对表，
// element_value 按 tag 递归）。

/// 原始 annotation 条目（Raw 载荷内）：类型描述符 CP + 字节区间。
struct RawParamAnnoEntry
{
    ca::u32     typeCp = 0;
    std::size_t begin  = 0;   ///< 条目首字节（type_index 起）
    std::size_t end    = 0;   ///< 条目尾后
};

/// 原始 annotation 的键值对表段（u2 对数 + {u2 元素名 + element_value}*）；
/// 形状非法返回 false。
bool skipRawAnnoPairs(Cursor& cur);

/// 原始 annotation 条目（type_index + 键值对表）的边界测量。
bool skipRawAnnotation(Cursor& cur)
{
    if (cur.skip(2).is_err()) return false;   // type_index
    return skipRawAnnoPairs(cur);
}

/// 原始 element_value 的边界测量（JVMS 4.7.16.1 按 tag 分派；未知 tag 视为
/// 形状非法——调用方对整槽 fail-closed 保原样）。
bool skipRawElemValue(Cursor& cur)
{
    const auto tag = cur.u8();
    if (tag.is_err()) return false;
    switch (static_cast<char>(tag.unwrap())) {
    case 'B':
    case 'C':
    case 'D':
    case 'F':
    case 'I':
    case 'J':
    case 'S':
    case 'Z':
    case 's':
    case 'c':
        return cur.skip(2).is_ok();   // 常量 / 类信息 CP 索引
    case 'e':
        return cur.skip(4).is_ok();   // 枚举常量：type_name + const_name
    case '@':
        return skipRawAnnotation(cur);   // 嵌套注解
    case '[': {
        const auto n = cur.u16();
        if (n.is_err()) return false;
        for (ca::u32 i = 0; i < n.unwrap(); ++i) {
            if (!skipRawElemValue(cur)) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

bool skipRawAnnoPairs(Cursor& cur)
{
    const auto pairs = cur.u16();
    if (pairs.is_err()) return false;
    for (ca::u32 i = 0; i < pairs.unwrap(); ++i) {
        if (cur.skip(2).is_err()) return false;   // element_name_index
        if (!skipRawElemValue(cur)) return false;
    }
    return true;
}

/// 参数注解两槽槽内过滤：命中谓词的条目摘除——部分摘除留槽（参数行数与行序
/// 保持：num_parameters 与方法描述符参数数对应，不缩行）、全部条目摘空时连
/// 槽摘除（旧栈 annotation 表清空后旧 writer 不再产出该属性，语义等价）。
/// 双道：0.4.16+ typed 面（ParameterAnnotationsAttr 行模型）为主，Raw 载荷
/// 二进制手术为 legacy 兜底（typed 解码拒绝的形态）；Raw 道布局不符/截尾/
/// 尾随字节整槽保守跳过（fail-closed，不产半截改写）。返回是否发生任何变化。
bool filterParameterAnnotationSlots(ClassFile&             cf,
                                    std::vector<AttrSlot>& attrs,
                                    const AnnoFilter       filter)
{
    bool changed = false;
    for (std::size_t i = attrs.size(); i-- > 0;) {
        AttrSlot& slot = attrs[i];
        if (!slotNameIs(cf, slot, kRvpa) && !slotNameIs(cf, slot, kRipa)) continue;
        auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()) continue;
        const AttrKind slotKind = std::move(decoded).unwrap()->kind();
        if (slotKind == AttrKind::ParameterAnnotations) {
            // 0.4.16+ typed 面：注解结构同 AnnotationsAttr 模型，行保持语义
            // 与 Raw 道一致（部分摘除留槽、全空摘槽）。
            const auto* ro = ca::core::tag_cast::cast<const ParameterAnnotationsAttr*>(slot.peek());
            const auto hit = [&cf, filter](const AnnotationsAttr::Annotation& a) {
                return annoFilterMatches(filter, cf.utf8(a.typeCp));
            };
            bool anyHit  = false;
            bool anyKept = false;
            for (const std::vector<AnnotationsAttr::Annotation>& row : ro->parameters) {
                for (const AnnotationsAttr::Annotation& a : row) {
                    if (hit(a)) {
                        anyHit = true;
                    } else {
                        anyKept = true;
                    }
                }
            }
            if (!anyHit) continue;
            changed = true;
            if (!anyKept) {
                attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            auto& rows =
                ca::core::tag_cast::cast<ParameterAnnotationsAttr*>(slot.mutate())->parameters;
            for (std::vector<AnnotationsAttr::Annotation>& row : rows) {
                row.erase(std::remove_if(row.begin(), row.end(), hit), row.end());
            }
            continue;
        }
        if (slotKind != AttrKind::Raw) continue;
        const std::vector<ca::u8>& bytes =
            ca::core::tag_cast::cast<const RawAttr*>(slot.peek())->bytes();
        if (bytes.empty()) continue;

        // 形状解析（fail-closed：任一步不合规即整槽跳过）。
        Cursor       cur{ByteView{bytes.data(), bytes.size()}};
        const auto   numParams = cur.u8();
        if (numParams.is_err()) continue;
        std::vector<std::vector<RawParamAnnoEntry>> rows(numParams.unwrap());
        bool                                        shapeOk = true;
        for (std::vector<RawParamAnnoEntry>& row : rows) {
            const auto n = cur.u16();
            if (n.is_err()) {
                shapeOk = false;
                break;
            }
            for (ca::u32 e = 0; e < n.unwrap(); ++e) {
                const std::size_t begin  = cur.pos();
                const auto        typeCp = cur.u16();
                if (typeCp.is_err() || !skipRawAnnoPairs(cur)) {
                    shapeOk = false;
                    break;
                }
                row.push_back({typeCp.unwrap(), begin, cur.pos()});
            }
            if (!shapeOk) break;
        }
        if (!shapeOk || !cur.eof()) continue;

        // 谓词判定：无命中零触碰；全空摘槽；部分摘除重建载荷。
        const auto hit = [&cf, filter](const RawParamAnnoEntry& e) {
            return annoFilterMatches(filter, cf.utf8(e.typeCp));
        };
        bool anyHit  = false;
        bool anyKept = false;
        for (const std::vector<RawParamAnnoEntry>& row : rows) {
            for (const RawParamAnnoEntry& e : row) {
                if (hit(e)) {
                    anyHit = true;
                }
                else {
                    anyKept = true;
                }
            }
        }
        if (!anyHit) continue;
        changed = true;
        if (!anyKept) {
            attrs.erase(attrs.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ByteSink out;
        out.u8(static_cast<ca::u8>(rows.size()));
        for (const std::vector<RawParamAnnoEntry>& row : rows) {
            ca::u32 kept = 0;
            for (const RawParamAnnoEntry& e : row) {
                if (!hit(e)) ++kept;
            }
            out.u16(static_cast<ca::u16>(kept));
            for (const RawParamAnnoEntry& e : row) {
                if (hit(e)) continue;
                out.raw(ByteView{bytes.data() + e.begin, e.end - e.begin});
            }
        }
        ca::core::tag_cast::cast<RawAttr*>(slot.mutate())->bytes() = std::move(out.bytes());
    }
    return changed;
}

/// 字段/方法访问位两面清位（unit() + backing 同步，pass-contract §1 双面
/// 落盘义务——标志位在两面都有模型，任一面落单都会失真）。
template <typename UnitMember>
bool clearMemberAccessBits(UnitMember& unitMember, MemberInfo& backingMember, ca::u16 mask)
{
    const ca::u16 cleaned = static_cast<ca::u16>(backingMember.access & ~mask);
    if (cleaned == backingMember.access) return false;
    backingMember.access = cleaned;
    unitMember.access &= ~static_cast<ca::u32>(mask);
    return true;
}

}   // namespace

// ── MetaInfoRemover ──

mjbase::spi::PreservedAnalyses MetaInfoRemover::run(jbc::ir::ClassSession&            session,
                                                    mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf = session.backing();
    bool       changed = false;

    // 类级：SourceFile/SourceDebug、注解两族（旧面 classNode 各注解向量清空；
    // type annotation 在 libjbc 的 localvar/code 目标形态与成员级同族模型）。
    // 未知属性不触碰（旧面 classNode.attrs 清空在旧树恒为空操作——tree reader
    // 从不回填 attrs 向量，未知属性在读入即弃；新载体按头注契约保持原样，
    // 摘除归 UnknownAttributeCleaner）。record 组件的注解旧面同步清空——
    // 组件嵌套注解/TypeAnnotation 槽与成员级同规则槽内过滤，父 Record 槽
    // 连带置脏（组件 Signature 与未知属性不动，与旧面一致）。
    changed |= removeNamedSlots(cf, cf.attrs, {kSourceFile, kSourceDebug});
    changed |= filterAnnotationSlots(cf, cf.attrs, AnnoFilter::All);
    changed |= filterTypeAnnotationSlots(cf, cf.attrs, AnnoFilter::All);
    changed |= clearRecordComponentAnnotationSlots(cf);
    for (MemberInfo& f : cf.fields) {
        changed |= filterAnnotationSlots(cf, f.attrs, AnnoFilter::All);
        changed |= filterTypeAnnotationSlots(cf, f.attrs, AnnoFilter::All);
    }
    for (MemberInfo& m : cf.methods) {
        // MethodParameters（旧面 parameters 清空）与 AnnotationDefault（旧面
        // annotationDefault 清空）整槽摘除；参数注解两槽同（旧面
        // visible/invisibleParameterAnnotations 清空——整槽摘除，无槽内过滤面）。
        changed |= removeNamedSlots(cf, m.attrs,
                                    {kMethodParameters, kAnnotationDefault, kRvpa, kRipa});
        changed |= filterAnnotationSlots(cf, m.attrs, AnnoFilter::All);
        changed |= filterTypeAnnotationSlots(cf, m.attrs, AnnoFilter::All);
        changed |= removeCodeNestedDebugInfo(cf, m);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── FinalRemover ──

mjbase::spi::PreservedAnalyses FinalRemover::run(jbc::ir::ClassSession&            session,
                                                 mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile&         cf   = session.backing();
    jbc::ir::ClassUnit& unit = session.unit();
    bool               changed = false;
    for (std::size_t i = 0; i < cf.fields.size(); ++i) {
        changed |= clearMemberAccessBits(unit.fields[i], cf.fields[i], kAccFinal);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── SyntheticBridgeRemover ──

mjbase::spi::PreservedAnalyses SyntheticBridgeRemover::run(jbc::ir::ClassSession&            session,
                                                           mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile&          cf   = session.backing();
    jbc::ir::ClassUnit& unit = session.unit();
    bool                changed = false;

    if ((cf.access & kAccSynthetic) != 0) {
        cf.access    = static_cast<ca::u16>(cf.access & ~kAccSynthetic);
        unit.access &= ~static_cast<ca::u32>(kAccSynthetic);
        changed = true;
    }
    for (std::size_t i = 0; i < cf.methods.size(); ++i) {
        changed |= clearMemberAccessBits(unit.methods[i], cf.methods[i],
                                         static_cast<ca::u16>(kAccSynthetic | kAccBridge));
    }
    for (std::size_t i = 0; i < cf.fields.size(); ++i) {
        changed |= clearMemberAccessBits(unit.fields[i], cf.fields[i], kAccSynthetic);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── KotlinMetadataRemover ──

mjbase::spi::PreservedAnalyses KotlinMetadataRemover::run(jbc::ir::ClassSession&            session,
                                                          mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf = session.backing();
    bool       changed = false;

    // 类级只查 visible 槽（旧实现面）：@kotlin/Metadata。
    changed |= filterAnnotationSlots(cf, cf.attrs, AnnoFilter::Kotlin, {kRva});
    for (MemberInfo& m : cf.methods) {
        changed |= filterAnnotationSlots(cf, m.attrs, AnnoFilter::Jetbrains);
    }
    for (MemberInfo& f : cf.fields) {
        changed |= filterAnnotationSlots(cf, f.attrs, AnnoFilter::Jetbrains);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── SourceInfoRemover ──

mjbase::spi::PreservedAnalyses SourceInfoRemover::run(jbc::ir::ClassSession&            session,
                                                      mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf      = session.backing();
    const bool changed = removeNamedSlots(cf, cf.attrs, {kSourceFile, kSourceDebug});
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── SignatureRemover ──

mjbase::spi::PreservedAnalyses SignatureRemover::run(jbc::ir::ClassSession&            session,
                                                     mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf = session.backing();
    bool       changed = removeNamedSlots(cf, cf.attrs, {kSignature});
    for (MemberInfo& m : cf.methods) {
        changed |= removeNamedSlots(cf, m.attrs, {kSignature});
    }
    for (MemberInfo& f : cf.fields) {
        changed |= removeNamedSlots(cf, f.attrs, {kSignature});
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── IllegalAnnotationRemover ──

mjbase::spi::PreservedAnalyses IllegalAnnotationRemover::run(jbc::ir::ClassSession&            session,
                                                             mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf = session.backing();
    bool       changed = filterAnnotationSlots(cf, cf.attrs, AnnoFilter::Malformed);
    for (MemberInfo& m : cf.methods) {
        changed |= filterAnnotationSlots(cf, m.attrs, AnnoFilter::Malformed);
    }
    for (MemberInfo& f : cf.fields) {
        changed |= filterAnnotationSlots(cf, f.attrs, AnnoFilter::Malformed);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── IllegalSignatureRemover ──

mjbase::spi::PreservedAnalyses IllegalSignatureRemover::run(jbc::ir::ClassSession&            session,
                                                            mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf = session.backing();
    bool changed = removeIllegalSignatureSlot(cf, cf.attrs, jbc::type::SignatureForm::Class);
    for (MemberInfo& m : cf.methods) {
        changed |= removeIllegalSignatureSlot(cf, m.attrs, jbc::type::SignatureForm::Method);
    }
    for (MemberInfo& f : cf.fields) {
        changed |= removeIllegalSignatureSlot(cf, f.attrs, jbc::type::SignatureForm::Field);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── IllegalTypeAnnotationRemover ──

mjbase::spi::PreservedAnalyses IllegalTypeAnnotationRemover::run(jbc::ir::ClassSession&            session,
                                                                 mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf = session.backing();
    bool       changed = filterTypeAnnotationSlots(cf, cf.attrs, AnnoFilter::Malformed);
    for (MemberInfo& m : cf.methods) {
        changed |= filterTypeAnnotationSlots(cf, m.attrs, AnnoFilter::Malformed);
        // code 目标形态（cast/instanceof/new/localvar）旧栈在方法向量上同面
        // 参与——Code 嵌套槽内过滤（父 Code 槽连带置脏）。
        changed |= filterCodeNestedTypeAnnotationSlots(cf, m, AnnoFilter::Malformed);
    }
    for (MemberInfo& f : cf.fields) {
        changed |= filterTypeAnnotationSlots(cf, f.attrs, AnnoFilter::Malformed);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── IllegalSourceInfoRemover ──

mjbase::spi::PreservedAnalyses IllegalSourceInfoRemover::run(jbc::ir::ClassSession&            session,
                                                             mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf      = session.backing();
    bool       changed = false;
    for (std::size_t i = cf.attrs.size(); i-- > 0;) {
        AttrSlot& slot = cf.attrs[i];
        if (slotNameIs(cf, slot, kSourceFile)) {
            if (isIllegalDebugInfo(sourceFileValue(cf, slot))) {
                cf.attrs.erase(cf.attrs.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
            }
        }
        else if (slotNameIs(cf, slot, kSourceDebug)) {
            if (isIllegalDebugInfo(sourceDebugValue(cf, slot))) {
                cf.attrs.erase(cf.attrs.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
            }
        }
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── UnknownAttributeCleaner ──

mjbase::spi::PreservedAnalyses UnknownAttributeCleaner::run(jbc::ir::ClassSession&            session,
                                                            mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf = session.backing();
    bool       changed = removeUnknownAttrSlots(cf, cf.attrs);
    for (MemberInfo& m : cf.methods) {
        changed |= removeUnknownAttrSlots(cf, m.attrs);
    }
    for (MemberInfo& f : cf.fields) {
        changed |= removeUnknownAttrSlots(cf, f.attrs);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── IllegalAccessRemover ──

mjbase::spi::PreservedAnalyses IllegalAccessRemover::run(jbc::ir::ClassSession&            session,
                                                         mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile&          cf   = session.backing();
    jbc::ir::ClassUnit& unit = session.unit();
    bool                changed = false;

    // 类级非法位（旧 ILLEGAL_CLASS_ACCESS 表位并集）。
    const ca::u16 kIllegalClassMask =
        kAccStatic | kAccVolatile | kAccVarargs | kAccNative | kAccStrict;
    if ((cf.access & kIllegalClassMask) != 0) {
        cf.access    = static_cast<ca::u16>(cf.access & ~kIllegalClassMask);
        unit.access &= ~static_cast<ca::u32>(kIllegalClassMask);
        changed = true;
    }
    // 方法级非法位（旧 ILLEGAL_METHOD_ACCESS 表位并集；RECORD 位超 u16 恒零）。
    const ca::u16 kIllegalMethodMask =
        kAccInterface | kAccAnnotation | kAccEnum | kAccModule;
    for (std::size_t i = 0; i < cf.methods.size(); ++i) {
        changed |= clearMemberAccessBits(unit.methods[i], cf.methods[i], kIllegalMethodMask);
        changed |= clearIllegalParameterAccess(cf, cf.methods[i].attrs);
    }
    // 字段级：类非 enum 时清 ENUM 位 + 非法位（旧 ILLEGAL_FIELD_ACCESS 表位
    // 并集；SUPER 与方法 SYNCHRONIZED 同位，别名位按上下文分列——字段上下文
    // 的 0x20 无语义）。
    const ca::u16 kIllegalFieldMask = kAccSuper | kAccNative | kAccInterface | kAccAbstract
                                      | kAccStrict | kAccAnnotation | kAccModule;
    const bool classIsEnum = (cf.access & kAccEnum) != 0;
    for (std::size_t i = 0; i < cf.fields.size(); ++i) {
        if (!classIsEnum) {
            changed |= clearMemberAccessBits(unit.fields[i], cf.fields[i], kAccEnum);
        }
        changed |= clearMemberAccessBits(unit.fields[i], cf.fields[i], kIllegalFieldMask);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

// ── SpecificAnnotationRemover ──

mjbase::spi::PreservedAnalyses SpecificAnnotationRemover::run(jbc::ir::ClassSession&            session,
                                                              mjbase::spi::ClassAnalysisManager& cam)
{
    (void)cam;
    ClassFile& cf = session.backing();
    bool       changed = filterAnnotationSlots(cf, cf.attrs, AnnoFilter::Blacklisted);
    for (MemberInfo& m : cf.methods) {
        changed |= filterAnnotationSlots(cf, m.attrs, AnnoFilter::Blacklisted);
        // 参数注解两槽槽内过滤：libjbc 未建模该族（Raw 槽），按 JVMS 4.7.20
        // 布局二进制级条目摘除——部分摘除留槽、全空摘槽（旧面
        // removeAnnotationsIf 同语义：条目清空后旧 writer 不再产出该属性）。
        changed |= filterParameterAnnotationSlots(cf, m.attrs, AnnoFilter::Blacklisted);
    }
    for (MemberInfo& f : cf.fields) {
        changed |= filterAnnotationSlots(cf, f.attrs, AnnoFilter::Blacklisted);
    }
    return changed ? mjbase::spi::PreservedAnalyses::none()
                   : mjbase::spi::PreservedAnalyses::all();
}

}   // namespace mjbase::pass
