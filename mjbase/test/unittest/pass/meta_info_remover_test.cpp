/// meta 元数据清理家族（批 B 类级骨架组 13 件）单测：移植自
/// mbase/unittest/pass/opt/clean_pass_test.cpp 的 meta 段用例，并按新契约补齐
/// 载体特有面——三层属性槽摘除/槽内过滤的落盘位置、access 标志位双面同步、
/// Code 嵌套属性表的有界破例面（家族头注）、条件 PA。注解族载荷按 JVMS
/// 4.7.16-4.7.21 手拼（本夹具只用空键值对的简单形态）。
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/pass/meta_info_remover.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

using jbc::core::AnnotationsAttr;
using jbc::core::AttrKind;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::MemberInfo;
using jbc::core::RawAttr;
using jbc::core::TypeAnnotationsAttr;

// ── 载荷拼装（CP 索引经 builder 先行物化）──

/// 注解表（Runtime{Visible,Invisible}Annotations）：u2 条数 + {u2 类型描述符
/// CP, u2 键值对数=0}*。
Bytes annoTable(FixtureBuilder& b, std::initializer_list<const char*> descs)
{
    Bytes out;
    putU16(out, static_cast<ca::u16>(descs.size()));
    for (const char* d : descs) {
        putU16(out, b.utf8(d));
        putU16(out, 0);
    }
    return out;
}

/// type annotation 表（Runtime{Visible,Invisible}TypeAnnotations）：u2 条数 +
/// {u1 target_type, 空 target_info, u1 路径长=0, u2 描述符 CP, u2 键值对=0}*。
/// 空 target_info 仅对 0x13/0x14/0x15 形态成立——本夹具只用这三类（含挂在
/// Code 嵌套槽的合成形态：libjbc 按条目自身 target_type 解码，不验挂载点）。
Bytes typeAnnoTable(FixtureBuilder& b, ca::u8 targetType, std::initializer_list<const char*> descs)
{
    Bytes out;
    putU16(out, static_cast<ca::u16>(descs.size()));
    for (const char* d : descs) {
        putU8(out, targetType);
        putU8(out, 0);
        putU16(out, b.utf8(d));
        putU16(out, 0);
    }
    return out;
}

/// 参数注解（Runtime{Visible,Invisible}ParameterAnnotations）：u2 参数数=1 +
/// u2 注解数 + 注解表（简单形态）。
Bytes paramAnnoTable(FixtureBuilder& b, std::initializer_list<const char*> descs)
{
    Bytes out;
    putU16(out, 1);
    putU16(out, static_cast<ca::u16>(descs.size()));
    for (const char* d : descs) {
        putU16(out, b.utf8(d));
        putU16(out, 0);
    }
    return out;
}

/// MethodParameters：u2 参数数 + {u2 名字 CP, u2 access}*。
Bytes methodParameters(FixtureBuilder&                     b,
                       std::initializer_list<std::pair<const char*, ca::u16>> params)
{
    Bytes out;
    putU16(out, static_cast<ca::u16>(params.size()));
    for (const auto& [n, acc] : params) {
        putU16(out, b.utf8(n));
        putU16(out, acc);
    }
    return out;
}

/// Signature：u2 签名串 CP。
Bytes signaturePayload(FixtureBuilder& b, const char* sig)
{
    Bytes out;
    putU16(out, b.utf8(sig));
    return out;
}

/// SourceFile：u2 源文件名 CP。
Bytes sourceFilePayload(FixtureBuilder& b, const char* name)
{
    Bytes out;
    putU16(out, b.utf8(name));
    return out;
}

/// AnnotationDefault：element_value（'B' 常量形态：tag + u2 Integer CP）。
Bytes annotationDefaultPayload(FixtureBuilder& b, ca::i32 v)
{
    Bytes out;
    putU8(out, 'B');
    putU16(out, b.intConst(v));
    return out;
}

// ── 断言辅助 ──

MemberInfo* findMember(ClassFile& cf, std::vector<MemberInfo>& ms, std::string_view name)
{
    for (MemberInfo& m : ms) {
        if (cf.utf8(m.nameCp) == name) return &m;
    }
    return nullptr;
}

std::set<std::string> attrNames(const ClassFile& cf, const std::vector<AttrSlot>& attrs)
{
    std::set<std::string> out;
    for (const AttrSlot& s : attrs) out.emplace(std::string(cf.utf8(s.nameCp)));
    return out;
}

/// 注解槽内条目的类型描述符序列（保持原序）。
std::vector<std::string> annoDescs(ClassFile& cf, AttrSlot& slot)
{
    std::vector<std::string> out;
    auto                     decoded = cf.decodeAttr(slot);
    if (decoded.is_err()) return out;
    if (std::move(decoded).unwrap()->kind() != AttrKind::Annotations) return out;
    const auto* body = ca::core::tag_cast::cast<const AnnotationsAttr*>(slot.peek());
    for (const AnnotationsAttr::Annotation& a : body->annos) {
        out.emplace_back(cf.utf8(a.typeCp));
    }
    return out;
}

/// type annotation 槽内条目的类型描述符序列（保持原序）。
std::vector<std::string> typeAnnoDescs(ClassFile& cf, AttrSlot& slot)
{
    std::vector<std::string> out;
    auto                     decoded = cf.decodeAttr(slot);
    if (decoded.is_err()) return out;
    if (std::move(decoded).unwrap()->kind() != AttrKind::TypeAnnotations) return out;
    const auto* body = ca::core::tag_cast::cast<const TypeAnnotationsAttr*>(slot.peek());
    for (const TypeAnnotationsAttr::Annotation& a : body->annotations) {
        out.emplace_back(cf.utf8(a.typeIndexCp));
    }
    return out;
}

/// 类/成员上首个指定名注解槽的条目描述符（无槽为空）。
std::vector<std::string> firstAnnoDescs(ClassFile& cf, std::vector<AttrSlot>& attrs,
                                        const char* slotName)
{
    for (AttrSlot& slot : attrs) {
        if (cf.utf8(slot.nameCp) == slotName) return annoDescs(cf, slot);
    }
    return {};
}

CodeAttr* decodeCodeAttr(ClassFile& cf, MemberInfo& m)
{
    for (AttrSlot& slot : m.attrs) {
        if (cf.utf8(slot.nameCp) != "Code") continue;
        auto decoded = cf.decodeCode(m);
        if (decoded.is_err()) return nullptr;
        return decoded.unwrap();
    }
    return nullptr;
}

/// 组会话（按值收 builder，CP 物化在 assemble 内完成）。
std::shared_ptr<jbc::ir::ClassSession> makeSession(FixtureBuilder builder)
{
    const Bytes bytes  = builder.assemble();
    auto        result = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(result.is_ok());
    return std::move(result).unwrap();
}

}   // namespace

// ── MetaInfoRemover ──

TEST(MjPassMetaInfoRemoverTest, StripsListedMetaAcrossThreeLevels)
{
    FixtureBuilder builder("T");
    builder.addClassAttr({"SourceFile", sourceFilePayload(builder, "T.java")});
    builder.addClassAttr({"SourceDebugExtension", Bytes{'a', 'b', 'c'}});
    builder.addClassAttr({"RuntimeVisibleAnnotations", annoTable(builder, {"LFoo/Keep;"})});
    builder.addClassAttr(
        {"RuntimeVisibleTypeAnnotations", typeAnnoTable(builder, 0x13, {"LFoo/Keep;"})});
    builder.addField({"fin", "I", 0x0019,
                      {{"RuntimeVisibleAnnotations", annoTable(builder, {"LFoo/Keep;"})}}});
    builder.addMethod({"m", "()V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"RuntimeVisibleAnnotations", annoTable(builder, {"LFoo/Keep;"})},
                        {"RuntimeVisibleParameterAnnotations", paramAnnoTable(builder, {"LFoo/Keep;"})},
                        {"RuntimeInvisibleParameterAnnotations", paramAnnoTable(builder, {"LFoo/Keep;"})},
                        {"MethodParameters", methodParameters(builder, {{"p0", 0x0010}})},
                        {"AnnotationDefault", annotationDefaultPayload(builder, 7)}}});
    builder.addMethod({"debug", "()V", nopReturn(), 0x0009, {}, 1, 1, {},
                       {{"LineNumberTable", Bytes{0x00, 0x01, 0x00, 0x00, 0x00, 0x2A}},
                        {"LocalVariableTable", Bytes{0x00, 0x00}},
                        {"RuntimeVisibleTypeAnnotations",
                         typeAnnoTable(builder, 0x13, {"LFoo/Keep;"})}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::MetaInfoRemover     pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.changed());

    ClassFile& cf = session->backing();
    EXPECT_TRUE(attrNames(cf, cf.attrs).empty());
    ASSERT_EQ(cf.fields.size(), 1u);
    EXPECT_TRUE(attrNames(cf, cf.fields[0].attrs).empty());
    ASSERT_EQ(cf.methods.size(), 2u);
    for (MemberInfo& m : cf.methods) {
        const std::set<std::string> names = attrNames(cf, m.attrs);
        EXPECT_EQ(names.count("Code"), 1u) << "Code 必须存活";
        EXPECT_EQ(names.size(), 1u) << "方法级 meta 槽应全数摘除";
    }
    // Code 嵌套：debug 方法的 LNT/LVT/嵌套 TA 整表消失，Code 本体存活。
    CodeAttr* debugCode = decodeCodeAttr(cf, cf.methods[1]);
    ASSERT_NE(debugCode, nullptr);
    EXPECT_TRUE(debugCode->attrs.empty());
    EXPECT_EQ(debugCode->code.size(), 2u);   // NOP; RETURN 原样
    EXPECT_EQ(debugCode->maxStack, 1u);
}

TEST(MjPassMetaInfoRemoverTest, KeepsSignatureConstantValueAndUnknownAttrs)
{
    FixtureBuilder builder("T");
    builder.addClassAttr({"Signature", signaturePayload(builder, "Ljava/lang/List<Ljava/lang/String;>;")});
    builder.addClassAttr({"NotAStandardAttr", Bytes{0x01, 0x02}});
    builder.addField(
        {"cv", "I", 0x0009,
         {{"ConstantValue", [&builder] {
              Bytes p;
              putU16(p, builder.intConst(42));
              return p;
          }()},
          {"Signature", signaturePayload(builder, "I")}}});
    builder.addMethod({"m", "(I)V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"Signature", signaturePayload(builder, "(I)V")}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::MetaInfoRemover     pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_FALSE(pa.changed()) << "本夹具无任何可摘面——条件 PA 应 all()";

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"Signature", "NotAStandardAttr"}));
    EXPECT_EQ(attrNames(cf, cf.fields[0].attrs), (std::set<std::string>{"ConstantValue", "Signature"}));
    EXPECT_EQ(attrNames(cf, cf.methods[0].attrs), (std::set<std::string>{"Code", "Signature"}));
}

TEST(MjPassMetaInfoRemoverTest, CleanClassIsByteIdentical)
{
    FixtureBuilder builder("T");
    builder.addMethod({"m", "()V", bareReturn()});
    auto session = makeSession(builder);
    auto beforeResult = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(beforeResult.is_ok());
    const Bytes before = std::move(beforeResult).unwrap();

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::MetaInfoRemover     pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.areAllPreserved());

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(written.unwrap(), before);
}

// ── FinalRemover ──

TEST(MjPassFinalRemoverTest, ClearsFieldFinalTwoSided)
{
    FixtureBuilder builder("T");
    builder.addField({"fin", "I", 0x0019});   // public static final
    builder.addField({"plain", "I", 0x0009});
    builder.addMethod({"m", "()V", bareReturn()});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::FinalRemover        pass;
    const auto                        pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.changed());

    ClassFile&            cf   = session->backing();
    jbc::ir::ClassUnit&   unit = session->unit();
    EXPECT_EQ(cf.fields[0].access, 0x0009);
    EXPECT_EQ(unit.fields[0].access & 0x0010u, 0u) << "unit 侧 FINAL 须同步清除";
    EXPECT_EQ(cf.fields[1].access, 0x0009);
    EXPECT_EQ(unit.fields[1].access & 0x0010u, 0u);
    EXPECT_EQ(cf.methods[0].access, 0x0009) << "方法不受面";

    EXPECT_TRUE(pass.run(*session, cam).areAllPreserved()) << "二跑幂等";
}

// ── SyntheticBridgeRemover ──

TEST(MjPassSyntheticBridgeRemoverTest, ClearsSyntheticAndBridgeBits)
{
    FixtureBuilder builder("T");
    builder.setClassAccess(0x1021);   // public super synthetic
    builder.addMethod({"br", "()V", bareReturn(), 0x1041});   // public bridge synthetic
    builder.addMethod({"plain", "()V", bareReturn(), 0x0009});
    builder.addField({"syn", "I", 0x1019});   // public static final synthetic
    builder.addField({"plain", "I", 0x0009});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::SyntheticBridgeRemover pass;
    const auto                           pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.changed());

    ClassFile&          cf   = session->backing();
    jbc::ir::ClassUnit& unit = session->unit();
    EXPECT_EQ(cf.access, 0x0021);
    EXPECT_EQ(unit.access & 0x1000u, 0u);
    EXPECT_EQ(cf.methods[0].access, 0x0001);
    EXPECT_EQ((cf.methods[0].access & (0x1000 | 0x0040)), 0);
    EXPECT_EQ(cf.methods[1].access, 0x0009);
    EXPECT_EQ(cf.fields[0].access, 0x0019);   // 0x1019 去 synthetic
    EXPECT_EQ(cf.fields[1].access, 0x0009);

    EXPECT_TRUE(pass.run(*session, cam).areAllPreserved()) << "二跑幂等";
}

// ── KotlinMetadataRemover ──

TEST(MjPassKotlinMetadataRemoverTest, ClassVisibleOnlyAndJetbrainsAtMembers)
{
    FixtureBuilder builder("T");
    builder.addClassAttr(
        {"RuntimeVisibleAnnotations",
         annoTable(builder, {"Lkotlin/Metadata;", "Lcom/app/Keep;"})});
    builder.addClassAttr(
        {"RuntimeInvisibleAnnotations", annoTable(builder, {"Lkotlin/Metadata;"})});
    builder.addMethod({"m1", "()V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"RuntimeVisibleAnnotations",
                         annoTable(builder, {"Lorg/jetbrains/annotations/NotNull;"})}}});
    builder.addMethod({"m2", "()V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"RuntimeInvisibleAnnotations",
                         annoTable(builder, {"Lorg/jetbrains/annotations/Nullable;"})}}});
    builder.addMethod({"m3", "()V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"RuntimeVisibleAnnotations", annoTable(builder, {"Lkotlin/Metadata;"})}}});
    builder.addField({"f", "I", 0x0009,
                      {{"RuntimeVisibleAnnotations",
                        annoTable(builder, {"Lorg/jetbrains/annotations/JvmName;"})}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::KotlinMetadataRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    // 类级 visible 槽：kotlin 条目删除，幸存者保持。
    const std::vector<std::string> classVisible =
        firstAnnoDescs(cf, cf.attrs, "RuntimeVisibleAnnotations");
    ASSERT_EQ(classVisible.size(), 1u);
    EXPECT_EQ(classVisible[0], "Lcom/app/Keep;");
    // 类级 invisible 不在面内：kotlin 条目原样。
    const std::vector<std::string> classInvisible =
        firstAnnoDescs(cf, cf.attrs, "RuntimeInvisibleAnnotations");
    ASSERT_EQ(classInvisible.size(), 1u);
    EXPECT_EQ(classInvisible[0], "Lkotlin/Metadata;");
    // m1/m2：jetbrains 两槽全清；m3：方法级 kotlin 不在面内。
    MemberInfo* m1 = findMember(cf, cf.methods, "m1");
    MemberInfo* m2 = findMember(cf, cf.methods, "m2");
    MemberInfo* m3 = findMember(cf, cf.methods, "m3");
    ASSERT_NE(m1, nullptr);
    ASSERT_NE(m2, nullptr);
    ASSERT_NE(m3, nullptr);
    EXPECT_TRUE(attrNames(cf, m1->attrs).count("RuntimeVisibleAnnotations") == 0);
    EXPECT_TRUE(attrNames(cf, m2->attrs).count("RuntimeInvisibleAnnotations") == 0);
    const std::vector<std::string> m3Visible =
        firstAnnoDescs(cf, m3->attrs, "RuntimeVisibleAnnotations");
    ASSERT_EQ(m3Visible.size(), 1u);
    EXPECT_EQ(m3Visible[0], "Lkotlin/Metadata;");
    // 字段 jetbrains 清除。
    MemberInfo* f = findMember(cf, cf.fields, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(attrNames(cf, f->attrs).count("RuntimeVisibleAnnotations") == 0);
}

// ── SignatureRemover / SourceInfoRemover ──

TEST(MjPassSignatureRemoverTest, StripsAllThreeLevels)
{
    FixtureBuilder builder("T");
    builder.addClassAttr({"Signature", signaturePayload(builder, "Ljava/lang/List<Ljava/lang/String;>;")});
    builder.addClassAttr({"SourceFile", sourceFilePayload(builder, "T.java")});
    builder.addField({"f", "I", 0x0009, {{"Signature", signaturePayload(builder, "I")}}});
    builder.addMethod({"m", "(I)V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"Signature", signaturePayload(builder, "(I)V")}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::SignatureRemover    pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"SourceFile"}));
    EXPECT_TRUE(attrNames(cf, cf.fields[0].attrs).empty());
    EXPECT_EQ(attrNames(cf, cf.methods[0].attrs), (std::set<std::string>{"Code"}));
}

TEST(MjPassSourceInfoRemoverTest, StripsAndConditionalPa)
{
    FixtureBuilder builder("T");
    builder.addClassAttr({"SourceFile", sourceFilePayload(builder, "T.java")});
    builder.addClassAttr({"SourceDebugExtension", Bytes{'s', 'm', 'a', 'p'}});
    builder.addMethod({"m", "()V", bareReturn()});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::SourceInfoRemover   pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());
    EXPECT_TRUE(attrNames(session->backing(), session->backing().attrs).empty());

    // 干净类：零改动 → all()。
    FixtureBuilder clean("C");
    clean.addMethod({"m", "()V", bareReturn()});
    auto cleanSession = makeSession(clean);
    EXPECT_TRUE(pass.run(*cleanSession, cam).areAllPreserved());
}

// ── IllegalAnnotationRemover ──

TEST(MjPassIllegalAnnotationRemoverTest, FiltersMalformedEntriesKeepsValid)
{
    FixtureBuilder builder("T");
    // 混槽：合法幸存、非法（码点 ≤ 3）删除。
    builder.addClassAttr({"RuntimeVisibleAnnotations",
                          annoTable(builder, {"Lgood/Anno;", "-"})});
    // 纯非法槽：整槽消失（不以 L 开头）。
    builder.addField({"f", "I", 0x0009,
                      {{"RuntimeVisibleAnnotations", annoTable(builder, {"Xbad/Anno;"})}}});
    // 方法 invisible 槽同面（不以 ; 结尾）。
    builder.addMethod({"m", "()V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"RuntimeInvisibleAnnotations", annoTable(builder, {"Lbad/Anno"})}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena             arena;
    mjbase::spi::ClassAnalysisManager    cam(arena);
    mjbase::pass::IllegalAnnotationRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    const std::vector<std::string> classVisible =
        firstAnnoDescs(cf, cf.attrs, "RuntimeVisibleAnnotations");
    ASSERT_EQ(classVisible.size(), 1u);
    EXPECT_EQ(classVisible[0], "Lgood/Anno;");
    MemberInfo* f = findMember(cf, cf.fields, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(attrNames(cf, f->attrs).count("RuntimeVisibleAnnotations") == 0);
    MemberInfo* m = findMember(cf, cf.methods, "m");
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(attrNames(cf, m->attrs).count("RuntimeInvisibleAnnotations") == 0);
}

// ── IllegalSignatureRemover ──

TEST(MjPassIllegalSignatureRemoverTest, RemovesParserRejectedKeepsValid)
{
    FixtureBuilder builder("T");
    builder.addClassAttr({"Signature",
                          signaturePayload(builder, "<T:Ljava/lang/Object;>Ljava/lang/Object;")});
    builder.addField({"bad", "I", 0x0009, {{"Signature", signaturePayload(builder, "-")}}});
    builder.addField({"good", "Ljava/util/List;", 0x0009,
                      {{"Signature",
                        signaturePayload(builder, "Ljava/util/List<Ljava/lang/String;>;")}}});
    builder.addMethod({"bad", "(I)V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"Signature", signaturePayload(builder, "-")}}});
    builder.addMethod({"good", "()V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"Signature", signaturePayload(builder, "()V")}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena             arena;
    mjbase::spi::ClassAnalysisManager    cam(arena);
    mjbase::pass::IllegalSignatureRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"Signature"}))
        << "合法类签名保留";
    MemberInfo* bad  = findMember(cf, cf.fields, "bad");
    MemberInfo* good = findMember(cf, cf.fields, "good");
    ASSERT_NE(bad, nullptr);
    ASSERT_NE(good, nullptr);
    EXPECT_TRUE(attrNames(cf, bad->attrs).empty());
    EXPECT_EQ(attrNames(cf, good->attrs), (std::set<std::string>{"Signature"}));
    MemberInfo* mBad  = findMember(cf, cf.methods, "bad");
    MemberInfo* mGood = findMember(cf, cf.methods, "good");
    ASSERT_NE(mBad, nullptr);
    ASSERT_NE(mGood, nullptr);
    EXPECT_EQ(attrNames(cf, mBad->attrs), (std::set<std::string>{"Code"}));
    EXPECT_EQ(attrNames(cf, mGood->attrs), (std::set<std::string>{"Code", "Signature"}));
}

// ── IllegalTypeAnnotationRemover ──

TEST(MjPassIllegalTypeAnnotationRemoverTest, CoversMemberAndCodeNestedSlots)
{
    FixtureBuilder builder("T");
    // 混槽：合法幸存、非法（描述符 "-"）删除；target 0x13 = 字段类型。
    builder.addClassAttr(
        {"RuntimeVisibleTypeAnnotations", typeAnnoTable(builder, 0x13, {"Lgood/Anno;", "-"})});
    builder.addMethod({"m", "()V", nopReturn(), 0x0009, {}, 1, 1, {},
                       {{"RuntimeVisibleTypeAnnotations",
                         typeAnnoTable(builder, 0x13, {"-"})}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena                  arena;
    mjbase::spi::ClassAnalysisManager         cam(arena);
    mjbase::pass::IllegalTypeAnnotationRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    const std::vector<std::string> classTa = [&] {
        for (AttrSlot& slot : cf.attrs) {
            if (cf.utf8(slot.nameCp) == "RuntimeVisibleTypeAnnotations") {
                return typeAnnoDescs(cf, slot);
            }
        }
        return std::vector<std::string>{};
    }();
    ASSERT_EQ(classTa.size(), 1u);
    EXPECT_EQ(classTa[0], "Lgood/Anno;");

    MemberInfo* m = findMember(cf, cf.methods, "m");
    ASSERT_NE(m, nullptr);
    CodeAttr* code = decodeCodeAttr(cf, const_cast<MemberInfo&>(*m));
    ASSERT_NE(code, nullptr);
    bool nestedTaSurvived = false;
    for (AttrSlot& slot : code->attrs) {
        if (cf.utf8(slot.nameCp) == "RuntimeVisibleTypeAnnotations") nestedTaSurvived = true;
    }
    EXPECT_FALSE(nestedTaSurvived) << "Code 嵌套槽内删空后连槽摘除";
}

// ── IllegalSourceInfoRemover ──

TEST(MjPassIllegalSourceInfoRemoverTest, LegalKeptIllegalStripped)
{
    FixtureBuilder builder("T");
    builder.addClassAttr({"SourceFile", sourceFilePayload(builder, "1-evil.txt")});
    builder.addClassAttr({"SourceDebugExtension", Bytes{'G', 'o', 'o', 'd', '.', 'j', 'a', 'v', 'a'}});
    builder.addMethod({"m", "()V", bareReturn()});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena               arena;
    mjbase::spi::ClassAnalysisManager      cam(arena);
    mjbase::pass::IllegalSourceInfoRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"SourceDebugExtension"}))
        << "合法 SourceDebug 保留，非法 SourceFile 摘除";

    // 双合法：零改动 → all()。
    FixtureBuilder legal("L");
    legal.addClassAttr({"SourceFile", sourceFilePayload(legal, "Legal1.java")});
    legal.addMethod({"m", "()V", bareReturn()});
    auto legalSession = makeSession(legal);
    EXPECT_TRUE(pass.run(*legalSession, cam).areAllPreserved());
}

// ── UnknownAttributeCleaner ──

TEST(MjPassUnknownAttributeCleanerTest, RemovesUnknownKeepsStandardAndCode)
{
    FixtureBuilder builder("T");
    builder.addClassAttr({"SourceFile", sourceFilePayload(builder, "T.java")});
    builder.addClassAttr({"ObfMark", Bytes{0xAA, 0xBB}});
    builder.addField({"cv", "I", 0x0009,
                      {{"ConstantValue", [&builder] {
                           Bytes p;
                           putU16(p, builder.intConst(5));
                           return p;
                       }()},
                       {"ObfMark", Bytes{0x01}}}});
    builder.addMethod({"m", "()V", nopReturn(), 0x0009, {}, 1, 1,
                       {{"ObfMark", Bytes{0x02}}},
                       {{"UnknownNested", Bytes{0x03}}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena            arena;
    mjbase::spi::ClassAnalysisManager   cam(arena);
    mjbase::pass::UnknownAttributeCleaner pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    EXPECT_EQ(attrNames(cf, cf.attrs), (std::set<std::string>{"SourceFile"}));
    EXPECT_EQ(attrNames(cf, cf.fields[0].attrs), (std::set<std::string>{"ConstantValue"}));
    EXPECT_EQ(attrNames(cf, cf.methods[0].attrs), (std::set<std::string>{"Code"}));
    // Code 嵌套不在面内：未知嵌套槽原样保留。
    CodeAttr* code = decodeCodeAttr(cf, cf.methods[0]);
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(attrNames(cf, code->attrs), (std::set<std::string>{"UnknownNested"}));

    EXPECT_TRUE(pass.run(*session, cam).areAllPreserved()) << "二跑幂等";
}

// ── IllegalAccessRemover ──

TEST(MjPassIllegalAccessRemoverTest, MasksPerKindTwoSidedAndParamPayload)
{
    FixtureBuilder builder("T");
    builder.setClassAccess(0x0069);   // public super static volatile
    builder.addMethod({"mi", "()V", bareReturn(), 0x0201});   // public interface
    builder.addMethod({"ms", "()V", bareReturn(), 0x0021});   // public synchronized(合法别名)
    builder.addMethod(
        {"mp", "()V", bareReturn(), 0x0009, {}, 1, 1,
         {{"MethodParameters",
           methodParameters(builder,
                            {{"p0", 0x0010}, {"p1", 0x0001}, {"p2", 0x8000}})}}});
    builder.addField({"fe", "I", 0x4002});    // private enum（类非 enum → 清）
    builder.addField({"fn", "I", 0x0108});    // static native → 清 native
    builder.addField({"ft", "I", 0x0088});    // static transient（合法，保留）
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::IllegalAccessRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile&          cf   = session->backing();
    jbc::ir::ClassUnit& unit = session->unit();
    EXPECT_EQ(cf.access, 0x0021);
    EXPECT_EQ(unit.access & 0x0048u, 0u);

    MemberInfo* mi = findMember(cf, cf.methods, "mi");
    MemberInfo* ms = findMember(cf, cf.methods, "ms");
    ASSERT_NE(mi, nullptr);
    ASSERT_NE(ms, nullptr);
    EXPECT_EQ(mi->access, 0x0001);
    EXPECT_EQ(ms->access, 0x0021) << "方法 synchronized 是合法位，保留";

    MemberInfo* fe = findMember(cf, cf.fields, "fe");
    MemberInfo* fn = findMember(cf, cf.fields, "fn");
    MemberInfo* ft = findMember(cf, cf.fields, "ft");
    ASSERT_NE(fe, nullptr);
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(ft, nullptr);
    EXPECT_EQ(fe->access, 0x0002);
    EXPECT_EQ(fn->access, 0x0008);
    EXPECT_EQ(ft->access, 0x0088);

    // MethodParameters 载荷：PUBLIC 清、FINAL/MANDATED 幸存。
    MemberInfo* mp = findMember(cf, cf.methods, "mp");
    ASSERT_NE(mp, nullptr);
    AttrSlot* paramsSlot = nullptr;
    for (AttrSlot& slot : mp->attrs) {
        if (cf.utf8(slot.nameCp) == "MethodParameters") paramsSlot = &slot;
    }
    ASSERT_NE(paramsSlot, nullptr);
    auto decoded = cf.decodeAttr(*paramsSlot);
    ASSERT_TRUE(decoded.is_ok());
    const auto* body = ca::core::tag_cast::cast<const RawAttr*>(paramsSlot->peek());
    ASSERT_NE(body, nullptr);
    const std::vector<ca::u8>& bytes = body->bytes();
    ASSERT_EQ(bytes.size(), 2u + 3u * 4u);
    EXPECT_EQ((static_cast<ca::u16>(bytes[4]) << 8) | bytes[5], 0x0010)
        << "FINAL 位幸存";
    EXPECT_EQ((static_cast<ca::u16>(bytes[8]) << 8) | bytes[9], 0x0000)
        << "PUBLIC 位清除";
    EXPECT_EQ((static_cast<ca::u16>(bytes[12]) << 8) | bytes[13], 0x8000)
        << "MANDATED 位幸存";

    EXPECT_TRUE(pass.run(*session, cam).areAllPreserved()) << "二跑幂等";
}

TEST(MjPassIllegalAccessRemoverTest, LegacyU1CountParameterLayoutMasked)
{
    FixtureBuilder builder("T");
    // 旧栈 writer 的遗留布局：u1 count + {u2 name, u2 acc}*（5 字节）。
    builder.addMethod({"mp", "()V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"MethodParameters", Bytes{0x01, 0x00, 0x21, 0x80, 0x11}}}});
    builder.addMethod({"m", "()V", bareReturn()});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::IllegalAccessRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    MemberInfo* mp = findMember(cf, cf.methods, "mp");
    ASSERT_NE(mp, nullptr);
    AttrSlot* paramsSlot = nullptr;
    for (AttrSlot& slot : mp->attrs) {
        if (cf.utf8(slot.nameCp) == "MethodParameters") paramsSlot = &slot;
    }
    ASSERT_NE(paramsSlot, nullptr);
    auto decoded = cf.decodeAttr(*paramsSlot);
    ASSERT_TRUE(decoded.is_ok());
    const auto* body = ca::core::tag_cast::cast<const RawAttr*>(paramsSlot->peek());
    ASSERT_NE(body, nullptr);
    const std::vector<ca::u8>& bytes = body->bytes();
    ASSERT_EQ(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 0x01) << "u1 count 原样";
    EXPECT_EQ((static_cast<ca::u16>(bytes[3]) << 8) | bytes[4], 0x8010)
        << "PUBLIC 位清除，MANDATED 位幸存";
}

TEST(MjPassIllegalAccessRemoverTest, EnumClassKeepsFieldEnumBits)
{
    FixtureBuilder builder("T");
    builder.setClassAccess(0x4031);   // public final super enum
    builder.addField({"E1", "Lfoo/Color;", 0x4019});   // public static final enum
    builder.addMethod({"m", "()V", bareReturn()});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    mjbase::pass::IllegalAccessRemover pass;
    const auto                         pa = pass.run(*session, cam);

    EXPECT_FALSE(pa.changed()) << "enum 类的 enum 字段位全合法——零改动";
    EXPECT_EQ(session->backing().fields[0].access, 0x4019);
}

// ── SpecificAnnotationRemover ──

TEST(MjPassSpecificAnnotationRemoverTest, BlacklistAndMalformedFiltered)
{
    FixtureBuilder builder("T");
    builder.addClassAttr({"RuntimeVisibleAnnotations",
                          annoTable(builder, {"Ltech/origincloud/Mapping;", "Lcom/app/Keep;"})});
    builder.addMethod({"m", "()V", bareReturn(), 0x0009, {}, 1, 1,
                       {{"RuntimeInvisibleAnnotations",
                         annoTable(builder, {"Lorg/jetbrains/annotations/Nullable;", "-"})}}});
    builder.addField({"f", "I", 0x0009,
                      {{"RuntimeInvisibleAnnotations",
                        annoTable(builder, {"Ljvavx/annotation/NotNull;"})}}});
    auto session = makeSession(builder);

    ca::str::Utf8StringArena                arena;
    mjbase::spi::ClassAnalysisManager       cam(arena);
    mjbase::pass::SpecificAnnotationRemover pass;
    EXPECT_TRUE(pass.run(*session, cam).changed());

    ClassFile& cf = session->backing();
    const std::vector<std::string> classVisible =
        firstAnnoDescs(cf, cf.attrs, "RuntimeVisibleAnnotations");
    ASSERT_EQ(classVisible.size(), 1u);
    EXPECT_EQ(classVisible[0], "Lcom/app/Keep;");
    MemberInfo* m = findMember(cf, cf.methods, "m");
    ASSERT_NE(m, nullptr);
    EXPECT_TRUE(attrNames(cf, m->attrs).count("RuntimeInvisibleAnnotations") == 0);
    MemberInfo* f = findMember(cf, cf.fields, "f");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(attrNames(cf, f->attrs).count("RuntimeInvisibleAnnotations") == 0);
}

// ── 注册面：13 件全可按名构造且 scope=Class ──

TEST(MjPassMetaFamilyRegistryTest, CreatesAllThirteenByName)
{
    const char* names[] = {
        "MetaInfoRemover",           "FinalRemover",
        "SyntheticBridgeRemover",    "KotlinMetadataRemover",
        "SourceInfoRemover",         "SignatureRemover",
        "IllegalAnnotationRemover",  "IllegalSignatureRemover",
        "IllegalTypeAnnotationRemover", "IllegalSourceInfoRemover",
        "UnknownAttributeCleaner",   "IllegalAccessRemover",
        "SpecificAnnotationRemover",
    };
    mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    for (const char* name : names) {
        const auto entry = reg.find(name);
        ASSERT_TRUE(entry.is_ok()) << name;
        EXPECT_EQ(entry.unwrap().scope, mjbase::spi::PassScope::Class) << name;
        auto pass = reg.createClassPass(name);
        ASSERT_TRUE(pass.is_ok()) << name;
        EXPECT_NE(std::move(pass).unwrap(), nullptr) << name;
    }
}
