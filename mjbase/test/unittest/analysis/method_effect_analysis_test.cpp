/// @file        method_effect_analysis_test.cpp
/// @brief       analysis 方法副作用模型测试：局部效应分类、callee 效应传播、
///              JDK 白名单与未知外部调用。
///
/// 夹具字节化重写，断言逐字等强。
#include <gtest/gtest.h>

#include "jbc/core/opcode.hpp"

#include "class_bytes_builder.hpp"
#include "mjbase/analysis/method_effect_analysis.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

using jbc::core::Op;
using mjbase::analysis::ConstantReturnKind;
using mjbase::analysis::MethodEffectKind;
using mjbase::analysis::MethodEffectModel;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::FieldSpec;
using mjbase::analysis::test_support::MethodSpec;

namespace {

/// buildEffectClass 的字节化形态：一个静态 int 字段 counter +
/// 纯/读/写/常量返回体与各形态调用方。
std::vector<FieldSpec> effectFixtureFields()
{
    return {FieldSpec{0x000A, "counter", "I"}};   // private static
}

MethodSpec method(std::string_view name, std::string_view desc, ca::u16 maxStack,
                  ca::u16 maxLocals, std::function<void(CodeEmit&)> emit)
{
    MethodSpec m;
    m.name      = name;
    m.desc      = desc;
    m.maxStack  = maxStack;
    m.maxLocals = maxLocals;
    m.emit      = std::move(emit);
    return m;
}

std::vector<MethodSpec> effectFixtureMethods()
{
    return {
        method("constFive", "()I", 1, 0, [](CodeEmit& e) {
            e.i32const(5);
            e.op(Op::IRETURN);
        }),
        method("constLong", "()J", 2, 0, [](CodeEmit& e) {
            e.ldcLong(1234567890123LL);
            e.op(Op::LRETURN);
        }),
        method("constText", "()Ljava/lang/String;", 1, 0, [](CodeEmit& e) {
            e.ldcString("stable");
            e.op(Op::ARETURN);
        }),
        method("readCounter", "()I", 1, 0, [](CodeEmit& e) {
            e.fieldInsn(Op::GETSTATIC, "EffectFixture", "counter", "I");
            e.op(Op::IRETURN);
        }),
        method("writeCounter", "()V", 1, 0, [](CodeEmit& e) {
            e.i32const(1);
            e.fieldInsn(Op::PUTSTATIC, "EffectFixture", "counter", "I");
            e.op(Op::RETURN);
        }),
        method("pureCaller", "()I", 1, 0, [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "EffectFixture", "constFive", "()I");
            e.op(Op::IRETURN);
        }),
        method("longCaller", "()J", 2, 0, [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "EffectFixture", "constLong", "()J");
            e.op(Op::LRETURN);
        }),
        method("textCaller", "()Ljava/lang/String;", 1, 0, [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "EffectFixture", "constText",
                     "()Ljava/lang/String;");
            e.op(Op::ARETURN);
        }),
        method("readCaller", "()I", 1, 0, [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "EffectFixture", "readCounter", "()I");
            e.op(Op::IRETURN);
        }),
        method("impureCaller", "()V", 0, 0, [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "EffectFixture", "writeCounter", "()V");
            e.op(Op::RETURN);
        }),
        method("mathAbs", "(I)I", 1, 1, [](CodeEmit& e) {
            e.varInsn(Op::ILOAD, 0);
            e.invoke(Op::INVOKESTATIC, "java/lang/Math", "abs", "(I)I");
            e.op(Op::IRETURN);
        }),
        method("unknownExternal", "()V", 0, 0, [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "Missing", "touch", "()V");
            e.op(Op::RETURN);
        }),
        method("unknownMath", "()V", 0, 0, [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "java/lang/Math", "sideEffect", "()V");
            e.op(Op::RETURN);
        }),
        method("wrongMathDescriptor", "()I", 1, 0, [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "java/lang/Math", "abs", "()I");
            e.op(Op::IRETURN);
        }),
        method("wrongStringDescriptor", "(Ljava/lang/String;)I", 2, 1, [](CodeEmit& e) {
            e.varInsn(Op::ALOAD, 0);
            e.invoke(Op::INVOKEVIRTUAL, "java/lang/String", "length", "(I)I");
            e.op(Op::IRETURN);
        }),
    };
}

mjbase::model::ResourceContainer buildEffectFixture()
{
    mjbase::model::ResourceContainer container;
    container.addResource("EffectFixture.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "EffectFixture.class",
                              mjbase::analysis::test_support::assembleClass(
                                  "EffectFixture", effectFixtureFields(),
                                  effectFixtureMethods())));
    return container;
}

}   // namespace

TEST(MjAnalysisMethodEffectTest, ClassifiesLocalPureReadonlyAndImpureBodies)
{
    auto container = buildEffectFixture();

    MethodEffectModel model;
    model.build(container);

    EXPECT_EQ(model.effectKindOf("EffectFixture", "constFive", "()I"), MethodEffectKind::Pure);
    EXPECT_EQ(model.effectKindOf("EffectFixture", "readCounter", "()I"),
              MethodEffectKind::ReadOnly);
    EXPECT_EQ(model.effectKindOf("EffectFixture", "writeCounter", "()V"),
              MethodEffectKind::Impure);
    EXPECT_TRUE(model.isPure("EffectFixture", "constFive", "()I"));
    EXPECT_TRUE(model.isReadOnlyOrPure("EffectFixture", "readCounter", "()I"));
    EXPECT_EQ(model.constantReturnOf("EffectFixture", "constFive", "()I").kind,
              ConstantReturnKind::Int);
    EXPECT_EQ(model.constantReturnOf("EffectFixture", "constFive", "()I").intValue, 5);
    EXPECT_EQ(model.constantReturnOf("EffectFixture", "constLong", "()J").kind,
              ConstantReturnKind::Long);
    EXPECT_EQ(model.constantReturnOf("EffectFixture", "constLong", "()J").intValue,
              1234567890123LL);
    EXPECT_EQ(
        model.constantReturnOf("EffectFixture", "constText", "()Ljava/lang/String;").stringValue,
        "stable");
}

TEST(MjAnalysisMethodEffectTest, PropagatesDeclaredCalleeEffects)
{
    auto container = buildEffectFixture();

    MethodEffectModel model;
    model.build(container);

    EXPECT_EQ(model.effectKindOf("EffectFixture", "pureCaller", "()I"), MethodEffectKind::Pure);
    EXPECT_EQ(model.effectKindOf("EffectFixture", "readCaller", "()I"),
              MethodEffectKind::ReadOnly);
    EXPECT_EQ(model.effectKindOf("EffectFixture", "impureCaller", "()V"),
              MethodEffectKind::Impure);
    EXPECT_EQ(model.constantReturnOf("EffectFixture", "pureCaller", "()I").intValue, 5);
    EXPECT_EQ(model.constantReturnOf("EffectFixture", "longCaller", "()J").intValue,
              1234567890123LL);
    EXPECT_EQ(
        model.constantReturnOf("EffectFixture", "textCaller", "()Ljava/lang/String;").stringValue,
        "stable");
    EXPECT_EQ(model.constantReturnOf("EffectFixture", "readCaller", "()I").kind,
              ConstantReturnKind::Unknown);
}

TEST(MjAnalysisMethodEffectTest, UsesTinyJdkWhitelistAndKeepsUnknownExternalCalls)
{
    auto container = buildEffectFixture();

    MethodEffectModel model;
    model.build(container);

    EXPECT_EQ(model.effectKindOf("EffectFixture", "mathAbs", "(I)I"), MethodEffectKind::Pure);
    EXPECT_EQ(model.effectKindOf("EffectFixture", "unknownExternal", "()V"),
              MethodEffectKind::Unknown);
    EXPECT_EQ(model.effectKindOf("EffectFixture", "unknownMath", "()V"),
              MethodEffectKind::Unknown);
    EXPECT_EQ(model.effectKindOf("EffectFixture", "wrongMathDescriptor", "()I"),
              MethodEffectKind::Unknown);
    EXPECT_EQ(model.effectKindOf("EffectFixture", "wrongStringDescriptor",
                                 "(Ljava/lang/String;)I"),
              MethodEffectKind::Unknown);
    EXPECT_STREQ(mjbase::analysis::methodEffectKindName(MethodEffectKind::ReadOnly), "ReadOnly");
}
