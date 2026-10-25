/// 静态字段默认值内联（scope:class 首件）单测：mbase 治理版语义对齐
/// （单次写内联 / 多次写保守跳过 / owner 校验）+ 新载体特有面
/// （ConstantValue RawAttr 槽与 CP 值条目类型、clinit 指令重编码字节、
/// 无操作路径 writeClass 字节恒等、注册面 scope:class 种子）。
/// 字节模式经 FixtureBuilder 手拼（lconst/dconst/fconst 不进 CP，夹具
/// 无双槽条目）。
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/pass/static_field_default_inliner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

using jbc::core::AttrKind;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::CpTag;
using jbc::core::MemberInfo;
using jbc::core::RawAttr;

using Bytes = std::vector<ca::u8>;

/// 解码后的 ConstantValue 视图：值条目 tag + 数值/位图。
struct CvView
{
    CpTag tag = CpTag::Integer;
    ca::i64 num  = 0;
    ca::u64 bits = 0;
};

/// 方法 Code 属性（无 Code / 解码失败为空）。
const CodeAttr* codeAttr(ClassFile& cf, MemberInfo& m)
{
    for (AttrSlot& slot : m.attrs) {
        if (cf.utf8(slot.nameCp) != "Code") continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return nullptr;
        if (std::move(d).unwrap()->kind() != AttrKind::Code) return nullptr;
        return ca::core::tag_cast::cast<const CodeAttr*>(slot.peek());
    }
    return nullptr;
}

MemberInfo* findMethod(ClassFile& cf, std::string_view name)
{
    for (MemberInfo& m : cf.methods) {
        if (cf.utf8(m.nameCp) == name) return &m;
    }
    return nullptr;
}

MemberInfo* findField(ClassFile& cf, std::string_view name)
{
    for (MemberInfo& f : cf.fields) {
        if (cf.utf8(f.nameCp) == name) return &f;
    }
    return nullptr;
}

/// 字段 ConstantValue 解码视图；无槽/载荷非法返回 nullopt。
std::optional<CvView> constantValueOf(ClassFile& cf, MemberInfo& f)
{
    for (AttrSlot& slot : f.attrs) {
        if (cf.utf8(slot.nameCp) != "ConstantValue") continue;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) return std::nullopt;
        if (std::move(d).unwrap()->kind() != AttrKind::Raw) return std::nullopt;
        const auto* raw = ca::core::tag_cast::cast<const RawAttr*>(slot.peek());
        if (raw == nullptr || raw->bytes().size() != 2) return std::nullopt;
        const ca::u32 idx = (static_cast<ca::u32>(raw->bytes()[0]) << 8) |
                            static_cast<ca::u32>(raw->bytes()[1]);
        const jbc::core::ConstantPool::Entry e = cf.cp.at(idx);
        switch (e.tag) {
        case CpTag::Integer: return CvView{CpTag::Integer, e.ival, 0};
        case CpTag::Float:   return CvView{CpTag::Float, 0, static_cast<ca::u64>(static_cast<ca::u32>(e.ival))};
        case CpTag::Long:    return CvView{CpTag::Long, e.lval, 0};
        case CpTag::Double:  return CvView{CpTag::Double, 0, static_cast<ca::u64>(e.lval)};
        default:             return std::nullopt;
        }
    }
    return std::nullopt;
}

/// clinit 拼装：常量装载 + putstatic 序列 + return。
Bytes clinitCode(FixtureBuilder& b, std::string_view owner)
{
    Bytes code;
    putU8(code, 0x08);                                        // iconst_5
    putU8(code, 0xB3); putU16(code, b.fieldRef(owner, "a", "I"));
    putU8(code, 0x10); putU8(code, 100);                      // bipush 100
    putU8(code, 0xB3); putU16(code, b.fieldRef(owner, "ab", "I"));
    putU8(code, 0x11); putU16(code, 30000);                   // sipush 30000
    putU8(code, 0xB3); putU16(code, b.fieldRef(owner, "cd", "I"));
    putU8(code, 0x0A);                                        // lconst_1
    putU8(code, 0xB3); putU16(code, b.fieldRef(owner, "e", "J"));
    putU8(code, 0x0F);                                        // dconst_1
    putU8(code, 0xB3); putU16(code, b.fieldRef(owner, "f", "D"));
    putU8(code, 0x0D);                                        // fconst_2
    putU8(code, 0xB3); putU16(code, b.fieldRef(owner, "g", "F"));
    putU8(code, 0xB1);                                        // return
    return code;
}

/// 组会话（按值收 builder，CP 物化在 assemble 内完成）。
std::shared_ptr<jbc::ir::ClassSession> makeSession(FixtureBuilder builder)
{
    const Bytes bytes = builder.assemble();
    auto        result = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(result.is_ok());
    return std::move(result).unwrap();
}

/// 全字段候选夹具：七字段（I×3 + J/D/F 各一），clinit 逐字段单写。
FixtureBuilder sevenFieldFixture()
{
    FixtureBuilder b("T");
    b.addField({"a", "I", 0x0009});
    b.addField({"ab", "I", 0x0009});
    b.addField({"cd", "I", 0x0009});
    b.addField({"e", "J", 0x0009});
    b.addField({"f", "D", 0x0009});
    b.addField({"g", "F", 0x0009});
    b.addMethod({"<clinit>", "()V", clinitCode(b, "T"), 0x0008});
    return b;
}

/// 跑一遍 pass（arena/cam 组装对齐全仓 class pass 用例）。
mjbase::spi::PreservedAnalyses runPass(jbc::ir::ClassSession& session)
{
    ca::str::Utf8StringArena                  arena;
    mjbase::spi::ClassAnalysisManager         cam(arena);
    mjbase::pass::StaticFieldDefaultInliner   pass;
    return pass.run(session, cam);
}

}   // namespace

// ── 内联主路径 ──

TEST(MjPassStaticFieldDefaultInlinerTest, InlinesAllFourKindsAndIntForms)
{
    auto session = makeSession(sevenFieldFixture());
    const auto pa = runPass(*session);
    EXPECT_TRUE(pa.changed());

    ClassFile& cf = session->backing();
    const auto a = constantValueOf(cf, *findField(cf, "a"));
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->tag, CpTag::Integer);
    EXPECT_EQ(a->num, 5);
    const auto ab = constantValueOf(cf, *findField(cf, "ab"));
    ASSERT_TRUE(ab.has_value());
    EXPECT_EQ(ab->num, 100);                                  // bipush
    const auto cd = constantValueOf(cf, *findField(cf, "cd"));
    ASSERT_TRUE(cd.has_value());
    EXPECT_EQ(cd->num, 30000);                                // sipush
    const auto e = constantValueOf(cf, *findField(cf, "e"));
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->tag, CpTag::Long);
    EXPECT_EQ(e->num, 1);
    const auto f = constantValueOf(cf, *findField(cf, "f"));
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->tag, CpTag::Double);
    EXPECT_EQ(f->bits, 0x3FF0000000000000ULL);
    const auto g = constantValueOf(cf, *findField(cf, "g"));
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->tag, CpTag::Float);
    EXPECT_EQ(g->bits, 0x40000000ULL);                        // 2.0f

    // 指令面：六对装载+putstatic 全删，clinit 只剩 return。
    const CodeAttr* code = codeAttr(cf, *findMethod(cf, "<clinit>"));
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(code->code.size(), 1u);
    EXPECT_EQ(code->code[0], 0xB1);
}

TEST(MjPassStaticFieldDefaultInlinerTest, InlinesLdcWideInt)
{
    FixtureBuilder b("T");
    b.addField({"big", "I", 0x0009});
    Bytes code;
    putU8(code, 0x12); putU8(code, static_cast<ca::u8>(b.intConst(100000)));   // ldc
    putU8(code, 0xB3); putU16(code, b.fieldRef("T", "big", "I"));
    putU8(code, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(code), 0x0008});
    auto session = makeSession(std::move(b));
    EXPECT_TRUE(runPass(*session).changed());

    ClassFile& cf = session->backing();
    const auto cv = constantValueOf(cf, *findField(cf, "big"));
    ASSERT_TRUE(cv.has_value());
    EXPECT_EQ(cv->tag, CpTag::Integer);
    EXPECT_EQ(cv->num, 100000);
}

// ── 保守跳过面 ──

TEST(MjPassStaticFieldDefaultInlinerTest, SkipsMultipleWritesByteIdentical)
{
    FixtureBuilder b("T");
    b.addField({"a", "I", 0x0009});
    Bytes code;
    putU8(code, 0x04);                                        // iconst_1
    putU8(code, 0xB3); putU16(code, b.fieldRef("T", "a", "I"));
    putU8(code, 0x05);                                        // iconst_2
    putU8(code, 0xB3); putU16(code, b.fieldRef("T", "a", "I"));
    putU8(code, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(code), 0x0008});
    auto session = makeSession(std::move(b));

    auto before = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(before.is_ok());
    const Bytes original = std::move(before).unwrap();

    EXPECT_TRUE(runPass(*session).areAllPreserved());

    auto after = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(after.unwrap(), original) << "多次写路径不得有字节面改动";
    EXPECT_FALSE(constantValueOf(session->backing(), *findField(session->backing(), "a")).has_value());
}

TEST(MjPassStaticFieldDefaultInlinerTest, SkipsExistingConstantValue)
{
    FixtureBuilder b("T");
    b.addField({"a", "I", 0x0019, {{"ConstantValue", [&b] {
        Bytes p;
        putU16(p, b.intConst(42));
        return p;
    }()}}});
    Bytes code;
    putU8(code, 0x08);
    putU8(code, 0xB3); putU16(code, b.fieldRef("T", "a", "I"));
    putU8(code, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(code), 0x0008});
    auto session = makeSession(std::move(b));

    EXPECT_TRUE(runPass(*session).areAllPreserved());
    const auto cv = constantValueOf(session->backing(), *findField(session->backing(), "a"));
    ASSERT_TRUE(cv.has_value());
    EXPECT_EQ(cv->num, 42) << "既有 ConstantValue 不得被改写";
}

TEST(MjPassStaticFieldDefaultInlinerTest, SkipsNonStaticFieldAndForeignOwner)
{
    FixtureBuilder b("T");
    b.addField({"a", "I", 0x0001});                           // 非 static
    b.addField({"st", "I", 0x0009});
    Bytes code;
    putU8(code, 0x08);
    putU8(code, 0xB3); putU16(code, b.fieldRef("T", "a", "I"));
    putU8(code, 0x07);                                        // iconst_4
    putU8(code, 0xB3); putU16(code, b.fieldRef("Other", "st", "I"));
    putU8(code, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(code), 0x0008});
    auto session = makeSession(std::move(b));

    EXPECT_TRUE(runPass(*session).areAllPreserved());
    ClassFile& cf = session->backing();
    EXPECT_FALSE(constantValueOf(cf, *findField(cf, "a")).has_value());
    EXPECT_FALSE(constantValueOf(cf, *findField(cf, "st")).has_value());
}

TEST(MjPassStaticFieldDefaultInlinerTest, SkipsNonConstantProducer)
{
    FixtureBuilder b("T");
    b.addField({"a", "I", 0x0009});
    Bytes code;
    putU8(code, 0x1A);                                        // iload_0（非常量）
    putU8(code, 0xB3); putU16(code, b.fieldRef("T", "a", "I"));
    putU8(code, 0xB1);
    b.addMethod({"<clinit>", "()V", std::move(code), 0x0008, {}, 1, 1});
    auto session = makeSession(std::move(b));

    EXPECT_TRUE(runPass(*session).areAllPreserved());
    EXPECT_FALSE(constantValueOf(session->backing(), *findField(session->backing(), "a")).has_value());
}

TEST(MjPassStaticFieldDefaultInlinerTest, NoClinitIsNoop)
{
    FixtureBuilder b("T");
    b.addField({"a", "I", 0x0009});
    b.addMethod({"m", "()V", Bytes{0xB1}, 0x0009});
    auto session = makeSession(std::move(b));

    EXPECT_TRUE(runPass(*session).areAllPreserved());
    EXPECT_FALSE(constantValueOf(session->backing(), *findField(session->backing(), "a")).has_value());
}

// ── 注册面 ──

TEST(MjPassStaticFieldDefaultInlinerTest, RegistrySeedsClassScope)
{
    const auto found = mjbase::spi::defaultPassRegistry().find("StaticFieldDefaultInliner");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, mjbase::spi::PassScope::Class);
}
