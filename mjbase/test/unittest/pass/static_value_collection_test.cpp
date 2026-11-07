// StaticValueCollection（容器级只读收集件）单测：正例（clinit 常量 /
// 走廊算术折叠 / ConstantValue 属性面）+ 拒绝面逐桶（volatile / 多次写 /
// 非 clinit 写 / 他类写 / 走廊不纯 / 父链不纯 / 父链容器外 / 预算 /
// 不可解析类）+ 只读面（容器字节恒等）+ 幂等二跑 + 注册面 scope。
// 夹具经 shared/structural_pass_util.hpp 手拼 classfile；视图经同一
// GlobalAnalysisManager 的 pinned 面读取。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/pass/static_value_collection.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublicStatic       = 0x0009;
constexpr ca::u16 kAccPublicStaticFinal  = 0x0019;
constexpr ca::u16 kAccPublicStaticVol    = 0x0049;
constexpr ca::u16 kAccStaticMethod       = 0x0008;

Bytes u16b(ca::u16 v) { return {static_cast<ca::u8>(v >> 8), static_cast<ca::u8>(v)}; }

Bytes cat(Bytes a, const Bytes& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Bytes bipush(ca::i8 v) { return {0x10, static_cast<ca::u8>(v)}; }
Bytes sipush(ca::u16 v) { return cat({0x11}, u16b(v)); }
Bytes putStatic(ca::u16 cp) { return cat({0xb3}, u16b(cp)); }
Bytes getStatic(ca::u16 cp) { return cat({0xb2}, u16b(cp)); }
// invokeStatic 用 shared/structural_pass_util.hpp 的同名件（避免重载歧义）。
Bytes ldc2wLong(ca::u16 cp) { return cat({0x14}, u16b(cp)); }
Bytes ldc1(ca::u16 cp) { return {0x12, static_cast<ca::u8>(cp)}; }

const Bytes kReturn = {0xb1};
const Bytes kIReturn = {0xac};

/// 读点类：m()I 里 GETSTATIC T.a; IRETURN。
FixtureBuilder readerFixture(const char* cls)
{
    FixtureBuilder b(cls);
    const ca::u16 fld = b.fieldRef("T", "a", "I");
    b.addMethod({"m", "()I", cat(getStatic(fld), kIReturn), kAccPublicStatic});
    return b;
}

/// 伴生常量类 T：I/J/D/F/String 五字段 + clinit 各写一条常量。
FixtureBuilder constClassFixture()
{
    FixtureBuilder b("T");
    b.addField({"a", "I", kAccPublicStatic});
    b.addField({"e", "J", kAccPublicStatic});
    b.addField({"f", "D", kAccPublicStatic});
    b.addField({"g", "F", kAccPublicStatic});
    b.addField({"s", "Ljava/lang/String;", kAccPublicStatic});
    const ca::u16 ai  = b.fieldRef("T", "a", "I");
    const ca::u16 ei  = b.fieldRef("T", "e", "J");
    const ca::u16 fi  = b.fieldRef("T", "f", "D");
    const ca::u16 gi  = b.fieldRef("T", "g", "F");
    const ca::u16 si  = b.fieldRef("T", "s", "Ljava/lang/String;");
    const ca::u16 lc  = b.longConst(72057594037927936L);
    const ca::u16 str = b.strConst("hello-const");
    Bytes code;
    appendBytes(code, cat(bipush(42), putStatic(ai)));
    appendBytes(code, ldc2wLong(lc));
    appendBytes(code, putStatic(ei));
    appendBytes(code, {0x0e});                    // dconst_0
    appendBytes(code, putStatic(fi));
    appendBytes(code, {0x0a});                    // fconst_0
    appendBytes(code, putStatic(gi));
    appendBytes(code, cat(ldc1(str), putStatic(si)));
    appendBytes(code, kReturn);
    b.addMethod({"<clinit>", "()V", std::move(code), kAccStaticMethod});
    return b;
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
    addClassResource(container, "T.class", constClassFixture().assemble());
    // 读点类同场：收集件只读，不要求读点存在/不被读点形态牵连。
    addClassResource(container, "R.class", readerFixture("R").assemble());
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
    FixtureBuilder b("T");
    b.addField({"x", "I", kAccPublicStatic});
    b.addField({"y", "I", kAccPublicStatic});
    const ca::u16 xi = b.fieldRef("T", "x", "I");
    const ca::u16 yi = b.fieldRef("T", "y", "I");
    Bytes code;
    appendBytes(code, cat(cat(bipush(20), bipush(22)), {0x60}));    // 20+22
    appendBytes(code, putStatic(xi));
    appendBytes(code, cat(cat(sipush(1000), bipush(2)), {0x68}));   // 1000*2
    appendBytes(code, putStatic(yi));
    appendBytes(code, kReturn);
    b.addMethod({"<clinit>", "()V", std::move(code), kAccStaticMethod});

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", b.assemble());
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
    FixtureBuilder b("T");
    b.addField({"k", "I", kAccPublicStaticFinal, {{"ConstantValue", [&b] {
        Bytes p;
        putU16(p, b.intConst(42));
        return p;
    }()}}});
    b.addMethod({"m", "()V", kReturn, kAccPublicStatic});

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", b.assemble());
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
    FixtureBuilder b("T");
    b.addField({"k", "I", kAccPublicStatic, {{"ConstantValue", [&b] {
        Bytes p;
        putU16(p, b.intConst(42));
        return p;
    }()}}});
    b.addMethod({"m", "()V", kReturn, kAccPublicStatic});

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", b.assemble());
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

// ── 拒绝面逐桶 ──

TEST(MjPassStaticValueCollectionTest, RejectsVolatileField)
{
    FixtureBuilder b("T");
    b.addField({"a", "I", kAccPublicStatic});
    b.addField({"va", "I", kAccPublicStaticVol});
    const ca::u16 ai = b.fieldRef("T", "a", "I");
    const ca::u16 vi = b.fieldRef("T", "va", "I");
    Bytes code;
    appendBytes(code, cat(bipush(42), putStatic(ai)));
    appendBytes(code, cat(bipush(7), putStatic(vi)));
    appendBytes(code, kReturn);
    b.addMethod({"<clinit>", "()V", std::move(code), kAccStaticMethod});

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", b.assemble());
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);

    const auto* view = runCollect(container, gam);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->find("T", "a", "I"), nullptr);      // 非 volatile 同场发布
    EXPECT_EQ(view->find("T", "va", "I"), nullptr);     // volatile 桶
}

TEST(MjPassStaticValueCollectionTest, RejectsMultipleWrites)
{
    FixtureBuilder b("T");
    b.addField({"a", "I", kAccPublicStatic});
    const ca::u16 ai = b.fieldRef("T", "a", "I");
    Bytes code;
    appendBytes(code, cat(bipush(1), putStatic(ai)));
    appendBytes(code, cat(bipush(2), putStatic(ai)));
    appendBytes(code, kReturn);
    b.addMethod({"<clinit>", "()V", std::move(code), kAccStaticMethod});

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", b.assemble());
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsNonClinitWrite)
{
    FixtureBuilder b("T");
    b.addField({"a", "I", kAccPublicStatic});
    const ca::u16 ai = b.fieldRef("T", "a", "I");
    Bytes clinit;
    appendBytes(clinit, cat(bipush(42), putStatic(ai)));
    appendBytes(clinit, kReturn);
    b.addMethod({"<clinit>", "()V", std::move(clinit), kAccStaticMethod});
    Bytes inM;
    appendBytes(inM, cat(bipush(1), putStatic(ai)));
    appendBytes(inM, kReturn);
    b.addMethod({"m", "()V", std::move(inM), kAccPublicStatic});

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", b.assemble());
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsForeignClassWrite)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("T");
        b.addField({"a", "I", kAccPublicStatic});
        const ca::u16 ai = b.fieldRef("T", "a", "I");
        Bytes code;
        appendBytes(code, cat(bipush(42), putStatic(ai)));
        appendBytes(code, kReturn);
        b.addMethod({"<clinit>", "()V", std::move(code), kAccStaticMethod});
        addClassResource(container, "T.class", b.assemble());
    }
    {
        // 他类方法对 T.a 的 PUTSTATIC：全域写点数 > 1 → 拒。
        FixtureBuilder b("U");
        const ca::u16 ai = b.fieldRef("T", "a", "I");
        Bytes code;
        appendBytes(code, cat(bipush(1), putStatic(ai)));
        appendBytes(code, kReturn);
        b.addMethod({"m", "()V", std::move(code), kAccPublicStatic});
        addClassResource(container, "U.class", b.assemble());
    }
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsImpureCorridor)
{
    // clinit 内出现 invokestatic（可观察初始化效应）→ 整类不发布。
    FixtureBuilder b("T");
    b.addField({"a", "I", kAccPublicStatic});
    const ca::u16 mi = b.methodRef("T", "m", "()V");
    const ca::u16 ai = b.fieldRef("T", "a", "I");
    Bytes code;
    appendBytes(code, invokeStatic(mi));
    appendBytes(code, cat(bipush(42), putStatic(ai)));
    appendBytes(code, kReturn);
    b.addMethod({"<clinit>", "()V", std::move(code), kAccStaticMethod});
    b.addMethod({"m", "()V", kReturn, kAccPublicStatic});

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", b.assemble());
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsWhenSuperClinitImpure)
{
    mjbase::model::ResourceContainer container;
    {
        // 父类 S：clinit 含 invokestatic（可观察效应）。
        FixtureBuilder s("S");
        const ca::u16 mi = s.methodRef("S", "m", "()V");
        s.addMethod({"<clinit>", "()V", cat(invokeStatic(mi), kReturn), kAccStaticMethod});
        s.addMethod({"m", "()V", kReturn, kAccPublicStatic});
        addClassResource(container, "S.class", s.assemble());
    }
    {
        FixtureBuilder t("T");
        t.setSuper("S");
        t.addField({"a", "I", kAccPublicStatic});
        const ca::u16 ai = t.fieldRef("T", "a", "I");
        t.addMethod({"<clinit>", "()V", cat(bipush(42), cat(putStatic(ai), kReturn)),
                     kAccStaticMethod});
        addClassResource(container, "T.class", t.assemble());
    }
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsWhenSuperOutsideContainer)
{
    // 父类 lib/Base 不在容器且非 java/ 前缀：父链初始化效应不可证 → 拒。
    mjbase::model::ResourceContainer container;
    FixtureBuilder t("T");
    t.setSuper("lib/Base");
    t.addField({"a", "I", kAccPublicStatic});
    const ca::u16 ai = t.fieldRef("T", "a", "I");
    t.addMethod({"<clinit>", "()V", cat(bipush(42), cat(putStatic(ai), kReturn)),
                 kAccStaticMethod});
    addClassResource(container, "T.class", t.assemble());

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

TEST(MjPassStaticValueCollectionTest, RejectsOnCorridorBudget)
{
    // 求值不收敛桶：clinit 指令数超 2 万预算 → 整类拒；恰好预算 → 发布。
    auto build = [](ca::usize nops) {
        FixtureBuilder b("T");
        b.addField({"a", "I", kAccPublicStatic});
        const ca::u16 ai = b.fieldRef("T", "a", "I");
        Bytes code(static_cast<ca::usize>(nops), 0x00);   // NOP × nops
        appendBytes(code, bipush(42));
        appendBytes(code, putStatic(ai));
        appendBytes(code, kReturn);
        b.addMethod({"<clinit>", "()V", std::move(code), kAccStaticMethod});
        return b.assemble();
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
    // 值面桶：double 字段的写点前驱是 float 常量 → 族不匹配 → 该字段拒
    //（同类其余字段不受牵连）。
    FixtureBuilder b("T");
    b.addField({"a", "I", kAccPublicStatic});
    b.addField({"d", "D", kAccPublicStatic});
    const ca::u16 ai = b.fieldRef("T", "a", "I");
    const ca::u16 di = b.fieldRef("T", "d", "D");
    Bytes code;
    appendBytes(code, cat(bipush(42), putStatic(ai)));
    appendBytes(code, cat({0x0a}, putStatic(di)));   // fconst_0 写 D 字段
    appendBytes(code, kReturn);
    b.addMethod({"<clinit>", "()V", std::move(code), kAccStaticMethod});

    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", b.assemble());
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
    addClassResource(container, "T.class", constClassFixture().assemble());
    addClassResource(container, "Bad.class", {0xca, 0xfe});   // 非 classfile
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_EQ(runCollect(container, gam), nullptr);
}

// ── 只读面与幂等 ──

TEST(MjPassStaticValueCollectionTest, ContainerBytesUntouched)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture().assemble());
    const Bytes before = resourceBytes(container, "T.class");

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_NE(runCollect(container, gam), nullptr);

    EXPECT_EQ(resourceBytes(container, "T.class"), before) << "收集件不得改容器字节";
}

TEST(MjPassStaticValueCollectionTest, IdempotentRepublish)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture().assemble());
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
    addClassResource(container, "T.class", constClassFixture().assemble());
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
