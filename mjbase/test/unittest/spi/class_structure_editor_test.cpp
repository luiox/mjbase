/// 类级结构编辑原语的直接用例：双面落盘一致性、backingIndex 全量修正、
/// 恒等排列零改动、非法置换拒绝、成员增删四操作（双面落盘 / 稳定身份 /
/// 失效账目 / 错误族收口）。夹具用升格后的共享 structural_pass_util（与
/// pass 域 sorter 用例共用）。
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/spi/class_structure_editor.hpp"
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

/// backing 侧按下标取方法内部名（writeClass 序列化源的直接观察面）。
std::string_view backingMethodName(const jbc::ir::ClassSession& session, ca::usize idx)
{
    return session.backing().nameOf(session.backing().methods[idx]);
}

std::string_view backingFieldName(const jbc::ir::ClassSession& session, ca::usize idx)
{
    return session.backing().nameOf(session.backing().fields[idx]);
}

/// 重读 class 字节里第 methodIdx 个方法的已解码 Code 属性（不可解析即失败）。
const jbc::core::CodeAttr* decodedCodeOf(jbc::core::ClassFile&           cf,
                                         const jbc::core::MemberInfo& member)
{
    for (const jbc::core::AttrSlot& slot : member.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
        if (!slot.decoded()) {
            if (cf.decodeAttr(const_cast<jbc::core::AttrSlot&>(slot)).is_err()) return nullptr;
        }
        return ca::core::tag_cast::cast<const jbc::core::CodeAttr>(slot.peek());
    }
    return nullptr;
}

/// 构造三字段（c:J / a:I / b:I）+ 三方法（m0/m1/m2）的会话。
std::shared_ptr<jbc::ir::ClassSession> makeThreeMemberSession()
{
    FixtureBuilder builder("T");
    builder.addField({"c", "J", 0x0000});
    builder.addField({"a", "I", 0x0000});
    builder.addField({"b", "I", 0x0000});
    builder.addMethod({"m0", "()V", bareReturn()});
    builder.addMethod({"m1", "()V", nopReturn()});
    builder.addMethod({"m2", "()V", bareReturn()});
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(builder.assemble()));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return nullptr;
    return std::move(sessionResult).unwrap();
}

}   // namespace

// 双面落盘：unit 与 backing 两面同排列，内容（desc/access）随成员整体移动。
TEST(ClassStructureEditorTest, ReorderFieldsSyncsUnitAndBacking)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    // order[newPos] = oldPos：a(1), b(2), c(0)。
    const auto result = mjbase::spi::reorderClassFields(*session, {1, 2, 0});
    ASSERT_TRUE(result.is_ok());

    ASSERT_EQ(session->unit().fields.size(), 3u);
    EXPECT_EQ(fieldName(*session, 0), "a");
    EXPECT_EQ(fieldName(*session, 1), "b");
    EXPECT_EQ(fieldName(*session, 2), "c");
    // desc 随成员走（a:I / b:I / c:J），两面同序。
    const jbc::ir::ClassUnit& unit = session->unit();
    EXPECT_EQ(unit.symbols().str(unit.fields[0].desc), "I");
    EXPECT_EQ(unit.symbols().str(unit.fields[2].desc), "J");
    ASSERT_EQ(session->backing().fields.size(), 3u);
    EXPECT_EQ(backingFieldName(*session, 0), "a");
    EXPECT_EQ(backingFieldName(*session, 1), "b");
    EXPECT_EQ(backingFieldName(*session, 2), "c");
}

// §1 backingIndex 义务：重排后全部 MethodUnit.backingIndex 改指新物理位，
// 重排前 promote 的方法体按稳定身份 lower 写回自己的槽位，不污染他人。
TEST(ClassStructureEditorTest, ReorderMethodsFixesBackingIndex)
{
    FixtureBuilder builder("T");
    builder.addMethod({"zz", "()V", bareReturn()});   // 物理位 0，排完必被挪走
    builder.addMethod({"aa", "()V", nopReturn()});    // 物理位 1
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(builder.assemble()));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    // 重排前 promote 物理位 0（zz），持稳其身份。
    const jbc::ir::MethodId zzId = session->unit().methods[0].id;
    auto promoted = jbc::ir::promoteMethod(*session, 0);
    ASSERT_TRUE(promoted.is_ok());
    jbc::ir::CodeUnitRef zzCode = std::move(promoted).unwrap();

    const auto result = mjbase::spi::reorderClassMethods(*session, {1, 0});
    ASSERT_TRUE(result.is_ok());

    // 逻辑序与 backing 序一致（aa, zz），身份随成员走。
    EXPECT_EQ(methodName(*session, 0), "aa");
    EXPECT_EQ(methodName(*session, 1), "zz");
    EXPECT_EQ(backingMethodName(*session, 0), "aa");
    EXPECT_EQ(backingMethodName(*session, 1), "zz");
    ASSERT_EQ(session->unit().methods.size(), 2u);
    EXPECT_EQ(session->unit().methods[0].backingIndex, 0u);
    EXPECT_EQ(session->unit().methods[1].backingIndex, 1u);
    EXPECT_EQ(session->unit().methods[1].id.slot, zzId.slot);

    // 改 zz 的已 promote 方法体（追加 NOP），按稳定身份 lower。
    jbc::ir::CodeEditor editor(*zzCode);
    auto                appended = editor.append(jbc::ir::Insn{});
    ASSERT_TRUE(appended.is_ok());
    auto lowered = jbc::ir::lowerMethod(*session, zzId);
    ASSERT_TRUE(lowered.is_ok());

    // 写出重读：aa 的 Code 原样（2 字节），zz 恰好多一个 NOP（2 字节）。
    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    auto reread = jbc::core::readClass(jbc::core::ByteView(written.unwrap()));
    ASSERT_TRUE(reread.is_ok());
    jbc::core::ReadResult reparsed = std::move(reread).unwrap();
    jbc::core::ClassFile& cf       = *reparsed.file;
    ASSERT_EQ(cf.methods.size(), 2u);
    EXPECT_EQ(cf.nameOf(cf.methods[0]), "aa");
    const jbc::core::CodeAttr* aaCode = decodedCodeOf(cf, cf.methods[0]);
    ASSERT_NE(aaCode, nullptr);
    EXPECT_EQ(aaCode->code.size(), 2u);   // NOP; RETURN 原样保真
}

// 恒等排列零改动：Ok 返回且写回字节与重排前一致（不触碰懒解码属性槽）。
TEST(ClassStructureEditorTest, IdentityPermutationIsZeroDiff)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    auto before = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(before.is_ok());

    ASSERT_TRUE(mjbase::spi::reorderClassFields(*session, {0, 1, 2}).is_ok());
    ASSERT_TRUE(mjbase::spi::reorderClassMethods(*session, {0, 1, 2}).is_ok());

    EXPECT_EQ(fieldName(*session, 0), "c");
    EXPECT_EQ(fieldName(*session, 1), "a");
    EXPECT_EQ(fieldName(*session, 2), "b");
    EXPECT_EQ(methodName(*session, 0), "m0");
    EXPECT_EQ(methodName(*session, 1), "m1");
    EXPECT_EQ(methodName(*session, 2), "m2");

    auto after = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(std::move(before).unwrap(), std::move(after).unwrap());
}

// 非恒等重排：写回重读，序与预期一致（序列化源确实落了盘）。
TEST(ClassStructureEditorTest, NonIdentityReorderRoundtripsThroughBytes)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    // 方法重排 [2,0,1]：m2, m0, m1。
    ASSERT_TRUE(mjbase::spi::reorderClassMethods(*session, {2, 0, 1}).is_ok());
    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    auto reread = jbc::core::readClass(jbc::core::ByteView(written.unwrap()));
    ASSERT_TRUE(reread.is_ok());
    jbc::core::ReadResult reparsed = std::move(reread).unwrap();
    const jbc::core::ClassFile& cf = *reparsed.file;
    ASSERT_EQ(cf.methods.size(), 3u);
    EXPECT_EQ(cf.nameOf(cf.methods[0]), "m2");
    EXPECT_EQ(cf.nameOf(cf.methods[1]), "m0");
    EXPECT_EQ(cf.nameOf(cf.methods[2]), "m1");
}

// 非法置换拒绝：长度不符 / 重复 / 越界 → Err(BadPermutation)，会话保持原样。
TEST(ClassStructureEditorTest, BadPermutationRejectedLeavesSessionIntact)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    using mjbase::spi::StructureEditError;
    auto wrongLength = mjbase::spi::reorderClassFields(*session, {0, 1});
    ASSERT_TRUE(wrongLength.is_err());
    EXPECT_EQ(wrongLength.unwrap_err(), StructureEditError::BadPermutation);
    auto duplicated = mjbase::spi::reorderClassFields(*session, {0, 0, 2});
    ASSERT_TRUE(duplicated.is_err());
    EXPECT_EQ(duplicated.unwrap_err(), StructureEditError::BadPermutation);
    auto outOfRange = mjbase::spi::reorderClassMethods(*session, {0, 1, 3});
    ASSERT_TRUE(outOfRange.is_err());
    EXPECT_EQ(outOfRange.unwrap_err(), StructureEditError::BadPermutation);
    auto emptyOrder = mjbase::spi::reorderClassMethods(*session, {});
    ASSERT_TRUE(emptyOrder.is_err());
    EXPECT_EQ(emptyOrder.unwrap_err(), StructureEditError::BadPermutation);   // 空序对非空成员
    EXPECT_EQ(mjbase::spi::to_string(StructureEditError::BadPermutation),
              std::string_view("BadPermutation"));

    // 拒绝路径不落半面：unit 与 backing 均保持原序。
    EXPECT_EQ(fieldName(*session, 0), "c");
    EXPECT_EQ(methodName(*session, 0), "m0");
    EXPECT_EQ(backingMethodName(*session, 0), "m0");
}

// 空类边界：零成员类配空序是恒等排列，Ok 且零改动。
TEST(ClassStructureEditorTest, EmptyOrderOnEmptyClassIsOk)
{
    FixtureBuilder builder("T");
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(builder.assemble()));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();

    auto fields  = mjbase::spi::reorderClassFields(*session, {});
    auto methods = mjbase::spi::reorderClassMethods(*session, {});
    ASSERT_TRUE(fields.is_ok());
    ASSERT_TRUE(methods.is_ok());
    EXPECT_TRUE(session->unit().fields.empty());
    EXPECT_TRUE(session->unit().methods.empty());
}

// ── 成员增删四操作（libjbc A3/A4 通道）：双面落盘 / 稳定身份 / 失效账目 ──

// 新增字段：unit 与 backing 两面同落，desc/access 随种子，写回重读在场。
TEST(ClassStructureEditorTest, AddFieldLandsOnBothFaces)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    auto added = mjbase::spi::addClassField(*session, "x", "D", 0x0002);
    ASSERT_TRUE(added.is_ok());
    const jbc::ir::FieldId xId = added.unwrap();

    ASSERT_EQ(session->unit().fields.size(), 4u);
    EXPECT_EQ(fieldName(*session, 3), "x");
    const jbc::ir::ClassUnit& unit = session->unit();
    EXPECT_EQ(unit.symbols().str(unit.fields[3].desc), "D");
    EXPECT_EQ(unit.fields[3].access, 0x0002u);
    EXPECT_EQ(unit.fields[3].id, xId);
    EXPECT_EQ(backingFieldName(*session, 3), "x");

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    auto reread = jbc::core::readClass(jbc::core::ByteView(written.unwrap()));
    ASSERT_TRUE(reread.is_ok());
    jbc::core::ReadResult reparsed = std::move(reread).unwrap();
    jbc::core::ClassFile& cf       = *reparsed.file;
    ASSERT_EQ(cf.fields.size(), 4u);
    EXPECT_EQ(cf.nameOf(cf.fields[3]), "x");
    EXPECT_EQ(cf.descOf(cf.fields[3]), "D");
    EXPECT_EQ(cf.fields[3].access, 0x0002u);
}

// 新增方法：骨架原语不带 Code 属性；稳定身份可被按名查找命中。
TEST(ClassStructureEditorTest, AddMethodSkeletonWithoutCode)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    auto added = mjbase::spi::addClassMethod(*session, "tail", "(I)V", 0x0001);
    ASSERT_TRUE(added.is_ok());
    const jbc::ir::MethodId tailId = added.unwrap();

    ASSERT_EQ(session->unit().methods.size(), 4u);
    EXPECT_EQ(methodName(*session, 3), "tail");
    EXPECT_EQ(session->unit().methods[3].id, tailId);
    EXPECT_EQ(backingMethodName(*session, 3), "tail");
    // withCode=false：无 Code 属性（abstract 形态），hasCode 分流为 false。
    EXPECT_FALSE(jbc::ir::hasCode(*session, 3));
    // 稳定身份与按名查找互通。
    EXPECT_EQ(session->unit().findMethodId("tail", "(I)V"), tailId);

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    auto reread = jbc::core::readClass(jbc::core::ByteView(written.unwrap()));
    ASSERT_TRUE(reread.is_ok());
    jbc::core::ReadResult reparsed = std::move(reread).unwrap();
    jbc::core::ClassFile& cf       = *reparsed.file;
    ASSERT_EQ(cf.methods.size(), 4u);
    EXPECT_EQ(cf.nameOf(cf.methods[3]), "tail");
}

// 新增种子预检：空名 / access 超 u16 域 / 会话内 name+desc 重名 → Err(BadSeed)，
// 会话保持原样（预检先于写入，不留半截成员）。
TEST(ClassStructureEditorTest, AddSeedRejectedLeavesSessionIntact)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    using mjbase::spi::StructureEditError;
    auto emptyName = mjbase::spi::addClassField(*session, "", "I", 0x0000);
    ASSERT_TRUE(emptyName.is_err());
    EXPECT_EQ(emptyName.unwrap_err(), StructureEditError::BadSeed);

    auto wideAccess = mjbase::spi::addClassMethod(*session, "w", "()V", 0x00010000u);
    ASSERT_TRUE(wideAccess.is_err());
    EXPECT_EQ(wideAccess.unwrap_err(), StructureEditError::BadSeed);

    // 同 name+desc 重名（字段族内、方法族内各一）；跨族同名字段/方法合法。
    auto dupField = mjbase::spi::addClassField(*session, "c", "J", 0x0000);
    ASSERT_TRUE(dupField.is_err());
    EXPECT_EQ(dupField.unwrap_err(), StructureEditError::BadSeed);
    auto dupMethod = mjbase::spi::addClassMethod(*session, "m0", "()V", 0x0000);
    ASSERT_TRUE(dupMethod.is_err());
    EXPECT_EQ(dupMethod.unwrap_err(), StructureEditError::BadSeed);
    // 同名不同描述符合法（重载/同名字段异型）。
    auto sameNameOtherDesc = mjbase::spi::addClassField(*session, "c", "I", 0x0000);
    ASSERT_TRUE(sameNameOtherDesc.is_ok());

    EXPECT_EQ(session->unit().fields.size(), 4u);   // 仅同名字段异型挂入
    EXPECT_EQ(session->unit().methods.size(), 3u);
    EXPECT_EQ(fieldName(*session, 3), "c");
    EXPECT_EQ(backingMethodName(*session, 0), "m0");

    EXPECT_EQ(mjbase::spi::to_string(StructureEditError::BadSeed),
              std::string_view("BadSeed"));
}

// 删除字段：unit 与 backing 两面同步摘除，其余成员原样，写回重读自洽。
TEST(ClassStructureEditorTest, RemoveFieldSyncsBothFaces)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    const jbc::ir::FieldId aId = session->unit().fields[1].id;   // "a"
    auto                   removed = mjbase::spi::removeClassField(*session, 1);
    ASSERT_TRUE(removed.is_ok());

    ASSERT_EQ(session->unit().fields.size(), 2u);
    EXPECT_EQ(fieldName(*session, 0), "c");
    EXPECT_EQ(fieldName(*session, 1), "b");
    EXPECT_EQ(backingFieldName(*session, 0), "c");
    EXPECT_EQ(backingFieldName(*session, 1), "b");
    // 被删身份立即失配：查询面空值（不抛错）。
    EXPECT_EQ(session->unit().field(aId), nullptr);
    EXPECT_EQ(session->unit().indexOf(aId), jbc::ir::kNoField);

    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    auto reread = jbc::core::readClass(jbc::core::ByteView(written.unwrap()));
    ASSERT_TRUE(reread.is_ok());
    jbc::core::ReadResult reparsed = std::move(reread).unwrap();
    jbc::core::ClassFile& cf       = *reparsed.file;
    ASSERT_EQ(cf.fields.size(), 2u);
    EXPECT_EQ(cf.nameOf(cf.fields[0]), "c");
    EXPECT_EQ(cf.nameOf(cf.fields[1]), "b");
}

// 删除方法：身份退役（generation 自增，同槽复用携新代，旧句柄不复活）、
// 其余方法身份不动、backingIndex 物理位统一平移、CP 悬挂不回收（写回自洽）。
TEST(ClassStructureEditorTest, RemoveMethodRetiresIdentityAndShiftsBackingIndex)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    const jbc::ir::MethodId m0Id = session->unit().methods[0].id;
    const jbc::ir::MethodId m1Id = session->unit().methods[1].id;
    const jbc::ir::MethodId m2Id = session->unit().methods[2].id;

    auto removed = mjbase::spi::removeClassMethod(*session, 1);
    ASSERT_TRUE(removed.is_ok());

    ASSERT_EQ(session->unit().methods.size(), 2u);
    EXPECT_EQ(methodName(*session, 0), "m0");
    EXPECT_EQ(methodName(*session, 1), "m2");
    // 幸存者身份不动，物理位平移后重新自然一致。
    EXPECT_EQ(session->unit().methods[0].id, m0Id);
    EXPECT_EQ(session->unit().methods[1].id, m2Id);
    EXPECT_EQ(session->unit().methods[0].backingIndex, 0u);
    EXPECT_EQ(session->unit().methods[1].backingIndex, 1u);
    // 被删身份失配：查询面 nullptr / kNoMethod。
    EXPECT_EQ(session->unit().method(m1Id), nullptr);
    EXPECT_EQ(session->unit().indexOf(m1Id), jbc::ir::kNoMethod);

    // 槽位复用：新成员优先落已删槽，携自增 generation——旧句柄结构性失配。
    auto reAdded = mjbase::spi::addClassMethod(*session, "newM", "()V", 0x0000);
    ASSERT_TRUE(reAdded.is_ok());
    const jbc::ir::MethodId newId = reAdded.unwrap();
    EXPECT_EQ(newId.slot, m1Id.slot);
    EXPECT_EQ(newId.generation, m1Id.generation + 1);
    EXPECT_NE(newId, m1Id);
    EXPECT_EQ(session->unit().indexOf(m1Id), jbc::ir::kNoMethod);

    // CP 引用悬挂口径：删除不回收池条目，写回照常自洽（m1 的 Utf8 留池不炸）。
    auto written = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    auto reread = jbc::core::readClass(jbc::core::ByteView(written.unwrap()));
    ASSERT_TRUE(reread.is_ok());
    jbc::core::ReadResult reparsed = std::move(reread).unwrap();
    jbc::core::ClassFile& cf       = *reparsed.file;
    ASSERT_EQ(cf.methods.size(), 3u);
    EXPECT_EQ(cf.nameOf(cf.methods[0]), "m0");
    EXPECT_EQ(cf.nameOf(cf.methods[1]), "m2");
    EXPECT_EQ(cf.nameOf(cf.methods[2]), "newM");
}

// 删除预检：下标越界与重复删除 → Err(BadHandle)，会话保持原样。
TEST(ClassStructureEditorTest, RemoveBadHandleRejectedLeavesSessionIntact)
{
    auto session = makeThreeMemberSession();
    ASSERT_NE(session, nullptr);

    using mjbase::spi::StructureEditError;
    auto outOfRange = mjbase::spi::removeClassMethod(*session, 3);
    ASSERT_TRUE(outOfRange.is_err());
    EXPECT_EQ(outOfRange.unwrap_err(), StructureEditError::BadHandle);

    ASSERT_TRUE(mjbase::spi::removeClassField(*session, 2).is_ok());
    auto doubleRemove = mjbase::spi::removeClassField(*session, 2);
    ASSERT_TRUE(doubleRemove.is_err());
    EXPECT_EQ(doubleRemove.unwrap_err(), StructureEditError::BadHandle);

    // 拒绝路径不落半面。
    EXPECT_EQ(session->unit().fields.size(), 2u);
    EXPECT_EQ(session->unit().methods.size(), 3u);
    EXPECT_EQ(mjbase::spi::to_string(StructureEditError::BadHandle),
              std::string_view("BadHandle"));
}
