/// @file        method_inliner_test.cpp
/// @brief       基础版小方法内联：容器级多类用例（static/跨类/private 形态、
///              前向分支体、门面拒绝逐条反证、幂等、注册面）。
///
/// 载体是 ResourceContainer + 手工字节夹具（FixtureBuilder）；语义断言走
/// 「改写 → 重读会话 → 指令序列事实」roundtrip 口径；拒绝面以字节逐位
/// 保持钉死。行为等价的 JVM 硬验收在 method_inliner_jvm_test.cpp。

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
using mjbase::test::FixtureBuilder;
using mjbase::test::FixtureMethod;
using mjbase::test::putU16;
using mjbase::test::putU32;
using mjbase::test::putU8;

namespace {

using jbc::core::Op;

constexpr ca::u16 kAccPublic       = 0x0001;
constexpr ca::u16 kAccPrivate      = 0x0002;
constexpr ca::u16 kAccStatic       = 0x0008;
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

}   // namespace

// ── 正例：同类 static 小方法内联 ──

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
    // 克隆体（quick 已升泛形）→ 出口 GOTO → 原 ireturn。
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
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i].op) << "index " << i;
        EXPECT_EQ(facts[i].a, expect[i].a) << "index " << i;
    }
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

// ── 正例：同类 private 实例方法（invokespecial，receiver 落槽 0）──

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
    const std::vector<InsnFact> expect = {
        {Op::ALOAD_0, 0, {}},        // 原接收者装载（caller 原指令）
        {Op::ILOAD_1, 0, {}},        // 原实参装载
        {Op::ISTORE, 3, {}},         // 落槽：被调槽 1（param，栈顶）
        {Op::ASTORE, 2, {}},         // 落槽：被调槽 0（receiver）
        {Op::ILOAD, 3, {}},          // 克隆体 iload_1 → 槽 3
        {Op::ICONST_2, 0, {}},
        {Op::IMUL, 0, {}},
        {Op::GOTO, 0, {}},
        {Op::IRETURN, 0, {}},
    };
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i].op) << "index " << i;
        EXPECT_EQ(facts[i].a, expect[i].a) << "index " << i;
    }
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
        // H4.k()I 包私有 static 纯（缺 ACC_PUBLIC）
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

TEST(MjPassMethodInlinerTest, RejectsLoopCalleeBackEdge)
{
    ResourceContainer container;
    {
        FixtureBuilder b("L");
        // L.f(I)I：iconst_1; goto 自身（回边）; ireturn
        Bytes callee;
        putU8(callee, 0x04);                          // iconst_1
        putU8(callee, 0xA7); putU16(callee, 0);       // goto 自身（offset 0）
        putU8(callee, 0xAC);
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 0});
        const ca::u16 ref = b.methodRef("L", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "L.class", b.assemble());
    }
    expectUntouched(container, "L.class");
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

TEST(MjPassMethodInlinerTest, RejectsSwitchCallee)
{
    ResourceContainer container;
    {
        FixtureBuilder b("SW");
        // SW.f(I)I：iload_0; lookupswitch(0 pairs, default→pop); pop; ireturn
        //   pc0 iload_0(1B)；pc1 lookupswitch：pad 2（pc2-3，操作数 4 对齐）→
        //   pc4 default(4B) → pc8 npairs=0(4B)；pc12 pop；pc13 ireturn
        Bytes callee;
        putU8(callee, 0x1A);
        putU8(callee, 0xAB);
        putU8(callee, 0x00); putU8(callee, 0x00);   // pad → default 起点 4 对齐
        putU32(callee, 11);           // default → pc12（pop），偏移自 opcode(pc1) 起
        putU32(callee, 0);            // npairs
        putU8(callee, 0x57);          // pop
        putU8(callee, 0xAC);
        b.addMethod({"f", "(I)I", std::move(callee), kAccPublic | kAccStatic, {}, 1, 1});
        const ca::u16 ref = b.methodRef("SW", "f", "(I)I");
        Bytes caller;
        putU8(caller, 0x1A);
        putU8(caller, 0xB8); putU16(caller, ref);
        putU8(caller, 0xAC);
        b.addMethod({"m", "(I)I", std::move(caller), kAccPublic | kAccStatic, {}, 1, 1});
        addClassResource(container, "SW.class", b.assemble());
    }
    expectUntouched(container, "SW.class");
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
        // 调用目标类不在容器内（无体可取）→ 站点保留。
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
