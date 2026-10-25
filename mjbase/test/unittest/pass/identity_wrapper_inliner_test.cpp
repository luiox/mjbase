/// @file        identity_wrapper_inliner_test.cpp
/// @brief       恒等包装不动点内联：容器级多类用例（链压缩 / 装拆箱 / 虚与
///              接口转发 / 环与形态门 / 访问门 / 幂等）。
///
/// 载体是 ResourceContainer + 手工字节夹具（FixtureBuilder）；语义断言走
/// 「改写 → writeClass → 重读会话 → 指令序列事实」roundtrip 口径。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/identity_wrapper_inliner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "shared/structural_pass_util.hpp"

namespace {

using jbc::core::Op;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::test::Bytes;
using mjbase::test::FixtureBuilder;
using mjbase::test::putU16;
using mjbase::test::putU8;

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

mjbase::spi::PreservedAnalyses runInliner(ResourceContainer& container)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::IdentityWrapperInliner pass;
    return pass.run(container, gam);
}

/// 重读事实：指令 op 序列 + invoke 族的语义目标（owner.name(desc)）。
struct InsnFact
{
    Op          op = Op::NOP;
    std::string member;
};

std::vector<InsnFact> methodFacts(const Bytes& bytes, std::string_view name,
                                  std::string_view desc)
{
    std::vector<InsnFact> facts;
    auto loaded = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(loaded.is_ok());
    if (loaded.is_err()) return facts;
    jbc::ir::ClassSession& session = *loaded.unwrap();
    const jbc::ir::ClassUnit& unit = session.unit();
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        const jbc::ir::MethodUnit& mu = unit.methods[i];
        if (unit.symbols().str(mu.name) != name || unit.symbols().str(mu.desc) != desc) continue;
        auto promoted = jbc::ir::promoteMethod(session, i);
        EXPECT_TRUE(promoted.is_ok());
        if (promoted.is_err()) return facts;
        jbc::ir::CodeUnit* cu = promoted.unwrap().get();
        if (cu == nullptr) return facts;
        for (const jbc::ir::InsnId id : cu->dom.order()) {
            const jbc::ir::Insn* in = cu->dom.peek(id);
            if (in == nullptr) continue;
            InsnFact fact;
            fact.op = in->op;
            jbc::ir::ClassSession::MemberSem sem;
            if (session.memberOf(*in, sem)) {
                fact.member.append(sem.owner).append(".").append(sem.name)
                    .append(sem.desc);
            }
            facts.push_back(std::move(fact));
        }
        return facts;
    }
    ADD_FAILURE() << "method not found: " << name << desc;
    return facts;
}

/// 壳类工厂：static 壳 w/w(JI) 等，体内按描述符装载后转发到 target。
Bytes buildForwarder(const char* self, const char* name, const char* desc,
                     const char* targetOwner, const char* targetName, const char* targetDesc)
{
    FixtureBuilder b(self);
    const ca::u16 ref = b.methodRef(targetOwner, targetName, targetDesc);
    Bytes code;
    // 装载序按 (JI)J 以外的常见形态由调用方保证；本工厂只服务单 int/单引用参。
    (void)desc;
    putU8(code, 0x1A);   // iload_0
    putU8(code, 0xB8); putU16(code, ref);
    putU8(code, 0xAC);   // ireturn
    b.addMethod({name, desc, std::move(code), 0x0009, {}, 1, 1});
    return b.assemble();
}

}   // namespace

// ── 形态 A：静态直通链（跨类路径压缩）──

TEST(MjPassIdentityWrapperInlinerTest, CompressesStaticForwardChain)
{
    ResourceContainer container;
    // T.real(I)I
    {
        FixtureBuilder b("T");
        const ca::u16 ref = b.methodRef("T", "real", "(I)I");
        Bytes code;
        putU8(code, 0x1A); putU8(code, 0x04); putU8(code, 0x60); putU8(code, 0xAC);
        b.addMethod({"real", "(I)I", std::move(code), 0x0009, {}, 2, 1});
        addClassResource(container, "T.class", b.assemble());
    }
    // W2.w → T.real
    addClassResource(container, "W2.class",
                     buildForwarder("W2", "w", "(I)I", "T", "real", "(I)I"));
    // W1.w → W2.w
    addClassResource(container, "W1.class",
                     buildForwarder("W1", "w", "(I)I", "W2", "w", "(I)I"));
    // Caller.m → W1.w
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("W1", "w", "(I)I");
        Bytes code;
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"m", "(I)I", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "Caller.class", b.assemble());
    }
    const Bytes w2Before = resourceBytes(container, "W2.class");

    EXPECT_TRUE(runInliner(container).changed());

    const auto caller = methodFacts(resourceBytes(container, "Caller.class"), "m", "(I)I");
    ASSERT_EQ(caller.size(), 3u);
    EXPECT_EQ(caller[0].op, Op::ILOAD_0);
    EXPECT_EQ(caller[1].op, Op::INVOKESTATIC);
    EXPECT_EQ(caller[1].member, "T.real(I)I");   // 一步直达，不落 W2
    EXPECT_EQ(caller[2].op, Op::IRETURN);
    // 壳体自身的转发也同步压缩
    const auto w1 = methodFacts(resourceBytes(container, "W1.class"), "w", "(I)I");
    ASSERT_EQ(w1.size(), 3u);
    EXPECT_EQ(w1[1].member, "T.real(I)I");
    // 链尾类无命中：字节原样
    EXPECT_EQ(resourceBytes(container, "W2.class"), w2Before);
}

// ── 形态 B：JDK 拆箱（invokestatic 壳 → invokevirtual intValue）──

TEST(MjPassIdentityWrapperInlinerTest, UnwrapsBoxedIntWrapperToVirtual)
{
    ResourceContainer container;
    {
        FixtureBuilder b("W");
        const ca::u16 intValue = b.methodRef("java/lang/Integer", "intValue", "()I");
        Bytes code;
        putU8(code, 0x2A);                                   // aload_0
        putU8(code, 0xB6); putU16(code, intValue);           // invokevirtual intValue
        putU8(code, 0xAC);
        b.addMethod({"u", "(Ljava/lang/Integer;)I", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "W.class", b.assemble());
    }
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("W", "u", "(Ljava/lang/Integer;)I");
        Bytes code;
        putU8(code, 0x2A);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"m", "(Ljava/lang/Integer;)I", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "Caller.class", b.assemble());
    }

    EXPECT_TRUE(runInliner(container).changed());

    const auto facts = methodFacts(resourceBytes(container, "Caller.class"),
                                   "m", "(Ljava/lang/Integer;)I");
    ASSERT_EQ(facts.size(), 3u);
    EXPECT_EQ(facts[0].op, Op::ALOAD_0);
    EXPECT_EQ(facts[1].op, Op::INVOKEVIRTUAL);
    EXPECT_EQ(facts[1].member, "java/lang/Integer.intValue()I");
    EXPECT_EQ(facts[2].op, Op::IRETURN);
}

// ── 形态 A：JDK 装箱（→ invokestatic valueOf）──

TEST(MjPassIdentityWrapperInlinerTest, RewritesBoxWrapperToValueOf)
{
    ResourceContainer container;
    {
        FixtureBuilder b("W");
        const ca::u16 valueOf = b.methodRef("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;");
        Bytes code;
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, valueOf);
        putU8(code, 0xB0);   // areturn
        b.addMethod({"b", "(I)Ljava/lang/Integer;", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "W.class", b.assemble());
    }
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("W", "b", "(I)Ljava/lang/Integer;");
        Bytes code;
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xB0);
        b.addMethod({"m", "(I)Ljava/lang/Integer;", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "Caller.class", b.assemble());
    }

    EXPECT_TRUE(runInliner(container).changed());

    const auto facts = methodFacts(resourceBytes(container, "Caller.class"),
                                   "m", "(I)Ljava/lang/Integer;");
    ASSERT_EQ(facts.size(), 3u);
    EXPECT_EQ(facts[1].op, Op::INVOKESTATIC);
    EXPECT_EQ(facts[1].member, "java/lang/Integer.valueOf(I)Ljava/lang/Integer;");
    EXPECT_EQ(facts[2].op, Op::ARETURN);
}

// ── 形态 B：容器内虚转发（接收者 = 原首实参）──

TEST(MjPassIdentityWrapperInlinerTest, RewritesVirtualForwardToContainerTarget)
{
    ResourceContainer container;
    {
        FixtureBuilder b("T");
        const ca::u16 init = b.methodRef("java/lang/Object", "<init>", "()V");
        Bytes ctor;
        putU8(ctor, 0x2A);
        putU8(ctor, 0xB7); putU16(ctor, init);   // invokespecial
        putU8(ctor, 0xB1);
        b.addMethod({"<init>", "()V", std::move(ctor), 0x0001, {}, 1, 1});
        Bytes twice;
        putU8(twice, 0x1B); putU8(twice, 0x1B);   // iload_1; iload_1
        putU8(twice, 0x60); putU8(twice, 0xAC);
        b.addMethod({"twice", "(I)I", std::move(twice), 0x0001, {}, 2, 2});
        addClassResource(container, "T.class", b.assemble());
    }
    {
        FixtureBuilder b("W");
        const ca::u16 ref = b.methodRef("T", "twice", "(I)I");
        Bytes code;
        putU8(code, 0x2A); putU8(code, 0x1B);
        putU8(code, 0xB6); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"v", "(LT;I)I", std::move(code), 0x0009, {}, 2, 2});
        addClassResource(container, "W.class", b.assemble());
    }
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("W", "v", "(LT;I)I");
        Bytes code;
        putU8(code, 0x2A); putU8(code, 0x1B);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"m", "(LT;I)I", std::move(code), 0x0009, {}, 2, 2});
        addClassResource(container, "Caller.class", b.assemble());
    }

    EXPECT_TRUE(runInliner(container).changed());

    const auto facts = methodFacts(resourceBytes(container, "Caller.class"), "m", "(LT;I)I");
    ASSERT_EQ(facts.size(), 4u);
    EXPECT_EQ(facts[0].op, Op::ALOAD_0);
    EXPECT_EQ(facts[1].op, Op::ILOAD_1);
    EXPECT_EQ(facts[2].op, Op::INVOKEVIRTUAL);
    EXPECT_EQ(facts[2].member, "T.twice(I)I");
    EXPECT_EQ(facts[3].op, Op::IRETURN);
}

// ── 形态 B：接口转发（invokeinterface + count 字节）──

TEST(MjPassIdentityWrapperInlinerTest, RewritesInterfaceForwardWithCountByte)
{
    ResourceContainer container;
    {
        FixtureBuilder b("Iface");
        b.setClassAccess(0x0601);   // public | interface | abstract
        Bytes twice;
        putU8(twice, 0x03); putU8(twice, 0xAC);   // 具体占位体（声明面用）
        b.addMethod({"twice", "(I)I", std::move(twice), 0x0001, {}, 1, 1});
        addClassResource(container, "Iface.class", b.assemble());
    }
    {
        FixtureBuilder b("W");
        const ca::u16 ref = b.interfaceMethodRef("Iface", "twice", "(I)I");
        Bytes code;
        putU8(code, 0x2A); putU8(code, 0x1B);
        putU8(code, 0xB9); putU16(code, ref); putU8(code, 0x02); putU8(code, 0x00);
        putU8(code, 0xAC);
        b.addMethod({"ifv", "(LIface;I)I", std::move(code), 0x0009, {}, 2, 2});
        addClassResource(container, "W.class", b.assemble());
    }
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("W", "ifv", "(LIface;I)I");
        Bytes code;
        putU8(code, 0x2A); putU8(code, 0x1B);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"m", "(LIface;I)I", std::move(code), 0x0009, {}, 2, 2});
        addClassResource(container, "Caller.class", b.assemble());
    }

    EXPECT_TRUE(runInliner(container).changed());

    const auto facts = methodFacts(resourceBytes(container, "Caller.class"), "m", "(LIface;I)I");
    ASSERT_EQ(facts.size(), 4u);
    EXPECT_EQ(facts[2].op, Op::INVOKEINTERFACE);
    EXPECT_EQ(facts[2].member, "Iface.twice(I)I");
    EXPECT_EQ(facts[3].op, Op::IRETURN);

    // count 字节：接收者(1) + int 实参(1) = 2（§6.5 invokeinterface）。
    auto loaded = jbc::ir::makeSessionFromBytes(
        jbc::core::ByteView(resourceBytes(container, "Caller.class")));
    ASSERT_TRUE(loaded.is_ok());
    jbc::ir::ClassSession& session = *loaded.unwrap();
    const jbc::ir::ClassUnit& unit = session.unit();
    for (ca::usize i = 0; i < unit.methods.size(); ++i) {
        const jbc::ir::MethodUnit& mu = unit.methods[i];
        if (unit.symbols().str(mu.name) != "m") continue;
        auto promoted = jbc::ir::promoteMethod(session, i);
        ASSERT_TRUE(promoted.is_ok());
        jbc::ir::CodeUnit* cu = promoted.unwrap().get();
        ASSERT_NE(cu, nullptr);
        for (const jbc::ir::InsnId id : cu->dom.order()) {
            const jbc::ir::Insn* in = cu->dom.peek(id);
            if (in != nullptr && in->op == Op::INVOKEINTERFACE) {
                EXPECT_EQ(in->a, 2);
                EXPECT_EQ(in->b, 0);
            }
        }
    }
}

// ── 环与形态门 ──

TEST(MjPassIdentityWrapperInlinerTest, SkipsMutualRecursionCycle)
{
    ResourceContainer container;
    addClassResource(container, "CA.class",
                     buildForwarder("CA", "a", "(I)I", "CB", "b", "(I)I"));
    addClassResource(container, "CB.class",
                     buildForwarder("CB", "b", "(I)I", "CA", "a", "(I)I"));
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("CA", "a", "(I)I");
        Bytes code;
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"m", "(I)I", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "Caller.class", b.assemble());
    }
    const Bytes callerBefore = resourceBytes(container, "Caller.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "Caller.class"), callerBefore);
}

TEST(MjPassIdentityWrapperInlinerTest, SkipsNonWrapperShapes)
{
    ResourceContainer container;
    {
        FixtureBuilder b("T");
        Bytes real;
        putU8(real, 0x1A); putU8(real, 0x04); putU8(real, 0x60); putU8(real, 0xAC);
        b.addMethod({"real", "(I)I", std::move(real), 0x0009, {}, 2, 1});
        Bytes addM;
        putU8(addM, 0x1A); putU8(addM, 0x1B); putU8(addM, 0x60); putU8(addM, 0xAC);
        b.addMethod({"add", "(II)I", std::move(addM), 0x0009, {}, 2, 2});
        addClassResource(container, "T.class", b.assemble());

        FixtureBuilder ws("WS");
        const ca::u16 fReal = ws.methodRef("T", "real", "(I)I");
        const ca::u16 fAdd  = ws.methodRef("T", "add", "(II)I");
        Bytes extra;   // 多一条算术：非恒等壳
        putU8(extra, 0x1A); putU8(extra, 0x04); putU8(extra, 0x60);
        putU8(extra, 0xB8); putU16(extra, fReal);
        putU8(extra, 0xAC);
        ws.addMethod({"extra", "(I)I", std::move(extra), 0x0009, {}, 2, 1});
        Bytes swapped;   // 装载乱序
        putU8(swapped, 0x1B); putU8(swapped, 0x1A);
        putU8(swapped, 0xB8); putU16(swapped, fAdd);
        putU8(swapped, 0xAC);
        ws.addMethod({"swapped", "(II)I", std::move(swapped), 0x0009, {}, 2, 2});
        Bytes synced;   // synchronized：monitor 是可观测行为
        putU8(synced, 0x1A);
        putU8(synced, 0xB8); putU16(synced, fReal);
        putU8(synced, 0xAC);
        ws.addMethod({"synced", "(I)I", std::move(synced), 0x0029, {}, 1, 1});
        addClassResource(container, "WS.class", ws.assemble());
    }
    {
        FixtureBuilder b("Caller");
        const ca::u16 rExtra  = b.methodRef("WS", "extra", "(I)I");
        const ca::u16 rSwap   = b.methodRef("WS", "swapped", "(II)I");
        const ca::u16 rSynced = b.methodRef("WS", "synced", "(I)I");
        Bytes code;
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, rExtra);
        putU8(code, 0x57);   // pop
        putU8(code, 0x1A); putU8(code, 0x04);
        putU8(code, 0xB8); putU16(code, rSwap);
        putU8(code, 0x57);
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, rSynced);
        putU8(code, 0x57);
        putU8(code, 0xB1);
        b.addMethod({"m", "()V", std::move(code), 0x0009, {}, 2, 1});
        addClassResource(container, "Caller.class", b.assemble());
    }
    const Bytes callerBefore = resourceBytes(container, "Caller.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "Caller.class"), callerBefore);
}

// ── Cat2 槽位推进：long 双槽后 int 落槽 2 ──

TEST(MjPassIdentityWrapperInlinerTest, AdvancesSlotsForCategory2Params)
{
    ResourceContainer container;
    {
        FixtureBuilder b("T");
        const ca::u16 ref = b.methodRef("T", "m", "(JI)J");
        Bytes code;
        putU8(code, 0x1E);   // lload_0
        putU8(code, 0x1C);   // iload_2
        putU8(code, 0xAD);   // lreturn
        b.addMethod({"m", "(JI)J", std::move(code), 0x0009, {}, 2, 4});
        addClassResource(container, "T.class", b.assemble());
    }
    {
        FixtureBuilder b("W");
        const ca::u16 ref = b.methodRef("T", "m", "(JI)J");
        Bytes code;
        putU8(code, 0x1E);
        putU8(code, 0x1C);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAD);
        b.addMethod({"w", "(JI)J", std::move(code), 0x0009, {}, 2, 4});
        addClassResource(container, "W.class", b.assemble());
    }
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("W", "w", "(JI)J");
        Bytes code;
        putU8(code, 0x1E);
        putU8(code, 0x1C);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAD);
        b.addMethod({"m", "(JI)J", std::move(code), 0x0009, {}, 2, 4});
        addClassResource(container, "Caller.class", b.assemble());
    }

    EXPECT_TRUE(runInliner(container).changed());

    const auto facts = methodFacts(resourceBytes(container, "Caller.class"), "m", "(JI)J");
    ASSERT_EQ(facts.size(), 4u);
    EXPECT_EQ(facts[0].op, Op::LLOAD_0);
    EXPECT_EQ(facts[1].op, Op::ILOAD_2);
    EXPECT_EQ(facts[2].op, Op::INVOKESTATIC);
    EXPECT_EQ(facts[2].member, "T.m(JI)J");
    EXPECT_EQ(facts[3].op, Op::LRETURN);
}

// ── 访问门 ──

TEST(MjPassIdentityWrapperInlinerTest, SkipsPrivateTargetInAnotherClass)
{
    ResourceContainer container;
    {
        FixtureBuilder b("P");
        Bytes code;
        putU8(code, 0x1A); putU8(code, 0xAC);
        b.addMethod({"secret", "(I)I", std::move(code), 0x000A, {}, 1, 1});
        addClassResource(container, "P.class", b.assemble());
    }
    addClassResource(container, "W.class",
                     buildForwarder("W", "w", "(I)I", "P", "secret", "(I)I"));
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("W", "w", "(I)I");
        Bytes code;
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"m", "(I)I", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "Caller.class", b.assemble());
    }
    const Bytes callerBefore = resourceBytes(container, "Caller.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "Caller.class"), callerBefore);
}

TEST(MjPassIdentityWrapperInlinerTest, AllowsPrivateTargetInsideCallerClass)
{
    ResourceContainer container;
    {
        FixtureBuilder b("C");
        const ca::u16 secretRef = b.methodRef("C", "secret", "(I)I");
        Bytes secret;
        putU8(secret, 0x1A); putU8(secret, 0xAC);
        b.addMethod({"secret", "(I)I", std::move(secret), 0x000A, {}, 1, 1});
        Bytes w;
        putU8(w, 0x1A);
        putU8(w, 0xB8); putU16(w, secretRef);
        putU8(w, 0xAC);
        b.addMethod({"w", "(I)I", std::move(w), 0x0009, {}, 1, 1});
        Bytes m;
        putU8(m, 0x1A);
        putU8(m, 0xB8); putU16(m, b.methodRef("C", "w", "(I)I"));
        putU8(m, 0xAC);
        b.addMethod({"m", "(I)I", std::move(m), 0x0009, {}, 1, 1});
        addClassResource(container, "C.class", b.assemble());
    }

    EXPECT_TRUE(runInliner(container).changed());

    const auto facts = methodFacts(resourceBytes(container, "C.class"), "m", "(I)I");
    ASSERT_EQ(facts.size(), 3u);
    EXPECT_EQ(facts[1].op, Op::INVOKESTATIC);
    EXPECT_EQ(facts[1].member, "C.secret(I)I");
}

TEST(MjPassIdentityWrapperInlinerTest, SkipsPackagePrivateTargetAcrossPackages)
{
    ResourceContainer container;
    {
        FixtureBuilder b("a/P");
        Bytes code;
        putU8(code, 0x1A); putU8(code, 0xAC);
        b.addMethod({"pkg", "(I)I", std::move(code), 0x0008, {}, 1, 1});
        addClassResource(container, "a/P.class", b.assemble());
    }
    addClassResource(container, "W.class",
                     buildForwarder("W", "w", "(I)I", "a/P", "pkg", "(I)I"));
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("W", "w", "(I)I");
        Bytes code;
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"m", "(I)I", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "Caller.class", b.assemble());
    }
    const Bytes callerBefore = resourceBytes(container, "Caller.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "Caller.class"), callerBefore);
}

TEST(MjPassIdentityWrapperInlinerTest, AllowsPackagePrivateTargetInSamePackage)
{
    ResourceContainer container;
    addClassResource(container, "a/W.class",
                     buildForwarder("a/W", "w", "(I)I", "a/P", "pkg", "(I)I"));
    {
        FixtureBuilder b("a/P");
        Bytes code;
        putU8(code, 0x1A); putU8(code, 0xAC);
        b.addMethod({"pkg", "(I)I", std::move(code), 0x0008, {}, 1, 1});
        addClassResource(container, "a/P.class", b.assemble());
    }
    {
        FixtureBuilder b("a/Caller");
        const ca::u16 ref = b.methodRef("a/W", "w", "(I)I");
        Bytes code;
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"m", "(I)I", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "a/Caller.class", b.assemble());
    }

    EXPECT_TRUE(runInliner(container).changed());

    const auto facts = methodFacts(resourceBytes(container, "a/Caller.class"), "m", "(I)I");
    ASSERT_EQ(facts.size(), 3u);
    EXPECT_EQ(facts[1].member, "a/P.pkg(I)I");
}

// ── 幂等与无壳容器 ──

TEST(MjPassIdentityWrapperInlinerTest, IdempotentOnSecondRun)
{
    ResourceContainer container;
    {
        FixtureBuilder b("T");
        Bytes code;
        putU8(code, 0x1A); putU8(code, 0x04); putU8(code, 0x60); putU8(code, 0xAC);
        b.addMethod({"real", "(I)I", std::move(code), 0x0009, {}, 2, 1});
        addClassResource(container, "T.class", b.assemble());
    }
    addClassResource(container, "W.class",
                     buildForwarder("W", "w", "(I)I", "T", "real", "(I)I"));
    {
        FixtureBuilder b("Caller");
        const ca::u16 ref = b.methodRef("W", "w", "(I)I");
        Bytes code;
        putU8(code, 0x1A);
        putU8(code, 0xB8); putU16(code, ref);
        putU8(code, 0xAC);
        b.addMethod({"m", "(I)I", std::move(code), 0x0009, {}, 1, 1});
        addClassResource(container, "Caller.class", b.assemble());
    }

    EXPECT_TRUE(runInliner(container).changed());
    const Bytes caller1 = resourceBytes(container, "Caller.class");
    const Bytes w1      = resourceBytes(container, "W.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "Caller.class"), caller1);
    EXPECT_EQ(resourceBytes(container, "W.class"), w1);
}

TEST(MjPassIdentityWrapperInlinerTest, LeavesWrapperFreeContainerUntouched)
{
    ResourceContainer container;
    {
        FixtureBuilder b("T");
        Bytes code;
        putU8(code, 0x1A); putU8(code, 0x04); putU8(code, 0x60); putU8(code, 0xAC);
        b.addMethod({"real", "(I)I", std::move(code), 0x0009, {}, 2, 1});
        addClassResource(container, "T.class", b.assemble());
    }
    const Bytes before = resourceBytes(container, "T.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "T.class"), before);
}

// ── 注册面 ──

TEST(MjPassIdentityWrapperInlinerTest, RegistrySeedsGlobalScope)
{
    const auto found = mjbase::spi::defaultPassRegistry().find("IdentityWrapperInliner");
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.unwrap().scope, mjbase::spi::PassScope::Global);
}
