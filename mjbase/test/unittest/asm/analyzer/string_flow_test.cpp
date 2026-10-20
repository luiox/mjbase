/// @file        string_flow_test.cpp
/// @brief       字符串流分析测试：字面量来源与逃逸、builder 闭环（含跨局部
///        变量）、Base64 解码管线、char[] 常量填充/全数组偏移循环、不可
///        求值输入与循环合并下的不动点终止。
///
/// 方法体用手写 ConstantPool + CodeDom 构造，不依赖 classfile 解析；断言
/// 看的是事实面（来源点、值、逃逸位、管线输入/输出），不是内部令牌。
#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "jbc/jbc.hpp"

#include "mjbase/asm/analyzer/string_flow.hpp"

using jbc::core::ConstantPool;
using jbc::core::CpTag;
using jbc::core::Op;
using jbc::ir::CodeDom;
using jbc::ir::CodeUnit;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using mjbase::asm_analyzer::analyzeStringFlow;
using mjbase::asm_analyzer::StringFlowPipeline;
using mjbase::asm_analyzer::StringFlowResult;
using mjbase::asm_analyzer::StringFlowSource;
using mjbase::asm_analyzer::StringOrigin;

namespace {

InsnId appendOp(CodeDom& d, Op o)
{
    jbc::ir::Insn in;
    in.op = o;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

InsnId appendImm(CodeDom& d, Op o, ca::i32 a)
{
    jbc::ir::Insn in;
    in.op = o;
    in.a = a;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

InsnId appendCp(CodeDom& d, Op o, ca::u32 cpIdx)
{
    jbc::ir::Insn in;
    in.op = o;
    in.cp = cpIdx;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

InsnId appendBr(CodeDom& d, Op o, LabelId t)
{
    jbc::ir::Insn in;
    in.op = o;
    in.target = t;
    const auto r = d.append(in);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

LabelId newLbl(CodeDom& d, InsnId anchor)
{
    const auto r = d.createLabel(anchor);
    EXPECT_TRUE(r.is_ok());
    return r.unwrap();
}

void bindLbl(CodeDom& d, LabelId l, InsnId at)
{
    EXPECT_TRUE(d.bindLabel(l, at).is_ok());
}

CodeUnit unitOf(CodeDom&& d)
{
    CodeUnit u;
    u.dom = std::move(d);
    return u;
}

ca::u32 classRef(ConstantPool& cp, std::string_view name)
{
    return cp.addClass(cp.addUtf8(name));
}

ca::u32 methodRef(ConstantPool& cp, std::string_view owner, std::string_view name,
                  std::string_view desc)
{
    return cp.addRef(CpTag::Methodref, classRef(cp, owner),
                     cp.addNameAndType(cp.addUtf8(name), cp.addUtf8(desc)));
}

ca::u32 fieldRef(ConstantPool& cp, std::string_view owner, std::string_view name,
                 std::string_view desc)
{
    return cp.addRef(CpTag::Fieldref, classRef(cp, owner),
                     cp.addNameAndType(cp.addUtf8(name), cp.addUtf8(desc)));
}

ca::u32 strRef(ConstantPool& cp, std::string_view value)
{
    return cp.addString(cp.addUtf8(value));
}

}   // namespace

TEST(MjAsmAnalyzerTest, StringFlowTracksLiteralSourcesAndEscape)
{
    ConstantPool cp;
    const ca::u32 hello = strRef(cp, "hello");

    {
        // ldc; pop; return —— 值只进不逃逸。
        CodeDom dom;
        const InsnId ldc = appendCp(dom, Op::LDC, hello);
        appendOp(dom, Op::POP);
        appendOp(dom, Op::RETURN);

        StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
        ASSERT_TRUE(r.ok());
        ASSERT_EQ(r.sources().size(), 1u);
        const StringFlowSource* s = r.sourceAt(ldc);
        ASSERT_NE(s, nullptr);
        EXPECT_TRUE(s->complete);
        EXPECT_EQ(s->origin, StringOrigin::Literal);
        EXPECT_EQ(s->value, "hello");
        EXPECT_FALSE(s->escaped);
        EXPECT_TRUE(r.pipelines().empty());
        EXPECT_EQ(r.pipelineAt(ldc), nullptr);
    }

    {
        // ldc; areturn —— 返回值即逃逸。
        CodeDom dom;
        const InsnId ldc = appendCp(dom, Op::LDC, hello);
        appendOp(dom, Op::ARETURN);

        StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
        ASSERT_TRUE(r.ok());
        const StringFlowSource* s = r.sourceAt(ldc);
        ASSERT_NE(s, nullptr);
        EXPECT_TRUE(s->complete);
        EXPECT_TRUE(s->escaped);
    }
}

TEST(MjAsmAnalyzerTest, StringFlowFoldsBuilderChain)
{
    ConstantPool cp;
    const ca::u32 sbClass = classRef(cp, "java/lang/StringBuilder");
    const ca::u32 ctor = methodRef(cp, "java/lang/StringBuilder", "<init>", "()V");
    const ca::u32 appendStr =
        methodRef(cp, "java/lang/StringBuilder", "append",
                  "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
    const ca::u32 appendChar =
        methodRef(cp, "java/lang/StringBuilder", "append",
                  "(C)Ljava/lang/StringBuilder;");
    const ca::u32 toString =
        methodRef(cp, "java/lang/StringBuilder", "toString", "()Ljava/lang/String;");
    const ca::u32 ab = strRef(cp, "ab");

    // new; dup; <init>; ldc "ab"; append(String); bipush '!'; append(C);
    // toString; pop; return
    CodeDom dom;
    appendCp(dom, Op::NEW, sbClass);
    appendOp(dom, Op::DUP);
    appendCp(dom, Op::INVOKESPECIAL, ctor);
    appendCp(dom, Op::LDC, ab);
    appendCp(dom, Op::INVOKEVIRTUAL, appendStr);
    appendImm(dom, Op::BIPUSH, 33);
    appendCp(dom, Op::INVOKEVIRTUAL, appendChar);
    const InsnId sink = appendCp(dom, Op::INVOKEVIRTUAL, toString);
    appendOp(dom, Op::POP);
    appendOp(dom, Op::RETURN);

    StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const StringFlowSource* s = r.sourceAt(sink);
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->complete);
    EXPECT_EQ(s->origin, StringOrigin::Concat);
    EXPECT_EQ(s->value, "ab!");
    EXPECT_FALSE(s->escaped);
}

TEST(MjAsmAnalyzerTest, StringFlowFoldsBuilderAcrossLocals)
{
    ConstantPool cp;
    const ca::u32 sbClass = classRef(cp, "java/lang/StringBuilder");
    const ca::u32 ctor = methodRef(cp, "java/lang/StringBuilder", "<init>", "()V");
    const ca::u32 appendStr =
        methodRef(cp, "java/lang/StringBuilder", "append",
                  "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
    const ca::u32 appendChar =
        methodRef(cp, "java/lang/StringBuilder", "append",
                  "(C)Ljava/lang/StringBuilder;");
    const ca::u32 toString =
        methodRef(cp, "java/lang/StringBuilder", "toString", "()Ljava/lang/String;");
    const ca::u32 x = strRef(cp, "x");

    // builder 存 local 0，append 结果丢弃；内容仍按方法序聚合成闭环。
    CodeDom dom;
    appendCp(dom, Op::NEW, sbClass);
    appendOp(dom, Op::DUP);
    appendCp(dom, Op::INVOKESPECIAL, ctor);
    appendOp(dom, Op::ASTORE_0);
    appendOp(dom, Op::ALOAD_0);
    appendCp(dom, Op::LDC, x);
    appendCp(dom, Op::INVOKEVIRTUAL, appendStr);
    appendOp(dom, Op::POP);
    appendOp(dom, Op::ALOAD_0);
    appendImm(dom, Op::BIPUSH, 65);
    appendCp(dom, Op::INVOKEVIRTUAL, appendChar);
    appendOp(dom, Op::POP);
    appendOp(dom, Op::ALOAD_0);
    const InsnId sink = appendCp(dom, Op::INVOKEVIRTUAL, toString);
    appendOp(dom, Op::POP);
    appendOp(dom, Op::RETURN);

    StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const StringFlowSource* s = r.sourceAt(sink);
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->complete);
    EXPECT_EQ(s->origin, StringOrigin::Concat);
    EXPECT_EQ(s->value, "xA");
    EXPECT_FALSE(s->escaped);
}

TEST(MjAsmAnalyzerTest, StringFlowMarksConditionalBuilderMutationIncomplete)
{
    ConstantPool cp;
    const ca::u32 sbClass = classRef(cp, "java/lang/StringBuilder");
    const ca::u32 ctor = methodRef(cp, "java/lang/StringBuilder", "<init>", "()V");
    const ca::u32 appendStr =
        methodRef(cp, "java/lang/StringBuilder", "append",
                  "(Ljava/lang/String;)Ljava/lang/StringBuilder;");
    const ca::u32 toString =
        methodRef(cp, "java/lang/StringBuilder", "toString", "()Ljava/lang/String;");
    const ca::u32 x = strRef(cp, "x");

    // 分支只跳过 append：内容在两条路径上不同，闭环必须 fail-closed。
    CodeDom dom;
    const InsnId first = appendCp(dom, Op::NEW, sbClass);
    appendOp(dom, Op::DUP);
    appendCp(dom, Op::INVOKESPECIAL, ctor);
    appendOp(dom, Op::ASTORE_0);
    appendOp(dom, Op::ICONST_1);
    const LabelId skipLbl = newLbl(dom, first);
    appendBr(dom, Op::IFEQ, skipLbl);
    appendOp(dom, Op::ALOAD_0);
    appendCp(dom, Op::LDC, x);
    appendCp(dom, Op::INVOKEVIRTUAL, appendStr);
    appendOp(dom, Op::POP);
    const InsnId skipAnchor = appendOp(dom, Op::ALOAD_0);
    bindLbl(dom, skipLbl, skipAnchor);
    const InsnId sink = appendCp(dom, Op::INVOKEVIRTUAL, toString);
    appendOp(dom, Op::POP);
    appendOp(dom, Op::RETURN);

    StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const StringFlowSource* s = r.sourceAt(sink);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->complete);
    EXPECT_TRUE(r.pipelines().empty());
}

TEST(MjAsmAnalyzerTest, StringFlowDecodesBase64Pipeline)
{
    ConstantPool cp;
    const ca::u32 decoderClass = classRef(cp, "java/util/Base64$Decoder");
    (void)decoderClass;
    const ca::u32 getDecoder =
        methodRef(cp, "java/util/Base64", "getDecoder",
                  "()Ljava/util/Base64$Decoder;");
    const ca::u32 decode =
        methodRef(cp, "java/util/Base64$Decoder", "decode",
                  "(Ljava/lang/String;)[B");
    const ca::u32 stringClass = classRef(cp, "java/lang/String");
    const ca::u32 ctor =
        methodRef(cp, "java/lang/String", "<init>",
                  "([BLjava/lang/String;)V");
    const ca::u32 encoded = strRef(cp, "SGVsbG8=");
    const ca::u32 utf8 = strRef(cp, "UTF-8");

    // getDecoder; ldc "SGVsbG8="; decode(String); astore_0;
    // new String; dup; aload_0; ldc "UTF-8"; <init>([B,String); pop; return
    CodeDom dom;
    appendCp(dom, Op::INVOKESTATIC, getDecoder);
    const InsnId literalSite = appendCp(dom, Op::LDC, encoded);
    appendCp(dom, Op::INVOKEVIRTUAL, decode);
    appendOp(dom, Op::ASTORE_0);
    appendCp(dom, Op::NEW, stringClass);
    appendOp(dom, Op::DUP);
    appendOp(dom, Op::ALOAD_0);
    appendCp(dom, Op::LDC, utf8);
    const InsnId sink = appendCp(dom, Op::INVOKESPECIAL, ctor);
    appendOp(dom, Op::POP);
    appendOp(dom, Op::RETURN);

    StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());

    const StringFlowSource* in = r.sourceAt(literalSite);
    ASSERT_NE(in, nullptr);
    EXPECT_TRUE(in->complete);
    EXPECT_EQ(in->origin, StringOrigin::Literal);
    EXPECT_EQ(in->value, "SGVsbG8=");
    EXPECT_FALSE(in->escaped);

    const StringFlowSource* out = r.sourceAt(sink);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(out->complete);
    EXPECT_EQ(out->origin, StringOrigin::Base64Decode);
    EXPECT_EQ(out->value, "Hello");
    EXPECT_FALSE(out->escaped);

    const StringFlowPipeline* p = r.pipelineAt(sink);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->kind, StringOrigin::Base64Decode);
    EXPECT_EQ(p->input, "SGVsbG8=");
    EXPECT_EQ(p->output, "Hello");
}

TEST(MjAsmAnalyzerTest, StringFlowFoldsCharShiftLoop)
{
    ConstantPool cp;
    const ca::u32 toCharArray =
        methodRef(cp, "java/lang/String", "toCharArray", "()[C");
    const ca::u32 stringClass = classRef(cp, "java/lang/String");
    const ca::u32 ctor = methodRef(cp, "java/lang/String", "<init>", "([C)V");
    const ca::u32 text = strRef(cp, "Ifmmp");

    // ldc "Ifmmp"; toCharArray; astore_0; iconst_0; istore_1; goto check;
    // body: aload_0; iload_1; aload_0; iload_1; caload; iconst_1; isub; i2c;
    //       castore; iinc 1,1;
    // check: iload_1; aload_0; arraylength; if_icmplt body;
    // new String(char[]); pop; return
    CodeDom dom;
    const InsnId first = appendCp(dom, Op::LDC, text);
    appendCp(dom, Op::INVOKEVIRTUAL, toCharArray);
    appendOp(dom, Op::ASTORE_0);
    appendOp(dom, Op::ICONST_0);
    appendOp(dom, Op::ISTORE_1);
    const LabelId checkLbl = newLbl(dom, first);
    appendBr(dom, Op::GOTO, checkLbl);

    const InsnId bodyAnchor = appendOp(dom, Op::ALOAD_0);
    const LabelId bodyLbl = newLbl(dom, bodyAnchor);
    bindLbl(dom, bodyLbl, bodyAnchor);
    appendOp(dom, Op::ILOAD_1);
    appendOp(dom, Op::ALOAD_0);
    appendOp(dom, Op::ILOAD_1);
    appendOp(dom, Op::CALOAD);
    appendOp(dom, Op::ICONST_1);
    appendOp(dom, Op::ISUB);
    appendOp(dom, Op::I2C);
    appendOp(dom, Op::CASTORE);
    appendImm(dom, Op::IINC, 1);

    const InsnId checkAnchor = appendOp(dom, Op::ILOAD_1);
    bindLbl(dom, checkLbl, checkAnchor);
    appendOp(dom, Op::ALOAD_0);
    appendOp(dom, Op::ARRAYLENGTH);
    appendBr(dom, Op::IF_ICMPLT, bodyLbl);

    appendCp(dom, Op::NEW, stringClass);
    appendOp(dom, Op::DUP);
    appendOp(dom, Op::ALOAD_0);
    const InsnId sink = appendCp(dom, Op::INVOKESPECIAL, ctor);
    appendOp(dom, Op::POP);
    appendOp(dom, Op::RETURN);

    StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());

    const StringFlowSource* decoded = r.sourceAt(sink);
    ASSERT_NE(decoded, nullptr);
    EXPECT_TRUE(decoded->complete);
    EXPECT_EQ(decoded->origin, StringOrigin::CharShift);
    EXPECT_EQ(decoded->value, "Hello");
    EXPECT_FALSE(decoded->escaped);

    const StringFlowPipeline* p = r.pipelineAt(sink);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->kind, StringOrigin::CharShift);
    EXPECT_EQ(p->input, "Ifmmp");
    EXPECT_EQ(p->output, "Hello");
}

TEST(MjAsmAnalyzerTest, StringFlowFoldsConstantCharArrayFill)
{
    ConstantPool cp;
    const ca::u32 stringClass = classRef(cp, "java/lang/String");
    const ca::u32 ctor = methodRef(cp, "java/lang/String", "<init>", "([C)V");

    // bipush 3; newarray char; 常量三元填充; new String(char[]); RETURN
    CodeDom dom;
    appendImm(dom, Op::BIPUSH, 3);
    appendImm(dom, Op::NEWARRAY, 5);
    appendOp(dom, Op::DUP);
    appendOp(dom, Op::ICONST_0);
    appendImm(dom, Op::BIPUSH, 'H');
    appendOp(dom, Op::CASTORE);
    appendOp(dom, Op::DUP);
    appendOp(dom, Op::ICONST_1);
    appendImm(dom, Op::BIPUSH, 'i');
    appendOp(dom, Op::CASTORE);
    appendOp(dom, Op::DUP);
    appendOp(dom, Op::ICONST_2);
    appendImm(dom, Op::BIPUSH, '!');
    appendOp(dom, Op::CASTORE);
    appendOp(dom, Op::ASTORE_0);
    appendCp(dom, Op::NEW, stringClass);
    appendOp(dom, Op::DUP);
    appendOp(dom, Op::ALOAD_0);
    const InsnId sink = appendCp(dom, Op::INVOKESPECIAL, ctor);
    appendOp(dom, Op::ARETURN);

    StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const StringFlowSource* s = r.sourceAt(sink);
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->complete);
    EXPECT_EQ(s->origin, StringOrigin::CharArray);
    EXPECT_EQ(s->value, "Hi!");
    EXPECT_TRUE(s->escaped);   // 返回值逃逸
    EXPECT_EQ(r.pipelineAt(sink), nullptr);
}

TEST(MjAsmAnalyzerTest, StringFlowMarksUnknownInputsIncomplete)
{
    ConstantPool cp;
    const ca::u32 stringClass = classRef(cp, "java/lang/String");
    const ca::u32 ctor = methodRef(cp, "java/lang/String", "<init>", "([B)V");

    // 平台默认 charset 的 new String(byte[]) 静态不可知；入参也未知。
    CodeDom dom;
    appendCp(dom, Op::NEW, stringClass);
    appendOp(dom, Op::DUP);
    appendOp(dom, Op::ALOAD_0);
    const InsnId sink = appendCp(dom, Op::INVOKESPECIAL, ctor);
    appendOp(dom, Op::ARETURN);

    StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    const StringFlowSource* s = r.sourceAt(sink);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->complete);
    EXPECT_EQ(s->origin, StringOrigin::Unknown);
    EXPECT_TRUE(s->value.empty());
    EXPECT_TRUE(r.pipelines().empty());
}

TEST(MjAsmAnalyzerTest, StringFlowTerminatesOnSelfReferentialLoop)
{
    ConstantPool cp;
    const ca::u32 concat =
        methodRef(cp, "java/lang/String", "concat",
                  "(Ljava/lang/String;)Ljava/lang/String;");
    const ca::u32 a = strRef(cp, "a");
    const ca::u32 b = strRef(cp, "b");

    // ldc "a"; astore_0; L: aload_0; ldc "b"; concat; astore_0; goto L
    // 循环头 local 0 每轮变成新 Call，合并退化为 Top；分析必须收敛。
    CodeDom dom;
    const InsnId first = appendCp(dom, Op::LDC, a);
    appendOp(dom, Op::ASTORE_0);
    const LabelId headLbl = newLbl(dom, first);
    const InsnId head = appendOp(dom, Op::ALOAD_0);
    bindLbl(dom, headLbl, head);
    appendCp(dom, Op::LDC, b);
    appendCp(dom, Op::INVOKEVIRTUAL, concat);
    appendOp(dom, Op::ASTORE_0);
    appendBr(dom, Op::GOTO, headLbl);

    StringFlowResult r = analyzeStringFlow(cp, unitOf(std::move(dom)));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.sources().size(), 2u);   // 两条 ldc 字面量
}
