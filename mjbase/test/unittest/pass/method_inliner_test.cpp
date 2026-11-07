/// @file        method_inliner_test.cpp
/// @brief       基础版小方法内联：容器级多类用例（static/跨类/private 形态、
///              v2 去虚化/nestmate/protected/switch/浅循环面、前向分支体、
///              门面拒绝逐条反证、幂等、注册面）。
///
/// 载体是 ResourceContainer + 手工字节夹具（FixtureBuilder）；语义断言走
/// 「改写 → 重读会话 → 指令序列事实」roundtrip 口径；拒绝面以字节逐位
/// 保持钉死。行为等价的 JVM 硬验收在 method_inliner_jvm_test.cpp。
///
/// v2 拼接形：非 static 被调统一带接收者 null 守卫（ALOAD fresh 槽 →
/// IFNONNULL 越过 → ACONST_NULL → ATHROW）——invoke 家族调用点接收者
/// null 检查的显式保留（头注「接收者 null 守卫」节），事实断言一并钉住。

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/method_inliner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"

using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::test::Bytes;
using mjbase::test::ExcSpec;
using mjbase::test::FixtureBuilder;
using mjbase::test::FixtureMethod;
using mjbase::test::putU16;
using mjbase::test::putU32;
using mjbase::test::putU8;

namespace {

using jbc::core::Op;

constexpr ca::u16 kAccPublic       = 0x0001;
constexpr ca::u16 kAccPrivate      = 0x0002;
constexpr ca::u16 kAccProtected    = 0x0004;
constexpr ca::u16 kAccStatic       = 0x0008;
constexpr ca::u16 kAccFinal        = 0x0010;
constexpr ca::u16 kAccInterface    = 0x0200;
constexpr ca::u16 kAccAbstract     = 0x0400;
constexpr ca::u16 kAccSynchronized = 0x0020;

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
    mjbase::pass::MethodInliner        pass;
    return pass.run(container, gam);
}

/// 重读事实：指令 op + 局部槽操作数 a + invoke/字段族语义目标。
struct InsnFact
{
    Op          op = Op::NOP;
    ca::i32     a  = 0;
    std::string member;
};

std::vector<InsnFact> methodFacts(const Bytes& bytes, std::string_view name,
                                  std::string_view desc, ca::u16* maxLocals = nullptr)
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
        if (maxLocals != nullptr) *maxLocals = cu->maxLocals;
        for (const jbc::ir::InsnId id : cu->dom.order()) {
            const jbc::ir::Insn* in = cu->dom.peek(id);
            if (in == nullptr) continue;
            InsnFact fact;
            fact.op = in->op;
            fact.a  = in->a;
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

void expectFacts(const std::vector<InsnFact>& facts, const std::vector<InsnFact>& expect)
{
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i].op) << "index " << i;
        EXPECT_EQ(facts[i].a, expect[i].a) << "index " << i;
    }
}

/// static 纯计算 callee：`(I)I → iload_0; iconst_2; imul; ireturn`
/// `()I → iconst_1; ireturn`（零参形态，无局部无分支）。
Bytes buildDouble(const char* self, const char* name, ca::u16 access, const char* desc = "(I)I")
{
    FixtureBuilder b(self);
    Bytes code;
    if (std::string_view(desc) == "()I") {
        putU8(code, 0x04);          // iconst_1
        putU8(code, 0xAC);          // ireturn
        b.addMethod({name, desc, std::move(code), access, {}, 1, 0});
    } else {
        putU8(code, 0x1A);          // iload_0
        putU8(code, 0x05);          // iconst_2
        putU8(code, 0x68);          // imul
        putU8(code, 0xAC);          // ireturn
        b.addMethod({name, desc, std::move(code), access, {}, 2, 1});
    }
    return b.assemble();
}

/// nest 属性载荷：NestHost = u2 宿主类 CP；NestMembers = u2 计数 + u2 列表。
Bytes nestHostPayload(FixtureBuilder& b, std::string_view host)
{
    Bytes payload;
    putU16(payload, b.cls(host));
    return payload;
}
Bytes nestMembersPayload(FixtureBuilder& b, const std::vector<std::string_view>& members)
{
    Bytes payload;
    putU16(payload, static_cast<ca::u16>(members.size()));
    for (std::string_view m : members) putU16(payload, b.cls(m));
    return payload;
}

}   // namespace

// ── 正例：同类 static 小方法内联（v1 基础面）──

TEST(MjPassMethodInlinerTest, InlinesStaticSameClassSmallPure)
{
    ResourceContainer container;
    // T.add(II)I：iload_0; iload_1; iadd; ireturn
    {
        FixtureBuilder b("T");
        Bytes callee;
        putU8(callee, 0x1A);        // iload_0
        putU8(callee, 0x1B);        // iload_1
        putU8(callee, 0x60);        // iadd
        putU8(callee, 0xAC);        // ireturn
        b.addMethod({"add", "(II)I", std::move(callee), kAccPublic | kAccStatic, {}, 2, 2});
        // T.m(II)I：iload_0; iload_1; invokestatic T.add; ireturn
        const ca::u16 ref = b.methodRef("T", "add", "(II)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0x1B);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(II)I", std::move(caller), kAccPublic | kAccStatic, {}, 2, 2});
        addClassResource(container, "T.class", b.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved()) << "应有改写面";

    ca::u16 maxLocals = 0;
    const auto facts  = methodFacts(resourceBytes(container, "T.class"), "m", "(II)I",
                                    &maxLocals);
    // 拼接形：原实参装载（quick 形原样保留）→ 实参逆序落槽（base=2）→
    // 克隆体（quick 已升泛形）→ 出口 GOTO → 原 ireturn。static 被调无
    // 接收者 null 守卫。
    const std::vector<InsnFact> expect = {
        {Op::ILOAD_0, 0, {}},        // iload_0（原实参装载，caller 原指令）
        {Op::ILOAD_1, 0, {}},        // iload_1
        {Op::ISTORE, 3, {}},         // 落槽：被调槽 1（栈顶最后参数）
        {Op::ISTORE, 2, {}},         // 落槽：被调槽 0
        {Op::ILOAD, 2, {}},          // 克隆体 iload_0 → 槽 2
        {Op::ILOAD, 3, {}},          // 克隆体 iload_1 → 槽 3
        {Op::IADD, 0, {}},
        {Op::GOTO, 0, {}},           // 克隆体 ireturn → 统一出口
        {Op::IRETURN, 0, {}},        // 原 caller 返回
    };
    expectFacts(facts, expect);
    EXPECT_EQ(maxLocals, 4) << "maxLocals = base(2) + 被调槽 2";
}

// ── 正例：跨类 static（双 public、无 clinit）+ ldc 常量跨会话 re-intern ──

TEST(MjPassMethodInlinerTest, InlinesStaticCrossClassWithConstReintern)
{
    ResourceContainer container;
    // H.k()I：bipush 100; ireturn（纯常量）
    {
        FixtureBuilder b("H");
        Bytes callee;
        putU8(callee, 0x10);        // bipush（opcode 0x10）
        putU8(callee, 100);         // 操作数字节
        putU8(callee, 0xAC);
        b.addMethod({"k", "()I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "H.class", b.assemble());
    }
    // C.use()I：invokestatic H.k; ireturn
    {
        FixtureBuilder b("C");
        const ca::u16 ref = b.methodRef("H", "k", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"use", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "C.class", b.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "C.class"), "use", "()I");
    // bipush 无 CP 载荷（op 原样随迁）；ldc 族才走跨会话 re-intern。
    const std::vector<Op> expect = {Op::BIPUSH, Op::GOTO, Op::IRETURN};
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i]) << "index " << i;
    }
    // 被调壳保留（删除延后删除族管线）。
    const auto calleeFacts = methodFacts(resourceBytes(container, "H.class"), "k", "()I");
    ASSERT_EQ(calleeFacts.size(), 2u);
    EXPECT_EQ(calleeFacts[0].op, Op::BIPUSH);
    EXPECT_EQ(calleeFacts[1].op, Op::IRETURN);
}

// ── 正例：同类 private 实例方法（invokespecial，接收者 null 守卫随拼接）──

TEST(MjPassMethodInlinerTest, InlinesPrivateSameClassViaInvokespecial)
{
    ResourceContainer container;
    {
        FixtureBuilder b("K");
        // K.dbl(I)I private：iload_1; iconst_2; imul; ireturn（槽 0=this, 1=param）
        Bytes callee;
        putU8(callee, 0x1B);        // iload_1
        putU8(callee, 0x05);        // iconst_2
        putU8(callee, 0x68);        // imul
        putU8(callee, 0xAC);
        b.addMethod({"dbl", "(I)I", std::move(callee), kAccPrivate, {}, 2, 2});
        // K.run(I)I：aload_0; iload_1; invokespecial K.dbl; ireturn
        const ca::u16 ref = b.methodRef("K", "dbl", "(I)I");
        Bytes caller;
        putU8(caller, 0x2A);        // aload_0
        putU8(caller, 0x1B);        // iload_1
        putU8(caller, 0xB7); putU16(caller, ref);   // invokespecial
        putU8(caller, 0xAC);
        b.addMethod({"run", "(I)I", std::move(caller), kAccPublic, {}, 2, 2});
        addClassResource(container, "K.class", b.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "K.class"), "run", "(I)I");
    // 拼接形：原接收者/实参装载 → 逆序落槽（base=2）→ 接收者 null 守卫 →
    // 克隆体 → 出口 GOTO → 原 ireturn。守卫保留 invoke 家族调用点的接收者
    // null 检查（克隆体白名单指令不解引用接收者，缺失即 null 路径 NPE 消失）。
    const std::vector<InsnFact> expect = {
        {Op::ALOAD_0, 0, {}},        // 原接收者装载（caller 原指令）
        {Op::ILOAD_1, 0, {}},        // 原实参装载
        {Op::ISTORE, 3, {}},         // 落槽：被调槽 1（param，栈顶）
        {Op::ASTORE, 2, {}},         // 落槽：被调槽 0（receiver）
        {Op::ALOAD, 2, {}},          // null 守卫：aload 接收者 fresh 槽
        {Op::IFNONNULL, 0, {}},      // 非 null → 越过抛出块
        {Op::ACONST_NULL, 0, {}},
        {Op::ATHROW, 0, {}},         // null → NPE（同型同点）
        {Op::ILOAD, 3, {}},          // 克隆体 iload_1 → 槽 3
        {Op::ICONST_2, 0, {}},
        {Op::IMUL, 0, {}},
        {Op::GOTO, 0, {}},
        {Op::IRETURN, 0, {}},
    };
    expectFacts(facts, expect);
}

// ── 正例：前向分支体（if + goto 目标随体迁移）──

TEST(MjPassMethodInlinerTest, InlinesBodyWithForwardBranches)
{
    ResourceContainer container;
    {
        FixtureBuilder b("P");
        // P.pick(Z)I：iload_0; ifeq L2; iconst_3; goto L3; L2: iconst_4; L3: ireturn
        //   字节：pc0 iload_0(1B)；pc1 ifeq(+7→pc8)(3B)；pc4 iconst_3(1B)；
        //         pc5 goto(+4→pc9)(3B)；pc8 iconst_4(1B)；pc9 ireturn
        Bytes callee;
        putU8(callee, 0x1A);                          // iload_0
        putU8(callee, 0x99); putU16(callee, 7);       // ifeq → pc8（else 分支 iconst_4）
        putU8(callee, 0x06);                          // iconst_3
        putU8(callee, 0xA7); putU16(callee, 4);       // goto → pc9（ireturn）
        putU8(callee, 0x07);                          // iconst_4
        putU8(callee, 0xAC);                          // ireturn
        b.addMethod({"pick", "(Z)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 1});
        const ca::u16 ref = b.methodRef("P", "pick", "(Z)I");
        Bytes caller;
        putU8(caller, 0x04);                          // iconst_1（true）
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"use", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "P.class", b.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "P.class"), "use", "()I");
    // 拼接形：原实参 iconst_1 → 落槽 → 克隆体（iload 升泛形 + 双前向分支
    // 随迁 + ireturn 化出口 GOTO）→ 原 ireturn。
    const std::vector<Op> expect = {
        Op::ICONST_1, Op::ISTORE, Op::ILOAD, Op::IFEQ, Op::ICONST_3, Op::GOTO,
        Op::ICONST_4, Op::GOTO, Op::IRETURN,
    };
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i]) << "index " << i;
    }
}

// ── 正例（v2）：final 方法去虚化（同类 invokevirtual）──

TEST(MjPassMethodInlinerTest, InlinesVirtualFinalMethodSameClass)
{
    ResourceContainer container;
    {
        // 单类装配：fin（final 方法，去虚化证据）+ run（调用方）。
        // 注意同一 URI 重复 addResource 会整体替换——被调与调用方必须
        // 进同一次 assemble。
        FixtureBuilder b("F");
        Bytes callee;
        putU8(callee, 0x1B);        // iload_1
        putU8(callee, 0x05);        // iconst_2
        putU8(callee, 0x68);        // imul
        putU8(callee, 0xAC);        // ireturn
        b.addMethod({"fin", "(I)I", std::move(callee), kAccPublic | kAccFinal, {}, 2, 2});
        // F.run(I)I：aload_0; iload_1; invokevirtual F.fin; ireturn
        // javac 11+ 对 private/final 实例方法发 invokevirtual（JEP 181 口径）。
        const ca::u16 ref = b.methodRef("F", "fin", "(I)I");
        Bytes caller;
        putU8(caller, 0x2A);        // aload_0
        putU8(caller, 0x1B);        // iload_1
        putU8(caller, 0xB6); putU16(caller, ref);   // invokevirtual
        putU8(caller, 0xAC);
        b.addMethod({"run", "(I)I", std::move(caller), kAccPublic, {}, 2, 2});
        addClassResource(container, "F.class", b.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "F.class"), "run", "(I)I");
    // 去虚化证据：声明方法 ACC_FINAL → 任何子类不可覆盖 → 唯一目标 = 声明体。
    // 拼接形与 private 实例面一致（接收者 null 守卫 + 克隆体 + 出口）。
    const std::vector<InsnFact> expect = {
        {Op::ALOAD_0, 0, {}},
        {Op::ILOAD_1, 0, {}},
        {Op::ISTORE, 3, {}},
        {Op::ASTORE, 2, {}},
        {Op::ALOAD, 2, {}},          // null 守卫
        {Op::IFNONNULL, 0, {}},
        {Op::ACONST_NULL, 0, {}},
        {Op::ATHROW, 0, {}},
        {Op::ILOAD, 3, {}},
        {Op::ICONST_2, 0, {}},
        {Op::IMUL, 0, {}},
        {Op::GOTO, 0, {}},
        {Op::IRETURN, 0, {}},
    };
    expectFacts(facts, expect);
}

// ── 正例（v2）：private 方法经 invokevirtual（JEP 181 同类形态）──

TEST(MjPassMethodInlinerTest, InlinesVirtualPrivateSameClass)
{
    ResourceContainer container;
    {
        // 单类装配：pv（private 实例）+ run（调用方）——同一 URI 重复
        // addResource 会整体替换，两个方法必须进同一次 assemble。
        FixtureBuilder b("PV");
        Bytes callee;
        putU8(callee, 0x1B);        // iload_1
        putU8(callee, 0x05);        // iconst_2
        putU8(callee, 0x68);        // imul
        putU8(callee, 0xAC);        // ireturn
        b.addMethod({"pv", "(I)I", std::move(callee), kAccPrivate, {}, 2, 2});
        // PV.run(I)I：aload_0; iload_1; invokevirtual PV.pv; ireturn
        //（javac 11+ 对同类 private 实例调用发 invokevirtual，探针实测。）
        const ca::u16 ref = b.methodRef("PV", "pv", "(I)I");
        Bytes caller;
        putU8(caller, 0x2A);
        putU8(caller, 0x1B);
        putU8(caller, 0xB6); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"run", "(I)I", std::move(caller), kAccPublic, {}, 2, 2});
        addClassResource(container, "PV.class", b.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "PV.class"), "run", "(I)I");
    // 去虚化证据：private 声明不参与虚分派 → 唯一目标。同类调用方免访问面门。
    const std::vector<Op> expect = {
        Op::ALOAD_0, Op::ILOAD_1, Op::ISTORE, Op::ASTORE,
        Op::ALOAD, Op::IFNONNULL, Op::ACONST_NULL, Op::ATHROW,
        Op::ILOAD, Op::ICONST_2, Op::IMUL, Op::GOTO, Op::IRETURN,
    };
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i]) << "index " << i;
    }
}

// ── 正例（v2）：final 类的虚方法去虚化（跨类，接收者运行时类型恰为该类）──

TEST(MjPassMethodInlinerTest, InlinesVirtualMethodOfFinalClassCrossClass)
{
    ResourceContainer container;
    {
        // FC final 类：v(I)I public（方法本身非 final——final 类背书唯一实现）
        FixtureBuilder bf("FC");
        bf.setClassAccess(kAccPublic | kAccFinal);
        Bytes callee;
        putU8(callee, 0x1B);        // iload_1
        putU8(callee, 0x06);        // iconst_3
        putU8(callee, 0x60);        // iadd
        putU8(callee, 0xAC);
        bf.addMethod({"v", "(I)I", std::move(callee), kAccPublic, {}, 2, 2});
        addClassResource(container, "FC.class", bf.assemble());
    }
    {
        FixtureBuilder bc("CU");
        // CU.use(LFC;I)I：aload_1; iload_2; invokevirtual FC.v; ireturn
        const ca::u16 ref = bc.methodRef("FC", "v", "(I)I");
        Bytes caller;
        putU8(caller, 0x2B);        // aload_1
        putU8(caller, 0x1C);        // iload_2
        putU8(caller, 0xB6); putU16(caller, ref);
        putU8(caller, 0xAC);
        bc.addMethod({"use", "(LFC;I)I", std::move(caller), kAccPublic | kAccStatic, {}, 3, 3});
        addClassResource(container, "CU.class", bc.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "CU.class"), "use", "(LFC;I)I");
    // 跨类去虚化面：publicOwner/publicMethod + 体白名单 + final 类证据。
    const std::vector<Op> expect = {
        Op::ALOAD_1, Op::ILOAD_2, Op::ISTORE, Op::ASTORE,
        Op::ALOAD, Op::IFNONNULL, Op::ACONST_NULL, Op::ATHROW,
        Op::ILOAD, Op::ICONST_3, Op::IADD, Op::GOTO, Op::IRETURN,
    };
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i]) << "index " << i;
    }
}

// ── 正例（v2）：nestmate 跨类 private static（invokestatic + 属性链判据）──

TEST(MjPassMethodInlinerTest, InlinesNestmatePrivateStatic)
{
    ResourceContainer container;
    {
        // H：宿主，NestMembers = [A, B]（无方法，仅承载属性）
        FixtureBuilder bh("H");
        bh.addClassAttr({"NestMembers", nestMembersPayload(bh, {"A", "B"})});
        addClassResource(container, "H.class", bh.assemble());
        // A：NestHost → H；priv(I)I private static：iload_0; iconst_2; imul; ireturn
        FixtureBuilder ba("A");
        ba.addClassAttr({"NestHost", nestHostPayload(ba, "H")});
        Bytes callee;
        putU8(callee, 0x1A);
        putU8(callee, 0x05);
        putU8(callee, 0x68);
        putU8(callee, 0xAC);
        ba.addMethod({"priv", "(I)I", std::move(callee), kAccPrivate | kAccStatic, {}, 1, 1});
        addClassResource(container, "A.class", ba.assemble());
        // B：NestHost → H；m(I)I：iload_0; invokestatic A.priv; ireturn
        //（javac 11+ 跨 nest private static 调用形态，探针实测。）
        FixtureBuilder bb("B");
        bb.addClassAttr({"NestHost", nestHostPayload(bb, "H")});
        const ca::u16 ref = bb.methodRef("A", "priv", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        bb.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "B.class", bb.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "B.class"), "m", "(I)I");
    // nestmate 面：双 public 门放行为属性链门；static 被调无 null 守卫。
    const std::vector<Op> expect = {Op::ILOAD_0, Op::ISTORE, Op::ILOAD, Op::ICONST_2,
                                    Op::IMUL, Op::GOTO, Op::IRETURN};
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i]) << "index " << i;
    }
}

// ── 正例（v2）：nestmate 跨类 private 实例（invokevirtual + null 守卫）──

TEST(MjPassMethodInlinerTest, InlinesNestmatePrivateInstance)
{
    ResourceContainer container;
    {
        FixtureBuilder bh("NH");
        bh.addClassAttr({"NestMembers", nestMembersPayload(bh, {"NA", "NB"})});
        addClassResource(container, "NH.class", bh.assemble());
        // NA：priv(I)I private 实例
        FixtureBuilder ba("NA");
        ba.addClassAttr({"NestHost", nestHostPayload(ba, "NH")});
        Bytes callee;
        putU8(callee, 0x1B);
        putU8(callee, 0x05);
        putU8(callee, 0x68);
        putU8(callee, 0xAC);
        ba.addMethod({"priv", "(I)I", std::move(callee), kAccPrivate, {}, 2, 2});
        addClassResource(container, "NA.class", ba.assemble());
        // NB：m(LNA;I)I：aload_1; iload_2; invokevirtual NA.priv; ireturn
        //（javac 11+ 跨 nest private 实例调用形态，探针实测。）
        FixtureBuilder bb("NB");
        bb.addClassAttr({"NestHost", nestHostPayload(bb, "NH")});
        const ca::u16 ref = bb.methodRef("NA", "priv", "(I)I");
        Bytes caller;
        putU8(caller, 0x2B);        // aload_1（receiver = NA 实参）
        putU8(caller, 0x1C);        // iload_2
        putU8(caller, 0xB6); putU16(caller, ref);
        putU8(caller, 0xAC);
        bb.addMethod({"m", "(LNA;I)I", std::move(caller), kAccPublic | kAccStatic, {}, 3, 3});
        addClassResource(container, "NB.class", bb.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "NB.class"), "m", "(LNA;I)I");
    const std::vector<InsnFact> expect = {
        {Op::ALOAD_1, 0, {}},
        {Op::ILOAD_2, 0, {}},
        {Op::ISTORE, 4, {}},         // base(3) + 被调槽 1
        {Op::ASTORE, 3, {}},         // base(3) + 被调槽 0（receiver）
        {Op::ALOAD, 3, {}},          // null 守卫（接收者是任意实参，检查保留）
        {Op::IFNONNULL, 0, {}},
        {Op::ACONST_NULL, 0, {}},
        {Op::ATHROW, 0, {}},
        {Op::ILOAD, 4, {}},
        {Op::ICONST_2, 0, {}},
        {Op::IMUL, 0, {}},
        {Op::GOTO, 0, {}},
        {Op::IRETURN, 0, {}},
    };
    expectFacts(facts, expect);
}

// ── 正例（v2）：protected 跨类（子类链证据；JVM protected 规则的子类支）──

TEST(MjPassMethodInlinerTest, InlinesProtectedStaticViaSubclass)
{
    ResourceContainer container;
    {
        // D.k()I protected static：iconst_1; ireturn
        FixtureBuilder bd("D");
        Bytes callee;
        putU8(callee, 0x04);
        putU8(callee, 0xAC);
        bd.addMethod({"k", "()I", std::move(callee), kAccProtected | kAccStatic, {}, 1, 0});
        addClassResource(container, "D.class", bd.assemble());
        // S extends D：m()I：invokestatic D.k; ireturn
        FixtureBuilder bs("S");
        bs.setSuper("D");
        const ca::u16 ref = bs.methodRef("D", "k", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        bs.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "S.class", bs.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "S.class"), "m", "()I");
    const std::vector<Op> expect = {Op::ICONST_1, Op::GOTO, Op::IRETURN};
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i]) << "index " << i;
    }
}

// ── 正例（v2）：private 接口方法经 invokeinterface（javac 17 实测形态）──

TEST(MjPassMethodInlinerTest, InlinesPrivateInterfaceMethod)
{
    ResourceContainer container;
    {
        FixtureBuilder bi("PI");
        bi.setClassAccess(kAccPublic | kAccInterface | kAccAbstract);
        // PI.p(I)I private：iload_1; iconst_2; imul; ireturn
        Bytes callee;
        putU8(callee, 0x1B);
        putU8(callee, 0x05);
        putU8(callee, 0x68);
        putU8(callee, 0xAC);
        bi.addMethod({"p", "(I)I", std::move(callee), kAccPrivate, {}, 2, 2});
        // PI.use(I)I public default：aload_0; iload_1; invokeinterface PI.p;
        // ireturn（invokeinterface count = 接收者 + 参数 = 2。）
        const ca::u16 ref = bi.interfaceMethodRef("PI", "p", "(I)I");
        Bytes caller;
        putU8(caller, 0x2A);                          // aload_0
        putU8(caller, 0x1B);                          // iload_1
        putU8(caller, 0xB9); putU16(caller, ref);     // invokeinterface
        putU8(caller, 0x02);                          // count = 2
        putU8(caller, 0x00);
        putU8(caller, 0xAC);
        bi.addMethod({"use", "(I)I", std::move(caller), kAccPublic, {}, 2, 2});
        addClassResource(container, "PI.class", bi.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "PI.class"), "use", "(I)I");
    // 接口 private 方法不参与分派（探针实测：同签名 public 实现不被命中）→
    // 唯一目标；声明者 = 接口自身 = 调用方，免访问面门。
    const std::vector<InsnFact> expect = {
        {Op::ALOAD_0, 0, {}},
        {Op::ILOAD_1, 0, {}},
        {Op::ISTORE, 3, {}},
        {Op::ASTORE, 2, {}},
        {Op::ALOAD, 2, {}},          // null 守卫
        {Op::IFNONNULL, 0, {}},
        {Op::ACONST_NULL, 0, {}},
        {Op::ATHROW, 0, {}},
        {Op::ILOAD, 3, {}},
        {Op::ICONST_2, 0, {}},
        {Op::IMUL, 0, {}},
        {Op::GOTO, 0, {}},
        {Op::IRETURN, 0, {}},
    };
    expectFacts(facts, expect);
}

// ── 正例（v2）：浅循环被调体（回边随体迁移，帽内）──

TEST(MjPassMethodInlinerTest, InlinesLoopCalleeBody)
{
    ResourceContainer container;
    {
        FixtureBuilder b("LP");
        // LP.f(I)I：s=0; i=0; while (i < n) { s += i; i++ } 返回 s
        //   pc0 iconst_0；pc1 istore_1；pc2 iconst_0；pc3 istore_2；
        //   pc4 iload_2；pc5 iload_0；pc6 if_icmpge(+13→pc19)；pc9 iload_1；
        //   pc10 iload_2；pc11 iadd；pc12 istore_1；pc13 iinc 2,1；
        //   pc16 goto(-12→pc4)；pc19 iload_1；pc20 ireturn
        Bytes callee;
        putU8(callee, 0x03);                                  // iconst_0
        putU8(callee, 0x3C);                                  // istore_1
        putU8(callee, 0x03);                                  // iconst_0
        putU8(callee, 0x3D);                                  // istore_2
        putU8(callee, 0x1C);                                  // iload_2
        putU8(callee, 0x1A);                                  // iload_0
        putU8(callee, 0xA2); putU16(callee, 13);              // if_icmpge → pc19
        putU8(callee, 0x1B);                                  // iload_1
        putU8(callee, 0x1C);                                  // iload_2
        putU8(callee, 0x60);                                  // iadd
        putU8(callee, 0x3C);                                  // istore_1
        putU8(callee, 0x84); putU8(callee, 2); putU8(callee, 1);   // iinc 2,1
        putU8(callee, 0xA7); putU16(callee, 0xFFF4);          // goto → pc4（回边）
        putU8(callee, 0x1B);                                  // iload_1
        putU8(callee, 0xAC);                                  // ireturn
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 2, 3});
        const ca::u16 ref = b.methodRef("LP", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x06);                                  // iconst_3
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "LP.class", b.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "LP.class"), "m", "()I",
                                   nullptr);
    // 循环体整体平移（base=0：调用方 m()I maxLocals=0）：回边目标随迁到
    // 克隆头；回边数 1 ≤ 帽 2。
    const std::vector<InsnFact> expect = {
        {Op::ICONST_3, 0, {}},
        {Op::ISTORE, 0, {}},         // 落槽：被调槽 0（n）
        {Op::ICONST_0, 0, {}},       // 克隆体：s = 0（被调槽 1 → 槽 1）
        {Op::ISTORE, 1, {}},
        {Op::ICONST_0, 0, {}},       // i = 0（被调槽 2 → 槽 2）
        {Op::ISTORE, 2, {}},
        {Op::ILOAD, 2, {}},          // 循环头
        {Op::ILOAD, 0, {}},
        {Op::IF_ICMPGE, 0, {}},      // 前向出口（随体迁移）
        {Op::ILOAD, 1, {}},
        {Op::ILOAD, 2, {}},
        {Op::IADD, 0, {}},
        {Op::ISTORE, 1, {}},
        {Op::IINC, 2, {}},           // iinc 槽随体平移
        {Op::GOTO, 0, {}},           // 回边 → 克隆循环头
        {Op::ILOAD, 1, {}},          // 出口：return s
        {Op::GOTO, 0, {}},           // ireturn → 统一出口
        {Op::IRETURN, 0, {}},
    };
    expectFacts(facts, expect);
}

// ── 正例（v2）：switch 被调体（tableswitch 多目标随体迁移）──

TEST(MjPassMethodInlinerTest, InlinesSwitchCalleeBody)
{
    ResourceContainer container;
    {
        FixtureBuilder b("SW2");
        // SW2.f(I)I：iload_0; tableswitch(low=1, high=2){1→pc24, 2→pc28,
        // default→pc32}; case 体各自 iconst + goto 汇合；default 直落 ireturn。
        //   pc0 iload_0(1B)；pc1 tableswitch（pad 2 → 操作数 pc4 起：default
        //   4B / low 4B / high 4B / case1 4B / case2 4B，偏移自 opcode pc1 起）；
        //   pc24 iconst_1；pc25 goto(+8→pc33)；pc28 iconst_2；pc29 goto(+4→
        //   pc33)；pc32 iconst_3；pc33 ireturn
        Bytes callee;
        putU8(callee, 0x1A);                                  // iload_0
        putU8(callee, 0xAA);                                  // tableswitch
        putU8(callee, 0x00); putU8(callee, 0x00);             // pad → 4 对齐
        putU32(callee, 31);                                   // default → pc32
        putU32(callee, 1);                                    // low
        putU32(callee, 2);                                    // high
        putU32(callee, 23);                                   // case 1 → pc24
        putU32(callee, 27);                                   // case 2 → pc28
        putU8(callee, 0x04);                                  // pc24 iconst_1
        putU8(callee, 0xA7); putU16(callee, 8);               // pc25 goto → pc33
        putU8(callee, 0x05);                                  // pc28 iconst_2
        putU8(callee, 0xA7); putU16(callee, 4);               // pc29 goto → pc33
        putU8(callee, 0x06);                                  // pc32 iconst_3
        putU8(callee, 0xAC);                                  // pc33 ireturn
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 2, 1});
        const ca::u16 ref = b.methodRef("SW2", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x05);                                  // iconst_2
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "SW2.class", b.assemble());
    }

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "SW2.class"), "m", "()I");
    // switch 随体：三目标（case1/case2/default）逐目标建标签；ireturn 化
    // 出口 GOTO 与 case 体 goto 同锚汇合，default 体直落该 GOTO（base=0：
    // 调用方 m()I maxLocals=0，被调槽 0 → 槽 0）。a = low 随迁。
    const std::vector<InsnFact> expect = {
        {Op::ICONST_2, 0, {}},
        {Op::ISTORE, 0, {}},         // 落槽
        {Op::ILOAD, 0, {}},          // 克隆体 iload_0 → 槽 0
        {Op::TABLESWITCH, 1, {}},    // low = 1 随迁
        {Op::ICONST_1, 0, {}},
        {Op::GOTO, 0, {}},
        {Op::ICONST_2, 0, {}},
        {Op::GOTO, 0, {}},
        {Op::ICONST_3, 0, {}},
        {Op::GOTO, 0, {}},           // 克隆体 ireturn → 统一出口（default 直落）
        {Op::IRETURN, 0, {}},
    };
    expectFacts(facts, expect);
}

// ── 拒绝面：逐条反证（字节逐位保持）──

/// 拒绝面断言：运行后目标类字节逐位不变 + PA all()（容器整体无改写面）。
void expectUntouched(ResourceContainer& container, const std::string& uri)
{
    const Bytes before = resourceBytes(container, uri);
    const auto  pa     = runInliner(container);
    EXPECT_TRUE(pa.areAllPreserved()) << uri << " 应无改写面";
    EXPECT_EQ(resourceBytes(container, uri), before) << uri << " 字节应逐位保持";
}

/// 站点保留断言：仅目标类字节逐位不变（容器其余类的合法改写不纳入）。
void expectSiteUntouched(ResourceContainer& container, const std::string& uri)
{
    const Bytes before = resourceBytes(container, uri);
    (void)runInliner(container);
    EXPECT_EQ(resourceBytes(container, uri), before) << uri << " 字节应逐位保持";
}

TEST(MjPassMethodInlinerTest, RejectsCalleeOverBudget)
{
    ResourceContainer container;
    {
        FixtureBuilder b("B");
        // B.big(I)I：33 条（nop × 32 + ireturn）
        Bytes callee;
        for (int i = 0; i < 32; ++i) putU8(callee, 0x00);   // nop
        putU8(callee, 0xAC);
        b.addMethod({"big", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 1});
        const ca::u16 ref = b.methodRef("B", "big", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "B.class", b.assemble());
    }
    expectUntouched(container, "B.class");
}

TEST(MjPassMethodInlinerTest, RejectsCalleeWithExceptionTable)
{
    ResourceContainer container;
    {
        FixtureBuilder b("E");
        // E.f()I：nop; iconst_1; ireturn + 一条 catch-all 异常表项
        Bytes callee;
        putU8(callee, 0x00);
        putU8(callee, 0x04);
        putU8(callee, 0xAC);
        FixtureMethod m;
        m.name      = "f";
        m.desc      = "()I";
        m.access    = kAccPublic | kAccStatic;
        m.code      = std::move(callee);
        m.exc       = {{0, 2, 2, 0}};
        m.maxStack  = 1;
        m.maxLocals = 0;
        b.addMethod(m);
        const ca::u16 ref = b.methodRef("E", "f", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "E.class", b.assemble());
    }
    expectUntouched(container, "E.class");
}

TEST(MjPassMethodInlinerTest, RejectsSynchronizedCallee)
{
    ResourceContainer container;
    {
        FixtureBuilder b("S");
        b.addMethod({"f", "()I", {0x04, 0xAC},
                     static_cast<ca::u16>(kAccPublic | kAccStatic | kAccSynchronized), {}, 1, 0});
        const ca::u16 ref = b.methodRef("S", "f", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "S.class", b.assemble());
    }
    expectUntouched(container, "S.class");
}

TEST(MjPassMethodInlinerTest, RejectsSelfRecursiveCallee)
{
    ResourceContainer container;
    {
        FixtureBuilder b("R");
        // R.f(I)I：iload_0; invokestatic R.f; ireturn（体自调）
        const ca::u16 selfRef = b.methodRef("R", "f", "(I)I");
        Bytes callee;
        putU8(callee, 0x1A);
        putU8(callee, 0xB8); putU16(callee, selfRef);
        putU8(callee, 0xAC);
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 1});
        const ca::u16 ref = b.methodRef("R", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "R.class", b.assemble());
    }
    expectUntouched(container, "R.class");
}

TEST(MjPassMethodInlinerTest, RejectsIndirectRecursion)
{
    ResourceContainer container;
    {
        // A.fa()I → B.fb()I；B.fb()I → A.fa()I（互调环，双纯计算体）
        FixtureBuilder ba("A");
        const ca::u16 aToB = ba.methodRef("B", "fb", "()I");
        Bytes bodyA;
        putU8(bodyA, 0xB8); putU16(bodyA, aToB);
        putU8(bodyA, 0xAC);
        ba.addMethod({"fa", "()I", std::move(bodyA), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "A.class", ba.assemble());

        FixtureBuilder bb("B");
        const ca::u16 bToA = bb.methodRef("A", "fa", "()I");
        Bytes bodyB;
        putU8(bodyB, 0xB8); putU16(bodyB, bToA);
        putU8(bodyB, 0xAC);
        bb.addMethod({"fb", "()I", std::move(bodyB), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "B.class", bb.assemble());

        FixtureBuilder bc("C");
        const ca::u16 cToA = bc.methodRef("A", "fa", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, cToA);
        putU8(caller, 0xAC);
        bc.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "C.class", bc.assemble());
    }
    expectUntouched(container, "C.class");
}

TEST(MjPassMethodInlinerTest, RejectsImpureCalleeFieldWrite)
{
    ResourceContainer container;
    {
        FixtureBuilder b("W");
        b.addField({"g", "I", kAccPublic | kAccStatic, {}});
        // W.f(I)I：iload_0; putstatic W.g; iload_0; ireturn（写堆）
        const ca::u16 fref = b.fieldRef("W", "g", "I");
        Bytes callee;
        putU8(callee, 0x1A);
        putU8(callee, 0xB3); putU16(callee, fref);
        putU8(callee, 0x1A);
        putU8(callee, 0xAC);
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 1});
        const ca::u16 ref = b.methodRef("W", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "W.class", b.assemble());
    }
    expectUntouched(container, "W.class");
}

TEST(MjPassMethodInlinerTest, RejectsReadOnlyCalleeFieldRead)
{
    ResourceContainer container;
    {
        FixtureBuilder b("RD");
        b.addField({"g", "I", kAccPublic | kAccStatic, {}});
        // RD.f()I：getstatic W.g; ireturn（只读 ≠ Pure）
        const ca::u16 fref = b.fieldRef("RD", "g", "I");
        Bytes callee;
        putU8(callee, 0xB2); putU16(callee, fref);
        putU8(callee, 0xAC);
        b.addMethod({"f", "()I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 0});
        const ca::u16 ref = b.methodRef("RD", "f", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "RD.class", b.assemble());
    }
    expectUntouched(container, "RD.class");
}

TEST(MjPassMethodInlinerTest, RejectsVirtualCallInBody)
{
    ResourceContainer container;
    {
        // Vm.v()I：iconst_1; ireturn（纯）——callee 经 invokevirtual 调它。
        FixtureBuilder bv("Vm");
        Bytes vbody;
        putU8(vbody, 0x04);
        putU8(vbody, 0xAC);
        bv.addMethod({"v", "()I", std::move(vbody), kAccPublic, {}, 1, 0});
        addClassResource(container, "Vm.class", bv.assemble());

        FixtureBuilder b("V");
        // V.g(LVm;)I：aload_0; invokevirtual Vm.v; ireturn
        const ca::u16 vref = b.methodRef("Vm", "v", "()I");
        Bytes callee;
        putU8(callee, 0x2A);
        putU8(callee, 0xB6); putU16(callee, vref);
        putU8(callee, 0xAC);
        b.addMethod({"g", "(LVm;)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 1});
        const ca::u16 ref = b.methodRef("V", "g", "(LVm;)I");
        Bytes caller;
        putU8(caller, 0x01);        // aconst_null（接收者占位）
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "V.class", b.assemble());
    }
    expectUntouched(container, "V.class");
}

TEST(MjPassMethodInlinerTest, RejectsCrossClassPrivate)
{
    ResourceContainer container;
    {
        FixtureBuilder bh("H2");
        // H2.priv(I)I private 实例：iload_1; iconst_2; imul; ireturn
        Bytes callee;
        putU8(callee, 0x1B);
        putU8(callee, 0x05);
        putU8(callee, 0x68);
        putU8(callee, 0xAC);
        bh.addMethod({"priv", "(I)I", std::move(callee), kAccPrivate, {}, 2, 2});
        addClassResource(container, "H2.class", bh.assemble());

        FixtureBuilder bc("C2");
        const ca::u16 ref = bc.methodRef("H2", "priv", "(I)I");
        Bytes caller;
        putU8(caller, 0x01);        // aconst_null（接收者占位）
        putU8(caller, 0xB7); putU16(caller, ref);   // 跨类 invokespecial
        putU8(caller, 0xAC);
        bc.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "C2.class", bc.assemble());
    }
    expectUntouched(container, "C2.class");
}

TEST(MjPassMethodInlinerTest, RejectsCrossClassStaticOwnerWithClinit)
{
    ResourceContainer container;
    {
        FixtureBuilder bh("H3");
        // H3.k()I public static 纯；H3 带 <clinit>（类初始化触发门）
        Bytes callee;
        putU8(callee, 0x64);        // bipush 100
        putU8(callee, 0xAC);
        bh.addMethod({"k", "()I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 0});
        Bytes clinit;
        putU8(clinit, 0xB1);        // return
        bh.addMethod({"<clinit>", "()V", std::move(clinit),
                      static_cast<ca::u16>(kAccPublic | kAccStatic), {}, 0, 0});
        addClassResource(container, "H3.class", bh.assemble());

        FixtureBuilder bc("C3");
        const ca::u16 ref = bc.methodRef("H3", "k", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        bc.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "C3.class", bc.assemble());
    }
    expectUntouched(container, "C3.class");
}

TEST(MjPassMethodInlinerTest, RejectsCrossClassNonPublicCallee)
{
    ResourceContainer container;
    {
        FixtureBuilder bh("H4");
        // H4.k()I 包私有 static 纯（缺 ACC_PUBLIC）——运行时包等价依赖
        // defining loader 身份，字节不可证，v2 仍拒绝（停报面）。
        Bytes callee;
        putU8(callee, 0x04);        // iconst_1
        putU8(callee, 0xAC);
        bh.addMethod({"k", "()I", std::move(callee), kAccStatic, {}, 1, 0});
        addClassResource(container, "H4.class", bh.assemble());

        FixtureBuilder bc("C4");
        const ca::u16 ref = bc.methodRef("H4", "k", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        bc.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "C4.class", bc.assemble());
    }
    expectUntouched(container, "C4.class");
}

TEST(MjPassMethodInlinerTest, RejectsCrossClassBodyWithCall)
{
    ResourceContainer container;
    {
        FixtureBuilder bh("H5");
        // H5.inner()I public static 纯
        Bytes inner;
        putU8(inner, 0x04);
        putU8(inner, 0xAC);
        bh.addMethod({"inner", "()I", std::move(inner), kAccPublic | kAccStatic, {}, 1, 0});
        // H5.w()I public static：invokestatic H5.inner（体含调用——跨类体不安全）
        const ca::u16 iref = bh.methodRef("H5", "inner", "()I");
        Bytes callee;
        putU8(callee, 0xB8); putU16(callee, iref);
        putU8(callee, 0xAC);
        bh.addMethod({"w", "()I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "H5.class", bh.assemble());

        FixtureBuilder bc("C5");
        const ca::u16 ref = bc.methodRef("H5", "w", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        bc.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "C5.class", bc.assemble());
    }
    // H5 内部（inner → w 同类 static 纯）可合法自内联；被拒的是跨类站点
    // C5.m → H5.w（体含成员引用指令，bodyCrossClassSafe 门）。站点保留
    // 断言只看 C5 字节。
    expectSiteUntouched(container, "C5.class");
}

TEST(MjPassMethodInlinerTest, RejectsBranchToMethodEnd)
{
    ResourceContainer container;
    {
        FixtureBuilder b("B2");
        // B2.f(I)I：iload_0; ifeq → 方法末边界（跳过 ireturn 之后）; ireturn
        Bytes callee;
        putU8(callee, 0x1A);                          // iload_0（pc0）
        putU8(callee, 0x99); putU16(callee, 4);       // ifeq → pc5 = code_length（End）
        putU8(callee, 0xAC);                          // ireturn（pc4）
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 1});
        const ca::u16 ref = b.methodRef("B2", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "B2.class", b.assemble());
    }
    expectUntouched(container, "B2.class");
}

// ── v2 拒绝面（扩面判据的 fail-closed 反证）──

TEST(MjPassMethodInlinerTest, RejectsVirtualOverridableCallee)
{
    ResourceContainer container;
    {
        // Vm2.v(I)I public 非 final、Vm2 非 final：唯一实现不可证（容器外
        // 子类可覆盖）——final 收窄之外的去虚化证据不成立。
        FixtureBuilder bv("Vm2");
        Bytes vbody;
        putU8(vbody, 0x1B);
        putU8(vbody, 0x04);
        putU8(vbody, 0x60);
        putU8(vbody, 0xAC);
        bv.addMethod({"v", "(I)I", std::move(vbody), kAccPublic, {}, 2, 2});
        addClassResource(container, "Vm2.class", bv.assemble());

        FixtureBuilder b("OV");
        const ca::u16 ref = b.methodRef("Vm2", "v", "(I)I");
        Bytes caller;
        putU8(caller, 0x2B);        // aload_1
        putU8(caller, 0x04);        // iconst_1
        putU8(caller, 0xB6); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(LVm2;)I", std::move(caller), kAccPublic | kAccStatic, {}, 2, 2});
        addClassResource(container, "OV.class", b.assemble());
    }
    expectUntouched(container, "OV.class");
}

TEST(MjPassMethodInlinerTest, RejectsVirtualFinalMethodCrossClassBodyWithCall)
{
    ResourceContainer container;
    {
        FixtureBuilder bh("FD");
        // FD.helper()I public static 纯
        Bytes helper;
        putU8(helper, 0x04);
        putU8(helper, 0xAC);
        bh.addMethod({"helper", "()I", std::move(helper), kAccPublic | kAccStatic, {}, 1, 0});
        // FD.fin()I public final：invokestatic FD.helper（体含成员引用——
        // 去虚化证据成立但跨类体不安全，bodyCrossClassSafe 门拒收）
        const ca::u16 iref = bh.methodRef("FD", "helper", "()I");
        Bytes callee;
        putU8(callee, 0xB8); putU16(callee, iref);
        putU8(callee, 0xAC);
        bh.addMethod({"fin", "()I", std::move(callee), kAccPublic | kAccFinal, {}, 1, 0});
        addClassResource(container, "FD.class", bh.assemble());

        FixtureBuilder bc("FDU");
        const ca::u16 ref = bc.methodRef("FD", "fin", "()I");
        Bytes caller;
        putU8(caller, 0x2B);        // aload_1（receiver 占位）
        putU8(caller, 0xB6); putU16(caller, ref);
        putU8(caller, 0xAC);
        bc.addMethod({"m", "(LFD;)I", std::move(caller), kAccPublic | kAccStatic, {}, 2, 1});
        addClassResource(container, "FDU.class", bc.assemble());
    }
    // FD 内部（helper → fin 同类）可合法自内联；被拒的是跨类站点 FDU.m →
    // FD.fin。站点保留断言只看 FDU 字节。
    expectSiteUntouched(container, "FDU.class");
}

TEST(MjPassMethodInlinerTest, RejectsNestmateMissingAttrs)
{
    ResourceContainer container;
    {
        // 无 NestHost/NestMembers 属性的跨类 private static 调用：nest 关系
        // 不可证，fail-closed（HotSpot 侧此类调用本就 IllegalAccessError）。
        FixtureBuilder ba("WA");
        Bytes callee;
        putU8(callee, 0x04);
        putU8(callee, 0xAC);
        ba.addMethod({"priv", "()I", std::move(callee), kAccPrivate | kAccStatic, {}, 1, 0});
        addClassResource(container, "WA.class", ba.assemble());

        FixtureBuilder bb("WB");
        const ca::u16 ref = bb.methodRef("WA", "priv", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        bb.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "WB.class", bb.assemble());
    }
    expectUntouched(container, "WB.class");
}

TEST(MjPassMethodInlinerTest, RejectsNestmateStaleMembership)
{
    ResourceContainer container;
    {
        // 属性链不完整：宿主 NestMembers 只列 A 未列 B——HotSpot 懒验证
        // 口径下 B 不在 nest（访问本会 IllegalAccessError），fail-closed。
        FixtureBuilder bh("SH");
        bh.addClassAttr({"NestMembers", nestMembersPayload(bh, {"SA"})});
        addClassResource(container, "SH.class", bh.assemble());
        FixtureBuilder ba("SA");
        ba.addClassAttr({"NestHost", nestHostPayload(ba, "SH")});
        Bytes callee;
        putU8(callee, 0x04);
        putU8(callee, 0xAC);
        ba.addMethod({"priv", "()I", std::move(callee), kAccPrivate | kAccStatic, {}, 1, 0});
        addClassResource(container, "SA.class", ba.assemble());
        FixtureBuilder bb("SB");
        bb.addClassAttr({"NestHost", nestHostPayload(bb, "SH")});
        const ca::u16 ref = bb.methodRef("SA", "priv", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        bb.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "SB.class", bb.assemble());
    }
    expectUntouched(container, "SB.class");
}

TEST(MjPassMethodInlinerTest, RejectsProtectedWithoutSubclass)
{
    ResourceContainer container;
    {
        // protected static + 无子类关系的跨类调用者：JVM protected 规则的
        // 包等价支字节不可证，子类支不成立 → 拒收。
        FixtureBuilder bd("PD");
        Bytes callee;
        putU8(callee, 0x04);
        putU8(callee, 0xAC);
        bd.addMethod({"k", "()I", std::move(callee), kAccProtected | kAccStatic, {}, 1, 0});
        addClassResource(container, "PD.class", bd.assemble());

        FixtureBuilder bu("PU");
        const ca::u16 ref = bu.methodRef("PD", "k", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        bu.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "PU.class", bu.assemble());
    }
    expectUntouched(container, "PU.class");
}

TEST(MjPassMethodInlinerTest, RejectsDeepLoopCallee)
{
    ResourceContainer container;
    {
        FixtureBuilder b("DL");
        // DL.f(I)I：goto 自身 ×3 + ireturn——回边数 3 超浅循环帽 2。
        Bytes callee;
        putU8(callee, 0xA7); putU16(callee, 0);       // 回边 1
        putU8(callee, 0xA7); putU16(callee, 0);       // 回边 2
        putU8(callee, 0xA7); putU16(callee, 0);       // 回边 3（超帽）
        putU8(callee, 0xAC);
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 0});
        const ca::u16 ref = b.methodRef("DL", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "DL.class", b.assemble());
    }
    expectUntouched(container, "DL.class");
}

TEST(MjPassMethodInlinerTest, RejectsImpureLoopCallee)
{
    ResourceContainer container;
    {
        FixtureBuilder b("IL");
        b.addField({"g", "I", kAccPublic | kAccStatic, {}});
        // IL.f(I)I：iload_0; putstatic IL.g; goto 回边; ireturn——循环形态
        // 合法但写堆，纯度门拒收（v1 RejectsLoopCalleeBackEdge 的 v2 重定位：
        // 回边本身已放行，拒绝责任回归纯度/预算/形状门）。
        const ca::u16 fref = b.fieldRef("IL", "g", "I");
        Bytes callee;
        putU8(callee, 0x1A);                          // iload_0（pc0，循环头）
        putU8(callee, 0xB3); putU16(callee, fref);    // putstatic（pc1）
        putU8(callee, 0xA7); putU16(callee, 0xFFF9);  // goto → pc0（回边，pc4）
        putU8(callee, 0xAC);                          // ireturn（pc7）
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 1});
        const ca::u16 ref = b.methodRef("IL", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "IL.class", b.assemble());
    }
    expectUntouched(container, "IL.class");
}

TEST(MjPassMethodInlinerTest, RejectsSwitchTargetToMethodEnd)
{
    ResourceContainer container;
    {
        FixtureBuilder b("SWE");
        // SWE.f(I)I：iload_0; lookupswitch(0 pairs, default→方法末边界); pop;
        // ireturn——switch 随体已放行，末边界目标门仍拒（v1 门不放宽）。
        Bytes callee;
        putU8(callee, 0x1A);
        putU8(callee, 0xAB);
        putU8(callee, 0x00); putU8(callee, 0x00);   // pad → default 起点 4 对齐
        putU32(callee, 13);           // default → pc14 = code_length（End）
        putU32(callee, 0);            // npairs
        putU8(callee, 0x57);          // pop
        putU8(callee, 0xAC);
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 1});
        const ca::u16 ref = b.methodRef("SWE", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "SWE.class", b.assemble());
    }
    expectUntouched(container, "SWE.class");
}

TEST(MjPassMethodInlinerTest, RejectsInterfaceNonPrivateSite)
{
    ResourceContainer container;
    {
        // 接口 default 方法（public 非 private）经 invokeinterface：实现类
        // 集合对容器外开放，唯一实现不可证 → 拒收（v2 接口面只收 private）。
        FixtureBuilder bi("IJ");
        bi.setClassAccess(kAccPublic | kAccInterface | kAccAbstract);
        Bytes dm;
        putU8(dm, 0x04);
        putU8(dm, 0xAC);
        bi.addMethod({"dm", "()I", std::move(dm), kAccPublic, {}, 1, 0});
        addClassResource(container, "IJ.class", bi.assemble());

        FixtureBuilder bc("IJU");
        const ca::u16 ref = bc.interfaceMethodRef("IJ", "dm", "()I");
        Bytes caller;
        putU8(caller, 0x2B);                          // aload_1
        putU8(caller, 0xB9); putU16(caller, ref);     // invokeinterface
        putU8(caller, 0x01);
        putU8(caller, 0x00);
        putU8(caller, 0xAC);
        bc.addMethod({"m", "(LIJ;)I", std::move(caller), kAccPublic | kAccStatic, {}, 2, 1});
        addClassResource(container, "IJU.class", bc.assemble());
    }
    expectUntouched(container, "IJU.class");
}

// ── 幂等与鲁棒 ──

TEST(MjPassMethodInlinerTest, IdempotentSecondRun)
{
    ResourceContainer container;
    addClassResource(container, "H6.class", buildDouble("H6", "k2", kAccPublic | kAccStatic, "()I"));
    {
        FixtureBuilder b("C6");
        const ca::u16 ref = b.methodRef("H6", "k2", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "C6.class", b.assemble());
    }

    const auto first = runInliner(container);
    ASSERT_FALSE(first.areAllPreserved());
    const Bytes settled = resourceBytes(container, "C6.class");

    const auto second = runInliner(container);
    EXPECT_TRUE(second.areAllPreserved()) << "二次运行应严格 no-op";
    EXPECT_EQ(resourceBytes(container, "C6.class"), settled);
}

TEST(MjPassMethodInlinerTest, UnparseableResourceSkippedGracefully)
{
    ResourceContainer container;
    addClassResource(container, "H7.class", buildDouble("H7", "k3", kAccPublic | kAccStatic, "()I"));
    {
        FixtureBuilder b("C7");
        const ca::u16 ref = b.methodRef("H7", "k3", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "C7.class", b.assemble());
    }
    // 垃圾字节资源：装载失败整类跳过，不阻断其余类改写。
    addClassResource(container, "junk.class", Bytes{0xCA, 0xFE, 0xBA, 0xBE, 0x00});

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());
    const auto facts = methodFacts(resourceBytes(container, "C7.class"), "m", "()I");
    // 零参 callee：无落槽序列，首条克隆指令即首条插入（入边重锚面）。
    ASSERT_EQ(facts.size(), 3u);
    EXPECT_EQ(facts[0].op, Op::ICONST_1);
    EXPECT_EQ(facts[1].op, Op::GOTO);
    EXPECT_EQ(facts[2].op, Op::IRETURN);
}

TEST(MjPassMethodInlinerTest, CalleeOwnerNotInContainerKeepsSite)
{
    ResourceContainer container;
    {
        FixtureBuilder b("C8");
        // 调用目标类不在容器内（无体可取，去虚化解析亦不可证）→ 站点保留。
        const ca::u16 ref = b.methodRef("Missing", "k", "()I");
        Bytes caller;
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "()I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 0});
        addClassResource(container, "C8.class", b.assemble());
    }
    expectUntouched(container, "C8.class");
}

// ── 注册面 ──

TEST(MjPassMethodInlinerTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("MethodInliner"));
    auto created = mjbase::spi::defaultPassRegistry().createGlobalPass("MethodInliner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
