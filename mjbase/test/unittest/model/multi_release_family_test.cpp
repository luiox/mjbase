/// @file        multi_release_family_test.cpp
/// @brief       Multi-Release 族契约测试：URI 形状原语与同内名族视图索引
///              （JEP 238 口径）。
///
/// 族契约（model/resource_container.hpp）：base 与 META-INF/versions/&lt;N&gt;
/// 变体共用同一类内部名，按内名结算的面必须对族内全部成员一致生效；本文件
/// 锁定容器层原语的形状口径，改写/删除面的族结算由 pass 域用例锁定，主属性
/// 守护面见 multi_release_manifest_guard_test.cpp。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "mjbase/jar/manifest.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::model;
using namespace mjbase::test;

namespace {

Bytes minimalClass(const std::string& internalName)
{
    FixtureBuilder builder(internalName);
    FixtureMethod  t;
    t.name       = "t";
    t.desc       = "()V";
    t.maxLocals  = 0;
    t.maxStack   = 0;
    t.code       = bareReturn();
    builder.addMethod(t);
    return builder.assemble();
}

void addVersionedClass(ResourceContainer& container, const char* uri, const char* internalName)
{
    container.addResource(uri,
                          std::make_unique<ClassResource>(uri, minimalClass(internalName)));
}

}   // namespace

// ── URI 形状原语 ────────────────────────────────────────────────────────────

TEST(MjModelMultiReleaseTest, VersionOfUriCoversShapeMatrix)
{
    // base 形与无关形状 → -1。
    EXPECT_EQ(multiReleaseVersionOfClassUri("app/Api.class"), -1);
    EXPECT_EQ(multiReleaseVersionOfClassUri("Api.class"), -1);
    EXPECT_EQ(multiReleaseVersionOfClassUri("META-INF/MANIFEST.MF"), -1);
    EXPECT_EQ(multiReleaseVersionOfClassUri("data.txt"), -1);
    EXPECT_EQ(multiReleaseVersionOfClassUri(""), -1);
    // 版本化形状：纯数字版本段（前缀后必须还有内容）。
    EXPECT_EQ(multiReleaseVersionOfClassUri("META-INF/versions/9/x/Y.class"), 9);
    EXPECT_EQ(multiReleaseVersionOfClassUri("META-INF/versions/17/app/Api.class"), 17);
    // 形状级判定对 < 9 的数字段同样成立（语义口径由调用方叠加）。
    EXPECT_EQ(multiReleaseVersionOfClassUri("META-INF/versions/8/app/Api.class"), 8);
    // 非法形状：前缀裸尾、缺版本段、非数字段、缺后续内容。
    EXPECT_EQ(multiReleaseVersionOfClassUri("META-INF/versions/"), -1);
    EXPECT_EQ(multiReleaseVersionOfClassUri("META-INF/versions/17/"), -1);
    EXPECT_EQ(multiReleaseVersionOfClassUri("META-INF/versions/abc/app/Api.class"), -1);
    EXPECT_EQ(multiReleaseVersionOfClassUri("META-INF/versions//app/Api.class"), -1);
    // 溢出守卫：超长数字段归非法。
    EXPECT_EQ(multiReleaseVersionOfClassUri("META-INF/versions/99999999999999999999/x.class"),
              -1);
}

TEST(MjModelMultiReleaseTest, InternalNameStripsPrefixAndExtension)
{
    EXPECT_EQ(internalNameFromClassUri("app/Api.class"), "app/Api");
    EXPECT_EQ(internalNameFromClassUri("Api.class"), "Api");
    EXPECT_EQ(internalNameFromClassUri("META-INF/versions/17/app/Api.class"), "app/Api");
    // JEP 238：版本 < 9 的版本段归非法，不剥前缀（非 canonical class URI）。
    EXPECT_EQ(internalNameFromClassUri("META-INF/versions/8/app/Api.class"), "");
    EXPECT_EQ(internalNameFromClassUri("META-INF/versions/abc/app/Api.class"), "");
    EXPECT_EQ(internalNameFromClassUri("data.txt"), "");
    EXPECT_EQ(internalNameFromClassUri("META-INF/versions/17/app/Api.CLASS"), "");
}

// ── 族视图索引 ──────────────────────────────────────────────────────────────

TEST(MjModelMultiReleaseTest, FamilyIndexGroupsBaseAndVariants)
{
    ResourceContainer container;
    addClassResource(container, "app/Main.class", minimalClass("app/Main"));
    addClassResource(container, "app/Api.class", minimalClass("app/Api"));
    addVersionedClass(container, "META-INF/versions/11/app/Api.class", "app/Api");
    addVersionedClass(container, "META-INF/versions/17/app/Api.class", "app/Api");
    addVersionedClass(container, "META-INF/versions/17/app/Extra.class", "app/Extra");
    container.addResource("META-INF/MANIFEST.MF",
                          std::make_unique<ManifestResource>(mjbase::jar::Manifest()));
    container.addResource("data.txt",
                          std::make_unique<UnknownResource>("data.txt", Bytes{0x01}));

    const auto index = buildClassFamilyIndex(container);
    ASSERT_EQ(index.size(), 3u);   // app/Main / app/Api / app/Extra

    const auto& api = index.at("app/Api");
    ASSERT_TRUE(!api.baseUri.empty());
    EXPECT_EQ(api.baseUri, "app/Api.class");
    ASSERT_EQ(api.variants.size(), 2u);
    EXPECT_EQ(api.variants[0].version, 11);   // (版本, URI) 升序
    EXPECT_EQ(api.variants[0].uri, "META-INF/versions/11/app/Api.class");
    EXPECT_EQ(api.variants[1].version, 17);
    EXPECT_EQ(api.variants[1].uri, "META-INF/versions/17/app/Api.class");

    // 仅存版本化变体的族原样呈现（base 缺失不升格、不造 base）。
    const auto& extra = index.at("app/Extra");
    EXPECT_TRUE(extra.baseUri.empty());
    ASSERT_EQ(extra.variants.size(), 1u);
    EXPECT_EQ(extra.variants[0].uri, "META-INF/versions/17/app/Extra.class");

    const auto& main = index.at("app/Main");
    EXPECT_EQ(main.baseUri, "app/Main.class");
    EXPECT_TRUE(main.variants.empty());

    // 非 class 资源不入索引。
    EXPECT_EQ(index.find("META-INF/MANIFEST.MF"), index.end());
}

TEST(MjModelMultiReleaseTest, FamilyIndexIgnoresIllegalVersionSegments)
{
    ResourceContainer container;
    addVersionedClass(container, "META-INF/versions/8/app/Api.class", "app/Api");
    const auto index = buildClassFamilyIndex(container);
    EXPECT_TRUE(index.empty());   // N < 9 归非法形状，不入族索引
}
