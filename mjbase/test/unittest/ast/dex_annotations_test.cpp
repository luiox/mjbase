#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <utility>

#include "dex/builder.hpp"
#include "dex/reader.hpp"
#include "smali/assemble.hpp"

#include "mjbase/ast/dex_annotations.hpp"

namespace {

using namespace mjbase::ast;

std::unique_ptr<dex::core::DexFile> assembleDex(std::string_view smali)
{
    auto fresh = dex::core::DexFile::createEmpty(35);
    dex::smali::SmaliAssembler assembler(*fresh);
    auto assembled = assembler.assembleClass(smali);
    if (assembled.is_err()) {
        ADD_FAILURE() << "smali line " << assembled.unwrap_err().line << ": "
                      << assembled.unwrap_err().detail;
        return nullptr;
    }
    auto built = dex::core::buildDex(*fresh, assembler.options());
    EXPECT_TRUE(built.is_ok());
    if (built.is_err()) return nullptr;
    const auto bytes = std::move(built).unwrap();
    auto parsed = dex::core::readDex({bytes.data(), bytes.size()});
    EXPECT_TRUE(parsed.is_ok());
    return parsed.is_ok() ? std::move(parsed).unwrap() : nullptr;
}

const AnnotationValue* findElement(const Annotation& annotation, std::string_view name,
                                   const mjbase::KeyPool& keys)
{
    for (const auto& [element, value] : annotation.elements) {
        if (keys.str(element) == name) return &value;
    }
    return nullptr;
}

TEST(MjAstDexAnnotationsTest, ReadsJavaValuesAndKeepsSystemAnnotationsInDex)
{
    auto dex = assembleDex(R"SMALI(
.class public LExample;
.super Ljava/lang/Object;
.annotation runtime Lannotations/Mark; items = {4, 7} type = [I
    marker = .subannotation Lannotations/Nested; label = "x\u0000\ud800" .end subannotation
    choice = .enum Lannotations/Mode;->FAST:Lannotations/Mode;
.end annotation
.annotation build Lannotations/Binary; .end annotation
.annotation system Ldalvik/annotation/EnclosingClass; value = null .end annotation
)SMALI");
    ASSERT_TRUE(dex);
    mjbase::KeyPool keys;
    auto result = readDexClassAnnotations(*dex, 0, keys);
    ASSERT_TRUE(result.is_ok());
    const auto declarations = std::move(result).unwrap();
    ASSERT_EQ(declarations.size(), 2u);
    const DeclarationAnnotation* mark = nullptr;
    for (const auto& declaration : declarations) {
        if (declaration.visibility == AnnotationVisibility::Runtime) mark = &declaration;
        else EXPECT_EQ(keys.str(declaration.annotation.descriptor), "Lannotations/Binary;");
    }
    ASSERT_NE(mark, nullptr);
    EXPECT_EQ(keys.str(mark->annotation.descriptor), "Lannotations/Mark;");

    const auto* items = findElement(mark->annotation, "items", keys);
    ASSERT_NE(items, nullptr);
    const auto* array = std::get_if<AnnotationValue::Array>(&items->value);
    ASSERT_NE(array, nullptr);
    ASSERT_EQ(array->size(), 2u);
    EXPECT_EQ(std::get<ca::i32>((*array)[0].value), 4);
    EXPECT_EQ(std::get<ca::i32>((*array)[1].value), 7);

    const auto* marker = findElement(mark->annotation, "marker", keys);
    ASSERT_NE(marker, nullptr);
    const auto* nested = std::get_if<AnnotationValue::Nested>(&marker->value);
    ASSERT_TRUE(nested && *nested);
    EXPECT_EQ(keys.str((*nested)->descriptor), "Lannotations/Nested;");
    const auto* label = findElement(**nested, "label", keys);
    ASSERT_NE(label, nullptr);
    const auto expectedLabel = std::u16string{u'x', 0, static_cast<char16_t>(0xd800)};
    EXPECT_EQ(std::get<std::u16string>(label->value), expectedLabel);

    const auto* choice = findElement(mark->annotation, "choice", keys);
    ASSERT_NE(choice, nullptr);
    const auto& enumKey = std::get<AnnotationEnum>(choice->value).constant;
    EXPECT_EQ(keys.str(enumKey.cls), "annotations/Mode");
    EXPECT_EQ(keys.str(enumKey.name), "FAST");
    EXPECT_EQ(keys.str(enumKey.desc), "Lannotations/Mode;");

    const auto* type = findElement(mark->annotation, "type", keys);
    ASSERT_NE(type, nullptr);
    EXPECT_EQ(keys.str(std::get<AnnotationClass>(type->value).descriptor), "[I");

    ASSERT_TRUE(dex->setClassAnnotations(0, {}).is_ok());
    auto cleared = readDexClassAnnotations(*dex, 0, keys);
    ASSERT_TRUE(cleared.is_ok());
    EXPECT_TRUE(std::move(cleared).unwrap().empty());
}

TEST(MjAstDexAnnotationsTest, ReportsNonJavaValuesInDeclarationAnnotations)
{
    auto dex = assembleDex(R"SMALI(
.class public LExample;
.super Ljava/lang/Object;
.annotation runtime Lannotations/Mark; value = null .end annotation
)SMALI");
    ASSERT_TRUE(dex);
    mjbase::KeyPool keys;
    auto result = readDexClassAnnotations(*dex, 0, keys);
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), AnnotationError::Unsupported);

    auto missing = readDexClassAnnotations(*dex, 1, keys);
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(missing.unwrap_err(), AnnotationError::InvalidTarget);
}

}   // namespace
