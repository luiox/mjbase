/// @file        class_kind_test.cpp
/// @brief       class 头形态分类：各 kind、并列旗标优先序、record 双判据。
#include <gtest/gtest.h>

#include "mjbase/project/class_kind.hpp"

using mjbase::project::classifyClassKind;
using mjbase::project::classKindName;
using mjbase::project::DbClassKind;

namespace {

// JVMS 4.1 class 访问旗标（测试用位）。
constexpr ca::i32 kAccPublic      = 0x0001;
constexpr ca::i32 kAccFinal       = 0x0010;
constexpr ca::i32 kAccSuper       = 0x0020;
constexpr ca::i32 kAccInterface   = 0x0200;
constexpr ca::i32 kAccAbstract    = 0x0400;
constexpr ca::i32 kAccSynthetic   = 0x1000;
constexpr ca::i32 kAccAnnotation  = 0x2000;
constexpr ca::i32 kAccEnum        = 0x4000;
constexpr ca::i32 kAccModule      = 0x8000;
constexpr ca::i32 kAccRecordPseudo = 0x10000;

}   // namespace

TEST(MjClassKindTest, PlainClassKinds)
{
    EXPECT_EQ(DbClassKind::Class,
              classifyClassKind(kAccPublic | kAccFinal | kAccSuper, "java/lang/Object"));
    EXPECT_EQ(DbClassKind::Class, classifyClassKind(0, ""));
    EXPECT_EQ(DbClassKind::Class, classifyClassKind(kAccFinal | kAccAbstract, "some/Base"));
    // 合成类仍是 class 形态（synthetic 是修饰位不是形态位）。
    EXPECT_EQ(DbClassKind::Class, classifyClassKind(kAccSynthetic, ""));
}

TEST(MjClassKindTest, InterfaceAndAnnotationKinds)
{
    EXPECT_EQ(DbClassKind::Interface,
              classifyClassKind(kAccInterface | kAccAbstract, "java/lang/Runnable"));
    EXPECT_EQ(DbClassKind::Annotation,
              classifyClassKind(kAccInterface | kAccAnnotation | kAccAbstract,
                                "java/lang/annotation/Annotation"));
    // 接口旗标与矛盾父类并存：形态位优先（父类名不参与 interface 判定）。
    EXPECT_EQ(DbClassKind::Interface,
              classifyClassKind(kAccInterface, "java/lang/Record"));
}

TEST(MjClassKindTest, EnumKind)
{
    EXPECT_EQ(DbClassKind::Enum,
              classifyClassKind(kAccPublic | kAccFinal | kAccEnum, "java/lang/Enum"));
}

TEST(MjClassKindTest, RecordKindBySuperAndPseudoBit)
{
    // 规范判据：直接父类 java/lang/Record。
    EXPECT_EQ(DbClassKind::Record,
              classifyClassKind(kAccPublic | kAccFinal, "java/lang/Record"));
    // 工具链伪位（0x10000，class 文件不编码）单独成立。
    EXPECT_EQ(DbClassKind::Record, classifyClassKind(kAccRecordPseudo, ""));
    // 子类不误判：record 的子类形态仍是 class。
    EXPECT_EQ(DbClassKind::Class, classifyClassKind(0, "example/Point"));
}

TEST(MjClassKindTest, ModuleKindAndPrecedence)
{
    EXPECT_EQ(DbClassKind::Module, classifyClassKind(kAccModule | kAccAbstract, ""));
    // 并列旗标按 Module > Annotation > Interface > Enum > Record 取先。
    EXPECT_EQ(DbClassKind::Module,
              classifyClassKind(kAccModule | kAccInterface | kAccEnum, "java/lang/Record"));
    EXPECT_EQ(DbClassKind::Annotation,
              classifyClassKind(kAccInterface | kAccAnnotation | kAccEnum, "java/lang/Record"));
    EXPECT_EQ(DbClassKind::Enum, classifyClassKind(kAccEnum | kAccRecordPseudo, "java/lang/Record"));
}

TEST(MjClassKindTest, KindNamesAreStableTokens)
{
    EXPECT_EQ("class", classKindName(DbClassKind::Class));
    EXPECT_EQ("interface", classKindName(DbClassKind::Interface));
    EXPECT_EQ("annotation", classKindName(DbClassKind::Annotation));
    EXPECT_EQ("enum", classKindName(DbClassKind::Enum));
    EXPECT_EQ("record", classKindName(DbClassKind::Record));
    EXPECT_EQ("module", classKindName(DbClassKind::Module));
}
