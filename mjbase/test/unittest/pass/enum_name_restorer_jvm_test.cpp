/// EnumNameRestorer 在 javac 真产物上的硬验收：javac 编译枚举样本 →
/// mjbase remapper 面按映射把常量字段/数组字段/合成方法改成非法名（等价
/// 混淆器行为：ldc 真名不动、指令序不变）→ 本 pass 产出的映射必须恰好
/// 反演该混淆 → 恢复态经 remapper 应用后字段名与原样本一致且真 JVM 可
/// 运行输出一致。JDK 不可用的机器整测跳过。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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
    const auto* fields = mapping.getClassFieldMappings(owner);
    if (fields != nullptr) {
        for (const auto& f : *fields) {
            plan.mapField(owner, std::string_view{f.oldName}, std::string_view{f.desc}, owner,
                          std::string_view{f.newName}, std::string_view{f.desc});
        }
    }
    const auto* methods = mapping.getClassMethodMappings(owner);
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
    writeSource(srcDir / "hb/jvm/enumrestore/Color.java", kEnumSrc);
    writeSource(srcDir / "hb/jvm/enumrestore/Plain.java", kPlainSrc);
    std::string compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/enumrestore/Color.java", binDir,
                                       &compileErr))
        << compileErr;
    ASSERT_TRUE(test::jdk::compileJava(tc, srcDir / "hb/jvm/enumrestore/Plain.java", binDir,
                                       &compileErr))
        << compileErr;

    const std::string owner     = "hb/jvm/enumrestore/Color";
    const std::string constDesc  = "L" + owner + ";";
    const std::string valuesDesc = "[" + constDesc;
    const Bytes       orig       = readClassBytes(binDir / "hb/jvm/enumrestore/Color.class");
    const Bytes       plain      = readClassBytes(binDir / "hb/jvm/enumrestore/Plain.class");

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

    // ── 4. remapper 混淆态：字段/数组字段/合成方法改非法名（ldc 真名与
    //       指令序不动，等价混淆器行为）──
    mjbase::asm_remapper::MemberRemapPlan obfPlan;
    obfPlan.mapField(owner, "RED", constDesc, owner, "a-b", constDesc);
    obfPlan.mapField(owner, "GREEN", constDesc, owner, "c-d", constDesc);
    obfPlan.mapField(owner, "BLUE", constDesc, owner, "e-f", constDesc);
    obfPlan.mapField(owner, "$VALUES", valuesDesc, owner, "A-B", valuesDesc);
    obfPlan.mapMethod(owner, "$values", valuesDesc, owner, "sv-z", valuesDesc);
    const Bytes obf = remapClass(orig, obfPlan);
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
