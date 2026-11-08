// JVMS 语义级 flag 规则门单测（issue #179）。
//
// 规则面与 oracle（JDK 17 HotSpot verify_legal_*_modifiers + <clinit>/接口
// <init> 特例）见 mjbase/jasm/semantic_flag_rules.hpp 头注。
//
// 夹具纪律（mjbase/doc/test-fixtures.rst）：合法形态一律 jasm 装配；非法
// flag 组合虽装配器可直装，仍按例外①路线从装配合法类出发做单点 flags 位
// 改写（shared/jasm_fixture.hpp 的 flipClassAccess/flipMemberAccess），改写
// 前后各一次 validate 自检——前自检即该规则合法侧的正例断言，非法直装会
// 绕过这道前自检，故不取。
#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "mjbase/jasm/jvm_jasm_service.hpp"

#include "shared/jasm_fixture.hpp"

namespace mjbase::jasm {

using namespace mjbase::test;   // Bytes / assembleJasm / flip* 夹具面

namespace {

// JVMS access_flags 位面（与 semantic_flag_rules.cpp 同表；测试侧只需改写
// 面，读面经诊断消息核对）。
constexpr ca::u16 kAccPublic       = 0x0001;
constexpr ca::u16 kAccPrivate      = 0x0002;
constexpr ca::u16 kAccProtected    = 0x0004;
constexpr ca::u16 kAccStatic       = 0x0008;
constexpr ca::u16 kAccFinal        = 0x0010;
constexpr ca::u16 kAccSuperOrSync  = 0x0020;
constexpr ca::u16 kAccVolOrBridge  = 0x0040;
constexpr ca::u16 kAccNative       = 0x0100;
constexpr ca::u16 kAccInterface    = 0x0200;
constexpr ca::u16 kAccAbstract     = 0x0400;
constexpr ca::u16 kAccStrict       = 0x0800;
constexpr ca::u16 kAccSynthetic    = 0x1000;
constexpr ca::u16 kAccAnnotation   = 0x2000;
constexpr ca::u16 kAccEnum         = 0x4000;
constexpr ca::u16 kAccModule       = 0x8000;

/// validate 须全绿：Ok 且零诊断（正例；失败把诊断逐条打出供直读）。
void expectAccepts(const Bytes& bytes, const char* what)
{
    JvmJasmService service;
    const auto validated = service.validate(bytes);
    if (!validated.isOk()) {
        for (const auto& diagnostic : validated.diagnostics) {
            ADD_FAILURE() << what << ": 意外诊断: "
                          << diagnostic.message.to_utf8_string().to_std_string();
        }
        return;
    }
    EXPECT_TRUE(validated.diagnostics.empty()) << what;
}

/// validate 须拒收（反例）：非 Ok，首个诊断 Error/Validate 且消息含期望片段
///（HotSpot 文本形态 + 位面十六进制，如 "0x100A"）。
void expectRejects(const Bytes& bytes, const char* what, std::string_view messagePart)
{
    JvmJasmService service;
    const auto validated = service.validate(bytes);
    EXPECT_FALSE(validated.isOk()) << what << ": 非法形态被放行";
    ASSERT_FALSE(validated.diagnostics.empty()) << what;
    EXPECT_EQ(DiagnosticSeverity::Error, validated.diagnostics.front().severity) << what;
    EXPECT_EQ(DiagnosticStage::Validate, validated.diagnostics.front().stage) << what;
    const std::string message
        = validated.diagnostics.front().message.to_utf8_string().to_std_string();
    EXPECT_NE(message.find(messagePart), std::string::npos)
        << what << ": 诊断不含「" << messagePart << "」：" << message;
}

/// 接口夹具（合法形态）：public interface abstract + 接口字段 public static
/// final K + public abstract compute。窗口参数化（49 前后分支、pre-49 窗）。
std::string interfaceSource(const std::string& internalName)
{
    return ".super java/lang/Object\n"
           ".class public interface abstract " + internalName + " {\n"
           "  .field public static final K I\n"
           "  .method public abstract compute ()I { }\n"
           "}\n";
}

Bytes assembleInterface(const std::string& internalName, ca::i32 major = 49)
{
    return test::assembleJasm(interfaceSource(internalName), major);
}

/// modern 接口夹具（major 52）：public static 与 private static 实体方法
/// + public abstract 声明——R11 现代分支的合法三态（方法序 answer/helper/
/// compute）。
Bytes assembleModernInterface()
{
    return test::assembleJasm(R"JASM(.super java/lang/Object
.class public interface abstract t/Mifx {
  .method public static answer ()I {
    code: {
      bipush 42
      ireturn
    }
  }
  .method private static helper ()I {
    code: {
      bipush 7
      ireturn
    }
  }
  .method public abstract compute ()I { }
}
)JASM", 52);
}

/// 空普通类（public super）。
Bytes assemblePlainClass(const std::string& internalName)
{
    return test::assembleClass(internalName, "java/lang/Object", "");
}

/// 仅 <init> 的普通类（R10 翻接口用——有 private 字段会先触发接口字段规则，
/// 抢占诊断首位）。
Bytes assembleInitOnlyClass()
{
    return test::assembleClass("t/WithInitOnly", "java/lang/Object",
        "  .method public <init> ()V {\n"
        "    parameters: { this },\n"
        "    code: {\n"
        "      aload this\n"
        "      invokespecial java/lang/Object.<init> ()V\n"
        "      return\n"
        "    }\n"
        "  }\n");
}

/// 带 <init> 的普通类（合法形态；方法序 init/getValue/count 字段序 0）。
Bytes assembleInitClass()
{
    return test::assembleClass("t/WithInit", "java/lang/Object",
        "  .field private count I\n"
        "  .method public <init> ()V {\n"
        "    parameters: { this },\n"
        "    code: {\n"
        "      aload this\n"
        "      invokespecial java/lang/Object.<init> ()V\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public getValue ()I {\n"
        "    parameters: { this },\n"
        "    code: {\n"
        "      aload this\n"
        "      getfield t/WithInit.count I\n"
        "      ireturn\n"
        "    }\n"
        "  }\n");
}

/// 抽象类夹具（合法形态）：public abstract super，可挂抽象方法。
Bytes assembleAbstractClass(const std::string& body = "")
{
    const std::string source = ".super java/lang/Object\n"
                               ".class public abstract super t/Abs {\n" + body + "}\n";
    return test::assembleJasm(source);
}

/// <clinit> 夹具：static <clinit> 实体（major 参数化——51 起强制 STATIC）。
Bytes assembleClinitClass(ca::i32 major = 52)
{
    return test::assembleJasm(R"JASM(.super java/lang/Object
.class public super t/Ci {
  .method static <clinit> ()V {
    code: {
      return
    }
  }
}
)JASM", major);
}

}   // namespace

// ── R1：ACC_MODULE（JVMS 4.1 模块类文件非类；HotSpot NoClassDefFoundError）──

TEST(MjSemanticFlagRulesTest, AccModuleClassIsRejected)
{
    const Bytes before = assemblePlainClass("t/Mod");
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "plain class");

    const Bytes flipped = test::flipClassAccess(before, kAccModule, 0);
    expectRejects(flipped, "class with ACC_MODULE", "ACC_MODULE is set");
}

// ── R2：ACC_ABSTRACT × ACC_FINAL 类面互斥（JVMS 4.1）──

TEST(MjSemanticFlagRulesTest, AbstractFinalClassIsRejected)
{
    const Bytes before = assembleAbstractClass();
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "abstract class");

    const Bytes flipped = test::flipClassAccess(before, kAccFinal, 0);
    expectRejects(flipped, "abstract final class", "Illegal class modifiers");
}

// ── R3：接口必须 ACC_ABSTRACT（JVMS 4.1-200）──

TEST(MjSemanticFlagRulesTest, InterfaceWithoutAbstractIsRejected)
{
    const Bytes before = assembleInterface("t/Ifx");
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "interface");

    const Bytes flipped = test::flipClassAccess(before, 0, kAccAbstract);
    expectRejects(flipped, "interface without ACC_ABSTRACT", "Illegal class modifiers");
}

// ── R4：接口自 major 49 起禁 ACC_SUPER/ACC_ENUM（JVMS 4.1 旗面）──

TEST(MjSemanticFlagRulesTest, InterfaceWithSuperOrEnumBitIsRejected)
{
    const Bytes before = assembleInterface("t/Ifx");
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "interface");

    const Bytes withSuper = test::flipClassAccess(before, kAccSuperOrSync, 0);
    expectRejects(withSuper, "interface with ACC_SUPER", "Illegal class modifiers");

    const Bytes withEnum = test::flipClassAccess(before, kAccEnum, 0);
    expectRejects(withEnum, "interface with ACC_ENUM", "Illegal class modifiers");
}

// ── R5：非 interface 不得 ACC_ANNOTATION；注解接口合法（JVMS 4.1）──

TEST(MjSemanticFlagRulesTest, AnnotationFlagFollowsInterfaceBoundary)
{
    const Bytes annotationInterface = test::flipClassAccess(assembleInterface("t/Ifx"),
                                                            kAccAnnotation, 0);
    ASSERT_FALSE(annotationInterface.empty());
    expectAccepts(annotationInterface, "annotation interface");

    const Bytes plain = assemblePlainClass("t/C");
    ASSERT_FALSE(plain.empty());

    const Bytes flipped = test::flipClassAccess(plain, kAccAnnotation, 0);
    expectRejects(flipped, "non-interface with ACC_ANNOTATION", "Illegal class modifiers");
}

// ── R6：接口字段必须 public static final 全置（issue #179 主案例面；
//        0x1019 过、0x100A/0x1009 拒，另加禁 private/protected/volatile/
//        transient 与 major 49 起的 ACC_ENUM）──

TEST(MjSemanticFlagRulesTest, InterfaceFieldPrivateStaticSyntheticIsRejected)
{
    const Bytes before = assembleInterface("t/Ifx");
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "interface with public static final field");

    // 0x0019 → 清 PUBLIC|FINAL、置 PRIVATE|SYNTHETIC = 0x100A（issue 实测
    // JDK 17.0.8 defineClass 拒收位面）。
    const Bytes flipped = test::flipMemberAccess(before, true, 0,
                                                 kAccPrivate | kAccSynthetic,
                                                 kAccPublic | kAccFinal);
    expectRejects(flipped, "interface field 0x100A", "0x100A");
}

TEST(MjSemanticFlagRulesTest, InterfaceFieldPublicStaticSyntheticIsRejected)
{
    const Bytes before = assembleInterface("t/Ifx");
    ASSERT_FALSE(before.empty());

    // 0x0019 → 清 FINAL、置 SYNTHETIC = 0x1009（缺 FINAL 即拒）。
    const Bytes flipped = test::flipMemberAccess(before, true, 0, kAccSynthetic, kAccFinal);
    expectRejects(flipped, "interface field 0x1009", "0x1009");
}

TEST(MjSemanticFlagRulesTest, InterfaceFieldLegalFormsPass)
{
    const Bytes plain = assembleInterface("t/Ifx");
    ASSERT_FALSE(plain.empty());
    expectAccepts(plain, "interface field 0x0019");

    // 0x1019（public static final synthetic，issue 实测过检位面）。
    const Bytes synthetic = test::flipMemberAccess(plain, true, 0, kAccSynthetic, 0);
    expectAccepts(synthetic, "interface field 0x1019");
}

TEST(MjSemanticFlagRulesTest, InterfaceFieldWithVolatileOrEnumBitIsRejected)
{
    const Bytes before = assembleInterface("t/Ifx");
    ASSERT_FALSE(before.empty());

    const Bytes withVolatile = test::flipMemberAccess(before, true, 0, kAccVolOrBridge, 0);
    expectRejects(withVolatile, "interface field with ACC_VOLATILE",
                  "Illegal field modifiers");

    const Bytes withEnum = test::flipMemberAccess(before, true, 0, kAccEnum, 0);
    expectRejects(withEnum, "interface field with ACC_ENUM", "Illegal field modifiers");
}

// ── R7：类字段可见性位互斥（JVMS 4.5 at most one）──

TEST(MjSemanticFlagRulesTest, ClassFieldVisibilityConflictIsRejected)
{
    const Bytes before = assembleInitClass();
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "class with private field");

    const Bytes flipped = test::flipMemberAccess(before, true, 0, kAccPublic, 0);
    expectRejects(flipped, "field with public+private", "Illegal field modifiers");
}

// ── R8：类字段 ACC_FINAL × ACC_VOLATILE 互斥（JVMS 4.5）──

TEST(MjSemanticFlagRulesTest, ClassFieldFinalVolatileIsRejected)
{
    const Bytes before = assembleInitClass();
    ASSERT_FALSE(before.empty());

    // 合法侧：volatile 字段（装配器方言无 volatile 修饰词，从合法 private
    // 字段单点置位构造，validate 前自检兜住该形态合法性）。
    const Bytes volatileField = test::flipMemberAccess(before, true, 0, kAccVolOrBridge, 0);
    expectAccepts(volatileField, "volatile field");

    const Bytes flipped
        = test::flipMemberAccess(before, true, 0, kAccVolOrBridge | kAccFinal, 0);
    expectRejects(flipped, "final volatile field", "Illegal field modifiers");
}

// ── R9：<clinit> 自 major 51 起必须 ACC_STATIC（JVMS 4.6；51 前豁免）──

TEST(MjSemanticFlagRulesTest, ClinitStaticFollowsMajor51Boundary)
{
    const Bytes before = assembleClinitClass();
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "static <clinit> at major 52");

    const Bytes flipped
        = test::flipMemberAccess(before, false, 0, 0, kAccStatic);
    expectRejects(flipped, "non-static <clinit> at major 52", "<clinit> is not static");

    // 51 前豁免窗：major 49 的非 static <clinit> 合法（HotSpot 掩蔽式回退）。
    const Bytes legacy = test::flipMemberAccess(assembleClinitClass(49), false, 0, 0,
                                                kAccStatic);
    expectAccepts(legacy, "non-static <clinit> at major 49");
}

// ── R10：接口不得声明 <init>（JVMS 4.6 / 2.9.1）──

TEST(MjSemanticFlagRulesTest, InterfaceCannotDeclareInit)
{
    const Bytes before = assembleInitOnlyClass();
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "class with <init>");

    // 类面翻 interface+abstract、同点清 SUPER（否则 R4 先响）——单类单偏差组，
    // 诊断应只剩 R10。
    const Bytes flipped
        = test::flipClassAccess(before, kAccInterface | kAccAbstract, kAccSuperOrSync);
    expectRejects(flipped, "interface with <init>", "cannot have a method named <init>");
}

// ── R11：接口方法 modern 窗（major >= 52；public/private 恰一，禁
//        protected/final/synchronized/native，abstract 加禁 private/static/
//        strict）──

TEST(MjSemanticFlagRulesTest, ModernInterfaceMethodLegalFormsPass)
{
    const Bytes before = assembleModernInterface();
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "modern interface with public/private static methods");
}

TEST(MjSemanticFlagRulesTest, ModernInterfaceMethodIllegalCombinationsAreRejected)
{
    const Bytes before = assembleModernInterface();
    ASSERT_FALSE(before.empty());

    // public static + abstract → abstract × static。
    const Bytes abstractStatic = test::flipMemberAccess(before, false, 0, kAccAbstract, 0);
    expectRejects(abstractStatic, "interface method abstract+static",
                  "illegal modifiers");

    // public abstract → private abstract（abstract × private）。
    const Bytes privateAbstract
        = test::flipMemberAccess(before, false, 2, kAccPrivate, kAccPublic);
    expectRejects(privateAbstract, "interface method private+abstract",
                  "illegal modifiers");

    // private static + public → public/private 双置（恰一破坏）。
    const Bytes bothVisibility
        = test::flipMemberAccess(before, false, 1, kAccPublic, 0);
    expectRejects(bothVisibility, "interface method public+private",
                  "illegal modifiers");

    // public abstract + native（except 列表）。
    const Bytes nativeAbstract = test::flipMemberAccess(before, false, 2, kAccNative, 0);
    expectRejects(nativeAbstract, "interface method native+abstract",
                  "illegal modifiers");

    // public static + final（except 列表）。
    const Bytes finalStatic = test::flipMemberAccess(before, false, 0, kAccFinal, 0);
    expectRejects(finalStatic, "interface method final+static", "illegal modifiers");

    // private static + synchronized（except 列表）。
    const Bytes synchronizedStatic
        = test::flipMemberAccess(before, false, 1, kAccSuperOrSync, 0);
    expectRejects(synchronizedStatic, "interface method synchronized+static",
                  "illegal modifiers");
}

TEST(MjSemanticFlagRulesTest, StrictInterfaceMethodFollowsMajor61Retirement)
{
    const Bytes before = assembleModernInterface();
    ASSERT_FALSE(before.empty());

    // major 52 ∈ [49,61)：abstract + strict 非法。
    const Bytes legacyStrict = test::flipMemberAccess(before, false, 2, kAccStrict, 0);
    expectRejects(legacyStrict, "interface method strict+abstract at major 52",
                  "illegal modifiers");

    // major 61 起 ACC_STRICT 退役：同位面放行。
    const std::string modernSource = R"JASM(.super java/lang/Object
.class public interface abstract t/Mifx61 {
  .method public abstract compute ()I { }
}
)JASM";
    const Bytes modern = test::assembleJasm(modernSource, 61);
    ASSERT_FALSE(modern.empty());
    const Bytes retiredStrict = test::flipMemberAccess(modern, false, 0, kAccStrict, 0);
    expectAccepts(retiredStrict, "interface method strict+abstract at major 61");
}

// ── R12：接口方法 49-51 窗（必须 public abstract；static/final/native/
//        synchronized/protected/private/strict 全禁）──

TEST(MjSemanticFlagRulesTest, LegacyInterfaceMethodWindowIsEnforced)
{
    const Bytes before = assembleInterface("t/Ifx");
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "interface method public abstract at major 49");

    const Bytes flipped = test::flipMemberAccess(before, false, 0, kAccStatic, 0);
    expectRejects(flipped, "interface method static+abstract at major 49",
                  "illegal modifiers");
}

// ── R13：接口方法 pre-49 窗（public abstract 必须；static/final/native 禁；
//        protected/private/synchronized/strict 不查——与 HotSpot 一致）──

TEST(MjSemanticFlagRulesTest, Pre49InterfaceMethodWindowIsEnforced)
{
    const Bytes before = assembleInterface("t/Ifx45", 45);
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "interface method public abstract at major 45");

    const Bytes flipped = test::flipMemberAccess(before, false, 0, kAccStatic, 0);
    expectRejects(flipped, "interface method static+abstract at major 45",
                  "illegal modifiers");

    // pre-49 窗不查 synchronized：同点置位放行（版本窗宽进面）。
    const Bytes tolerated = test::flipMemberAccess(before, false, 0, kAccSuperOrSync, 0);
    expectAccepts(tolerated, "interface method synchronized+abstract at major 45");
}

// ── R14：类方法可见性位互斥（JVMS 4.6 at most one）──

TEST(MjSemanticFlagRulesTest, ClassMethodVisibilityConflictIsRejected)
{
    const Bytes before = assembleInitClass();
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "class with public method");

    const Bytes flipped = test::flipMemberAccess(before, false, 1, kAccPrivate, 0);
    expectRejects(flipped, "method with public+private", "illegal modifiers");
}

// ── R15：类 <init> 修饰位面（禁 static/final/synchronized/native/abstract，
//        major 49 起禁 bridge）──

TEST(MjSemanticFlagRulesTest, InitMethodIllegalModifiersAreRejected)
{
    const Bytes before = assembleInitClass();
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "class with plain <init>");

    const Bytes flipped = test::flipMemberAccess(before, false, 0, kAccSuperOrSync, 0);
    expectRejects(flipped, "synchronized <init>", "illegal modifiers");
}

// ── R16：类 abstract 方法互斥面（final/native/private/static 恒禁；
//        synchronized/strict 自 major 49，strict 退役窗同 R11）──

TEST(MjSemanticFlagRulesTest, AbstractClassMethodIllegalCombinationsAreRejected)
{
    const Bytes before = assembleAbstractClass(
        "  .method public abstract compute ()I { }\n");
    ASSERT_FALSE(before.empty());
    expectAccepts(before, "class with abstract method");

    const Bytes withFinal = test::flipMemberAccess(before, false, 0, kAccFinal, 0);
    expectRejects(withFinal, "abstract final class method", "illegal modifiers");

    const Bytes withStatic = test::flipMemberAccess(before, false, 0, kAccStatic, 0);
    expectRejects(withStatic, "abstract static class method", "illegal modifiers");

    const Bytes withNative = test::flipMemberAccess(before, false, 0, kAccNative, 0);
    expectRejects(withNative, "abstract native class method", "illegal modifiers");
}

TEST(MjSemanticFlagRulesTest, StrictAbstractClassMethodFollowsRetirementWindow)
{
    // major 52：abstract + synchronized / strict 均非法。
    const std::string source52 = ".super java/lang/Object\n"
                                 ".class public abstract super t/Abs52 {\n"
                                 "  .method public abstract compute ()I { }\n"
                                 "}\n";
    const Bytes at52 = test::assembleJasm(source52, 52);
    ASSERT_FALSE(at52.empty());

    const Bytes withSync52 = test::flipMemberAccess(at52, false, 0, kAccSuperOrSync, 0);
    expectRejects(withSync52, "abstract synchronized method at major 52",
                  "illegal modifiers");

    const Bytes withStrict52 = test::flipMemberAccess(at52, false, 0, kAccStrict, 0);
    expectRejects(withStrict52, "abstract strict method at major 52",
                  "illegal modifiers");

    // major 61：strict 退役放行。
    const std::string source61 = ".super java/lang/Object\n"
                                 ".class public abstract super t/Abs61 {\n"
                                 "  .method public abstract compute ()I { }\n"
                                 "}\n";
    const Bytes at61 = test::assembleJasm(source61, 61);
    ASSERT_FALSE(at61.empty());
    const Bytes withStrict61 = test::flipMemberAccess(at61, false, 0, kAccStrict, 0);
    expectAccepts(withStrict61, "abstract strict method at major 61");
}

// ── Off 档契约：语义门随档位关断（直通零诊断）──

TEST(MjSemanticFlagRulesTest, OffModeSkipsFlagGate)
{
    const Bytes legal = assembleInterface("t/Ifx");
    ASSERT_FALSE(legal.empty());
    const Bytes illegal = test::flipMemberAccess(legal, true, 0, kAccPrivate | kAccSynthetic,
                                                 kAccPublic | kAccFinal);
    ASSERT_FALSE(illegal.empty());

    JvmJasmService   service;
    JvmJasmContext   context;
    context.validationMode = ValidationMode::Off;
    const auto validated = service.validate(illegal, context);
    EXPECT_TRUE(validated.isOk()) << "Off 档直通，flag 门不参与";
}

// ── 诊断不在时 relaxed 数据流照旧跑通（flag 门只加不替）──

TEST(MjSemanticFlagRulesTest, LegalClassStillReachesDataflowGate)
{
    const Bytes bytes = assembleModernInterface();
    ASSERT_FALSE(bytes.empty());
    expectAccepts(bytes, "modern interface (flag gate pass → dataflow gate pass)");
}

}   // namespace mjbase::jasm
