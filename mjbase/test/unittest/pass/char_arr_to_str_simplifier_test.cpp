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
#include "shared/jasm_fixture.hpp"

using namespace mjbase::test;
using jbc::core::Op;

namespace {

/// DUP; ICONST_j; <element 拍>; CASTORE（一个 store 拍；element 拍为指令
/// 文本，装配器按自身口径落地窄常量形态）。
std::string storeByte(ca::u8 idx, std::string_view elementInsn)
{
    static constexpr const char* kIconst[4] = {"iconst_0", "iconst_1", "iconst_2",
                                               "iconst_3"};
    return std::string("dup\n") + kIconst[idx] + "\n" + std::string(elementInsn)
           + "\ncastore\n";
}

/// count 拍 + NEWARRAY T_CHAR + stores...; ARETURN 的 m()[C 方法容器
///（jasm 文本装配）。
Bytes arrMethod(std::string_view countInsn, std::vector<std::string> stores)
{
    std::string body(countInsn);
    body += "\nnewarray char\n";
    for (const std::string& s : stores) body += s;
    body += "areturn\n";
    return assembleMethodClass("m", "()[C", std::move(body));
}

}   // namespace

// ── 常量元素的 ASCII 内容：3 拍改写为 LDC + toCharArray ──
TEST(CharArrToStrSimplifierTest, asciiContentRewritten)
{
    std::vector<std::string> stores;
    stores.push_back(storeByte(0, "bipush 97"));   // 'a'（窄常量装配按规范化落地）
    stores.push_back(storeByte(1, "bipush 98"));   // 'b'
    stores.push_back(storeByte(2, "bipush 99"));   // 'c'
    const Bytes in = arrMethod("iconst_3", std::move(stores));
    const MethodPassRun r = runMethodPass(in, mjbase::pass::CharArrToStrSimplifier{});

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

// ── LDC(Int) 计数臂 + UTF-16 内容（U+4E2D）跨 MUTF-8 编码 ──
TEST(CharArrToStrSimplifierTest, utf16ContentEncodesToMutf8)
{
    std::vector<std::string> stores;
    stores.push_back(storeByte(0, "bipush 65"));       // 'A'
    stores.push_back(storeByte(1, "sipush 20013"));    // '中'
    stores.push_back(storeByte(2, "bipush 66"));       // 'B'
    const Bytes in = arrMethod("ldc 3", std::move(stores));
    const MethodPassRun r = runMethodPass(in, mjbase::pass::CharArrToStrSimplifier{});

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
    std::vector<std::string> stores;
    stores.push_back(storeByte(0, "bipush 97"));
    stores.push_back(storeByte(1, "bipush 98"));
    const Bytes         in = arrMethod("iconst_5", std::move(stores));
    const MethodPassRun r  = runMethodPass(in, mjbase::pass::CharArrToStrSimplifier{});

    EXPECT_TRUE(r.pa.areAllPreserved());
    EXPECT_EQ(r.out, in);
}

// ── 元素非字符常量写法（POP 折叠面之外）: GETSTATIC 进 store 拍 → 不改写 ──
TEST(CharArrToStrSimplifierTest, nonConstantElementBlocksRewrite)
{
    std::vector<std::string> stores;
    stores.push_back(storeByte(0, "getstatic t/X.f I"));   // 非常量 int 拍
    const Bytes         in = arrMethod("iconst_1", std::move(stores));
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
