/// @file        semantic_fp_test.cpp
/// @brief       semantic_fp 方法语义指纹测试：期望表全矩阵 + 八项边界 +
///              确定性/唯一性（#132 规范 §验收，全部合成类夹具）。
///
/// 「该稳的稳、该翻的翻」双向钉死：期望表 10 行逐行、表外 3 条（方法自身
/// 改名/被引用字段改名/native-abstract 恒定）、八项边界按原型口径各一条、
/// 同方法双算一致 + CP 重索引零变化 + 合成语料去重统计。混淆形态样本用
/// 空格名（21845 字节）合成形态，不引用任何具体工具名。
///
/// 夹具纪律：方法体一律 jasm 文本装配（semantic_fp_fixture.hpp 版本口径
/// ——49 帧less 主体 / 61 indy·condy 面）；jasm 不能直达的形（LDC_W 强制
/// 形、WIDE 前缀窄槽位、invokeinterface count、原始属性载荷、空格名、
/// 异常表原始 pc 端点、lookupswitch 体、BSM 嵌套动态实参）走 DOM 单点
/// 偏差载面，逐用例申报。CP 重索引夹具以 spacer 方法前置真实平移语义槽位。
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/reader.hpp"

#include "semantic_fp_fixture.hpp"
#include "mjbase/analysis/semantic_fp.hpp"

// 本目录另有 analysis 夹具的同名头；指纹夹具的命名空间经别名区分。
namespace fp_fix = mjbase::analysis::fp_test_support;

using fp_fix::Bytes;
using jbc::core::CpTag;
using mjbase::analysis::SemanticFp;
using mjbase::analysis::SemanticFpError;

namespace {

/// 解析类字节（夹具字节受控，读失败即夹具 bug）。
std::unique_ptr<jbc::core::ClassFile> parseClass(const Bytes& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView{bytes});
    if (read.is_err()) {
        ADD_FAILURE() << "fixture class failed to parse";
        return nullptr;
    }
    return std::move(read).unwrap().file;
}

/// 按名找方法（断言存在）。
jbc::core::MemberInfo* methodOf(jbc::core::ClassFile& cf, std::string_view name)
{
    for (auto& m : cf.methods) {
        if (cf.nameOf(m) == name) {
            return &m;
        }
    }
    ADD_FAILURE() << "method not found in fixture: " << name;
    return nullptr;
}

/// 算指纹（错误即断言失败并返回默认值；仅用于「应当成功」的路径）。
SemanticFp fpOf(const Bytes& bytes, std::string_view name)
{
    SemanticFp out;
    auto cf = parseClass(bytes);
    if (cf == nullptr) {
        return out;
    }
    auto* m = methodOf(*cf, name);
    if (m == nullptr) {
        return out;
    }
    auto r = mjbase::analysis::semanticFp(*cf, *m);
    if (r.is_err()) {
        ADD_FAILURE() << "semanticFp failed: "
                      << mjbase::analysis::to_string(r.unwrap_err());
        return out;
    }
    return std::move(r).unwrap();
}

/// 算指纹但不吞错误（边界「报错拒绝」用例专用）。
SemanticFpError fpErrOf(const Bytes& bytes, std::string_view name)
{
    auto cf = parseClass(bytes);
    if (cf == nullptr) {
        return SemanticFpError::BadInsn;
    }
    auto* m = methodOf(*cf, name);
    if (m == nullptr) {
        return SemanticFpError::BadInsn;
    }
    auto r = mjbase::analysis::semanticFp(*cf, *m);
    if (r.is_err()) {
        return r.unwrap_err();
    }
    return SemanticFpError::Ok;
}

// ── jasm 源面拼接 helper ──

/// 方法块：mods name desc + 可选 exceptions 段 + code 体指令文本。
std::string method(std::string_view mods, std::string_view name,
                   std::string_view desc, std::string_view insns,
                   std::string_view exc = "")
{
    const std::string excSeg = exc.empty()
        ? std::string()
        : "    exceptions: {\n" + std::string(exc) + "    },\n";
    return "  .method " + std::string(mods) + " " + std::string(name) + " "
         + std::string(desc) + " {\n" + excSeg + "    code: {\n"
         + std::string(insns) + "    }\n  }\n";
}

/// 抽象方法块（无方法体）。
std::string abstractMethod(std::string_view mods, std::string_view name,
                           std::string_view desc)
{
    return "  .method " + std::string(mods) + " " + std::string(name) + " "
         + std::string(desc) + " { }\n";
}

/// 类源装配（.super Object + public super 类体；major 缺省帧less 49）。
Bytes cls(std::string_view name, const std::string& members, ca::i32 major = 49)
{
    return fp_fix::asmFp(".super java/lang/Object\n.class public super "
                             + std::string(name) + " {\n" + members + "}\n",
                         major);
}

/// 同上，多方法块拼接形。
Bytes cls(std::string_view name, std::vector<std::string> members, ca::i32 major = 49)
{
    std::string joined;
    for (const std::string& m : members) {
        joined += m;
    }
    return cls(name, joined, major);
}

/// invokedynamic 长形指令行（站点名/描述符 + BSM 句柄 + 静态实参对象）。
std::string indyInsn(std::string_view siteName, std::string_view desc,
                     std::string_view bsmOwner, std::string_view bsmName,
                     std::string_view bsmDesc, std::string_view args)
{
    return "      invokedynamic " + std::string(siteName) + " " + std::string(desc)
         + " { invokestatic, " + std::string(bsmOwner) + "." + std::string(bsmName)
         + ", " + std::string(bsmDesc) + " } { " + std::string(args) + " }\n";
}

constexpr std::string_view kMeta3Desc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;";
constexpr std::string_view kMeta4HandleDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;)"
    "Ljava/lang/invoke/CallSite;";
constexpr std::string_view kMeta4StringDesc =
    "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
    "Ljava/lang/invoke/MethodType;Ljava/lang/String;)Ljava/lang/invoke/CallSite;";

const std::string kSpaceName(21845, ' ');   // 空格名混淆形态（21845 字节，PoC 同规格）

// ── lookupswitch 方法体字节（例外②：canonical 源语法未实证，经 DOM 写入；
//    字节为合法 classfile 内容：iconst_1; lookupswitch; pop; return）──

void put8(std::vector<ca::u8>& b, ca::u8 v) { b.push_back(v); }

void put16(std::vector<ca::u8>& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

void put32(std::vector<ca::u8>& b, ca::u32 v)
{
    b.push_back(static_cast<ca::u8>(v >> 24));
    b.push_back(static_cast<ca::u8>(v >> 16));
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

/// 构造 iconst_1 + lookupswitch + pop + return 的 code 体：pairs 为
/// (key, 目标)（'P' = pop 块，'R' = return 块），default 恒指 return 块。
std::vector<ca::u8> lookupswitchBody(const std::vector<std::pair<ca::i32, char>>& pairs)
{
    const ca::usize switchPc     = 1;
    const ca::usize operandStart = switchPc + 1;
    const ca::usize pad          = (4 - operandStart % 4) % 4;
    const ca::usize popPc        = operandStart + pad + 12 + 8 * pairs.size();
    const ca::usize retPc        = popPc + 1;

    std::vector<ca::u8> code;
    put8(code, 0x04);   // iconst_1
    put8(code, 0xAA);   // lookupswitch
    for (ca::usize i = 0; i < pad; ++i) put8(code, 0);
    put32(code, static_cast<ca::u32>(retPc - switchPc));   // default → return
    put32(code, static_cast<ca::u32>(pairs.size()));
    for (const auto& [key, target] : pairs) {
        put32(code, static_cast<ca::u32>(key));
        const ca::usize targetPc = target == 'P' ? popPc : retPc;
        put32(code, static_cast<ca::u32>(targetPc - switchPc));
    }
    put8(code, 0x57);   // pop
    put8(code, 0xB1);   // return
    return code;
}

/// 方法体整体单点替换为 lookupswitch 形（DOM Code 编辑面）。
Bytes withLookupswitchBody(const Bytes& bytes, std::string_view name,
                           const std::vector<std::pair<ca::i32, char>>& pairs)
{
    auto body = lookupswitchBody(pairs);
    return fp_fix::editMethodCode(bytes, "lookupswitch-body", name,
                                  [&](std::vector<ca::u8>& code) { code = body; });
}

}   // namespace

// ============================================================================
// 期望表（10 行）逐行钉死
// ============================================================================

/// 常量折叠：iconst/bipush/sipush/ldc int 归一为「int 值」token，跨编码不改。
TEST(MjAnalysisSemanticFpTest, ExpectConstFoldStable)
{
    const Bytes bytes = cls("FpConstFold", {
        method("public static", "c0", "()V", "      iconst_5\n      pop\n      return\n"),
        method("public static", "c1", "()V", "      bipush 5\n      pop\n      return\n"),
        method("public static", "c2", "()V", "      sipush 5\n      pop\n      return\n"),
        method("public static", "c3", "()V", "      ldc 5\n      pop\n      return\n"),
        method("public static", "h0", "()V", "      bipush 100\n      pop\n      return\n"),
        method("public static", "h1", "()V", "      sipush 100\n      pop\n      return\n"),
        method("public static", "h2", "()V", "      ldc 100\n      pop\n      return\n"),
    });

    EXPECT_EQ(fpOf(bytes, "c0"), fpOf(bytes, "c1"));
    EXPECT_EQ(fpOf(bytes, "c1"), fpOf(bytes, "c2"));
    EXPECT_EQ(fpOf(bytes, "c2"), fpOf(bytes, "c3"));
    EXPECT_EQ(fpOf(bytes, "h0"), fpOf(bytes, "h1"));
    EXPECT_EQ(fpOf(bytes, "h1"), fpOf(bytes, "h2"));
    EXPECT_NE(fpOf(bytes, "c3"), fpOf(bytes, "h0"));   // 5 与 100 不同指
}

/// 死代码增删：只看从入口可达的指令，不可达字节不进指纹。
TEST(MjAnalysisSemanticFpTest, ExpectDeadCodeStable)
{
    auto withDead = [](std::string_view name, ca::u32 deadNops) {
        std::string body = "      goto L\n";
        for (ca::u32 i = 0; i < deadNops; ++i) {
            body += "      nop\n";   // 不可达 NOP 填充
        }
        body += "      L:\n      iconst_0\n      pop\n      return\n";
        return method("public static", name, "()V", body);
    };
    auto tailDead = [](std::string_view name, ca::u32 deadNops) {
        std::string body = "      return\n";
        for (ca::u32 i = 0; i < deadNops; ++i) {
            body += "      nop\n";
        }
        return method("public static", name, "()V", body);
    };

    const Bytes bytes = cls("FpDeadCode", {
        withDead("d0", 0), withDead("d3", 3), withDead("d9", 9),
        tailDead("t0", 0), tailDead("t5", 5),
    });

    EXPECT_EQ(fpOf(bytes, "d0"), fpOf(bytes, "d3"));
    EXPECT_EQ(fpOf(bytes, "d0"), fpOf(bytes, "d9"));
    EXPECT_EQ(fpOf(bytes, "t0"), fpOf(bytes, "t5"));
}

/// 布局偏移/对齐填充：不改变存活指令相对顺序的变化不改指纹；真正的存活块
/// 重排翻指纹（规范序定义的边界，PoC 收口定案）。
TEST(MjAnalysisSemanticFpTest, ExpectLayoutDriftStableButBlockReorderFlips)
{
    auto drift = [](std::string_view name, ca::u32 fillerNops) {
        std::string body = "      goto L\n";
        for (ca::u32 i = 0; i < fillerNops; ++i) {
            body += "      nop\n";   // 死填充：存活指令相对序不变，pc 全部漂移
        }
        body += "      L:\n      iconst_1\n      pop\n      return\n";
        return method("public static", name, "()V", body);
    };
    // switch 对齐 padding 宽度不同（死填充改变 switch 落点对齐）而 token 同形。
    auto padded = [](std::string_view name, ca::u32 fillerNops) {
        std::string body = "      goto L1\n";
        for (ca::u32 i = 0; i < fillerNops; ++i) {
            body += "      nop\n";
        }
        body += "      L1:\n      iconst_1\n"
                "      tableswitch { min: 5, max: 6, cases: { L2, Ld }, default: Ld }\n"
                "      L2:\n      pop\n      Ld:\n      return\n";
        return method("public static", name, "()V", body);
    };
    // 存活块重排：条件分支的两个都可达的返回块互换落位（真实重排，必翻）。
    // v1: fallthrough 块体 ICONST_1，分支目标块体 ICONST_0；v2 互换。
    auto blockOrder = [](std::string_view name, bool firstBlockFirst) {
        std::string body = "      iconst_0\n      ifeq La\n      ";
        body += firstBlockFirst ? "iconst_1" : "iconst_0";
        body += "\n      pop\n      return\n      La:\n      ";
        body += firstBlockFirst ? "iconst_0" : "iconst_1";
        body += "\n      pop\n      return\n";
        return method("public static", name, "()V", body);
    };

    const Bytes bytes = cls("FpLayout", {
        drift("x0", 0), drift("x1", 1), drift("x7", 7),
        padded("p0", 0), padded("p1", 1), padded("p2", 2), padded("p3", 3),
        blockOrder("ba", true), blockOrder("bb", false),
    });

    EXPECT_EQ(fpOf(bytes, "x0"), fpOf(bytes, "x1"));
    EXPECT_EQ(fpOf(bytes, "x0"), fpOf(bytes, "x7"));
    EXPECT_EQ(fpOf(bytes, "p0"), fpOf(bytes, "p1"));
    EXPECT_EQ(fpOf(bytes, "p0"), fpOf(bytes, "p2"));
    EXPECT_EQ(fpOf(bytes, "p0"), fpOf(bytes, "p3"));
    EXPECT_NE(fpOf(bytes, "ba"), fpOf(bytes, "bb"));
}

/// 重排常量池、重写索引：CP 垃圾前缀逼全部语义槽位平移，指纹零变化。
TEST(MjAnalysisSemanticFpTest, ExpectCpReindexStable)
{
    // 一组把 CP 解引用面全部踩到的方法。indy/condy 需 BSM/Dynamic 面 → 61；
    // 其余（含 wide 超槽位形）49。重索引对：以 spacer 方法前置真实平移
    // 后续全部语义 CP 槽位（DOM 常量池只追加不可前插，故走源面）。

    // ── 61 对：indy + condy（BSM 表同序，BSM 实参 CP 同步平移）──
    const std::string spacer61 =
        method("public static", "zzSpacer", "()J",
               "      ldc 1L\n      ldc 2L\n      ladd\n      lreturn\n");
    auto host61 = [&](bool spacer) {
        std::string members = spacer ? spacer61 : std::string();
        members += method("public static", "indy", "()V",
                          indyInsn("apply", "()Ljava/util/function/Function;",
                                   "FpBsm", "metafactory", kMeta3Desc,
                                   "()Ljava/util/function/Function;")
                              + "      pop\n      return\n");
        members += method("public static", "condy", "()V",
                          "      ldc { make, Ljava/lang/String;, { invokestatic, "
                          "FpBsm2.stringFactory, " + std::string(kMeta4StringDesc)
                          + " }, { \"boot\" } }\n      pop\n      return\n");
        return cls("FpReindex61", members, 61);
    };
    const Bytes plain61   = host61(false);
    const Bytes junked61  = host61(true);
    EXPECT_EQ(fpOf(plain61, "indy"), fpOf(junked61, "indy"));
    EXPECT_EQ(fpOf(plain61, "condy"), fpOf(junked61, "condy"));

    // ── 49 对：ldc 全载荷（MethodHandle/MethodType 经 DOM 改锚）、引用面、
    //    switch、wide 超槽位 ──
    const std::string spacer49 =
        method("public static", "zzSpacer", "()V",
               "      ldc \"fpS1\"\n      ldc \"fpS2\"\n      ldc 2000000\n"
               "      ldc \"fpS3\"\n      pop\n      pop\n      pop\n      pop\n"
               "      return\n");
    std::string members49 = spacer49;
    members49 += method("public static", "ldcI", "()V", "      ldc 123456\n      pop\n      return\n");
    members49 += method("public static", "ldcS", "()V", "      ldc \"hello\"\n      pop\n      return\n");
    members49 += method("public static", "ldcC", "()V", "      ldc Ljava/lang/String;\n      pop\n      return\n");
    members49 += method("public static", "ldcJ", "()V", "      ldc -1L\n      pop2\n      return\n");
    members49 += method("public static", "ldcMh", "()V", "      ldc 1\n      pop\n      return\n");
    members49 += method("public static", "ldcMt", "()V", "      ldc 2\n      pop\n      return\n");
    members49 += method("public static", "field", "()V", "      getstatic FpRefs.fld I\n      pop\n      return\n");
    members49 += method("public static", "invoke", "()V", "      invokestatic FpRefs.run ()V\n      return\n");
    members49 += method("public static", "iface", "()V", "      invokeinterface FpIface.go ()V\n      return\n");
    members49 += method("public static", "types", "()V",
                        "      new FpRefs\n      pop\n      checkcast FpIface\n      pop\n      return\n");
    members49 += method("public static", "marray", "()V", "      multianewarray [[I 2\n      pop\n      return\n");
    members49 += method("public static", "switchy", "()V",
                        "      iconst_1\n      pop\n      return\n");   // 体被 DOM 换 lookupswitch 形
    members49 += method("public static", "widey", "()V", "      iload 300\n      pop\n      return\n");

    Bytes plain49 = cls("FpReindex49", members49);
    const auto mhBuilder = [](jbc::core::ConstantPool& cp) {
        const ca::u32 clsCp = cp.addClass(cp.addUtf8("FpRefs"));
        const ca::u32 nat = cp.addNameAndType(cp.addUtf8("run"), cp.addUtf8("()V"));
        return cp.addMethodHandle(6, cp.addRef(CpTag::Methodref, clsCp, nat));
    };
    const auto mtBuilder = [](jbc::core::ConstantPool& cp) {
        return cp.addMethodType(cp.addUtf8("(I)V"));
    };
    Bytes junked49 = cls("FpReindex49", members49);
    plain49        = fp_fix::pointLdcAt(plain49, "ldcMh", mhBuilder, false);
    plain49        = fp_fix::pointLdcAt(plain49, "ldcMt", mtBuilder, false);
    plain49 = withLookupswitchBody(plain49, "switchy", {{1, 'P'}, {9, 'R'}});
    junked49 = fp_fix::pointLdcAt(junked49, "ldcMh", mhBuilder, false);
    junked49 = fp_fix::pointLdcAt(junked49, "ldcMt", mtBuilder, false);
    junked49 = withLookupswitchBody(junked49, "switchy", {{1, 'P'}, {9, 'R'}});

    for (const char* name : {"ldcI", "ldcS", "ldcC", "ldcJ", "ldcMh", "ldcMt", "field",
                             "invoke", "iface", "types", "marray", "switchy", "widey"}) {
        EXPECT_EQ(fpOf(plain49, name), fpOf(junked49, name)) << name;
    }
}

/// 改行号、LVT、注解、Signature、Exceptions、StackMapTable、SourceFile：
/// 排除面字节全程不读，指纹不变。
TEST(MjAnalysisSemanticFpTest, ExpectExcludedAttrsStable)
{
    const std::string body = "      iconst_1\n      pop\n      return\n";
    const Bytes bareBytes = cls("FpExcluded", method("public static", "m", "()V", body));

    // 同体方法挂满排除面属性（载荷字节受控，结构按真实属性形态写；
    // 原始属性载荷不经 jasm 注解面——例外② DOM 追加）。
    Bytes richBytes = cls("FpExcluded", method("public static", "m", "()V", body));
    richBytes = fp_fix::addMethodRawAttrs(richBytes, "m", {
        {"LineNumberTable", Bytes{0x00, 0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04}},
        {"LocalVariableTable", Bytes{0x00, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01}},
        {"RuntimeVisibleAnnotations", Bytes{0x00, 0x01, 0x00, 0x09, 0x00, 0x00}},
        {"Signature", Bytes{0x00, 0x0B}},
        {"Exceptions", Bytes{0x00, 0x01, 0x00, 0x0C}},
    });
    richBytes = fp_fix::addCodeRawAttr(richBytes, "m", "StackMapTable",
                                       Bytes{0x00, 0x01, 65});
    richBytes = fp_fix::addClassRawAttr(richBytes, "SourceFile", Bytes{'S', 'x'});

    EXPECT_EQ(fpOf(bareBytes, "m"), fpOf(richBytes, "m"));
}

/// 改 opcode（iadd 换 isub）：语义变了指纹必变。
TEST(MjAnalysisSemanticFpTest, ExpectOpcodeChangeFlips)
{
    const Bytes bytes = cls("FpOpcode", {
        method("public static", "add", "()V", "      iadd\n      return\n"),
        method("public static", "sub", "()V", "      isub\n      return\n"),
    });
    EXPECT_NE(fpOf(bytes, "add"), fpOf(bytes, "sub"));
}

/// 增删异常处理器：异常表参与指纹（增、删、换捕获类型都翻）。
TEST(MjAnalysisSemanticFpTest, ExpectExceptionHandlerChangeFlips)
{
    // 三个方法字节完全同形；差异只在异常表。标签序固定 start=S/end=E/h=H。
    const std::string_view body =
        "      S:\n      nop\n      E:\n      return\n      H:\n      pop\n      return\n";
    const Bytes bytes = cls("FpExc", {
        method("public static", "bare", "()V", body),
        method("public static", "withCatch", "()V", body,
               "      { S, E, H, java/lang/Exception },\n"),
        method("public static", "withFinally", "()V", body,
               "      { S, E, H, * },\n"),
        method("public static", "withOther", "()V", body,
               "      { S, E, H, java/lang/Throwable },\n"),
    });

    EXPECT_NE(fpOf(bytes, "bare"), fpOf(bytes, "withCatch"));          // 增处理器
    EXPECT_NE(fpOf(bytes, "withCatch"), fpOf(bytes, "withFinally"));   // catchType 有无
    EXPECT_NE(fpOf(bytes, "withCatch"), fpOf(bytes, "withOther"));     // 捕获类型改
}

/// 改访问标志：方法头参与指纹（原始 u16 记录）。SYNTHETIC/BRIDGE/VARARGS
/// 位经 DOM 单点改写（装配器修饰面不收旗标组合——例外①，改写前自检）。
TEST(MjAnalysisSemanticFpTest, ExpectAccessFlagChangeFlips)
{
    Bytes base = cls("FpAccess", {
        method("public", "a1", "()V", "      return\n"),
        method("public static", "a2", "()V", "      return\n"),
        method("public", "a3", "()V", "      return\n"),
        method("public static", "a4", "()V", "      return\n"),
        method("public static", "a5", "()V", "      return\n"),
    });
    ASSERT_FALSE(base.empty());
    mjbase::test::expectValidateOk(base);
    Bytes bytes = fp_fix::setMethodAccess(base, "a3", 0x1000, 0);   // SYNTHETIC
    bytes      = fp_fix::setMethodAccess(bytes, "a4", 0x0040, 0);   // BRIDGE
    bytes      = fp_fix::setMethodAccess(bytes, "a5", 0x0080, 0);   // VARARGS
    EXPECT_NE(fpOf(bytes, "a1"), fpOf(bytes, "a2"));
    EXPECT_NE(fpOf(bytes, "a1"), fpOf(bytes, "a3"));
    EXPECT_NE(fpOf(bytes, "a2"), fpOf(bytes, "a4"));
    EXPECT_NE(fpOf(bytes, "a2"), fpOf(bytes, "a5"));
}

/// 改 indy bootstrap 参数：解引用面参与指纹。61 装配（BSM 面）；(I)… 形
/// 站点以 iconst_1 垫出实参保证帧自洽，垫栈不产生站点。
TEST(MjAnalysisSemanticFpTest, ExpectIndyBsmArgsChangeFlips)
{
    std::string members;
    // 方法一律 ()V（帧自洽）；站点描述符独立传参（与原字节面一致）。
    auto add = [&](std::string_view name, std::string_view siteDesc,
                   std::string_view bsmName, std::string_view bsmDesc,
                   std::string_view args, const std::string& pad = "") {
        members += method("public static", name, "()V",
                          pad + indyInsn("foo", siteDesc, "FpBsm", bsmName, bsmDesc, args)
                              + "      pop\n      return\n");
    };
    // MethodType 实参变（站点形同，BSM 静态参数异）。
    add("mt1", "(I)Ljava/lang/invoke/MethodHandle;", "metafactory", kMeta3Desc, "(I)V",
        "      iconst_1\n");
    add("mt2", "(I)Ljava/lang/invoke/MethodHandle;", "metafactory", kMeta3Desc, "(J)V",
        "      iconst_1\n");
    // String 实参变（BSM 同柄不同实参，jasm 长形逐站点自持 BSM）。
    add("str1", "()Ljava/lang/String;", "stringFactory", kMeta4StringDesc, "\"alpha\"");
    add("str2", "()Ljava/lang/String;", "stringFactory", kMeta4StringDesc, "\"beta\"");
    // indy 自身名变（BSM 同 str1，只动站点名 foo→goo）。
    members += method("public static", "name1", "()V",
                      indyInsn("foo", "()Ljava/lang/String;", "FpBsm", "stringFactory",
                               kMeta4StringDesc, "\"alpha\"")
                          + "      pop\n      return\n");
    members += method("public static", "name2", "()V",
                      indyInsn("goo", "()Ljava/lang/String;", "FpBsm", "stringFactory",
                               kMeta4StringDesc, "\"alpha\"")
                          + "      pop\n      return\n");
    // MethodHandle 实参变。
    add("h1", "()Ljava/lang/invoke/MethodHandle;", "handleFactory", kMeta4HandleDesc,
        "{ invokestatic, FpRefs.run, ()V }");
    add("h2", "()Ljava/lang/invoke/MethodHandle;", "handleFactory", kMeta4HandleDesc,
        "{ invokestatic, FpRefs.run2, ()V }");
    // 多实参其一变（整数；类常量形 { FpRefs } 为未实证字面，CI 装配为准）。
    add("multi1", "()Ljava/lang/Object;", "multi", kMeta3Desc, "1, { FpRefs }, \"x\"");
    add("multi2", "()Ljava/lang/Object;", "multi", kMeta3Desc, "2, { FpRefs }, \"x\"");

    const Bytes bytes = cls("FpIndy", members, 61);

    EXPECT_NE(fpOf(bytes, "mt1"), fpOf(bytes, "mt2"));       // MethodType 实参变
    EXPECT_NE(fpOf(bytes, "str1"), fpOf(bytes, "str2"));     // String 实参变
    EXPECT_NE(fpOf(bytes, "name1"), fpOf(bytes, "name2"));   // indy 自身名变
    EXPECT_NE(fpOf(bytes, "h1"), fpOf(bytes, "h2"));         // MethodHandle 实参变
    EXPECT_NE(fpOf(bytes, "multi1"), fpOf(bytes, "multi2")); // 多实参其一变
}

/// 被引用的类/方法改名：出口语义就是名字，必翻。
TEST(MjAnalysisSemanticFpTest, ExpectReferencedNameChangeFlips)
{
    const Bytes bytes = cls("FpRefRename", {
        method("public static", "m1", "()V", "      invokestatic FpRefs.run ()V\n      return\n"),
        method("public static", "m2", "()V", "      invokestatic FpRefs.run2 ()V\n      return\n"),
        method("public static", "m3", "()V", "      invokestatic FpOther.run ()V\n      return\n"),
        method("public static", "f1", "()V", "      getstatic FpRefs.fld I\n      pop\n      return\n"),
        method("public static", "f2", "()V", "      getstatic FpRefs.fld2 I\n      pop\n      return\n"),
        method("public static", "t1", "()V", "      new FpRefs\n      pop\n      return\n"),
        method("public static", "t2", "()V", "      new FpRenamed\n      pop\n      return\n"),
    });
    EXPECT_NE(fpOf(bytes, "m1"), fpOf(bytes, "m2"));   // 被调方法名改
    EXPECT_NE(fpOf(bytes, "m1"), fpOf(bytes, "m3"));   // 被调类名改
    EXPECT_NE(fpOf(bytes, "f1"), fpOf(bytes, "f2"));   // 被引用字段名改
    EXPECT_NE(fpOf(bytes, "t1"), fpOf(bytes, "t2"));   // 类型引用改
}

// ============================================================================
// 表外 3 条（PoC 补验面）
// ============================================================================

/// 方法自身改名不进指纹：空格名混淆形态（21845 字节）同体双方法恒等。
/// 空格名 jasm 无法从源码构造——占位名装配后 DOM 单点改名（例外②）。
TEST(MjAnalysisSemanticFpTest, SelfRenameStable)
{
    const std::string body = "      ldc \"body\"\n      pop\n      return\n";
    const Bytes a = cls("FpSelfNameA",
                        method("public static", "plainName", "()V", body));

    Bytes sp = cls("FpSelfNameB",
                   method("public static", "placeholderName", "()V", body));
    ASSERT_FALSE(sp.empty());
    mjbase::test::expectValidateOk(sp);
    sp = fp_fix::renameMethod(sp, "placeholderName", kSpaceName);

    EXPECT_EQ(fpOf(a, "plainName"), fpOf(sp, kSpaceName));
}

/// 被引用名字段改名翻引用者（空格名字段名同样参与；DOM 改锚例外②）。
TEST(MjAnalysisSemanticFpTest, ReferencedFieldRenameFlips)
{
    const std::string spaceField(21845, ' ');
    const std::string body = "      getstatic FpRefs.orig I\n      pop\n      return\n";
    Bytes bytes = cls("FpFieldRename", {
        method("public static", "r1", "()V", body),
        method("public static", "r2", "()V", body),
    });
    ASSERT_FALSE(bytes.empty());
    mjbase::test::expectValidateOk(bytes);
    bytes = fp_fix::patchMemberRefName(bytes, "r2", 0xB2 /*GETSTATIC*/, spaceField, "I");

    EXPECT_NE(fpOf(bytes, "r1"), fpOf(bytes, "r2"));
}

/// native/abstract 没有方法体：指纹只盖方法头（版本 + 访问标志 + 描述符），
/// 永远恒定；方法名/类名/Exceptions 不参与。Exceptions/SourceFile 原始
/// 载荷经 DOM 追加（例外②）。
TEST(MjAnalysisSemanticFpTest, NativeAbstractHeaderOnlyStable)
{
    const Bytes aBytes = cls("FpNativeA", {
        abstractMethod("public abstract", "calc", "(I)J"),
        abstractMethod("public abstract", "calc0", "(I)J"),
        abstractMethod("abstract", "calc1", "(I)J"),
    });

    Bytes bBytes = cls("FpNativeB", abstractMethod("public abstract", "calc", "(I)J"));
    ASSERT_FALSE(bBytes.empty());
    mjbase::test::expectValidateOk(bBytes);
    bBytes = fp_fix::addMethodRawAttrs(bBytes, "calc",
                                       {{"Exceptions", Bytes{0x00, 0x01, 0x00, 0x63}}});
    bBytes = fp_fix::addClassRawAttr(bBytes, "SourceFile", Bytes{'N', 'a'});

    // 同访问标志同描述符：跨类、带不带 Exceptions、不同方法名（身份层的事）
    // 指纹恒定。
    EXPECT_EQ(fpOf(aBytes, "calc"), fpOf(bBytes, "calc"));
    EXPECT_EQ(fpOf(aBytes, "calc"), fpOf(aBytes, "calc0"));
    // 访问标志仍是方法头一部分：变了就翻。
    EXPECT_NE(fpOf(aBytes, "calc"), fpOf(aBytes, "calc1"));
}

// ============================================================================
// 八项边界（按原型口径各一条钉死）
// ============================================================================

/// 边界 1：异常表 pc 映射端点语义——覆盖端点映射为「pc 不小于端点的首条
/// 存活指令」的规范序号（end 不含、越过末尾取存活总数）；handler 必须精确
/// 落在存活指令上，否则报错拒绝；catchType 0 记空。
/// 原始 pc 端点 jasm 异常表不收——DOM 单点改写（例外②申报）。
TEST(MjAnalysisSemanticFpTest, EdgeExcEndpointMapping)
{
    // 体形：GOTO L; NOP; NOP(dead); L: NOP; RETURN; POP; RETURN。
    // code 长度 9；存活 pcs = {0, 5, 6, 7, 8}；POP 在 pc7。
    const std::string_view body =
        "      goto L\n      nop\n      nop\n      L:\n      nop\n      return\n"
        "      pop\n      return\n";
    Bytes bytes = cls("FpExcEdge", {
        method("public static", "v1", "()V", body),
        method("public static", "v2", "()V", body),
        method("public static", "v3", "()V", body),
        method("public static", "v4", "()V", body),
        method("public static", "liveDeadHandler", "()V", body),
        method("public static", "badHandler", "()V", body),
    });
    bytes = fp_fix::setExcEntries(bytes, "v1", {fp_fix::ExcPcSpec{0, 3, 7, "java/lang/Exception"}});
    bytes = fp_fix::setExcEntries(bytes, "v2", {fp_fix::ExcPcSpec{0, 1, 7, "java/lang/Exception"}});
    bytes = fp_fix::setExcEntries(bytes, "v3", {fp_fix::ExcPcSpec{0, 9, 7, "java/lang/Exception"}});
    bytes = fp_fix::setExcEntries(bytes, "v4", {fp_fix::ExcPcSpec{0, 200, 7, "java/lang/Exception"}});
    // 指向死指令的 handler 合法——处理器入口按可达根处理，直接救活；
    // 指向指令中部（GOTO 操作数内 pc=1）→ 拒绝，不做静默近似。
    bytes = fp_fix::setExcEntries(bytes, "liveDeadHandler", {fp_fix::ExcPcSpec{0, 3, 3, "java/lang/Exception"}});
    bytes = fp_fix::setExcEntries(bytes, "badHandler", {fp_fix::ExcPcSpec{0, 1, 1, "java/lang/Exception"}});

    EXPECT_EQ(fpOf(bytes, "v1"), fpOf(bytes, "v2"));
    EXPECT_EQ(fpOf(bytes, "v3"), fpOf(bytes, "v4"));
    EXPECT_NE(fpOf(bytes, "v1"), fpOf(bytes, "v3"));
    EXPECT_EQ(fpErrOf(bytes, "liveDeadHandler"), SemanticFpError::Ok);
    EXPECT_EQ(fpErrOf(bytes, "badHandler"), SemanticFpError::BadExcEntry);
}

/// 边界 1 补：catchType 0（any/finally）合法且记空串；表项顺序原样保留。
TEST(MjAnalysisSemanticFpTest, EdgeExcCatchTypeAndOrder)
{
    // 双处理器体：NOP; RETURN; H1: POP; RETURN; H2: ACONST_NULL; POP; RETURN。
    const std::string_view body =
        "      S:\n      nop\n      E:\n      return\n      H1:\n      pop\n"
        "      return\n      H2:\n      aconst_null\n      pop\n      return\n";
    const Bytes bytes = cls("FpExcOrder", {
        method("public static", "anyFirst", "()V", body,
               "      { S, E, H1, * },\n      { S, E, H2, java/lang/Throwable },\n"),
        method("public static", "typedFirst", "()V", body,
               "      { S, E, H2, java/lang/Throwable },\n      { S, E, H1, * },\n"),
    });
    EXPECT_NE(fpOf(bytes, "anyFirst"), fpOf(bytes, "typedFirst"));
}

/// 边界 2：方法头不含所属类名与 Exceptions——同类签名同体方法跨类恒等，
/// Exceptions/Signature/注解/MethodParameters 全程不读不记。
TEST(MjAnalysisSemanticFpTest, EdgeHeaderExcludesClassAndExceptions)
{
    const std::string body = "      invokestatic FpRefs.run ()V\n      return\n";
    const Bytes a = cls("FpClsA", method("public static", "run", "()V", body));

    Bytes b = cls("FpClsB", method("public static", "run", "()V", body));
    b = fp_fix::addMethodRawAttrs(b, "run", {
        {"Exceptions", Bytes{0x00, 0x01, 0x00, 0x63}},
        {"Signature", Bytes{0x00, 0x09}},
    });
    EXPECT_EQ(fpOf(a, "run"), fpOf(b, "run"));
}

/// 边界 3：非 int 常量不折叠——FCONST_1 与 ldc float 同值不同指；
/// lconst/fconst/dconst 记 plain opcode，ldc 家族记原始位型。
TEST(MjAnalysisSemanticFpTest, EdgeNonIntConstNotFolded)
{
    const Bytes bytes = cls("FpNonInt", {
        method("public static", "fc1", "()V", "      fconst_1\n      return\n"),
        method("public static", "fLdc", "()V", "      ldc 1.0F\n      return\n"),
        method("public static", "fLdc2", "()V", "      ldc 2.0F\n      return\n"),
        method("public static", "lc1", "()V", "      lconst_1\n      return\n"),
        method("public static", "lLdc", "()V", "      ldc 1L\n      return\n"),
        method("public static", "dc1", "()V", "      dconst_1\n      return\n"),
        method("public static", "dLdc", "()V", "      ldc 1.0D\n      return\n"),
        method("public static", "fc0", "()V", "      fconst_0\n      return\n"),
    });
    EXPECT_NE(fpOf(bytes, "fc1"), fpOf(bytes, "fLdc"));    // 同值不同编码：翻
    EXPECT_NE(fpOf(bytes, "fLdc"), fpOf(bytes, "fLdc2"));  // 位型不同：翻
    EXPECT_NE(fpOf(bytes, "lc1"), fpOf(bytes, "lLdc"));
    EXPECT_NE(fpOf(bytes, "dc1"), fpOf(bytes, "dLdc"));
    EXPECT_NE(fpOf(bytes, "fc0"), fpOf(bytes, "fc1"));     // 不同 opcode：翻
}

/// 边界 4：ldc 全载荷解到语义值——ldc/ldc_w 的 opcode 不进流（同载荷同
/// token）；MethodHandle 解两层到（kind，类，名，描述符）；MethodType 解为
/// 描述符；Class 解为类名。MethodHandle/MethodType 载荷与 LDC_W 强制形
/// 经 DOM 改锚（例外①申报）。
TEST(MjAnalysisSemanticFpTest, EdgeLdcFullDeref)
{
    std::string members;
    members += method("public static", "mhLdc", "()V", "      ldc 1\n      pop\n      return\n");
    members += method("public static", "mhLdcW", "()V", "      ldc 1\n      pop\n      return\n");
    members += method("public static", "mhLdc2", "()V", "      ldc 1\n      pop\n      return\n");
    members += method("public static", "mt1", "()V", "      ldc 2\n      pop\n      return\n");
    members += method("public static", "mt2", "()V", "      ldc 3\n      pop\n      return\n");
    members += method("public static", "cls1", "()V", "      ldc LFpRefs;\n      pop\n      return\n");
    members += method("public static", "cls2", "()V", "      ldc LFpOther;\n      pop\n      return\n");
    members += method("public static", "s1", "()V", "      ldc \"alpha\"\n      pop\n      return\n");
    members += method("public static", "s2", "()V", "      ldc \"beta\"\n      pop\n      return\n");
    Bytes bytes = cls("FpLdcDeref", members);

    const auto mhRun = [](jbc::core::ConstantPool& cp) {
        const ca::u32 owner = cp.addClass(cp.addUtf8("FpRefs"));
        const ca::u32 nat   = cp.addNameAndType(cp.addUtf8("run"), cp.addUtf8("()V"));
        return cp.addMethodHandle(6, cp.addRef(CpTag::Methodref, owner, nat));
    };
    const auto mhRun2 = [](jbc::core::ConstantPool& cp) {
        const ca::u32 owner = cp.addClass(cp.addUtf8("FpRefs"));
        const ca::u32 nat   = cp.addNameAndType(cp.addUtf8("run2"), cp.addUtf8("()V"));
        return cp.addMethodHandle(6, cp.addRef(CpTag::Methodref, owner, nat));
    };
    bytes = fp_fix::pointLdcAt(bytes, "mhLdc", mhRun, false);
    bytes = fp_fix::pointLdcAt(bytes, "mhLdcW", mhRun, true);   // LDC_W 强制形
    bytes = fp_fix::pointLdcAt(bytes, "mhLdc2", mhRun2, false);
    bytes = fp_fix::pointLdcAt(bytes, "mt1",
                               [](jbc::core::ConstantPool& cp) {
                                   return cp.addMethodType(cp.addUtf8("(I)V"));
                               }, false);
    bytes = fp_fix::pointLdcAt(bytes, "mt2",
                               [](jbc::core::ConstantPool& cp) {
                                   return cp.addMethodType(cp.addUtf8("(J)V"));
                               }, false);

    // ldc 与 ldc_w 同载荷同 token（opcode 不进流）。
    EXPECT_EQ(fpOf(bytes, "mhLdc"), fpOf(bytes, "mhLdcW"));
    // MethodHandle 两层解引用：名字变即翻（名字在 NAT，两层跳转之外）。
    EXPECT_NE(fpOf(bytes, "mhLdc"), fpOf(bytes, "mhLdc2"));
    EXPECT_NE(fpOf(bytes, "mt1"), fpOf(bytes, "mt2"));
    EXPECT_NE(fpOf(bytes, "cls1"), fpOf(bytes, "cls2"));
    EXPECT_NE(fpOf(bytes, "s1"), fpOf(bytes, "s2"));
}

/// 边界 4 补：condy 按 indy 同款记（名、描述符、BSM 句柄、全部静态参数），
/// 静态参数逐个类型化解引用。61 装配（Dynamic 面）。
TEST(MjAnalysisSemanticFpTest, EdgeCondyFullDeref)
{
    auto condySrc = [](std::string_view dynName, std::string_view arg) {
        return "      ldc { " + std::string(dynName)
             + ", Ljava/lang/String;, { invokestatic, FpBsm.stringFactory, "
             + std::string(kMeta4StringDesc) + " }, { " + std::string(arg)
             + " } }\n      pop\n      return\n";
    };
    const Bytes bytes = cls("FpCondy", {
        method("public static", "c1", "()V", condySrc("make", "\"alpha\"")),
        method("public static", "c2", "()V", condySrc("make", "\"beta\"")),
        method("public static", "c3", "()V", condySrc("make2", "\"alpha\"")),
    }, 61);
    EXPECT_NE(fpOf(bytes, "c1"), fpOf(bytes, "c2"));   // 静态参数变
    EXPECT_NE(fpOf(bytes, "c1"), fpOf(bytes, "c3"));   // condy 自身名变
}

/// 边界 5：访问标志不剔除编译器旗标——BRIDGE/SYNTHETIC/VARARGS 原样参与
///（DOM 单点位改写，例外①申报）。
TEST(MjAnalysisSemanticFpTest, EdgeAccessFlagsKeepCompilerBits)
{
    Bytes base = cls("FpCompilerBits", {
        method("public", "plain", "()V", "      return\n"),
        method("public", "synthetic", "()V", "      return\n"),
        method("public", "bridge", "()V", "      return\n"),
        method("public", "varargs", "()V", "      return\n"),
    });
    ASSERT_FALSE(base.empty());
    mjbase::test::expectValidateOk(base);
    Bytes bytes      = fp_fix::setMethodAccess(base, "synthetic", 0x1000, 0);
    bytes            = fp_fix::setMethodAccess(bytes, "bridge", 0x0040, 0);
    bytes            = fp_fix::setMethodAccess(bytes, "varargs", 0x0080, 0);
    EXPECT_NE(fpOf(bytes, "plain"), fpOf(bytes, "synthetic"));
    EXPECT_NE(fpOf(bytes, "plain"), fpOf(bytes, "bridge"));
    EXPECT_NE(fpOf(bytes, "plain"), fpOf(bytes, "varargs"));
}

/// 边界 6：wide 前缀不单记——wide 与非 wide 同语义同 token；槽位与增量照
/// 常记实际值。窄槽位 WIDE 形装配器不直达——DOM 单点前插（例外①申报）；
/// 超槽位（>255）形依赖装配器自动宽化。
TEST(MjAnalysisSemanticFpTest, EdgeWideNotRecorded)
{
    Bytes bytes = cls("FpWide", {
        method("public static", "loadN", "()V", "      iload 1\n      pop\n      return\n"),
        method("public static", "loadW", "()V", "      iload 1\n      pop\n      return\n"),
        method("public static", "iincN", "()V", "      iinc 3 1\n      return\n"),
        method("public static", "iincW", "()V", "      iinc 3 1\n      return\n"),
        method("public static", "big300", "()V", "      iload 300\n      pop\n      return\n"),
        method("public static", "big301", "()V", "      iload 301\n      pop\n      return\n"),
    });
    bytes = fp_fix::widenVarInsn(bytes, "loadW");
    bytes = fp_fix::widenIinc(bytes, "iincW");
    EXPECT_EQ(fpOf(bytes, "loadN"), fpOf(bytes, "loadW"));
    EXPECT_EQ(fpOf(bytes, "iincN"), fpOf(bytes, "iincW"));
    EXPECT_NE(fpOf(bytes, "big300"), fpOf(bytes, "big301"));
}

/// 边界 7：BSM 参数解一整层——Class/MethodType/MethodHandle 展开到语义值；
/// 参数位再嵌 condy/indy 直接报错，不递归。嵌套动态形 JVMS 禁、装配器
/// 拒收——占位空实参装配后 DOM 补嵌套 CP 索引（例外①申报：改写前自检 +
/// fpErrOf 实跑为证）。
TEST(MjAnalysisSemanticFpTest, EdgeBsmOneLevelDerefNestedErrors)
{
    std::string members;
    members += method("public static", "ok", "()V",
                      indyInsn("foo", "()Ljava/lang/Object;", "FpBsm", "rich", kMeta3Desc,
                               "{ FpRefs }, (I)V, { invokestatic, FpRefs.run, ()V }, 7, 1L, 1.0F, 1.0D, \"x\"")
                          + "      pop\n      return\n");
    members += method("public static", "nestedCondy", "()V",
                      indyInsn("foo", "()Ljava/lang/Object;", "FpBsm", "nested", kMeta3Desc, "")
                          + "      pop\n      return\n");
    members += method("public static", "nestedIndy", "()V",
                      indyInsn("foo", "()Ljava/lang/Object;", "FpBsm", "nested2", kMeta3Desc, "")
                          + "      pop\n      return\n");
    Bytes bytes = cls("FpBsmNested", members, 61);
    ASSERT_FALSE(bytes.empty());
    mjbase::test::expectValidateOk(bytes);
    // BSM 表按站点序展开（entries[0]=rich, [1]=nested, [2]=nested2）。
    bytes = fp_fix::appendBsmNestedDynamic(bytes, 1, "c", "I", false);
    bytes = fp_fix::appendBsmNestedDynamic(bytes, 2, "x", "()V", true);

    EXPECT_EQ(fpErrOf(bytes, "ok"), SemanticFpError::Ok);
    EXPECT_EQ(fpErrOf(bytes, "nestedCondy"), SemanticFpError::NestedDynamic);
    EXPECT_EQ(fpErrOf(bytes, "nestedIndy"), SemanticFpError::NestedDynamic);
}

/// 边界 8：switch key 原样进指纹——tableswitch 按 low 重推连续 key 全列，
/// lookupswitch 逐 case 记原始 key；default 先记、顺序与 class 文件一致。
/// lookupswitch 体经 DOM 字节写入（例外②申报，合法 classfile 内容）。
TEST(MjAnalysisSemanticFpTest, EdgeSwitchKeysExact)
{
    // 四路同形 switch（default + 两目标），差异只在 key 面。
    auto ts = [](std::string_view name, ca::i32 low) {
        return method("public static", name, "()V",
                      "      iconst_1\n      tableswitch { min: " + std::to_string(low)
                          + ", max: " + std::to_string(low + 1)
                          + ", cases: { La, Lb }, default: Ld }\n"
                          "      La:\n      pop\n      Lb:\n      pop\n      Ld:\n      return\n");
    };
    Bytes bytes = cls("FpSwitch", {
        ts("ts56", 5),
        ts("ts67", 6),
        method("public static", "ls56", "()V", "      iconst_1\n      pop\n      return\n"),
        method("public static", "ls57", "()V", "      iconst_1\n      pop\n      return\n"),
        method("public static", "lsSwap", "()V", "      iconst_1\n      pop\n      return\n"),
    });
    bytes = withLookupswitchBody(bytes, "ls56", {{1, 'P'}, {9, 'R'}});
    bytes = withLookupswitchBody(bytes, "ls57", {{1, 'P'}, {7, 'R'}});
    bytes = withLookupswitchBody(bytes, "lsSwap", {{6, 'R'}, {5, 'P'}});
    EXPECT_NE(fpOf(bytes, "ts56"), fpOf(bytes, "ts67"));    // key 全列平移
    EXPECT_NE(fpOf(bytes, "ls56"), fpOf(bytes, "ls57"));    // 原始 key 变
    EXPECT_NE(fpOf(bytes, "ls56"), fpOf(bytes, "lsSwap"));  // case 顺序原样保留
    EXPECT_NE(fpOf(bytes, "ts56"), fpOf(bytes, "ls56"));    // opcode 精确记录
}

/// switch 目标按规范序号记录：目标前的死填充漂移 pc 不翻（原 lookupswitch
/// 形以 tableswitch 承载——目标规范序性质 switch 无关）。
TEST(MjAnalysisSemanticFpTest, EdgeSwitchTargetsCanonical)
{
    auto withFiller = [](std::string_view name, ca::u32 fillerNops) {
        std::string body = "      goto L1\n";
        for (ca::u32 i = 0; i < fillerNops; ++i) {
            body += "      nop\n";
        }
        body += "      L1:\n      iconst_1\n"
                "      tableswitch { min: 1, max: 2, cases: { L2, Ld }, default: Ld }\n"
                "      L2:\n      pop\n      Ld:\n      return\n";
        return method("public static", name, "()V", body);
    };
    const Bytes bytes = cls("FpSwitchCanon", {
        withFiller("w0", 0), withFiller("w5", 5),
    });
    EXPECT_EQ(fpOf(bytes, "w0"), fpOf(bytes, "w5"));
}

/// invokeinterface 的 count/0 字节可由描述符导出，不进指纹。count=3 形
/// 装配器不直达——DOM 单点改写（例外①申报）。
TEST(MjAnalysisSemanticFpTest, EdgeInvokeInterfaceCountNotRecorded)
{
    const std::string body = "      invokeinterface FpIface.go ()V\n      return\n";
    Bytes bytes = cls("FpIfaceCount", {
        method("public static", "c1", "()V", body),
        method("public static", "c3", "()V", body),
    });
    bytes = fp_fix::patchIfaceCount(bytes, "c3", 3);
    EXPECT_EQ(fpOf(bytes, "c1"), fpOf(bytes, "c3"));
}

/// 分支目标落在指令中部：拒绝（BadBranchTarget），不静默近似。
/// 中部目标装配器不直达——合法基座经 retargetFirstGoto 重锚（例外①申报）。
TEST(MjAnalysisSemanticFpTest, EdgeMidInstructionTargetRejected)
{
    const Bytes base = mjbase::test::assembleMethodClass(
        "mid", "()V", "      goto L0\n      L0:\n      sipush 0\n      pop\n      return\n",
        "FpMidInsn");
    ASSERT_FALSE(base.empty());
    mjbase::test::expectValidateOk(base);
    const Bytes bytes = mjbase::test::retargetFirstGoto(base, 2);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(fpErrOf(bytes, "mid"), SemanticFpError::BadBranchTarget);
}

// ============================================================================
// 确定性与唯一性
// ============================================================================

/// 同一方法双算一致（重解析、重新构建均一致）。indy/tableswitch 面 61
///（帧自洽），wide 超槽位面 49（帧less）——确定性属性与类装箱无关，逐类
/// 双算 + 拷贝重解析验收。
TEST(MjAnalysisSemanticFpTest, DeterminismDoubleCompute)
{
    const Bytes a = cls("FpDeterminismA", {
        method("public static", "indy", "()V",
               indyInsn("apply", "()Ljava/util/function/Function;", "FpBsm",
                        "metafactory", kMeta3Desc, "()Ljava/util/function/Function;")
                   + "      pop\n      return\n"),
        method("public static", "switchy", "()V",
               "      iconst_1\n"
               "      tableswitch { min: 1, max: 3, cases: { L1, Ld, L1 }, default: Ld }\n"
               "      L1:\n      pop\n      Ld:\n      return\n"),
    }, 61);
    const Bytes b = cls("FpDeterminismB", {
        method("public static", "widey", "()V",
               "      iload 300\n      iinc 300 -5\n      pop\n      return\n"),
    });

    for (const char* name : {"indy", "switchy"}) {
        EXPECT_EQ(fpOf(a, name), fpOf(a, name)) << name;
    }
    for (const char* name : {"widey"}) {
        EXPECT_EQ(fpOf(b, name), fpOf(b, name)) << name;
    }
    // 拷贝一份字节重新解析（fresh DOM），指纹仍一致。
    const Bytes copyA = a;
    const Bytes copyB = b;
    for (const char* name : {"indy", "switchy"}) {
        EXPECT_EQ(fpOf(a, name), fpOf(copyA, name)) << name;
    }
    for (const char* name : {"widey"}) {
        EXPECT_EQ(fpOf(b, name), fpOf(copyB, name)) << name;
    }
}

/// 唯一性：合成语料去重统计——同体组内恒等、组间与唯一体全不同、零碰撞。
/// indy 面 61 单独装箱（帧自洽），其余 49——去重统计跨两类合并验收。
TEST(MjAnalysisSemanticFpTest, CorpusDedupStats)
{
    std::vector<std::pair<const Bytes*, std::string>> corpus;
    ca::usize uniqueCount = 0;
    auto uniq = [&](const Bytes& bytes, std::string_view name) {
        corpus.emplace_back(&bytes, std::string(name));
        if (name[0] == 'u') {
            ++uniqueCount;   // 唯一体计数（g 组同体方法不入统计，与原口径一致）
        }
    };

    Bytes b49 = cls("FpCorpus49", {
        method("public static", "u01", "()V", "      return\n"),
        method("public static", "u02", "()V", "      iconst_1\n      pop\n      return\n"),
        method("public static", "u03", "()V", "      iconst_2\n      pop\n      return\n"),
        method("public static", "u04", "()V", "      ldc 77\n      pop\n      return\n"),
        method("public static", "u05", "()V", "      aconst_null\n      pop\n      return\n"),
        method("public static", "u06", "()V", "      new FpA\n      pop\n      return\n"),
        method("public static", "u07", "()V", "      new FpB\n      pop\n      return\n"),
        method("public static", "u08", "()V", "      getstatic FpA.a I\n      pop\n      return\n"),
        method("public static", "u09", "()V", "      getstatic FpA.b I\n      pop\n      return\n"),
        method("public static", "u10", "()V", "      invokestatic FpA.m ()V\n      return\n"),
        method("public static", "u11", "()V", "      invokestatic FpA.n ()V\n      return\n"),
        method("public static", "u12", "()V", "      ldc \"s1\"\n      pop\n      return\n"),
        method("public static", "u13", "()V", "      ldc \"s2\"\n      pop\n      return\n"),
        method("public static", "u14", "()V", "      iadd\n      return\n"),
        method("public static", "u15", "()V", "      isub\n      return\n"),
        method("public static", "u16", "()V",
               "      iconst_0\n      ifeq L\n      pop\n      L:\n      nop\n      return\n"),
        method("public static", "u17", "()V",
               "      iconst_0\n      ifne L\n      pop\n      L:\n      nop\n      return\n"),
        method("public static", "u18", "()V",
               "      tableswitch { min: 1, max: 3, cases: { La, Lb, Lc }, default: Ld }\n"
               "      La:\n      return\n      Lb:\n      return\n      Lc:\n      return\n"
               "      Ld:\n      return\n"),
        method("public static", "u19", "()V", "      pop\n      return\n"),
        method("public static", "u20", "()V", "      pop\n      return\n"),
        method("public static", "u21", "()V", "      iload 300\n      pop\n      return\n"),
        method("public static", "u22", "()V", "      iinc 300 5\n      return\n"),
        method("public static", "u24", "()V", "      ldc 1\n      pop\n      return\n"),
        method("public static", "u25", "()V", "      invokeinterface FpIface.go ()V\n      return\n"),
        method("public static", "u26", "()V", "      multianewarray [[I 2\n      pop\n      return\n"),
        method("public static", "g1a", "()V", "      dup\n      pop\n      return\n"),
        method("public static", "g1b", "()V", "      dup\n      pop\n      return\n"),
        method("public static", "g1c", "()V", "      dup\n      pop\n      return\n"),
        method("public static", "g2a", "()V", "      iinc 0 1\n      return\n"),
        method("public static", "g2b", "()V", "      iinc 0 1\n      return\n"),
    });
    // lookupswitch 体（u19/u20）与 MethodHandle ldc（u24）经 DOM 载面落地。
    b49 = withLookupswitchBody(b49, "u19", {{1, 'P'}, {9, 'R'}});
    b49 = withLookupswitchBody(b49, "u20", {{1, 'P'}, {10, 'R'}});
    b49 = fp_fix::pointLdcAt(b49, "u24",
                             [](jbc::core::ConstantPool& cp) {
                                 const ca::u32 owner = cp.addClass(cp.addUtf8("FpA"));
                                 const ca::u32 nat   = cp.addNameAndType(cp.addUtf8("m"),
                                                                         cp.addUtf8("()V"));
                                 return cp.addMethodHandle(6, cp.addRef(CpTag::Methodref, owner, nat));
                             }, false);

    const Bytes b61 = cls("FpCorpus61", {
        method("public static", "u23", "()V",
               indyInsn("apply", "()Ljava/util/function/Function;", "FpBsm",
                        "metafactory", kMeta3Desc, "()Ljava/util/function/Function;")
                   + "      pop\n      return\n"),
    }, 61);

    for (const char* name :
         {"u01", "u02", "u03", "u04", "u05", "u06", "u07", "u08", "u09", "u10", "u11",
          "u12", "u13", "u14", "u15", "u16", "u17", "u18", "u19", "u20", "u21", "u22",
          "u24", "u25", "u26", "g1a", "g1b", "g1c", "g2a", "g2b"}) {
        uniq(b49, name);
    }
    uniq(b61, "u23");

    std::map<std::string, SemanticFp> fps;
    for (const auto& [bytes, name] : corpus) {
        fps.emplace(name, fpOf(*bytes, name));
    }
    ASSERT_EQ(fps.size(), corpus.size());

    // 组内恒等。
    EXPECT_EQ(fps.at("g1a"), fps.at("g1b"));
    EXPECT_EQ(fps.at("g1a"), fps.at("g1c"));
    EXPECT_EQ(fps.at("g2a"), fps.at("g2b"));
    // 组间不同体必不同。
    EXPECT_NE(fps.at("g1a"), fps.at("g2a"));

    // 唯一体两两不同（零碰撞）。
    std::set<SemanticFp> distinct;
    for (const auto& [bytes, name] : corpus) {
        if (name[0] != 'u') {
            continue;
        }
        distinct.insert(fps.at(name));
    }
    EXPECT_EQ(distinct.size(), uniqueCount);

    // 全语料去重统计：26 唯一体 + 2 组 = 28 个不同指纹。
    std::set<SemanticFp> all;
    for (const auto& [name, fp] : fps) {
        all.insert(fp);
    }
    EXPECT_EQ(all.size(), uniqueCount + 2);
}
