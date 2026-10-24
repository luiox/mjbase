// 角色化容器登记（P20）验收面：mount(role=library) → info 恰一条记录 →
// detach（support ID 唯一句柄）→ 零条。library/runtime 与遗留 support 共用
// 同一在册表：路径/内容唯一约束跨角色生效，role 只是记录标签——不再出现
// "support 半边 + library 半边"的双注册，也没有无 detach 句柄的残留条目。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/container_stack.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/workspace/workspace.hpp"

namespace mjbase::workspace {
namespace model = mjbase::model;

namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::filesystem::path writeSubjectJar(const std::string& fileName)
{
    jasm::JvmJasmService service;
    const std::string    source = ".super java/lang/Object\n"
                 ".class public super workspace/Subject {\n"
                 "  .method public static value ()I {\n"
                 "    code: {\n"
                 "      bipush 1\n"
                 "      ireturn\n"
                 "    }\n"
                 "  }\n"
                 "}\n";
    auto result = service.assemble(utf16("Subject.jasm").ref(), utf16(source).ref());
    if (!result.isOk()) throw std::runtime_error("role fixture subject did not assemble");
    model::ResourceContainer container;
    const std::string        uri = "workspace/Subject.class";
    container.addResource(uri,
                          std::make_unique<model::ClassResource>(uri, result.value->bytes));
    const auto path = test::getTempOutPath(fileName);
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path writeLibraryJar(const std::string& fileName)
{
    jasm::JvmJasmService service;
    const std::string    source = ".super java/lang/Object\n"
                 ".class public super support/Library {\n"
                 "}\n";
    auto result = service.assemble(utf16("Library.jasm").ref(), utf16(source).ref());
    if (!result.isOk()) throw std::runtime_error("role fixture library did not assemble");
    model::ResourceContainer container;
    const std::string        uri = "support/Library.class";
    container.addResource(uri,
                          std::make_unique<model::ClassResource>(uri, result.value->bytes));
    const auto path = test::getTempOutPath(fileName);
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// 复制出同内容不同路径的 JAR（内容唯一约束的判别面）。
std::filesystem::path copyJar(const std::filesystem::path& source, const std::string& fileName)
{
    const auto  copy = test::getTempOutPath(fileName);
    std::error_code error;
    std::filesystem::remove(copy, error);
    error.clear();
    std::filesystem::copy_file(source, copy, error);
    if (error) throw std::runtime_error("role fixture copy failed");
    return copy;
}

}   // namespace

TEST(MjWorkspaceSupportingRoleTest, libraryMountProducesOneRecordAndDetachClearsIt)
{
    WorkspaceBase workspace;
    const auto    primary = writeSubjectJar("mjbase_role_primary.jar");
    const auto    library = writeLibraryJar("mjbase_role_library.jar");
    ASSERT_TRUE(workspace.openJar(primary.string()).is_ok());

    // mount（role=library）→ info 恰一条记录，role 标签为 library。
    auto mounted = workspace.addSupportingContainer(library.string(),
                                                    model::ContainerRole::Library);
    ASSERT_TRUE(mounted.is_ok());
    const auto mountedValue = std::move(mounted).unwrap();
    EXPECT_EQ("support-1", mountedValue.id);
    EXPECT_EQ("library", mountedValue.role);
    EXPECT_EQ(0U, mountedValue.fingerprint.find("fnv1a64:"));

    const auto listed = workspace.supportingResources();
    ASSERT_EQ(1U, listed.size());
    EXPECT_EQ("library", listed[0].role);
    EXPECT_EQ(mountedValue.id, listed[0].id);

    // 换角色重挂同一路径/同内容：唯一约束跨角色生效，不追加第二条记录。
    auto samePath = workspace.addSupportingJar(library.string());
    ASSERT_TRUE(samePath.is_err());
    EXPECT_EQ(WorkspaceError::AlreadyExists, std::move(samePath).unwrap_err().code);
    const auto copied = copyJar(library, "mjbase_role_library_copy.jar");
    auto sameContent = workspace.addSupportingContainer(copied.string(),
                                                        model::ContainerRole::Runtime);
    ASSERT_TRUE(sameContent.is_err());
    EXPECT_EQ(WorkspaceError::AlreadyExists, std::move(sameContent).unwrap_err().code);
    EXPECT_EQ(1U, workspace.supportingResources().size());

    // detach（support ID 唯一句柄）→ 该容器全部角色注册一次性清理，零残留。
    auto detached = workspace.removeSupportingJar(mountedValue.id);
    ASSERT_TRUE(detached.is_ok());
    EXPECT_EQ("library", std::move(detached).unwrap().role);
    EXPECT_TRUE(workspace.supportingResources().empty());
}

TEST(MjWorkspaceSupportingRoleTest, primaryRoleRejectedAndLegacySupportUnchanged)
{
    WorkspaceBase workspace;
    const auto    primary = writeSubjectJar("mjbase_role_support_primary.jar");
    const auto    library = writeLibraryJar("mjbase_role_support_lib.jar");
    ASSERT_TRUE(workspace.openJar(primary.string()).is_ok());

    // primary 由 project_open 绑定，不接受挂载登记。
    auto rejected = workspace.addSupportingContainer(library.string(),
                                                     model::ContainerRole::Primary);
    ASSERT_TRUE(rejected.is_err());
    EXPECT_EQ(WorkspaceError::InvalidArgument, std::move(rejected).unwrap_err().code);
    EXPECT_TRUE(workspace.supportingResources().empty());

    // 遗留 addSupportingJar 语义不变：缺省 role 标签 support。
    auto legacy = workspace.addSupportingJar(library.string());
    ASSERT_TRUE(legacy.is_ok());
    EXPECT_EQ("support", std::move(legacy).unwrap().role);
    const auto listed = workspace.supportingResources();
    ASSERT_EQ(1U, listed.size());
    EXPECT_EQ("support", listed[0].role);
}

}   // namespace mjbase::workspace
