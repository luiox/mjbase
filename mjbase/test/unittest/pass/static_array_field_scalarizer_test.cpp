/// 静态数组字段标量化（#39 第一切片）单测：候选识别与拆分主链
/// （roundtrip 语义断言：改写 → writeClass → 重读会话 → 指令序列与
/// 字段表面验证），保守跳过面（非常量下标 / 整体引用 / arraylength /
/// 无分配 / 重赋 / 名冲突 / 非 private / 引用数组），注册面种子。
///
/// 夹具纪律：类骨架与 clinit/<init>/读方法体一律 jasm 文本装配
///（JvmJasmService）。窄 int 常量（bipush/sipush）装配器按自身口径规范
/// 化为 LDC——写窗值面经 pass 的 intConstOf ldc-Int 支路同判（语义不变）；
/// 标量化产出指令面（BIPUSH/PUTSTATIC 序列）由 pass 自身发射，事实断言
/// 不受夹具载体影响。
///
/// 纪律申报：无例外——本件全部夹具形态均落在装配器可表达域（含
/// anewarray [I 数组类型操作数与 ldc Class/ldc String 常量面）。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
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
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;

namespace {

/// 组会话（按字节开组）。
std::shared_ptr<jbc::ir::ClassSession> makeSession(const Bytes& bytes)
{
    auto result = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(result.is_ok());
    return result.is_ok() ? std::move(result).unwrap() : nullptr;
}

mjbase::spi::PreservedAnalyses runPass(jbc::ir::ClassSession& session)
{
    ca::str::Utf8StringArena                 arena;
    mjbase::spi::ClassAnalysisManager        cam(arena);
    mjbase::pass::StaticArrayFieldScalarizer pass;
    return pass.run(session, cam);
}

// ── jasm 装配便捷形 ─────────────────────────────────────────────────

/// 方法声明：修饰词文本（如 "public static"）/名/desc/code 体文本。
struct JMethod
{
    std::string_view mods;
    std::string_view name;
    std::string_view desc;
    std::string_view body;
};

/// 类装配：public super + .class cls { fields methods }，fields 为
/// .field 声明文本。
Bytes asmT(const std::string& cls, std::string_view fields, const std::vector<JMethod>& ms)
{
    std::string src = ".super java/lang/Object\n.class public super ";
    src.append(cls).append(" {\n").append(fields);
    for (const JMethod& m : ms) {
        src += "  .method ";
        src.append(m.mods).append(" ").append(m.name).append(" ").append(m.desc);
        src += " {\n    code: {\n";
        src.append(m.body);
        src += "    }\n  }\n";
    }
    src += "}\n";
    return assembleJasm(src);
}

/// int 常量装载文本：0..5 走 iconst_N，其余 bipush（装配器规范化为 LDC）。
std::string intLoad(ca::i32 v)
{
    if (v >= 0 && v <= 5) return "iconst_" + std::to_string(v);
    return "bipush " + std::to_string(v);
}

/// 主链夹具：tbl int[3]（private static），clinit 分配 + 三写，
/// read()I 常量下标读一。
Bytes tableFixture()
{
    return asmT("T", "  .field private static tbl [I\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_3\n"
             "      newarray int\n"
             "      putstatic T.tbl [I\n"
             "      getstatic T.tbl [I\n"
             "      iconst_0\n      bipush 11\n      iastore\n"
             "      getstatic T.tbl [I\n"
             "      iconst_1\n      bipush 22\n      iastore\n"
             "      getstatic T.tbl [I\n"
             "      iconst_2\n      bipush 33\n      iastore\n"
             "      return\n"},
            {"public static", "read", "()I",
             "      getstatic T.tbl [I\n      iconst_1\n      iaload\n      ireturn\n"},
        });
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
    ASSERT_NE(session, nullptr);
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
    const Bytes in = asmT("T", "  .field private static lt [J\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_2\n"
             "      newarray long\n"
             "      putstatic T.lt [J\n"
             "      getstatic T.lt [J\n"
             "      iconst_0\n      lconst_1\n      lastore\n"
             "      getstatic T.lt [J\n"
             "      iconst_1\n      lconst_0\n      lastore\n"
             "      return\n"},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
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
    const Bytes in = asmT("T", "  .field private static z [I\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      newarray int\n      putstatic T.z [I\n      return\n"},
            {"public static", "read", "()I",
             "      getstatic T.z [I\n      iconst_0\n      iaload\n      ireturn\n"},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
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

/// 通用骨架：build 夹具 → 跑 pass → 断言 all() + 字节恒等。
void expectUntouched(const Bytes& in)
{
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
    auto before = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(before.is_ok());
    const Bytes original = std::move(before).unwrap();

    EXPECT_TRUE(runPass(*session).areAllPreserved());

    auto after = jbc::core::writeClass(session->backing());
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(after.unwrap(), original);
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsNonConstantIndex)
{
    expectUntouched(asmT("T", "  .field private static tbl [I\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      newarray int\n      putstatic T.tbl [I\n      return\n"},
            // iload_0 下标（非常量）
            {"public static", "read", "(I)I",
             "      getstatic T.tbl [I\n      iload 0\n      iaload\n      ireturn\n"},
        }));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsWholeArrayReference)
{
    expectUntouched(asmT("T", "  .field private static tbl [I\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      newarray int\n      putstatic T.tbl [I\n      return\n"},
            // getstatic 整体传参
            {"public static", "sink", "()V",
             "      getstatic T.tbl [I\n      invokestatic Other.s ([I)V\n      return\n"},
        }));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsArraylengthUse)
{
    expectUntouched(asmT("T", "  .field private static tbl [I\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      newarray int\n      putstatic T.tbl [I\n      return\n"},
            {"public static", "len", "()I",
             "      getstatic T.tbl [I\n      arraylength\n      ireturn\n"},
        }));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsReadWithoutAllocation)
{
    expectUntouched(asmT("T", "  .field private static tbl [I\n",   // 无分配：null 数组语义
        {
            {"public static", "read", "()I",
             "      getstatic T.tbl [I\n      iconst_0\n      iaload\n      ireturn\n"},
        }));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsWholeReassignment)
{
    expectUntouched(asmT("T", "  .field private static tbl [I\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      newarray int\n      putstatic T.tbl [I\n"
             "      iconst_1\n      newarray int\n      putstatic T.tbl [I\n"
             "      return\n"},
        }));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsScalarNameCollision)
{
    expectUntouched(asmT("T",
        "  .field private static tbl [I\n"
        "  .field private static tbl_0 I\n",   // 新名已被占用
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      newarray int\n      putstatic T.tbl [I\n"
             "      getstatic T.tbl [I\n      iconst_0\n      iaload\n      pop\n"
             "      return\n"},
        }));
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsNonPrivateField)
{
    expectUntouched(asmT("T", "  .field public static tbl [I\n",   // 非 private static
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      newarray int\n      putstatic T.tbl [I\n      return\n"},
        }));
}

TEST(MjPassStaticArrayFieldScalarizerTest, ScalarizesStringArrayKeepCheckcast)
{
    const Bytes in = asmT("T", "  .field private static names [Ljava/lang/String;\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_2\n"
             "      anewarray java/lang/String\n"
             "      putstatic T.names [Ljava/lang/String;\n"
             "      getstatic T.names [Ljava/lang/String;\n"
             "      iconst_0\n      ldc \"alpha\"\n      aastore\n"
             "      getstatic T.names [Ljava/lang/String;\n"
             "      iconst_1\n      ldc \"beta\"\n      aastore\n"
             "      return\n"},
            // 读 + checkcast（保留）
            {"public static", "read", "()Ljava/lang/String;",
             "      getstatic T.names [Ljava/lang/String;\n"
             "      iconst_0\n      aaload\n      checkcast java/lang/String\n"
             "      areturn\n"},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
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
    expectUntouched(asmT("T", "  .field private static grid [[I\n",   // 多维：元素面不支持
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      anewarray [I\n      putstatic T.grid [[I\n"
             "      return\n"},
        }));
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
    return asmT("ct/TableHost",
        "  .field private static tbl [I\n"
        "  .field private static names [Ljava/lang/String;\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_3\n"
             "      newarray int\n"
             "      putstatic ct/TableHost.tbl [I\n"
             "      getstatic ct/TableHost.tbl [I\n"
             "      iconst_0\n      bipush 7\n      iastore\n"
             "      getstatic ct/TableHost.tbl [I\n"
             "      iconst_1\n      bipush 8\n      iastore\n"
             "      getstatic ct/TableHost.tbl [I\n"
             "      iconst_2\n      bipush 9\n      iastore\n"
             "      iconst_2\n"
             "      anewarray java/lang/String\n"
             "      putstatic ct/TableHost.names [Ljava/lang/String;\n"
             "      getstatic ct/TableHost.names [Ljava/lang/String;\n"
             "      iconst_0\n      ldc \"zero\"\n      aastore\n"
             "      getstatic ct/TableHost.names [Ljava/lang/String;\n"
             "      iconst_1\n      ldc \"one\"\n      aastore\n"
             "      return\n"},
            {"public static", "third", "()I",
             "      getstatic ct/TableHost.tbl [I\n      iconst_2\n      iaload\n"
             "      ireturn\n"},
            // 读 + checkcast（保留）
            {"public static", "firstName", "()Ljava/lang/String;",
             "      getstatic ct/TableHost.names [Ljava/lang/String;\n"
             "      iconst_0\n      aaload\n      checkcast java/lang/String\n"
             "      areturn\n"},
        });
}

/// ct/PublicTable：public static 数组（非 private）——应整类不动。
Bytes buildPublicTable()
{
    return asmT("ct/PublicTable", "  .field public static open [I\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      newarray int\n      putstatic ct/PublicTable.open [I\n"
             "      getstatic ct/PublicTable.open [I\n"
             "      iconst_0\n      iconst_1\n      iastore\n"
             "      return\n"},
        });
}

/// ct/DynamicUser：private static 数组但下标来自参数（非常量）——应整类不动。
Bytes buildDynamicUser()
{
    return asmT("ct/DynamicUser", "  .field private static vals [J\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_1\n      newarray long\n      putstatic ct/DynamicUser.vals [J\n"
             "      return\n"},
            {"public static", "pick", "(I)J",
             "      getstatic ct/DynamicUser.vals [J\n"
             "      iload 0\n      laload\n      lreturn\n"},   // iload_0：非常量下标
        });
}

/// ct/Plain：无候选字段——应整类不动。
Bytes buildPlain()
{
    return asmT("ct/Plain", "",
        {
            {"public static", "answer", "()I", "      iconst_0\n      ireturn\n"},
        });
}

// ── 实例字段切片夹具 ──

/// 实例主链：tbl int[3]（private 实例字段），<init> 分配 + 两写，
/// read()I 常量下标读一（aload_0; getfield; iconst; iaload; ireturn）。
/// extra 为调用方注入的变体方法（reset/len/len2 面），同源单类一次装配。
Bytes tableIFixture(const std::vector<JMethod>& extra = {})
{
    std::string src = R"JASM(.super java/lang/Object
.class public super T {
  .field private tbl [I
  .method private <init> ()V {
    code: {
      aload 0
      iconst_3
      newarray int
      putfield T.tbl [I
      aload 0
      getfield T.tbl [I
      iconst_0
      bipush 11
      iastore
      aload 0
      getfield T.tbl [I
      iconst_1
      bipush 22
      iastore
      return
    }
  }
  .method private read ()I {
    code: {
      aload 0
      getfield T.tbl [I
      iconst_1
      iaload
      ireturn
    }
  }
)JASM";
    for (const JMethod& m : extra) {
        src += "  .method ";
        src.append(m.mods).append(" ").append(m.name).append(" ").append(m.desc);
        src += " {\n    code: {\n";
        src.append(m.body);
        src += "    }\n  }\n";
    }
    src += "}\n";
    return assembleJasm(src);
}

}   // namespace

TEST(MjPassStaticArrayFieldScalarizerTest, ScalarizesInstanceField)
{
    auto session = makeSession(tableIFixture());
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(runPass(*session).changed());

    auto re = reRead(*session);
    const jbc::core::ClassFile& cf = re->backing();
    EXPECT_EQ(findField(cf, "tbl"), nullptr) << "旧数组字段应删除";
    for (const char* name : {"tbl_0", "tbl_1"}) {
        const jbc::core::MemberInfo* f = findField(cf, name);
        ASSERT_NE(f, nullptr) << name;
        EXPECT_EQ(cf.utf8(f->descCp), "I");
        EXPECT_EQ(f->access, 0x0002u) << "private 实例";
    }

    using jbc::core::Op;
    const auto ctor = methodFacts(*re, "<init>");
    // 写窗接收者 aload_0 保持原位（由新 putfield 消费）；分配窗四条全删。
    ASSERT_EQ(ctor.size(), 7u);
    EXPECT_EQ(ctor[0].op, Op::ALOAD_0);
    EXPECT_EQ(ctor[1].op, Op::BIPUSH);
    EXPECT_EQ(ctor[2].op, Op::PUTFIELD);
    EXPECT_EQ(ctor[2].member, "tbl_0");
    EXPECT_EQ(ctor[3].op, Op::ALOAD_0);
    EXPECT_EQ(ctor[4].op, Op::BIPUSH);
    EXPECT_EQ(ctor[5].op, Op::PUTFIELD);
    EXPECT_EQ(ctor[5].member, "tbl_1");
    EXPECT_EQ(ctor[6].op, Op::RETURN);

    const auto read = methodFacts(*re, "read");
    ASSERT_EQ(read.size(), 3u);
    EXPECT_EQ(read[0].op, Op::ALOAD_0);
    EXPECT_EQ(read[1].op, Op::GETFIELD);
    EXPECT_EQ(read[1].member, "tbl_1");
    EXPECT_EQ(read[2].op, Op::IRETURN);
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsInstanceAllocOutsideInit)
{
    // 追加 reset()V：非 <init> 的第二次分配窗——重执行重置语义不闭包。
    // （tbl_2 未访问下标不产生字段；reset 触发多分配守卫。）
    auto session = makeSession(tableIFixture(
        {{"private", "reset", "()V",
          "      aload 0\n      iconst_3\n      newarray int\n      putfield T.tbl [I\n"
          "      return\n"}}));
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(runPass(*session).changed());
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsInstanceNonWindowAccess)
{
    // 追加 len()I：arraylength 整体引用形态。
    auto session = makeSession(tableIFixture(
        {{"private", "len", "()I",
          "      aload 0\n      getfield T.tbl [I\n      arraylength\n      ireturn\n"}}));
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(runPass(*session).changed());
}

TEST(MjPassStaticArrayFieldScalarizerTest, SkipsInstanceNonWindowReceiver)
{
    // 追加 len2()I：getfield 前驱非接收者 aload（前驱是前窗 iastore 残段
    // ——邻接性破坏 → 放弃）。装配器活代码帧分析不收 getfield 欠接收者
    // 形——坏形块以 goto 越段死代码表达（死段不进帧分析、pass 照扫）。
    auto session = makeSession(tableIFixture(
        {{"private", "len2", "()I",
          "      goto Lend\n"
          "      iconst_1\n      getfield T.tbl [I\n      iconst_1\n      iaload\n"
          "      ireturn\n"
          "      Lend:\n"
          "      iconst_1\n      ireturn\n"}}));
    ASSERT_NE(session, nullptr);
    EXPECT_FALSE(runPass(*session).changed());
}

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
    auto reHost = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(hostAfter));
    ASSERT_TRUE(reHost.is_ok());
    const auto third = methodFacts(*reHost.unwrap(), "third");
    ASSERT_EQ(third.size(), 2u);
    EXPECT_EQ(third[0].op, Op::GETSTATIC);
    EXPECT_EQ(third[0].member, "tbl_2");
    const auto firstName = methodFacts(*reHost.unwrap(), "firstName");
    ASSERT_EQ(firstName.size(), 3u) << "checkcast 保留";
    EXPECT_EQ(firstName[0].member, "names_0");
    EXPECT_EQ(firstName[1].op, Op::CHECKCAST);

    // 保守面：非 private / 非常量下标 / 无候选三类字节恒等。
    EXPECT_EQ(resourceBytes(container, "ct/PublicTable.class"), pub);
    EXPECT_EQ(resourceBytes(container, "ct/DynamicUser.class"), dynamic);
    EXPECT_EQ(resourceBytes(container, "ct/Plain.class"), plain);
}


// ── #39 切片 3：引用元素类型精化（Object[] 声明 + 访问点证据） ──

namespace {

/// Object[2] 写窗值形态（jasm 字面：null / String 常量 / Class 常量）。
enum class StoreVal : ca::u8 { Null, StrX, ClassString };

/// 写窗值 jasm 装载文本。
std::string_view storeValText(StoreVal v)
{
    switch (v) {
    case StoreVal::StrX: return "ldc \"x\"";
    case StoreVal::ClassString: return "ldc Ljava/lang/String;";   // ldc Class
    default: return "aconst_null";
    }
}

/// 读方法体：getstatic; iconst_idx; aaload; [checkcast cast]; areturn。
std::string readBody(ca::i32 idx, const std::string& cast)
{
    std::string body = "      getstatic T.objs [Ljava/lang/Object;\n      "
                       + intLoad(idx) + "\n      aaload\n";
    if (!cast.empty()) {
        body += "      checkcast " + cast + "\n";
    }
    body += "      areturn\n";
    return body;
}

/// Object[2] 主链夹具：clinit 分配 + 两个写窗，read0/read1 各一带可选
/// checkcast 读。
Bytes objsFixture(StoreVal v0, StoreVal v1,
                  const std::string& cast0 = {},
                  const std::string& cast1 = {},
                  ca::i32 idx0 = 0, ca::i32 idx1 = 1)
{
    const std::string clinitBody =
        "      iconst_2\n"
        "      anewarray java/lang/Object\n"
        "      putstatic T.objs [Ljava/lang/Object;\n"
        "      getstatic T.objs [Ljava/lang/Object;\n"
        "      iconst_0\n      " + std::string(storeValText(v0)) + "\n      aastore\n"
        "      getstatic T.objs [Ljava/lang/Object;\n"
        "      iconst_1\n      " + std::string(storeValText(v1)) + "\n      aastore\n"
        "      return\n";
    return asmT("T", "  .field private static objs [Ljava/lang/Object;\n",
        {
            {"static", "<clinit>", "()V", clinitBody},
            {"public static", "read0", "()Ljava/lang/Object;", readBody(idx0, cast0)},
            {"public static", "read1", "()Ljava/lang/Object;", readBody(idx1, cast1)},
        });
}

}   // namespace

// ── String 证据（写窗 ldc String + 读窗 checkcast String）：objs_0 精化
//    Ljava/lang/String;，checkcast 随读窗折叠；无证据下标沿用声明元素型 ──
TEST(MjPassStaticArrayFieldScalarizerTest, RefinesObjectArrayElementByEvidence)
{
    auto session = makeSession(objsFixture(StoreVal::StrX, StoreVal::Null,
                                           "java/lang/String"));
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(runPass(*session).changed());
    auto re = reRead(*session);
    const jbc::core::ClassFile& cf = re->backing();
    EXPECT_EQ(findField(cf, "objs"), nullptr);
    const jbc::core::MemberInfo* f0 = findField(cf, "objs_0");
    ASSERT_NE(f0, nullptr);
    EXPECT_EQ(cf.utf8(f0->descCp), "Ljava/lang/String;");
    const jbc::core::MemberInfo* f1 = findField(cf, "objs_1");
    ASSERT_NE(f1, nullptr);
    EXPECT_EQ(cf.utf8(f1->descCp), "Ljava/lang/Object;");   // 无证据下标不精化
    const auto read0 = methodFacts(*re, "read0");
    ASSERT_EQ(read0.size(), 3u);          // cast 与写证据一致：保留原位
    EXPECT_EQ(read0[0].op, jbc::core::Op::GETSTATIC);
    EXPECT_EQ(read0[0].member, "objs_0");
    EXPECT_EQ(read0[1].op, jbc::core::Op::CHECKCAST);
    EXPECT_EQ(read0[2].op, jbc::core::Op::ARETURN);
}

// ── 同下标证据冲突（checkcast String vs checkcast Integer）：整字段放弃 ──
TEST(MjPassStaticArrayFieldScalarizerTest, ConflictingEvidenceAbortsField)
{
    auto session = makeSession(objsFixture(StoreVal::Null, StoreVal::Null,
                                           "java/lang/String", "java/lang/Integer",
                                           0, 0));
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(runPass(*session).areAllPreserved());
    auto re = reRead(*session);
    EXPECT_NE(findField(re->backing(), "objs"), nullptr);
    EXPECT_EQ(findField(re->backing(), "objs_0"), nullptr);
}

// ── Class 常量写 + checkcast Class 读：精化 Ljava/lang/Class; ──
TEST(MjPassStaticArrayFieldScalarizerTest, ClassConstWriteRefinesToClass)
{
    auto session = makeSession(objsFixture(StoreVal::ClassString, StoreVal::Null,
                                           "java/lang/Class"));
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(runPass(*session).changed());
    auto re = reRead(*session);
    const jbc::core::MemberInfo* f0 = findField(re->backing(), "objs_0");
    ASSERT_NE(f0, nullptr);
    EXPECT_EQ(re->backing().utf8(f0->descCp), "Ljava/lang/Class;");
    const auto read0 = methodFacts(*re, "read0");
    ASSERT_EQ(read0.size(), 3u);          // checkcast Class 与证据一致：保留
    EXPECT_EQ(read0[1].op, jbc::core::Op::CHECKCAST);
}

// ── Class 常量写 + checkcast String 读：证据冲突，整字段放弃 ──
TEST(MjPassStaticArrayFieldScalarizerTest, ClassWriteStringReadAborts)
{
    auto session = makeSession(objsFixture(StoreVal::ClassString, StoreVal::Null,
                                           "java/lang/String"));
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(runPass(*session).areAllPreserved());
    auto re = reRead(*session);
    EXPECT_NE(findField(re->backing(), "objs"), nullptr);
}

// ── 声明元素非 Object：证据与声明不一致（ldc Class 写入 String[]）→
//    放弃（修复既有改写面会产出验证错误的潜在缺口） ──
TEST(MjPassStaticArrayFieldScalarizerTest, NonObjectDeclaredMismatchAborts)
{
    const Bytes in = asmT("T", "  .field private static names [Ljava/lang/String;\n",
        {
            {"static", "<clinit>", "()V",
             "      iconst_2\n"
             "      anewarray java/lang/String\n"
             "      putstatic T.names [Ljava/lang/String;\n"
             "      getstatic T.names [Ljava/lang/String;\n"
             "      iconst_0\n      ldc Ljava/lang/String;\n      aastore\n"
             "      return\n"},
        });
    auto session = makeSession(in);
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(runPass(*session).areAllPreserved());
    auto re = reRead(*session);
    EXPECT_NE(findField(re->backing(), "names"), nullptr);
}

// ── 逐下标独立精化：index0 String 证据、index1 Integer 证据并存 ──
TEST(MjPassStaticArrayFieldScalarizerTest, PerIndexIndependentRefinement)
{
    auto session = makeSession(objsFixture(StoreVal::StrX, StoreVal::Null,
                                           "java/lang/String", "java/lang/Integer"));
    ASSERT_NE(session, nullptr);
    EXPECT_TRUE(runPass(*session).changed());
    auto re = reRead(*session);
    const jbc::core::ClassFile& cf = re->backing();
    const jbc::core::MemberInfo* f0 = findField(cf, "objs_0");
    ASSERT_NE(f0, nullptr);
    EXPECT_EQ(cf.utf8(f0->descCp), "Ljava/lang/String;");
    const jbc::core::MemberInfo* f1 = findField(cf, "objs_1");
    ASSERT_NE(f1, nullptr);
    EXPECT_EQ(cf.utf8(f1->descCp), "Ljava/lang/Integer;");
    const auto read1 = methodFacts(*re, "read1");
    ASSERT_EQ(read1.size(), 2u);          // idx1 首证据来自读 cast：折叠
    EXPECT_EQ(read1[0].op, jbc::core::Op::GETSTATIC);
    EXPECT_EQ(read1[0].member, "objs_1");
    EXPECT_EQ(read1[1].op, jbc::core::Op::ARETURN);
}
