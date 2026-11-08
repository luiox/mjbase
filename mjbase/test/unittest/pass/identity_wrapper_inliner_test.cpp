/// @file        identity_wrapper_inliner_test.cpp
/// @brief       恒等包装不动点内联：容器级多类用例（链压缩 / 装拆箱 / 虚与
///              接口转发 / 环与形态门 / 访问门 / 幂等）。
///
/// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）；语义断言
/// 走「改写 → writeClass → 重读会话 → 指令序列事实」roundtrip 口径。
/// 装配器申报面：Caller.m()V 壳读未播种槽 0（原字节夹具同形）为装配器
/// 放行面（与 static_field_default_inliner 批内申报同源）。

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
#include "shared/jasm_fixture.hpp"

namespace {

using jbc::core::Op;
using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::test::Bytes;

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

/// 单方法类装配便捷形（public super 骨架，超 java/lang/Object）。
Bytes asmOne(const std::string& cls, std::string_view mods, std::string_view name,
             std::string_view desc, std::string_view body)
{
    const std::string src = R"JASM(.super java/lang/Object
.class public super )JASM" + cls + R"JASM( {
  .method )JASM" + std::string(mods) + " " + std::string(name) + " " + std::string(desc)
        + R"JASM( {
    code: {
)JASM" + std::string(body) + R"JASM(    }
  }
}
)JASM";
    return mjbase::test::assembleJasm(src);
}

/// 壳类工厂：static 壳 w(I)I，体内 iload_0 后转发到 target。
Bytes buildForwarder(const char* self, const char* name, const char* desc,
                     const char* targetOwner, const char* targetName, const char* targetDesc)
{
    const std::string invoke = std::string("      invokestatic ") + targetOwner + "."
                               + targetName + " " + targetDesc + "\n      ireturn\n";
    return asmOne(self, "public static", name, desc, "      iload 0\n" + invoke);
}

}   // namespace

// ── 形态 A：静态直通链（跨类路径压缩）──

TEST(MjPassIdentityWrapperInlinerTest, CompressesStaticForwardChain)
{
    ResourceContainer container;
    // T.real(I)I：iload_0; iconst_1; iadd; ireturn
    addClassResource(container, "T.class",
                     asmOne("T", "public static", "real", "(I)I",
                            "      iload 0\n      iconst_1\n      iadd\n      ireturn\n"));
    // W2.w → T.real；W1.w → W2.w；Caller.m → W1.w
    addClassResource(container, "W2.class",
                     buildForwarder("W2", "w", "(I)I", "T", "real", "(I)I"));
    addClassResource(container, "W1.class",
                     buildForwarder("W1", "w", "(I)I", "W2", "w", "(I)I"));
    addClassResource(container, "Caller.class",
                     buildForwarder("Caller", "m", "(I)I", "W1", "w", "(I)I"));
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
    // W.u(Ljava/lang/Integer;)I：aload_0; invokevirtual intValue; ireturn
    addClassResource(container, "W.class",
                     asmOne("W", "public static", "u", "(Ljava/lang/Integer;)I",
                            "      aload 0\n      invokevirtual java/lang/Integer.intValue "
                            "()I\n      ireturn\n"));
    addClassResource(container, "Caller.class",
                     buildForwarder("Caller", "m", "(Ljava/lang/Integer;)I", "W", "u",
                                    "(Ljava/lang/Integer;)I"));

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
    // W.b(I)Ljava/lang/Integer;：iload_0; invokestatic valueOf; areturn
    addClassResource(container, "W.class",
                     asmOne("W", "public static", "b", "(I)Ljava/lang/Integer;",
                            "      iload 0\n      invokestatic java/lang/Integer.valueOf "
                            "(I)Ljava/lang/Integer;\n      areturn\n"));
    addClassResource(container, "Caller.class",
                     buildForwarder("Caller", "m", "(I)Ljava/lang/Integer;", "W", "b",
                                    "(I)Ljava/lang/Integer;"));

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
    // T：<init>（super 构造）+ twice(I)I（iload_1 ×2 + iadd）
    {
        const std::string src = R"JASM(.super java/lang/Object
.class public super T {
  .method public <init> ()V {
    code: {
      aload 0
      invokespecial java/lang/Object.<init> ()V
      return
    }
  }
  .method public twice (I)I {
    code: {
      iload 1
      iload 1
      iadd
      ireturn
    }
  }
}
)JASM";
        addClassResource(container, "T.class", mjbase::test::assembleJasm(src));
    }
    // W.v(LT;I)I：aload_0; iload_1; invokevirtual T.twice; ireturn
    addClassResource(container, "W.class",
                     asmOne("W", "public static", "v", "(LT;I)I",
                            "      aload 0\n      iload 1\n      invokevirtual T.twice "
                            "(I)I\n      ireturn\n"));
    addClassResource(container, "Caller.class",
                     buildForwarder("Caller", "m", "(LT;I)I", "W", "v", "(LT;I)I"));

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
    // Iface 接口：twice(I)I public 具体占位体（声明面用）
    addClassResource(container, "Iface.class",
                     asmOne("Iface", "public", "twice", "(I)I",
                            "      iconst_0\n      ireturn\n"));
    // W.ifv(LIface;I)I：aload_0; iload_1; invokeinterface Iface.twice; ireturn
    //（count 由装配器按 desc 自动发：接收者(1) + int 实参(1) = 2。）
    addClassResource(container, "W.class",
                     asmOne("W", "public static", "ifv", "(LIface;I)I",
                            "      aload 0\n      iload 1\n      invokeinterface Iface.twice "
                            "(I)I\n      ireturn\n"));
    addClassResource(container, "Caller.class",
                     buildForwarder("Caller", "m", "(LIface;I)I", "W", "ifv",
                                    "(LIface;I)I"));

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
    addClassResource(container, "Caller.class",
                     buildForwarder("Caller", "m", "(I)I", "CA", "a", "(I)I"));
    const Bytes callerBefore = resourceBytes(container, "Caller.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "Caller.class"), callerBefore);
}

TEST(MjPassIdentityWrapperInlinerTest, SkipsNonWrapperShapes)
{
    ResourceContainer container;
    {
        // T.real：iload_0; iconst_1; iadd；T.add：iload_0; iload_1; iadd
        const std::string src = R"JASM(.super java/lang/Object
.class public super T {
  .method public static real (I)I {
    code: {
      iload 0
      iconst_1
      iadd
      ireturn
    }
  }
  .method public static add (II)I {
    code: {
      iload 0
      iload 1
      iadd
      ireturn
    }
  }
}
)JASM";
        addClassResource(container, "T.class", mjbase::test::assembleJasm(src));
    }
    {
        // WS 三壳变体：多一条算术（非恒等壳）/装载乱序/synchronized（monitor
        // 是可观测行为）
        const std::string src = R"JASM(.super java/lang/Object
.class public super WS {
  .method public static extra (I)I {
    code: {
      iload 0
      iconst_1
      iadd
      invokestatic T.real (I)I
      ireturn
    }
  }
  .method public static swapped (II)I {
    code: {
      iload 1
      iload 0
      invokestatic T.add (II)I
      ireturn
    }
  }
  .method public static synchronized synced (I)I {
    code: {
      iload 0
      invokestatic T.real (I)I
      ireturn
    }
  }
}
)JASM";
        addClassResource(container, "WS.class", mjbase::test::assembleJasm(src));
    }
    {
        // Caller.m()V：三站点各 invokestatic + pop 收口（壳读未播种槽 0，
        // 装配器放行面）
        const std::string src = R"JASM(.super java/lang/Object
.class public super Caller {
  .method public static m ()V {
    code: {
      iload 0
      invokestatic WS.extra (I)I
      pop
      iload 0
      iconst_1
      invokestatic WS.swapped (II)I
      pop
      iload 0
      invokestatic WS.synced (I)I
      pop
      return
    }
  }
}
)JASM";
        addClassResource(container, "Caller.class", mjbase::test::assembleJasm(src));
    }
    const Bytes callerBefore = resourceBytes(container, "Caller.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "Caller.class"), callerBefore);
}

// ── Cat2 槽位推进：long 双槽后 int 落槽 2 ──

TEST(MjPassIdentityWrapperInlinerTest, AdvancesSlotsForCategory2Params)
{
    ResourceContainer container;
    // T.m(JI)J：lload_0; iload_2; lreturn
    addClassResource(container, "T.class",
                     asmOne("T", "public static", "m", "(JI)J",
                            "      lload 0\n      iload 2\n      lreturn\n"));
    // W.w(JI)J：同形转发
    addClassResource(container, "W.class",
                     asmOne("W", "public static", "w", "(JI)J",
                            "      lload 0\n      iload 2\n      invokestatic T.m (JI)J\n"
                            "      lreturn\n"));
    addClassResource(container, "Caller.class",
                     asmOne("Caller", "public static", "m", "(JI)J",
                            "      lload 0\n      iload 2\n      invokestatic W.w (JI)J\n"
                            "      lreturn\n"));

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
    // P.secret(I)I private static：iload_0; ireturn
    addClassResource(container, "P.class",
                     asmOne("P", "private static", "secret", "(I)I",
                            "      iload 0\n      ireturn\n"));
    addClassResource(container, "W.class",
                     buildForwarder("W", "w", "(I)I", "P", "secret", "(I)I"));
    addClassResource(container, "Caller.class",
                     buildForwarder("Caller", "m", "(I)I", "W", "w", "(I)I"));
    const Bytes callerBefore = resourceBytes(container, "Caller.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "Caller.class"), callerBefore);
}

TEST(MjPassIdentityWrapperInlinerTest, AllowsPrivateTargetInsideCallerClass)
{
    ResourceContainer container;
    // C：secret private static + w public static（转发）+ m public static
    const std::string src = R"JASM(.super java/lang/Object
.class public super C {
  .method private static secret (I)I {
    code: {
      iload 0
      ireturn
    }
  }
  .method public static w (I)I {
    code: {
      iload 0
      invokestatic C.secret (I)I
      ireturn
    }
  }
  .method public static m (I)I {
    code: {
      iload 0
      invokestatic C.w (I)I
      ireturn
    }
  }
}
)JASM";
    addClassResource(container, "C.class", mjbase::test::assembleJasm(src));

    EXPECT_TRUE(runInliner(container).changed());

    const auto facts = methodFacts(resourceBytes(container, "C.class"), "m", "(I)I");
    ASSERT_EQ(facts.size(), 3u);
    EXPECT_EQ(facts[1].op, Op::INVOKESTATIC);
    EXPECT_EQ(facts[1].member, "C.secret(I)I");
}

TEST(MjPassIdentityWrapperInlinerTest, SkipsPackagePrivateTargetAcrossPackages)
{
    ResourceContainer container;
    // a/P.pkg(I)I 包私有 static（跨包不可证）
    addClassResource(container, "a/P.class",
                     asmOne("a/P", "static", "pkg", "(I)I",
                            "      iload 0\n      ireturn\n"));
    addClassResource(container, "W.class",
                     buildForwarder("W", "w", "(I)I", "a/P", "pkg", "(I)I"));
    addClassResource(container, "Caller.class",
                     buildForwarder("Caller", "m", "(I)I", "W", "w", "(I)I"));
    const Bytes callerBefore = resourceBytes(container, "Caller.class");

    EXPECT_FALSE(runInliner(container).changed());
    EXPECT_EQ(resourceBytes(container, "Caller.class"), callerBefore);
}

TEST(MjPassIdentityWrapperInlinerTest, AllowsPackagePrivateTargetInSamePackage)
{
    ResourceContainer container;
    addClassResource(container, "a/W.class",
                     buildForwarder("a/W", "w", "(I)I", "a/P", "pkg", "(I)I"));
    addClassResource(container, "a/P.class",
                     asmOne("a/P", "static", "pkg", "(I)I",
                            "      iload 0\n      ireturn\n"));
    addClassResource(container, "a/Caller.class",
                     buildForwarder("a/Caller", "m", "(I)I", "a/W", "w", "(I)I"));

    EXPECT_TRUE(runInliner(container).changed());

    const auto facts = methodFacts(resourceBytes(container, "a/Caller.class"), "m", "(I)I");
    ASSERT_EQ(facts.size(), 3u);
    EXPECT_EQ(facts[1].member, "a/P.pkg(I)I");
}

// ── 幂等与无壳容器 ──

TEST(MjPassIdentityWrapperInlinerTest, IdempotentOnSecondRun)
{
    ResourceContainer container;
    // T.real(I)I：iload_0; iconst_1; iadd; ireturn
    addClassResource(container, "T.class",
                     asmOne("T", "public static", "real", "(I)I",
                            "      iload 0\n      iconst_1\n      iadd\n      ireturn\n"));
    addClassResource(container, "W.class",
                     buildForwarder("W", "w", "(I)I", "T", "real", "(I)I"));
    addClassResource(container, "Caller.class",
                     buildForwarder("Caller", "m", "(I)I", "W", "w", "(I)I"));

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
    addClassResource(container, "T.class",
                     asmOne("T", "public static", "real", "(I)I",
                            "      iload 0\n      iconst_1\n      iadd\n      ireturn\n"));
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
