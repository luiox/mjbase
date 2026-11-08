// ConstCharArrayStringFolder：常量 char 数组构造的 `new String(char[])` 整窗
// 折叠为字符串 ldc（含 XOR/算术混淆、乱序下标与二次写、求值红线守卫、窗内
// 锚定标签的保守口径）。
//
// 夹具纪律：类骨架与方法体一律 jasm 文本装配（JvmJasmService）。窄常量
//（bipush/sipush）装配器按自身口径规范化为 LDC，窗口匹配面同收。两处
// 帧 面 调 整：非 常 量 元 素 拍 由 ACONST_NULL 改 getstatic int（castore
// 值槽须 int）、错挂 ctor 改 String.<init>()V（invokespecial <init> 的
// 接收者须未初始化 NEW 结果）——语义钉点（非常量生产者 / ctor 失配）不变。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/const_char_array_string_folder.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// 窗口夹具：m ()Ljava/lang/String; 的方法体 jasm 片段直拼
///（NEW String + DUP 头 / len / newarray char / arms / seal / areturn 由
/// 调用方在片段里按序给出）。
Bytes windowMethod(std::string_view body)
{
    return assembleMethodClass("m", "()Ljava/lang/String;", body);
}

}   // namespace


// ── 常量元素的 ASCII 形状：整窗折叠为单条 LDC <"abc"> + ARETURN ──
TEST(ConstCharArrayStringFolderTest, asciiConstructionFoldsToLdc)
{
    const MethodPassRun r = runMethodPass(
        windowMethod(R"JASM(
new java/lang/String
dup
iconst_3
newarray char
dup
iconst_0
bipush 97
castore
dup
iconst_1
bipush 98
castore
dup
iconst_2
bipush 99
castore
invokespecial java/lang/String.<init> ([C)V
areturn
)JASM"),
        mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 2u);
    const jbc::ir::Insn* ldc = ins.cu->dom.peek(ids[0]);
    EXPECT_EQ(ldc->op, Op::LDC);
    EXPECT_EQ(ldcStringOf(ins, ldc), "abc");
    EXPECT_EQ(ins.cu->dom.peek(ids[1])->op, Op::ARETURN);
}

// ── 混淆算术形状：len 走 ISUB；元素走 LDC+IXOR+I2C / IADD / IOR ──
TEST(ConstCharArrayStringFolderTest, obfuscatedConstantExpressionsFold)
{
    const MethodPassRun r = runMethodPass(
        windowMethod(R"JASM(
new java/lang/String
dup
iconst_2
iconst_m1
isub
newarray char
dup
iconst_0
ldc 19968
bipush 45
ixor
i2c
castore
dup
iconst_1
iconst_3
sipush 94
iadd
castore
dup
iconst_2
iconst_2
bipush 64
ior
castore
invokespecial java/lang/String.<init> ([C)V
areturn
)JASM"),
        mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 2u);
    // MUTF-8("中aB")（无 NUL、无增补面 → 与 UTF-8 同形）。
    const std::string_view expected = "\xE4\xB8\xAD" "aB";
    EXPECT_EQ(ldcStringOf(ins, ins.cu->dom.peek(ids[0])), expected);
}

// ── 同下标二次写：拍数==len 的形状约束下 pigeonhole 必致未覆盖下标 →
//    整组保留（「last-wins」分支出不可达，注释只作防御性语义）。
//    另查乱序下标不破坏匹配：同 len 下前两拍乱序 + 第三拍覆盖完成员。 ──
TEST(ConstCharArrayStringFolderTest, outOfOrderStoresKeptOrFolded)
{
    // 3 长度 4 拍（[1] 双写）→ 恰好覆盖不成立，整组保留。
    {
        const Bytes in = windowMethod(R"JASM(
new java/lang/String
dup
iconst_3
newarray char
dup
iconst_2
bipush 67
castore
dup
iconst_0
bipush 65
castore
dup
iconst_1
bipush 88
castore
dup
iconst_1
bipush 98
castore
invokespecial java/lang/String.<init> ([C)V
areturn
)JASM");
        const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
        EXPECT_TRUE(r.pa.areAllPreserved());
        EXPECT_EQ(r.out, in);
    }
    // 乱序但恰覆盖（[2],[0],[1]）→ 折叠；断言串按内容序 "AbC"。
    {
        const MethodPassRun r = runMethodPass(
            windowMethod(R"JASM(
new java/lang/String
dup
iconst_3
newarray char
dup
iconst_2
bipush 67
castore
dup
iconst_0
bipush 65
castore
dup
iconst_1
bipush 98
castore
invokespecial java/lang/String.<init> ([C)V
areturn
)JASM"),
            mjbase::pass::ConstCharArrayStringFolder{});
        EXPECT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        ASSERT_EQ(ins.cu->dom.order().size(), 2u);
        EXPECT_EQ(ldcStringOf(ins, ins.cu->dom.peek(ins.cu->dom.order()[0])), "AbC");
    }
}

// ── 求值红线：len 表达式含 IDIV → 整组保留、字节不动 ──
TEST(ConstCharArrayStringFolderTest, idivExpressionKept)
{
    const Bytes in = windowMethod(R"JASM(
new java/lang/String
dup
iconst_4
iconst_2
idiv
newarray char
dup
iconst_0
bipush 97
castore
dup
iconst_1
bipush 98
castore
invokespecial java/lang/String.<init> ([C)V
areturn
)JASM");
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 覆盖不完整：len 3 但只写 [0]/[2] → 整组保留 ──
TEST(ConstCharArrayStringFolderTest, uncoveredIndexKept)
{
    const Bytes in = windowMethod(R"JASM(
new java/lang/String
dup
iconst_3
newarray char
dup
iconst_0
bipush 97
castore
dup
iconst_2
bipush 99
castore
invokespecial java/lang/String.<init> ([C)V
areturn
)JASM");
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 非常量生产者：元素拍里 getstatic int → 求值失配，整组保留 ──
TEST(ConstCharArrayStringFolderTest, unknownConstantProducerKept)
{
    const Bytes in = windowMethod(R"JASM(
new java/lang/String
dup
iconst_1
newarray char
dup
iconst_0
getstatic t/X.f I
castore
invokespecial java/lang/String.<init> ([C)V
areturn
)JASM");
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 孤立代理：值 = SIPUSH 0xD800 → Utf16String 抛错整组保留 ──
TEST(ConstCharArrayStringFolderTest, loneSurrogateKept)
{
    const Bytes in = windowMethod(R"JASM(
new java/lang/String
dup
iconst_1
newarray char
dup
iconst_0
sipush 55296
castore
invokespecial java/lang/String.<init> ([C)V
areturn
)JASM");
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 非 char 数组：NEWARRAY T_INT + IASTORE 拍 → 形状失配整组保留 ──
TEST(ConstCharArrayStringFolderTest, nonCharArrayKept)
{
    const Bytes in = windowMethod(R"JASM(
new java/lang/String
dup
iconst_1
newarray int
dup
iconst_0
bipush 65
iastore
invokespecial java/lang/String.<init> ([C)V
areturn
)JASM");
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 错挂 ctor：seal 引用 desc 不是 String.<init>([C)V → 整组保留 ──
TEST(ConstCharArrayStringFolderTest, wrongConstructorKept)
{
    // 帧 面 合 法 化：数组段 pop 弃后对 NEW 接收者挂 String.<init>()V
    //（desc 失配钉点不变）。
    const Bytes in = windowMethod(R"JASM(
new java/lang/String
dup
iconst_1
newarray char
pop
invokespecial java/lang/String.<init> ()V
areturn
)JASM");
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 窗内锚定标签（异常边跳进窗内 DUP 头）：保守整组保留（手工形态偏离差集，
//    golden 字节对拍刻意避开，由本用例钉死口径）──
TEST(ConstCharArrayStringFolderTest, midWindowAnchorKept)
{
    // handler 目标 = NEW 后的 DUP（labelsAnchorAt 守卫位）。原口径的 GOTO
    // 中窗锚（跳过 NEW 后 dup 欠栈）装配器栈分析拒收，改异常边等价表达：
    // try [L0,La) catch-any，handler=La，锚定面同构且栈面合法（带 exceptions
    // 块，不走 windowMethod 的 code 包裹形）。
    const Bytes in = assembleJasm(R"JASM(
.super java/lang/Object
.class public super t/Fx {
  .method public static m ()Ljava/lang/String; {
    exceptions: {
      { L0, La, La, * }
    },
    code: {
      L0:
      new java/lang/String
      La:
      dup
      iconst_3
      newarray char
      dup
      iconst_0
      bipush 97
      castore
      dup
      iconst_1
      bipush 98
      castore
      dup
      iconst_2
      bipush 99
      castore
      invokespecial java/lang/String.<init> ([C)V
      areturn
    }
  }
}
)JASM");
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── registry 按名可解 ──
TEST(ConstCharArrayStringFolderTest, resolvableViaRegistry)
{
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("ConstCharArrayStringFolder");
    ASSERT_TRUE(created.is_ok());
    if (created.is_ok()) {
        std::unique_ptr<mjbase::spi::MethodPass> pass = std::move(created).unwrap();
        EXPECT_NE(pass, nullptr);
    }
}
