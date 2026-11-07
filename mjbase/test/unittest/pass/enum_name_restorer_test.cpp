/// EnumNameRestorer 单测：枚举 clinit 泄漏真名的映射生成面。夹具一律 jasm
/// 文本装配（JvmJasmService；汇编器自带 frame 分析与验证，栈形不可验证或
/// 名字不合法直接打回——夹具错误不静默），形状对齐 javap 亲验的 javac 17
/// 枚举产物（构造点 ldc + $values 合成方法数组序 + values/valueOf 桥签名），
/// 覆盖：全形状恢复（字段/数组字段/合成方法三类映射）、合法名不动口径
/// （正名与合法短名两态）、拒绝面（非枚举/桥缺失/序数断档/双形状互证
/// 失败/数组形状缺失/窗口断裂）、幂等二跑、形状契约探测与注册挂名。
#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/enum_name_restorer.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

namespace {

using Bytes = std::vector<ca::u8>;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;

/// UTF-8 文本 → UTF-16（jasm 源入口口径，renamer_fixture 同式）。
ca::str::Utf16String utf16Of(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 源码 → class 字节（汇编失败逐条报诊断并致命，夹具错误不静默；
/// 失败时附整份源码与行号定位，供 CI 日志直读）。
Bytes assembleJasm(const std::string& source)
{
    mjbase::jasm::JvmJasmService service;
    const auto           sourceName = utf16Of("enum_name_restorer_fixture.jasm");
    const auto           text       = utf16Of(source);
    const auto           result     = service.assemble(sourceName.ref(), text.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            std::string where;
            if (d.location.has_value()) {
                where = " [line " + std::to_string(d.location->line) + " col "
                        + std::to_string(d.location->column) + "]";
            }
            ADD_FAILURE() << "jasm" << where << ": "
                          << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败，源码如下：\n" << source;
        return {};
    }
    return result.value->bytes;
}

/// 枚举夹具规格：形状对齐 javac 17 产物（clinit 直线 putstatic + $values
/// 合成方法 + values/valueOf 桥）；可按用例调整混淆名/序数/形状开关。
struct EnumFixture
{
    std::string                     owner    = "t/Color";
    std::array<std::string, 3>      leaks    = {"RED", "GREEN", "BLUE"};
    std::array<std::string, 3>      fields   = {"a-b", "c-d", "e-f"};
    std::array<ca::i32, 3>          ordinals = {0, 1, 2};
    std::string                     arrField = "A-B";
    std::string                     valuesName = "sv-z";
    bool                            enumFlag      = true;
    bool                            valuesBridge  = true;
    bool                            valueOfBridge = true;
    bool                            arrayShape    = true;   ///< false: clinit 无 $values 调用段
    bool                            shuffleArray  = false;  ///< $values 体 getstatic 序前两位对调
    bool                            dropLastPoint = false;  ///< 第三构造点无 putstatic（窗口断裂弃点）
};

/// ordinal 装载文本：0..5 走 iconst_n，字节域走 bipush，更宽走 sipush。
std::string ordinalLoad(ca::i32 v)
{
    if (v >= 0 && v <= 5) return "iconst_" + std::to_string(v);
    if (v >= -128 && v <= 127) return "bipush " + std::to_string(v);
    return "sipush " + std::to_string(v);
}

/// 按规格生成枚举类 jasm 源（声明序：三常量字段 + 数组字段 + <init> +
/// values/valueOf 桥 + 数组合成方法 + <clinit>）。
std::string enumJasmSource(const EnumFixture& fx)
{
    const std::string owner      = fx.owner;
    const std::string constDesc  = "L" + owner + ";";
    const std::string valuesDesc = "[" + constDesc;

    std::string s;
    s += ".super java/lang/Enum\n";
    s += ".class " + std::string(fx.enumFlag ? "public super final enum" : "public super final")
         + " " + owner + " {\n";

    for (const std::string& f : fx.fields) {
        s += "  .field public static final enum " + f + " " + constDesc + "\n";
    }
    s += "  .field private static final synthetic " + fx.arrField + " " + valuesDesc + "\n";

    // <init>(Ljava/lang/String;I)V：super 构造转发（javac 生成形状；槽位
    // 0/1/2 即 this/name/ordinal，帧分析按描述符推导，无需 parameters 声明）。
    s += "  .method private <init> (Ljava/lang/String;I)V {\n    code: {\n";
    s += "      aload_0\n      aload_1\n      iload_2\n";
    s += "      invokespecial java/lang/Enum.<init> (Ljava/lang/String;I)V\n      return\n";
    s += "    }\n  }\n";

    // values/valueOf 桥（判据只查存在性；桩体 aconst_null 收口）。
    if (fx.valuesBridge) {
        s += "  .method public static values " + valuesDesc + " {\n    code: {\n";
        s += "      aconst_null\n      areturn\n    }\n  }\n";
    }
    if (fx.valueOfBridge) {
        s += "  .method public static valueOf (Ljava/lang/String;)" + constDesc
             + " {\n    code: {\n      aconst_null\n      areturn\n    }\n  }\n";
    }

    // 数组合成方法：iconst_3; anewarray; {dup; iconst_k; getstatic; aastore}×3;
    // areturn。shuffleArray 时前两位 getstatic 对调（互证失败面）。
    s += "  .method private static synthetic " + fx.valuesName + " " + valuesDesc
         + " {\n    code: {\n";
    s += "      iconst_3\n      anewarray " + owner + "\n";
    const std::array<std::string, 3> seq =
        fx.shuffleArray ? std::array<std::string, 3>{fx.fields[1], fx.fields[0], fx.fields[2]}
                        : std::array<std::string, 3>{fx.fields[0], fx.fields[1], fx.fields[2]};
    for (ca::usize k = 0; k < 3; ++k) {
        s += "      dup\n      " + ordinalLoad(static_cast<ca::i32>(k)) + "\n"
             + "      getstatic " + owner + "." + seq[k] + " " + constDesc + "\n"
             + "      aastore\n";
    }
    s += "      areturn\n    }\n  }\n";

    // <clinit>：三构造点（new/dup/ldc/ordinal/invokespecial/putstatic）+
    // $values 调用段 + return。dropLastPoint 时第三点无 putstatic。
    s += "  .method static <clinit> ()V {\n    code: {\n";
    for (ca::usize k = 0; k < 3; ++k) {
        s += "      new " + owner + "\n      dup\n";
        s += "      ldc \"" + fx.leaks[k] + "\"\n";
        s += "      " + ordinalLoad(fx.ordinals[k]) + "\n";
        s += "      invokespecial " + owner + ".<init> (Ljava/lang/String;I)V\n";
        if (!fx.dropLastPoint || k < 2) {
            s += "      putstatic " + owner + "." + fx.fields[k] + " " + constDesc + "\n";
        }
    }
    if (fx.arrayShape) {
        s += "      invokestatic " + owner + "." + fx.valuesName + " " + valuesDesc + "\n";
        s += "      putstatic " + owner + "." + fx.arrField + " " + valuesDesc + "\n";
    }
    s += "      return\n    }\n  }\n";

    s += "}\n";
    return s;
}

/// 按规格装配枚举类字节。
Bytes buildEnumClass(const EnumFixture& fx) { return assembleJasm(enumJasmSource(fx)); }

void addClassResource(ResourceContainer& container, const std::string& uri, const Bytes& bytes)
{
    container.addResource(uri, std::make_unique<ClassResource>(uri, bytes));
}

/// 便捷载入：装配并按内部名 + ".class" 入容器。
void loadClass(ResourceContainer& container, const EnumFixture& fx)
{
    addClassResource(container, fx.owner + ".class", buildEnumClass(fx));
}

/// 跑 pass 并返回容器 AM 上的映射（只读面：字节零改动由调用方断言）。
const mjbase::mapping::IntermediateMapping& runPass(ResourceContainer&                  container,
                                                    mjbase::spi::GlobalAnalysisManager& gam)
{
    mjbase::pass::EnumNameRestorer                     pass;
    const mjbase::spi::PreservedAnalyses pa = pass.run(container, gam);
    EXPECT_TRUE(pa.areAllPreserved());
    return gam.getResult<mjbase::pass::EnumNameMappingAnalysis>(container);
}

/// 便捷取用：按 URI 取容器内 class 资源。
const ClassResource* classResource(ResourceContainer& container, const std::string& uri)
{
    return ca::core::tag_cast::dyn_cast<const ClassResource*>(container.getResource(uri));
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
    ResourceContainer                  container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EnumFixture                        fx;
    loadClass(container, fx);
    const Bytes inBytes = classResource(container, fx.owner + ".class")->get();

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
    EXPECT_EQ(classResource(container, fx.owner + ".class")->get(), inBytes);
}

// ─── 合法名不动：正名产物零映射 ─────────────────────────────────────────
TEST(EnumNameRestorerTest, legalCanonicalNamesUntouched)
{
    ResourceContainer                  container;
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
    ResourceContainer                  container;
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
    ResourceContainer                  container;
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
    ResourceContainer                  container;
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
    ResourceContainer                  container;
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
    ResourceContainer                  container;
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
    ResourceContainer                  container;
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
    ResourceContainer                  container;
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
    ResourceContainer                  container;
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
    ResourceContainer container;
    EnumFixture       fx;
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

// ─── 形状契约探测：jasm 装配夹具经会话 promote 后的判据上游逐拍断言 ────
// （判据读取的每个语义视图——op 序 / memberOf / constOf / classOf /
//  findMethod / 字段 access——在此钉死；判据产出异常时先看本用例。）
TEST(EnumNameRestorerTest, clinitShapeProbe)
{
    ResourceContainer container;
    EnumFixture       fx;
    loadClass(container, fx);

    const ClassResource* res = classResource(container, fx.owner + ".class");
    ASSERT_NE(res, nullptr);
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(res->get()));
    ASSERT_TRUE(sessionResult.is_ok()) << "session open failed";
    auto                   session  = std::move(sessionResult).unwrap();
    jbc::ir::ClassUnit&    unit     = session->unit();
    const std::string_view thisName = unit.symbols().str(unit.thisClass);
    EXPECT_EQ(thisName, "t/Color");
    EXPECT_NE(session->backing().access & 0x4000, 0) << "ACC_ENUM lost";

    const std::string constDesc  = "L" + std::string(thisName) + ";";
    const std::string valuesDesc = "[" + constDesc;

    // 桥与字段面。
    EXPECT_NE(unit.findMethod("values", valuesDesc), jbc::ir::kNoMethod) << "values bridge";
    EXPECT_NE(unit.findMethod("valueOf", "(Ljava/lang/String;)" + constDesc), jbc::ir::kNoMethod)
        << "valueOf bridge";
    int enumFields = 0;
    for (const auto& f : unit.fields) {
        const std::string fname = std::string{unit.symbols().str(f.name)};
        if (unit.symbols().str(f.desc) == constDesc && (f.access & 0x4000) != 0) ++enumFields;
    }
    EXPECT_EQ(enumFields, 3) << "ACC_ENUM constant fields";

    // clinit 指令序逐拍：三构造点各 6 拍 + INVOKESTATIC + PUTSTATIC + RETURN。
    const ca::usize clinitIdx = unit.findMethod("<clinit>", "()V");
    ASSERT_NE(clinitIdx, jbc::ir::kNoMethod);
    ASSERT_TRUE(jbc::ir::hasCode(*session, clinitIdx));
    auto promoted = jbc::ir::promoteMethod(*session, clinitIdx);
    ASSERT_TRUE(promoted.is_ok()) << "promote clinit failed";
    auto cuRef = std::move(promoted).unwrap();
    const auto order = cuRef->dom.order();
    ASSERT_EQ(order.size(), 21u) << "clinit insn count";

    auto opAt = [&](ca::usize i) {
        const jbc::ir::Insn* in = cuRef->dom.peek(order[i]);
        return in == nullptr ? jbc::core::Op::NOP : in->op;
    };
    EXPECT_EQ(opAt(0), jbc::core::Op::NEW);
    {
        const jbc::ir::Insn* in = cuRef->dom.peek(order[0]);
        EXPECT_EQ(std::string{session->classOf(*in)}, "t/Color") << "NEW classOf";
    }
    EXPECT_EQ(opAt(1), jbc::core::Op::DUP);
    {
        const jbc::ir::Insn* in = cuRef->dom.peek(order[2]);
        ASSERT_TRUE(in->op == jbc::core::Op::LDC || in->op == jbc::core::Op::LDC_W);
        jbc::ir::ClassSession::ConstSem c;
        ASSERT_TRUE(session->constOf(*in, c)) << "ldc constOf";
        ASSERT_EQ(c.kind, jbc::ir::ClassSession::ConstSem::Kind::String);
        EXPECT_EQ(std::string{c.str}, "RED") << "leak name";
    }
    EXPECT_EQ(opAt(3), jbc::core::Op::ICONST_0);
    {
        const jbc::ir::Insn* in = cuRef->dom.peek(order[4]);
        ASSERT_EQ(in->op, jbc::core::Op::INVOKESPECIAL);
        jbc::ir::ClassSession::MemberSem ref;
        ASSERT_TRUE(session->memberOf(*in, ref)) << "invokespecial memberOf";
        EXPECT_EQ(std::string{ref.owner}, "t/Color");
        EXPECT_EQ(std::string{ref.name}, "<init>");
        EXPECT_EQ(std::string{ref.desc}, "(Ljava/lang/String;I)V");
    }
    {
        const jbc::ir::Insn* in = cuRef->dom.peek(order[5]);
        ASSERT_EQ(in->op, jbc::core::Op::PUTSTATIC);
        jbc::ir::ClassSession::MemberSem ref;
        ASSERT_TRUE(session->memberOf(*in, ref)) << "putstatic memberOf";
        EXPECT_EQ(std::string{ref.owner}, "t/Color");
        EXPECT_EQ(std::string{ref.name}, "a-b");
        EXPECT_EQ(std::string{ref.desc}, constDesc);
    }
    {
        const jbc::ir::Insn* in = cuRef->dom.peek(order[18]);
        ASSERT_EQ(in->op, jbc::core::Op::INVOKESTATIC);
        jbc::ir::ClassSession::MemberSem ref;
        ASSERT_TRUE(session->memberOf(*in, ref)) << "invokestatic memberOf";
        EXPECT_EQ(std::string{ref.owner}, "t/Color");
        EXPECT_EQ(std::string{ref.name}, "sv-z");
        EXPECT_EQ(std::string{ref.desc}, valuesDesc);
    }
    {
        const jbc::ir::Insn* in = cuRef->dom.peek(order[19]);
        ASSERT_EQ(in->op, jbc::core::Op::PUTSTATIC);
        jbc::ir::ClassSession::MemberSem ref;
        ASSERT_TRUE(session->memberOf(*in, ref)) << "arr putstatic memberOf";
        EXPECT_EQ(std::string{ref.name}, "A-B");
        EXPECT_EQ(std::string{ref.desc}, valuesDesc);
    }
    EXPECT_EQ(opAt(20), jbc::core::Op::RETURN);

    // 数组方法体 getstatic 序。
    const ca::usize valuesIdx = unit.findMethod("sv-z", valuesDesc);
    ASSERT_NE(valuesIdx, jbc::ir::kNoMethod);
    ASSERT_TRUE(jbc::ir::hasCode(*session, valuesIdx));
    auto vp = jbc::ir::promoteMethod(*session, valuesIdx);
    ASSERT_TRUE(vp.is_ok()) << "promote $values failed";
    auto                      vcu = std::move(vp).unwrap();
    std::vector<std::string>  gs;
    for (const jbc::ir::InsnId id : vcu->dom.order()) {
        const jbc::ir::Insn* in = vcu->dom.peek(id);
        if (in == nullptr || in->op != jbc::core::Op::GETSTATIC) continue;
        jbc::ir::ClassSession::MemberSem ref;
        if (!session->memberOf(*in, ref)) continue;
        gs.emplace_back(ref.name);
    }
    ASSERT_EQ(gs.size(), 3u) << "array getstatic count";
    EXPECT_EQ(gs[0], "a-b");
    EXPECT_EQ(gs[1], "c-d");
    EXPECT_EQ(gs[2], "e-f");
}

// ─── 注册挂名面：名字可解析且产出本 pass ───────────────────────────────
TEST(EnumNameRestorerTest, registrableByName)
{
    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass("EnumNameRestorer");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
