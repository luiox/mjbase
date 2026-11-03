/// @file        semantic_fp_test.cpp
/// @brief       semantic_fp 方法语义指纹测试：期望表全矩阵 + 八项边界 +
///              确定性/唯一性（#132 规范 §验收，全部合成类夹具）。
///
/// 「该稳的稳、该翻的翻」双向钉死：期望表 10 行逐行、表外 3 条（方法自身
/// 改名/被引用字段改名/native-abstract 恒定）、八项边界按原型口径各一条、
/// 同方法双算一致 + CP 垃圾注入逼重索引零变化 + 合成语料去重统计。混淆
/// 形态样本用空格名（21845 字节）合成形态，不引用任何具体工具名。
#include <gtest/gtest.h>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"

#include "semantic_fp_fixture.hpp"
#include "mjbase/analysis/semantic_fp.hpp"

// 本目录另有 analysis 夹具的同名头；指纹夹具的命名空间经别名区分。
namespace fp_fix = mjbase::analysis::fp_test_support;

using fp_fix::Bytes;
using fp_fix::BsmArg;
using fp_fix::ClassBytesBuilder;
using fp_fix::CodeEmit;
using fp_fix::ExcSpec;
using fp_fix::MethodSpec;
using fp_fix::RawAttrSpec;
using jbc::core::Op;
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

/// 组装多方法类。
Bytes assembleClass(std::string_view className, const std::vector<MethodSpec>& methods,
                    ca::u16 cpPrefixJunk = 0)
{
    ClassBytesBuilder b(className, cpPrefixJunk);
    return b.assemble(methods);
}

/// 具名方法规格。
MethodSpec spec(std::string_view name, std::function<void(CodeEmit&)> emit, ca::u16 access = 0x0009)
{
    MethodSpec m;
    m.name   = name;
    m.access = access;
    m.emit   = std::move(emit);
    return m;
}

const std::string kSpaceName(21845, ' ');   // 空格名混淆形态（21845 字节，PoC 同规格）

}   // namespace

// ============================================================================
// 期望表（10 行）逐行钉死
// ============================================================================

/// 常量折叠：iconst/bipush/sipush/ldc int 归一为「int 值」token，跨编码不改。
TEST(MjAnalysisSemanticFpTest, ExpectConstFoldStable)
{
    const Bytes bytes = assembleClass("FpConstFold", {
        spec("c0", [](CodeEmit& e) { e.i32const(5); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("c1", [](CodeEmit& e) { e.op(Op::BIPUSH); e.u8(5); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("c2", [](CodeEmit& e) { e.op(Op::SIPUSH); e.u16(5); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("c3", [](CodeEmit& e) { e.ldcInteger(5); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("h0", [](CodeEmit& e) { e.op(Op::BIPUSH); e.u8(100); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("h1", [](CodeEmit& e) { e.op(Op::SIPUSH); e.u16(100); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("h2", [](CodeEmit& e) { e.ldcInteger(100); e.op(Op::POP); e.op(Op::RETURN); }),
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
    auto withDead = [](std::string_view name, ca::u32 deadBytes) {
        return spec(name, [deadBytes](CodeEmit& e) {
            const std::size_t l = e.newLabel();
            e.branch(Op::GOTO, l);
            for (ca::u32 i = 0; i < deadBytes; ++i) {
                e.u8(0x00);   // 不可达 NOP 填充
            }
            e.bindLabel(l);
            e.op(Op::ICONST_0);
            e.op(Op::POP);
            e.op(Op::RETURN);
        });
    };
    auto tailDead = [](std::string_view name, ca::u32 deadBytes) {
        return spec(name, [deadBytes](CodeEmit& e) {
            e.op(Op::RETURN);
            for (ca::u32 i = 0; i < deadBytes; ++i) {
                e.u8(0x00);
            }
        });
    };

    const Bytes bytes = assembleClass("FpDeadCode", {
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
    auto drift = [](std::string_view name, ca::u32 filler) {
        return spec(name, [filler](CodeEmit& e) {
            const std::size_t l = e.newLabel();
            e.branch(Op::GOTO, l);
            for (ca::u32 i = 0; i < filler; ++i) {
                e.u8(0x00);   // 死填充：存活指令相对序不变，pc 全部漂移
            }
            e.bindLabel(l);
            e.op(Op::ICONST_1);
            e.op(Op::POP);
            e.op(Op::RETURN);
        });
    };
    // switch 对齐 padding 宽度不同（死填充改变 switch 落点对齐）而 token 同形。
    auto padded = [](std::string_view name, ca::u32 filler) {
        return spec(name, [filler](CodeEmit& e) {
            const std::size_t l1 = e.newLabel();
            const std::size_t l2 = e.newLabel();
            const std::size_t ld = e.newLabel();
            e.branch(Op::GOTO, l1);
            for (ca::u32 i = 0; i < filler; ++i) {
                e.u8(0x00);
            }
            e.bindLabel(l1);
            e.op(Op::ICONST_1);
            e.tableswitch(ld, 5, 6, {l2, ld});
            e.bindLabel(l2);
            e.op(Op::POP);
            e.bindLabel(ld);
            e.op(Op::RETURN);
        });
    };
    // 存活块重排：条件分支的两个都可达的返回块互换落位（真实重排，必翻）。
    // v1: fallthrough 块体 ICONST_1，分支目标块体 ICONST_0；v2 互换。
    auto blockOrder = [](std::string_view name, bool firstBlockFirst) {
        return spec(name, [firstBlockFirst](CodeEmit& e) {
            const std::size_t a = e.newLabel();
            e.op(Op::ICONST_0);
            e.branch(Op::IFEQ, a);
            // fallthrough 块（可达）。
            e.op(firstBlockFirst ? Op::ICONST_1 : Op::ICONST_0);
            e.op(Op::POP);
            e.op(Op::RETURN);
            // 分支目标块（可达）。
            e.bindLabel(a);
            e.op(firstBlockFirst ? Op::ICONST_0 : Op::ICONST_1);
            e.op(Op::POP);
            e.op(Op::RETURN);
        });
    };

    const Bytes bytes = assembleClass("FpLayout", {
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
    // 一组把 CP 解引用面全部踩到的方法：ldc 全载荷、字段/方法/接口调用、
    // 类型指令、indy、condy、switch、wide。
    std::vector<MethodSpec> methods;
    auto add = [&methods](std::string_view name, std::function<void(CodeEmit&)> emit) {
        methods.push_back(spec(name, std::move(emit)));
    };
    add("ldcI", [](CodeEmit& e) { e.ldcInteger(123456); e.op(Op::POP); e.op(Op::RETURN); });
    add("ldcS", [](CodeEmit& e) { e.ldcString("hello"); e.op(Op::POP); e.op(Op::RETURN); });
    add("ldcC", [](CodeEmit& e) { e.ldcClass("java/lang/String"); e.op(Op::POP); e.op(Op::RETURN); });
    add("ldcJ", [](CodeEmit& e) { e.ldcLong(-1); e.op(Op::POP2); e.op(Op::RETURN); });
    add("ldcMh", [](CodeEmit& e) {
        e.ldcMethodHandle(6, "FpRefs", "run", "()V");
        e.op(Op::POP);
        e.op(Op::RETURN);
    });
    add("ldcMt", [](CodeEmit& e) { e.ldcMethodType("(I)V"); e.op(Op::POP); e.op(Op::RETURN); });
    add("field", [](CodeEmit& e) { e.fieldInsn(Op::GETSTATIC, "FpRefs", "fld", "I"); e.op(Op::POP); e.op(Op::RETURN); });
    add("invoke", [](CodeEmit& e) { e.invoke(Op::INVOKESTATIC, "FpRefs", "run", "()V"); e.op(Op::RETURN); });
    add("iface", [](CodeEmit& e) { e.invokeInterface("FpIface", "go", "()V", 1); e.op(Op::RETURN); });
    add("types", [](CodeEmit& e) {
        e.typeInsn(Op::NEW, "FpRefs");
        e.op(Op::POP);
        e.typeInsn(Op::CHECKCAST, "FpIface");
        e.op(Op::POP);
        e.op(Op::RETURN);
    });
    // MULTIANEWARRAY：类名 + 维数操作数手拼。
    add("marray", [](CodeEmit& e) {
        e.op(Op::MULTIANEWARRAY);
        e.u16(e.cls("[[I"));
        e.u8(2);
        e.op(Op::POP);
        e.op(Op::RETURN);
    });
    add("switchy", [](CodeEmit& e) {
        const std::size_t ld = e.newLabel();
        const std::size_t l1 = e.newLabel();
        e.op(Op::ICONST_1);
        e.lookupswitch(ld, {{1, l1}, {9, ld}});
        e.bindLabel(l1);
        e.op(Op::POP);
        e.bindLabel(ld);
        e.op(Op::RETURN);
    });
    add("widey", [](CodeEmit& e) {
        e.varInsnW(Op::ILOAD, 300);
        e.op(Op::POP);
        e.op(Op::RETURN);
    });
    methods.push_back(spec("indy", [](CodeEmit& e) {
        e.invokedynamic(0, "apply", "()Ljava/util/function/Function;");
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));
    methods.push_back(spec("condy", [](CodeEmit& e) {
        e.ldcCondy(1, "make", "()Ljava/lang/String;");
        e.op(Op::POP);
        e.op(Op::RETURN);
    }));

    ClassBytesBuilder plain("FpReindex");
    ClassBytesBuilder shifted("FpReindex", 29);   // 29 个垃圾前缀条目
    plain.addBootstrapMethod("FpBsm", "metafactory",
                             "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                             "Ljava/lang/invoke/MethodType;)"
                             "Ljava/lang/invoke/CallSite;",
                             {BsmArg::methodType("()Ljava/util/function/Function;")});
    shifted.addBootstrapMethod("FpBsm", "metafactory",
                               "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;)"
                               "Ljava/lang/invoke/CallSite;",
                               {BsmArg::methodType("()Ljava/util/function/Function;")});
    shifted.addBootstrapMethod("FpBsm2", "stringFactory",
                               "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                               "Ljava/lang/invoke/MethodType;Ljava/lang/String;)"
                               "Ljava/lang/invoke/CallSite;",
                               {BsmArg::str("boot")});
    plain.addBootstrapMethod("FpBsm2", "stringFactory",
                             "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                             "Ljava/lang/invoke/MethodType;Ljava/lang/String;)"
                             "Ljava/lang/invoke/CallSite;",
                             {BsmArg::str("boot")});

    const Bytes base   = plain.assemble(methods);
    const Bytes junked = shifted.assemble(methods);
    for (const MethodSpec& m : methods) {
        EXPECT_EQ(fpOf(base, m.name), fpOf(junked, m.name)) << m.name;
    }
}

/// 改行号、LVT、注解、Signature、Exceptions、StackMapTable、SourceFile：
/// 排除面字节全程不读，指纹不变。
TEST(MjAnalysisSemanticFpTest, ExpectExcludedAttrsStable)
{
    auto plain = [](std::string_view name) {
        return spec(name, [](CodeEmit& e) { e.op(Op::ICONST_1); e.op(Op::POP); e.op(Op::RETURN); });
    };
    // 同体方法挂满排除面属性（载荷字节受控，结构按真实属性形态写）。
    MethodSpec decorated = plain("m");
    {
        Bytes lnt;
        fp_fix::putU16(lnt, 2);
        fp_fix::putU32(lnt, 0x00010002u);   // 两行 (start_pc, line) 打包占位
        fp_fix::putU32(lnt, 0x00030004u);
        Bytes lvt;
        fp_fix::putU16(lvt, 1);
        fp_fix::putU32(lvt, 0x00010002u);
        fp_fix::putU16(lvt, 7);
        fp_fix::putU16(lvt, 0);
        fp_fix::putU16(lvt, 1);
        Bytes annos;
        fp_fix::putU16(annos, 1);
        fp_fix::putU16(annos, 9);
        fp_fix::putU16(annos, 0);
        Bytes smt;
        fp_fix::putU16(smt, 1);
        fp_fix::putU8(smt, 65);   // same_locals_1_stack_item + delta 1
        Bytes sig;
        fp_fix::putU16(sig, 11);
        Bytes exc;
        fp_fix::putU16(exc, 1);
        fp_fix::putU16(exc, 12);
        decorated.rawAttrs = {RawAttrSpec{"LineNumberTable", lnt},
                              RawAttrSpec{"LocalVariableTable", lvt},
                              RawAttrSpec{"RuntimeVisibleAnnotations", annos},
                              RawAttrSpec{"Signature", sig},
                              RawAttrSpec{"Exceptions", exc}};
        decorated.codeAttrs = {RawAttrSpec{"StackMapTable", smt}};
    }

    const Bytes bareBytes = assembleClass("FpExcluded", {plain("m")});

    ClassBytesBuilder richBuilder("FpExcluded");
    richBuilder.setClassRawAttrs({RawAttrSpec{"SourceFile", Bytes{'S', 'x'}}});
    const Bytes richBytes = richBuilder.assemble({decorated});

    EXPECT_EQ(fpOf(bareBytes, "m"), fpOf(richBytes, "m"));
}

/// 改 opcode（iadd 换 isub）：语义变了指纹必变。
TEST(MjAnalysisSemanticFpTest, ExpectOpcodeChangeFlips)
{
    const Bytes bytes = assembleClass("FpOpcode", {
        spec("add", [](CodeEmit& e) { e.op(Op::IADD); e.op(Op::RETURN); }),
        spec("sub", [](CodeEmit& e) { e.op(Op::ISUB); e.op(Op::RETURN); }),
    });
    EXPECT_NE(fpOf(bytes, "add"), fpOf(bytes, "sub"));
}

/// 增删异常处理器：异常表参与指纹（增、删、换捕获类型都翻）。
TEST(MjAnalysisSemanticFpTest, ExpectExceptionHandlerChangeFlips)
{
    // 三个方法字节完全同形；差异只在异常表。标签序固定 start=0/end=1/h=2。
    auto tryBody = [](CodeEmit& e) {
        const std::size_t start = e.newLabel();
        const std::size_t end   = e.newLabel();
        const std::size_t h     = e.newLabel();
        e.bindLabel(start);
        e.op(Op::NOP);
        e.bindLabel(end);
        e.op(Op::RETURN);
        e.bindLabel(h);
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    MethodSpec bare = spec("bare", tryBody);
    MethodSpec withCatch = spec("withCatch", tryBody);
    withCatch.exceptions = {ExcSpec{0, 1, 2, "java/lang/Exception"}};
    MethodSpec withFinally = spec("withFinally", tryBody);
    withFinally.exceptions = {ExcSpec{0, 1, 2, ""}};
    MethodSpec withOther = spec("withOther", tryBody);
    withOther.exceptions = {ExcSpec{0, 1, 2, "java/lang/Throwable"}};

    const Bytes bytes = assembleClass("FpExc", {bare, withCatch, withFinally, withOther});

    EXPECT_NE(fpOf(bytes, "bare"), fpOf(bytes, "withCatch"));      // 增处理器
    EXPECT_NE(fpOf(bytes, "withCatch"), fpOf(bytes, "withFinally"));   // catchType 有无
    EXPECT_NE(fpOf(bytes, "withCatch"), fpOf(bytes, "withOther")); // 捕获类型改
}

/// 改访问标志：方法头参与指纹（原始 u16 记录）。
TEST(MjAnalysisSemanticFpTest, ExpectAccessFlagChangeFlips)
{
    const Bytes bytes = assembleClass("FpAccess", {
        spec("a1", [](CodeEmit& e) { e.op(Op::RETURN); }, 0x0001),
        spec("a2", [](CodeEmit& e) { e.op(Op::RETURN); }, 0x0009),
        spec("a3", [](CodeEmit& e) { e.op(Op::RETURN); }, 0x0001 | 0x1000),   // SYNTHETIC
        spec("a4", [](CodeEmit& e) { e.op(Op::RETURN); }, 0x0009 | 0x0040),   // BRIDGE
        spec("a5", [](CodeEmit& e) { e.op(Op::RETURN); }, 0x0009 | 0x0080),   // VARARGS
    });
    EXPECT_NE(fpOf(bytes, "a1"), fpOf(bytes, "a2"));
    EXPECT_NE(fpOf(bytes, "a1"), fpOf(bytes, "a3"));
    EXPECT_NE(fpOf(bytes, "a2"), fpOf(bytes, "a4"));
    EXPECT_NE(fpOf(bytes, "a2"), fpOf(bytes, "a5"));
}

/// 改 indy bootstrap 参数：解引用面参与指纹。
TEST(MjAnalysisSemanticFpTest, ExpectIndyBsmArgsChangeFlips)
{
    ClassBytesBuilder b("FpIndy");
    const ca::u16 bsmTypeI = b.addBootstrapMethod(
        "FpBsm", "metafactory",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;",
        {BsmArg::methodType("(I)V")});
    const ca::u16 bsmTypeJ = b.addBootstrapMethod(
        "FpBsm", "metafactory",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;",
        {BsmArg::methodType("(J)V")});
    const ca::u16 bsmStrA = b.addBootstrapMethod(
        "FpBsm", "stringFactory",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;Ljava/lang/String;)Ljava/lang/invoke/CallSite;",
        {BsmArg::str("alpha")});
    const ca::u16 bsmStrB = b.addBootstrapMethod(
        "FpBsm", "stringFactory",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;Ljava/lang/String;)Ljava/lang/invoke/CallSite;",
        {BsmArg::str("beta")});
    const ca::u16 bsmH1 = b.addBootstrapMethod(
        "FpBsm", "handleFactory",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;)"
        "Ljava/lang/invoke/CallSite;",
        {BsmArg::handle(6, "FpRefs", "run", "()V")});
    const ca::u16 bsmH2 = b.addBootstrapMethod(
        "FpBsm", "handleFactory",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;)"
        "Ljava/lang/invoke/CallSite;",
        {BsmArg::handle(6, "FpRefs", "run2", "()V")});
    const ca::u16 bsmMulti1 = b.addBootstrapMethod(
        "FpBsm", "multi",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;",
        {BsmArg::integer(1), BsmArg::cls("FpRefs"), BsmArg::str("x")});
    const ca::u16 bsmMulti2 = b.addBootstrapMethod(
        "FpBsm", "multi",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;",
        {BsmArg::integer(2), BsmArg::cls("FpRefs"), BsmArg::str("x")});

    const Bytes bytes = b.assemble({
        spec("mt1", [bsmTypeI](CodeEmit& e) {
            e.invokedynamic(bsmTypeI, "foo", "(I)Ljava/lang/invoke/MethodHandle;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("mt2", [bsmTypeJ](CodeEmit& e) {
            e.invokedynamic(bsmTypeJ, "foo", "(I)Ljava/lang/invoke/MethodHandle;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("str1", [bsmStrA](CodeEmit& e) {
            e.invokedynamic(bsmStrA, "foo", "()Ljava/lang/String;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("str2", [bsmStrB](CodeEmit& e) {
            e.invokedynamic(bsmStrB, "foo", "()Ljava/lang/String;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("name1", [bsmStrA](CodeEmit& e) {
            e.invokedynamic(bsmStrA, "foo", "()Ljava/lang/String;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("name2", [bsmStrA](CodeEmit& e) {
            e.invokedynamic(bsmStrA, "goo", "()Ljava/lang/String;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("h1", [bsmH1](CodeEmit& e) {
            e.invokedynamic(bsmH1, "foo", "()Ljava/lang/invoke/MethodHandle;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("h2", [bsmH2](CodeEmit& e) {
            e.invokedynamic(bsmH2, "foo", "()Ljava/lang/invoke/MethodHandle;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("multi1", [bsmMulti1](CodeEmit& e) {
            e.invokedynamic(bsmMulti1, "foo", "()Ljava/lang/Object;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("multi2", [bsmMulti2](CodeEmit& e) {
            e.invokedynamic(bsmMulti2, "foo", "()Ljava/lang/Object;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
    });

    EXPECT_NE(fpOf(bytes, "mt1"), fpOf(bytes, "mt2"));       // MethodType 实参变
    EXPECT_NE(fpOf(bytes, "str1"), fpOf(bytes, "str2"));     // String 实参变
    EXPECT_NE(fpOf(bytes, "name1"), fpOf(bytes, "name2"));   // indy 自身名变
    EXPECT_NE(fpOf(bytes, "h1"), fpOf(bytes, "h2"));         // MethodHandle 实参变
    EXPECT_NE(fpOf(bytes, "multi1"), fpOf(bytes, "multi2")); // 多实参其一变
}

/// 被引用的类/方法改名：出口语义就是名字，必翻。
TEST(MjAnalysisSemanticFpTest, ExpectReferencedNameChangeFlips)
{
    const Bytes bytes = assembleClass("FpRefRename", {
        spec("m1", [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "FpRefs", "run", "()V");
            e.op(Op::RETURN);
        }),
        spec("m2", [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "FpRefs", "run2", "()V");
            e.op(Op::RETURN);
        }),
        spec("m3", [](CodeEmit& e) {
            e.invoke(Op::INVOKESTATIC, "FpOther", "run", "()V");
            e.op(Op::RETURN);
        }),
        spec("f1", [](CodeEmit& e) {
            e.fieldInsn(Op::GETSTATIC, "FpRefs", "fld", "I");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("f2", [](CodeEmit& e) {
            e.fieldInsn(Op::GETSTATIC, "FpRefs", "fld2", "I");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("t1", [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "FpRefs");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("t2", [](CodeEmit& e) {
            e.typeInsn(Op::NEW, "FpRenamed");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
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
TEST(MjAnalysisSemanticFpTest, SelfRenameStable)
{
    const Bytes a = assembleClass("FpSelfNameA", {spec("plainName", [](CodeEmit& e) {
        e.ldcString("body");
        e.op(Op::POP);
        e.op(Op::RETURN);
    })});

    ClassBytesBuilder b("FpSelfNameB");
    MethodSpec m = spec("plainName", [](CodeEmit& e) {
        e.ldcString("body");
        e.op(Op::POP);
        e.op(Op::RETURN);
    });
    m.name = kSpaceName;
    const Bytes sp = b.assemble({m});

    EXPECT_EQ(fpOf(a, "plainName"), fpOf(sp, kSpaceName));
}

/// 被引用名字段改名翻引用者（空格名字段名同样参与）。
TEST(MjAnalysisSemanticFpTest, ReferencedFieldRenameFlips)
{
    const std::string spaceField(21845, ' ');
    ClassBytesBuilder b("FpFieldRename");
    const Bytes bytes = b.assemble({
        spec("r1", [](CodeEmit& e) {
            e.fieldInsn(Op::GETSTATIC, "FpRefs", "orig", "I");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("r2", [&spaceField](CodeEmit& e) {
            e.fieldInsn(Op::GETSTATIC, "FpRefs", spaceField, "I");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
    });
    EXPECT_NE(fpOf(bytes, "r1"), fpOf(bytes, "r2"));
}

/// native/abstract 没有方法体：指纹只盖方法头（版本 + 访问标志 + 描述符），
/// 永远恒定；方法名/类名/Exceptions 不参与。
TEST(MjAnalysisSemanticFpTest, NativeAbstractHeaderOnlyStable)
{
    MethodSpec abstractNoExc;
    abstractNoExc.name   = "calc";
    abstractNoExc.access = 0x0441;   // public abstract
    abstractNoExc.desc   = "(I)J";
    MethodSpec abstractRenamed = abstractNoExc;
    abstractRenamed.name       = "calc0";
    MethodSpec abstractNoPublic = abstractNoExc;
    abstractNoPublic.name       = "calc1";
    abstractNoPublic.access     = 0x0401;   // abstract（去 public）
    MethodSpec abstractWithExc = abstractNoExc;
    abstractWithExc.rawAttrs   = {RawAttrSpec{"Exceptions", Bytes{0x00, 0x01, 0x00, 0x63}}};

    ClassBytesBuilder a("FpNativeA");
    const Bytes aBytes = a.assemble({abstractNoExc, abstractRenamed, abstractNoPublic});
    ClassBytesBuilder b("FpNativeB");
    b.setClassRawAttrs({RawAttrSpec{"SourceFile", Bytes{'N', 'a'}}});
    const Bytes bBytes = b.assemble({abstractWithExc});

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
TEST(MjAnalysisSemanticFpTest, EdgeExcEndpointMapping)
{
    // 体形：GOTO L; NOP; NOP(dead); L: NOP; RETURN; H: POP; RETURN。
    // 标签序：l=0, h=1。code 长度 9；存活 pcs = {0, 5, 6, 7, 8}。
    auto body = [](CodeEmit& e) {
        const std::size_t l = e.newLabel();
        const std::size_t h = e.newLabel();
        e.branch(Op::GOTO, l);
        e.u8(0x00);
        e.u8(0x00);
        e.bindLabel(l);
        e.op(Op::NOP);
        e.op(Op::RETURN);
        e.bindLabel(h);
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    auto mk = [&](std::string_view name, ca::u16 start, ca::u16 end) {
        MethodSpec m = spec(name, body);
        // start/end 走原始 pc（端点映射语义的测试面），handler 走标签。
        m.exceptions = {ExcSpec{0, 0, 0, "java/lang/Exception", true}};
        m.exceptions[0].start   = start;
        m.exceptions[0].end     = end;
        m.exceptions[0].handler = 7;   // H: POP 的 pc
        return m;
    };
    const Bytes bytes = assembleClass("FpExcEdge", {
        mk("v1", 0, 3),    // end=死区首 pc → 首条存活 ≥3 = L（序 1）
        mk("v2", 0, 1),    // end=GOTO 后 → 首条存活 ≥1 = L（同 v1）
        mk("v3", 0, 9),    // end 越过末尾（code 长度 9）→ 存活总数
        mk("v4", 0, 200),  // end 远超末尾 → 存活总数（同 v3）
    });
    EXPECT_EQ(fpOf(bytes, "v1"), fpOf(bytes, "v2"));
    EXPECT_EQ(fpOf(bytes, "v3"), fpOf(bytes, "v4"));
    EXPECT_NE(fpOf(bytes, "v1"), fpOf(bytes, "v3"));

    // handler 指向指令中部（GOTO 操作数内 pc=1）→ 拒绝，不做静默近似。
    // 注：指向死指令的 handler 合法——处理器入口按可达根处理，直接救活。
    MethodSpec liveDeadHandler = spec("liveDeadHandler", body);
    liveDeadHandler.exceptions = {ExcSpec{0, 3, 3, "java/lang/Exception", true}};
    const Bytes liveDeadBytes = assembleClass("FpExcEdgeDeadLive", {liveDeadHandler});
    EXPECT_EQ(fpErrOf(liveDeadBytes, "liveDeadHandler"), SemanticFpError::Ok);

    MethodSpec badHandler = spec("badHandler", body);
    badHandler.exceptions = {ExcSpec{0, 1, 1, "java/lang/Exception", true}};
    const Bytes badBytes = assembleClass("FpExcEdgeBad", {badHandler});
    EXPECT_EQ(fpErrOf(badBytes, "badHandler"), SemanticFpError::BadExcEntry);
}

/// 边界 1 补：catchType 0（any/finally）合法且记空串；表项顺序原样保留。
TEST(MjAnalysisSemanticFpTest, EdgeExcCatchTypeAndOrder)
{
    // 双处理器体：NOP; RETURN; H1: POP; RETURN; H2: ACONST_NULL; POP; RETURN。
    auto twoHandlerBody = [](CodeEmit& e) {
        const std::size_t start = e.newLabel();
        const std::size_t end   = e.newLabel();
        const std::size_t h1    = e.newLabel();
        const std::size_t h2    = e.newLabel();
        e.bindLabel(start);
        e.op(Op::NOP);
        e.bindLabel(end);
        e.op(Op::RETURN);
        e.bindLabel(h1);
        e.op(Op::POP);
        e.op(Op::RETURN);
        e.bindLabel(h2);
        e.op(Op::ACONST_NULL);
        e.op(Op::POP);
        e.op(Op::RETURN);
    };
    MethodSpec anyFirst = spec("anyFirst", twoHandlerBody);
    anyFirst.exceptions = {ExcSpec{0, 1, 2, ""},
                           ExcSpec{0, 1, 3, "java/lang/Throwable"}};
    MethodSpec typedFirst = spec("typedFirst", twoHandlerBody);
    typedFirst.exceptions = {ExcSpec{0, 1, 3, "java/lang/Throwable"},
                             ExcSpec{0, 1, 2, ""}};

    const Bytes bytes = assembleClass("FpExcOrder", {anyFirst, typedFirst});
    EXPECT_NE(fpOf(bytes, "anyFirst"), fpOf(bytes, "typedFirst"));
}

/// 边界 2：方法头不含所属类名与 Exceptions——同类签名同体方法跨类恒等，
/// Exceptions/Signature/注解/MethodParameters 全程不读不记。
TEST(MjAnalysisSemanticFpTest, EdgeHeaderExcludesClassAndExceptions)
{
    auto body = [](CodeEmit& e) {
        e.invoke(Op::INVOKESTATIC, "FpRefs", "run", "()V");
        e.op(Op::RETURN);
    };
    MethodSpec withExc = spec("run", body);
    withExc.rawAttrs   = {RawAttrSpec{"Exceptions", Bytes{0x00, 0x01, 0x00, 0x63}},
                        RawAttrSpec{"Signature", Bytes{0x00, 0x09}}};

    const Bytes a = assembleClass("FpClsA", {spec("run", body)});
    const Bytes b = assembleClass("FpClsB", {withExc});
    EXPECT_EQ(fpOf(a, "run"), fpOf(b, "run"));
}

/// 边界 3：非 int 常量不折叠——FCONST_1 与 ldc float 同值不同指；
/// lconst/fconst/dconst 记 plain opcode，ldc 家族记原始位型。
TEST(MjAnalysisSemanticFpTest, EdgeNonIntConstNotFolded)
{
    const Bytes bytes = assembleClass("FpNonInt", {
        spec("fc1", [](CodeEmit& e) { e.op(Op::FCONST_1); e.op(Op::RETURN); }),
        spec("fLdc", [](CodeEmit& e) {
            e.ldcFloatBits(0x3F800000u);   // float 1.0
            e.op(Op::RETURN);
        }),
        spec("fLdc2", [](CodeEmit& e) {
            e.ldcFloatBits(0x40000000u);   // float 2.0
            e.op(Op::RETURN);
        }),
        spec("lc1", [](CodeEmit& e) { e.op(Op::LCONST_1); e.op(Op::RETURN); }),
        spec("lLdc", [](CodeEmit& e) { e.ldcLong(1); e.op(Op::RETURN); }),
        spec("dc1", [](CodeEmit& e) { e.op(Op::DCONST_1); e.op(Op::RETURN); }),
        spec("dLdc", [](CodeEmit& e) {
            e.ldcDoubleBits(0x3FF0000000000000ull);   // double 1.0
            e.op(Op::RETURN);
        }),
        spec("fc0", [](CodeEmit& e) { e.op(Op::FCONST_0); e.op(Op::RETURN); }),
    });
    EXPECT_NE(fpOf(bytes, "fc1"), fpOf(bytes, "fLdc"));    // 同值不同编码：翻
    EXPECT_NE(fpOf(bytes, "fLdc"), fpOf(bytes, "fLdc2"));  // 位型不同：翻
    EXPECT_NE(fpOf(bytes, "lc1"), fpOf(bytes, "lLdc"));
    EXPECT_NE(fpOf(bytes, "dc1"), fpOf(bytes, "dLdc"));
    EXPECT_NE(fpOf(bytes, "fc0"), fpOf(bytes, "fc1"));     // 不同 opcode：翻
}

/// 边界 4：ldc 全载荷解到语义值——ldc/ldc_w 的 opcode 不进流（同载荷同
/// token）；MethodHandle 解两层到（kind，类，名，描述符）；MethodType 解为
/// 描述符；Class 解为类名。
TEST(MjAnalysisSemanticFpTest, EdgeLdcFullDeref)
{
    const Bytes bytes = assembleClass("FpLdcDeref", {
        spec("mhLdc", [](CodeEmit& e) {
            e.ldcMethodHandle(6, "FpRefs", "run", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("mhLdcW", [](CodeEmit& e) {
            e.ldcWCp(e.methodHandleCp(6, "FpRefs", "run", "()V"));
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("mhLdc2", [](CodeEmit& e) {
            e.ldcMethodHandle(6, "FpRefs", "run2", "()V");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("mt1", [](CodeEmit& e) { e.ldcMethodType("(I)V"); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("mt2", [](CodeEmit& e) { e.ldcMethodType("(J)V"); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("cls1", [](CodeEmit& e) { e.ldcClass("FpRefs"); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("cls2", [](CodeEmit& e) { e.ldcClass("FpOther"); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("s1", [](CodeEmit& e) { e.ldcString("alpha"); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("s2", [](CodeEmit& e) { e.ldcString("beta"); e.op(Op::POP); e.op(Op::RETURN); }),
    });
    // ldc 与 ldc_w 同载荷同 token（opcode 不进流）。
    EXPECT_EQ(fpOf(bytes, "mhLdc"), fpOf(bytes, "mhLdcW"));
    // MethodHandle 两层解引用：名字变即翻（名字在 NAT，两层跳转之外）。
    EXPECT_NE(fpOf(bytes, "mhLdc"), fpOf(bytes, "mhLdc2"));
    EXPECT_NE(fpOf(bytes, "mt1"), fpOf(bytes, "mt2"));
    EXPECT_NE(fpOf(bytes, "cls1"), fpOf(bytes, "cls2"));
    EXPECT_NE(fpOf(bytes, "s1"), fpOf(bytes, "s2"));
}

/// 边界 4 补：condy 按 indy 同款记（名、描述符、BSM 句柄、全部静态参数），
/// 静态参数逐个类型化解引用。
TEST(MjAnalysisSemanticFpTest, EdgeCondyFullDeref)
{
    ClassBytesBuilder b("FpCondy");
    const ca::u16 bsmA = b.addBootstrapMethod(
        "FpBsm", "stringFactory",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;Ljava/lang/String;)Ljava/lang/invoke/CallSite;",
        {BsmArg::str("alpha")});
    const ca::u16 bsmB = b.addBootstrapMethod(
        "FpBsm", "stringFactory",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;Ljava/lang/String;)Ljava/lang/invoke/CallSite;",
        {BsmArg::str("beta")});
    const Bytes bytes = b.assemble({
        spec("c1", [bsmA](CodeEmit& e) {
            e.ldcCondy(bsmA, "make", "()Ljava/lang/String;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("c2", [bsmB](CodeEmit& e) {
            e.ldcCondy(bsmB, "make", "()Ljava/lang/String;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("c3", [bsmA](CodeEmit& e) {
            e.ldcCondy(bsmA, "make2", "()Ljava/lang/String;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
    });
    EXPECT_NE(fpOf(bytes, "c1"), fpOf(bytes, "c2"));   // 静态参数变
    EXPECT_NE(fpOf(bytes, "c1"), fpOf(bytes, "c3"));   // condy 自身名变
}

/// 边界 5：访问标志不剔除编译器旗标——BRIDGE/SYNTHETIC/VARARGS 原样参与。
TEST(MjAnalysisSemanticFpTest, EdgeAccessFlagsKeepCompilerBits)
{
    const Bytes bytes = assembleClass("FpCompilerBits", {
        spec("plain", [](CodeEmit& e) { e.op(Op::RETURN); }, 0x0001),
        spec("synthetic", [](CodeEmit& e) { e.op(Op::RETURN); }, 0x1001),
        spec("bridge", [](CodeEmit& e) { e.op(Op::RETURN); }, 0x0041),
        spec("varargs", [](CodeEmit& e) { e.op(Op::RETURN); }, 0x0081),
    });
    EXPECT_NE(fpOf(bytes, "plain"), fpOf(bytes, "synthetic"));
    EXPECT_NE(fpOf(bytes, "plain"), fpOf(bytes, "bridge"));
    EXPECT_NE(fpOf(bytes, "plain"), fpOf(bytes, "varargs"));
}

/// 边界 6：wide 前缀不单记——wide 与非 wide 同语义同 token；槽位与增量照
/// 常记实际值。
TEST(MjAnalysisSemanticFpTest, EdgeWideNotRecorded)
{
    const Bytes bytes = assembleClass("FpWide", {
        spec("loadN", [](CodeEmit& e) { e.varInsn(Op::ILOAD, 1); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("loadW", [](CodeEmit& e) { e.varInsnW(Op::ILOAD, 1); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("iincN", [](CodeEmit& e) { e.iincInsn(3, 1); e.op(Op::RETURN); }),
        spec("iincW", [](CodeEmit& e) { e.iincInsnW(3, 1); e.op(Op::RETURN); }),
        spec("big300", [](CodeEmit& e) { e.varInsnW(Op::ILOAD, 300); e.op(Op::POP); e.op(Op::RETURN); }),
        spec("big301", [](CodeEmit& e) { e.varInsnW(Op::ILOAD, 301); e.op(Op::POP); e.op(Op::RETURN); }),
    });
    EXPECT_EQ(fpOf(bytes, "loadN"), fpOf(bytes, "loadW"));
    EXPECT_EQ(fpOf(bytes, "iincN"), fpOf(bytes, "iincW"));
    EXPECT_NE(fpOf(bytes, "big300"), fpOf(bytes, "big301"));
}

/// 边界 7：BSM 参数解一整层——Class/MethodType/MethodHandle 展开到语义值；
/// 参数位再嵌 condy/indy 直接报错，不递归。
TEST(MjAnalysisSemanticFpTest, EdgeBsmOneLevelDerefNestedErrors)
{
    ClassBytesBuilder b("FpBsmNested");
    const ca::u16 okBsm = b.addBootstrapMethod(
        "FpBsm", "rich",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;",
        {BsmArg::cls("FpRefs"), BsmArg::methodType("(I)V"),
         BsmArg::handle(6, "FpRefs", "run", "()V"), BsmArg::integer(7),
         BsmArg::longVal(1), BsmArg::floatBits(0x3F800000u),
         BsmArg::doubleBits(0x3FF0000000000000ull), BsmArg::str("x")});
    const ca::u16 condyBsm = b.addBootstrapMethod(
        "FpBsm", "nested",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;",
        {BsmArg::condy(0, "c", "I")});
    const ca::u16 indyBsm = b.addBootstrapMethod(
        "FpBsm", "nested2",
        "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
        "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;",
        {BsmArg::indy(0, "x", "()V")});

    const Bytes bytes = b.assemble({
        spec("ok", [okBsm](CodeEmit& e) {
            e.invokedynamic(okBsm, "foo", "()Ljava/lang/Object;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("nestedCondy", [condyBsm](CodeEmit& e) {
            e.invokedynamic(condyBsm, "foo", "()Ljava/lang/Object;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
        spec("nestedIndy", [indyBsm](CodeEmit& e) {
            e.invokedynamic(indyBsm, "foo", "()Ljava/lang/Object;");
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
    });
    EXPECT_EQ(fpErrOf(bytes, "ok"), SemanticFpError::Ok);
    EXPECT_EQ(fpErrOf(bytes, "nestedCondy"), SemanticFpError::NestedDynamic);
    EXPECT_EQ(fpErrOf(bytes, "nestedIndy"), SemanticFpError::NestedDynamic);
}

/// 边界 8：switch key 原样进指纹——tableswitch 按 low 重推连续 key 全列，
/// lookupswitch 逐 case 记原始 key；default 先记、顺序与 class 文件一致。
TEST(MjAnalysisSemanticFpTest, EdgeSwitchKeysExact)
{
    const Bytes bytes = assembleClass("FpSwitch", {
        // 四路同形 switch（default + 两目标），差异只在 key 面。
        spec("ts56", [](CodeEmit& e) {
            const std::size_t ld = e.newLabel();
            const std::size_t a = e.newLabel();
            const std::size_t b = e.newLabel();
            e.op(Op::ICONST_1);
            e.tableswitch(ld, 5, 6, {a, b});
            e.bindLabel(a);
            e.op(Op::POP);
            e.bindLabel(b);
            e.op(Op::POP);
            e.bindLabel(ld);
            e.op(Op::RETURN);
        }),
        spec("ts67", [](CodeEmit& e) {
            const std::size_t ld = e.newLabel();
            const std::size_t a = e.newLabel();
            const std::size_t b = e.newLabel();
            e.op(Op::ICONST_1);
            e.tableswitch(ld, 6, 7, {a, b});
            e.bindLabel(a);
            e.op(Op::POP);
            e.bindLabel(b);
            e.op(Op::POP);
            e.bindLabel(ld);
            e.op(Op::RETURN);
        }),
        spec("ls56", [](CodeEmit& e) {
            const std::size_t ld = e.newLabel();
            const std::size_t a = e.newLabel();
            const std::size_t b = e.newLabel();
            e.op(Op::ICONST_1);
            e.lookupswitch(ld, {{5, a}, {6, b}});
            e.bindLabel(a);
            e.op(Op::POP);
            e.bindLabel(b);
            e.op(Op::POP);
            e.bindLabel(ld);
            e.op(Op::RETURN);
        }),
        spec("ls57", [](CodeEmit& e) {
            const std::size_t ld = e.newLabel();
            const std::size_t a = e.newLabel();
            const std::size_t b = e.newLabel();
            e.op(Op::ICONST_1);
            e.lookupswitch(ld, {{5, a}, {7, b}});
            e.bindLabel(a);
            e.op(Op::POP);
            e.bindLabel(b);
            e.op(Op::POP);
            e.bindLabel(ld);
            e.op(Op::RETURN);
        }),
        spec("lsSwap", [](CodeEmit& e) {
            const std::size_t ld = e.newLabel();
            const std::size_t a = e.newLabel();
            const std::size_t b = e.newLabel();
            e.op(Op::ICONST_1);
            e.lookupswitch(ld, {{6, b}, {5, a}});
            e.bindLabel(a);
            e.op(Op::POP);
            e.bindLabel(b);
            e.op(Op::POP);
            e.bindLabel(ld);
            e.op(Op::RETURN);
        }),
    });
    EXPECT_NE(fpOf(bytes, "ts56"), fpOf(bytes, "ts67"));    // key 全列平移
    EXPECT_NE(fpOf(bytes, "ls56"), fpOf(bytes, "ls57"));    // 原始 key 变
    EXPECT_NE(fpOf(bytes, "ls56"), fpOf(bytes, "lsSwap"));  // case 顺序原样保留
    EXPECT_NE(fpOf(bytes, "ts56"), fpOf(bytes, "ls56"));    // opcode 精确记录
}

/// switch 目标按规范序号记录：目标前的死填充漂移 pc 不翻。
TEST(MjAnalysisSemanticFpTest, EdgeSwitchTargetsCanonical)
{
    auto withFiller = [](std::string_view name, ca::u32 filler) {
        return spec(name, [filler](CodeEmit& e) {
            const std::size_t l1 = e.newLabel();
            const std::size_t l2 = e.newLabel();
            const std::size_t ld = e.newLabel();
            e.branch(Op::GOTO, l1);
            for (ca::u32 i = 0; i < filler; ++i) {
                e.u8(0x00);
            }
            e.bindLabel(l1);
            e.op(Op::ICONST_1);
            e.lookupswitch(ld, {{1, l2}, {9, ld}});
            e.bindLabel(l2);
            e.op(Op::POP);
            e.bindLabel(ld);
            e.op(Op::RETURN);
        });
    };
    const Bytes bytes = assembleClass("FpSwitchCanon", {
        withFiller("w0", 0), withFiller("w5", 5),
    });
    EXPECT_EQ(fpOf(bytes, "w0"), fpOf(bytes, "w5"));
}

/// invokeinterface 的 count/0 字节可由描述符导出，不进指纹。
TEST(MjAnalysisSemanticFpTest, EdgeInvokeInterfaceCountNotRecorded)
{
    const Bytes bytes = assembleClass("FpIfaceCount", {
        spec("c1", [](CodeEmit& e) {
            e.invokeInterface("FpIface", "go", "()V", 1);
            e.op(Op::RETURN);
        }),
        spec("c3", [](CodeEmit& e) {
            e.invokeInterface("FpIface", "go", "()V", 3);
            e.op(Op::RETURN);
        }),
    });
    EXPECT_EQ(fpOf(bytes, "c1"), fpOf(bytes, "c3"));
}

/// 分支目标落在指令中部：拒绝（BadBranchTarget），不静默近似。
TEST(MjAnalysisSemanticFpTest, EdgeMidInstructionTargetRejected)
{
    const Bytes bytes = assembleClass("FpMidInsn", {
        spec("mid", [](CodeEmit& e) {
            e.op(Op::GOTO);
            e.u16(2);   // 目标 pc=2：下一条 SIPUSH（pc1 起 3 字节）的中部
            e.op(Op::SIPUSH);
            e.u16(0);
            e.op(Op::POP);
            e.op(Op::RETURN);
        }),
    });
    EXPECT_EQ(fpErrOf(bytes, "mid"), SemanticFpError::BadBranchTarget);
}

// ============================================================================
// 确定性与唯一性
// ============================================================================

/// 同一方法双算一致（重解析、重新构建均一致）。
TEST(MjAnalysisSemanticFpTest, DeterminismDoubleCompute)
{
    std::vector<MethodSpec> methods;
    auto add = [&methods](std::string_view name, std::function<void(CodeEmit&)> emit) {
        methods.push_back(spec(name, std::move(emit)));
    };
    add("indy", [](CodeEmit& e) {
        e.invokedynamic(0, "apply", "()Ljava/util/function/Function;");
        e.op(Op::POP);
        e.op(Op::RETURN);
    });
    add("switchy", [](CodeEmit& e) {
        const std::size_t ld = e.newLabel();
        const std::size_t l1 = e.newLabel();
        e.op(Op::ICONST_1);
        e.tableswitch(ld, 1, 3, {l1, ld, l1});
        e.bindLabel(l1);
        e.op(Op::POP);
        e.bindLabel(ld);
        e.op(Op::RETURN);
    });
    add("widey", [](CodeEmit& e) {
        e.varInsnW(Op::ILOAD, 300);
        e.iincInsnW(300, -5);
        e.op(Op::POP);
        e.op(Op::RETURN);
    });

    ClassBytesBuilder b("FpDeterminism");
    b.addBootstrapMethod("FpBsm", "metafactory",
                         "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                         "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;",
                         {BsmArg::methodType("()Ljava/util/function/Function;")});
    const Bytes bytes = b.assemble(methods);

    for (const MethodSpec& m : methods) {
        EXPECT_EQ(fpOf(bytes, m.name), fpOf(bytes, m.name)) << m.name;
    }
    // 拷贝一份字节重新解析（fresh DOM），指纹仍一致。
    const Bytes copy = bytes;
    for (const MethodSpec& m : methods) {
        EXPECT_EQ(fpOf(bytes, m.name), fpOf(copy, m.name)) << m.name;
    }
}

/// 唯一性：合成语料去重统计——同体组内恒等、组间与唯一体全不同、零碰撞。
TEST(MjAnalysisSemanticFpTest, CorpusDedupStats)
{
    std::vector<MethodSpec> methods;
    ca::usize uniqueCount = 0;
    auto uniq = [&](std::string_view name, std::function<void(CodeEmit&)> emit) {
        methods.push_back(spec(name, std::move(emit)));
        ++uniqueCount;
    };

    uniq("u01", [](CodeEmit& e) { e.op(Op::RETURN); });
    uniq("u02", [](CodeEmit& e) { e.i32const(1); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u03", [](CodeEmit& e) { e.i32const(2); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u04", [](CodeEmit& e) { e.i32const(77); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u05", [](CodeEmit& e) { e.op(Op::ACONST_NULL); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u06", [](CodeEmit& e) { e.typeInsn(Op::NEW, "FpA"); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u07", [](CodeEmit& e) { e.typeInsn(Op::NEW, "FpB"); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u08", [](CodeEmit& e) { e.fieldInsn(Op::GETSTATIC, "FpA", "a", "I"); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u09", [](CodeEmit& e) { e.fieldInsn(Op::GETSTATIC, "FpA", "b", "I"); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u10", [](CodeEmit& e) { e.invoke(Op::INVOKESTATIC, "FpA", "m", "()V"); e.op(Op::RETURN); });
    uniq("u11", [](CodeEmit& e) { e.invoke(Op::INVOKESTATIC, "FpA", "n", "()V"); e.op(Op::RETURN); });
    uniq("u12", [](CodeEmit& e) { e.ldcString("s1"); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u13", [](CodeEmit& e) { e.ldcString("s2"); e.op(Op::POP); e.op(Op::RETURN); });
    uniq("u14", [](CodeEmit& e) { e.op(Op::IADD); e.op(Op::RETURN); });
    uniq("u15", [](CodeEmit& e) { e.op(Op::ISUB); e.op(Op::RETURN); });
    uniq("u16", [](CodeEmit& e) {
        const std::size_t l = e.newLabel();
        e.op(Op::ICONST_0);
        e.branch(Op::IFEQ, l);
        e.op(Op::POP);
        e.bindLabel(l);
        e.op(Op::NOP);
        e.op(Op::RETURN);
    });
    uniq("u17", [](CodeEmit& e) {
        const std::size_t l = e.newLabel();
        e.op(Op::ICONST_0);
        e.branch(Op::IFNE, l);
        e.op(Op::POP);
        e.bindLabel(l);
        e.op(Op::NOP);
        e.op(Op::RETURN);
    });
    uniq("u18", [](CodeEmit& e) {
        const std::size_t ld = e.newLabel();
        const std::size_t a = e.newLabel();
        const std::size_t b = e.newLabel();
        const std::size_t c = e.newLabel();
        e.tableswitch(ld, 1, 3, {a, b, c});
        e.bindLabel(a);
        e.op(Op::RETURN);
        e.bindLabel(b);
        e.op(Op::RETURN);
        e.bindLabel(c);
        e.op(Op::RETURN);
        e.bindLabel(ld);
        e.op(Op::RETURN);
    });
    uniq("u19", [](CodeEmit& e) {
        const std::size_t ld = e.newLabel();
        const std::size_t a = e.newLabel();
        e.lookupswitch(ld, {{1, a}, {9, ld}});
        e.bindLabel(a);
        e.op(Op::RETURN);
        e.bindLabel(ld);
        e.op(Op::RETURN);
    });
    uniq("u20", [](CodeEmit& e) {
        const std::size_t ld = e.newLabel();
        const std::size_t a = e.newLabel();
        e.lookupswitch(ld, {{1, a}, {10, ld}});
        e.bindLabel(a);
        e.op(Op::RETURN);
        e.bindLabel(ld);
        e.op(Op::RETURN);
    });
    uniq("u21", [](CodeEmit& e) {
        e.varInsnW(Op::ILOAD, 300);
        e.op(Op::POP);
        e.op(Op::RETURN);
    });
    uniq("u22", [](CodeEmit& e) {
        e.iincInsnW(300, 5);
        e.op(Op::RETURN);
    });
    uniq("u23", [](CodeEmit& e) {
        e.invokedynamic(0, "apply", "()Ljava/util/function/Function;");
        e.op(Op::POP);
        e.op(Op::RETURN);
    });
    uniq("u24", [](CodeEmit& e) {
        e.ldcMethodHandle(6, "FpA", "m", "()V");
        e.op(Op::POP);
        e.op(Op::RETURN);
    });
    uniq("u25", [](CodeEmit& e) {
        e.invokeInterface("FpIface", "go", "()V", 1);
        e.op(Op::RETURN);
    });
    uniq("u26", [](CodeEmit& e) {
        e.op(Op::MULTIANEWARRAY);
        e.u16(e.cls("[[I"));
        e.u8(2);
        e.op(Op::POP);
        e.op(Op::RETURN);
    });

    // 同体组：组内同名体异名方法恒等。
    const auto groupBody1 = [](CodeEmit& e) { e.op(Op::DUP); e.op(Op::POP); e.op(Op::RETURN); };
    const auto groupBody2 = [](CodeEmit& e) { e.iincInsn(0, 1); e.op(Op::RETURN); };
    methods.push_back(spec("g1a", groupBody1));
    methods.push_back(spec("g1b", groupBody1));
    methods.push_back(spec("g1c", groupBody1));
    methods.push_back(spec("g2a", groupBody2));
    methods.push_back(spec("g2b", groupBody2));

    ClassBytesBuilder b("FpCorpus");
    b.addBootstrapMethod("FpBsm", "metafactory",
                         "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                         "Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;",
                         {BsmArg::methodType("()Ljava/util/function/Function;")});
    const Bytes bytes = b.assemble(methods);

    std::map<std::string_view, SemanticFp> fps;
    for (const MethodSpec& m : methods) {
        fps.emplace(m.name, fpOf(bytes, m.name));
    }
    ASSERT_EQ(fps.size(), methods.size());

    // 组内恒等。
    EXPECT_EQ(fps.at("g1a"), fps.at("g1b"));
    EXPECT_EQ(fps.at("g1a"), fps.at("g1c"));
    EXPECT_EQ(fps.at("g2a"), fps.at("g2b"));
    // 组间不同体必不同。
    EXPECT_NE(fps.at("g1a"), fps.at("g2a"));

    // 唯一体两两不同（零碰撞）。
    std::set<SemanticFp> distinct;
    for (ca::usize i = 0; i < methods.size() && methods[i].name[0] == 'u'; ++i) {
        distinct.insert(fps.at(methods[i].name));
    }
    EXPECT_EQ(distinct.size(), uniqueCount);

    // 全语料去重统计：26 唯一体 + 2 组 = 28 个不同指纹。
    std::set<SemanticFp> all;
    for (const auto& [name, fp] : fps) {
        all.insert(fp);
    }
    EXPECT_EQ(all.size(), uniqueCount + 2);
}

