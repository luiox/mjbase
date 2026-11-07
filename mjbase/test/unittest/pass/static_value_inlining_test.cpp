// StaticValueInlining（容器级物化件）单测：链式正例（收集件 pinned 视图 →
// 跨类读点原位物化，cat1/cat2 与 String 三族）+ 独立启停（无收集件严格
// no-op）+ 拒绝面逐桶（clinit/<init> 读点、volatile 复核、类别失配，
// 手工视图注入逐桶反证）+ 幂等二跑 + 常量类字节不动。夹具经
// shared/structural_pass_util.hpp 手拼；改写结果经会话 promote 后指令序断言。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/pass/static_value_collection.hpp"
#include "mjbase/pass/static_value_inlining.hpp"
#include "mjbase/spi/analysis_manager.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublicStatic  = 0x0009;
constexpr ca::u16 kAccPublic        = 0x0001;
constexpr ca::u16 kAccPublicStaticVol = 0x0049;
constexpr ca::u16 kAccStaticMethod  = 0x0008;

Bytes u16b(ca::u16 v) { return {static_cast<ca::u8>(v >> 8), static_cast<ca::u8>(v)}; }

Bytes cat(Bytes a, const Bytes& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Bytes bipush(ca::i8 v) { return {0x10, static_cast<ca::u8>(v)}; }
Bytes putStatic(ca::u16 cp) { return cat({0xb3}, u16b(cp)); }
Bytes getStatic(ca::u16 cp) { return cat({0xb2}, u16b(cp)); }
Bytes ldc1(ca::u16 cp) { return {0x12, static_cast<ca::u8>(cp)}; }

const Bytes kReturn   = {0xb1};
const Bytes kIReturn  = {0xac};
const Bytes kLReturn  = {0xad};
const Bytes kAReturn  = {0xb0};
const Bytes kPop      = {0x57};

/// 伴生常量类 T：clinit 写 int a=42、long e、String s。
FixtureBuilder constClassFixture()
{
    FixtureBuilder b("T");
    b.addField({"a", "I", kAccPublicStatic});
    b.addField({"e", "J", kAccPublicStatic});
    b.addField({"s", "Ljava/lang/String;", kAccPublicStatic});
    const ca::u16 ai  = b.fieldRef("T", "a", "I");
    const ca::u16 ei  = b.fieldRef("T", "e", "J");
    const ca::u16 si  = b.fieldRef("T", "s", "Ljava/lang/String;");
    const ca::u16 lc  = b.longConst(72057594037927936L);
    const ca::u16 str = b.strConst("hello-const");
    Bytes code;
    appendBytes(code, cat(bipush(42), putStatic(ai)));
    appendBytes(code, cat({0x14}, cat(u16b(lc), putStatic(ei))));   // ldc2_w
    appendBytes(code, cat(ldc1(str), putStatic(si)));
    appendBytes(code, kReturn);
    b.addMethod({"<clinit>", "()V", std::move(code), kAccStaticMethod});
    return b;
}

/// 读点类：m()I 读 T.a；n()Ljava/lang/String; 读 T.s；l()J 读 T.e。
FixtureBuilder readerFixture()
{
    FixtureBuilder b("R");
    {
        const ca::u16 fld = b.fieldRef("T", "a", "I");
        b.addMethod({"m", "()I", cat(getStatic(fld), kIReturn), kAccPublicStatic});
    }
    {
        const ca::u16 fld = b.fieldRef("T", "s", "Ljava/lang/String;");
        b.addMethod({"n", "()Ljava/lang/String;", cat(getStatic(fld), kAReturn),
                     kAccPublicStatic});
    }
    {
        const ca::u16 fld = b.fieldRef("T", "e", "J");
        b.addMethod({"l", "()J", cat(getStatic(fld), kLReturn), kAccPublicStatic});
    }
    return b;
}

/// 链式跑：收集件 + 物化件共用同一 GAM。
mjbase::spi::PreservedAnalyses runChain(mjbase::model::ResourceContainer& container,
                                        ca::str::Utf8StringArena&         arena,
                                        mjbase::spi::GlobalAnalysisManager& gam)
{
    mjbase::pass::StaticValueCollection collector;
    (void)collector.run(container, gam);
    mjbase::pass::StaticValueInlining inliner;
    return inliner.run(container, gam);
}

/// 读回 class 字节中某方法 promote 后的指令序（按方法名取第一个命中）。
std::vector<jbc::core::Op> methodOps(const Bytes& bytes, const char* methodName)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok()) << "session open failed: " << methodName;
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (unit.symbols().str(unit.methods[idx].name) != methodName) continue;
        if (!jbc::ir::hasCode(*session, idx)) return {};
        auto promoted = jbc::ir::promoteMethod(*session, idx);
        EXPECT_TRUE(promoted.is_ok()) << "promote failed: " << methodName;
        if (promoted.is_err()) return {};
        auto                       cuRef = std::move(promoted).unwrap();
        std::vector<jbc::core::Op> ops;
        for (const jbc::ir::InsnId id : cuRef->dom.order()) {
            const jbc::ir::Insn* in = cuRef->dom.peek(id);
            EXPECT_NE(in, nullptr);
            if (in == nullptr) return {};
            ops.push_back(in->op);
        }
        return ops;
    }
    ADD_FAILURE() << "method not found: " << methodName;
    return {};
}

Bytes resourceBytes(mjbase::model::ResourceContainer& container, const std::string& uri)
{
    const auto* res = ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
    EXPECT_NE(res, nullptr);
    return res == nullptr ? Bytes{} : res->get();
}

}   // namespace

// ── 链式正例 ──

TEST(MjPassStaticValueInliningTest, MaterializesCollectedCrossClassReads)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture().assemble());
    addClassResource(container, "R.class", readerFixture().assemble());
    const Bytes readerBefore = resourceBytes(container, "R.class");
    const Bytes constBefore  = resourceBytes(container, "T.class");

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    const auto pa = runChain(container, arena, gam);
    EXPECT_TRUE(pa.changed());

    // int 读点 → bipush（cat1 物化）。
    const std::vector<jbc::core::Op> m =
        methodOps(resourceBytes(container, "R.class"), "m");
    ASSERT_EQ(m.size(), 2u);
    EXPECT_EQ(m[0], jbc::core::Op::BIPUSH);
    EXPECT_EQ(m[1], jbc::core::Op::IRETURN);
    // String 读点 → ldc（视图 Str 物化）。
    const std::vector<jbc::core::Op> n =
        methodOps(resourceBytes(container, "R.class"), "n");
    ASSERT_EQ(n.size(), 2u);
    EXPECT_EQ(n[0], jbc::core::Op::LDC);
    EXPECT_EQ(n[1], jbc::core::Op::ARETURN);
    // long 读点 → ldc2_w（cat2 物化，栈形状恒等）。
    const std::vector<jbc::core::Op> l =
        methodOps(resourceBytes(container, "R.class"), "l");
    ASSERT_EQ(l.size(), 2u);
    EXPECT_EQ(l[0], jbc::core::Op::LDC2_W);
    EXPECT_EQ(l[1], jbc::core::Op::LRETURN);

    // 常量类自身字节不动（clinit 写侧留原地；本件只动读点）。
    EXPECT_EQ(resourceBytes(container, "T.class"), constBefore);
    EXPECT_NE(resourceBytes(container, "R.class"), readerBefore) << "读点侧必须有字节面改动";
}

TEST(MjPassStaticValueInliningTest, InlinerAloneIsStrictNoop)
{
    // 独立启停桶：链上没有收集件 → 严格 no-op（不得自行重算视图）。
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture().assemble());
    addClassResource(container, "R.class", readerFixture().assemble());
    const Bytes readerBefore = resourceBytes(container, "R.class");

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::StaticValueInlining  inliner;
    EXPECT_TRUE(inliner.run(container, gam).areAllPreserved());
    EXPECT_EQ(resourceBytes(container, "R.class"), readerBefore);
}

// ── 初始化语义面 ──

TEST(MjPassStaticValueInliningTest, SkipsReadsInsideClinit)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture().assemble());
    {
        // U 的 clinit 内读 T.a（前向引用形态）——读点保留。
        FixtureBuilder b("U");
        const ca::u16 fld = b.fieldRef("T", "a", "I");
        b.addMethod({"<clinit>", "()V", cat(getStatic(fld), cat(kPop, kReturn)),
                     kAccStaticMethod});
        addClassResource(container, "U.class", b.assemble());
    }
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    runChain(container, arena, gam);

    const std::vector<jbc::core::Op> ops =
        methodOps(resourceBytes(container, "U.class"), "<clinit>");
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], jbc::core::Op::GETSTATIC);
    EXPECT_EQ(ops[1], jbc::core::Op::POP);
    EXPECT_EQ(ops[2], jbc::core::Op::RETURN);
}

TEST(MjPassStaticValueInliningTest, SkipsReadsInsideInit)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture().assemble());
    {
        // U 的构造器内读 T.a（初始化环形态）——读点保留。
        FixtureBuilder b("U");
        const ca::u16 fld = b.fieldRef("T", "a", "I");
        b.addMethod({"<init>", "()V", cat(getStatic(fld), cat(kPop, kReturn)), kAccPublic});
        addClassResource(container, "U.class", b.assemble());
    }
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    runChain(container, arena, gam);

    const std::vector<jbc::core::Op> ops =
        methodOps(resourceBytes(container, "U.class"), "<init>");
    ASSERT_EQ(ops.size(), 3u);
    EXPECT_EQ(ops[0], jbc::core::Op::GETSTATIC);
    EXPECT_EQ(ops[1], jbc::core::Op::POP);
    EXPECT_EQ(ops[2], jbc::core::Op::RETURN);
}

// ── 物化件自持拒绝桶（手工视图注入反证：收集面不产此类条目，注入即证
//    物化面独立成立）──

TEST(MjPassStaticValueInliningTest, VolatileFieldReadKeptByInlinerRecheck)
{
    // 手工视图注入 volatile 字段条目（收集面不会产出）：物化件旗标复核
    // 保留读点。
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("T");
        b.addField({"va", "I", kAccPublicStaticVol});
        b.addMethod({"m", "()V", kReturn, kAccPublicStatic});
        addClassResource(container, "T.class", b.assemble());
    }
    {
        FixtureBuilder b("R");
        const ca::u16 fld = b.fieldRef("T", "va", "I");
        b.addMethod({"m", "()I", cat(getStatic(fld), kIReturn), kAccPublicStatic});
        addClassResource(container, "R.class", b.assemble());
    }

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::StaticValueView      view;
    mjbase::pass::StaticValue          v;
    v.kind = mjbase::pass::StaticValue::Kind::Int;
    v.num  = 42;
    view.put("T", "va", "I", v);
    gam.setPinned<mjbase::pass::StaticValueViewAnalysis>(std::move(view));

    mjbase::pass::StaticValueInlining inliner;
    EXPECT_TRUE(inliner.run(container, gam).areAllPreserved());
    const std::vector<jbc::core::Op> ops =
        methodOps(resourceBytes(container, "R.class"), "m");
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], jbc::core::Op::GETSTATIC);
    EXPECT_EQ(ops[1], jbc::core::Op::IRETURN);
}

TEST(MjPassStaticValueInliningTest, CategoryMismatchReadKept)
{
    // 手工视图注入类别失配条目（Int 值配 J 字段 = cat1/cat2 桶）：读点保留。
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("T");
        b.addField({"e", "J", kAccPublicStatic});
        b.addMethod({"m", "()V", kReturn, kAccPublicStatic});
        addClassResource(container, "T.class", b.assemble());
    }
    {
        FixtureBuilder b("R");
        const ca::u16 fld = b.fieldRef("T", "e", "J");
        b.addMethod({"l", "()J", cat(getStatic(fld), kLReturn), kAccPublicStatic});
        addClassResource(container, "R.class", b.assemble());
    }

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::StaticValueView      view;
    mjbase::pass::StaticValue          bad;
    bad.kind = mjbase::pass::StaticValue::Kind::Int;   // J 字段配 Int = 失配
    bad.num  = 42;
    view.put("T", "e", "J", bad);
    gam.setPinned<mjbase::pass::StaticValueViewAnalysis>(std::move(view));

    mjbase::pass::StaticValueInlining inliner;
    EXPECT_TRUE(inliner.run(container, gam).areAllPreserved());
    const std::vector<jbc::core::Op> ops =
        methodOps(resourceBytes(container, "R.class"), "l");
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0], jbc::core::Op::GETSTATIC);
    EXPECT_EQ(ops[1], jbc::core::Op::LRETURN);
}

// ── 幂等 ──

TEST(MjPassStaticValueInliningTest, SecondChainRunIsNoop)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "T.class", constClassFixture().assemble());
    addClassResource(container, "R.class", readerFixture().assemble());

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    EXPECT_TRUE(runChain(container, arena, gam).changed());
    const Bytes afterFirst = resourceBytes(container, "R.class");

    mjbase::pass::StaticValueCollection collector;
    EXPECT_TRUE(collector.run(container, gam).areAllPreserved());
    mjbase::pass::StaticValueInlining inliner;
    EXPECT_TRUE(inliner.run(container, gam).areAllPreserved()) << "二跑：读点已消失";
    EXPECT_EQ(resourceBytes(container, "R.class"), afterFirst);
}
