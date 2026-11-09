/// @file        long_annotation_remover_test.cpp
/// @brief       巨型注解删除 pass 单测：三层注解/type annotation/参数注解
///              与 Code 嵌套面的阈值命中与幸存、嵌套注解值面递归修剪、阈值
///              两侧边界、fail-closed（判据不满足字节不动/解码失败槽保持）、
///              幂等二跑、与 SpecificAnnotationRemover 共存、注册面与
///              opt-in 接线钉形（默认注册表种子 + 管线配置启用含 maxLength
///              参数 + default-optimize 零变化）。
///
/// 载体口径：基类一律 jasm 文本装配（JvmJasmService）；注解属性挂载走 CF
/// 级 typed 属性写面（AnnotationsAttr/TypeAnnotationsAttr/
/// ParameterAnnotationsAttr + AttrSlot::makeNew + writeClass）——jasm 方言
/// 未覆盖注解属性构造（fixture 纪律第二类例外，模拟上游字节码手术的注解
/// 挂载工序）。断言走「重读 backing 注解槽事实」口径。
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
#include "mjbase/pass/long_annotation_remover.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_registry.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/pipeline_config.hpp"

using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::pass::LongAnnotationRemover;
using mjbase::pass::LongAnnotationRemoverOptions;

namespace {

using Bytes = std::vector<ca::u8>;

// ── jasm 文本装配 ──

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// jasm 文本汇编最小类（多方法；汇编失败即测试失败，诊断逐条落地）。
Bytes assembleClass(const std::string&                                     className,
                    const std::vector<std::pair<std::string, std::string>>& methods)
{
    mjbase::jasm::JvmJasmService service;
    std::string                  source =
        ".super java/lang/Object\n.class public super " + className + " {\n";
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

Bytes writeBytes(jbc::ir::ClassSession& session)
{
    auto written = jbc::core::writeClass(session.backing());
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

// ── 注解属性挂载（CF 级 typed 属性写面，模拟上游注解挂载工序）──

/// 类级注解槽（RuntimeVisibleAnnotations / RuntimeInvisibleAnnotations）。
Bytes withClassAnnotations(const Bytes& bytes, const std::string& attrName,
                           const std::vector<std::string>& descs)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto session = std::move(created).unwrap();
    auto attr    = std::make_unique<jbc::core::AnnotationsAttr>();
    for (const std::string& d : descs) {
        jbc::core::AnnotationsAttr::Annotation a;
        a.typeCp = session->backing().cp.addUtf8(d);
        attr->annos.push_back(std::move(a));
    }
    session->backing().attrs.push_back(jbc::core::AttrSlot::makeNew(
        session->backing().cp.addUtf8(attrName), std::move(attr)));
    return writeBytes(*session);
}

/// 成员级注解槽（fieldIdx/methodIdx 二选一，另一侧传 nullopt）。
Bytes withMemberAnnotations(const Bytes&                    bytes,
                            const std::optional<ca::usize>& fieldIdx,
                            const std::optional<ca::usize>& methodIdx,
                            const std::string&              attrName,
                            const std::vector<std::string>& descs)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto                     session = std::move(created).unwrap();
    jbc::core::ClassFile&    cf      = session->backing();
    std::vector<jbc::core::AttrSlot>* memberAttrs = nullptr;
    if (fieldIdx.has_value()) memberAttrs = &cf.fields[*fieldIdx].attrs;
    if (methodIdx.has_value()) memberAttrs = &cf.methods[*methodIdx].attrs;
    EXPECT_NE(memberAttrs, nullptr);
    if (memberAttrs == nullptr) return bytes;
    auto attr = std::make_unique<jbc::core::AnnotationsAttr>();
    for (const std::string& d : descs) {
        jbc::core::AnnotationsAttr::Annotation a;
        a.typeCp = cf.cp.addUtf8(d);
        attr->annos.push_back(std::move(a));
    }
    memberAttrs->push_back(
        jbc::core::AttrSlot::makeNew(cf.cp.addUtf8(attrName), std::move(attr)));
    return writeBytes(*session);
}

/// type annotation 槽条目规格（空 target_info 形态——0x13/0x14/0x15）。
struct TypeAnnoSpec
{
    ca::u8       targetType = 0x13;   ///< kField（libjbc 默认；0x14 return 同空形）
    std::string  desc;
};

/// type annotation 槽挂载（slotLevel: 0=类级 1=字段 2=方法 3=方法 Code 嵌套
/// ——Code 嵌套用合成形态（0x13 空形条目挂嵌套槽，libjbc 按条目自身
/// target_type 解码不验挂载点，meta 家族单测同款），父 Code 槽连带置脏）。
Bytes withTypeAnnotations(const Bytes&                     bytes,
                          const ca::u8                     slotLevel,
                          const std::string&               attrName,
                          const std::vector<TypeAnnoSpec>& entries)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto                  session = std::move(created).unwrap();
    jbc::core::ClassFile& cf      = session->backing();

    auto attr = std::make_unique<jbc::core::TypeAnnotationsAttr>();
    for (const TypeAnnoSpec& e : entries) {
        jbc::core::TypeAnnotationsAttr::Annotation a;
        a.targetType  = e.targetType;
        a.typeIndexCp = cf.cp.addUtf8(e.desc);
        attr->annotations.push_back(std::move(a));
    }
    const ca::u16 slotName = cf.cp.addUtf8(attrName);
    if (slotLevel == 3) {
        jbc::core::AttrSlot* codeSlot = nullptr;
        for (jbc::core::AttrSlot& slot : cf.methods[0].attrs) {
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
        codeAttr->attrs.push_back(jbc::core::AttrSlot::makeNew(slotName, std::move(attr)));
        (void)codeSlot->mutate();
    }
    else {
        std::vector<jbc::core::AttrSlot>* host = &cf.attrs;
        if (slotLevel == 1) host = &cf.fields[0].attrs;
        if (slotLevel == 2) host = &cf.methods[0].attrs;
        host->push_back(jbc::core::AttrSlot::makeNew(slotName, std::move(attr)));
    }
    return writeBytes(*session);
}

/// 参数注解槽挂载（行数 = rows.size()，行序保持）。
Bytes withParamAnnotations(const Bytes&                                       bytes,
                           const std::string&                                 attrName,
                           const std::vector<std::vector<std::string>>&       rows)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto                     session = std::move(created).unwrap();
    jbc::core::ClassFile&    cf      = session->backing();
    auto                     attr    = std::make_unique<jbc::core::ParameterAnnotationsAttr>();
    for (const std::vector<std::string>& row : rows) {
        std::vector<jbc::core::AnnotationsAttr::Annotation> rowEntries;
        for (const std::string& d : row) {
            jbc::core::AnnotationsAttr::Annotation a;
            a.typeCp = cf.cp.addUtf8(d);
            rowEntries.push_back(std::move(a));
        }
        attr->parameters.push_back(std::move(rowEntries));
    }
    cf.methods[0].attrs.push_back(
        jbc::core::AttrSlot::makeNew(cf.cp.addUtf8(attrName), std::move(attr)));
    return writeBytes(*session);
}

/// 类级注解槽挂载带 '@' 嵌套值面的条目：parentDesc + 键值对（数组包裹可选）
/// ——嵌套修剪面的载体（LongSubAnnoRemover 对位）。
Bytes withClassNestedAnnotation(const Bytes& bytes, const std::string& attrName,
                                const std::string& parentDesc,
                                const std::string& nestedDesc, const bool inArray)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto                     session = std::move(created).unwrap();
    jbc::core::ClassFile&    cf      = session->backing();

    auto nested = std::make_unique<jbc::core::AnnotationsAttr::Annotation>();
    nested->typeCp = cf.cp.addUtf8(nestedDesc);
    jbc::core::AnnotationsAttr::ElemValue ev;
    ev.tag    = '@';
    ev.nested = std::move(nested);
    if (inArray) {
        jbc::core::AnnotationsAttr::ElemValue arr;
        arr.tag = '[';
        arr.items.push_back(std::move(ev));
        ev = std::move(arr);
    }
    jbc::core::AnnotationsAttr::Annotation parent;
    parent.typeCp = cf.cp.addUtf8(parentDesc);
    parent.values.emplace_back(cf.cp.addUtf8("value"), std::move(ev));
    auto attr = std::make_unique<jbc::core::AnnotationsAttr>();
    attr->annos.push_back(std::move(parent));
    cf.attrs.push_back(
        jbc::core::AttrSlot::makeNew(cf.cp.addUtf8(attrName), std::move(attr)));
    return writeBytes(*session);
}

/// 类级槽挂载 typed 拒收的 Raw 体（解码面类型不符的 fail-closed 载体；
/// payload 为残截的注解表字节——typed 分派对该名槽建 RawAttr）。
Bytes withRawAnnotationSlot(const Bytes& bytes, const std::string& attrName)
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    if (!created.is_ok()) return bytes;
    auto                     session = std::move(created).unwrap();
    jbc::core::ClassFile&    cf      = session->backing();
    cf.attrs.push_back(jbc::core::AttrSlot::makeNew(
        cf.cp.addUtf8(attrName),
        std::make_unique<jbc::core::RawAttr>(Bytes{0x00, 0x01})));
    return writeBytes(*session);
}

// ── 断言面（重读 backing 注解槽事实）──

/// 指定类级注解槽的条目描述符序列（槽缺失返回 nullopt）。
std::optional<std::vector<std::string>> readClassAnnoSlot(jbc::ir::ClassSession& session,
                                                          const std::string&     attrName)
{
    jbc::core::ClassFile& cf = session.backing();
    for (jbc::core::AttrSlot& slot : cf.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, attrName)) continue;
        if (!slot.decoded() && cf.decodeAttr(slot).is_err()) return std::nullopt;
        const auto* body = ca::core::tag_cast::cast<const jbc::core::AnnotationsAttr>(
            slot.peek());
        if (body == nullptr) return std::nullopt;
        std::vector<std::string> out;
        for (const jbc::core::AnnotationsAttr::Annotation& a : body->annos) {
            out.emplace_back(cf.cp.utf8Of(a.typeCp));
        }
        return out;
    }
    return std::nullopt;
}

/// 成员级注解槽条目描述符序列（fieldIdx/methodIdx 二选一）。
std::optional<std::vector<std::string>> readMemberAnnoSlot(jbc::ir::ClassSession& session,
                                                           const std::optional<ca::usize>& fieldIdx,
                                                           const std::optional<ca::usize>& methodIdx,
                                                           const std::string&              attrName)
{
    jbc::core::ClassFile& cf = session.backing();
    const std::vector<jbc::core::AttrSlot>* memberAttrs = nullptr;
    if (fieldIdx.has_value()) memberAttrs = &cf.fields[*fieldIdx].attrs;
    if (methodIdx.has_value()) memberAttrs = &cf.methods[*methodIdx].attrs;
    if (memberAttrs == nullptr) return std::nullopt;
    for (const jbc::core::AttrSlot& slot : *memberAttrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, attrName)) continue;
        const auto* body = ca::core::tag_cast::cast<const jbc::core::AnnotationsAttr>(
            slot.peek());
        if (body == nullptr) return std::nullopt;
        std::vector<std::string> out;
        for (const jbc::core::AnnotationsAttr::Annotation& a : body->annos) {
            out.emplace_back(cf.cp.utf8Of(a.typeCp));
        }
        return out;
    }
    return std::nullopt;
}

/// 方法 Code 嵌套 type annotation 槽是否还在（有槽返回条目数，无槽 nullopt）。
std::optional<ca::usize> readCodeTypeAnnoCount(jbc::ir::ClassSession& session,
                                               const std::string&     attrName)
{
    jbc::core::ClassFile& cf = session.backing();
    auto                  decoded = cf.decodeCode(cf.methods[0]);
    if (decoded.is_err()) return std::nullopt;
    const jbc::core::CodeAttr* code = decoded.unwrap();
    if (code == nullptr) return std::nullopt;
    for (const jbc::core::AttrSlot& slot : code->attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, attrName)) continue;
        const auto* body = ca::core::tag_cast::cast<const jbc::core::TypeAnnotationsAttr>(
            slot.peek());
        if (body == nullptr) return std::nullopt;
        return body->annotations.size();
    }
    return std::nullopt;
}

/// 参数注解槽行视图（槽缺失返回 nullopt）。
std::optional<std::vector<std::vector<std::string>>> readParamAnnoRows(
    jbc::ir::ClassSession& session, const std::string& attrName)
{
    jbc::core::ClassFile& cf = session.backing();
    for (jbc::core::AttrSlot& slot : cf.methods[0].attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, attrName)) continue;
        const auto* body = ca::core::tag_cast::cast<const jbc::core::ParameterAnnotationsAttr>(
            slot.peek());
        if (body == nullptr) return std::nullopt;
        std::vector<std::vector<std::string>> out;
        for (const std::vector<jbc::core::AnnotationsAttr::Annotation>& row :
             body->parameters) {
            std::vector<std::string> rowOut;
            for (const jbc::core::AnnotationsAttr::Annotation& a : row) {
                rowOut.emplace_back(cf.cp.utf8Of(a.typeCp));
            }
            out.push_back(std::move(rowOut));
        }
        return out;
    }
    return std::nullopt;
}

// ── pass 驱动 ──

struct RunResult
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    mjbase::spi::PreservedAnalyses         pa;
};

RunResult runLongRemover(const Bytes& bytes, const LongAnnotationRemoverOptions& opts = {})
{
    auto created = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(created.is_ok());
    auto                              session = std::move(created).unwrap();
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    LongAnnotationRemover             pass(opts);
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

// ── 描述符样本 ──

/// 长度 9 的正常注解描述符（判据下方幸存）。
constexpr std::string_view kShortDesc = "Lfoo/Bar;";
/// 长度 200 的巨型垃圾描述符（判据上方删除）：L + 198×A + ;。
std::string longDesc(const ca::usize len = 200)
{
    return "L" + std::string(len - 2, 'A') + ";";
}

}   // namespace

// ── a) 正例：三层注解面阈值命中 ──

TEST(MjPassLongAnnotationRemoverTest, RemovesLongClassAnnotationKeepsShort)
{
    const Bytes bytes = withClassAnnotations(
        assembleClass("R", {{"public static f ()V", "      return\n"}}),
        "RuntimeVisibleAnnotations", {std::string(kShortDesc), longDesc()});
    const auto run = runLongRemover(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto descs = readClassAnnoSlot(*run.session, "RuntimeVisibleAnnotations");
    ASSERT_TRUE(descs.has_value());
    EXPECT_EQ(descs->size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*descs)[0], kShortDesc);
}

TEST(MjPassLongAnnotationRemoverTest, RemovesLongFieldAndMethodAnnotations)
{
    // 带字段的基类：.field public static a I + 方法 f。
    mjbase::jasm::JvmJasmService service;
    const std::string            source =
        ".super java/lang/Object\n.class public super RF {\n"
        "  .field public static a I\n"
        "  .method public static f ()V {\n    code: {\n      return\n    }\n  }\n"
        "}\n";
    const auto result = service.assemble(utf16("RF.jasm").ref(), utf16(source).ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败";
    }
    const Bytes base = result.isOk() && result.value.has_value() ? result.value->bytes : Bytes{};
    ASSERT_FALSE(base.empty());
    const Bytes bytes = withMemberAnnotations(
        withMemberAnnotations(base, std::optional<ca::usize>(0), std::nullopt,
                              "RuntimeVisibleAnnotations", {longDesc()}),
        std::nullopt, std::optional<ca::usize>(0), "RuntimeInvisibleAnnotations",
        {longDesc(), std::string(kShortDesc)});
    const auto run = runLongRemover(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto fieldDescs =
        readMemberAnnoSlot(*run.session, std::optional<ca::usize>(0), std::nullopt,
                           "RuntimeVisibleAnnotations");
    EXPECT_FALSE(fieldDescs.has_value()) << "字段槽删空连槽摘除";
    const auto methodDescs =
        readMemberAnnoSlot(*run.session, std::nullopt, std::optional<ca::usize>(0),
                           "RuntimeInvisibleAnnotations");
    ASSERT_TRUE(methodDescs.has_value());
    EXPECT_EQ(methodDescs->size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*methodDescs)[0], kShortDesc);
}

TEST(MjPassLongAnnotationRemoverTest, RemovesLongTypeAnnotationsIncludingCodeNested)
{
    const Bytes bytes = withTypeAnnotations(
        withTypeAnnotations(assembleClass("R", {{"public static f ()V", "      return\n"}}),
                            2, "RuntimeVisibleTypeAnnotations",
                            {{0x14, longDesc()}, {0x13, std::string(kShortDesc)}}),
        3, "RuntimeInvisibleTypeAnnotations", {{0x13, longDesc()}});
    const auto run = runLongRemover(bytes);

    EXPECT_TRUE(run.pa.changed());
    // 方法级槽：巨型条目删、短条目幸存（槽留）。
    jbc::core::ClassFile& cf = run.session->backing();
    bool                  memberSlotSeen = false;
    for (jbc::core::AttrSlot& slot : cf.methods[0].attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "RuntimeVisibleTypeAnnotations")) continue;
        memberSlotSeen = true;
        const auto* body = ca::core::tag_cast::cast<const jbc::core::TypeAnnotationsAttr>(
            slot.peek());
        ASSERT_NE(body, nullptr);
        ASSERT_EQ(body->annotations.size(), static_cast<ca::usize>(1));
        EXPECT_EQ(cf.cp.utf8Of(body->annotations[0].typeIndexCp), kShortDesc);
    }
    EXPECT_TRUE(memberSlotSeen);
    // Code 嵌套槽：唯一条目巨型 → 删空连槽摘除。
    const auto codeCount = readCodeTypeAnnoCount(*run.session, "RuntimeInvisibleTypeAnnotations");
    EXPECT_FALSE(codeCount.has_value()) << "Code 嵌套槽删空摘除";
}

TEST(MjPassLongAnnotationRemoverTest, RemovesLongParameterAnnotationsKeepingRows)
{
    const Bytes bytes = withParamAnnotations(
        assembleClass("R", {{"public static f (I)V", "      return\n"}}),
        "RuntimeVisibleParameterAnnotations",
        {{std::string(kShortDesc), longDesc()}, {std::string(kShortDesc)}});
    const auto run = runLongRemover(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto rows = readParamAnnoRows(*run.session, "RuntimeVisibleParameterAnnotations");
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), static_cast<ca::usize>(2)) << "行数与行序保持";
    EXPECT_EQ((*rows)[0].size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*rows)[0][0], kShortDesc);
    EXPECT_EQ((*rows)[1].size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*rows)[1][0], kShortDesc);
}

// ── a) 正例：嵌套注解值面递归修剪（LongSubAnnoRemover 对位）──

TEST(MjPassLongAnnotationRemoverTest, PrunesLongNestedAnnotationValue)
{
    // 键值对 '@' 嵌套巨型注解：整对删除、父注解保留（值表空）；数组内
    // '@' 嵌套巨型注解：元素删除、父注解保留。
    const Bytes bytes = withClassNestedAnnotation(
        withClassNestedAnnotation(
            assembleClass("R", {{"public static f ()V", "      return\n"}}),
            "RuntimeVisibleAnnotations", std::string(kShortDesc), longDesc(), false),
        "RuntimeInvisibleAnnotations", std::string(kShortDesc), longDesc(), true);
    const auto run = runLongRemover(bytes);

    EXPECT_TRUE(run.pa.changed());
    const auto visible = readClassAnnoSlot(*run.session, "RuntimeVisibleAnnotations");
    ASSERT_TRUE(visible.has_value());
    ASSERT_EQ(visible->size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*visible)[0], kShortDesc);
    const auto invisible = readClassAnnoSlot(*run.session, "RuntimeInvisibleAnnotations");
    ASSERT_TRUE(invisible.has_value());
    ASSERT_EQ(invisible->size(), static_cast<ca::usize>(1));
    EXPECT_EQ((*invisible)[0], kShortDesc) << "数组内巨型嵌套元素整项删除，父注解保留";
    jbc::core::ClassFile& cf = run.session->backing();
    for (jbc::core::AttrSlot& slot : cf.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "RuntimeInvisibleAnnotations")) continue;
        const auto* body = ca::core::tag_cast::cast<const jbc::core::AnnotationsAttr>(
            slot.peek());
        ASSERT_NE(body, nullptr);
        ASSERT_EQ(body->annos.size(), static_cast<ca::usize>(1));
        // 数组值面：键值对本身保留（'[' 非嵌套注解），元素被修剪成空数组。
        ASSERT_EQ(body->annos[0].values.size(), static_cast<ca::usize>(1));
        EXPECT_EQ(body->annos[0].values[0].second.tag, '[');
        EXPECT_TRUE(body->annos[0].values[0].second.items.empty()) << "巨型嵌套元素已删除";
    }
}

// ── b) 反例与 fail-closed ──

TEST(MjPassLongAnnotationRemoverTest, KeepsShortAnnotationsUntouched)
{
    const Bytes original = withClassAnnotations(
        assembleClass("R", {{"public static f ()V", "      return\n"}}),
        "RuntimeVisibleAnnotations", {std::string(kShortDesc)});
    const auto run = runLongRemover(original);

    EXPECT_FALSE(run.pa.changed()) << "判据不满足：零改动";
    auto written = jbc::core::writeClass(run.session->backing());
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(std::move(written).unwrap(), original) << "字节不动";
}

TEST(MjPassLongAnnotationRemoverTest, DecodeFailureSlotKeptUntouched)
{
    // typed 分派拒收的 Raw 槽（形状不符）：fail-closed 保持原样。
    const Bytes original = withRawAnnotationSlot(
        assembleClass("R", {{"public static f ()V", "      return\n"}}),
        "RuntimeVisibleAnnotations");
    const auto run = runLongRemover(original);

    EXPECT_FALSE(run.pa.changed()) << "Raw 槽保守跳过";
    auto written = jbc::core::writeClass(run.session->backing());
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(std::move(written).unwrap(), original);
}

// ── c) 边界：阈值两侧 ──

TEST(MjPassLongAnnotationRemoverTest, ThresholdBoundaryTwoSides)
{
    // 阈值 10：长度 9 幸存、长度 10 删除（保留 = 长度 < 阈值）。
    LongAnnotationRemoverOptions opts;
    opts.maxDescLength = 10;
    const Bytes bytes = withClassAnnotations(
        assembleClass("R", {{"public static f ()V", "      return\n"}}),
        "RuntimeVisibleAnnotations",
        {std::string(kShortDesc), longDesc(10), longDesc(9)});
    const auto run = runLongRemover(bytes, opts);

    EXPECT_TRUE(run.pa.changed());
    const auto descs = readClassAnnoSlot(*run.session, "RuntimeVisibleAnnotations");
    ASSERT_TRUE(descs.has_value());
    ASSERT_EQ(descs->size(), static_cast<ca::usize>(2));
    EXPECT_EQ((*descs)[0], kShortDesc);
    EXPECT_EQ((*descs)[1], longDesc(9));
}

// ── d) 幂等二跑 ──

TEST(MjPassLongAnnotationRemoverTest, IdempotentSecondRun)
{
    const Bytes original = withClassAnnotations(
        assembleClass("R", {{"public static f ()V", "      return\n"}}),
        "RuntimeVisibleAnnotations", {std::string(kShortDesc), longDesc()});
    const auto first = runLongRemover(original);
    EXPECT_TRUE(first.pa.changed());

    // 同会话二跑（槽已解码）。
    {
        ca::str::Utf8StringArena          arena;
        mjbase::spi::ClassAnalysisManager cam(arena);
        LongAnnotationRemover             pass;
        EXPECT_FALSE(pass.run(*first.session, cam).changed());
    }
    // 跨会话二跑（写回重载，lazy 槽回到未解码形态）。
    {
        const Bytes settled = writeBytes(*first.session);
        const auto  second  = runLongRemover(settled);
        EXPECT_FALSE(second.pa.changed());
        const auto descs = readClassAnnoSlot(*second.session, "RuntimeVisibleAnnotations");
        ASSERT_TRUE(descs.has_value());
        EXPECT_EQ(descs->size(), static_cast<ca::usize>(1));
    }
}

// ── e) 与其他删除件共存 ──

TEST(MjPassLongAnnotationRemoverTest, CoexistsWithSpecificAnnotationRemover)
{
    // 黑名单件吃 jetbrains 注解、本件吃巨型注解，短非黑名单注解幸存——
    // 体积判据与黑名单判据正交，同链两件各自命中。
    const Bytes bytes = withClassAnnotations(
        assembleClass("R", {{"public static f ()V", "      return\n"}}),
        "RuntimeVisibleAnnotations",
        {"Lorg/jetbrains/annotations/NotNull;", longDesc(), std::string(kShortDesc)});
    ResourceContainer container;
    addClassResource(container, "R.class", bytes);

    const auto loaded = mjbase::spi::loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"SpecificAnnotationRemover"},)json"
        R"json({"pass":"LongAnnotationRemover"}]})json",
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
    EXPECT_EQ((*descs)[0], kShortDesc);
}

// ── f) 注册面与 opt-in 接线 ──

TEST(MjPassLongAnnotationRemoverTest, RegisteredAsLongAnnotationRemover)
{
    using mjbase::spi::PassScope;
    mjbase::spi::PassNameRegistry& reg = mjbase::spi::defaultPassRegistry();
    EXPECT_TRUE(reg.has("LongAnnotationRemover"));
    const auto found = reg.find("LongAnnotationRemover");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, PassScope::Class);
    EXPECT_FALSE(found.unwrap().description.empty());

    auto created = reg.createClassPass("LongAnnotationRemover");
    ASSERT_TRUE(created.is_ok());
    const Bytes bytes = withClassAnnotations(
        assembleClass("RG", {{"public static f ()V", "      return\n"}}),
        "RuntimeVisibleAnnotations", {longDesc()});
    auto createdSession = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(createdSession.is_ok());
    auto                              session = std::move(createdSession).unwrap();
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    const auto pa = std::move(created).unwrap()->run(*session, cam);
    EXPECT_TRUE(pa.changed()) << "注册表工厂默认口径开箱可用";
    const auto descs = readClassAnnoSlot(*session, "RuntimeVisibleAnnotations");
    EXPECT_FALSE(descs.has_value()) << "唯一条目巨型 → 删空连槽摘除";
}

TEST(MjPassLongAnnotationRemoverTest, PipelineConfigOptInEnablesRemoval)
{
    // 管线配置直引 + maxLength 参数桶装载钉形：参数对声明 schema 过门（键与
    // 值家族在装载期校验）；参数 → 工厂闭包注入是 phase 2 接线（未接线期
    // 注册表工厂产默认口径 150），此处断言默认阈值生效面——巨型删除、短
    // 注解幸存。
    ResourceContainer container;
    const Bytes bytes = withClassAnnotations(
        assembleClass("PI", {{"public static f ()V", "      return\n"}}),
        "RuntimeVisibleAnnotations", {longDesc(), std::string(kShortDesc)});
    addClassResource(container, "PI.class", bytes);

    const auto loaded = mjbase::spi::loadPipelineConfig(
        R"json({"version":1,"passes":[{"pass":"LongAnnotationRemover",)json"
        R"json("params":{"maxLength":10}}]})json",
        mjbase::spi::defaultPassRegistry());
    ASSERT_TRUE(loaded.is_ok()) << loaded.unwrap_err().detail;
    ASSERT_EQ(loaded.unwrap().sequence.size(), static_cast<ca::usize>(1));
    ASSERT_EQ(loaded.unwrap().sequence[0].params.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(loaded.unwrap().sequence[0].params[0].key, "maxLength");
    EXPECT_EQ(loaded.unwrap().sequence[0].params[0].value.kind(),
              mjbase::spi::PipelineConfigParamKind::Int);

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
    EXPECT_EQ(descs->at(0), kShortDesc) << "默认阈值口径：巨型删除、短注解幸存";
}

TEST(MjPassLongAnnotationRemoverTest, DefaultChainLeavesAnnotationsUntouched)
{
    (void)mjbase::spi::registerBuiltinPipelines();
    const auto& pipelines = mjbase::spi::PassRegistry::instance();
    ASSERT_TRUE(pipelines.hasPipeline("default-optimize"));
    const auto pipeline = pipelines.getPipeline("default-optimize");
    for (const auto& name : pipeline.classPassNames()) {
        EXPECT_NE(name, "LongAnnotationRemover") << "opt-in 不入默认链";
    }
    for (const auto& name : pipeline.methodPassNames()) {
        EXPECT_NE(name, "LongAnnotationRemover") << "opt-in 不入默认链";
    }

    ResourceContainer container;
    const Bytes bytes = withClassAnnotations(
        assembleClass("PD", {{"public static f ()V", "      return\n"}}),
        "RuntimeVisibleAnnotations", {longDesc()});
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
    ASSERT_EQ(descs->size(), static_cast<ca::usize>(1));
    EXPECT_EQ(descs->at(0), longDesc()) << "默认链不收：巨型注解原样";
}
