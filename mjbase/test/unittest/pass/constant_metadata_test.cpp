/// @file constant_metadata_test.cpp
/// @brief Constant folding retains source PCs required by Code metadata.
#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "jbc/core/attribute.hpp"
#include "mjbase/ast/jvm.hpp"
#include "mjbase/pass/constant_folder.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;

namespace {

template<class Edit>
Bytes withCodeMetadata(Bytes bytes, Edit edit)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{bytes});
    EXPECT_TRUE(created.is_ok());
    if (created.is_err()) return bytes;
    auto session = std::move(created).unwrap();
    auto& file = session->backing();
    for (auto& slot : file.methods[0].attrs) {
        if (!file.cp.utf8Equals(slot.nameCp, "Code")) continue;
        auto decoded = file.decodeAttr(slot);
        EXPECT_TRUE(decoded.is_ok());
        if (decoded.is_err()) return bytes;
        EXPECT_EQ(decoded.unwrap()->kind(), jbc::core::AttrKind::Code);
        if (decoded.unwrap()->kind() != jbc::core::AttrKind::Code) return bytes;
        edit(file, *static_cast<jbc::core::CodeAttr*>(slot.mutate()));
        auto written = jbc::core::writeClass(file);
        EXPECT_TRUE(written.is_ok());
        return written.is_ok() ? std::move(written).unwrap() : bytes;
    }
    ADD_FAILURE() << "Code attribute missing";
    return bytes;
}

}   // namespace

TEST(MjConstantMetadataTest, KeepsLineNumberInsideAFoldWindow)
{
    const auto input = assembleMethodClass("m", "()I", R"JASM(
      iconst_2
      iconst_3
      line 42
      iadd
      ireturn
)JASM");
    ASSERT_FALSE(input.empty());
    const auto original = inspect(input);
    const auto protectedPcs = mjbase::ast::jvmConstantFoldProtectedPcs(*original.session, *original.cu);
    ASSERT_TRUE(protectedPcs.has_value());
    EXPECT_EQ(protectedPcs->count(2), 1u);

    const auto result = runMethodPass(input, mjbase::pass::ConstantFolder{});
    EXPECT_EQ(result.out, input);
    EXPECT_TRUE(result.pa.areAllPreserved());
}

TEST(MjConstantMetadataTest, KeepsLocalVariableRangeEndInsideAFoldWindow)
{
    const auto input = withCodeMetadata(
        assembleMethodClass("m", "()I", "iconst_2\niconst_3\niadd\nireturn\n"),
        [](jbc::core::ClassFile& file, jbc::core::CodeAttr& code) {
            auto table = std::make_unique<jbc::core::LocalVariableTableAttr>();
            table->rows.push_back({0, 2, file.cp.addUtf8("unused"), file.cp.addUtf8("I"), 0});
            code.maxLocals = 1;
            code.attrs.push_back(jbc::core::AttrSlot::makeNew(
                file.cp.addUtf8("LocalVariableTable"), std::move(table)));
        });
    ASSERT_FALSE(input.empty());
    const auto original = inspect(input);
    const auto protectedPcs = mjbase::ast::jvmConstantFoldProtectedPcs(*original.session, *original.cu);
    ASSERT_TRUE(protectedPcs.has_value());
    EXPECT_EQ(protectedPcs->count(0), 1u);
    EXPECT_EQ(protectedPcs->count(2), 1u);
    const auto result = runMethodPass(input, mjbase::pass::ConstantFolder{});
    EXPECT_EQ(result.out, input);
    EXPECT_TRUE(result.pa.areAllPreserved());
}

TEST(MjConstantMetadataTest, AllowsLocalVariableRangeEndingAtCodeLength)
{
    const auto input = withCodeMetadata(
        assembleMethodClass("m", "()I", "iconst_2\niconst_3\niadd\nireturn\n"),
        [](jbc::core::ClassFile& file, jbc::core::CodeAttr& code) {
            auto table = std::make_unique<jbc::core::LocalVariableTableAttr>();
            table->rows.push_back({0, static_cast<ca::u32>(code.code.size()),
                                   file.cp.addUtf8("unused"), file.cp.addUtf8("I"), 0});
            code.maxLocals = 1;
            code.attrs.push_back(jbc::core::AttrSlot::makeNew(
                file.cp.addUtf8("LocalVariableTable"), std::move(table)));
        });
    const auto result = runMethodPass(input, mjbase::pass::ConstantFolder{});
    EXPECT_TRUE(result.pa.changed());
    const auto output = inspect(result.out);
    EXPECT_EQ(opsOf(output), (std::vector<jbc::core::Op>{jbc::core::Op::ICONST_5,
                                                       jbc::core::Op::IRETURN}));
}

TEST(MjConstantMetadataTest, RejectsOpaqueCodeMetadata)
{
    const auto input = withCodeMetadata(
        assembleMethodClass("m", "()I", "iconst_2\niconst_3\niadd\nireturn\n"),
        [](jbc::core::ClassFile& file, jbc::core::CodeAttr& code) {
            code.attrs.push_back(jbc::core::AttrSlot::makeNew(
                file.cp.addUtf8("CustomPcTable"),
                std::make_unique<jbc::core::RawAttr>(std::vector<ca::u8>{})));
        });
    const auto original = inspect(input);
    EXPECT_FALSE(mjbase::ast::jvmConstantFoldProtectedPcs(*original.session, *original.cu).has_value());
    const auto result = runMethodPass(input, mjbase::pass::ConstantFolder{});
    EXPECT_EQ(result.out, input);
    EXPECT_TRUE(result.pa.areAllPreserved());
}

TEST(MjConstantMetadataTest, CollectsStackMapDeltasAndUninitializedNewOffsets)
{
    const auto input = withCodeMetadata(
        assembleMethodClass("m", "()V", R"JASM(
      new java/lang/Object
      dup
      invokespecial java/lang/Object.<init> ()V
      pop
      return
)JASM"),
        [](jbc::core::ClassFile& file, jbc::core::CodeAttr& code) {
            using StackMap = jbc::core::StackMapTableAttr;
            auto table = std::make_unique<StackMap>();
            // The DUP at pc 3 sees one uninitialized object produced by NEW at pc 0.
            StackMap::Frame first;
            first.type = StackMap::Frame::kFull;
            first.offset = 3;
            first.stack.push_back({StackMap::VType::kUninitialized, 0});
            table->frames.push_back(std::move(first));
            // The constructor at pc 4 sees two copies. Its delta is zero, not pc 4.
            StackMap::Frame second;
            second.type = StackMap::Frame::kFull;
            second.offset = 0;
            second.stack.push_back({StackMap::VType::kUninitialized, 0});
            second.stack.push_back({StackMap::VType::kUninitialized, 0});
            table->frames.push_back(std::move(second));
            code.attrs.push_back(jbc::core::AttrSlot::makeNew(
                file.cp.addUtf8("StackMapTable"), std::move(table)));
        });
    ASSERT_FALSE(input.empty());
    expectValidateOk(input);
    const auto original = inspect(input);
    const auto protectedPcs = mjbase::ast::jvmConstantFoldProtectedPcs(*original.session, *original.cu);
    ASSERT_TRUE(protectedPcs.has_value());
    EXPECT_EQ(*protectedPcs, (std::unordered_set<ca::u32>{0, 3, 4}));
}

TEST(MjConstantMetadataTest, CollectsTypeAnnotationRangesAndInstructionOffsets)
{
    const auto input = withCodeMetadata(
        assembleMethodClass("m", "()V", R"JASM(
      iconst_0
      istore_0
      iconst_2
      iconst_3
      iadd
      istore_0
      aconst_null
      instanceof java/lang/Object
      pop
      return
)JASM"),
        [](jbc::core::ClassFile& file, jbc::core::CodeAttr& code) {
            using TypeAnnotations = jbc::core::TypeAnnotationsAttr;
            auto table = std::make_unique<TypeAnnotations>();
            TypeAnnotations::Annotation local;
            local.targetType = TypeAnnotations::kLocalVar;
            local.typeIndexCp = file.cp.addUtf8("Lt/Marker;");
            local.localVars.push_back({2, 2, 0});
            table->annotations.push_back(std::move(local));
            TypeAnnotations::Annotation resource;
            resource.targetType = TypeAnnotations::kLocalVarResource;
            resource.typeIndexCp = file.cp.addUtf8("Lt/Marker;");
            resource.localVars.push_back({2, static_cast<ca::u32>(code.code.size() - 2), 0});
            table->annotations.push_back(std::move(resource));
            TypeAnnotations::Annotation offset;
            offset.targetType = TypeAnnotations::kOffsetBase;
            offset.typeIndexCp = file.cp.addUtf8("Lt/Marker;");
            offset.offset = 7;
            table->annotations.push_back(std::move(offset));
            code.attrs.push_back(jbc::core::AttrSlot::makeNew(
                file.cp.addUtf8("RuntimeVisibleTypeAnnotations"), std::move(table)));
        });
    ASSERT_FALSE(input.empty());
    expectValidateOk(input);
    const auto original = inspect(input);
    const auto protectedPcs = mjbase::ast::jvmConstantFoldProtectedPcs(*original.session, *original.cu);
    ASSERT_TRUE(protectedPcs.has_value());
    EXPECT_EQ(*protectedPcs, (std::unordered_set<ca::u32>{2, 4, 7}));
    const auto result = runMethodPass(input, mjbase::pass::ConstantFolder{});
    EXPECT_EQ(result.out, input);
    EXPECT_TRUE(result.pa.areAllPreserved());
}

TEST(MjConstantMetadataTest, KeepsCharacterRangeInclusiveEndpointsInsideAFoldWindow)
{
    const auto input = withCodeMetadata(
        assembleMethodClass("m", "()I", "iconst_2\niconst_3\niadd\nireturn\n"),
        [](jbc::core::ClassFile& file, jbc::core::CodeAttr& code) {
            auto table = std::make_unique<jbc::core::CharacterRangeTableAttr>();
            table->rows.push_back({1, 2, 1024, 1030, 1});
            code.attrs.push_back(jbc::core::AttrSlot::makeNew(
                file.cp.addUtf8("CharacterRangeTable"), std::move(table)));
        });
    ASSERT_FALSE(input.empty());
    expectValidateOk(input);
    const auto original = inspect(input);
    const auto protectedPcs = mjbase::ast::jvmConstantFoldProtectedPcs(*original.session, *original.cu);
    ASSERT_TRUE(protectedPcs.has_value());
    EXPECT_EQ(*protectedPcs, (std::unordered_set<ca::u32>{1, 2}));
    const auto result = runMethodPass(input, mjbase::pass::ConstantFolder{});
    EXPECT_EQ(result.out, input);
    EXPECT_TRUE(result.pa.areAllPreserved());
}
