// 未配对代理（lone surrogate）CP Utf8 的反汇编回归：BoogerV2 语料的病态类
// 曾因孤立代理单元无法重编码为标准 UTF-8（Utf16String::to_utf8_string 显式
// 拒绝）整类 disassemble 一票否决。EscapeUtil::escape 对孤立代理落 \uXXXX
// 转义文本（合法代理对保持增补平面字符呈现）后，文本面对全部引用位可
// 反汇编；字符串常量位（语料真实形态：XOR 加密常量）经 unescape +
// modifiedUtf8 逐位还原 CESU-8 三字节形，往返字节可逆。
//
// fixture（test_resource/jasm_lone_surrogate/*.class）是手拼最小 v52 类：
// CP Utf8 #8 = ED B0 B1（U+DC31 的 CESU-8 三字节形），五个变体各把它放进
// 单一引用位——ldc String（v_ldc）、方法名（v_name）、类 Signature（v_sig）、
// 注解类型（v_anno）、仅驻池不引用（v_unref）。
#include "jhasm/jhasm.hpp"

#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<ca::u8> readFixture(const char* name)
{
    const auto path = test::getResourcePath(std::string("jasm_lone_surrogate/") + name + ".class");
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// 重装配后 CP 里必须仍能找到孤立代理的 CESU-8 三字节形（字节级可逆）。
bool containsLoneForm(const std::vector<ca::u8>& bytes)
{
    for (ca::usize i = 0; i + 2 < bytes.size(); ++i) {
        if (bytes[i] == 0xED && bytes[i + 1] == 0xB0 && bytes[i + 2] == 0xB1) return true;
    }
    return false;
}

}   // namespace

TEST(MjLoneSurrogateDisasm, DisassemblesAllReferenceSites)
{
    for (const char* variant : {"v_unref", "v_ldc", "v_name", "v_sig", "v_anno"}) {
        const auto bytes = readFixture(variant);
        ASSERT_FALSE(bytes.empty()) << variant;

        auto whole = jhasm::disassemble(bytes);
        ASSERT_TRUE(whole.is_ok()) << variant;
        EXPECT_FALSE(whole.get()->ref().empty()) << variant;

        const ca::u8 loneName[] = {0xED, 0xB0, 0xB1};
        auto method =
            variant == std::string("v_name")
                ? jhasm::disassembleMethod(bytes, ca::str::Utf8StringRef::from_data(loneName, 3),
                                           ca::str::Utf8StringRef::from_cstr("()V"))
                : jhasm::disassembleMethod(bytes, ca::str::Utf8StringRef::from_cstr("m"),
                                           ca::str::Utf8StringRef::from_cstr("()V"));
        ASSERT_TRUE(method.is_ok()) << variant;
    }
}

TEST(MjLoneSurrogateDisasm, EmitsReversibleUnicodeEscapeAndRestoresBytes)
{
    const auto bytes = readFixture("v_ldc");
    ASSERT_FALSE(bytes.empty());

    auto whole = jhasm::disassemble(bytes);
    ASSERT_TRUE(whole.is_ok());
    const auto text = whole.get()->ref().to_utf8_string().to_std_string();
    EXPECT_NE(text.find("\\uDC31"), std::string::npos)
        << "lone surrogate must appear as reversible \\uXXXX escape";

    auto reassembled = jhasm::assemble(utf16("Lone.jasm"), whole.get()->ref());
    ASSERT_TRUE(reassembled.is_ok());
    for (const auto& error : reassembled.errors())
        ADD_FAILURE() << error.message().to_utf8_string().to_std_string();
    EXPECT_TRUE(containsLoneForm(reassembled.get()->bytes));
}

// 方法名位（裸 token）的 \uXXXX 不做反转义是既有解析面缺口（LRM 等合法
// 转义名同样往返走样）；本批交付面是「可反汇编」——文本含可逆转义即可。
TEST(MjLoneSurrogateDisasm, ShowsEscapedLoneSurrogateMethodName)
{
    const auto bytes = readFixture("v_name");
    ASSERT_FALSE(bytes.empty());

    auto whole = jhasm::disassemble(bytes);
    ASSERT_TRUE(whole.is_ok());
    const auto text = whole.get()->ref().to_utf8_string().to_std_string();
    EXPECT_NE(text.find("\\uDC31"), std::string::npos)
        << "lone surrogate method name must display as \\uXXXX escape";
}
