// StaticValueCollection（容器级只读收集件）单测：正例（clinit 常量 /
// 走廊算术折叠 / ConstantValue 属性面）+ 拒绝面逐桶（volatile / 多次写 /
// 非 clinit 写 / 他类写 / 走廊不纯 / 父链不纯 / 父链容器外 / 预算 /
// 不可解析类）+ 只读面（容器字节恒等）+ 幂等二跑 + 注册面 scope。
// 夹具一律 jasm 文本装配（JvmJasmService，一下一个类）；jasm 文本面不载
// 字段属性，ConstantValue 桶经 SFDI 同款写面（AttrSlot::makeNew + RawAttr
// 二字节载荷）挂载——「SFDI 前道工序产出」的构成实样。视图经同一
// GlobalAnalysisManager 的 pinned 面读取。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/pass/static_value_collection.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

/// UTF-8 → UTF-16（jasm 服务入参口径）。
ca::str::Utf16String utf16Of(const std::string& s)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(s.data()), s.size()));
}

/// jasm 文本汇编为 class 字节（一下一个类；汇编失败即测试失败）。
/// superName 用 internal name；body 为类体文本（.field / .method 声明序列）。
Bytes assembleClass(const std::string& internalName, const std::string& superName,
                    const std::string& body)
{
    mjbase::jasm::JvmJasmService service;
    const std::string source = ".super " + superName +
                               "\n.class public super " + internalName + " {\n" +
                               body + "}\n";
    const auto sourceName = utf16Of(internalName + ".jasm");
    const auto sourceText = utf16Of(source);
    const auto result     = service.assemble(sourceName.ref(), sourceText.ref());
    if (!result.isOk() || !result.value.has_value()) {
        for (const auto& d : result.diagnostics) {
            ADD_FAILURE() << "jasm: " << d.message.to_utf8_string().to_std_string();
        }
        ADD_FAILURE() << "jasm 汇编失败: " << internalName;
        return {};
    }
    return result.value->bytes;
}

/// 伴生常量类 T：五族字段 + clinit 各写一条常量。
Bytes constClassFixture()
{
    return assembleClass("T", "java/lang/Object",
                         "  .field public static a I\n"
                         "  .field public static e J\n"
                         "  .field public static f D\n"
                         "  .field public static g F\n"
                         "  .field public static s Ljava/lang/String;\n"
                         "\n"
                         "  .method static <clinit> ()V {\n"
                         "    code: {\n"
                         "      bipush 42\n"
                         "      putstatic T.a I\n"
                         "      ldc 72057594037927936L\n"
                         "      putstatic T.e J\n"
                         "      dconst_0\n"
                         "      putstatic T.f D\n"
                         "      fconst_0\n"
                         "      putstatic T.g F\n"
                         "      ldc \"hello-const\"\n"
                         "      putstatic T.s Ljava/lang/String;\n"
                         "      return\n"
                         "    }\n"
                         "  }\n");
}

/// 读点类 R：m()I 读 T.a；n 读 T.s；l()J 读 T.e（cls 仅定类名）。
Bytes readerFixture(const char* cls)
{
    return assembleClass(std::string(cls), "java/lang/Object",
                         "  .method public static m ()I {\n"
                         "    code: {\n"
                         "      getstatic T.a I\n"
                         "      ireturn\n"
                         "    }\n"
                         "  }\n"
                         "  .method public static n ()Ljava/lang/String; {\n"
                         "    code: {\n"
                         "      getstatic T.s Ljava/lang/String;\n"
                         "      areturn\n"
                         "    }\n"
                         "  }\n"
                         "  .method public static l ()J {\n"
                         "    code: {\n"
                         "      getstatic T.e J\n"
                         "      lreturn\n"
                         "    }\n"
                         "  }\n");
}

/// 经 SFDI 同款写面（AttrSlot::makeNew + RawAttr）给字段挂
/// ConstantValue(int 42)（JVMS 4.7.2 体恰为 u2 CP 索引）：构成「SFDI 前道
/// 工序产出」的实样，收集件属性面照收。
Bytes withConstantValueInt42(const Bytes& assembled, const std::string& fieldName)
{
    auto made = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(assembled));
    EXPECT_TRUE(made.is_ok());
    if (made.is_err()) return {};
    auto                  session = std::move(made).unwrap();
    jbc::core::ClassFile& cf      = session->backing();
    const ca::u32         nameCp  = session->internString("ConstantValue");
    const ca::u32         valCp   = session->internInteger(42);
    Bytes                 payload;
    putU16(payload, static_cast<ca::u16>(valCp));
    bool hit = false;
    for (jbc::core::MemberInfo& f : cf.fields) {
        if (cf.nameOf(f) != fieldName) continue;
        f.attrs.push_back(jbc::core::AttrSlot::makeNew(
            nameCp, std::make_unique<jbc::core::RawAttr>(payload)));
        hit = true;
        break;
    }
    EXPECT_TRUE(hit) << "field not found: " << fieldName;
    if (!hit) return {};
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// 收集件 + 同一 GAM；返回发布视图（无内容为 nullptr）。
const mjbase::pass::StaticValueView* runCollect(mjbase::model::ResourceContainer& container,
                                                mjbase::spi::GlobalAnalysisManager& gam)
{
    mjbase::pass::StaticValueCollection collector;
    const auto pa = collector.run(container, gam);
    EXPECT_TRUE(pa.areAllPreserved()) << "收集件是只读件，恒 all()";
    return gam.getPinned<mjbase::pass::StaticValueViewAnalysis>();
}

Bytes resourceBytes(mjbase::model::ResourceContainer& container, const std::string& uri)
{
    const auto* res = ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
    EXPECT_NE(res, nullptr);
    return res == nullptr ? Bytes{} : res->get();
}

}   // namespace

// ── 正例 ──

TEST(MjPassStaticValueCollectionTest, CollectsClinitConstantsIntoView)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture());
    // 读点类同场：收集件只读，不要求读点存在/不被读点形态牵连。
    addClassResource(container, "R.class", readerFixture("R"));
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    const auto* view = runCollect(container, gam);
    ASSERT_NE(view, nullptr);
    const auto* a = view->find("T", "a", "I");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->kind, mjbase::pass::StaticValue::Kind::Int);
    EXPECT_EQ(a->num, 42);
    const auto* e = view->find("T", "e", "J");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->kind, mjbase::pass::StaticValue::Kind::Long);
    EXPECT_EQ(e->num, 72057594037927936LL);
    const auto* f = view->find("T", "f", "D");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->kind, mjbase::pass::StaticValue::Kind::Double);
    EXPECT_EQ(f->bits, 0);   // dconst_0 = +0.0
    const auto* g = view->find("T", "g", "F");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->kind, mjbase::pass::StaticValue::Kind::Float);
    EXPECT_EQ(g->bits, 0);
    const auto* s = view->find("T", "s", "Ljava/lang/String;");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->kind, mjbase::pass::StaticValue::Kind::Str);
    EXPECT_EQ(s->str, "hello-const");
}

TEST(MjPassStaticValueCollectionTest, FoldsCorridorArithmetic)
{
    const Bytes t = assembleClass(
        "T", "java/lang/Object",
        "  .field public static x I\n"
        "  .field public static y I\n"
        "\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 20\n"
        "      bipush 22\n"
        "      iadd\n"
        "      putstatic T.x I\n"
        "      sipush 1000\n"
        "      bipush 2\n"
        "      imul\n"
        "      putstatic T.y I\n"
        "      return\n"
        "    }\n"
        "  }\n");

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", t);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    const auto* view = runCollect(container, gam);
    ASSERT_NE(view, nullptr);
    const auto* x = view->find("T", "x", "I");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->num, 42);
    const auto* y = view->find("T", "y", "I");
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(y->num, 2000);
}

TEST(MjPassStaticValueCollectionTest, HarvestsConstantValueAttribute)
{
    const Bytes t = withConstantValueInt42(
        assembleClass("T", "java/lang/Object",
                      "  .field public static final k I\n"
                      "  .method public static m ()V {\n"
                      "    code: {\n"
                      "      return\n"
                      "    }\n"
                      "  }\n"),
        "k");

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", t);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    const auto* view = runCollect(container, gam);
    ASSERT_NE(view, nullptr);
    const auto* k = view->find("T", "k", "I");
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->kind, mjbase::pass::StaticValue::Kind::Int);
    EXPECT_EQ(k->num, 42);
}

TEST(MjPassStaticValueCollectionTest, NonFinalConstantValueNotHarvested)
{
    // JVM 只对 static final 生效 ConstantValue；非 final 挂属性值不可信。
    const Bytes t = withConstantValueInt42(
        assembleClass("T", "java/lang/Object",
                      "  .field public static k I\n"
                      "  .method public static m ()V {\n"
                      "    code: {\n"
                      "      return\n"
                      "    }\n"
                      "  }\n"),
        "k");

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", t);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

// ── 拒绝面逐桶 ──

TEST(MjPassStaticValueCollectionTest, RejectsVolatileField)
{
    const Bytes t = assembleClass(
        "T", "java/lang/Object",
        "  .field public static a I\n"
        "  .field public static volatile va I\n"
        "\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 42\n"
        "      putstatic T.a I\n"
        "      bipush 7\n"
        "      putstatic T.va I\n"
        "      return\n"
        "    }\n"
        "  }\n");

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", t);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    const auto* view = runCollect(container, gam);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->find("T", "a", "I"), nullptr);      // 非 volatile 同场发布
    EXPECT_EQ(view->find("T", "va", "I"), nullptr);     // volatile 桶
}

TEST(MjPassStaticValueCollectionTest, RejectsMultipleWrites)
{
    const Bytes t = assembleClass(
        "T", "java/lang/Object",
        "  .field public static a I\n"
        "\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 1\n"
        "      putstatic T.a I\n"
        "      bipush 2\n"
        "      putstatic T.a I\n"
        "      return\n"
        "    }\n"
        "  }\n");

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", t);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsNonClinitWrite)
{
    const Bytes t = assembleClass(
        "T", "java/lang/Object",
        "  .field public static a I\n"
        "\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 42\n"
        "      putstatic T.a I\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static m ()V {\n"
        "    code: {\n"
        "      bipush 1\n"
        "      putstatic T.a I\n"
        "      return\n"
        "    }\n"
        "  }\n");

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", t);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsForeignClassWrite)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture());
    {
        // 他类方法对 T.a 的 PUTSTATIC：全域写点数 > 1 → 拒。
        const Bytes u = assembleClass(
            "U", "java/lang/Object",
            "  .method public static m ()V {\n"
            "    code: {\n"
            "      bipush 1\n"
            "      putstatic T.a I\n"
            "      return\n"
            "    }\n"
            "  }\n");
        addClassResource(container, "U.class", u);
    }
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    // 单写者普查按字段拒：T.a 写点数 > 1 → a 拒；同类其余字段不牵连。
    const auto* view = runCollect(container, gam);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->find("T", "a", "I"), nullptr);
    EXPECT_NE(view->find("T", "e", "J"), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsImpureCorridor)
{
    // clinit 内出现 invokestatic（可观察初始化效应）→ 整类不发布。
    const Bytes t = assembleClass(
        "T", "java/lang/Object",
        "  .field public static a I\n"
        "\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      invokestatic T.m ()V\n"
        "      bipush 42\n"
        "      putstatic T.a I\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static m ()V {\n"
        "    code: {\n"
        "      return\n"
        "    }\n"
        "  }\n");

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", t);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsWhenSuperClinitImpure)
{
    mjbase::model::ResourceContainer container;
    {
        // 父类 S：clinit 含 invokestatic（可观察效应）。
        const Bytes s = assembleClass(
            "S", "java/lang/Object",
            "  .method static <clinit> ()V {\n"
            "    code: {\n"
            "      invokestatic S.m ()V\n"
            "      return\n"
            "    }\n"
            "  }\n"
            "  .method public static m ()V {\n"
            "    code: {\n"
            "      return\n"
            "    }\n"
            "  }\n");
        addClassResource(container, "S.class", s);
    }
    {
        const Bytes t = assembleClass(
            "T", "S",
            "  .field public static a I\n"
            "\n"
            "  .method static <clinit> ()V {\n"
            "    code: {\n"
            "      bipush 42\n"
            "      putstatic T.a I\n"
            "      return\n"
            "    }\n"
            "  }\n");
        addClassResource(container, "T.class", t);
    }
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsWhenSuperOutsideContainer)
{
    // 父类 lib/Base 不在容器且非 java/ 前缀：父链初始化效应不可证 → 拒。
    const Bytes t = assembleClass(
        "T", "lib/Base",
        "  .field public static a I\n"
        "\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 42\n"
        "      putstatic T.a I\n"
        "      return\n"
        "    }\n"
        "  }\n");

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", t);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsOnCorridorBudget)
{
    // 求值不收敛桶：clinit 指令数超 2 万预算 → 整类拒；恰好预算 → 发布。
    auto build = [](ca::usize nops) {
        std::string body = "  .field public static a I\n"
                           "\n"
                           "  .method static <clinit> ()V {\n"
                           "    code: {\n";
        for (ca::usize i = 0; i < nops; ++i) body += "      nop\n";
        body += "      bipush 42\n"
                "      putstatic T.a I\n"
                "      return\n"
                "    }\n"
                "  }\n";
        return assembleClass("T", "java/lang/Object", body);
    };

    {
        // 预算内（nops + 3 == kMaxCorridorInsns）：发布。
        mjbase::model::ResourceContainer container;
        addClassResource(container, "T.class",
                         build(mjbase::pass::StaticValueCollection::kMaxCorridorInsns - 3));
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        const auto* view = runCollect(container, gam);
        ASSERT_NE(view, nullptr);
        EXPECT_NE(view->find("T", "a", "I"), nullptr);
    }
    {
        // 超预算（+1）：整类拒。
        mjbase::model::ResourceContainer container;
        addClassResource(container, "T.class",
                         build(mjbase::pass::StaticValueCollection::kMaxCorridorInsns - 2));
        ca::str::Utf8StringArena           arena;
        mjbase::spi::GlobalAnalysisManager gam(arena);
        EXPECT_EQ(runCollect(container, gam), nullptr);
    }
}

TEST(MjPassStaticValueCollectionTest, RejectsFamilyMismatchPair)
{
    // 值面桶：double 字段的写点前驱是 float 常量（fconst_0）→ 族不匹配 →
    // 该字段拒（同类其余字段不受牵连）。
    const Bytes t = assembleClass(
        "T", "java/lang/Object",
        "  .field public static a I\n"
        "  .field public static d D\n"
        "\n"
        "  .method static <clinit> ()V {\n"
        "    code: {\n"
        "      bipush 42\n"
        "      putstatic T.a I\n"
        "      fconst_0\n"
        "      putstatic T.d D\n"
        "      return\n"
        "    }\n"
        "  }\n");

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", t);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    const auto* view = runCollect(container, gam);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->find("T", "a", "I"), nullptr);
    EXPECT_EQ(view->find("T", "d", "D"), nullptr);
}

TEST(MjPassStaticValueCollectionTest, UnparseableClassAbortsWholePass)
{
    mjbase::model::ResourceContainer container;
    const Bytes good = constClassFixture();
    addClassResource(container, "T.class", good);
    const Bytes truncated(good.begin(), good.begin() + 8);   // 截断真类 = 不可解析
    addClassResource(container, "Bad.class", truncated);
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

// ── 只读面与幂等 ──

TEST(MjPassStaticValueCollectionTest, ContainerBytesUntouched)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture());
    const Bytes before = resourceBytes(container, "T.class");

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_NE(runCollect(container, gam), nullptr);

    EXPECT_EQ(resourceBytes(container, "T.class"), before) << "收集件不得改容器字节";
}

TEST(MjPassStaticValueCollectionTest, IdempotentRepublish)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture());
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    const auto* first = runCollect(container, gam);
    ASSERT_NE(first, nullptr);
    const ca::usize firstSize = first->size();

    mjbase::pass::StaticValueCollection collector;
    EXPECT_TRUE(collector.run(container, gam).areAllPreserved());
    const auto* second = gam.getPinned<mjbase::pass::StaticValueViewAnalysis>();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->size(), firstSize);
    const auto* a = second->find("T", "a", "I");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->num, 42);
}

TEST(MjPassStaticValueCollectionTest, EmptyResultUnpins)
{
    // 无候选类：不发布（此前批次若发布过也撤钉）——物化件必须严格 no-op。
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture());
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    ASSERT_NE(runCollect(container, gam), nullptr);

    mjbase::model::ResourceContainer empty;
    mjbase::pass::StaticValueCollection collector;
    EXPECT_TRUE(collector.run(empty, gam).areAllPreserved());
    EXPECT_EQ(gam.getPinned<mjbase::pass::StaticValueViewAnalysis>(), nullptr);
}

// ── 注册面 ──

TEST(MjPassStaticValueCollectionTest, RegistrySeedsGlobalScope)
{
    const auto found = mjbase::spi::defaultPassRegistry().find("StaticValueCollection");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, mjbase::spi::PassScope::Global);
}

TEST(MjPassStaticValueCollectionTest, InliningRegistrySeedsGlobalScope)
{
    const auto found = mjbase::spi::defaultPassRegistry().find("StaticValueInlining");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, mjbase::spi::PassScope::Global);
}
