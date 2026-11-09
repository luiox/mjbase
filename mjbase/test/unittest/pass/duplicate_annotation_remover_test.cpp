/// @file        duplicate_annotation_remover_test.cpp
/// @brief       重复注解去重 pass 单测：同目标同描述符去重（首个保留）、
///              visible/invisible 共享一份集、成员级分集、type annotation
///              目标位全集键、参数注解行保持、record 组件面、Code 嵌套面、
///              fail-closed（无重复字节不动）、幂等二跑、与
///              SpecificAnnotationRemover 共存、注册面与 opt-in 接线钉形。
///
/// 载体口径：基类一律 jasm 文本装配；注解属性挂载走 CF 级 typed 属性写面
/// （fixture 纪律第二类例外——jasm 方言未覆盖注解属性构造，模拟上游字节码
/// 手术的注解挂载工序）。断言走「重读 backing 注解槽事实」口径。
#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/duplicate_annotation_remover.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_registry.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/pipeline_config.hpp"

using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::pass::DuplicateAnnotationRemover;

namespace {

using Bytes = std::vector<ca::u8>;

using jbc::core::AnnotationsAttr;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::RecordAttr;
using jbc::core::TypeAnnotationsAttr;

// ── jasm 文本装配 ──

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 文本汇编最小类（字段声明列表 + 方法 decl/body 对；汇编失败即测试失败）。
Bytes assembleClass(const std::string&                                     className,
                    const std::vector<std::string>&                        fieldDecls,
                    const std::vector<std::pair<std::string, std::string>>& methods)
{
    mjbase::jasm::JvmJasmService service;
    std::string                  source =
        ".super java/lang/Object\n.class public super " + className + " {\n";
    for (const std::string& f : fieldDecls) {
        source += "  .field " + f + "\n";
    }
    for (const auto& [decl, body] : methods) {
        source += "  .method " + decl + " {\n    code: {\n" + body + "    }\n  }\n";
    }
    source += "}\n";
    const auto sourceName = utf16(className + ".jasm");
    const auto sourceText = utf16(source);
    const auto result     = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败";
        return {};
    }
    return result.value->bytes;
}

/// 单方法基类便捷形。
Bytes assembleSingle(const std::string& className, const std::string& decl,
                     const std::string& body = "      return\n")
{
    return assembleClass(className, {}, {{decl, body}});
}

Bytes writeBytes(jbc::ir::ClassSession& session)
{
    auto written = jbc::core::writeClass(session.backing());
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

// ── 注解属性挂载（CF 级 typed 属性写面）──

Bytes withClassAnnotations(const Bytes& bytes, const std::string& attrName,
                           const std::vector<std::string>& descs)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto session = std::move(created).unwrap();
    auto attr    = std::make_unique<AnnotationsAttr>();
    for (const std::string& d : descs) {
        AnnotationsAttr::Annotation a;
        a.typeCp = session->backing().cp.addUtf8(d);
        attr->annos.push_back(std::move(a));
    }
    session->backing().attrs.push_back(AttrSlot::makeNew(
        session->backing().cp.addUtf8(attrName), std::move(attr)));
    return writeBytes(*session);
}

/// 成员级注解槽（fieldIdx/methodIdx 二选一，另一侧 nullopt）。
Bytes withMemberAnnotations(const Bytes&                    bytes,
                            const std::optional<ca::usize>& fieldIdx,
                            const std::optional<ca::usize>& methodIdx,
                            const std::string&              attrName,
                            const std::vector<std::string>& descs)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto                            session = std::move(created).unwrap();
    ClassFile&                      cf      = session->backing();
    std::vector<AttrSlot>* memberAttrs = nullptr;
    if (fieldIdx.has_value()) memberAttrs = &cf.fields[*fieldIdx].attrs;
    if (methodIdx.has_value()) memberAttrs = &cf.methods[*methodIdx].attrs;
    EXPECT_NE(memberAttrs, nullptr);
    if (memberAttrs == nullptr) return bytes;
    auto attr = std::make_unique<AnnotationsAttr>();
    for (const std::string& d : descs) {
        AnnotationsAttr::Annotation a;
        a.typeCp = cf.cp.addUtf8(d);
        attr->annos.push_back(std::move(a));
    }
    memberAttrs->push_back(AttrSlot::makeNew(cf.cp.addUtf8(attrName), std::move(attr)));
    return writeBytes(*session);
}

/// type annotation 槽条目规格（空 target_info 形态——0x13/0x14/0x15）。
struct TypeAnnoSpec
{
    ca::u8      targetType = 0x13;
    std::string desc;
};

/// type annotation 槽挂载（slotLevel: 2=方法级 3=方法 Code 嵌套——嵌套用
/// 合成形态（空形条目挂嵌套槽，libjbc 按条目自身 target_type 解码不验挂载
/// 点，meta 家族单测同款），父 Code 槽连带置脏）。
Bytes withTypeAnnotations(const Bytes&                     bytes,
                          const ca::u8                     slotLevel,
                          const std::string&               attrName,
                          const std::vector<TypeAnnoSpec>& entries)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto         session = std::move(created).unwrap();
    ClassFile&   cf      = session->backing();

    auto attr = std::make_unique<TypeAnnotationsAttr>();
    for (const TypeAnnoSpec& e : entries) {
        TypeAnnotationsAttr::Annotation a;
        a.targetType  = e.targetType;
        a.typeIndexCp = cf.cp.addUtf8(e.desc);
        attr->annotations.push_back(std::move(a));
    }
    const ca::u16 slotName = cf.cp.addUtf8(attrName);
    if (slotLevel == 3) {
        AttrSlot* codeSlot = nullptr;
        for (AttrSlot& slot : cf.methods[0].attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, "Code")) codeSlot = &slot;
        }
        EXPECT_NE(codeSlot, nullptr) << "Code 槽缺失";
        if (codeSlot == nullptr) return bytes;
        auto decoded = cf.decodeAttr(*codeSlot);
        EXPECT_TRUE(decoded.is_ok());
        if (decoded.is_err()) return bytes;
        auto* codeAttr = ca::core::tag_cast::cast<jbc::core::CodeAttr>(decoded.unwrap());
        EXPECT_NE(codeAttr, nullptr);
        if (codeAttr == nullptr) return bytes;
        codeAttr->attrs.push_back(AttrSlot::makeNew(slotName, std::move(attr)));
        (void)codeSlot->mutate();
    }
    else {
        cf.methods[0].attrs.push_back(AttrSlot::makeNew(slotName, std::move(attr)));
    }
    return writeBytes(*session);
}

/// 参数注解槽挂载（行数 = rows.size()）。
Bytes withParamAnnotations(const Bytes&                                 bytes,
                           const std::string&                           attrName,
                           const std::vector<std::vector<std::string>>& rows)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto         session = std::move(created).unwrap();
    ClassFile&   cf      = session->backing();
    auto         attr    = std::make_unique<jbc::core::ParameterAnnotationsAttr>();
    for (const std::vector<std::string>& row : rows) {
        std::vector<AnnotationsAttr::Annotation> rowEntries;
        for (const std::string& d : row) {
            AnnotationsAttr::Annotation a;
            a.typeCp = cf.cp.addUtf8(d);
            rowEntries.push_back(std::move(a));
        }
        attr->parameters.push_back(std::move(rowEntries));
    }
    cf.methods[0].attrs.push_back(
        AttrSlot::makeNew(cf.cp.addUtf8(attrName), std::move(attr)));
    return writeBytes(*session);
}

/// record 组件注解挂载：单组件（x:I）携带 RVA + RIA 两槽（typed RecordAttr，
/// 父 Record 槽 New 态）。
Bytes withRecordComponentAnnotations(const Bytes&                    bytes,
                                     const std::vector<std::string>& visibleDescs,
                                     const std::vector<std::string>& invisibleDescs)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto       session = std::move(created).unwrap();
    ClassFile& cf      = session->backing();
    auto       rec     = std::make_unique<RecordAttr>();
    RecordAttr::Component component;
    component.nameCp       = cf.cp.addUtf8("x");
    component.descriptorCp = cf.cp.addUtf8("I");
    const auto mount = [&](const std::string& attrName,
                           const std::vector<std::string>& descs) {
        auto attr = std::make_unique<AnnotationsAttr>();
        for (const std::string& d : descs) {
            AnnotationsAttr::Annotation a;
            a.typeCp = cf.cp.addUtf8(d);
            attr->annos.push_back(std::move(a));
        }
        component.attrs.push_back(AttrSlot::makeNew(cf.cp.addUtf8(attrName), std::move(attr)));
    };
    mount("RuntimeVisibleAnnotations", visibleDescs);
    mount("RuntimeInvisibleAnnotations", invisibleDescs);
    rec->components.push_back(std::move(component));
    cf.attrs.push_back(AttrSlot::makeNew(cf.cp.addUtf8("Record"), std::move(rec)));
    return writeBytes(*session);
}

// ── 断言面 ──

std::optional<std::vector<std::string>> readClassAnnoSlot(jbc::ir::ClassSession& session,
                                                          const std::string&     attrName)
{
    ClassFile& cf = session.backing();
    for (AttrSlot& slot : cf.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, attrName)) continue;
        if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return std::nullopt;
        const auto* body = ca::core::tag_cast::cast<const AnnotationsAttr>(slot.peek());
        if (body == nullptr) return std::nullopt;
        std::vector<std::string> out;
        for (const AnnotationsAttr::Annotation& a : body->annos) {
            out.emplace_back(cf.cp.utf8Of(a.typeCp));
        }
        return out;
    }
    return std::nullopt;
}

/// 方法级 type annotation 槽条目 (targetType, desc) 序列。
std::optional<std::vector<std::pair<ca::u8, std::string>>> readMethodTypeAnnoEntries(
    jbc::ir::ClassSession& session, const std::string& attrName)
{
    ClassFile& cf = session.backing();
    for (AttrSlot& slot : cf.methods[0].attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, attrName)) continue;
        if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return std::nullopt;
        const auto* body = ca::core::tag_cast::cast<const TypeAnnotationsAttr>(slot.peek());
        if (body == nullptr) return std::nullopt;
        std::vector<std::pair<ca::u8, std::string>> out;
        for (const TypeAnnotationsAttr::Annotation& a : body->annotations) {
            out.emplace_back(a.targetType, std::string(cf.cp.utf8Of(a.typeIndexCp)));
        }
        return out;
    }
    return std::nullopt;
}

/// 方法 Code 嵌套 type annotation 槽条目数（槽缺失 nullopt）。
std::optional<ca::usize> readCodeTypeAnnoCount(jbc::ir::ClassSession& session,
                                               const std::string&     attrName)
{
    ClassFile& cf          = session.backing();
    auto       decoded     = cf.decodeCode(cf.methods[0]);
    if (decoded.is_err()) return std::nullopt;
    jbc::core::CodeAttr* code = decoded.unwrap();
    if (code == nullptr) return std::nullopt;
    for (AttrSlot& slot : code->attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, attrName)) continue;
        if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return std::nullopt;
        const auto* body = ca::core::tag_cast::cast<const TypeAnnotationsAttr>(slot.peek());
        if (body == nullptr) return std::nullopt;
        return body->annotations.size();
    }
    return std::nullopt;
}

/// 参数注解槽行视图（槽缺失 nullopt）。
std::optional<std::vector<std::vector<std::string>>> readParamAnnoRows(
    jbc::ir::ClassSession& session, const std::string& attrName)
{
    ClassFile& cf = session.backing();
    for (AttrSlot& slot : cf.methods[0].attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, attrName)) continue;
        if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return std::nullopt;
        const auto* body =
            ca::core::tag_cast::cast<const jbc::core::ParameterAnnotationsAttr>(slot.peek());
        if (body == nullptr) return std::nullopt;
        std::vector<std::vector<std::string>> out;
        for (const std::vector<AnnotationsAttr::Annotation>& row : body->parameters) {
            std::vector<std::string> rowOut;
            for (const AnnotationsAttr::Annotation& a : row) {
                rowOut.emplace_back(cf.cp.utf8Of(a.typeCp));
            }
            out.push_back(std::move(rowOut));
        }
        return out;
    }
    return std::nullopt;
}

/// record 组件注解槽条目描述符（经运行会话内 RecordAttr 体直读；组件缺失
/// 返回 nullopt）。
std::optional<std::vector<std::string>> readRecordComponentAnnoDescs(
    jbc::ir::ClassSession& session, const std::string& attrName)
{
    ClassFile& cf = session.backing();
    for (AttrSlot& slot : cf.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "Record")) continue;
        if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return std::nullopt;
        const auto* rec = ca::core::tag_cast::cast<const RecordAttr>(slot.peek());
        if (rec == nullptr || rec->components.empty()) return std::nullopt;
        std::vector<std::string> out;
        for (const AttrSlot& memberSlot : rec->components[0].attrs) {
            if (!cf.cp.utf8Equals(memberSlot.nameCp, attrName)) continue;
            const auto* body = ca::core::tag_cast::cast<const AnnotationsAttr>(
                memberSlot.peek());
            if (body == nullptr) return std::nullopt;
            for (const AnnotationsAttr::Annotation& a : body->annos) {
                out.emplace_back(cf.cp.utf8Of(a.typeCp));
            }
            return out;
        }
        return std::vector<std::string>{};
    }
    return std::nullopt;
}

// ── pass 驱动 ──

struct RunResult
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    mjbase::spi::PreservedAnalyses         pa;
};

RunResult runDedup(const Bytes& bytes)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    auto                              session = std::move(created).unwrap();
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    DuplicateAnnotationRemover        pass;
    RunResult                         run;
    run.session = session;
    run.pa      = pass.run(*session, cam);
    return run;
}

void addClassResource(ResourceContainer& container, const std::string& uri, const Bytes& bytes)
{
    container.addResource(uri, std::make_unique<ClassResource>(uri, bytes));
}

Bytes containerBytes(ResourceContainer& container, const std::string& uri)
{
    const auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr) << uri;
    const auto* cls = ca::core::tag_cast::cast<const ClassResource*>(resource);
    EXPECT_NE(cls, nullptr) << uri;
    return cls != nullptr ? cls->get() : Bytes{};
}

constexpr std::string_view kFoo = "Lfoo/Ann;";
constexpr std::string_view kBar = "Lfoo/Other;";

}   // namespace

// ── a) 正例：去重面 ──

TEST(MjPassDuplicateAnnotationRemoverTest, DedupesClassAnnotationsKeepingFirst)
{
    const Bytes bytes = withClassAnnotations(
        assembleSingle("R", "public static f ()V"), "RuntimeVisibleAnnotations",
        {std::string(kFoo), std::string(kFoo), std::string(kBar), std::string(kFoo)});
    const auto run = runDedup(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto descs = readClassAnnoSlot(*run.session, "RuntimeVisibleAnnotations");
    ASSERT_TRUE(descs.has_value());
    ASSERT_EQ(descs->size(), static_cast<ca::usize>(2));
    EXPECT_EQ((*descs)[0], kFoo);
    EXPECT_EQ((*descs)[1], kBar);
}

TEST(MjPassDuplicateAnnotationRemoverTest, DedupesAcrossVisibleInvisible)
{
    // visible 与 invisible 共享一份集（Recaf visitor 同口径）：同描述符跨
    // 两槽只留先到者，后槽条目摘除、槽保留（Bar 幸存）。
    const Bytes bytes = withClassAnnotations(
        withClassAnnotations(assembleSingle("R", "public static f ()V"),
                             "RuntimeVisibleAnnotations", {std::string(kFoo)}),
        "RuntimeInvisibleAnnotations", {std::string(kFoo), std::string(kBar)});
    const auto run = runDedup(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto visible = readClassAnnoSlot(*run.session, "RuntimeVisibleAnnotations");
    ASSERT_TRUE(visible.has_value());
    ASSERT_EQ(visible->size(), static_cast<ca::usize>(1));
    const auto invisible = readClassAnnoSlot(*run.session, "RuntimeInvisibleAnnotations");
    ASSERT_TRUE(invisible.has_value());
    ASSERT_EQ(invisible->size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*invisible)[0], kBar);
}

TEST(MjPassDuplicateAnnotationRemoverTest, DedupesPerMemberNotAcrossMembers)
{
    // 分集粒度 = 成员：同描述符在两个不同字段上各保留首个实例。
    const Bytes base = assembleClass("RF", {"public static a I", "public static b I"},
                                     {{"public static f ()V", "      return\n"}});
    const Bytes bytes = withMemberAnnotations(
        withMemberAnnotations(
            withMemberAnnotations(base, std::optional<ca::usize>(0), std::nullopt,
                                  "RuntimeVisibleAnnotations", {std::string(kFoo)}),
            std::optional<ca::usize>(1), std::nullopt, "RuntimeVisibleAnnotations",
            {std::string(kFoo)}),
        std::nullopt, std::optional<ca::usize>(0), "RuntimeVisibleAnnotations",
        {std::string(kFoo)});
    const auto run = runDedup(bytes);

    EXPECT_FALSE(run.pa.changed()) << "跨成员不共享集：零改动";
}

TEST(MjPassDuplicateAnnotationRemoverTest, DedupesTypeAnnotationsByFullTargetKey)
{
    // 键 = 目标位全集：同 target_type 同描述符去重；target_type 不同（0x13
    // 字段 vs 0x14 返回）各保留。
    const Bytes bytes = withTypeAnnotations(
        assembleSingle("R", "public static f ()V"), 2, "RuntimeVisibleTypeAnnotations",
        {{0x13, std::string(kFoo)}, {0x13, std::string(kFoo)}, {0x14, std::string(kFoo)}});
    const auto run = runDedup(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto entries = readMethodTypeAnnoEntries(*run.session,
                                                   "RuntimeVisibleTypeAnnotations");
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), static_cast<ca::usize>(2));
    EXPECT_EQ((*entries)[0].first, 0x13);
    EXPECT_EQ((*entries)[1].first, 0x14);
}

TEST(MjPassDuplicateAnnotationRemoverTest, DedupesParameterAnnotationsPerRow)
{
    // 键 = (参数行, 描述符)：行内重复去重、跨行同描述符各保留；visible 与
    // invisible 共享一份集 → RIPA 的 Foo 随 RVA 先到被摘，槽删空摘除。
    const Bytes bytes = withParamAnnotations(
        withParamAnnotations(assembleSingle("R", "public static f (II)V"),
                             "RuntimeVisibleParameterAnnotations",
                             {{std::string(kFoo), std::string(kFoo)},
                              {std::string(kFoo)}}),
        "RuntimeInvisibleParameterAnnotations", {{std::string(kFoo)}});
    const auto run = runDedup(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto rows = readParamAnnoRows(*run.session, "RuntimeVisibleParameterAnnotations");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), static_cast<ca::usize>(2)) << "行数与行序保持";
    ASSERT_EQ((*rows)[0].size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*rows)[0][0], kFoo);
    ASSERT_EQ((*rows)[1].size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*rows)[1][0], kFoo);
    const auto invisibleRows =
        readParamAnnoRows(*run.session, "RuntimeInvisibleParameterAnnotations");
    EXPECT_FALSE(invisibleRows.has_value()) << "全空摘槽";
}

TEST(MjPassDuplicateAnnotationRemoverTest, DedupesRecordComponentAnnotations)
{
    const Bytes bytes = withRecordComponentAnnotations(
        assembleSingle("R", "public static f ()V"),
        {std::string(kFoo), std::string(kFoo)}, {std::string(kFoo)});
    const auto run = runDedup(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto visibleDescs =
        readRecordComponentAnnoDescs(*run.session, "RuntimeVisibleAnnotations");
    ASSERT_TRUE(visibleDescs.has_value());
    ASSERT_EQ(visibleDescs->size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*visibleDescs)[0], kFoo);
    const auto invisibleDescs =
        readRecordComponentAnnoDescs(*run.session, "RuntimeInvisibleAnnotations");
    ASSERT_TRUE(invisibleDescs.has_value());
    EXPECT_TRUE(invisibleDescs->empty()) << "组件面跨槽共享集：RIA 条目摘空摘槽";
}

TEST(MjPassDuplicateAnnotationRemoverTest, DedupesCodeNestedTypeAnnotations)
{
    const Bytes bytes = withTypeAnnotations(
        assembleSingle("R", "public static f ()V"), 3, "RuntimeVisibleTypeAnnotations",
        {{0x13, std::string(kFoo)}, {0x13, std::string(kFoo)}});
    const auto run = runDedup(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto count = readCodeTypeAnnoCount(*run.session, "RuntimeVisibleTypeAnnotations");
    ASSERT_TRUE(count.has_value());
    EXPECT_EQ(*count, static_cast<ca::usize>(1));
}

// ── b) 反例：无重复字节不动 ──

TEST(MjPassDuplicateAnnotationRemoverTest, KeepsDistinctAnnotationsUntouched)
{
    const Bytes original = withClassAnnotations(
        withTypeAnnotations(assembleSingle("R", "public static f (I)V"), 2,
                            "RuntimeVisibleTypeAnnotations",
                            {{0x13, std::string(kFoo)}, {0x14, std::string(kBar)}}),
        "RuntimeVisibleAnnotations", {std::string(kFoo), std::string(kBar)});
    const auto run = runDedup(original);

    EXPECT_FALSE(run.pa.changed()) << "判据不满足：零改动";
    auto written = jbc::core::writeClass(run.session->backing());
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(std::move(written).unwrap(), original) << "字节不动";
}

// ── c) 幂等二跑 ──

TEST(MjPassDuplicateAnnotationRemoverTest, IdempotentSecondRun)
{
    const Bytes original = withClassAnnotations(
        assembleSingle("R", "public static f ()V"), "RuntimeVisibleAnnotations",
        {std::string(kFoo), std::string(kFoo)});
    const auto first = runDedup(original);
    EXPECT_TRUE(first.pa.changed());

    {
        ca::str::Utf8StringArena          arena;
        mjbase::spi::ClassAnalysisManager cam(arena);
        DuplicateAnnotationRemover        pass;
        EXPECT_FALSE(pass.run(*first.session, cam).changed());
    }
    {
        const Bytes settled = writeBytes(*first.session);
        const auto  second  = runDedup(settled);
        EXPECT_FALSE(second.pa.changed());
        const auto descs = readClassAnnoSlot(*second.session, "RuntimeVisibleAnnotations");
        ASSERT_TRUE(descs.has_value());
        EXPECT_EQ(descs->size(), static_cast<ca::usize>(1));
    }
}

// ── d) 与其他删除件共存 ──

TEST(MjPassDuplicateAnnotationRemoverTest, CoexistsWithSpecificAnnotationRemover)
{
    // 黑名单件吃 jetbrains 描述符、本件吃其重复实例——两判据正交同链。
    const Bytes bytes = withClassAnnotations(
        assembleSingle("R", "public static f ()V"), "RuntimeVisibleAnnotations",
        {"Lorg/jetbrains/annotations/NotNull;", "Lorg/jetbrains/annotations/NotNull;",
         std::string(kFoo), std::string(kFoo)});
    ResourceContainer container;
    addClassResource(container, "R.class", bytes);

    const auto loaded = mjbase::spi::loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"SpecificAnnotationRemover"},)json"
        R"json({"pass":"DuplicateAnnotationRemover"}]})json",
        mjbase::spi::defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    mjbase::spi::PipelineBuilder builder;
    ASSERT_TRUE(mjbase::spi::applyConfigTo(mjbase::spi::defaultPassRegistry(),
                                           loaded.unwrap(), builder)
                    .is_ok());
    mjbase::spi::PassJobContext job;
    auto                        mgr = builder.buildNestedPipeline(job);
    const auto                  ran = mjbase::spi::runContainerPipeline(container, mgr, job);
    ASSERT_TRUE(ran.is_ok()) << ran.unwrap_err().detail;

    const Bytes after = containerBytes(container, "R.class");
    const auto  read  = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(after));
    ASSERT_TRUE(read.is_ok());
    const auto descs = readClassAnnoSlot(*std::move(read).unwrap(),
                                         "RuntimeVisibleAnnotations");
    ASSERT_TRUE(descs.has_value());
    ASSERT_EQ(descs->size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*descs)[0], kFoo);
}

// ── e) 注册面与 opt-in 接线 ──

TEST(MjPassDuplicateAnnotationRemoverTest, RegisteredAsDuplicateAnnotationRemover)
{
    using mjbase::spi::PassScope;
    mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    EXPECT_TRUE(reg.has("DuplicateAnnotationRemover"));
    const auto found = reg.find("DuplicateAnnotationRemover");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, PassScope::Class);
    EXPECT_FALSE(found.unwrap().description.empty());

    auto created = reg.createClassPass("DuplicateAnnotationRemover");
    ASSERT_TRUE(created.is_ok());
    const Bytes bytes = withClassAnnotations(
        assembleSingle("RG", "public static f ()V"), "RuntimeVisibleAnnotations",
        {std::string(kFoo), std::string(kFoo)});
    auto createdSession = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(createdSession.is_ok());
    auto                              session = std::move(createdSession).unwrap();
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    const auto pa = std::move(created).unwrap()->run(*session, cam);
    EXPECT_TRUE(pa.changed()) << "注册表工厂默认口径开箱可用";
    const auto descs = readClassAnnoSlot(*session, "RuntimeVisibleAnnotations");
    ASSERT_TRUE(descs.has_value());
    EXPECT_EQ(descs->size(), static_cast<ca::usize>(1));
}

TEST(MjPassDuplicateAnnotationRemoverTest, PipelineConfigOptInEnablesDedup)
{
    ResourceContainer container;
    const Bytes bytes = withClassAnnotations(
        assembleSingle("PI", "public static f ()V"), "RuntimeVisibleAnnotations",
        {std::string(kFoo), std::string(kFoo), std::string(kFoo)});
    addClassResource(container, "PI.class", bytes);

    const auto loaded = mjbase::spi::loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"DuplicateAnnotationRemover"}]})json",
        mjbase::spi::defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    EXPECT_EQ(loaded.unwrap().sequence.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(loaded.unwrap().sequence[0].scope, mjbase::spi::PassScope::Class);

    mjbase::spi::PipelineBuilder builder;
    ASSERT_TRUE(mjbase::spi::applyConfigTo(mjbase::spi::defaultPassRegistry(),
                                           loaded.unwrap(), builder)
                    .is_ok());
    mjbase::spi::PassJobContext job;
    auto                        mgr = builder.buildNestedPipeline(job);
    const auto                  ran = mjbase::spi::runContainerPipeline(container, mgr, job);
    ASSERT_TRUE(ran.is_ok()) << ran.unwrap_err().detail;

    const Bytes after = containerBytes(container, "PI.class");
    const auto  read  = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(after));
    ASSERT_TRUE(read.is_ok());
    const auto descs = readClassAnnoSlot(*std::move(read).unwrap(),
                                         "RuntimeVisibleAnnotations");
    ASSERT_TRUE(descs.has_value());
    ASSERT_EQ(descs->size(), static_cast<ca::usize>(1));
}

TEST(MjPassDuplicateAnnotationRemoverTest, DefaultChainLeavesDuplicatesUntouched)
{
    (void)mjbase::spi::registerBuiltinPipelines();
    const auto& pipelines = mjbase::spi::PassRegistry::instance();
    ASSERT_TRUE(pipelines.hasPipeline("default-optimize"));
    const auto pipeline = pipelines.getPipeline("default-optimize");
    for (const auto& name : pipeline.classPassNames()) {
        EXPECT_NE(name, "DuplicateAnnotationRemover") << "opt-in 不入默认链";
    }
    for (const auto& name : pipeline.methodPassNames()) {
        EXPECT_NE(name, "DuplicateAnnotationRemover") << "opt-in 不入默认链";
    }

    ResourceContainer container;
    const Bytes bytes = withClassAnnotations(
        assembleSingle("PD", "public static f ()V"), "RuntimeVisibleAnnotations",
        {std::string(kFoo), std::string(kFoo)});
    addClassResource(container, "PD.class", bytes);

    mjbase::spi::PassJobContext job;
    auto                        mgr = pipeline.buildNestedPipeline(job);
    const auto                  ran = mjbase::spi::runContainerPipeline(container, mgr, job);
    ASSERT_TRUE(ran.is_ok()) << ran.unwrap_err().detail;

    const Bytes after = containerBytes(container, "PD.class");
    const auto  read  = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(after));
    ASSERT_TRUE(read.is_ok());
    const auto descs = readClassAnnoSlot(*std::move(read).unwrap(),
                                         "RuntimeVisibleAnnotations");
    ASSERT_TRUE(descs.has_value());
    ASSERT_EQ(descs->size(), static_cast<ca::usize>(2)) << "默认链不收：重复原样";
}
