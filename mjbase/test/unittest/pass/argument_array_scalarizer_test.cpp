// ArgumentArrayScalarizer（#37 切片 1）：装箱签名 [Ljava/lang/Object; 参数
// 标量化——被调方取参窗改直接参数槽 + 局部平移、调用方装箱窗拆直接传参、
// 描述符全局同步；守卫面（实参类型失配/数组逃逸/下标缺口/非窗调用位点/
// null 实参/重复取参）+ 注册面。夹具经 shared/structural_pass_util.hpp 手拼
// classfile；断言经会话 promote 后的指令序与符号池描述符完成。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/pass/argument_array_scalarizer.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/structural_pass_util.hpp"

using namespace mjbase::test;

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

/// 读回某方法 promote 后的指令序。
std::vector<jbc::core::Op> methodOps(const Bytes& bytes, const char* methodName)
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

/// 读回某方法的声明描述符。
std::string methodDesc(const Bytes& bytes, const char* methodName)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    for (const jbc::ir::MethodUnit& m : unit.methods) {
        if (unit.symbols().str(m.name) == methodName) {
            return std::string(unit.symbols().str(m.desc));
        }
    }
    ADD_FAILURE() << "method not found: " << methodName;
    return {};
}

/// 读回某方法体里泛型 ALOAD 的槽位序列（紧凑形折算）。
std::vector<ca::i32> aloadSlots(const Bytes& bytes, const char* methodName)
{
    auto sessionResult = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    EXPECT_TRUE(sessionResult.is_ok());
    if (sessionResult.is_err()) return {};
    auto session = std::move(sessionResult).unwrap();
    auto& unit   = session->unit();
    for (ca::usize idx = 0; idx < unit.methods.size(); ++idx) {
        if (unit.symbols().str(unit.methods[idx].name) != methodName) continue;
        auto promoted = jbc::ir::promoteMethod(*session, idx);
        EXPECT_TRUE(promoted.is_ok());
        if (promoted.is_err()) return {};
        auto              cuRef = std::move(promoted).unwrap();
        std::vector<ca::i32> out;
        for (const jbc::ir::InsnId id : cuRef->dom.order()) {
            const jbc::ir::Insn* in = cuRef->dom.peek(id);
            if (in == nullptr) continue;
            if (in->op == jbc::core::Op::ALOAD) out.push_back(in->a);
            else if (in->op >= jbc::core::Op::ALOAD_0 && in->op <= jbc::core::Op::ALOAD_3) {
                out.push_back(static_cast<ca::i32>(in->op) - static_cast<ca::i32>(jbc::core::Op::ALOAD_0));
            }
        }
        return out;
    }
    ADD_FAILURE() << "method not found: " << methodName;
    return {};
}

const mjbase::model::ClassResource* classResource(mjbase::model::ResourceContainer& container,
                                                  const std::string&                uri)
{
    return ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource*>(
        container.getResource(uri));
}

mjbase::spi::PreservedAnalyses runPass(mjbase::model::ResourceContainer& container)
{
    ca::str::Utf8StringArena           arena;
    mjbase::spi::GlobalAnalysisManager gam(arena);
    mjbase::pass::ArgumentArrayScalarizer pass;
    return pass.run(container, gam);
}

/// 被调方：go([Ljava/lang/Object;)V，取参窗两组（元素 0/1 均 checkcast
/// String）后 astore 到局部 1/2，收尾 RETURN。
Bytes buildCallee()
{
    FixtureBuilder b("Svc");
    const ca::u16 strCls = b.cls("java/lang/String");
    // 元素 0：aload_0; iconst_0; aaload; checkcast String; astore_1
    Bytes w0 = cat({0x2a, 0x03, 0x32, 0xc0}, cat(u16b(strCls), {0x4c}));
    // 元素 1：aload_0; iconst_1; aaload; checkcast String; astore_2
    Bytes w1 = cat({0x2a, 0x04, 0x32, 0xc0}, cat(u16b(strCls), {0x4d}));
    b.addMethod(method("go", "([Ljava/lang/Object;)V", cat(cat(w0, w1), {0xb1})));
    return b.assemble();
}

/// 调用方：装箱窗 [iconst_2; anewarray Object; dup; ldc "x"; iconst_0;
/// aastore; dup; ldc "y"; iconst_1; aastore; invokestatic go] + RETURN。
Bytes buildCallerBoxed(FixtureBuilder& b)
{
    const ca::u16 objCls = b.cls("java/lang/Object");
    const ca::u16 mref   = b.methodRef("Svc", "go", "([Ljava/lang/Object;)V");
    const ca::u16 xStr   = b.strConst("x");
    const ca::u16 yStr   = b.strConst("y");
    Bytes code = cat({0x05, 0xbd}, u16b(objCls));                      // iconst_2; anewarray
    code = cat(code, cat({0x59, 0x13}, cat(u16b(xStr), {0x03, 0x53}))); // dup; ldc_w x; iconst_0; aastore
    code = cat(code, cat({0x59, 0x13}, cat(u16b(yStr), {0x04, 0x53}))); // dup; ldc_w y; iconst_1; aastore
    code = cat(code, cat({0xb8}, cat(u16b(mref), {0xb1})));             // invokestatic; return
    return code;
}

}   // namespace

// ── 切片 2 件：被调方单取参窗（元素 0 checkcast Ti）──
Bytes buildCalleeTi(const std::string& tiInternal)
{
    FixtureBuilder b("Svc");
    const ca::u16 tiCls = b.cls(tiInternal);
    // 元素 0：aload_0; iconst_0; aaload; checkcast Ti; astore_1
    Bytes w0 = cat({0x2a, 0x03, 0x32, 0xc0}, cat(u16b(tiCls), {0x4c}));
    b.addMethod(method("go", "([Ljava/lang/Object;)V", cat(w0, {0xb1})));
    return b.assemble();
}

/// 单实参装箱窗调用方：[iconst_1; anewarray Object; dup; ARG; iconst_0;
/// aastore; invokestatic go; RETURN]。
Bytes buildCaller1(FixtureBuilder& b, const Bytes& arg)
{
    const ca::u16 objCls = b.cls("java/lang/Object");
    const ca::u16 mref   = b.methodRef("Svc", "go", "([Ljava/lang/Object;)V");
    Bytes code = cat({0x04, 0xbd}, u16b(objCls));
    code = cat(code, cat({0x59}, arg));
    code = cat(code, cat({0x03, 0x53}, cat({0xb8}, cat(u16b(mref), {0xb1}))));
    return code;
}

// ── 切片 2 正面：ldc Class 常量按常量类对齐 ──
TEST(ArgumentArrayScalarizerTest, classConstArgScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("java/lang/Class"));
    {
        FixtureBuilder b("Caller");
        const ca::u16 clsConst = b.cls("java/lang/Class");
        b.addMethod(method("main", "()V", buildCaller1(b, cat({0x13}, u16b(clsConst)))));
        addClassResource(container, "Caller.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(Ljava/lang/Class;)V");
    const std::vector<jbc::core::Op> cops = methodOps(classResource(container, "Caller.class")->get(), "main");
    ASSERT_EQ(cops.size(), 3u);
    EXPECT_EQ(cops[0], jbc::core::Op::LDC_W);
}

// ── 切片 2 拒绝：Class 常量与 Ti 失配 ──
TEST(ArgumentArrayScalarizerTest, classConstMismatchRejected)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("java/lang/String"));
    {
        FixtureBuilder b("Caller");
        const ca::u16 clsConst = b.cls("java/lang/Object");
        b.addMethod(method("main", "()V", buildCaller1(b, cat({0x13}, u16b(clsConst)))));
        addClassResource(container, "Caller.class", b.assemble());
    }
    EXPECT_FALSE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "([Ljava/lang/Object;)V");
}

// ── 切片 2 正面：调用方入参槽类型提升（static 入参直传）──
TEST(ArgumentArrayScalarizerTest, callerParamPassthroughScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("java/lang/String"));
    {
        FixtureBuilder b("Caller");
        // static main(Ljava/lang/String;)V：aload_0 是自身入参（槽 0），
        // 声明型对齐 Ti。
        b.addMethod(method("main", "(Ljava/lang/String;)V",
                           buildCaller1(b, cat({0x2a}, {}))));
        addClassResource(container, "Caller.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(Ljava/lang/String;)V");
    const std::vector<jbc::core::Op> cops = methodOps(classResource(container, "Caller.class")->get(), "main");
    ASSERT_EQ(cops.size(), 3u);
    EXPECT_EQ(cops[0], jbc::core::Op::ALOAD_0);
}

// ── 切片 2 拒绝：槽被重存储，入参定值证据失效 ──
TEST(ArgumentArrayScalarizerTest, callerParamRestoredRejected)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("java/lang/String"));
    {
        FixtureBuilder b("Caller");
        // 前置 aconst_null; astore_0 覆写入参槽 → aload_0 到达定值非入参。
        Bytes code = cat({0x01, 0x4b}, buildCaller1(b, cat({0x2a}, {})));
        b.addMethod(method("main", "(Ljava/lang/String;)V", std::move(code)));
        addClassResource(container, "Caller.class", b.assemble());
    }
    EXPECT_FALSE(runPass(container).changed());
}

// ── 切片 2 正面：实例调用方 this 直传（槽 0 = 调用方类）──
TEST(ArgumentArrayScalarizerTest, thisAliasPassthroughScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("Caller"));
    {
        FixtureBuilder b("Caller");
        FixtureMethod im = method("run", "()V", buildCaller1(b, cat({0x2a}, {})));
        im.access = 0x0001;   // 去 static：槽 0 = this，声明型 = 调用方类
        b.addMethod(std::move(im));
        addClassResource(container, "Caller.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(LCaller;)V");
}

// ── 正面：全链还原（被调方取参窗 + 描述符 + 调用方装箱窗）──
TEST(ArgumentArrayScalarizerTest, fullRoundTripScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCallee());
    {
        FixtureBuilder b("Caller");
        b.addMethod(method("main", "()V", buildCallerBoxed(b)));
        addClassResource(container, "Caller.class", b.assemble());
    }

    EXPECT_TRUE(runPass(container).changed());

    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(Ljava/lang/String;Ljava/lang/String;)V");
    // 取参窗四条 → 一条 ALOAD：aload 0; astore 3; aload 1; astore 4; return
    // （原 astore_1/astore_2 平移 +1）。
    const std::vector<jbc::core::Op> ops = methodOps(callee->get(), "go");
    ASSERT_EQ(ops.size(), 5u);
    EXPECT_EQ(ops[0], jbc::core::Op::ALOAD);
    EXPECT_EQ(ops[1], jbc::core::Op::ASTORE);
    EXPECT_EQ(ops[2], jbc::core::Op::ALOAD);
    EXPECT_EQ(ops[3], jbc::core::Op::ASTORE);
    EXPECT_EQ(ops[4], jbc::core::Op::RETURN);
    EXPECT_EQ(aloadSlots(callee->get(), "go"), (std::vector<ca::i32>{0, 1}));

    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    const std::vector<jbc::core::Op> cops = methodOps(caller->get(), "main");
    ASSERT_EQ(cops.size(), 4u);
    EXPECT_EQ(cops[0], jbc::core::Op::LDC_W);
    EXPECT_EQ(cops[1], jbc::core::Op::LDC_W);
    EXPECT_EQ(cops[2], jbc::core::Op::INVOKESTATIC);
    EXPECT_EQ(cops[3], jbc::core::Op::RETURN);
}

// ── null 实参（任意引用 Ti 匹配）──
TEST(ArgumentArrayScalarizerTest, nullArgScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCallee());
    {
        FixtureBuilder b("Caller");
        const ca::u16 objCls = b.cls("java/lang/Object");
        const ca::u16 mref   = b.methodRef("Svc", "go", "([Ljava/lang/Object;)V");
        const ca::u16 yStr   = b.strConst("y");
        // 元素 0 实参 aconst_null、元素 1 实参 ldc "y"。
        Bytes code = cat({0x05, 0xbd}, u16b(objCls));
        code = cat(code, cat({0x59, 0x01, 0x03, 0x53}, {0x59, 0x13}));
        code = cat(code, cat(u16b(yStr), cat({0x04, 0x53}, cat({0xb8}, cat(u16b(mref), {0xb1})))));
        b.addMethod(method("main", "()V", std::move(code)));
        addClassResource(container, "Caller.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    const std::vector<jbc::core::Op> cops = methodOps(caller->get(), "main");
    ASSERT_EQ(cops.size(), 4u);
    EXPECT_EQ(cops[0], jbc::core::Op::ACONST_NULL);
    EXPECT_EQ(cops[1], jbc::core::Op::LDC_W);
}

// ── 实参类型失配（ALOAD 实参 vs Ti=String）→ 整族不动 ──
TEST(ArgumentArrayScalarizerTest, argTypeMismatchAbandoned)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCallee());
    {
        FixtureBuilder b("Caller");
        const ca::u16 objCls = b.cls("java/lang/Object");
        const ca::u16 mref   = b.methodRef("Svc", "go", "([Ljava/lang/Object;)V");
        const ca::u16 yStr   = b.strConst("y");
        // 元素 0 实参 aload_1（槽类型不可见，Ti=String 失配）。
        Bytes code = cat({0x05, 0xbd}, u16b(objCls));
        code = cat(code, cat({0x59, 0x1b, 0x03, 0x53}, {0x59, 0x13}));
        code = cat(code, cat(u16b(yStr), cat({0x04, 0x53}, cat({0xb8}, cat(u16b(mref), {0xb1})))));
        b.addMethod(method("main", "()V", std::move(code)));
        addClassResource(container, "Caller.class", b.assemble());
    }
    runPass(container);
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "([Ljava/lang/Object;)V");
    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    EXPECT_GT(methodOps(caller->get(), "main").size(), 4u);
}

// ── 数组逃逸（arraylength）→ 被调方整族不动 ──
TEST(ArgumentArrayScalarizerTest, escapeAbandoned)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("Svc");
        const ca::u16 strCls = b.cls("java/lang/String");
        Bytes w0 = cat({0x2a, 0x03, 0x32, 0xc0}, cat(u16b(strCls), {0x4c}));
        // arraylength（0xbe）吃掉数组 → 逃逸。
        b.addMethod(method("go", "([Ljava/lang/Object;)V",
                           cat(cat({0x2a, 0xbe, 0x57}, w0), {0xb1})));
        addClassResource(container, "Svc.class", b.assemble());
    }
    {
        FixtureBuilder b("Caller");
        b.addMethod(method("main", "()V", buildCallerBoxed(b)));
        addClassResource(container, "Caller.class", b.assemble());
    }
    runPass(container);
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "([Ljava/lang/Object;)V");
}

// ── 下标缺口（只取元素 1）→ 被调方整族不动 ──
TEST(ArgumentArrayScalarizerTest, gapIndexAbandoned)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("Svc");
        const ca::u16 strCls = b.cls("java/lang/String");
        Bytes w1 = cat({0x2a, 0x04, 0x32, 0xc0}, cat(u16b(strCls), {0x4c}));
        b.addMethod(method("go", "([Ljava/lang/Object;)V", cat(w1, {0xb1})));
        addClassResource(container, "Svc.class", b.assemble());
    }
    {
        FixtureBuilder b("Caller");
        b.addMethod(method("main", "()V", buildCallerBoxed(b)));
        addClassResource(container, "Caller.class", b.assemble());
    }
    runPass(container);
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "([Ljava/lang/Object;)V");
}

// ── 非窗调用位点（数组先存局部再传参）→ 整族不动 ──
TEST(ArgumentArrayScalarizerTest, nonWindowSiteAbandoned)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCallee());
    {
        FixtureBuilder b("Caller");
        b.addMethod(method("main", "()V", buildCallerBoxed(b)));
        // 第二位点：数组经 astore_1 转手再 aload_1 传参（非规范窗）。
        const ca::u16 objCls = b.cls("java/lang/Object");
        const ca::u16 mref   = b.methodRef("Svc", "go", "([Ljava/lang/Object;)V");
        Bytes code2 = cat({0x05, 0xbd}, cat(u16b(objCls), {0x4c, 0x1b, 0xb8}));
        code2 = cat(code2, cat(u16b(mref), {0xb1}));
        b.addMethod(method("other", "()V", std::move(code2)));
        addClassResource(container, "Caller.class", b.assemble());
    }
    runPass(container);
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "([Ljava/lang/Object;)V");
}

// ── 重复取参（同元素两窗）→ 单参数签名，两窗同槽 ──
TEST(ArgumentArrayScalarizerTest, repeatIndexScalarized)
{
    mjbase::model::ResourceContainer container;
    {
        FixtureBuilder b("Svc");
        const ca::u16 strCls = b.cls("java/lang/String");
        Bytes w0 = cat({0x2a, 0x03, 0x32, 0xc0}, cat(u16b(strCls), {0x4c}));
        b.addMethod(method("go", "([Ljava/lang/Object;)V", cat(cat(w0, w0), {0xb1})));
        addClassResource(container, "Svc.class", b.assemble());
    }
    {
        FixtureBuilder b("Caller");
        const ca::u16 objCls = b.cls("java/lang/Object");
        const ca::u16 mref   = b.methodRef("Svc", "go", "([Ljava/lang/Object;)V");
        const ca::u16 xStr   = b.strConst("x");
        Bytes code = cat({0x04, 0xbd}, u16b(objCls));                    // iconst_1; anewarray
        code = cat(code, cat({0x59, 0x13}, cat(u16b(xStr), {0x03, 0x53}))); // dup; ldc_w x; iconst_0; aastore
        code = cat(code, cat({0xb8}, cat(u16b(mref), {0xb1})));
        b.addMethod(method("main", "()V", std::move(code)));
        addClassResource(container, "Caller.class", b.assemble());
    }
    EXPECT_TRUE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(Ljava/lang/String;)V");
    // 两窗同改 ALOAD 0；astore_1 平移 +0（shift = n-1 = 0，原样）。
    EXPECT_EQ(aloadSlots(callee->get(), "go"), (std::vector<ca::i32>{0, 0}));
}

// ── 注册面 ──
TEST(ArgumentArrayScalarizerTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("ArgumentArrayScalarizer"));
    auto created =
        mjbase::spi::defaultPassRegistry().createGlobalPass("ArgumentArrayScalarizer");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
