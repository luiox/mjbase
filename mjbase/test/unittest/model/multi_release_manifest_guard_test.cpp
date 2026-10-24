/// @file        multi_release_manifest_guard_test.cpp
/// @brief       Multi-Release 主属性守护测试：MANIFEST.MF 的 Multi-Release:
///              true 保证（字节级保序）与 exportToJar 接线（JEP 238：缺属性
///              则 JVM 9+ 忽略整棵 versions 树）。
///
/// 守护口径（model/resource_helper.hpp）：容器存在 ≥ 9 的版本化 class 条目时
/// 保证主属性在场——既有键原位覆写、缺失键追加尾部、无 Manifest 资源则新增
/// 最小 Manifest；反向（删光版本化条目后属性残留）不剥、零改动。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <memory>
#include <string>
#include <vector>

#include "mjbase/jar/manifest.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
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

const ManifestResource* manifestOf(const ResourceContainer& container)
{
    return ca::core::tag_cast::dyn_cast<const ManifestResource>(
        container.getResource(ca::str::Utf8StringRef::from_cstr("META-INF/MANIFEST.MF")));
}

std::string manifestText(const ResourceContainer& container)
{
    const auto* manifest = manifestOf(container);
    if (manifest == nullptr) return {};
    const auto bytes = manifest->get().toBytes();
    return std::string(bytes.begin(), bytes.end());
}

Bytes manifestBytesOf(const char* text)
{
    const std::string raw(text);
    return Bytes(raw.begin(), raw.end());
}

}   // namespace

TEST(MjModelMultiReleaseGuardTest, AddsAttributeAppendingTail)
{
    ResourceContainer container;
    addClassResource(container, "app/Main.class", minimalClass("app/Main"));
    addVersionedClass(container, "META-INF/versions/17/app/Api.class", "app/Api");
    container.addResource("META-INF/MANIFEST.MF",
                          std::make_unique<ManifestResource>(ManifestResource::fromBytes(
                              manifestBytesOf("Manifest-Version: 1.0\r\nMain-Class: "
                                              "app.Main\r\nCreated-By: test\r\n\r\n"))));

    EXPECT_TRUE(ResourceHelper::ensureMultiReleaseManifest(container));
    // 字节级保序：既有行原位不动，新键追加尾部。
    EXPECT_EQ(manifestText(container),
              "Manifest-Version: 1.0\r\n"
              "Main-Class: app.Main\r\n"
              "Created-By: test\r\n"
              "Multi-Release: true\r\n"
              "\r\n");
}

TEST(MjModelMultiReleaseGuardTest, OverwritesExistingValueInPlace)
{
    ResourceContainer container;
    addVersionedClass(container, "META-INF/versions/11/app/Api.class", "app/Api");
    container.addResource(
        "META-INF/MANIFEST.MF",
        std::make_unique<ManifestResource>(ManifestResource::fromBytes(
            manifestBytesOf("Manifest-Version: 1.0\r\nMulti-Release: false\r\n\r\n"))));

    EXPECT_TRUE(ResourceHelper::ensureMultiReleaseManifest(container));
    // 既有键原位覆写，行位置不变。
    EXPECT_EQ(manifestText(container),
              "Manifest-Version: 1.0\r\nMulti-Release: true\r\n\r\n");
}

TEST(MjModelMultiReleaseGuardTest, IsIdempotentWhenAlreadyTrue)
{
    ResourceContainer container;
    addVersionedClass(container, "META-INF/versions/17/app/Api.class", "app/Api");
    const Bytes manifest = manifestBytesOf("Manifest-Version: 1.0\r\nMulti-Release: true\r\n\r\n");
    container.addResource("META-INF/MANIFEST.MF",
                          std::make_unique<ManifestResource>(ManifestResource::fromBytes(manifest)));

    EXPECT_FALSE(ResourceHelper::ensureMultiReleaseManifest(container));
    EXPECT_EQ(manifestText(container),
              std::string(manifest.begin(), manifest.end()));
}

TEST(MjModelMultiReleaseGuardTest, CreatesMinimalManifestWhenAbsent)
{
    ResourceContainer container;
    addClassResource(container, "app/Main.class", minimalClass("app/Main"));
    addVersionedClass(container, "META-INF/versions/17/app/Api.class", "app/Api");

    EXPECT_TRUE(ResourceHelper::ensureMultiReleaseManifest(container));
    const auto* manifest = manifestOf(container);
    ASSERT_NE(manifest, nullptr);
    EXPECT_EQ(manifestText(container),
              "Manifest-Version: 1.0\r\nMulti-Release: true\r\n\r\n");
}

TEST(MjModelMultiReleaseGuardTest, NoOpWithoutVersionedClassEntries)
{
    ResourceContainer container;
    addClassResource(container, "app/Main.class", minimalClass("app/Main"));
    addClassResource(container, "app/Api.class", minimalClass("app/Api"));
    const Bytes manifest = manifestBytesOf("Manifest-Version: 1.0\r\n\r\n");
    container.addResource("META-INF/MANIFEST.MF",
                          std::make_unique<ManifestResource>(ManifestResource::fromBytes(manifest)));

    // 无版本化条目：零改动（属性缺失也不补）。
    EXPECT_FALSE(ResourceHelper::ensureMultiReleaseManifest(container));
    EXPECT_EQ(manifestText(container), std::string(manifest.begin(), manifest.end()));

    // N < 9 的版本化形状是 JVM 不可识别条目，同样不触发守护。
    ResourceContainer legacy;
    addVersionedClass(legacy, "META-INF/versions/8/app/Api.class", "app/Api");
    legacy.addResource("META-INF/MANIFEST.MF",
                       std::make_unique<ManifestResource>(ManifestResource::fromBytes(manifest)));
    EXPECT_FALSE(ResourceHelper::ensureMultiReleaseManifest(legacy));
    EXPECT_EQ(manifestText(legacy), std::string(manifest.begin(), manifest.end()));
}

// ── 导出接线 ────────────────────────────────────────────────────────────────

TEST(MjModelMultiReleaseGuardTest, ExportToJarWiresGuardAndRoundTrips)
{
    ResourceContainer container;
    addClassResource(container, "app/Main.class", minimalClass("app/Main"));
    addVersionedClass(container, "META-INF/versions/11/app/Api.class", "app/Api");
    addVersionedClass(container, "META-INF/versions/17/app/Api.class", "app/Api");
    container.addResource(
        "META-INF/MANIFEST.MF",
        std::make_unique<ManifestResource>(
            ManifestResource::fromBytes(manifestBytesOf("Manifest-Version: 1.0\r\n\r\n"))));

    const auto path = test::getTempOutPath("mjbase_mr_guard_export.jar");
    ASSERT_TRUE(ResourceHelper::exportToJar(container, path.string()).is_ok());

    // 重读导出产物：主属性已补齐，版本化条目全部在位。
    ResourceContainer reloaded;
    ASSERT_TRUE(ResourceHelper::importFromJar(reloaded, path.string()).is_ok());
    const auto* manifest = manifestOf(reloaded);
    ASSERT_NE(manifest, nullptr);
    EXPECT_EQ(manifest->get()
                  .getMainAttribute(ca::str::Utf8StringRef::from_cstr("Multi-Release"))
                  .to_std_string(),
              "true");
    EXPECT_NE(reloaded.getResource("META-INF/versions/11/app/Api.class"), nullptr);
    EXPECT_NE(reloaded.getResource("META-INF/versions/17/app/Api.class"), nullptr);
    EXPECT_NE(reloaded.getResource("app/Main.class"), nullptr);
}

TEST(MjModelMultiReleaseGuardTest, ExportToJarWithoutVersionedEntriesKeepsManifestUntouched)
{
    ResourceContainer container;
    addClassResource(container, "app/Main.class", minimalClass("app/Main"));
    container.addResource(
        "META-INF/MANIFEST.MF",
        std::make_unique<ManifestResource>(
            ManifestResource::fromBytes(manifestBytesOf("Manifest-Version: 1.0\r\n\r\n"))));

    const auto path = test::getTempOutPath("mjbase_mr_guard_plain_export.jar");
    ASSERT_TRUE(ResourceHelper::exportToJar(container, path.string()).is_ok());

    ResourceContainer reloaded;
    ASSERT_TRUE(ResourceHelper::importFromJar(reloaded, path.string()).is_ok());
    const auto* manifest = manifestOf(reloaded);
    ASSERT_NE(manifest, nullptr);
    EXPECT_EQ(manifest->get()
                  .getMainAttribute(ca::str::Utf8StringRef::from_cstr("Multi-Release"))
                  .to_std_string(),
              "");   // 无版本化条目不添加属性
}
