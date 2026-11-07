/// EnumNameRestorer 单测：枚举 clinit 泄漏真名的映射生成面。夹具按 javap
/// 亲验的 javac 17 枚举产物形状手拼（new/dup/ldc/ordinal/invokespecial/
/// putstatic 构造点 + $values 合成方法数组序 + values/valueOf 桥签名），
/// 覆盖：全形状恢复（字段/数组字段/合成方法三类映射）、合法名不动口径
/// （正名与合法短名两态）、拒绝面（非枚举/桥缺失/序数断档/双形状互证
/// 失败/数组形状缺失/窗口断裂）、幂等二跑与注册挂名。
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/enum_name_restorer.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublic    = 0x0001;
constexpr ca::u16 kAccSuper     = 0x0020;
constexpr ca::u16 kAccStatic    = 0x0008;
constexpr ca::u16 kAccFinal     = 0x0010;
constexpr ca::u16 kAccEnum      = 0x4000;
constexpr ca::u16 kAccSynthetic = 0x1000;

/// 枚举夹具规格：形状对齐 javac 17 产物（clinit 直线 putstatic + $values
/// 合成方法 + values/valueOf 桥）；可按用例调整混淆名/序数/形状开关。
struct EnumFixture
{
    std::string owner                   = "t/Color";
    std::array<std::string_view, 3> leaks    = {"RED", "GREEN", "BLUE"};
    std::array<std::string_view, 3> fields   = {"a-b", "c-d", "e-f"};
    std::array<ca::i32, 3>          ordinals = {0, 1, 2};
    std::string_view arrField            = "A-B";
    std::string_view valuesName          = "sv-z";
    bool             enumFlag      = true;
    bool             valuesBridge  = true;
    bool             valueOfBridge = true;
    bool             arrayShape    = true;   ///< false: clinit 无 $values 调用段
    bool             shuffleArray  = false;  ///< $values 体 getstatic 序与前两位对调
    bool             dropLastPoint = false;  ///< 第三构造点无 putstatic（窗口断裂弃点）
};

/// ordinal 装载：0..5 走 ICONST_n，>5 走 SIPUSH（夹具序数面够用）。
Bytes ordinalLoad(ca::i32 v)
{
    if (v >= 0 && v <= 5) return {static_cast<ca::u8>(0x03 + v)};
    return {0x11, static_cast<ca::u8>(v >> 8), static_cast<ca::u8>(v)};   // SIPUSH
}

Bytes u16Insn(ca::u8 op, ca::u16 cp)
{
    return {op, static_cast<ca::u8>(cp >> 8), static_cast<ca::u8>(cp)};
}

/// LDC（1 字节操作数；夹具 CP 条目数恒 < 256）。
Bytes ldc8(ca::u16 cp) { return {0x12, static_cast<ca::u8>(cp)}; }

/// 按规格手拼枚举类字节。
Bytes buildEnumClass(const EnumFixture& fx)
{
    const std::string constDesc  = "L" + fx.owner + ";";
    const std::string valuesDesc = "[" + constDesc;

    FixtureBuilder b(fx.owner);
    b.setSuper("java/lang/Enum");
    b.setClassAccess(kAccPublic | kAccSuper | (fx.enumFlag ? kAccEnum : 0));

    // 字段：三枚举常量（ACC_ENUM 位）+ 数组字段。
    for (const std::string_view f : fx.fields) {
        b.addField({f, constDesc, kAccPublic | kAccFinal | kAccStatic | kAccEnum, {}});
    }
    b.addField({fx.arrField, valuesDesc,
                kAccFinal | kAccStatic | kAccSynthetic | 0x0002 /*ACC_PRIVATE*/, {}});

    // <init>(Ljava/lang/String;I)V：aload_0; aload_1; iload_2; invokespecial
    // java/lang/Enum.<init>; return。
    {
        const ca::u16 enumInit =
            b.methodRef("java/lang/Enum", "<init>", "(Ljava/lang/String;I)V");
        Bytes body;
        appendBytes(body, {0x2A, 0x2B, 0x1C});
        appendBytes(body, u16Insn(0xB7, enumInit));
        appendBytes(body, {0xB1});
        b.addMethod({"<init>", "(Ljava/lang/String;I)V", std::move(body), 0x0002, {}, 3, 3});
    }
    // values/valueOf 桥（判据只查存在性；桩体 aconst_null; areturn）。
    if (fx.valuesBridge) {
        b.addMethod({"values", valuesDesc, {0x01, 0xB0}, 0x0019, {}, 1, 0});
    }
    if (fx.valueOfBridge) {
        b.addMethod({"valueOf", "(Ljava/lang/String;)" + constDesc, {0x01, 0xB0}, 0x0019, {}, 1,
                     0});
    }
    // 数组合成方法：iconst_3; anewarray; dup; iconst_k; getstatic; aastore ×3;
    // areturn。shuffleArray 时 getstatic 序前两位对调（互证失败面）。
    {
        const ca::u16 arrCls = b.cls(valuesDesc);
        const ca::u16 ref0   = b.fieldRef(fx.owner, fx.fields[0], constDesc);
        const ca::u16 ref1   = b.fieldRef(fx.owner, fx.fields[1], constDesc);
        const ca::u16 ref2   = b.fieldRef(fx.owner, fx.fields[2], constDesc);
        const ca::u16 seq0   = fx.shuffleArray ? ref1 : ref0;
        const ca::u16 seq1   = fx.shuffleArray ? ref0 : ref1;
        Bytes body;
        appendBytes(body, {0x06});               // ICONST_3
        appendBytes(body, u16Insn(0xBD, arrCls));   // ANEWARRAY
        auto element = [&](ca::u8 iconst, ca::u16 ref) {
            appendBytes(body, {0x59});               // DUP
            appendBytes(body, {iconst});             // ICONST_k
            appendBytes(body, u16Insn(0xB2, ref));   // GETSTATIC
            appendBytes(body, {0x53});               // AASTORE
        };
        element(0x03, seq0);
        element(0x04, seq1);
        element(0x05, ref2);
        appendBytes(body, {0xB0});   // ARETURN
        b.addMethod({fx.valuesName, valuesDesc, std::move(body),
                     kAccSynthetic | kAccStatic | 0x0002 /*ACC_PRIVATE*/, {}, 4, 0});
    }
    // <clinit>：三构造点（new/dup/ldc/ordinal/invokespecial/putstatic）+
    // $values 调用段 + return。dropLastPoint 时第三点无 putstatic。
    {
        const ca::u16 initCp = b.methodRef(fx.owner, "<init>", "(Ljava/lang/String;I)V");
        const ca::u16 arrMethodCp = b.methodRef(fx.owner, fx.valuesName, valuesDesc);
        const ca::u16 arrFieldCp  = b.fieldRef(fx.owner, fx.arrField, valuesDesc);
        Bytes           body;
        for (ca::usize k = 0; k < 3; ++k) {
            const ca::u16 newCp  = b.cls(fx.owner);
            const ca::u16 ldcCp  = b.strConst(fx.leaks[k]);
            const ca::u16 putCp  = b.fieldRef(fx.owner, fx.fields[k], constDesc);
            appendBytes(body, u16Insn(0xBB, newCp));      // NEW
            appendBytes(body, {0x59});                    // DUP
            appendBytes(body, ldc8(ldcCp));               // LDC
            appendBytes(body, ordinalLoad(fx.ordinals[k]));
            appendBytes(body, u16Insn(0xB7, initCp));     // INVOKESPECIAL
            if (!fx.dropLastPoint || k < 2) {
                appendBytes(body, u16Insn(0xB3, putCp));  // PUTSTATIC
            }
        }
        if (fx.arrayShape) {
            appendBytes(body, u16Insn(0xB8, arrMethodCp));   // INVOKESTATIC
            appendBytes(body, u16Insn(0xB3, arrFieldCp));    // PUTSTATIC
        }
        appendBytes(body, {0xB1});   // RETURN
        b.addMethod({"<clinit>", "()V", std::move(body), kAccStatic, {}, 4, 0});
    }
    return b.assemble();
}

/// 跑 pass 并返回容器 AM 上的映射（只读面：字节零改动由调用方断言）。
const mjbase::mapping::IntermediateMapping& runPass(mjbase::model::ResourceContainer& container,
                                                    mjbase::spi::GlobalAnalysisManager& gam)
{
    mjbase::pass::EnumNameRestorer pass;
    const mjbase::spi::PreservedAnalyses pa = pass.run(container, gam);
    EXPECT_TRUE(pa.areAllPreserved());
    return gam.getResult<mjbase::pass::EnumNameMappingAnalysis>(container);
}

/// 便捷载入：类字节入容器（URI 按内部名 + ".class"）。
void loadClass(mjbase::model::ResourceContainer& container, const EnumFixture& fx)
{
    addClassResource(container, (fx.owner + ".class").c_str(), buildEnumClass(fx));
}

/// 便捷取用：按 URI 取容器内 class 资源。
const mjbase::model::ClassResource* classResource(mjbase::model::ResourceContainer& container,
                                                  const std::string& uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

/// 便捷查询：字段映射新名；未命中返回空串。
std::string mappedField(const mjbase::mapping::IntermediateMapping& m, std::string_view owner,
                        std::string_view name, std::string_view desc)
{
    const auto mapped =
        m.getMappedFieldName(ca::str::Utf8StringRef::from_string_view(owner),
                             ca::str::Utf8StringRef::from_string_view(name),
                             ca::str::Utf8StringRef::from_string_view(desc));
    return std::string{static_cast<std::string_view>(mapped)};
}

std::string mappedMethod(const mjbase::mapping::IntermediateMapping& m, std::string_view owner,
                         std::string_view name, std::string_view desc)
{
    const auto mapped =
        m.getMappedMethodName(ca::str::Utf8StringRef::from_string_view(owner),
                              ca::str::Utf8StringRef::from_string_view(name),
                              ca::str::Utf8StringRef::from_string_view(desc));
    return std::string{static_cast<std::string_view>(mapped)};
}

std::string textOf(std::string_view v) { return std::string{v}; }

}   // namespace

// ─── 全形状恢复：三字段 + 数组字段 + 合成方法三类映射 ───────────────────
TEST(EnumNameRestorerTest, restoresIllegalObfuscatedNames)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    loadClass(container, fx);
    const Bytes inBytes = classResource(container, (fx.owner + ".class").c_str())->get();

    const auto& mapping = runPass(container, gam);

    const std::string constDesc  = "L" + fx.owner + ";";
    const std::string valuesDesc = "[" + constDesc;
    EXPECT_EQ(mappedField(mapping, fx.owner, "a-b", constDesc), "RED");
    EXPECT_EQ(mappedField(mapping, fx.owner, "c-d", constDesc), "GREEN");
    EXPECT_EQ(mappedField(mapping, fx.owner, "e-f", constDesc), "BLUE");
    EXPECT_EQ(mappedField(mapping, fx.owner, "A-B", valuesDesc), "$VALUES");
    EXPECT_EQ(mappedMethod(mapping, fx.owner, "sv-z", valuesDesc), "$values");
    // 无关查询不串线。
    EXPECT_TRUE(mappedField(mapping, fx.owner, "RED", constDesc).empty());
    EXPECT_TRUE(mapping.getClasses().empty());   // 类名映射零条
    // 字节零改动（只读件）。
    EXPECT_EQ(classResource(container, (fx.owner + ".class").c_str())->get(), inBytes);
}

// ─── 合法名不动：正名产物零映射 ─────────────────────────────────────────
TEST(EnumNameRestorerTest, legalCanonicalNamesUntouched)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    fx.fields     = {"RED", "GREEN", "BLUE"};
    fx.arrField   = "$VALUES";
    fx.valuesName = "$values";
    loadClass(container, fx);

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 合法名不动：合法短名混淆（a/b/c）零映射（映射面最小化口径）─────────
TEST(EnumNameRestorerTest, legalShortObfNamesUntouched)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    fx.fields     = {"a", "b", "c"};
    fx.arrField   = "v1";
    fx.valuesName = "sv1";
    loadClass(container, fx);

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 拒绝面：缺 ACC_ENUM 类锚 ──────────────────────────────────────────
TEST(EnumNameRestorerTest, nonEnumClassRejected)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    fx.enumFlag = false;
    loadClass(container, fx);

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 拒绝面：values 桥缺失 ─────────────────────────────────────────────
TEST(EnumNameRestorerTest, missingValuesBridgeRejected)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    fx.valuesBridge = false;
    loadClass(container, fx);

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 拒绝面：valueOf 桥缺失 ────────────────────────────────────────────
TEST(EnumNameRestorerTest, missingValueOfBridgeRejected)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    fx.valueOfBridge = false;
    loadClass(container, fx);

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 拒绝面：序数断档（0,1,3 非连续无缺）→ 整类拒 ───────────────────────
TEST(EnumNameRestorerTest, ordinalSequenceBrokenRejected)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    fx.ordinals = {0, 1, 3};
    loadClass(container, fx);

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 拒绝面：双形状互证失败（$values 体 getstatic 序错位）→ 整类拒 ───────
TEST(EnumNameRestorerTest, arrayOrderMismatchRejected)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    fx.shuffleArray = true;
    loadClass(container, fx);

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 拒绝面：$values 数组形状缺失（clinit 无调用段）→ 整类拒 ────────────
TEST(EnumNameRestorerTest, arrayShapeMissingRejected)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    fx.arrayShape = false;
    loadClass(container, fx);

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 拒绝面：构造点窗口断裂（第三点无 putstatic）→ 互证不齐整类拒 ───────
TEST(EnumNameRestorerTest, brokenWindowRejected)
{
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    fx.dropLastPoint = true;
    loadClass(container, fx);

    const auto& mapping = runPass(container, gam);
    EXPECT_TRUE(mapping.isEmpty());
}

// ─── 幂等二跑：同 GAM 二跑缓存命中恒等；独立 GAM 三跑逐条相等 ───────────
TEST(EnumNameRestorerTest, idempotentSecondRun)
{
    mjbase::model::ResourceContainer container;
    EnumFixture                      fx;
    loadClass(container, fx);

    const auto ownerRef = ca::str::Utf8StringRef::from_string_view(fx.owner);
    ca::str::Utf8StringArena           arena1;
    mjbase::spi::GlobalAnalysisManager gam1(arena1);
    const auto&                        first = runPass(container, gam1);
    const auto* firstFields = first.getClassFieldMappings(ownerRef);
    ASSERT_NE(firstFields, nullptr);
    const ca::usize firstCount = firstFields->size();

    const auto& second = runPass(container, gam1);
    EXPECT_EQ(second.getClassFieldMappings(ownerRef), firstFields);   // 缓存命中同址

    ca::str::Utf8StringArena           arena2;
    mjbase::spi::GlobalAnalysisManager gam2(arena2);
    const auto&                        third = runPass(container, gam2);
    const auto* thirdFields = third.getClassFieldMappings(ownerRef);
    ASSERT_NE(thirdFields, nullptr);
    ASSERT_EQ(thirdFields->size(), firstCount);
    for (ca::usize k = 0; k < firstCount; ++k) {
        EXPECT_EQ(textOf(thirdFields->at(k).oldName), textOf(firstFields->at(k).oldName));
        EXPECT_EQ(textOf(thirdFields->at(k).newName), textOf(firstFields->at(k).newName));
        EXPECT_EQ(textOf(thirdFields->at(k).desc), textOf(firstFields->at(k).desc));
    }
}

// ─── 注册挂名面：名字可解析且产出本 pass ───────────────────────────────
TEST(EnumNameRestorerTest, registrableByName)
{
    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass("EnumNameRestorer");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
