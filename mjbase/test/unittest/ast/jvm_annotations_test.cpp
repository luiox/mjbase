#include <gtest/gtest.h>

#include <fstream>

#include "jdk_fixture.hpp"
#include "mjbase/ast/jvm_annotations.hpp"
#include "shared/jasm_fixture.hpp"

namespace {

using jbc::core::ByteView;
using mjbase::KeyPool;
using namespace mjbase::ast;

constexpr JvmAnnotationTarget kClassTarget{};
constexpr JvmAnnotationTarget kFieldTarget{JvmAnnotationTarget::Kind::Field, "entry", "I"};
constexpr JvmAnnotationTarget kMethodTarget{JvmAnnotationTarget::Kind::Method, "run", "()V"};
constexpr JvmAnnotationTarget kOverload{JvmAnnotationTarget::Kind::Method, "run", "(I)V"};

const char* kFixture = R"JASM(
.visible-annotation Lanno/Outer; {
    child: .annotation Lanno/Inner; {
        kind: .enum Lanno/Choice; BLUE,
        message: "old"
    },
    list: { "a", "b" }
}
.invisible-annotation Lanno/Binary; { value: "hidden" }
.super java/lang/Object
.class public super anno/Holder {
    .visible-annotation Lanno/FieldTag; { value: "field" }
    .field public entry I

    .visible-annotation Lanno/MethodTag; { value: "method" }
    .method public static run ()V {
        code: {
            return
        }
    }
    .visible-annotation Lanno/OverloadTag; { value: "overload" }
    .method public static run (I)V {
        code: {
            return
        }
    }
}
)JASM";

template <typename T>
const T* elementOf(const Annotation& annotation, const KeyPool& keys, std::string_view name)
{
    for (const auto& element : annotation.elements) {
        if (keys.str(element.first) == name) return std::get_if<T>(&element.second.value);
    }
    return nullptr;
}

void expectAnnotation(jbc::ir::ClassSession& session, JvmAnnotationTarget target,
                      AnnotationVisibility visibility, KeyPool& keys, std::string_view desc,
                      std::u16string_view text)
{
    auto result = readJvmAnnotations(session, target, visibility, keys);
    ASSERT_TRUE(result.is_ok());
    const auto annotations = std::move(result).unwrap();
    ASSERT_EQ(annotations.size(), 1u);
    EXPECT_EQ(keys.str(annotations.front().descriptor), desc);
    const auto* value = elementOf<std::u16string>(annotations.front(), keys, "value");
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, text);
}

TEST(MjAstJvmAnnotationsTest, ReplacesNestedValueAndPreservesOtherDeclarations)
{
    const auto bytes = mjbase::test::assembleJasm(kFixture);
    ASSERT_FALSE(bytes.empty());
    const std::u16string replacement{u'A', u'\0', static_cast<char16_t>(0xD800)};
    auto edited = rewriteJvmAnnotations(ByteView(bytes), kClassTarget, AnnotationVisibility::Runtime,
        [&](std::vector<Annotation>& annotations, KeyPool& keys) {
            if (annotations.size() == 1 && keys.str(annotations.front().descriptor) == "Lanno/Outer;") {
                for (auto& [name, item] : annotations.front().elements) {
                    if (keys.str(name) != "child") continue;
                    auto* child = std::get_if<AnnotationValue::Nested>(&item.value);
                    if (!child || !*child) break;
                    for (auto& [childName, childItem] : (*child)->elements) {
                        if (keys.str(childName) != "message") continue;
                        EXPECT_EQ(std::get<std::u16string>(childItem.value), u"old");
                        childItem.value = replacement;
                        return true;
                    }
                }
            }
            ADD_FAILURE() << "missing nested value";
            return false;
        });
    ASSERT_TRUE(edited.is_ok());
    auto rewrittenBytes = std::move(edited).unwrap();
    ASSERT_NE(bytes, rewrittenBytes);
    auto result = jbc::ir::makeSessionFromBytes(ByteView(rewrittenBytes));
    ASSERT_TRUE(result.is_ok());
    auto session = result.unwrap();
    KeyPool keys;
    auto read = readJvmAnnotations(*session, kClassTarget, AnnotationVisibility::Runtime, keys);
    ASSERT_TRUE(read.is_ok());
    const auto annotations = std::move(read).unwrap();
    ASSERT_EQ(annotations.size(), 1u);
    const auto* child = elementOf<AnnotationValue::Nested>(annotations.front(), keys, "child");
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(*child);
    EXPECT_EQ(keys.str((*child)->descriptor), "Lanno/Inner;");
    const auto* message = elementOf<std::u16string>(**child, keys, "message");
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(*message, replacement);

    const auto* enumValue = elementOf<AnnotationEnum>(**child, keys, "kind");
    ASSERT_NE(enumValue, nullptr);
    EXPECT_EQ(keys.str(enumValue->constant.cls), "anno/Choice");
    EXPECT_EQ(keys.str(enumValue->constant.name), "BLUE");
    EXPECT_EQ(keys.str(enumValue->constant.desc), "Lanno/Choice;");
    const auto* list = elementOf<AnnotationValue::Array>(annotations.front(), keys, "list");
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->size(), 2u);
    EXPECT_EQ(std::get<std::u16string>((*list)[0].value), u"a");
    EXPECT_EQ(std::get<std::u16string>((*list)[1].value), u"b");

    expectAnnotation(*session, kClassTarget, AnnotationVisibility::Binary, keys,
                     "Lanno/Binary;", u"hidden");
    expectAnnotation(*session, kFieldTarget, AnnotationVisibility::Runtime, keys,
                     "Lanno/FieldTag;", u"field");
    expectAnnotation(*session, kMethodTarget, AnnotationVisibility::Runtime, keys,
                     "Lanno/MethodTag;", u"method");

    auto deleted = rewriteJvmAnnotations(ByteView(rewrittenBytes), kClassTarget,
        AnnotationVisibility::Runtime, [](std::vector<Annotation>& annotations, KeyPool&) {
            annotations.clear();
            return true;
        });
    ASSERT_TRUE(deleted.is_ok());
    auto withoutClassAnnotation = jbc::ir::makeSessionFromBytes(ByteView(deleted.unwrap()));
    ASSERT_TRUE(withoutClassAnnotation.is_ok());
    auto absent = readJvmAnnotations(*withoutClassAnnotation.unwrap(), kClassTarget,
                                     AnnotationVisibility::Runtime, keys);
    ASSERT_TRUE(absent.is_ok());
    EXPECT_TRUE(std::move(absent).unwrap().empty());
}

TEST(MjAstJvmAnnotationsTest, EditsOnlyExactOverloadAndReturnsByteIdentityOnFalse)
{
    const auto bytes = mjbase::test::assembleJasm(kFixture);
    ASSERT_FALSE(bytes.empty());
    auto result = rewriteJvmAnnotations(ByteView(bytes), kOverload, AnnotationVisibility::Runtime,
        [](std::vector<Annotation>& annotations, KeyPool& keys) {
            if (annotations.size() != 1 || keys.str(annotations.front().descriptor) != "Lanno/OverloadTag;") {
                ADD_FAILURE() << "wrong overload";
                return false;
            }
            annotations.front().elements[0].second.value = std::u16string(u"changed");
            return true;
        });
    ASSERT_TRUE(result.is_ok());
    const auto rewrittenBytes = std::move(result).unwrap();
    auto loaded = jbc::ir::makeSessionFromBytes(ByteView(rewrittenBytes));
    ASSERT_TRUE(loaded.is_ok());
    auto session = loaded.unwrap();
    KeyPool keys;
    expectAnnotation(*session, kOverload, AnnotationVisibility::Runtime, keys,
                     "Lanno/OverloadTag;", u"changed");
    expectAnnotation(*session, kMethodTarget, AnnotationVisibility::Runtime, keys,
                     "Lanno/MethodTag;", u"method");

    auto declined = rewriteJvmAnnotations(ByteView(rewrittenBytes), kOverload,
        AnnotationVisibility::Runtime, [](std::vector<Annotation>& annotations, KeyPool&) {
            annotations.clear();
            return false;
        });
    ASSERT_TRUE(declined.is_ok());
    EXPECT_EQ(declined.unwrap(), rewrittenBytes);
}

TEST(MjAstJvmAnnotationsTest, AddsRuntimeAnnotationReadableByJvmReflection)
{
    const auto jdk = test::jdk::probeJdk();
    if (!jdk.available) GTEST_SKIP() << jdk.reason;
    const auto bytes = mjbase::test::assembleClass("AnnotationAstTarget", "java/lang/Object", "");
    ASSERT_FALSE(bytes.empty());
    auto edited = rewriteJvmAnnotations(ByteView(bytes), kClassTarget, AnnotationVisibility::Runtime,
        [](std::vector<Annotation>& annotations, KeyPool& keys) {
            EXPECT_TRUE(annotations.empty());
            Annotation tag{keys.intern("LAnnotationAstMarker;"), {}};
            tag.elements.emplace_back(keys.intern("value"),
                                      AnnotationValue{std::u16string(u"semantic")});
            tag.elements.emplace_back(keys.intern("empty"), AnnotationValue{std::u16string{}});
            tag.elements.emplace_back(keys.intern("none"),
                                      AnnotationValue{AnnotationValue::Array{}});
            annotations.push_back(std::move(tag));
            return true;
        });
    ASSERT_TRUE(edited.is_ok());

    const auto markerPath = test::getTempOutPath("AnnotationAstMarker.java");
    std::ofstream marker(markerPath);
    ASSERT_TRUE(marker);
    marker << R"JAVA(
@java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
public @interface AnnotationAstMarker {
    String value();
    String empty();
    int[] none();
}
)JAVA";
    marker.close();
    test::jdk::RunnerSpec spec;
    spec.classes.emplace_back("AnnotationAstTarget", std::move(edited).unwrap());
    spec.extraJavaSources.push_back(markerPath);
    spec.runnerBody = R"JAVA(
        AnnotationAstMarker marker = AnnotationAstTarget.class.getAnnotation(AnnotationAstMarker.class);
        System.out.println(marker.value());
        System.out.println(marker.empty().length());
        System.out.println(marker.none().length);
    )JAVA";
    const auto run = test::jdk::runWithRunnerOutcome(jdk, spec);
    ASSERT_TRUE(run.ok()) << run.output;
    EXPECT_EQ(run.output, "semantic\n0\n0\n");
}

}   // namespace
