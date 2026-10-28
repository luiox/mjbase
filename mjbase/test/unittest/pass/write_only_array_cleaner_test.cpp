// WriteOnlyArrayCleaner（#37 第三件）：只写数组分配连存储窗整体删除——
// S1 直通形/S2 暂存形窗匹配、纯常量元素/下标、守卫面（读取逃逸/非常量
// 生产者/异常域相交/窗内锚标/size 生产者非纯）、注册面、与
// ArgumentArrayScalarizer 的管线串联裁决（调用窗无残段 + 哑数组串联清理）。
// 夹具经 shared/structural_pass_util.hpp 手拼 classfile。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/pass/argument_array_scalarizer.hpp"
#include "mjbase/pass/write_only_array_cleaner.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

constexpr ca::u16 kAccPublicStatic = 0x0009;

FixtureMethod method(const char* name, const char* desc, Bytes code)
{
    FixtureMethod m;
    m.name      = name;
    m.desc      = desc;
    m.access    = kAccPublicStatic;
    m.maxStack  = 8;
    m.maxLocals = 6;
    m.code      = std::move(code);
    return m;
}

Bytes u16b(ca::u16 v) { return {static_cast<ca::u8>(v >> 8), static_cast<ca::u8>(v)}; }

Bytes cat(Bytes a, Bytes b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Bytes anewObject(FixtureBuilder& b) { return cat({0xbd}, u16b(b.cls("java/lang/Object"))); }

const mjbase::model::ClassResource* classResource(
    mjbase::model::ResourceContainer& container, const std::string& uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

/// 读回某方法 promote 后的指令序。
std::vector<Op> methodOps(const Bytes& bytes, const char* methodName)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (unit.symbols().str(unit.methods[idx].name) != methodName) continue;
        if (!jbc::ir::hasCode(*session, idx)) return {};
        auto promoted = jbc::ir::promoteMethod(*session, idx);
        EXPECT_TRUE(promoted.is_ok());
        if (promoted.is_err()) return {};
        auto            cuRef = std::move(promoted).unwrap();
        std::vector<Op> ops;
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

std::vector<Op> runAndOps(Bytes in)
{
    const MethodPassRun r = runMethodPass(std::move(in), mjbase::pass::WriteOnlyArrayCleaner{});
    return opsOf(inspect(r.out));
}

/// S1 直通形 k=2：bipush3; anew; (dup; iconst; aconst_null; aastore)*2; pop;
/// return。
Bytes buildDirectWindow(FixtureBuilder& b)
{
    Bytes code = cat({0x10, 0x03}, anewObject(b));
    code = cat(code, {0x59, 0x03, 0x01, 0x53});
    code = cat(code, {0x59, 0x04, 0x01, 0x53});
    code = cat(code, {0x57, 0xb1});
    return code;
}

/// S2 暂存形 k=1：bipush2; anew; astore 1; aload 1; iconst_0; aconst_null;
/// aastore; return。
Bytes buildLocalWindow(FixtureBuilder& b)
{
    Bytes code = cat({0x10, 0x02}, anewObject(b));
    code = cat(code, {0x3a, 0x01});
    code = cat(code, {0x19, 0x01, 0x03, 0x01, 0x53, 0xb1});
    return code;
}

/// 单实参装箱窗调用方（对齐 scalarizer 全链夹具形态）：[iconst_1; anewarray
/// Object; dup; ldc_w "x"; iconst_0; aastore; invokestatic go; RETURN]。
Bytes buildCallerBoxed(FixtureBuilder& b)
{
    const ca::u16 objCls = b.cls("java/lang/Object");
    const ca::u16 mref   = b.methodRef("Svc", "go", "([Ljava/lang/Object;)V");
    const ca::u16 xStr   = b.strConst("x");
    Bytes         code   = cat({0x04, 0xbd}, u16b(objCls));
    code = cat(code, cat({0x59, 0x13}, cat(u16b(xStr), {0x03, 0x53})));
    code = cat(code, cat({0xb8}, cat(u16b(mref), {0xb1})));
    return code;
}

}   // namespace

// ── S1 直通形：分配 + 两组元素存储 + pop 整体消失 ──
TEST(WriteOnlyArrayCleanerTest, directWindowRemoved)
{
    FixtureBuilder b("t/U");
    b.addMethod(method("m", "()V", buildDirectWindow(b)));
    const std::vector<Op> ops = runAndOps(b.assemble());
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── S2 暂存形：astore/aload 暂存链连分配整体消失 ──
TEST(WriteOnlyArrayCleanerTest, localWindowRemoved)
{
    FixtureBuilder b("t/U");
    b.addMethod(method("m", "()V", buildLocalWindow(b)));
    const std::vector<Op> ops = runAndOps(b.assemble());
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── NEWARRAY 原始数组同面（S1 k=1，元素 int 常量） ──
TEST(WriteOnlyArrayCleanerTest, primitiveArrayWindowRemoved)
{
    FixtureBuilder b("t/U");
    b.addMethod(method(
        "m", "()V", Bytes{0x10, 0x01, 0xbc, 0x0a, 0x59, 0x03, 0x05, 0x4f, 0x57, 0xb1}));
    const std::vector<Op> ops = runAndOps(b.assemble());
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── S1 k=0 特形：分配; pop 死分配 ──
TEST(WriteOnlyArrayCleanerTest, zeroStoreAllocationPopRemoved)
{
    FixtureBuilder b("t/U");
    b.addMethod(method("m", "()V", cat(cat({0x10, 0x01}, anewObject(b)), {0x57, 0xb1})));
    const std::vector<Op> ops = runAndOps(b.assemble());
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 局部快捷形（astore_1/aload_1）同面 ──
TEST(WriteOnlyArrayCleanerTest, shorthandLocalWindowRemoved)
{
    FixtureBuilder b("t/U");
    Bytes code = cat({0x10, 0x02}, anewObject(b));
    code = cat(code, {0x4c});   // astore_1
    code = cat(code, {0x2b, 0x03, 0x01, 0x53, 0xb1});   // aload_1; iconst_0; null; aastore
    b.addMethod(method("m", "()V", std::move(code)));
    const std::vector<Op> ops = runAndOps(b.assemble());
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0], Op::RETURN);
}

// ── 读取在窗尾（arraylength）：窗保持 ──
TEST(WriteOnlyArrayCleanerTest, readonlyUseKeepsArray)
{
    FixtureBuilder b("t/U");
    Bytes code = cat({0x10, 0x01}, anewObject(b));
    code = cat(code, {0x59, 0x03, 0x01, 0x53, 0xbe, 0x57, 0xb1});
    b.addMethod(method("m", "()V", std::move(code)));
    const Bytes          in = b.assemble();
    const MethodPassRun  r  = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── S2 逃逸面：窗后局部另有读点（arraylength 喂弹）→ 保守保留 ──
TEST(WriteOnlyArrayCleanerTest, escapedLocalKeepsArray)
{
    FixtureBuilder b("t/U");
    Bytes code = cat({0x10, 0x02}, anewObject(b));
    code = cat(code, {0x3a, 0x01});
    code = cat(code, {0x19, 0x01, 0x03, 0x01, 0x53});
    code = cat(code, {0x19, 0x01, 0xbe, 0x57, 0xb1});
    b.addMethod(method("m", "()V", std::move(code)));
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 元素生产者非纯（invokestatic）：整窗放弃 ──
TEST(WriteOnlyArrayCleanerTest, nonConstElementKeepsArray)
{
    FixtureBuilder b("t/U");
    const ca::u16  mref = b.methodRef("Bog", "m", "()Ljava/lang/Object;");
    Bytes          code = cat({0x10, 0x01}, anewObject(b));
    code = cat(code, {0x59, 0x03});
    code = cat(code, cat({0xb8}, u16b(mref)));
    code = cat(code, {0x53, 0x57, 0xb1});
    b.addMethod(method("m", "()V", std::move(code)));
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── size 生产者非纯 int 常量（invokestatic 产出）：整窗放弃 ──
TEST(WriteOnlyArrayCleanerTest, impureSizeProducerKeepsArray)
{
    FixtureBuilder b("t/U");
    const ca::u16  mref = b.methodRef("Bog", "size", "()I");
    Bytes          code = cat({0xb8}, u16b(mref));
    code = cat(code, anewObject(b));
    code = cat(code, {0x59, 0x03, 0x01, 0x53, 0x57, 0xb1});
    b.addMethod(method("m", "()V", std::move(code)));
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 异常范围与窗相交：分配/存储的抛出可观测，全含亦弃 ──
TEST(WriteOnlyArrayCleanerTest, excOverlapKeepsArray)
{
    FixtureBuilder b("t/U");
    // 窗 0..6（size/alloc/dup/idx/val/aastore/pop），return@7，handler@8；
    // exc(0, 8, 8, 0) 全含窗。
    Bytes code = cat({0x10, 0x01}, anewObject(b));
    code = cat(code, {0x59, 0x03, 0x01, 0x53, 0x57, 0xb1, 0xb1});
    FixtureMethod m = method("m", "()V", std::move(code));
    m.exc           = {{0, 8, 8, 0}};
    b.addMethod(m);
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 窗内锚标签（GOTO 直指窗尾 pop，回边重执行分配语义改变）：整窗放弃 ──
TEST(WriteOnlyArrayCleanerTest, labelAnchorInsideWindowKeepsArray)
{
    FixtureBuilder b("t/U");
    // goto pc12（= pop 位）；bipush@3; anew@5; dup@8; iconst_0@9; null@10;
    // aastore@11; pop@12; return@13。
    Bytes code = {0xa7, 0x00, 0x0c};
    code = cat(code, {0x10, 0x01});
    code = cat(code, anewObject(b));
    code = cat(code, {0x59, 0x03, 0x01, 0x53, 0x57, 0xb1});
    b.addMethod(method("m", "()V", std::move(code)));
    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 管线串联裁决：调用窗由 scalarizer 自删零残段；窗外哑数组经本件清理 ──
TEST(WriteOnlyArrayCleanerTest, pipelineScalarizerResidueZeroThenDummyCleaned)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("Svc");
        const ca::u16  strCls = b.cls("java/lang/String");
        b.addMethod(method("go", "([Ljava/lang/Object;)V",
                           cat(cat({0x2a, 0x03, 0x32, 0xc0}, u16b(strCls)), {0x4c, 0xb1})));
        addClassResource(container, "Svc.class", b.assemble());
    }
    {
        FixtureBuilder b("Caller");
        b.addMethod(method("main", "()V", buildCallerBoxed(b)));
        addClassResource(container, "Caller.class", b.assemble());
    }
    {
        FixtureBuilder b("Dummy");
        b.addMethod(method("dummy", "()V", buildLocalWindow(b)));
        addClassResource(container, "Dummy.class", b.assemble());
    }

    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::ArgumentArrayScalarizer scalarizer;
    EXPECT_TRUE(scalarizer.run(container, gam).changed());

    // 裁决一：调用方装箱窗改写后零 anewarray/aastore 残段（装箱窗由
    // scalarizer 自删，死数组清理不背残留面）。
    const std::vector<Op> cops =
        methodOps(classResource(container, "Caller.class")->get(), "main");
    for (const Op op : cops) {
        EXPECT_NE(op, Op::ANEWARRAY);
        EXPECT_NE(op, Op::AASTORE);
    }

    // 裁决二：哑数组非 scalarizer 候选，survive 全局件；随后本件串联清理。
    const auto* dummy = classResource(container, "Dummy.class");
    ASSERT_NE(dummy, nullptr);
    const std::vector<Op> dopsBefore = methodOps(dummy->get(), "dummy");
    ASSERT_EQ(dopsBefore.size(), 8u);
    EXPECT_EQ(dopsBefore[1], Op::ANEWARRAY);   // bipush; anewarray; astore; ...
    const MethodPassRun r =
        runMethodPass(dummy->get(), mjbase::pass::WriteOnlyArrayCleaner{});
    EXPECT_TRUE(r.pa.changed());
    const std::vector<Op> dops = opsOf(inspect(r.out));
    ASSERT_EQ(dops.size(), 1u);
    EXPECT_EQ(dops[0], Op::RETURN);
}

// ── 注册面 ──
TEST(WriteOnlyArrayCleanerTest, registeredInRegistry)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("WriteOnlyArrayCleaner"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("WriteOnlyArrayCleaner");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
