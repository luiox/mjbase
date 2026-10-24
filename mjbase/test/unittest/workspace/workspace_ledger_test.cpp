// WorkspaceBase 台账换底（schema v2 交付 3）：commit/undo/redo/revert_class 走
// EditLedgerStore 的行为面——undo/redo 窗口与事务链一致、revert_class 反向应用、
// savepoint = 书签（restore 记反向事务，undo 窗不受损）、bind → flush → 重开
// 采纳（undo 跨 flush 存活）、手动压缩前后 restore 结果一致。
//
// 断言面全部走 WorkspaceBase 公开签名（换底不外泄内部引擎）+ bundle 磁盘面
// （project_store 清单原语）；既有语义（revision 序列等）由 workspace_test.cpp
// 承载。契约点见 doc_ai/analysis-project-schema-v2.md §6（mjt 波次 2b 接线）。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/project/edit_ledger_store.hpp"
#include "mjbase/project/project_store.hpp"
#include "mjbase/workspace/workspace.hpp"

namespace mjbase::workspace {
namespace model = mjbase::model;

namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string classSource(const std::string& name, const std::string& value)
{
    return ".super java/lang/Object\n"
           ".class public super " +
           name +
           " {\n"
           "  .field public counter I\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      bipush " +
           value +
           "\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& value)
{
    jasm::JvmJasmService service;
    auto                 sourceName = utf16(name + ".jasm");
    auto                 source     = utf16(classSource(name, value));
    auto                 result     = service.assemble(sourceName.ref(), source.ref());
    if (!result.isOk()) throw std::runtime_error("workspace ledger fixture did not assemble");
    return result.value->bytes;
}

/// 单类 JAR 夹具（workspace/Subject = <value>）。
std::filesystem::path writeLedgerFixture(const std::string& fileName, const std::string& value)
{
    model::ResourceContainer container;
    const std::string        uri = "workspace/Subject.class";
    container.addResource(uri, std::make_unique<model::ClassResource>(
                                   uri, assemble("workspace/Subject", value)));
    const auto path = test::getTempOutPath(fileName);
    model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// 清掉上一次 run 的同名临时 bundle，返回干净路径。
std::filesystem::path freshTempDir(const std::string& name)
{
    const auto  path = test::getTempOutPath(name);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path;
}

MutationRequest expected(ca::u64 revision, bool dryRun = false)
{
    MutationRequest request;
    request.expectedRevision = revision;
    request.dryRun           = dryRun;
    return request;
}

/// 工作副本中 Subject 类的原始字节（事务位一致性的比较基准）。
std::vector<ca::u8> subjectBytes(const WorkspaceBase& workspace)
{
    auto content = workspace.readClassBytes("workspace/Subject");
    EXPECT_TRUE(content.is_ok());
    return content.is_ok() ? std::move(content).unwrap().bytes : std::vector<ca::u8>{};
}

/// bundle 清单中已登记的 section 名集合。
std::vector<std::string> registeredSections(const std::string& dirStr)
{
    auto loaded = project::readStoreManifestFile(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(dirStr.data()),
                                          dirStr.size()));
    EXPECT_FALSE(loaded.is_err());
    std::vector<std::string> names;
    if (loaded.is_ok()) {
        for (const auto& entry : std::move(loaded).unwrap().sections) {
            names.push_back(entry.name.to_std_string());
        }
    }
    return names;
}

bool applySubjectValue(WorkspaceBase& workspace, ca::u64 revision, const std::string& value)
{
    auto sourceName = utf16("Subject.jasm");
    auto outcome    = workspace.applyJasm("workspace/Subject.class", sourceName.ref(),
                                          utf16(classSource("workspace/Subject", value)).ref(),
                                          expected(revision));
    return outcome.status.is_ok();
}

}   // namespace

TEST(MjWorkspaceLedgerTest, commitUndoRedoWindowMatchesTransactionChain)
{
    WorkspaceBase workspace;
    const auto    input = writeLedgerFixture("mjbase_workspace_ledger_input.jar", "1");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    const auto  originalBytes = subjectBytes(workspace);

    ASSERT_TRUE(applySubjectValue(workspace, 0, "42"));
    const auto  afterFirstBytes = subjectBytes(workspace);
    ASSERT_TRUE(applySubjectValue(workspace, 1, "43"));
    EXPECT_EQ(2U, workspace.revision());
    const auto  headBytes = subjectBytes(workspace);
    EXPECT_NE(originalBytes, headBytes);

    // undo/redo 窗口：头指针回退/前进，字节精确回到对应事务位。
    auto undone = workspace.undo(expected(2));
    ASSERT_TRUE(undone.status.is_ok());
    EXPECT_EQ(afterFirstBytes, subjectBytes(workspace));
    EXPECT_TRUE(workspace.isDirty());   // 尚有一格未退到基线

    ASSERT_TRUE(workspace.undo(expected(3)).status.is_ok());
    EXPECT_EQ(originalBytes, subjectBytes(workspace));
    EXPECT_FALSE(workspace.isDirty());   // 退到基线 = 干净

    auto redone = workspace.redo(expected(4));
    ASSERT_TRUE(redone.status.is_ok());
    EXPECT_EQ(afterFirstBytes, subjectBytes(workspace));

    ASSERT_TRUE(workspace.redo(expected(5)).status.is_ok());
    EXPECT_EQ(headBytes, subjectBytes(workspace));
    EXPECT_TRUE(workspace.isDirty());

    // 新 commit 放弃 redo 尾分支（对齐旧 commit 清 redo 栈）。
    ASSERT_TRUE(applySubjectValue(workspace, 6, "99"));
    auto deadRedo = workspace.redo(expected(7));
    ASSERT_FALSE(deadRedo.status.is_ok());
    EXPECT_EQ(WorkspaceError::FailedPrecondition, deadRedo.status.code);
}

TEST(MjWorkspaceLedgerTest, revertClassRestoresOriginalBytesThroughLedger)
{
    WorkspaceBase workspace;
    const auto    input = writeLedgerFixture("mjbase_workspace_ledger_revert.jar", "7");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    const auto  originalBytes = subjectBytes(workspace);

    ASSERT_TRUE(applySubjectValue(workspace, 0, "8"));
    const auto  afterFirstBytes = subjectBytes(workspace);
    ASSERT_TRUE(applySubjectValue(workspace, 1, "9"));
    const auto  afterSecondBytes = subjectBytes(workspace);

    // revert_class = 反向应用到原始字节（再走一次普通 commit，记新事务）。
    auto reverted = workspace.revertClass("workspace/Subject.class", expected(2));
    ASSERT_TRUE(reverted.status.is_ok());
    EXPECT_TRUE(reverted.committed);
    EXPECT_EQ(originalBytes, subjectBytes(workspace));
    EXPECT_FALSE(workspace.isDirty());   // revert = 回到原始字节，不再偏离

    // revert 本身可 undo（回到 revert 前头位），再逐级退回基线。
    auto undone = workspace.undo(expected(3));
    ASSERT_TRUE(undone.status.is_ok());
    EXPECT_EQ(afterSecondBytes, subjectBytes(workspace));
    ASSERT_TRUE(workspace.undo(expected(4)).status.is_ok());
    EXPECT_EQ(afterFirstBytes, subjectBytes(workspace));
    ASSERT_TRUE(workspace.undo(expected(5)).status.is_ok());
    EXPECT_EQ(originalBytes, subjectBytes(workspace));
}

TEST(MjWorkspaceLedgerTest, savepointRestoresAcrossLaterCommitsWithoutLosingUndo)
{
    WorkspaceBase workspace;
    const auto    input = writeLedgerFixture("mjbase_workspace_ledger_savepoint.jar", "1");
    ASSERT_TRUE(workspace.openJar(input.string()).is_ok());
    const auto  originalBytes = subjectBytes(workspace);

    ASSERT_TRUE(workspace.createSavepoint("original").is_ok());
    ASSERT_TRUE(applySubjectValue(workspace, 0, "2"));
    const auto  afterFirstBytes = subjectBytes(workspace);
    ASSERT_TRUE(applySubjectValue(workspace, 1, "3"));
    const auto  beforeRestoreBytes = subjectBytes(workspace);

    // restore = 物化书签位状态 + 普通提交（反向事务），undo 窗不受损。
    auto restored = workspace.restoreSavepoint("original", expected(2));
    ASSERT_TRUE(restored.status.is_ok());
    EXPECT_EQ(3U, restored.revision);
    EXPECT_EQ(originalBytes, subjectBytes(workspace));

    // 反向事务可 undo（回到 restore 前头位），再 redo 回 restore 后。
    auto undone = workspace.undo(expected(3));
    ASSERT_TRUE(undone.status.is_ok());
    EXPECT_EQ(beforeRestoreBytes, subjectBytes(workspace));
    auto redone = workspace.redo(expected(4));
    ASSERT_TRUE(redone.status.is_ok());
    EXPECT_EQ(originalBytes, subjectBytes(workspace));

    // 未知名 savepoint 拒绝。
    auto missing = workspace.restoreSavepoint("never-was", expected(5));
    ASSERT_FALSE(missing.status.is_ok());
    EXPECT_EQ(WorkspaceError::NotFound, missing.status.code);
}

TEST(MjWorkspaceLedgerTest, bindFlushReopenAdoptsHistoryAndKeepsUndoAlive)
{
    const auto  jar    = writeLedgerFixture("mjbase_workspace_ledger_bind.jar", "1");
    const auto  dir    = freshTempDir("mjbase_workspace_ledger_bind.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(project::createBundleSkeleton(project::copyLedgerText(dirStr)).is_err());

    {
        WorkspaceBase workspace;
        ASSERT_TRUE(workspace.openJar(jar.string()).is_ok());
        ASSERT_TRUE(workspace.bindLedgerStore(dirStr).is_ok());
        ASSERT_TRUE(applySubjectValue(workspace, 0, "2"));
        ASSERT_TRUE(workspace.createSavepoint("edited").is_ok());
        ASSERT_TRUE(workspace.flushLedger().is_ok());
        EXPECT_EQ(1U, workspace.revision());   // flush/savepoint 不改 revision 计数
    }
    // 磁盘面契约：flush 后清单带台账代 + overlay 载荷 section。
    const auto  sections = registeredSections(dirStr);
    bool ledgerSeen = false;
    bool overlaySeen = false;
    for (const auto& name : sections) {
        if (name == "edit-ledger-1") ledgerSeen = true;
        if (name == "overlay-1-0") overlaySeen = true;
    }
    EXPECT_TRUE(ledgerSeen);
    EXPECT_TRUE(overlaySeen);

    // 新会话绑定同一 bundle：台账采纳（工作副本推进到头事务位），undo 跨 flush
    // 存活，savepoint 可 restore。
    WorkspaceBase reopened;
    ASSERT_TRUE(reopened.openJar(jar.string()).is_ok());
    const auto  originalBytes = subjectBytes(reopened);
    ASSERT_TRUE(reopened.bindLedgerStore(dirStr).is_ok());
    const auto  adoptedBytes = subjectBytes(reopened);
    EXPECT_NE(originalBytes, adoptedBytes);
    EXPECT_EQ(0U, reopened.revision());

    auto undone = reopened.undo(expected(0));
    ASSERT_TRUE(undone.status.is_ok());
    EXPECT_EQ(originalBytes, subjectBytes(reopened));

    auto saved = reopened.restoreSavepoint("edited", expected(1));
    ASSERT_TRUE(saved.status.is_ok());
    EXPECT_EQ(adoptedBytes, subjectBytes(reopened));
}

TEST(MjWorkspaceLedgerTest, manualCompactionKeepsRestoreAndUndoWindowConsistent)
{
    const auto  jar    = writeLedgerFixture("mjbase_workspace_ledger_compact.jar", "1");
    const auto  dir    = freshTempDir("mjbase_workspace_ledger_compact.mjt-project");
    const std::string dirStr = dir.string();
    ASSERT_FALSE(project::createBundleSkeleton(project::copyLedgerText(dirStr)).is_err());

    // 外提捕获：会话作用域在压缩续链 flush 后收口（单写者锁随对象释放）。
    std::vector<ca::u8> headBytes;
    std::vector<ca::u8> bookmarkedBytes;
    std::vector<ca::u8> twoEditsBytes;
    project::LedgerCompactRequest request;
    request.keepUndoDepth = 2;
    {
        WorkspaceBase workspace;
        ASSERT_TRUE(workspace.openJar(jar.string()).is_ok());
        ASSERT_TRUE(workspace.bindLedgerStore(dirStr).is_ok());
        const auto  originalBytes = subjectBytes(workspace);

    // 四个编辑事务（txns 1..4）+ t2 位书签。
    const char* values[5] = {"1", "2", "3", "4", "5"};
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(applySubjectValue(workspace, static_cast<ca::u64>(i), values[i + 1]));
        if (i == 1) {
            ASSERT_TRUE(workspace.createSavepoint("keep-me").is_ok());
            twoEditsBytes = subjectBytes(workspace);
        }
    }
    ASSERT_TRUE(workspace.flushLedger().is_ok());
    headBytes = subjectBytes(workspace);

    // 手动压缩：undo 承诺 2、书签全保留 → 保留点 = min(head-2, 书签位) = 2。
    auto compacted = workspace.compactLedger(request);
    ASSERT_FALSE(compacted.is_err());
    const auto  report = std::move(compacted).unwrap();
    EXPECT_TRUE(report.executed);
    EXPECT_EQ(2U, report.retentionTxn);
    EXPECT_TRUE(report.droppedBookmarks.empty());

    // 磁盘面：新基线 + 新代台账已登记。
    const auto  sections = registeredSections(dirStr);
    bool baselineSeen = false;
    bool newLedgerSeen = false;
    for (const auto& name : sections) {
        if (name == "edit-baseline-2") baselineSeen = true;
        if (name == "edit-ledger-2") newLedgerSeen = true;
    }
    EXPECT_TRUE(baselineSeen);
    EXPECT_TRUE(newLedgerSeen);

    // 压缩不变量 1：undo 承诺兑现——恰好两步回退到书签位，第三步拒绝。
    auto undoOne = workspace.undo(expected(4));
    ASSERT_TRUE(undoOne.status.is_ok());
    auto undoTwo = workspace.undo(expected(5));
    ASSERT_TRUE(undoTwo.status.is_ok());
    // 保留点 = 书签位（txn 2）：两步 undo 停在书签位状态，而不是基线。
    EXPECT_EQ(twoEditsBytes, subjectBytes(workspace));
    EXPECT_NE(originalBytes, subjectBytes(workspace));
    auto undoThree = workspace.undo(expected(6));
    ASSERT_FALSE(undoThree.status.is_ok());
    EXPECT_EQ(WorkspaceError::FailedPrecondition, undoThree.status.code);

    // 压缩不变量 2：书签位状态不漂移（restore 越过压缩锚与被压缩区间）。
    auto redone = workspace.redo(expected(6));
    ASSERT_TRUE(redone.status.is_ok());
    auto redoneAgain = workspace.redo(expected(7));
    ASSERT_TRUE(redoneAgain.status.is_ok());
    EXPECT_EQ(headBytes, subjectBytes(workspace));
    auto saved = workspace.restoreSavepoint("keep-me", expected(8));
    ASSERT_TRUE(saved.status.is_ok());
    bookmarkedBytes = subjectBytes(workspace);
    EXPECT_NE(headBytes, bookmarkedBytes);

    // 压缩后链继续生长（txn 5 = restore 反向事务），flush + 重开闭合。
    ASSERT_TRUE(applySubjectValue(workspace, 9, "6"));
    ASSERT_TRUE(workspace.flushLedger().is_ok());
    }

    WorkspaceBase reopened;
    ASSERT_TRUE(reopened.openJar(jar.string()).is_ok());
    auto bound = reopened.bindLedgerStore(dirStr);
    if (bound.is_err()) {
        const auto bindErr = std::move(bound).unwrap_err();
        FAIL() << "reopen bind: kind=" << static_cast<int>(bindErr.code) << " "
               << bindErr.message;
    }
    const auto  finalBytes = subjectBytes(reopened);
    EXPECT_NE(headBytes, finalBytes);

    // 重开后书签 restore 结果与压缩侧一致（书签跨压缩与跨会话存活）。
    auto restoredAgain = reopened.restoreSavepoint("keep-me", expected(0));
    ASSERT_TRUE(restoredAgain.status.is_ok());
    EXPECT_EQ(bookmarkedBytes, subjectBytes(reopened));

    // 保留点不前移（undo 承诺 2 ≥ 头-锚距离的可达上界受书签约束）→ 拒绝再压。
    auto refused = reopened.compactLedger(request);
    ASSERT_TRUE(refused.is_err());
    EXPECT_EQ(WorkspaceError::FailedPrecondition, std::move(refused).unwrap_err().code);
}

}   // namespace mjbase::workspace
