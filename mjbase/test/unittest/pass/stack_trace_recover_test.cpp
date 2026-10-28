// StackTraceRecover（全局件）：SourceFile/SourceDebug 恢复、handler 入口
// dup+printStackTrace 插入（单型取该型、catch-all/多型共享取 Throwable）、
// 每标签一行 LNT（类级跨方法递增）。覆盖：四类 owner 归组、LNT 行与
// 跨方法行号、source 属性覆盖式写入、fail-closed、注册面。夹具经
// shared/structural_pass_util.hpp 手拼 classfile；LNT/source 断言经写回
// 字节的属性解码完成。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/stack_trace_recover.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublicStatic = 0x0009;

mjbase::spi::PreservedAnalyses runPass(mjbase::model::ResourceContainer& container)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::StackTraceRecover    pass;
    return pass.run(container, gam);
}

const mjbase::model::ClassResource* classResource(
    mjbase::model::ResourceContainer& container, const std::string& uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

/// 单方法类：code + 可选异常表（catchCp 由调用方给 0 或 Class 索引）。
Bytes singleMethodClass(FixtureBuilder& b, Bytes code, std::vector<ExcSpec> exc)
{
    b.addMethod({"m", "()V", std::move(code), kAccPublicStatic, std::move(exc), 1,
                 0});
    return b.assemble();
}

/// 读回写回字节中方法 m 的指令操作码序列。
std::vector<jbc::core::Op> methodOps(const Bytes& bytes)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    EXPECT_TRUE(promoted.is_ok());
    if (promoted.is_err()) return {};
    auto                              cuRef = std::move(promoted).unwrap();
    std::vector<jbc::core::Op>        ops;
    for (const jbc::ir::InsnId id : cuRef->dom.order()) {
        ops.push_back(cuRef->dom.peek(id)->op);
    }
    return ops;
}

/// 读回写回字节中方法 m 第 idx 条指令的成员引用语义。
bool memberSemAt(const Bytes& bytes, ca::usize insnIdx,
                 jbc::ir::ClassSession::MemberSem& out)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return false;
    auto session = std::move(sessionResult).unwrap();
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    EXPECT_TRUE(promoted.is_ok());
    if (promoted.is_err()) return false;
    auto      cuRef = std::move(promoted).unwrap();
    const jbc::ir::Insn* in = cuRef->dom.peek(cuRef->dom.order()[insnIdx]);
    return in != nullptr && session->memberOf(*in, out);
}

/// 读回写回字节中第 idx 个方法的 LNT 行。
std::vector<jbc::core::LineNumberTableAttr::Row> lntRows(const Bytes& bytes,
                                                         ca::usize    methodIdx)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    jbc::core::ClassFile& backing = session->backing();
    auto codeDecoded = backing.decodeCode(backing.methods[methodIdx]);
    EXPECT_TRUE(codeDecoded.is_ok());
    if (codeDecoded.is_err()) return {};
    jbc::core::CodeAttr* codeAttr = codeDecoded.unwrap();
    for (jbc::core::AttrSlot& slot : codeAttr->attrs) {
        if (!backing.cp.utf8Equals(slot.nameCp, "LineNumberTable")) continue;
        auto decoded = backing.decodeAttr(slot);
        EXPECT_TRUE(decoded.is_ok());
        if (decoded.is_err()) return {};
        auto* lnt =
            ca::core::tag_cast::dyn_cast<jbc::core::LineNumberTableAttr*>(decoded.unwrap());
        EXPECT_NE(lnt, nullptr);
        if (lnt == nullptr) return {};
        return lnt->rows;
    }
    ADD_FAILURE() << "no LineNumberTable attribute";
    return {};
}

}   // namespace

// ── 单型 handler：入口插 dup + invokevirtual <类型>.printStackTrace()V ──
TEST(StackTraceRecoverTest, handlerInsertsPrintStackTrace)
{
    mjbase::model::ResourceContainer container;
    Bytes bytes;
    {
        FixtureBuilder b("t/S");
        const ca::u16  cpEx = b.cls("java/lang/Exception");
        // nop; nop; return + exc(0,2,2,Exception)：handler = pc2（return）。
        bytes = singleMethodClass(b, {0x00, 0x00, 0xb1}, {ExcSpec{0, 2, 2, cpEx}});
        addClassResource(container, "t/S.class", std::move(bytes));
    }

    EXPECT_TRUE(runPass(container).changed());

    const auto* cls = classResource(container, "t/S.class");
    ASSERT_NE(cls, nullptr);
    const std::vector<jbc::core::Op> ops = methodOps(cls->get());
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], jbc::core::Op::NOP);
    EXPECT_EQ(ops[1], jbc::core::Op::NOP);
    EXPECT_EQ(ops[2], jbc::core::Op::DUP);
    EXPECT_EQ(ops[3], jbc::core::Op::INVOKEVIRTUAL);
    EXPECT_EQ(ops[4], jbc::core::Op::RETURN);
    jbc::ir::ClassSession::MemberSem sem;
    ASSERT_TRUE(memberSemAt(cls->get(), 3, sem));
    EXPECT_EQ(sem.owner, "java/lang/Exception");
    EXPECT_EQ(sem.name, "printStackTrace");
    EXPECT_EQ(sem.desc, "()V");
}

// ── catch-all handler：owner 取 Throwable ──
TEST(StackTraceRecoverTest, catchAllUsesThrowable)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("t/S");
        Bytes bytes = singleMethodClass(b, {0x00, 0x00, 0xb1}, {ExcSpec{0, 2, 2, 0}});
        addClassResource(container, "t/S.class", std::move(bytes));
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* cls = classResource(container, "t/S.class");
    ASSERT_NE(cls, nullptr);
    jbc::ir::ClassSession::MemberSem sem;
    ASSERT_TRUE(memberSemAt(cls->get(), 3, sem));
    EXPECT_EQ(sem.owner, "java/lang/Throwable");
}

// ── 多类型共享 handler：owner 取 Throwable ──
TEST(StackTraceRecoverTest, conflictedSharedHandlerUsesThrowable)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("t/S");
        const ca::u16 cpEx  = b.cls("java/lang/Exception");
        const ca::u16 cpErr = b.cls("java/lang/Error");
        Bytes         bytes = singleMethodClass(
            b, {0x00, 0x00, 0xb1},
            {ExcSpec{0, 1, 2, cpEx}, ExcSpec{1, 2, 2, cpErr}});
        addClassResource(container, "t/S.class", std::move(bytes));
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* cls = classResource(container, "t/S.class");
    ASSERT_NE(cls, nullptr);
    // 共享 handler 只插一组（dup+invoke 聚合后一处）。
    const std::vector<jbc::core::Op> ops = methodOps(cls->get());
    ASSERT_EQ(ops.size(), 5u);
    jbc::ir::ClassSession::MemberSem sem;
    ASSERT_TRUE(memberSemAt(cls->get(), 3, sem));
    EXPECT_EQ(sem.owner, "java/lang/Throwable");
}

// ── 同类型共享 handler：owner 取该类型，且只插一组 ──
TEST(StackTraceRecoverTest, sameTypeSharedHandlerUsesType)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("t/S");
        const ca::u16 cpEx  = b.cls("java/lang/Exception");
        Bytes         bytes = singleMethodClass(
            b, {0x00, 0x00, 0xb1},
            {ExcSpec{0, 1, 2, cpEx}, ExcSpec{1, 2, 2, cpEx}});
        addClassResource(container, "t/S.class", std::move(bytes));
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* cls = classResource(container, "t/S.class");
    ASSERT_NE(cls, nullptr);
    jbc::ir::ClassSession::MemberSem sem;
    ASSERT_TRUE(memberSemAt(cls->get(), 3, sem));
    EXPECT_EQ(sem.owner, "java/lang/Exception");
}

// ── 每标签一行 LNT（分支目标锚点；行号类级从 1 递增） ──
TEST(StackTraceRecoverTest, lntRowsWritten)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("t/S");
        // goto pc5; nop; nop; return —— GOTO 目标锚在 return（pc5）。
        Bytes bytes = singleMethodClass(b, {0xa7, 0x00, 0x05, 0x00, 0x00, 0xb1}, {});
        addClassResource(container, "t/S.class", std::move(bytes));
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* cls = classResource(container, "t/S.class");
    ASSERT_NE(cls, nullptr);
    const std::vector<jbc::core::LineNumberTableAttr::Row> rows = lntRows(cls->get(), 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].startPc, 5u);
    EXPECT_EQ(rows[0].line, 1u);
}

// ── 行号类级跨方法递增 ──
TEST(StackTraceRecoverTest, lineCounterCrossesMethods)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("t/S");
        b.addMethod({"a", "()V",
                     Bytes{0xa7, 0x00, 0x04, 0x00, 0xb1},   // goto pc4; nop; return
                     kAccPublicStatic, {}, 1, 0});
        b.addMethod({"b", "()V",
                     Bytes{0xa7, 0x00, 0x04, 0x00, 0xb1}, kAccPublicStatic, {}, 1, 0});
        addClassResource(container, "t/S.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* cls = classResource(container, "t/S.class");
    ASSERT_NE(cls, nullptr);
    const std::vector<jbc::core::LineNumberTableAttr::Row> rowsA = lntRows(cls->get(), 0);
    ASSERT_EQ(rowsA.size(), 1u);
    EXPECT_EQ(rowsA[0].line, 1u);
    const std::vector<jbc::core::LineNumberTableAttr::Row> rowsB = lntRows(cls->get(), 1);
    ASSERT_EQ(rowsB.size(), 1u);
    EXPECT_EQ(rowsB[0].line, 2u);
}

// ── SourceFile/SourceDebug 覆盖式写入 ──
TEST(StackTraceRecoverTest, sourceAttrsWritten)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("t/S");
        b.addMethod({"m", "()V", Bytes{0xb1}, kAccPublicStatic, {}, 1, 0});
        addClassResource(container, "t/S.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* cls = classResource(container, "t/S.class");
    ASSERT_NE(cls, nullptr);
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();
    jbc::core::ClassFile& backing = session->backing();
    EXPECT_EQ(backing.thisName(), "t/S");

    jbc::core::AttrSlot* sf = backing.findClassAttr("SourceFile");
    ASSERT_NE(sf, nullptr);
    ASSERT_TRUE(backing.decodeAttr(*sf).is_ok());
    const auto* sfBody =
        ca::core::tag_cast::dyn_cast<jbc::core::SourceFileAttr*>(sf->peek());
    ASSERT_NE(sfBody, nullptr);
    EXPECT_EQ(backing.cp.utf8Of(sfBody->sourceFileCp), "t.S.java");

    jbc::core::AttrSlot* sd = backing.findClassAttr("SourceDebug");
    ASSERT_NE(sd, nullptr);
    ASSERT_TRUE(backing.decodeAttr(*sd).is_ok());
    const auto* sdBody = ca::core::tag_cast::dyn_cast<jbc::core::RawAttr*>(sd->peek());
    ASSERT_NE(sdBody, nullptr);
    const ca::u32 expectIdx = backing.cp.findUtf8("t.S.java");
    ASSERT_NE(expectIdx, 0u);
    ASSERT_EQ(sdBody->bytes().size(), 2u);
    EXPECT_EQ(sdBody->bytes()[0], static_cast<ca::u8>(expectIdx >> 8));
    EXPECT_EQ(sdBody->bytes()[1], static_cast<ca::u8>(expectIdx));
}

// ── 已有 SourceFile 槽被改写（旧栈 setter 语义） ──
TEST(StackTraceRecoverTest, sourceFileOverwriteExisting)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("t/S");
        const ca::u16  oldUtf8 = b.utf8("Old.java");
        FixtureAttr    attr;
        attr.name   = "SourceFile";
        attr.payload = {static_cast<ca::u8>(oldUtf8 >> 8), static_cast<ca::u8>(oldUtf8)};
        b.addClassAttr(attr);
        b.addMethod({"m", "()V", Bytes{0xb1}, kAccPublicStatic, {}, 1, 0});
        addClassResource(container, "t/S.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* cls = classResource(container, "t/S.class");
    ASSERT_NE(cls, nullptr);
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();
    jbc::core::ClassFile& backing = session->backing();
    jbc::core::AttrSlot*  sf      = backing.findClassAttr("SourceFile");
    ASSERT_NE(sf, nullptr);
    ASSERT_TRUE(backing.decodeAttr(*sf).is_ok());
    const auto* sfBody =
        ca::core::tag_cast::dyn_cast<jbc::core::SourceFileAttr*>(sf->peek());
    ASSERT_NE(sfBody, nullptr);
    EXPECT_EQ(backing.cp.utf8Of(sfBody->sourceFileCp), "t.S.java");
}

// ── 类不可解析 → 整件放弃（fail-closed） ──
TEST(StackTraceRecoverTest, unparsableClassAborts)
{
    mjbase::model::ResourceContainer container;
    container.replaceResource("bad.class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  "bad.class", Bytes{0xCA, 0xFE, 0xBA, 0xBE, 0x00}));
    {
        FixtureBuilder b("t/S");
        b.addMethod({"m", "()V", Bytes{0xb1}, kAccPublicStatic, {}, 1, 0});
        addClassResource(container, "t/S.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).areAllPreserved());
    const auto* cls = classResource(container, "t/S.class");
    ASSERT_NE(cls, nullptr);
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(cls->get()));
    ASSERT_TRUE(sessionResult.is_ok());
    EXPECT_EQ(sessionResult.unwrap()->backing().findClassAttr("SourceFile"), nullptr);
}

// ── 注册面 ──
TEST(StackTraceRecoverTest, registeredInRegistry)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("StackTraceRecover"));
    auto created =
        mjbase::spi::defaultPassRegistry().createGlobalPass("StackTraceRecover");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
