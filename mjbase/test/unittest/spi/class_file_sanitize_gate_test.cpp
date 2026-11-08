// classfile 净化面 + 管线入口净化门（校验+拒绝）行为契约。
// 诊断类别与门禁语义对齐 class_file_sanitizer /
// ClassFileSanitizeGate 的既有契约（诊断 kind 与拒绝路径逐条验收）。
//
// 夹具纪律：合法对照类（wellFormed / Good.class）走 jasm 装配
//（buildNopClassBytes，spi_test_util）。诊断触发形态是「格式即坏」的
// 字节语料（截断头 / 坏魔数 / 非法 CP tag / 悬垂引用 / 保留槽 / BSM
// 缺属性 / CP 环）——例外申报：合法基类 + DOM 偏差无法表达「CP 条目 tag
// 本身非法」「类头残缺」等字节格式态（DOM/装配器只写合法结构），字节
// 书写属净化器输入语料而非类夹具，按最小触发形保留字节级构造。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mjbase/model/resource_model.hpp"
#include "mjbase/spi/class_file_sanitize.hpp"
#include "mjbase/spi/class_file_sanitize_gate.hpp"
#include "spi_test_util.hpp"

using namespace mjbase::spi;
using namespace mjbase::spi::test;

namespace {

ca::str::Utf8StringArena tSanitizeArena;

// ── 字节语料 helper（每条诊断类别的最小触发形态）──

/// 完整 classfile 头（magic/minor/major/cpCount）。
Bytes header(ca::u16 cpCount)
{
    Bytes b;
    b.push_back(0xCA); b.push_back(0xFE); b.push_back(0xBA); b.push_back(0xBE);
    b.push_back(0); b.push_back(0);
    b.push_back(0); b.push_back(0x34);
    b.push_back(static_cast<ca::u8>(cpCount >> 8));
    b.push_back(static_cast<ca::u8>(cpCount));
    return b;
}

Bytes utf8Entry(std::string_view s)
{
    Bytes b;
    b.push_back(0x01);
    b.push_back(static_cast<ca::u8>(s.size() >> 8));
    b.push_back(static_cast<ca::u8>(s.size()));
    for (const char c : s) b.push_back(static_cast<ca::u8>(c));
    return b;
}

Bytes classEntry(ca::u16 nameIdx)
{
    Bytes b;
    b.push_back(0x07);
    b.push_back(static_cast<ca::u8>(nameIdx >> 8));
    b.push_back(static_cast<ca::u8>(nameIdx));
    return b;
}

// u8 / 字符串落笔与拼接（字节语料写面）。
void put8(Bytes& b, ca::u8 v) { b.push_back(v); }

void putRawText(Bytes& b, std::string_view s)
{
    for (const char c : s) b.push_back(static_cast<ca::u8>(c));
}

void appendInto(Bytes& b, Bytes src) { b.insert(b.end(), src.begin(), src.end()); }

void put16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

void put32(Bytes& b, ca::u32 v)
{
    b.push_back(static_cast<ca::u8>(v >> 24));
    b.push_back(static_cast<ca::u8>(v >> 16));
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

/// 完整 classfile 头（magic/minor/major/cpCount）。
Bytes header(ca::u16 cpCount)
{
    Bytes b;
    put32(b, 0xCAFEBABEu);
    put16(b, 0);
    put16(b, 0x34);
    put16(b, cpCount);
    return b;
}

Bytes utf8Entry(std::string_view s)
{
    Bytes b;
    put8(b, 0x01);
    put16(b, static_cast<ca::u16>(s.size()));
    putRawText(b, s);
    return b;
}

Bytes classEntry(ca::u16 nameIdx)
{
    Bytes b;
    put8(b, 0x07);
    put16(b, nameIdx);
    return b;
}

/// bool hasIssues(kind) 形式的断言 helper。
bool hasKind(const std::vector<SanitizeIssue>& issues, SanitizeIssueKind kind)
{
    for (const auto& i : issues) {
        if (i.kind == kind) return true;
    }
    return false;
}

}   // namespace

// ── sanitizeClassFile：合法 classfile 零误报 ──
TEST(ClassFileSanitizeTest, wellFormedClassPassesClean)
{
    auto issues = sanitizeClassFile(buildNopClassBytes("com/example/Ok"));
    EXPECT_TRUE(issues.empty());
}

// ── BadHeader：截断 / 魔数错 ──
TEST(ClassFileSanitizeTest, badHeaderTruncatedAndBadMagic)
{
    auto truncated = sanitizeClassFile(Bytes{0xCA, 0xFE, 0xBA});
    ASSERT_EQ(truncated.size(), 1u);
    EXPECT_EQ(truncated[0].kind, SanitizeIssueKind::BadHeader);

    Bytes badMagic = header(1);
    badMagic[0]    = 0x00;
    auto issues    = sanitizeClassFile(badMagic);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].kind, SanitizeIssueKind::BadHeader);
}

// ── BadCpTag：非法 tag ──
TEST(ClassFileSanitizeTest, badCpTagReported)
{
    Bytes b = header(2);
    put8(b, 0x2A);   // 非法 tag
    put16(b, 1);
    auto issues = sanitizeClassFile(b);
    ASSERT_FALSE(issues.empty());
    EXPECT_EQ(issues[0].kind, SanitizeIssueKind::BadCpTag);
    EXPECT_NE(issues[0].message.to_std_string().find("unknown constant tag 42"),
              std::string::npos);
}

// ── RefTagMismatch：引用指向 tag 不符的条目 ──
TEST(ClassFileSanitizeTest, refTagMismatchReported)
{
    // cp: 1=Utf8"T" 2=Class→1 3=Class→2（Class.name_index 指向 Class，应为 Utf8）
    Bytes b = header(4);
    appendInto(b, utf8Entry("T"));
    appendInto(b, classEntry(1));
    appendInto(b, classEntry(2));
    put16(b, 0x0021);   // access
    put16(b, 2);        // this_class
    put16(b, 0);        // super_class
    put16(b, 0);        // ifaces
    put16(b, 0);        // fields
    put16(b, 0);        // methods
    put16(b, 0);        // class attrs

    auto issues = sanitizeClassFile(b);
    ASSERT_TRUE(hasKind(issues, SanitizeIssueKind::RefTagMismatch));
    const SanitizeIssue* mismatch = nullptr;
    for (const auto& i : issues) {
        if (i.kind == SanitizeIssueKind::RefTagMismatch) mismatch = &i;
    }
    ASSERT_NE(mismatch, nullptr);
    EXPECT_EQ(mismatch->cpIndex, 3u);
    EXPECT_NE(mismatch->message.to_std_string().find("expected CONSTANT_Utf8, got CONSTANT_Class"),
              std::string::npos);
}

// ── RefIndexOutOfRange：this_class=0 / long-double 保留槽 ──
TEST(ClassFileSanitizeTest, refIndexOutOfRangeVariants)
{
    // this_class=0（null reference）。CP 为空（count=1）不加条目——utf8Entry
    // 字节若落在 CP 之后会被结构走查误读成 access/this_class。
    Bytes b = header(1);
    put16(b, 0x0021);
    put16(b, 0);   // this_class = 0
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    auto issues = sanitizeClassFile(b);
    ASSERT_TRUE(hasKind(issues, SanitizeIssueKind::RefIndexOutOfRange));
    EXPECT_NE(issues[0].message.to_std_string().find("(null reference)"), std::string::npos);

    // 引用指向 long/double 第二槽（保留槽）
    Bytes b2 = header(3);
    put8(b2, 0x05);   // Long（占 1、2 两槽）
    put32(b2, 0);
    put32(b2, 0);
    put16(b2, 0x0021);
    put16(b2, 2);   // this_class → 保留槽
    put16(b2, 0);
    put16(b2, 0);
    put16(b2, 0);
    put16(b2, 0);
    put16(b2, 0);
    auto issues2 = sanitizeClassFile(b2);
    ASSERT_TRUE(hasKind(issues2, SanitizeIssueKind::RefIndexOutOfRange));
    bool reserved = false;
    for (const auto& i : issues2) {
        if (i.message.to_std_string().find("reserved slot") != std::string::npos) reserved = true;
    }
    EXPECT_TRUE(reserved);
}

// ── BadMethodHandleKind：reference_kind 不在 [1,9] ──
TEST(ClassFileSanitizeTest, badMethodHandleKindReported)
{
    // cp: 1=Utf8"T" 2=Class→1 3=MethodHandle(kind=0, ref=2)
    Bytes b = header(4);
    appendInto(b, utf8Entry("T"));
    appendInto(b, classEntry(1));
    put8(b, 0x0F);   // MethodHandle
    put8(b, 0);      // reference_kind = 0（非法）
    put16(b, 2);
    put16(b, 0x0021);
    put16(b, 2);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    auto issues = sanitizeClassFile(b);
    ASSERT_TRUE(hasKind(issues, SanitizeIssueKind::BadMethodHandleKind));
}

// ── BootstrapIndexOutOfRange：InvokeDynamic 无 BootstrapMethods 属性 ──
TEST(ClassFileSanitizeTest, bootstrapIndexOutOfRangeWithoutBsmAttr)
{
    // cp: 1=Utf8"T" 2=Class→1 3=Utf8"n" 4=Utf8"()V" 5=NAT(3,4) 6=InvokeDynamic(0,5)
    Bytes b = header(7);
    appendInto(b, utf8Entry("T"));
    appendInto(b, classEntry(1));
    appendInto(b, utf8Entry("n"));
    appendInto(b, utf8Entry("()V"));
    put8(b, 0x0C);
    put16(b, 3);
    put16(b, 4);
    put8(b, 0x12);   // InvokeDynamic
    put16(b, 0);     // bsm index
    put16(b, 5);
    put16(b, 0x0021);
    put16(b, 2);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    auto issues = sanitizeClassFile(b);
    ASSERT_TRUE(hasKind(issues, SanitizeIssueKind::BootstrapIndexOutOfRange));
    EXPECT_NE(issues[0].message.to_std_string().find("requires a BootstrapMethods attribute"),
              std::string::npos);
}

// ── CpCycle：Dynamic → bootstrap args 引用自身（常量解析无界递归向量）──
TEST(ClassFileSanitizeTest, cpCycleViaBootstrapArgsDetected)
{
    // cp: 1=Utf8"T" 2=Class→1 3=Utf8"n" 4=Utf8"()V" 5=NAT(3,4) 6=Dynamic(0,5)
    //     7=Utf8"BootstrapMethods" 8=Methodref(2,5) 9=MethodHandle(6,8)
    // class attrs: [BootstrapMethods] count=1 ref=9 args=[6] → Dynamic(6) 自引
    Bytes b = header(10);
    appendInto(b, utf8Entry("T"));
    appendInto(b, classEntry(1));
    appendInto(b, utf8Entry("n"));
    appendInto(b, utf8Entry("()V"));
    put8(b, 0x0C);
    put16(b, 3);
    put16(b, 4);
    put8(b, 0x11);   // Dynamic
    put16(b, 0);     // bsm index
    put16(b, 5);
    appendInto(b, utf8Entry("BootstrapMethods"));
    put8(b, 0x0A);   // Methodref
    put16(b, 2);
    put16(b, 5);
    put8(b, 0x0F);   // MethodHandle
    put8(b, 6);      // REF_invokeStatic
    put16(b, 8);
    put16(b, 0x0021);
    put16(b, 2);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    // class attrs：1 个 BootstrapMethods
    put16(b, 1);
    put16(b, 7);   // name → "BootstrapMethods"
    put32(b, 8);   // length: count(2)+ref(2)+argc(2)+arg(2)
    put16(b, 1);   // bsm count
    put16(b, 9);   // method_ref → MethodHandle
    put16(b, 1);   // argc
    put16(b, 6);   // argument → Dynamic 自身

    auto issues = sanitizeClassFile(b);
    ASSERT_TRUE(hasKind(issues, SanitizeIssueKind::CpCycle));
    EXPECT_NE(issues[0].message.to_std_string().find("constant pool cycle"), std::string::npos);
}

// ── joinSanitizeIssues：单行契约 + 截断标注 ──
TEST(ClassFileSanitizeTest, joinIssuesSingleLineWithTruncation)
{
    std::vector<SanitizeIssue> issues;
    for (int i = 0; i < 3; ++i) {
        issues.push_back(SanitizeIssue{SanitizeIssueKind::BadCpTag, 0, 0,
                                       ca::str::Utf8String::from_cstr("issue")});
    }
    auto joined = joinSanitizeIssues(issues);
    EXPECT_EQ(joined.to_std_string(), "3 issue(s): issue; issue; issue");

    auto truncated = joinSanitizeIssues(issues, 2);
    EXPECT_EQ(truncated.to_std_string(), "3 issue(s): issue; issue; ...(+1 more)");

    EXPECT_TRUE(joinSanitizeIssues({}).is_empty());
}

// ── ClassFileSanitizeGate：默认档位拒绝路径（fail-fast，资源不剔除）──
TEST(ClassFileSanitizeGateTest, defaultTierThrowsAndKeepsResource)
{
    mjbase::model::ResourceContainer container;
    auto bad = std::make_unique<mjbase::model::ClassResource>(
        "Bad.class", std::vector<ca::u8>{0xCA, 0xFE, 0xBA});
    auto* badRaw = bad.get();
    container.addResource("Bad.class", std::move(bad));
    container.addResource("Good.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "Good.class", buildNopClassBytes("Good")));

    ClassFileSanitizeGate gate(PassPipelineOptions{});
    GlobalAnalysisManager gam(tSanitizeArena);
    try {
        gate.run(container, gam);
        FAIL() << "malformed class should be rejected";
    }
    catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("Bad.class"), std::string::npos);
    }
    // 默认档位作业级原子：资源不剔除，bytes 保持原样。
    EXPECT_TRUE(container.hasResource("Bad.class"));
    EXPECT_EQ(badRaw->get(), (std::vector<ca::u8>{0xCA, 0xFE, 0xBA}));
    // 好类不受影响（仍在容器中）。
    EXPECT_TRUE(container.hasResource("Good.class"));
}

// ── ClassFileSanitizeGate：容错档位剔除 + 诊断（reason=sanitize_error）──
TEST(ClassFileSanitizeGateTest, faultTierErasesAndReports)
{
    mjbase::model::ResourceContainer container;
    container.addResource("Bad.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "Bad.class", std::vector<ca::u8>{0xCA, 0xFE, 0xBA}));
    container.addResource("Good.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "Good.class", buildNopClassBytes("Good")));

    PassFaultReport     report;
    PassPipelineOptions options;
    options.perClassFaultIsolation = true;
    options.faultReport            = &report;

    ClassFileSanitizeGate gate(options);
    GlobalAnalysisManager gam(tSanitizeArena);
    auto                  pa = gate.run(container, gam);

    // 坏类被剔除：下游 pass 与导出均不再见；好类保留。
    EXPECT_FALSE(container.hasResource("Bad.class"));
    EXPECT_TRUE(container.hasResource("Good.class"));
    EXPECT_FALSE(pa.areAllPreserved());   // 剔除即容器改动

    ASSERT_EQ(report.skippedCount(), 1u);
    const auto& entry = report.entries.front();
    EXPECT_EQ(entry.scope, "class");
    EXPECT_EQ(entry.target, "Bad.class");
    EXPECT_EQ(entry.reason, "sanitize_error");
    EXPECT_FALSE(entry.detail.is_empty());
}

// ── 空 bytes 资源沿既有契约放行 ──
TEST(ClassFileSanitizeGateTest, emptyBytesResourcePasses)
{
    mjbase::model::ResourceContainer container;
    container.addResource(
        "Empty.class",
        std::make_unique<mjbase::model::ClassResource>("Empty.class", std::vector<ca::u8>{}));

    PassFaultReport     report;
    PassPipelineOptions options;
    options.perClassFaultIsolation = true;
    options.faultReport            = &report;

    ClassFileSanitizeGate gate(options);
    GlobalAnalysisManager gam(tSanitizeArena);
    auto                  pa = gate.run(container, gam);
    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_TRUE(report.empty());
    EXPECT_TRUE(container.hasResource("Empty.class"));
}
