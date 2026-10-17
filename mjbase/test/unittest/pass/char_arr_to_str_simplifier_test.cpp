// CharArrToStrSimplifier：char[] 字面构造 → String+toCharArray 改写（含
// UTF-16 内容跨 MUTF-8 编码）+ 形态不匹配守卫。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/pass/char_arr_to_str_simplifier.hpp"
#include "mjbase/spi/pass_name_registry.hpp"

#include "method_pass_test_util.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// DUP; ICONST_j; BIPUSH c; CASTORE（一个 store 拍）。
Bytes storeByte(ca::u8 idx, ca::u8 value)
{
    return {0x59, static_cast<ca::u8>(0x03 + idx), 0x10, value, 0x55};
}

/// DUP; ICONST_j; SIPUSH c; CASTORE（SIPUSH 版 store 拍）。
Bytes storeSipush(ca::u8 idx, ca::u16 value)
{
    return {0x59,
            static_cast<ca::u8>(0x03 + idx),
            0x11,
            static_cast<ca::u8>(value >> 8),
            static_cast<ca::u8>(value),
            0x55};
}

/// count 指令字节; NEWARRAY T_CHAR; stores...; ARETURN 的方法容器。
Bytes arrMethod(Bytes count, std::vector<Bytes> stores)
{
    FixtureBuilder b("t/C");
    Bytes          code = std::move(count);
    appendBytes(code, {0xbc, 0x05});   // NEWARRAY T_CHAR
    for (Bytes& s : stores) appendBytes(code, std::move(s));
    code.push_back(0xb0);   // ARETURN
    b.addMethod({"m", "()[C", std::move(code), 0x0009, {}, 2, 1});
    return b.assemble();
}

}   // namespace

// ── 全 BIPUSH 的 ASCII 内容：3 拍改写为 LDC + toCharArray ──
TEST(CharArrToStrSimplifierTest, asciiContentRewritten)
{
    std::vector<Bytes> stores;
    stores.push_back(storeByte(0, 'a'));
    stores.push_back(storeByte(1, 'b'));
    stores.push_back(storeByte(2, 'c'));
    const MethodPassRun r = runMethodPass(
        arrMethod({0x06}, std::move(stores)), mjbase::pass::CharArrToStrSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection      ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 3u);
    const jbc::ir::Insn* ldc  = ins.cu->dom.peek(ids[0]);
    const jbc::ir::Insn* inv  = ins.cu->dom.peek(ids[1]);
    EXPECT_EQ(ldc->op, Op::LDC);
    EXPECT_EQ(inv->op, Op::INVOKEVIRTUAL);
    EXPECT_EQ(ldcStringOf(ins, ldc), "abc");
}

// ── LDC(Int) 计数臂 + UTF-16 内容（SIPUSH U+4E2D）跨 MUTF-8 编码 ──
TEST(CharArrToStrSimplifierTest, utf16ContentEncodesToMutf8)
{
    FixtureBuilder b("t/C");
    const ca::u16  three = b.intConst(3);
    Bytes          code  = {0x13, static_cast<ca::u8>(three >> 8), static_cast<ca::u8>(three)};
    appendBytes(code, {0xbc, 0x05});   // NEWARRAY T_CHAR
    appendBytes(code, storeByte(0, 'A'));
    appendBytes(code, storeSipush(1, 0x4E2D));   // '中'
    appendBytes(code, storeByte(2, 'B'));
    code.push_back(0xb0);
    b.addMethod({"m", "()[C", std::move(code), 0x0009, {}, 2});
    const MethodPassRun r = runMethodPass(b.assemble(), mjbase::pass::CharArrToStrSimplifier{});

    EXPECT_TRUE(r.pa.changed());
    const Inspection ins = inspect(r.out);
    const std::vector<jbc::ir::InsnId> ids = ins.cu->dom.order();
    ASSERT_EQ(ids.size(), 3u);
    // "A中B" 的 MUTF-8 字节（与 UTF-8 同形：无 NUL 无增补面）。
    const std::string_view expect = "A\xE4\xB8\xAD" "B";
    EXPECT_EQ(ldcStringOf(ins, ins.cu->dom.peek(ids[0])), expect);
}

// ── 拍数与计数不符：不改写 ──
TEST(CharArrToStrSimplifierTest, countMismatchStaysUntouched)
{
    // 计数 5，只有 2 拍（不足的组失配；其余起点也构不成合法组——任何
    // ICONST_0 的下一个指令都不是 NEWARRAY）。
    std::vector<Bytes> stores;
    stores.push_back(storeByte(0, 'a'));
    stores.push_back(storeByte(1, 'b'));
    const Bytes         in = arrMethod({0x08}, std::move(stores));
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::CharArrToStrSimplifier{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 元素常量非字符写法（POP 折叠面之外）: ALOAD 进 store 拍 → 不改写 ──
TEST(CharArrToStrSimplifierTest, nonConstantElementBlocksRewrite)
{
    std::vector<Bytes> stores;
    stores.push_back({0x59, 0x03, 0x2a, 0x55});   // DUP; ICONST_0; ALOAD_0; CASTORE
    const Bytes         in = arrMethod({0x04}, std::move(stores));
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::CharArrToStrSimplifier{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 注册面 ──
TEST(CharArrToStrSimplifierTest, registeredByName)
{
    EXPECT_TRUE(mjbase::spi::defaultPassRegistry().has("CharArrToStrSimplifier"));
    auto created = mjbase::spi::defaultPassRegistry().createMethodPass("CharArrToStrSimplifier");
    ASSERT_TRUE(created.is_ok());
    EXPECT_NE(std::move(created).unwrap(), nullptr);
}
