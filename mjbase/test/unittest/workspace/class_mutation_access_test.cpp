// 访问位编辑语义（P19）验收面：
// - change_access（新 op）：{set, clear} 两组名单增量编辑——未提及位一律保留
//   （ACC_SUPER 等 set_class_access 整体替换下丢失的 flag 不再丢）；
// - "deprecated" 是 Deprecated 属性不是 access flag：映射为属性增删，禁止
//   静默丢弃；旧 op（set_class_access）带 0x20000 位同样映射属性（历史版本
//   静默丢弃的修补，整体替换语义本身不变——旧工程重放兼容契约）；
// - 未知名 flag 报错并列出 class 目标合法 flag 全集；set/clear 相交报错。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "jbc/ir/units.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/workspace/classfile_support.hpp"
#include "mjbase/workspace/workspace.hpp"

namespace mjbase::workspace {

namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string subjectSource()
{
    return ".super java/lang/Object\n"
           ".class public super workspace/Subject {\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      bipush 1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::filesystem::path writeAccessFixture(const std::string& fileName)
{
    jasm::JvmJasmService service;
    auto                 result = service.assemble(utf16("Subject.jasm").ref(),
                                                   utf16(subjectSource()).ref());
    if (!result.isOk()) throw std::runtime_error("access fixture did not assemble");
    model::ResourceContainer container;
    const std::string        uri = "workspace/Subject.class";
    container.addResource(uri, std::make_unique<model::ClassResource>(
                                   uri, result.value->bytes));
    const auto path = test::getTempOutPath(fileName);
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

MutationRequest request(ca::u64 revision)
{
    MutationRequest options;
    options.expectedRevision = revision;
    return options;
}

/// @brief 工作副本中 Subject 的 access 位（classInfo 面）。
ca::i32 subjectAccess(const WorkspaceBase& workspace)
{
    auto info = workspace.classInfo("workspace/Subject");
    EXPECT_TRUE(info.is_ok());
    return info.is_ok() ? std::move(info).unwrap().access : -1;
}

/// @brief CP Utf8 条目文本（骨架解析面；非 Utf8/越界返回空）。
std::string supportUtf8Name(const jbc::core::ClassFile& file, ca::u32 nameCp)
{
    return support::utf8Text(file, nameCp);
}

/// @brief 工作副本中 Subject 的类级属性名集合（jbc IR 会话解析，与 decodeClass
///        同一解析面；会话须在局部存活的 unique_ptr 中持有，backing 引用不可
///        绑到 unwrap 临时量上）。
std::vector<std::string> subjectAttributeNames(const WorkspaceBase& workspace)
{
    std::vector<std::string> names;
    auto content = workspace.readClassBytes("workspace/Subject");
    EXPECT_TRUE(content.is_ok());
    if (content.is_err()) return names;
    auto parsed = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView(std::move(content).unwrap().bytes));
    EXPECT_FALSE(parsed.is_err());
    if (parsed.is_err()) return names;
    auto session = std::move(parsed).unwrap();
    const jbc::core::ClassFile& file = session->backing();
    for (const auto& slot : file.attrs) {
        names.push_back(supportUtf8Name(file, slot.nameCp));
    }
    return names;
}

/// @brief ChangeClassAccess 便捷构造。
ClassMutation changeAccess(std::vector<std::string> setFlags,
                           std::vector<std::string> clearFlags)
{
    ClassMutation mutation;
    mutation.classUri   = "workspace/Subject.class";
    mutation.kind       = ClassMutationKind::ChangeClassAccess;
    mutation.setFlags   = std::move(setFlags);
    mutation.clearFlags = std::move(clearFlags);
    return mutation;
}

bool applyOk(WorkspaceBase& workspace, const ClassMutation& mutation, ca::u64 revision)
{
    auto outcome = workspace.applyClassMutation(mutation, request(revision));
    return outcome.status.is_ok();
}

}   // namespace

TEST(MjClassMutationAccessTest, changeAccessKeepsUnmentionedBitsLikeAccSuper)
{
    WorkspaceBase workspace;
    const auto    input = writeAccessFixture("mjbase_access_change.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    // ".class public super" → 0x0021（public | super）。
    EXPECT_EQ(0x0021, subjectAccess(workspace));

    // set final → 0x0031：super 位原样保留（P19 破坏性整体替换的对照）。
    ASSERT_TRUE(applyOk(workspace, changeAccess({"final"}, {}), 0));
    EXPECT_EQ(0x0031, subjectAccess(workspace));

    // set abstract + clear final → 0x0421：提及位增删，未提及位（super）不动。
    ASSERT_TRUE(applyOk(workspace, changeAccess({"abstract"}, {"final"}), 1));
    EXPECT_EQ(0x0421, subjectAccess(workspace));

    // 空名单 = 无操作（幂等直通）。
    ASSERT_TRUE(applyOk(workspace, changeAccess({}, {}), 2));
    EXPECT_EQ(0x0421, subjectAccess(workspace));
}

TEST(MjClassMutationAccessTest, deprecatedMapsToAttributeNotSilentlyDropped)
{
    WorkspaceBase workspace;
    const auto    input = writeAccessFixture("mjbase_access_deprecated.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    const auto before = subjectAttributeNames(workspace);
    EXPECT_EQ(std::vector<std::string>::difference_type(0),
              std::count(before.begin(), before.end(), "Deprecated"));

    // change_access set deprecated → Deprecated 属性出现（access 位不变）。
    ASSERT_TRUE(applyOk(workspace, changeAccess({"deprecated"}, {}), 0));
    EXPECT_EQ(0x0021, subjectAccess(workspace));
    const auto added = subjectAttributeNames(workspace);
    EXPECT_NE(std::count(added.begin(), added.end(), "Deprecated"), 0);

    // clear deprecated → 属性移除。
    ASSERT_TRUE(applyOk(workspace, changeAccess({}, {"deprecated"}), 1));
    const auto removed = subjectAttributeNames(workspace);
    EXPECT_EQ(std::count(removed.begin(), removed.end(), "Deprecated"), 0);
}

TEST(MjClassMutationAccessTest, legacySetClassAccessKeepsWholeReplaceAndMapsDeprecated)
{
    WorkspaceBase workspace;
    const auto    input = writeAccessFixture("mjbase_access_legacy.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());

    // 旧字段旧语义（版本兼容契约）：整体替换 access 位，未提及位丢弃照旧——
    // 旧工程重放按此解释，行为不漂移。
    ClassMutation legacy;
    legacy.classUri = "workspace/Subject.class";
    legacy.kind     = ClassMutationKind::SetClassAccess;
    legacy.access   = 0x0031;   // public final super
    ASSERT_TRUE(applyOk(workspace, legacy, 0));
    EXPECT_EQ(0x0031, subjectAccess(workspace));

    legacy.access = 0x0001;   // 只给 public：0x0030 丢弃（旧语义本身如此）
    ASSERT_TRUE(applyOk(workspace, legacy, 1));
    EXPECT_EQ(0x0001, subjectAccess(workspace));

    // 0x20000 deprecated 位：历史版本静默丢弃 → 现映射 Deprecated 属性（禁止
    // 静默丢弃；access 位内该位本就不是 flag）。
    legacy.access = 0x20001;   // public + deprecated(属性)
    ASSERT_TRUE(applyOk(workspace, legacy, 2));
    EXPECT_EQ(0x0001, subjectAccess(workspace));
    const auto attrs = subjectAttributeNames(workspace);
    EXPECT_NE(std::count(attrs.begin(), attrs.end(), "Deprecated"), 0);
}

TEST(MjClassMutationAccessTest, unknownFlagReportsLegalSetAndConflictsReject)
{
    WorkspaceBase workspace;
    const auto    input = writeAccessFixture("mjbase_access_unknown.jar");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());

    // 未知名报错并列出 class 目标合法 flag 全集。
    auto outcome = workspace.applyClassMutation(changeAccess({"sealedd"}, {}),
                                                request(kAnyRevision));
    ASSERT_TRUE(outcome.status.is_err());
    const auto message = outcome.status.message;
    EXPECT_NE(std::string::npos, message.find("unknown access flag"));
    EXPECT_NE(std::string::npos, message.find("legal flags"));
    EXPECT_NE(std::string::npos, message.find("sealed"));
    EXPECT_NE(std::string::npos, message.find("deprecated"));

    // set 与 clear 相交拒绝。
    auto conflict = workspace.applyClassMutation(changeAccess({"final"}, {"final"}),
                                                 request(kAnyRevision));
    ASSERT_TRUE(conflict.status.is_err());
    EXPECT_NE(std::string::npos, conflict.status.message.find("both set and cleared"));

    // 结果态违约拒绝：interface 必须 abstract（现类非接口，set interface 落
    // interface&!abstract）。
    auto illegal = workspace.applyClassMutation(changeAccess({"interface"}, {}),
                                                request(kAnyRevision));
    ASSERT_TRUE(illegal.status.is_err());
    EXPECT_NE(std::string::npos, illegal.status.message.find("interface must be abstract"));
}

}   // namespace mjbase::workspace
