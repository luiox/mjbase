// ConstCharArrayStringFolder：常量 char 数组构造的 `new String(char[])` 整窗
// 折叠为字符串 ldc（含 XOR/算术混淆、乱序下标与二次写、求值红线守卫、窗内
// 锚定标签的保守口径）。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/const_char_array_string_folder.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// String 窗口须的 CP 索引对：类 + `<init>([C)V` 引用。
struct StringCp
{
    ca::u16 strCls  = 0;
    ca::u16 initRef = 0;
};

StringCp makeStringCp(FixtureBuilder& b)
{
    return {b.cls("java/lang/String"), b.methodRef("java/lang/String", "<init>", "([C)V")};
}

/// `new String` 窗口头两拍：NEW + DUP。
Bytes windowHead(const StringCp& cp)
{
    return {0xBB, static_cast<ca::u8>(cp.strCls >> 8), static_cast<ca::u8>(cp.strCls), 0x59};
}

/// 元素拍：DUP; ICONST_j; <值字节>; CASTORE。
Bytes arm(ca::u8 idx, Bytes valueBytes)
{
    Bytes code{0x59, static_cast<ca::u8>(0x03 + idx)};
    appendBytes(code, std::move(valueBytes));
    code.push_back(0x55);   // CASTORE
    return code;
}

/// 窗口收尾拍：INVOKESPECIAL java/lang/String.<init>([C)V。
Bytes seal(const StringCp& cp)
{
    return {0xB7, static_cast<ca::u8>(cp.initRef >> 8), static_cast<ca::u8>(cp.initRef)};
}

/// LDC_W int 常量装栈（idx 由 b.intConst 先行物化）。
Bytes loadIntW(ca::u16 idx)
{
    return {0x13, static_cast<ca::u8>(idx >> 8), static_cast<ca::u8>(idx)};
}

}   // namespace


// ── 全 BIPUSH 的 ASCII 形状：整窗折叠为单条 LDC <"abc"> + ARETURN ──
TEST(ConstCharArrayStringFolderTest, asciiConstructionFoldsToLdc)
{
    FixtureBuilder b("t/F");
    const StringCp cp = makeStringCp(b);

    Bytes code = windowHead(cp);
    appendBytes(code, {0x06});                               // ICONST_3
    appendBytes(code, {0xBC, 0x05});                         // NEWARRAY T_CHAR
    appendBytes(code, arm(0, {0x10, 0x61}));                 // 'a'
    appendBytes(code, arm(1, {0x10, 0x62}));                 // 'b'
    appendBytes(code, arm(2, {0x10, 0x63}));                 // 'c'
    appendBytes(code, seal(cp));
    code.push_back(0xB0);                                    // ARETURN
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 2u);
    const jbc::ir::Insn* ldc = ins.cu->dom.peek(ids[0]);
    EXPECT_EQ(ldc->op, Op::LDC);
    EXPECT_EQ(ldcStringOf(ins, ldc), "abc");
    EXPECT_EQ(ins.cu->dom.peek(ids[1])->op, Op::ARETURN);
}

// ── 混淆算术形状：len 走 ISUB；元素走 LDC_W+IXOR+I2C / IADD / IOR ──
TEST(ConstCharArrayStringFolderTest, obfuscatedConstantExpressionsFold)
{
    FixtureBuilder b("t/F");
    const StringCp cp      = makeStringCp(b);
    const ca::u16  cjkBase = b.intConst(0x4E00);   // Integer 19968

    Bytes code = windowHead(cp);
    appendBytes(code, {0x05, 0x02, 0x64});                   // ICONST_2; ICONST_M1; ISUB → 3
    appendBytes(code, {0xBC, 0x05});                         // NEWARRAY T_CHAR
    // '中' = 0x4E00 ^ 0x2D = 0x4E2D（LDC_W + BIPUSH + IXOR + I2C）
    {
        Bytes v;
        appendBytes(v, loadIntW(cjkBase));
        appendBytes(v, {0x10, 0x2D, 0x82, 0x92});            // BIPUSH 0x2D; IXOR; I2C
        appendBytes(code, arm(0, std::move(v)));
    }
    // 'a' = 3 + 0x5E = 0x61（BIPUSH + SIPUSH + IADD）
    {
        Bytes v;
        appendBytes(v, {0x06, 0x11, 0x00, 0x5E, 0x60});
        appendBytes(code, arm(1, std::move(v)));
    }
    // 'B' = 2 | 0x40 = 0x42（BIPUSH + BIPUSH + IOR）
    {
        Bytes v;
        appendBytes(v, {0x05, 0x10, 0x40, 0x80});
        appendBytes(code, arm(2, std::move(v)));
    }
    appendBytes(code, seal(cp));
    code.push_back(0xB0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

    const MethodPassRun r =
        runMethodPass(b.assemble(), mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.changed());
    const Inspection            ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 2u);
    // MUTF-8("中aB")（无 NUL、无增补面 → 与 UTF-8 同形）。
    const std::string_view expected = "\xE4\xB8\xAD" "aB";
    EXPECT_EQ(ldcStringOf(ins, ins.cu->dom.peek(ids[0])), expected);
}

// ── 同下标二次写：拍数==len 的形状约束下 pigeonhole 必致未覆盖下标 →
//    整组保留（旧栈「last-wins」分支出不可达，注释只作防御性语义）。
//    另查乱序下标不破坏匹配：同 len 下前两拍乱序 + 第三拍覆盖完成员。 ──
TEST(ConstCharArrayStringFolderTest, outOfOrderStoresKeptOrFolded)
{
    // 3 长度 4 拍（[1] 双写）→ 恰好覆盖不成立，整组保留。
    {
        FixtureBuilder b("t/F");
        const StringCp cp = makeStringCp(b);

        Bytes code = windowHead(cp);
        appendBytes(code, {0x06});
        appendBytes(code, {0xBC, 0x05});
        appendBytes(code, arm(2, {0x10, 0x43}));                 // [2]='C'
        appendBytes(code, arm(0, {0x10, 0x41}));                 // [0]='A'
        appendBytes(code, arm(1, {0x10, 0x58}));                 // [1]='X'
        appendBytes(code, arm(1, {0x10, 0x62}));                 // [1]='b' → idx1 双写
        appendBytes(code, seal(cp));
        code.push_back(0xB0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

        const Bytes         in = b.assemble();
        const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
        EXPECT_TRUE(r.pa.areAllPreserved());
        EXPECT_EQ(r.out, in);
    }
    // 乱序但恰覆盖（[2],[0],[1]）→ 折叠为 "CAb"... 注意 arm 常量：'C'@2 'A'@0 'b'@1。
    {
        FixtureBuilder b("t/F");
        const StringCp cp = makeStringCp(b);

        Bytes code = windowHead(cp);
        appendBytes(code, {0x06});
        appendBytes(code, {0xBC, 0x05});
        appendBytes(code, arm(2, {0x10, 0x43}));                 // [2]='C'
        appendBytes(code, arm(0, {0x10, 0x41}));                 // [0]='A'
        appendBytes(code, arm(1, {0x10, 0x62}));                 // [1]='b'
        appendBytes(code, seal(cp));
        code.push_back(0xB0);
        b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

        const MethodPassRun r =
            runMethodPass(b.assemble(), mjbase::pass::ConstCharArrayStringFolder{});
        EXPECT_TRUE(r.pa.changed());
        const Inspection ins = inspect(r.out);
        ASSERT_EQ(ins.cu->dom.order().size(), 2u);
        EXPECT_EQ(ldcStringOf(ins, ins.cu->dom.peek(ins.cu->dom.order()[0])), "AbC");
    }
}

// ── 求值红线：len 表达式含 IDIV → 整组保留、字节不动 ──
TEST(ConstCharArrayStringFolderTest, idivExpressionKept)
{
    FixtureBuilder b("t/F");
    const StringCp cp = makeStringCp(b);

    Bytes code = windowHead(cp);
    appendBytes(code, {0x07, 0x05, 0x6C});                   // ICONST_4; ICONST_2; IDIV
    appendBytes(code, {0xBC, 0x05});
    appendBytes(code, arm(0, {0x10, 0x61}));
    appendBytes(code, arm(1, {0x10, 0x62}));
    appendBytes(code, seal(cp));
    code.push_back(0xB0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 覆盖不完整：len 3 但只写 [0]/[2] → 整组保留 ──
TEST(ConstCharArrayStringFolderTest, uncoveredIndexKept)
{
    FixtureBuilder b("t/F");
    const StringCp cp = makeStringCp(b);

    Bytes code = windowHead(cp);
    appendBytes(code, {0x06});
    appendBytes(code, {0xBC, 0x05});
    appendBytes(code, arm(0, {0x10, 0x61}));
    appendBytes(code, arm(2, {0x10, 0x63}));
    appendBytes(code, seal(cp));
    code.push_back(0xB0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 非常量生产者：元素拍里 ACONST_NULL → 求值失配，整组保留 ──
TEST(ConstCharArrayStringFolderTest, unknownConstantProducerKept)
{
    FixtureBuilder b("t/F");
    const StringCp cp = makeStringCp(b);

    Bytes code = windowHead(cp);
    appendBytes(code, {0x04});                               // ICONST_1
    appendBytes(code, {0xBC, 0x05});
    appendBytes(code, arm(0, {0x01}));                       // ACONST_NULL
    appendBytes(code, seal(cp));
    code.push_back(0xB0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 孤立代理：值 = SIPUSH 0xD800 → Utf16String 抛错整组保留 ──
TEST(ConstCharArrayStringFolderTest, loneSurrogateKept)
{
    FixtureBuilder b("t/F");
    const StringCp cp = makeStringCp(b);

    Bytes code = windowHead(cp);
    appendBytes(code, {0x04});                               // ICONST_1
    appendBytes(code, {0xBC, 0x05});
    appendBytes(code, arm(0, {0x11, 0xD8, 0x00}));           // SIPUSH 0xD800
    appendBytes(code, seal(cp));
    code.push_back(0xB0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 非 char 数组：NEWARRAY T_INT + IASTORE 拍 → 形状失配整组保留 ──
TEST(ConstCharArrayStringFolderTest, nonCharArrayKept)
{
    FixtureBuilder b("t/F");
    const StringCp cp = makeStringCp(b);

    Bytes code = windowHead(cp);
    appendBytes(code, {0x04});                               // ICONST_1
    appendBytes(code, {0xBC, 0x0A});                         // NEWARRAY T_INT
    {
        Bytes v{0x59, 0x03, 0x10, 0x41, 0x4F};               // DUP; ICONST_0; BIPUSH 'A'; IASTORE
        appendBytes(code, std::move(v));
    }
    appendBytes(code, seal(cp));
    code.push_back(0xB0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 错挂 ctor：init 引用不是 String.<init>([C)V → 整组保留 ──
TEST(ConstCharArrayStringFolderTest, wrongConstructorKept)
{
    FixtureBuilder b("t/F");
    const StringCp cp    = makeStringCp(b);
    const ca::u16  badRef = b.methodRef("java/lang/Object", "<init>", "()V");

    Bytes code = windowHead(cp);
    appendBytes(code, {0x04});
    appendBytes(code, {0xBC, 0x05});
    appendBytes(code, arm(0, {0x10, 0x41}));
    appendBytes(code, {0xB7, static_cast<ca::u8>(badRef >> 8), static_cast<ca::u8>(badRef)});
    code.push_back(0xB0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

    const Bytes         in = b.assemble();
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::ConstCharArrayStringFolder{});
    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 窗内锚定标签（GOTO 跳进 arm1 头）：保守整组保留（旧栈手工形态偏离差集，
//    golden 字节对拍刻意避开，由本用例钉死口径）──
TEST(ConstCharArrayStringFolderTest, midWindowAnchorKept)
{
    FixtureBuilder b("t/F");
    const StringCp cp = makeStringCp(b);

    // 字节布局：GOTO(3) | NEW(3) | DUP(1) | ICONST_3(1) | NEWARRAY(2)
    //   | arm0(5) | arm1 @pc15 | arm2 | INVOKESPECIAL | ARETURN
    // GOTO 目标 = pc15（arm1 的 DUP 头，相对 pc0 偏移 +15，与 旧栈 侧
    // hb_const_char_fold 语料的 m_keep_anchor 同布局）。
    Bytes code;
    appendBytes(code, {0xA7, 0x00, 0x0F});
    appendBytes(code, windowHead(cp));
    appendBytes(code, {0x06});
    appendBytes(code, {0xBC, 0x05});
    appendBytes(code, arm(0, {0x10, 0x61}));
    appendBytes(code, arm(1, {0x10, 0x62}));
    appendBytes(code, arm(2, {0x10, 0x63}));
    appendBytes(code, seal(cp));
    code.push_back(0xB0);
    b.addMethod({"m", "()Ljava/lang/String;", std::move(code), 0x0009, {}, 6, 0});

    const Bytes         in = b.assemble();
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
