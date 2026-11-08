#include "semantic_flag_rules.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjbase::jasm {
namespace {

namespace jc = jbc::core;

using ca::str::Utf8StringRef;

// ---------------------------------------------------------------------------
// JVMS access_flags 位面（JVMS 4.1/4.5/4.6 表 A 共位：同一 bit 在类/字段/
// 方法三面名字不同——0x0020 类面是 ACC_SUPER、方法面是 ACC_SYNCHRONIZED，
// 0x0040 字段面是 ACC_VOLATILE、方法面是 ACC_BRIDGE）。
// ---------------------------------------------------------------------------
constexpr ca::u16 kAccPublic       = 0x0001;
constexpr ca::u16 kAccPrivate      = 0x0002;
constexpr ca::u16 kAccProtected    = 0x0004;
constexpr ca::u16 kAccStatic       = 0x0008;
constexpr ca::u16 kAccFinal        = 0x0010;
constexpr ca::u16 kAccSuperOrSync  = 0x0020;
constexpr ca::u16 kAccVolOrBridge  = 0x0040;
constexpr ca::u16 kAccTransient    = 0x0080;
constexpr ca::u16 kAccNative       = 0x0100;
constexpr ca::u16 kAccInterface    = 0x0200;
constexpr ca::u16 kAccAbstract     = 0x0400;
constexpr ca::u16 kAccStrict       = 0x0800;
constexpr ca::u16 kAccSynthetic    = 0x1000;
constexpr ca::u16 kAccAnnotation   = 0x2000;
constexpr ca::u16 kAccEnum         = 0x4000;
constexpr ca::u16 kAccModule       = 0x8000;

// 版本窗（HotSpot JAVA_1_5/8/17_VERSION = major 49/52/61）。
constexpr ca::u16 kMajorInterfacesLegacyBits = 49;  ///< 接口禁 SUPER/ENUM 位、类外注解禁
constexpr ca::u16 kMajorClinitStaticRequired = 51;  ///< ``<clinit>`` 强制 ACC_STATIC 起始
constexpr ca::u16 kMajorInterfaceMethodsModern = 52;  ///< 接口方法 public/private 恰一
constexpr ca::u16 kMajorStrictObsolete = 61;         ///< ACC_STRICT 退役（仅 major ≤ 60 查）

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// HotSpot 诊断文本的位面段（%X 形，如 0x100A——对拍可直读）。
std::string hexFlags(ca::u32 flags)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%X", flags);
    return std::string(buffer);
}

void fail(std::vector<Diagnostic>& out, std::string message)
{
    out.push_back({DiagnosticSeverity::Error, DiagnosticStage::Validate, utf16(message),
                   std::nullopt});
}

/// 可见性位互斥（JVMS 4.5/4.6「at most one of ACC_PUBLIC/ACC_PRIVATE/
/// ACC_PROTECTED」；HotSpot has_illegal_visibility 同式）。
bool hasIllegalVisibility(ca::u16 flags)
{
    const bool isPublic    = (flags & kAccPublic) != 0;
    const bool isProtected = (flags & kAccProtected) != 0;
    const bool isPrivate   = (flags & kAccPrivate) != 0;
    return (isPublic && isProtected) || (isPublic && isPrivate) || (isProtected && isPrivate);
}

/// R1-R5（类面，JVMS 4.1；HotSpot verify_legal_class_modifiers + 模块特例）。
///
/// - R1 ACC_MODULE：模块类文件 JVM 直接 NoClassDefFoundError（先于其余
///   检查，JVMS 4.1 module-info 专属）；
/// - R2 ACC_ABSTRACT × ACC_FINAL 互斥（4.1 "must not have both"）；
/// - R3 接口必须 ACC_ABSTRACT（4.1-200）；
/// - R4 接口自 major 49 起禁 ACC_SUPER/ACC_ENUM（4.1 接口旗面；49 前豁免
///   与 HotSpot 一致）；
/// - R5 非 interface 不得 ACC_ANNOTATION（4.1 注解接口必为接口；major 49 起）。
void checkClassFlags(const jc::ClassFile& cf, std::vector<Diagnostic>& out)
{
    const ca::u16 flags = cf.access;
    const std::string name{cf.thisName()};

    if ((flags & kAccModule) != 0) {
        // HotSpot 文本（NoClassDefFoundError 形）：模块类文件不是类。
        fail(out, name + " is not a class because access_flag ACC_MODULE is set");
        return;
    }

    const bool isInterface = (flags & kAccInterface) != 0;
    const bool isIllegal   = ((flags & kAccAbstract) != 0 && (flags & kAccFinal) != 0)
                           || (isInterface && (flags & kAccAbstract) == 0)
                           || (isInterface && cf.major >= kMajorInterfacesLegacyBits
                               && (flags & (kAccSuperOrSync | kAccEnum)) != 0)
                           || (!isInterface && cf.major >= kMajorInterfacesLegacyBits
                               && (flags & kAccAnnotation) != 0);
    if (isIllegal)
        fail(out, "Illegal class modifiers in class " + name + ": " + hexFlags(flags));
}

/// R6-R8（字段面，JVMS 4.5；HotSpot verify_legal_field_modifiers）。
///
/// - R6 接口字段（issue #179 主案例）：必须 ACC_PUBLIC+ACC_STATIC+ACC_FINAL
///   全置（4.5 "Fields of interfaces must have their ACC_PUBLIC, ACC_STATIC,
///   and ACC_FINAL flags set"），且禁 ACC_PRIVATE/ACC_PROTECTED/ACC_VOLATILE/
///   ACC_TRANSIENT（major 49 起加禁 ACC_ENUM 位）；ACC_SYNTHETIC/ACC_MANDATED
///   豁免——0x1019 过、0x100A/0x1009 拒即由本规则承载；
/// - R7 类字段可见性位互斥（4.5 at most one）；
/// - R8 类字段 ACC_FINAL × ACC_VOLATILE 互斥（4.5 JLS §8.3.1.4）。
void checkFieldFlags(const jc::ClassFile& cf, const jc::MemberInfo& field,
                     std::vector<Diagnostic>& out)
{
    const ca::u16 flags      = field.access;
    const bool    isInterface = (cf.access & kAccInterface) != 0;
    const std::string name{cf.thisName()};

    bool isIllegal = false;
    if (isInterface) {
        constexpr ca::u16 kRequired = kAccPublic | kAccStatic | kAccFinal;
        isIllegal = (flags & kRequired) != kRequired
                    || (flags & (kAccPrivate | kAccProtected | kAccVolOrBridge | kAccTransient))
                           != 0
                    || (cf.major >= kMajorInterfacesLegacyBits && (flags & kAccEnum) != 0);
    }
    else {
        isIllegal = hasIllegalVisibility(flags)
                    || ((flags & kAccFinal) != 0 && (flags & kAccVolOrBridge) != 0);
    }
    if (isIllegal)
        fail(out, "Illegal field modifiers in class " + name + ": " + hexFlags(flags));
}

/// R9-R16（方法面，JVMS 4.6；HotSpot verify_legal_method_modifiers +
/// parse_method 的 ``<clinit>``/接口 ``<init>`` 特例）。
///
/// - R9 ``<clinit>``（major 51 起）必须 ACC_STATIC（4.6；其余位 JVM 掩蔽
///   忽略，故命中即返回、不走组合检查——与 HotSpot 掩蔽式一致）；
/// - R10 接口不得声明 ``<init>``（4.6/2.9.1）；
/// - R11 接口方法 modern 分支（major >= 52）：ACC_PUBLIC/ACC_PRIVATE 恰一
///   （4.6 "exactly one"），禁 ACC_NATIVE/ACC_PROTECTED/ACC_FINAL/
///   ACC_SYNCHRONIZED（4.6 "except"），ACC_ABSTRACT 加禁 ACC_PRIVATE/
///   ACC_STATIC（ACC_STRICT 仅 major < 61 计）；
/// - R12 接口方法 49-51 窗：必须 public abstract，禁 private/protected/
///   static/final/synchronized/native/strict；
/// - R13 接口方法 pre-49 窗：必须 public abstract，禁 static/final/native；
/// - R14 类方法可见性位互斥（4.6 at most one）；
/// - R15 类 ``<init>``：禁 static/final/synchronized/native/abstract，
///   major 49 起禁 ACC_BRIDGE（0x0040 同位）；
/// - R16 类 abstract 方法（非 ``<init>``）：禁 final/native/private/static，
///   major 49 起加禁 synchronized 与 strict（strict 退役窗同 R11）。
void checkMethodFlags(const jc::ClassFile& cf, const jc::MemberInfo& method,
                      std::vector<Diagnostic>& out)
{
    const ca::u16     flags      = method.access;
    const std::string name{cf.nameOf(method)};
    const std::string className{cf.thisName()};
    const bool        isInterface = (cf.access & kAccInterface) != 0;
    const ca::u16     major       = cf.major;

    if (name == "<clinit>") {
        if (major >= kMajorClinitStaticRequired && (flags & kAccStatic) == 0)
            fail(out, "Method <clinit> is not static in class file " + className);
        return;
    }
    if (isInterface && name == "<init>") {
        fail(out, "Interface cannot have a method named <init>, class file " + className);
        return;
    }

    const bool strictCounts = major < kMajorStrictObsolete;
    bool       isIllegal    = false;
    if (isInterface) {
        if (major >= kMajorInterfaceMethodsModern) {
            const bool isPublic = (flags & kAccPublic) != 0;
            const bool isPrivate = (flags & kAccPrivate) != 0;
            isIllegal = (isPublic == isPrivate)
                        || (flags & (kAccNative | kAccProtected | kAccFinal | kAccSuperOrSync))
                               != 0
                        || ((flags & kAccAbstract) != 0
                            && ((flags & (kAccPrivate | kAccStatic)) != 0
                                || (strictCounts && (flags & kAccStrict) != 0)));
        }
        else if (major >= kMajorInterfacesLegacyBits) {
            isIllegal = (flags & kAccPublic) == 0 || (flags & kAccPrivate) != 0
                        || (flags & kAccProtected) != 0 || (flags & kAccStatic) != 0
                        || (flags & kAccFinal) != 0 || (flags & kAccSuperOrSync) != 0
                        || (flags & kAccNative) != 0 || (flags & kAccAbstract) == 0
                        || (flags & kAccStrict) != 0;
        }
        else {
            isIllegal = (flags & kAccPublic) == 0 || (flags & kAccStatic) != 0
                        || (flags & kAccFinal) != 0 || (flags & kAccNative) != 0
                        || (flags & kAccAbstract) == 0;
        }
    }
    else if (hasIllegalVisibility(flags)) {
        isIllegal = true;
    }
    else if (name == "<init>") {
        isIllegal = (flags & (kAccStatic | kAccFinal | kAccSuperOrSync | kAccNative
                              | kAccAbstract))
                           != 0
                    || (major >= kMajorInterfacesLegacyBits && (flags & kAccVolOrBridge) != 0);
    }
    else if ((flags & kAccAbstract) != 0) {
        isIllegal = (flags & (kAccFinal | kAccNative | kAccPrivate | kAccStatic)) != 0
                    || (major >= kMajorInterfacesLegacyBits
                        && ((flags & kAccSuperOrSync) != 0
                            || (strictCounts && (flags & kAccStrict) != 0)));
    }

    if (isIllegal)
        fail(out, "Method " + name + " in class " + className + " has illegal modifiers: "
                      + hexFlags(flags));
}

std::vector<Diagnostic> analyzeSemanticFlagsClass(const jc::ClassFile& cf)
{
    std::vector<Diagnostic> out;
    checkClassFlags(cf, out);
    for (const auto& field : cf.fields) checkFieldFlags(cf, field, out);
    for (const auto& method : cf.methods) checkMethodFlags(cf, method, out);
    return out;
}

}   // namespace

std::vector<Diagnostic> analyzeSemanticFlags(const std::vector<ca::u8>& classBytes)
{
    auto read = jc::readClass(jc::ByteView{classBytes.data(), classBytes.size()});
    if (read.is_err()) return {};   // 解析失败归因读取闸，不在本门重复报
    auto parsed = std::move(read).unwrap();   // ReadResult 持 unique_ptr，只能移出
    return analyzeSemanticFlagsClass(*parsed.file);
}

}   // namespace mjbase::jasm
