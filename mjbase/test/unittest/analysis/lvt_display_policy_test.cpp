/// @file        lvt_display_policy_test.cpp
/// @brief       LVT 显示名策略测试：逐名形态判定、jar 级兜底阈值、
///              var<槽位号> 呈现、cat2 槽位跳号、同槽多类型后缀。
///
/// 全部为纯函数面用例（无字节码夹具）；阈值（64 字节上限、1/2 兜底占比）
/// 在此钉死，改动须连同策略语义一起评审。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mjbase/analysis/lvt_display_policy.hpp"

using mjbase::analysis::LvtDisplayRow;
using mjbase::analysis::LvtShapeStats;
using mjbase::analysis::lvtDisplayNames;
using mjbase::analysis::lvtJarFallback;
using mjbase::analysis::kLvtMaxTrustedNameBytes;
using mjbase::analysis::lvtNameTrustworthy;

namespace {

/// @brief 行集快捷构造。
std::vector<LvtDisplayRow> rows(std::initializer_list<LvtDisplayRow> list)
{
    return std::vector<LvtDisplayRow>{list};
}

/// @brief 呈现名输出 → std::string 列表（断言面友好）。
std::vector<std::string> rendered(const std::vector<ca::str::Utf8String>& names)
{
    std::vector<std::string> out;
    out.reserve(names.size());
    for (const auto& name : names) out.push_back(name.to_std_string());
    return out;
}

LvtDisplayRow row(std::string_view name, std::string_view descriptor, ca::u32 slot)
{
    return LvtDisplayRow{name, descriptor, slot};
}

/// @brief 控制字符开头的对抗名（char 字面量拼接，避开十六进制转义贪吃）。
std::string ctrlName(const char* suffix)
{
    return std::string(1, '\x01') + suffix;
}

/// @brief 含非 ASCII 字节（UTF-8 编码 ä = C3 A4）的对抗名。
std::string nonAsciiName()
{
    return std::string("v") + '\xC3' + '\xA4' + "l";
}

}   // namespace

TEST(LvtDisplayPolicyTest, TrustworthyNamesPassShapeCheck)
{
    EXPECT_TRUE(lvtNameTrustworthy("value"));
    EXPECT_TRUE(lvtNameTrustworthy("held"));
    EXPECT_TRUE(lvtNameTrustworthy("data_2"));
    EXPECT_TRUE(lvtNameTrustworthy("_private"));
    EXPECT_TRUE(lvtNameTrustworthy("$anon"));            // $ 是 Java 合法字符
    EXPECT_TRUE(lvtNameTrustworthy("lambda$main$0"));    // javac 产物形态
    EXPECT_TRUE(lvtNameTrustworthy("a"));                // obf 短名：合法保留
    EXPECT_TRUE(lvtNameTrustworthy(std::string(kLvtMaxTrustedNameBytes, 'a')));
}

TEST(LvtDisplayPolicyTest, UntrustworthyNamesFailShapeCheck)
{
    EXPECT_FALSE(lvtNameTrustworthy(""));                                  // 空
    EXPECT_FALSE(lvtNameTrustworthy("1var"));                              // 首字符数字
    EXPECT_FALSE(lvtNameTrustworthy(std::string("a") + '\x01' + "b"));     // 控制字符
    EXPECT_FALSE(lvtNameTrustworthy(std::string("a") + '\x7F' + "b"));     // DEL
    EXPECT_FALSE(lvtNameTrustworthy(nonAsciiName()));                      // 非 ASCII
    EXPECT_FALSE(lvtNameTrustworthy(std::string("a\\u0041b")));            // \uXXXX 字面形态
    EXPECT_FALSE(lvtNameTrustworthy(std::string(kLvtMaxTrustedNameBytes + 1, 'a')));  // 超长
    EXPECT_FALSE(lvtNameTrustworthy("a b"));                               // 空格
    EXPECT_FALSE(lvtNameTrustworthy("a-b"));                               // 连字符
}

TEST(LvtDisplayPolicyTest, JarFallbackThresholdPinned)
{
    // 0 行：无 LVT 无兜底。
    EXPECT_FALSE(lvtJarFallback(LvtShapeStats{0, 0}));
    // 全合法：不兜底。
    EXPECT_FALSE(lvtJarFallback(LvtShapeStats{10, 0}));
    // 阈值 = 1/2：恰好一半非法即兜底（宁枉勿纵）。
    EXPECT_FALSE(lvtJarFallback(LvtShapeStats{100, 49}));
    EXPECT_TRUE(lvtJarFallback(LvtShapeStats{100, 50}));
    EXPECT_TRUE(lvtJarFallback(LvtShapeStats{100, 99}));
    EXPECT_TRUE(lvtJarFallback(LvtShapeStats{1, 1}));
    // 小样本：2 行 1 非法 = 1/2，兜底。
    EXPECT_TRUE(lvtJarFallback(LvtShapeStats{2, 1}));
}

TEST(LvtDisplayPolicyTest, KeepsTrustworthyOriginalsWithoutFallback)
{
    const auto names = rendered(lvtDisplayNames(rows({row("value", "I", 0),
                                                      row("held", "Ljava/lang/String;", 1),
                                                      row(ctrlName("garbage"), "J", 3)}),
                                               false));
    EXPECT_EQ((std::vector<std::string>{"value", "held", "var3"}), names);
}

TEST(LvtDisplayPolicyTest, FallbackForcesAllRowsToVarSlot)
{
    // jar 级兜底：合法名（含 obf 短名）也一律 var<槽位号>，不逐名纠结。
    const auto names = rendered(lvtDisplayNames(rows({row("value", "I", 0),
                                                      row("a", "J", 2),
                                                      row(ctrlName("x"), "I", 4)}),
                                               true));
    EXPECT_EQ((std::vector<std::string>{"var0", "var2", "var4"}), names);
}

TEST(LvtDisplayPolicyTest, Cat2SlotsNumberByPhysicalSlot)
{
    // cat2（long/double）占两槽：呈现名用行槽位号本身，编号按物理槽跳号
    // （slot 2 的 long 与 slot 4 的 int 之间无 var3）。
    const auto names = rendered(lvtDisplayNames(rows({row(ctrlName("a"), "J", 2),
                                                      row(ctrlName("b"), "I", 4),
                                                      row(ctrlName("c"), "D", 5)}),
                                               true));
    EXPECT_EQ((std::vector<std::string>{"var2", "var4", "var5"}), names);
}

TEST(LvtDisplayPolicyTest, SameSlotDistinctDescriptorsGetTypedSuffix)
{
    // 同槽跨作用域多类型：var 形态行加类型后缀。
    const auto names = rendered(lvtDisplayNames(rows({row(ctrlName("a"), "I", 3),
                                                      row(ctrlName("b"), "J", 3),
                                                      row(ctrlName("c"), "I", 5)}),
                                               true));
    EXPECT_EQ((std::vector<std::string>{"var3_int", "var3_long", "var5"}), names);
}

TEST(LvtDisplayPolicyTest, SameSlotRepeatedDescriptorStaysPlain)
{
    // 同槽同描述符（同类型跨作用域）：无歧义，不加后缀。
    const auto names = rendered(lvtDisplayNames(rows({row(ctrlName("a"), "I", 3),
                                                      row(ctrlName("b"), "I", 3)}),
                                               true));
    EXPECT_EQ((std::vector<std::string>{"var3", "var3"}), names);
}

TEST(LvtDisplayPolicyTest, SuffixSkipsRowsThatKeepOriginalNames)
{
    // 混合槽：可保留原名行不加后缀（原名本身即区分），var 形态行加后缀。
    const auto names = rendered(lvtDisplayNames(rows({row("data", "I", 3),
                                                      row(ctrlName("b"), "J", 3)}),
                                               false));
    EXPECT_EQ((std::vector<std::string>{"data", "var3_long"}), names);
}

TEST(LvtDisplayPolicyTest, ObjectTypeSuffixTakesSimpleName)
{
    // 两个对象类型描述符同槽复用：后缀取最后一段简单名。
    const auto names = rendered(lvtDisplayNames(rows({row(ctrlName("a"), "Ljava/lang/String;", 1),
                                                      row(ctrlName("b"), "Ljava/util/List;", 1)}),
                                               true));
    EXPECT_EQ((std::vector<std::string>{"var1_String", "var1_List"}), names);
}

TEST(LvtDisplayPolicyTest, LvtAndLvttSignatureRowsConvergeOnSameSuffix)
{
    // LVT 行（描述符）与 LVTT 行（泛型签名文本）同一物理变量：简单名在
    // '<'/';' 截断后收敛，呈现同名。
    const auto names =
        rendered(lvtDisplayNames(rows({row(ctrlName("a"), "Ljava/util/List;", 1),
                                       row(ctrlName("a"), "Ljava/util/List<Ljava/lang/String;>;", 1)}),
                                 true));
    EXPECT_EQ((std::vector<std::string>{"var1_List", "var1_List"}), names);
}

TEST(LvtDisplayPolicyTest, ArrayDescriptorsAppendArray)
{
    // 三种数组描述符同槽复用：元素后缀递归拼接 Array。
    const auto names = rendered(lvtDisplayNames(rows({row(ctrlName("a"), "[I", 1),
                                                      row(ctrlName("b"), "[Ljava/lang/String;", 1),
                                                      row(ctrlName("c"), "[[J", 1)}),
                                               true));
    EXPECT_EQ(
        (std::vector<std::string>{"var1_intArray", "var1_StringArray", "var1_longArrayArray"}),
        names);
}

TEST(LvtDisplayPolicyTest, MalformedDescriptorsFallBackToOrdinal)
{
    // 畸形描述符：多类型槽内按该槽去重描述符首现序落 t<序号>；畸形描述符
    // 独占槽位时无歧义，与普通形态一致不加后缀。
    const auto names = rendered(lvtDisplayNames(rows({row(ctrlName("a"), "X", 2),
                                                      row(ctrlName("b"), "", 2),
                                                      row(ctrlName("c"), "I", 2),
                                                      row(ctrlName("d"), "X", 4)}),
                                               true));
    EXPECT_EQ((std::vector<std::string>{"var2_t1", "var2_t2", "var2_int", "var4"}), names);
}

TEST(LvtDisplayPolicyTest, EmptyRowSetYieldsEmptyOutput)
{
    EXPECT_TRUE(lvtDisplayNames({}, false).empty());
    EXPECT_TRUE(lvtDisplayNames({}, true).empty());
}

TEST(LvtDisplayPolicyTest, OutputIsAsciiSafeUnderAdversarialNames)
{
    // 对抗名（控制字符/非 ASCII）不沿呈现面流出：输出恒为 ASCII。
    const auto names = rendered(lvtDisplayNames(rows({row(std::string(1, '\x7F'), "I", 0),
                                                      row(nonAsciiName(), "J", 1)}),
                                               false));
    for (const auto& name : names) {
        for (const char character : name) {
            const auto byte = static_cast<unsigned char>(character);
            ASSERT_LT(byte, 0x80U) << "non-ASCII byte leaked to display name";
        }
    }
    EXPECT_EQ((std::vector<std::string>{"var0", "var1"}), names);
}
