/// EnumNameRestorer 在 javac 真产物上的硬验收：javac 编译枚举样本 →
/// mjbase remapper 面按映射把常量字段/数组字段/合成方法改成非法名（等价
/// 混淆器行为：ldc 真名不动、指令序不变）→ 本 pass 产出的映射必须恰好
/// 反演该混淆 → 恢复态经 remapper 应用后字段名与原样本一致且真 JVM 可
/// 运行输出一致。JDK 不可用的机器整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/asm/remapper/member_remapper.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/enum_name_restorer.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<ca::u8>;

/// 三常量枚举样本（javac 现代形状：clinit 直线 putstatic + $values 合成
/// 方法 + values/valueOf 桥）。
constexpr const char* kEnumSrc = R"JAVA(
package hb.jvm.enumrestore;

public enum Color {
    RED, GREEN, BLUE;

    public static Color pick(int i) {
        return values()[i % values().length];
    }
}
)JAVA";

/// 普通类对照样本（static 字段 + clinit，无 ACC_ENUM）。
constexpr const char* kPlainSrc = R"JAVA(
package hb.jvm.enumrestore;

public class Plain {
    public static int MAGIC = 42;
}
)JAVA";

void writeSource(const fs::path& path, const char* src)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good()) << "cannot write sample source: " << path.string();
    out << src;
}

Bytes readClassBytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "cannot read sample class: " << path.string();
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// 读回 class 字节里全部字段名（声明序）。
std::vector<std::string> fieldNames(const Bytes& bytes)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok()) << "session open failed";
    if (sessionResult.is_err()) return {};
    auto                      session = std::move(sessionResult).unwrap();
    std::vector<std::string>  names;
    for (const auto& f : session->unit().fields) {
        names.emplace_back(session->unit().symbols().str(f.name));
    }
    return names;
}

/// 便捷查询：字段/方法映射新名；未命中返回空串（Utf8StringRef 转换收口）。
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

/// 把映射批构造成 remapper 计划（old→new 方向，消费方式实证）。
mjbase::asm_remapper::MemberRemapPlan planFromMapping(
    const mjbase::mapping::IntermediateMapping& mapping, const std::string& owner)
{
    mjbase::asm_remapper::MemberRemapPlan plan;
    const auto  ownerRef = ca::str::Utf8StringRef::from_string_view(owner);
    const auto* fields   = mapping.getClassFieldMappings(ownerRef);
    if (fields != nullptr) {
        for (const auto& f : *fields) {
            plan.mapField(owner, std::string_view{f.oldName}, std::string_view{f.desc}, owner,
                          std::string_view{f.newName}, std::string_view{f.desc});
        }
    }
    const auto* methods = mapping.getClassMethodMappings(ownerRef);
    if (methods != nullptr) {
        for (const auto& m : *methods) {
            plan.mapMethod(owner, std::string_view{m.oldName}, std::string_view{m.desc}, owner,
                           std::string_view{m.newName}, std::string_view{m.desc});
        }
    }
    return plan;
}

/// 对一段类字节应用 remapper 计划（bytecodeOnly），返回改写后字节。
Bytes remapClass(const Bytes& in, const mjbase::asm_remapper::MemberRemapPlan& plan)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(in));
    EXPECT_TRUE(sessionResult.is_ok()) << "session open failed for remap";
    if (sessionResult.is_err()) return {};
    auto  session = std::move(sessionResult).unwrap();
    auto  applied = mjbase::asm_remapper::applyMemberRemapPlan(*session, plan);
    EXPECT_TRUE(applied.is_ok()) << "remap apply failed";
    if (applied.is_err()) return {};
    auto written = jbc::core::writeClass(session->backing());
    EXPECT_TRUE(written.is_ok()) << "writeClass failed";
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// javac 编译 + remapper 混淆，一次备齐两态字节（orig = javac 正名态，
/// obf = 字段/数组字段/合成方法改非法名后的混淆态）。
struct JavacSample
{
    Bytes orig;
    Bytes obf;
};

JavacSample compileAndObfuscate(const test::jdk::JdkToolchain& tc, const fs::path& gate)
{
    JavacSample s;
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin";
    writeSource(srcDir / "hb/jvm/enumrestore/Color.java", kEnumSrc);
    std::string compileErr;
    if (!test::jdk::compileJava(tc, srcDir / "hb/jvm/enumrestore/Color.java", binDir,
                                &compileErr)) {
        ADD_FAILURE() << "javac failed: " << compileErr;
        return s;
    }
    const std::string owner      = "hb/jvm/enumrestore/Color";
    const std::string constDesc  = "L" + owner + ";";
    const std::string valuesDesc = "[" + constDesc;
    s.orig = readClassBytes(binDir / "hb/jvm/enumrestore/Color.class");
    // remapper 混淆态：字段/数组字段/合成方法改非法名（ldc 真名与指令序不动，
    // 等价混淆器行为）。
    mjbase::asm_remapper::MemberRemapPlan obfPlan;
    obfPlan.mapField(owner, "RED", constDesc, owner, "a-b", constDesc);
    obfPlan.mapField(owner, "GREEN", constDesc, owner, "c-d", constDesc);
    obfPlan.mapField(owner, "BLUE", constDesc, owner, "e-f", constDesc);
    obfPlan.mapField(owner, "$VALUES", valuesDesc, owner, "A-B", valuesDesc);
    obfPlan.mapMethod(owner, "$values", valuesDesc, owner, "sv-z", valuesDesc);
    s.obf = remapClass(s.orig, obfPlan);
    EXPECT_FALSE(s.obf.empty());
    return s;
}

}   // namespace

TEST(EnumNameRestorerJvm, RestoresJavacObfuscatedEnumAndRoundTrips)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate_enum_restore";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin";

    // ── 1. javac 编译真样本 ──
    writeSource(srcDir / "hb/jvm/enumrestore/Plain.java", kPlainSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/enumrestore/Plain.java", binDir,
                                       &compileErr))
        << compileErr;

    const std::string owner     = "hb/jvm/enumrestore/Color";
    const std::string constDesc  = "L" + owner + ";";
    const std::string valuesDesc = "[" + constDesc;
    const JavacSample sample    = compileAndObfuscate(tc, gate);
    const Bytes       orig      = sample.orig;
    const Bytes       plain      = readClassBytes(binDir / "hb/jvm/enumrestore/Plain.class");
    ASSERT_FALSE(orig.empty());

    // ── 2. 正名态：判据形状命中但合法名不动 → 零映射 ──
    {
        mjbase::model::ResourceContainer   container;
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        container.addResource(owner + ".class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  owner + ".class", orig));
        mjbase::pass::EnumNameRestorer pass;
        ASSERT_TRUE(pass.run(container, gam).areAllPreserved());
        const auto& mapping = gam.getResult<mjbase::pass::EnumNameMappingAnalysis>(container);
        EXPECT_TRUE(mapping.isEmpty());
    }

    // ── 3. 非枚举对照：零映射 ──
    {
        mjbase::model::ResourceContainer   container;
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        container.addResource("hb/jvm/enumrestore/Plain.class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  "hb/jvm/enumrestore/Plain.class", plain));
        mjbase::pass::EnumNameRestorer pass;
        ASSERT_TRUE(pass.run(container, gam).areAllPreserved());
        const auto& mapping = gam.getResult<mjbase::pass::EnumNameMappingAnalysis>(container);
        EXPECT_TRUE(mapping.isEmpty());
    }

    // ── 4. remapper 混淆态字节就绪（compileAndObfuscate），声明序核对 ──
    const Bytes obf = sample.obf;
    ASSERT_FALSE(obf.empty());
    {
        const std::vector<std::string> names = fieldNames(obf);
        ASSERT_EQ(names.size(), 4u);
        EXPECT_EQ(names[0], "a-b");
        EXPECT_EQ(names[3], "A-B");
    }

    // ── 5. 混淆态跑 pass：映射必须恰好反演混淆（映射引用与 GAM 同作用域，
    //       视图寿命 = 容器 AM 存活期）──
    mjbase::model::ResourceContainer   container;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    container.addResource(owner + ".class",
                          std::make_unique<mjbase::model::ClassResource>(owner + ".class", obf));
    mjbase::pass::EnumNameRestorer pass;
    ASSERT_TRUE(pass.run(container, gam).areAllPreserved());
    const auto& mapping = gam.getResult<mjbase::pass::EnumNameMappingAnalysis>(container);
    EXPECT_EQ(mappedField(mapping, owner, "a-b", constDesc), "RED");
    EXPECT_EQ(mappedField(mapping, owner, "c-d", constDesc), "GREEN");
    EXPECT_EQ(mappedField(mapping, owner, "e-f", constDesc), "BLUE");
    EXPECT_EQ(mappedField(mapping, owner, "A-B", valuesDesc), "$VALUES");
    EXPECT_EQ(mappedMethod(mapping, owner, "sv-z", valuesDesc), "$values");
    EXPECT_TRUE(mapping.getClasses().empty());

    // ── 6. 消费闭环：映射经 remapper 应用回混淆态 → 字段名复原 ──
    {
        const mjbase::asm_remapper::MemberRemapPlan restorePlan = planFromMapping(mapping, owner);
        const Bytes                                 restored    = remapClass(obf, restorePlan);
        ASSERT_FALSE(restored.empty());
        const std::vector<std::string> names = fieldNames(restored);
        ASSERT_EQ(names.size(), 4u);
        EXPECT_EQ(names[0], "RED");
        EXPECT_EQ(names[1], "GREEN");
        EXPECT_EQ(names[2], "BLUE");
        EXPECT_EQ(names[3], "$VALUES");
    }
}

/// 判据逐门探针：在 javac 混淆字节上按 EnumNameMappingAnalysis 的判据序
/// 走一遍（形态锚 → 形状 B → 形状 A 采集 → 互证输入 → pass 实跑产出），
/// 每门带定位诊断；主用例映射为空时先看本用例哪一门红。诊断用例，允许
/// 与主用例共存（同名夹具目录串行复用）。
TEST(EnumNameRestorerJvm, ObfClinitJudgeProbe)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const JavacSample sample =
        compileAndObfuscate(tc, test::getTopProjectPath() / "test/jvm_gate_enum_restore");
    ASSERT_FALSE(sample.obf.empty());

    const std::string owner      = "hb/jvm/enumrestore/Color";
    const std::string constDesc  = "L" + owner + ";";
    const std::string valuesDesc = "[" + constDesc;

    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(sample.obf));
    ASSERT_TRUE(sessionResult.is_ok()) << "obf session open failed";
    auto                session = std::move(sessionResult).unwrap();
    jbc::ir::ClassUnit& unit    = session->unit();
    EXPECT_EQ(std::string{unit.symbols().str(unit.thisClass)}, owner) << "门0: thisClass";

    // 门 1：形态锚（ACC_ENUM + 双桥 + 三常量字段带 ACC_ENUM）。
    EXPECT_NE(session->backing().access & 0x4000, 0) << "门1: 类 ACC_ENUM";
    EXPECT_NE(unit.findMethod("values", valuesDesc), jbc::ir::kNoMethod) << "门1: values 桥";
    EXPECT_NE(unit.findMethod("valueOf", "(Ljava/lang/String;)" + constDesc), jbc::ir::kNoMethod)
        << "门1: valueOf 桥";
    for (const char* fname : {"a-b", "c-d", "e-f"}) {
        bool declared = false;
        bool accEnum  = false;
        for (const auto& fu : unit.fields) {
            if (unit.symbols().str(fu.name) == fname &&
                unit.symbols().str(fu.desc) == constDesc) {
                declared = true;
                accEnum  = (fu.access & 0x4000) != 0;
            }
        }
        EXPECT_TRUE(declared) << "门1: 字段声明在 " << fname;
        EXPECT_TRUE(accEnum) << "门1: 字段 ACC_ENUM 位 " << fname;
    }

    // 门 2：形状 B（clinit 恰一处 INVOKESTATIC 本类 ()[L本类; + 直线
    // PUTSTATIC 数组字段）。
    const ca::usize clinitIdx = unit.findMethod("<clinit>", "()V");
    ASSERT_NE(clinitIdx, jbc::ir::kNoMethod);
    ASSERT_TRUE(jbc::ir::hasCode(*session, clinitIdx));
    auto promoted = jbc::ir::promoteMethod(*session, clinitIdx);
    ASSERT_TRUE(promoted.is_ok()) << "门2: promote clinit failed";
    auto cu = std::move(promoted).unwrap();
    EXPECT_EQ(cu->dom.order().size(), 21u) << "门2: clinit 拍数（javac 17 形状）";
    int         valuesCalls = 0;
    std::string valuesCallName;
    for (ca::usize i = 0; i < cu->dom.order().size(); ++i) {
        const jbc::ir::Insn* in = cu->dom.peek(cu->dom.order()[i]);
        if (in == nullptr || in->op != jbc::core::Op::INVOKESTATIC) continue;
        jbc::ir::ClassSession::MemberSem ref;
        if (!session->memberOf(*in, ref)) {
            ADD_FAILURE() << "门2: clinit[" << i << "] INVOKESTATIC memberOf 失败";
            continue;
        }
        if (std::string{ref.owner} != owner || std::string{ref.desc} != valuesDesc) continue;
        ++valuesCalls;
        valuesCallName = std::string{ref.name};
        const jbc::ir::Insn* nxt = cu->dom.peek(cu->dom.order()[i + 1]);
        ASSERT_NE(nxt, nullptr);
        EXPECT_EQ(nxt->op, jbc::core::Op::PUTSTATIC) << "门2: 调用段后非 PUTSTATIC";
        jbc::ir::ClassSession::MemberSem putRef;
        ASSERT_TRUE(session->memberOf(*nxt, putRef)) << "门2: PUTSTATIC memberOf 失败";
        EXPECT_EQ(std::string{putRef.name}, "A-B") << "门2: 数组字段名";
        EXPECT_EQ(std::string{putRef.desc}, valuesDesc) << "门2: 数组字段 desc";
    }
    EXPECT_EQ(valuesCalls, 1) << "门2: 本类数组调用段出现次数";
    EXPECT_EQ(valuesCallName, "sv-z") << "门2: 调用位方法名";

    // 门 2.5：数组方法体 GETSTATIC 序。
    {
        const ca::usize valuesIdx = unit.findMethod(valuesCallName, valuesDesc);
        ASSERT_NE(valuesIdx, jbc::ir::kNoMethod) << "门2.5: findMethod " << valuesCallName;
        ASSERT_TRUE(jbc::ir::hasCode(*session, valuesIdx));
        auto vp = jbc::ir::promoteMethod(*session, valuesIdx);
        ASSERT_TRUE(vp.is_ok()) << "门2.5: promote " << valuesCallName << " failed";
        auto                     vcu = std::move(vp).unwrap();
        std::vector<std::string> gs;
        for (const jbc::ir::InsnId id : vcu->dom.order()) {
            const jbc::ir::Insn* in = vcu->dom.peek(id);
            if (in == nullptr || in->op != jbc::core::Op::GETSTATIC) continue;
            jbc::ir::ClassSession::MemberSem ref;
            if (!session->memberOf(*in, ref)) {
                ADD_FAILURE() << "门2.5: GETSTATIC memberOf 失败";
                continue;
            }
            if (std::string{ref.owner} == owner && std::string{ref.desc} == constDesc) {
                gs.emplace_back(ref.name);
            }
        }
        ASSERT_EQ(gs.size(), 3u) << "门2.5: 数组体 GETSTATIC 数";
        EXPECT_EQ(gs[0], "a-b") << "门2.5: 数组序[0]";
        EXPECT_EQ(gs[1], "c-d") << "门2.5: 数组序[1]";
        EXPECT_EQ(gs[2], "e-f") << "门2.5: 数组序[2]";
    }

    // 门 3：形状 A 构造点采集（ldc 泄漏名 + 前后锚 + ordinal + 字段）。
    std::vector<std::string> leaks;
    std::vector<ca::i32>     ordinals;
    std::vector<std::string> pointFields;
    {
        const auto ord = cu->dom.order();
        for (ca::usize i = 0; i < ord.size(); ++i) {
            const jbc::ir::Insn* ldc = cu->dom.peek(ord[i]);
            if (ldc == nullptr ||
                (ldc->op != jbc::core::Op::LDC && ldc->op != jbc::core::Op::LDC_W)) {
                continue;
            }
            jbc::ir::ClassSession::ConstSem c;
            if (!session->constOf(*ldc, c) ||
                c.kind != jbc::ir::ClassSession::ConstSem::Kind::String) {
                continue;
            }
            leaks.emplace_back(c.str);
            ASSERT_GE(i, 2u) << "门3: ldc 位置过前";
            const jbc::ir::Insn* newInsn = cu->dom.peek(ord[i - 2]);
            const jbc::ir::Insn* dupInsn = cu->dom.peek(ord[i - 1]);
            ASSERT_NE(newInsn, nullptr);
            ASSERT_NE(dupInsn, nullptr);
            EXPECT_EQ(newInsn->op, jbc::core::Op::NEW) << "门3: [" << i << "] 前锚 NEW";
            EXPECT_EQ(std::string{session->classOf(*newInsn)}, owner) << "门3: [" << i
                                                                     << "] NEW classOf";
            EXPECT_EQ(dupInsn->op, jbc::core::Op::DUP) << "门3: [" << i << "] 前锚 DUP";
            const jbc::ir::Insn* ordInsn = cu->dom.peek(ord[i + 1]);
            ASSERT_NE(ordInsn, nullptr);
            ca::i32 v = -1;
            if (ordInsn->op >= jbc::core::Op::ICONST_0 &&
                ordInsn->op <= jbc::core::Op::ICONST_5) {
                v = static_cast<ca::i32>(ordInsn->op) - static_cast<ca::i32>(jbc::core::Op::ICONST_0);
            }
            EXPECT_GE(v, 0) << "门3: [" << i << "] ordinal 装载可读";
            ordinals.push_back(v);
            const jbc::ir::Insn* ctor = cu->dom.peek(ord[i + 2]);
            ASSERT_NE(ctor, nullptr);
            ASSERT_EQ(ctor->op, jbc::core::Op::INVOKESPECIAL) << "门3: [" << i << "] 构造调用";
            jbc::ir::ClassSession::MemberSem ctorRef;
            ASSERT_TRUE(session->memberOf(*ctor, ctorRef)) << "门3: [" << i
                                                          << "] INVOKESPECIAL memberOf";
            EXPECT_EQ(std::string{ctorRef.name}, "<init>") << "门3: [" << i << "] ctor 名";
            EXPECT_EQ(std::string{ctorRef.desc}, "(Ljava/lang/String;I)V")
                << "门3: [" << i << "] ctor desc";
            const jbc::ir::Insn* put = cu->dom.peek(ord[i + 3]);
            ASSERT_NE(put, nullptr);
            ASSERT_EQ(put->op, jbc::core::Op::PUTSTATIC) << "门3: [" << i << "] putstatic 直达";
            jbc::ir::ClassSession::MemberSem putRef;
            ASSERT_TRUE(session->memberOf(*put, putRef)) << "门3: [" << i << "] PUTSTATIC memberOf";
            EXPECT_EQ(std::string{putRef.desc}, constDesc) << "门3: [" << i << "] 字段 desc";
            pointFields.emplace_back(putRef.name);
        }
    }
    ASSERT_EQ(leaks.size(), 3u) << "门3: 构造点数";
    EXPECT_EQ(leaks[0], "RED") << "门3: 泄漏名[0]";
    EXPECT_EQ(leaks[1], "GREEN") << "门3: 泄漏名[1]";
    EXPECT_EQ(leaks[2], "BLUE") << "门3: 泄漏名[2]";
    EXPECT_EQ(ordinals[0], 0) << "门3: ordinal[0]";
    EXPECT_EQ(ordinals[1], 1) << "门3: ordinal[1]";
    EXPECT_EQ(ordinals[2], 2) << "门3: ordinal[2]";
    EXPECT_EQ(pointFields[0], "a-b") << "门3: 字段[0]";
    EXPECT_EQ(pointFields[1], "c-d") << "门3: 字段[1]";
    EXPECT_EQ(pointFields[2], "e-f") << "门3: 字段[2]";

    // 门 4：pass 实跑产出（前三门全绿而此处仍空 → 判据决策层问题）。
    {
        mjbase::model::ResourceContainer   container;
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        container.addResource(
            owner + ".class",
            std::make_unique<mjbase::model::ClassResource>(owner + ".class", sample.obf));
        mjbase::pass::EnumNameRestorer pass;
        EXPECT_TRUE(pass.run(container, gam).areAllPreserved());
        const auto& mapping = gam.getResult<mjbase::pass::EnumNameMappingAnalysis>(container);
        EXPECT_FALSE(mapping.isEmpty()) << "门4: 判据输入全绿但映射为空";
    }
}
