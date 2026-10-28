// GlobalStrPoolOptimizer（全局件）：容器内 String ldc 去重迁移进合成池类，
// ldc 改写为「索引装载 + getStr 调用」——迁移与改写（含索引波段与 maxStack
// 上界）、跨类去重、非 String ldc 保留、非 class 资源忽略、池类已存在幂等
// 跳过、不可解析整件放弃、无字符串不动、helper 形状、注册面。夹具经
// shared/structural_pass_util.hpp 手拼 classfile；方法体断言经会话 promote
// 后的指令序完成。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/global_str_pool_optimizer.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

namespace {

constexpr ca::u16 kAccPublicStatic = 0x0009;

/// LDC 1 字节 CP 索引。
Bytes ldc(ca::u16 cp) { return {0x12, static_cast<ca::u8>(cp)}; }

mjbase::spi::PreservedAnalyses runPass(mjbase::model::ResourceContainer& container)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::GlobalStrPoolOptimizer pass;
    return pass.run(container, gam);
}

const mjbase::model::ClassResource* classResource(
    mjbase::model::ResourceContainer& container, const std::string& uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

/// 读回 class 字节中某方法 promote 后的 (op, cp) 序列（按方法名取首个命中）。
struct OpCp
{
    jbc::core::Op op;
    ca::u32       cp;
};

std::vector<OpCp> methodOpCps(const Bytes& bytes, const char* methodName)
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
        EXPECT_TRUE(promoted.is_ok())
            << "promote failed: " << methodName << " ("
            << jbc::ir::to_string(promoted.unwrap_err()) << ")";
        if (promoted.is_err()) return {};
        auto              cuRef = std::move(promoted).unwrap();
        std::vector<OpCp> ops;
        for (const jbc::ir::InsnId id : cuRef->dom.order()) {
            const jbc::ir::Insn* in = cuRef->dom.peek(id);
            EXPECT_NE(in, nullptr);
            if (in == nullptr) return {};
            ops.push_back({in->op, in->cp});
        }
        return ops;
    }
    ADD_FAILURE() << "method not found: " << methodName;
    return {};
}

/// 读回 class 字节中某方法 promote 后的 maxStack。
ca::u16 methodMaxStack(const Bytes& bytes, const char* methodName)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return 0;
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (unit.symbols().str(unit.methods[idx].name) != methodName) continue;
        if (!jbc::ir::hasCode(*session, idx)) return 0;
        auto promoted = jbc::ir::promoteMethod(*session, idx);
        EXPECT_TRUE(promoted.is_ok());
        if (promoted.is_err()) return 0;
        return std::move(promoted).unwrap()->maxStack;
    }
    ADD_FAILURE() << "method not found: " << methodName;
    return 0;
}

/// 池类形状断言：str/Pool 类名 + strPool 字段 + <init>/<clinit>/getStr
/// 指令面与 maxStack 口径（<init> 1 / <clinit> 3 / getStr 2）。
void expectPoolShape(const mjbase::model::ClassResource* pool, ca::usize entryCount)
{
    ASSERT_NE(pool, nullptr);
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(pool->get()));
    ASSERT_TRUE(sessionResult.is_ok());
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    EXPECT_EQ(unit.symbols().str(unit.thisClass), "str/Pool");
    bool hasField = false;
    for (const auto& f : unit.fields) {
        if (unit.symbols().str(f.name) == "strPool"
            && unit.symbols().str(f.desc) == "[Ljava/lang/String;") {
            hasField = true;
        }
    }
    EXPECT_TRUE(hasField);
    const std::vector<OpCp> clinit = methodOpCps(pool->get(), "<clinit>");
    // [idx; anewarray; putstatic; {(getstatic; idx; ldc; aastore)×n}; return]
    ASSERT_EQ(clinit.size(), 4u + entryCount * 4u);
    EXPECT_EQ(clinit[1].op, jbc::core::Op::ANEWARRAY);
    EXPECT_EQ(clinit[2].op, jbc::core::Op::PUTSTATIC);
    EXPECT_EQ(clinit[3].op, jbc::core::Op::GETSTATIC);
    EXPECT_EQ(methodMaxStack(pool->get(), "<clinit>"), 3);
    const std::vector<OpCp> getStr = methodOpCps(pool->get(), "getStr");
    ASSERT_EQ(getStr.size(), 4u);
    EXPECT_EQ(getStr[0].op, jbc::core::Op::GETSTATIC);
    EXPECT_EQ(getStr[1].op, jbc::core::Op::ILOAD);
    EXPECT_EQ(getStr[2].op, jbc::core::Op::AALOAD);
    EXPECT_EQ(getStr[3].op, jbc::core::Op::ARETURN);
    EXPECT_EQ(methodMaxStack(pool->get(), "getStr"), 2);
    EXPECT_EQ(methodMaxStack(pool->get(), "<init>"), 1);
}

}   // namespace

// ── 单类两字符串（含重复）→ ldc 全部改写为索引+调用 ──
TEST(GlobalStrPoolOptimizerTest, stringsRelocatedToPool)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("C1");
        const ca::u16 sb = b.strConst("b");
        const ca::u16 sa = b.strConst("a");
        Bytes        code;   // ldc "b"; ldc "a"; ldc "a"; areturn
        appendBytes(code, ldc(sb));
        appendBytes(code, ldc(sa));
        appendBytes(code, ldc(sa));
        code.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(code), kAccPublicStatic,
                     {}, 2, 0});
        addClassResource(container, "C1.class", b.assemble());
    }

    EXPECT_TRUE(runPass(container).changed());

    const auto* c1 = classResource(container, "C1.class");
    ASSERT_NE(c1, nullptr);
    // 索引按遭遇序：b→0、a→1（对齐旧栈编号语义）；0/1 落 ICONST 波段。
    const std::vector<OpCp> ops = methodOpCps(c1->get(), "m");
    ASSERT_EQ(ops.size(), 7u);
    EXPECT_EQ(ops[0].op, jbc::core::Op::ICONST_0);
    EXPECT_EQ(ops[1].op, jbc::core::Op::INVOKESTATIC);
    EXPECT_EQ(ops[2].op, jbc::core::Op::ICONST_1);
    EXPECT_EQ(ops[3].op, jbc::core::Op::INVOKESTATIC);
    EXPECT_EQ(ops[4].op, jbc::core::Op::ICONST_1);
    EXPECT_EQ(ops[5].op, jbc::core::Op::INVOKESTATIC);
    EXPECT_EQ(ops[6].op, jbc::core::Op::ARETURN);
    // 三次 getStr 调用共享同一 methodref。
    EXPECT_EQ(ops[1].cp, ops[3].cp);
    EXPECT_EQ(ops[3].cp, ops[5].cp);
    // 改写方法 maxStack 保守 +1（夹具 2 → 3）。
    EXPECT_EQ(methodMaxStack(c1->get(), "m"), 3);
    expectPoolShape(classResource(container, "str/Pool.class"), 2u);
}

// ── 跨类去重：两类共享字符串 → 池内唯一 ──
TEST(GlobalStrPoolOptimizerTest, dedupAcrossClasses)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("C1");
        const ca::u16  sx = b.strConst("x");
        Bytes          c  = ldc(sx);
        c.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(c), kAccPublicStatic,
                     {}, 2, 0});
        addClassResource(container, "C1.class", b.assemble());
    }
    {
        FixtureBuilder b("C2");
        const ca::u16  sx = b.strConst("x");
        const ca::u16  sy = b.strConst("y");
        Bytes          c  = ldc(sx);
        Bytes          cy = ldc(sy);
        c.insert(c.end(), cy.begin(), cy.end());
        c.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(c), kAccPublicStatic,
                     {}, 2, 0});
        addClassResource(container, "C2.class", b.assemble());
    }

    EXPECT_TRUE(runPass(container).changed());
    const auto* pool = classResource(container, "str/Pool.class");
    ASSERT_NE(pool, nullptr);
    expectPoolShape(pool, 2u);   // x、y 去重后恰 2 项
    const auto* c2 = classResource(container, "C2.class");
    ASSERT_NE(c2, nullptr);
    const std::vector<OpCp> ops = methodOpCps(c2->get(), "m");
    ASSERT_EQ(ops.size(), 5u);   // [idx, getStr][idx, getStr] areturn
    EXPECT_EQ(ops[4].op, jbc::core::Op::ARETURN);
}

// ── 非 String ldc（int 常量）不迁移 ──
TEST(GlobalStrPoolOptimizerTest, nonStringLdcKept)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("C1");
        const ca::u16 icp  = b.intConst(42);
        const ca::u16 scp  = b.strConst("s");
        Bytes         code = ldc(icp);   // LDC Integer
        appendBytes(code, ldc(scp));      // LDC String
        code.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(code), kAccPublicStatic,
                     {}, 2, 0});
        addClassResource(container, "C1.class", b.assemble());
    }

    EXPECT_TRUE(runPass(container).changed());
    const auto* c1 = classResource(container, "C1.class");
    ASSERT_NE(c1, nullptr);
    const std::vector<OpCp> ops = methodOpCps(c1->get(), "m");
    ASSERT_EQ(ops.size(), 4u);   // [int ldc 原样][idx; getStr] areturn
    EXPECT_EQ(ops[0].op, jbc::core::Op::LDC);
    EXPECT_EQ(ops[1].op, jbc::core::Op::ICONST_0);
    EXPECT_EQ(ops[2].op, jbc::core::Op::INVOKESTATIC);
}

// ── 非 class 资源忽略 ──
TEST(GlobalStrPoolOptimizerTest, nonClassResourceIgnored)
{
    mjbase::model::ResourceContainer container;
    container.replaceResource(
        "meta.txt", std::make_unique<mjbase::model::UnknownResource>(
                        "meta.txt", Bytes{0x00, 0x01, 0x02}));
    {
        FixtureBuilder b("C1");
        const ca::u16  scp = b.strConst("s");
        Bytes          c    = ldc(scp);
        c.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(c), kAccPublicStatic,
                     {}, 2, 0});
        addClassResource(container, "C1.class", b.assemble());
    }

    EXPECT_TRUE(runPass(container).changed());
    EXPECT_NE(container.getResource("meta.txt"), nullptr);
    EXPECT_NE(classResource(container, "str/Pool.class"), nullptr);
}

// ── 池类已存在 → 幂等整件跳过 ──
TEST(GlobalStrPoolOptimizerTest, poolPresentSkips)
{
    mjbase::model::ResourceContainer container;
    {
        // 先跑一遍产出池类，再放回池类 + 一个含字符串的新类。
        FixtureBuilder b("C1");
        const ca::u16  scp = b.strConst("s");
        Bytes          c    = ldc(scp);
        c.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(c), kAccPublicStatic,
                     {}, 2, 0});
        addClassResource(container, "C1.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* pool = classResource(container, "str/Pool.class");
    ASSERT_NE(pool, nullptr);
    const Bytes poolBytes = pool->get();

    mjbase::model::ResourceContainer container2;
    container2.replaceResource("str/Pool.class",
                               std::make_unique<mjbase::model::ClassResource>(
                                   "str/Pool.class", poolBytes));
    {
        FixtureBuilder b("C2");
        const ca::u16  scp = b.strConst("t");
        Bytes          c    = ldc(scp);
        c.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(c), kAccPublicStatic,
                     {}, 2, 0});
        addClassResource(container2, "C2.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container2).areAllPreserved());
    // C2 未被改写（池已存在，字符串不迁移）。
    const auto* c2 = classResource(container2, "C2.class");
    ASSERT_NE(c2, nullptr);
    const std::vector<OpCp> ops = methodOpCps(c2->get(), "m");
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].op, jbc::core::Op::LDC);
}

// ── 类资源不可解析 → 整件放弃（fail-closed） ──
TEST(GlobalStrPoolOptimizerTest, unparsableClassAborts)
{
    mjbase::model::ResourceContainer container;
    container.replaceResource("bad.class",
                              std::make_unique<mjbase::model::ClassResource>(
                                  "bad.class", Bytes{0xCA, 0xFE, 0xBA, 0xBE, 0x00}));
    {
        FixtureBuilder b("C1");
        const ca::u16  scp = b.strConst("s");
        Bytes          c    = ldc(scp);
        c.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(c), kAccPublicStatic,
                     {}, 2, 0});
        addClassResource(container, "C1.class", b.assemble());
    }

    EXPECT_TRUE(runPass(container).areAllPreserved());
    EXPECT_EQ(classResource(container, "str/Pool.class"), nullptr);
    const auto* c1 = classResource(container, "C1.class");
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(methodOpCps(c1->get(), "m").size(), 2u);   // 未改写
}

// ── 无字符串常量 → 不动 ──
TEST(GlobalStrPoolOptimizerTest, noStringsNoop)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("C1");
        b.addMethod({"m", "()I", {0x04, 0xac}, kAccPublicStatic, {}, 1, 0});
        addClassResource(container, "C1.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).areAllPreserved());
    EXPECT_EQ(classResource(container, "str/Pool.class"), nullptr);
}

// ── 幂等：二轮整件跳过（池类已在容器） ──
TEST(GlobalStrPoolOptimizerTest, secondRunNoop)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("C1");
        const ca::u16  scp = b.strConst("s");
        Bytes          c    = ldc(scp);
        c.push_back(0xb0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(c), kAccPublicStatic,
                     {}, 2, 0});
        addClassResource(container, "C1.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    EXPECT_TRUE(runPass(container).areAllPreserved());
}

// ── 注册面 ──
TEST(GlobalStrPoolOptimizerTest, registeredInRegistry)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("GlobalStrPoolOptimizer"));
    auto created =
        mjbase::spi::defaultPassRegistry().createGlobalPass("GlobalStrPoolOptimizer");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
