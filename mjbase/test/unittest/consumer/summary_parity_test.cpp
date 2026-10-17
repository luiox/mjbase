/// @file        summary_parity_test.cpp
/// @brief       E3 骨架扫描对拍与失败面用例：mbase listClassSummaries 口径
///              逐字段对拍 + KeyPool 驻留稳定性 + fail-closed 错误路径。
///
/// 与 class_source_test.cpp 互补：那边钉 API 行为（fail-closed/枚举序/
/// first-wins/hasCode 分流），本文件钉扫描产物的字段级口径——
/// internalName/superName/access/version/interfaceCount/fieldCount/methodCount
/// 与 mbase WorkspaceBase::listClassSummaries 逐字段相等（version 编码同口径
/// `(minor << 16) | major`，即旧栈 visit(version) 的 V1_8 形态）；期望值全部
/// 由夹具规格独立给出，不经被测代码换算。降级读入（Recovered）照常出骨架、
/// 截断逐字节全 sweep 拒收，也在此钉死。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/core/bytes.hpp"

#include "mjbase/consumer/class_source.hpp"
#include "mjbase/consumer/summary_scanner.hpp"
#include "mjbase/keys.hpp"
#include "shared/class_bytes_builder.hpp"

namespace {

using Bytes = std::vector<ca::u8>;

using mjbase::ClassKey;
using mjbase::KeyPool;
using mjbase::consumer::ClassSummary;
using mjbase::consumer::ErrorCode;
using mjbase::consumer::SummaryScanner;
using mjbase::reference::test_support::ClassBytesBuilder;
using mjbase::reference::test_support::FieldSpec;
using mjbase::reference::test_support::MethodSpec;

/// 对拍夹具：public final 类、自定义父类、双接口、双字段、三方法
///（一具体一 abstract 一 static final）——mbase 摘要的每个计数字段都非平凡。
/// 汇编器固定 V1_8（minor=0, major=52）。
Bytes makeParityClass()
{
    ClassBytesBuilder b("com/x/Sample");
    b.setAccess(0x0031);   // public final super
    b.setSuper("com/i/Base");
    b.setInterfaces({"com/i/ShapeA", "com/i/ShapeB"});
    b.addField(FieldSpec{0x0002, "count", "I"});
    b.addField(FieldSpec{0x001A, "name", "Ljava/lang/String;"});   // private static final
    std::vector<MethodSpec> methods;
    methods.push_back(MethodSpec{0x0009, "run", "()V", "", 1, 1,
                                 [](mjbase::reference::test_support::CodeEmit& e) {
                                     e.op(jbc::core::Op::RETURN);
                                 }});
    methods.push_back(MethodSpec{0x0401, "shape", "(I)I"});   // public abstract：无体
    methods.push_back(MethodSpec{0x001A, "describe", "(Ljava/lang/Object;)Ljava/lang/String;", "", 1, 1,
                                 [](mjbase::reference::test_support::CodeEmit& e) {
                                     e.op(jbc::core::Op::ACONST_NULL);
                                     e.op(jbc::core::Op::ARETURN);
                                 }});
    return b.assemble(methods);
}

/// E3-9：mbase listClassSummaries 口径逐字段对拍——期望值由夹具规格独立给出，
/// 成员序 = classfile 声明序（计数即容器 size，与 mbase 计数口径相等）。
TEST(MjConsumerSummaryParityTest, FieldsMatchMbaseListClassSummariesContract)
{
    const Bytes               bytes   = makeParityClass();
    KeyPool                   pool;
    const SummaryScanner      scanner;
    auto                      scanned = scanner.scan(jbc::core::ByteView{bytes}, pool);
    ASSERT_TRUE(scanned.is_ok());
    const ClassSummary& s = scanned.unwrap();

    // mbase 摘要七字段逐一对拍（internalName/superName/access/version/
    // interfaceCount/fieldCount/methodCount）。
    EXPECT_EQ(pool.str(s.self.id), "com/x/Sample");
    EXPECT_EQ(pool.str(s.super.id), "com/i/Base");
    EXPECT_EQ(s.access, 0x0031u);
    EXPECT_EQ(s.version, 52u);   // V1_8 = (0 << 16) | 52，旧栈 visit(version) 同形
    ASSERT_EQ(s.interfaces.size(), 2u);   // interfaceCount
    ASSERT_EQ(s.fields.size(), 2u);       // fieldCount
    ASSERT_EQ(s.methods.size(), 3u);      // methodCount

    // 接口序 = classfile 声明序。
    EXPECT_EQ(pool.str(s.interfaces[0].id), "com/i/ShapeA");
    EXPECT_EQ(pool.str(s.interfaces[1].id), "com/i/ShapeB");

    // 成员签名（消费方在计数之外的交付面）：名字/描述符/access 按声明序。
    EXPECT_EQ(pool.str(s.methods[0].name), "run");
    EXPECT_EQ(pool.str(s.methods[0].desc), "()V");
    EXPECT_EQ(s.methods[0].access, 0x0009u);
    EXPECT_TRUE(s.methods[0].hasCode);
    EXPECT_EQ(pool.str(s.methods[1].name), "shape");
    EXPECT_EQ(pool.str(s.methods[1].desc), "(I)I");
    EXPECT_EQ(s.methods[1].access, 0x0401u);
    EXPECT_FALSE(s.methods[1].hasCode);
    EXPECT_EQ(pool.str(s.methods[2].name), "describe");
    EXPECT_EQ(pool.str(s.methods[2].desc), "(Ljava/lang/Object;)Ljava/lang/String;");
    EXPECT_EQ(s.methods[2].access, 0x001Au);

    EXPECT_EQ(pool.str(s.fields[0].name), "count");
    EXPECT_EQ(pool.str(s.fields[0].desc), "I");
    EXPECT_EQ(s.fields[0].access, 0x0002u);
    EXPECT_EQ(pool.str(s.fields[1].name), "name");
    EXPECT_EQ(pool.str(s.fields[1].desc), "Ljava/lang/String;");
    EXPECT_EQ(s.fields[1].access, 0x001Au);
}

/// E3-10：version 编码 `(minor << 16) | major`——minor 非零时高位段生效
///（汇编器固定 minor=0，这里按 classfile 布局直接改头 4..5 字节）。
TEST(MjConsumerSummaryParityTest, VersionEncodesMinorInHighBits)
{
    Bytes bytes = makeParityClass();
    ASSERT_GE(bytes.size(), 8u);
    bytes[4] = 0x00;
    bytes[5] = 0x03;   // minor_version = 3（big-endian，偏移 4..5）

    KeyPool              pool;
    const SummaryScanner scanner;
    auto                 scanned = scanner.scan(jbc::core::ByteView{bytes}, pool);
    ASSERT_TRUE(scanned.is_ok());
    EXPECT_EQ(scanned.unwrap().version, (3u << 16) | 52u);
}

/// E3-11：KeyPool 驻留稳定性——同字节重扫恒等同 id；预驻留名字复用既有 id；
/// 池继续增长不改变既有键；invalid super（Object）不新占驻留槽。
TEST(MjConsumerSummaryParityTest, KeyPoolInterningIsStableAcrossScans)
{
    const Bytes bytes = makeParityClass();
    KeyPool     pool;
    const SummaryScanner scanner;

    auto first = scanner.scan(jbc::core::ByteView{bytes}, pool);
    ASSERT_TRUE(first.is_ok());
    const ClassSummary one = first.unwrap();

    // 预驻留一个将出现在摘要里的名字：扫描必须复用同一 id。
    const ca::u32 preInterned = pool.intern("com/i/Base");

    auto second = scanner.scan(jbc::core::ByteView{bytes}, pool);
    ASSERT_TRUE(second.is_ok());
    const ClassSummary two = second.unwrap();

    EXPECT_EQ(two.self.id, one.self.id);
    EXPECT_EQ(two.super.id, one.super.id);
    EXPECT_EQ(two.super.id, preInterned);
    ASSERT_EQ(two.interfaces.size(), one.interfaces.size());
    for (ca::usize i = 0; i < two.interfaces.size(); ++i) {
        EXPECT_EQ(two.interfaces[i].id, one.interfaces[i].id);
    }
    ASSERT_EQ(two.methods.size(), one.methods.size());
    for (ca::usize i = 0; i < two.methods.size(); ++i) {
        EXPECT_EQ(two.methods[i].name, one.methods[i].name);
        EXPECT_EQ(two.methods[i].desc, one.methods[i].desc);
        EXPECT_EQ(two.methods[i].access, one.methods[i].access);
        EXPECT_EQ(two.methods[i].hasCode, one.methods[i].hasCode);
    }
    ASSERT_EQ(two.fields.size(), one.fields.size());
    for (ca::usize i = 0; i < two.fields.size(); ++i) {
        EXPECT_EQ(two.fields[i].name, one.fields[i].name);
        EXPECT_EQ(two.fields[i].desc, one.fields[i].desc);
    }

    // 不同内容不同 id；池再驻留新类名后既有键不变（视图稳定）。
    EXPECT_NE(one.self.id, one.super.id);
    const ca::u32 selfBefore = one.self.id;
    const ca::u32 other      = pool.intern("com/y/Other");
    EXPECT_EQ(pool.str(selfBefore), "com/x/Sample");
    EXPECT_NE(other, selfBefore);
}

/// E3-12：Object 自身无 super 条目——invalid 键且不新占驻留槽（0 号槽即
/// 空内容哨兵，扫描不得为空父名追加条目）。
TEST(MjConsumerSummaryParityTest, MissingSuperConsumesNoPoolSlot)
{
    ClassBytesBuilder builder("java/lang/Object");
    builder.setNoSuper();
    std::vector<MethodSpec> methods;
    methods.push_back(MethodSpec{0x0009, "registerNatives", "()V"});   // static native：无体
    const Bytes bytes = builder.assemble(methods);

    KeyPool              pool;
    const SummaryScanner scanner;
    auto                 scanned = scanner.scan(jbc::core::ByteView{bytes}, pool);
    ASSERT_TRUE(scanned.is_ok());
    const ClassSummary& s = scanned.unwrap();
    EXPECT_FALSE(s.super.valid());
    EXPECT_TRUE(pool.str(s.super.id).empty());
    // 槽位账：0 号哨兵 + this + 方法名 + 描述符，恰 4 条，无空父名条目。
    EXPECT_EQ(pool.size(), 4u);
}

/// E3-13：截断 fail-closed——合法类字节从头到尾逐字节截断（全部前缀长度），
/// 每个截断点都必须 Err(BadClassFile)，绝不产出半截摘要。
TEST(MjConsumerSummaryParityTest, EveryTruncationLengthIsRejected)
{
    const Bytes              bytes  = makeParityClass();
    KeyPool                  pool;
    const SummaryScanner     scanner;
    ca::usize                rejected = 0;
    for (ca::usize cut = 0; cut < bytes.size(); ++cut) {
        auto scanned = scanner.scan(jbc::core::ByteView{bytes.data(), cut}, pool);
        ASSERT_TRUE(scanned.is_err()) << "truncation accepted at length " << cut;
        EXPECT_EQ(scanned.unwrap_err().code, ErrorCode::BadClassFile);
        ++rejected;
    }
    EXPECT_EQ(rejected, bytes.size());
}

/// E3-14：降级读入（Recovered，结构可确定族）照常出完整骨架——尾部多余
/// 字节不拒收、不缺字段，骨架与干净字节扫描逐字段等同。
TEST(MjConsumerSummaryParityTest, RecoveredTrailingBytesStillYieldFullSkeleton)
{
    const Bytes clean = makeParityClass();

    KeyPool              pool;
    const SummaryScanner scanner;
    auto                 baseline = scanner.scan(jbc::core::ByteView{clean}, pool);
    ASSERT_TRUE(baseline.is_ok());
    const ClassSummary cleanSummary = baseline.unwrap();

    Bytes degraded = clean;
    degraded.push_back(0xDE);
    degraded.push_back(0xAD);
    degraded.push_back(0xBE);
    degraded.push_back(0xEF);
    auto scanned = scanner.scan(jbc::core::ByteView{degraded}, pool);
    ASSERT_TRUE(scanned.is_ok());
    const ClassSummary& s = scanned.unwrap();

    EXPECT_EQ(s.self.id, cleanSummary.self.id);
    EXPECT_EQ(s.super.id, cleanSummary.super.id);
    ASSERT_EQ(s.interfaces.size(), cleanSummary.interfaces.size());
    ASSERT_EQ(s.methods.size(), cleanSummary.methods.size());
    ASSERT_EQ(s.fields.size(), cleanSummary.fields.size());
    EXPECT_EQ(s.access, cleanSummary.access);
    EXPECT_EQ(s.version, cleanSummary.version);
    for (ca::usize i = 0; i < s.methods.size(); ++i) {
        EXPECT_EQ(s.methods[i].name, cleanSummary.methods[i].name);
        EXPECT_EQ(s.methods[i].hasCode, cleanSummary.methods[i].hasCode);
    }
    for (ca::usize i = 0; i < s.fields.size(); ++i) {
        EXPECT_EQ(s.fields[i].name, cleanSummary.fields[i].name);
    }
}

}   // namespace
