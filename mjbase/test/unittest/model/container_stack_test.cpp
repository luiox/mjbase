// 角色化容器栈（RFC 0001 §5 容器抽象 / §10-P5）：解析序 primary > library
// （挂载序）> runtime 的 first-wins、重复条目记录（resource_container.hpp
// 现状语义的栈级对应物）、角色过滤遍历、卸载后引用一致性（被遮蔽副本浮现）
// 与复合寻址扩展位（单容器形态可解析；嵌套 bang 链显式拒绝 = 本期只落接口）。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mjbase/model/container_stack.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"

using namespace mjbase::model;

namespace {

std::string toStdString(const ca::str::Utf8StringRef& value)
{
    return value.to_std_string();
}

constexpr ca::u8 kPrimaryMark = 0x01;
constexpr ca::u8 kLibraryMark = 0x02;
constexpr ca::u8 kRuntimeMark = 0x03;

/// 造一个 class 资源（解析/合并按 URI 键，字节内容只作所有物判别）。
std::unique_ptr<ClassResource> classResource(std::string_view uri, ca::u8 mark)
{
    return std::make_unique<ClassResource>(uri, std::vector<ca::u8>{0xCA, 0xFE, 0xBA, 0xBE, mark});
}

ResourceContainer containerWith(
    std::initializer_list<std::pair<std::string_view, ca::u8>> entries)
{
    ResourceContainer container;
    for (const auto& [uri, mark] : entries) {
        container.addResource(uri, classResource(uri, mark));
    }
    return container;
}

const ClassResource* classOf(const IResource* resource)
{
    return ca::core::tag_cast::dyn_cast<const ClassResource>(resource);
}

}   // namespace

//-- 挂载与解析序 --------------------------------------------------------------

TEST(MjModelContainerStackTest, resolveFollowsPrimaryLibraryRuntimeOrder)
{
    auto primary = containerWith({{"com/x/A.class", kPrimaryMark}});
    auto library = containerWith({{"com/x/A.class", kLibraryMark}});
    auto runtime = containerWith({{"com/x/A.class", kRuntimeMark}});

    MountedContainerSet stack;
    // 挂载序故意与角色序相反：解析序由角色决定，与挂载先后无关。
    ASSERT_FALSE(stack.mount("rt", ContainerRole::Runtime, &runtime).is_err());
    ASSERT_FALSE(stack.mount("lib", ContainerRole::Library, &library).is_err());
    ASSERT_FALSE(stack.mount("app", ContainerRole::Primary, &primary).is_err());

    EXPECT_EQ(classOf(stack.resolve("com/x/A.class"))->get().back(), kPrimaryMark);
    ASSERT_TRUE(stack.unmount("app"));
    EXPECT_EQ(classOf(stack.resolve("com/x/A.class"))->get().back(), kLibraryMark);
    ASSERT_TRUE(stack.unmount("lib"));
    EXPECT_EQ(classOf(stack.resolve("com/x/A.class"))->get().back(), kRuntimeMark);
}

TEST(MjModelContainerStackTest, libraryResolutionFollowsMountOrderWithinRole)
{
    auto first  = containerWith({{"com/x/A.class", kPrimaryMark}});
    auto second = containerWith({{"com/x/A.class", kLibraryMark}});

    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("lib-1", ContainerRole::Library, &first).is_err());
    ASSERT_FALSE(stack.mount("lib-2", ContainerRole::Library, &second).is_err());
    // 同角色按挂载序 first-wins（RFC §5.2 解析序的 role 内细则）。
    EXPECT_EQ(classOf(stack.resolve("com/x/A.class"))->get().back(), kPrimaryMark);
}

TEST(MjModelContainerStackTest, mountRejectsDuplicateIdAndBadArguments)
{
    auto container = containerWith({{"a.class", kPrimaryMark}});
    MountedContainerSet stack;
    EXPECT_EQ(stack.mount("", ContainerRole::Primary, &container).unwrap_err(),
              ModelError::BadArgument);
    auto nullMount = stack.mount("x", ContainerRole::Primary, nullptr);
    EXPECT_EQ(nullMount.unwrap_err(), ModelError::BadArgument);
    ASSERT_FALSE(stack.mount("app", ContainerRole::Primary, &container).is_err());
    EXPECT_EQ(stack.mount("app", ContainerRole::Library, &container).unwrap_err(),
              ModelError::BadArgument);
    EXPECT_EQ(stack.size(), 1u);
    EXPECT_NE(stack.find("app"), nullptr);
    EXPECT_EQ(stack.find("missing"), nullptr);
    EXPECT_FALSE(stack.unmount("missing"));
    EXPECT_TRUE(stack.unmount("app"));
    EXPECT_EQ(stack.size(), 0u);
}

TEST(MjModelContainerStackTest, mountRejectsSameContainerUnderSecondIdOrRole)
{
    // 一容器一记录（P20）：同一容器对象换 id/换角色重复注册在模型层拒绝——
    // 双注册是 project_info 双条目与 detach 半边泄漏的模型层根源。
    auto container = containerWith({{"a.class", kPrimaryMark}});
    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("app", ContainerRole::Primary, &container).is_err());
    EXPECT_EQ(stack.mount("support-1", ContainerRole::Library, &container).unwrap_err(),
              ModelError::BadArgument);
    EXPECT_EQ(stack.mount("support-2", ContainerRole::Runtime, &container).unwrap_err(),
              ModelError::BadArgument);
    EXPECT_EQ(stack.size(), 1u);
    // 卸载后同指针可重挂（生命周期正常面）。
    ASSERT_TRUE(stack.unmount("app"));
    ASSERT_FALSE(stack.mount("lib", ContainerRole::Library, &container).is_err());
    EXPECT_EQ(stack.size(), 1u);
}

//-- 重复条目记录与卸载一致性 ---------------------------------------------------

TEST(MjModelContainerStackTest, shadowedEntriesRecordFirstWinsLosers)
{
    auto primary  = containerWith({{"com/x/A.class", kPrimaryMark}, {"com/x/B.class", kPrimaryMark}});
    auto library  = containerWith({{"com/x/A.class", kLibraryMark}, {"com/x/C.class", kLibraryMark}});
    auto runtime  = containerWith({{"com/x/A.class", kRuntimeMark}});

    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("app", ContainerRole::Primary, &primary).is_err());
    ASSERT_FALSE(stack.mount("lib", ContainerRole::Library, &library).is_err());
    ASSERT_FALSE(stack.mount("rt", ContainerRole::Runtime, &runtime).is_err());

    const auto& shadowed = stack.shadowedEntries();
    ASSERT_EQ(shadowed.size(), 2u);   // A 被 lib、rt 遮蔽；B/C 无冲突
    EXPECT_EQ(toStdString(shadowed[0].uri), "com/x/A.class");
    EXPECT_EQ(toStdString(shadowed[0].winnerId), "app");
    EXPECT_EQ(toStdString(shadowed[0].shadowedId), "lib");
    EXPECT_EQ(toStdString(shadowed[1].shadowedId), "rt");
}

TEST(MjModelContainerStackTest, unmountRevealsShadowedCopyConsistently)
{
    auto primary = containerWith({{"com/x/A.class", kPrimaryMark}});
    auto library = containerWith({{"com/x/A.class", kLibraryMark}});

    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("app", ContainerRole::Primary, &primary).is_err());
    ASSERT_FALSE(stack.mount("lib", ContainerRole::Library, &library).is_err());
    ASSERT_EQ(stack.shadowedEntries().size(), 1u);

    // 卸载 = 重建解析序与重复记录：被遮蔽副本浮现（JVM 移除 classpath 条目
    // 后下一副本可见的同款语义），记录同步清空。
    ASSERT_TRUE(stack.unmount("app"));
    EXPECT_TRUE(stack.shadowedEntries().empty());
    ASSERT_NE(stack.resolve("com/x/A.class"), nullptr);
    EXPECT_EQ(classOf(stack.resolve("com/x/A.class"))->get().back(), kLibraryMark);
}

//-- 角色过滤与合并视图 ---------------------------------------------------------

TEST(MjModelContainerStackTest, visitRoleFiltersContainersByRole)
{
    auto primary = containerWith({{"com/x/A.class", kPrimaryMark}});
    auto library = containerWith({{"com/x/L.class", kLibraryMark}});
    auto runtime = containerWith({{"java/lang/Object.class", kRuntimeMark}});

    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("app", ContainerRole::Primary, &primary).is_err());
    ASSERT_FALSE(stack.mount("lib", ContainerRole::Library, &library).is_err());
    ASSERT_FALSE(stack.mount("rt", ContainerRole::Runtime, &runtime).is_err());

    std::vector<std::string> libraryUris;
    stack.visitRole(ContainerRole::Library,
                    [&](std::string_view uri, const IResource&) {
                        libraryUris.emplace_back(uri);
                    });
    ASSERT_EQ(libraryUris.size(), 1u);
    EXPECT_EQ(libraryUris[0], "com/x/L.class");

    std::vector<std::string> runtimeUris;
    stack.visitRole(ContainerRole::Runtime,
                    [&](std::string_view uri, const IResource&) {
                        runtimeUris.emplace_back(uri);
                    });
    ASSERT_EQ(runtimeUris.size(), 1u);
    EXPECT_EQ(runtimeUris[0], "java/lang/Object.class");
}

TEST(MjModelContainerStackTest, mergedViewClonesFirstWinsWithDuplicateRecord)
{
    auto primary = containerWith({{"com/x/A.class", kPrimaryMark}});
    auto library = containerWith({{"com/x/A.class", kLibraryMark}, {"com/x/B.class", kLibraryMark}});

    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("app", ContainerRole::Primary, &primary).is_err());
    ASSERT_FALSE(stack.mount("lib", ContainerRole::Library, &library).is_err());

    auto merged = stack.mergedView();
    ASSERT_FALSE(merged.is_err());
    auto view = std::move(merged).unwrap();
    EXPECT_EQ(view->size(), 2u);
    // first-wins：A 取 primary 字节；重复记录进容器自身 duplicateResourceUris。
    ASSERT_NE(view->getResource("com/x/A.class"), nullptr);
    EXPECT_EQ(classOf(view->getResource("com/x/A.class"))->get().back(), kPrimaryMark);
    ASSERT_EQ(view->duplicateResourceUris().size(), 1u);
    EXPECT_EQ(view->duplicateResourceUris()[0], "com/x/A.class");
    // 合并视图独立于源容器（值克隆，改视图不伤源）。
    view->getResource("com/x/A.class");
}

//-- 复合寻址扩展位（本期只落接口）----------------------------------------------

TEST(MjModelContainerStackTest, mountAddressAndSingleSegmentResolve)
{
    EXPECT_EQ(toStdString(MountedContainerSet::mountAddress("app.jar", "com/x/A.class")),
              "app.jar!/com/x/A.class");

    auto container = containerWith({{"com/x/A.class", kPrimaryMark}});
    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("app.jar", ContainerRole::Primary, &container).is_err());

    auto resolved = stack.resolveAddress("app.jar!/com/x/A.class");
    ASSERT_FALSE(resolved.is_err());
    auto address = std::move(resolved).unwrap();
    EXPECT_EQ(toStdString(address.container->id), "app.jar");
    EXPECT_EQ(toStdString(address.entryUri), "com/x/A.class");
    // 地址钉死容器：命中与否只看该容器（不做跨栈 first-wins）。
    EXPECT_TRUE(stack.resolveAddress("app.jar!/missing.class").is_err());
}

TEST(MjModelContainerStackTest, resolveAddressRejectsMalformedAndNestedForms)
{
    auto container = containerWith({{"com/x/A.class", kPrimaryMark}});
    MountedContainerSet stack;
    ASSERT_FALSE(stack.mount("app.jar", ContainerRole::Primary, &container).is_err());

    // 无分隔符 / 空容器段 / 空条目段 = BadArgument。
    EXPECT_EQ(stack.resolveAddress("com/x/A.class").unwrap_err(), ModelError::BadArgument);
    EXPECT_EQ(stack.resolveAddress("!/com/x/A.class").unwrap_err(), ModelError::BadArgument);
    EXPECT_EQ(stack.resolveAddress("app.jar!/").unwrap_err(), ModelError::BadArgument);
    // 未知容器 id = NotFound。
    EXPECT_EQ(stack.resolveAddress("other.jar!/com/x/A.class").unwrap_err(),
              ModelError::NotFound);
    // 嵌套 bang 链（WAR 内 JAR / fat jar）= 复合寻址扩展位，本期显式拒绝。
    EXPECT_EQ(stack.resolveAddress("app.jar!/WEB-INF/lib/b.jar!/com/x/A.class").unwrap_err(),
              ModelError::BadArgument);
}
