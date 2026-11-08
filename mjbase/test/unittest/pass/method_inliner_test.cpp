/// @file        method_inliner_test.cpp
/// @brief       基础版小方法内联：容器级多类用例（static/跨类/private 形态、
///              v2 去虚化/nestmate/protected/switch/浅循环面、前向分支体、
///              门面拒绝逐条反证、幂等、注册面）。
///
/// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）；窄常量
///（bipush/sipush）装配器按自身口径规范化为 LDC——跨类常量 re-intern 面
/// 随之改由 ldc 载体承担（原 bipush 无 CP 载荷对照随字节级 builder 一并
/// 退役）。语义断言走「改写 → 重读会话 → 指令序列事实」roundtrip 口径；
/// 拒绝面以字节逐位保持钉死。行为等价的 JVM 硬验收在
/// method_inliner_jvm_test.cpp。
///
/// 纪律例外①申报：分支/switch 落方法末边界的非法形态 jasm 不表达——从装
/// 配合法基座出发，经首方法 Code 单点重锚（branchToMethodEnd /
/// switchDefaultToMethodEnd）产出偏差输入，前后 relaxed validate 自检
///（改写前 Ok / 改写后 Fail）对随夹具内置。
///
/// 纪律例外②申报：NestHost/NestMembers 属性 jasm 无指令面——从装配合法
/// 骨架出发经 CP/AttrSlot DOM 追加 Raw 槽（withNestHost / withNestMembers）。
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

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/method_inliner.hpp"
#include "mjbase/spi/analysis_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/jasm_fixture.hpp"

using mjbase::model::ClassResource;
using mjbase::model::ResourceContainer;
using mjbase::test::Bytes;

namespace {

using jbc::core::AttrKind;
using jbc::core::AttrSlot;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::Op;

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

// ── jasm 装配便捷形 ─────────────────────────────────────────────────

/// 方法声明：修饰词文本（如 "public static"）/名/desc/code 体文本。
struct JMethod
{
    std::string_view mods;
    std::string_view name;
    std::string_view desc;
    std::string_view body;
};

/// 类装配：.super super + .class <classMods> <cls> { fields methods }。
/// fields 为 .field 声明文本（可为空串）。
Bytes asmClass(const std::string& cls, std::string_view classMods, std::string_view super,
               std::string_view fields, const std::vector<JMethod>& ms)
{
    std::string src = ".super ";
    src.append(super).append("\n.class ").append(classMods).append(" ").append(cls).append(" {\n");
    src.append(fields);
    for (const JMethod& m : ms) {
        src += "  .method ";
        src.append(m.mods).append(" ").append(m.name).append(" ").append(m.desc);
        src += " {\n    code: {\n";
        src.append(m.body);
        src += "    }\n  }\n";
    }
    src += "}\n";
    return mjbase::test::assembleJasm(src);
}

/// 类装配便捷形（public super 默认类修饰，超 java/lang/Object，无字段）。
Bytes asmDefaultClass(const std::string& cls, const std::vector<JMethod>& ms)
{
    return asmClass(cls, "public super", "java/lang/Object", "", ms);
}

/// static 纯计算 callee：`(I)I → iload 0; iconst_2; imul; ireturn`
/// `()I → iconst_1; ireturn`（零参形态，无局部无分支）。
Bytes buildDouble(const std::string& self, std::string_view name, std::string_view mods,
                  std::string_view desc = "(I)I")
{
    if (desc == "()I") {
        return asmDefaultClass(self, {{mods, name, desc, "      iconst_1\n      ireturn\n"}});
    }
    return asmDefaultClass(
        self, {{mods, name, desc, "      iload 0\n      iconst_2\n      imul\n      ireturn\n"}});
}

/// 纪律例外②：装配合法骨架后经 CP/AttrSlot DOM 追加类级 NestHost
///（u2 宿主 Class CP；jasm 无该属性指令面）。
Bytes withNestHost(Bytes bytes, std::string_view host)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return {};
    jbc::core::ReadResult rr = std::move(read).unwrap();
    ClassFile&          cf   = *rr.file;
    const ca::u32       hostCp = cf.cp.addClass(cf.cp.addUtf8(host));
    const ca::u32       nameCp = cf.cp.addUtf8("NestHost");
    Bytes               payload{static_cast<ca::u8>(hostCp >> 8), static_cast<ca::u8>(hostCp)};
    cf.attrs.push_back(AttrSlot::makeNew(nameCp,
                                         std::make_unique<jbc::core::RawAttr>(payload)));
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    return written.is_ok() ? std::move(written).unwrap() : Bytes{};
}

/// 纪律例外②：同面追加类级 NestMembers（u2 计数 + u2 成员 Class CP 列表）。
Bytes withNestMembers(Bytes bytes, const std::vector<std::string_view>& members)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return {};
    jbc::core::ReadResult rr = std::move(read).unwrap();
    ClassFile&          cf   = *rr.file;
    Bytes               payload;
    payload.push_back(static_cast<ca::u8>(members.size() >> 8));
    payload.push_back(static_cast<ca::u8>(members.size()));
    for (std::string_view m : members) {
        const ca::u32 clsCp = cf.cp.addClass(cf.cp.addUtf8(m));
        payload.push_back(static_cast<ca::u8>(clsCp >> 8));
        payload.push_back(static_cast<ca::u8>(clsCp));
    }
    const ca::u32 nameCp = cf.cp.addUtf8("NestMembers");
    cf.attrs.push_back(AttrSlot::makeNew(nameCp,
                                         std::make_unique<jbc::core::RawAttr>(payload)));
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    return written.is_ok() ? std::move(written).unwrap() : Bytes{};
}

/// 纪律例外①载面（分支落方法末边界）：把首方法 Code 里第一条 opcode 分支
///（2 字节偏移形，target = opPc + off）单点重锚到 code_length。装配基座
/// 改写前 expectValidateOk、改写后 expectValidateFail——申报的非法形态
/// 自检对随夹具内置。
Bytes branchToMethodEnd(Bytes bytes, ca::u8 op)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return {};
    mjbase::test::expectValidateOk(bytes);
    jbc::core::ReadResult rr = std::move(read).unwrap();
    ClassFile&          cf   = *rr.file;
    for (jbc::core::MemberInfo& m : cf.methods) {
        for (AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
            if (!slot.decoded() && cf.decodeAttr(slot).is_err()) {
                ADD_FAILURE() << "branchToMethodEnd: Code 解码失败";
                return {};
            }
            auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
            if (code == nullptr) {
                ADD_FAILURE() << "branchToMethodEnd: Code 槽类型不符";
                return {};
            }
            const ca::u32 codeLen = code->code.size();
            for (ca::u32 pc = 0; pc < codeLen; ++pc) {
                if (code->code[pc] != op) continue;
                const ca::i32 off =
                    static_cast<ca::i32>(codeLen) - static_cast<ca::i32>(pc);
                code->code[pc + 1] = static_cast<ca::u8>(static_cast<ca::u16>(off) >> 8);
                code->code[pc + 2] = static_cast<ca::u8>(off);
                auto written = jbc::core::writeClass(cf);
                EXPECT_TRUE(written.is_ok());
                if (written.is_err()) return {};
                const Bytes deviated = std::move(written).unwrap();
                mjbase::test::expectValidateFail(deviated);
                return deviated;
            }
            ADD_FAILURE() << "branchToMethodEnd: 未找到目标分支";
            return {};
        }
    }
    ADD_FAILURE() << "branchToMethodEnd: 无 Code 属性";
    return {};
}

/// 纪律例外①载面（switch default 落方法末边界）：把首方法 Code 里第一条
/// lookupswitch（0xab）的 default 目标单点重锚到 code_length。自检对同上。
Bytes switchDefaultToMethodEnd(Bytes bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return {};
    mjbase::test::expectValidateOk(bytes);
    jbc::core::ReadResult rr = std::move(read).unwrap();
    ClassFile&          cf   = *rr.file;
    for (jbc::core::MemberInfo& m : cf.methods) {
        for (AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) continue;
            if (!slot.decoded() && cf.decodeAttr(slot).is_err()) {
                ADD_FAILURE() << "switchDefaultToMethodEnd: Code 解码失败";
                return {};
            }
            auto* code = ca::core::tag_cast::cast<jbc::core::CodeAttr>(slot.mutate());
            if (code == nullptr) {
                ADD_FAILURE() << "switchDefaultToMethodEnd: Code 槽类型不符";
                return {};
            }
            const ca::u32 codeLen = code->code.size();
            for (ca::u32 pc = 0; pc < codeLen; ++pc) {
                if (code->code[pc] != 0xab) continue;   // LOOKUPSWITCH
                const ca::u32 pad    = (4 - ((pc + 1) % 4)) % 4;
                const ca::u32 defAt = pc + 1 + pad;
                const ca::i32 off =
                    static_cast<ca::i32>(codeLen) - static_cast<ca::i32>(pc);
                code->code[defAt + 0] = static_cast<ca::u8>(static_cast<ca::u32>(off) >> 24);
                code->code[defAt + 1] = static_cast<ca::u8>(static_cast<ca::u32>(off) >> 16);
                code->code[defAt + 2] = static_cast<ca::u8>(static_cast<ca::u32>(off) >> 8);
                code->code[defAt + 3] = static_cast<ca::u8>(off);
                auto written = jbc::core::writeClass(cf);
                EXPECT_TRUE(written.is_ok());
                if (written.is_err()) return {};
                const Bytes deviated = std::move(written).unwrap();
                mjbase::test::expectValidateFail(deviated);
                return deviated;
            }
            ADD_FAILURE() << "switchDefaultToMethodEnd: 未找到 lookupswitch";
            return {};
        }
    }
    ADD_FAILURE() << "switchDefaultToMethodEnd: 无 Code 属性";
    return {};
}

}   // namespace

// ── 正例：同类 static 小方法内联（v1 基础面）──

TEST(MjPassMethodInlinerTest, InlinesStaticSameClassSmallPure)
{
    ResourceContainer container;
    // T.add(II)I：iload_0; iload_1; iadd; ireturn；T.m(II)I：invokestatic T.add
    addClassResource(container, "T.class", asmDefaultClass("T",
        {
            {"public static", "add", "(II)I",
             "      iload 0\n      iload 1\n      iadd\n      ireturn\n"},
            {"public static", "m", "(II)I",
             "      iload 0\n      iload 1\n      invokestatic T.add (II)I\n      ireturn\n"},
        }));

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
    // H.k()I：ldc 100; ireturn（纯常量；原 bipush 载体经装配器规范化口径
    // 改 ldc——re-intern 面恰由 CP 载荷常量承担，与用例名同向）。
    addClassResource(container, "H.class",
                     asmDefaultClass("H",
                                     {{"public static", "k", "()I",
                                       "      ldc 100\n      ireturn\n"}}));
    // C.use()I：invokestatic H.k; ireturn
    addClassResource(container, "C.class",
                     asmDefaultClass("C",
                                     {{"public static", "use", "()I",
                                       "      invokestatic H.k ()I\n      ireturn\n"}}));

    const auto pa = runInliner(container);
    ASSERT_FALSE(pa.areAllPreserved());

    const auto facts = methodFacts(resourceBytes(container, "C.class"), "use", "()I");
    // ldc 有 CP 载荷：跨会话 re-intern 后 op 原样随迁。
    const std::vector<Op> expect = {Op::LDC, Op::GOTO, Op::IRETURN};
    ASSERT_EQ(facts.size(), expect.size());
    for (ca::usize i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(facts[i].op, expect[i]) << "index " << i;
    }
    // 被调壳保留（删除延后删除族管线）。
    const auto calleeFacts = methodFacts(resourceBytes(container, "H.class"), "k", "()I");
    ASSERT_EQ(calleeFacts.size(), 2u);
    EXPECT_EQ(calleeFacts[0].op, Op::LDC);
    EXPECT_EQ(calleeFacts[1].op, Op::IRETURN);
}

// ── 正例：同类 private 实例方法（invokespecial，接收者 null 守卫随拼接）──

TEST(MjPassMethodInlinerTest, InlinesPrivateSameClassViaInvokespecial)
{
    ResourceContainer container;
    // K.dbl(I)I private：iload_1; iconst_2; imul; ireturn（槽 0=this, 1=param）；
    // K.run(I)I：aload_0; iload_1; invokespecial K.dbl; ireturn
    addClassResource(container, "K.class", asmDefaultClass("K",
        {
            {"private", "dbl", "(I)I",
             "      iload 1\n      iconst_2\n      imul\n      ireturn\n"},
            {"public", "run", "(I)I",
             "      aload 0\n      iload 1\n      invokespecial K.dbl (I)I\n      ireturn\n"},
        }));

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
    // P.pick(Z)I：iload_0; ifeq Lelse; iconst_3; goto Lret; Lelse: iconst_4;
    // Lret: ireturn；P.use()I：iconst_1; invokestatic P.pick; ireturn
    addClassResource(container, "P.class", asmDefaultClass("P",
        {
            {"public static", "pick", "(Z)I",
             "      iload 0\n"
             "      ifeq Lelse\n"
             "      iconst_3\n"
             "      goto Lret\n"
             "      Lelse:\n"
             "      iconst_4\n"
             "      Lret:\n"
             "      ireturn\n"},
            {"public static", "use", "()I",
             "      iconst_1\n      invokestatic P.pick (Z)I\n      ireturn\n"},
        }));

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
    // 单类装配：fin（final 方法，去虚化证据）+ run（调用方）。javac 11+
    // 对 private/final 实例方法发 invokevirtual（JEP 181 口径）。
    addClassResource(container, "F.class", asmDefaultClass("F",
        {
            {"public final", "fin", "(I)I",
             "      iload 1\n      iconst_2\n      imul\n      ireturn\n"},
            {"public", "run", "(I)I",
             "      aload 0\n      iload 1\n      invokevirtual F.fin (I)I\n      ireturn\n"},
        }));

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
    // 单类装配：pv（private 实例）+ run（调用方）——javac 11+ 对同类
    // private 实例调用发 invokevirtual，探针实测。
    addClassResource(container, "PV.class", asmDefaultClass("PV",
        {
            {"private", "pv", "(I)I",
             "      iload 1\n      iconst_2\n      imul\n      ireturn\n"},
            {"public", "run", "(I)I",
             "      aload 0\n      iload 1\n      invokevirtual PV.pv (I)I\n      ireturn\n"},
        }));

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
    // FC final 类：v(I)I public（方法本身非 final——final 类背书唯一实现）；
    // CU.use(LFC;I)I：aload_1; iload_2; invokevirtual FC.v; ireturn
    addClassResource(container, "FC.class", asmClass("FC", "public final",
        "java/lang/Object", "",
        {{"public", "v", "(I)I",
          "      iload 1\n      iconst_3\n      iadd\n      ireturn\n"}}));
    addClassResource(container, "CU.class", asmDefaultClass("CU",
        {{"public static", "use", "(LFC;I)I",
          "      aload 1\n      iload 2\n      invokevirtual FC.v (I)I\n      ireturn\n"}}));

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
    // H：宿主，NestMembers = [A, B]（无方法，仅承载属性）
    addClassResource(container, "H.class", withNestMembers(asmDefaultClass("H", {}),
                                                           {"A", "B"}));
    // A：NestHost → H；priv(I)I private static：iload_0; iconst_2; imul; ireturn
    addClassResource(container, "A.class",
                     withNestHost(asmDefaultClass("A",
                                                  {{"private static", "priv", "(I)I",
                                                    "      iload 0\n      iconst_2\n"
                                                    "      imul\n      ireturn\n"}}),
                                  "H"));
    // B：NestHost → H；m(I)I：iload_0; invokestatic A.priv; ireturn
    //（javac 11+ 跨 nest private static 调用形态，探针实测。）
    addClassResource(container, "B.class",
                     withNestHost(asmDefaultClass("B",
                                                  {{"public static", "m", "(I)I",
                                                    "      iload 0\n      invokestatic "
                                                    "A.priv (I)I\n      ireturn\n"}}),
                                  "H"));

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
    // NH：宿主，NestMembers = [NA, NB]
    addClassResource(container, "NH.class", withNestMembers(asmDefaultClass("NH", {}),
                                                            {"NA", "NB"}));
    // NA：NestHost → NH；priv(I)I private 实例
    addClassResource(container, "NA.class",
                     withNestHost(asmDefaultClass("NA",
                                                  {{"private", "priv", "(I)I",
                                                    "      iload 1\n      iconst_2\n"
                                                    "      imul\n      ireturn\n"}}),
                                  "NH"));
    // NB：NestHost → NH；m(LNA;I)I：aload_1; iload_2; invokevirtual NA.priv;
    // ireturn（javac 11+ 跨 nest private 实例调用形态，探针实测。）
    addClassResource(container, "NB.class",
                     withNestHost(asmDefaultClass("NB",
                                                  {{"public static", "m", "(LNA;I)I",
                                                    "      aload 1\n      iload 2\n"
                                                    "      invokevirtual NA.priv (I)I\n"
                                                    "      ireturn\n"}}),
                                  "NH"));

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
    // D.k()I protected static：iconst_1; ireturn；S extends D：m()I
    addClassResource(container, "D.class",
                     asmDefaultClass("D",
                                     {{"protected static", "k", "()I",
                                       "      iconst_1\n      ireturn\n"}}));
    addClassResource(container, "S.class",
                     asmClass("S", "public super", "D", "",
                              {{"public static", "m", "()I",
                                "      invokestatic D.k ()I\n      ireturn\n"}}));

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
    // PI 接口：p(I)I private + use(I)I public default（invokeinterface
    // count = 接收者 + 参数 = 2，装配器按 desc 自动发 count）。
    addClassResource(container, "PI.class", asmClass("PI", "public interface abstract",
        "java/lang/Object", "",
        {
            {"private", "p", "(I)I",
             "      iload 1\n      iconst_2\n      imul\n      ireturn\n"},
            {"public", "use", "(I)I",
             "      aload 0\n      iload 1\n      invokeinterface PI.p (I)I\n"
             "      ireturn\n"},
        }));

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
    // LP.f(I)I：s=0; i=0; while (i < n) { s += i; i++ } 返回 s；
    // LP.m()I：iconst_3; invokestatic LP.f; ireturn
    addClassResource(container, "LP.class", asmDefaultClass("LP",
        {
            {"public static", "f", "(I)I",
             "      iconst_0\n"
             "      istore 1\n"
             "      iconst_0\n"
             "      istore 2\n"
             "      Lhead:\n"
             "      iload 2\n"
             "      iload 0\n"
             "      if_icmpge Lexit\n"
             "      iload 1\n"
             "      iload 2\n"
             "      iadd\n"
             "      istore 1\n"
             "      iinc 2, 1\n"
             "      goto Lhead\n"
             "      Lexit:\n"
             "      iload 1\n"
             "      ireturn\n"},
            {"public static", "m", "()I",
             "      iconst_3\n      invokestatic LP.f (I)I\n      ireturn\n"},
        }));

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
    // SW2.f(I)I：iload_0; tableswitch(min=1, cases [Lc1, Lc2], default Ld)；
    // case 体各自 iconst + goto 汇合；default 直落汇合点 ireturn。
    addClassResource(container, "SW2.class", asmDefaultClass("SW2",
        {
            {"public static", "f", "(I)I",
             "      iload 0\n"
             "      tableswitch { min: 1, default: Ld, cases: [Lc1, Lc2] }\n"
             "      Lc1:\n"
             "      iconst_1\n"
             "      goto Lret\n"
             "      Lc2:\n"
             "      iconst_2\n"
             "      goto Lret\n"
             "      Ld:\n"
             "      iconst_3\n"
             "      Lret:\n"
             "      ireturn\n"},
            {"public static", "m", "()I",
             "      iconst_2\n      invokestatic SW2.f (I)I\n      ireturn\n"},
        }));

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
    // B.big(I)I：33 条（nop × 32 + ireturn）
    std::string big;
    for (int i = 0; i < 32; ++i) big += "      nop\n";
    big += "      ireturn\n";
    addClassResource(container, "B.class", asmDefaultClass("B",
        {
            {"public static", "big", "(I)I", big},
            {"public static", "m", "(I)I",
             "      iload 0\n      invokestatic B.big (I)I\n      ireturn\n"},
        }));
    expectUntouched(container, "B.class");
}

TEST(MjPassMethodInlinerTest, RejectsCalleeWithExceptionTable)
{
    ResourceContainer container;
    // E.f()I：保护区 [L0, L2) 覆盖 nop/iconst_1，catch-all handler L2 收口
    //（pop + iconst_1 + ireturn 帧合法形——被调含异常表即拒，handler 形与
    // 门判据无关）。exceptions 表直拼源（asmClass 便捷形不带 exceptions）。
    const std::string src = R"JASM(.super java/lang/Object
.class public super E {
  .method public static f ()I {
    exceptions: {
      { L0, L2, L2, * }
    }
    code: {
      L0:
      nop
      iconst_1
      L2:
      pop
      iconst_1
      ireturn
    }
  }
  .method public static m ()I {
    code: {
      invokestatic E.f ()I
      ireturn
    }
  }
}
)JASM";
    addClassResource(container, "E.class", mjbase::test::assembleJasm(src));
    expectUntouched(container, "E.class");
}

TEST(MjPassMethodInlinerTest, RejectsSynchronizedCallee)
{
    ResourceContainer container;
    addClassResource(container, "S.class", asmDefaultClass("S",
        {
            {"public static synchronized", "f", "()I", "      iconst_1\n      ireturn\n"},
            {"public static", "m", "()I",
             "      invokestatic S.f ()I\n      ireturn\n"},
        }));
    expectUntouched(container, "S.class");
}

TEST(MjPassMethodInlinerTest, RejectsSelfRecursiveCallee)
{
    ResourceContainer container;
    // R.f(I)I：iload_0; invokestatic R.f; ireturn（体自调）
    addClassResource(container, "R.class", asmDefaultClass("R",
        {
            {"public static", "f", "(I)I",
             "      iload 0\n      invokestatic R.f (I)I\n      ireturn\n"},
            {"public static", "m", "(I)I",
             "      iload 0\n      invokestatic R.f (I)I\n      ireturn\n"},
        }));
    expectUntouched(container, "R.class");
}

TEST(MjPassMethodInlinerTest, RejectsIndirectRecursion)
{
    ResourceContainer container;
    // A.fa()I → B.fb()I；B.fb()I → A.fa()I（互调环，双纯计算体）
    addClassResource(container, "A.class",
                     asmDefaultClass("A",
                                     {{"public static", "fa", "()I",
                                       "      invokestatic B.fb ()I\n      ireturn\n"}}));
    addClassResource(container, "B.class",
                     asmDefaultClass("B",
                                     {{"public static", "fb", "()I",
                                       "      invokestatic A.fa ()I\n      ireturn\n"}}));
    addClassResource(container, "C.class",
                     asmDefaultClass("C",
                                     {{"public static", "m", "()I",
                                       "      invokestatic A.fa ()I\n      ireturn\n"}}));
    expectUntouched(container, "C.class");
}

TEST(MjPassMethodInlinerTest, RejectsImpureCalleeFieldWrite)
{
    ResourceContainer container;
    // W.f(I)I：iload_0; putstatic W.g; iload_0; ireturn（写堆）——字段声明
    // 走 asmClass fields 入参。
    addClassResource(container, "W.class", asmClass("W", "public super",
        "java/lang/Object", "  .field public static g I\n",
        {
            {"public static", "f", "(I)I",
             "      iload 0\n      putstatic W.g I\n      iload 0\n      ireturn\n"},
            {"public static", "m", "(I)I",
             "      iload 0\n      invokestatic W.f (I)I\n      ireturn\n"},
        }));
    expectUntouched(container, "W.class");
}

TEST(MjPassMethodInlinerTest, RejectsReadOnlyCalleeFieldRead)
{
    ResourceContainer container;
    // RD.f()I：getstatic RD.g; ireturn（只读 ≠ Pure）
    addClassResource(container, "RD.class", asmClass("RD", "public super",
        "java/lang/Object", "  .field public static g I\n",
        {
            {"public static", "f", "()I",
             "      getstatic RD.g I\n      ireturn\n"},
            {"public static", "m", "()I",
             "      invokestatic RD.f ()I\n      ireturn\n"},
        }));
    expectUntouched(container, "RD.class");
}

TEST(MjPassMethodInlinerTest, RejectsVirtualCallInBody)
{
    ResourceContainer container;
    // Vm.v()I：iconst_1; ireturn（纯）——callee 经 invokevirtual 调它；
    // V.g(LVm;)I：aload_0; invokevirtual Vm.v; ireturn；V.m()I：
    // aconst_null; invokestatic V.g; ireturn
    addClassResource(container, "Vm.class",
                     asmDefaultClass("Vm",
                                     {{"public", "v", "()I",
                                       "      iconst_1\n      ireturn\n"}}));
    addClassResource(container, "V.class", asmDefaultClass("V",
        {
            {"public static", "g", "(LVm;)I",
             "      aload 0\n      invokevirtual Vm.v ()I\n      ireturn\n"},
            {"public static", "m", "()I",
             "      aconst_null\n      invokestatic V.g (LVm;)I\n      ireturn\n"},
        }));
    expectUntouched(container, "V.class");
}

TEST(MjPassMethodInlinerTest, RejectsCrossClassPrivate)
{
    ResourceContainer container;
    // H2.priv(I)I private 实例；C2.m()I：aconst_null; invokespecial H2.priv
    //（跨类 invokespecial）
    addClassResource(container, "H2.class",
                     asmDefaultClass("H2",
                                     {{"private", "priv", "(I)I",
                                       "      iload 1\n      iconst_2\n      imul\n"
                                       "      ireturn\n"}}));
    addClassResource(container, "C2.class",
                     asmDefaultClass("C2",
                                     {{"public static", "m", "()I",
                                       "      aconst_null\n      invokespecial "
                                       "H2.priv (I)I\n      ireturn\n"}}));
    expectUntouched(container, "C2.class");
}

TEST(MjPassMethodInlinerTest, RejectsCrossClassStaticOwnerWithClinit)
{
    ResourceContainer container;
    // H3.k()I public static 纯；H3 带 <clinit>（类初始化触发门）
    addClassResource(container, "H3.class", asmDefaultClass("H3",
        {
            {"public static", "k", "()I", "      bipush 100\n      ireturn\n"},
            {"public static", "<clinit>", "()V", "      return\n"},
        }));
    addClassResource(container, "C3.class",
                     asmDefaultClass("C3",
                                     {{"public static", "m", "()I",
                                       "      invokestatic H3.k ()I\n      ireturn\n"}}));
    expectUntouched(container, "C3.class");
}

TEST(MjPassMethodInlinerTest, RejectsCrossClassNonPublicCallee)
{
    ResourceContainer container;
    // H4.k()I 包私有 static 纯（缺 ACC_PUBLIC）——运行时包等价依赖
    // defining loader 身份，字节不可证，v2 仍拒绝（停报面）。
    addClassResource(container, "H4.class",
                     asmDefaultClass("H4",
                                     {{"static", "k", "()I",
                                       "      iconst_1\n      ireturn\n"}}));
    addClassResource(container, "C4.class",
                     asmDefaultClass("C4",
                                     {{"public static", "m", "()I",
                                       "      invokestatic H4.k ()I\n      ireturn\n"}}));
    expectUntouched(container, "C4.class");
}

TEST(MjPassMethodInlinerTest, RejectsCrossClassBodyWithCall)
{
    ResourceContainer container;
    // H5.inner()I public static 纯；H5.w()I public static：invokestatic
    // H5.inner（体含调用——跨类体不安全）；C5.m：invokestatic H5.w
    addClassResource(container, "H5.class", asmDefaultClass("H5",
        {
            {"public static", "inner", "()I", "      iconst_1\n      ireturn\n"},
            {"public static", "w", "()I",
             "      invokestatic H5.inner ()I\n      ireturn\n"},
        }));
    addClassResource(container, "C5.class",
                     asmDefaultClass("C5",
                                     {{"public static", "m", "()I",
                                       "      invokestatic H5.w ()I\n      ireturn\n"}}));
    // H5 内部（inner → w 同类 static 纯）可合法自内联；被拒的是跨类站点
    // C5.m → H5.w（体含成员引用指令，bodyCrossClassSafe 门）。站点保留
    // 断言只看 C5 字节。
    expectSiteUntouched(container, "C5.class");
}

TEST(MjPassMethodInlinerTest, RejectsBranchToMethodEnd)
{
    ResourceContainer container;
    // B2.f(I)I：iload_0; ifeq → 方法末边界（跳过 ireturn 之后）; ireturn
    // ——例外①载面：装配合法基座后 ifeq 单点重锚到 code_length。
    const Bytes f = branchToMethodEnd(asmDefaultClass("B2",
        {
            {"public static", "f", "(I)I",
             "      iload 0\n      ifeq Lret\n      Lret:\n      ireturn\n"},
            {"public static", "m", "(I)I",
             "      iload 0\n      invokestatic B2.f (I)I\n      ireturn\n"},
        }),
        0x99);   // IFEQ
    ASSERT_NE(f.size(), 0u);
    addClassResource(container, "B2.class", f);
    expectUntouched(container, "B2.class");
}

// ── v2 拒绝面（扩面判据的 fail-closed 反证）──

TEST(MjPassMethodInlinerTest, RejectsVirtualOverridableCallee)
{
    ResourceContainer container;
    // Vm2.v(I)I public 非 final、Vm2 非 final：唯一实现不可证（容器外
    // 子类可覆盖）——final 收窄之外的去虚化证据不成立。
    addClassResource(container, "Vm2.class",
                     asmDefaultClass("Vm2",
                                     {{"public", "v", "(I)I",
                                       "      iload 1\n      iconst_1\n      iadd\n"
                                       "      ireturn\n"}}));
    addClassResource(container, "OV.class", asmDefaultClass("OV",
        {{"public static", "m", "(LVm2;)I",
          "      aload 1\n      iconst_1\n      invokevirtual Vm2.v (I)I\n"
          "      ireturn\n"}}));
    expectUntouched(container, "OV.class");
}

TEST(MjPassMethodInlinerTest, RejectsVirtualFinalMethodCrossClassBodyWithCall)
{
    ResourceContainer container;
    // FD.helper()I public static 纯；FD.fin()I public final：invokestatic
    // FD.helper（体含成员引用——去虚化证据成立但跨类体不安全，
    // bodyCrossClassSafe 门拒收）；FDU.m(LFD;)I：aload_1; invokevirtual FD.fin
    addClassResource(container, "FD.class", asmDefaultClass("FD",
        {
            {"public static", "helper", "()I", "      iconst_1\n      ireturn\n"},
            {"public final", "fin", "()I",
             "      invokestatic FD.helper ()I\n      ireturn\n"},
        }));
    addClassResource(container, "FDU.class", asmDefaultClass("FDU",
        {{"public static", "m", "(LFD;)I",
          "      aload 1\n      invokevirtual FD.fin ()I\n      ireturn\n"}}));
    // FD 内部（helper → fin 同类）可合法自内联；被拒的是跨类站点 FDU.m →
    // FD.fin。站点保留断言只看 FDU 字节。
    expectSiteUntouched(container, "FDU.class");
}

TEST(MjPassMethodInlinerTest, RejectsNestmateMissingAttrs)
{
    ResourceContainer container;
    // 无 NestHost/NestMembers 属性的跨类 private static 调用：nest 关系
    // 不可证，fail-closed（HotSpot 侧此类调用本就 IllegalAccessError）。
    addClassResource(container, "WA.class",
                     asmDefaultClass("WA",
                                     {{"private static", "priv", "()I",
                                       "      iconst_1\n      ireturn\n"}}));
    addClassResource(container, "WB.class",
                     asmDefaultClass("WB",
                                     {{"public static", "m", "()I",
                                       "      invokestatic WA.priv ()I\n      ireturn\n"}}));
    expectUntouched(container, "WB.class");
}

TEST(MjPassMethodInlinerTest, RejectsNestmateStaleMembership)
{
    ResourceContainer container;
    // 属性链不完整：宿主 NestMembers 只列 A 未列 B——HotSpot 懒验证
    // 口径下 B 不在 nest（访问本会 IllegalAccessError），fail-closed。
    addClassResource(container, "SH.class", withNestMembers(asmDefaultClass("SH", {}),
                                                            {"SA"}));
    addClassResource(container, "SA.class",
                     withNestHost(asmDefaultClass("SA",
                                                  {{"private static", "priv", "()I",
                                                    "      iconst_1\n      ireturn\n"}}),
                                  "SH"));
    addClassResource(container, "SB.class",
                     withNestHost(asmDefaultClass("SB",
                                                  {{"public static", "m", "()I",
                                                    "      invokestatic SA.priv ()I\n"
                                                    "      ireturn\n"}}),
                                  "SH"));
    expectUntouched(container, "SB.class");
}

TEST(MjPassMethodInlinerTest, RejectsProtectedWithoutSubclass)
{
    ResourceContainer container;
    // protected static + 无子类关系的跨类调用者：JVM protected 规则的
    // 包等价支字节不可证，子类支不成立 → 拒收。
    addClassResource(container, "PD.class",
                     asmDefaultClass("PD",
                                     {{"protected static", "k", "()I",
                                       "      iconst_1\n      ireturn\n"}}));
    addClassResource(container, "PU.class",
                     asmDefaultClass("PU",
                                     {{"public static", "m", "()I",
                                       "      invokestatic PD.k ()I\n      ireturn\n"}}));
    expectUntouched(container, "PU.class");
}

TEST(MjPassMethodInlinerTest, RejectsDeepLoopCallee)
{
    ResourceContainer container;
    // DL.f(I)I：goto 自身 ×3 + ireturn——回边数 3 超浅循环帽 2。
    addClassResource(container, "DL.class", asmDefaultClass("DL",
        {
            {"public static", "f", "(I)I",
             "      La:\n      goto La\n      Lb:\n      goto Lb\n      Lc:\n"
             "      goto Lc\n      ireturn\n"},
            {"public static", "m", "(I)I",
             "      iload 0\n      invokestatic DL.f (I)I\n      ireturn\n"},
        }));
    expectUntouched(container, "DL.class");
}

TEST(MjPassMethodInlinerTest, RejectsImpureLoopCallee)
{
    ResourceContainer container;
    // IL.f(I)I：iload_0; putstatic IL.g; goto 回边; ireturn——循环形态
    // 合法但写堆，纯度门拒收（v1 RejectsLoopCalleeBackEdge 的 v2 重定位：
    // 回边本身已放行，拒绝责任回归纯度/预算/形状门）。
    addClassResource(container, "IL.class", asmClass("IL", "public super",
        "java/lang/Object", "  .field public static g I\n",
        {
            {"public static", "f", "(I)I",
             "      Lhead:\n      iload 0\n      putstatic IL.g I\n      goto Lhead\n"
             "      ireturn\n"},
            {"public static", "m", "(I)I",
             "      iload 0\n      invokestatic IL.f (I)I\n      ireturn\n"},
        }));
    expectUntouched(container, "IL.class");
}

TEST(MjPassMethodInlinerTest, RejectsSwitchTargetToMethodEnd)
{
    ResourceContainer container;
    // SWE.f(I)I：iload_0; lookupswitch(0 pairs, default→方法末边界); pop;
    // ireturn——switch 随体已放行，末边界目标门仍拒（v1 门不放宽）。
    // 例外①载面：装配合法基座（default 落 Ld）后 default 单点重锚到
    // code_length；重锚后 pop/ireturn 段与原夹具同为死段。
    const Bytes f = switchDefaultToMethodEnd(asmDefaultClass("SWE",
        {
            {"public static", "f", "(I)I",
             "      iconst_1\n"
             "      iload 0\n"
             "      lookupswitch { default: Ld }\n"
             "      Ld:\n"
             "      pop\n"
             "      iconst_1\n"
             "      ireturn\n"},
            {"public static", "m", "(I)I",
             "      iload 0\n      invokestatic SWE.f (I)I\n      ireturn\n"},
        }));
    ASSERT_NE(f.size(), 0u);
    addClassResource(container, "SWE.class", f);
    expectUntouched(container, "SWE.class");
}

TEST(MjPassMethodInlinerTest, RejectsInterfaceNonPrivateSite)
{
    ResourceContainer container;
    // 接口 default 方法（public 非 private）经 invokeinterface：实现类
    // 集合对容器外开放，唯一实现不可证 → 拒收（v2 接口面只收 private）。
    addClassResource(container, "IJ.class", asmClass("IJ", "public interface abstract",
        "java/lang/Object", "",
        {{"public", "dm", "()I", "      iconst_1\n      ireturn\n"}}));
    addClassResource(container, "IJU.class", asmDefaultClass("IJU",
        {{"public static", "m", "(LIJ;)I",
          "      aload 1\n      invokeinterface IJ.dm ()I\n      ireturn\n"}}));
    expectUntouched(container, "IJU.class");
}

// ── 幂等与鲁棒 ──

TEST(MjPassMethodInlinerTest, IdempotentSecondRun)
{
    ResourceContainer container;
    addClassResource(container, "H6.class", buildDouble("H6", "k2", "public static", "()I"));
    addClassResource(container, "C6.class",
                     asmDefaultClass("C6",
                                     {{"public static", "m", "()I",
                                       "      invokestatic H6.k2 ()I\n      ireturn\n"}}));

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
    addClassResource(container, "H7.class", buildDouble("H7", "k3", "public static", "()I"));
    addClassResource(container, "C7.class",
                     asmDefaultClass("C7",
                                     {{"public static", "m", "()I",
                                       "      invokestatic H7.k3 ()I\n      ireturn\n"}}));
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
    // 调用目标类不在容器内（无体可取，去虚化解析亦不可证）→ 站点保留。
    addClassResource(container, "C8.class",
                     asmDefaultClass("C8",
                                     {{"public static", "m", "()I",
                                       "      invokestatic Missing.k ()I\n      ireturn\n"}}));
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
