/// 移植自旧栈 unittest/pass/opt/recover_pass_test.cpp 的 ClassMemberSorterTest
/// 段（206 行起），并按任务补齐旧栈缺失的独立直接用例：排序键矩阵、条件 PA、
/// 重排后 lower/backingIndex 一致性。载体按新契约重定型：ClassNode 骨架 →
/// ClassSession（backing + unit 同步重排）。
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/class_member_sorter.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/preserved_analyses.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

/// 会话里按下标取方法内部名。
std::string_view methodName(const jbc::ir::ClassSession& session, ca::usize idx)
{
    const jbc::ir::ClassUnit& unit = session.unit();
    return unit.symbols().str(unit.methods[idx].name);
}

std::string_view fieldName(const jbc::ir::ClassSession& session, ca::usize idx)
{
    const jbc::ir::ClassUnit& unit = session.unit();
    return unit.symbols().str(unit.fields[idx].name);
}

}   // namespace

// 旧栈用例移植：字段按 desc→name 排序（b:I, a:I → a, b），产物可写出重读。
TEST(MjPassClassMemberSorterTest, SortsFieldsByDescThenNameRoundtrip)
{
    FixtureBuilder builder("T");
    builder.addField({"b", "I", 0x0000});
    builder.addField({"a", "I", 0x0000});
    builder.addMethod({"t", "()V", bareReturn()});
    auto bytes = builder.assemble();

    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    mjbase::pass::ClassMemberSorter    pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::ClassAnalysisManager  cam(arena);
    const auto                         pa = pass.run(*session, cam);

    ASSERT_EQ(session->unit().fields.size(), 2u);
    EXPECT_EQ(fieldName(*session, 0), "a");
    EXPECT_EQ(fieldName(*session, 1), "b");
    EXPECT_TRUE(pa.changed());

    // roundtrip：重排落进 backing，写出可重读且顺序保持。
    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    auto reread = jbc::core::readClass(jbc::core::ByteView(written.unwrap()));
    ASSERT_TRUE(reread.is_ok());
    jbc::core::ReadResult reparsed = std::move(reread).unwrap();
    const jbc::core::ClassFile& cf = *reparsed.file;
    ASSERT_EQ(cf.fields.size(), 2u);
    EXPECT_EQ(cf.nameOf(cf.fields[0]), "a");
    EXPECT_EQ(cf.nameOf(cf.fields[1]), "b");
}

// 补齐直接用例：方法分组序 —— <clinit> 最前、其余按名、<init> 垫底。
TEST(MjPassClassMemberSorterTest, SortsMethodsClinitPlainThenInits)
{
    FixtureBuilder builder("T");
    builder.addMethod({"t2", "()V", bareReturn()});
    builder.addMethod({"zz", "()V", bareReturn()});
    builder.addMethod({"<init>", "()V", bareReturn()});
    builder.addMethod({"t1", "()V", bareReturn()});
    builder.addMethod({"<clinit>", "()V", bareReturn()});
    builder.addMethod({"aa", "()V", bareReturn()});
    auto bytes = builder.assemble();

    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    mjbase::pass::ClassMemberSorter    pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::ClassAnalysisManager  cam(arena);
    const auto                         pa = pass.run(*session, cam);

    EXPECT_TRUE(pa.changed());
    ASSERT_EQ(session->unit().methods.size(), 6u);
    EXPECT_EQ(methodName(*session, 0), "<clinit>");
    EXPECT_EQ(methodName(*session, 1), "aa");
    EXPECT_EQ(methodName(*session, 2), "t1");
    EXPECT_EQ(methodName(*session, 3), "t2");
    EXPECT_EQ(methodName(*session, 4), "zz");
    EXPECT_EQ(methodName(*session, 5), "<init>");
}

// 补齐直接用例：字段主键是 desc（"I" < "J"），同 desc 内才按 name。
TEST(MjPassClassMemberSorterTest, FieldDescIsPrimarySortKey)
{
    FixtureBuilder builder("T");
    builder.addField({"c", "J", 0x0000});
    builder.addField({"a", "I", 0x0000});
    builder.addField({"b", "I", 0x0000});
    builder.addMethod({"t", "()V", bareReturn()});
    auto bytes = builder.assemble();

    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    mjbase::pass::ClassMemberSorter    pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::ClassAnalysisManager  cam(arena);
    const auto                         pa = pass.run(*session, cam);

    EXPECT_TRUE(pa.changed());
    ASSERT_EQ(session->unit().fields.size(), 3u);
    EXPECT_EQ(fieldName(*session, 0), "a");
    EXPECT_EQ(fieldName(*session, 1), "b");
    EXPECT_EQ(fieldName(*session, 2), "c");
}

// 补齐直接用例：已序输入 → 不发生排序，PA all()（条件 PA 语义，与旧实现的
// 无条件 none() 偏离已在头注声明）。
TEST(MjPassClassMemberSorterTest, ReturnsAllWhenAlreadySorted)
{
    FixtureBuilder builder("T");
    builder.addField({"a", "I", 0x0000});
    builder.addField({"b", "I", 0x0000});
    builder.addMethod({"<clinit>", "()V", bareReturn()});
    builder.addMethod({"t", "()V", bareReturn()});
    builder.addMethod({"<init>", "()V", bareReturn()});
    auto bytes = builder.assemble();

    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    mjbase::pass::ClassMemberSorter    pass;
    ca::str::Utf8StringArena           arena;
    mjbase::spi::ClassAnalysisManager  cam(arena);
    const auto                         pa = pass.run(*session, cam);

    EXPECT_TRUE(pa.areAllPreserved());
    EXPECT_FALSE(pa.changed());
}

// 补齐直接用例（新底座关键契约）：重排后 MethodUnit.backingIndex 必须改指
// 新物理位——重排前 promote 的方法体在重排后 lower，必须写回自己（而非别人）
// 的物理成员，未动方法的 Code 原样保真。
TEST(MjPassClassMemberSorterTest, LowerAfterReorderWritesOwnBackingSlot)
{
    FixtureBuilder builder("T");
    // zz 在物理位 0（排完序后必被挪走），aa 在物理位 1。
    builder.addMethod({"zz", "()V", bareReturn()});
    builder.addMethod({"aa", "()V", nopReturn()});
    auto bytes = builder.assemble();

    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    // 重排前 promote 物理位 0（zz），并持稳其方法身份。
    const jbc::ir::MethodId zzId = session->unit().methods[0].id;
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    ASSERT_TRUE(promoted.is_ok());
    jbc::ir::CodeUnitRef zzCode = std::move(promoted).unwrap();

    // 排序：aa（nopReturn）应到物理位 0，zz 退到物理位 1。
    mjbase::pass::ClassMemberSorter   pass;
    ca::str::Utf8StringArena          arena;
    mjbase::spi::ClassAnalysisManager cam(arena);
    const auto                        pa = pass.run(*session, cam);
    EXPECT_TRUE(pa.changed());
    ASSERT_EQ(methodName(*session, 0), "aa");
    ASSERT_EQ(methodName(*session, 1), "zz");
    EXPECT_EQ(session->unit().methods[1].id.slot, zzId.slot);

    // 修改 zz 的已 promote 方法体（追加 NOP），按稳定身份 lower。
    jbc::ir::CodeEditor editor(*zzCode);
    auto                appended = editor.append(jbc::ir::Insn{});
    ASSERT_TRUE(appended.is_ok());
    auto lowered = jbc::ir::lowerMethod(*session, zzId);
    ASSERT_TRUE(lowered.is_ok());

    // 写出重读：aa 的 Code 原样保真（未被 zz 的 lower 污染），
    // zz 的 Code 恰好多出一个 NOP。
    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    auto reread = jbc::core::readClass(jbc::core::ByteView(written.unwrap()));
    ASSERT_TRUE(reread.is_ok());
    jbc::core::ReadResult reparsed = std::move(reread).unwrap();
    jbc::core::ClassFile& cf       = *reparsed.file;
    ASSERT_EQ(cf.methods.size(), 2u);
    const jbc::core::MemberInfo* aaMember = nullptr;
    const jbc::core::MemberInfo* zzMember = nullptr;
    for (const jbc::core::MemberInfo& m : cf.methods) {
        if (cf.nameOf(m) == "aa") aaMember = &m;
        if (cf.nameOf(m) == "zz") zzMember = &m;
    }
    ASSERT_NE(aaMember, nullptr);
    ASSERT_NE(zzMember, nullptr);

    auto decodeCode = [&cf](const jbc::core::MemberInfo& m) -> const jbc::core::CodeAttr* {
        for (const jbc::core::AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
            if (!slot.decoded()) {
                if (cf.decodeAttr(const_cast<jbc::core::AttrSlot&>(slot)).is_err()) return nullptr;
            }
            return ca::core::tag_cast::cast<const jbc::core::CodeAttr>(slot.peek());
        }
        return nullptr;
    };
    const jbc::core::CodeAttr* aaCode = decodeCode(*aaMember);
    const jbc::core::CodeAttr* zzCodeBack = decodeCode(*zzMember);
    ASSERT_NE(aaCode, nullptr);
    ASSERT_NE(zzCodeBack, nullptr);
    EXPECT_EQ(aaCode->code.size(), 2u);   // NOP; RETURN 原样
    EXPECT_EQ(zzCodeBack->code.size(), 2u);   // RETURN + 追加的 NOP
}
