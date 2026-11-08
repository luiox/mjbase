// ArgumentArrayScalarizer（#37 切片 1）：装箱签名 [Ljava/lang/Object; 参数
// 标量化——被调方取参窗改直接参数槽 + 局部平移、调用方装箱窗拆直接传参、
// 描述符全局同步；守卫面（实参类型失配/数组逃逸/下标缺口/非窗调用位点/
// null 实参/重复取参）+ 注册面。断言经会话 promote 后的指令序与符号池
// 描述符完成。
//
// 夹具纪律：类与方法一律 jasm 文本装配（JvmJasmService），原
// structural_pass_util 手拼载体退役。调用方实参生产者指令在改写中保留
// 原样——原 ldc_w 形装配器发 1 字节 LDC（小池无 2 字节形态），期望值改记
// Op::LDC；「实参槽类型不可见」拒绝面以 int 实参表达（原字节形 iload_1
// 挂 ()V 无定义槽，帧不可装配——caller 带 (I)V 形参使 1 槽有定义）。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/tag_cast.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/pass/argument_array_scalarizer.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;

namespace {

/// 类便捷装配：cls 类挂 methodsText（若干 `.method` 声明文本）。
Bytes asmClass(const std::string& cls, std::string_view methodsText)
{
    const std::string src = R"JASM(.super java/lang/Object
.class public super )JASM" + cls + R"JASM( {
)JASM" + std::string(methodsText) + R"JASM(}
)JASM";
    return assembleJasm(src);
}

/// 被调方取参窗文本：receiver（"aload 0"/"aload 1"）; iconst_I; aaload;
/// checkcast Ti; astore slot。
std::string windowText(std::string_view receiver, int idx, const std::string& ti,
                       int slot)
{
    return "      " + std::string(receiver) + "\n      iconst_" + std::to_string(idx)
         + "\n      aaload\n      checkcast " + ti + "\n      astore "
         + std::to_string(slot) + "\n";
}

/// 被调方：go([Ljava/lang/Object;)V，取参窗两组（元素 0/1 均 checkcast
/// String）后 astore 到局部 1/2，收尾 RETURN。
Bytes buildCallee()
{
    return asmClass("Svc",
                    "  .method public static go ([Ljava/lang/Object;)V {\n"
                    "    code: {\n"
                        + windowText("aload 0", 0, "java/lang/String", 1)
                        + windowText("aload 0", 1, "java/lang/String", 2)
                        + "      return\n    }\n  }\n");
}

// ── 切片 2 件：被调方单取参窗（元素 0 checkcast Ti）──
Bytes buildCalleeTi(const std::string& tiInternal)
{
    return asmClass("Svc",
                    "  .method public static go ([Ljava/lang/Object;)V {\n"
                    "    code: {\n"
                        + windowText("aload 0", 0, tiInternal, 1)
                        + "      return\n    }\n  }\n");
}

/// 单元素装箱窗 caller 方法文本（mod 形如 "public static"；headText 为
/// receiver 铺垫指令文本，可空；argText 为第 0 元素实参指令文本）。
std::string caller1Method(std::string_view mod, std::string_view name,
                          std::string_view desc, std::string_view headText,
                          std::string_view argText, std::string_view invokeText)
{
    std::string s = "  .method " + std::string(mod) + " " + std::string(name) + " "
                  + std::string(desc) + " {\n    code: {\n";
    s += std::string(headText);
    s += "      iconst_1\n      anewarray java/lang/Object\n      dup\n";
    s += "      " + std::string(argText) + "\n";
    s += "      iconst_0\n      aastore\n";
    s += "      " + std::string(invokeText) + "\n      return\n    }\n  }\n";
    return s;
}

/// 双元素装箱窗 caller 方法文本（元素 0 = arg0Text，元素 1 = arg1Text）。
std::string caller2Method(std::string_view mod, std::string_view name,
                          std::string_view desc, std::string_view arg0Text,
                          std::string_view arg1Text, std::string_view invokeText)
{
    std::string s = "  .method " + std::string(mod) + " " + std::string(name) + " "
                  + std::string(desc) + " {\n    code: {\n";
    s += "      iconst_2\n      anewarray java/lang/Object\n      dup\n";
    s += "      " + std::string(arg0Text) + "\n      iconst_0\n      aastore\n";
    s += "      dup\n      " + std::string(arg1Text)
       + "\n      iconst_1\n      aastore\n";
    s += "      " + std::string(invokeText) + "\n      return\n    }\n  }\n";
    return s;
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
        auto                 cuRef = std::move(promoted).unwrap();
        std::vector<ca::i32> out;
        for (const jbc::ir::InsnId id : cuRef->dom.order()) {
            const jbc::ir::Insn* in = cuRef->dom.peek(id);
            if (in == nullptr) continue;
            if (in->op == jbc::core::Op::ALOAD) out.push_back(in->a);
            else if (in->op >= jbc::core::Op::ALOAD_0 && in->op <= jbc::core::Op::ALOAD_3) {
                out.push_back(static_cast<ca::i32>(in->op)
                              - static_cast<ca::i32>(jbc::core::Op::ALOAD_0));
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
    ca::str::Utf8StringArena               arena;
    mjbase::spi::GlobalAnalysisManager     gam(arena);
    mjbase::pass::ArgumentArrayScalarizer pass;
    return pass.run(container, gam);
}

}   // namespace

// ── 切片 2 正面：ldc Class 常量按常量类对齐 ──
TEST(ArgumentArrayScalarizerTest, classConstArgScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("java/lang/Class"));
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller1Method("public static", "main", "()V", "",
                                            "ldc Ljava/lang/Class;",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
    EXPECT_TRUE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(Ljava/lang/Class;)V");
    const std::vector<jbc::core::Op> cops =
        methodOps(classResource(container, "Caller.class")->get(), "main");
    ASSERT_EQ(cops.size(), 3u);
    EXPECT_EQ(cops[0], jbc::core::Op::LDC);   // 小池发 1 字节 LDC，生产者保留
}

// ── 切片 2 拒绝：Class 常量与 Ti 失配 ──
TEST(ArgumentArrayScalarizerTest, classConstMismatchRejected)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("java/lang/String"));
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller1Method("public static", "main", "()V", "",
                                            "ldc Ljava/lang/Object;",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
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
    // static main(Ljava/lang/String;)V：aload_0 是自身入参（槽 0），
    // 声明型对齐 Ti。
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller1Method("public static", "main",
                                            "(Ljava/lang/String;)V", "", "aload 0",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
    EXPECT_TRUE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(Ljava/lang/String;)V");
    const std::vector<jbc::core::Op> cops =
        methodOps(classResource(container, "Caller.class")->get(), "main");
    ASSERT_EQ(cops.size(), 3u);
    EXPECT_EQ(cops[0], jbc::core::Op::ALOAD_0);
}

// ── 切片 2 拒绝：槽被重存储，入参定值证据失效 ──
TEST(ArgumentArrayScalarizerTest, callerParamRestoredRejected)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("java/lang/String"));
    // 前置 aconst_null; astore_0 覆写入参槽 → aload_0 到达定值非入参。
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller1Method("public static", "main",
                                            "(Ljava/lang/String;)V",
                                            "      aconst_null\n      astore 0\n",
                                            "aload 0",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
    EXPECT_FALSE(runPass(container).changed());
}

// ── 切片 2 正面：实例调用方 this 直传（槽 0 = 调用方类）──
TEST(ArgumentArrayScalarizerTest, thisAliasPassthroughScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("Caller"));
    // 实例 run()V：槽 0 = this，声明型 = 调用方类。
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller1Method("public", "run", "()V", "", "aload 0",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
    EXPECT_TRUE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(LCaller;)V");
}

// ── 切片 3 正面：双指令装箱链实参（iconst_5; Integer.valueOf）──
TEST(ArgumentArrayScalarizerTest, boxValueOfArgScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("java/lang/Integer"));
    addClassResource(
        container, "Caller.class",
        asmClass("Caller",
                 caller1Method("public static", "main", "()V", "",
                               "iconst_5\n      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;",
                               "invokestatic Svc.go ([Ljava/lang/Object;)V")));
    EXPECT_TRUE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(Ljava/lang/Integer;)V");
    const std::vector<jbc::core::Op> cops =
        methodOps(classResource(container, "Caller.class")->get(), "main");
    ASSERT_EQ(cops.size(), 4u);
    EXPECT_EQ(cops[0], jbc::core::Op::ICONST_5);
    EXPECT_EQ(cops[1], jbc::core::Op::INVOKESTATIC);
}

// ── 切片 3 拒绝：装箱链产出型与 Ti 失配 ──
TEST(ArgumentArrayScalarizerTest, boxValueOfMismatchRejected)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCalleeTi("java/lang/String"));
    addClassResource(
        container, "Caller.class",
        asmClass("Caller",
                 caller1Method("public static", "main", "()V", "",
                               "iconst_5\n      invokestatic java/lang/Integer.valueOf (I)Ljava/lang/Integer;",
                               "invokestatic Svc.go ([Ljava/lang/Object;)V")));
    EXPECT_FALSE(runPass(container).changed());
}

// ── 切片 3 正面：实例形态（槽 1 取参 + invokevirtual 位点）──
TEST(ArgumentArrayScalarizerTest, instanceCalleeScalarized)
{
    mjbase::model::ResourceContainer container;
    // 实例取参窗：aload_1（槽 1 = 首参）; iconst_0; aaload; checkcast; astore_2。
    addClassResource(container, "Svc.class",
                     asmClass("Svc",
                              "  .method public go ([Ljava/lang/Object;)V {\n"
                              "    code: {\n"
                                  + windowText("aload 1", 0, "java/lang/String", 2)
                                  + "      return\n    }\n  }\n"));
    // aconst_null 作 receiver；装箱窗；invokevirtual。
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller1Method("public static", "main", "()V",
                                            "      aconst_null\n", "ldc \"z\"",
                                            "invokevirtual Svc.go ([Ljava/lang/Object;)V")));
    EXPECT_TRUE(runPass(container).changed());
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "(Ljava/lang/String;)V");
    const std::vector<jbc::core::Op> cops =
        methodOps(classResource(container, "Caller.class")->get(), "main");
    ASSERT_EQ(cops.size(), 4u);
    EXPECT_EQ(cops[0], jbc::core::Op::ACONST_NULL);
    EXPECT_EQ(cops[1], jbc::core::Op::LDC);
    EXPECT_EQ(cops[2], jbc::core::Op::INVOKEVIRTUAL);
}

// ── 正面：全链还原（被调方取参窗 + 描述符 + 调用方装箱窗）──
TEST(ArgumentArrayScalarizerTest, fullRoundTripScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCallee());
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller2Method("public static", "main", "()V", "ldc \"x\"",
                                            "ldc \"y\"",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));

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
    EXPECT_EQ(cops[0], jbc::core::Op::LDC);   // ldc_w 原形经装配器发 1 字节 LDC
    EXPECT_EQ(cops[1], jbc::core::Op::LDC);
    EXPECT_EQ(cops[2], jbc::core::Op::INVOKESTATIC);
    EXPECT_EQ(cops[3], jbc::core::Op::RETURN);
}

// ── null 实参（任意引用 Ti 匹配）──
TEST(ArgumentArrayScalarizerTest, nullArgScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCallee());
    // 元素 0 实参 aconst_null、元素 1 实参 ldc "y"。
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller2Method("public static", "main", "()V", "aconst_null",
                                            "ldc \"y\"",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
    EXPECT_TRUE(runPass(container).changed());
    const auto* caller = classResource(container, "Caller.class");
    ASSERT_NE(caller, nullptr);
    const std::vector<jbc::core::Op> cops = methodOps(caller->get(), "main");
    ASSERT_EQ(cops.size(), 4u);
    EXPECT_EQ(cops[0], jbc::core::Op::ACONST_NULL);
    EXPECT_EQ(cops[1], jbc::core::Op::LDC);
}

// ── 实参类型失配（int 实参 vs Ti=String）→ 整族不动 ──
TEST(ArgumentArrayScalarizerTest, argTypeMismatchAbandoned)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class", buildCallee());
    // 元素 0 实参 iload 1（int 槽类型，Ti=String 失配；caller 带 (I)V 形参
    // 使 1 槽有定义，帧自洽）。
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller2Method("public static", "main", "(I)V", "iload 1",
                                            "ldc \"y\"",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
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
    // arraylength 吃掉数组 → 逃逸。
    addClassResource(
        container, "Svc.class",
        asmClass("Svc",
                 "  .method public static go ([Ljava/lang/Object;)V {\n"
                 "    code: {\n      aload 0\n      arraylength\n      pop\n"
                     + windowText("aload 0", 0, "java/lang/String", 1)
                     + "      return\n    }\n  }\n"));
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller2Method("public static", "main", "()V", "ldc \"x\"",
                                            "ldc \"y\"",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
    runPass(container);
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "([Ljava/lang/Object;)V");
}

// ── 下标缺口（只取元素 1）→ 被调方整族不动 ──
TEST(ArgumentArrayScalarizerTest, gapIndexAbandoned)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class",
                     asmClass("Svc",
                              "  .method public static go ([Ljava/lang/Object;)V {\n"
                              "    code: {\n"
                                  + windowText("aload 0", 1, "java/lang/String", 1)
                                  + "      return\n    }\n  }\n"));
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller2Method("public static", "main", "()V", "ldc \"x\"",
                                            "ldc \"y\"",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
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
    addClassResource(
        container, "Caller.class",
        asmClass("Caller",
                 caller2Method("public static", "main", "()V", "ldc \"x\"", "ldc \"y\"",
                               "invokestatic Svc.go ([Ljava/lang/Object;)V")
                     // 第二位点：数组经 astore_1 转手再 aload_1 传参（非规范窗）。
                     + "  .method public static other ()V {\n    code: {\n"
                       "      iconst_2\n      anewarray java/lang/Object\n"
                       "      astore 1\n      aload 1\n"
                       "      invokestatic Svc.go ([Ljava/lang/Object;)V\n"
                       "      return\n    }\n  }\n"));
    runPass(container);
    const auto* callee = classResource(container, "Svc.class");
    ASSERT_NE(callee, nullptr);
    EXPECT_EQ(methodDesc(callee->get(), "go"), "([Ljava/lang/Object;)V");
}

// ── 重复取参（同元素两窗）→ 单参数签名，两窗同槽 ──
TEST(ArgumentArrayScalarizerTest, repeatIndexScalarized)
{
    mjbase::model::ResourceContainer container;
    addClassResource(container, "Svc.class",
                     asmClass("Svc",
                              "  .method public static go ([Ljava/lang/Object;)V {\n"
                              "    code: {\n"
                                  + windowText("aload 0", 0, "java/lang/String", 1)
                                  + windowText("aload 0", 0, "java/lang/String", 1)
                                  + "      return\n    }\n  }\n"));
    addClassResource(container, "Caller.class",
                     asmClass("Caller",
                              caller1Method("public static", "main", "()V", "",
                                            "ldc \"x\"",
                                            "invokestatic Svc.go ([Ljava/lang/Object;)V")));
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
