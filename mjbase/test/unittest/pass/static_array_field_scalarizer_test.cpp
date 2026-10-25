/// 静态数组字段标量化（#39 第一切片）单测：候选识别与拆分主链
/// （roundtrip 语义断言：改写 → writeClass → 重读会话 → 指令序列与
/// 字段表面验证），保守跳过面（非常量下标 / 整体引用 / arraylength /
/// 无分配 / 重赋 / 名冲突 / 非 private / 引用数组），注册面种子。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/pass/static_array_field_scalarizer.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

using Bytes = std::vector<ca::u8>;

/// 组会话（按值收 builder，CP 物化在 assemble 内完成）。
std::shared_ptr<jbc::ir::ClassSession> makeSession(FixtureBuilder builder)
{
    const Bytes bytes  = builder.assemble();
    auto        result = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(result.is_ok());
    return std::move(result).unwrap();
}

mjbase::spi::PreservedAnalyses runPass(jbc::ir::ClassSession& session)
{
    ca::str::Utf8StringArena               arena;
    mjbase::spi::ClassAnalysisManager      cam(arena);
    mjbase::pass::StaticArrayFieldScalarizer pass;
    return pass.run(session, cam);
}

/// 主链夹具：tbl int[3]（private static），clinit 分配 + 三写，
/// read()I 常量下标读一。返回 builder（调用方再补方法/字段变体）。
FixtureBuilder tableFixture()
{
    FixtureBuilder b("T");
    b.addField({"tbl", "[I", 0x000A});
    Bytes clinit;
    putU8(clinit, 0x06);                                          // iconst_3
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);                     // newarray int
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0xB2); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0x03); putU8(clinit, 0x10); putU8(clinit, 11);  // const_0; bipush 11
    putU8(clinit, 0x4F);                                          // iastore
    putU8(clinit, 0xB2); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0x04); putU8(clinit, 0x10); putU8(clinit, 22);
    putU8(clinit, 0x4F);
    putU8(clinit, 0xB2); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0x05); putU8(clinit, 0x10); putU8(clinit, 33);
    putU8(clinit, 0x4F);
    putU8(clinit, 0xB1);                                          // return
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 3, 0});
    Bytes read;
    putU8(read, 0xB2); putU16(read, b.fieldRef("T", "tbl", "[I"));
    putU8(read, 0x04);                                            // iconst_1
    putU8(read, 0x2E);                                            // iaload
    putU8(read, 0xAC);                                            // ireturn
    b.addMethod({"read", "()I", std::move(read), 0x0009, {}, 2, 0});
    return b;
}

/// 字段面断言工具。
const jbc::core::MemberInfo* findField(const jbc::core::ClassFile& cf, std::string_view name)
{
    for (const jbc::core::MemberInfo& f : cf.fields) {
        if (cf.utf8(f.nameCp) == name) return &f;
    }
    return nullptr;
}

const jbc::core::MemberInfo* findMethod(const jbc::core::ClassFile& cf, std::string_view name)
{
    for (const jbc::core::MemberInfo& m : cf.methods) {
        if (cf.utf8(m.nameCp) == name) return &m;
    }
    return nullptr;
}

/// roundtrip：改写后字节 → 新会话 → 方法指令序列的 (op, member-name) 视图。
struct InsnFact
{
    jbc::core::Op                 op;
    std::string                   member;   ///< invoke/get/put 目标名（无 CP 操作数为空）
};
std::vector<InsnFact> methodFacts(jbc::ir::ClassSession& session, std::string_view method)
{
    const ca::usize idx = session.unit().findMethod(method);
    EXPECT_NE(idx, jbc::ir::kNoMethod);
    auto promoted = jbc::ir::promoteMethod(session, idx);
    EXPECT_TRUE(promoted.is_ok());
    std::vector<InsnFact> facts;
    for (const jbc::ir::InsnId id : promoted.unwrap()->dom.order()) {
        const jbc::ir::Insn* in = promoted.unwrap()->dom.peek(id);
        EXPECT_NE(in, nullptr);
        jbc::ir::ClassSession::MemberSem ref;
        std::string member;
        if (session.memberOf(*in, ref)) member = std::string(ref.name);
        facts.push_back({in->op, std::move(member)});
    }
    return facts;
}

std::shared_ptr<jbc::ir::ClassSession> reRead(jbc::ir::ClassSession& session)
{
    auto written = jbc::core::writeClass(session.backing());
    EXPECT_TRUE(written.is_ok());
    auto re = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView(std::move(written).unwrap()));
    EXPECT_TRUE(re.is_ok());
    return std::move(re).unwrap();
}

}   // namespace

// ── 主链 ──

TEST(MjPassStaticArrayFieldScalarizerTest, ScalarizesConstantTable)
{
    auto session = makeSession(tableFixture());
    EXPECT_TRUE(runPass(*session).changed());

    auto re = reRead(*session);
    const jbc::core::ClassFile& cf = re->backing();
    EXPECT_EQ(findField(cf, "tbl"), nullptr) << "旧数组字段应删除";
    for (const char* name : {"tbl_0", "tbl_1", "tbl_2"}) {
        const jbc::core::MemberInfo* f = findField(cf, name);
        ASSERT_NE(f, nullptr) << name;
        EXPECT_EQ(cf.utf8(f->descCp), "I");
        EXPECT_EQ(f->access, 0x000Au) << "private static";
    }

    using jbc::core::Op;
    const auto clinit = methodFacts(*re, "<clinit>");
    ASSERT_EQ(clinit.size(), 7u);
    EXPECT_EQ(clinit[0].op, Op::BIPUSH);
    EXPECT_EQ(clinit[1].op, Op::PUTSTATIC);
    EXPECT_EQ(clinit[1].member, "tbl_0");
    EXPECT_EQ(clinit[2].op, Op::BIPUSH);
    EXPECT_EQ(clinit[3].op, Op::PUTSTATIC);
    EXPECT_EQ(clinit[3].member, "tbl_1");
    EXPECT_EQ(clinit[4].op, Op::BIPUSH);
    EXPECT_EQ(clinit[5].op, Op::PUTSTATIC);
    EXPECT_EQ(clinit[5].member, "tbl_2");
    EXPECT_EQ(clinit[6].op, Op::RETURN);

    const auto read = methodFacts(*re, "read");
    ASSERT_EQ(read.size(), 2u);
    EXPECT_EQ(read[0].op, Op::GETSTATIC);
    EXPECT_EQ(read[0].member, "tbl_1");
    EXPECT_EQ(read[1].op, Op::IRETURN);
}

TEST(MjPassStaticArrayFieldScalarizerTest, ScalarizesLongArrayWithLdcIndexAccess)
{
    FixtureBuilder b("T");
    b.addField({"lt", "[J", 0x000A});
    Bytes clinit;
    putU8(clinit, 0x05);                                          // iconst_2
    putU8(clinit, 0xBC); putU8(clinit, 0x0B);                     // newarray long
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "lt", "[J"));
    putU8(clinit, 0xB2); putU16(clinit, b.fieldRef("T", "lt", "[J"));
    putU8(clinit, 0x03); putU8(clinit, 0x0A); putU8(clinit, 0x50);   // const_0; lconst_1; lastore
    putU8(clinit, 0xB2); putU16(clinit, b.fieldRef("T", "lt", "[J"));
    putU8(clinit, 0x04); putU8(clinit, 0x09); putU8(clinit, 0x50);   // const_1; lconst_0; lastore
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 4, 0});
    auto session = makeSession(std::move(b));
    EXPECT_TRUE(runPass(*session).changed());

    auto re = reRead(*session);
    const jbc::core::ClassFile& cf = re->backing();
    EXPECT_EQ(findField(cf, "lt"), nullptr);
    ASSERT_NE(findField(cf, "lt_0"), nullptr);
    ASSERT_NE(findField(cf, "lt_1"), nullptr);
    EXPECT_EQ(cf.utf8(findField(cf, "lt_0")->descCp), "J");
}

TEST(MjPassStaticArrayFieldScalarizerTest, ScalarizesZeroInitializedRead)
{
    FixtureBuilder b("T");
    b.addField({"z", "[I", 0x000A});
    Bytes clinit;
    putU8(clinit, 0x04);                                          // iconst_1
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "z", "[I"));
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 2, 0});
    Bytes read;
    putU8(read, 0xB2); putU16(read, b.fieldRef("T", "z", "[I"));
    putU8(read, 0x03);
    putU8(read, 0x2E);
    putU8(read, 0xAC);
    b.addMethod({"read", "()I", std::move(read), 0x0009, {}, 2, 0});
    auto session = makeSession(std::move(b));
    EXPECT_TRUE(runPass(*session).changed());

    auto re = reRead(*session);
    const jbc::core::ClassFile& cf = re->backing();
    EXPECT_EQ(findField(cf, "z"), nullptr);
    const jbc::core::MemberInfo* f = findField(cf, "z_0");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(cf.utf8(f->descCp), "I");
    using jbc::core::Op;
    const auto facts = methodFacts(*re, "read");
    ASSERT_EQ(facts.size(), 2u);
    EXPECT_EQ(facts[0].op, Op::GETSTATIC);
    EXPECT_EQ(facts[0].member, "z_0");
}

// ── 保守跳过面（全部字节恒等）──

/// 通用骨架：build 夹具（含变体注入）→ 跑 pass → 断言 all() + 字节恒等。
void expectUntouched(FixtureBuilder builder)
{
    auto session = makeSession(std::move(builder));
    auto before  = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(before.is_ok());
    const Bytes original = std::move(before).unwrap();

    EXPECT_TRUE(runPass(*session).areAllPreserved());

    auto after = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(after.unwrap(), original);
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsNonConstantIndex)
{
    FixtureBuilder b("T");
    b.addField({"tbl", "[I", 0x000A});
    Bytes clinit;
    putU8(clinit, 0x04);
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 2, 0});
    Bytes read;                                                   // iload_0 下标（非常量）
    putU8(read, 0xB2); putU16(read, b.fieldRef("T", "tbl", "[I"));
    putU8(read, 0x1A);
    putU8(read, 0x2E);
    putU8(read, 0xAC);
    b.addMethod({"read", "(I)I", std::move(read), 0x0009, {}, 2, 1});
    expectUntouched(std::move(b));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsWholeArrayReference)
{
    FixtureBuilder b("T");
    b.addField({"tbl", "[I", 0x000A});
    Bytes clinit;
    putU8(clinit, 0x04);
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 2, 0});
    Bytes sink;                                                   // getstatic 整体传参
    putU8(sink, 0xB2); putU16(sink, b.fieldRef("T", "tbl", "[I"));
    putU8(sink, 0xB8); putU16(sink, b.methodRef("Other", "s", "([I)V"));
    putU8(sink, 0xB1);
    b.addMethod({"sink", "()V", std::move(sink), 0x0009, {}, 1, 0});
    expectUntouched(std::move(b));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsArraylengthUse)
{
    FixtureBuilder b("T");
    b.addField({"tbl", "[I", 0x000A});
    Bytes clinit;
    putU8(clinit, 0x04);
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 2, 0});
    Bytes len;
    putU8(len, 0xB2); putU16(len, b.fieldRef("T", "tbl", "[I"));
    putU8(len, 0xBE);                                             // arraylength
    putU8(len, 0xAC);
    b.addMethod({"len", "()I", std::move(len), 0x0009, {}, 1, 0});
    expectUntouched(std::move(b));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsReadWithoutAllocation)
{
    FixtureBuilder b("T");
    b.addField({"tbl", "[I", 0x000A});                            // 无分配：null 数组语义
    Bytes read;
    putU8(read, 0xB2); putU16(read, b.fieldRef("T", "tbl", "[I"));
    putU8(read, 0x03);
    putU8(read, 0x2E);
    putU8(read, 0xAC);
    b.addMethod({"read", "()I", std::move(read), 0x0009, {}, 2, 0});
    expectUntouched(std::move(b));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsWholeReassignment)
{
    FixtureBuilder b("T");
    b.addField({"tbl", "[I", 0x000A});
    Bytes clinit;
    putU8(clinit, 0x04);                                          // 分配一
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0x04);                                          // 重赋第二个数组
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 2, 0});
    expectUntouched(std::move(b));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsScalarNameCollision)
{
    FixtureBuilder b("T");
    b.addField({"tbl", "[I", 0x000A});
    b.addField({"tbl_0", "I", 0x000A});                           // 新名已被占用
    Bytes clinit;
    putU8(clinit, 0x04);
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0xB2); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0x03);
    putU8(clinit, 0x2E);
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 2, 0});
    expectUntouched(std::move(b));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsNonPrivateField)
{
    FixtureBuilder b("T");
    b.addField({"tbl", "[I", 0x0009});                            // package-private static
    Bytes clinit;
    putU8(clinit, 0x04);
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "tbl", "[I"));
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 2, 0});
    expectUntouched(std::move(b));
}

TEST(MjPassStaticArrayFieldScalarizerTest, ScalarizesStringArrayKeepCheckcast)
{
    FixtureBuilder b("T");
    b.addField({"names", "[Ljava/lang/String;", 0x000A});
    const ca::u16 strCls = b.cls("java/lang/String");
    const ca::u16 s0     = b.strConst("alpha");
    const ca::u16 s1     = b.strConst("beta");
    Bytes clinit;
    putU8(clinit, 0x05);                                          // iconst_2
    putU8(clinit, 0xBD); putU16(clinit, strCls);                  // anewarray String
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "names", "[Ljava/lang/String;"));
    putU8(clinit, 0xB2); putU16(clinit, b.fieldRef("T", "names", "[Ljava/lang/String;"));
    putU8(clinit, 0x03); putU8(clinit, 0x12); putU8(clinit, static_cast<ca::u8>(s0));
    putU8(clinit, 0x53);                                          // aastore
    putU8(clinit, 0xB2); putU16(clinit, b.fieldRef("T", "names", "[Ljava/lang/String;"));
    putU8(clinit, 0x04); putU8(clinit, 0x12); putU8(clinit, static_cast<ca::u8>(s1));
    putU8(clinit, 0x53);
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 4, 0});
    Bytes read;                                                   // 读 + checkcast（保留）
    putU8(read, 0xB2); putU16(read, b.fieldRef("T", "names", "[Ljava/lang/String;"));
    putU8(read, 0x03);
    putU8(read, 0x32);                                            // aaload
    putU8(read, 0xC0); putU16(read, strCls);                      // checkcast
    putU8(read, 0xB0);                                            // areturn
    b.addMethod({"read", "()Ljava/lang/String;", std::move(read), 0x0009, {}, 2, 0});
    auto session = makeSession(std::move(b));
    EXPECT_TRUE(runPass(*session).changed());

    auto re = reRead(*session);
    const jbc::core::ClassFile& cf = re->backing();
    EXPECT_EQ(findField(cf, "names"), nullptr);
    for (const char* n : {"names_0", "names_1"}) {
        const jbc::core::MemberInfo* f = findField(cf, n);
        ASSERT_NE(f, nullptr) << n;
        EXPECT_EQ(cf.utf8(f->descCp), "Ljava/lang/String;");
    }
    using jbc::core::Op;
    const auto readFacts = methodFacts(*re, "read");
    ASSERT_EQ(readFacts.size(), 3u) << "checkcast 应保留在 getstatic 之后";
    EXPECT_EQ(readFacts[0].op, Op::GETSTATIC);
    EXPECT_EQ(readFacts[0].member, "names_0");
    EXPECT_EQ(readFacts[1].op, Op::CHECKCAST);
    EXPECT_EQ(readFacts[2].op, Op::ARETURN);
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsMultiDimensionalArray)
{
    FixtureBuilder b("T");
    b.addField({"grid", "[[I", 0x000A});                          // 多维：元素面不支持
    Bytes clinit;
    putU8(clinit, 0x04);
    putU8(clinit, 0xBD); putU16(clinit, b.cls("[I"));
    putU8(clinit, 0xB3); putU16(clinit, b.fieldRef("T", "grid", "[[I"));
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 1, 0});
    expectUntouched(std::move(b));
}

// ── 注册面 ──

TEST(MjPassStaticArrayFieldScalarizerTest, RegistrySeedsClassScope)
{
    const auto found = mjbase::spi::defaultPassRegistry().find("StaticArrayFieldScalarizer");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, mjbase::spi::PassScope::Class);
}

// ── 容器级（ResourceContainer + GlobalToClassPassAdaptor，多类混合）──

namespace {

using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;

void addClassResource(ResourceContainer& container, const std::string& uri, const Bytes& bytes)
{
    container.addResource(uri, std::make_unique<ClassResource>(uri, bytes));
}

Bytes resourceBytes(ResourceContainer& container, const std::string& uri)
{
    const auto* resource = container.getResource(uri);
    EXPECT_NE(resource, nullptr) << uri;
    const auto* cls = ca::core::tag_cast::cast<const ClassResource*>(resource);
    EXPECT_NE(cls, nullptr) << uri;
    return cls != nullptr ? cls->get() : Bytes{};
}

/// ct/TableHost：两类数组字段混合（int 表 + String 表），clinit 分配与写，
/// 两个读方法（含 checkcast 面）。扩段后两字段都应拆分。
Bytes buildTableHost()
{
    FixtureBuilder b("ct/TableHost");
    b.addField({"tbl", "[I", 0x000A});
    b.addField({"names", "[Ljava/lang/String;", 0x000A});
    const ca::u16 frTbl = b.fieldRef("ct/TableHost", "tbl", "[I");
    const ca::u16 frNames = b.fieldRef("ct/TableHost", "names", "[Ljava/lang/String;");
    const ca::u16 strCls = b.cls("java/lang/String");
    const ca::u16 s0 = b.strConst("zero");
    const ca::u16 s1 = b.strConst("one");
    Bytes clinit;
    putU8(clinit, 0x06);                                          // iconst_3
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, frTbl);
    putU8(clinit, 0xB2); putU16(clinit, frTbl);
    putU8(clinit, 0x03); putU8(clinit, 0x10); putU8(clinit, 7);
    putU8(clinit, 0x4F);
    putU8(clinit, 0xB2); putU16(clinit, frTbl);
    putU8(clinit, 0x04); putU8(clinit, 0x10); putU8(clinit, 8);
    putU8(clinit, 0x4F);
    putU8(clinit, 0xB2); putU16(clinit, frTbl);
    putU8(clinit, 0x05); putU8(clinit, 0x10); putU8(clinit, 9);
    putU8(clinit, 0x4F);
    putU8(clinit, 0x05);                                          // iconst_2
    putU8(clinit, 0xBD); putU16(clinit, strCls);
    putU8(clinit, 0xB3); putU16(clinit, frNames);
    putU8(clinit, 0xB2); putU16(clinit, frNames);
    putU8(clinit, 0x03); putU8(clinit, 0x12); putU8(clinit, static_cast<ca::u8>(s0));
    putU8(clinit, 0x53);
    putU8(clinit, 0xB2); putU16(clinit, frNames);
    putU8(clinit, 0x04); putU8(clinit, 0x12); putU8(clinit, static_cast<ca::u8>(s1));
    putU8(clinit, 0x53);
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 4, 0});
    Bytes readInt;
    putU8(readInt, 0xB2); putU16(readInt, frTbl);
    putU8(readInt, 0x05);
    putU8(readInt, 0x2E);
    putU8(readInt, 0xAC);
    b.addMethod({"third", "()I", std::move(readInt), 0x0009, {}, 2, 0});
    Bytes readStr;                                                // 读 + checkcast（保留）
    putU8(readStr, 0xB2); putU16(readStr, frNames);
    putU8(readStr, 0x03);
    putU8(readStr, 0x32);
    putU8(readStr, 0xC0); putU16(readStr, strCls);
    putU8(readStr, 0xB0);
    b.addMethod({"firstName", "()Ljava/lang/String;", std::move(readStr), 0x0009, {}, 2, 0});
    return b.assemble();
}

/// ct/PublicTable：public static 数组（非 private）——应整类不动。
Bytes buildPublicTable()
{
    FixtureBuilder b("ct/PublicTable");
    b.addField({"open", "[I", 0x0009});                           // public static
    const ca::u16 fr = b.fieldRef("ct/PublicTable", "open", "[I");
    Bytes clinit;
    putU8(clinit, 0x04);
    putU8(clinit, 0xBC); putU8(clinit, 0x0A);
    putU8(clinit, 0xB3); putU16(clinit, fr);
    putU8(clinit, 0xB2); putU16(clinit, fr);
    putU8(clinit, 0x03); putU8(clinit, 0x10); putU8(clinit, 1);
    putU8(clinit, 0x4F);
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 3, 0});
    return b.assemble();
}

/// ct/DynamicUser：private static 数组但下标来自参数（非常量）——应整类不动。
Bytes buildDynamicUser()
{
    FixtureBuilder b("ct/DynamicUser");
    b.addField({"vals", "[J", 0x000A});
    const ca::u16 fr = b.fieldRef("ct/DynamicUser", "vals", "[J");
    Bytes clinit;
    putU8(clinit, 0x04);
    putU8(clinit, 0xBC); putU8(clinit, 0x0B);
    putU8(clinit, 0xB3); putU16(clinit, fr);
    putU8(clinit, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(clinit), 0x0008, {}, 2, 0});
    Bytes pick;
    putU8(pick, 0xB2); putU16(pick, fr);
    putU8(pick, 0x1A);                                            // iload_0：非常量下标
    putU8(pick, 0x2F);                                            // laload
    putU8(pick, 0xAD);                                            // lreturn
    b.addMethod({"pick", "(I)J", std::move(pick), 0x0009, {}, 2, 1});
    return b.assemble();
}

/// ct/Plain：无候选字段——应整类不动。
Bytes buildPlain()
{
    FixtureBuilder b("ct/Plain");
    Bytes m;
    putU8(m, 0x03);
    putU8(m, 0xAC);
    b.addMethod({"answer", "()I", std::move(m), 0x0009, {}, 1, 0});
    return b.assemble();
}

}   // namespace

// 容器级主链：TableHost 两字段拆分并提交回容器，其余三类字节恒等。
TEST(MjPassStaticArrayFieldScalarizerContainerTest, ScalarizesTableHostAndLeavesOthers)
{
    ResourceContainer container;
    const Bytes host    = buildTableHost();
    const Bytes pub     = buildPublicTable();
    const Bytes dynamic = buildDynamicUser();
    const Bytes plain   = buildPlain();
    addClassResource(container, "ct/TableHost.class", host);
    addClassResource(container, "ct/PublicTable.class", pub);
    addClassResource(container, "ct/DynamicUser.class", dynamic);
    addClassResource(container, "ct/Plain.class", plain);

    mjbase::spi::ClassPassManager cpm;
    cpm.addPass(std::make_unique<mjbase::pass::StaticArrayFieldScalarizer>());
    ca::str::Utf8StringArena            arena;
    mjbase::spi::GlobalAnalysisManager  gam(arena);
    mjbase::spi::GlobalToClassPassAdaptor adaptor(std::move(cpm));
    adaptor.run(container, gam);

    // 提交面：容器里的 TableHost 已是改写字节（commit 路径生效）。
    const Bytes hostAfter = resourceBytes(container, "ct/TableHost.class");
    EXPECT_NE(hostAfter, host);
    auto re = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(hostAfter));
    ASSERT_TRUE(re.is_ok());
    const jbc::core::ClassFile& cf = re.unwrap()->backing();
    EXPECT_EQ(findField(cf, "tbl"), nullptr);
    EXPECT_EQ(findField(cf, "names"), nullptr);
    for (const char* n : {"tbl_0", "tbl_1", "tbl_2"}) {
        const jbc::core::MemberInfo* f = findField(cf, n);
        ASSERT_NE(f, nullptr) << n;
        EXPECT_EQ(cf.utf8(f->descCp), "I");
        EXPECT_EQ(f->access, 0x000Au);
    }
    for (const char* n : {"names_0", "names_1"}) {
        const jbc::core::MemberInfo* f = findField(cf, n);
        ASSERT_NE(f, nullptr) << n;
        EXPECT_EQ(cf.utf8(f->descCp), "Ljava/lang/String;");
    }
    using jbc::core::Op;
    const auto third = methodFacts(*re.unwrap(), "third");
    ASSERT_EQ(third.size(), 2u);
    EXPECT_EQ(third[0].op, Op::GETSTATIC);
    EXPECT_EQ(third[0].member, "tbl_2");
    const auto firstName = methodFacts(*re.unwrap(), "firstName");
    ASSERT_EQ(firstName.size(), 3u) << "checkcast 保留";
    EXPECT_EQ(firstName[0].member, "names_0");
    EXPECT_EQ(firstName[1].op, Op::CHECKCAST);

    // 保守面：非 private / 非常量下标 / 无候选三类字节恒等。
    EXPECT_EQ(resourceBytes(container, "ct/PublicTable.class"), pub);
    EXPECT_EQ(resourceBytes(container, "ct/DynamicUser.class"), dynamic);
    EXPECT_EQ(resourceBytes(container, "ct/Plain.class"), plain);
}
