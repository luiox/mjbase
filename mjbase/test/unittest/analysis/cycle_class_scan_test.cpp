// CycleClassScan（循环继承腐坏判据件）单测：A↔B 双类环 / 自环（super=自己）/
// 三类长环 / 环中含入口可达真类（判据不受可达性抑制，环外入口不误报）/
// 环外正常继承与悬挂 super 零命中 / 幂等二跑 / 同名重复合并 / 畸形类
// fail-open。夹具一律 jasm 文本装配（JvmJasmService，一下一个类），环引用
// 直接写 .super。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "libca/str/utf16_string.hpp"

#include "mjbase/analysis/cycle_class_scan.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

/// UTF-8 → UTF-16（jasm 服务入参口径）。
ca::str::Utf16String utf16Of(const std::string& s)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(s.data()), s.size()));
}

/// jasm 文本汇编为 class 字节（一下一个类；汇编失败即测试失败）。
/// superName 用 internal name；body 为类体文本（.method 声明序列）。
Bytes assembleClass(const std::string& internalName, const std::string& superName,
                    const std::string& body)
{
    mjbase::jasm::JvmJasmService service;
    const std::string source = ".super " + superName + R"JASM(
.class public super )JASM" + internalName + R"JASM( {
)JASM" + body + R"JASM(}
)JASM";
    const auto sourceName = utf16Of(internalName + ".jasm");
    const auto sourceText = utf16Of(source);
    const auto result     = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败: " << internalName;
        return {};
    }
    return result.value->bytes;
}

/// 装配 + 入容器（uri = <名>.class）。
void addJasmClass(mjbase::model::ResourceContainer& container, const std::string& name,
                  const std::string& superName, const std::string& body = "")
{
    addClassResource(container, (name + ".class").c_str(),
                     assembleClass(name, superName, body));
}

/// 判据运行面：扫描容器 → 环名集合（排序后返回，容器迭代序无关比较）。
std::vector<std::string> scanCycleNamesSorted(mjbase::model::ResourceContainer& container)
{
    const std::vector<mjbase::analysis::ScannedClass> scanned =
        mjbase::analysis::scanClassResources(container);
    std::vector<std::string> names;
    for (const std::string_view name :
         mjbase::analysis::findInheritanceCycleClasses(scanned)) {
        names.emplace_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

/// 期望集合（排序）与实际一致断言。
void expectCycles(const std::vector<std::string>& actual,
                  std::vector<std::string>        expected)
{
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(actual, expected);
}

}   // namespace

// 用例：A↔B 双类 super 环判腐坏；环外挂靠子类（Leaf extends A）不判。
TEST(MjAnalysisCycleClassScanTest, ReportsTwoClassSuperCycleOnly)
{
    mjbase::model::ResourceContainer container;
    addJasmClass(container, "A", "B");
    addJasmClass(container, "B", "A");
    addJasmClass(container, "Leaf", "A");
    expectCycles(scanCycleNamesSorted(container), {"A", "B"});
}

// 用例：自环（super=自己）单类判腐坏。
TEST(MjAnalysisCycleClassScanTest, ReportsSelfSuperCycle)
{
    mjbase::model::ResourceContainer container;
    addJasmClass(container, "SelfCycle", "SelfCycle");
    addJasmClass(container, "Normal", "java/lang/Object");
    expectCycles(scanCycleNamesSorted(container), {"SelfCycle"});
}

// 用例：三类长环 C1→C2→C3→C1 全体成员判腐坏。
TEST(MjAnalysisCycleClassScanTest, ReportsThreeClassLongCycle)
{
    mjbase::model::ResourceContainer container;
    addJasmClass(container, "C1", "C2");
    addJasmClass(container, "C2", "C3");
    addJasmClass(container, "C3", "C1");
    expectCycles(scanCycleNamesSorted(container), {"C1", "C2", "C3"});
}

// 用例：环成员被入口可达（App.main → A.m）不抑制判据——环类照常点名，
// 入口真类 App 不误报（判据独立于可达性的口径钉死）。
TEST(MjAnalysisCycleClassScanTest, ReportsReachableCycleMembersWithoutFalsePositives)
{
    mjbase::model::ResourceContainer container;
    addJasmClass(container, "A", "B",
                 R"JASM(  .method public static m ()V {
    code: {
      return
    }
  }
)JASM");
    addJasmClass(container, "B", "A");
    addJasmClass(container, "App", "java/lang/Object",
                 R"JASM(  .method public static main ([Ljava/lang/String;)V {
    code: {
      invokestatic A.m ()V
      return
    }
  }
)JASM");
    expectCycles(scanCycleNamesSorted(container), {"A", "B"});
}

// 用例：环外正常继承（Object→Base→Sub→Leaf 长链）与悬挂 super（指向容器外）
// 零命中。
TEST(MjAnalysisCycleClassScanTest, NormalInheritanceAndDanglingSuperNeverHit)
{
    mjbase::model::ResourceContainer container;
    addJasmClass(container, "Base", "java/lang/Object");
    addJasmClass(container, "Sub", "Base");
    addJasmClass(container, "Leaf", "Sub");
    addJasmClass(container, "Dangling", "Ghost/NotInContainer");
    addJasmClass(container, "Deep0", "java/lang/Object");
    addJasmClass(container, "Deep1", "Deep0");
    addJasmClass(container, "Deep2", "Deep1");
    addJasmClass(container, "Deep3", "Deep2");
    addJasmClass(container, "Deep4", "Deep3");
    addJasmClass(container, "Deep5", "Deep4");
    addJasmClass(container, "Deep6", "Deep5");
    addJasmClass(container, "Deep7", "Deep6");
    expectCycles(scanCycleNamesSorted(container), {});
}

// 用例：二跑幂等——同一扫描集合重复求值与重扫容器结果一致。
TEST(MjAnalysisCycleClassScanTest, SecondScanIsIdempotent)
{
    mjbase::model::ResourceContainer container;
    addJasmClass(container, "A", "B");
    addJasmClass(container, "B", "A");
    addJasmClass(container, "SelfCycle", "SelfCycle");
    addJasmClass(container, "Normal", "java/lang/Object");

    const std::vector<mjbase::analysis::ScannedClass> scanned =
        mjbase::analysis::scanClassResources(container);
    std::vector<std::string> first;
    for (const std::string_view name :
         mjbase::analysis::findInheritanceCycleClasses(scanned)) {
        first.emplace_back(name);
    }
    std::vector<std::string> second;
    for (const std::string_view name :
         mjbase::analysis::findInheritanceCycleClasses(scanned)) {
        second.emplace_back(name);
    }
    EXPECT_EQ(first, second);
    expectCycles(scanCycleNamesSorted(container), first);
}

// 用例：同名重复扫描类首个成节点（先到先得）——后到同名类的边不并入。
// 容器迭代序对同名重复条目不保证先后，直接构造扫描集合控制两个方向：
// 自环变体先到 → 按名报告 {"Dup"}；正常变体先到 → 自环边不并入，零命中。
TEST(MjAnalysisCycleClassScanTest, DuplicateNamesCollapseToFirstNode)
{
    const Bytes selfSuper = assembleClass("Dup", "Dup", "");
    const Bytes normal    = assembleClass("Dup", "java/lang/Object", "");
    const auto makeScanned = [](const Bytes& bytes) {
        auto made = jbc::ir::makeSessionFromBytes(
            jbc::core::ByteView{bytes.data(), bytes.size()});
        EXPECT_TRUE(made.is_ok());
        if (made.is_err()) return mjbase::analysis::ScannedClass{};
        mjbase::analysis::ScannedClass scanned;
        scanned.session = std::move(made).unwrap();
        scanned.name =
            scanned.session->unit().symbols().str(scanned.session->unit().thisClass);
        return scanned;
    };

    const auto cycleNamesOf = [](const std::vector<mjbase::analysis::ScannedClass>& scanned) {
        std::vector<std::string> names;
        for (const std::string_view name :
             mjbase::analysis::findInheritanceCycleClasses(scanned)) {
            names.emplace_back(name);
        }
        return names;
    };

    expectCycles(cycleNamesOf({makeScanned(selfSuper), makeScanned(normal)}), {"Dup"});
    expectCycles(cycleNamesOf({makeScanned(normal), makeScanned(selfSuper)}), {});
}

// 用例：畸形类字节 fail-open（scanClassResources 跳过，不产出事实），
// 与正常类共存时判据不受影响。
TEST(MjAnalysisCycleClassScanTest, MalformedClassContributesNoFacts)
{
    mjbase::model::ResourceContainer container;
    addJasmClass(container, "Normal", "java/lang/Object");
    addClassResource(container, "Bad.class", Bytes{0xCA, 0xFE, 0xBA, 0x00});
    expectCycles(scanCycleNamesSorted(container), {});
}
