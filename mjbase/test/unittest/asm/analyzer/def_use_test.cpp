/// @file        def_use_test.cpp
/// @brief       slot 级 def-use 测试：定义集合格、store→load 链、分支合并、
///              死存储、dup 复制、cat2 槽布局、异常边处理器合并与字段访问事实。
///
/// 方法体经手拼 class 字节 + 会话升格获得（带真实 srcPc 与异常表），
/// 断言落在 analyzeDefUse 的边/访问/字段事实面上。
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"

#include "analysis/class_bytes_builder.hpp"
#include "mjbase/asm/analyzer/def_use.hpp"

namespace {

using jbc::analysis::MethodSig;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using mjbase::analysis::test_support::Bytes;
using mjbase::analysis::test_support::ClassBytesBuilder;
using mjbase::analysis::test_support::CodeEmit;
using mjbase::analysis::test_support::ExcSpec;
using mjbase::analysis::test_support::FieldSpec;
using mjbase::analysis::test_support::MethodSpec;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::DefSet;
using mjbase::asm_analyzer::DefUseEdge;
using mjbase::asm_analyzer::DefUseSlot;
using mjbase::asm_analyzer::FieldAccess;
using mjbase::asm_analyzer::LocalAccess;
using mjbase::asm_analyzer::MethodDefUse;

constexpr ca::u16 kPublic = 0x0001;
constexpr ca::u16 kStatic = 0x0008;

MethodSpec method(std::string_view name, std::string_view desc, ca::u16 maxStack,
                  ca::u16 maxLocals, std::function<void(CodeEmit&)> emit,
                  ca::u16 access = kPublic | kStatic,
                  std::vector<ExcSpec> exceptions = {})
{
    MethodSpec out;
    out.access     = access;
    out.name       = name;
    out.desc       = desc;
    out.maxStack   = maxStack;
    out.maxLocals  = maxLocals;
    out.emit       = std::move(emit);
    out.exceptions = std::move(exceptions);
    return out;
}

/// 单类夹具：会话保活 + 按名字取升格后的 CodeUnit。
struct Fixture
{
    std::shared_ptr<jbc::ir::ClassSession> session;
    std::vector<jbc::ir::CodeVisit>        visits;

    explicit Fixture(const Bytes& bytes)
    {
        auto made = jbc::ir::makeSessionFromBytes(
            jbc::core::ByteView{bytes.data(), bytes.size()});
        EXPECT_TRUE(made.is_ok());
        session = std::move(made).unwrap();
        visits  = jbc::ir::collectCodeUnits(*session);
    }

    const jbc::core::ConstantPool& cp() const { return session->backing().cp; }

    const CodeUnit* code(std::string_view name) const
    {
        for (const jbc::ir::CodeVisit& visit : visits) {
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) {
                continue;
            }
            const jbc::ir::MethodUnit& unit = session->unit().methods[visit.methodIdx];
            if (session->unit().symbols().str(unit.name) == name) {
                return visit.unit.get();
            }
        }
        return nullptr;
    }
};

bool hasEdge(const MethodDefUse& use, DefId def, jbc::ir::InsnId at, DefUseSlot slot,
             ca::u32 index)
{
    for (const DefUseEdge& e : use.edges()) {
        if (e.def == def && e.use == at && e.slot == slot && e.index == index) {
            return true;
        }
    }
    return false;
}

}   // namespace

// ---------------------------------------------------------------------------
// 定义集合格
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, DefUseSetOrdersDedupesAndTruncates)
{
    DefSet set;
    EXPECT_TRUE(set.empty());
    EXPECT_TRUE(set.add(DefId::instruction(InsnId{1, 7, 0})));
    EXPECT_TRUE(set.add(DefId::instruction(InsnId{1, 3, 0})));
    EXPECT_TRUE(set.add(DefId::parameter(2)));
    EXPECT_FALSE(set.add(DefId::parameter(2)));

    // Parameter(0) 在 Instruction(1) 之前：kind 是排序主键。
    ASSERT_EQ(set.size(), 3u);
    EXPECT_TRUE(set.at(0) == DefId::parameter(2));
    EXPECT_TRUE(set.at(1) == DefId::instruction(InsnId{1, 3, 0}));
    EXPECT_TRUE(set.at(2) == DefId::instruction(InsnId{1, 7, 0}));
    EXPECT_FALSE(set.truncated());

    // 超过 kMaxDefs：保留字典序最小的 16 个，且插入顺序不影响结果。
    DefSet forward;
    DefSet backward;
    for (ca::u32 i = 0; i < DefSet::kMaxDefs + 4; ++i) {
        forward.add(DefId::instruction(InsnId{2, i, 0}));
    }
    for (ca::u32 i = DefSet::kMaxDefs + 3; i > 0; --i) {
        backward.add(DefId::instruction(InsnId{2, i - 1, 0}));
    }
    ASSERT_EQ(forward.size(), DefSet::kMaxDefs);
    EXPECT_TRUE(forward.truncated());
    EXPECT_TRUE(forward == backward);
    EXPECT_TRUE(forward.at(0) == DefId::instruction(InsnId{2, 0, 0}));
    EXPECT_TRUE(forward.at(DefSet::kMaxDefs - 1) ==
                DefId::instruction(InsnId{2, DefSet::kMaxDefs - 1, 0}));

    // merge 传播 truncated。
    DefSet merged = forward;
    merged.add(DefId::instruction(InsnId{2, 99, 0}));
    EXPECT_TRUE(merged.truncated());
}

// ---------------------------------------------------------------------------
// 局部链
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, DefUseStoreLoadChain)
{
    ClassBytesBuilder b("Chain");
    const Bytes bytes = b.assemble({method("run", "()I", 1, 2, [](CodeEmit& e) {
        e.i32const(7);
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.op(jbc::core::Op::IRETURN);
    })});
    Fixture fx(bytes);
    const CodeUnit* code = fx.code("run");
    ASSERT_NE(code, nullptr);
    ASSERT_EQ(code->dom.order().size(), 4u);

    const InsnId iconst = code->dom.order()[0];
    const InsnId store  = code->dom.order()[1];
    const InsnId load   = code->dom.order()[2];
    const InsnId ret    = code->dom.order()[3];

    MethodDefUse u = mjbase::asm_analyzer::analyzeDefUse(
        fx.cp(), *code, MethodSig{true, "()I"});
    ASSERT_TRUE(u.ok());
    EXPECT_FALSE(u.truncated());

    // iconst 值被 store 消费；store 重定义 local1；load 穿透把定义带进栈。
    EXPECT_TRUE(hasEdge(u, DefId::instruction(iconst), store, DefUseSlot::Stack, 0));
    EXPECT_TRUE(hasEdge(u, DefId::instruction(store), load, DefUseSlot::Local, 1));
    EXPECT_TRUE(hasEdge(u, DefId::instruction(store), ret, DefUseSlot::Stack, 0));

    const auto storeUses = u.usesOf(DefId::instruction(store));
    EXPECT_EQ(storeUses.size(), 2u);

    const auto reads = u.readsOfLocal(1);
    ASSERT_EQ(reads.size(), 1u);
    EXPECT_TRUE(reads[0]->insn == load);
    EXPECT_TRUE(reads[0]->defs.contains(DefId::instruction(store)));

    const InsnId* resolved = u.instructionOf(DefId::instruction(store));
    ASSERT_NE(resolved, nullptr);
    EXPECT_TRUE(*resolved == store);
    EXPECT_EQ(u.instructionOf(DefId::parameter(0)), nullptr);
}

TEST(MjAsmAnalyzerTest, DefUseParametersKeepLocalIdentity)
{
    ClassBytesBuilder b("Params");
    const Bytes bytes = b.assemble({method("add", "(II)I", 2, 2, [](CodeEmit& e) {
        e.varInsn(jbc::core::Op::ILOAD, 0);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.op(jbc::core::Op::IADD);
        e.op(jbc::core::Op::IRETURN);
    })});
    Fixture fx(bytes);
    const CodeUnit* code = fx.code("add");
    ASSERT_NE(code, nullptr);
    ASSERT_EQ(code->dom.order().size(), 4u);

    const InsnId add = code->dom.order()[2];
    MethodDefUse u = mjbase::asm_analyzer::analyzeDefUse(
        fx.cp(), *code, MethodSig{true, "(II)I"});
    ASSERT_TRUE(u.ok());

    // 栈顶是 param1，其下是 param0（depth 按值计）；两者各被自己的 load
    // 读出一次、再各在 iadd 处消费一次。
    EXPECT_TRUE(hasEdge(u, DefId::parameter(1), add, DefUseSlot::Stack, 0));
    EXPECT_TRUE(hasEdge(u, DefId::parameter(0), add, DefUseSlot::Stack, 1));
    EXPECT_TRUE(hasEdge(u, DefId::parameter(0), code->dom.order()[0], DefUseSlot::Local, 0));
    EXPECT_TRUE(hasEdge(u, DefId::parameter(1), code->dom.order()[1], DefUseSlot::Local, 1));
    EXPECT_EQ(u.usesOf(DefId::parameter(0)).size(), 2u);
    EXPECT_EQ(u.usesOf(DefId::parameter(1)).size(), 2u);
}

TEST(MjAsmAnalyzerTest, DefUseBranchMergeKeepsBothDefs)
{
    ClassBytesBuilder b("Merge");
    const Bytes bytes = b.assemble({method("pick", "(I)I", 1, 2, [](CodeEmit& e) {
        const std::size_t lElse = e.newLabel();
        const std::size_t lJoin = e.newLabel();
        e.varInsn(jbc::core::Op::ILOAD, 0);
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.branch(jbc::core::Op::IFEQ, lElse);
        e.i32const(2);
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.branch(jbc::core::Op::GOTO, lJoin);
        e.bindLabel(lElse);
        e.i32const(3);
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.bindLabel(lJoin);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.op(jbc::core::Op::IRETURN);
    })});
    Fixture fx(bytes);
    const CodeUnit* code = fx.code("pick");
    ASSERT_NE(code, nullptr);
    ASSERT_EQ(code->dom.order().size(), 11u);
    for (const InsnId& id : code->dom.order()) {
        ASSERT_NE(code->dom.peek(id), nullptr);
    }

    const InsnId thenStore = code->dom.order()[5];
    const InsnId elseStore = code->dom.order()[8];
    const InsnId joinLoad  = code->dom.order()[9];

    MethodDefUse u = mjbase::asm_analyzer::analyzeDefUse(
        fx.cp(), *code, MethodSig{true, "(I)I"});
    ASSERT_TRUE(u.ok());

    const auto reads = u.readsOfLocal(1);
    ASSERT_EQ(reads.size(), 2u);   // 分支前的读 + 汇合后的读
    const LocalAccess& join = *reads[1];
    EXPECT_TRUE(join.insn == joinLoad);
    EXPECT_EQ(join.defs.size(), 2u);
    EXPECT_TRUE(join.defs.contains(DefId::instruction(thenStore)));
    EXPECT_TRUE(join.defs.contains(DefId::instruction(elseStore)));
    EXPECT_TRUE(hasEdge(u, DefId::instruction(thenStore), joinLoad, DefUseSlot::Local, 1));
    EXPECT_TRUE(hasEdge(u, DefId::instruction(elseStore), joinLoad, DefUseSlot::Local, 1));

    // 分支前的 store 仍被 ifeq 前的 load 使用（load 的局部读 + ifeq 的栈消费），
    // 但在汇合点已被覆盖。
    EXPECT_EQ(u.usesOf(DefId::instruction(code->dom.order()[1])).size(), 2u);
}

TEST(MjAsmAnalyzerTest, DefUseDeadStoreHasNoUses)
{
    ClassBytesBuilder b("DeadStore");
    const Bytes bytes = b.assemble({method("run", "()I", 1, 2, [](CodeEmit& e) {
        e.i32const(1);
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.i32const(2);
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.op(jbc::core::Op::IRETURN);
    })});
    Fixture fx(bytes);
    const CodeUnit* code = fx.code("run");
    ASSERT_NE(code, nullptr);
    ASSERT_EQ(code->dom.order().size(), 6u);

    const InsnId firstStore = code->dom.order()[1];
    const InsnId lastStore  = code->dom.order()[3];

    MethodDefUse u = mjbase::asm_analyzer::analyzeDefUse(
        fx.cp(), *code, MethodSig{true, "()I"});
    ASSERT_TRUE(u.ok());

    // 第一次 store 被后续 store 覆盖：没有任何使用边消费它的定义。
    EXPECT_TRUE(u.usesOf(DefId::instruction(firstStore)).empty());
    EXPECT_TRUE(hasEdge(u, DefId::instruction(lastStore), code->dom.order()[4],
                        DefUseSlot::Local, 1));
}

TEST(MjAsmAnalyzerTest, DefUseDupCopiesWithoutUse)
{
    ClassBytesBuilder b("DupCase");
    const Bytes bytes = b.assemble({method("run", "()I", 2, 2, [](CodeEmit& e) {
        e.i32const(1);
        e.op(jbc::core::Op::DUP);
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.op(jbc::core::Op::IRETURN);
    })});
    Fixture fx(bytes);
    const CodeUnit* code = fx.code("run");
    ASSERT_NE(code, nullptr);
    ASSERT_EQ(code->dom.order().size(), 4u);

    const InsnId iconst = code->dom.order()[0];
    const InsnId dup    = code->dom.order()[1];
    const InsnId store  = code->dom.order()[2];
    const InsnId ret    = code->dom.order()[3];

    MethodDefUse u = mjbase::asm_analyzer::analyzeDefUse(
        fx.cp(), *code, MethodSig{true, "()I"});
    ASSERT_TRUE(u.ok());

    // dup 只复制槽：不产使用边；两个副本分别被 store 与 ireturn 消费。
    EXPECT_TRUE(u.usesAt(dup).empty());
    EXPECT_TRUE(hasEdge(u, DefId::instruction(iconst), store, DefUseSlot::Stack, 0));
    EXPECT_TRUE(hasEdge(u, DefId::instruction(iconst), ret, DefUseSlot::Stack, 0));
    EXPECT_EQ(u.usesOf(DefId::instruction(iconst)).size(), 2u);
}

TEST(MjAsmAnalyzerTest, DefUseCat2SlotsStayPaired)
{
    ClassBytesBuilder b("Wide");
    const Bytes bytes = b.assemble({method("pass", "(J)J", 2, 4, [](CodeEmit& e) {
        e.varInsn(jbc::core::Op::LLOAD, 0);
        e.varInsn(jbc::core::Op::LSTORE, 2);
        e.varInsn(jbc::core::Op::LLOAD, 2);
        e.op(jbc::core::Op::LRETURN);
    })});
    Fixture fx(bytes);
    const CodeUnit* code = fx.code("pass");
    ASSERT_NE(code, nullptr);
    ASSERT_EQ(code->dom.order().size(), 4u);

    const InsnId load0  = code->dom.order()[0];
    const InsnId store  = code->dom.order()[1];
    const InsnId load2  = code->dom.order()[2];
    const InsnId lret   = code->dom.order()[3];

    MethodDefUse u = mjbase::asm_analyzer::analyzeDefUse(
        fx.cp(), *code, MethodSig{true, "(J)J"});
    ASSERT_TRUE(u.ok());

    // cat2 弹栈只记一个值使用（底槽不产第二份边）。
    EXPECT_TRUE(hasEdge(u, DefId::parameter(0), store, DefUseSlot::Stack, 0));
    EXPECT_TRUE(hasEdge(u, DefId::instruction(store), lret, DefUseSlot::Stack, 0));
    EXPECT_TRUE(hasEdge(u, DefId::instruction(store), load2, DefUseSlot::Local, 2));
    EXPECT_TRUE(hasEdge(u, DefId::parameter(0), load0, DefUseSlot::Local, 0));
    EXPECT_EQ(u.usesOf(DefId::instruction(store)).size(), 2u);

    const auto reads = u.readsOfLocal(2);
    ASSERT_EQ(reads.size(), 1u);
    EXPECT_TRUE(reads[0]->defs.contains(DefId::instruction(store)));
}

// ---------------------------------------------------------------------------
// 异常边
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, DefUseExceptionHandlerInheritsThrowSiteLocals)
{
    ClassBytesBuilder b("ThrowMerge");
    MethodSpec thrower = method("run", "()I", 2, 3, [](CodeEmit& e) {
        const std::size_t start   = e.newLabel();
        const std::size_t end     = e.newLabel();
        const std::size_t handler = e.newLabel();
        e.i32const(2);
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.bindLabel(start);
        e.typeInsn(jbc::core::Op::NEW, "java/lang/Object");
        e.op(jbc::core::Op::POP);
        e.bindLabel(end);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.op(jbc::core::Op::IRETURN);
        e.bindLabel(handler);
        e.varInsn(jbc::core::Op::ASTORE, 2);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.op(jbc::core::Op::IRETURN);
    });
    thrower.exceptions = {ExcSpec{0, 1, 2, "java/lang/Throwable"}};
    const Bytes bytes = b.assemble({thrower});
    Fixture fx(bytes);
    const CodeUnit* code = fx.code("run");
    ASSERT_NE(code, nullptr);
    ASSERT_EQ(code->exc.size(), 1u);

    const InsnId store = code->dom.order()[1];

    MethodDefUse u = mjbase::asm_analyzer::analyzeDefUse(
        fx.cp(), *code, MethodSig{true, "()I"});
    ASSERT_TRUE(u.ok());

    // handler 的 local1 来自抛出点 in-state：仍指向 try 前的 store。
    const auto reads = u.readsOfLocal(1);
    ASSERT_EQ(reads.size(), 2u);
    EXPECT_TRUE(reads[1]->defs.contains(DefId::instruction(store)));
}

TEST(MjAsmAnalyzerTest, DefUseDeadTryRangeHandlerConservativeSeed)
{
    ClassBytesBuilder b("DeadTry");
    MethodSpec dead = method("run", "()I", 1, 3, [](CodeEmit& e) {
        const std::size_t start   = e.newLabel();
        const std::size_t end     = e.newLabel();
        const std::size_t handler = e.newLabel();
        e.i32const(3);
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.bindLabel(start);
        e.op(jbc::core::Op::NOP);
        e.bindLabel(end);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.op(jbc::core::Op::IRETURN);
        e.bindLabel(handler);
        e.varInsn(jbc::core::Op::ASTORE, 2);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.op(jbc::core::Op::IRETURN);
    });
    dead.exceptions = {ExcSpec{0, 1, 2, "java/lang/Throwable"}};
    const Bytes bytes = b.assemble({dead});
    Fixture fx(bytes);
    const CodeUnit* code = fx.code("run");
    ASSERT_NE(code, nullptr);
    const InsnId store = code->dom.order()[1];

    MethodDefUse u = mjbase::asm_analyzer::analyzeDefUse(
        fx.cp(), *code, MethodSig{true, "()I"});
    ASSERT_TRUE(u.ok());

    // libjbc 0.4.22 起精确档异常边发射域放宽到范围内可达指令（JVM 保守
    // 口径）：范围内无可抛指令也不再剪 handler——其 local1 读保留，定义
    // 归 try 前的 store（入口 in-state 即抛出点帧）。
    const auto reads = u.readsOfLocal(1);
    ASSERT_EQ(reads.size(), 2u);
    EXPECT_TRUE(reads[1]->defs.contains(DefId::instruction(store)));
}

// ---------------------------------------------------------------------------
// 字段访问事实
// ---------------------------------------------------------------------------

TEST(MjAsmAnalyzerTest, DefUseFieldAccessFacts)
{
    ClassBytesBuilder b("FieldFixture");
    b.addField(FieldSpec{kPublic, "value", "I"});
    b.addField(FieldSpec{static_cast<ca::u16>(kPublic | kStatic), "sval", "I"});
    const Bytes bytes = b.assemble({method("touch", "()V", 2, 2, [](CodeEmit& e) {
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.fieldInsn(jbc::core::Op::GETFIELD, "FieldFixture", "value", "I");
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.varInsn(jbc::core::Op::ALOAD, 0);
        e.varInsn(jbc::core::Op::ILOAD, 1);
        e.fieldInsn(jbc::core::Op::PUTFIELD, "FieldFixture", "value", "I");
        e.fieldInsn(jbc::core::Op::GETSTATIC, "FieldFixture", "sval", "I");
        e.varInsn(jbc::core::Op::ISTORE, 1);
        e.fieldInsn(jbc::core::Op::GETSTATIC, "FieldFixture", "sval", "I");
        e.op(jbc::core::Op::POP);
        e.i32const(3);
        e.fieldInsn(jbc::core::Op::PUTSTATIC, "FieldFixture", "sval", "I");
        e.op(jbc::core::Op::RETURN);
    }, kPublic)});
    Fixture fx(bytes);
    const CodeUnit* code = fx.code("touch");
    ASSERT_NE(code, nullptr);

    MethodDefUse u = mjbase::asm_analyzer::analyzeDefUse(
        fx.cp(), *code, MethodSig{false, "()V", "touch", "FieldFixture"});
    ASSERT_TRUE(u.ok());
    EXPECT_FALSE(u.hasUnresolvedFieldRefs());

    const std::vector<FieldAccess>& fields = u.fieldAccesses();
    ASSERT_EQ(fields.size(), 5u);

    const FieldAccess& getValue = fields[0];
    EXPECT_EQ(getValue.owner, "FieldFixture");
    EXPECT_EQ(getValue.name, "value");
    EXPECT_EQ(getValue.desc, "I");
    EXPECT_FALSE(getValue.write);
    EXPECT_FALSE(getValue.isStatic);
    EXPECT_TRUE(getValue.objectDefs.contains(DefId::parameter(0)));
    EXPECT_TRUE(getValue.valueDefs.empty());

    const FieldAccess& putValue = fields[1];
    EXPECT_TRUE(putValue.write);
    EXPECT_FALSE(putValue.isStatic);
    EXPECT_TRUE(putValue.objectDefs.contains(DefId::parameter(0)));
    ASSERT_EQ(putValue.valueDefs.size(), 1u);
    EXPECT_EQ(putValue.valueDefs.at(0).kind, DefId::Kind::Instruction);

    EXPECT_FALSE(fields[2].write);
    EXPECT_TRUE(fields[2].isStatic);
    EXPECT_EQ(fields[2].name, "sval");
    EXPECT_TRUE(fields[2].objectDefs.empty());

    EXPECT_FALSE(fields[3].write);
    EXPECT_TRUE(fields[3].isStatic);

    EXPECT_TRUE(fields[4].write);
    EXPECT_TRUE(fields[4].isStatic);
    ASSERT_EQ(fields[4].valueDefs.size(), 1u);
    EXPECT_TRUE(fields[4].valueDefs.at(0) ==
                DefId::instruction(code->dom.order()[code->dom.order().size() - 3]));
}
