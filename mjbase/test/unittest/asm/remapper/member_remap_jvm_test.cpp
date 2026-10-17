/// @file        member_remap_jvm_test.cpp
/// @brief       member_remapper Nest 族 typed 手术路径的 JVM 真跑硬验收：
///              javac 编译 nestmate + sealed 样本（NestHost/NestMembers/
///              PermittedSubclasses 齐备）→ applyMemberRemapPlan 三件走
///              typed 体重映射 → 新名类真 java 执行，getNestHost/
///              getNestMembers/getPermittedSubclasses 反射读出新名、行为
///              输出与原样本一致。JDK 不可用整测跳过；JDK < 17（sealed
///              编译失败）按环境能力跳过。
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/asm/remapper/member_remapper.hpp"

#include "test_helper.hpp"
#include "jdk_fixture.hpp"

namespace {

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;

/// nestmate + sealed 样本（单文件多顶层类，唯一 public 是探针）：
/// - Host/Host$Token 真 nestmate（私有成员跨类直读）→ Token 带 NestHost、
///   Host 带 NestMembers；
/// - Seal sealed 接口 permits Impl → Seal 带 PermittedSubclasses；
/// - 探针打印三件反射读出与行为和（改名前后只差类名段）。
constexpr const char* kSampleSrc = R"JAVA(
package hb.jvm.nestremap;

public class NestSample {
    public static void main(String[] args) {
        Host h = new Host();
        Host.Token t = h.make();
        System.out.println("host=" + t.getClass().getNestHost().getName());
        System.out.println("members=" + t.getClass().getNestMembers().length);
        System.out.println("declaring=" + t.getClass().getDeclaringClass().getName());
        System.out.println("permits=" + Seal.class.getPermittedSubclasses()[0].getName());
        System.out.println("sealed=" + Seal.class.isSealed());
        System.out.println("v=" + (h.value() + t.read() + new Impl().v()));
    }
}

class Host {
    private int secret = 7;

    public class Token {
        public int read() {
            return secret;
        }
    }

    public Token make() {
        return new Token();
    }

    public int value() {
        return 35;
    }
}

sealed interface Seal permits Impl {
    int v();
}

final class Impl implements Seal {
    public int v() {
        return 1;
    }
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

std::string nestSampleOutput(std::string_view host, std::string_view token,
                             std::string_view seal, std::string_view impl)
{
    std::string out = "host=";
    out += host;
    out += "\nmembers=2\ndeclaring=";
    out += host;
    out += "\npermits=";
    out += impl;
    out += "\nsealed=true\nv=43\n";
    return out;
}

/// typed 面断言：改名后 NestHost/NestMembers/PermittedSubclasses 槽必须是
/// typed 体（Raw 形即分派失效），且各槽指向新名。重映射命中的槽已被
/// makeNew 置 New（typed 体挂在槽上、无源 span），未命中的槽仍是 Untouched
/// ——两种状态分别走槽体直读与公开解码入口。
void verifyNestFaceTyped(jbc::core::ClassFile& cf, std::string_view newHost,
                         std::string_view newToken, std::string_view newImpl)
{
    const auto findSlot = [&cf](std::string_view name) -> jbc::core::AttrSlot* {
        for (jbc::core::AttrSlot& slot : cf.attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, name)) return &slot;
        }
        return nullptr;
    };
    const auto classNameAt = [&cf](ca::u32 classCp) -> std::string {
        return std::string(cf.className(classCp));
    };

    if (jbc::core::AttrSlot* nh = findSlot("NestHost")) {
        std::unique_ptr<jbc::core::AttrBody> decoded;
        const jbc::core::AttrBody* bodyPtr = nh->body.get();
        if (bodyPtr == nullptr) {
            auto d = jbc::core::decodeNestHostBody(cf, *nh);
            ASSERT_TRUE(d.is_ok());
            decoded = std::move(d).unwrap();
            bodyPtr = decoded.get();
        }
        EXPECT_EQ(bodyPtr->kind(), jbc::core::AttrKind::NestHost)
            << "NestHost not typed after dispatch wiring";
        const auto& nhAttr = static_cast<const jbc::core::NestHostAttr&>(*bodyPtr);
        EXPECT_EQ(classNameAt(nhAttr.hostClassCp), newHost);
    }
    if (jbc::core::AttrSlot* nm = findSlot("NestMembers")) {
        std::unique_ptr<jbc::core::AttrBody> decoded;
        const jbc::core::AttrBody* bodyPtr = nm->body.get();
        if (bodyPtr == nullptr) {
            auto d = jbc::core::decodeNestMembersBody(cf, *nm);
            ASSERT_TRUE(d.is_ok());
            decoded = std::move(d).unwrap();
            bodyPtr = decoded.get();
        }
        EXPECT_EQ(bodyPtr->kind(), jbc::core::AttrKind::NestMembers)
            << "NestMembers not typed after dispatch wiring";
        const auto& nmAttr = static_cast<const jbc::core::NestMembersAttr&>(*bodyPtr);
        ASSERT_GE(nmAttr.memberCps.size(), 1U);
        bool found = false;
        for (const ca::u32 cp : nmAttr.memberCps) {
            if (classNameAt(cp) == newToken) found = true;
        }
        EXPECT_TRUE(found) << "NestMembers lost renamed member " << newToken;
    }
    if (jbc::core::AttrSlot* ps = findSlot("PermittedSubclasses")) {
        std::unique_ptr<jbc::core::AttrBody> decoded;
        const jbc::core::AttrBody* bodyPtr = ps->body.get();
        if (bodyPtr == nullptr) {
            auto d = jbc::core::decodePermittedSubclassesBody(cf, *ps);
            ASSERT_TRUE(d.is_ok());
            decoded = std::move(d).unwrap();
            bodyPtr = decoded.get();
        }
        EXPECT_EQ(bodyPtr->kind(), jbc::core::AttrKind::PermittedSubclasses)
            << "PermittedSubclasses not typed after dispatch wiring";
        const auto& psAttr =
            static_cast<const jbc::core::PermittedSubclassesAttr&>(*bodyPtr);
        ASSERT_GE(psAttr.permittedCps.size(), 1U);
        EXPECT_EQ(classNameAt(psAttr.permittedCps[0]), newImpl);
    }
}

}   // namespace

TEST(MemberRemapJvm, NestFaceRenamedClassesRunOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate_nest_remap";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin";
    const fs::path remapDir = gate / "remapped";

    // ── 1. 源码 + javac 编译（JDK < 17 sealed 编译失败 → 环境能力跳过）──
    writeSource(srcDir / "hb/jvm/nestremap/NestSample.java", kSampleSrc);
    std::string compileErr;
    if (!test::jdk::compileJava(tc, srcDir / "hb/jvm/nestremap/NestSample.java", binDir,
                                &compileErr)) {
        GTEST_SKIP() << "样本编译失败（JDK < 17 无 sealed）： " << compileErr;
    }

    // ── 2. 原样本基线运行（钉死样本形态：nest 反射读出 + 行为和）──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.nestremap.NestSample");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    EXPECT_EQ(test::jdk::normalizeOutput(orig.out),
              test::jdk::normalizeOutput(nestSampleOutput("hb.jvm.nestremap.Host",
                                                          "hb.jvm.nestremap.Host$Token",
                                                          "hb.jvm.nestremap.Seal",
                                                          "hb.jvm.nestremap.Impl")));

    // ── 3. applyMemberRemapPlan 四类映射（Token/Impl 同步改名 → NestMembers/
    //       PermittedSubclasses 表内条目有命中面），逐类改写并按新二进制名落盘。
    mjbase::asm_remapper::MemberRemapPlan plan;
    plan.mapClass("hb/jvm/nestremap/Host", "hb/jvm/nestremap/HostX");
    plan.mapClass("hb/jvm/nestremap/Host$Token", "hb/jvm/nestremap/Tkn");
    plan.mapClass("hb/jvm/nestremap/Seal", "hb/jvm/nestremap/SealX");
    plan.mapClass("hb/jvm/nestremap/Impl", "hb/jvm/nestremap/ImplX");

    bool anyChanged = false;
    ca::u32 nestSlotsSeen = 0;
    std::error_code ec;
    fs::remove_all(remapDir, ec);
    for (const char* simple : {"NestSample", "Host", "Host$Token", "Seal", "Impl"}) {
        const Bytes corpus =
            readClassBytes(binDir / "hb/jvm/nestremap" / (std::string(simple) + ".class"));
        ASSERT_FALSE(corpus.empty()) << simple;
        auto sr = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
        ASSERT_TRUE(sr.is_ok()) << simple;
        auto session = std::move(sr).unwrap();
        auto rr = mjbase::asm_remapper::applyMemberRemapPlan(
            *session, plan, mjbase::asm_remapper::RemapPolicy::bytecodeOnly());
        ASSERT_TRUE(rr.is_ok()) << simple;
        anyChanged |= rr.unwrap();

        jbc::core::ClassFile& cf = session->backing();
        const std::string newName(cf.className(cf.thisClass));
        // typed 面断言在新名落盘前钉住（NestSample 不带 Nest 槽，函数内按名寻槽）。
        verifyNestFaceTyped(cf, "hb/jvm/nestremap/HostX", "hb/jvm/nestremap/Tkn",
                            "hb/jvm/nestremap/ImplX");
        for (const jbc::core::AttrSlot& slot : cf.attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, "NestHost")
                || cf.cp.utf8Equals(slot.nameCp, "NestMembers")
                || cf.cp.utf8Equals(slot.nameCp, "PermittedSubclasses")) {
                ++nestSlotsSeen;
            }
        }

        auto w = jbc::core::writeClass(cf);
        ASSERT_TRUE(w.is_ok()) << simple;
        // className 是内部名（'/' 分隔），直接作为相对路径落盘——JVM 按名定位。
        const fs::path dst = remapDir / (newName + ".class");
        fs::create_directories(dst.parent_path());
        std::ofstream fout(dst, std::ios::binary);
        ASSERT_TRUE(fout.good()) << dst.string();
        fout.write(reinterpret_cast<const char*>(w.unwrap().data()),
                   static_cast<std::streamsize>(w.unwrap().size()));
    }
    EXPECT_TRUE(anyChanged) << "改名未发生";
    // 触点护栏：五类里三文件带 Nest 槽（Token=NestHost、Host=NestMembers、
    // Seal=PermittedSubclasses），typed 手术必须有真触面。
    EXPECT_EQ(nestSlotsSeen, 3u) << "样本 Nest 槽数目漂移，护栏失真";

    // ── 4. 硬验收：新名类真 java 执行——getNestHost/getNestMembers/
    //       getPermittedSubclasses 读新名，行为输出与原样本一致。
    const auto renamed =
        test::jdk::runJavaMain(tc, remapDir, "hb.jvm.nestremap.NestSample");
    EXPECT_EQ(renamed.exitCode, 0) << "renamed nest family rejected/failed:\n"
                                   << renamed.out;
    EXPECT_EQ(test::jdk::normalizeOutput(renamed.out),
              test::jdk::normalizeOutput(nestSampleOutput("hb.jvm.nestremap.HostX",
                                                          "hb.jvm.nestremap.Tkn",
                                                          "hb.jvm.nestremap.SealX",
                                                          "hb.jvm.nestremap.ImplX")));
}

// ---- Record 属性（JVMS 4.7.30）typed 手术路径的 JVM 真跑硬验收 -----------

/// record 样本（单文件多顶层类，唯一 public 是探针）：
/// - RecordSample 带 Record 属性：注解组件（RECORD_COMPONENT 目标 RUNTIME
///   注解 → 组件 RuntimeVisibleAnnotations）+ 泛型组件（Holder<String> →
///   组件 Signature）；紧凑规范构造器形态（校验体，编译器注入字段赋值）；
/// - 探针打印 isRecord/getRecordComponents 的名字/类型/注解/泛型读出与
///   行为和（改名前后只差组件名与泛型类名段）。
constexpr const char* kRecordSampleSrc = R"JAVA(
package hb.jvm.recordremap;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

public class RecSample {
    public static void main(String[] args) {
        RecordSample p = new RecordSample(7, new Holder<>("x"));
        System.out.println("rec=" + RecordSample.class.isRecord());
        java.lang.reflect.RecordComponent[] comps = RecordSample.class.getRecordComponents();
        System.out.println("n=" + comps.length);
        System.out.println("c0=" + comps[0].getName() + ":" + comps[0].getType().getName());
        System.out.println("c0mark=" + (comps[0].getAnnotation(Mark.class) != null));
        System.out.println("c1=" + comps[1].getName() + ":"
            + comps[1].getGenericType().getTypeName());
        System.out.println("v=" + p.value() + p.tag().get());
    }
}

@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.RECORD_COMPONENT, ElementType.FIELD, ElementType.METHOD})
@interface Mark {
}

class Holder<T> {
    private final T v;

    Holder(T v) {
        this.v = v;
    }

    T get() {
        return v;
    }
}

record RecordSample(@Mark int value, Holder<String> tag) {
    public RecordSample {
        if (value < 0) {
            throw new IllegalArgumentException("value < 0");
        }
    }
}
)JAVA";

std::string recordSampleOutput(std::string_view comp0, std::string_view holderDotted)
{
    std::string out = "rec=true\nn=2\nc0=";
    out += comp0;
    out += ":int\nc0mark=true\nc1=tag:";
    out += holderDotted;
    out += "<java.lang.String>\nv=7x\n";
    return out;
}

/// typed 面断言：改名后 Record 槽必须是 typed 体（Raw 形即分派失效），
/// 组件名/描述符读新值、嵌套 Signature 随类改名、注解组件嵌套槽原样。
void verifyRecordFaceTyped(jbc::core::ClassFile& cf)
{
    jbc::core::AttrSlot* slot = nullptr;
    for (jbc::core::AttrSlot& s : cf.attrs) {
        if (cf.cp.utf8Equals(s.nameCp, "Record")) slot = &s;
    }
    ASSERT_NE(slot, nullptr) << "sample lost Record attribute";
    // 手术后槽体已挂在槽上（makeNew 置 New、span 清空）：体优先，未触碰
    // 槽才走公开解码入口直读（与 Nest 面校验同一形态）。
    std::unique_ptr<jbc::core::AttrBody> decoded;
    const jbc::core::AttrBody* bodyPtr = slot->body.get();
    if (bodyPtr == nullptr) {
        auto d = jbc::core::decodeRecordBody(cf, *slot);
        ASSERT_TRUE(d.is_ok());
        decoded = std::move(d).unwrap();
        bodyPtr = decoded.get();
    }
    ASSERT_EQ(bodyPtr->kind(), jbc::core::AttrKind::Record)
        << "Record not typed after dispatch wiring";
    const auto& rec = static_cast<const jbc::core::RecordAttr&>(*bodyPtr);
    ASSERT_EQ(rec.components.size(), 2U);
    EXPECT_EQ(cf.cp.utf8Of(rec.components[0].nameCp), "val");
    EXPECT_EQ(cf.cp.utf8Of(rec.components[0].descriptorCp), "I");
    EXPECT_EQ(cf.cp.utf8Of(rec.components[1].nameCp), "tag");
    EXPECT_EQ(cf.cp.utf8Of(rec.components[1].descriptorCp), "Lhb/jvm/recordremap/Bearer;");
    // 组件 0：嵌套 RuntimeVisibleAnnotations 槽原样（注解类型未参与改名）。
    bool annoSeen = false;
    for (const jbc::core::AttrSlot& nested : rec.components[0].attrs) {
        if (cf.cp.utf8Equals(nested.nameCp, "RuntimeVisibleAnnotations")) annoSeen = true;
    }
    EXPECT_TRUE(annoSeen) << "annotated component lost its nested annotation slot";
    // 组件 1：嵌套 Signature 槽读出改名后的泛型串。
    bool sigRenamed = false;
    for (const jbc::core::AttrSlot& nested : rec.components[1].attrs) {
        if (cf.cp.utf8Equals(nested.nameCp, "Signature")) {
            auto sd = cf.decodeAttr(const_cast<jbc::core::AttrSlot&>(nested));
            ASSERT_TRUE(sd.is_ok());
            ASSERT_EQ(std::move(sd).unwrap()->kind(), jbc::core::AttrKind::Signature);
            const auto& sig = static_cast<const jbc::core::SignatureAttr&>(*nested.body);
            EXPECT_EQ(cf.cp.utf8Of(sig.signatureCp), "Lhb/jvm/recordremap/Bearer<Ljava/lang/String;>;")
                << "component signature not renamed";
            sigRenamed = true;
        }
    }
    EXPECT_TRUE(sigRenamed) << "generic component lost its nested signature slot";
}

TEST(MemberRemapJvm, RecordFaceRenamedRunsOnRealJvm)
{
    auto tc = test::jdk::probeJdk();
    if (!tc.available) {
        GTEST_SKIP() << "JDK 不可用: " << tc.reason;
    }

    const fs::path gate   = test::getTopProjectPath() / "test/jvm_gate_record_remap";
    const fs::path srcDir = gate / "src";
    const fs::path binDir = gate / "bin";
    const fs::path remapDir = gate / "remapped";

    // ── 1. 源码 + javac 编译（record 形态 JDK 16+，JDK 本体 17 起）──
    writeSource(srcDir / "hb/jvm/recordremap/RecSample.java", kRecordSampleSrc);
    std::string compileErr;
    if (!test::jdk::compileJava(tc, srcDir / "hb/jvm/recordremap/RecSample.java", binDir,
                                &compileErr)) {
        GTEST_SKIP() << "样本编译失败（record 需要 JDK 16+）： " << compileErr;
    }

    // ── 2. 原样本基线运行（钉死样本形态：record 反射读出 + 行为和）──
    const auto orig = test::jdk::runJavaMain(tc, binDir, "hb.jvm.recordremap.RecSample");
    ASSERT_EQ(orig.exitCode, 0) << orig.out;
    EXPECT_EQ(test::jdk::normalizeOutput(orig.out),
              test::jdk::normalizeOutput(recordSampleOutput("value",
                                                            "hb.jvm.recordremap.Holder")));

    // ── 3. applyMemberRemapPlan：类映射（RecordSample/Holder）+ 组件 value 的
    //       字段/访问器双规则改名 + 全开策略（组件签名/注解门全过）。逐类改写
    //       并按新二进制名落盘。
    mjbase::asm_remapper::MemberRemapPlan plan;
    plan.mapClass("hb/jvm/recordremap/RecordSample", "hb/jvm/recordremap/RecX");
    plan.mapClass("hb/jvm/recordremap/Holder", "hb/jvm/recordremap/Bearer");
    plan.mapField("hb/jvm/recordremap/RecordSample", "value", "I",
                  "hb/jvm/recordremap/RecX", "val", "I");
    plan.mapMethod("hb/jvm/recordremap/RecordSample", "value", "()I",
                   "hb/jvm/recordremap/RecX", "val", "()I");

    bool anyChanged = false;
    ca::u32 recordSlotsSeen = 0;
    std::error_code ec;
    fs::remove_all(remapDir, ec);
    for (const char* simple : {"RecSample", "RecordSample", "Holder", "Mark"}) {
        const Bytes corpus =
            readClassBytes(binDir / "hb/jvm/recordremap" / (std::string(simple) + ".class"));
        ASSERT_FALSE(corpus.empty()) << simple;
        auto sr = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(corpus));
        ASSERT_TRUE(sr.is_ok()) << simple;
        auto session = std::move(sr).unwrap();
        auto rr = mjbase::asm_remapper::applyMemberRemapPlan(
            *session, plan, mjbase::asm_remapper::RemapPolicy::all());
        ASSERT_TRUE(rr.is_ok()) << simple;
        anyChanged |= rr.unwrap();

        jbc::core::ClassFile& cf = session->backing();
        const std::string newName(cf.className(cf.thisClass));
        // typed 面断言在新名落盘前钉住（函数内按名寻 Record 槽）。
        if (std::string(simple) == std::string("RecordSample")) {
            verifyRecordFaceTyped(cf);
        }
        for (const jbc::core::AttrSlot& slot : cf.attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, "Record")) {
                ++recordSlotsSeen;
            }
        }

        auto w = jbc::core::writeClass(cf);
        ASSERT_TRUE(w.is_ok()) << simple;
        const fs::path dst = remapDir / (newName + ".class");
        fs::create_directories(dst.parent_path());
        std::ofstream fout(dst, std::ios::binary);
        ASSERT_TRUE(fout.good()) << dst.string();
        fout.write(reinterpret_cast<const char*>(w.unwrap().data()),
                   static_cast<std::streamsize>(w.unwrap().size()));
    }
    EXPECT_TRUE(anyChanged) << "改名未发生";
    // 触点护栏：四类里仅 RecordSample 带 Record 槽，typed 手术必须有真触面。
    EXPECT_EQ(recordSlotsSeen, 1u) << "样本 Record 槽数目漂移，护栏失真";

    // ── 4. 硬验收：新名类真 java 执行——isRecord/getRecordComponents 读新
    //       组件名与新泛型类名、组件注解反射原样、行为输出与原样本一致。
    const auto renamed =
        test::jdk::runJavaMain(tc, remapDir, "hb.jvm.recordremap.RecSample");
    EXPECT_EQ(renamed.exitCode, 0) << "renamed record rejected/failed:\n"
                                   << renamed.out;
    EXPECT_EQ(test::jdk::normalizeOutput(renamed.out),
              test::jdk::normalizeOutput(recordSampleOutput("val",
                                                            "hb.jvm.recordremap.Bearer")));
}
