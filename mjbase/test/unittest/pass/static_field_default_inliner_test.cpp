/// 静态字段默认值内联（scope:class 首件）单测：mbase 治理版语义对齐
/// （单次写内联 / 多次写保守跳过 / owner 校验）+ 新载体特有面
/// （ConstantValue RawAttr 槽与 CP 值条目类型、clinit 指令重编码字节、
/// 无操作路径 writeClass 字节恒等、注册面 scope:class 种子）。
///
/// 夹具纪律：类骨架与 clinit 方法体一律 jasm 文本装配（JvmJasmService），
/// `iload 0` 读未播种槽为装配器放行面。纪律例外②申报：SkipsExisting-
/// ConstantValue 的字段 ConstantValue 属性 jasm 无对应指令面，从装配合法
/// 类出发经 CP/AttrSlot DOM 追加（addInteger + AttrSlot::makeNew）。
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

#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;

namespace {

using jbc::core::AttrKind;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::CpTag;
using jbc::core::MemberInfo;
using jbc::core::RawAttr;

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
        ca::u32 idx = 0;
        if (std::move(d).unwrap()->kind() == AttrKind::ConstantValue) {
            idx = ca::core::tag_cast::cast<const jbc::core::ConstantValueAttr*>(slot.peek())
                      ->constantValueCp;
        } else if (slot.peek()->kind() == AttrKind::Raw) {
            const auto* raw = ca::core::tag_cast::cast<const RawAttr*>(slot.peek());
            if (raw == nullptr || raw->bytes().size() != 2) return std::nullopt;
            idx = (static_cast<ca::u32>(raw->bytes()[0]) << 8) |
                  static_cast<ca::u32>(raw->bytes()[1]);
        } else {
            return std::nullopt;
        }
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

/// 类骨架：T + fields 声明序列 + clinitBody 为 <clinit> 的 code 体文本。
/// clinit 传 nullptr 即不带 clinit（无操作路径）。
Bytes classWithFields(std::string_view fieldDecls, const char* clinitBody)
{
    std::string source = R"JASM(
.super java/lang/Object
.class public super T {
)JASM" + std::string(fieldDecls);
    if (clinitBody != nullptr) {
        source += R"JASM(  .method static <clinit> ()V {
    code: {
)JASM" + std::string(clinitBody) + R"JASM(    }
  }
)JASM";
    }
    source += "}\n";
    return assembleJasm(source);
}

/// 全字段候选夹具：七字段（I×3 + J/D/F 各一），clinit 逐字段单写。
Bytes sevenFieldFixture()
{
    return classWithFields(R"JASM(  .field public static a I
  .field public static ab I
  .field public static cd I
  .field public static e J
  .field public static f D
  .field public static g F
)JASM",
                           R"JASM(      iconst_5
      putstatic T.a I
      bipush 100
      putstatic T.ab I
      sipush 30000
      putstatic T.cd I
      lconst_1
      putstatic T.e J
      dconst_1
      putstatic T.f D
      fconst_2
      putstatic T.g F
      return
)JASM");
}

/// 纪律例外②：装配合法类后经 CP/AttrSlot DOM 给字段 a 追加 ConstantValue
///（Integer 42）。jasm 无字段属性指令面，写面为申报的 CF 级偏差。
Bytes withExistingConstantValue(Bytes bytes)
{
    auto made = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return {};
    jbc::core::ReadResult rr = std::move(made).unwrap();
    jbc::core::ClassFile&  cf = *rr.file;
    const ca::u32          valIdx  = cf.cp.addInteger(42);
    const ca::u32          nameCp  = cf.cp.addUtf8("ConstantValue");
    auto                   body    = std::make_unique<jbc::core::ConstantValueAttr>();
    body->constantValueCp        = valIdx;
    MemberInfo* f = findField(cf, "a");
    EXPECT_NE(f, nullptr);
    if (f == nullptr) return {};
    f->attrs.push_back(AttrSlot::makeNew(nameCp, std::move(body)));
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    return written.is_ok() ? std::move(written).unwrap() : Bytes{};
}

/// 组会话（按字节开组）。
std::shared_ptr<jbc::ir::ClassSession> makeSession(const Bytes& bytes)
{
    auto result = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(result.is_ok());
    return result.is_ok() ? std::move(result).unwrap() : nullptr;
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
    ASSERT_NE(session, nullptr);
    const auto pa = runPass(*session);
    EXPECT_TRUE(pa.changed());

    ClassFile& cf = session->backing();
    const auto a = constantValueOf(cf, *findField(cf, "a"));
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->tag, CpTag::Integer);
    EXPECT_EQ(a->num, 5);
    const auto ab = constantValueOf(cf, *findField(cf, "ab"));
    ASSERT_TRUE(ab.has_value());
    EXPECT_EQ(ab->num, 100);                                  // bipush→LDC 同收
    const auto cd = constantValueOf(cf, *findField(cf, "cd"));
    ASSERT_TRUE(cd.has_value());
    EXPECT_EQ(cd->num, 30000);                                // sipush→LDC 同收
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
    const Bytes in = classWithFields(R"JASM(  .field public static big I
)JASM",
                                     R"JASM(      ldc 100000
      putstatic T.big I
      return
)JASM");
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
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
    const Bytes in = classWithFields(R"JASM(  .field public static a I
)JASM",
                                     R"JASM(      iconst_1
      putstatic T.a I
      iconst_2
      putstatic T.a I
      return
)JASM");
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

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
    const Bytes in =
        withExistingConstantValue(classWithFields(R"JASM(  .field public static a I
)JASM",
                                                  R"JASM(      iconst_5
      putstatic T.a I
      return
)JASM"));
    ASSERT_NE(in.size(), 0u);
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    EXPECT_TRUE(runPass(*session).areAllPreserved());
    const auto cv = constantValueOf(session->backing(), *findField(session->backing(), "a"));
    ASSERT_TRUE(cv.has_value());
    EXPECT_EQ(cv->num, 42) << "既有 ConstantValue 不得被改写";
}

TEST(MjPassStaticFieldDefaultInlinerTest, SkipsNonStaticFieldAndForeignOwner)
{
    const Bytes in = classWithFields(R"JASM(  .field public a I
  .field public static st I
)JASM",
                                     R"JASM(      iconst_5
      putstatic T.a I
      iconst_4
      putstatic Other.st I
      return
)JASM");
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    EXPECT_TRUE(runPass(*session).areAllPreserved());
    ClassFile& cf = session->backing();
    EXPECT_FALSE(constantValueOf(cf, *findField(cf, "a")).has_value());
    EXPECT_FALSE(constantValueOf(cf, *findField(cf, "st")).has_value());
}

TEST(MjPassStaticFieldDefaultInlinerTest, SkipsNonConstantProducer)
{
    const Bytes in = classWithFields(R"JASM(  .field public static a I
)JASM",
                                     R"JASM(      iload 0
      putstatic T.a I
      return
)JASM");
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

    EXPECT_TRUE(runPass(*session).areAllPreserved());
    EXPECT_FALSE(constantValueOf(session->backing(), *findField(session->backing(), "a")).has_value());
}

TEST(MjPassStaticFieldDefaultInlinerTest, NoClinitIsNoop)
{
    const Bytes in = classWithFields(R"JASM(  .field public static a I
  .method public m ()V {
    code: {
      return
    }
  }
)JASM",
                                     nullptr);
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);

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
