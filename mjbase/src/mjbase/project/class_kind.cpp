#include "mjbase/project/class_kind.hpp"

namespace mjbase::project {
namespace {

// JVMS 4.1 class 访问旗标（本判定消费的位；数值即位掩码）。
constexpr ca::i32 kClassAccInterface  = 0x0200;
constexpr ca::i32 kClassAccAnnotation = 0x2000;
constexpr ca::i32 kClassAccEnum       = 0x4000;
constexpr ca::i32 kClassAccModule     = 0x8000;
// ACC_RECORD 是工具链伪位：class 文件格式不编码 record 旗标（record 的
// 规范判据是父类），classfile API / ASM 系以 0x10000 伪位在内存态表达，
// 与父类判据互补（构造语料可能只携带其一）。
constexpr ca::i32 kClassAccRecordPseudo = 0x10000;

constexpr std::string_view kRecordSuperName = "java/lang/Record";

}   // namespace

DbClassKind classifyClassKind(ca::i32 access, std::string_view superName) noexcept
{
    if ((access & kClassAccModule) != 0) return DbClassKind::Module;
    if ((access & kClassAccInterface) != 0) {
        if ((access & kClassAccAnnotation) != 0) return DbClassKind::Annotation;
        return DbClassKind::Interface;
    }
    if ((access & kClassAccEnum) != 0) return DbClassKind::Enum;
    if ((access & kClassAccRecordPseudo) != 0 || superName == kRecordSuperName) {
        return DbClassKind::Record;
    }
    return DbClassKind::Class;
}

std::string_view classKindName(DbClassKind kind) noexcept
{
    switch (kind) {
    case DbClassKind::Class: return "class";
    case DbClassKind::Interface: return "interface";
    case DbClassKind::Annotation: return "annotation";
    case DbClassKind::Enum: return "enum";
    case DbClassKind::Record: return "record";
    case DbClassKind::Module: return "module";
    }
    return "class";
}

}   // namespace mjbase::project
